#include <chrono>
#include <cctype>
#include <filesystem>
#include <stdexcept>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "realscript/compiler/Compilation.h"
#include "realscript/game/GameProductization.h"
#include "realscript/game/GameScripting.h"
#include "realscript/game/GameplayMetadata.h"
#include "realscript/game/GameplayScripting.h"
#include "realscript/runtime/AsyncRuntime.h"
#include "realscript/runtime/FiberScheduler.h"

using namespace realscript;
using namespace realscript::game;

#define RS_REQUIRE(expr)                                                        \
    do {                                                                        \
        if (!(expr)) {                                                          \
            throw std::runtime_error(                                           \
                std::string("RS_REQUIRE failed at ") + __FILE__ + ":" +         \
                std::to_string(__LINE__) + ": " #expr);                         \
        }                                                                       \
    } while (false)

namespace {

GameCompileResult compileSourcesOrDie(
    const GameApi& api,
    const std::vector<compiler::SourceFile>& sources,
    const char* label) {
    GameScriptCompiler compiler(api);
    auto result = compiler.compile(sources);
    if (!result.succeeded()) {
        std::cerr << "[FAIL] Compilation failed in " << label << ":\n";
        for (const auto& item : result.diagnostics.items()) {
            std::cerr << "  " << item.code << ": " << item.message << "\n";
        }
        std::abort();
    }
    return result;
}

runtime::CompiledFiberModule compileFiberSourceOrDie(
    const std::string& sourceText,
    const char* label) {
    compiler::Compilation comp;
    comp.addSource(compiler::SourceFile{"fiber_test.rscript", sourceText});
    auto fe = comp.build();
    if (fe.diagnostics.hasErrors() || fe.modules.empty()) {
        std::cerr << "[FAIL] Fiber MIR compilation failed in " << label << "\n";
        for (const auto& d : fe.diagnostics.items()) {
            std::cerr << "  " << d.code << ": " << d.message << "\n";
        }
        std::abort();
    }
    const mir::Module* targetMod = &fe.modules.back();
    for (const auto& m : fe.modules) {
        if (m.name == "AI.Bt") {
            targetMod = &m;
            break;
        }
    }
    return runtime::FiberCompiler::compileModule(*targetMod);
}

std::int64_t getIntMemberOrDie(
    const ScriptRuntime& rt,
    const ScriptObject& obj,
    const std::string& name) {
    runtime::RuntimeError err;
    const auto val = rt.getMember(obj, name, err);
    RS_REQUIRE(val.has_value());
    RS_REQUIRE(std::holds_alternative<std::int64_t>(*val));
    return std::get<std::int64_t>(*val);
}

std::string getStringMemberOrDie(
    const ScriptRuntime& rt,
    const ScriptObject& obj,
    const std::string& name) {
    runtime::RuntimeError err;
    const auto val = rt.getMember(obj, name, err);
    RS_REQUIRE(val.has_value());
    if (const auto* s = std::get_if<std::string>(&*val)) {
        return *s;
    }
    if (const auto* sv = std::get_if<runtime::StringValue>(&*val)) {
        return sv->str();
    }
    if (const auto* ref = std::get_if<runtime::ObjectRef>(&*val)) {
        const auto view = rt.heap()->stringView(*ref);
        RS_REQUIRE(view.has_value());
        return std::string(*view);
    }
    std::abort();
}

// ============================================================================
// 1. perf_4 Tests: Flattened Lifecycle Hot Path, Mutations & Reentrancy
// ============================================================================

void testPerf4SteadyStateFlattenedHotPath() {
    std::cout << "[RUN] testPerf4SteadyStateFlattenedHotPath...\n";

    std::vector<int> executionTrace;
    GameApi api;
    const bool bound = api.function(
        "Host.Trace",
        "Push",
        [&executionTrace](std::int32_t tag) -> bool {
            executionTrace.push_back(static_cast<int>(tag));
            return true;
        });
    RS_REQUIRE(bound && api.valid());

    const std::string scriptSrc = R"RS(
module Perf4.Scene;
import Host.Trace;

[DefaultExecutionOrder(-20)]
public class EarlyPhysicsComp {
    public int id = 0;
    public int ticks = 0;
    public void OnFixedUpdate(double dt) {
        this.ticks = this.ticks + 1;
        Push(1000 + this.id);
    }
    public void OnUpdate(double dt) {
        this.ticks = this.ticks + 1;
        Push(2000 + this.id);
    }
}

[DefaultExecutionOrder(0)]
public class MidLogicComp {
    public int id = 0;
    public int updates = 0;
    public void OnUpdate(double dt) {
        this.updates = this.updates + 1;
        Push(3000 + this.id);
    }
}

[DefaultExecutionOrder(15)]
public class LateCameraComp {
    public int id = 0;
    public int lateTicks = 0;
    public void OnLateUpdate(double dt) {
        this.lateTicks = this.lateTicks + 1;
        Push(4000 + this.id);
    }
}

public class PassiveDataComp {
    public int marker = 42;
}
)RS";

    auto compiled = compileSourcesOrDie(
        api, {{compiler::SourceFile{"perf4.rscript", scriptSrc}}}, "perf4_steady");
    ScriptRuntime rt(compiled.program);
    SceneScriptRuntime scene(rt);

    for (SceneEntityId e = 1; e <= 16; ++e) {
        SceneScriptOptions opts;
        opts.initialMembers["id"] = static_cast<std::int64_t>(e);
        RS_REQUIRE(scene.attachComponent(e, "Perf4.Scene::MidLogicComp", opts));
        RS_REQUIRE(scene.attachComponent(e, "Perf4.Scene::PassiveDataComp", {}));
        if ((e % 2u) == 0u) {
            RS_REQUIRE(scene.attachComponent(e, "Perf4.Scene::EarlyPhysicsComp", opts));
        }
        if ((e % 4u) == 0u) {
            RS_REQUIRE(scene.attachComponent(e, "Perf4.Scene::LateCameraComp", opts));
        }
    }

    scene.start();
    // Trigger initial cache build on frame 0
    scene.fixedUpdate(0.02);
    scene.update(0.016);
    scene.lateUpdate(0.016);
    RS_REQUIRE(scene.errors().empty());

    const auto warmStats = scene.lifecycleHotPathStats();
    RS_REQUIRE(warmStats.activeCachedComponents == 16u + 16u + 8u + 4u);
    RS_REQUIRE(warmStats.cachedFixedUpdateSlots == 8u);
    RS_REQUIRE(warmStats.cachedUpdateSlots == 24u);
    RS_REQUIRE(warmStats.cachedLateUpdateSlots == 4u);

    // Check deterministic ExecutionOrder on frame 0:
    // FixedUpdate: EarlyPhysicsComp (-20) on even entities 2..16 (8 entries: 1002..1016)
    // Update: EarlyPhysicsComp (-20) on 2..16, then MidLogicComp (0) on 1..16
    // LateUpdate: LateCameraComp (15) on 4, 8, 12, 16
    RS_REQUIRE(executionTrace.size() == 8u + 24u + 4u);
    RS_REQUIRE(executionTrace[0] == 1002);
    RS_REQUIRE(executionTrace[7] == 1016);
    RS_REQUIRE(executionTrace[8] == 2002);
    RS_REQUIRE(executionTrace[15] == 2016);
    RS_REQUIRE(executionTrace[16] == 3001);
    RS_REQUIRE(executionTrace[31] == 3016);
    RS_REQUIRE(executionTrace[32] == 4004);
    RS_REQUIRE(executionTrace[35] == 4016);

    scene.resetLifecycleHotPathStats();
    executionTrace.clear();

    constexpr std::size_t kFrames = 50;
    for (std::size_t f = 0; f < kFrames; ++f) {
        scene.fixedUpdate(0.02);
        scene.update(0.016);
        scene.lateUpdate(0.016);
    }
    RS_REQUIRE(scene.errors().empty());

    const auto stats = scene.lifecycleHotPathStats();
    RS_REQUIRE(stats.cacheRebuilds == 0u);
    RS_REQUIRE(stats.onDemandSnapshots == 0u);
    RS_REQUIRE(stats.midTickMutations == 0u);
    RS_REQUIRE(stats.reentrantDispatches == 0u);
    RS_REQUIRE(stats.fallbackDispatchedCallbacks == 0u);
    RS_REQUIRE(stats.cacheHits == kFrames * 3u);
    RS_REQUIRE(stats.fastDispatchedCallbacks == kFrames * (8u + 24u + 4u));
    RS_REQUIRE(stats.currentDispatchDepth == 0u);

    auto* mid1 = scene.getComponent(1, "Perf4.Scene::MidLogicComp");
    RS_REQUIRE(mid1 != nullptr);
    RS_REQUIRE(getIntMemberOrDie(rt, *mid1, "updates") == static_cast<std::int64_t>(kFrames + 1u));
}

void testPerf4MidCallbackMutationsAndReentrancy() {
    std::cout << "[RUN] testPerf4MidCallbackMutationsAndReentrancy...\n";

    ScriptRuntime* rtPtr = nullptr;
    SceneScriptRuntime* scenePtr = nullptr;
    std::vector<std::string> log;

    // Build fresh GameApi with custom mutation & reentrant hooks bound to pointer indirection
    GameApi hostApi;
    const bool okBind =
        hostApi.function(
            "Host.Mutate",
            "Record",
            [&log](std::string entry) -> bool {
                log.push_back(std::move(entry));
                return true;
            }) &&
        hostApi.function(
            "Host.Mutate",
            "MutateSceneMidUpdate",
            [&scenePtr]() -> bool {
                if (!scenePtr) return false;
                // Disable entity 2 before its OnUpdate runs in the same frame!
                (void)scenePtr->setEntityActive(2, false);
                // Detach entity 3 completely mid-frame!
                (void)scenePtr->detach(3);
                // Attach new entity 5 mid-frame (must start next frame, not mid-tick)!
                (void)scenePtr->attachComponent(5, "Perf4.Mut::Worker", {});
                return true;
            }) &&
        hostApi.function(
            "Host.Mutate",
            "TriggerNestedFixedUpdate",
            [&scenePtr]() -> bool {
                if (!scenePtr) return false;
                RS_REQUIRE(scenePtr->lifecycleDispatchDepth() == 1u);
                scenePtr->fixedUpdate(0.02);
                RS_REQUIRE(scenePtr->lifecycleDispatchDepth() == 1u);
                return true;
            });
    RS_REQUIRE(okBind && hostApi.valid());
    (void)rtPtr;

    const std::string src = R"RS(
module Perf4.Mut;
import Host.Mutate;

[DefaultExecutionOrder(-10)]
public class Mutator {
    public bool triggerMutation = false;
    public bool triggerReentrancy = false;

    public void OnUpdate(double dt) {
        Record("Mutator.Update");
        if (this.triggerMutation) {
            this.triggerMutation = false;
            MutateSceneMidUpdate();
        }
        if (this.triggerReentrancy) {
            this.triggerReentrancy = false;
            TriggerNestedFixedUpdate();
        }
    }

    public void OnFixedUpdate(double dt) {
        Record("Mutator.Fixed");
    }
}

[DefaultExecutionOrder(10)]
[Tool]
public class Worker {
    public string tag = "W";
    public void OnStart() {
        Record("Worker.Start:" + this.tag);
    }
    public void OnUpdate(double dt) {
        Record("Worker.Update:" + this.tag);
    }
    public void OnFixedUpdate(double dt) {
        Record("Worker.Fixed:" + this.tag);
    }
}
)RS";

    auto compiled = compileSourcesOrDie(
        hostApi, {{compiler::SourceFile{"perf4_mut.rscript", src}}}, "perf4_mut");
    ScriptRuntime rt(compiled.program);
    SceneScriptRuntime scene(rt);
    scenePtr = &scene;

    RS_REQUIRE(scene.attachComponent(1, "Perf4.Mut::Mutator", {}));
    for (int id = 2; id <= 4; ++id) {
        SceneScriptOptions opts;
        opts.initialMembers["tag"] = std::string("E") + std::to_string(id);
        RS_REQUIRE(scene.attachComponent(static_cast<SceneEntityId>(id), "Perf4.Mut::Worker", opts));
    }

    scene.start();
    scene.update(0.016); // Warm steady-state cache
    RS_REQUIRE(scene.errors().empty());

    // 1. Trigger mid-callback structural mutation on Entity 1
    log.clear();
    scene.resetLifecycleHotPathStats();
    runtime::RuntimeError err;
    RS_REQUIRE(rt.setMember(*scene.getComponent(1, "Perf4.Mut::Mutator"), "triggerMutation", true, err));

    const std::uint64_t genBefore = scene.lifecycleStructuralGeneration();
    scene.update(0.016);
    RS_REQUIRE(scene.errors().empty());
    RS_REQUIRE(scene.lifecycleStructuralGeneration() > genBefore);

    const auto mutStats = scene.lifecycleHotPathStats();
    RS_REQUIRE(mutStats.midTickMutations == 1u);
    RS_REQUIRE(mutStats.onDemandSnapshots == 1u);
    RS_REQUIRE(mutStats.fastDispatchedCallbacks == 1u);     // Entity 1 ran on fast path
    RS_REQUIRE(mutStats.fallbackDispatchedCallbacks == 1u); // Only Entity 4 remained active in tail!

    // Entity 2 was deactivated and Entity 3 was detached, so ONLY Entity 4 ran after Mutator;
    // Entity 5 was newly attached mid-frame (unstarted) so it did NOT run in the same tick!
    RS_REQUIRE(log.size() == 2u);
    RS_REQUIRE(log[0] == "Mutator.Update");
    RS_REQUIRE(log[1] == "Worker.Update:E4");

    // Next frame: Entity 5 runs OnStart and cache lazily rebuilds once, then hits steady state
    log.clear();
    scene.update(0.016);
    scene.update(0.016);
    const auto postMutStats = scene.lifecycleHotPathStats();
    RS_REQUIRE(postMutStats.cacheHits >= 1u);
    RS_REQUIRE(scene.isComponentStarted(5, "Perf4.Mut::Worker"));

    // 2. Trigger nested reentrant fixedUpdate inside OnUpdate
    log.clear();
    scene.resetLifecycleHotPathStats();
    RS_REQUIRE(rt.setMember(*scene.getComponent(1, "Perf4.Mut::Mutator"), "triggerReentrancy", true, err));
    scene.update(0.016);
    RS_REQUIRE(scene.errors().empty());
    RS_REQUIRE(scene.lifecycleDispatchDepth() == 0u);

    const auto reentStats = scene.lifecycleHotPathStats();
    RS_REQUIRE(reentStats.reentrantDispatches == 1u);
    RS_REQUIRE(reentStats.onDemandSnapshots == 1u);
    RS_REQUIRE(log.size() >= 5u);
    RS_REQUIRE(log[0] == "Mutator.Update");
    RS_REQUIRE(log[1] == "Mutator.Fixed");

    // 3. Editor mode filters out non-[Tool] Mutator while keeping [Tool] Worker
    const std::uint64_t genPlay = scene.lifecycleStructuralGeneration();
    scene.setExecutionMode(SceneExecutionMode::Editor);
    RS_REQUIRE(scene.lifecycleStructuralGeneration() > genPlay);
    log.clear();
    scene.update(0.016);
    for (const auto& item : log) {
        RS_REQUIRE(item.find("Mutator") == std::string::npos);
    }
    RS_REQUIRE(!log.empty());
}

// ============================================================================
// 2. eff_1 Tests: Coroutine & Sequence State-Machine Fingerprinting & Hot Reload
// ============================================================================

void testEff1FingerprintDeterminismAndSensitivity() {
    std::cout << "[RUN] testEff1FingerprintDeterminismAndSensitivity...\n";

    GameApi api;
    RS_REQUIRE(api.valid());

    const std::string srcV1 = R"RS(
module Eff1.Fp;
import RealScript.Async;

public class Hero {
    public int hp = 100;

    public async Awaitable<int> ComputeBonus(int baseVal) {
        int localFactor = baseVal + 5;
        await Awaitable.Yield();
        return localFactor + this.hp;
    }
}
)RS";

    // V2: prepends a brand-new helper class/method (shifting functionIndex!), adds a field to Hero,
    // and changes arithmetic expressions BEFORE and AFTER await without changing parameter/local/await layout!
    const std::string srcV2SameLayout = R"RS(
module Eff1.Fp;
import RealScript.Async;

public class AAHelper {
    public static int Boost(int x) { return x * 10 + 50; }
}

public class Hero {
    public int hp = 100;

    public async Awaitable<int> ComputeBonus(int baseVal) {
        int localFactor = (baseVal * 2) + 99;
        await Awaitable.Yield();
        return AAHelper.Boost(localFactor) + this.hp;
    }
}
)RS";

    // V3: adds a new local variable across suspension -> changes state-machine layout!
    const std::string srcV3AddedLocal = R"RS(
module Eff1.Fp;
import RealScript.Async;

public class Hero {
    public int hp = 100;

    public async Awaitable<int> ComputeBonus(int baseVal) {
        int localFactor = baseVal + 5;
        int extraLocal = 77;
        await Awaitable.Yield();
        return localFactor + extraLocal + this.hp;
    }
}
)RS";

    // V4: adds a second suspension point -> changes state-machine layout!
    const std::string srcV4AddedAwait = R"RS(
module Eff1.Fp;
import RealScript.Async;

public class Hero {
    public int hp = 100;

    public async Awaitable<int> ComputeBonus(int baseVal) {
        int localFactor = baseVal + 5;
        await Awaitable.Yield();
        await Awaitable.Yield();
        return localFactor + this.hp;
    }
}
)RS";

    auto c1 = compileSourcesOrDie(api, {{compiler::SourceFile{"v1.rscript", srcV1}}}, "fp_v1");
    auto c1Repeat = compileSourcesOrDie(api, {{compiler::SourceFile{"v1.rscript", srcV1}}}, "fp_v1_repeat");
    auto c2 = compileSourcesOrDie(api, {{compiler::SourceFile{"v2.rscript", srcV2SameLayout}}}, "fp_v2");
    auto c3 = compileSourcesOrDie(api, {{compiler::SourceFile{"v3.rscript", srcV3AddedLocal}}}, "fp_v3");
    auto c4 = compileSourcesOrDie(api, {{compiler::SourceFile{"v4.rscript", srcV4AddedAwait}}}, "fp_v4");

    const auto fp1 = findCoroutineStateMachineFingerprint(
        *c1.program.program(), "Eff1.Fp::Hero.ComputeBonus");
    const auto fp1Repeat = findCoroutineStateMachineFingerprint(
        *c1Repeat.program.program(), "Eff1.Fp::Hero.ComputeBonus");
    const auto fp2 = findCoroutineStateMachineFingerprint(
        *c2.program.program(), "Eff1.Fp::Hero.ComputeBonus");
    const auto fp3 = findCoroutineStateMachineFingerprint(
        *c3.program.program(), "Eff1.Fp::Hero.ComputeBonus");
    const auto fp4 = findCoroutineStateMachineFingerprint(
        *c4.program.program(), "Eff1.Fp::Hero.ComputeBonus");

    RS_REQUIRE(fp1.has_value() && fp1Repeat.has_value() && fp2.has_value() &&
           fp3.has_value() && fp4.has_value());

    // Deterministic across runs & unchanged when body expressions / class fields / functionIndex change
    RS_REQUIRE(fp1->structuralHash == fp1Repeat->structuralHash);
    RS_REQUIRE(fp1->matchesLayout(*fp2));
    RS_REQUIRE(fp1->functionIndex != fp2->functionIndex); // Shifted due to AAHelper!
    RS_REQUIRE(fp1->suspensionPointCount == 1u);
    RS_REQUIRE(fp2->suspensionPointCount == 1u);

    // Sensitive to local slot layout & suspension point layout changes
    RS_REQUIRE(!fp1->matchesLayout(*fp3));
    RS_REQUIRE(fp1->structuralHash != fp3->structuralHash);
    RS_REQUIRE(!fp1->matchesLayout(*fp4));
    RS_REQUIRE(fp1->structuralHash != fp4->structuralHash);
    RS_REQUIRE(fp4->suspensionPointCount == 2u);
}

void testEff1AsyncCoroutinePreservationAndChainedHotReload() {
    std::cout << "[RUN] testEff1AsyncCoroutinePreservationAndChainedHotReload...\n";

    GameApi api;
    SceneScriptRuntime* sceneHook = nullptr;

    // Bind scene async HTTP & custom host inspection
    const bool bound = api.function(
        "Host.Net",
        "FetchBonusAsync",
        [&sceneHook](std::string url) -> TypedAwaitable<std::string> {
            SceneScriptRuntime::HttpRequestSpec spec;
            spec.method = "GET";
            spec.url = std::move(url);
            return TypedAwaitable<std::string>{
                sceneHook->httpRequestAsync(std::move(spec), false)};
        });
    RS_REQUIRE(bound && api.valid());

    const std::string srcV1 = R"RS(
module Eff1.Live;
import RealScript.Async;
import Host.Net;

public class AllyBuff {
    public int aura = 7;
}

public class QuestActor {
    public int basePower = 10;
    public int oldMultiplier = 2;
    public int finalScore = 0;
    public string finalPayload = "";
    public AllyBuff ally;

    public void OnStart() {
        this.RunQuestPipeline(5);
    }

    public async Awaitable RunQuestPipeline(int seedStep) {
        int capturedLocal = this.basePower + seedStep; // 10 + 5 = 15
        await Awaitable.Yield();
        string resp = await FetchBonusAsync("https://game.local/bonus");
        this.finalPayload = resp;
        this.finalScore = capturedLocal * this.oldMultiplier;
    }
}
)RS";

    // V2: Prepends a new class & static function (shifting QuestActor.RunQuestPipeline's functionIndex!),
    // renames `oldMultiplier` -> `renamedMultiplier` via `[FormerlySerializedAs]`, adds `newFlatBonus = 100`,
    // and updates post-resume calculation while keeping RunQuestPipeline's state-machine fingerprint intact!
    const std::string srcV2 = R"RS(
module Eff1.Live;
import RealScript.Async;
import Host.Net;

public class AAExtraModuleUtility {
    public static int ComputeShift(int v) {
        return v + 1000;
    }
}

public class AllyBuff {
    public int aura = 7;
    public int extraAura = 3;
}

public class QuestActor {
    public int basePower = 10;
    [FormerlySerializedAs("oldMultiplier")]
    public int renamedMultiplier = 2;
    public int newFlatBonus = 100;
    public int finalScore = 0;
    public string finalPayload = "";
    public AllyBuff ally;

    public void OnStart() {
        this.RunQuestPipeline(5);
    }

    public async Awaitable RunQuestPipeline(int seedStep) {
        int capturedLocal = this.basePower + seedStep;
        await Awaitable.Yield();
        string resp = await FetchBonusAsync("https://game.local/bonus");
        this.finalPayload = resp + ":v2";
        this.finalScore = (capturedLocal * this.renamedMultiplier) + this.newFlatBonus + this.ally.aura;
    }
}
)RS";

    // V3: Chained second hot reload while suspended at the SECOND await (`FetchBonusAsync`)!
    const std::string srcV3 = R"RS(
module Eff1.Live;
import RealScript.Async;
import Host.Net;

public class AAExtraModuleUtility {
    public static int ComputeShift(int v) { return v + 2000; }
    public static int SecondHelper(int v) { return v * 2; }
}

public class AllyBuff {
    public int aura = 7;
    public int extraAura = 3;
}

public class QuestActor {
    public int basePower = 10;
    public int renamedMultiplier = 2;
    public int newFlatBonus = 100;
    public int v3Multiplier = 10;
    public int finalScore = 0;
    public string finalPayload = "";
    public AllyBuff ally;

    public void OnStart() {
        this.RunQuestPipeline(5);
    }

    public async Awaitable RunQuestPipeline(int seedStep) {
        int capturedLocal = this.basePower + seedStep;
        await Awaitable.Yield();
        string resp = await FetchBonusAsync("https://game.local/bonus");
        this.finalPayload = resp + ":v3";
        this.finalScore = (capturedLocal * this.renamedMultiplier) +
            this.newFlatBonus + this.ally.aura + this.ally.extraAura + this.v3Multiplier;
    }
}
)RS";

    auto c1 = compileSourcesOrDie(api, {{compiler::SourceFile{"live_v1.rscript", srcV1}}}, "live_v1");
    auto c2 = compileSourcesOrDie(api, {{compiler::SourceFile{"live_v2.rscript", srcV2}}}, "live_v2");
    auto c3 = compileSourcesOrDie(api, {{compiler::SourceFile{"live_v3.rscript", srcV3}}}, "live_v3");

    ScriptRuntime rt(c1.program);
    SceneScriptRuntime scene(rt);
    sceneHook = &scene;
    scene.registerHttpEndpoint("GET", "https://game.local/bonus", 200, "OK_BONUS", 2);

    SceneScriptOptions allyOpts;
    allyOpts.initialMembers["aura"] = std::int64_t{9};
    RS_REQUIRE(scene.attachComponent(10, "Eff1.Live::AllyBuff", allyOpts));

    SceneScriptOptions questOpts;
    questOpts.initialMembers["basePower"] = std::int64_t{20};
    questOpts.initialMembers["oldMultiplier"] = std::int64_t{3};
    questOpts.initialMembers["ally"] = scene.getComponent(10, "Eff1.Live::AllyBuff")->value();
    RS_REQUIRE(scene.attachComponent(20, "Eff1.Live::QuestActor", questOpts));

    // Start scene -> RunQuestPipeline computes capturedLocal = 20 + 5 = 25, suspends at Awaitable.Yield()!
    scene.start();
    RS_REQUIRE(scene.errors().empty());
    RS_REQUIRE(rt.asyncRuntime()->coroutines().statistics().activeCount == 1u);

    const auto fpV1 = findCoroutineStateMachineFingerprint(
        *c1.program.program(), "Eff1.Live::QuestActor.RunQuestPipeline");
    const auto fpV2 = findCoroutineStateMachineFingerprint(
        *c2.program.program(), "Eff1.Live::QuestActor.RunQuestPipeline");
    RS_REQUIRE(fpV1.has_value() && fpV2.has_value());
    RS_REQUIRE(fpV1->functionIndex != fpV2->functionIndex);

    // Hot Reload #1 (V1 -> V2) while suspended at await #1 (`Awaitable.Yield`)!
    const auto report1 = scene.hotReload(c2.program);
    RS_REQUIRE(report1.succeeded);
    RS_REQUIRE(report1.instancesMigrated == 2u);
    RS_REQUIRE(report1.fieldsRenamed == 1u);
    RS_REQUIRE(report1.preservedCoroutines == 1u);
    RS_REQUIRE(report1.abortedCoroutines == 0u);
    const auto remapIt = report1.remappedCoroutineFunctionIndices.find(fpV1->functionIndex);
    RS_REQUIRE(remapIt != report1.remappedCoroutineFunctionIndices.end());
    RS_REQUIRE(remapIt->second == fpV2->functionIndex);
    RS_REQUIRE(rt.asyncRuntime()->coroutines().statistics().activeCount == 1u);

    // Frame 1: resumes from Awaitable.Yield() inside V2, issues FetchBonusAsync (latency=2 frames),
    // and suspends at await #2!
    scene.update(0.016);
    RS_REQUIRE(scene.errors().empty());
    RS_REQUIRE(rt.asyncRuntime()->coroutines().statistics().activeCount == 1u);

    // Hot Reload #2 (V2 -> V3) while suspended at await #2 (`FetchBonusAsync`)!
    const auto report2 = scene.hotReload(c3.program);
    RS_REQUIRE(report2.succeeded);
    RS_REQUIRE(report2.preservedCoroutines == 1u);
    RS_REQUIRE(report2.abortedCoroutines == 0u);
    RS_REQUIRE(rt.asyncRuntime()->coroutines().statistics().activeCount == 1u);

    // Advance frames so HTTP latency finishes and coroutine resumes into V3!
    scene.update(0.016);
    scene.update(0.016);
    scene.update(0.016);
    RS_REQUIRE(scene.errors().empty());
    RS_REQUIRE(rt.asyncRuntime()->coroutines().statistics().activeCount == 0u);
    RS_REQUIRE(rt.asyncRuntime()->coroutines().statistics().activeGcRoots == 0u);

    const auto* actorV3 = scene.getComponent(20, "Eff1.Live::QuestActor");
    RS_REQUIRE(actorV3 != nullptr);
    // capturedLocal (25) * renamedMultiplier (3) + newFlatBonus (100) + ally.aura (9) + ally.extraAura (3) + v3Multiplier (10) = 197!
    RS_REQUIRE(getIntMemberOrDie(rt, *actorV3, "finalScore") == 197);
    RS_REQUIRE(getStringMemberOrDie(rt, *actorV3, "finalPayload") == "OK_BONUS:v3");
}

void testEff1SelectivePreserveAndSafeAbortConcurrentCoroutines() {
    std::cout << "[RUN] testEff1SelectivePreserveAndSafeAbortConcurrentCoroutines...\n";

    GameApi api;
    RS_REQUIRE(api.valid());

    const std::string srcV1 = R"RS(
module Eff1.Selective;
import RealScript.Async;

public class StableWorker {
    public int result = 0;
    public void OnStart() { this.Run(); }
    public async Awaitable Run() {
        int seed = 11;
        await Awaitable.Yield();
        this.result = seed + 1;
    }
}

public class ChangedLayoutWorker {
    public int result = 0;
    public void OnStart() { this.Run(); }
    public async Awaitable Run() {
        int seed = 22;
        await Awaitable.Yield();
        this.result = seed + 1;
    }
}

public class RemovedCoroWorker {
    public int result = 0;
    public void OnStart() { this.ObsoleteCoro(); }
    public async Awaitable ObsoleteCoro() {
        await Awaitable.Yield();
        this.result = 999;
    }
}
)RS";

    const std::string srcV2 = R"RS(
module Eff1.Selective;
import RealScript.Async;

public class StableWorker {
    public int result = 0;
    public void OnStart() { this.Run(); }
    public async Awaitable Run() {
        int seed = 11;
        await Awaitable.Yield();
        this.result = seed * 10; // Updated body logic, identical state-machine layout!
    }
}

public class ChangedLayoutWorker {
    public int result = 0;
    public void OnStart() { this.Run(); }
    public async Awaitable Run() {
        int seed = 22;
        string addedLocal = "changed_slot_layout";
        await Awaitable.Yield();
        await Awaitable.Yield();
        if (addedLocal != "") {
            this.result = seed + 500;
        }
    }
}

public class RemovedCoroWorker {
    public int result = 77;
}
)RS";

    auto c1 = compileSourcesOrDie(api, {{compiler::SourceFile{"sel_v1.rscript", srcV1}}}, "sel_v1");
    auto c2 = compileSourcesOrDie(api, {{compiler::SourceFile{"sel_v2.rscript", srcV2}}}, "sel_v2");

    ScriptRuntime rt(c1.program);
    SceneScriptRuntime scene(rt);

    RS_REQUIRE(scene.attachComponent(1, "Eff1.Selective::StableWorker", {}));
    RS_REQUIRE(scene.attachComponent(2, "Eff1.Selective::ChangedLayoutWorker", {}));
    RS_REQUIRE(scene.attachComponent(3, "Eff1.Selective::RemovedCoroWorker", {}));

    scene.start();
    RS_REQUIRE(rt.asyncRuntime()->coroutines().statistics().activeCount == 3u);

    const auto report = scene.hotReload(c2.program);
    RS_REQUIRE(report.succeeded);
    RS_REQUIRE(report.instancesMigrated == 3u);
    RS_REQUIRE(report.preservedCoroutines == 1u);
    RS_REQUIRE(report.abortedCoroutines == 2u);
    RS_REQUIRE(rt.asyncRuntime()->coroutines().statistics().activeCount == 1u);

    // Step next frame: StableWorker completes with V2 logic (11 * 10 = 110);
    // ChangedLayoutWorker and RemovedCoroWorker remain untouched and leak-free!
    scene.update(0.016);
    RS_REQUIRE(scene.errors().empty());
    RS_REQUIRE(rt.asyncRuntime()->coroutines().statistics().activeCount == 0u);
    RS_REQUIRE(rt.asyncRuntime()->coroutines().statistics().activeGcRoots == 0u);

    RS_REQUIRE(getIntMemberOrDie(rt, *scene.getComponent(1, "Eff1.Selective::StableWorker"), "result") == 110);
    RS_REQUIRE(getIntMemberOrDie(rt, *scene.getComponent(2, "Eff1.Selective::ChangedLayoutWorker"), "result") == 0);
    RS_REQUIRE(getIntMemberOrDie(rt, *scene.getComponent(3, "Eff1.Selective::RemovedCoroWorker"), "result") == 0);
}

void testEff1SequenceAndFiberSelectiveHotReload() {
    std::cout << "[RUN] testEff1SequenceAndFiberSelectiveHotReload...\n";

    auto host = std::make_shared<GameplayHost>(20); // 20 ticks/sec -> 0.1s = 2 ticks
    GameApi api;
    RS_REQUIRE(installGameplayBindings(api, host));
    RS_REQUIRE(api.valid());

    const std::string seqV1 = R"RS(
module Eff1.Seq;
import RealScript.Game;

public class SeqActor {
    public int keptStage = 0;
    public int changedStage = 0;

    public sequence KeepFlow(long selfId) {
        this.keptStage = 1;
        yield wait_seconds(0.1);
        this.keptStage = 10;
    }

    public sequence LayoutChangedFlow(long selfId) {
        int count = 1;
        this.changedStage = count;
        yield wait_seconds(0.1);
        this.changedStage = count + 10;
    }
}
)RS";

    const std::string seqV2 = R"RS(
module Eff1.Seq;
import RealScript.Game;

public class SeqActor {
    public int keptStage = 0;
    public int changedStage = 0;
    public int bonus = 77;

    public sequence KeepFlow(long selfId) {
        this.keptStage = 1;
        yield wait_seconds(0.1);
        this.keptStage = 200 + this.bonus; // Unchanged sequence layout, updated post-yield body!
    }

    public sequence LayoutChangedFlow(long selfId) {
        int count = 1;
        int extraSlot = 50; // Added local slot + extra yield -> layout changed!
        this.changedStage = count;
        yield wait_seconds(0.1);
        yield wait_seconds(0.1);
        this.changedStage = count + extraSlot;
    }
}
)RS";

    auto sc1 = compileSourcesOrDie(api, {{compiler::SourceFile{"seq_v1.rscript", seqV1}}}, "seq_v1");
    auto sc2 = compileSourcesOrDie(api, {{compiler::SourceFile{"seq_v2.rscript", seqV2}}}, "seq_v2");

    ScriptRuntime rt(sc1.program);
    SceneScriptRuntime scene(rt);
    SceneGameplayDriver driver(scene, host);

    const SceneEntityId ent = 7;
    RS_REQUIRE(scene.attachComponent(ent, "Eff1.Seq::SeqActor", {}));
    scene.start();

    RS_REQUIRE(scene.invoke(
        ent,
        "KeepFlow",
        {runtime::LongValue{7}}).succeeded);
    RS_REQUIRE(scene.invoke(
        ent,
        "LayoutChangedFlow",
        {runtime::LongValue{7}}).succeeded);

    RS_REQUIRE(getIntMemberOrDie(rt, *scene.getComponent(ent, "Eff1.Seq::SeqActor"), "keptStage") == 1);
    RS_REQUIRE(getIntMemberOrDie(rt, *scene.getComponent(ent, "Eff1.Seq::SeqActor"), "changedStage") == 1);

    const auto seqReport = scene.hotReload(sc2.program);
    RS_REQUIRE(seqReport.succeeded);
    RS_REQUIRE(seqReport.preservedCoroutines == 1u);
    RS_REQUIRE(seqReport.abortedCoroutines == 1u);

    // Advance GameplayHost 2 ticks (0.1s at 20 TPS): KeepFlow resumes with V2 code (200 + 77 = 277),
    // while LayoutChangedFlow was safely canceled and stays at 1!
    (void)driver.advanceTicks(2);
    RS_REQUIRE(scene.errors().empty());
    RS_REQUIRE(getIntMemberOrDie(rt, *scene.getComponent(ent, "Eff1.Seq::SeqActor"), "keptStage") == 277);
    RS_REQUIRE(getIntMemberOrDie(rt, *scene.getComponent(ent, "Eff1.Seq::SeqActor"), "changedStage") == 1);

    // Also verify FiberScheduler::reloadModuleWithReport selective preservation & abortion
    struct HostRecorder final : runtime::IFiberHostCallbacks {
        std::vector<int> values;
        void onHostCommand(
            runtime::FiberContext&,
            std::uint8_t,
            std::uint16_t,
            std::uint32_t arg32) noexcept override {
            values.push_back(static_cast<int>(arg32));
        }
    } fiberRecorder;

    const std::string fibV1 = R"RS(
module AI.Bt;
import RealScript.Fiber;

public void KeepPatrol() {
    Fiber.Yield(2);
    Fiber.Command(1, 0, 10);
}

public void MutatedPatrol() {
    Fiber.Yield(2);
    Fiber.Command(1, 0, 20);
}
)RS";

    const std::string fibV2 = R"RS(
module AI.Bt;
import RealScript.Fiber;

public void PrependedUtility() {
    Fiber.Command(1, 0, 1);
}

public void KeepPatrol() {
    Fiber.Yield(2);
    Fiber.Command(1, 0, 999);
}

public void MutatedPatrol() {
    Fiber.Yield(2);
    Fiber.Yield(2);
    Fiber.Command(1, 0, 555);
}
)RS";

    const auto fMod1 = compileFiberSourceOrDie(fibV1, "fib_v1");
    const auto fMod2 = compileFiberSourceOrDie(fibV2, "fib_v2");

    runtime::FiberScheduler scheduler(&fiberRecorder);
    const std::uint32_t progId = scheduler.registerModule(fMod1);
    (void)scheduler.spawnFiberByProgram(
        progId, fMod1.entryPc("KeepPatrol"), runtime::GenerationalEntityHandle{1, 1});
    (void)scheduler.spawnFiberByProgram(
        progId, fMod1.entryPc("MutatedPatrol"), runtime::GenerationalEntityHandle{2, 1});

    (void)scheduler.stepTick(1); // Both fibers enter Yield(2) sleep (wakeTick = 3)
    RS_REQUIRE(scheduler.activeFiberCount() == 2u);

    const auto fibReport = scheduler.reloadModuleWithReport(progId, fMod1, fMod2);
    RS_REQUIRE(fibReport.succeeded);
    RS_REQUIRE(fibReport.preservedFibers == 1u);
    RS_REQUIRE(fibReport.abortedFibers == 1u);
    RS_REQUIRE(scheduler.activeFiberCount() == 1u);

    (void)scheduler.stepTick(2);
    (void)scheduler.stepTick(3);
    RS_REQUIRE(fiberRecorder.values.size() == 1u);
    RS_REQUIRE(fiberRecorder.values[0] == 999);
}

// ============================================================================
// 3. eff_3 Tests: AssetGuid, AssetGuidRegistry, Refactoring & Git 3-Way Merge
// ============================================================================

void testEff3AssetGuidAndRegistryRefactoring() {
    std::cout << "[RUN] testEff3AssetGuidAndRegistryRefactoring...\n";

    const AssetGuid gTypeA = AssetGuid::fromTypeName("Game.Combat::TurretController");
    const AssetGuid gTypeARepeat = AssetGuid::fromTypeName("Game.Combat::TurretController");
    const AssetGuid gTypeB = AssetGuid::fromTypeName("Game.Combat::EnemySpawner");
    RS_REQUIRE(gTypeA.valid());
    RS_REQUIRE(gTypeA == gTypeARepeat);
    RS_REQUIRE(gTypeA != gTypeB);

    const std::string uuidStr = gTypeA.toString();
    RS_REQUIRE(uuidStr.size() == 36u);
    RS_REQUIRE(uuidStr[8] == '-' && uuidStr[13] == '-' && uuidStr[18] == '-' && uuidStr[23] == '-');
    RS_REQUIRE(uuidStr[14] == '5'); // RFC-4122 UUID v5 version nibble!

    const auto parsedPlain = AssetGuid::parse(uuidStr);
    const auto parsedBraced = AssetGuid::parse("{" + uuidStr + "}");
    std::string hex32;
    for (char c : uuidStr) {
        if (c != '-') hex32.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    const auto parsedHex32 = AssetGuid::parse(hex32);
    RS_REQUIRE(parsedPlain == gTypeA);
    RS_REQUIRE(parsedBraced == gTypeA);
    RS_REQUIRE(parsedHex32 == gTypeA);
    RS_REQUIRE(!AssetGuid::parse("not-a-valid-uuid").has_value());

    // Path normalization determinism across Windows & POSIX separators
    const AssetGuid gPathPosix = AssetGuid::fromAssetPath("./assets//weapons/plasma_rifle.asset/");
    const AssetGuid gPathWin = AssetGuid::fromAssetPath("assets\\weapons\\plasma_rifle.asset");
    RS_REQUIRE(gPathPosix == gPathWin);

    // Bidirectional registry + rename refactoring + text persistence
    AssetGuidRegistry reg;
    const AssetGuid tGuid = reg.registerType("Legacy::Cannon");
    const AssetGuid aGuid = reg.registerAssetPath("assets/old/cannon.cfg");
    RS_REQUIRE(reg.findGuidForType("Legacy::Cannon") == tGuid);
    RS_REQUIRE(reg.resolveTypeName(tGuid) == std::string("Legacy::Cannon"));
    RS_REQUIRE(reg.findGuidForAssetPath("assets\\old\\cannon.cfg") == aGuid);
    RS_REQUIRE(reg.resolveAssetPath(aGuid) == std::string("assets/old/cannon.cfg"));

    // Rename both class and asset path while keeping stable GUIDs!
    RS_REQUIRE(reg.rebindType(tGuid, "Modern.Weapons::HeavyCannon"));
    RS_REQUIRE(reg.rebindAssetPath(aGuid, "content/weapons/heavy_cannon_v2.cfg"));
    RS_REQUIRE(!reg.findGuidForType("Legacy::Cannon").has_value());
    RS_REQUIRE(reg.findGuidForType("Modern.Weapons::HeavyCannon") == tGuid);
    RS_REQUIRE(reg.resolveTypeName(tGuid) == std::string("Modern.Weapons::HeavyCannon"));
    RS_REQUIRE(reg.resolveAssetPath(aGuid) == std::string("content/weapons/heavy_cannon_v2.cfg"));

    const std::string regText = reg.serializeRegistryText();
    AssetGuidRegistry restoredReg;
    runtime::RuntimeError err;
    RS_REQUIRE(restoredReg.deserializeRegistryText(regText, err));
    RS_REQUIRE(restoredReg.resolveTypeName(tGuid) == std::string("Modern.Weapons::HeavyCannon"));
    RS_REQUIRE(restoredReg.resolveAssetPath(aGuid) == std::string("content/weapons/heavy_cannon_v2.cfg"));
}

void testEff3SceneAndPrefabGuidRenameAndRoundTrip() {
    std::cout << "[RUN] testEff3SceneAndPrefabGuidRenameAndRoundTrip...\n";

    GameApi api;
    RS_REQUIRE(api.valid());

    const std::string srcV1 = R"RS(
module OldGame.Units;

delegate void AlertHandler(int power);

public enum AlertState {
    Idle = 0,
    Engaged = 2
}

public struct TurretStats {
    public int damage;
    public int range;
}

[Guid("11111111-2222-5333-8444-555555555555")]
public class TargetBeacon {
    public string label = "Beacon";
    public int hits = 0;

    public void OnAlert(int power) {
        this.hits = this.hits + power;
    }
}

public class TurretUnit {
    public event AlertHandler Alerted;
    public int ammo = 30;
    [AssetRef]
    public string configAssetPath = "";
    public AlertState state = AlertState.Idle;
    public TurretStats stats;
    public TargetBeacon lockedTarget;
    public int[] patrolWaypoints;

    public void FireAlert(int power) {
        Alerted(power);
    }
}
)RS";

    // V2: Module & class names refactored (`OldGame.Units` -> `Refactored.Defense`,
    // `TurretUnit` -> `SmartTurretV2`, `TargetBeacon` -> `RadarBeaconV2` preserving explicit `[Guid]`)!
    const std::string srcV2 = R"RS(
module Refactored.Defense;

delegate void AlertHandler(int power);

public enum AlertStateV2 {
    Idle = 0,
    Engaged = 2
}

public struct TurretStatsV2 {
    public int damage;
    public int range;
}

[Guid("11111111-2222-5333-8444-555555555555")]
public class RadarBeaconV2 {
    public string label = "Beacon";
    public int hits = 0;

    public void OnAlert(int power) {
        this.hits = this.hits + power;
    }
}

public class SmartTurretV2 {
    public event AlertHandler Alerted;
    public int ammo = 30;
    [AssetRef]
    public string configAssetPath = "";
    public AlertStateV2 state = AlertStateV2.Idle;
    public TurretStatsV2 stats;
    public RadarBeaconV2 lockedTarget;
    public int[] patrolWaypoints;

    public void FireAlert(int power) {
        Alerted(power);
    }
}
)RS";

    auto c1 = compileSourcesOrDie(api, {{compiler::SourceFile{"eff3_v1.rscript", srcV1}}}, "eff3_v1");
    auto c2 = compileSourcesOrDie(api, {{compiler::SourceFile{"eff3_v2.rscript", srcV2}}}, "eff3_v2");

    AssetGuidRegistry guidReg;
    guidReg.populateFromProgram(c1.program);
    const AssetGuid cfgAssetGuid = guidReg.registerAssetPath("assets/turrets/mk1.cfg");
    RS_REQUIRE(guidReg.findGuidForType("OldGame.Units::TargetBeacon")->toString() ==
           "11111111-2222-5333-8444-555555555555");

    ScriptRuntime rt1(c1.program);
    SceneScriptRuntime scene1(rt1);

    // Populate Entity 10 (Beacon) & Entity 20 (Turret child of 10)
    RS_REQUIRE(scene1.attachComponent(10, "OldGame.Units::TargetBeacon", {}));
    scene1.setEntityName(10, "MainBeacon");
    scene1.addEntityToGroup(10, "radar");
    scene1.addEntityToGroup(10, "objectives");

    Transform3D tf10 = Transform3D::identity();
    tf10.position = Vec3{Fixed::fromInt(4), Fixed::fromInt(8), Fixed::fromInt(0)};
    scene1.setLocalTransform(10, tf10);

    SceneCollider2D col2d;
    col2d.isCircle = true;
    col2d.radius = Fixed::fromInt(3);
    col2d.isTrigger = true;
    scene1.setEntityCollider2D(10, col2d);

    runtime::RuntimeError err;
    RS_REQUIRE(rt1.setMember(*scene1.getComponent(10, "OldGame.Units::TargetBeacon"), "label", std::string("Alpha"), err));

    RS_REQUIRE(scene1.attachComponent(20, "OldGame.Units::TurretUnit", {}));
    scene1.setEntityName(20, "DefenseTurret");
    RS_REQUIRE(scene1.setParent(20, 10));

    auto* turret1 = scene1.getComponent(20, "OldGame.Units::TurretUnit");
    RS_REQUIRE(turret1 != nullptr);
    RS_REQUIRE(rt1.setMember(*turret1, "ammo", std::int64_t{75}, err));
    RS_REQUIRE(rt1.setMember(*turret1, "configAssetPath", std::string("assets/turrets/mk1.cfg"), err));
    const auto enumTid1 = semantic::stableTypeId("OldGame.Units::AlertState");
    RS_REQUIRE(rt1.setMember(*turret1, "state", runtime::Value{runtime::EnumValue{enumTid1, 2}}, err));
    const auto statsVal = rt1.createStruct(
        "OldGame.Units::TurretStats",
        {std::int64_t{45}, std::int64_t{120}},
        err);
    RS_REQUIRE(statsVal.has_value());
    RS_REQUIRE(rt1.setMember(*turret1, "stats", *statsVal, err));
    RS_REQUIRE(rt1.setMember(
        *turret1,
        "lockedTarget",
        scene1.getComponent(10, "OldGame.Units::TargetBeacon")->value(),
        err));

    const auto wpArrTid = semantic::stableTypeId(
        semantic::arrayTypeName(semantic::PrimitiveType::Int, {}));
    const auto wpArr = rt1.heap()->allocateTypedArray(
        wpArrTid, semantic::PrimitiveType::Int, 0, 3, std::int64_t{0}, &err);
    RS_REQUIRE(wpArr.has_value());
    RS_REQUIRE(rt1.writeArrayInt(*wpArr, {10, 20, 30}, err));
    RS_REQUIRE(rt1.setMember(*turret1, "patrolWaypoints", runtime::Value{*wpArr}, err));

    RS_REQUIRE(scene1.connectSignal(
        20, "OldGame.Units::TurretUnit", "Alerted",
        10, "OldGame.Units::TargetBeacon", "OnAlert"));
    scene1.setInputAction("Fire", true);
    scene1.setInputAxis2D("Aim", Vec2{Fixed::fromInt(1), Fixed::fromInt(-1)});
    RS_REQUIRE(rt1.setEcsEntityComponent(20, "OldGame.Units::TurretStats", *statsVal, err));

    scene1.start();

    const std::string sceneText1 = serializeSceneText(scene1, err, &guidReg);
    RS_REQUIRE(err.code == runtime::ErrorCode::None && !sceneText1.empty());

    // Round-trip into clean V1 scene and verify byte-for-byte deterministic re-serialization!
    ScriptRuntime rt1Copy(c1.program);
    SceneScriptRuntime scene1Copy(rt1Copy);
    RS_REQUIRE(deserializeSceneText(scene1Copy, sceneText1, err, &guidReg));
    const std::string sceneText1RoundTrip = serializeSceneText(scene1Copy, err, &guidReg);
    RS_REQUIRE(sceneText1 == sceneText1RoundTrip);

    // Also serialize a multi-node Prefab referencing V1 class names & asset path
    PrefabNodeTemplate prefabV1;
    prefabV1.name = "SquadRoot";
    prefabV1.localTransform = tf10;
    {
        PrefabComponentTemplate beaconComp;
        beaconComp.scriptType = "OldGame.Units::TargetBeacon";
        beaconComp.options.initialMembers["label"] = std::string("PrefabBeacon");
        prefabV1.components.push_back(std::move(beaconComp));

        PrefabNodeTemplate turretChild;
        turretChild.name = "TurretMount";
        PrefabComponentTemplate turretComp;
        turretComp.scriptType = "OldGame.Units::TurretUnit";
        turretComp.options.initialMembers["ammo"] = std::int64_t{99};
        turretComp.options.initialMembers["configAssetPath"] = std::string("assets/turrets/mk1.cfg");
        turretChild.components.push_back(std::move(turretComp));
        prefabV1.children.push_back(std::move(turretChild));
    }
    const std::string prefabText1 = serializePrefabText(prefabV1, err, &guidReg, &rt1);
    RS_REQUIRE(err.code == runtime::ErrorCode::None && !prefabText1.empty());

    // Refactor types & asset path in AssetGuidRegistry for V2:
    // - RadarBeaconV2 automatically rebinds via its matching `[Guid("11111111-...")]` in populateFromProgram!
    // - TurretUnit, AlertState, TurretStats, and mk1.cfg are rebound via GUID!
    (void)guidReg.registerRenamedType("OldGame.Units::TurretUnit", "Refactored.Defense::SmartTurretV2");
    (void)guidReg.registerRenamedType("OldGame.Units::AlertState", "Refactored.Defense::AlertStateV2");
    (void)guidReg.registerRenamedType("OldGame.Units::TurretStats", "Refactored.Defense::TurretStatsV2");
    RS_REQUIRE(guidReg.rebindAssetPath(cfgAssetGuid, "content/defense/smart_turret_v2.cfg"));
    guidReg.populateFromProgram(c2.program);

    // Deserialize V1 scene text directly into V2 runtime!
    ScriptRuntime rt2(c2.program);
    SceneScriptRuntime scene2(rt2);
    RS_REQUIRE(deserializeSceneText(scene2, sceneText1, err, &guidReg));

    auto* beacon2 = scene2.getComponent(10, "Refactored.Defense::RadarBeaconV2");
    auto* turret2 = scene2.getComponent(20, "Refactored.Defense::SmartTurretV2");
    RS_REQUIRE(beacon2 != nullptr && turret2 != nullptr);
    RS_REQUIRE(scene2.getParent(20) == 10);
    RS_REQUIRE(scene2.isEntityInGroup(10, "radar") && scene2.isEntityInGroup(10, "objectives"));
    RS_REQUIRE(getIntMemberOrDie(rt2, *turret2, "ammo") == 75);
    RS_REQUIRE(getStringMemberOrDie(rt2, *turret2, "configAssetPath") ==
           "content/defense/smart_turret_v2.cfg");

    // Cross-entity ObjectRef & wired signal both survived class & module renames!
    const auto lockedVal = rt2.getMember(*turret2, "lockedTarget", err);
    RS_REQUIRE(lockedVal.has_value());
    RS_REQUIRE(std::get<runtime::ObjectRef>(*lockedVal) == beacon2->reference());

    RS_REQUIRE(scene2.invoke(20, "FireAlert", {std::int64_t{12}}).succeeded);
    RS_REQUIRE(getIntMemberOrDie(rt2, *beacon2, "hits") == 12);
    RS_REQUIRE(scene2.isInputActionPressed("Fire"));
    RS_REQUIRE(rt2.hasEcsEntityComponent(20, "Refactored.Defense::TurretStatsV2"));

    // Deserialize V1 prefab text into V2 and instantiate!
    const auto prefabV2 = deserializePrefabText(prefabText1, err, &guidReg, &rt2);
    RS_REQUIRE(prefabV2.has_value());
    RS_REQUIRE(prefabV2->components.size() == 1u);
    RS_REQUIRE(prefabV2->components[0].scriptType == "Refactored.Defense::RadarBeaconV2");
    RS_REQUIRE(prefabV2->children.size() == 1u);
    RS_REQUIRE(prefabV2->children[0].components[0].scriptType == "Refactored.Defense::SmartTurretV2");

    SceneEntityId nextId = 100;
    const SceneEntityId instRoot = scene2.instantiatePrefab(*prefabV2, nextId);
    RS_REQUIRE(instRoot == 100);
    const auto children = scene2.getChildren(instRoot);
    RS_REQUIRE(children.size() == 1u);
    auto* instTurret = scene2.getComponent(children[0], "Refactored.Defense::SmartTurretV2");
    RS_REQUIRE(instTurret != nullptr);
    RS_REQUIRE(getIntMemberOrDie(rt2, *instTurret, "ammo") == 99);
    RS_REQUIRE(getStringMemberOrDie(rt2, *instTurret, "configAssetPath") ==
           "content/defense/smart_turret_v2.cfg");
}

void testEff3GitThreeWayMergeConflictFree() {
    std::cout << "[RUN] testEff3GitThreeWayMergeConflictFree...\n";

    GameApi api;
    RS_REQUIRE(api.valid());

    const std::string src = R"RS(
module Collab.Game;

public class UnitStats {
    public int hp = 100;
    public int speed = 10;
    public string role = "guard";
}

public class AiBrain {
    public int aggroRadius = 15;
}
)RS";

    auto compiled = compileSourcesOrDie(
        api, {{compiler::SourceFile{"collab.rscript", src}}}, "collab");
    AssetGuidRegistry guidReg;
    guidReg.populateFromProgram(compiled.program);

    const auto buildBaseScene = [&](SceneScriptRuntime& sc) {
        for (SceneEntityId id = 1; id <= 3; ++id) {
            SceneScriptOptions opts;
            opts.initialMembers["hp"] = static_cast<std::int64_t>(100 * id);
            opts.initialMembers["speed"] = static_cast<std::int64_t>(10 * id);
            opts.initialMembers["role"] = std::string("unit_") + std::to_string(id);
            RS_REQUIRE(sc.attachComponent(id, "Collab.Game::UnitStats", opts));
            sc.setEntityName(id, "Entity" + std::to_string(id));
        }
    };

    runtime::RuntimeError err;

    // 1. Scene 3-Way Merge Test:
    // - Base: Entities 1, 2, 3
    // - Branch A: Edits Entity 1 (hp=999, position X=50, adds group "alpha")
    // - Branch B: Edits Entity 3 (speed=77, attaches AiBrain) and adds Entity 4
    ScriptRuntime rtBase(compiled.program);
    SceneScriptRuntime scBase(rtBase);
    buildBaseScene(scBase);
    const std::string baseSceneTxt = serializeSceneText(scBase, err, &guidReg);

    ScriptRuntime rtA(compiled.program);
    SceneScriptRuntime scA(rtA);
    RS_REQUIRE(deserializeSceneText(scA, baseSceneTxt, err, &guidReg));
    scA.setLocalPosition2D(1, Vec2{Fixed::fromInt(50), Fixed::fromInt(0)});
    scA.addEntityToGroup(1, "alpha");
    RS_REQUIRE(rtA.setMember(*scA.getComponent(1, "Collab.Game::UnitStats"), "hp", std::int64_t{999}, err));
    const std::string branchASceneTxt = serializeSceneText(scA, err, &guidReg);

    ScriptRuntime rtB(compiled.program);
    SceneScriptRuntime scB(rtB);
    RS_REQUIRE(deserializeSceneText(scB, baseSceneTxt, err, &guidReg));
    RS_REQUIRE(rtB.setMember(*scB.getComponent(3, "Collab.Game::UnitStats"), "speed", std::int64_t{77}, err));
    {
        SceneScriptOptions brainOpts;
        brainOpts.initialMembers["aggroRadius"] = std::int64_t{42};
        RS_REQUIRE(scB.attachComponent(3, "Collab.Game::AiBrain", brainOpts));
        SceneScriptOptions e4Opts;
        e4Opts.initialMembers["hp"] = std::int64_t{500};
        RS_REQUIRE(scB.attachComponent(4, "Collab.Game::UnitStats", e4Opts));
        scB.setEntityName(4, "BossEntity");
    }
    const std::string branchBSceneTxt = serializeSceneText(scB, err, &guidReg);

    const auto quoteShellArg = [](const std::string& arg) -> std::string {
#if defined(_WIN32)
        std::string quoted = "\"";
        for (const char ch : arg) {
            if (ch == '\"') quoted += "\\\"";
            else quoted.push_back(ch);
        }
        quoted.push_back('"');
        return quoted;
#else
        std::string quoted = "'";
        for (const char ch : arg) {
            if (ch == '\'') quoted += "'\\''";
            else quoted.push_back(ch);
        }
        quoted.push_back('\'');
        return quoted;
#endif
    };

    const auto runGitMergeFile = [&](
        const std::string& baseTxt,
        const std::string& oursTxt,
        const std::string& theirsTxt,
        const std::string& prefix) -> std::string {
        const std::filesystem::path tmpDir = std::filesystem::temp_directory_path();
        const std::filesystem::path basePath = tmpDir / (prefix + "_base.txt");
        const std::filesystem::path oursPath = tmpDir / (prefix + "_ours.txt");
        const std::filesystem::path theirsPath = tmpDir / (prefix + "_theirs.txt");
        {
            std::ofstream(basePath, std::ios::binary) << baseTxt;
            std::ofstream(oursPath, std::ios::binary) << oursTxt;
            std::ofstream(theirsPath, std::ios::binary) << theirsTxt;
        }
        const std::string cmd =
            "git merge-file " + quoteShellArg(oursPath.string()) + " " +
            quoteShellArg(basePath.string()) + " " +
            quoteShellArg(theirsPath.string());
        const int rc = std::system(cmd.c_str());
        RS_REQUIRE(rc == 0);
        std::ostringstream ss;
        ss << std::ifstream(oursPath, std::ios::binary).rdbuf();
        std::error_code ec;
        std::filesystem::remove(basePath, ec);
        std::filesystem::remove(oursPath, ec);
        std::filesystem::remove(theirsPath, ec);
        return ss.str();
    };

    const std::string mergedSceneTxt =
        runGitMergeFile(baseSceneTxt, branchASceneTxt, branchBSceneTxt, "rs_eff3_scene");
    RS_REQUIRE(mergedSceneTxt.find("<<<<<<<") == std::string::npos);

    ScriptRuntime rtMerged(compiled.program);
    SceneScriptRuntime scMerged(rtMerged);
    RS_REQUIRE(deserializeSceneText(scMerged, mergedSceneTxt, err, &guidReg));
    RS_REQUIRE(scMerged.getWorldPosition2D(1).x == Fixed::fromInt(50));
    RS_REQUIRE(scMerged.isEntityInGroup(1, "alpha"));
    RS_REQUIRE(getIntMemberOrDie(rtMerged, *scMerged.getComponent(1, "Collab.Game::UnitStats"), "hp") == 999);
    RS_REQUIRE(getIntMemberOrDie(rtMerged, *scMerged.getComponent(2, "Collab.Game::UnitStats"), "hp") == 200);
    RS_REQUIRE(getIntMemberOrDie(rtMerged, *scMerged.getComponent(3, "Collab.Game::UnitStats"), "speed") == 77);
    RS_REQUIRE(getIntMemberOrDie(rtMerged, *scMerged.getComponent(3, "Collab.Game::AiBrain"), "aggroRadius") == 42);
    RS_REQUIRE(getIntMemberOrDie(rtMerged, *scMerged.getComponent(4, "Collab.Game::UnitStats"), "hp") == 500);

    // 2. Prefab 3-Way Merge Test:
    // - Base prefab: Root with children "Armor" and "Weapon"
    // - Branch A: Edits "Armor" hp=800
    // - Branch B: Edits "Weapon" speed=25 and adds child "Sensor" with AiBrain
    PrefabNodeTemplate basePrefab;
    basePrefab.name = "Mech";
    {
        PrefabNodeTemplate armor;
        armor.name = "Armor";
        PrefabComponentTemplate c1;
        c1.scriptType = "Collab.Game::UnitStats";
        c1.options.initialMembers["hp"] = std::int64_t{300};
        armor.components.push_back(std::move(c1));

        PrefabNodeTemplate weapon;
        weapon.name = "Weapon";
        PrefabComponentTemplate c2;
        c2.scriptType = "Collab.Game::UnitStats";
        c2.options.initialMembers["speed"] = std::int64_t{12};
        weapon.components.push_back(std::move(c2));

        basePrefab.children.push_back(std::move(armor));
        basePrefab.children.push_back(std::move(weapon));
    }
    const std::string basePrefabTxt = serializePrefabText(basePrefab, err, &guidReg);

    PrefabNodeTemplate prefabA = basePrefab;
    prefabA.children[0].components[0].options.initialMembers["hp"] = std::int64_t{800};
    const std::string branchAPrefabTxt = serializePrefabText(prefabA, err, &guidReg);

    PrefabNodeTemplate prefabB = basePrefab;
    prefabB.children[1].components[0].options.initialMembers["speed"] = std::int64_t{25};
    {
        PrefabNodeTemplate sensor;
        sensor.name = "Sensor";
        PrefabComponentTemplate cb;
        cb.scriptType = "Collab.Game::AiBrain";
        cb.options.initialMembers["aggroRadius"] = std::int64_t{60};
        sensor.components.push_back(std::move(cb));
        prefabB.children.push_back(std::move(sensor));
    }
    const std::string branchBPrefabTxt = serializePrefabText(prefabB, err, &guidReg);

    const std::string mergedPrefabTxt =
        runGitMergeFile(basePrefabTxt, branchAPrefabTxt, branchBPrefabTxt, "rs_eff3_prefab");
    RS_REQUIRE(mergedPrefabTxt.find("<<<<<<<") == std::string::npos);

    const auto mergedPrefab = deserializePrefabText(mergedPrefabTxt, err, &guidReg, &rtMerged);
    RS_REQUIRE(mergedPrefab.has_value());
    RS_REQUIRE(mergedPrefab->children.size() == 3u);
    RS_REQUIRE(std::get<std::int64_t>(mergedPrefab->children[0].components[0].options.initialMembers.at("hp")) == 800);
    RS_REQUIRE(std::get<std::int64_t>(mergedPrefab->children[1].components[0].options.initialMembers.at("speed")) == 25);
    RS_REQUIRE(mergedPrefab->children[2].name == "Sensor");
}


// ============================================================================
// 4. Track A Cross-Review Regression Suite (P0-5, P2-3c, P1-5, P1-6, P2-3b)
// ============================================================================

void testTrackAP05AndP23cHotReloadSoundness() {
    std::cout << "[RUN] testTrackAP05AndP23cHotReloadSoundness...\n";

    int hostPingCalls = 0;
    GameApi api;
    RS_REQUIRE(api.function(
        "Host.Bridge",
        "Ping",
        [&hostPingCalls](std::int32_t v) -> std::int32_t {
            hostPingCalls += static_cast<int>(v);
            return static_cast<std::int32_t>(hostPingCalls);
        }));
    RS_REQUIRE(api.valid());

    // 1. P0-5: Non-component heap class field reorder & struct field layout mutation MUST abort
    //    in-flight coroutines holding them, AND HotReloadCoroBridge must fall back to runtime_.bindings_
    //    when newProgram.bindings() == nullptr!
    const std::string v1Src = R"RS(
module P05.Reload;
import RealScript.Async;
import Host.Bridge;

public struct InnerData {
    public int a;
    public int b;
}

public class NonCompPayload {
    public int first = 10;
    public int second = 20;
}

public class SafeWorker {
    public int resultVal = 0;
    public void OnStart() { this.Run(); }
    public async Awaitable Run() {
        int seed = 5;
        await Awaitable.Yield();
        this.resultVal = Ping(seed);
    }
}

public class HeapObjWorker {
    public int resultVal = 0;
    public void OnStart() { this.Run(); }
    public async Awaitable Run() {
        NonCompPayload p = new NonCompPayload();
        p.first = 111;
        p.second = 222;
        await Awaitable.Yield();
        this.resultVal = p.first;
    }
}

public class StructWorker {
    public int resultVal = 0;
    public void OnStart() { this.Run(); }
    public async Awaitable Run() {
        InnerData d = new InnerData();
        d.a = 7;
        d.b = 8;
        await Awaitable.Yield();
        this.resultVal = d.a + d.b;
    }
}
)RS";

    const std::string v2Src = R"RS(
module P05.Reload;
import RealScript.Async;
import Host.Bridge;

public struct InnerData {
    public int insertedField;
    public int a;
    public int b;
}

public class NonCompPayload {
    public string ReorderedPrefix = "shifted_offset";
    public int first = 10;
    public int second = 20;
}

public class SafeWorker {
    public int resultVal = 0;
    public void OnStart() { this.Run(); }
    public async Awaitable Run() {
        int seed = 5;
        await Awaitable.Yield();
        this.resultVal = Ping(seed + 10);
    }
}

public class HeapObjWorker {
    public int resultVal = 0;
    public void OnStart() { this.Run(); }
    public async Awaitable Run() {
        NonCompPayload p = new NonCompPayload();
        p.first = 111;
        p.second = 222;
        await Awaitable.Yield();
        this.resultVal = p.first;
    }
}

public class StructWorker {
    public int resultVal = 0;
    public void OnStart() { this.Run(); }
    public async Awaitable Run() {
        InnerData d = new InnerData();
        d.a = 7;
        d.b = 8;
        await Awaitable.Yield();
        this.resultVal = d.a + d.b;
    }
}
)RS";

    auto c1 = compileSourcesOrDie(api, {{compiler::SourceFile{"p05_v1.rscript", v1Src}}}, "p05_v1");
    auto c2 = compileSourcesOrDie(api, {{compiler::SourceFile{"p05_v2.rscript", v2Src}}}, "p05_v2");

    // Verify fingerprint sensitivity to local variable rename as well
    const std::string vRenamedLocal = R"RS(
module P05.Reload;
import RealScript.Async;
import Host.Bridge;
public class SafeWorker {
    public int resultVal = 0;
    public void OnStart() { this.Run(); }
    public async Awaitable Run() {
        int renamedSeed = 5;
        await Awaitable.Yield();
        this.resultVal = Ping(renamedSeed);
    }
}
)RS";
    auto cRenamed = compileSourcesOrDie(
        api, {{compiler::SourceFile{"p05_ren.rscript", vRenamedLocal}}}, "p05_ren");
    const auto fpSafe1 = findCoroutineStateMachineFingerprint(
        *c1.program.program(), "P05.Reload::SafeWorker.Run");
    const auto fpSafeRenamed = findCoroutineStateMachineFingerprint(
        *cRenamed.program.program(), "P05.Reload::SafeWorker.Run");
    RS_REQUIRE(fpSafe1.has_value() && fpSafeRenamed.has_value());
    RS_REQUIRE(!fpSafe1->matchesLayout(*fpSafeRenamed));

    // Construct a GameProgram copy WITHOUT bindings (newProgram.bindings() == nullptr)
    // to verify HotReloadCoroBridge falls back to ScriptRuntime's existing bindings_!
    const GameProgram unboundV2Program = GameProgram::fromProgramImage(
        c2.program.program(),
        nullptr,
        c2.program.languageMetadata());
    RS_REQUIRE(unboundV2Program.bindings() == nullptr);

    ScriptRuntime rt(c1.program);
    SceneScriptRuntime scene(rt);
    RS_REQUIRE(scene.attachComponent(1, "P05.Reload::SafeWorker", {}));
    RS_REQUIRE(scene.attachComponent(2, "P05.Reload::HeapObjWorker", {}));
    RS_REQUIRE(scene.attachComponent(3, "P05.Reload::StructWorker", {}));
    scene.start();
    RS_REQUIRE(rt.asyncRuntime()->coroutines().statistics().activeCount == 3u);

    // Also verify O(capacity) forEachActiveFrame visits exact 3 live frames
    std::size_t visitedActive = 0;
    rt.asyncRuntime()->coroutines().forEachActiveFrame(
        [&visitedActive](runtime::CoroutineFrameHandle, runtime::CoroutineFrame&) {
            ++visitedActive;
        });
    RS_REQUIRE(visitedActive == 3u);

    const auto reloadRep = scene.hotReload(unboundV2Program);
    RS_REQUIRE(reloadRep.succeeded);
    RS_REQUIRE(reloadRep.preservedCoroutines == 1u);
    RS_REQUIRE(reloadRep.abortedCoroutines == 2u);

    scene.update(0.016);
    RS_REQUIRE(scene.errors().empty());
    RS_REQUIRE(getIntMemberOrDie(rt, *scene.getComponent(1, "P05.Reload::SafeWorker"), "resultVal") == 15);
    RS_REQUIRE(getIntMemberOrDie(rt, *scene.getComponent(2, "P05.Reload::HeapObjWorker"), "resultVal") == 0);
    RS_REQUIRE(getIntMemberOrDie(rt, *scene.getComponent(3, "P05.Reload::StructWorker"), "resultVal") == 0);

    // 2. P2-3c: FiberScheduler Call-site return PC shift abort & recycled fiberId ABA immunity
    struct FiberHost final : runtime::IFiberHostCallbacks {
        std::vector<int> trace;
        void onHostCommand(
            runtime::FiberContext&,
            std::uint8_t,
            std::uint16_t,
            std::uint32_t arg32) noexcept override {
            trace.push_back(static_cast<int>(arg32));
        }
    } fHost;

    const std::string fibCallV1 = R"RS(
module AI.Bt;
import RealScript.Fiber;

public void SubTask() {
    Fiber.Yield(2);
    Fiber.Command(1, 0, 10);
}

public void MainRoutine() {
    SubTask();
    Fiber.Command(1, 0, 20);
}
)RS";

    // V2 inserts extra instructions BEFORE SubTask() inside MainRoutine(), shifting Call site PC!
    const std::string fibCallV2 = R"RS(
module AI.Bt;
import RealScript.Fiber;

public void SubTask() {
    Fiber.Yield(2);
    Fiber.Command(1, 0, 10);
}

public void MainRoutine() {
    Fiber.Command(1, 0, 1);
    Fiber.Command(1, 0, 2);
    SubTask();
    Fiber.Command(1, 0, 20);
}
)RS";

    const auto fm1 = compileFiberSourceOrDie(fibCallV1, "fib_call_v1");
    const auto fm2 = compileFiberSourceOrDie(fibCallV2, "fib_call_v2");

    runtime::FiberScheduler sched(&fHost);
    const std::uint32_t pid = sched.registerModule(fm1);
    const std::uint32_t fid = sched.spawnFiberByProgram(
        pid, fm1.entryPc("MainRoutine"), runtime::GenerationalEntityHandle{1, 1});
    RS_REQUIRE(fid != 0u);
    (void)sched.stepTick(1); // MainRoutine calls SubTask(), which sleeps at Yield(2) until tick 3
    RS_REQUIRE(sched.activeFiberCount() == 1u);

    const auto fRep = sched.reloadModuleWithReport(pid, fm1, fm2);
    RS_REQUIRE(fRep.succeeded);
    RS_REQUIRE(fRep.preservedFibers == 0u);
    RS_REQUIRE(fRep.abortedFibers == 1u);

    // P2-3c ABA check: spawn a sleeping fiber (wake at tick 12), kill it, immediately spawn a new
    // fiber reusing the same slot sleeping until tick 25; verify tick 12 does NOT wake it early!
    const std::string fibAbaSrc = R"RS(
module AI.Bt;
import RealScript.Fiber;

public void ShortSleep() {
    Fiber.Yield(2);
    Fiber.Command(1, 0, 111);
}

public void LongSleep() {
    Fiber.Yield(15);
    Fiber.Command(1, 0, 222);
}
)RS";
    const auto fmAba = compileFiberSourceOrDie(fibAbaSrc, "fib_aba");
    const std::uint32_t abaProg = sched.registerModule(fmAba);
    const std::uint32_t fShort = sched.spawnFiberByProgram(
        abaProg, fmAba.entryPc("ShortSleep"), runtime::GenerationalEntityHandle{9, 1});
    (void)sched.stepTick(10); // fShort queued in timing wheel for wakeTick = 12
    sched.killFiberTree(fShort);
    RS_REQUIRE(sched.activeFiberCount() == 0u);

    fHost.trace.clear();
    const std::uint32_t fLong = sched.spawnFiberByProgram(
        abaProg, fmAba.entryPc("LongSleep"), runtime::GenerationalEntityHandle{9, 2});
    RS_REQUIRE(fLong == fShort); // Reused recycled fiberId!
    (void)sched.stepTick(11);    // fLong enters Yield(15) -> wakeTick = 26
    (void)sched.stepTick(12);    // Stale wakeTick=12 entry fires here — MUST NOT wake fLong!
    RS_REQUIRE(fHost.trace.empty());
    for (std::uint32_t t = 13; t <= 26; ++t) {
        (void)sched.stepTick(t);
    }
    RS_REQUIRE(fHost.trace.size() == 1u && fHost.trace[0] == 222);
}

void testTrackAP15LifecycleSafety() {
    std::cout << "[RUN] testTrackAP15LifecycleSafety...\n";

    SceneScriptRuntime* scenePtr = nullptr;
    std::vector<std::string> events;

    GameApi api;
    RS_REQUIRE(
        api.function(
            "Host.P15",
            "Emit",
            [&events](std::string s) -> bool {
                events.push_back(std::move(s));
                return true;
            }) &&
        api.function(
            "Host.P15",
            "DetachCompAOnSelf",
            [&scenePtr]() -> bool {
                return scenePtr && scenePtr->detachComponent(1, "P15.Life::CompA");
            }) &&
        api.function(
            "Host.P15",
            "BurstAttachSiblings",
            [&scenePtr]() -> bool {
                if (!scenePtr) return false;
                for (int i = 0; i < 8; ++i) {
                    (void)scenePtr->attachComponent(
                        50, "P15.Life::PassiveExtra" + std::to_string(i), {});
                }
                return true;
            }) &&
        api.function(
            "Host.P15",
            "ClearWholeSceneMidTick",
            [&scenePtr]() -> bool {
                if (!scenePtr) return false;
                scenePtr->clear();
                (void)scenePtr->attachComponent(99, "P15.Life::CompC", {});
                return true;
            }));
    RS_REQUIRE(api.valid());

    const std::string src = R"RS(
module P15.Life;
import Host.P15;

[DefaultExecutionOrder(-20)]
public class CompA {
    public void OnUpdate(double dt) {
        Emit("A");
    }
}

[DefaultExecutionOrder(0)]
public class CompB {
    public bool detachA = false;
    public bool resetScene = false;
    public void OnUpdate(double dt) {
        Emit("B");
        if (this.detachA) {
            this.detachA = false;
            DetachCompAOnSelf();
        }
        if (this.resetScene) {
            this.resetScene = false;
            ClearWholeSceneMidTick();
        }
    }
}

[DefaultExecutionOrder(20)]
public class CompC {
    public void OnUpdate(double dt) {
        Emit("C");
    }
}

public class PassiveExtra0 { public int v = 0; }
public class PassiveExtra1 { public int v = 1; }
public class PassiveExtra2 { public int v = 2; }
public class PassiveExtra3 { public int v = 3; }
public class PassiveExtra4 { public int v = 4; }
public class PassiveExtra5 { public int v = 5; }
public class PassiveExtra6 { public int v = 6; }
public class PassiveExtra7 { public int v = 7; }

public class ReentrantCreator {
    public void OnCreate() {
        Emit("Create");
        BurstAttachSiblings();
    }
    public void OnEnable() {
        Emit("Enable");
    }
}
)RS";

    auto compiled = compileSourcesOrDie(api, {{compiler::SourceFile{"p15.rscript", src}}}, "p15");
    ScriptRuntime rt(compiled.program);
    SceneScriptRuntime scene(rt);
    scenePtr = &scene;

    // 1. Reentrant attachComponent inside OnCreate reallocating instances_[50] must NOT UAF!
    RS_REQUIRE(scene.attachComponent(50, "P15.Life::ReentrantCreator", {}));
    RS_REQUIRE(events.size() == 2u && events[0] == "Create" && events[1] == "Enable");
    RS_REQUIRE(scene.getComponents(50).size() == 9u);

    // 2. Same-entity detachComponent of earlier sibling CompA during CompB.OnUpdate must:
    //    - NOT double-call CompA
    //    - NOT skip CompC (whose vector index shifted from 2 -> 1)!
    events.clear();
    RS_REQUIRE(scene.attachComponent(1, "P15.Life::CompA", {}));
    RS_REQUIRE(scene.attachComponent(1, "P15.Life::CompB", {}));
    RS_REQUIRE(scene.attachComponent(1, "P15.Life::CompC", {}));
    scene.start();
    scene.update(0.016); // Warm flat cache
    events.clear();

    runtime::RuntimeError err;
    RS_REQUIRE(rt.setMember(*scene.getComponent(1, "P15.Life::CompB"), "detachA", true, err));
    scene.update(0.016);
    RS_REQUIRE(scene.errors().empty());
    RS_REQUIRE(events.size() == 3u);
    RS_REQUIRE(events[0] == "A" && events[1] == "B" && events[2] == "C");

    // 3. Whole-scene replacement (scene.clear()) mid-update must immediately abort old frame cursor!
    events.clear();
    RS_REQUIRE(rt.setMember(*scene.getComponent(1, "P15.Life::CompB"), "resetScene", true, err));
    scene.update(0.016);
    RS_REQUIRE(scene.errors().empty());
    RS_REQUIRE(events.size() == 1u && events[0] == "B");
}

void testTrackAP16AndP23bSerializationFixes() {
    std::cout << "[RUN] testTrackAP16AndP23bSerializationFixes...\n";

    auto host = std::make_shared<GameplayHost>(60);
    GameApi api;
    RS_REQUIRE(installGameplayBindings(api, host));
    RS_REQUIRE(api.valid());

    const std::string src = R"RS(
module P16.Ser;

public struct Vec2 {
    public float x;
    public float y;
}

public class Enemy {
    public string title = "grunt";
    public Enemy next;
}

public class Holder {
    public string[] emptyStrings;
    public float[] emptyFloats;
    public bool[] emptyBools;
    public Vec2[] emptyVec2s;
    public Enemy[] emptyEnemies;
    public string ordinaryText = "";
    [AssetRef]
    public string assetRefPath = "";
    public Enemy dagLeft;
    public Enemy dagRight;
    public Enemy ringHead;

    public void InitEmptyAndGraphs() {
        this.emptyStrings = new string[0];
        this.emptyFloats = new float[0];
        this.emptyBools = new bool[0];
        this.emptyVec2s = new Vec2[0];
        this.emptyEnemies = new Enemy[0];

        Enemy shared = new Enemy();
        shared.title = "shared_dag_node";
        this.dagLeft = shared;
        this.dagRight = shared;

        Enemy r1 = new Enemy();
        Enemy r2 = new Enemy();
        r1.title = "ring_1";
        r2.title = "ring_2";
        r1.next = r2;
        r2.next = r1;
        this.ringHead = r1;
    }
}
)RS";

    auto compiled = compileSourcesOrDie(api, {{compiler::SourceFile{"p16.rscript", src}}}, "p16");
    AssetGuidRegistry guidReg;
    guidReg.populateFromProgram(compiled.program);
    const AssetGuid regGuid = guidReg.registerAssetPath("assets/configs/shared_item.cfg");

    ScriptRuntime rt1(compiled.program);
    SceneScriptRuntime sc1(rt1);
    RS_REQUIRE(sc1.attachComponent(1, "P16.Ser::Holder", {}));
    sc1.start();
    RS_REQUIRE(sc1.invoke(1, "InitEmptyAndGraphs", {}).succeeded);

    auto* holder1 = sc1.getComponent(1, "P16.Ser::Holder");
    RS_REQUIRE(holder1 != nullptr);
    runtime::RuntimeError err;
    // Set BOTH ordinaryText and [AssetRef] assetRefPath to the exact registered asset path!
    RS_REQUIRE(rt1.setMember(
        *holder1, "ordinaryText", std::string("assets/configs/shared_item.cfg"), err));
    RS_REQUIRE(rt1.setMember(
        *holder1, "assetRefPath", std::string("assets/configs/shared_item.cfg"), err));

    // 1. JSON round-trip of non-int empty arrays

    const std::string jsonSnap = exportSceneJson(sc1, err);
    RS_REQUIRE(err.code == runtime::ErrorCode::None && !jsonSnap.empty());
    ScriptRuntime rtJson(compiled.program);
    SceneScriptRuntime scJson(rtJson);
    RS_REQUIRE(importSceneJson(scJson, jsonSnap, err));
    for (const char* arrField : {"emptyStrings", "emptyFloats", "emptyBools", "emptyVec2s", "emptyEnemies"}) {
        const auto av = rtJson.getMember(*scJson.getComponent(1, "P16.Ser::Holder"), arrField, err);
        RS_REQUIRE(av.has_value() && std::holds_alternative<runtime::ObjectRef>(*av));
        const auto len = rtJson.heap()->arrayLength(std::get<runtime::ObjectRef>(*av));
        RS_REQUIRE(len.has_value() && *len == 0u);
    }


    // 2. Text scene serialization: verify ordinary string is `str:` (NOT `asset_path:`),
    //    `[AssetRef]` string IS `asset_path:`, empty arrays preserve element types,
    //    shared DAG preserves identity, and cycle round-trips without becoming null_obj!
    const std::string sceneTxt = serializeSceneText(sc1, err, &guidReg);
    RS_REQUIRE(err.code == runtime::ErrorCode::None && !sceneTxt.empty());
    RS_REQUIRE(sceneTxt.find(".field.ordinaryText = str:\"assets/configs/shared_item.cfg\"") != std::string::npos);
    RS_REQUIRE(sceneTxt.find(".field.assetRefPath = asset_path:") != std::string::npos);
    RS_REQUIRE(sceneTxt.find("obj_ref:") != std::string::npos);

    // Rebind the asset path in AssetGuidRegistry: ordinaryText MUST NOT change, assetRefPath MUST update!
    RS_REQUIRE(guidReg.rebindAssetPath(regGuid, "assets/configs/relocated_v2.cfg"));

    ScriptRuntime rt2(compiled.program);
    SceneScriptRuntime sc2(rt2);
    RS_REQUIRE(deserializeSceneText(sc2, sceneTxt, err, &guidReg));
    auto* holder2 = sc2.getComponent(1, "P16.Ser::Holder");
    RS_REQUIRE(holder2 != nullptr);

    RS_REQUIRE(getStringMemberOrDie(rt2, *holder2, "ordinaryText") == "assets/configs/shared_item.cfg");
    RS_REQUIRE(getStringMemberOrDie(rt2, *holder2, "assetRefPath") == "assets/configs/relocated_v2.cfg");

    for (const char* arrField : {"emptyStrings", "emptyFloats", "emptyBools", "emptyVec2s", "emptyEnemies"}) {
        const auto av = rt2.getMember(*holder2, arrField, err);
        RS_REQUIRE(av.has_value() && std::holds_alternative<runtime::ObjectRef>(*av));
        const auto arrRef = std::get<runtime::ObjectRef>(*av);
        const auto len = rt2.heap()->arrayLength(arrRef);
        RS_REQUIRE(len.has_value() && *len == 0u);
    }
    RS_REQUIRE(rt2.heap()->arrayElementType(
        std::get<runtime::ObjectRef>(*rt2.getMember(*holder2, "emptyStrings", err))) ==
        semantic::PrimitiveType::String);
    RS_REQUIRE(rt2.heap()->arrayElementType(
        std::get<runtime::ObjectRef>(*rt2.getMember(*holder2, "emptyFloats", err))) ==
        semantic::PrimitiveType::Float);
    RS_REQUIRE(rt2.heap()->arrayElementType(
        std::get<runtime::ObjectRef>(*rt2.getMember(*holder2, "emptyBools", err))) ==
        semantic::PrimitiveType::Bool);
    RS_REQUIRE(rt2.heap()->arrayElementType(
        std::get<runtime::ObjectRef>(*rt2.getMember(*holder2, "emptyVec2s", err))) ==
        semantic::PrimitiveType::Struct);
    RS_REQUIRE(rt2.heap()->arrayElementType(
        std::get<runtime::ObjectRef>(*rt2.getMember(*holder2, "emptyEnemies", err))) ==
        semantic::PrimitiveType::Object);

    // Shared DAG identity check: dagLeft and dagRight point to the EXACT SAME ObjectRef!
    const auto dagL = std::get<runtime::ObjectRef>(*rt2.getMember(*holder2, "dagLeft", err));
    const auto dagR = std::get<runtime::ObjectRef>(*rt2.getMember(*holder2, "dagRight", err));
    RS_REQUIRE(dagL.valid() && dagL == dagR);

    // Cyclic graph check: ringHead -> next -> next == ringHead!
    const auto ring1 = std::get<runtime::ObjectRef>(*rt2.getMember(*holder2, "ringHead", err));
    RS_REQUIRE(ring1.valid());
    const auto enemyType = rt2.findType("P16.Ser::Enemy");
    RS_REQUIRE(enemyType.has_value());
    std::size_t nextIdx = 0;
    for (const auto& f : enemyType->descriptor.fields) {
        if (f.name == "next") nextIdx = f.index;
    }
    const auto ring2Val = rt2.heap()->fieldGet(ring1, nextIdx);
    RS_REQUIRE(ring2Val.has_value() && std::holds_alternative<runtime::ObjectRef>(*ring2Val));
    const auto ring2 = std::get<runtime::ObjectRef>(*ring2Val);
    RS_REQUIRE(ring2.valid() && ring2 != ring1);
    const auto backVal = rt2.heap()->fieldGet(ring2, nextIdx);
    RS_REQUIRE(backVal.has_value() && std::holds_alternative<runtime::ObjectRef>(*backVal));
    RS_REQUIRE(std::get<runtime::ObjectRef>(*backVal) == ring1);

    // Deterministic re-serialization stability even with DAG + cycle!
    const std::string sceneTxt2 = serializeSceneText(sc2, err, &guidReg);
    ScriptRuntime rt3(compiled.program);
    SceneScriptRuntime sc3(rt3);
    RS_REQUIRE(deserializeSceneText(sc3, sceneTxt2, err, &guidReg));
    RS_REQUIRE(serializeSceneText(sc3, err, &guidReg) == sceneTxt2);

    // 3. Prefab initialMembers keys containing '.', '=', and '[' round-trip cleanly!
    PrefabNodeTemplate pRoot;
    pRoot.name = "EscapedPrefab";
    PrefabComponentTemplate pComp;
    pComp.scriptType = "P16.Ser::Holder";
    pComp.options.initialMembers["ordinaryText"] = std::string("assets/configs/relocated_v2.cfg");
    pComp.options.initialMembers["nested.dotted.key"] = std::int64_t{123};
    pComp.options.initialMembers["key=with=equals"] = std::string("eq_val");
    pComp.options.initialMembers["slot[0].override"] = true;
    pRoot.components.push_back(std::move(pComp));

    const std::string pTxt = serializePrefabText(pRoot, err, &guidReg, &rt2);
    RS_REQUIRE(err.code == runtime::ErrorCode::None && !pTxt.empty());
    RS_REQUIRE(pTxt.find(".member.ordinaryText = str:") != std::string::npos);

    const auto pDecoded = deserializePrefabText(pTxt, err, &guidReg, &rt2);
    RS_REQUIRE(pDecoded.has_value() && pDecoded->components.size() == 1u);
    const auto& im = pDecoded->components[0].options.initialMembers;
    RS_REQUIRE(std::get<std::int64_t>(im.at("nested.dotted.key")) == 123);
    RS_REQUIRE(std::get<runtime::StringValue>(im.at("key=with=equals")) == "eq_val");
    RS_REQUIRE(std::get<bool>(im.at("slot[0].override")) == true);
}

} // namespace

int main() {
    try {
        testPerf4SteadyStateFlattenedHotPath();
        testPerf4MidCallbackMutationsAndReentrancy();
        testEff1FingerprintDeterminismAndSensitivity();
        testEff1AsyncCoroutinePreservationAndChainedHotReload();
        testEff1SelectivePreserveAndSafeAbortConcurrentCoroutines();
        testEff1SequenceAndFiberSelectiveHotReload();
        testEff3AssetGuidAndRegistryRefactoring();
        testEff3SceneAndPrefabGuidRenameAndRoundTrip();
        testEff3GitThreeWayMergeConflictFree();
        testTrackAP05AndP23cHotReloadSoundness();
        testTrackAP15LifecycleSafety();
        testTrackAP16AndP23bSerializationFixes();

        std::cout << "[PASS] All sprint2_scene_hotreload_guid_tests passed!\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[FAIL] " << ex.what() << "\n";
        return 1;
    }
}
