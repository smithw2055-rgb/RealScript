#include "realscript/runtime/RTSFiberScheduler.h"
#include "realscript/game/StarCraftRTSBridge.h"

#include <cassert>
#include <chrono>
#include <iostream>
#include <vector>

using Opcode = realscript::runtime::RTSOpcode;
using namespace realscript::runtime;
using namespace realscript::game;

int main() {
    std::cout << "=== RealScript C++17/20 StarCraft RTS Refactoring Verification ===\n";

    // 1. Verify 8-byte CompactInstruction & 8-byte GenerationalEntityHandle layout
    static_assert(sizeof(CompactInstruction) == 8, "CompactInstruction must be 8 bytes");
    static_assert(sizeof(GenerationalEntityHandle) == 8, "GenerationalEntityHandle must be 8 bytes");
    std::cout << "[PASS] sizeof(CompactInstruction) = " << sizeof(CompactInstruction)
              << " B (8 instructions per 64B cache line)\n";

    // 2. Verify Fixed24_8 deterministic Q24.8 fixed-point math (matching flingy.dat 1/256th pixel speed)
    Fixed24_8 topSpeed = Fixed24_8::fromFloat(5.33203125); // SCV speed
    Fixed24_8 ticks = Fixed24_8::fromInt(24);
    Fixed24_8 distPerSec = topSpeed * ticks;
    assert(distPerSec.toIntFloor() == 127);
    std::cout << "[PASS] Fixed24_8 Q24.8 deterministic speed * 24 ticks = "
              << distPerSec.toDouble() << " px/sec (raw=" << distPerSec.raw << ")\n";

    // 3. Build a compiled StarCraft SCV IScript_084 animation program (IsWorking + Walking loop)
    //    using 8-byte CompactInstructions:
    //    0: FastSetSpriteFrame(frameSet=2)
    //    1: FastSpawnSocketOverlay(imageId=431 scvspark.grp, socketSlot=0, above=true)
    //    2: FastPlaySoundCue(soundId=65 EDrRep00.wav)
    //    3: FastTriggerCombatHit(weaponSlot=0 Fusion Cutter)
    //    4: YieldTicks(2 ticks = 83.3ms)
    //    5: FastSetSpriteFrame(frameSet=3)
    //    6: YieldTicks(2 ticks)
    //    7: Jump(0) -> Loops back to PC=0
    std::vector<CompactInstruction> scvIsWorkingStream = {
        CompactInstruction(Opcode::FastSetSpriteFrame, 0, 2, 0),
        CompactInstruction(Opcode::FastSpawnSocketOverlay, 1, 431, 0),
        CompactInstruction(Opcode::FastPlaySoundCue, 0, 0, 65),
        CompactInstruction(Opcode::FastTriggerCombatHit, 0, 0, 0),
        CompactInstruction(Opcode::YieldTicks, 0, 0, 2),
        CompactInstruction(Opcode::FastSetSpriteFrame, 0, 3, 0),
        CompactInstruction(Opcode::YieldTicks, 0, 0, 2),
        CompactInstruction(Opcode::Jump, 0, 0, 0)
    };

    // 4. Build a compiled StarCraft AI Behavior Tree stream (`Ter3` / `PB2A` with `multirun` ForkFiber & `wait_build` Predicate):
    //    Main thread (PC 0..5):
    //      0: ForkFiber(targetPc=6) -> Spawns background expansion/harass daemon!
    //      1: FastAIBuildOrder(unitId=7 Terran_SCV, count=4, priority=80)
    //      2: FastAIBuildOrder(unitId=111 Terran_Barracks, count=2, priority=80)
    //      3: YieldUntilPredicate(UnitOrBuildingCompleted=1, unitId=111 Terran_Barracks, reqCount=2)
    //      4: FastAIDispatchWave(waveSize=18)
    //      5: KillFiber()
    //    Background daemon thread (PC 6..8):
    //      6: FastAIBuildOrder(unitId=124 Terran_Missile_Turret, count=3, priority=60)
    //      7: YieldTicks(10)
    //      8: Jump(6)
    std::vector<CompactInstruction> terranAIBehaviorStream = {
        CompactInstruction(Opcode::ForkFiber, 0, 0, 6),
        CompactInstruction(Opcode::FastAIBuildOrder, 80, 7, 4),
        CompactInstruction(Opcode::FastAIBuildOrder, 80, 111, 2),
        CompactInstruction(Opcode::YieldUntilPredicate, static_cast<uint8_t>(WaitPredicateKind::UnitOrBuildingCompleted), 111, 2),
        CompactInstruction(Opcode::FastAIDispatchWave, 0, 0, 18),
        CompactInstruction(Opcode::KillFiber, 0, 0, 0),
        // Background multirun daemon at PC=6:
        CompactInstruction(Opcode::FastAIBuildOrder, 60, 124, 3),
        CompactInstruction(Opcode::YieldTicks, 0, 0, 10),
        CompactInstruction(Opcode::Jump, 0, 0, 6)
    };

    StarCraftRTSBridge bridge(2048);
    RTSFiberScheduler scheduler(&bridge);

    // Spawn 1,600 concurrent unit animation fibers + 1 Terran AI Behavior Tree root fiber
    constexpr size_t NUM_UNITS = 1600;
    std::vector<GenerationalEntityHandle> units;
    units.reserve(NUM_UNITS);
    for (size_t i = 0; i < NUM_UNITS; ++i) {
        auto h = bridge.allocateUnitEntity(247); // main_247.anim (Terran SCV)
        units.push_back(h);
        scheduler.spawnFiber(scvIsWorkingStream, 0, h, 1, 0, static_cast<uint32_t>(1000 + i));
    }
    scheduler.spawnFiber(terranAIBehaviorStream, 0, GenerationalEntityHandle{}, 1, 0, 0xCAFEBABEu);

    // Step 240 logical ticks (10 seconds of 24Hz RTS lockstep gameplay)
    auto t0 = std::chrono::high_resolution_clock::now();
    uint64_t totalInstr = 0;
    std::vector<uint8_t> snapshotAtTick100;
    uint64_t digestAtTick120_RunA = 0;

    for (uint32_t tick = 0; tick <= 120; ++tick) {
        totalInstr += scheduler.stepTick(tick);
        if (tick == 50) {
            // Simulate 2 Terran Barracks completing construction at tick 50 -> wakes AI fiber!
            uint32_t woken = scheduler.notifyBlackboardEvent(1, WaitPredicateKind::UnitOrBuildingCompleted, 111, 2);
            (void)woken;
            assert(woken == 1);
        }
        if (tick == 100) {
            snapshotAtTick100 = scheduler.captureBinarySnapshot();
        }
        if (tick == 120) {
            digestAtTick120_RunA = scheduler.computeLockstepDigest();
        }
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double elapsedMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // Verify ECS SoA zero-copy state updates & AI Blackboard execution
    assert(bridge.sprites().lastOverlaySpawned[units[0].slot] == 431);
    assert(bridge.sprites().lastSoundCuePlayed[units[0].slot] == 65);
    assert(bridge.sprites().combatHitsTriggered[units[0].slot] > 25);
    assert(bridge.factionBlackboard(1).buildOrderQueued[111] == 2);
    assert(bridge.factionBlackboard(1).buildOrderQueued[124] == 3); // From forked multirun daemon!
    assert(bridge.factionBlackboard(1).attackWavesDispatched == 1);
    assert(bridge.factionBlackboard(1).lastAttackWaveSize == 18);

    // Verify Deterministic Lockstep Snapshot Restore & Replay Digest Match
    scheduler.restoreBinarySnapshot(snapshotAtTick100);
    for (uint32_t tick = 101; tick <= 120; ++tick) {
        scheduler.stepTick(tick);
    }
    uint64_t digestAtTick120_RunB = scheduler.computeLockstepDigest();
    (void)digestAtTick120_RunB;
    assert(digestAtTick120_RunA == digestAtTick120_RunB);

    std::cout << "[PASS] 1,600 concurrent SCV IScript fibers + Forked AI Behavior Tree stepped 120 ticks in "
              << elapsedMs << " ms (" << (elapsedMs / 120.0) << " ms/tick, " << totalInstr << " instructions)\n";
    std::cout << "[PASS] Event-Driven Blackboard Wakeup (wait_build Barracks=2 -> ExecuteAttackWave=18) verified\n";
    std::cout << "[PASS] Lockstep Snapshot Restore & 64-bit FNV-1a Digest verified: 0x"
              << std::hex << digestAtTick120_RunA << std::dec << "\n";
    return 0;
}
