#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace realscript::runtime {

/**
 * Deterministic Q24.8 Fixed-Point primitive (24-bit signed integer + 8-bit fractional part).
 * Matches StarCraft's flingy.dat / weapons.dat 1/256th pixel sub-tick precision for
 * 100% bit-identical cross-platform lockstep simulation and replay verification.
 */
struct Fixed24_8 {
    std::int32_t raw{0};

    constexpr Fixed24_8() noexcept = default;
    constexpr explicit Fixed24_8(std::int32_t rawVal) noexcept : raw(rawVal) {}

    [[nodiscard]] static constexpr Fixed24_8 fromInt(std::int32_t v) noexcept {
        return Fixed24_8(v << 8);
    }
    [[nodiscard]] static constexpr Fixed24_8 fromFloat(double v) noexcept {
        return Fixed24_8(static_cast<std::int32_t>(v * 256.0));
    }
    [[nodiscard]] constexpr std::int32_t toIntFloor() const noexcept { return raw >> 8; }
    [[nodiscard]] constexpr double toDouble() const noexcept { return static_cast<double>(raw) / 256.0; }

    constexpr Fixed24_8 operator+(Fixed24_8 rhs) const noexcept { return Fixed24_8(raw + rhs.raw); }
    constexpr Fixed24_8 operator-(Fixed24_8 rhs) const noexcept { return Fixed24_8(raw - rhs.raw); }
    constexpr Fixed24_8 operator*(Fixed24_8 rhs) const noexcept {
        return Fixed24_8(static_cast<std::int32_t>((static_cast<std::int64_t>(raw) * rhs.raw) >> 8));
    }
    constexpr Fixed24_8 operator/(Fixed24_8 rhs) const noexcept {
        return rhs.raw != 0
            ? Fixed24_8(static_cast<std::int32_t>((static_cast<std::int64_t>(raw) << 8) / rhs.raw))
            : Fixed24_8(0);
    }
    constexpr bool operator==(Fixed24_8 rhs) const noexcept { return raw == rhs.raw; }
    constexpr bool operator!=(Fixed24_8 rhs) const noexcept { return raw != rhs.raw; }
    constexpr bool operator<(Fixed24_8 rhs) const noexcept { return raw < rhs.raw; }
    constexpr bool operator<=(Fixed24_8 rhs) const noexcept { return raw <= rhs.raw; }
    constexpr bool operator>(Fixed24_8 rhs) const noexcept { return raw > rhs.raw; }
    constexpr bool operator>=(Fixed24_8 rhs) const noexcept { return raw >= rhs.raw; }
};

/**
 * Compact 8-Byte Generational Entity Handle directly isomorphic to ECS Entity IDs
 * and compatible with `realscript::runtime::NativeHandle` (`slot` + `generation`).
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
 * Deterministic PCG32 / LCG64 PRNG for bit-identical RTS lockstep & replay execution.
 * Used by `randcondjmp` / `waitrand` (iscript.bin) and `random_jump` / `set_randomseed` (aiscript.bin).
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
 * Compact 8-Byte RTS Opcode set for high-density 24Hz lockstep execution of
 * StarCraft `iscript.bin` animation controllers and `aiscript.bin` / `bwscript.bin` Behavior Trees.
 */
enum class RTSOpcode : std::uint8_t {
    Nop = 0,
    LoadInt,
    LoadFixed24_8,
    Add,
    Jump,
    Call,
    Return,
    RandJump,               // Deterministic PRNG branch: if (pcg_u8() < chance) goto targetPc
    YieldTicks,             // Suspends fiber on O(1) Timing Wheel for N 24Hz ticks (wait / waitrand)
    YieldUntilPredicate,    // Suspends fiber until Blackboard predicate fires (wait_build / wait_force)
    ForkFiber,              // Spawns concurrent child fiber sharing parent Blackboard (multirun / expand / panic)
    KillFiber,              // Terminates current fiber or child tree (end / stop / kill_thread)
    FastSetSpriteFrame,     // Zero-copy ECS SoA write: sets SpriteRenderSoA.frameSet[entitySlot] (playfram)
    FastSetSpriteOffset,    // Zero-copy ECS SoA write: sets local (dx, dy) pixel offset (sethorpos / setvertpos)
    FastSpawnSocketOverlay, // Spawns child VFX sprite on 2D Socket mount (.lo* / imgol / sprol)
    FastPlaySoundCue,       // Enqueues deterministic audio cue ID on AudioNotify bus (playsnd / playsndrand)
    FastTriggerCombatHit,   // Emits combat damage/projectile event (attackwith / domissiledmg / attackmelee)
    FastAIBuildOrder,       // Issues town build/train/upgrade/tech directive to FactionBlackboard
    FastAIDispatchWave      // Commits accumulated attack wave roster and triggers attack_do
};

/**
 * Cache-Aligned 8-Byte (64-bit) Hot-Loop Instruction representation.
 * 8 instructions fit inside a single 64-byte CPU L1 cache line.
 */
struct alignas(8) CompactInstruction {
    RTSOpcode opcode{RTSOpcode::Nop};
    std::uint8_t flags{0};       // Sub-opcode mode, predicate kind, or probability threshold
    std::uint16_t regOrSlot{0};  // Target register, unit ID, or socket index
    union {
        struct {
            std::uint16_t srcA;
            std::uint16_t srcB;
        } regs;
        std::uint32_t imm32{0};  // Target PC offset, tick count, sound ID, or constant index
    };

    constexpr CompactInstruction() noexcept : opcode(RTSOpcode::Nop), flags(0), regOrSlot(0), imm32(0) {}
    constexpr CompactInstruction(RTSOpcode op, std::uint8_t flg, std::uint16_t r0, std::uint32_t imm) noexcept
        : opcode(op), flags(flg), regOrSlot(r0), imm32(imm) {}
    constexpr CompactInstruction(RTSOpcode op, std::uint8_t flg, std::uint16_t dst, std::uint16_t a, std::uint16_t b) noexcept
        : opcode(op), flags(flg), regOrSlot(dst), regs{a, b} {}
};
static_assert(sizeof(CompactInstruction) == 8, "CompactInstruction must be exactly 8 bytes (8 per L1 cache line)");

enum class WaitPredicateKind : std::uint8_t {
    None = 0,
    UnitOrBuildingCompleted = 1, // wait_build / wait_train / wait_force
    BuildingStarted = 2,         // wait_buildstart
    UpgradeOrTechFinished = 3,   // wait_upgrades
    AttackWaveFinished = 4       // wait_finishattack
};

enum class FiberStatus : std::uint8_t {
    Inactive = 0,
    Ready = 1,
    SleepingTicks = 2,
    WaitingPredicate = 3,
    Terminated = 4
};

struct RTSFiberContext {
    std::uint32_t fiberId{0};
    std::uint32_t parentFiberId{0};
    GenerationalEntityHandle boundEntity{};
    std::uint8_t factionId{0};
    FiberStatus status{FiberStatus::Inactive};
    bool unbreakableSection{false};

    const std::vector<CompactInstruction>* codeStream{nullptr};
    std::uint32_t pc{0};
    std::uint32_t wakeTick{0};

    std::array<std::uint32_t, 8> callStack{};
    std::uint8_t callDepth{0};

    WaitPredicateKind waitPredicate{WaitPredicateKind::None};
    std::uint16_t waitTargetId{0};
    std::uint8_t waitRequiredCount{0};

    std::array<std::int32_t, 8> regs{};
    DeterministicPRNG prng{};
};

struct IRTSHostCallbacks {
    virtual ~IRTSHostCallbacks() = default;
    virtual void onSetSpriteFrame(GenerationalEntityHandle entity, std::uint16_t frameSet) noexcept = 0;
    virtual void onSetSpriteOffset(GenerationalEntityHandle entity, std::int8_t dx, std::int8_t dy) noexcept = 0;
    virtual void onSpawnSocketOverlay(GenerationalEntityHandle entity, std::uint16_t imageOrSpriteId, std::uint8_t socketSlot, bool above) noexcept = 0;
    virtual void onPlaySoundCue(GenerationalEntityHandle entity, std::uint16_t soundId) noexcept = 0;
    virtual void onTriggerCombatHit(GenerationalEntityHandle entity, std::uint8_t weaponSlot) noexcept = 0;
    virtual void onAIBuildOrder(std::uint8_t factionId, std::uint16_t unitOrUpgradeId, std::uint8_t count, std::uint8_t priority) noexcept = 0;
    virtual void onAIDispatchWave(std::uint8_t factionId, std::uint32_t waveCount) noexcept = 0;
};

class RTSFiberScheduler {
public:
    static constexpr std::size_t WHEEL_SLOTS = 4096;
    static constexpr std::size_t WHEEL_MASK = WHEEL_SLOTS - 1;

    explicit RTSFiberScheduler(IRTSHostCallbacks* hostCallbacks = nullptr) noexcept;

    std::uint32_t spawnFiber(
        const std::vector<CompactInstruction>& codeStream,
        std::uint32_t entryPc,
        GenerationalEntityHandle entity,
        std::uint8_t factionId = 0,
        std::uint32_t parentFiberId = 0,
        std::uint32_t seed = 0x53434257u);

    void killFiberTree(std::uint32_t rootFiberId) noexcept;
    std::uint64_t stepTick(std::uint32_t currentTick);

    std::uint32_t notifyBlackboardEvent(
        std::uint8_t factionId,
        WaitPredicateKind kind,
        std::uint16_t targetId,
        std::uint8_t currentOwnedCount);

    [[nodiscard]] std::vector<std::uint8_t> captureBinarySnapshot() const;
    void restoreBinarySnapshot(const std::vector<std::uint8_t>& snapshot);
    [[nodiscard]] std::uint64_t computeLockstepDigest() const noexcept;

    [[nodiscard]] std::size_t activeFiberCount() const noexcept;
    [[nodiscard]] const RTSFiberContext* getFiber(std::uint32_t fiberId) const noexcept;

private:
    void executeFiberSlice(RTSFiberContext& fiber, std::uint32_t currentTick, std::uint64_t& instrCounter);
    void scheduleAtTick(std::uint32_t fiberId, std::uint32_t wakeTick);

    IRTSHostCallbacks* host_{nullptr};
    std::uint32_t currentTick_{0};
    std::vector<RTSFiberContext> fibers_;
    std::vector<std::uint32_t> readyQueue_;
    std::array<std::vector<std::uint32_t>, WHEEL_SLOTS> timingWheel_{};
    std::vector<std::uint32_t> predicateWaitList_;
};

} // namespace realscript::runtime
