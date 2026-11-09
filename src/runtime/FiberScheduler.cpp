#include <algorithm>
#include "realscript/runtime/FiberScheduler.h"
#include "realscript/mir/Mir.h"

#include <condition_variable>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace realscript::runtime {

using Opcode = FiberOpcode;

struct FiberScheduler::ParallelWorkerPool {
    std::mutex dispatchMutex;
    std::mutex mutex;
    std::condition_variable cvStart;
    std::condition_variable cvDone;
    std::vector<std::thread> threads;
    std::function<void(std::size_t, std::size_t)> currentTask;
    std::size_t activeWorkerCount = 0;
    std::size_t completedWorkers = 0;
    std::uint64_t generation = 0;
    bool stop = false;

    ~ParallelWorkerPool() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stop = true;
            ++generation;
        }
        cvStart.notify_all();
        for (auto& t : threads) {
            if (t.joinable()) t.join();
        }
    }

    void ensureThreads(std::size_t bgCount) {
        while (threads.size() < bgCount) {
            const std::size_t workerIdx = threads.size() + 1;
            const std::uint64_t initialGen = generation;
            threads.emplace_back([this, workerIdx, initialGen]() {
                std::uint64_t seenGen = initialGen;
                std::unique_lock<std::mutex> lock(mutex);
                for (;;) {
                    cvStart.wait(lock, [&]() { return stop || generation != seenGen; });
                    if (stop) return;
                    seenGen = generation;
                    if (workerIdx < activeWorkerCount && currentTask) {
                        const auto task = currentTask;
                        const std::size_t total = activeWorkerCount;
                        lock.unlock();
                        task(workerIdx, total);
                        lock.lock();
                        if (++completedWorkers == activeWorkerCount - 1) {
                            cvDone.notify_one();
                        }
                    }
                }
            });
        }
    }

    void dispatch(
        std::size_t numThreads,
        const std::function<void(std::size_t, std::size_t)>& task) {
        if (numThreads <= 1) {
            task(0, 1);
            return;
        }
        std::lock_guard<std::mutex> dispatchLock(dispatchMutex);
        {
            std::unique_lock<std::mutex> lock(mutex);
            ensureThreads(numThreads - 1);
            currentTask = task;
            activeWorkerCount = numThreads;
            completedWorkers = 0;
            ++generation;
        }
        cvStart.notify_all();
        task(0, numThreads);
        {
            std::unique_lock<std::mutex> lock(mutex);
            cvDone.wait(lock, [&]() { return completedWorkers == numThreads - 1; });
            currentTask = nullptr;
        }
    }
};

FiberScheduler::FiberScheduler(IFiberHostCallbacks* hostCallbacks) noexcept
    : host_(hostCallbacks) {
    programs_.reserve(64);
    externalStreamCache_.reserve(64);
    // Reserve index 0 as invalid program sentinel
    programs_.push_back({});
    externalStreamCache_.push_back(nullptr);

    fibers_.reserve(2048);
    // Reserve index 0 as sentinel
    fibers_.push_back(FiberContext{});
}

std::uint32_t FiberScheduler::registerProgram(std::vector<CompactInstruction> codeStream) {
    std::uint32_t progId = static_cast<std::uint32_t>(programs_.size());
    programs_.push_back(std::move(codeStream));
    externalStreamCache_.push_back(nullptr);
    return progId;
}

std::uint32_t FiberScheduler::registerModule(const CompiledFiberModule& module) {
    return registerProgram(module.instructions);
}

bool FiberScheduler::reloadProgram(
    std::uint32_t programId,
    std::vector<CompactInstruction> newCodeStream) {
    if (programId == 0 || programId >= programs_.size() || newCodeStream.empty()) {
        return false;
    }
    const auto newSize = static_cast<std::uint32_t>(newCodeStream.size());
    programs_[programId] = std::move(newCodeStream);
    externalStreamCache_[programId] = nullptr;
    for (std::size_t i = 1; i < fibers_.size(); ++i) {
        auto& f = fibers_[i];
        if (f.programId != programId ||
            f.status == FiberStatus::Inactive ||
            f.status == FiberStatus::Terminated) {
            continue;
        }
        bool callStackInvalid = (f.pc >= newSize);
        for (std::uint8_t d = 0; !callStackInvalid && d < f.callDepth && d < f.callStack.size(); ++d) {
            if (f.callStack[d] >= newSize) {
                callStackInvalid = true;
            }
        }
        if (callStackInvalid) {
            f.pc = 0;
            f.callDepth = 0;
        }
    }
    return true;
}

bool FiberScheduler::reloadModule(
    std::uint32_t programId,
    const CompiledFiberModule& oldModule,
    const CompiledFiberModule& newModule) {
    if (programId == 0 || programId >= programs_.size() || newModule.instructions.empty()) {
        return false;
    }
    struct FuncRange {
        std::string name;
        std::uint32_t startPc = 0;
        std::uint32_t endPc = 0;
    };
    std::vector<FuncRange> oldRanges;
    oldRanges.reserve(oldModule.entryPoints.size());
    for (const auto& kv : oldModule.entryPoints) {
        oldRanges.push_back(FuncRange{kv.first, kv.second, static_cast<std::uint32_t>(oldModule.instructions.size())});
    }
    std::sort(oldRanges.begin(), oldRanges.end(), [](const FuncRange& a, const FuncRange& b) {
        return a.startPc < b.startPc;
    });
    for (std::size_t i = 0; i + 1 < oldRanges.size(); ++i) {
        oldRanges[i].endPc = oldRanges[i + 1].startPc;
    }

    std::vector<FuncRange> newRanges;
    newRanges.reserve(newModule.entryPoints.size());
    for (const auto& kv : newModule.entryPoints) {
        newRanges.push_back(FuncRange{kv.first, kv.second, static_cast<std::uint32_t>(newModule.instructions.size())});
    }
    std::sort(newRanges.begin(), newRanges.end(), [](const FuncRange& a, const FuncRange& b) {
        return a.startPc < b.startPc;
    });
    for (std::size_t i = 0; i + 1 < newRanges.size(); ++i) {
        newRanges[i].endPc = newRanges[i + 1].startPc;
    }
    std::unordered_map<std::string, FuncRange> newRangeByName;
    for (const auto& nr : newRanges) {
        newRangeByName.emplace(nr.name, nr);
    }

    const auto remapPc = [&](std::uint32_t oldPc) -> std::uint32_t {
        for (const auto& orng : oldRanges) {
            if (oldPc >= orng.startPc && oldPc < orng.endPc) {
                const auto nit = newRangeByName.find(orng.name);
                if (nit != newRangeByName.end()) {
                    const std::uint32_t relOffset = oldPc - orng.startPc;
                    const std::uint32_t candidate = nit->second.startPc + relOffset;
                    if (candidate < nit->second.endPc) {
                        return candidate;
                    }
                    return nit->second.startPc;
                }
                return 0u;
            }
        }
        return oldPc < newModule.instructions.size() ? oldPc : 0u;
    };

    programs_[programId] = newModule.instructions;
    externalStreamCache_[programId] = nullptr;
    for (std::size_t i = 1; i < fibers_.size(); ++i) {
        auto& f = fibers_[i];
        if (f.programId != programId ||
            f.status == FiberStatus::Inactive ||
            f.status == FiberStatus::Terminated) {
            continue;
        }
        f.pc = remapPc(f.pc);
        for (std::uint8_t d = 0; d < f.callDepth && d < f.callStack.size(); ++d) {
            f.callStack[d] = remapPc(f.callStack[d]);
        }
    }
    return true;
}

std::uint32_t FiberScheduler::spawnFiberByProgram(
    std::uint32_t programId,
    std::uint32_t entryPc,
    GenerationalEntityHandle entity,
    std::uint8_t ownerGroupId,
    std::uint32_t parentFiberId,
    std::uint32_t seed) {
    std::uint32_t id = 0;
    if (!freeFiberIds_.empty()) {
        id = freeFiberIds_.back();
        freeFiberIds_.pop_back();
    } else {
        id = static_cast<std::uint32_t>(fibers_.size());
        fibers_.push_back(FiberContext{});
    }

    FiberContext ctx{};
    ctx.fiberId = id;
    ctx.parentFiberId = parentFiberId;
    ctx.programId = programId;
    ctx.boundEntity = entity;
    ctx.ownerGroupId = ownerGroupId;
    ctx.status = FiberStatus::Ready;
    ctx.pc = entryPc;
    ctx.wakeTick = currentTick_;
    ctx.prng.seed(seed ^ (id * 0x9E3779B9u));

    fibers_[id] = ctx;
    readyQueue_.push_back(id);
    return id;
}

std::uint32_t FiberScheduler::spawnFiber(
    const std::vector<CompactInstruction>& codeStream,
    std::uint32_t entryPc,
    GenerationalEntityHandle entity,
    std::uint8_t ownerGroupId,
    std::uint32_t parentFiberId,
    std::uint32_t seed) {
    std::uint32_t programId = 0;
    for (std::size_t i = 1; i < externalStreamCache_.size(); ++i) {
        if (externalStreamCache_[i] == &codeStream) {
            programId = static_cast<std::uint32_t>(i);
            break;
        }
    }
    if (programId == 0) {
        programId = static_cast<std::uint32_t>(programs_.size());
        programs_.push_back(codeStream);
        externalStreamCache_.push_back(&codeStream);
    }
    return spawnFiberByProgram(programId, entryPc, entity, ownerGroupId, parentFiberId, seed);
}

std::uint32_t FiberScheduler::spawnFunction(
    const CompiledFiberModule& module,
    const std::string& functionName,
    GenerationalEntityHandle entity,
    std::uint8_t ownerGroupId,
    std::uint32_t parentFiberId,
    std::uint32_t seed) {
    return spawnFiber(
        module.instructions,
        module.entryPc(functionName),
        entity,
        ownerGroupId,
        parentFiberId,
        seed);
}

bool FiberScheduler::interruptFiber(std::uint32_t fiberId, std::uint32_t newEntryPc, bool force) noexcept {
    if (fiberId == 0 || fiberId >= fibers_.size()) return false;
    auto& f = fibers_[fiberId];
    if (f.status == FiberStatus::Inactive || f.status == FiberStatus::Terminated) return false;
    if (f.unbreakableSection && !force) return false;

    f.pc = newEntryPc;
    f.callDepth = 0;
    f.waitPredicateChannel = 0;
    if (f.status != FiberStatus::Ready) {
        f.status = FiberStatus::Ready;
        readyQueue_.push_back(fiberId);
    }
    return true;
}

void FiberScheduler::killFiberTree(std::uint32_t rootFiberId) noexcept {
    if (rootFiberId == 0 || rootFiberId >= fibers_.size()) return;
    if (fibers_[rootFiberId].status == FiberStatus::Terminated ||
        fibers_[rootFiberId].status == FiberStatus::Inactive) {
        return;
    }
    fibers_[rootFiberId].status = FiberStatus::Terminated;
    fibers_[rootFiberId].unbreakableSection = false;
    freeFiberIds_.push_back(rootFiberId);

    for (std::size_t i = 1; i < fibers_.size(); ++i) {
        if (fibers_[i].parentFiberId == rootFiberId &&
            fibers_[i].status != FiberStatus::Terminated &&
            fibers_[i].status != FiberStatus::Inactive) {
            killFiberTree(static_cast<std::uint32_t>(i));
        }
    }
}

std::size_t FiberScheduler::killEntityFibers(GenerationalEntityHandle entity) noexcept {
    if (!entity.valid()) return 0;
    std::size_t killed = 0;
    for (std::size_t i = 1; i < fibers_.size(); ++i) {
        if (fibers_[i].boundEntity == entity &&
            fibers_[i].status != FiberStatus::Terminated &&
            fibers_[i].status != FiberStatus::Inactive) {
            killFiberTree(static_cast<std::uint32_t>(i));
            ++killed;
        }
    }
    return killed;
}

std::size_t FiberScheduler::killEntityFibersBySlot(std::uint32_t entitySlot) noexcept {
    if (entitySlot == 0 || entitySlot == GenerationalEntityHandle::InvalidSlot) return 0;
    std::size_t killed = 0;
    for (std::size_t i = 1; i < fibers_.size(); ++i) {
        if (fibers_[i].boundEntity.slot == entitySlot &&
            fibers_[i].status != FiberStatus::Terminated &&
            fibers_[i].status != FiberStatus::Inactive) {
            killFiberTree(static_cast<std::uint32_t>(i));
            ++killed;
        }
    }
    return killed;
}

void FiberScheduler::scheduleAtTick(std::uint32_t fiberId, std::uint32_t wakeTick) {
    fibers_[fiberId].status = FiberStatus::SleepingTicks;
    fibers_[fiberId].wakeTick = wakeTick;
    std::size_t slot = static_cast<std::size_t>(wakeTick) & WHEEL_MASK;
    timingWheel_[slot].push_back(fiberId);
}

std::uint64_t FiberScheduler::stepTick(std::uint32_t currentTick) {
    currentTick_ = currentTick;
    std::uint64_t instrExecuted = 0;

    // 1. Pop all fibers due in the current O(1) Timing Wheel slot
    std::size_t slot = static_cast<std::size_t>(currentTick) & WHEEL_MASK;
    auto& bucket = timingWheel_[slot];
    std::size_t writeIdx = 0;
    for (std::size_t i = 0; i < bucket.size(); ++i) {
        const std::uint32_t fid = bucket[i];
        if (fid >= fibers_.size()) continue;
        auto& f = fibers_[fid];
        if (f.status == FiberStatus::SleepingTicks) {
            if (f.wakeTick <= currentTick) {
                f.status = FiberStatus::Ready;
                readyQueue_.push_back(fid);
            } else {
                // Multi-rotation long wait (> 4096 ticks) compacted in-place without heap allocation
                bucket[writeIdx++] = fid;
            }
        }
    }
    bucket.resize(writeIdx);

    // 2. Drain readyQueue_ (note: ForkFiber may append new child fibers during loop)
    for (std::size_t idx = 0; idx < readyQueue_.size(); ++idx) {
        std::uint32_t fid = readyQueue_[idx];
        if (fid == 0 || fid >= fibers_.size()) continue;
        auto& fiber = fibers_[fid];
        if (fiber.status != FiberStatus::Ready) continue;
        executeFiberSlice(fiber, currentTick, instrExecuted);
    }
    readyQueue_.clear();

    return instrExecuted;
}

std::uint64_t FiberScheduler::stepTickParallel(
    std::uint32_t currentTick,
    std::size_t workerCount) {
    if (workerCount <= 1) {
        return stepTick(currentTick);
    }
    currentTick_ = currentTick;

    const std::size_t slot = static_cast<std::size_t>(currentTick) & WHEEL_MASK;
    auto& bucket = timingWheel_[slot];
    std::size_t writeIdx = 0;
    for (std::size_t i = 0; i < bucket.size(); ++i) {
        const std::uint32_t fid = bucket[i];
        if (fid >= fibers_.size()) continue;
        auto& f = fibers_[fid];
        if (f.status == FiberStatus::SleepingTicks) {
            if (f.wakeTick <= currentTick) {
                f.status = FiberStatus::Ready;
                readyQueue_.push_back(fid);
            } else {
                bucket[writeIdx++] = fid;
            }
        }
    }
    bucket.resize(writeIdx);

    if (readyQueue_.size() < 2) {
        std::uint64_t instrExecuted = 0;
        for (std::size_t idx = 0; idx < readyQueue_.size(); ++idx) {
            std::uint32_t fid = readyQueue_[idx];
            if (fid == 0 || fid >= fibers_.size()) continue;
            auto& fiber = fibers_[fid];
            if (fiber.status != FiberStatus::Ready) continue;
            executeFiberSlice(fiber, currentTick, instrExecuted);
        }
        readyQueue_.clear();
        return instrExecuted;
    }

    struct DeferredFiberOutcome {
        std::uint32_t fiberId = 0;
        std::uint64_t instructions = 0;
        bool sleepScheduled = false;
        std::uint32_t wakeTick = 0;
        bool predicateWait = false;
        bool killed = false;
        bool forked = false;
        std::uint32_t forkProgramId = 0;
        std::uint32_t forkTargetPc = 0;
        GenerationalEntityHandle forkEntity{};
        std::uint8_t forkGroup = 0;
        std::uint32_t forkSeed = 0;
    };

    const std::vector<std::uint32_t> batch = readyQueue_;
    readyQueue_.clear();
    std::vector<DeferredFiberOutcome> outcomes(batch.size());
    std::mutex hostMutex;

    const auto runIsolatedFiber = [&](std::size_t batchIndex) {
        const std::uint32_t fid = batch[batchIndex];
        auto& out = outcomes[batchIndex];
        out.fiberId = fid;
        if (fid == 0 || fid >= fibers_.size()) return;
        auto& fiber = fibers_[fid];
        if (fiber.status != FiberStatus::Ready) return;
        if (fiber.programId == 0 || fiber.programId >= programs_.size()) {
            out.killed = true;
            return;
        }
        const auto& code = programs_[fiber.programId];
        std::uint32_t sliceBudget = 256;
        while (fiber.pc < code.size() && sliceBudget-- > 0) {
            const CompactInstruction& ins = code[fiber.pc];
            ++out.instructions;
            switch (ins.opcode) {
            case Opcode::Nop: ++fiber.pc; break;
            case Opcode::LoadInt:
            case Opcode::LoadFixed24_8:
                if (ins.regOrSlot < fiber.regs.size()) {
                    fiber.regs[ins.regOrSlot] = static_cast<std::int32_t>(ins.imm32);
                }
                ++fiber.pc;
                break;
            case Opcode::MoveReg:
                if (ins.regOrSlot < fiber.regs.size()) {
                    fiber.regs[ins.regOrSlot] = fiber.regs[ins.regs.srcA & 7u];
                }
                ++fiber.pc;
                break;
            case Opcode::Add:
                if (ins.regOrSlot < fiber.regs.size()) {
                    fiber.regs[ins.regOrSlot] =
                        fiber.regs[ins.regs.srcA & 7u] + fiber.regs[ins.regs.srcB & 7u];
                }
                ++fiber.pc;
                break;
            case Opcode::Sub:
                if (ins.regOrSlot < fiber.regs.size()) {
                    fiber.regs[ins.regOrSlot] =
                        fiber.regs[ins.regs.srcA & 7u] - fiber.regs[ins.regs.srcB & 7u];
                }
                ++fiber.pc;
                break;
            case Opcode::Mul:
                if (ins.regOrSlot < fiber.regs.size()) {
                    fiber.regs[ins.regOrSlot] =
                        fiber.regs[ins.regs.srcA & 7u] * fiber.regs[ins.regs.srcB & 7u];
                }
                ++fiber.pc;
                break;
            case Opcode::Div:
                if (ins.regOrSlot < fiber.regs.size()) {
                    std::int32_t rhs = fiber.regs[ins.regs.srcB & 7u];
                    fiber.regs[ins.regOrSlot] = (rhs != 0) ? (fiber.regs[ins.regs.srcA & 7u] / rhs) : 0;
                }
                ++fiber.pc;
                break;
            case Opcode::Mod:
                if (ins.regOrSlot < fiber.regs.size()) {
                    std::int32_t rhs = fiber.regs[ins.regs.srcB & 7u];
                    fiber.regs[ins.regOrSlot] = (rhs != 0) ? (fiber.regs[ins.regs.srcA & 7u] % rhs) : 0;
                }
                ++fiber.pc;
                break;
            case Opcode::CmpEq:
                if (ins.regOrSlot < fiber.regs.size()) {
                    fiber.regs[ins.regOrSlot] =
                        (fiber.regs[ins.regs.srcA & 7u] == fiber.regs[ins.regs.srcB & 7u]) ? 1 : 0;
                }
                ++fiber.pc;
                break;
            case Opcode::CmpNe:
                if (ins.regOrSlot < fiber.regs.size()) {
                    fiber.regs[ins.regOrSlot] =
                        (fiber.regs[ins.regs.srcA & 7u] != fiber.regs[ins.regs.srcB & 7u]) ? 1 : 0;
                }
                ++fiber.pc;
                break;
            case Opcode::CmpLt:
                if (ins.regOrSlot < fiber.regs.size()) {
                    fiber.regs[ins.regOrSlot] =
                        (fiber.regs[ins.regs.srcA & 7u] < fiber.regs[ins.regs.srcB & 7u]) ? 1 : 0;
                }
                ++fiber.pc;
                break;
            case Opcode::CmpLe:
                if (ins.regOrSlot < fiber.regs.size()) {
                    fiber.regs[ins.regOrSlot] =
                        (fiber.regs[ins.regs.srcA & 7u] <= fiber.regs[ins.regs.srcB & 7u]) ? 1 : 0;
                }
                ++fiber.pc;
                break;
            case Opcode::CmpGt:
                if (ins.regOrSlot < fiber.regs.size()) {
                    fiber.regs[ins.regOrSlot] =
                        (fiber.regs[ins.regs.srcA & 7u] > fiber.regs[ins.regs.srcB & 7u]) ? 1 : 0;
                }
                ++fiber.pc;
                break;
            case Opcode::CmpGe:
                if (ins.regOrSlot < fiber.regs.size()) {
                    fiber.regs[ins.regOrSlot] =
                        (fiber.regs[ins.regs.srcA & 7u] >= fiber.regs[ins.regs.srcB & 7u]) ? 1 : 0;
                }
                ++fiber.pc;
                break;
            case Opcode::Jump:
                fiber.pc = ins.imm32;
                break;
            case Opcode::JumpIfZero:
                if (fiber.regs[ins.regOrSlot & 7u] == 0) fiber.pc = ins.imm32;
                else ++fiber.pc;
                break;
            case Opcode::JumpIfNonZero:
                if (fiber.regs[ins.regOrSlot & 7u] != 0) fiber.pc = ins.imm32;
                else ++fiber.pc;
                break;
            case Opcode::Call:
                if (fiber.callDepth < fiber.callStack.size()) {
                    fiber.callStack[fiber.callDepth++] = fiber.pc + 1;
                    fiber.pc = ins.imm32;
                } else {
                    ++fiber.pc;
                }
                break;
            case Opcode::Return:
                if (fiber.callDepth > 0) {
                    fiber.pc = fiber.callStack[--fiber.callDepth];
                } else {
                    out.killed = true;
                    return;
                }
                break;
            case Opcode::RandJump: {
                std::uint8_t roll = fiber.prng.nextByte255();
                if (roll < ins.flags) fiber.pc = ins.imm32;
                else ++fiber.pc;
                break;
            }
            case Opcode::HostCondJump: {
                bool cond = false;
                if (host_ != nullptr) {
                    std::lock_guard<std::mutex> lock(hostMutex);
                    cond = host_->onEvaluateCondition(fiber, ins.flags, ins.regs.srcA, ins.regs.srcB);
                }
                if (cond) fiber.pc = ins.regOrSlot;
                else ++fiber.pc;
                break;
            }
            case Opcode::YieldTicks: {
                std::uint32_t waitTicks = ins.imm32;
                if (ins.flags > 0) {
                    waitTicks = fiber.prng.nextRange(ins.regOrSlot, static_cast<std::uint16_t>(ins.imm32));
                }
                ++fiber.pc;
                out.sleepScheduled = true;
                out.wakeTick = currentTick + (waitTicks > 0 ? waitTicks : 1u);
                return;
            }
            case Opcode::YieldUntilPredicate: {
                ++fiber.pc;
                fiber.status = FiberStatus::WaitingPredicate;
                fiber.waitPredicateChannel = ins.flags;
                fiber.waitTargetKey = ins.regOrSlot;
                fiber.waitRequiredValue = ins.imm32;
                out.predicateWait = true;
                return;
            }
            case Opcode::ForkFiber: {
                out.forked = true;
                out.forkProgramId = fiber.programId;
                out.forkTargetPc = ins.imm32;
                out.forkEntity = fiber.boundEntity;
                out.forkGroup = fiber.ownerGroupId;
                out.forkSeed = fiber.prng.nextUInt32();
                ++fiber.pc;
                out.sleepScheduled = true;
                out.wakeTick = currentTick + 1;
                return;
            }
            case Opcode::KillFiber:
                out.killed = true;
                return;
            case Opcode::SetUnbreakable:
                fiber.unbreakableSection = (ins.flags != 0);
                ++fiber.pc;
                break;
            case Opcode::HostCommand:
                if (host_) {
                    std::lock_guard<std::mutex> lock(hostMutex);
                    host_->onHostCommand(fiber, ins.flags, ins.regOrSlot, ins.imm32);
                }
                ++fiber.pc;
                break;
            default:
                ++fiber.pc;
                break;
            }
        }
        if (fiber.pc >= code.size()) {
            out.killed = true;
        } else if (fiber.status == FiberStatus::Ready) {
            out.sleepScheduled = true;
            out.wakeTick = currentTick + 1;
        }
    };

    const std::size_t numThreads = std::min<std::size_t>(workerCount, batch.size());
    if (numThreads <= 1 || batch.size() <= 8) {
        for (std::size_t idx = 0; idx < batch.size(); ++idx) {
            runIsolatedFiber(idx);
        }
    } else {
        if (!workerPool_) {
            workerPool_ = std::make_shared<ParallelWorkerPool>();
        }
        workerPool_->dispatch(numThreads, [&](std::size_t t, std::size_t total) {
            for (std::size_t idx = t; idx < batch.size(); idx += total) {
                runIsolatedFiber(idx);
            }
        });
    }

    std::uint64_t totalInstr = 0;
    for (const auto& out : outcomes) {
        totalInstr += out.instructions;
        if (out.killed) {
            killFiberTree(out.fiberId);
            continue;
        }
        if (out.sleepScheduled) {
            scheduleAtTick(out.fiberId, out.wakeTick);
        }
        if (out.predicateWait) {
            predicateWaitList_.push_back(out.fiberId);
        }
        if (out.forked) {
            spawnFiberByProgram(
                out.forkProgramId,
                out.forkTargetPc,
                out.forkEntity,
                out.forkGroup,
                out.fiberId,
                out.forkSeed);
        }
    }

    for (std::size_t idx = 0; idx < readyQueue_.size(); ++idx) {
        std::uint32_t fid = readyQueue_[idx];
        if (fid == 0 || fid >= fibers_.size()) continue;
        auto& fiber = fibers_[fid];
        if (fiber.status != FiberStatus::Ready) continue;
        executeFiberSlice(fiber, currentTick, totalInstr);
    }
    readyQueue_.clear();
    return totalInstr;
}

void FiberScheduler::executeFiberSlice(
    FiberContext& fiber,
    std::uint32_t currentTick,
    std::uint64_t& instrCounter) {
    if (fiber.programId == 0 || fiber.programId >= programs_.size()) {
        killFiberTree(fiber.fiberId);
        return;
    }
    const auto& code = programs_[fiber.programId];
    std::uint32_t sliceBudget = 256;

    while (fiber.pc < code.size() && sliceBudget-- > 0) {
        const CompactInstruction& ins = code[fiber.pc];
        ++instrCounter;

        switch (ins.opcode) {
        case Opcode::Nop:
            ++fiber.pc;
            break;

        case Opcode::LoadInt:
        case Opcode::LoadFixed24_8:
            if (ins.regOrSlot < fiber.regs.size()) {
                fiber.regs[ins.regOrSlot] = static_cast<std::int32_t>(ins.imm32);
            }
            ++fiber.pc;
            break;

        case Opcode::MoveReg:
            if (ins.regOrSlot < fiber.regs.size()) {
                fiber.regs[ins.regOrSlot] = fiber.regs[ins.regs.srcA & 7u];
            }
            ++fiber.pc;
            break;

        case Opcode::Add:
            if (ins.regOrSlot < fiber.regs.size()) {
                fiber.regs[ins.regOrSlot] =
                    fiber.regs[ins.regs.srcA & 7u] + fiber.regs[ins.regs.srcB & 7u];
            }
            ++fiber.pc;
            break;

        case Opcode::Sub:
            if (ins.regOrSlot < fiber.regs.size()) {
                fiber.regs[ins.regOrSlot] =
                    fiber.regs[ins.regs.srcA & 7u] - fiber.regs[ins.regs.srcB & 7u];
            }
            ++fiber.pc;
            break;

        case Opcode::Mul:
            if (ins.regOrSlot < fiber.regs.size()) {
                fiber.regs[ins.regOrSlot] =
                    fiber.regs[ins.regs.srcA & 7u] * fiber.regs[ins.regs.srcB & 7u];
            }
            ++fiber.pc;
            break;

        case Opcode::Div:
            if (ins.regOrSlot < fiber.regs.size()) {
                std::int32_t rhs = fiber.regs[ins.regs.srcB & 7u];
                fiber.regs[ins.regOrSlot] = (rhs != 0) ? (fiber.regs[ins.regs.srcA & 7u] / rhs) : 0;
            }
            ++fiber.pc;
            break;

        case Opcode::Mod:
            if (ins.regOrSlot < fiber.regs.size()) {
                std::int32_t rhs = fiber.regs[ins.regs.srcB & 7u];
                fiber.regs[ins.regOrSlot] = (rhs != 0) ? (fiber.regs[ins.regs.srcA & 7u] % rhs) : 0;
            }
            ++fiber.pc;
            break;

        case Opcode::CmpEq:
            if (ins.regOrSlot < fiber.regs.size()) {
                fiber.regs[ins.regOrSlot] =
                    (fiber.regs[ins.regs.srcA & 7u] == fiber.regs[ins.regs.srcB & 7u]) ? 1 : 0;
            }
            ++fiber.pc;
            break;

        case Opcode::CmpNe:
            if (ins.regOrSlot < fiber.regs.size()) {
                fiber.regs[ins.regOrSlot] =
                    (fiber.regs[ins.regs.srcA & 7u] != fiber.regs[ins.regs.srcB & 7u]) ? 1 : 0;
            }
            ++fiber.pc;
            break;

        case Opcode::CmpLt:
            if (ins.regOrSlot < fiber.regs.size()) {
                fiber.regs[ins.regOrSlot] =
                    (fiber.regs[ins.regs.srcA & 7u] < fiber.regs[ins.regs.srcB & 7u]) ? 1 : 0;
            }
            ++fiber.pc;
            break;

        case Opcode::CmpLe:
            if (ins.regOrSlot < fiber.regs.size()) {
                fiber.regs[ins.regOrSlot] =
                    (fiber.regs[ins.regs.srcA & 7u] <= fiber.regs[ins.regs.srcB & 7u]) ? 1 : 0;
            }
            ++fiber.pc;
            break;

        case Opcode::CmpGt:
            if (ins.regOrSlot < fiber.regs.size()) {
                fiber.regs[ins.regOrSlot] =
                    (fiber.regs[ins.regs.srcA & 7u] > fiber.regs[ins.regs.srcB & 7u]) ? 1 : 0;
            }
            ++fiber.pc;
            break;

        case Opcode::CmpGe:
            if (ins.regOrSlot < fiber.regs.size()) {
                fiber.regs[ins.regOrSlot] =
                    (fiber.regs[ins.regs.srcA & 7u] >= fiber.regs[ins.regs.srcB & 7u]) ? 1 : 0;
            }
            ++fiber.pc;
            break;

        case Opcode::Jump:
            fiber.pc = ins.imm32;
            break;

        case Opcode::JumpIfZero:
            if (fiber.regs[ins.regOrSlot & 7u] == 0) {
                fiber.pc = ins.imm32;
            } else {
                ++fiber.pc;
            }
            break;

        case Opcode::JumpIfNonZero:
            if (fiber.regs[ins.regOrSlot & 7u] != 0) {
                fiber.pc = ins.imm32;
            } else {
                ++fiber.pc;
            }
            break;

        case Opcode::Call:
            if (fiber.callDepth < fiber.callStack.size()) {
                fiber.callStack[fiber.callDepth++] = fiber.pc + 1;
                fiber.pc = ins.imm32;
            } else {
                ++fiber.pc;
            }
            break;

        case Opcode::Return:
            if (fiber.callDepth > 0) {
                fiber.pc = fiber.callStack[--fiber.callDepth];
            } else {
                killFiberTree(fiber.fiberId);
                return;
            }
            break;

        case Opcode::RandJump: {
            std::uint8_t roll = fiber.prng.nextByte255();
            if (roll < ins.flags) {
                fiber.pc = ins.imm32;
            } else {
                ++fiber.pc;
            }
            break;
        }

        case Opcode::HostCondJump: {
            bool cond = host_ != nullptr &&
                host_->onEvaluateCondition(fiber, ins.flags, ins.regs.srcA, ins.regs.srcB);
            if (cond) {
                fiber.pc = ins.regOrSlot;
            } else {
                ++fiber.pc;
            }
            break;
        }

        case Opcode::YieldTicks: {
            std::uint32_t waitTicks = ins.imm32;
            if (ins.flags > 0) {
                waitTicks = fiber.prng.nextRange(ins.regOrSlot, static_cast<std::uint16_t>(ins.imm32));
            }
            ++fiber.pc;
            scheduleAtTick(fiber.fiberId, currentTick + (waitTicks > 0 ? waitTicks : 1u));
            return;
        }

        case Opcode::YieldUntilPredicate: {
            ++fiber.pc;
            fiber.status = FiberStatus::WaitingPredicate;
            fiber.waitPredicateChannel = ins.flags;
            fiber.waitTargetKey = ins.regOrSlot;
            fiber.waitRequiredValue = ins.imm32;
            predicateWaitList_.push_back(fiber.fiberId);
            return;
        }

        case Opcode::ForkFiber: {
            std::uint32_t targetPc = ins.imm32;
            std::uint32_t parentId = fiber.fiberId;
            std::uint32_t progId = fiber.programId;
            GenerationalEntityHandle ent = fiber.boundEntity;
            std::uint8_t grp = fiber.ownerGroupId;
            std::uint32_t childSeed = fiber.prng.nextUInt32();
            ++fiber.pc;
            // spawnFiberByProgram may reallocate `fibers_`, so update `fiber.pc` beforehand
            scheduleAtTick(parentId, currentTick + 1);
            spawnFiberByProgram(progId, targetPc, ent, grp, parentId, childSeed);
            return;
        }

        case Opcode::KillFiber:
            killFiberTree(fiber.fiberId);
            return;

        case Opcode::SetUnbreakable:
            fiber.unbreakableSection = (ins.flags != 0);
            ++fiber.pc;
            break;

        case Opcode::HostCommand:
            if (host_) {
                host_->onHostCommand(fiber, ins.flags, ins.regOrSlot, ins.imm32);
            }
            ++fiber.pc;
            break;

        default:
            ++fiber.pc;
            break;
        }
    }

    if (fiber.pc >= code.size()) {
        killFiberTree(fiber.fiberId);
    } else if (fiber.status == FiberStatus::Ready) {
        // Re-schedule on next tick if slice budget was reached
        scheduleAtTick(fiber.fiberId, currentTick + 1);
    }
}

std::uint32_t FiberScheduler::notifyBlackboardEvent(
    std::uint8_t ownerGroupId,
    std::uint8_t predicateChannel,
    std::uint16_t targetKey,
    std::uint32_t currentValue) {
    std::uint32_t woken = 0;
    std::vector<std::uint32_t> stillWaiting;
    for (std::uint32_t fid : predicateWaitList_) {
        if (fid == 0 || fid >= fibers_.size()) continue;
        auto& f = fibers_[fid];
        if (f.status != FiberStatus::WaitingPredicate) continue;
        if (f.ownerGroupId == ownerGroupId &&
            f.waitPredicateChannel == predicateChannel &&
            f.waitTargetKey == targetKey &&
            currentValue >= f.waitRequiredValue) {
            f.status = FiberStatus::Ready;
            f.waitPredicateChannel = 0;
            readyQueue_.push_back(fid);
            ++woken;
        } else {
            stillWaiting.push_back(fid);
        }
    }
    predicateWaitList_.swap(stillWaiting);
    return woken;
}

std::size_t FiberScheduler::activeFiberCount() const noexcept {
    std::size_t cnt = 0;
    for (std::size_t i = 1; i < fibers_.size(); ++i) {
        if (fibers_[i].status != FiberStatus::Inactive &&
            fibers_[i].status != FiberStatus::Terminated) {
            ++cnt;
        }
    }
    return cnt;
}

const FiberContext* FiberScheduler::getFiber(std::uint32_t fiberId) const noexcept {
    if (fiberId == 0 || fiberId >= fibers_.size()) return nullptr;
    return &fibers_[fiberId];
}

std::uint64_t FiberScheduler::computeLockstepDigest() const noexcept {
    // 64-bit FNV-1a hash across all active fiber states for deterministic replay & netcode check
    std::uint64_t hash = 14695981039346656037ULL;
    auto mix = [&hash](std::uint64_t v) {
        hash ^= v;
        hash *= 1099511628211ULL;
    };
    mix(currentTick_);
    for (std::size_t i = 1; i < fibers_.size(); ++i) {
        const auto& f = fibers_[i];
        if (f.status == FiberStatus::Terminated || f.status == FiberStatus::Inactive) continue;
        mix(f.fiberId);
        mix(f.programId);
        mix(static_cast<std::uint64_t>(f.status));
        mix(f.pc);
        mix(f.wakeTick);
        mix(f.prng.state);
        mix(f.boundEntity.pack64());
    }
    return hash;
}

std::vector<std::uint8_t> FiberScheduler::captureBinarySnapshot() const {
    static_assert(std::is_trivially_copyable_v<FiberContext>,
                  "FiberContext must be trivially copyable with no raw pointers");
    std::vector<std::uint8_t> buf;
    std::uint32_t count = static_cast<std::uint32_t>(fibers_.size());
    buf.resize(sizeof(std::uint32_t) * 2 + count * sizeof(FiberContext));
    std::memcpy(buf.data(), &currentTick_, sizeof(std::uint32_t));
    std::memcpy(buf.data() + sizeof(std::uint32_t), &count, sizeof(std::uint32_t));
    std::memcpy(buf.data() + sizeof(std::uint32_t) * 2, fibers_.data(), count * sizeof(FiberContext));
    return buf;
}

void FiberScheduler::restoreBinarySnapshot(const std::vector<std::uint8_t>& snapshot) {
    if (snapshot.size() < sizeof(std::uint32_t) * 2) return;
    std::uint32_t count = 0;
    std::memcpy(&currentTick_, snapshot.data(), sizeof(std::uint32_t));
    std::memcpy(&count, snapshot.data() + sizeof(std::uint32_t), sizeof(std::uint32_t));
    if (snapshot.size() < sizeof(std::uint32_t) * 2 + count * sizeof(FiberContext)) return;

    fibers_.resize(count);
    std::memcpy(fibers_.data(), snapshot.data() + sizeof(std::uint32_t) * 2, count * sizeof(FiberContext));

    readyQueue_.clear();
    predicateWaitList_.clear();
    freeFiberIds_.clear();
    for (auto& slot : timingWheel_) slot.clear();

    for (std::uint32_t i = 1; i < count; ++i) {
        const auto& f = fibers_[i];
        if (f.status == FiberStatus::Ready) {
            readyQueue_.push_back(i);
        } else if (f.status == FiberStatus::SleepingTicks) {
            timingWheel_[static_cast<std::size_t>(f.wakeTick) & WHEEL_MASK].push_back(i);
        } else if (f.status == FiberStatus::WaitingPredicate) {
            predicateWaitList_.push_back(i);
        } else {
            freeFiberIds_.push_back(i);
        }
    }
}

namespace {

struct HostCondExpr {
    bool isRand{false};
    bool negated{false};
    std::uint8_t kindOrThreshold{0};
    std::uint16_t key16{0};
    std::uint16_t threshold16{0};
};

struct BlockPatch {
    std::size_t instrIndex{0};
    mir::BlockId targetBlock{0};
    bool patchRegOrSlot{false};
};

struct FunctionPatch {
    std::size_t instrIndex{0};
    std::string targetFunction;
};

[[nodiscard]] bool isFiberIntrinsic(const std::string& symbolName, const char* methodSuffix) {
    const std::string full = std::string("RealScript.Fiber::Fiber.") + methodSuffix;
    return symbolName == full;
}

} // namespace

CompiledFiberModule FiberCompiler::compileModule(const realscript::mir::Module& module) {
    CompiledFiberModule result;
    result.moduleName = module.name;

    std::unordered_map<std::string, const mir::Function*> funcMap;
    for (const auto& fn : module.functions) {
        funcMap[fn.name] = &fn;
        if (!module.name.empty()) {
            funcMap[module.name + "::" + fn.name] = &fn;
        }
    }

    std::vector<FunctionPatch> functionPatches;

    for (const auto& fn : module.functions) {
        if (fn.blocks.empty()) continue;

        const std::uint32_t fnEntryPc = static_cast<std::uint32_t>(result.instructions.size());
        result.entryPoints[fn.name] = fnEntryPc;
        if (!module.name.empty()) {
            result.entryPoints[module.name + "::" + fn.name] = fnEntryPc;
        }

        // Count local writes to distinguish compile-time constant locals from loop/dynamic locals
        std::unordered_map<std::size_t, std::size_t> localWriteCounts;
        for (const auto& blk : fn.blocks) {
            for (const auto& ins : blk.instructions) {
                if (ins.opcode == mir::Opcode::StoreLocal) {
                    ++localWriteCounts[ins.localIndex];
                }
            }
        }

        // First pass: quick constant evaluation for reachability (eliminates dead exit blocks after `while (true)`)
        std::unordered_map<mir::ValueId, std::int64_t> globalConstVals;
        for (const auto& blk : fn.blocks) {
            for (const auto& ins : blk.instructions) {
                if (ins.opcode == mir::Opcode::ConstantInt) {
                    globalConstVals[ins.result] = ins.integerImmediate;
                } else if (ins.opcode == mir::Opcode::ConstantBool) {
                    globalConstVals[ins.result] = ins.boolImmediate ? 1 : 0;
                }
            }
        }

        std::unordered_map<mir::BlockId, std::size_t> blockIndexById;
        for (std::size_t i = 0; i < fn.blocks.size(); ++i) {
            blockIndexById[fn.blocks[i].id] = i;
        }

        std::unordered_set<mir::BlockId> reachableBlocks;
        std::vector<mir::BlockId> worklist;
        reachableBlocks.insert(fn.blocks[0].id);
        worklist.push_back(fn.blocks[0].id);
        while (!worklist.empty()) {
            mir::BlockId bid = worklist.back();
            worklist.pop_back();
            auto it = blockIndexById.find(bid);
            if (it == blockIndexById.end()) continue;
            const auto& term = fn.blocks[it->second].terminator;
            if (term.kind == mir::TerminatorKind::Jump) {
                if (reachableBlocks.insert(term.target).second) {
                    worklist.push_back(term.target);
                }
            } else if (term.kind == mir::TerminatorKind::Branch) {
                auto cIt = globalConstVals.find(term.condition);
                if (cIt != globalConstVals.end()) {
                    mir::BlockId taken = (cIt->second != 0) ? term.target : term.falseTarget;
                    if (reachableBlocks.insert(taken).second) {
                        worklist.push_back(taken);
                    }
                } else {
                    if (reachableBlocks.insert(term.target).second) {
                        worklist.push_back(term.target);
                    }
                    if (reachableBlocks.insert(term.falseTarget).second) {
                        worklist.push_back(term.falseTarget);
                    }
                }
            }
        }

        std::vector<const mir::BasicBlock*> activeBlocks;
        for (const auto& blk : fn.blocks) {
            if (reachableBlocks.find(blk.id) != reachableBlocks.end()) {
                activeBlocks.push_back(&blk);
            }
        }

        std::unordered_map<mir::BlockId, std::uint32_t> blockStartPc;
        std::vector<BlockPatch> blockPatches;

        std::unordered_map<mir::ValueId, std::int64_t> constInt;
        std::unordered_map<mir::ValueId, std::string> constStr;
        std::unordered_map<std::size_t, std::int64_t> constLocal;
        std::unordered_map<mir::ValueId, std::uint16_t> valReg;
        std::unordered_map<mir::ValueId, HostCondExpr> condExprs;

        auto evalConstInt = [&](mir::ValueId vid, std::int64_t fallback = 0) -> std::int64_t {
            auto it = constInt.find(vid);
            return it != constInt.end() ? it->second : fallback;
        };

        auto ensureValReg = [&](mir::ValueId vid, std::uint16_t scratchReg) -> std::uint16_t {
            auto rIt = valReg.find(vid);
            if (rIt != valReg.end()) return rIt->second;
            std::int64_t cval = evalConstInt(vid, 0);
            result.instructions.push_back(CompactInstruction(
                Opcode::LoadInt, std::uint8_t{0}, scratchReg, static_cast<std::uint32_t>(static_cast<std::int32_t>(cval))));
            return scratchReg;
        };

        // Helper lambda to emit instructions for a basic block (supports 1-level helper inlining)
        auto emitInstructions = [&](const std::vector<mir::Instruction>& insts,
                                    const std::unordered_map<std::size_t, std::int64_t>* inlinedParamConsts,
                                    auto& selfRef) -> void {
            for (const auto& ins : insts) {
                switch (ins.opcode) {
                case mir::Opcode::Parameter: {
                    if (inlinedParamConsts) {
                        auto pIt = inlinedParamConsts->find(ins.localIndex);
                        if (pIt != inlinedParamConsts->end()) {
                            constInt[ins.result] = pIt->second;
                        }
                    }
                    valReg[ins.result] = static_cast<std::uint16_t>(ins.localIndex & 7u);
                    break;
                }
                case mir::Opcode::ConstantInt:
                    constInt[ins.result] = ins.integerImmediate;
                    break;
                case mir::Opcode::ConstantBool:
                    constInt[ins.result] = ins.boolImmediate ? 1 : 0;
                    break;
                case mir::Opcode::ConstantString:
                    constStr[ins.result] = ins.stringImmediate;
                    break;
                case mir::Opcode::LoadLocal: {
                    if (inlinedParamConsts) {
                        auto pIt = inlinedParamConsts->find(ins.localIndex);
                        if (pIt != inlinedParamConsts->end()) {
                            constInt[ins.result] = pIt->second;
                            break;
                        }
                    }
                    auto cIt = constLocal.find(ins.localIndex);
                    if (cIt != constLocal.end() && localWriteCounts[ins.localIndex] <= 1) {
                        constInt[ins.result] = cIt->second;
                    }
                    valReg[ins.result] = static_cast<std::uint16_t>(ins.localIndex & 7u);
                    break;
                }
                case mir::Opcode::StoreLocal: {
                    if (ins.operands.empty()) break;
                    mir::ValueId src = ins.operands[0];
                    std::uint16_t dstReg = static_cast<std::uint16_t>(ins.localIndex & 7u);
                    auto cIt = constInt.find(src);
                    if (cIt != constInt.end()) {
                        if (localWriteCounts[ins.localIndex] <= 1) {
                            constLocal[ins.localIndex] = cIt->second;
                        } else {
                            result.instructions.push_back(CompactInstruction(
                                Opcode::LoadInt,
                                std::uint8_t{0},
                                dstReg,
                                static_cast<std::uint32_t>(static_cast<std::int32_t>(cIt->second))));
                        }
                    } else if (valReg.find(src) != valReg.end()) {
                        std::uint16_t srcReg = valReg[src];
                        if (srcReg != dstReg) {
                            result.instructions.push_back(
                                CompactInstruction(Opcode::MoveReg, std::uint8_t{0}, dstReg, srcReg, std::uint16_t{0}));
                        }
                    }
                    break;
                }
                case mir::Opcode::NegateInt:
                case mir::Opcode::NegateLong: {
                    if (!ins.operands.empty() && constInt.find(ins.operands[0]) != constInt.end()) {
                        constInt[ins.result] = -constInt[ins.operands[0]];
                    }
                    break;
                }
                case mir::Opcode::LogicalNot: {
                    if (ins.operands.empty()) break;
                    mir::ValueId src = ins.operands[0];
                    if (constInt.find(src) != constInt.end()) {
                        constInt[ins.result] = (constInt[src] == 0) ? 1 : 0;
                    }
                    auto condIt = condExprs.find(src);
                    if (condIt != condExprs.end()) {
                        HostCondExpr neg = condIt->second;
                        neg.negated = !neg.negated;
                        condExprs[ins.result] = neg;
                    }
                    break;
                }
                case mir::Opcode::AddInt:
                case mir::Opcode::SubtractInt:
                case mir::Opcode::MultiplyInt:
                case mir::Opcode::DivideInt:
                case mir::Opcode::RemainderInt:
                case mir::Opcode::Equal:
                case mir::Opcode::NotEqual:
                case mir::Opcode::LessInt:
                case mir::Opcode::LessOrEqualInt:
                case mir::Opcode::GreaterInt:
                case mir::Opcode::GreaterOrEqualInt: {
                    if (ins.operands.size() < 2) break;
                    mir::ValueId lhs = ins.operands[0];
                    mir::ValueId rhs = ins.operands[1];
                    bool lhsConst = constInt.find(lhs) != constInt.end();
                    bool rhsConst = constInt.find(rhs) != constInt.end();
                    if (lhsConst && rhsConst) {
                        std::int64_t a = constInt[lhs];
                        std::int64_t b = constInt[rhs];
                        switch (ins.opcode) {
                        case mir::Opcode::AddInt: constInt[ins.result] = a + b; break;
                        case mir::Opcode::SubtractInt: constInt[ins.result] = a - b; break;
                        case mir::Opcode::MultiplyInt: constInt[ins.result] = a * b; break;
                        case mir::Opcode::DivideInt: constInt[ins.result] = (b != 0) ? (a / b) : 0; break;
                        case mir::Opcode::RemainderInt: constInt[ins.result] = (b != 0) ? (a % b) : 0; break;
                        case mir::Opcode::Equal: constInt[ins.result] = (a == b) ? 1 : 0; break;
                        case mir::Opcode::NotEqual: constInt[ins.result] = (a != b) ? 1 : 0; break;
                        case mir::Opcode::LessInt: constInt[ins.result] = (a < b) ? 1 : 0; break;
                        case mir::Opcode::LessOrEqualInt: constInt[ins.result] = (a <= b) ? 1 : 0; break;
                        case mir::Opcode::GreaterInt: constInt[ins.result] = (a > b) ? 1 : 0; break;
                        case mir::Opcode::GreaterOrEqualInt: constInt[ins.result] = (a >= b) ? 1 : 0; break;
                        default: break;
                        }
                    } else {
                        std::uint16_t rA = ensureValReg(lhs, 5);
                        std::uint16_t rB = ensureValReg(rhs, 6);
                        std::uint16_t rDst = 7;
                        Opcode op = Opcode::Add;
                        switch (ins.opcode) {
                        case mir::Opcode::AddInt: op = Opcode::Add; break;
                        case mir::Opcode::SubtractInt: op = Opcode::Sub; break;
                        case mir::Opcode::MultiplyInt: op = Opcode::Mul; break;
                        case mir::Opcode::DivideInt: op = Opcode::Div; break;
                        case mir::Opcode::RemainderInt: op = Opcode::Mod; break;
                        case mir::Opcode::Equal: op = Opcode::CmpEq; break;
                        case mir::Opcode::NotEqual: op = Opcode::CmpNe; break;
                        case mir::Opcode::LessInt: op = Opcode::CmpLt; break;
                        case mir::Opcode::LessOrEqualInt: op = Opcode::CmpLe; break;
                        case mir::Opcode::GreaterInt: op = Opcode::CmpGt; break;
                        case mir::Opcode::GreaterOrEqualInt: op = Opcode::CmpGe; break;
                        default: break;
                        }
                        result.instructions.push_back(CompactInstruction(op, std::uint8_t{0}, rDst, rA, rB));
                        valReg[ins.result] = rDst;
                    }
                    break;
                }
                case mir::Opcode::Call: {
                    if (isFiberIntrinsic(ins.symbolName, "Yield") && ins.operands.size() >= 1) {
                        result.instructions.push_back(CompactInstruction(
                            Opcode::YieldTicks,
                            std::uint8_t{0},
                            std::uint16_t{0},
                            static_cast<std::uint32_t>(evalConstInt(ins.operands[0], 1))));
                    } else if (isFiberIntrinsic(ins.symbolName, "YieldRandom") && ins.operands.size() >= 2) {
                        result.instructions.push_back(CompactInstruction(
                            Opcode::YieldTicks,
                            std::uint8_t{1},
                            static_cast<std::uint16_t>(evalConstInt(ins.operands[0], 1)),
                            static_cast<std::uint32_t>(evalConstInt(ins.operands[1], 1))));
                    } else if (isFiberIntrinsic(ins.symbolName, "WaitUntil") && ins.operands.size() >= 3) {
                        result.instructions.push_back(CompactInstruction(
                            Opcode::YieldUntilPredicate,
                            static_cast<std::uint8_t>(evalConstInt(ins.operands[0], 0)),
                            static_cast<std::uint16_t>(evalConstInt(ins.operands[1], 0)),
                            static_cast<std::uint32_t>(evalConstInt(ins.operands[2], 0))));
                    } else if (isFiberIntrinsic(ins.symbolName, "SetUnbreakable") && ins.operands.size() >= 1) {
                        result.instructions.push_back(CompactInstruction(
                            Opcode::SetUnbreakable,
                            static_cast<std::uint8_t>(evalConstInt(ins.operands[0], 0) != 0 ? 1 : 0),
                            std::uint16_t{0},
                            std::uint32_t{0}));
                    } else if (isFiberIntrinsic(ins.symbolName, "Terminate")) {
                        result.instructions.push_back(
                            CompactInstruction(Opcode::KillFiber, std::uint8_t{0}, std::uint16_t{0}, std::uint32_t{0}));
                    } else if (isFiberIntrinsic(ins.symbolName, "Fork") && ins.operands.size() >= 1) {
                        std::size_t idx = result.instructions.size();
                        result.instructions.push_back(
                            CompactInstruction(Opcode::ForkFiber, std::uint8_t{0}, std::uint16_t{0}, std::uint32_t{0}));
                        auto sIt = constStr.find(ins.operands[0]);
                        if (sIt != constStr.end()) {
                            functionPatches.push_back(FunctionPatch{idx, sIt->second});
                        }
                    } else if (isFiberIntrinsic(ins.symbolName, "ForkPc") && ins.operands.size() >= 1) {
                        result.instructions.push_back(CompactInstruction(
                            Opcode::ForkFiber,
                            std::uint8_t{0},
                            std::uint16_t{0},
                            static_cast<std::uint32_t>(evalConstInt(ins.operands[0], 0))));
                    } else if (isFiberIntrinsic(ins.symbolName, "Command") && ins.operands.size() >= 3) {
                        result.instructions.push_back(CompactInstruction(
                            Opcode::HostCommand,
                            static_cast<std::uint8_t>(evalConstInt(ins.operands[0], 0)),
                            static_cast<std::uint16_t>(evalConstInt(ins.operands[1], 0)),
                            static_cast<std::uint32_t>(evalConstInt(ins.operands[2], 0))));
                    } else if (isFiberIntrinsic(ins.symbolName, "Cond") && ins.operands.size() >= 3) {
                        HostCondExpr ce{};
                        ce.isRand = false;
                        ce.negated = false;
                        ce.kindOrThreshold = static_cast<std::uint8_t>(evalConstInt(ins.operands[0], 0));
                        ce.key16 = static_cast<std::uint16_t>(evalConstInt(ins.operands[1], 0));
                        ce.threshold16 = static_cast<std::uint16_t>(evalConstInt(ins.operands[2], 0));
                        condExprs[ins.result] = ce;
                    } else if (isFiberIntrinsic(ins.symbolName, "Chance") && ins.operands.size() >= 1) {
                        HostCondExpr ce{};
                        ce.isRand = true;
                        ce.negated = false;
                        ce.kindOrThreshold = static_cast<std::uint8_t>(evalConstInt(ins.operands[0], 0));
                        condExprs[ins.result] = ce;
                    } else if (isFiberIntrinsic(ins.symbolName, "GetReg") && ins.operands.size() >= 1) {
                        valReg[ins.result] = static_cast<std::uint16_t>(evalConstInt(ins.operands[0], 0) & 7);
                    } else if (isFiberIntrinsic(ins.symbolName, "SetReg") && ins.operands.size() >= 2) {
                        std::uint16_t slot = static_cast<std::uint16_t>(evalConstInt(ins.operands[0], 0) & 7);
                        if (constInt.find(ins.operands[1]) != constInt.end()) {
                            result.instructions.push_back(CompactInstruction(
                                Opcode::LoadInt,
                                std::uint8_t{0},
                                slot,
                                static_cast<std::uint32_t>(static_cast<std::int32_t>(constInt[ins.operands[1]]))));
                        } else if (valReg.find(ins.operands[1]) != valReg.end()) {
                            result.instructions.push_back(CompactInstruction(
                                Opcode::MoveReg, std::uint8_t{0}, slot, valReg[ins.operands[1]], std::uint16_t{0}));
                        }
                    } else {
                        // Check if target is a single-block helper function in the same module
                        auto targetIt = funcMap.find(ins.symbolName);
                        if (targetIt != funcMap.end() &&
                            targetIt->second != &fn &&
                            targetIt->second->blocks.size() == 1) {
                            std::unordered_map<std::size_t, std::int64_t> paramConsts;
                            for (std::size_t p = 0; p < ins.operands.size(); ++p) {
                                if (constInt.find(ins.operands[p]) != constInt.end()) {
                                    paramConsts[p] = constInt[ins.operands[p]];
                                }
                            }
                            selfRef(targetIt->second->blocks[0].instructions, &paramConsts, selfRef);
                        } else if (targetIt != funcMap.end()) {
                            std::size_t idx = result.instructions.size();
                            result.instructions.push_back(
                                CompactInstruction(Opcode::Call, std::uint8_t{0}, std::uint16_t{0}, std::uint32_t{0}));
                            functionPatches.push_back(FunctionPatch{idx, ins.symbolName});
                        }
                    }
                    break;
                }
                default:
                    break;
                }
            }
        };

        for (std::size_t bIdx = 0; bIdx < activeBlocks.size(); ++bIdx) {
            const mir::BasicBlock& blk = *activeBlocks[bIdx];
            blockStartPc[blk.id] = static_cast<std::uint32_t>(result.instructions.size());
            const bool hasNextBlock = (bIdx + 1 < activeBlocks.size());
            const mir::BlockId nextBlockId = hasNextBlock ? activeBlocks[bIdx + 1]->id : 0xFFFFFFFFu;

            emitInstructions(blk.instructions, nullptr, emitInstructions);

            const auto& term = blk.terminator;
            switch (term.kind) {
            case mir::TerminatorKind::Jump: {
                if (!hasNextBlock || term.target != nextBlockId) {
                    std::size_t patchIdx = result.instructions.size();
                    result.instructions.push_back(
                        CompactInstruction(Opcode::Jump, std::uint8_t{0}, std::uint16_t{0}, std::uint32_t{0}));
                    blockPatches.push_back(BlockPatch{patchIdx, term.target, false});
                }
                break;
            }
            case mir::TerminatorKind::Branch: {
                // 1. Check compile-time constant condition (e.g. `while (true)`)
                auto cIt = constInt.find(term.condition);
                if (cIt != constInt.end()) {
                    mir::BlockId taken = (cIt->second != 0) ? term.target : term.falseTarget;
                    if (!hasNextBlock || taken != nextBlockId) {
                        std::size_t patchIdx = result.instructions.size();
                        result.instructions.push_back(
                            CompactInstruction(Opcode::Jump, std::uint8_t{0}, std::uint16_t{0}, std::uint32_t{0}));
                        blockPatches.push_back(BlockPatch{patchIdx, taken, false});
                    }
                    break;
                }

                // 2. Check `Fiber.Cond(...)` or `Fiber.Chance(...)`
                auto condIt = condExprs.find(term.condition);
                if (condIt != condExprs.end()) {
                    const HostCondExpr& ce = condIt->second;
                    // When host condition is TRUE, jumpTarget is `term.target` (or `term.falseTarget` if negated)
                    mir::BlockId jumpOnHostTrue = ce.negated ? term.falseTarget : term.target;
                    mir::BlockId fallthroughOnHostFalse = ce.negated ? term.target : term.falseTarget;

                    std::size_t condIdx = result.instructions.size();
                    if (ce.isRand) {
                        result.instructions.push_back(CompactInstruction(
                            Opcode::RandJump, ce.kindOrThreshold, std::uint16_t{0}, std::uint32_t{0}));
                        blockPatches.push_back(BlockPatch{condIdx, jumpOnHostTrue, false});
                    } else {
                        result.instructions.push_back(CompactInstruction(
                            Opcode::HostCondJump, ce.kindOrThreshold, std::uint16_t{0}, ce.key16, ce.threshold16));
                        blockPatches.push_back(BlockPatch{condIdx, jumpOnHostTrue, true});
                    }

                    if (!hasNextBlock || fallthroughOnHostFalse != nextBlockId) {
                        std::size_t jmpIdx = result.instructions.size();
                        result.instructions.push_back(
                            CompactInstruction(Opcode::Jump, std::uint8_t{0}, std::uint16_t{0}, std::uint32_t{0}));
                        blockPatches.push_back(BlockPatch{jmpIdx, fallthroughOnHostFalse, false});
                    }
                    break;
                }

                // 3. General register condition (`while (step < 3)`, `if (a == b)`)
                std::uint16_t condReg = ensureValReg(term.condition, 7);
                if (hasNextBlock && term.target == nextBlockId) {
                    std::size_t jzIdx = result.instructions.size();
                    result.instructions.push_back(
                        CompactInstruction(Opcode::JumpIfZero, std::uint8_t{0}, condReg, std::uint32_t{0}));
                    blockPatches.push_back(BlockPatch{jzIdx, term.falseTarget, false});
                } else {
                    std::size_t jnzIdx = result.instructions.size();
                    result.instructions.push_back(
                        CompactInstruction(Opcode::JumpIfNonZero, std::uint8_t{0}, condReg, std::uint32_t{0}));
                    blockPatches.push_back(BlockPatch{jnzIdx, term.target, false});
                    if (!hasNextBlock || term.falseTarget != nextBlockId) {
                        std::size_t jmpIdx = result.instructions.size();
                        result.instructions.push_back(
                            CompactInstruction(Opcode::Jump, std::uint8_t{0}, std::uint16_t{0}, std::uint32_t{0}));
                        blockPatches.push_back(BlockPatch{jmpIdx, term.falseTarget, false});
                    }
                }
                break;
            }
            case mir::TerminatorKind::ReturnVoid:
            case mir::TerminatorKind::ReturnValue: {
                result.instructions.push_back(
                    CompactInstruction(Opcode::Return, std::uint8_t{0}, std::uint16_t{0}, std::uint32_t{0}));
                break;
            }
            default:
                break;
            }
        }

        // Resolve basic-block PC relocations for this function
        for (const auto& patch : blockPatches) {
            auto bIt = blockStartPc.find(patch.targetBlock);
            if (bIt == blockStartPc.end()) continue;
            if (patch.patchRegOrSlot) {
                result.instructions[patch.instrIndex].regOrSlot = static_cast<std::uint16_t>(bIt->second);
            } else {
                result.instructions[patch.instrIndex].imm32 = bIt->second;
            }
        }
    }

    // Resolve cross-function PC relocations (`Fiber.Fork("...")` and subroutine `Call`)
    for (const auto& fpatch : functionPatches) {
        auto fIt = result.entryPoints.find(fpatch.targetFunction);
        if (fIt != result.entryPoints.end()) {
            result.instructions[fpatch.instrIndex].imm32 = fIt->second;
        }
    }

    return result;
}

} // namespace realscript::runtime
