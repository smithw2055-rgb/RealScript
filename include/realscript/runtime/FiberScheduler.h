#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace realscript::mir {
struct Module;
} // namespace realscript::mir

namespace realscript::runtime {

/**
 * Generic Deterministic Fixed-Point Arithmetic Template (`FixedPoint32<FracBits>`).
 * Eliminates IEEE-754 floating-point non-determinism across x86_64 and ARM64 lockstep peers.
 * - `FixedPoint32<8>`  (Q24.8): 1/256th unit precision for 2D/tile/pixel coordinates
 * - `FixedPoint32<16>` (Q16.16): 1/65536th unit precision for 3D physics & angular math
 */
template <std::uint32_t FracBits>
struct FixedPoint32 {
    static_assert(FracBits > 0 && FracBits < 31, "FracBits must be in [1, 30]");
    static constexpr std::int32_t SCALE = (1 << FracBits);
    std::int32_t raw{0};

    constexpr FixedPoint32() noexcept = default;
    constexpr explicit FixedPoint32(std::int32_t rawValue) noexcept : raw(rawValue) {}

    [[nodiscard]] static constexpr FixedPoint32 fromRaw(std::int32_t rawValue) noexcept {
        return FixedPoint32(rawValue);
    }

    [[nodiscard]] static constexpr FixedPoint32 fromInt(std::int32_t val) noexcept {
        return FixedPoint32(val * SCALE);
    }

    [[nodiscard]] static constexpr FixedPoint32 fromFloat(double val) noexcept {
        return FixedPoint32(static_cast<std::int32_t>(
            val * static_cast<double>(SCALE) + (val >= 0.0 ? 0.5 : -0.5)));
    }

    [[nodiscard]] constexpr std::int32_t toIntFloor() const noexcept {
        return raw >> FracBits;
    }

    [[nodiscard]] constexpr double toDouble() const noexcept {
        return static_cast<double>(raw) / static_cast<double>(SCALE);
    }

    [[nodiscard]] static constexpr std::int32_t clampRaw64(std::int64_t val) noexcept {
        if (val > 2147483647LL) return static_cast<std::int32_t>(2147483647LL);
        if (val < -2147483648LL) return static_cast<std::int32_t>(-2147483648LL);
        return static_cast<std::int32_t>(val);
    }

    [[nodiscard]] constexpr FixedPoint32 saturatingAdd(FixedPoint32 rhs) const noexcept {
        return FixedPoint32(clampRaw64(static_cast<std::int64_t>(raw) + static_cast<std::int64_t>(rhs.raw)));
    }

    [[nodiscard]] constexpr FixedPoint32 saturatingSub(FixedPoint32 rhs) const noexcept {
        return FixedPoint32(clampRaw64(static_cast<std::int64_t>(raw) - static_cast<std::int64_t>(rhs.raw)));
    }

    [[nodiscard]] constexpr FixedPoint32 saturatingMul(FixedPoint32 rhs) const noexcept {
        const std::int64_t prod = static_cast<std::int64_t>(raw) * static_cast<std::int64_t>(rhs.raw);
        return FixedPoint32(clampRaw64(prod >> FracBits));
    }

    constexpr FixedPoint32 operator-() const noexcept {
        return FixedPoint32(static_cast<std::int32_t>(0u - static_cast<std::uint32_t>(raw)));
    }
    constexpr FixedPoint32 operator+(FixedPoint32 rhs) const noexcept {
        return FixedPoint32(static_cast<std::int32_t>(
            static_cast<std::uint32_t>(raw) + static_cast<std::uint32_t>(rhs.raw)));
    }
    constexpr FixedPoint32 operator-(FixedPoint32 rhs) const noexcept {
        return FixedPoint32(static_cast<std::int32_t>(
            static_cast<std::uint32_t>(raw) - static_cast<std::uint32_t>(rhs.raw)));
    }
    constexpr FixedPoint32 operator*(FixedPoint32 rhs) const noexcept {
        const std::int64_t prod = static_cast<std::int64_t>(raw) * static_cast<std::int64_t>(rhs.raw);
        return FixedPoint32(static_cast<std::int32_t>(static_cast<std::uint64_t>(prod >> FracBits)));
    }
    constexpr FixedPoint32 operator/(FixedPoint32 rhs) const noexcept {
        if (rhs.raw == 0) return FixedPoint32(0);
        const std::int64_t num = (static_cast<std::int64_t>(raw) << FracBits);
        return FixedPoint32(static_cast<std::int32_t>(static_cast<std::uint64_t>(num / rhs.raw)));
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
    MoveReg,                // regs[regOrSlot] = regs[srcA]
    Add,                    // regs[regOrSlot] = regs[srcA] + regs[srcB]
    Sub,                    // regs[regOrSlot] = regs[srcA] - regs[srcB]
    Mul,                    // regs[regOrSlot] = regs[srcA] * regs[srcB]
    Div,                    // regs[regOrSlot] = (regs[srcB] != 0) ? (regs[srcA] / regs[srcB]) : 0
    Mod,                    // regs[regOrSlot] = (regs[srcB] != 0) ? (regs[srcA] % regs[srcB]) : 0
    CmpEq,                  // regs[regOrSlot] = (regs[srcA] == regs[srcB]) ? 1 : 0
    CmpNe,                  // regs[regOrSlot] = (regs[srcA] != regs[srcB]) ? 1 : 0
    CmpLt,                  // regs[regOrSlot] = (regs[srcA] <  regs[srcB]) ? 1 : 0
    CmpLe,                  // regs[regOrSlot] = (regs[srcA] <= regs[srcB]) ? 1 : 0
    CmpGt,                  // regs[regOrSlot] = (regs[srcA] >  regs[srcB]) ? 1 : 0
    CmpGe,                  // regs[regOrSlot] = (regs[srcA] >= regs[srcB]) ? 1 : 0
    Jump,                   // pc = imm32
    JumpIfZero,             // if (regs[regOrSlot] == 0) pc = imm32 else ++pc
    JumpIfNonZero,          // if (regs[regOrSlot] != 0) pc = imm32 else ++pc
    Call,                   // push(pc + 1); pc = imm32
    Return,                 // pop(pc) or terminate if callDepth == 0
    RandJump,               // BT_ProbabilitySelector: if (prng.nextByte255() < flags) pc = imm32
    HostCondJump,           // BT_ConditionDecorator / Switch: if (host->onEvaluateCondition(fiber, flags, srcA, srcB)) pc = regOrSlot
    YieldTicks,             // Suspends fiber on O(1) Timing Wheel for imm32 ticks (or random [regOrSlot..imm32] if flags != 0)
    YieldUntilPredicate,    // Suspends fiber until notifyBlackboardEvent(ownerGroup, flags, regOrSlot, >= imm32) fires
    ForkFiber,              // BT_Parallel / Daemon: Spawns concurrent child fiber at targetPc = imm32
    KillFiber,              // Terminates current fiber and all descendant fibers
    SetUnbreakable,         // Sets fiber.unbreakableSection = (flags != 0) to guard critical animation/action windows
    HostCommand             // Invokes host->onHostCommand(fiber, flags, regOrSlot, imm32) for zero-copy ECS/Blackboard writes
};

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
struct FiberContext {
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
 * Generic Host Callback Interface for FiberScheduler.
 * Specific game adapters implement this interface to bridge generic opcodes
 * to their ECS SoA buffers & AI Blackboards.
 */
struct IFiberHostCallbacks {
    virtual ~IFiberHostCallbacks() = default;

    virtual void onHostCommand(
        FiberContext& fiber,
        std::uint8_t commandId,
        std::uint16_t arg16,
        std::uint32_t arg32) noexcept = 0;

    virtual bool onEvaluateCondition(
        const FiberContext& fiber,
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

using IFiberHost = IFiberHostCallbacks;

/**
 * Compiled Fiber Program produced by lowering a RealScript (`.rs`) `mir::Module`.
 * Contains a single contiguous 8-byte instruction stream and a symbol table mapping
 * `.rs` function names to their entry PC offsets.
 */
struct CompiledFiberModule {
    std::string moduleName;
    std::vector<CompactInstruction> instructions;
    std::unordered_map<std::string, std::uint32_t> entryPoints;

    [[nodiscard]] bool hasFunction(const std::string& name) const noexcept {
        return entryPoints.find(name) != entryPoints.end();
    }

    [[nodiscard]] std::uint32_t entryPc(const std::string& name) const noexcept {
        auto it = entryPoints.find(name);
        return it != entryPoints.end() ? it->second : 0u;
    }
};

/**
 * Compiler that lowers a RealScript `mir::Module` (written in `.rs` using `import RealScript.Fiber;`)
 * directly into an 8-byte `CompactInstruction` stream for `FiberScheduler`.
 */
class FiberCompiler {
public:
    [[nodiscard]] static CompiledFiberModule compileModule(const realscript::mir::Module& module);
};

/**
 * Engine-Agnostic Deterministic Fiber & Behavior Tree Scheduler.
 */
class FiberScheduler {
public:
    static constexpr std::size_t WHEEL_SLOTS = 4096;
    static constexpr std::size_t WHEEL_MASK = WHEEL_SLOTS - 1;

    explicit FiberScheduler(IFiberHostCallbacks* hostCallbacks = nullptr) noexcept;

    void setHostCallbacks(IFiberHostCallbacks* hostCallbacks) noexcept {
        host_ = hostCallbacks;
    }
    [[nodiscard]] IFiberHostCallbacks* hostCallbacks() const noexcept {
        return host_;
    }

    std::uint32_t registerProgram(std::vector<CompactInstruction> codeStream);
    std::uint32_t registerModule(const CompiledFiberModule& module);
    bool reloadProgram(std::uint32_t programId, std::vector<CompactInstruction> newCodeStream);
    bool reloadModule(
        std::uint32_t programId,
        const CompiledFiberModule& oldModule,
        const CompiledFiberModule& newModule);

    std::uint32_t spawnFiberByProgram(
        std::uint32_t programId,
        std::uint32_t entryPc,
        GenerationalEntityHandle entity,
        std::uint8_t ownerGroupId = 0,
        std::uint32_t parentFiberId = 0,
        std::uint32_t seed = 0x53434257u);

    std::uint32_t spawnFiber(
        const std::vector<CompactInstruction>& codeStream,
        std::uint32_t entryPc,
        GenerationalEntityHandle entity,
        std::uint8_t ownerGroupId = 0,
        std::uint32_t parentFiberId = 0,
        std::uint32_t seed = 0x53434257u);

    std::uint32_t spawnFunction(
        const CompiledFiberModule& module,
        const std::string& functionName,
        GenerationalEntityHandle entity,
        std::uint8_t ownerGroupId = 0,
        std::uint32_t parentFiberId = 0,
        std::uint32_t seed = 0x53434257u);

    bool interruptFiber(std::uint32_t fiberId, std::uint32_t newEntryPc, bool force = false) noexcept;

    void killFiberTree(std::uint32_t rootFiberId) noexcept;
    std::size_t killEntityFibers(GenerationalEntityHandle entity) noexcept;
    std::size_t killEntityFibersBySlot(std::uint32_t entitySlot) noexcept;

    std::uint64_t stepTick(std::uint32_t currentTick);
    std::uint64_t stepTickParallel(std::uint32_t currentTick, std::size_t workerCount = 2);

    std::uint32_t notifyBlackboardEvent(
        std::uint8_t ownerGroupId,
        std::uint8_t predicateChannel,
        std::uint16_t targetKey,
        std::uint32_t currentValue);

    [[nodiscard]] std::vector<std::uint8_t> captureBinarySnapshot() const;
    void restoreBinarySnapshot(const std::vector<std::uint8_t>& snapshot);
    [[nodiscard]] std::uint64_t computeLockstepDigest() const noexcept;

    [[nodiscard]] std::size_t activeFiberCount() const noexcept;
    [[nodiscard]] const FiberContext* getFiber(std::uint32_t fiberId) const noexcept;
    [[nodiscard]] std::uint32_t currentTick() const noexcept { return currentTick_; }

private:
    void executeFiberSlice(FiberContext& fiber, std::uint32_t currentTick, std::uint64_t& instrCounter);
    void scheduleAtTick(std::uint32_t fiberId, std::uint32_t wakeTick);

    IFiberHostCallbacks* host_{nullptr};
    std::uint32_t currentTick_{0};
    std::vector<std::vector<CompactInstruction>> programs_;
    std::vector<const std::vector<CompactInstruction>*> externalStreamCache_;
    std::vector<FiberContext> fibers_;
    std::vector<std::uint32_t> freeFiberIds_;
    std::vector<std::uint32_t> readyQueue_;
    std::array<std::vector<std::uint32_t>, WHEEL_SLOTS> timingWheel_{};
    std::vector<std::uint32_t> predicateWaitList_;
};

using DeterministicFiberScheduler = FiberScheduler;

} // namespace realscript::runtime
