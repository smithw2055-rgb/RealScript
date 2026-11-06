#include "realscript/game/StarCraftRTSBridge.h"

namespace realscript::game {

StarCraftRTSBridge::StarCraftRTSBridge(size_t maxEntities) {
    sprites_.resize(maxEntities);
}

runtime::GenerationalEntityHandle StarCraftRTSBridge::allocateUnitEntity(uint16_t imageId) {
    uint32_t slot = 0;
    if (!freeEntitySlots_.empty()) {
        slot = freeEntitySlots_.back();
        freeEntitySlots_.pop_back();
    } else {
        slot = nextEntitySlot_++;
        if (slot >= sprites_.frameSet.size()) {
            sprites_.resize(static_cast<size_t>(slot) + 512);
        }
    }

    uint32_t nextGen = sprites_.generations[slot] + 1u;
    if (nextGen == 0) nextGen = 1u;
    sprites_.generations[slot] = nextGen;
    sprites_.imageId[slot] = imageId;
    sprites_.frameSet[slot] = 0;
    sprites_.offsetX[slot] = 0;
    sprites_.offsetY[slot] = 0;
    sprites_.lastOverlaySpawned[slot] = 0;
    sprites_.lastSoundCuePlayed[slot] = 0;
    sprites_.combatHitsTriggered[slot] = 0;

    return runtime::GenerationalEntityHandle{slot, nextGen};
}

bool StarCraftRTSBridge::destroyUnitEntity(runtime::GenerationalEntityHandle entity) noexcept {
    if (!isEntityValid(entity)) return false;
    ++sprites_.generations[entity.slot];
    freeEntitySlots_.push_back(entity.slot);
    return true;
}

bool StarCraftRTSBridge::isEntityValid(runtime::GenerationalEntityHandle entity) const noexcept {
    return entity.valid() &&
           entity.slot < sprites_.generations.size() &&
           sprites_.generations[entity.slot] == entity.generation;
}

void StarCraftRTSBridge::onHostCommand(
    runtime::RTSFiberContext& fiber,
    uint8_t commandId,
    uint16_t arg16,
    uint32_t arg32) noexcept {
    const auto cmd = static_cast<StarCraftHostCommand>(commandId);
    switch (cmd) {
    case StarCraftHostCommand::SetSpriteFrame:
        onSetSpriteFrame(fiber.boundEntity, arg16);
        break;

    case StarCraftHostCommand::SetSpriteOffset: {
        auto dx = static_cast<int8_t>(arg32 & 0xFFu);
        auto dy = static_cast<int8_t>((arg32 >> 8u) & 0xFFu);
        onSetSpriteOffset(fiber.boundEntity, dx, dy);
        break;
    }

    case StarCraftHostCommand::SpawnSocketOverlay:
        onSpawnSocketOverlay(
            fiber.boundEntity,
            arg16,
            static_cast<uint8_t>(arg32 & 0xFFu),
            (arg32 & 0x100u) != 0u);
        break;

    case StarCraftHostCommand::PlaySoundCue:
        onPlaySoundCue(fiber.boundEntity, static_cast<uint16_t>(arg32 & 0xFFFFu));
        break;

    case StarCraftHostCommand::TriggerCombatHit:
        onTriggerCombatHit(fiber.boundEntity, static_cast<uint8_t>(arg16 & 0xFFu));
        break;

    case StarCraftHostCommand::DefineMaxUnitCap: {
        auto& bb = blackboards_[fiber.ownerGroupId & 7u];
        if (arg16 < bb.maxUnitCaps.size()) {
            bb.maxUnitCaps[arg16] = static_cast<uint8_t>(arg32 & 0xFFu);
        }
        break;
    }

    case StarCraftHostCommand::AIBuildUnit:
        onAIBuildOrder(
            fiber.ownerGroupId,
            arg16,
            static_cast<uint8_t>(arg32 & 0xFFu),
            static_cast<uint8_t>((arg32 >> 8u) & 0xFFu));
        break;

    case StarCraftHostCommand::AIResearchUpgrade: {
        auto& bb = blackboards_[fiber.ownerGroupId & 7u];
        if (arg16 < bb.upgradesQueued.size()) {
            bb.upgradesQueued[arg16] = static_cast<uint8_t>(arg32 & 0xFFu);
        }
        break;
    }

    case StarCraftHostCommand::AIResearchTech: {
        auto& bb = blackboards_[fiber.ownerGroupId & 7u];
        if (arg16 < bb.techsQueued.size()) {
            bb.techsQueued[arg16] = 1u;
        }
        break;
    }

    case StarCraftHostCommand::AIWaveClear: {
        auto& bb = blackboards_[fiber.ownerGroupId & 7u];
        bb.pendingWaveRoster.clear();
        break;
    }

    case StarCraftHostCommand::AIWaveAdd: {
        auto& bb = blackboards_[fiber.ownerGroupId & 7u];
        bb.pendingWaveRoster.push_back(WaveRosterEntry{arg16, static_cast<uint8_t>(arg32 & 0xFFu)});
        break;
    }

    case StarCraftHostCommand::AIDispatchWave:
        onAIDispatchWave(fiber.ownerGroupId, arg32);
        break;

    case StarCraftHostCommand::AIUpdateDefense: {
        auto& bb = blackboards_[fiber.ownerGroupId & 7u];
        uint8_t cat = static_cast<uint8_t>((arg32 >> 8u) & 0x03u);
        uint8_t cnt = static_cast<uint8_t>(arg32 & 0xFFu);
        if (arg16 < bb.defenseForces[cat].size()) {
            bb.defenseForces[cat][arg16] = cnt;
        }
        break;
    }

    default:
        break;
    }
}

bool StarCraftRTSBridge::onEvaluateCondition(
    const runtime::RTSFiberContext& fiber,
    uint8_t conditionKind,
    uint16_t key16,
    uint16_t threshold16) const noexcept {
    const auto& bb = blackboards_[fiber.ownerGroupId & 7u];
    const auto kind = static_cast<StarCraftConditionKind>(conditionKind);
    switch (kind) {
    case StarCraftConditionKind::EnemyRaceEquals:
        return bb.enemyRace == static_cast<uint8_t>(key16 & 0xFFu);

    case StarCraftConditionKind::MineralsAtLeast:
        return bb.minerals >= threshold16;

    case StarCraftConditionKind::GasAtLeast:
        return bb.gas >= threshold16;

    case StarCraftConditionKind::OwnsUnitAtLeast:
        return key16 < bb.unitCounts.size() && bb.unitCounts[key16] >= threshold16;

    case StarCraftConditionKind::NotOwnsUnit:
        return key16 < bb.unitCounts.size() && bb.unitCounts[key16] == 0u;

    case StarCraftConditionKind::EnemyOwnsAtLeast:
        return false;

    default:
        return false;
    }
}

void StarCraftRTSBridge::onSetSpriteFrame(runtime::GenerationalEntityHandle entity, uint16_t frameSet) noexcept {
    if (isEntityValid(entity)) {
        sprites_.frameSet[entity.slot] = frameSet;
    }
}

void StarCraftRTSBridge::onSetSpriteOffset(runtime::GenerationalEntityHandle entity, int8_t dx, int8_t dy) noexcept {
    if (isEntityValid(entity)) {
        sprites_.offsetX[entity.slot] = dx;
        sprites_.offsetY[entity.slot] = dy;
    }
}

void StarCraftRTSBridge::onSpawnSocketOverlay(
    runtime::GenerationalEntityHandle entity,
    uint16_t imageOrSpriteId,
    uint8_t /*socketSlot*/,
    bool /*above*/) noexcept {
    if (isEntityValid(entity)) {
        sprites_.lastOverlaySpawned[entity.slot] = imageOrSpriteId;
    }
}

void StarCraftRTSBridge::onPlaySoundCue(runtime::GenerationalEntityHandle entity, uint16_t soundId) noexcept {
    if (isEntityValid(entity)) {
        sprites_.lastSoundCuePlayed[entity.slot] = soundId;
    }
}

void StarCraftRTSBridge::onTriggerCombatHit(runtime::GenerationalEntityHandle entity, uint8_t /*weaponSlot*/) noexcept {
    if (isEntityValid(entity)) {
        ++sprites_.combatHitsTriggered[entity.slot];
    }
}

void StarCraftRTSBridge::onAIBuildOrder(
    uint8_t factionId,
    uint16_t unitId,
    uint8_t count,
    uint8_t /*priority*/) noexcept {
    auto& bb = blackboards_[factionId & 7u];
    if (unitId < bb.buildOrderQueued.size()) {
        uint8_t cap = bb.maxUnitCaps[unitId];
        bb.buildOrderQueued[unitId] = (cap > 0 && count > cap) ? cap : count;
    }
}

void StarCraftRTSBridge::onAIDispatchWave(uint8_t factionId, uint32_t explicitWaveCount) noexcept {
    auto& bb = blackboards_[factionId & 7u];
    uint32_t rosterTotal = 0;
    for (const auto& entry : bb.pendingWaveRoster) {
        rosterTotal += entry.count;
    }
    ++bb.attackWavesDispatched;
    bb.lastAttackWaveSize = explicitWaveCount > 0 ? explicitWaveCount : rosterTotal;
}

} // namespace realscript::game
