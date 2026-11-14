#include "realscript/AbiGovernance.h"
#include "realscript/bytecode/Bytecode.h"
#include "realscript/game/DeterministicMath.h"
#include "realscript/game/GameBindings.h"
#include "realscript/game/GameProductization.h"
#include "realscript/game/GameScripting.h"
#include "realscript/game/GameplayMetadata.h"
#include "realscript/game/GameplayPrimitives.h"
#include "realscript/game/GameplayScripting.h"
#include "realscript/game/GameplayStateCodec.h"
#include "realscript/game/ResourceHandle.h"
#include "realscript/game/RsBindParser.h"
#include "realscript/runtime/AsyncRuntime.h"
#include "realscript/text/AssetGuid.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using namespace realscript;

[[noreturn]] void failCheck(const char* expr, int line) noexcept {
    std::cerr << "Check failed at app_game_sdk_and_scene_tests.cpp:" << line << ": " << expr << std::endl;
    std::exit(1);
}

#define RS_CHECK(...)                                                                              \
    do {                                                                                           \
        if (!(__VA_ARGS__)) {                                                                      \
            failCheck(#__VA_ARGS__, __LINE__);                                                     \
        }                                                                                          \
    } while (false)

struct NativeDamageSpec {
    std::int32_t baseDamage = 0;
    std::int32_t bonus = 0;
};

struct NativeWeapon {
    std::int32_t damage = 15;
    bool destroyed = false;

    explicit NativeWeapon(std::int32_t initial = 15) : damage(initial) {}

    std::int32_t strike(std::int32_t mult) const {
        return damage * mult;
    }

    std::int32_t getDamage() const { return damage; }
    void setDamage(std::int32_t v) { damage = v; }
};

class DemoTexture final : public game::HostResource {
public:
    explicit DemoTexture(std::atomic<int>& disposedCounter)
        : disposedCounter_(disposedCounter) {}

protected:
    void onDispose() override {
        disposedCounter_.fetch_add(1, std::memory_order_relaxed);
    }

private:
    std::atomic<int>& disposedCounter_;
};

game::GameCompileResult compileGameScript(
    const game::GameApi& api,
    const std::vector<compiler::SourceFile>& sources) {
    game::GameScriptCompiler compiler(api);
    auto res = compiler.compile(sources);
    if (!res.succeeded()) {
        for (const auto& d : res.diagnostics.items()) {
            std::cerr << "GameSdk compile error [" << d.code << "]: " << d.message << "\n";
        }
    }
    RS_CHECK(res.succeeded());
    return res;
}

void testGameApiBindingsAndResourceHandles() {
    game::GameApi api;
    api.structType<NativeDamageSpec>("Game.Native", "DamageSpec")
        .field<&NativeDamageSpec::baseDamage>("baseDamage")
        .field<&NativeDamageSpec::bonus>("bonus");

    api.type<NativeWeapon>("Game.Native", "Weapon")
        .constructor<std::int32_t>("Create")
        .method<&NativeWeapon::strike>("strike")
        .property<&NativeWeapon::getDamage, &NativeWeapon::setDamage>("damage");

    api.function("Game.Native", "scaleDamage", [](NativeDamageSpec spec, std::int32_t factor) {
        return (spec.baseDamage + spec.bonus) * factor;
    });

    RS_CHECK(api.valid());
    auto stubs = api.exportDeclarationStubs();
    RS_CHECK(!stubs.empty());

    std::vector<compiler::SourceFile> sources;
    sources.push_back({
        "combat.rscript",
        R"(module Game.Combat;
import Game.Native;

public class CombatRunner
{
    public static int runAttack(int baseVal)
    {
        var spec = new DamageSpec();
        spec.baseDamage = baseVal;
        spec.bonus = 5;
        int scaled = scaleDamage(spec, 2);

        var blade = Weapon.Create(20);
        blade.damage = blade.damage + scaled;
        return blade.strike(2);
    }
}
)",
    });

    const auto compiled = compileGameScript(api, sources);
    game::ScriptRuntime rt(compiled.program);
    const auto runnerType = rt.findType("Game.Combat::CombatRunner");
    RS_CHECK(runnerType.has_value());
    const auto fn = rt.findMethod(*runnerType, "runAttack", 1);
    RS_CHECK(fn.has_value());
    const auto res = rt.invokeStatic(*fn, {runtime::Value{std::int64_t{10}}});
    RS_CHECK(res.succeeded);
    // (10 + 5) * 2 = 30; blade.damage = 20 + 30 = 50; strike(2) = 100
    RS_CHECK(std::get<std::int64_t>(res.value) == 100);

    // HostResource & ResourceHandle<T> deterministic refcounting and release.
    std::atomic<int> disposedCount{0};
    game::ResourceRegistry reg;
    {
        auto h1 = reg.emplace<DemoTexture>(
            "res://Textures/Hero.png",
            text::AssetGuid::fromAssetPath("res://Textures/Hero.png").toString(),
            disposedCount);
        RS_CHECK(h1.isValid());
        RS_CHECK(h1.useCount() == 1);
        auto h2 = h1.retain();
        RS_CHECK(h2.isValid());
        RS_CHECK(h1.useCount() == 2);
        h2.dispose();
        RS_CHECK(!h2.isValid());
        RS_CHECK(disposedCount.load() == 0);
        RS_CHECK(h1.useCount() == 1);
    }
    RS_CHECK(disposedCount.load() == 1);

    std::cout << "[PASS] GameApi typed bindings & ResourceHandle refcount lifecycle\n";
}

void testRsBindParserPreprocessorAndParity() {
    const std::string header = R"(
#pragma once

RS_EXPORT_ENUM("Game.SdkBind")
enum class CombatMode {
    Melee = 0,
    Ranged = 1,
};

RS_EXPORT_STRUCT("Game.SdkBind")
struct HitInfo {
    RS_EXPORT_FIELD int amount;
    RS_EXPORT_FIELD double knockback;
};

#ifdef ENABLE_EXPERIMENTAL_ABILITY
RS_EXPORT_CLASS("Game.SdkBind")
class ExperimentalAbility {
public:
    RS_EXPORT_CONSTRUCTOR() ExperimentalAbility();
    RS_EXPORT_METHOD int power() const;
};
#else
RS_EXPORT_CLASS("Game.SdkBind")
class StandardAbility {
public:
    RS_EXPORT_CONSTRUCTOR() StandardAbility();
    RS_EXPORT_METHOD int power() const;
};
#endif

#if 0
RS_EXPORT_FUNCTION("Game.SdkBind")
int deadBranchHelper(int x);
#endif

RS_EXPORT_FUNCTION("Game.SdkBind")
int liveBranchHelper(int x);
)";

    const auto offMacro = game::GameApi::generateBindingsFromCppHeader(header, "Game.SdkBind", {});
    const auto offParity = game::rsbind_detail::generateFromParsedHeader(header, "Game.SdkBind", {});
    RS_CHECK(offMacro.scriptDeclarationStub == offParity.scriptDeclarationStub);
    RS_CHECK(offMacro.cppRegistrationSource == offParity.cppRegistrationSource);
    RS_CHECK(offMacro.enumsExported == 1);
    RS_CHECK(offMacro.structsExported == 1);
    RS_CHECK(offMacro.classesExported == 1);
    RS_CHECK(offMacro.functionsExported == 1);
    RS_CHECK(offMacro.scriptDeclarationStub.find("StandardAbility") != std::string::npos);
    RS_CHECK(offMacro.scriptDeclarationStub.find("ExperimentalAbility") == std::string::npos);
    RS_CHECK(offMacro.scriptDeclarationStub.find("deadBranchHelper") == std::string::npos);
    RS_CHECK(offMacro.scriptDeclarationStub.find("liveBranchHelper") != std::string::npos);

    const std::unordered_map<std::string, std::string> macros{{"ENABLE_EXPERIMENTAL_ABILITY", "1"}};
    const auto onMacro = game::GameApi::generateBindingsFromCppHeader(header, "Game.SdkBind", macros);
    RS_CHECK(onMacro.classesExported == 1);
    RS_CHECK(onMacro.scriptDeclarationStub.find("ExperimentalAbility") != std::string::npos);
    RS_CHECK(onMacro.scriptDeclarationStub.find("StandardAbility") == std::string::npos);

    std::cout << "[PASS] RsBindParser C++ header binding generation, #if filtering & parity\n";
}

void testPackageLoaderAbiAndGameplayStateCodec() {
    game::GameApi api;
    api.function("Game.Abi", "hostTickMul", [](std::int32_t x) { return x * 4; });

    std::vector<compiler::SourceFile> sources;
    sources.push_back({
        "pkg_entry.rscript",
        R"(module Game.Pkg;
import Game.Abi;
public class PkgRunner {
    public static int runPkg(int v) { return hostTickMul(v) + 1; }
}
)",
    });

    const auto compiled = compileGameScript(api, sources);
    std::vector<std::vector<std::uint8_t>> encodedModules;
    encodedModules.reserve(compiled.modules.size());
    for (const auto& m : compiled.modules) {
        encodedModules.push_back(bytecode::encodeModule(m));
    }

    const auto abiManifest = game::buildPackageAbiManifest(compiled.modules);
    const auto preflight = game::preflightBytecodePackageHeaders(encodedModules);
    RS_CHECK(preflight.canLoad());

    game::GameProgramLoader loader(api);
    const auto loaded = loader.loadBytecodeModules(encodedModules, abiManifest);
    RS_CHECK(loaded.succeeded());
    RS_CHECK(loaded.package.valid());

    const auto abiReport = game::verifyPackageAbiCompatibility(loaded.package);
    RS_CHECK(abiReport.canLoad());

    const auto envelope = game::buildPackageContractEnvelope(loaded.package);
    const auto envJson = envelope.toJson();
    const auto parsedEnv = game::GamePackageContractEnvelope::fromJson(envJson);
    RS_CHECK(parsedEnv.has_value());
    const auto envCheck = game::verifyPackageContractEnvelope(
        *parsedEnv, &api, loaded.package.programContentHash);
    RS_CHECK(envCheck.canLoad());

    auto loadedRt = loaded.package.createRuntime();
    const auto pkgType = loadedRt.findType("Game.Pkg::PkgRunner");
    RS_CHECK(pkgType.has_value());
    const auto fn = loadedRt.findMethod(*pkgType, "runPkg", 1);
    RS_CHECK(fn.has_value());
    const auto inv = loadedRt.invokeStatic(*fn, {runtime::Value{std::int64_t{7}}});
    RS_CHECK(inv.succeeded);
    RS_CHECK(std::get<std::int64_t>(inv.value) == 29);

    // Deterministic entities, clocks, RNG, scheduler, events + .rsgs GameplayStateCodec.
    game::GameplayHost hostA(60, 0x1234ULL, 0x5678ULL);
    const auto e1 = hostA.core().entities().create();
    RS_CHECK(hostA.core().entities().alive(e1));
    (void)hostA.core().random().nextInt(1, 100);
    (void)hostA.scheduleCall(10u, "onBeat", 3u, {game::GameplayValue{std::int64_t{42}}});
    (void)hostA.subscribe("quest.updated", 10u, "onQuest");
    (void)hostA.publish("quest.updated", {game::GameplayValue{std::int64_t{99}}}, 3u);

    const std::uint64_t digestBefore = hostA.core().stableHash();
    game::GameplayStateError codecErr;
    const auto rsgsBytes = game::encodeGameplayHostState(hostA, codecErr);
    RS_CHECK(!codecErr.failed() && !rsgsBytes.empty());

    game::GameplayHost hostB(60, 1ULL, 2ULL);
    RS_CHECK(game::restoreGameplayHostState(hostB, rsgsBytes, codecErr));
    RS_CHECK(!codecErr.failed());
    RS_CHECK(hostB.core().stableHash() == digestBefore);

    const auto batchA = hostA.drainDue(3u);
    const auto batchB = hostB.drainDue(3u);
    RS_CHECK(batchA.calls.size() == batchB.calls.size());
    RS_CHECK(!batchA.calls.empty());
    RS_CHECK(hostA.core().random().nextInt(0, 100000) == hostB.core().random().nextInt(0, 100000));

    // Corrupt / truncated .rsgs stream must be cleanly rejected.
    std::vector<std::uint8_t> corrupt(rsgsBytes.begin(), rsgsBytes.begin() + 8);
    game::GameplayStateError badErr;
    RS_CHECK(!game::decodeGameplayHostState(corrupt, badErr).has_value());
    RS_CHECK(badErr.failed());

    std::cout << "[PASS] GameProgramLoader, ABI envelope & .rsgs GameplayStateCodec\n";
}

void testSceneLifecycleEventsTriggersAndAsyncCancellation() {
    game::GameApi api;
    const std::string script = R"(module Game.SceneFlow;

public delegate void PingHandler(int bonus);

public class AgentComponent {
    public event PingHandler ping;
    public int starts = 0;
    public int updates = 0;
    public int fixedUpdates = 0;
    public int signals = 0;
    public int triggers = 0;

    public void OnStart() { starts = starts + 1; }
    public void OnFixedUpdate(double dt) { fixedUpdates = fixedUpdates + 1; }
    public void OnUpdate(double dt) { updates = updates + 1; }
    public void onPing(int bonus) { signals = signals + bonus; }
    public void onTriggered() { triggers = triggers + 1; }
}
)";

    const auto compiled = compileGameScript(api, {{"scene_flow.rscript", script}});
    game::ScriptRuntime rt(compiled.program);
    rt.setAsyncRuntime(std::make_shared<runtime::AsyncRuntimeContext>());
    game::SceneScriptRuntime scene(rt);

    const auto e1 = scene.spawnGenerationalEntity("Hero");
    RS_CHECK(scene.isEntityAlive(e1));
    RS_CHECK(!scene.isStaleGenerationalEntity(e1));
    RS_CHECK(scene.attachComponent(e1, "Game.SceneFlow::AgentComponent"));

    RS_CHECK(scene.connectSignal(e1, "Game.SceneFlow::AgentComponent", "ping",
                               e1, "Game.SceneFlow::AgentComponent", "onPing"));

    bool conditionArmed = false;
    RS_CHECK(scene.addTrigger(game::ScriptTrigger{
        "low_hp",
        e1,
        "onTriggered",
        {},
        [&conditionArmed]() { return conditionArmed; },
        true,
        true,
        false,
    }));

    scene.start();
    scene.fixedUpdate(1.0 / 60.0);
    scene.update(0.016);
    scene.lateUpdate(0.016);
    RS_CHECK(scene.evaluateTriggers() == 0);

    conditionArmed = true;
    RS_CHECK(scene.evaluateTriggers() == 1);
    RS_CHECK(scene.emitSignal(e1, "ping", {runtime::Value{std::int64_t{5}}}) == 1);

    const auto* obj = scene.getComponent(e1, "Game.SceneFlow::AgentComponent");
    RS_CHECK(obj != nullptr);
    runtime::RuntimeError memErr;
    const auto starts = rt.getMember(*obj, "starts", memErr);
    const auto fixed = rt.getMember(*obj, "fixedUpdates", memErr);
    const auto updates = rt.getMember(*obj, "updates", memErr);
    const auto signals = rt.getMember(*obj, "signals", memErr);
    const auto triggers = rt.getMember(*obj, "triggers", memErr);
    RS_CHECK(std::get<std::int64_t>(*starts) == 1);
    RS_CHECK(std::get<std::int64_t>(*fixed) == 1);
    RS_CHECK(std::get<std::int64_t>(*updates) == 1);
    RS_CHECK(std::get<std::int64_t>(*signals) == 5);
    RS_CHECK(std::get<std::int64_t>(*triggers) == 1);

    // Async request completion + entityDestroyToken automatic cancellation upon entity destruction.
    const auto reqId = scene.createAsyncRequest("load_save");
    RS_CHECK(scene.waitAsyncRequest(reqId, e1, "onTriggered", "Game.SceneFlow::AgentComponent"));
    RS_CHECK(scene.completeAsyncRequest(reqId, runtime::Value{std::int64_t{3}}));
    RS_CHECK(scene.flushCompletedAsyncRequests() == 1);
    const auto triggersAfterAsync = rt.getMember(*obj, "triggers", memErr);
    RS_CHECK(std::get<std::int64_t>(*triggersAfterAsync) == 2);

    const auto token = scene.entityDestroyToken(e1);
    RS_CHECK(scene.hasEntityDestroyToken(e1));
    RS_CHECK(!rt.asyncRuntime()->cancellations().isCancellationRequested(token));

    scene.queueDestroyEntity(e1);
    RS_CHECK(scene.pendingDestroyCount() == 1);
    RS_CHECK(scene.flushDestroyedEntities() >= 1);
    RS_CHECK(scene.isStaleGenerationalEntity(e1));
    RS_CHECK(rt.asyncRuntime()->cancellations().isCancellationRequested(token));

    std::cout << "[PASS] SceneScriptRuntime lifecycle, triggers, deferred destroy & async token cancel\n";
}

void testHotReloadGuidsSerializationAndRemoteDebug() {
    game::GameApi api;
    const std::string v1Src = R"(module Game.Reload;

public class PlayerStats {
    [Range(min = 0, max = 500)]
    public int oldHp = 100;
    public int level = 1;
}
)";
    const std::string v2Src = R"(module Game.Reload;

public class PlayerStats {
    [FormerlySerializedAs(name = "oldHp")]
    [Range(min = 0, max = 500)]
    public int health = 50;
    public int level = 1;
    public int shield = 25;
}
)";

    const auto c1 = compileGameScript(api, {{"stats_v1.rscript", v1Src}});
    game::ScriptRuntime rt(c1.program);
    game::SceneScriptRuntime scene(rt);

    const auto e1 = scene.spawnGenerationalEntity("StatsHolder");
    RS_CHECK(scene.attachComponent(e1, "Game.Reload::PlayerStats"));
    RS_CHECK(scene.setInspectorMember(
        e1, "Game.Reload::PlayerStats", "oldHp", runtime::Value{std::int64_t{275}}));

    game::ScriptMetadataRegistry metaV1;
    (void)metaV1.populateFromLanguageMetadata(c1.languageMetadata);
    const auto schemaV1 = game::inspectTypeSchema(rt, metaV1, "Game.Reload::PlayerStats");
    RS_CHECK(schemaV1.has_value());
    RS_CHECK(!schemaV1->fields.empty());

    // AssetGuidRegistry + Scene text round-trip.
    game::AssetGuidRegistry guidReg;
    const auto scriptGuid = guidReg.registerType("Game.Reload::PlayerStats");
    const auto resGuid = guidReg.registerAssetPath("res://Prefabs/Hero.prefab");
    RS_CHECK(!scriptGuid.toString().empty() && !resGuid.toString().empty());

    runtime::RuntimeError serErr;
    const std::string sceneText = game::serializeSceneText(scene, serErr, &guidReg);
    RS_CHECK(serErr.code == runtime::ErrorCode::None);
    RS_CHECK(sceneText.find(scriptGuid.toString()) != std::string::npos);

    game::SceneScriptRuntime restoredScene(rt);
    RS_CHECK(game::deserializeSceneText(restoredScene, sceneText, serErr, &guidReg));

    // Hot-reload v1 -> v2 with [FormerlySerializedAs("oldHp")] preserving oldHp -> health = 275.
    const auto c2 = compileGameScript(api, {{"stats_v2.rscript", v2Src}});
    game::ScriptMetadataRegistry metaV2;
    (void)metaV2.populateFromLanguageMetadata(c2.languageMetadata);
    const auto reloadReport = scene.hotReload(c2.program, &metaV2);
    RS_CHECK(reloadReport.succeeded);

    const auto* migrated = scene.getComponent(e1, "Game.Reload::PlayerStats");
    RS_CHECK(migrated != nullptr);
    runtime::RuntimeError readErr;
    const auto healthVal = rt.getMember(*migrated, "health", readErr);
    RS_CHECK(healthVal.has_value() && std::get<std::int64_t>(*healthVal) == 275);
    const auto shieldVal = rt.getMember(*migrated, "shield", readErr);
    RS_CHECK(shieldVal.has_value() && std::get<std::int64_t>(*shieldVal) == 25);

    // Remote debugger build toggle contract (#if REALSCRIPT_HAS_DEBUG_SERVICES).
#if REALSCRIPT_HAS_DEBUG_SERVICES
    RS_CHECK(rt.startRemoteDebugger(0, "127.0.0.1"));
    RS_CHECK(rt.remoteDebuggerPort() > 0);
    rt.stopRemoteDebugger();
    RS_CHECK(rt.remoteDebuggerPort() == 0);
#else
    RS_CHECK(!rt.startRemoteDebugger(0, "127.0.0.1"));
    RS_CHECK(rt.remoteDebuggerPort() == 0);
#endif

    std::cout << "[PASS] Hot-reload [FormerlySerializedAs], GUID serialization, Inspector & Debug toggle\n";
}

void testSceneHotReloadFieldTagsAndGameplayCodecMigration() {
    game::GameApi api;
    const std::string v1Src = R"(module Game.SchemaEvol;

public class HeroStats {
    [FieldTag(11)]
    public int hp = 100;

    public int legacyMana = 40;
}
)";
    const std::string v2Src = R"(module Game.SchemaEvol;

public class HeroStats {
    [FormerlySerializedAs("legacyMana")]
    [FormerlySerializedAs("manaPool")]
    public int mp = 5;

    [FieldTag(11)]
    public int currentHp = 10;

    public int stamina = 55;
}
)";

    const auto c1 = compileGameScript(api, {{"evol_v1.rscript", v1Src}});
    game::ScriptRuntime rt(c1.program);
    game::SceneScriptRuntime scene(rt);

    const auto hero = scene.spawnGenerationalEntity("Hero");
    RS_CHECK(scene.attachComponent(hero, "Game.SchemaEvol::HeroStats"));
    RS_CHECK(scene.setInspectorMember(
        hero, "Game.SchemaEvol::HeroStats", "hp", runtime::Value{std::int64_t{350}}));
    RS_CHECK(scene.setInspectorMember(
        hero, "Game.SchemaEvol::HeroStats", "legacyMana", runtime::Value{std::int64_t{75}}));

    const auto c2 = compileGameScript(api, {{"evol_v2.rscript", v2Src}});
    game::ScriptMetadataRegistry metaV2;
    (void)metaV2.populateFromLanguageMetadata(c2.languageMetadata);
    // Do NOT need FormerlySerializedAs for currentHp: [FieldTag(11)] in decoded .rsbc preserves it.
    const auto reloadReport = scene.hotReload(c2.program, &metaV2);
    RS_CHECK(reloadReport.succeeded);

    const auto* migratedComp = scene.getComponent(hero, "Game.SchemaEvol::HeroStats");
    RS_CHECK(migratedComp != nullptr);
    runtime::RuntimeError readErr;
    const auto hpVal = rt.getMember(*migratedComp, "currentHp", readErr);
    RS_CHECK(hpVal.has_value() && std::get<std::int64_t>(*hpVal) == 350);
    const auto mpVal = rt.getMember(*migratedComp, "mp", readErr);
    RS_CHECK(mpVal.has_value() && std::get<std::int64_t>(*mpVal) == 75);
    const auto stamVal = rt.getMember(*migratedComp, "stamina", readErr);
    RS_CHECK(stamVal.has_value() && std::get<std::int64_t>(*stamVal) == 55);

    // Also test GameplayStateCodec::migrateComponentFieldValues directly with FieldTag + multiple formerNames.
    const std::vector<game::FieldSchemaDescriptor> oldSchema{
        {"oldHp", std::optional<std::int32_t>{11}, {}},
        {"legacyMana", std::nullopt, {}},
    };
    const std::vector<game::FieldSchemaDescriptor> newSchema{
        {"currentHp", std::optional<std::int32_t>{11}, {}},
        {"mp", std::nullopt, {"legacyMana", "manaPool"}},
        {"stamina", std::nullopt, {}},
    };
    const std::map<std::string, game::GameplayValue> savedFields{
        {"oldHp", game::GameplayValue{std::int64_t{350}}},
        {"legacyMana", game::GameplayValue{std::int64_t{75}}},
    };
    const std::map<std::string, game::GameplayValue> defaultValues{
        {"currentHp", game::GameplayValue{std::int64_t{10}}},
        {"mp", game::GameplayValue{std::int64_t{5}}},
        {"stamina", game::GameplayValue{std::int64_t{55}}},
    };

    const auto migratedTwoSchema = game::migrateComponentFieldValues(
        savedFields, oldSchema, newSchema, defaultValues);
    RS_CHECK(std::get<std::int64_t>(migratedTwoSchema.at("currentHp")) == 350);
    RS_CHECK(std::get<std::int64_t>(migratedTwoSchema.at("mp")) == 75);
    RS_CHECK(std::get<std::int64_t>(migratedTwoSchema.at("stamina")) == 55);

    const std::map<std::string, game::GameplayValue> v2Saved{
        {"manaPool", game::GameplayValue{std::int64_t{90}}},
    };
    const auto migratedAliasOnly = game::migrateComponentFieldValues(
        v2Saved, newSchema, defaultValues);
    RS_CHECK(std::get<std::int64_t>(migratedAliasOnly.at("mp")) == 90);
    RS_CHECK(std::get<std::int64_t>(migratedAliasOnly.at("currentHp")) == 10);

    std::cout << "[PASS] Pillar 6: SceneLifecycleRuntime & GameplayStateCodec [FieldTag]/multi-[FormerlySerializedAs] migration\n";
}

} // namespace

int main() {
    testGameApiBindingsAndResourceHandles();
    testRsBindParserPreprocessorAndParity();
    testPackageLoaderAbiAndGameplayStateCodec();
    testSceneLifecycleEventsTriggersAndAsyncCancellation();
    testHotReloadGuidsSerializationAndRemoteDebug();
    testSceneHotReloadFieldTagsAndGameplayCodecMigration();
    std::cout << "All app_game_sdk_and_scene_tests passed!\n";
    return 0;
}
