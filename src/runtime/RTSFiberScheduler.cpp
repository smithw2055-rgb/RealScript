#include "realscript/runtime/RTSFiberScheduler.h"

#include <cstring>
#include <utility>

namespace realscript::runtime {

using Opcode = FiberOpcode;

RTSFiberScheduler::RTSFiberScheduler(IFiberHostCallbacks* hostCallbacks) noexcept
    : host_(hostCallbacks) {
    programs_.reserve(64);
    externalStreamCache_.reserve(64);
    // Reserve index 0 as invalid program sentinel
    programs_.push_back({});
    externalStreamCache_.push_back(nullptr);

    fibers_.reserve(2048);
    // Reserve index 0 as sentinel
    fibers_.push_back(RTSFiberContext{});
}

uint32_t RTSFiberScheduler::registerProgram(std::vector<CompactInstruction> codeStream) {
    uint32_t progId = static_cast<uint32_t>(programs_.size());
    programs_.push_back(std::move(codeStream));
    externalStreamCache_.push_back(nullptr);
    return progId;
}

uint32_t RTSFiberScheduler::spawnFiberByProgram(
    uint32_t programId,
    uint32_t entryPc,
    GenerationalEntityHandle entity,
    uint8_t ownerGroupId,
    uint32_t parentFiberId,
    uint32_t seed) {
    uint32_t id = 0;
    if (!freeFiberIds_.empty()) {
        id = freeFiberIds_.back();
        freeFiberIds_.pop_back();
    } else {
        id = static_cast<uint32_t>(fibers_.size());
        fibers_.push_back(RTSFiberContext{});
    }

    RTSFiberContext ctx{};
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

uint32_t RTSFiberScheduler::spawnFiber(
    const std::vector<CompactInstruction>& codeStream,
    uint32_t entryPc,
    GenerationalEntityHandle entity,
    uint8_t ownerGroupId,
    uint32_t parentFiberId,
    uint32_t seed) {
    uint32_t programId = 0;
    for (size_t i = 1; i < externalStreamCache_.size(); ++i) {
        if (externalStreamCache_[i] == &codeStream) {
            programId = static_cast<uint32_t>(i);
            break;
        }
    }
    if (programId == 0) {
        programId = static_cast<uint32_t>(programs_.size());
        programs_.push_back(codeStream);
        externalStreamCache_.push_back(&codeStream);
    }
    return spawnFiberByProgram(programId, entryPc, entity, ownerGroupId, parentFiberId, seed);
}

bool RTSFiberScheduler::interruptFiber(uint32_t fiberId, uint32_t newEntryPc, bool force) noexcept {
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

void RTSFiberScheduler::killFiberTree(uint32_t rootFiberId) noexcept {
    if (rootFiberId == 0 || rootFiberId >= fibers_.size()) return;
    if (fibers_[rootFiberId].status == FiberStatus::Terminated ||
        fibers_[rootFiberId].status == FiberStatus::Inactive) {
        return;
    }
    fibers_[rootFiberId].status = FiberStatus::Terminated;
    fibers_[rootFiberId].unbreakableSection = false;
    freeFiberIds_.push_back(rootFiberId);

    for (size_t i = 1; i < fibers_.size(); ++i) {
        if (fibers_[i].parentFiberId == rootFiberId &&
            fibers_[i].status != FiberStatus::Terminated &&
            fibers_[i].status != FiberStatus::Inactive) {
            killFiberTree(static_cast<uint32_t>(i));
        }
    }
}

void RTSFiberScheduler::scheduleAtTick(uint32_t fiberId, uint32_t wakeTick) {
    fibers_[fiberId].status = FiberStatus::SleepingTicks;
    fibers_[fiberId].wakeTick = wakeTick;
    size_t slot = static_cast<size_t>(wakeTick) & WHEEL_MASK;
    timingWheel_[slot].push_back(fiberId);
}

uint64_t RTSFiberScheduler::stepTick(uint32_t currentTick) {
    currentTick_ = currentTick;
    uint64_t instrExecuted = 0;

    // 1. Pop all fibers due in the current O(1) Timing Wheel slot
    size_t slot = static_cast<size_t>(currentTick) & WHEEL_MASK;
    auto& bucket = timingWheel_[slot];
    std::vector<uint32_t> remainingInBucket;
    for (uint32_t fid : bucket) {
        if (fid >= fibers_.size()) continue;
        auto& f = fibers_[fid];
        if (f.status == FiberStatus::SleepingTicks) {
            if (f.wakeTick <= currentTick) {
                f.status = FiberStatus::Ready;
                readyQueue_.push_back(fid);
            } else {
                // Multi-rotation long wait (> 4096 ticks)
                remainingInBucket.push_back(fid);
            }
        }
    }
    bucket.swap(remainingInBucket);

    // 2. Drain readyQueue_ (note: ForkFiber may append new child fibers during loop)
    for (size_t idx = 0; idx < readyQueue_.size(); ++idx) {
        uint32_t fid = readyQueue_[idx];
        if (fid == 0 || fid >= fibers_.size()) continue;
        auto& fiber = fibers_[fid];
        if (fiber.status != FiberStatus::Ready) continue;
        executeFiberSlice(fiber, currentTick, instrExecuted);
    }
    readyQueue_.clear();

    return instrExecuted;
}

void RTSFiberScheduler::executeFiberSlice(
    RTSFiberContext& fiber,
    uint32_t currentTick,
    uint64_t& instrCounter) {
    if (fiber.programId == 0 || fiber.programId >= programs_.size()) {
        killFiberTree(fiber.fiberId);
        return;
    }
    const auto& code = programs_[fiber.programId];
    uint32_t sliceBudget = 256;

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
                fiber.regs[ins.regOrSlot] = static_cast<int32_t>(ins.imm32);
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

        case Opcode::Jump:
            fiber.pc = ins.imm32;
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
            uint8_t roll = fiber.prng.nextByte255();
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
            uint32_t waitTicks = ins.imm32;
            if (ins.flags > 0) {
                // Random jitter wait (`waitrand(min, max)` where min=regOrSlot, max=imm32)
                waitTicks = fiber.prng.nextRange(ins.regOrSlot, static_cast<uint16_t>(ins.imm32));
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
            uint32_t targetPc = ins.imm32;
            uint32_t parentId = fiber.fiberId;
            uint32_t progId = fiber.programId;
            GenerationalEntityHandle ent = fiber.boundEntity;
            uint8_t grp = fiber.ownerGroupId;
            uint32_t childSeed = fiber.prng.nextUInt32();
            ++fiber.pc;
            // Note: spawnFiberByProgram may reallocate `fibers_`, so we update `fiber.pc` beforehand
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

uint32_t RTSFiberScheduler::notifyBlackboardEvent(
    uint8_t ownerGroupId,
    uint8_t predicateChannel,
    uint16_t targetKey,
    uint32_t currentValue) {
    uint32_t woken = 0;
    std::vector<uint32_t> stillWaiting;
    for (uint32_t fid : predicateWaitList_) {
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

size_t RTSFiberScheduler::activeFiberCount() const noexcept {
    size_t cnt = 0;
    for (size_t i = 1; i < fibers_.size(); ++i) {
        if (fibers_[i].status != FiberStatus::Inactive &&
            fibers_[i].status != FiberStatus::Terminated) {
            ++cnt;
        }
    }
    return cnt;
}

const RTSFiberContext* RTSFiberScheduler::getFiber(uint32_t fiberId) const noexcept {
    if (fiberId == 0 || fiberId >= fibers_.size()) return nullptr;
    return &fibers_[fiberId];
}

uint64_t RTSFiberScheduler::computeLockstepDigest() const noexcept {
    // 64-bit FNV-1a hash across all active fiber states for deterministic replay & netcode check
    uint64_t hash = 14695981039346656037ULL;
    auto mix = [&hash](uint64_t v) {
        hash ^= v;
        hash *= 1099511628211ULL;
    };
    mix(currentTick_);
    for (size_t i = 1; i < fibers_.size(); ++i) {
        const auto& f = fibers_[i];
        if (f.status == FiberStatus::Terminated || f.status == FiberStatus::Inactive) continue;
        mix(f.fiberId);
        mix(f.programId);
        mix(static_cast<uint64_t>(f.status));
        mix(f.pc);
        mix(f.wakeTick);
        mix(f.prng.state);
        mix(f.boundEntity.pack64());
    }
    return hash;
}

std::vector<uint8_t> RTSFiberScheduler::captureBinarySnapshot() const {
    static_assert(std::is_trivially_copyable_v<RTSFiberContext>,
                  "RTSFiberContext must be trivially copyable with no raw pointers");
    std::vector<uint8_t> buf;
    uint32_t count = static_cast<uint32_t>(fibers_.size());
    buf.resize(sizeof(uint32_t) * 2 + count * sizeof(RTSFiberContext));
    std::memcpy(buf.data(), &currentTick_, sizeof(uint32_t));
    std::memcpy(buf.data() + sizeof(uint32_t), &count, sizeof(uint32_t));
    std::memcpy(buf.data() + sizeof(uint32_t) * 2, fibers_.data(), count * sizeof(RTSFiberContext));
    return buf;
}

void RTSFiberScheduler::restoreBinarySnapshot(const std::vector<uint8_t>& snapshot) {
    if (snapshot.size() < sizeof(uint32_t) * 2) return;
    uint32_t count = 0;
    std::memcpy(&currentTick_, snapshot.data(), sizeof(uint32_t));
    std::memcpy(&count, snapshot.data() + sizeof(uint32_t), sizeof(uint32_t));
    if (snapshot.size() < sizeof(uint32_t) * 2 + count * sizeof(RTSFiberContext)) return;

    fibers_.resize(count);
    std::memcpy(fibers_.data(), snapshot.data() + sizeof(uint32_t) * 2, count * sizeof(RTSFiberContext));

    readyQueue_.clear();
    predicateWaitList_.clear();
    freeFiberIds_.clear();
    for (auto& slot : timingWheel_) slot.clear();

    for (uint32_t i = 1; i < count; ++i) {
        const auto& f = fibers_[i];
        if (f.status == FiberStatus::Ready) {
            readyQueue_.push_back(i);
        } else if (f.status == FiberStatus::SleepingTicks) {
            timingWheel_[static_cast<size_t>(f.wakeTick) & WHEEL_MASK].push_back(i);
        } else if (f.status == FiberStatus::WaitingPredicate) {
            predicateWaitList_.push_back(i);
        } else {
            freeFiberIds_.push_back(i);
        }
    }
}

} // namespace realscript::runtime
