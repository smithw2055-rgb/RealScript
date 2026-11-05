#pragma once

#include "realscript/runtime/RTSFiberScheduler.h"

#include <array>
#include <cstdint>
#include <vector>

namespace realscript::game {

/**
 * Structure-of-Arrays (SoA) ECS Component buffer for StarCraft: Remastered 4K HD Sprites.
 * Directly written by RealScript `FastSetSpriteFrame` and `FastSetSpriteOffset` with zero marshaling.
 */
struct SpriteRenderSoA {
    std::vector<uint16_t> imageId;
    std::vector<uint16_t> frameSet;
    std::vector<uint8_t> direction17;
    std::vector<int8_t> flipX;
    std::vector<int8_t> offsetX;
    std::vector<int8_t> offsetY;
    std::vector<uint16_t> lastOverlaySpawned;
    std::vector<uint16_t> lastSoundCuePlayed;
    std::vector<uint32_t> combatHitsTriggered;

    void resize(size_t count) {
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

/**
 * Faction AI Blackboard state for StarCraft `aiscript.bin` & `bwscript.bin` Behavior Trees.
 */
struct FactionAIBlackboard {
    std::array<uint8_t, 228> unitCounts{};
    std::array<uint8_t, 228> buildOrderQueued{};
    uint32_t attackWavesDispatched{0};
    uint32_t lastAttackWaveSize{0};
};

/**
 * Zero-Copy Bridge connecting RealScript's `RTSFiberScheduler` to the ECS SoA buffers
 * and Faction AI Blackboards.
 */
class StarCraftRTSBridge final : public runtime::IRTSHostCallbacks {
public:
    explicit StarCraftRTSBridge(size_t maxEntities = 2048);

    runtime::GenerationalEntityHandle allocateUnitEntity(uint16_t imageId);

    // IRTSHostCallbacks overrides (invoked directly by 8-byte CompactInstruction opcodes)
    void onSetSpriteFrame(runtime::GenerationalEntityHandle entity, uint16_t frameSet) noexcept override;
    void onSetSpriteOffset(runtime::GenerationalEntityHandle entity, int8_t dx, int8_t dy) noexcept override;
    void onSpawnSocketOverlay(runtime::GenerationalEntityHandle entity, uint16_t imageOrSpriteId, uint8_t socketSlot, bool above) noexcept override;
    void onPlaySoundCue(runtime::GenerationalEntityHandle entity, uint16_t soundId) noexcept override;
    void onTriggerCombatHit(runtime::GenerationalEntityHandle entity, uint8_t weaponSlot) noexcept override;
    void onAIBuildOrder(uint8_t factionId, uint16_t unitOrUpgradeId, uint8_t count, uint8_t priority) noexcept override;
    void onAIDispatchWave(uint8_t factionId, uint32_t waveCount) noexcept override;

    [[nodiscard]] SpriteRenderSoA& sprites() noexcept { return sprites_; }
    [[nodiscard]] const SpriteRenderSoA& sprites() const noexcept { return sprites_; }
    [[nodiscard]] FactionAIBlackboard& factionBlackboard(uint8_t factionId) noexcept {
        return blackboards_[factionId & 7u];
    }

private:
    std::uint32_t nextEntitySlot_{0};
    SpriteRenderSoA sprites_;
    std::array<FactionAIBlackboard, 8> blackboards_{};
};

} // namespace realscript::game
