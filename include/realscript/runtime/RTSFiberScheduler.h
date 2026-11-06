#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace realscript::runtime {

/**
 * Generic Deterministic Fixed-Point primitive (Signed 32-bit storage with FracBits fractional bits).
 * Ensures 100% bit-identical cross-platform lockstep simulation, physics, and replay verification
 * across all game genres (RTS, fighting, physics, bullet-hell) without IEEE-754 float drift.
 */
template <std::uint8_t FracBits>
struct FixedPoint32 {
    static_assert(FracBits > 0 && FracBits < 31, "FracBits must be between 1 and 30");
    static constexpr std::int32_t Scale = (1 << FracBits);

    std::int32_t raw{0};

    constexpr FixedPoint32() noexcept = default;
    constexpr explicit FixedPoint32(std::int32_t rawVal) noexcept : raw(rawVal) {}

    [[nodiscard]] static constexpr FixedPoint32 fromRaw(std::int32_t rawVal) noexcept {
        return FixedPoint32(rawVal);
    }
    [[nodiscard]] static constexpr FixedPoint32 fromInt(std::int32_t v) noexcept {
        return FixedPoint32(v << FracBits);
    }
    [[nodiscard]] static constexpr FixedPoint32 fromFloat(double v) noexcept {
        return FixedPoint32(static_cast<std::int32_t>(v * static_cast<double>(Scale)));
    }
    [[nodiscard]] constexpr std::int32_t toIntFloor() const noexcept { return raw >> FracBits; }
    [[nodiscard]] constexpr double toDouble() const noexcept {
        return static_cast<double>(raw) / static_cast<double>(Scale);
    }

    constexpr FixedPoint32 operator+() const noexcept { return *this; }
    constexpr FixedPoint32 operator-() const noexcept { return FixedPoint32(-raw); }
    constexpr FixedPoint32 operator+(FixedPoint32 rhs) const noexcept { return FixedPoint32(raw + rhs.raw); }
    constexpr FixedPoint32 operator-(FixedPoint32 rhs) const noexcept { return FixedPoint32(raw - rhs.raw); }
    constexpr FixedPoint32 operator*(FixedPoint32 rhs) const noexcept {
        return FixedPoint32(static_cast<std::int32_t>((static_cast<std::int64_t>(raw) * rhs.raw) >> FracBits));
    }
    constexpr FixedPoint32 operator/(FixedPoint32 rhs) const noexcept {
        return rhs.raw != 0
            ? FixedPoint32(static_cast<std::int32_t>((static_cast<std::int64_t>(raw) << FracBits) / rhs.raw))
            : FixedPoint32(0);
    }
    constexpr bool operator==(FixedPoint32 rhs) const noexcept { return raw == rhs.raw; }
    constexpr bool operator!=(FixedPoint32 rhs) const noexcept { return raw != rhs.raw; }
    constexpr bool operator<(FixedPoint32 rhs) const noexcept { return raw < rhs.raw; }
    constexpr bool operator<=(FixedPoint32 rhs) const noexcept { return raw <= rhs.raw; }
    constexpr bool operator>(FixedPoint32 rhs) const noexcept { return raw > rhs.raw; }
    constexpr bool operator>=(FixedPoint32 rhs) const noexcept { return raw >= rhs.raw; }
};

using Fixed24_8 = FixedPoint32<8>;   // Q24.8 (1/256th unit precision)
using Fixed16_16 = FixedPoint32<16>; // Q16.16 (1/65536th unit precision)

/**
 * Compact 8-Byte Generational Entity Handle directly isomorphic to `realscript::game::EntityId`
 * and `realscript::runtime::NativeHandle` (`slot` + `generation`).
 */
struct GenerationalEntityHandle {
    static constexpr std::uint32_t InvalidSlot = 0xFFFFFFFFu;
    std::uint32_t slot{InvalidSlot};
    std::uint32_t generation{0};

    [[nodiscard]] constexpr bool valid() const noexcept { return slot != InvalidSlot && generation != 0; }
    [[nodiscard]] constexpr std::uint64_t pack64() const noexcept {
        return (static_cast<std::uint64_t>(generation) << 32) | static_cast<std::uint64_t>(slot);
    }
    [[nodiscard]] static constexpr GenerationalEntityHandle unpack64(std::uint64_t packed) noexcept {
        return GenerationalEntityHandle{
            static_cast<std::uint32_t>(packed & 0xFFFFFFFFu),
            static_cast<std::uint32_t>(packed >> 32)
        };
    }
    constexpr bool operator==(const GenerationalEntityHandle& rhs) const noexcept {
        return slot == rhs.slot && generation == rhs.generation;
    }
    constexpr bool operator!=(const GenerationalEntityHandle& rhs) const noexcept {
        return !(*this == rhs);
    }
};
static_assert(sizeof(GenerationalEntityHandle) == 8, "GenerationalEntityHandle must be 8 bytes");

/**
 * Deterministic PCG32 PRNG for bit-identical lockstep & replay execution.
 * Uses the same default state/stream constants as `realscript::game::RandomStream`.
 */
struct DeterministicPRNG {
    std::uint64_t state{0x853C49E6748FEA9BULL};
    std::uint64_t inc{0xDA3E39CB94B95BDBULL};

    constexpr void seed(std::uint32_t seedVal) noexcept {
        state = 0U;
        inc = (static_cast<std::uint64_t>(seedVal) << 1U) | 1ULL;
        (void)nextUInt32();
        state += 0x853C49E6748FEA9BULL;
        (void)nextUInt32();
    }

    [[nodiscard]] constexpr std::uint32_t nextUInt32() noexcept {
        std::uint64_t oldstate = state;
        state = oldstate * 6364136223846793005ULL + (inc | 1ULL);
        std::uint32_t xorshifted = static_cast<std::uint32_t>(((oldstate >> 18U) ^ oldstate) >> 27U);
        std::uint32_t rot = static_cast<std::uint32_t>(oldstate >> 59U);
        return (xorshifted >> rot) | (xorshifted << ((0U - rot) & 31U));
    }

    [[nodiscard]] constexpr std::uint8_t nextByte255() noexcept {
        return static_cast<std::uint8_t>(nextUInt32() & 0xFFu);
    }

    [[nodiscard]] constexpr std::uint16_t nextRange(std::uint16_t minVal, std::uint16_t maxVal) noexcept {
        if (maxVal <= minVal) return minVal;
        std::uint32_t span = static_cast<std::uint32_t>(maxVal - minVal) + 1u;
        return static_cast<std::uint16_t>(minVal + (nextUInt32() % span));
    }
};

/**
 * Engine-Agnostic Compact 8-Byte Fiber Opcode Set.
 * Drives high-density state machines, coroutines, timelines, and Behavior Trees (BT).
 * All game-specific actions and queries are delegated through generic `HostCommand` and `HostCondJump`.
 */
enum class FiberOpcode : std::uint8_t {
    Nop = 0,
    LoadInt,                // regs[regOrSlot] = static_cast<int32_t>(imm32)
    LoadFixed24_8,          // regs[regOrSlot] = FixedPoint raw value in imm32
    Add,                    // regs[regOrSlot] = regs[srcA] + regs[srcB]
    Sub,                    // regs[regOrSlot] = regs[srcA] - regs[srcB]
    Jump,                   // pc = imm32
    Call,                   // push(pc + 1); pc = imm32
    Return,                 // pop(pc) or terminate if callDepth == 0
    RandJump,               // BT_ProbabilitySelector: if (prng.nextByte255() < flags) pc = imm32
    HostCondJump,           // BT_ConditionDecorator / Switch: if (host->onEvaluateCondition(fiber, flags, regOrSlot, imm32)) pc = target
    YieldTicks,             // Suspends fiber on O(1) Timing Wheel for imm32 ticks (or random [regOrSlot..imm32] if flags != 0)
    YieldUntilPredicate,    // Suspends fiber until notifyBlackboardEvent(ownerGroup, flags, regOrSlot, >= imm32) fires
    ForkFiber,              // BT_Parallel / Daemon: Spawns concurrent child fiber at targetPc = imm32
    KillFiber,              // Terminates current fiber and all descendant fibers
    SetUnbreakable,         // Sets fiber.unbreakableSection = (flags != 0) to guard critical animation/action windows
    HostCommand             // Invokes host->onHostCommand(fiber, flags, regOrSlot, imm32) for zero-copy ECS/Blackboard writes
};

using RTSOpcode = FiberOpcode;

/**
 * Cache-Aligned 8-Byte (64-bit) Hot-Loop Instruction representation.
 * 8 instructions fit inside a single 64-byte CPU L1 cache line.
 */
struct alignas(8) CompactInstruction {
    FiberOpcode opcode{FiberOpcode::Nop};
    std::uint8_t flags{0};       // Command ID, condition kind, predicate channel, or probability threshold
    std::uint16_t regOrSlot{0};  // Target register, 16-bit parameter, or condition jump target PC
    union {
        struct {
            std::uint16_t srcA;
            std::uint16_t srcB;
        } regs;
        std::uint32_t imm32{0};  // Target PC offset, tick count, or 32-bit command payload
    };

    constexpr CompactInstruction() noexcept : opcode(FiberOpcode::Nop), flags(0), regOrSlot(0), imm32(0) {}
    constexpr CompactInstruction(FiberOpcode op, std::uint8_t flg, std::uint16_t r0, std::uint32_t imm) noexcept
        : opcode(op), flags(flg), regOrSlot(r0), imm32(imm) {}
    constexpr CompactInstruction(FiberOpcode op, std::uint8_t flg, std::uint16_t dst, std::uint16_t a, std::uint16_t b) noexcept
        : opcode(op), flags(flg), regOrSlot(dst), regs{a, b} {}
};
static_assert(sizeof(CompactInstruction) == 8, "CompactInstruction must be exactly 8 bytes (8 per L1 cache line)");

enum class FiberStatus : std::uint8_t {
    Inactive = 0,
    Ready = 1,
    SleepingTicks = 2,
    WaitingPredicate = 3,
    Terminated = 4
};

/**
 * Purely POD / Trivially Copyable Fiber Execution Context (zero raw pointers).
 * Safe for cross-process binary snapshots, network rollback, and hot-reload.
 */
struct RTSFiberContext {
    std::uint32_t fiberId{0};
    std::uint32_t parentFiberId{0};
    std::uint32_t programId{0};
    GenerationalEntityHandle boundEntity{};
    std::uint8_t ownerGroupId{0};
    FiberStatus status{FiberStatus::Inactive};
    bool unbreakableSection{false};
    std::uint8_t callDepth{0};

    std::uint32_t pc{0};
    std::uint32_t wakeTick{0};

    std::uint8_t waitPredicateChannel{0};
    std::uint16_t waitTargetKey{0};
    std::uint32_t waitRequiredValue{0};

    std::array<std::uint32_t, 8> callStack{};
    std::array<std::int32_t, 8> regs{};
    DeterministicPRNG prng{};
};

/**
 * Generic Host Callback Interface for DeterministicFiberScheduler.
 * Specific games (RTS, Fighting, Action RPG, Bullet-Hell) implement this interface
 * in their own adapter layer to bridge generic opcodes to their ECS SoA buffers & AI Blackboards.
 */
struct IFiberHostCallbacks {
    virtual ~IFiberHostCallbacks() = default;

    /**
     * Invoked by `FiberOpcode::HostCommand`.
     * @param fiber      Active fiber context (provides `boundEntity`, `ownerGroupId`, `regs`, `prng`).
     * @param commandId  Game-defined command ID (`ins.flags`).
     * @param arg16      16-bit parameter (`ins.regOrSlot`).
     * @param arg32      32-bit parameter (`ins.imm32`).
     */
    virtual void onHostCommand(
        RTSFiberContext& fiber,
        std::uint8_t commandId,
        std::uint16_t arg16,
        std::uint32_t arg32) noexcept = 0;

    /**
     * Invoked by `FiberOpcode::HostCondJump` to evaluate game-specific blackboard/world predicates
     * (e.g. enemy faction/race check, resource threshold, unit ownership, difficulty level).
     * @param fiber         Active fiber context.
     * @param conditionKind Game-defined condition type (`ins.flags`).
     * @param key16         16-bit condition key (`ins.regs.srcA`).
     * @param threshold16   16-bit comparison operand (`ins.regs.srcB`).
     * @return true to branch to `ins.regOrSlot`, false to fall through to `pc + 1`.
     */
    virtual bool onEvaluateCondition(
        const RTSFiberContext& fiber,
        std::uint8_t conditionKind,
        std::uint16_t key16,
        std::uint16_t threshold16) const noexcept {
        (void)fiber;
        (void)conditionKind;
        (void)key16;
        (void)threshold16;
        return false;
    }
};

using IRTSHostCallbacks = IFiberHostCallbacks;

/**
 * Engine-Agnostic Deterministic Micro-Fiber & Behavior Tree Scheduler.
 * Features:
 * - Program table registration (`registerProgram`) eliminating raw pointers in snapshots
 * - Free-list fiber slot recycling (`freeFiberIds_`) preventing unbounded memory growth
 * - O(1) 4096-slot hierarchical timing wheel for high-frequency tick sleeps
 * - Event-driven Blackboard predicate wakeup (`notifyBlackboardEvent`)
 * - 64-bit FNV-1a lockstep state digest (`computeLockstepDigest`)
 */
class RTSFiberScheduler {
public:
    static constexpr std::size_t WHEEL_SLOTS = 4096;
    static constexpr std::size_t WHEEL_MASK = WHEEL_SLOTS - 1;

    explicit RTSFiberScheduler(IFiberHostCallbacks* hostCallbacks = nullptr) noexcept;

    /**
     * Registers an immutable instruction stream in the scheduler's program table
     * and returns its `programId` (1-based). Safe across snapshots and hot-reload.
     */
    std::uint32_t registerProgram(std::vector<CompactInstruction> codeStream);

    /**
     * Spawns a fiber bound to a registered `programId`.
     */
    std::uint32_t spawnFiberByProgram(
        std::uint32_t programId,
        std::uint32_t entryPc,
        GenerationalEntityHandle entity,
        std::uint8_t ownerGroupId = 0,
        std::uint32_t parentFiberId = 0,
        std::uint32_t seed = 0x53434257u);

    /**
     * Convenience overload that registers or reuses `codeStream` and spawns a fiber.
     */
    std::uint32_t spawnFiber(
        const std::vector<CompactInstruction>& codeStream,
        std::uint32_t entryPc,
        GenerationalEntityHandle entity,
        std::uint8_t ownerGroupId = 0,
        std::uint32_t parentFiberId = 0,
        std::uint32_t seed = 0x53434257u);

    /**
     * Attempts to interrupt/redirect a fiber to a new entry point.
     * Respects `fiber.unbreakableSection` unless `force == true`.
     */
    bool interruptFiber(std::uint32_t fiberId, std::uint32_t newEntryPc, bool force = false) noexcept;

    void killFiberTree(std::uint32_t rootFiberId) noexcept;
    std::uint64_t stepTick(std::uint32_t currentTick);

    /**
     * Wakes all fibers waiting on `(ownerGroupId, predicateChannel, targetKey)`
     * whose `waitRequiredValue <= currentValue`.
     */
    std::uint32_t notifyBlackboardEvent(
        std::uint8_t ownerGroupId,
        std::uint8_t predicateChannel,
        std::uint16_t targetKey,
        std::uint32_t currentValue);

    [[nodiscard]] std::vector<std::uint8_t> captureBinarySnapshot() const;
    void restoreBinarySnapshot(const std::vector<std::uint8_t>& snapshot);
    [[nodiscard]] std::uint64_t computeLockstepDigest() const noexcept;

    [[nodiscard]] std::size_t activeFiberCount() const noexcept;
    [[nodiscard]] const RTSFiberContext* getFiber(std::uint32_t fiberId) const noexcept;

private:
    void executeFiberSlice(RTSFiberContext& fiber, std::uint32_t currentTick, std::uint64_t& instrCounter);
    void scheduleAtTick(std::uint32_t fiberId, std::uint32_t wakeTick);

    IFiberHostCallbacks* host_{nullptr};
    std::uint32_t currentTick_{0};
    std::vector<std::vector<CompactInstruction>> programs_;
    std::vector<const std::vector<CompactInstruction>*> externalStreamCache_;
    std::vector<RTSFiberContext> fibers_;
    std::vector<std::uint32_t> freeFiberIds_;
    std::vector<std::uint32_t> readyQueue_;
    std::array<std::vector<std::uint32_t>, WHEEL_SLOTS> timingWheel_{};
    std::vector<std::uint32_t> predicateWaitList_;
};

using DeterministicFiberScheduler = RTSFiberScheduler;

} // namespace realscript::runtime
