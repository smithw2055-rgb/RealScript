#include "realscript/game/StarCraftRTSBridge.h"

namespace realscript::game {

StarCraftRTSBridge::StarCraftRTSBridge(size_t maxEntities) {
    sprites_.resize(maxEntities);
}

runtime::GenerationalEntityHandle StarCraftRTSBridge::allocateUnitEntity(uint16_t imageId) {
    runtime::GenerationalEntityHandle handle{nextEntitySlot_++, 1u};
    if (handle.slot >= sprites_.frameSet.size()) {
        sprites_.resize(static_cast<size_t>(handle.slot) + 512);
    }
    sprites_.imageId[handle.slot] = imageId;
    sprites_.frameSet[handle.slot] = 0;
    return handle;
}

void StarCraftRTSBridge::onSetSpriteFrame(runtime::GenerationalEntityHandle entity, uint16_t frameSet) noexcept {
    if (entity.slot < sprites_.frameSet.size()) {
        sprites_.frameSet[entity.slot] = frameSet;
    }
}

void StarCraftRTSBridge::onSetSpriteOffset(runtime::GenerationalEntityHandle entity, int8_t dx, int8_t dy) noexcept {
    if (entity.slot < sprites_.offsetX.size()) {
        sprites_.offsetX[entity.slot] = dx;
        sprites_.offsetY[entity.slot] = dy;
    }
}

void StarCraftRTSBridge::onSpawnSocketOverlay(
    runtime::GenerationalEntityHandle entity,
    uint16_t imageOrSpriteId,
    uint8_t /*socketSlot*/,
    bool /*above*/) noexcept {
    if (entity.slot < sprites_.lastOverlaySpawned.size()) {
        sprites_.lastOverlaySpawned[entity.slot] = imageOrSpriteId;
    }
}

void StarCraftRTSBridge::onPlaySoundCue(runtime::GenerationalEntityHandle entity, uint16_t soundId) noexcept {
    if (entity.slot < sprites_.lastSoundCuePlayed.size()) {
        sprites_.lastSoundCuePlayed[entity.slot] = soundId;
    }
}

void StarCraftRTSBridge::onTriggerCombatHit(runtime::GenerationalEntityHandle entity, uint8_t /*weaponSlot*/) noexcept {
    if (entity.slot < sprites_.combatHitsTriggered.size()) {
        ++sprites_.combatHitsTriggered[entity.slot];
    }
}

void StarCraftRTSBridge::onAIBuildOrder(
    uint8_t factionId,
    uint16_t unitOrUpgradeId,
    uint8_t count,
    uint8_t /*priority*/) noexcept {
    auto& bb = blackboards_[factionId & 7u];
    if (unitOrUpgradeId < bb.buildOrderQueued.size()) {
        bb.buildOrderQueued[unitOrUpgradeId] = count;
    }
}

void StarCraftRTSBridge::onAIDispatchWave(uint8_t factionId, uint32_t waveCount) noexcept {
    auto& bb = blackboards_[factionId & 7u];
    ++bb.attackWavesDispatched;
    bb.lastAttackWaveSize = waveCount;
}

} // namespace realscript::game
