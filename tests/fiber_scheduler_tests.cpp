#include "realscript/runtime/FiberScheduler.h"
#include "realscript/game/DeterministicMath.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/bytecode/Bytecode.h"
#include "realscript/runtime/Runtime.h"
#include "realscript/aot_cpp/AotCpp.h"

#include <array>
#include <cassert>
#include <chrono>
#include <iostream>
#include <type_traits>
#include <vector>

using Opcode = realscript::runtime::FiberOpcode;
using namespace realscript::runtime;
using namespace realscript::game;

namespace {

/**
 * Test-local mock game adapter implementing `IFiberHostCallbacks` (`onHostCommand` / `onEvaluateCondition`).
 * Kept strictly in test code so the RealScript engine core stays 100% game-agnostic.
 */
class TestEntityHostFixture final : public IFiberHostCallbacks {
public:
    struct SpriteSoA {
        std::vector<std::uint16_t> spriteId;
        std::vector<std::uint16_t> frameSet;
        std::vector<std::uint16_t> lastOverlaySpawned;
        std::vector<std::uint16_t> lastSoundCuePlayed;
        std::vector<std::uint32_t> combatHitsTriggered;
        std::vector<std::uint32_t> generations;

        void resize(std::size_t cap) {
            spriteId.assign(cap, 0);
            frameSet.assign(cap, 0);
            lastOverlaySpawned.assign(cap, 0);
            lastSoundCuePlayed.assign(cap, 0);
            combatHitsTriggered.assign(cap, 0);
            generations.assign(cap, 0);
        }
    };

    struct GroupBlackboard {
        std::uint8_t enemyGroup{0};
        std::array<std::uint16_t, 256> maxUnitCaps{};
        std::array<std::uint16_t, 256> buildOrderQueued{};
        std::array<std::uint8_t, 64> upgradesQueued{};
        std::array<std::uint8_t, 64> techsQueued{};
        std::array<std::array<std::uint16_t, 256>, 4> defenseForces{};
        std::array<std::uint16_t, 256> waveRoster{};
        std::uint32_t attackWavesDispatched{0};
        std::uint32_t lastAttackWaveSize{0};
    };

    explicit TestEntityHostFixture(std::size_t maxEntities = 2048) {
        sprites_.resize(maxEntities);
        freeSlots_.reserve(maxEntities);
        for (std::size_t i = maxEntities; i > 0; --i) {
            freeSlots_.push_back(static_cast<std::uint32_t>(i - 1));
        }
    }

    GenerationalEntityHandle allocateEntity(std::uint16_t assetId) {
        if (freeSlots_.empty()) return GenerationalEntityHandle{};
        std::uint32_t slot = freeSlots_.back();
        freeSlots_.pop_back();
        std::uint32_t gen = ++sprites_.generations[slot];
        if (gen == 0) gen = ++sprites_.generations[slot];
        sprites_.spriteId[slot] = assetId;
        sprites_.frameSet[slot] = 0;
        sprites_.lastOverlaySpawned[slot] = 0;
        sprites_.lastSoundCuePlayed[slot] = 0;
        sprites_.combatHitsTriggered[slot] = 0;
        return GenerationalEntityHandle{slot, gen};
    }

    void destroyEntity(GenerationalEntityHandle handle) noexcept {
        if (!isEntityAlive(handle)) return;
        ++sprites_.generations[handle.slot];
        freeSlots_.push_back(handle.slot);
    }

    [[nodiscard]] bool isEntityAlive(GenerationalEntityHandle handle) const noexcept {
        return handle.valid() &&
               handle.slot < sprites_.generations.size() &&
               sprites_.generations[handle.slot] == handle.generation;
    }

    void setSpriteFrame(GenerationalEntityHandle handle, std::uint16_t frame) noexcept {
        if (!isEntityAlive(handle)) return;
        sprites_.frameSet[handle.slot] = frame;
    }

    void onHostCommand(
        FiberContext& fiber,
        std::uint8_t commandId,
        std::uint16_t arg16,
        std::uint32_t arg32) noexcept override {
        auto& bb = blackboards_[fiber.ownerGroupId & 7u];
        switch (commandId) {
        case 1: // SetSpriteFrame
            setSpriteFrame(fiber.boundEntity, static_cast<std::uint16_t>(arg32 & 0xFFFFu));
            break;
        case 2: // SpawnSocketOverlay
            if (isEntityAlive(fiber.boundEntity)) {
                sprites_.lastOverlaySpawned[fiber.boundEntity.slot] = static_cast<std::uint16_t>(arg32 & 0xFFFFu);
            }
            break;
        case 3: // PlaySoundCue
            if (isEntityAlive(fiber.boundEntity)) {
                sprites_.lastSoundCuePlayed[fiber.boundEntity.slot] = static_cast<std::uint16_t>(arg32 & 0xFFFFu);
            }
            break;
        case 4: // TriggerCombatHit
            if (isEntityAlive(fiber.boundEntity)) {
                ++sprites_.combatHitsTriggered[fiber.boundEntity.slot];
            }
            break;
        case 10: // DefineMaxUnitCap
            bb.maxUnitCaps[arg16 & 0xFFu] = static_cast<std::uint16_t>(arg32 & 0xFFFFu);
            break;
        case 11: { // AIBuildOrder
            std::uint16_t count = static_cast<std::uint16_t>(arg32 & 0xFFFFu);
            std::uint16_t cap = bb.maxUnitCaps[arg16 & 0xFFu];
            if (cap > 0 && count > cap) count = cap;
            bb.buildOrderQueued[arg16 & 0xFFu] = count;
            break;
        }
        case 12: // AIResearchUpgrade
            bb.upgradesQueued[arg16 & 63u] = static_cast<std::uint8_t>(arg32 & 0xFFu);
            break;
        case 13: // AIResearchTech
            bb.techsQueued[arg16 & 63u] = 1;
            break;
        case 14: { // AIDispatchWave
            std::uint16_t explicitSize = static_cast<std::uint16_t>(arg32 & 0xFFFFu);
            if (explicitSize == 0) {
                std::uint32_t sum = 0;
                for (std::uint16_t c : bb.waveRoster) sum += c;
                bb.lastAttackWaveSize = sum;
            } else {
                bb.lastAttackWaveSize = explicitSize;
            }
            ++bb.attackWavesDispatched;
            break;
        }
        case 15: // AIUpdateDefense
            bb.defenseForces[arg16 & 3u][(arg32 >> 16) & 0xFFu] = static_cast<std::uint16_t>(arg32 & 0xFFFFu);
            break;
        case 16: // AIWaveClear
            bb.waveRoster.fill(0);
            break;
        case 17: // AIWaveAdd
            bb.waveRoster[arg16 & 0xFFu] =
                static_cast<std::uint16_t>(bb.waveRoster[arg16 & 0xFFu] + static_cast<std::uint16_t>(arg32 & 0xFFFFu));
            break;
        default:
            break;
        }
    }

    [[nodiscard]] bool onEvaluateCondition(
        const FiberContext& fiber,
        std::uint8_t conditionKind,
        std::uint16_t key16,
        std::uint16_t threshold16) const noexcept override {
        (void)threshold16;
        const auto& bb = blackboards_[fiber.ownerGroupId & 7u];
        if (conditionKind == 1) {
            return bb.enemyGroup == static_cast<std::uint8_t>(key16 & 0xFFu);
        }
        return false;
    }

    [[nodiscard]] const SpriteSoA& sprites() const noexcept { return sprites_; }
    [[nodiscard]] GroupBlackboard& groupBlackboard(std::uint8_t groupId) noexcept {
        return blackboards_[groupId & 7u];
    }
    [[nodiscard]] const GroupBlackboard& groupBlackboard(std::uint8_t groupId) const noexcept {
        return blackboards_[groupId & 7u];
    }

private:
    SpriteSoA sprites_;
    std::vector<std::uint32_t> freeSlots_;
    std::array<GroupBlackboard, 8> blackboards_{};
};

} // namespace

int main() {
    std::cout << "=== RealScript FiberScheduler (.rs Lowering) + Math Verification ===\n";

    // 1. Verify 8-byte CompactInstruction, 8-byte GenerationalEntityHandle, and zero-pointer POD FiberContext
    static_assert(sizeof(CompactInstruction) == 8, "CompactInstruction must be 8 bytes");
    static_assert(sizeof(GenerationalEntityHandle) == 8, "GenerationalEntityHandle must be 8 bytes");
    static_assert(std::is_trivially_copyable_v<FiberContext>, "FiberContext must be POD with no raw pointers");
    std::cout << "[PASS] sizeof(CompactInstruction) = " << sizeof(CompactInstruction)
              << " B (8 instructions per 64B cache line), FiberContext is trivially copyable\n";

    // 2. Verify generic FixedPoint32<FracBits> deterministic math (Q24.8 and Q16.16)
    Fixed24_8 topSpeed = Fixed24_8::fromFloat(5.33203125);
    Fixed24_8 ticks = Fixed24_8::fromInt(24);
    [[maybe_unused]] Fixed24_8 distPerSec = topSpeed * ticks;
    assert(distPerSec.toIntFloor() == 127);

    [[maybe_unused]] Fixed16_16 physicsVel = Fixed16_16::fromFloat(1.5);
    [[maybe_unused]] Fixed16_16 physicsTime = Fixed16_16::fromInt(10);
    assert((physicsVel * physicsTime).toIntFloor() == 15);
    std::cout << "[PASS] FixedPoint32<8> (Q24.8) & FixedPoint32<16> (Q16.16) deterministic math verified\n";

    // 3. Compile RealScript (.rs) fiber coroutines & AI Behavior Trees directly into FiberScheduler!
    realscript::compiler::Compilation fiberCompilation;
    fiberCompilation.addSource(realscript::compiler::SourceFile{
        "entity_fibers.rs",
        R"RS(
module EntityBehaviors;
import RealScript.Fiber;

// Zero-overhead .rs host command helpers (inlined by FiberCompiler into 8B CompactInstructions)
void SetSpriteFrame(int frameOffset) {
    Fiber.Command(1, 0, frameOffset);
}

void SpawnSocketOverlay(int overlayAssetId, int socketSlot, int aboveFlag) {
    Fiber.Command(2, socketSlot, aboveFlag * 65536 + overlayAssetId);
}

void PlaySoundCue(int soundAssetId) {
    Fiber.Command(3, 0, soundAssetId);
}

void TriggerCombatHit(int weaponSlot) {
    Fiber.Command(4, weaponSlot, 0);
}

void DefineMaxUnitCap(int unitTypeId, int maxCap) {
    Fiber.Command(10, unitTypeId, maxCap);
}

void AIBuildOrder(int unitTypeId, int count, int priority) {
    Fiber.Command(11, unitTypeId, priority * 65536 + count);
}

void AIResearchUpgrade(int upgradeId, int targetLevel, int priority) {
    Fiber.Command(12, upgradeId, priority * 65536 + targetLevel);
}

void AIResearchTech(int techId, int priority) {
    Fiber.Command(13, techId, priority);
}

void AIDispatchWave(int explicitSize) {
    Fiber.Command(14, 0, explicitSize);
}

void AIUpdateDefense(int category, int unitTypeId, int count) {
    Fiber.Command(15, category, unitTypeId * 65536 + count);
}

void AIWaveClear() {
    Fiber.Command(16, 0, 0);
}

void AIWaveAdd(int unitTypeId, int count) {
    Fiber.Command(17, unitTypeId, count);
}

// High-frequency unit harvest/work animation coroutine written in .rs
public void WorkerHarvestLoop() {
    while (true) {
        Fiber.SetUnbreakable(true);
        SetSpriteFrame(2);
        SpawnSocketOverlay(431, 0, 1);
        PlaySoundCue(65);
        TriggerCombatHit(0);
        Fiber.Yield(2);
        Fiber.SetUnbreakable(false);
        SetSpriteFrame(3);
        Fiber.Yield(2);
    }
}

// Background tactical defense daemon spawned via Fiber.Fork("DefenseDaemon")
public void DefenseDaemon() {
    while (true) {
        AIUpdateDefense(0, 0, 4);    // GroundToGround: 4 Infantry
        AIUpdateDefense(2, 124, 3);  // GroundToAir: 3 Turrets
        AIBuildOrder(124, 3, 60);
        Fiber.Yield(10);
    }
}

// Strategic AI Behavior Tree written in .rs
public void CommanderBehaviorTree() {
    DefineMaxUnitCap(0, 24);
    DefineMaxUnitCap(5, 8);

    // BT_Parallel: Fork background DefenseDaemon fiber
    Fiber.Fork("DefenseDaemon");

    // BT_ConditionDecorator: Check if EnemyGroup == 1 (ConditionKind=1, Key=1, Threshold=0)
    if (!Fiber.Cond(1, 1, 0)) {
        AIBuildOrder(7, 2, 80);
    }

    // BT_Sequence: Build & Tech Order Phase
    AIBuildOrder(7, 4, 80);
    AIBuildOrder(111, 2, 80);
    AIResearchUpgrade(0, 1, 80);
    AIResearchTech(0, 80);

    // Event-driven wait until 2 structures (unitTypeId=111) complete on channel 1
    Fiber.WaitUntil(1, 111, 2);

    // BT_Sequence: Assemble & dispatch attack wave (12 Infantry + 6 Tanks = 18)
    AIWaveClear();
    AIWaveAdd(0, 12);
    AIWaveAdd(5, 6);
    AIDispatchWave(0);
}
)RS"});

    auto fiberBuild = fiberCompilation.build();
    if (fiberBuild.diagnostics.hasErrors()) {
        for (const auto& diag : fiberBuild.diagnostics.items()) {
            std::cerr << "Fiber Script Compile Error [" << diag.code << "]: " << diag.message << "\n";
        }
    }
    assert(!fiberBuild.diagnostics.hasErrors());

    // Find the compiled `EntityBehaviors` MIR module and lower it to an 8-byte CompactInstruction stream
    const realscript::mir::Module* entityBehaviorsMir = nullptr;
    for (const auto& m : fiberBuild.modules) {
        if (m.name == "EntityBehaviors") {
            entityBehaviorsMir = &m;
            break;
        }
    }
    assert(entityBehaviorsMir != nullptr);

    CompiledFiberModule fiberModule = FiberCompiler::compileModule(*entityBehaviorsMir);
    assert(fiberModule.hasFunction("WorkerHarvestLoop"));
    assert(fiberModule.hasFunction("DefenseDaemon"));
    assert(fiberModule.hasFunction("CommanderBehaviorTree"));

    TestEntityHostFixture host(2048);
    // Configure Group 1 Blackboard: enemyGroup is 1
    host.groupBlackboard(1).enemyGroup = 1;

    FiberScheduler scheduler(&host);

    // Spawn 1,600 concurrent unit animation fibers + 1 AI Behavior Tree root fiber directly from compiled .rs functions!
    constexpr std::size_t NUM_UNITS = 1600;
    std::vector<GenerationalEntityHandle> units;
    units.reserve(NUM_UNITS);
    for (std::size_t i = 0; i < NUM_UNITS; ++i) {
        auto h = host.allocateEntity(247);
        units.push_back(h);
        scheduler.spawnFunction(fiberModule, "WorkerHarvestLoop", h, 1, 0, static_cast<std::uint32_t>(1000 + i));
    }
    scheduler.spawnFunction(fiberModule, "CommanderBehaviorTree", GenerationalEntityHandle{}, 1, 0, 0xCAFEBABEu);

    // Step 120 logical ticks
    auto t0 = std::chrono::high_resolution_clock::now();
    std::uint64_t totalInstr = 0;
    std::vector<std::uint8_t> snapshotAtTick100;
    std::uint64_t digestAtTick120_RunA = 0;

    for (std::uint32_t tick = 0; tick <= 120; ++tick) {
        totalInstr += scheduler.stepTick(tick);
        if (tick == 50) {
            // Simulate 2 structures (id=111) completing construction at tick 50 -> wakes CommanderBehaviorTree fiber!
            std::uint32_t woken = scheduler.notifyBlackboardEvent(1, 1, 111, 2);
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

    // Verify ECS SoA zero-copy state updates & AI Blackboard semantics driven by .rs fibers
    [[maybe_unused]] const auto& bb = host.groupBlackboard(1);
    assert(host.sprites().lastOverlaySpawned[units[0].slot] == 431);
    assert(host.sprites().lastSoundCuePlayed[units[0].slot] == 65);
    assert(host.sprites().combatHitsTriggered[units[0].slot] > 25);
    assert(bb.maxUnitCaps[0] == 24 && bb.maxUnitCaps[5] == 8);
    assert(bb.buildOrderQueued[7] == 4);
    assert(bb.buildOrderQueued[111] == 2);
    assert(bb.upgradesQueued[0] == 1);
    assert(bb.techsQueued[0] == 1);
    assert(bb.defenseForces[0][0] == 4);
    assert(bb.defenseForces[2][124] == 3);
    assert(bb.buildOrderQueued[124] == 3); // From forked DefenseDaemon .rs function!
    assert(bb.attackWavesDispatched >= 1);
    assert(bb.lastAttackWaveSize == 18);   // Computed from waveRoster (12 + 6)

    // Verify Generational Entity Handle safety (destroying an entity invalidates stale fiber writes)
    auto tempUnit = host.allocateEntity(100);
    host.setSpriteFrame(tempUnit, 7);
    assert(host.sprites().frameSet[tempUnit.slot] == 7);
    host.destroyEntity(tempUnit);
    host.setSpriteFrame(tempUnit, 99); // Stale handle write must be ignored!
    assert(host.sprites().frameSet[tempUnit.slot] == 7);

    // Verify Cross-Scheduler Binary Snapshot Restore (proving zero raw pointer dependency)
    FiberScheduler freshScheduler(&host);
    freshScheduler.registerModule(fiberModule);
    freshScheduler.restoreBinarySnapshot(snapshotAtTick100);
    for (std::uint32_t tick = 101; tick <= 120; ++tick) {
        freshScheduler.stepTick(tick);
    }
    std::uint64_t digestAtTick120_RunB = freshScheduler.computeLockstepDigest();
    (void)digestAtTick120_RunB;
    assert(digestAtTick120_RunA == digestAtTick120_RunB);

    std::cout << "[PASS] 1,600 concurrent .rs WorkerHarvestLoop fibers + Forked .rs CommanderBehaviorTree stepped 120 ticks in "
              << elapsedMs << " ms (" << (elapsedMs / 120.0) << " ms/tick, " << totalInstr << " instructions)\n";
    std::cout << "[PASS] .rs -> FiberCompiler lowering (inlined helpers, Fiber.Fork, Fiber.Cond, Fiber.WaitUntil) verified\n";
    std::cout << "[PASS] Cross-instance Lockstep Snapshot Restore & 64-bit FNV-1a Digest verified: 0x"
              << std::hex << digestAtTick120_RunA << std::dec << "\n";

    // 4. Verify Direction #1 (Operator Overloading & Compound Assignment) and
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
