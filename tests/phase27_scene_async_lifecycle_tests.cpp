#include "realscript/game/Gameplay.h"
#include "realscript/runtime/AsyncRuntime.h"

#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace realscript;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string diagnosticsText(const diagnostics::DiagnosticBag& bag) {
    std::string out;
    for (const auto& d : bag.items()) {
        out += d.code + ": " + d.message + "\n";
    }
    return out;
}

// ============================================================================
// 1. Async Lifecycle `OnStart()` + Main-Thread Prefab Streaming, Asset Loading
//    & SaveGame Read/Write via `RealScript.Scene`
// ============================================================================
void testAsyncLifecycleStreamingAssetAndSaveLoad() {
    game::GameApi api;
    // Dummy runtime reference just for binding registration; rebound via scene below
    const std::string scriptSource = R"RS(
module Phase27.Scene;
import RealScript.Async;
import RealScript.Scene;

public class WeaponAsset {
    public string displayName;
    public int baseDamage;
    public int validatedDamage;

    public void OnValidate() {
        validatedDamage = baseDamage * 2;
    }
}

public class TurretUnit {
    public int unitCode;
    public bool created;
    public bool enabledFlag;
    public int startCalls;
    public int updateTicks;

    public void OnCreate() {
        created = true;
    }

    public void OnEnable() {
        enabledFlag = true;
    }

    public void OnStart() {
        startCalls = startCalls + 1;
    }

    public void OnUpdate(double dt) {
        if (dt >= 0.0) {
            updateTicks = updateTicks + 1;
        }
    }
}

public class LevelDirector {
    public long selfEntity;
    public string loadedSave;
    public int loadedDamage;
    public string loadedWeaponName;
    public long spawnedSquadRoot;
    public int squadChildrenCheck;
    public bool completed;

    public async Awaitable OnStart() {
        CancellationToken destroyToken = EntityDestroyToken(selfEntity);
        bool saved = await SaveGameAsync("slot_alpha", "chapter=3;coins=450", destroyToken);
        if (!saved) {
            return;
        }
        loadedSave = await LoadGameAsync("slot_alpha", destroyToken);

        Awaitable[] batch = new Awaitable[2];
        Awaitable<bool> assetTask = LoadAssetAsync(
            "assets/weapons/sunblade",
            "Phase27.Scene::WeaponAsset",
            destroyToken);
        Awaitable yieldTask = Awaitable.YieldWithToken(destroyToken);
        batch[0] = assetTask.AsUntyped();
        batch[1] = yieldTask;
        await Async.WhenAllWithToken(batch, destroyToken);

        if (HasLoadedAsset("assets/weapons/sunblade")) {
            loadedDamage = GetLoadedAssetInt("assets/weapons/sunblade", "validatedDamage");
            loadedWeaponName = GetLoadedAssetString("assets/weapons/sunblade", "displayName");
        }

        spawnedSquadRoot = await LoadPrefabSceneAsync("SquadPrefab", selfEntity, destroyToken);
        if (spawnedSquadRoot > 0L && GetParent(spawnedSquadRoot) == selfEntity) {
            squadChildrenCheck = HasComponent(spawnedSquadRoot, "Phase27.Scene::TurretUnit") ? 1 : 0;
        }
        completed = true;
    }
}
)RS";

    // Build with temporary dummy scene holder? Notice installSceneComponentBindings captures `&scene`,
    // whereas ScriptRuntime needs `GameProgram`! Look at how existing SceneScriptRuntime tests initialize:
    // Let's construct an empty ScriptRuntime first, bind `scene`, compile `GameProgram`, and `replaceProgram`!
    game::ScriptRuntime runtime(
        nullptr,
        api.bindings(),
        api.heap(),
        api.nativeHandles());
    game::SceneScriptRuntime scene(runtime);
    require(game::installSceneComponentBindings(api, scene), "installSceneComponentBindings failed");

    game::GameScriptCompiler compiler(api);
    const auto compiled = compiler.compile({{"phase27_scene.rscript", scriptSource}});
    require(compiled.succeeded(), "compile failed:\n" + diagnosticsText(compiled.diagnostics));
    runtime.replaceProgram(compiled.program);

    scene.registerAsyncAssetDefinition(
        "assets/weapons/sunblade",
        {
            {"displayName", std::string("Sunblade Mk.II")},
            {"baseDamage", std::int64_t{95}},
        });

    game::PrefabNodeTemplate squadPrefab;
    squadPrefab.name = "SquadRoot";
    game::PrefabComponentTemplate turretComp;
    turretComp.scriptType = "Phase27.Scene::TurretUnit";
    turretComp.options.initialMembers["unitCode"] = std::int64_t{77};
    squadPrefab.components.push_back(turretComp);

    game::PrefabNodeTemplate childWing;
    childWing.name = "SquadWingman";
    childWing.components.push_back(turretComp);
    squadPrefab.children.push_back(childWing);
    scene.registerPrefabTemplate("SquadPrefab", squadPrefab);

    const game::SceneEntityId directorId = scene.spawnGenerationalEntity("Director");
    game::SceneScriptOptions opts;
    opts.initialMembers["selfEntity"] = runtime::LongValue{static_cast<std::int64_t>(directorId)};
    require(scene.attachComponent(directorId, "Phase27.Scene::LevelDirector", opts),
        "failed to attach LevelDirector");

    scene.start();
    for (int frame = 0; frame < 6; ++frame) {
        scene.update(0.016);
    }

    require(scene.errors().empty(), "unexpected scene errors in OnStart async pipeline");
    const auto* dirObj = scene.getComponent(directorId, "Phase27.Scene::LevelDirector");
    require(dirObj != nullptr, "missing LevelDirector component");

    runtime::RuntimeError err;
    const auto completedVal = runtime.getMember(*dirObj, "completed", err);
    require(completedVal && std::holds_alternative<bool>(*completedVal) && std::get<bool>(*completedVal),
        "LevelDirector async OnStart did not complete");

    const auto saveVal = runtime.getMember(*dirObj, "loadedSave", err);
    require(saveVal.has_value(), "missing loadedSave");
    std::string loadedSaveStr;
    if (const auto* s = std::get_if<std::string>(&*saveVal)) {
        loadedSaveStr = *s;
    } else if (const auto* ref = std::get_if<runtime::ObjectRef>(&*saveVal)) {
        loadedSaveStr = std::string(*runtime.heap()->stringView(*ref));
    }
    require(loadedSaveStr == "chapter=3;coins=450",
        "unexpected loadedSave: " + loadedSaveStr);

    const auto dmgVal = runtime.getMember(*dirObj, "loadedDamage", err);
    require(dmgVal && std::get<std::int64_t>(*dmgVal) == 190,
        "OnValidate or GetLoadedAssetInt mismatch");

    const auto wnameVal = runtime.getMember(*dirObj, "loadedWeaponName", err);
    std::string wname;
    if (const auto* s = std::get_if<std::string>(&*wnameVal)) wname = *s;
    else if (const auto* r = std::get_if<runtime::ObjectRef>(&*wnameVal)) wname = std::string(*runtime.heap()->stringView(*r));
    require(wname == "Sunblade Mk.II", "unexpected loadedWeaponName: " + wname);

    const auto rootIdVal = runtime.getMember(*dirObj, "spawnedSquadRoot", err);
    require(rootIdVal && std::holds_alternative<runtime::LongValue>(*rootIdVal), "missing spawnedSquadRoot");
    const auto squadRoot = static_cast<game::SceneEntityId>(std::get<runtime::LongValue>(*rootIdVal).value);
    require(squadRoot != 0 && scene.isEntityAlive(squadRoot), "spawned squad root is not alive");
    require(scene.findEntityByName("SquadRoot") == squadRoot,
        "instantiatePrefab must register root entity name");
    require(scene.getChildren(squadRoot).size() == 1, "expected 1 child under SquadRoot");
    const auto wingmanId = scene.getChildren(squadRoot)[0];
    require(scene.findEntityByName("SquadWingman") == wingmanId,
        "instantiatePrefab must register child entity name");

    const auto* rootTurret = scene.getComponent(squadRoot, "Phase27.Scene::TurretUnit");
    require(rootTurret != nullptr, "missing TurretUnit on SquadRoot");
    require(scene.isComponentStarted(squadRoot, "Phase27.Scene::TurretUnit"),
        "streamed prefab component must transition to started in Play mode");
    const auto startCallsVal = runtime.getMember(*rootTurret, "startCalls", err);
    const auto updateTicksVal = runtime.getMember(*rootTurret, "updateTicks", err);
    require(startCallsVal && std::get<std::int64_t>(*startCallsVal) == 1,
        "streamed prefab component must invoke OnStart exactly once");
    require(updateTicksVal && std::get<std::int64_t>(*updateTicksVal) >= 1,
        "streamed prefab component must tick OnUpdate in Play mode");

    const auto checkVal = runtime.getMember(*dirObj, "squadChildrenCheck", err);
    require(checkVal && std::get<std::int64_t>(*checkVal) == 1, "squad component check failed");

    const auto coroStats = scene.asyncRuntime()->coroutines().statistics();
    const auto awaitStats = scene.asyncRuntime()->awaitables().statistics();
    require(coroStats.activeCount == 0 && coroStats.activeGcRoots == 0,
        "coroutine pool leaked slots or GC roots");
    require(awaitStats.activeCount == 0 && awaitStats.activeGcRoots == 0,
        "awaitable pool leaked slots or GC roots");
}

// ============================================================================
// 2. Entity-Destroy-Bound `CancellationToken` O(1) Cancellation, Zero Zombie
//    Prefab Spawning, `try/catch/finally` Cleanup & Non-Gen Epoch ABA Safety
// ============================================================================
void testEntityDestroyTokenCancelsInFlightOperationsCleanly() {
    game::GameApi api;
    game::ScriptRuntime runtime(nullptr, api.bindings(), api.heap(), api.nativeHandles());
    game::SceneScriptRuntime scene(runtime);
    require(game::installSceneComponentBindings(api, scene), "installSceneComponentBindings failed");

    const std::string scriptSource = R"RS(
module Phase27.Cancel;
import RealScript.Async;
import RealScript.Scene;

public class AuditState {
    public static int catchCanceledCount;
    public static int finallyCleanupCount;
    public static int zombieSpawnCount;
    public static int aliveInsideFinallyCount;
}

public class StreamLoaderComponent {
    public long selfEntity;

    public async Awaitable OnStart() {
        CancellationToken ct = EntityDestroyToken(selfEntity);
        try {
            long spawned = await LoadPrefabSceneAsync("HeavyArena", selfEntity, ct);
            if (spawned > 0L) {
                AuditState.zombieSpawnCount = AuditState.zombieSpawnCount + 1;
            }
        } catch (AsyncCanceledException ex) {
            AuditState.catchCanceledCount = AuditState.catchCanceledCount + 1;
        } finally {
            AuditState.finallyCleanupCount = AuditState.finallyCleanupCount + 1;
            if (IsEntityAlive(selfEntity)) {
                AuditState.aliveInsideFinallyCount = AuditState.aliveInsideFinallyCount + 1;
            }
        }
    }
}

public class SaveWriterComponent {
    public long selfEntity;

    public async Awaitable OnStart() {
        CancellationToken ct = EntityDestroyToken(selfEntity);
        try {
            bool ok = await SaveGameAsync("aborted_slot", "partial_data", ct);
            if (ok) {
                AuditState.zombieSpawnCount = AuditState.zombieSpawnCount + 10;
            }
        } catch (AsyncCanceledException ex) {
            AuditState.catchCanceledCount = AuditState.catchCanceledCount + 1;
        } finally {
            AuditState.finallyCleanupCount = AuditState.finallyCleanupCount + 1;
        }
    }
}
)RS";

    game::GameScriptCompiler compiler(api);
    const auto compiled = compiler.compile({{"phase27_cancel.rscript", scriptSource}});
    require(compiled.succeeded(), "compile failed:\n" + diagnosticsText(compiled.diagnostics));
    runtime.replaceProgram(compiled.program);

    game::PrefabNodeTemplate arenaPrefab;
    arenaPrefab.name = "ZombieArenaEntity";
    scene.registerPrefabTemplate("HeavyArena", arenaPrefab);

    // Simulate 3 frames of async I/O latency
    scene.setAsyncIoLatencyFrames(3);

    // Entity A: generational entity destroyed via QueueDestroyEntity mid-flight
    const game::SceneEntityId genEntity = scene.spawnGenerationalEntity("LoaderGen");
    {
        game::SceneScriptOptions opts;
        opts.initialMembers["selfEntity"] = runtime::LongValue{static_cast<std::int64_t>(genEntity)};
        require(scene.attachComponent(genEntity, "Phase27.Cancel::StreamLoaderComponent", opts),
            "attach StreamLoaderComponent failed");
    }

    // Entity B: plain non-generational entity ID 42 destroyed via direct C++ `scene.detach(42)`
    const game::SceneEntityId rawEntity = 42;
    {
        game::SceneScriptOptions opts;
        opts.initialMembers["selfEntity"] = runtime::LongValue{static_cast<std::int64_t>(rawEntity)};
        require(scene.attachComponent(rawEntity, "Phase27.Cancel::SaveWriterComponent", opts),
            "attach SaveWriterComponent failed");
    }
    const auto oldRawToken = scene.entityDestroyToken(rawEntity);
    require(!scene.asyncRuntime()->cancellations().isCancellationRequested(oldRawToken),
        "oldRawToken should not be canceled while entity 42 is alive");

    scene.start();
    scene.update(0.016); // Frame 1: in flight (2 latency frames remaining)
    require(scene.asyncRuntime()->coroutines().statistics().activeCount == 2,
        "expected 2 suspended coroutines in flight");

    // Destroy both entities mid-flight!
    scene.queueDestroyEntity(genEntity);
    require(scene.detach(rawEntity), "direct detach(42) failed");
    require(scene.asyncRuntime()->cancellations().isCancellationRequested(oldRawToken),
        "oldRawToken must be canceled immediately after detach(42)");

    // Re-use raw entity ID 42 immediately to verify ABA epoch isolation!
    {
        game::SceneScriptOptions opts;
        opts.initialMembers["selfEntity"] = runtime::LongValue{static_cast<std::int64_t>(rawEntity)};
        // Re-attach inert node name so entity 42 is alive again
        scene.setEntityName(rawEntity, "ReusedEntity42");
    }
    const auto newRawToken = scene.entityDestroyToken(rawEntity);
    require(newRawToken != oldRawToken, "reused raw entity ID 42 must get a distinct epoch token");
    require(!scene.asyncRuntime()->cancellations().isCancellationRequested(newRawToken),
        "newRawToken on reused entity 42 must not be pre-canceled");
    require(scene.asyncRuntime()->cancellations().isCancellationRequested(oldRawToken),
        "oldRawToken must stay canceled after entity 42 reuse");

    // Tick 5 more frames past the 3-frame I/O latency window
    for (int f = 0; f < 5; ++f) {
        scene.update(0.016);
    }

    runtime::RuntimeError err;
    const auto catches = runtime.getStaticMember("Phase27.Cancel::AuditState", "catchCanceledCount", err);
    const auto finallies = runtime.getStaticMember("Phase27.Cancel::AuditState", "finallyCleanupCount", err);
    const auto zombies = runtime.getStaticMember("Phase27.Cancel::AuditState", "zombieSpawnCount", err);
    const auto aliveInFinally = runtime.getStaticMember("Phase27.Cancel::AuditState", "aliveInsideFinallyCount", err);

    require(catches && std::get<std::int64_t>(*catches) == 2,
        "expected both destroyed entities to catch AsyncCanceledException");
    require(finallies && std::get<std::int64_t>(*finallies) == 2,
        "expected both destroyed entities to run finally blocks");
    require(zombies && std::get<std::int64_t>(*zombies) == 0,
        "canceled prefab/save operations must never materialize zombie entities or writes");
    require(aliveInFinally && std::get<std::int64_t>(*aliveInFinally) == 0,
        "entity must already be marked dead when cancellation finally runs");
    require(scene.findEntityByName("ZombieArenaEntity") == 0,
        "ZombieArenaEntity must not exist in scene");
    require(!scene.getSaveGameSlot("aborted_slot").has_value(),
        "aborted_slot must not have been committed");

    require(scene.asyncRuntime()->coroutines().statistics().activeCount == 0,
        "active coroutines leaked after entity destruction");
    require(scene.asyncRuntime()->awaitables().statistics().activeCount == 0,
        "active awaitables leaked after entity destruction");
    require(scene.asyncRuntime()->awaitables().statistics().activeGcRoots == 0,
        "awaitable GC roots leaked after entity destruction");
}

// ============================================================================
// 3. Unhandled Fault in Fire-and-Forget `async Awaitable OnStart()` Surface in
//    `SceneScriptRuntime::errors()` Without Slot Leaks
// ============================================================================
void testUnhandledAsyncLifecycleFaultRecordedInSceneErrors() {
    game::GameApi api;
    game::ScriptRuntime runtime(nullptr, api.bindings(), api.heap(), api.nativeHandles());
    game::SceneScriptRuntime scene(runtime);
    require(game::installSceneComponentBindings(api, scene), "installSceneComponentBindings failed");

    const std::string scriptSource = R"RS(
module Phase27.Fault;
import RealScript.Async;
import RealScript.Scene;

public class FaultingLoader {
    public long selfEntity;

    public async Awaitable OnStart() {
        await Awaitable.Yield();
        // Missing save slot triggers async fault after suspension without try/catch
        string data = await LoadGameAsync("missing_profile_slot", EntityDestroyToken(selfEntity));
        if (data == "") {
            return;
        }
    }
}
)RS";

    game::GameScriptCompiler compiler(api);
    const auto compiled = compiler.compile({{"phase27_fault.rscript", scriptSource}});
    require(compiled.succeeded(), "compile failed:\n" + diagnosticsText(compiled.diagnostics));
    runtime.replaceProgram(compiled.program);

    const auto ent = scene.spawnGenerationalEntity("FaultEntity");
    game::SceneScriptOptions opts;
    opts.initialMembers["selfEntity"] = runtime::LongValue{static_cast<std::int64_t>(ent)};
    require(scene.attachComponent(ent, "Phase27.Fault::FaultingLoader", opts),
        "attach FaultingLoader failed");

    scene.start();
    require(scene.errors().empty(), "fault should occur asynchronously after Yield, not synchronously");

    scene.update(0.016); // Resolves Yield(), initiates LoadGameAsync("missing_profile_slot")
    scene.update(0.016); // PreTickIo drains failure -> Update resumes coroutine -> unhandled fault recorded!

    require(scene.errors().size() == 1,
        "expected exactly 1 recorded async lifecycle error, got " + std::to_string(scene.errors().size()));
    require(scene.errors()[0].entity == ent, "error entity mismatch");
    require(scene.errors()[0].callback == "OnStart", "error callback mismatch");
    require(scene.errors()[0].error.message.find("missing_profile_slot") != std::string::npos,
        "unexpected error message: " + scene.errors()[0].error.message);

    require(scene.asyncRuntime()->coroutines().statistics().activeCount == 0,
        "faulted lifecycle coroutine leaked frame");
    require(scene.asyncRuntime()->awaitables().statistics().activeCount == 0,
        "faulted lifecycle coroutine leaked awaitable slot");
}

// ============================================================================
// 4. Multi-Threaded Background Workers + `DeterminismSession` Record & Replay
// ============================================================================
struct ReplayRunOutcome {
    int completedTick = -1;
    std::string loadedSave;
    std::int64_t weaponPower = 0;
    game::SceneEntityId spawnedBossId = 0;
};

void testBackgroundWorkersAndDeterministicRecordReplay() {
    const std::string scriptSource = R"RS(
module Phase27.Replay;
import RealScript.Async;
import RealScript.Scene;

public class ReplayAsset {
    public int power;
}

public class ReplayBoss {
    public int tier;
}

public class ReplayController {
    public long selfEntity;
    public int frameCounter;
    public int completedFrame;
    public string saveText;
    public int weaponPower;
    public long spawnedBoss;

    public async Awaitable OnStart() {
        CancellationToken ct = EntityDestroyToken(selfEntity);
        saveText = await LoadGameAsync("net_save_01", ct);
        bool ok = await LoadAssetAsync("bundles/boss_sword", "Phase27.Replay::ReplayAsset", ct);
        if (ok) {
            weaponPower = GetLoadedAssetInt("bundles/boss_sword", "power");
        }
        spawnedBoss = await LoadPrefabSceneAsync("BossPrefab", selfEntity, ct);
        completedFrame = frameCounter;
    }

    public void OnUpdate(double dt) {
        frameCounter = frameCounter + 1;
    }
}
)RS";

    auto replayLog = std::make_shared<runtime::ReplayLog>();

    const auto runSimulation = [&](bool recordMode) -> ReplayRunOutcome {
        game::GameApi api;
        game::ScriptRuntime runtime(nullptr, api.bindings(), api.heap(), api.nativeHandles());
        game::SceneScriptRuntime scene(runtime);
        require(game::installSceneComponentBindings(api, scene), "installSceneComponentBindings failed");

        game::GameScriptCompiler compiler(api);
        const auto compiled = compiler.compile({{"phase27_replay.rscript", scriptSource}});
        require(compiled.succeeded(), "compile failed:\n" + diagnosticsText(compiled.diagnostics));
        runtime.replaceProgram(compiled.program);

        scene.registerAsyncAssetDefinition(
            "bundles/boss_sword",
            {{"power", std::int64_t{320}}});
        game::PrefabNodeTemplate bossTpl;
        bossTpl.name = "BossRoot";
        game::PrefabComponentTemplate bossComp;
        bossComp.scriptType = "Phase27.Replay::ReplayBoss";
        bossComp.options.initialMembers["tier"] = std::int64_t{5};
        bossTpl.components.push_back(bossComp);
        scene.registerPrefabTemplate("BossPrefab", bossTpl);

        runtime::DeterminismOptions detOpts;
        detOpts.mode = recordMode
            ? runtime::DeterminismMode::Record
            : runtime::DeterminismMode::Replay;
        detOpts.replayLog = replayLog;
        runtime::DeterminismSession detSession(detOpts);
        scene.setDeterminismSession(&detSession);

        std::vector<std::thread> bgThreads;
        if (recordMode) {
            scene.setSaveReadWorker([&bgThreads](
                const std::string& slotKey,
                runtime::AsyncRequestId reqId,
                runtime::CancellationToken,
                runtime::AsyncCompletionQueue& cq) {
                bgThreads.emplace_back([slotKey, reqId, &cq]() {
                    std::this_thread::sleep_for(std::chrono::milliseconds(3));
                    cq.submitString(reqId, "save_from_worker:" + slotKey);
                });
            });
            scene.setAssetLoadWorker([&bgThreads](
                const std::string&,
                const std::string&,
                runtime::AsyncRequestId reqId,
                runtime::CancellationToken,
                runtime::AsyncCompletionQueue& cq) {
                bgThreads.emplace_back([reqId, &cq]() {
                    std::this_thread::sleep_for(std::chrono::milliseconds(3));
                    cq.submitScalar(reqId, runtime::Value(true));
                });
            });
            scene.setPrefabStreamWorker([&bgThreads](
                const std::string&,
                runtime::AsyncRequestId reqId,
                runtime::CancellationToken,
                runtime::AsyncCompletionQueue& cq) {
                bgThreads.emplace_back([reqId, &cq]() {
                    std::this_thread::sleep_for(std::chrono::milliseconds(3));
                    cq.submitScalar(reqId, runtime::Value(true));
                });
            });
        } else {
            // In Replay mode, worker callbacks MUST NOT even be invoked!
            scene.setSaveReadWorker([](const auto&, auto, auto, auto&) {
                throw std::runtime_error("SaveReadWorker must not run during Replay!");
            });
            scene.setAssetLoadWorker([](const auto&, const auto&, auto, auto, auto&) {
                throw std::runtime_error("AssetLoadWorker must not run during Replay!");
            });
            scene.setPrefabStreamWorker([](const auto&, auto, auto, auto&) {
                throw std::runtime_error("PrefabStreamWorker must not run during Replay!");
            });
        }

        const auto ctrlId = scene.spawnGenerationalEntity("ReplayCtrl");
        game::SceneScriptOptions opts;
        opts.initialMembers["selfEntity"] = runtime::LongValue{static_cast<std::int64_t>(ctrlId)};
        require(scene.attachComponent(ctrlId, "Phase27.Replay::ReplayController", opts),
            "attach ReplayController failed");

        scene.start();
        for (int tick = 0; tick < 12; ++tick) {
            if (recordMode) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            scene.update(0.016);
        }

        for (auto& t : bgThreads) {
            if (t.joinable()) t.join();
        }

        if (!scene.errors().empty()) { throw std::runtime_error(std::string(recordMode ? "[Record] " : "[Replay] ") + scene.errors()[0].callback + ": " + scene.errors()[0].error.message); }
        const auto* ctrlObj = scene.getComponent(ctrlId, "Phase27.Replay::ReplayController");
        require(ctrlObj != nullptr, "missing ReplayController");

        runtime::RuntimeError err;
        ReplayRunOutcome out;
        out.completedTick = static_cast<int>(
            std::get<std::int64_t>(*runtime.getMember(*ctrlObj, "completedFrame", err)));
        const auto stVal = *runtime.getMember(*ctrlObj, "saveText", err);
        if (const auto* s = std::get_if<std::string>(&stVal)) out.loadedSave = *s;
        else if (const auto* r = std::get_if<runtime::ObjectRef>(&stVal)) {
            out.loadedSave = std::string(*runtime.heap()->stringView(*r));
        }
        out.weaponPower = std::get<std::int64_t>(*runtime.getMember(*ctrlObj, "weaponPower", err));
        out.spawnedBossId = static_cast<game::SceneEntityId>(
            std::get<runtime::LongValue>(*runtime.getMember(*ctrlObj, "spawnedBoss", err)).value);
        return out;
    };

    const ReplayRunOutcome recorded = runSimulation(true);
    require(recorded.completedTick > 0, "record run did not complete");
    require(recorded.loadedSave == "save_from_worker:net_save_01", "recorded save mismatch");
    require(recorded.weaponPower == 320, "recorded weaponPower mismatch");
    require(recorded.spawnedBossId != 0, "recorded spawnedBossId mismatch");

    const ReplayRunOutcome replayed = runSimulation(false);
    require(replayed.completedTick == recorded.completedTick,
        "Replay completedTick diverged from Record (" +
        std::to_string(replayed.completedTick) + " vs " + std::to_string(recorded.completedTick) + ")");
    require(replayed.loadedSave == recorded.loadedSave, "Replay saveText diverged");
    require(replayed.weaponPower == recorded.weaponPower, "Replay weaponPower diverged");
    require(replayed.spawnedBossId == recorded.spawnedBossId, "Replay spawnedBossId diverged");
}

// ============================================================================
// 5. Converged `$sequence_state_*` Pre-Indexed Fast-Path Verification &
//    50,000-Cycle Detach/Disable Microbenchmark (Old Reflection vs Stage 3)
// ============================================================================
void testLegacySequenceConvergenceAndBenchmark() {
    game::GameApi api;
    auto host = std::make_shared<game::GameplayHost>(60, 11, 17);
    require(game::installGameplayBindings(api, host), "installGameplayBindings failed");

    game::ScriptRuntime runtime(nullptr, api.bindings(), api.heap(), api.nativeHandles());
    game::SceneScriptRuntime scene(runtime);
    require(game::installSceneComponentBindings(api, scene), "installSceneComponentBindings failed");

    const std::string scriptSource = R"RS(
module Phase27.Bench;
import RealScript.Async;
import RealScript.Game;
import RealScript.Scene;

public class LegacySeqActor {
    public int hp;
    public int a;
    public int b;
    public int c;
    public int d;
    public int stepOut;

    public sequence Patrol(long target) {
        stepOut = 1;
        yield wait_ticks(5);
        stepOut = 2;
    }

    public sequence Guard(long target) {
        yield wait_ticks(5);
    }
}

public class ModernAsyncActor {
    public int hp;
    public int a;
    public int b;
    public int c;
    public int d;
    public long selfEntity;

    public async Awaitable RunAsync() {
        await Awaitable.YieldWithToken(EntityDestroyToken(selfEntity));
    }
}
)RS";

    game::GameScriptCompiler compiler(api);
    const auto compiled = compiler.compile({{"phase27_bench.rscript", scriptSource}});
    require(compiled.succeeded(), "compile failed:\n" + diagnosticsText(compiled.diagnostics));
    runtime.replaceProgram(compiled.program);

    // 5a. Idle legacy sequence component: detach must skip VM invoke completely!
    require(scene.attachComponent(1, "Phase27.Bench::LegacySeqActor"), "attach LegacySeqActor 1 failed");
    require(scene.detach(1), "detach LegacySeqActor 1 failed");
    require(scene.legacySequenceCancelChecks() == 1, "expected 1 fast sequence cancel check");
    require(scene.legacySequenceCancelInvocations() == 0,
        "idle legacy sequence should skip VM invoke completely (0 invocations)");

    // 5b. Active legacy sequence component: detach must invoke pre-bound CancelPatrol!
    require(scene.attachComponent(2, "Phase27.Bench::LegacySeqActor"), "attach LegacySeqActor 2 failed");
    const auto startRes = scene.invoke(2, "Patrol", {runtime::LongValue{2}});
    require(startRes.succeeded, "starting legacy Patrol sequence failed");
    require(scene.detach(2), "detach LegacySeqActor 2 failed");
    require(scene.legacySequenceCancelInvocations() >= 1,
        "active legacy sequence must invoke pre-bound CancelPatrol on detach");

    // 5c. Microbenchmark: 50,000 component disable/enable or cancel checks
    // Compare:
    //  (A) Old reflection cancellation logic (`rfind("$sequence_state_")` + `"Cancel"+name` + `findMethod` + `invoke`)
    //  (B) Stage 3 converged `SequenceCancelPlan` on idle `LegacySeqActor`
    //  (C) Stage 3 modern `ModernAsyncActor` (`sequenceCancelPlan == nullptr` O(1) no-op)
    //  (D) Stage 3 `EntityDestroyToken` create + `detach` O(1) async cancellation cycle
    constexpr int kIterations = 50000;

    require(scene.attachComponent(10, "Phase27.Bench::LegacySeqActor"), "attach bench legacy failed");
    const auto* legacyObj = scene.getComponent(10, "Phase27.Bench::LegacySeqActor");
    require(legacyObj != nullptr, "missing bench legacyObj");

    // (A) Old reflection scan + unconditional VM invoke
    const auto tOld0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        static constexpr const char* kPrefix = "$sequence_state_";
        const std::size_t prefixLen = std::strlen(kPrefix);
        for (const auto& field : legacyObj->type().descriptor.fields) {
            if (!field.synthetic) continue;
            if (field.name.size() > prefixLen && field.name.rfind(kPrefix, 0) == 0) {
                const std::string seqName = field.name.substr(prefixLen);
                if (const auto cancelMethod = runtime.findMethod(
                        legacyObj->type(), "Cancel" + seqName, 0)) {
                    (void)runtime.invoke(*legacyObj, *cancelMethod, {});
                }
            }
        }
    }
    const auto tOld1 = std::chrono::steady_clock::now();
    const double oldNsPerOp =
        std::chrono::duration<double, std::nano>(tOld1 - tOld0).count() / kIterations;

    // (B) Stage 3 converged pre-indexed `SequenceCancelPlan` via `setComponentEnabled(false/true)`
    const auto tConv0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        (void)scene.setComponentEnabled(10, "Phase27.Bench::LegacySeqActor", false);
        (void)scene.setComponentEnabled(10, "Phase27.Bench::LegacySeqActor", true);
    }
    const auto tConv1 = std::chrono::steady_clock::now();
    const double convNsPerOp =
        std::chrono::duration<double, std::nano>(tConv1 - tConv0).count() / kIterations;

    // (C) Stage 3 modern `ModernAsyncActor` (`sequenceCancelPlan == nullptr`) disable/enable
    require(scene.attachComponent(11, "Phase27.Bench::ModernAsyncActor"), "attach bench modern failed");
    const auto tMod0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        (void)scene.setComponentEnabled(11, "Phase27.Bench::ModernAsyncActor", false);
        (void)scene.setComponentEnabled(11, "Phase27.Bench::ModernAsyncActor", true);
    }
    const auto tMod1 = std::chrono::steady_clock::now();
    const double modNsPerOp =
        std::chrono::duration<double, std::nano>(tMod1 - tMod0).count() / kIterations;

    // (D) Generational entity spawn + `entityDestroyToken` + `cancelEntityDestroyToken` + `detach`
    const auto tTok0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) {
        const auto eid = scene.spawnGenerationalEntity();
        const auto tok = scene.entityDestroyToken(eid);
        if (!tok.canBeCanceled()) throw std::runtime_error("unexpected uncancellable token");
        (void)scene.detach(eid);
    }
    const auto tTok1 = std::chrono::steady_clock::now();
    const double tokNsPerOp =
        std::chrono::duration<double, std::nano>(tTok1 - tTok0).count() / kIterations;

    std::cout << std::fixed << std::setprecision(1)
              << "[bench] Stage 3 Cancellation Convergence (" << kIterations << " iterations):\n"
              << "  Old $sequence_state_* reflection + VM invoke : " << oldNsPerOp << " ns/op\n"
              << "  Stage 3 SequenceCancelPlan (idle sequence)   : " << convNsPerOp << " ns/op\n"
              << "  Stage 3 ModernAsyncActor (null plan fastpath): " << modNsPerOp << " ns/op\n"
              << "  Stage 3 Generational EntityDestroyToken cycle: " << tokNsPerOp << " ns/op\n";
}

} // namespace


// ============================================================================
// 6. FixedTick / EndOfFrame Phase Queues, GameplayHost::destroyEntity Token
//    Cancellation, Strict Determinism Guard & Hot-Reload Plan Reset
// ============================================================================
void testPhaseQueuesGameplayDestroyAndHotReload() {
    game::GameApi api;
    auto host = std::make_shared<game::GameplayHost>(60, 19, 23);
    require(game::installGameplayBindings(api, host), "installGameplayBindings failed");

    game::ScriptRuntime runtime(nullptr, api.bindings(), api.heap(), api.nativeHandles());
    game::SceneScriptRuntime scene(runtime);
    require(game::installSceneComponentBindings(api, scene), "installSceneComponentBindings failed");
    game::SceneGameplayDriver driver(scene, host);

    const std::string srcV1 = R"RS(
module Phase27.Phases;
import RealScript.Async;
import RealScript.Game;
import RealScript.Scene;

public class PhaseActor {
    public int value;
    public sequence IdleSeq(long target) {
        yield wait_ticks(2);
    }
    public void OnLateUpdate(double dt) {
        value = value + 1;
    }
}
)RS";

    const std::string srcV2 = R"RS(
module Phase27.Phases;
import RealScript.Async;
import RealScript.Game;
import RealScript.Scene;

public class PhaseActor {
    public int value;
    public void OnLateUpdate(double dt) {
        value = value + 10;
    }
}
)RS";

    game::GameScriptCompiler compiler(api);
    const auto compiledV1 = compiler.compile({{"phase27_phases_v1.rscript", srcV1}});
    require(compiledV1.succeeded(), "compile v1 failed");
    runtime.replaceProgram(compiledV1.program);

    // Verify GameplayHost::entityDestroyToken + GameplayHost::destroyEntity O(1) cancel
    const auto gpEntity = host->core().entities().create();
    const auto gpToken = host->entityDestroyToken(gpEntity);
    require(gpToken.canBeCanceled() &&
            !scene.asyncRuntime()->cancellations().isCancellationRequested(gpToken),
        "gpToken should start active");
    require(host->destroyEntity(gpEntity), "GameplayHost::destroyEntity failed");
    require(scene.asyncRuntime()->cancellations().isCancellationRequested(gpToken),
        "GameplayHost::destroyEntity must cancel bound EntityDestroyToken");

    // Verify FixedTick and EndOfFrame phase continuations drain in fixedUpdate / lateUpdate
    auto& pool = scene.asyncRuntime()->awaitables();
    int fixedRan = 0;
    int eofRan = 0;
    const auto fixedHandle = pool.allocatePending(runtime::ResumeQueue::FixedTick);
    {
        runtime::AsyncContinuation cont{};
        cont.resumeQueue = runtime::ResumeQueue::FixedTick;
        cont.context = &fixedRan;
        cont.callback = [](void* ctx, runtime::AwaitableHandle h, runtime::AwaitStatus, std::uint64_t) {
            *static_cast<int*>(ctx) += 1;
            (void)h;
        };
        require(pool.onCompleted(fixedHandle, cont), "onCompleted FixedTick failed");
        require(pool.trySetResult(fixedHandle, runtime::Value(1)), "trySetResult FixedTick failed");
        (void)pool.discard(fixedHandle);
    }
    const auto eofHandle = pool.allocatePending(runtime::ResumeQueue::EndOfFrame);
    {
        runtime::AsyncContinuation cont{};
        cont.resumeQueue = runtime::ResumeQueue::EndOfFrame;
        cont.context = &eofRan;
        cont.callback = [](void* ctx, runtime::AwaitableHandle h, runtime::AwaitStatus, std::uint64_t) {
            *static_cast<int*>(ctx) += 1;
            (void)h;
        };
        require(pool.onCompleted(eofHandle, cont), "onCompleted EndOfFrame failed");
        require(pool.trySetResult(eofHandle, runtime::Value(1)), "trySetResult EndOfFrame failed");
        (void)pool.discard(eofHandle);
    }

    const auto actorId = scene.spawnGenerationalEntity("PhaseActor");
    require(scene.attachComponent(actorId, "Phase27.Phases::PhaseActor"), "attach PhaseActor failed");
    scene.start();
    (void)driver.step(); // steps GameplayHost + calls scene.fixedUpdate(...) -> drains FixedTick!
    require(fixedRan == 1 && eofRan == 0, "FixedTick continuation should drain in fixedUpdate only");

    scene.lateUpdate(0.016); // runs OnLateUpdate + drains EndOfFrame!
    require(eofRan == 1, "EndOfFrame continuation should drain in lateUpdate");

    // Hot-reload from V1 (had legacy sequence plan) to V2 (no legacy sequence plan)
    const auto compiledV2 = compiler.compile({{"phase27_phases_v2.rscript", srcV2}});
    require(compiledV2.succeeded(), "compile v2 failed");
    const auto reloadReport = scene.hotReload(compiledV2.program);
    require(reloadReport.succeeded, "hotReload failed");
    scene.lateUpdate(0.016);
    const auto* actorObj = scene.getComponent(actorId, "Phase27.Phases::PhaseActor");
    runtime::RuntimeError err;
    const auto v = runtime.getMember(*actorObj, "value", err);
    require(v && std::get<std::int64_t>(*v) == 11, "hot-reloaded value mismatch");
    require(scene.detach(actorId), "detach after hotReload failed");

    // Verify Strict determinism mode rejects Recordable async SaveGame I/O cleanly
    runtime::DeterminismOptions strictOpts;
    strictOpts.mode = runtime::DeterminismMode::Strict;
    runtime::DeterminismSession strictSession(strictOpts);
    scene.setDeterminismSession(&strictSession);
    const auto strictSave = scene.saveGameAsync("strict_slot", "x");
    require(!scene.stepAsyncPreTickIo(), "Strict mode must reject non-deterministic async I/O");
    require(!scene.errors().empty() &&
            scene.errors().back().error.code == runtime::ErrorCode::DeterminismViolation,
        "expected DeterminismViolation in scene.errors()");
    (void)pool.discard(strictSave);
}

// ============================================================================
// 7. Stage 3 Review Regressions:
//    - Explicit Entity ID non-collision & rollback safety in LoadPrefabSceneAsync
//    - Struct typeId validation in lifecycle/events & BindingMarshal + TypedAwaitable<Vec3>
//    - GameplayHost vs SceneScriptRuntime EntityDestroyDomain keyspace isolation
//    - SceneScriptRuntime destruction cleanup on outliving AsyncRuntimeContext
//    - Hot-reload clean abort of suspended async coroutines
// ============================================================================
void testStage3ReviewRegressions() {
    // 7A: Explicit entity ID collision & rollback safety + struct return typeId validation
    //     + TypedAwaitable<Vec3> marshaling + async event/signal fault tracking
    {
        game::GameApi api;
        game::ScriptRuntime runtime(nullptr, api.bindings(), api.heap(), api.nativeHandles());
        game::SceneScriptRuntime scene(runtime);
        require(game::installSceneComponentBindings(api, scene), "installSceneComponentBindings failed");

        api.function(
            "Phase27.Bridge",
            "ComputeWaypointAsync",
            [&scene](game::Vec3 origin) -> game::TypedAwaitable<game::Vec3> {
                const game::Vec3 shifted{
                    origin.x + game::Fixed::fromInt(10),
                    origin.y + game::Fixed::fromInt(20),
                    origin.z + game::Fixed::fromInt(30)};
                const auto val = runtime::Value{
                    runtime::Vec3Value{shifted.x.raw, shifted.y.raw, shifted.z.raw}};
                return game::TypedAwaitable<game::Vec3>{
                    scene.asyncRuntime()->awaitables().fromResult(val)};
            });
        api.function(
            "Phase27.Bridge",
            "ConsumeVoidAwaitable",
            [](runtime::AwaitableHandle h) -> bool {
                return h.valid();
            });
        api.function(
            "Phase27.Bridge",
            "ConsumeIntAwaitable",
            [](game::TypedAwaitable<int> h) -> bool {
                return h.handle.valid();
            });

        const std::string src = R"RS(
module Phase27.Regressions;
import RealScript.Async;
import RealScript.Math;
import RealScript.Scene;
import Phase27.Bridge;

public struct PackedPayload {
    public long bits;
}

public class MissingDep {}

[RequireComponent("Phase27.Regressions::MissingDep")]
public class BrokenPrefabUnit {
    public int x;
}

public class SafePrefabUnit {
    public int marker;
}

public class ExplicitRootActor {
    public Vec3 waypoint;
    public long spawnedChild;
    public bool failedPrefabCaught;

    public PackedPayload OnCreate() {
        PackedPayload p = new PackedPayload();
        p.bits = 4294967297L; // slot=1, version=1 -> must NOT be treated as AwaitableHandle!
        return p;
    }

    public CancellationToken OnEnable() {
        return EntityDestroyToken(1L);
    }

    public async Awaitable OnStart() {
        CancellationToken ct = EntityDestroyToken(1L);
        waypoint = await ComputeWaypointAsync(new Vec3(1, 2, 3));
        try {
            long bad = await LoadPrefabSceneAsync("BrokenPrefab", 1L, ct);
        } catch (AsyncException ex) {
            failedPrefabCaught = true;
        }
        spawnedChild = await LoadPrefabSceneAsync("SafePrefab", 1L, ct);
    }

    public async Awaitable OnBoomEvent() {
        await Awaitable.Yield();
        throw new AsyncException("async_event_boom");
    }
}
)RS";

        game::GameScriptCompiler compiler(api);
        const auto compiled = compiler.compile({{"phase27_reg.rscript", src}});
        require(compiled.succeeded(), "7A compile failed:\n" + diagnosticsText(compiled.diagnostics));
        runtime.replaceProgram(compiled.program);

        // Pre-allocate an unrelated pending Awaitable at slot 0 / 1 to verify OnCreate/OnEnable
        // returning PackedPayload / CancellationToken do NOT hijack or fault AwaitableCorePool!
        const auto sentinelPending = scene.asyncRuntime()->awaitables().allocatePending();

        game::PrefabNodeTemplate brokenPrefab;
        brokenPrefab.name = "BrokenNode";
        brokenPrefab.components.push_back({"Phase27.Regressions::BrokenPrefabUnit", {}});
        scene.registerPrefabTemplate("BrokenPrefab", brokenPrefab);

        game::PrefabNodeTemplate safePrefab;
        safePrefab.name = "SafeNode";
        safePrefab.components.push_back({"Phase27.Regressions::SafePrefabUnit", {}});
        scene.registerPrefabTemplate("SafePrefab", safePrefab);

        // Attach explicitly onto non-generational entity ID 1 (nextAutoEntityId_ is still 1!)
        scene.setEntityName(1, "ExplicitOne");
        require(scene.attachComponent(1, "Phase27.Regressions::ExplicitRootActor"),
            "7A attachComponent(1) failed");
        require(scene.errors().empty(),
            "7A OnCreate/OnEnable non-Awaitable struct returns must not record errors");
        require(scene.asyncRuntime()->awaitables().status(sentinelPending) == runtime::AwaitStatus::Pending,
            "7A non-Awaitable struct return must not touch sentinelPending");
        (void)scene.asyncRuntime()->awaitables().trySetCanceled(sentinelPending);
        (void)scene.asyncRuntime()->awaitables().discard(sentinelPending);

        // Verify BindingMarshal readValue<AwaitableHandle> & readValue<TypedAwaitable<T>>
        // reject non-matching StructValue typeIds
        {
            const runtime::Value bogusField{runtime::LongValue{static_cast<std::int64_t>((1ULL << 32u) | 1ULL)}};
            const runtime::Value bogusStruct{runtime::makeStructValueFromFields(
                semantic::stableTypeId("Phase27.Regressions::PackedPayload"),
                runtime::ValueSpan(&bogusField, 1))};
            runtime::RuntimeError marshalErr;
            bytecode::FunctionReference fnRef{};
            fnRef.name = "Phase27.Bridge::__rs_native_ConsumeVoidAwaitable";
            const auto callOut = api.bindings()->invoke(
                fnRef, runtime::ValueSpan(&bogusStruct, 1), marshalErr);
            require(!callOut.has_value() &&
                    marshalErr.code == runtime::ErrorCode::TypeMismatch,
                "BindingMarshal must reject non-Awaitable struct passed to AwaitableHandle parameter");

            const runtime::Value wrongGenericStruct{runtime::makeStructValueFromFields(
                semantic::stableTypeId("RealScript.Async::Awaitable__long"),
                runtime::ValueSpan(&bogusField, 1))};
            runtime::RuntimeError genericErr;
            bytecode::FunctionReference intFnRef{};
            intFnRef.name = "Phase27.Bridge::__rs_native_ConsumeIntAwaitable";
            const auto genericOut = api.bindings()->invoke(
                intFnRef, runtime::ValueSpan(&wrongGenericStruct, 1), genericErr);
            require(!genericOut.has_value() &&
                    genericErr.code == runtime::ErrorCode::TypeMismatch,
                "BindingMarshal must reject Awaitable<long> passed to TypedAwaitable<int> parameter");
        }

        scene.start();
        for (int i = 0; i < 4; ++i) {
            scene.update(0.016);
        }

        // Entity 1 MUST still be alive, named "ExplicitOne", and own ExplicitRootActor!
        require(scene.isEntityAlive(1) && scene.getEntityName(1) == "ExplicitOne",
            "7A BrokenPrefab rollback or SafePrefab allocation clobbered explicit entity 1");
        const auto* rootActor = scene.getComponent(1, "Phase27.Regressions::ExplicitRootActor");
        require(rootActor != nullptr, "7A ExplicitRootActor was erased from entity 1");

        runtime::RuntimeError err;
        const auto caughtVal = runtime.getMember(*rootActor, "failedPrefabCaught", err);
        require(caughtVal && std::get<bool>(*caughtVal), "7A BrokenPrefab fault was not caught");
        const auto childVal = runtime.getMember(*rootActor, "spawnedChild", err);
        const auto childId = static_cast<game::SceneEntityId>(std::get<runtime::LongValue>(*childVal).value);
        require(childId != 0 && childId != 1 && scene.getParent(childId) == 1,
            "7A SafePrefab must allocate distinct entity ID parented to entity 1");
        const auto wpVal = runtime.getMember(*rootActor, "waypoint", err);
        require(wpVal && std::holds_alternative<runtime::Vec3Value>(*wpVal), "7A missing waypoint Vec3");
        const auto wp = std::get<runtime::Vec3Value>(*wpVal);
        require(wp.x == game::Fixed::fromInt(11).raw &&
                wp.y == game::Fixed::fromInt(22).raw &&
                wp.z == game::Fixed::fromInt(33).raw,
            "7A TypedAwaitable<Vec3> result mismatch");

        scene.clearErrors();
        require(scene.dispatch({1, "OnBoomEvent", {}}), "7A async dispatch initial call failed");
        scene.update(0.016);
        require(scene.errors().size() == 1 &&
                scene.errors()[0].callback == "OnBoomEvent",
            "7A async dispatch unhandled fault must be recorded");
    }

    // 7B: GameplayHost vs SceneScriptRuntime EntityDestroyDomain isolation on {1, 1}
    {
        game::GameApi api;
        game::ScriptRuntime runtime(nullptr, api.bindings(), api.heap(), api.nativeHandles());
        game::SceneScriptRuntime scene(runtime);
        auto host = std::make_shared<game::GameplayHost>(60);
        game::SceneGameplayDriver driver(scene, host);
        (void)driver;

        const auto gp0 = host->core().entities().create(); // {0, 1}
        const auto gp1 = host->core().entities().create(); // {1, 1}
        (void)gp0;
        const auto sceneGen1 = scene.spawnGenerationalEntity("SceneGen1"); // slot=1, gen=1 -> {1, 1}
        require(gp1.index == 1 && gp1.generation == 1, "expected GameplayHost {1, 1}");
        require(game::entityIdSlot(sceneGen1) == 1 &&
                game::entityIdGeneration(sceneGen1) == 1,
            "expected SceneScriptRuntime {1, 1}");

        const auto gpTok = host->entityDestroyToken(gp1);
        const auto sceneTok = scene.entityDestroyToken(sceneGen1);
        require(gpTok != sceneTok, "GameplayHost and SceneScriptRuntime {1,1} tokens must not alias");

        require(host->destroyEntity(gp1), "destroy gp1 failed");
        require(scene.asyncRuntime()->cancellations().isCancellationRequested(gpTok),
            "gpTok must be canceled");
        require(!scene.asyncRuntime()->cancellations().isCancellationRequested(sceneTok),
            "destroying GameplayHost {1,1} must NOT cancel SceneScriptRuntime {1,1}");

        require(scene.detach(sceneGen1), "detach sceneGen1 failed");
        require(scene.asyncRuntime()->cancellations().isCancellationRequested(sceneTok),
            "detaching SceneScriptRuntime {1,1} must cancel sceneTok");
    }

    // 7C: Outliving ScriptRuntime/AsyncRuntimeContext safety when SceneScriptRuntime is destroyed
    {
        game::GameApi api;
        game::ScriptRuntime runtime(nullptr, api.bindings(), api.heap(), api.nativeHandles());
        runtime::AwaitableHandle orphanPrefabTask{};
        runtime::AwaitableHandle orphanSaveTask{};
        {
            game::SceneScriptRuntime transientScene(runtime);
            transientScene.setAsyncIoLatencyFrames(3);
            game::PrefabNodeTemplate tpl;
            tpl.name = "TransientNode";
            transientScene.registerPrefabTemplate("TransientPrefab", tpl);
            orphanPrefabTask = transientScene.loadPrefabSceneAsync("TransientPrefab");
            orphanSaveTask = transientScene.saveGameAsync("slot0", "payload");
            // Complete the underlying I/O packet into PreTickIo ready queue, then destroy transientScene
            // BEFORE draining continuations!
            transientScene.setAsyncIoLatencyFrames(0);
            const auto fastTask = transientScene.loadPrefabSceneAsync("TransientPrefab");
            (void)runtime.asyncRuntime()->awaitables().discard(fastTask);
            runtime::RuntimeError drainErr;
            (void)runtime.asyncRuntime()->completionQueue().drainToMainThread(
                runtime.asyncRuntime()->awaitables(),
                runtime.heap().get(),
                nullptr,
                drainErr);
            require(runtime.asyncRuntime()->awaitables().readyContinuationCount(
                        runtime::ResumeQueue::PreTickIo) == 1,
                "7C expected 1 armed continuation in PreTickIo before destruction");
        }
        // Draining PreTickIo & Update after transientScene is destroyed must NOT UAF!
        runtime::RuntimeError ioErr;
        require(runtime.asyncRuntime()->stepPreTickIo(nullptr, ioErr),
            "7C stepPreTickIo after SceneScriptRuntime destruction failed");
        (void)runtime.asyncRuntime()->stepResumeQueue(runtime::ResumeQueue::Update);
        require(runtime.asyncRuntime()->awaitables().status(orphanPrefabTask) ==
                runtime::AwaitStatus::Canceled,
            "7C orphan outerHandle should be canceled on SceneScriptRuntime destruction");
        require(runtime.asyncRuntime()->awaitables().status(orphanSaveTask) ==
                runtime::AwaitStatus::Canceled,
            "7C orphan saveGameAsync handle should be canceled on SceneScriptRuntime destruction");
        (void)runtime.asyncRuntime()->awaitables().discard(orphanPrefabTask);
        (void)runtime.asyncRuntime()->awaitables().discard(orphanSaveTask);
        require(runtime.asyncRuntime()->awaitables().statistics().activeCount == 0,
            "7C awaitable slots leaked after SceneScriptRuntime destruction");
    }

    // 7D: Hot-reload while an async coroutine is suspended aborts cleanly without UAF or leaks
    {
        game::GameApi api;
        game::ScriptRuntime runtime(nullptr, api.bindings(), api.heap(), api.nativeHandles());
        game::SceneScriptRuntime scene(runtime);
        require(game::installSceneComponentBindings(api, scene), "7D install bindings failed");

        const std::string hotV1 = R"RS(
module Phase27.HotAsync;
import RealScript.Async;

public class Worker {
    public int ticks;
    public async Awaitable OnStart() {
        await Awaitable.Yield();
        ticks = 999;
    }
}
)RS";
        const std::string hotV2 = R"RS(
module Phase27.HotAsync;
import RealScript.Async;

public class Worker {
    public int ticks;
    public void OnUpdate(double dt) {
        ticks = ticks + 5;
    }
}
)RS";
        game::GameScriptCompiler compiler(api);
        const auto c1 = compiler.compile({{"hot_v1.rscript", hotV1}});
        const auto c2 = compiler.compile({{"hot_v2.rscript", hotV2}});
        require(c1.succeeded() && c2.succeeded(), "7D compile failed");

        runtime.replaceProgram(c1.program);
        const auto ent = scene.spawnGenerationalEntity("HotWorker");
        require(scene.attachComponent(ent, "Phase27.HotAsync::Worker"), "7D attach failed");
        scene.start(); // Suspends Worker.OnStart() at Awaitable.Yield()
        require(scene.asyncRuntime()->coroutines().statistics().activeCount == 1,
            "7D expected 1 suspended coroutine before hotReload");

        const auto rep = scene.hotReload(c2.program);
        require(rep.succeeded, "7D hotReload failed");
        require(scene.asyncRuntime()->coroutines().statistics().activeCount == 0 &&
                scene.asyncRuntime()->coroutines().statistics().activeGcRoots == 0,
            "7D hotReload must abort suspended coroutines and release GC roots");

        scene.update(0.016);
        const auto* obj = scene.getComponent(ent, "Phase27.HotAsync::Worker");
        runtime::RuntimeError err;
        const auto ticksVal = runtime.getMember(*obj, "ticks", err);
        require(ticksVal && std::get<std::int64_t>(*ticksVal) == 5,
            "7D aborted v1 coroutine must not overwrite migrated v2 component state");
        require(scene.errors().empty(), "7D unexpected scene errors across hotReload");
    }
}

int main() {
    try {
        testAsyncLifecycleStreamingAssetAndSaveLoad();
        testEntityDestroyTokenCancelsInFlightOperationsCleanly();
        testUnhandledAsyncLifecycleFaultRecordedInSceneErrors();
        testBackgroundWorkersAndDeterministicRecordReplay();
        testLegacySequenceConvergenceAndBenchmark();
        testPhaseQueuesGameplayDestroyAndHotReload();
        testStage3ReviewRegressions();
        std::cout << "Phase 27 scene async lifecycle & bridge tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[FAIL] " << ex.what() << "\n";
        return 1;
    }
}
