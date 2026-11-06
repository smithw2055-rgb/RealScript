#pragma once

#include "realscript/runtime/RTSFiberScheduler.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace realscript::game {

/**
 * Game-Specific Command IDs for StarCraft `iscript.bin`, `aiscript.bin`, and `bwscript.bin`.
 * Dispatched via the generic `FiberOpcode::HostCommand` (`ins.flags`).
 */
enum class StarCraftHostCommand : std::uint8_t {
    SetSpriteFrame = 1,      // playfram: regOrSlot = frameSet
    SetSpriteOffset = 2,     // sethorpos/setvertpos: imm32 = (dy << 8) | dx
    SpawnSocketOverlay = 3,  // imgol/sprol: regOrSlot = imageId, imm32 = (above << 8) | socketSlot
    PlaySoundCue = 4,        // playsnd: imm32 = soundId
    TriggerCombatHit = 5,    // attackwith/domissiledmg: regOrSlot = weaponSlot
    DefineMaxUnitCap = 6,    // 0x4B define_max (Blackboard cap): regOrSlot = unitId, imm32 = maxCount
    AIBuildUnit = 7,         // 0x06 build / 0x4C train: regOrSlot = unitId, imm32 = (priority << 8) | count
    AIResearchUpgrade = 8,   // 0x07 upgrade: regOrSlot = upgradeId (0..60), imm32 = (priority << 8) | level
    AIResearchTech = 9,      // 0x08 tech: regOrSlot = techId (0..43), imm32 = priority
    AIWaveClear = 10,        // 0x0B attack_clear: clears pending attack wave roster
    AIWaveAdd = 11,          // 0x0C attack_add: regOrSlot = unitId, imm32 = count
    AIDispatchWave = 12,     // 0x0E attack_do: commits wave_roster; imm32 = explicit override or 0 for roster sum
    AIUpdateDefense = 13     // 0x13..0x1A defensebuild/use_gg/ag/ga/aa: regOrSlot = unitId, imm32 = (category << 8) | count
};

/**
 * Game-Specific Blackboard Condition Types for `FiberOpcode::HostCondJump` (`ins.flags`).
 * Maps directly to `BT_SwitchSelector_EnemyRace` & `BT_ConditionDecorator` in the PDF spec.
 */
enum class StarCraftConditionKind : std::uint8_t {
    EnemyRaceEquals = 1,     // 0x39 race_jump: key16 = expectedRace (0=Terran, 1=Zerg, 2=Protoss)
    MineralsAtLeast = 2,     // 0x54 resources_jump: threshold16 = required minerals
    GasAtLeast = 3,          // 0x54 resources_jump: threshold16 = required vespene gas
    OwnsUnitAtLeast = 4,     // 0x6A if_owned: key16 = unitId, threshold16 = minCount
    NotOwnsUnit = 5,         // 0x01 notowns_jump: key16 = unitId
    EnemyOwnsAtLeast = 6     // 0x5D enemyowns_jump: key16 = unitId, threshold16 = minCount
};

/**
 * Game-Specific Predicate Channels for `FiberOpcode::YieldUntilPredicate` (`ins.flags`).
 */
enum class WaitPredicateKind : std::uint8_t {
    None = 0,
    UnitOrBuildingCompleted = 1, // 0x09 wait_build / 0x3E wait_force
    BuildingStarted = 2,         // 0x0A wait_buildstart
    UpgradeOrTechFinished = 3,   // wait_upgrades
    AttackWaveFinished = 4       // wait_finishattack
};

/**
 * Defense Force Category (`gg`, `ag`, `ga`, `aa`) for `BT_Action_UpdateDefenseForce`.
 */
enum class DefenseCategory : std::uint8_t {
    GroundToGround = 0, // gg: 0x13 defensebuild_gg / 0x17 defenseuse_gg
    AirToGround = 1,    // ag: 0x14 defensebuild_ag / 0x18 defenseuse_ag
    GroundToAir = 2,    // ga: 0x15 defensebuild_ga / 0x19 defenseuse_ga
    AirToAir = 3        // aa: 0x16 defensebuild_aa / 0x1A defenseuse_aa
};

/**
 * Helper factory methods to build 8-byte `CompactInstruction` for StarCraft scripts
 * on top of the generic `FiberOpcode::HostCommand` & `FiberOpcode::HostCondJump`.
 */
namespace sc_instr {

constexpr runtime::CompactInstruction setSpriteFrame(std::uint16_t frameSet) noexcept {
    return runtime::CompactInstruction(
        runtime::FiberOpcode::HostCommand,
        static_cast<std::uint8_t>(StarCraftHostCommand::SetSpriteFrame),
        frameSet,
        0u);
}

constexpr runtime::CompactInstruction setSpriteOffset(std::int8_t dx, std::int8_t dy) noexcept {
    std::uint32_t packed = (static_cast<std::uint32_t>(static_cast<std::uint8_t>(dy)) << 8u) |
                           static_cast<std::uint32_t>(static_cast<std::uint8_t>(dx));
    return runtime::CompactInstruction(
        runtime::FiberOpcode::HostCommand,
        static_cast<std::uint8_t>(StarCraftHostCommand::SetSpriteOffset),
        0u,
        packed);
}

constexpr runtime::CompactInstruction spawnSocketOverlay(
    std::uint16_t imageId,
    std::uint8_t socketSlot,
    bool above) noexcept {
    std::uint32_t packed = (above ? 0x100u : 0u) | static_cast<std::uint32_t>(socketSlot);
    return runtime::CompactInstruction(
        runtime::FiberOpcode::HostCommand,
        static_cast<std::uint8_t>(StarCraftHostCommand::SpawnSocketOverlay),
        imageId,
        packed);
}

constexpr runtime::CompactInstruction playSoundCue(std::uint16_t soundId) noexcept {
    return runtime::CompactInstruction(
        runtime::FiberOpcode::HostCommand,
        static_cast<std::uint8_t>(StarCraftHostCommand::PlaySoundCue),
        0u,
        soundId);
}

constexpr runtime::CompactInstruction triggerCombatHit(std::uint8_t weaponSlot) noexcept {
    return runtime::CompactInstruction(
        runtime::FiberOpcode::HostCommand,
        static_cast<std::uint8_t>(StarCraftHostCommand::TriggerCombatHit),
        weaponSlot,
        0u);
}

constexpr runtime::CompactInstruction defineMaxUnitCap(std::uint16_t unitId, std::uint8_t maxCap) noexcept {
    return runtime::CompactInstruction(
        runtime::FiberOpcode::HostCommand,
        static_cast<std::uint8_t>(StarCraftHostCommand::DefineMaxUnitCap),
        unitId,
        maxCap);
}

constexpr runtime::CompactInstruction aiBuildOrder(
    std::uint16_t unitId,
    std::uint8_t count,
    std::uint8_t priority = 80) noexcept {
    std::uint32_t packed = (static_cast<std::uint32_t>(priority) << 8u) | static_cast<std::uint32_t>(count);
    return runtime::CompactInstruction(
        runtime::FiberOpcode::HostCommand,
        static_cast<std::uint8_t>(StarCraftHostCommand::AIBuildUnit),
        unitId,
        packed);
}

constexpr runtime::CompactInstruction aiResearchUpgrade(
    std::uint16_t upgradeId,
    std::uint8_t level,
    std::uint8_t priority = 80) noexcept {
    std::uint32_t packed = (static_cast<std::uint32_t>(priority) << 8u) | static_cast<std::uint32_t>(level);
    return runtime::CompactInstruction(
        runtime::FiberOpcode::HostCommand,
        static_cast<std::uint8_t>(StarCraftHostCommand::AIResearchUpgrade),
        upgradeId,
        packed);
}

constexpr runtime::CompactInstruction aiResearchTech(
    std::uint16_t techId,
    std::uint8_t priority = 80) noexcept {
    return runtime::CompactInstruction(
        runtime::FiberOpcode::HostCommand,
        static_cast<std::uint8_t>(StarCraftHostCommand::AIResearchTech),
        techId,
        priority);
}

constexpr runtime::CompactInstruction aiWaveClear() noexcept {
    return runtime::CompactInstruction(
        runtime::FiberOpcode::HostCommand,
        static_cast<std::uint8_t>(StarCraftHostCommand::AIWaveClear),
        0u,
        0u);
}

constexpr runtime::CompactInstruction aiWaveAdd(std::uint16_t unitId, std::uint8_t count) noexcept {
    return runtime::CompactInstruction(
        runtime::FiberOpcode::HostCommand,
        static_cast<std::uint8_t>(StarCraftHostCommand::AIWaveAdd),
        unitId,
        count);
}

constexpr runtime::CompactInstruction aiDispatchWave(std::uint32_t explicitWaveSize = 0) noexcept {
    return runtime::CompactInstruction(
        runtime::FiberOpcode::HostCommand,
        static_cast<std::uint8_t>(StarCraftHostCommand::AIDispatchWave),
        0u,
        explicitWaveSize);
}

constexpr runtime::CompactInstruction aiUpdateDefense(
    DefenseCategory category,
    std::uint16_t unitId,
    std::uint8_t count) noexcept {
    std::uint32_t packed = (static_cast<std::uint32_t>(category) << 8u) | static_cast<std::uint32_t>(count);
    return runtime::CompactInstruction(
        runtime::FiberOpcode::HostCommand,
        static_cast<std::uint8_t>(StarCraftHostCommand::AIUpdateDefense),
        unitId,
        packed);
}

constexpr runtime::CompactInstruction aiCondJump(
    StarCraftConditionKind kind,
    std::uint16_t targetPc,
    std::uint16_t key16,
    std::uint16_t threshold16 = 0) noexcept {
    return runtime::CompactInstruction(
        runtime::FiberOpcode::HostCondJump,
        static_cast<std::uint8_t>(kind),
        targetPc,
        key16,
        threshold16);
}

} // namespace sc_instr

/**
 * Structure-of-Arrays (SoA) ECS Component buffer for StarCraft: Remastered 4K HD Sprites.
 * Includes per-slot generation tracking so stale fibers cannot corrupt recycled entity slots.
 */
struct SpriteRenderSoA {
    std::vector<std::uint32_t> generations;
    std::vector<std::uint16_t> imageId;
    std::vector<std::uint16_t> frameSet;
    std::vector<std::uint8_t> direction17;
    std::vector<std::int8_t> flipX;
    std::vector<std::int8_t> offsetX;
    std::vector<std::int8_t> offsetY;
    std::vector<std::uint16_t> lastOverlaySpawned;
    std::vector<std::uint16_t> lastSoundCuePlayed;
    std::vector<std::uint32_t> combatHitsTriggered;

    void resize(std::size_t count) {
        generations.resize(count, 0);
        imageId.resize(count, 0);
        frameSet.resize(count, 0);
        direction17.resize(count, 0);
        flipX.resize(count, 1);
        offsetX.resize(count, 0);
        offsetY.resize(count, 0);
        lastOverlaySpawned.resize(count, 0);
        lastSoundCuePlayed.resize(count, 0);
        combatHitsTriggered.resize(count, 0);
    }
};

struct WaveRosterEntry {
    std::uint16_t unitId{0};
    std::uint8_t count{0};
};

/**
 * Complete Faction AI Blackboard state matching the PDF Behavior Tree specification:
 * - Separate namespaces for Units (0..227), Upgrades (0..60), and Techs (0..43)
 * - `maxUnitCaps` (`0x4B define_max`)
 * - `waveRoster` (`0x0B attack_clear`, `0x0C attack_add`, `0x0E attack_do`)
 * - 4D Defense Matrix (`gg`, `ag`, `ga`, `aa`) for `BT_Action_UpdateDefenseForce`
 * - Faction resources & enemy race state for `BT_ConditionDecorator`
 */
struct FactionAIBlackboard {
    std::array<std::uint8_t, 228> unitCounts{};
    std::array<std::uint8_t, 228> maxUnitCaps{};
    std::array<std::uint8_t, 228> buildOrderQueued{};
    std::array<std::uint8_t, 61> upgradesQueued{};
    std::array<std::uint8_t, 44> techsQueued{};
    std::array<std::array<std::uint8_t, 228>, 4> defenseForces{}; // [gg/ag/ga/aa][unitId]

    std::vector<WaveRosterEntry> pendingWaveRoster;
    std::uint32_t attackWavesDispatched{0};
    std::uint32_t lastAttackWaveSize{0};

    std::uint8_t enemyRace{0}; // 0=Terran, 1=Zerg, 2=Protoss
    std::uint32_t minerals{0};
    std::uint32_t gas{0};
};

/**
 * Game-Specific Adapter connecting RealScript's generic `RTSFiberScheduler`
 * to StarCraft's ECS SoA buffers and Faction AI Blackboards.
 */
class StarCraftRTSBridge final : public runtime::IFiberHostCallbacks {
public:
    explicit StarCraftRTSBridge(std::size_t maxEntities = 2048);

    runtime::GenerationalEntityHandle allocateUnitEntity(std::uint16_t imageId);
    bool destroyUnitEntity(runtime::GenerationalEntityHandle entity) noexcept;
    [[nodiscard]] bool isEntityValid(runtime::GenerationalEntityHandle entity) const noexcept;

    // Generic IFiberHostCallbacks overrides
    void onHostCommand(
        runtime::RTSFiberContext& fiber,
        std::uint8_t commandId,
        std::uint16_t arg16,
        std::uint32_t arg32) noexcept override;

    [[nodiscard]] bool onEvaluateCondition(
        const runtime::RTSFiberContext& fiber,
        std::uint8_t conditionKind,
        std::uint16_t key16,
        std::uint16_t threshold16) const noexcept override;

    // Direct helper methods
    void onSetSpriteFrame(runtime::GenerationalEntityHandle entity, std::uint16_t frameSet) noexcept;
    void onSetSpriteOffset(runtime::GenerationalEntityHandle entity, std::int8_t dx, std::int8_t dy) noexcept;
    void onSpawnSocketOverlay(runtime::GenerationalEntityHandle entity, std::uint16_t imageOrSpriteId, std::uint8_t socketSlot, bool above) noexcept;
    void onPlaySoundCue(runtime::GenerationalEntityHandle entity, std::uint16_t soundId) noexcept;
    void onTriggerCombatHit(runtime::GenerationalEntityHandle entity, std::uint8_t weaponSlot) noexcept;
    void onAIBuildOrder(std::uint8_t factionId, std::uint16_t unitId, std::uint8_t count, std::uint8_t priority) noexcept;
    void onAIDispatchWave(std::uint8_t factionId, std::uint32_t explicitWaveCount = 0) noexcept;

    [[nodiscard]] SpriteRenderSoA& sprites() noexcept { return sprites_; }
    [[nodiscard]] const SpriteRenderSoA& sprites() const noexcept { return sprites_; }
    [[nodiscard]] FactionAIBlackboard& factionBlackboard(std::uint8_t factionId) noexcept {
        return blackboards_[factionId & 7u];
    }
    [[nodiscard]] const FactionAIBlackboard& factionBlackboard(std::uint8_t factionId) const noexcept {
        return blackboards_[factionId & 7u];
    }

private:
    std::uint32_t nextEntitySlot_{0};
    std::vector<std::uint32_t> freeEntitySlots_;
    SpriteRenderSoA sprites_;
    std::array<FactionAIBlackboard, 8> blackboards_{};
};

} // namespace realscript::game
