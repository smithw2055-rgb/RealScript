#include "realscript/runtime/RTSFiberScheduler.h"

#include <cstring>

namespace realscript::runtime {

using Opcode = RTSOpcode;

RTSFiberScheduler::RTSFiberScheduler(IRTSHostCallbacks* hostCallbacks) noexcept
    : host_(hostCallbacks) {
    fibers_.reserve(2048);
    // Reserve index 0 as sentinel
    fibers_.push_back(RTSFiberContext{});
}

uint32_t RTSFiberScheduler::spawnFiber(
    const std::vector<CompactInstruction>& codeStream,
    uint32_t entryPc,
    GenerationalEntityHandle entity,
    uint8_t factionId,
    uint32_t parentFiberId,
    uint32_t seed) {
    uint32_t id = static_cast<uint32_t>(fibers_.size());
    RTSFiberContext ctx{};
    ctx.fiberId = id;
    ctx.parentFiberId = parentFiberId;
    ctx.boundEntity = entity;
    ctx.factionId = factionId;
    ctx.status = FiberStatus::Ready;
    ctx.codeStream = &codeStream;
    ctx.pc = entryPc;
    ctx.wakeTick = currentTick_;
    ctx.prng.seed(seed ^ (id * 0x9E3779B9u));

    fibers_.push_back(ctx);
    readyQueue_.push_back(id);
    return id;
}

void RTSFiberScheduler::killFiberTree(uint32_t rootFiberId) noexcept {
    if (rootFiberId == 0 || rootFiberId >= fibers_.size()) return;
    fibers_[rootFiberId].status = FiberStatus::Terminated;
    for (size_t i = 1; i < fibers_.size(); ++i) {
        if (fibers_[i].parentFiberId == rootFiberId && fibers_[i].status != FiberStatus::Terminated) {
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
    if (!fiber.codeStream) {
        fiber.status = FiberStatus::Terminated;
        return;
    }
    const auto& code = *fiber.codeStream;
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
                fiber.status = FiberStatus::Terminated;
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
            fiber.waitPredicate = static_cast<WaitPredicateKind>(ins.flags);
            fiber.waitTargetId = ins.regOrSlot;
            fiber.waitRequiredCount = static_cast<uint8_t>(ins.imm32 & 0xFFu);
            predicateWaitList_.push_back(fiber.fiberId);
            return;
        }

        case Opcode::ForkFiber: {
            uint32_t targetPc = ins.imm32;
            uint32_t parentId = fiber.fiberId;
            GenerationalEntityHandle ent = fiber.boundEntity;
            uint8_t fac = fiber.factionId;
            const auto* streamPtr = fiber.codeStream;
            uint32_t childSeed = fiber.prng.nextUInt32();
            ++fiber.pc;
            // Note: spawnFiber may reallocate `fibers_`, so we update `fiber.pc` beforehand
            // and re-obtain or continue safely via index if needed.
            scheduleAtTick(parentId, currentTick + 1);
            spawnFiber(*streamPtr, targetPc, ent, fac, parentId, childSeed);
            return;
        }

        case Opcode::KillFiber:
            killFiberTree(fiber.fiberId);
            return;

        case Opcode::FastSetSpriteFrame:
            if (host_) {
                host_->onSetSpriteFrame(fiber.boundEntity, ins.regOrSlot);
            }
            ++fiber.pc;
            break;

        case Opcode::FastSetSpriteOffset:
            if (host_) {
                auto dx = static_cast<int8_t>(ins.imm32 & 0xFFu);
                auto dy = static_cast<int8_t>((ins.imm32 >> 8) & 0xFFu);
                host_->onSetSpriteOffset(fiber.boundEntity, dx, dy);
            }
            ++fiber.pc;
            break;

        case Opcode::FastSpawnSocketOverlay:
            if (host_) {
                host_->onSpawnSocketOverlay(
                    fiber.boundEntity,
                    ins.regOrSlot,
                     static_cast<uint8_t>(ins.imm32 & 0xFFu),
                    ins.flags != 0);
            }
            ++fiber.pc;
            break;

        case Opcode::FastPlaySoundCue:
            if (host_) {
                host_->onPlaySoundCue(fiber.boundEntity, static_cast<uint16_t>(ins.imm32 & 0xFFFFu));
            }
            ++fiber.pc;
            break;

        case Opcode::FastTriggerCombatHit:
            if (host_) {
                host_->onTriggerCombatHit(fiber.boundEntity, ins.flags);
            }
            ++fiber.pc;
            break;

        case Opcode::FastAIBuildOrder:
            if (host_) {
                host_->onAIBuildOrder(
                    fiber.factionId,
                    ins.regOrSlot,
                    static_cast<uint8_t>(ins.imm32 & 0xFFu),
                    ins.flags);
            }
            ++fiber.pc;
            break;

        case Opcode::FastAIDispatchWave:
            if (host_) {
                host_->onAIDispatchWave(fiber.factionId, ins.imm32);
            }
            ++fiber.pc;
            break;

        default:
            ++fiber.pc;
            break;
        }
    }

    if (fiber.pc >= code.size()) {
        fiber.status = FiberStatus::Terminated;
    } else if (fiber.status == FiberStatus::Ready) {
        // Re-schedule on next tick if slice budget was reached or after ForkFiber
        scheduleAtTick(fiber.fiberId, currentTick + 1);
    }
}

uint32_t RTSFiberScheduler::notifyBlackboardEvent(
    uint8_t factionId,
    WaitPredicateKind kind,
    uint16_t targetId,
    uint8_t currentOwnedCount) {
    uint32_t woken = 0;
    std::vector<uint32_t> stillWaiting;
    for (uint32_t fid : predicateWaitList_) {
        if (fid == 0 || fid >= fibers_.size()) continue;
        auto& f = fibers_[fid];
        if (f.status != FiberStatus::WaitingPredicate) continue;
        if (f.factionId == factionId &&
            f.waitPredicate == kind &&
            f.waitTargetId == targetId &&
            currentOwnedCount >= f.waitRequiredCount) {
            f.status = FiberStatus::Ready;
            f.waitPredicate = WaitPredicateKind::None;
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
        mix(static_cast<uint64_t>(f.status));
        mix(f.pc);
        mix(f.wakeTick);
        mix(f.prng.state);
        mix(f.boundEntity.pack64());
    }
    return hash;
}

std::vector<uint8_t> RTSFiberScheduler::captureBinarySnapshot() const {
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
    for (auto& slot : timingWheel_) slot.clear();

    for (uint32_t i = 1; i < count; ++i) {
        const auto& f = fibers_[i];
        if (f.status == FiberStatus::Ready) {
            readyQueue_.push_back(i);
        } else if (f.status == FiberStatus::SleepingTicks) {
            timingWheel_[static_cast<size_t>(f.wakeTick) & WHEEL_MASK].push_back(i);
        } else if (f.status == FiberStatus::WaitingPredicate) {
            predicateWaitList_.push_back(i);
        }
    }
}

} // namespace realscript::runtime
