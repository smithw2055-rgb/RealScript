#include "realscript/runtime/RTSFiberScheduler.h"
#include "realscript/game/StarCraftRTSBridge.h"
#include "realscript/game/DeterministicMath.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/bytecode/Bytecode.h"
#include "realscript/runtime/Runtime.h"
#include "realscript/aot_cpp/AotCpp.h"

#include <cassert>
#include <chrono>
#include <iostream>
#include <type_traits>
#include <vector>

using Opcode = realscript::runtime::FiberOpcode;
using namespace realscript::runtime;
using namespace realscript::game;

int main() {
    std::cout << "=== RealScript Engine-Agnostic Fiber Core + StarCraft Adapter Verification ===\n";

    // 1. Verify 8-byte CompactInstruction, 8-byte GenerationalEntityHandle, and zero-pointer POD RTSFiberContext
    static_assert(sizeof(CompactInstruction) == 8, "CompactInstruction must be 8 bytes");
    static_assert(sizeof(GenerationalEntityHandle) == 8, "GenerationalEntityHandle must be 8 bytes");
    static_assert(std::is_trivially_copyable_v<RTSFiberContext>, "RTSFiberContext must be POD with no raw pointers");
    std::cout << "[PASS] sizeof(CompactInstruction) = " << sizeof(CompactInstruction)
              << " B (8 instructions per 64B cache line), RTSFiberContext is trivially copyable\n";

    // 2. Verify generic FixedPoint32<FracBits> deterministic math (Q24.8 and Q16.16)
    Fixed24_8 topSpeed = Fixed24_8::fromFloat(5.33203125); // SCV speed in Q24.8
    Fixed24_8 ticks = Fixed24_8::fromInt(24);
    Fixed24_8 distPerSec = topSpeed * ticks;
    assert(distPerSec.toIntFloor() == 127);

    Fixed16_16 physicsVel = Fixed16_16::fromFloat(1.5);
    Fixed16_16 physicsTime = Fixed16_16::fromInt(10);
    assert((physicsVel * physicsTime).toIntFloor() == 15);
    std::cout << "[PASS] FixedPoint32<8> (Q24.8) & FixedPoint32<16> (Q16.16) deterministic math verified\n";

    // 3. Build a compiled StarCraft SCV IScript_084 animation program using generic opcodes + sc_instr adapter:
    //    0: SetUnbreakable(true) -> nobrkcodestart
    //    1: sc_instr::setSpriteFrame(2)
    //    2: sc_instr::spawnSocketOverlay(431 scvspark.grp, socketSlot=0, above=true)
    //    3: sc_instr::playSoundCue(65 EDrRep00.wav)
    //    4: sc_instr::triggerCombatHit(0 Fusion Cutter)
    //    5: YieldTicks(2 ticks = 83.3ms)
    //    6: SetUnbreakable(false) -> nobrkcodeend
    //    7: sc_instr::setSpriteFrame(3)
    //    8: YieldTicks(2 ticks)
    //    9: Jump(0) -> Loops back to PC=0
    std::vector<CompactInstruction> scvIsWorkingStream = {
        CompactInstruction(Opcode::SetUnbreakable, 1, 0, 0),
        sc_instr::setSpriteFrame(2),
        sc_instr::spawnSocketOverlay(431, 0, true),
        sc_instr::playSoundCue(65),
        sc_instr::triggerCombatHit(0),
        CompactInstruction(Opcode::YieldTicks, 0, 0, 2),
        CompactInstruction(Opcode::SetUnbreakable, 0, 0, 0),
        sc_instr::setSpriteFrame(3),
        CompactInstruction(Opcode::YieldTicks, 0, 0, 2),
        CompactInstruction(Opcode::Jump, 0, 0, 0)
    };

    // 4. Build a full StarCraft AI Behavior Tree stream (`Ter3` / `PB2A`) covering all PDF node types:
    //    - Blackboard layer: define_max (Marine=24, SiegeTank=8)
    //    - BT_Parallel: ForkFiber -> spawns background expansion/defense daemon at PC=14
    //    - BT_ConditionDecorator: race_jump (if EnemyRace == Zerg(1), jump to Zerg counter branch at PC=6)
    //    - BT_Sequence (BuildAndTechOrderPhase): build SCV/Barracks, upgrade Metabolic/U238, tech StimPacks, wait_build
    //    - BT_Action_UpdateDefenseForce: defensebuild_gg / defenseuse_aa
    //    - BT_Sequence (ExecuteAttackWave): attack_clear -> attack_add(Marine=12, Tank=6) -> attack_do
    std::vector<CompactInstruction> terranAIBehaviorStream = {
        // PC 0..1: Blackboard max_unit_caps
        sc_instr::defineMaxUnitCap(0, 24),   // Terran_Marine max 24
        sc_instr::defineMaxUnitCap(5, 8),    // Terran_Siege_Tank max 8
        // PC 2: BT_Parallel (multirun background daemon at PC=14)
        CompactInstruction(Opcode::ForkFiber, 0, 0, 14),
        // PC 3: BT_SwitchSelector_EnemyRace (if enemyRace == 1 (Zerg) -> jump to PC=5)
        sc_instr::aiCondJump(StarCraftConditionKind::EnemyRaceEquals, 5, 1, 0),
        // PC 4: Non-Zerg fallback (skipped when enemyRace == 1)
        sc_instr::aiBuildOrder(7, 2, 80),
        // PC 5..8: BT_Sequence: BuildAndTechOrderPhase
        sc_instr::aiBuildOrder(7, 4, 80),    // Build 4 Terran_SCV (unitId=7)
        sc_instr::aiBuildOrder(111, 2, 80),  // Build 2 Terran_Barracks (unitId=111)
        sc_instr::aiResearchUpgrade(0, 1, 80), // Upgrade Terran Infantry Armor (upgradeId=0, level=1)
        sc_instr::aiResearchTech(0, 80),     // Tech Stim_Packs (techId=0)
        // PC 9: Event-Driven wait_build (Barracks=111, count=2)
        CompactInstruction(
            Opcode::YieldUntilPredicate,
            static_cast<uint8_t>(WaitPredicateKind::UnitOrBuildingCompleted),
            111,
            2),
        // PC 10..12: BT_Sequence: ExecuteAttackWave (wave_roster: 12 Marine + 6 Siege Tank = 18)
        sc_instr::aiWaveClear(),
        sc_instr::aiWaveAdd(0, 12),
        sc_instr::aiWaveAdd(5, 6),
        // PC 13: Dispatch wave & terminate main phase
        sc_instr::aiDispatchWave(0),
        // PC 14..17: Background multirun daemon (BT_Action_UpdateDefenseForce + Turret build loop)
        sc_instr::aiUpdateDefense(DefenseCategory::GroundToGround, 0, 4), // gg: 4 Marines
        sc_instr::aiUpdateDefense(DefenseCategory::GroundToAir, 124, 3),  // ga: 3 Missile Turrets
        sc_instr::aiBuildOrder(124, 3, 60),
        CompactInstruction(Opcode::YieldTicks, 0, 0, 10),
        CompactInstruction(Opcode::Jump, 0, 0, 14)
    };

    StarCraftRTSBridge bridge(2048);
    // Configure Faction 1 Blackboard: enemy is Zerg (1)
    bridge.factionBlackboard(1).enemyRace = 1;

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

    // Step 120 logical ticks
    auto t0 = std::chrono::high_resolution_clock::now();
    uint64_t totalInstr = 0;
    std::vector<uint8_t> snapshotAtTick100;
    uint64_t digestAtTick120_RunA = 0;

    for (uint32_t tick = 0; tick <= 120; ++tick) {
        totalInstr += scheduler.stepTick(tick);
        if (tick == 50) {
            // Simulate 2 Terran Barracks completing construction at tick 50 -> wakes AI fiber!
            uint32_t woken = scheduler.notifyBlackboardEvent(
                1,
                static_cast<uint8_t>(WaitPredicateKind::UnitOrBuildingCompleted),
                111,
                2);
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

    // Verify ECS SoA zero-copy state updates & full PDF AI Blackboard semantics
    const auto& bb = bridge.factionBlackboard(1);
    assert(bridge.sprites().lastOverlaySpawned[units[0].slot] == 431);
    assert(bridge.sprites().lastSoundCuePlayed[units[0].slot] == 65);
    assert(bridge.sprites().combatHitsTriggered[units[0].slot] > 25);
    assert(bb.maxUnitCaps[0] == 24 && bb.maxUnitCaps[5] == 8);
    assert(bb.buildOrderQueued[7] == 4);
    assert(bb.buildOrderQueued[111] == 2);
    assert(bb.upgradesQueued[0] == 1);
    assert(bb.techsQueued[0] == 1);
    assert(bb.defenseForces[static_cast<size_t>(DefenseCategory::GroundToGround)][0] == 4);
    assert(bb.defenseForces[static_cast<size_t>(DefenseCategory::GroundToAir)][124] == 3);
    assert(bb.buildOrderQueued[124] == 3); // From forked multirun daemon!
    assert(bb.attackWavesDispatched >= 1);
    assert(bb.lastAttackWaveSize == 18);   // Computed from wave_roster (12 Marines + 6 Siege Tanks)

    // Verify Generational Entity Handle safety (destroying an entity invalidates stale fiber writes)
    auto tempUnit = bridge.allocateUnitEntity(100);
    bridge.onSetSpriteFrame(tempUnit, 7);
    assert(bridge.sprites().frameSet[tempUnit.slot] == 7);
    bridge.destroyUnitEntity(tempUnit);
    bridge.onSetSpriteFrame(tempUnit, 99); // Stale handle write must be ignored!
    assert(bridge.sprites().frameSet[tempUnit.slot] == 7);

    // Verify Cross-Scheduler Binary Snapshot Restore (proving zero raw pointer dependency)
    RTSFiberScheduler freshScheduler(&bridge);
    freshScheduler.registerProgram(scvIsWorkingStream);
    freshScheduler.registerProgram(terranAIBehaviorStream);
    freshScheduler.restoreBinarySnapshot(snapshotAtTick100);
    for (uint32_t tick = 101; tick <= 120; ++tick) {
        freshScheduler.stepTick(tick);
    }
    uint64_t digestAtTick120_RunB = freshScheduler.computeLockstepDigest();
    (void)digestAtTick120_RunB;
    assert(digestAtTick120_RunA == digestAtTick120_RunB);

    std::cout << "[PASS] 1,600 concurrent SCV fibers + Forked AI Behavior Tree stepped 120 ticks in "
              << elapsedMs << " ms (" << (elapsedMs / 120.0) << " ms/tick, " << totalInstr << " instructions)\n";
    std::cout << "[PASS] Full PDF Behavior Tree semantics (Blackboard caps, RaceCondJump, WaveRoster=18, 4D Defense) verified\n";
    std::cout << "[PASS] Cross-instance Lockstep Snapshot Restore & 64-bit FNV-1a Digest verified: 0x"
              << std::hex << digestAtTick120_RunA << std::dec << "\n";

    // 5. Verify Direction #1 (Operator Overloading & Compound Assignment) and
    //    Direction #2 (Built-in Deterministic Fixed-Point & Vector Math Library: RealScript.Math)
    {
        realscript::compiler::Compilation compilation;
        compilation.addSource(realscript::compiler::SourceFile{
            "deterministic_gameplay_math.rs",
            R"RS(
module GameSim;
import RealScript.Math;

public class UnitBody {
    public Vec2 pos;
    public Vec2 vel;
    public int energy;

    public UnitBody(Vec2 initialPos, Vec2 initialVel) {
        pos = initialPos;
        vel = initialVel;
        energy = 100;
    }

    public void StepTick(Fixed dt) {
        pos += vel * dt;
        this.energy -= 3;
        energy *= 2;
        energy /= 2;
    }
}

public int RunDeterministicPhysicsSimulation() {
    // 1. Fixed & Vec2 operator overloading + compound assignment + int mixed operators
    Vec2 start = new Vec2(3, 4);
    if (start.Length() != 5) return -1;

    Vec2 neg = -start;
    if (neg.x != -3 || neg.y != -4) return -2;

    UnitBody body = new UnitBody(new Vec2(10, 20), new Vec2(4, -2));
    Fixed halfSec = Fixed.Half(); // 0.5s (raw = 128)

    // Step 4 ticks of 0.5s each (total 2.0s: dx = +8, dy = -4 -> pos = (18, 16))
    int tick = 0;
    while (tick < 4) {
        body.StepTick(halfSec);
        tick += 1;
    }
    if (body.pos != new Vec2(18, 16)) return -3;
    if (body.energy != 88) return -4;

    // 2. MoveTowards, Dot, Cross, Manhattan
    Vec2 target = new Vec2(21, 20); // delta = (3, 4), distance = 5
    Vec2 stepped = Vec2.MoveTowards(body.pos, target, 5);
    if (stepped != target) return -5;

    Fixed manhattan = Vec2.Manhattan(body.pos, target); // |3| + |4| = 7
    if (manhattan != 7) return -6;

    // 3. Vec3 3D Cross & Dot product
    Vec3 axisX = new Vec3(1, 0, 0);
    Vec3 axisY = new Vec3(0, 1, 0);
    Vec3 axisZ = Vec3.Cross(axisX, axisY);
    if (axisZ != new Vec3(0, 0, 1)) return -7;

    // 4. Fixed16 high-precision fixed-point & Sqrt
    Fixed16 hp = Fixed16.Of(9);
    hp += 16; // 25
    Fixed16 root = Fixed16.Sqrt(hp);
    if (root != 5) return -8;

    // 5. Array element compound assignment
    int[] counters = new int[2];
    counters[0] = 10;
    counters[0] += 5;
    counters[0] *= 2;
    counters[0] %= 7; // (15 * 2) % 7 = 2
    if (counters[0] != 2) return -9;

    return body.pos.x.ToInt() * 100 + body.pos.y.ToInt() + counters[0];
}
)RS"});

        auto build = compilation.build();
        if (build.diagnostics.hasErrors()) {
            for (const auto& diag : build.diagnostics.items()) {
                std::cerr << "Compile Error [" << diag.code << "]: " << diag.message << "\n";
            }
        }
        assert(!build.diagnostics.hasErrors());

        // Lower to Bytecode & execute in Interpreter
        realscript::bytecode::Lowerer lowerer;
        std::vector<realscript::bytecode::Module> bcModules;
        for (const auto& mirMod : build.modules) {
            bcModules.push_back(lowerer.lower(mirMod));
        }
        realscript::runtime::Interpreter interpreter(std::move(bcModules));
        auto execResult = interpreter.invoke("GameSim::RunDeterministicPhysicsSimulation");
        if (!execResult.succeeded) {
            std::cerr << "Runtime Error: " << execResult.error.message << "\n";
        }
        assert(execResult.succeeded);
        assert(std::holds_alternative<std::int64_t>(execResult.value));
        assert(std::get<std::int64_t>(execResult.value) == 1818);

        // Verify C++17 AOT Backend generates clean C++ code for overloaded operators & RealScript.Math structs
        realscript::diagnostics::DiagnosticBag aotDiagnostics;
        realscript::aot::CppGenerator aotGen;
        auto aotProgram = aotGen.generate(build.modules, aotDiagnostics);
        assert(!aotDiagnostics.hasErrors());
        assert(aotProgram.source.find("op_Addition") != std::string::npos);
        assert(aotProgram.source.find("op_Multiply") != std::string::npos);

        // Verify C++ host Vec2 / Vec3 parity with script results
        Vec2 hostStart{3, 4};
        assert(hostStart.length() == Fixed::fromInt(5));

        std::cout << "[PASS] Direction #1 (Operator Overloading & Compound Assignment) verified across Bytecode & C++17 AOT\n";
        std::cout << "[PASS] Direction #2 (Built-in RealScript.Math: Fixed, Fixed16, Vec2, Vec3) verified (result="
                  << std::get<std::int64_t>(execResult.value) << ")\n";
    }

    return 0;
}
