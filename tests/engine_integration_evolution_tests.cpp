#include "realscript/hot_reload/HotReload.h"
#include "realscript/debug/DapServer.h"
#include "realscript/debug/Debugger.h"
#include "realscript/game/GameProductization.h"
#include "realscript/game/GameplayMetadata.h"
#include "realscript/game/GameplayScripting.h"
#include "realscript/tooling/LanguageService.h"
#include "realscript/tooling/LspServer.h"
#include "realscript/optimization/Optimizer.h"
#include "realscript/aot_cpp/AotCpp.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string diagnosticsText(
    const realscript::diagnostics::DiagnosticBag& diagnostics) {
    std::string result;
    for (const auto& item : diagnostics.items()) {
        if (!result.empty()) result.push_back('\n');
        result += item.code + ": " + item.message;
    }
    return result;
}

struct NativeActor {
    int health = 100;
    realscript::game::Vec2 position{
        realscript::game::Fixed::fromInt(2),
        realscript::game::Fixed::fromInt(3),
    };

    int readHealth() const { return health; }
    void writeHealth(int value) { health = value; }
    void heal(int amount) { health += amount; }
    realscript::game::Vec2 getPosition() const { return position; }
    void setPosition(realscript::game::Vec2 value) { position = value; }
};

void testScriptInheritingNativeAndMathMarshaling() {
    realscript::game::GameApi api;
    api.type<NativeActor>("Engine.Core", "NativeActor")
        .property<&NativeActor::readHealth, &NativeActor::writeHealth>("Health")
        .method<&NativeActor::heal>("Heal")
        .method<&NativeActor::getPosition>("GetPosition")
        .method<&NativeActor::setPosition>("SetPosition");

    require(
        api.function(
            "Engine.Core",
            "BoostActor",
            [](std::shared_ptr<NativeActor> actor, int bonus) -> int {
                actor->health += bonus;
                return actor->health;
            }),
        "failed to bind BoostActor");
    require(
        api.function(
            "Engine.Core",
            "StepPosition",
            [](realscript::game::Vec2 pos,
               realscript::game::Vec2 vel,
               realscript::game::Fixed dt) -> realscript::game::Vec2 {
                return pos + vel * dt;
            }),
        "failed to bind StepPosition");
    require(api.valid(), "Engine.Core GameApi should be valid");

    const char* scriptSource = R"(
module Game.Actors;
import Engine.Core;
import RealScript.Math;

class Hero : NativeActor
{
    int shield;

    Hero(int initialShield)
    {
        shield = initialShield;
        Heal(initialShield);
    }

    int TakeHit(int damage)
    {
        int absorbed = damage < shield ? damage : shield;
        shield = shield - absorbed;
        Health = Health - (damage - absorbed);
        return BoostActor(this, 5);
    }

    Vec2 Advance(Vec2 velocity, Fixed dt)
    {
        Vec2 start = GetPosition();
        Vec2 next = start + velocity * dt;
        SetPosition(next);
        return StepPosition(next, velocity, dt);
    }
}
)";

    realscript::game::GameScriptCompiler compiler(api);
    auto compiled = compiler.compile({{"actors.rs", scriptSource}});
    require(
        compiled.succeeded(),
        "script inheriting native failed to compile:\n" +
            diagnosticsText(compiled.diagnostics));

    realscript::game::ScriptRuntime runtime(compiled.program);
    auto nativeHero = std::make_shared<NativeActor>();
    nativeHero->health = 80;

    realscript::runtime::RuntimeError error;
    auto heroObj = runtime.createDerivedNative<NativeActor>(
        "Game.Actors::Hero",
        nativeHero,
        error,
        {std::int64_t{20}},
        {},
        "Hero#1");
    require(
        heroObj.has_value(),
        "createDerivedNative failed: " + error.message);
    require(
        nativeHero->health == 100,
        "Hero constructor should have called base C++ Heal(20) on this");
    require(
        runtime.isNativeAlive(*heroObj),
        "native handle on derived Hero should be alive");

    const auto takeHit = runtime.findMethod(heroObj->type(), "TakeHit", 1);
    require(takeHit.has_value(), "Hero.TakeHit method not found");
    const auto hitResult = runtime.invoke(*heroObj, *takeHit, {std::int64_t{35}});
    require(
        hitResult.succeeded && std::get<std::int64_t>(hitResult.value) == 90,
        "Hero.TakeHit should absorb 20 shield, reduce health to 85, and BoostActor(this, 5) to 90");
    require(nativeHero->health == 90, "C++ NativeActor health should be 90");

    // Inherited base C++ method and property lookup directly on derived ScriptObject
    const auto baseHeal = runtime.findMethod(heroObj->type(), "Heal", 1);
    require(
        baseHeal.has_value(),
        "inherited C++ method Heal should be discoverable on derived Hero");
    const auto healExec = runtime.invoke(*heroObj, *baseHeal, {std::int64_t{10}});
    require(healExec.succeeded && nativeHero->health == 100, "direct invoke of inherited C++ Heal failed");

    const auto healthProp = runtime.getMember(*heroObj, "Health", error);
    require(
        healthProp.has_value() && std::get<std::int64_t>(*healthProp) == 100,
        "getMember on inherited C++ property Health failed: " + error.message);

    // Math struct marshaling + operator overloading
    const auto advanceMethod = runtime.findMethod(heroObj->type(), "Advance", 2);
    require(advanceMethod.has_value(), "Hero.Advance method not found");
    const realscript::game::Vec2 velocity{
        realscript::game::Fixed::fromInt(4),
        realscript::game::Fixed::fromInt(-1),
    };
    const realscript::game::Fixed dt = realscript::game::Fixed::fromInt(2);
    auto velStorage = std::make_shared<realscript::runtime::StructStorage>();
    velStorage->fields = {
        realscript::game::detail::makeFixedStructValue(velocity.x),
        realscript::game::detail::makeFixedStructValue(velocity.y),
    };
    const realscript::runtime::Value velVal = realscript::runtime::StructValue{
        realscript::semantic::stableTypeId("RealScript.Math::Vec2"),
        std::move(velStorage),
    };
    const realscript::runtime::Value dtVal =
        realscript::game::detail::makeFixedStructValue(dt);

    const auto advExec = runtime.invoke(*heroObj, *advanceMethod, {velVal, dtVal});
    require(
        advExec.succeeded,
        "Hero.Advance failed: " + advExec.error.message);
    require(
        nativeHero->position.x == realscript::game::Fixed::fromInt(10) &&
            nativeHero->position.y == realscript::game::Fixed::fromInt(1),
        "NativeActor position should be updated to (10, 1)");
    require(
        std::holds_alternative<realscript::runtime::Vec2Value>(advExec.value),
        "returned value should be inline Vec2Value");
    const auto retVec = realscript::game::ScriptRuntime::readVec2(advExec.value);
    require(
        retVec.has_value() &&
            retVec->x == realscript::game::Fixed::fromInt(18) &&
            retVec->y == realscript::game::Fixed::fromInt(-1),
        "StepPosition should return (18, -1)");

    // Native object destruction / handle invalidation
    require(runtime.releaseNative(*heroObj), "releaseNative should succeed");
    require(!runtime.isNativeAlive(*heroObj), "isNativeAlive should be false after releaseNative");
    const auto deadInvoke = runtime.invoke(*heroObj, *baseHeal, {std::int64_t{5}});
    require(
        !deadInvoke.succeeded &&
            deadInvoke.error.code == realscript::runtime::ErrorCode::InvalidNativeHandle,
        "calling C++ method on released native handle should fail with InvalidNativeHandle");
}

void testMultiComponentSceneAndEditorMode() {
    realscript::game::GameApi api;
    const char* source = R"(
module Game.SceneTest;
import RealScript.Scene;

interface IDamageable
{
    void ApplyDamage(int amount);
}

class HealthComponent : IDamageable
{
    int hp;

    void OnCreate()
    {
        hp = 100;
    }

    void OnStart()
    {
        hp = 120;
    }

    void OnUpdate(double dt)
    {
        hp = hp + 1;
    }

    public void ApplyDamage(int amount)
    {
        hp = hp - amount;
    }
}

[Tool]
class GizmoOverlayComponent
{
    int editorTicks;
    int siblingCount;

    void OnUpdate(double dt)
    {
        editorTicks = editorTicks + 1;
    }

    void DamageSibling(long entityId, int dmg)
    {
        siblingCount = GetComponentCount(entityId);
        if (HasComponent(entityId, "IDamageable"))
        {
            SendComponentEventInt(entityId, "ApplyDamage", dmg);
        }
    }
}
)";

    realscript::game::GameProgram emptyProgram;
    realscript::game::ScriptRuntime placeholderRuntime(emptyProgram);
    realscript::game::SceneScriptRuntime* scenePtr = nullptr;

    // Bind scene functions through a forwarding scene runtime
    require(
        api.function(
            "RealScript.Scene",
            "HasComponent",
            [&scenePtr](std::int64_t entityId, std::string typeName) -> bool {
                return scenePtr && scenePtr->hasComponent(
                    static_cast<realscript::game::SceneEntityId>(entityId), typeName);
            }) &&
        api.function(
            "RealScript.Scene",
            "GetComponentCount",
            [&scenePtr](std::int64_t entityId) -> std::int32_t {
                return scenePtr ? static_cast<std::int32_t>(scenePtr->componentCount(
                    static_cast<realscript::game::SceneEntityId>(entityId))) : 0;
            }) &&
        api.function(
            "RealScript.Scene",
            "SendComponentEventInt",
            [&scenePtr](std::int64_t entityId, std::string callback, std::int32_t payload) -> bool {
                return scenePtr && scenePtr->dispatch(realscript::game::ScriptEvent{
                    static_cast<realscript::game::SceneEntityId>(entityId),
                    std::move(callback),
                    {static_cast<std::int64_t>(payload)}});
            }),
        "failed to bind scene component helpers");

    realscript::game::GameScriptCompiler compiler(api);
    auto compiled = compiler.compile({{"scene_test.rs", source}});
    require(
        compiled.succeeded(),
        "multi-component scene script failed to compile:\n" +
            diagnosticsText(compiled.diagnostics));

    realscript::game::ScriptRuntime runtime(compiled.program);
    realscript::game::SceneScriptRuntime scene(runtime);
    scenePtr = &scene;

    const realscript::game::SceneEntityId entity = 42;
    require(
        scene.attachComponent(entity, "Game.SceneTest::HealthComponent"),
        "failed to attach HealthComponent");
    require(
        scene.attachComponent(entity, "Game.SceneTest::GizmoOverlayComponent"),
        "failed to attach GizmoOverlayComponent to same entity");
    require(
        scene.componentCount(entity) == 2,
        "entity should have 2 components attached");
    require(
        scene.hasComponent(entity, "IDamageable"),
        "hasComponent should match implemented interface IDamageable");
    require(
        scene.getComponent(entity, "IDamageable") != nullptr,
        "getComponent should resolve HealthComponent via IDamageable");

    // Test Editor mode: only [Tool] GizmoOverlayComponent should tick OnUpdate
    scene.setExecutionMode(realscript::game::SceneExecutionMode::Editor);
    scene.update(0.016);

    realscript::runtime::RuntimeError error;
    auto* healthObj = scene.getComponent(entity, "HealthComponent");
    auto* gizmoObj = scene.getComponent(entity, "GizmoOverlayComponent");
    require(healthObj && gizmoObj, "both components must be retrievable");
    require(
        std::get<std::int64_t>(*runtime.getMember(*healthObj, "hp", error)) == 100,
        "HealthComponent should NOT tick OnUpdate in Editor mode");
    require(
        std::get<std::int64_t>(*runtime.getMember(*gizmoObj, "editorTicks", error)) == 1,
        "[Tool] GizmoOverlayComponent SHOULD tick OnUpdate in Editor mode");

    // Switch to Play mode: both components start and tick
    scene.setExecutionMode(realscript::game::SceneExecutionMode::Play);
    scene.start();
    scene.update(0.016);
    require(
        std::get<std::int64_t>(*runtime.getMember(*healthObj, "hp", error)) == 121,
        "HealthComponent should run OnStart (120) + OnUpdate (+1 = 121) in Play mode");

    // Script-side cross-component interaction
    const auto dmgCall = scene.invokeComponent(
        entity,
        "GizmoOverlayComponent",
        "DamageSibling",
        {realscript::runtime::LongValue{42}, std::int64_t{21}});
    require(dmgCall.succeeded, "DamageSibling failed: " + dmgCall.error.message);
    require(
        std::get<std::int64_t>(*runtime.getMember(*gizmoObj, "siblingCount", error)) == 2,
        "GetComponentCount from script should return 2");
    require(
        std::get<std::int64_t>(*runtime.getMember(*healthObj, "hp", error)) == 100,
        "SendComponentEventInt should dispatch ApplyDamage(21) to HealthComponent");
}

void testInspectorAttributesJsonAndStatefulHotReload() {
    realscript::game::GameApi api;
    const char* v1Source = R"(
module Game.HotReload;

[ExecuteInEditMode]
class TurretController
{
    [Export]
    [Range(min = 1, max = 50)]
    [Tooltip(text = "Shots per minute")]
    public int fireRate;

    [HideInInspector]
    private int internalCounter;

    public int ammoCount;

    void OnStart()
    {
        internalCounter = 7;
    }

    int ComputePower()
    {
        return fireRate + ammoCount;
    }
}
)";

    realscript::game::GameScriptCompiler compiler(api);
    auto v1Compiled = compiler.compile({{"turret_v1.rs", v1Source}});
    require(
        v1Compiled.succeeded(),
        "turret v1 compile failed:\n" + diagnosticsText(v1Compiled.diagnostics));

    realscript::game::ScriptMetadataRegistry metadata;
    const auto ingestedCount = metadata.populateFromProgram(v1Compiled.program);
    require(ingestedCount >= 5, "populateFromProgram should ingest compiler attributes");

    realscript::game::ScriptRuntime runtime(v1Compiled.program);
    const auto turretType = runtime.findType("Game.HotReload::TurretController");
    require(turretType.has_value(), "TurretController type not found");

    realscript::runtime::RuntimeError error;
    const auto inspector = realscript::game::inspectTypeSchema(
        runtime, metadata, "Game.HotReload::TurretController");
    require(inspector.has_value(), "inspectTypeSchema failed");
    require(inspector->executeInEditMode, "InspectorTypeSchema should detect [ExecuteInEditMode]");
    require(inspector->fields.size() == 3, "TurretController should have 3 reflected fields");
    require(
        inspector->fields[0].name == "fireRate" &&
            inspector->fields[0].exported &&
            !inspector->fields[0].hiddenInInspector &&
            inspector->fields[0].rangeMin.has_value() &&
            *inspector->fields[0].rangeMin == 1 &&
            inspector->fields[0].rangeMax.has_value() &&
            *inspector->fields[0].rangeMax == 50 &&
            inspector->fields[0].tooltip == "Shots per minute",
        "fireRate InspectorFieldSchema metadata mismatch");
    require(
        inspector->fields[1].name == "internalCounter" &&
            inspector->fields[1].hiddenInInspector &&
            !inspector->fields[1].exported,
        "internalCounter should be hidden in inspector");

    // JSON object state round-trip
    auto objA = runtime.createObject(*turretType, {}, error);
    require(objA.has_value(), "failed to create TurretController instance");
    require(runtime.setMember(*objA, "fireRate", std::int64_t{25}, error), "setMember fireRate failed");
    require(runtime.setMember(*objA, "ammoCount", std::int64_t{90}, error), "setMember ammoCount failed");

    const std::string jsonText = realscript::game::exportScriptObjectJson(runtime, *objA, error);
    require(!jsonText.empty(), "exportScriptObjectJson failed: " + error.message);

    auto objB = runtime.createObject(*turretType, {}, error);
    require(objB.has_value(), "failed to create target TurretController instance");
    require(
        realscript::game::importScriptObjectJson(runtime, *objB, jsonText, error),
        "importScriptObjectJson failed: " + error.message);
    require(
        std::get<std::int64_t>(*runtime.getMember(*objB, "fireRate", error)) == 25 &&
            std::get<std::int64_t>(*runtime.getMember(*objB, "ammoCount", error)) == 90,
        "JSON round-trip did not preserve field values");

    // Stateful Structural Hot Reload in SceneScriptRuntime
    realscript::game::SceneScriptRuntime scene(runtime);
    const realscript::game::SceneEntityId entity = 100;
    require(
        scene.attach(entity, "Game.HotReload::TurretController"),
        "failed to attach TurretController");
    scene.start();
    auto* liveTurret = scene.object(entity);
    require(liveTurret != nullptr, "live turret object is null");
    require(runtime.setMember(*liveTurret, "fireRate", std::int64_t{35}, error), "set fireRate");
    require(runtime.setMember(*liveTurret, "ammoCount", std::int64_t{60}, error), "set ammoCount");

    // v2 renames ammoCount -> magazineCapacity with [FormerlySerializedAs("ammoCount")],
    // adds a new field heatLevel, removes internalCounter, and changes ComputePower() formula.
    const char* v2Source = R"(
module Game.HotReload;

[ExecuteInEditMode]
class TurretController
{
    [Export]
    public int fireRate;

    [FormerlySerializedAs(oldName = "ammoCount")]
    public int magazineCapacity;

    public int heatLevel;

    int ComputePower()
    {
        return fireRate * 2 + magazineCapacity + heatLevel;
    }
}
)";

    auto v2Compiled = compiler.compile({{"turret_v2.rs", v2Source}});
    require(
        v2Compiled.succeeded(),
        "turret v2 compile failed:\n" + diagnosticsText(v2Compiled.diagnostics));

    const auto reloadReport = scene.hotReload(v2Compiled.program);
    require(
        reloadReport.succeeded &&
            reloadReport.instancesMigrated == 1 &&
            reloadReport.fieldsRenamed == 1 &&
            reloadReport.fieldsDefaulted == 1,
        "SceneScriptRuntime::hotReload report mismatch");

    const auto powerResult = scene.invoke(entity, "ComputePower");
    require(
        powerResult.succeeded &&
            std::get<std::int64_t>(powerResult.value) == (35 * 2 + 60 + 0),
        "hot-reloaded ComputePower() should use migrated fireRate (35) and renamed magazineCapacity (60)");
}

void testCoroutineWaitSeconds() {
    auto host = std::make_shared<realscript::game::GameplayHost>(20); // 20 ticks per second
    realscript::game::GameApi api;
    require(
        realscript::game::installGameplayBindings(api, host),
        "installGameplayBindings failed");

    const char* source = R"(
module Game.Coroutines;
import RealScript.Game;

class CutsceneRunner
{
    int stage;

    sequence PlayIntro(long selfEntity)
    {
        stage = 1;
        yield wait_seconds(0.5);
        stage = 2;
        yield wait_seconds(1);
        stage = 3;
    }
}
)";

    realscript::game::GameScriptCompiler compiler(api);
    auto compiled = compiler.compile({{"coroutines.rs", source}});
    require(
        compiled.succeeded(),
        "wait_seconds coroutine failed to compile:\n" +
            diagnosticsText(compiled.diagnostics));

    realscript::game::ScriptRuntime runtime(compiled.program);
    realscript::game::SceneScriptRuntime scene(runtime);
    realscript::game::SceneGameplayDriver driver(scene, host);

    const realscript::game::SceneEntityId entity = 7;
    require(
        scene.attach(entity, "Game.Coroutines::CutsceneRunner"),
        "failed to attach CutsceneRunner");
    const auto startExec = scene.invoke(
        entity, "PlayIntro", {realscript::runtime::LongValue{7}});
    require(startExec.succeeded, "PlayIntro failed to start");

    realscript::runtime::RuntimeError error;
    auto* obj = scene.object(entity);
    require(
        std::get<std::int64_t>(*runtime.getMember(*obj, "stage", error)) == 1,
        "stage should be 1 immediately after PlayIntro start");

    // 0.5 seconds at 20 TPS = 10 ticks
    (void)driver.advanceTicks(9);
    require(
        std::get<std::int64_t>(*runtime.getMember(*obj, "stage", error)) == 1,
        "stage should still be 1 at tick 9");
    (void)driver.advanceTicks(1);
    require(
        std::get<std::int64_t>(*runtime.getMember(*obj, "stage", error)) == 2,
        "stage should advance to 2 after 0.5s (10 ticks)");

    // Next wait_seconds(1) = 20 ticks
    (void)driver.advanceTicks(19);
    require(
        std::get<std::int64_t>(*runtime.getMember(*obj, "stage", error)) == 2,
        "stage should still be 2 after 19 more ticks");
    (void)driver.advanceTicks(1);
    require(
        std::get<std::int64_t>(*runtime.getMember(*obj, "stage", error)) == 3,
        "stage should advance to 3 after 1.0s (20 ticks)");
}

void testLspStubsDotCompletionAndSignatureHelp() {
    realscript::game::GameApi api;
    api.type<NativeActor>("Engine.Core", "NativeActor")
        .property<&NativeActor::readHealth, &NativeActor::writeHealth>("Health")
        .method<&NativeActor::heal>("Heal");
    require(
        api.function(
            "Engine.Core",
            "SpawnWave",
            [](int count, double intervalSeconds) -> int {
                return count + static_cast<int>(intervalSeconds);
            }),
        "failed to bind SpawnWave");

    const auto stubs = api.exportDeclarationStubs();
    require(!stubs.empty(), "exportDeclarationStubs should produce .d.rs stubs");
    const std::string configJson = api.exportProjectConfigJson("SampleGame");
    require(!configJson.empty(), "exportProjectConfigJson should produce JSON");

    realscript::tooling::LanguageService service;
    require(
        service.loadProjectConfigJson(configJson),
        "LanguageService::loadProjectConfigJson failed");

    const std::string userCode = R"(module Game.Client;
import Engine.Core;

class Player : NativeActor
{
    void Tick(NativeActor target)
    {
        target.
        SpawnWave(3, 1.5);
    }
}
)";
    service.open("player.rs", userCode, 1);

    // Dot completion right after "target." on line 7 (0-indexed), column 15
    const auto completions = service.completion("player.rs", {7, 15});
    bool foundHeal = false;
    bool foundHealth = false;
    for (const auto& item : completions) {
        if (item.label == "Heal") foundHeal = true;
        if (item.label == "Health") foundHealth = true;
    }
    require(
        foundHeal && foundHealth,
        "dot completion on 'target.' should include NativeActor's Heal method and Health property");

    // Signature help inside "SpawnWave(3, 1.5)" after comma on line 8, column 21
    const auto sigHelp = service.signatureHelp("player.rs", {8, 21});
    require(
        sigHelp.has_value() &&
            !sigHelp->signatures.empty() &&
            sigHelp->activeParameter == 1 &&
            sigHelp->signatures.front().parameters.size() == 2,
        "signatureHelp on SpawnWave(3, |) should resolve 2 parameters with activeParameter == 1");
}

bool aotFastAddImpl(
    realscript::aot::ExecutionContext&,
    const realscript::runtime::Value* args,
    std::size_t count,
    realscript::runtime::Value& result) {
    if (count != 2) return false;
    const auto* rhs = std::get_if<std::int64_t>(&args[1]);
    if (!rhs) return false;
    // Return a distinct sentinel offset (+1000) to prove execution went through the AOT backend
    result = *rhs + 1000;
    return true;
}

void testTieredExecutionDispatcher() {
    realscript::game::GameApi api;
    const char* source = R"(
module Game.Tiered;

class Calculator
{
    int Compute(int x)
    {
        return x + 1;
    }

    int InterpretOnly(int x)
    {
        return x * 2;
    }
}
)";
    realscript::game::GameScriptCompiler compiler(api);
    auto compiled = compiler.compile({{"tiered.rs", source}});
    require(compiled.succeeded(), "tiered test compile failed");

    realscript::game::ScriptRuntime runtime(compiled.program);
    const auto calcType = runtime.findType("Game.Tiered::Calculator");
    require(calcType.has_value(), "Calculator type not found");
    const auto computeMethod = runtime.findMethod(*calcType, "Compute", 1);
    const auto interpMethod = runtime.findMethod(*calcType, "InterpretOnly", 1);
    require(computeMethod.has_value() && interpMethod.has_value(), "Calculator methods not found");

    realscript::runtime::RuntimeError error;
    auto calcObj = runtime.createObject(*calcType, {}, error);
    require(calcObj.has_value(), "failed to create Calculator");

    // Default interpreter backend
    require(
        runtime.activeBackend() == realscript::game::ScriptExecutionBackend::Interpreter,
        "default backend should be Interpreter");
    const auto interpRes = runtime.invoke(*calcObj, *computeMethod, {std::int64_t{10}});
    require(
        interpRes.succeeded && std::get<std::int64_t>(interpRes.value) == 11,
        "Interpreter Compute(10) should return 11");

    // Register AOT backend providing native implementation for Compute, while InterpretOnly falls back to Interpreter
    const realscript::semantic::PrimitiveType paramTypes[] = {
        realscript::semantic::PrimitiveType::Object,
        realscript::semantic::PrimitiveType::Int,
    };
    const realscript::semantic::SymbolId paramTypeIds[] = {
        calcType->descriptor.id,
        0,
    };
    const realscript::aot::FunctionDescriptor aotFunctions[] = {
        {
            computeMethod->descriptor.id,
            "Game.Tiered::Calculator.Compute",
            realscript::semantic::PrimitiveType::Int,
            0,
            paramTypes,
            paramTypeIds,
            2,
            &aotFastAddImpl,
            nullptr,
        },
    };
    const realscript::aot::TypeDescriptor aotTypes[] = {
        {
            calcType->descriptor.id,
            realscript::semantic::TypeKind::Class,
            0,
            "Game.Tiered",
            "Calculator",
            nullptr,
            0,
            nullptr,
            0,
            nullptr,
            0,
            false,
            nullptr,
            0,
            false,
        },
    };
    const char* const moduleNames[] = {"Game.Tiered"};
    const realscript::aot::ProgramDescriptor aotDesc{
        realscript::aot::RuntimeAbiMajor,
        realscript::aot::RuntimeAbiMinor,
        "TieredTest",
        12345ULL,
        moduleNames,
        1,
        aotTypes,
        1,
        aotFunctions,
        1,
        nullptr,
        0,
    };

    runtime.setAotBackend(&aotDesc);
    require(
        runtime.activeBackend() == realscript::game::ScriptExecutionBackend::Aot,
        "activeBackend should be Aot");

    const auto aotRes = runtime.invoke(*calcObj, *computeMethod, {std::int64_t{10}});
    require(
        aotRes.succeeded && std::get<std::int64_t>(aotRes.value) == 1010,
        "AOT Compute(10) should dispatch through AOT implementation (1010)");

    const auto fallbackRes = runtime.invoke(*calcObj, *interpMethod, {std::int64_t{10}});
    require(
        fallbackRes.succeeded && std::get<std::int64_t>(fallbackRes.value) == 20,
        "InterpretOnly(10) should seamlessly fall back to Interpreter (20)");
}

void testStage1EditorDataflowSignalsAndHotReloadRefs() {
    const char* sourceV1 = R"(
module Game.Stage1;

enum AlertLevel
{
    Calm = 0,
    Caution = 1,
    Danger = 5
}

struct TuningData
{
    int minDamage;
    double critMultiplier;
}

delegate void SignalHandler(int code);

class DoorComponent
{
    public int openCode;
    public bool isOpen;

    public void OnSwitchTriggered(int code)
    {
        openCode = code;
        isOpen = code > 0;
    }
}

class SwitchComponent
{
    public AlertLevel alert;
    public TuningData tuning;
    public int[] checkpoints;
    public List<string> tags;
    [EntityRef(Type = "Actor")]
    public long linkedEntityId;
    [AssetRef(Type = "AudioClip")]
    public string clickSoundPath;
    public DoorComponent targetDoor;
    public int powerLevel;
    public event SignalHandler OnTriggered;

    public void OnValidate()
    {
        if (powerLevel < 0) powerLevel = 0;
        if (powerLevel > 100) powerLevel = 100;
    }

    public void PullSwitch(int code)
    {
        if (targetDoor != null)
        {
            targetDoor.openCode = code * 10;
        }
        OnTriggered(code);
    }
}
)";

    realscript::game::GameApi api;
    realscript::game::GameScriptCompiler compiler(api);
    auto compiled = compiler.compile({{"stage1.rs", sourceV1}});
    require(
        compiled.succeeded(),
        "Stage 1 script failed to compile:\n" + diagnosticsText(compiled.diagnostics));

    realscript::game::ScriptRuntime runtime(compiled.program);
    realscript::game::ScriptMetadataRegistry metadata;
    metadata.populateFromProgram(compiled.program);

    const auto schemaOpt = realscript::game::inspectTypeSchema(
        runtime, metadata, "Game.Stage1::SwitchComponent");
    require(schemaOpt.has_value(), "inspectTypeSchema for SwitchComponent failed");
    const auto& schema = *schemaOpt;

    require(schema.hasOnValidate, "SwitchComponent schema should detect OnValidate()");
    require(
        schema.signals.size() == 1 &&
            schema.signals[0].name == "OnTriggered" &&
            schema.signals[0].parameters.size() == 1 &&
            schema.signals[0].parameters[0].type == realscript::semantic::PrimitiveType::Int,
        "SwitchComponent schema should expose OnTriggered(int) signal");

    bool checkedEnum = false;
    bool checkedStruct = false;
    bool checkedArray = false;
    bool checkedList = false;
    bool checkedEntityRef = false;
    bool checkedAssetRef = false;
    bool checkedComponentRef = false;
    for (const auto& field : schema.fields) {
        if (field.name == "alert") {
            checkedEnum = field.enumOptions.size() == 3 &&
                field.enumOptions[0].name == "Calm" && field.enumOptions[0].value == 0 &&
                field.enumOptions[2].name == "Danger" && field.enumOptions[2].value == 5;
        } else if (field.name == "tuning") {
            checkedStruct = field.nestedFields.size() == 2 &&
                field.nestedFields[0].name == "minDamage" &&
                field.nestedFields[1].name == "critMultiplier";
        } else if (field.name == "checkpoints") {
            checkedArray = field.isCollection && field.elementTypeName == "int";
        } else if (field.name == "tags") {
            checkedList = field.isCollection && field.elementTypeName == "string";
        } else if (field.name == "linkedEntityId") {
            checkedEntityRef =
                field.referenceKind == realscript::game::InspectorReferenceKind::Entity &&
                field.referencedTypeName == "Actor";
        } else if (field.name == "clickSoundPath") {
            checkedAssetRef =
                field.referenceKind == realscript::game::InspectorReferenceKind::Asset &&
                field.referencedTypeName == "AudioClip";
        } else if (field.name == "targetDoor") {
            checkedComponentRef =
                field.referenceKind == realscript::game::InspectorReferenceKind::Component &&
                field.referencedTypeName == "Game.Stage1::DoorComponent";
        }
    }
    require(checkedEnum, "InspectorFieldSchema enumOptions reflection failed");
    require(checkedStruct, "InspectorFieldSchema nestedFields reflection failed");
    require(checkedArray, "InspectorFieldSchema array collection reflection failed");
    require(checkedList, "InspectorFieldSchema generic List reflection failed");
    require(checkedEntityRef, "InspectorFieldSchema EntityRef reflection failed");
    require(checkedAssetRef, "InspectorFieldSchema AssetRef reflection failed");
    require(checkedComponentRef, "InspectorFieldSchema ComponentRef reflection failed");

    // Scene wiring: entity 1 has SwitchComponent, entity 2 has DoorComponent
    realscript::game::SceneScriptRuntime scene(runtime);
    require(scene.attachComponent(1, "Game.Stage1::SwitchComponent"), "attach SwitchComponent failed");
    require(scene.attachComponent(2, "Game.Stage1::DoorComponent"), "attach DoorComponent failed");

    realscript::runtime::RuntimeError error;
    // Test setInspectorMember + OnValidate clamping
    require(
        scene.setInspectorMember(
            1, "SwitchComponent", "powerLevel", std::int64_t{250}),
        "setInspectorMember failed");
    auto* switchObj = scene.getComponent(1, "SwitchComponent");
    auto* doorObj = scene.getComponent(2, "DoorComponent");
    require(switchObj && doorObj, "failed to look up SwitchComponent or DoorComponent");
    const auto clampedPower = runtime.getMember(*switchObj, "powerLevel", error);
    require(
        clampedPower.has_value() && std::get<std::int64_t>(*clampedPower) == 100,
        "OnValidate should clamp powerLevel from 250 to 100");

    // Set cross-entity component reference & array field, then round-trip JSON
    require(
        runtime.setMember(*switchObj, "targetDoor", doorObj->value(), error),
        "setting targetDoor reference failed: " + error.message);
    auto arrRef = runtime.heap()->allocateArray(2, std::int64_t{0}, &error);
    require(arrRef.has_value(), "allocateArray failed");
    runtime.heap()->arraySet(*arrRef, 0, std::int64_t{10});
    runtime.heap()->arraySet(*arrRef, 1, std::int64_t{20});
    require(
        runtime.setMember(*switchObj, "checkpoints", *arrRef, error),
        "setting checkpoints array failed");

    realscript::game::ScriptStatePolicy jsonPolicy;
    jsonPolicy.allowDoubles = true;
    const std::string exportedJson = realscript::game::exportScriptObjectJson(
        runtime, *switchObj, error, jsonPolicy, &scene);
    require(
        error.code == realscript::runtime::ErrorCode::None &&
            exportedJson.find("\"$entity\":2") != std::string::npos &&
            exportedJson.find("[10,20]") != std::string::npos,
        "exportScriptObjectJson should serialize cross-entity component ref and array: " +
            error.message + " / " + exportedJson);

    // Clear fields and re-import from JSON
    require(
        runtime.setMember(*switchObj, "targetDoor", realscript::runtime::NullObject{}, error),
        "clearing targetDoor failed");
    require(
        realscript::game::importScriptObjectJson(
            runtime, *switchObj, exportedJson, error, jsonPolicy, &scene),
        "importScriptObjectJson failed: " + error.message);

    // Connect signal from entity 1 SwitchComponent.OnTriggered -> entity 2 DoorComponent.OnSwitchTriggered
    require(
        scene.connectSignal(
            1, "SwitchComponent", "OnTriggered", 2, "DoorComponent", "OnSwitchTriggered"),
        "connectSignal failed");

    // Hot-reload v2 and verify both targetDoor reference AND OnTriggered signal survive migration!
    const char* sourceV2 = R"(
module Game.Stage1;

enum AlertLevel
{
    Calm = 0,
    Caution = 1,
    Danger = 5
}

struct TuningData
{
    int minDamage;
    double critMultiplier;
}

delegate void SignalHandler(int code);

class DoorComponent
{
    public int openCode;
    public bool isOpen;
    public int reloadBonus;

    public void OnSwitchTriggered(int code)
    {
        openCode = code + reloadBonus;
        isOpen = true;
    }
}

class SwitchComponent
{
    public AlertLevel alert;
    public TuningData tuning;
    public int[] checkpoints;
    public List<string> tags;
    public long linkedEntityId;
    public string clickSoundPath;
    public DoorComponent targetDoor;
    public int powerLevel;
    public event SignalHandler OnTriggered;

    public void PullSwitch(int code)
    {
        if (targetDoor != null)
        {
            targetDoor.reloadBonus = code * 10;
        }
        OnTriggered(code);
    }
}
)";
    auto compiledV2 = compiler.compile({{"stage1.rs", sourceV2}});
    require(compiledV2.succeeded(), "Stage 1 v2 failed to compile");
    const auto reloadReport = scene.hotReload(compiledV2.program);
    require(
        reloadReport.succeeded && reloadReport.instancesMigrated == 2,
        "Stage 1 hotReload failed");

    const auto pullRes = scene.invokeComponent(
        1, "SwitchComponent", "PullSwitch", {std::int64_t{7}});
    require(
        pullRes.succeeded,
        "PullSwitch after hotReload failed: " + pullRes.error.message);

    auto* reloadedDoor = scene.getComponent(2, "DoorComponent");
    require(reloadedDoor != nullptr, "reloaded DoorComponent not found");
    const auto openCodeVal = runtime.getMember(*reloadedDoor, "openCode", error);
    const auto isOpenVal = runtime.getMember(*reloadedDoor, "isOpen", error);
    require(
        openCodeVal.has_value() && std::get<std::int64_t>(*openCodeVal) == 77 &&
            isOpenVal.has_value() && std::get<bool>(*isOpenVal),
        "After hotReload, targetDoor ref should set reloadBonus=70 and signal should set openCode=77");
}

void testStage2CoroutinesAndDebuggerEvaluation() {
    auto host = std::make_shared<realscript::game::GameplayHost>(60);
    realscript::game::GameApi api;
    require(
        realscript::game::installGameplayBindings(api, host),
        "installGameplayBindings failed");

    const char* coroutineSource = R"(
module Game.Stage2;
import RealScript.Game;

class QuestRunner
{
    public int stage;
    public int energy;

    sequence RunQuest(long selfEntity)
    {
        stage = 1;
        yield wait_event("GateOpened");
        stage = 2;
        yield wait_until(energy >= 3);
        stage = 3;
    }

    public void AddEnergy(int amount)
    {
        energy = energy + amount;
    }
}
)";
    realscript::game::GameScriptCompiler compiler(api);
    auto compiled = compiler.compile({{"stage2.rs", coroutineSource}});
    require(
        compiled.succeeded(),
        "Stage 2 coroutine script failed to compile:\n" +
            diagnosticsText(compiled.diagnostics));

    realscript::game::ScriptRuntime runtime(compiled.program);
    realscript::game::SceneScriptRuntime scene(runtime);
    realscript::game::SceneGameplayDriver driver(scene, host);

    std::cout << "  [8a] starting QuestRunner..." << std::endl;
    require(scene.attach(1, "Game.Stage2::QuestRunner"), "attach QuestRunner failed");
    require(
        scene.invoke(1, "RunQuest", {realscript::runtime::LongValue{1}}).succeeded,
        "starting RunQuest failed");

    realscript::runtime::RuntimeError error;
    auto readStage = [&]() -> std::int64_t {
        const auto val = runtime.getMember(*scene.object(1), "stage", error);
        return val ? std::get<std::int64_t>(*val) : -1;
    };
    require(readStage() == 1, "RunQuest should start at stage 1 and wait for GateOpened");

    // Advance 3 ticks without publishing GateOpened -> still stage 1
    (void)driver.advanceTicks(3);
    require(readStage() == 1, "RunQuest should remain at stage 1 before GateOpened");

    // Publish GateOpened -> transitions to stage 2 and begins polling wait_until(energy >= 3)
    std::cout << "  [8b] publishing GateOpened..." << std::endl;
    (void)host->publish("GateOpened", {}, 1);
    (void)driver.advanceTicks(1);
    require(readStage() == 2, "RunQuest should advance to stage 2 on GateOpened event");

    // Add energy = 2 -> still waiting
    require(scene.invoke(1, "AddEnergy", {std::int64_t{2}}).succeeded, "AddEnergy(2) failed");
    (void)driver.advanceTicks(2);
    require(readStage() == 2, "RunQuest should remain at stage 2 while energy == 2");

    // Hot-reload while suspended in wait_until!
    std::cout << "  [8c] hotReload during wait_until..." << std::endl;
    const auto reloadReport = scene.hotReload(compiled.program);
    require(reloadReport.succeeded, "hotReload during wait_until failed");

    // Add energy +1 -> energy == 3 -> wait_until succeeds on next tick -> stage 3!
    require(scene.invoke(1, "AddEnergy", {std::int64_t{1}}).succeeded, "AddEnergy(1) failed");
    (void)driver.advanceTicks(1);
    require(readStage() == 3, "RunQuest should reach stage 3 once energy >= 3");

    // Verify automatic sequence cancellation when an entity is detached
    std::cout << "  [8d] detach entity 2..." << std::endl;
    require(scene.attach(2, "Game.Stage2::QuestRunner"), "attach QuestRunner #2 failed");
    require(
        scene.invoke(2, "RunQuest", {realscript::runtime::LongValue{2}}).succeeded,
        "starting RunQuest #2 failed");
    require(scene.detach(2), "detach entity 2 failed");
    (void)host->publish("GateOpened", {}, 1);
    (void)driver.advanceTicks(2);
    require(driver.errors().empty(), "detached entity sequence should be cancelled cleanly without callback errors");

    std::cout << "  [8e] launching DebugSession..." << std::endl;

    // Test Conditional Breakpoints & Side-Effect-Free Watch/Hover Expression Evaluation
    const char* debugSource = R"(
module Game.DebugTest;

class ScoreTracker
{
    int multiplier;
    int[] bonusTable;

    ScoreTracker()
    {
        multiplier = 10;
        bonusTable = new int[3];
        bonusTable[0] = 5;
        bonusTable[1] = 15;
        bonusTable[2] = 25;
    }

    int Accumulate(int count)
    {
        int total = 0;
        for (int i = 0; i < count; i = i + 1)
        {
            total = total + i * multiplier;
        }
        return total;
    }
}

int Main()
{
    ScoreTracker tracker = new ScoreTracker();
    return tracker.Accumulate(5);
}
)";
    auto debugCompiled = compiler.compile({{"debug_test.rs", debugSource}});
    require(debugCompiled.succeeded(), "debug_test.rs failed to compile");

    realscript::debug::DebugSession session(
        debugCompiled.program.program(),
        debugCompiled.program.heap());
    // Set conditional breakpoint inside loop (line 23, 0-based 22) when i == 3 && multiplier == 10
    const auto bps = session.controller()->setConditionalBreakpoints(
        "debug_test.rs",
        {realscript::debug::BreakpointSpec{{22, 0}, "i == 3 && multiplier == 10"}});
    require(bps.size() == 1 && bps[0].verified, "conditional breakpoint should be verified");

    require(session.launch("Game.DebugTest::Main"), "DebugSession launch failed");
    require(
        session.waitForStop(std::chrono::seconds{5}),
        "DebugSession should stop on conditional breakpoint");

    std::string evalError;
    const auto iVal = session.evaluate("i", 1, &evalError);
    require(
        iVal.has_value() && std::get<std::int64_t>(*iVal) == 3,
        "conditional breakpoint should stop only when i == 3, got error: " + evalError);

    const auto watchExpr = session.evaluate("total + i * this.multiplier + bonusTable[1]", 1, &evalError);
    require(
        watchExpr.has_value() && std::get<std::int64_t>(*watchExpr) == (30 + 30 + 15),
        "watch expression evaluation failed: " + evalError);

    const auto lenExpr = session.evaluate("this.bonusTable.Length", 1, &evalError);
    require(
        lenExpr.has_value() && std::get<std::int64_t>(*lenExpr) == 3,
        "array Length evaluation failed: " + evalError);

    session.controller()->clearBreakpoints();
    session.continueExecution();
    session.join();
    require(
        session.result().has_value() &&
            session.result()->succeeded &&
            std::get<std::int64_t>(session.result()->value) == 100,
        "DebugSession should finish and return 100");
}

void testStage3FastMathAndChromeTracing() {
    realscript::game::GameApi api;
    const char* source = R"(
module Game.Stage3;
import RealScript.Math;

class Mover
{
    public Vec2 position;
    public Vec2 velocity;

    public void Step()
    {
        position = position + velocity;
    }
}
)";
    realscript::game::GameScriptCompiler compiler(api);
    auto compiled = compiler.compile({{"stage3.rs", source}});
    require(
        compiled.succeeded(),
        "Stage 3 script failed to compile:\n" + diagnosticsText(compiled.diagnostics));

    realscript::game::ScriptRuntime runtime(compiled.program);
    realscript::runtime::RuntimeError error;
    auto mover = runtime.createObject("Game.Stage3::Mover", error);
    require(mover.has_value(), "failed to create Mover: " + error.message);

    require(
        runtime.setMemberVec2(
            *mover,
            "position",
            realscript::game::Vec2{
                realscript::game::Fixed::fromInt(1),
                realscript::game::Fixed::fromInt(2),
            },
            error) &&
        runtime.setMemberVec2(
            *mover,
            "velocity",
            realscript::game::Vec2{
                realscript::game::Fixed::fromInt(3),
                realscript::game::Fixed::fromInt(4),
            },
            error),
        "initial setMemberVec2 failed: " + error.message);

    const auto initialPos = runtime.getMemberVec2(*mover, "position", error);
    require(
        initialPos.has_value() &&
            initialPos->x == realscript::game::Fixed::fromInt(1) &&
            initialPos->y == realscript::game::Fixed::fromInt(2),
        "getMemberVec2 initial position mismatch");

    // Mutate Vec2 repeatedly and verify values
    for (int i = 1; i <= 5; ++i) {
        require(
            runtime.setMemberVec2(
                *mover,
                "position",
                realscript::game::Vec2{
                    realscript::game::Fixed::fromInt(i * 10),
                    realscript::game::Fixed::fromInt(i * 20),
                },
                error),
            "setMemberVec2 failed: " + error.message);
    }
    const auto updatedPos = runtime.getMemberVec2(*mover, "position", error);
    require(
        updatedPos.has_value() &&
            updatedPos->x == realscript::game::Fixed::fromInt(50) &&
            updatedPos->y == realscript::game::Fixed::fromInt(100),
        "getMemberVec2 after setMemberVec2 mismatch");

    // Profile execution and export Chrome Trace JSON
    auto collector = std::make_shared<realscript::runtime::ProfileCollector>();
    realscript::runtime::ExecutionOptions execOptions;
    execOptions.profile = collector;
    const auto stepMethod = runtime.findMethod(mover->type(), "Step", 0);
    require(stepMethod.has_value(), "Mover.Step not found");
    const auto stepRes = runtime.invoke(*mover, *stepMethod, {}, execOptions);
    require(stepRes.succeeded, "Mover.Step failed");

    const auto traceJson = realscript::runtime::exportChromeTraceJson(
        collector->snapshot(), &runtime.heap()->statistics());
    require(
        traceJson.find("\"traceEvents\":[") != std::string::npos &&
            traceJson.find("\"cat\":\"script\"") != std::string::npos &&
            traceJson.find("\"name\":\"GC Memory\"") != std::string::npos,
        "exportChromeTraceJson output is missing expected trace events: " + traceJson);
}

void testBoostStage1InlineMathAndValueSpanBindings() {
    realscript::game::GameApi api;
    std::size_t observedSpanCalls = 0;
    const realscript::runtime::Value* observedDataPtr = nullptr;

    require(
        api.function(
            "Engine.Stage1",
            "InspectSpan",
            [&](realscript::game::Fixed f, realscript::game::Vec2 v2, realscript::game::Vec3 v3) {
                ++observedSpanCalls;
                return realscript::game::Fixed::fromRaw(
                    f.raw + v2.x.raw + v2.y.raw + v3.x.raw + v3.y.raw + v3.z.raw);
            }),
        "failed to register Engine.Stage1::InspectSpan");

    // Also verify direct ValueSpan zero-allocation invocation on BindingRegistry
    {
        realscript::bytecode::FunctionReference ref;
        ref.name = "Engine.Stage1::__rs_native_InspectSpan";
        const auto resolved = api.bindings()->resolve(ref);
        require(resolved.has_value() && resolved->function != nullptr, "failed to resolve InspectSpan binding");
        std::array<realscript::runtime::Value, 3> stackArgs{
            realscript::runtime::FixedValue{256},
            realscript::runtime::Vec2Value{512, 768},
            realscript::runtime::Vec3Value{256, 256, 256},
        };
        realscript::runtime::ValueSpan spanView(stackArgs);
        observedDataPtr = spanView.data();
        require(observedDataPtr == stackArgs.data(), "ValueSpan must point directly to stack array without copying");
        realscript::runtime::RuntimeError directErr;
        const auto directRes = (*resolved->function)(ref, spanView, directErr);
        require(
            directRes.has_value() &&
                std::holds_alternative<realscript::runtime::FixedValue>(*directRes) &&
                std::get<realscript::runtime::FixedValue>(*directRes).raw == (256 + 512 + 768 + 256 + 256 + 256),
            "direct ValueSpan invocation of native binding failed");
        observedSpanCalls = 0;
    }

    const char* source = R"(
module Game.BoostStage1;
import RealScript.Math;
import Engine.Stage1;

class MathRunner
{
    public Vec3 currentPos;
    public Quat currentRot;

    public Fixed RunInlineCheck()
    {
        Fixed f = FixedFromInt(3);
        Vec2 v2 = new Vec2(FixedFromInt(4), FixedFromInt(5));
        Vec3 v3 = new Vec3(FixedFromInt(1), FixedFromInt(2), FixedFromInt(6));
        v2.x = v2.x + FixedFromInt(10);
        v3.z = v3.z + FixedFromInt(20);
        currentPos = v3;
        currentRot = QuatIdentity();
        return InspectSpan(f, v2, v3);
    }
}
)";

    realscript::game::GameScriptCompiler compiler(api);
    auto compiled = compiler.compile({{"boost_stage1.rs", source}});
    require(
        compiled.succeeded(),
        "boost_stage1 compile failed:\n" + diagnosticsText(compiled.diagnostics));

    realscript::game::ScriptRuntime runtime(compiled.program);
    const auto runnerType = runtime.findType("Game.BoostStage1::MathRunner");
    require(runnerType.has_value(), "MathRunner type not found");

    realscript::runtime::RuntimeError error;
    auto runner = runtime.createObject(*runnerType, {}, error);
    require(runner.has_value(), "failed to create MathRunner: " + error.message);

    const auto runMethod = runtime.findMethod(*runnerType, "RunInlineCheck", 0);
    require(runMethod.has_value(), "RunInlineCheck not found");
    const auto execRes = runtime.invoke(*runner, *runMethod, {});
    require(execRes.succeeded, "RunInlineCheck failed: " + execRes.error.message);
    require(
        observedSpanCalls == 1 && observedDataPtr != nullptr,
        "InspectSpan should have been invoked via non-null ValueSpan");
    require(
        std::holds_alternative<realscript::runtime::FixedValue>(execRes.value),
        "Fixed return value must be inline FixedValue in runtime::Value");
    const auto totalFixed = realscript::game::Fixed::fromRaw(
        std::get<realscript::runtime::FixedValue>(execRes.value).raw);
    // 3 + (4+10) + 5 + 1 + 2 + (6+20) = 51
    require(
        totalFixed == realscript::game::Fixed::fromInt(51),
        "RunInlineCheck expected Fixed(51)");

    const auto posVal = runtime.getMember(*runner, "currentPos", error);
    require(
        posVal.has_value() &&
            std::holds_alternative<realscript::runtime::Vec3Value>(*posVal),
        "Vec3 field must be stored inline as Vec3Value without StructStorage allocation");
    const auto rotVal = runtime.getMember(*runner, "currentRot", error);
    require(
        rotVal.has_value() &&
            std::holds_alternative<realscript::runtime::QuatValue>(*rotVal),
        "Quat field must be stored inline as QuatValue without StructStorage allocation");

    // Verify JSON round-trip and Inspector schema on inline math struct fields
    const std::string mathJson = realscript::game::exportScriptObjectJson(runtime, *runner, error);
    require(!mathJson.empty(), "exportScriptObjectJson on inline math fields failed: " + error.message);

    auto runnerCopy = runtime.createObject(*runnerType, {}, error);
    require(runnerCopy.has_value(), "failed to create MathRunner copy");
    require(
        realscript::game::importScriptObjectJson(runtime, *runnerCopy, mathJson, error),
        "importScriptObjectJson on inline math fields failed: " + error.message);
    const auto copyPosVal = runtime.getMember(*runnerCopy, "currentPos", error);
    require(
        copyPosVal.has_value() &&
            std::holds_alternative<realscript::runtime::Vec3Value>(*copyPosVal) &&
            std::get<realscript::runtime::Vec3Value>(*copyPosVal).z == realscript::game::Fixed::fromInt(26).raw,
        "importScriptObjectJson must preserve inline Vec3Value storage and field values");

    realscript::game::ScriptMetadataRegistry mathMetadata;
    mathMetadata.populateFromProgram(compiled.program);
    const auto mathSchema = realscript::game::inspectTypeSchema(
        runtime, mathMetadata, "Game.BoostStage1::MathRunner");
    require(
        mathSchema.has_value() &&
            mathSchema->fields.size() == 2 &&
            mathSchema->fields[0].nestedFields.size() == 3 &&
            std::holds_alternative<realscript::runtime::FixedValue>(mathSchema->fields[0].nestedFields[0].defaultValue),
        "inspectTypeSchema must populate nested defaultValue for inline Vec3/Fixed fields");
}

void testBoostStage2ConstStaticStringInterpolationMathAndContainers() {
    realscript::game::GameApi api;

    // 1. Verify const assignment/mutation produces diagnostic RS2483
    {
        const char* badConstSource = R"(
module Game.BadConst;

class BadConst
{
    public const int MaxPlayers = 4;

    public int Mutate()
    {
        const int localLimit = 10;
        localLimit = 20;
        MaxPlayers = 8;
        return localLimit;
    }
}
)";
        realscript::game::GameScriptCompiler compiler(api);
        auto badCompiled = compiler.compile({{"bad_const.rs", badConstSource}});
        require(!badCompiled.succeeded(), "mutating const field/local must fail compilation");
        bool sawConstDiag = false;
        for (const auto& item : badCompiled.diagnostics.items()) {
            if (item.code == "RS2483") sawConstDiag = true;
        }
        require(sawConstDiag, "expected RS2483 diagnostic when assigning to const symbol");
    }

    // 2. Verify const/static fields, $"..." interpolation, RealScript.Math (Sin/Cos/Atan2/Lerp/Quat),
    //    string methods, and List/Dictionary/HashSet/Queue/Stack standard methods.
    const char* source = R"(
module Game.BoostStage2;
import RealScript.Math;

class CounterConfig
{
    public const int BaseBonus = 10;
    public const int ScaledBonus = BaseBonus * 3 + 5;
    public static int GlobalCount = 100;
    public int instanceBias = 7;
    public float scaleFactor = 2.5;
    public long seedId = 900;
    public Vec2 spawnOffset = new Vec2(FixedFromInt(3), FixedFromInt(4));

    public int Bump(int delta)
    {
        CounterConfig.GlobalCount = CounterConfig.GlobalCount + delta;
        return CounterConfig.GlobalCount + ScaledBonus + instanceBias;
    }

    public int VerifyFieldInitializers()
    {
        CounterConfig fresh = new CounterConfig();
        if (fresh.scaleFactor < 2.4 || fresh.scaleFactor > 2.6) return -1;
        if (fresh.seedId != 900) return -2;
        if (fresh.spawnOffset.x != FixedFromInt(3) || fresh.spawnOffset.y != FixedFromInt(4)) return -3;
        return 1;
    }
}

class ProductivitySuite
{
    public string FormatReport(string hero, int hp, bool alive)
    {
        const string prefix = "Hero";
        string nested = $"outer {$"inner {"}"}:{hero}"}";
        if (nested != "outer inner }:Aria")
        {
            return "bad_nested:" + nested;
        }
        string raw = $"  [{prefix}:{hero}] hp={hp + CounterConfig.BaseBonus} alive={alive} {{ok}}  ";
        string trimmed = raw.Trim();
        if (!trimmed.StartsWith("[Hero:") || !trimmed.EndsWith("{ok}"))
        {
            return "bad_affix";
        }
        if (!trimmed.Contains("hp=90") || trimmed.IndexOf("alive=true") < 0)
        {
            return "bad_content";
        }
        string replaced = trimmed.Replace("{ok}", "READY");
        string sub = replaced.Substring(1, 9).ToUpper();
        return sub + "|len=" + replaced.Length;
    }

    public int VerifyTrigAndQuat()
    {
        Fixed zero = Fixed.Zero();
        Fixed one = Fixed.One();
        Fixed half = Fixed.Half();
        Fixed halfPi = HalfPi();
        Fixed s0 = Sin(zero);
        Fixed c0 = Cos(zero);
        Fixed s90 = Sin(halfPi);
        Fixed c90 = Cos(halfPi);

        // s0 == 0, c0 == 1, s90 ~= 1, c90 ~= 0
        if (Abs(s0) > FixedFromRatio(1, 100)) return -1;
        if (Abs(c0 - one) > FixedFromRatio(1, 100)) return -2;
        if (Abs(s90 - one) > FixedFromRatio(2, 100)) return -3;
        if (Abs(c90) > FixedFromRatio(2, 100)) return -4;

        Fixed angle = Atan2(one, one); // ~ Pi / 4
        Fixed expectedQuarterPi = Pi() / FixedFromInt(4);
        if (Abs(angle - expectedQuarterPi) > FixedFromRatio(2, 100)) return -5;

        Fixed lerped = Lerp(FixedFromInt(10), FixedFromInt(30), half);
        if (lerped != FixedFromInt(20)) return -6;

        // Rotate (1, 0, 0) by 90 degrees around Z axis -> ~(0, 1, 0)
        Quat rotZ = QuatFromAxisAngle(new Vec3(zero, zero, one), halfPi);
        Quat normRot = QuatNormalize(rotZ);
        Vec3 rotated = QuatRotateVec3(normRot, new Vec3(one, zero, zero));
        if (Abs(rotated.x) > FixedFromRatio(3, 100)) return -7;
        if (Abs(rotated.y - one) > FixedFromRatio(3, 100)) return -8;

        return 42;
    }

    public int VerifyContainers()
    {
        List<int> list = new List<int>();
        if (!list.IsEmpty()) return -1;
        list.Add(10);
        list.Add(20);
        list.Add(10);
        list.Add(30);
        if (list.IsEmpty() || list.First() != 10 || list.Last() != 30) return -2;
        if (list.LastIndexOf(10) != 2) return -3;
        list.Reverse();
        int[] listArr = list.ToArray();
        if (listArr.Length != 4 || listArr.Count != 4 || listArr[0] != 30 || listArr[3] != 10) return -4;

        Dictionary<string, int> dict = new Dictionary<string, int>();
        if (!dict.IsEmpty()) return -5;
        dict.Put("alpha", 11);
        dict.Put("beta", 22);
        if (!dict.ContainsValue(22) || dict.ContainsValue(99)) return -6;
        int foundVal = 0;
        if (!dict.TryGetValue("beta", out foundVal) || foundVal != 22) return -7;
        if (dict.TryGetValue("missing", out foundVal) || foundVal != 0) return -8;
        if (dict.GetOrDefault("missing", 77) != 77 || dict.GetOrDefault("alpha", 77) != 11) return -9;
        string[] keys = dict.Keys();
        int[] vals = dict.Values();
        if (keys.Length != 2 || vals.Length != 2) return -10;

        Queue<int> queue = new Queue<int>();
        if (!queue.IsEmpty()) return -11;
        queue.Enqueue(5);
        queue.Enqueue(9);
        if (!queue.Contains(9) || queue.Contains(3)) return -12;
        int[] qArr = queue.ToArray();
        if (qArr.Length != 2 || qArr[0] != 5 || qArr[1] != 9) return -13;

        Stack<int> stack = new Stack<int>();
        if (!stack.IsEmpty()) return -14;
        stack.Push(4);
        stack.Push(8);
        if (!stack.Contains(4) || stack.Contains(99)) return -15;
        int[] sArr = stack.ToArray();
        if (sArr.Length != 2 || sArr[0] != 8 || sArr[1] != 4) return -16;

        HashSet<int> uniqueSet = new HashSet<int>();
        if (!uniqueSet.IsEmpty()) return -17;
        uniqueSet.Add(100);
        uniqueSet.Add(200);
        int[] setArr = uniqueSet.ToArray();
        if (setArr.Length != 2 || setArr[0] != 100 || setArr[1] != 200) return -18;

        return listArr[0] + foundVal + dict.GetOrDefault("alpha", 0) + qArr[1] + sArr[0] + setArr[1];
    }
}
)";

    realscript::game::GameScriptCompiler compiler(api);
    auto compiled = compiler.compile({{"boost_stage2.rs", source}});
    require(
        compiled.succeeded(),
        "boost_stage2 compile failed:\n" + diagnosticsText(compiled.diagnostics));

    realscript::game::ScriptRuntime runtime(compiled.program);
    realscript::runtime::RuntimeError error;

    const auto cfgType = runtime.findType("Game.BoostStage2::CounterConfig");
    require(cfgType.has_value(), "CounterConfig not found");
    auto cfgA = runtime.createObject(*cfgType, {}, error);
    auto cfgB = runtime.createObject(*cfgType, {}, error);
    require(cfgA.has_value() && cfgB.has_value(), "failed to create CounterConfig instances");
    const auto bumpMethod = runtime.findMethod(*cfgType, "Bump", 1);
    require(bumpMethod.has_value(), "CounterConfig.Bump not found");

    // First call: GlobalCount becomes 100 + 5 = 105; returns 105 + 35 + 7 = 147
    const auto bump1 = runtime.invoke(*cfgA, *bumpMethod, {std::int64_t{5}});
    require(
        bump1.succeeded && std::get<std::int64_t>(bump1.value) == 147,
        "CounterConfig.Bump(5) expected 147, got " +
            (bump1.succeeded ? std::to_string(std::get<std::int64_t>(bump1.value)) : bump1.error.message));

    // Second call on a DIFFERENT instance: GlobalCount becomes 105 + 15 = 120; returns 120 + 35 + 7 = 162
    const auto bump2 = runtime.invoke(*cfgB, *bumpMethod, {std::int64_t{15}});
    require(
        bump2.succeeded && std::get<std::int64_t>(bump2.value) == 162,
        "CounterConfig.Bump(15) on second instance should share static GlobalCount and return 162");

    // Verify host C++ getMember/setMember on const and static fields
    const auto constVal = runtime.getMember(*cfgA, "ScaledBonus", error);
    require(
        constVal.has_value() && std::get<std::int64_t>(*constVal) == 35,
        "getMember on const field ScaledBonus expected 35");
    realscript::runtime::RuntimeError constMutErr;
    require(
        !runtime.setMember(*cfgA, "ScaledBonus", std::int64_t{999}, constMutErr) &&
            constMutErr.code == realscript::runtime::ErrorCode::InvalidArguments,
        "setMember on const field ScaledBonus must be rejected with InvalidArguments");

    const auto staticVal = runtime.getMember(*cfgA, "GlobalCount", error);
    require(
        staticVal.has_value() && std::get<std::int64_t>(*staticVal) == 120,
        "getMember on static field GlobalCount expected 120");
    require(
        runtime.setMember(*cfgA, "GlobalCount", std::int64_t{500}, error),
        "setMember on static field GlobalCount failed: " + error.message);
    const auto staticValB = runtime.getMember(*cfgB, "GlobalCount", error);
    require(
        staticValB.has_value() && std::get<std::int64_t>(*staticValB) == 500,
        "getMember on second instance for static field GlobalCount expected 500");
    const auto bump3 = runtime.invoke(*cfgB, *bumpMethod, {std::int64_t{10}});
    require(
        bump3.succeeded && std::get<std::int64_t>(bump3.value) == (510 + 35 + 7),
        "Bump(10) after host setMember(GlobalCount, 500) expected 552");

    const auto verifyInitMethod = runtime.findMethod(*cfgType, "VerifyFieldInitializers", 0);
    require(verifyInitMethod.has_value(), "CounterConfig.VerifyFieldInitializers not found");
    const auto verifyInitRes = runtime.invoke(*cfgA, *verifyInitMethod, {});
    require(
        verifyInitRes.succeeded && std::get<std::int64_t>(verifyInitRes.value) == 1,
        "CounterConfig.VerifyFieldInitializers failed: " +
            (verifyInitRes.succeeded ? std::to_string(std::get<std::int64_t>(verifyInitRes.value)) : verifyInitRes.error.message));

    realscript::game::ScriptMetadataRegistry stage2Meta;
    stage2Meta.populateFromProgram(compiled.program);
    const auto cfgSchema = realscript::game::inspectTypeSchema(
        runtime, stage2Meta, "Game.BoostStage2::CounterConfig");
    require(
        cfgSchema.has_value() && cfgSchema->fields.size() == 4,
        "CounterConfig InspectorTypeSchema should include 4 instance fields and exclude const/static fields");
    realscript::game::ScriptStatePolicy jsonPolicy;
    jsonPolicy.allowDoubles = true;
    const std::string cfgJson =
        realscript::game::exportScriptObjectJson(runtime, *cfgA, error, jsonPolicy);
    require(
        !cfgJson.empty() &&
            cfgJson.find("BaseBonus") == std::string::npos &&
            cfgJson.find("GlobalCount") == std::string::npos &&
            cfgJson.find("instanceBias") != std::string::npos,
        "exportScriptObjectJson should exclude const/static fields");

    const auto suiteType = runtime.findType("Game.BoostStage2::ProductivitySuite");
    require(suiteType.has_value(), "ProductivitySuite not found");
    auto suite = runtime.createObject(*suiteType, {}, error);
    require(suite.has_value(), "failed to create ProductivitySuite");

    const auto formatMethod = runtime.findMethod(*suiteType, "FormatReport", 3);
    require(formatMethod.has_value(), "FormatReport not found");
    const auto reportRes = runtime.invoke(
        *suite,
        *formatMethod,
        {std::string("Aria"), std::int64_t{80}, true});
    std::string reportStr;
    if (reportRes.succeeded) {
        if (const auto* s = std::get_if<std::string>(&reportRes.value)) {
            reportStr = *s;
        } else if (const auto* ref = std::get_if<realscript::runtime::ObjectRef>(&reportRes.value)) {
            if (const auto sv = runtime.heap()->stringView(*ref)) {
                reportStr = std::string(*sv);
            }
        }
    }
    require(
        reportRes.succeeded && reportStr == "HERO:ARIA|len=34",
        "FormatReport interpolation/string methods mismatch: " +
            (reportRes.succeeded ? reportStr : reportRes.error.message));

    const auto trigMethod = runtime.findMethod(*suiteType, "VerifyTrigAndQuat", 0);
    require(trigMethod.has_value(), "VerifyTrigAndQuat not found");
    const auto trigRes = runtime.invoke(*suite, *trigMethod, {});
    require(
        trigRes.succeeded && std::get<std::int64_t>(trigRes.value) == 42,
        "VerifyTrigAndQuat failed with code: " +
            (trigRes.succeeded ? std::to_string(std::get<std::int64_t>(trigRes.value)) : trigRes.error.message));

    const auto containersMethod = runtime.findMethod(*suiteType, "VerifyContainers", 0);
    require(containersMethod.has_value(), "VerifyContainers not found");
    const auto containersRes = runtime.invoke(*suite, *containersMethod, {});
    // 30 + 0 + 11 + 9 + 8 + 200 = 258
    require(
        containersRes.succeeded && std::get<std::int64_t>(containersRes.value) == 258,
        "VerifyContainers failed with code: " +
            (containersRes.succeeded
                 ? std::to_string(std::get<std::int64_t>(containersRes.value))
                 : containersRes.error.message));
}

void testBoostStage3LspExactSymbolIdReferencesRenameAndChainedCompletion() {
    realscript::tooling::LanguageService service;
    const std::string source = R"(module Game.LspStage3;

class Weapon
{
    public int damage;
    public string title;

    public string GetTitle()
    {
        return title;
    }
}

class Hero
{
    public int hp;
    public Weapon weapon;

    public void TakeDamage(int amount)
    {
        hp = hp - amount;
    }
}

class Monster
{
    public int hp;
    public int unreferencedHp;

    public void Reset()
    {
        hp = 100;
    }
}

class Party
{
    public Hero leader;
    public Hero[] members;
    public int unreferencedHp;

    public Hero GetLeader()
    {
        return leader;
    }

    public int ComputeLocal(Party party)
    {
        int hp = 50;
        hp = hp + 10;
        unreferencedHp = 1;
        party.leader.weapon.
        party.GetLeader().weapon.title.
        (party).members[0].weapon.
        return hp;
    }
}
)";
    service.open("stage3_lsp.rs", source, 1);

    // 1. Exact SymbolId references & rename on Hero.hp (line 15: "    public int hp;")
    //    Hero.hp is declared on line 15, col 15, and referenced twice on line 20 ("        hp = hp - amount;").
    //    Monster.hp (line 26, 31) and local `int hp` (line 48, 49, 54) MUST NOT be included!
    const auto heroHpRefs = service.references("stage3_lsp.rs", {15, 15}, true);
    require(
        heroHpRefs.size() == 3,
        "Hero.hp references(includeDefinition=true) should be 3 (1 decl + 2 uses), got " +
            std::to_string(heroHpRefs.size()));
    for (const auto& loc : heroHpRefs) {
        require(
            loc.range.start.line == 15 || loc.range.start.line == 20,
            "Hero.hp reference leaked to unrelated symbol on line " +
                std::to_string(loc.range.start.line));
    }

    const auto heroHpRename = service.rename("stage3_lsp.rs", {20, 8}, "hitPoints");
    require(
        heroHpRename.size() == 3,
        "Hero.hp rename from usage site should produce 3 edits");

    // 1b. Unreferenced field Monster.unreferencedHp (line 27, col 15) MUST NOT fall back to lexical token scanning
    //     and leak to Party.unreferencedHp (line 39, 50)!
    const auto monsterUnrefRefsNoDef = service.references("stage3_lsp.rs", {27, 15}, false);
    require(
        monsterUnrefRefsNoDef.empty(),
        "unreferenced Monster.unreferencedHp references(includeDefinition=false) must be empty, got " +
            std::to_string(monsterUnrefRefsNoDef.size()));
    const auto monsterUnrefRename = service.rename("stage3_lsp.rs", {27, 15}, "monsterOnlyHp");
    require(
        monsterUnrefRename.size() == 1 && monsterUnrefRename[0].location.range.start.line == 27,
        "renaming unreferenced Monster.unreferencedHp must only edit its own declaration on line 27");

    // 2. Exact SymbolId references on local `hp` in Party.ComputeLocal (line 48: "        int hp = 50;")
    //    Declared on line 48, used twice on line 49, and once on line 54 -> total 4 with definition, 3 without.
    const auto localHpRefsNoDef = service.references("stage3_lsp.rs", {48, 12}, false);
    require(
        localHpRefsNoDef.size() == 3,
        "local hp references(includeDefinition=false) should be 3, got " +
            std::to_string(localHpRefsNoDef.size()));
    for (const auto& loc : localHpRefsNoDef) {
        require(
            loc.range.start.line == 49 || loc.range.start.line == 54,
            "local hp reference leaked to line " + std::to_string(loc.range.start.line));
    }

    // 3. Chained dot completion:
    //    Line 51: "        party.leader.weapon." (column 28) -> Weapon members (damage, title, GetTitle)
    const auto weaponChainItems = service.completion("stage3_lsp.rs", {51, 28});
    bool foundDamage = false;
    bool foundGetTitle = false;
    for (const auto& item : weaponChainItems) {
        if (item.label == "damage") foundDamage = true;
        if (item.label == "GetTitle") foundGetTitle = true;
    }
    require(
        foundDamage && foundGetTitle,
        "chained completion 'party.leader.weapon.' should resolve Weapon members");

    //    Line 52: "        party.GetLeader().weapon.title." (column 39) -> string members (Trim, ToUpper, Length)
    const auto stringChainItems = service.completion("stage3_lsp.rs", {52, 39});
    bool foundTrim = false;
    bool foundToUpper = false;
    bool foundLength = false;
    for (const auto& item : stringChainItems) {
        if (item.label == "Trim") foundTrim = true;
        if (item.label == "ToUpper") foundToUpper = true;
        if (item.label == "Length") foundLength = true;
    }
    require(
        foundTrim && foundToUpper && foundLength,
        "chained completion 'party.GetLeader().weapon.title.' should resolve string methods/properties");

    //    Line 53: "        (party).members[0].weapon." (column 34) -> Weapon members via parenthesized receiver + array indexing
    const auto indexedChainItems = service.completion("stage3_lsp.rs", {53, 34});
    bool foundIndexedDamage = false;
    for (const auto& item : indexedChainItems) {
        if (item.label == "damage") foundIndexedDamage = true;
    }
    require(
        foundIndexedDamage,
        "chained completion '(party).members[0].weapon.' should resolve Weapon members through parenthesized receiver and array element indexing");
}

void testCompleteEvolutionMilestones() {
    using namespace realscript;

    // 1. O2 MIR Function Inlining, LICM, and Struct Field Forwarding (SROA)
    {
        const char* optSrc = R"RS(
module Evolution.Opt;

struct DamageInfo {
    int baseDmg;
    int bonusDmg;
}

int computeLeafBonus(int x) {
    return x * 2 + 5;
}

int runOptimizedLoop(int n, double scale) {
    DamageInfo info = new DamageInfo { baseDmg = 10, bonusDmg = 20 };
    int forwarded = info.baseDmg + info.bonusDmg;
    int acc = 0;
    int i = 0;
    while (i < n) {
        double inv = scale * 2.5 + 1.0;
        if (inv > 0.0) {
            acc = acc + computeLeafBonus(forwarded);
        }
        i = i + 1;
    }
    return acc;
}
)RS";
        compiler::Compilation comp;
        comp.addSource({"opt_test.rs", optSrc});
        auto build = comp.build();
        require(!build.diagnostics.hasErrors(), "Evolution.Opt failed to compile");

        diagnostics::DiagnosticBag optDiag;
        optimization::Optimizer optimizer;
        optimization::Options opts;
        opts.level = optimization::Level::Aggressive;
        auto optimized = optimizer.optimize(build.modules, optDiag, opts);
        if (optDiag.hasErrors()) { for (const auto& d : optDiag.items()) std::cerr << d.code << ": " << d.message << "\n"; }
        require(!optDiag.hasErrors(), "Evolution.Opt O2 optimization emitted errors");
        require(optimized.statistics.functionsInlined >= 1,
            "O2 optimizer should inline computeLeafBonus");
        require(optimized.statistics.structFieldsForwarded >= 2,
            "O2 optimizer should forward DamageInfo struct fields");
        require(optimized.statistics.loopInvariantsHoisted >= 1,
            "O2 optimizer should hoist loop-invariant double expression");

        bytecode::Lowerer lowerer;
        std::vector<bytecode::Module> bc;
        for (const auto& m : optimized.modules) bc.push_back(lowerer.lower(m));
        runtime::Interpreter interp(std::move(bc));
        const auto res = interp.invoke("Evolution.Opt::runOptimizedLoop", {std::int64_t{4}, 2.0});
        require(res.succeeded && std::get<std::int64_t>(res.value) == 4 * (30 * 2 + 5),
            "Evolution.Opt result mismatch");
    }

    // 2. Typed AOT expansion for Enum, Object fields, Array elements, and UnboxedCustomStruct
    {
        const char* aotSrc = R"RS(
module Evolution.TypedAot;

enum StateKind {
    Idle = 0,
    Active = 2
}

struct StatBlock {
    int attack;
    int defense;
}

class UnitActor {
    public int hp;
    public UnitActor(int initHp) {
        hp = initHp;
    }
}

int evaluateActorAndArray(UnitActor actor, int[] scores, StateKind st) {
    StatBlock sb = new StatBlock { attack = 15, defense = 5 };
    int total = sb.attack + sb.defense;
    if (st == StateKind.Active) {
        actor.hp = actor.hp + total;
    }
    int i = 0;
    while (i < scores.Length) {
        scores[i] = scores[i] + actor.hp;
        total = total + scores[i];
        i = i + 1;
    }
    return total;
}
)RS";
        compiler::Compilation comp;
        comp.addSource({"typed_aot_obj.rs", aotSrc});
        auto build = comp.build();
        require(!build.diagnostics.hasErrors(), "Evolution.TypedAot failed to compile");

        diagnostics::DiagnosticBag aotDiag;
        aot::CppGenerator gen;
        aot::GenerationOptions genOpts;
        genOpts.programName = "EvolutionTypedAotObj";
        auto generated = gen.generate(build.modules, aotDiag, genOpts);
        require(!aotDiag.hasErrors(), "Evolution.TypedAot C++ codegen failed");
        require(
            generated.source.find("_scalar(") != std::string::npos && generated.source.find("context.initUnboxedStruct") != std::string::npos,
            "evaluateActorAndArray (with Object, Array, Enum, and custom Struct) must emit _scalar fast-path");
    }

    // 3. Packed Array Bulk Transfer & Generational GC Nursery (`collectYoung`) & `using` statement
    {
        const char* usingSrc = R"RS(
module Evolution.ResourceAndGc;

public class DisposableBuffer {
    public static int disposeCount = 0;
    public int id;

    public DisposableBuffer(int v) {
        id = v;
    }

    public void Dispose() {
        disposeCount = disposeCount + id;
    }
}

public int runUsingNormal(int v) {
    using (DisposableBuffer buf = new DisposableBuffer(v)) {
        return buf.id * 10;
    }
}

public int runUsingEarlyReturn(int v) {
    using (DisposableBuffer buf = new DisposableBuffer(v)) {
        if (v > 0) {
            return buf.id + 100;
        }
    }
    return 0;
}
)RS";
        game::GameApi api;
        game::GameScriptCompiler compiler(api);
        auto compiled = compiler.compile({{"using_test.rs", usingSrc}});
        require(compiled.succeeded(), "Evolution.ResourceAndGc failed to compile");
        game::ScriptRuntime rt(compiled.program);

        runtime::Interpreter interp(compiled.program.program(), compiled.program.heap());
        const auto r1 = interp.invoke("Evolution.ResourceAndGc::runUsingNormal", {std::int64_t{7}});
        require(r1.succeeded && std::get<std::int64_t>(r1.value) == 70, "runUsingNormal failed");
        const auto r2 = interp.invoke("Evolution.ResourceAndGc::runUsingEarlyReturn", {std::int64_t{5}});
        require(r2.succeeded && std::get<std::int64_t>(r2.value) == 105, "runUsingEarlyReturn failed");

        // Verify Generational GC nursery collection & promotion
        auto heap = compiled.program.heap();
        runtime::RuntimeError heapErr;
        auto transientStr = heap->allocateString("transient_nursery_string", &heapErr);
        auto survivedStr = heap->allocateString("survived_nursery_string", &heapErr);
        require(transientStr.has_value() && survivedStr.has_value(), "nursery string alloc failed");
        auto root = heap->retain(*survivedStr);
        require(heap->nurserySize() >= 2, "nursery should contain newly allocated objects");

        runtime::ShadowStack emptyStack;
        const std::size_t processed = heap->collectYoung(emptyStack);
        require(processed >= 2, "collectYoung should process nursery slots");
        require(!heap->isAlive(*transientStr), "unrooted nursery object should be reclaimed by collectYoung");
        require(heap->isAlive(*survivedStr), "rooted nursery object should survive collectYoung and be promoted");
        require(heap->statistics().youngCollections >= 1 &&
                heap->statistics().youngObjectsReclaimed >= 1 &&
                heap->statistics().objectsPromoted >= 1,
            "GcStatistics should record youngCollections, youngObjectsReclaimed, and objectsPromoted");

        // Verify Packed Array Bulk Transfer
        auto intArr = heap->allocateTypedArray(
            0, semantic::PrimitiveType::Int, 0, 4, std::int64_t{0}, &heapErr);
        require(intArr.has_value(), "failed to allocate int array");
        std::vector<std::int64_t> srcInts{10, 20, 30, 40};
        require(rt.writeArrayInt(*intArr, srcInts, heapErr), "writeArrayInt failed");
        std::vector<std::int64_t> dstInts;
        require(rt.readArrayInt(*intArr, dstInts, heapErr) && dstInts == srcInts,
            "readArrayInt round-trip mismatch");

        auto vec2Arr = heap->allocateTypedArray(
            0, semantic::PrimitiveType::Struct, runtime::vec2TypeId(), 2,
            runtime::Vec2Value{0, 0}, &heapErr);
        require(vec2Arr.has_value(), "failed to allocate Vec2 array");
        std::vector<game::Vec2> srcVecs{game::Vec2::of(3, 4), game::Vec2::of(5, 6)};
        require(rt.writeArrayVec2(*vec2Arr, srcVecs, heapErr), "writeArrayVec2 failed");
        std::vector<game::Vec2> dstVecs;
        require(rt.readArrayVec2(*vec2Arr, dstVecs, heapErr) && dstVecs == srcVecs,
            "readArrayVec2 round-trip mismatch");
        require(!rt.writeArrayInt(*vec2Arr, srcInts, heapErr) &&
                heapErr.code == runtime::ErrorCode::TypeMismatch,
            "writeArrayInt on a Vec2[] array must be rejected with TypeMismatch");
    }

    // 4. Fixed64 (Q32.32), Trigonometric Quat::slerp, and 3D Spatial Math (Vec4, Mat4, Transform3D, AABB, Ray)
    {
        const auto f64A = game::Fixed64::fromInt(100000);
        const auto f64B = game::Fixed64::fromInt(3);
        const auto f64Prod = f64A * f64B;
        require(f64Prod.toIntFloor() == 300000, "Fixed64 multiplication failed");
        const auto f64Div = f64Prod / f64B;
        require(f64Div.toIntFloor() == 100000, "Fixed64 division failed");
        // Verify Fixed64 division by small fractional divisor (0 < rhs.raw < 65536) does not divide-by-zero
        const auto f64Tiny = game::Fixed64::fromRaw(1024); // 2^-22
        const auto f64One = game::Fixed64::fromInt(1);
        const auto f64TinyQuot = f64One / f64Tiny;
        require(f64TinyQuot.toIntFloor() == (1LL << 22),
            "Fixed64 division by sub-2^-16 fraction failed");

        const auto q0 = game::Quat::identity();
        const auto q90 = game::Quat::fromAxisAngle(game::Vec3::of(0, 0, 1), game::fixedHalfPi());
        const auto qMid = game::Quat::slerp(q0, q90, game::Fixed::fromRaw(128));
        const auto rotatedX = qMid.rotate(game::Vec3::of(1, 0, 0));
        require(rotatedX.x.raw > 170 && rotatedX.x.raw < 190 &&
                rotatedX.y.raw > 170 && rotatedX.y.raw < 190,
            "trigonometric Quat::slerp at t=0.5 of 90 deg Z rotation should rotate (1,0,0) to (~0.707, ~0.707, 0)");

        const game::Transform3D parentTf{
            game::Vec3::of(10, 0, 0),
            game::Quat::identity(),
            game::Vec3::of(2, 2, 2)};
        const game::Transform3D childTf{
            game::Vec3::of(3, 4, 0),
            game::Quat::identity(),
            game::Vec3::of(1, 1, 1)};
        const auto worldTf = game::Transform3D::combine(parentTf, childTf);
        require(worldTf.position == game::Vec3::of(16, 8, 0),
            "Transform3D::combine position mismatch");

        const auto box = game::AABB::fromCenterExtents(game::Vec3::of(10, 0, 0), game::Vec3::of(2, 2, 2));
        const game::Ray ray{game::Vec3::of(0, 0, 0), game::Vec3::of(1, 0, 0)};
        game::Fixed hitT{};
        require(ray.intersectsAABB(box, &hitT) && hitT == game::Fixed::fromInt(8),
            "Ray::intersectsAABB should hit box at t = 8");
    }

    // 5. Scene Transform Hierarchy, DefaultExecutionOrder, RequireComponent, Prefab Instantiation, and AsyncOperation (`wait_async`)
    {
        auto host = std::make_shared<game::GameplayHost>(60);
        game::GameApi api;
        require(game::installGameplayBindings(api, host), "installGameplayBindings failed");

        const char* sceneSrc = R"RS(
module Evolution.SceneHierarchy;
import RealScript.Game;
import RealScript.Math;

public class OrderTracker {
    public static int logValue = 0;
}

[DefaultExecutionOrder(-10)]
public class EarlySystem {
    public void OnUpdate(double dt) {
        OrderTracker.logValue = OrderTracker.logValue * 10 + 1;
    }
}

[DefaultExecutionOrder(20)]
[RequireComponent("EarlySystem")]
public class LateSystem {
    public int customPower = 1;
    public int asyncDoneValue = 0;

    public void OnUpdate(double dt) {
        OrderTracker.logValue = OrderTracker.logValue * 10 + 2;
    }

    public sequence RunAsyncLoad(long selfEntity, long opId) {
        yield wait_async(opId);
        asyncDoneValue = GetAsyncOperationResultInt(opId) + customPower;
    }
}
)RS";
        game::GameScriptCompiler compiler(api);
        auto compiled = compiler.compile({{"scene_hierarchy.rs", sceneSrc}});
        require(compiled.succeeded(), "Evolution.SceneHierarchy failed to compile");

        game::ScriptRuntime rt(compiled.program);
        game::SceneScriptRuntime scene(rt);

        // Verify [RequireComponent("EarlySystem")] blocks attaching LateSystem before EarlySystem
        require(!scene.attachComponent(1, "Evolution.SceneHierarchy::LateSystem"),
            "RequireComponent should reject attaching LateSystem when EarlySystem is missing");
        scene.clearErrors();

        // Instantiate prefab with hierarchy and overrides
        game::PrefabNodeTemplate rootPrefab;
        rootPrefab.name = "Root";
        rootPrefab.localTransform = game::Transform3D{
            game::Vec3::of(5, 10, 0), game::Quat::identity(), game::Vec3::of(1, 1, 1)};
        rootPrefab.components.push_back({"Evolution.SceneHierarchy::EarlySystem", {}});
        rootPrefab.components.push_back({"Evolution.SceneHierarchy::LateSystem", {}});

        game::PrefabNodeTemplate childPrefab;
        childPrefab.name = "Child";
        childPrefab.localTransform = game::Transform3D{
            game::Vec3::of(3, 4, 0), game::Quat::identity(), game::Vec3::of(1, 1, 1)};
        childPrefab.components.push_back({"Evolution.SceneHierarchy::EarlySystem", {}});
        rootPrefab.children.push_back(childPrefab);

        game::SceneEntityId nextId = 100;
        const auto rootId = scene.instantiatePrefab(
            rootPrefab, nextId, 0, {{"customPower", std::int64_t{42}}});
        require(rootId == 100, "instantiatePrefab rootId mismatch");
        const auto children = scene.getChildren(rootId);
        require(children.size() == 1 && children[0] == 101, "instantiatePrefab childId mismatch");
        require(scene.getParent(101) == 100, "getParent mismatch");
        require(scene.getWorldPosition2D(101) == game::Vec2::of(8, 14),
            "child world position should combine parent (5,10) + child (3,4) = (8,14)");
        require(scene.getComponentInParent(101, "LateSystem") != nullptr,
            "getComponentInParent should find LateSystem on parent entity 100");
        require(scene.getComponentInChildren(100, "EarlySystem") != nullptr,
            "getComponentInChildren should find EarlySystem");

        scene.start();
        scene.update(1.0 / 60.0);
        // Both entity 100 and 101 have EarlySystem (-10), and entity 100 has LateSystem (+20)
        // Execution order must be: EarlySystem(100) -> EarlySystem(101) -> LateSystem(100) => 112
        auto* lateObj = scene.getComponent(100, "LateSystem");
        require(lateObj != nullptr, "LateSystem not found on root");

        // Test deterministic AsyncOperation + yield wait_async(opId)
        const auto opId = host->createAsyncOperation("LoadLevelChunk");
        const auto startSeqRes = scene.invokeComponent(
            100, "LateSystem", "RunAsyncLoad",
            {runtime::LongValue{100}, runtime::LongValue{static_cast<std::int64_t>(opId)}});
        require(startSeqRes.succeeded, "RunAsyncLoad failed to start");

        game::SceneGameplayDriver driver(scene, host);
        (void)driver.step();
        runtime::RuntimeError memberErr;
        auto doneBefore = rt.getMember(*lateObj, "asyncDoneValue", memberErr);
        require(doneBefore && std::get<std::int64_t>(*doneBefore) == 0,
            "asyncDoneValue should be 0 before CompleteAsyncOperation");

        require(host->completeAsyncOperation(opId, game::GameplayValue{std::int32_t{58}}, 1),
            "completeAsyncOperation failed");
        (void)driver.step();
        auto doneAfter = rt.getMember(*lateObj, "asyncDoneValue", memberErr);
        require(doneAfter && std::get<std::int64_t>(*doneAfter) == 100,
            "wait_async should wake on tick boundary and compute 58 + 42 = 100");

        // Cascading detach of parent 100 must also detach child 101
        require(scene.detach(100), "detach(100) failed");
        require(scene.componentCount(101) == 0, "cascading detach should remove child 101");
    }

    // 6. Multi-threaded FiberScheduler::stepTickParallel determinism parity
    {
        std::vector<runtime::CompactInstruction> prog{
            {runtime::FiberOpcode::LoadInt, 0, 0, 10},
            {runtime::FiberOpcode::LoadInt, 0, 1, 20},
            {runtime::FiberOpcode::Add, 0, 2, 0, 1},
            {runtime::FiberOpcode::YieldTicks, 0, 0, 1},
            {runtime::FiberOpcode::Add, 0, 2, 2, 1},
            {runtime::FiberOpcode::Return, 0, 0, 0},
        };
        runtime::FiberScheduler seqSched;
        runtime::FiberScheduler parSched;
        for (std::uint32_t i = 1; i <= 16; ++i) {
            seqSched.spawnFiber(prog, 0, {i, 1}, static_cast<std::uint8_t>(i & 3u), 0, 12345u + i);
            parSched.spawnFiber(prog, 0, {i, 1}, static_cast<std::uint8_t>(i & 3u), 0, 12345u + i);
        }
        for (std::uint32_t tick = 1; tick <= 4; ++tick) {
            const auto seqIns = seqSched.stepTick(tick);
            const auto parIns = parSched.stepTickParallel(tick, 4);
            require(seqIns == parIns, "stepTickParallel instruction count diverged from stepTick");
            require(seqSched.computeLockstepDigest() == parSched.computeLockstepDigest(),
                "stepTickParallel lockstep digest diverged from stepTick");
        }
    }

    // 7. LSP Incremental Document Sync (`textDocumentSync: 2`) & C++ Header Auto-Binding Generator
    {
        tooling::LspServer lsp;
        std::ostringstream notifications;
        const auto initResp = lsp.handle(tooling::Json::Object{
            {"jsonrpc", "2.0"},
            {"id", 1},
            {"method", "initialize"},
            {"params", tooling::Json::Object{}},
        }, &notifications);
        const auto* syncKind = initResp.find("result")->find("capabilities")->find("textDocumentSync");
        require(syncKind && syncKind->integerValue() == 2,
            "LSP server should advertise textDocumentSync = 2 (Incremental)");

        tooling::LanguageService ls;
        ls.open("inc.rs", "module Inc;\nint val() {\n    return 10;\n}\n", 1);
        debug::SourceRange editRange{};
        editRange.start = {3, 12};
        editRange.end = {3, 14};
        require(ls.applyContentChanges("inc.rs", {{editRange, "42"}}, 2),
            "applyContentChanges failed");
        require(ls.documentContent("inc.rs").find("return 42;") != std::string::npos,
            "incremental range edit did not produce 'return 42;'");
        require(ls.diagnostics("inc.rs").empty(),
            "incrementally edited document should have zero diagnostics");

        const char* sampleHeader = R"CPP(
        RS_EXPORT_CLASS("Game.Native")
        class PhysicsBody {
        public:
            RS_EXPORT_METHOD
            void ApplyImpulse(const Vec2 &impulse);
            RS_EXPORT_METHOD
            float GetMass();
        };
        RS_EXPORT_FUNCTION("Game.Native")
        int StepPhysicsWorld(int subSteps);
        )CPP";
        const auto genBind = game::GameApi::generateBindingsFromCppHeader(sampleHeader, "Game.Native");
        require(genBind.classesExported == 1 &&
                genBind.methodsExported == 2 &&
                genBind.functionsExported == 1 &&
                genBind.cppRegistrationSource.find("PhysicsBody::ApplyImpulse") != std::string::npos &&
                genBind.scriptDeclarationStub.find("class PhysicsBody") != std::string::npos &&
                genBind.scriptDeclarationStub.find("void ApplyImpulse(Vec2 impulse)") != std::string::npos &&
                genBind.scriptDeclarationStub.find("float GetMass()") != std::string::npos,
            "generateBindingsFromCppHeader failed to generate C++ registration and .d.rs stub");
    }
}

} // namespace

void testModernEngineParityCompletionGoals() {
    using namespace realscript;

    // 1. Empty Transform Parent Node setEntityActive, decoupled setComponentEnabled,
    //    and Two-Phase Prefab Instantiation ([RequireComponent] order independence)
    {
        game::GameApi api;
        game::ScriptRuntime dummyRt{game::GameProgram{}};
        const char* src = R"RS(
module Evolution.ParityPhase1;
import RealScript.Scene;
import RealScript.Math;

class MotorComponent {
    public int speed = 15;
    public int enabledCount = 0;
    public int disabledCount = 0;

    public void OnEnable() { enabledCount = enabledCount + 1; }
    public void OnDisable() { disabledCount = disabledCount + 1; }
}

[RequireComponent("MotorComponent")]
class VehicleController {
    public int initSawMotor = 0;
    public int activeTicks = 0;

    public void OnCreate() {
        if (HasComponent(100, "MotorComponent") && HasComponent(101, "MotorComponent")) {
            initSawMotor = 1;
        }
    }

    public void OnUpdate(double dt) {
        activeTicks = activeTicks + 1;
    }
}
)RS";
        game::SceneScriptRuntime* sceneBindingTarget = nullptr;
        api.function("RealScript.Scene", "HasComponent", [&sceneBindingTarget]( std::int64_t entityId, std::string typeName) -> bool {
            if (!sceneBindingTarget || entityId <= 0) return false;
            return sceneBindingTarget->hasComponent(static_cast<game::SceneEntityId>(entityId), typeName);
        });

        game::GameScriptCompiler compiler(api);
        auto compiled = compiler.compile({{"parity_p1.rs", src}});
        require(compiled.succeeded(), "Evolution.ParityPhase1 failed to compile:\n" + diagnosticsText(compiled.diagnostics));

        game::ScriptRuntime rt(compiled.program);
        game::SceneScriptRuntime scene(rt);
        sceneBindingTarget = &scene;

        // Build prefab where VehicleController is listed BEFORE its required MotorComponent,
        // and also has a child entity 101 with MotorComponent.
        game::PrefabNodeTemplate rootPrefab;
        rootPrefab.name = "VehicleRoot";
        rootPrefab.localTransform = game::Transform3D{
            game::Vec3::of(10, 20, 0), game::Quat::identity(), game::Vec3::of(1, 1, 1)};
        rootPrefab.components.push_back({"Evolution.ParityPhase1::VehicleController", {}});
        rootPrefab.components.push_back({"Evolution.ParityPhase1::MotorComponent", {}});

        game::PrefabNodeTemplate wheelPrefab;
        wheelPrefab.name = "WheelChild";
        wheelPrefab.localTransform = game::Transform3D{
            game::Vec3::of(2, 3, 0), game::Quat::identity(), game::Vec3::of(1, 1, 1)};
        wheelPrefab.components.push_back({"Evolution.ParityPhase1::MotorComponent", {}});
        rootPrefab.children.push_back(wheelPrefab);

        // Create an empty transform group node (entity 50) with 0 components as parent
        scene.setLocalPosition2D(50, game::Vec2::of(100, 200));
        game::SceneEntityId nextId = 100;
        const auto rootId = scene.instantiatePrefab(rootPrefab, nextId, 50);
        require(rootId == 100, "Two-phase instantiatePrefab should succeed even when RequireComponent target is listed second");

        auto* ctrlObj = scene.getComponent(100, "VehicleController");
        auto* motorObj = scene.getComponent(100, "MotorComponent");
        require(ctrlObj && motorObj, "VehicleController and MotorComponent should exist on entity 100");

        runtime::RuntimeError err;
        auto sawMotor = rt.getMember(*ctrlObj, "initSawMotor", err);
        require(sawMotor && std::get<std::int64_t>(*sawMotor) == 1,
            "Two-phase instantiatePrefab should allocate all root and child components before invoking OnCreate");

        // Verify dirty-cached world transform across 3-level hierarchy (50 -> 100 -> 101)
        require(scene.getWorldPosition2D(101) == game::Vec2::of(112, 223),
            "3-level world transform (100,200)+(10,20)+(2,3) should be (112,223)");
        scene.setLocalPosition2D(50, game::Vec2::of(10, 10));
        require(scene.getWorldPosition2D(101) == game::Vec2::of(22, 33),
            "invalidated world transform cache should recompute (22, 33)");

        // Decoupled setComponentEnabled: disabling MotorComponent on 100 must NOT disable VehicleController or entityActiveSelf
        require(scene.setComponentEnabled(100, "MotorComponent", false), "setComponentEnabled(false) failed");
        require(!scene.isComponentEnabled(100, "MotorComponent"), "MotorComponent should be disabled");
        require(scene.isEntityActiveSelf(100), "Entity 100 should remain active when one component is disabled");

        scene.start();
        scene.update(0.016);
        auto ticksAfter1 = rt.getMember(*ctrlObj, "activeTicks", err);
        require(ticksAfter1 && std::get<std::int64_t>(*ticksAfter1) == 1,
            "VehicleController should still tick while sibling MotorComponent is disabled");

        // Empty transform parent node (entity 50) setEntityActive(false) must cascade to 100 and 101
        require(scene.setEntityActive(50, false), "setEntityActive(50, false) on empty transform node failed");
        require(!scene.isActiveInHierarchy(100) && !scene.isActiveInHierarchy(101),
            "children of deactivated empty parent node 50 must be inactive in hierarchy");
        scene.update(0.016);
        auto ticksWhileInactive = rt.getMember(*ctrlObj, "activeTicks", err);
        require(ticksWhileInactive && std::get<std::int64_t>(*ticksWhileInactive) == 1,
            "VehicleController must not tick while parent empty node 50 is inactive");

        require(scene.setEntityActive(50, true), "setEntityActive(50, true) failed");
        require(scene.isActiveInHierarchy(100), "entity 100 should be active again");
    }

    // 2. Signal disconnectSignal, Auto-Disconnect on Detach, Whole-Scene JSON Serialization,
    //    and Deep Array/Struct Reference Patching on Hot Reload
    {
        game::GameApi api;
        const char* srcV1 = R"RS(
module Evolution.SceneGraphV1;

delegate void AlertHandler(int code);

class ListenerComp {
    public int receivedSum = 0;

    public void OnAlert(int code) {
        receivedSum = receivedSum + code;
    }
}

struct TargetHolder {
    public ListenerComp primary;
}

class HubComp {
    public event AlertHandler OnAlertSignal;
    public ListenerComp[] listeners;
    public TargetHolder holder;

    public void SetupLinks(ListenerComp a, ListenerComp b) {
        listeners = new ListenerComp[2];
        listeners[0] = a;
        listeners[1] = b;
        TargetHolder th = new TargetHolder();
        th.primary = a;
        holder = th;
    }

    public void Broadcast(int code) {
        OnAlertSignal(code);
    }

    public int PingArrayAndStruct(int bonus) {
        listeners[0].OnAlert(bonus);
        holder.primary.OnAlert(bonus);
        return listeners[0].receivedSum + listeners[1].receivedSum;
    }
}
)RS";
        game::GameScriptCompiler compiler(api);
        auto compiledV1 = compiler.compile({{"scene_v1.rs", srcV1}});
        require(compiledV1.succeeded(), "SceneGraphV1 failed to compile:\n" + diagnosticsText(compiledV1.diagnostics));

        game::ScriptRuntime rt(compiledV1.program);
        game::SceneScriptRuntime scene(rt);

        require(scene.attachComponent(1, "Evolution.SceneGraphV1::HubComp"), "attach HubComp failed");
        require(scene.attachComponent(2, "Evolution.SceneGraphV1::ListenerComp"), "attach Listener 2 failed");
        require(scene.attachComponent(3, "Evolution.SceneGraphV1::ListenerComp"), "attach Listener 3 failed");
        require(scene.setParent(2, 1) && scene.setParent(3, 1), "setParent failed");
        scene.setLocalPosition2D(1, game::Vec2::of(5, 6));
        scene.setLocalPosition2D(2, game::Vec2::of(1, 2));

        auto* l2 = scene.getComponent(2, "ListenerComp");
        auto* l3 = scene.getComponent(3, "ListenerComp");
        require(l2 && l3, "listeners missing");
        const auto setupRes = scene.invokeComponent(1, "HubComp", "SetupLinks", {l2->value(), l3->value()});
        require(setupRes.succeeded, "SetupLinks failed: " + setupRes.error.message);

        require(scene.connectSignal(1, "HubComp", "OnAlertSignal", 2, "ListenerComp", "OnAlert"),
            "connectSignal 1->2 failed");
        require(scene.connectSignal(1, "HubComp", "OnAlertSignal", 3, "ListenerComp", "OnAlert"),
            "connectSignal 1->3 failed");

        require(scene.invokeComponent(1, "HubComp", "Broadcast", {std::int64_t{10}}).succeeded,
            "Broadcast(10) failed");

        // Export whole scene to JSON and restore into a brand new SceneScriptRuntime
        runtime::RuntimeError jsonErr;
        game::ScriptStatePolicy policy;
        policy.allowDoubles = true;
        const std::string sceneJson = game::exportSceneJson(scene, jsonErr, policy);
        require(!sceneJson.empty(), "exportSceneJson failed: " + jsonErr.message);

        game::ScriptRuntime rtRestored(compiledV1.program);
        game::SceneScriptRuntime restoredScene(rtRestored);
        require(game::importSceneJson(restoredScene, sceneJson, jsonErr, policy),
            "importSceneJson failed: " + jsonErr.message);
        require(restoredScene.getParent(2) == 1 &&
                restoredScene.getWorldPosition2D(2) == game::Vec2::of(6, 8),
            "restored scene hierarchy/transform mismatch");
        require(restoredScene.signalConnections().size() == 2,
            "restored scene should have 2 wired signal connections");

        // Verify restored signals fire properly
        require(restoredScene.invokeComponent(1, "HubComp", "Broadcast", {std::int64_t{5}}).succeeded,
            "Broadcast on restoredScene failed");
        auto* rl2 = restoredScene.getComponent(2, "ListenerComp");
        auto* rl3 = restoredScene.getComponent(3, "ListenerComp");
        runtime::RuntimeError mErr;
        require(std::get<std::int64_t>(*rtRestored.getMember(*rl2, "receivedSum", mErr)) == 15 &&
                std::get<std::int64_t>(*rtRestored.getMember(*rl3, "receivedSum", mErr)) == 15,
            "restored listeners should both have receivedSum == 15");

        // Test disconnectSignal on restoredScene
        require(restoredScene.disconnectSignal(1, "HubComp", "OnAlertSignal", 3, "ListenerComp", "OnAlert"),
            "disconnectSignal 1->3 failed");
        require(restoredScene.invokeComponent(1, "HubComp", "Broadcast", {std::int64_t{7}}).succeeded,
            "Broadcast after disconnectSignal failed");
        require(std::get<std::int64_t>(*rtRestored.getMember(*rl2, "receivedSum", mErr)) == 22 &&
                std::get<std::int64_t>(*rtRestored.getMember(*rl3, "receivedSum", mErr)) == 15,
            "only listener 2 should receive signal after listener 3 disconnected");

        // Test automatic signal disconnection when target entity 2 is detached
        require(restoredScene.detach(2), "detach(2) failed");
        require(restoredScene.signalConnections().empty(),
            "detaching entity 2 should automatically clean up remaining signal connection");
        require(restoredScene.invokeComponent(1, "HubComp", "Broadcast", {std::int64_t{9}}).succeeded,
            "Broadcast after detaching all listeners should succeed cleanly");

        // Now test Hot Reload on original `scene` with Array and Struct ObjectRef patching + Signal Rebinding
        const char* srcV2 = R"RS(
module Evolution.SceneGraphV1;

delegate void AlertHandler(int code);

class ListenerComp {
    public int receivedSum = 0;
    public int scaleFactor = 2;

    public void OnAlert(int code) {
        receivedSum = receivedSum + code * scaleFactor;
    }
}

struct TargetHolder {
    public ListenerComp primary;
}

class HubComp {
    public event AlertHandler OnAlertSignal;
    public ListenerComp[] listeners;
    public TargetHolder holder;

    public void SetupLinks(ListenerComp a, ListenerComp b) {
        listeners = new ListenerComp[2];
        listeners[0] = a;
        listeners[1] = b;
        TargetHolder th = new TargetHolder();
        th.primary = a;
        holder = th;
    }

    public void Broadcast(int code) {
        OnAlertSignal(code);
    }

    public int PingArrayAndStruct(int bonus) {
        listeners[0].OnAlert(bonus);
        holder.primary.OnAlert(bonus);
        return listeners[0].receivedSum + listeners[1].receivedSum;
    }
}
)RS";
        auto compiledV2 = compiler.compile({{"scene_v2.rs", srcV2}});
        require(compiledV2.succeeded(), "SceneGraphV2 failed to compile");

        // Check structural hot_reload::prepare with ReloadOptions{true}
        const auto strictPlan = hot_reload::prepare(*compiledV1.program.program(), compiledV2.modules);
        require(!strictPlan.compatible, "strict body-only hot_reload::prepare should reject added field");
        const auto structPlan = hot_reload::prepare(
            *compiledV1.program.program(),
            compiledV2.modules,
            hot_reload::ReloadOptions{true, false});
        require(structPlan.compatible && structPlan.structuralMigrationRequired &&
                !structPlan.layoutChangedTypes.empty(),
            "structural hot_reload::prepare should succeed and report layoutChangedTypes");

        const auto reloadRep = scene.hotReload(compiledV2.program);
        require(reloadRep.succeeded && reloadRep.signalsRebound == 1,
            "scene.hotReload failed or did not rebind signal chain");

        // Verify both wired signal and Array/Struct inner ObjectRef point to the migrated V2 instances
        require(scene.invokeComponent(1, "HubComp", "Broadcast", {std::int64_t{5}}).succeeded,
            "Broadcast after hotReload failed");
        const auto pingRes = scene.invokeComponent(1, "HubComp", "PingArrayAndStruct", {std::int64_t{3}});
        require(pingRes.succeeded && std::get<std::int64_t>(pingRes.value) == 52,
            "PingArrayAndStruct after hotReload should update migrated V2 ListenerComp instances (20 + 6 + 6 + 20 = 52 or 62)");
    }

    // 3. Scene + FiberScheduler Bridge (SceneFiberHostAdapter & Auto-Kill on Detach)
    //    and Multi-line C++ Header Binding Generator with Enum & Property
    {
        const char* fiberScript = R"RS(
module Evolution.SceneFiber;
import RealScript.Fiber;

void PatrolRoutine() {
    while (true) {
        if (Fiber.Cond(0, 1, 10)) {
            Fiber.Command(1, 5, 25);
        }
        Fiber.Yield(1);
    }
}
)RS";
        compiler::Compilation fiberComp;
        fiberComp.addSource({"patrol_fiber.rs", fiberScript});
        auto fiberBuild = fiberComp.build();
        require(!fiberBuild.diagnostics.hasErrors() && !fiberBuild.modules.empty(),
            "Evolution.SceneFiber failed to compile");
        const auto fiberMod = runtime::FiberCompiler::compileModule(fiberBuild.modules.front());
        require(fiberMod.hasFunction("PatrolRoutine"), "PatrolRoutine missing in CompiledFiberModule");

        game::GameApi api;
        const char* unitScript = R"RS(
module Evolution.UnitComp;

class GuardUnit {
    public int energy = 12;
    public int attacks = 0;

    public void ExecuteAttack(int mode, int damage) {
        attacks = attacks + mode + damage;
    }
}
)RS";
        game::GameScriptCompiler unitCompiler(api);
        auto unitCompiled = unitCompiler.compile({{"guard.rs", unitScript}});
        require(unitCompiled.succeeded(), "Evolution.UnitComp failed to compile");

        game::ScriptRuntime rt(unitCompiled.program);
        game::SceneScriptRuntime scene(rt);
        require(scene.attachComponent(10, "Evolution.UnitComp::GuardUnit"), "attach GuardUnit failed");

        game::SceneFiberHostAdapter adapter(scene);
        adapter.registerConditionMember(1, "energy", "GuardUnit");
        adapter.registerCommandCallback(1, "ExecuteAttack", "GuardUnit");

        runtime::FiberScheduler scheduler(&adapter);
        scene.setFiberScheduler(&scheduler);

        const auto fid = scene.spawnEntityFiber(10, fiberMod, "PatrolRoutine");
        require(fid != 0 && scheduler.activeFiberCount() == 1, "spawnEntityFiber failed");

        (void)scene.stepFibers(1);
        auto* guardObj = scene.getComponent(10, "GuardUnit");
        runtime::RuntimeError err;
        auto attacksVal = rt.getMember(*guardObj, "attacks", err);
        require(attacksVal && std::get<std::int64_t>(*attacksVal) == 30,
            "SceneFiberHostAdapter should evaluate energy>=10 and invoke ExecuteAttack(5, 25) -> 30");

        // Detaching entity 10 must automatically terminate its bound fibers in FiberScheduler
        require(scene.detach(10), "detach(10) failed");
        require(scheduler.activeFiberCount() == 0,
            "detaching entity 10 should automatically kill its bound fibers in FiberScheduler");

        // Verify upgraded multi-line C++ Header Binding Generator (Enum, Property, Multi-line, Default Args)
        const char* complexHeader = R"CPP(
        // Sample multi-line header with comments, enum, property, and default args
        RS_EXPORT_ENUM
        enum class MotionMode : int {
            Grounded = 0,
            Airborne = 1,
            Swimming = 2
        };

        RS_EXPORT_CLASS("Game.Native")
        class CharacterMotor {
        public:
            RS_EXPORT_PROPERTY("MaxSpeed", GetMaxSpeed, SetMaxSpeed)
            RS_EXPORT_METHOD
            float GetMaxSpeed() const;

            RS_EXPORT_METHOD
            void SetMaxSpeed(float value);

            /* Multi-line method with const ref and default parameter */
            RS_EXPORT_METHOD
            Vec3 MoveCharacter(
                const Vec3& velocity,
                float deltaTime = 0.016f
            ) const;
        };
        )CPP";
        const auto bindGen = game::GameApi::generateBindingsFromCppHeader(complexHeader, "Game.Native");
        require(
            bindGen.enumsExported == 1 &&
            bindGen.classesExported == 1 &&
            bindGen.propertiesExported == 1 &&
            bindGen.methodsExported == 3 &&
            bindGen.scriptDeclarationStub.find("enum MotionMode") != std::string::npos &&
            bindGen.scriptDeclarationStub.find("float MaxSpeed") != std::string::npos &&
            bindGen.scriptDeclarationStub.find("Vec3 MoveCharacter(Vec3 velocity, float deltaTime)") != std::string::npos &&
            bindGen.cppRegistrationSource.find(".property<&CharacterMotor::GetMaxSpeed, &CharacterMotor::SetMaxSpeed>(\"MaxSpeed\")") != std::string::npos,
            "upgraded generateBindingsFromCppHeader failed on multi-line/enum/property header");

        // Verify inspectTypeSchema fallback (createPrototypeObject) on NativeType-derived class without 0-arg ctor
        game::GameApi nativeApi;
        nativeApi.type<NativeActor>("Engine.Core", "NativeActor")
            .method<&NativeActor::heal>("Heal");
        const char* derivedSrc = R"RS(
module Evolution.DerivedSchema;
import Engine.Core;

class ParameterizedHero : NativeActor {
    [Export]
    [Range(0, 500)]
    public int armor = 75;

    ParameterizedHero(int initialBonus) {
        Heal(initialBonus);
    }
}
)RS";
        game::GameScriptCompiler derivedComp(nativeApi);
        auto derivedBuilt = derivedComp.compile({{"derived.rs", derivedSrc}});
        require(derivedBuilt.succeeded(), "Evolution.DerivedSchema failed to compile");
        game::ScriptRuntime derivedRt(derivedBuilt.program);
        game::ScriptMetadataRegistry derivedMeta;
        derivedMeta.populateFromProgram(derivedBuilt.program);
        const auto heroSchema = game::inspectTypeSchema(
            derivedRt, derivedMeta, "Evolution.DerivedSchema::ParameterizedHero");
        require(heroSchema.has_value() && heroSchema->fields.size() == 1 &&
                heroSchema->fields[0].name == "armor" &&
                heroSchema->fields[0].exported &&
                heroSchema->fields[0].rangeMax == 500,
            "inspectTypeSchema should succeed on NativeActor-derived class without 0-arg constructor");
    }
}

void testZeroHeapInlineStructAndEcsQueryMirLowering() {
    using namespace realscript;

    // 1. Custom Struct Zero-Heap-Allocation Contiguous Layout + Compile-time MIR Lowering of Query<Write<T>, Read<U>>()
    {
        game::GameApi api;
        const char* ecsSrc = R"RS(
module Evolution.EcsDataOriented;
import RealScript.Math;

struct Transform {
    public Vec2 position;
    public Fixed rotation;
    public int layer;

    public Transform(Vec2 pos, Fixed rot, int l) {
        position = pos;
        rotation = rot;
        layer = l;
    }
}

struct Velocity {
    public Vec2 linear;
    public Fixed angular;

    public Velocity(Vec2 lin, Fixed ang) {
        linear = lin;
        angular = ang;
    }
}

struct OversizedStruct {
    public int a = 1;
    public int b = 2;
    public int c = 3;
    public int d = 4;
    public int e = 5;
}

class PhysicsSystem {
    public Transform sampleTransform;
    public OversizedStruct sampleOversized;
    public int processedCount = 0;

    public void InitSamples() {
        sampleTransform = new Transform(new Vec2(FixedFromInt(10), FixedFromInt(20)), FixedFromInt(5), 3);
        sampleOversized = new OversizedStruct();
    }

    public int VerifyValueCopySemantics() {
        Transform original = new Transform(new Vec2(FixedFromInt(7), FixedFromInt(8)), FixedFromInt(1), 10);
        Transform copied = original;
        copied.layer = 99;
        copied.position = new Vec2(FixedFromInt(100), FixedFromInt(200));
        if (original.position.x != FixedFromInt(7)) return -1;
        return original.layer + 7;
    }

    public void StepPhysics(Fixed dt) {
        int count = 0;
        foreach (ref var transform, in var velocity in Query<Write<Transform>, Read<Velocity>>()) {
            transform.position = transform.position + velocity.linear * dt;
            transform.rotation = transform.rotation + velocity.angular * dt;
            transform.layer = transform.layer + 1;
            count = count + 1;
        }
        processedCount = count;
    }
}
)RS";
        game::GameScriptCompiler compiler(api);
        auto built = compiler.compile({{"ecs_system.rs", ecsSrc}});
        if (!built.succeeded()) { for (const auto& d : built.diagnostics.items()) std::cerr << d.code << ": " << d.message << "\n"; }
        require(built.succeeded(), "Evolution.EcsDataOriented failed to compile");

        game::ScriptRuntime rt(built.program);
        runtime::RuntimeError err;
        auto sysObj = rt.createObject("Evolution.EcsDataOriented::PhysicsSystem", err);
        require(sysObj.has_value(), "failed to create PhysicsSystem");

        auto initMethod = rt.findMethod(sysObj->type(), "InitSamples", 0);
        require(initMethod.has_value(), "failed to find InitSamples");
        require(rt.invoke(*sysObj, *initMethod, {}).succeeded, "InitSamples failed");

        // Verify custom value-type struct with <= 4 fields uses zero-heap-allocation inline storage!
        auto sampleTVal = rt.getMember(*sysObj, "sampleTransform", err);
        require(sampleTVal.has_value() && std::holds_alternative<runtime::StructValue>(*sampleTVal),
            "sampleTransform should be a StructValue");
        const auto& inlineStruct = std::get<runtime::StructValue>(*sampleTVal);
        require(inlineStruct.isInline && inlineStruct.storage == nullptr && inlineStruct.inlineCount == 3,
            "Transform (3 value fields: Vec2, Fixed, int) must use zero-heap-allocation inline layout (isInline == true, storage == nullptr)");

        // Verify struct with > 4 fields falls back to heap StructStorage cleanly
        auto sampleOverVal = rt.getMember(*sysObj, "sampleOversized", err);
        require(sampleOverVal.has_value() && std::holds_alternative<runtime::StructValue>(*sampleOverVal),
            "sampleOversized should be a StructValue");
        const auto& fallbackStruct = std::get<runtime::StructValue>(*sampleOverVal);
        require(!fallbackStruct.isInline && fallbackStruct.storage != nullptr && fallbackStruct.fieldCount() == 5,
            "OversizedStruct (5 fields) should fall back to heap StructStorage");

        // Verify value-copy semantics on inline structs
        auto copyMethod = rt.findMethod(sysObj->type(), "VerifyValueCopySemantics", 0);
        require(copyMethod.has_value(), "failed to find VerifyValueCopySemantics");
        auto copyRes = rt.invoke(*sysObj, *copyMethod, {});
        require(copyRes.succeeded && std::get<std::int64_t>(copyRes.value) == 17,
            "inline struct value-copy semantics failed");

        // Populate ECS contiguous component columns from C++ host via createStruct + setEcsComponentColumn
        std::vector<runtime::Value> transforms;
        std::vector<runtime::Value> velocities;
        for (int i = 0; i < 3; ++i) {
            auto t = rt.createStruct(
                "Evolution.EcsDataOriented::Transform",
                {
                    game::ScriptRuntime::makeVec2({game::Fixed::fromInt(i * 10), game::Fixed::fromInt(i * 20)}),
                    game::ScriptRuntime::makeFixed(game::Fixed::fromInt(i)),
                    static_cast<std::int64_t>(100 + i),
                },
                err);
            require(t.has_value(), "createStruct Transform failed: " + err.message);
            const auto& tStruct = std::get<runtime::StructValue>(*t);
            require(tStruct.isInline && tStruct.storage == nullptr,
                "host-created Transform must be inline with zero heap allocation");
            transforms.push_back(*t);

            auto v = rt.createStruct(
                "Evolution.EcsDataOriented::Velocity",
                {
                    game::ScriptRuntime::makeVec2({game::Fixed::fromInt(2 + i), game::Fixed::fromInt(3 + i)}),
                    game::ScriptRuntime::makeFixed(game::Fixed::fromInt(1)),
                },
                err);
            require(v.has_value(), "createStruct Velocity failed: " + err.message);
            velocities.push_back(*v);
        }

        require(rt.setEcsComponentColumn("Evolution.EcsDataOriented::Transform", transforms, err),
            "setEcsComponentColumn Transform failed: " + err.message);
        require(rt.setEcsComponentColumn("Evolution.EcsDataOriented::Velocity", velocities, err),
            "setEcsComponentColumn Velocity failed: " + err.message);

        auto stepMethod = rt.findMethod(sysObj->type(), "StepPhysics", 1);
        require(stepMethod.has_value(), "failed to find StepPhysics");
        auto stepRes = rt.invoke(*sysObj, *stepMethod, {game::ScriptRuntime::makeFixed(game::Fixed::fromInt(2))});
        require(stepRes.succeeded, "StepPhysics failed: " + stepRes.error.message);

        auto procCount = rt.getMember(*sysObj, "processedCount", err);
        require(procCount.has_value() && std::get<std::int64_t>(*procCount) == 3,
            "StepPhysics should have iterated over 3 ECS entities");

        std::vector<runtime::Value> updatedTransforms;
        require(rt.getEcsComponentColumn("Evolution.EcsDataOriented::Transform", updatedTransforms, err),
            "getEcsComponentColumn Transform failed: " + err.message);
        require(updatedTransforms.size() == 3, "expected 3 updated Transform components");

        for (int i = 0; i < 3; ++i) {
            const auto& ut = std::get<runtime::StructValue>(updatedTransforms[static_cast<std::size_t>(i)]);
            require(ut.isInline && ut.storage == nullptr,
                "mutated Transform in ECS column must remain inline with zero heap allocation");
            const auto posVal = runtime::structValueFieldAt(ut, 0);
            const auto rotVal = runtime::structValueFieldAt(ut, 1);
            const auto layerVal = runtime::structValueFieldAt(ut, 2);
            require(posVal.has_value() && rotVal.has_value() && layerVal.has_value(),
                "failed to read fields from updated inline Transform");
            const auto pos = game::ScriptRuntime::readVec2(*posVal);
            const auto rot = game::ScriptRuntime::readFixed(*rotVal);
            const auto layer = std::get<std::int64_t>(*layerVal);
            // Expected: pos.x = i*10 + (2+i)*2, pos.y = i*20 + (3+i)*2, rot = i + 1*2, layer = 100 + i + 1
            require(pos.has_value() &&
                    pos->x == game::Fixed::fromInt(i * 10 + (2 + i) * 2) &&
                    pos->y == game::Fixed::fromInt(i * 20 + (3 + i) * 2),
                "ECS Query<Write<Transform>, Read<Velocity>>() produced wrong Transform.position");
            require(rot.has_value() && *rot == game::Fixed::fromInt(i + 2),
                "ECS Query<Write<Transform>, Read<Velocity>>() produced wrong Transform.rotation");
            require(layer == 100 + i + 1,
                "ECS Query<Write<Transform>, Read<Velocity>>() produced wrong Transform.layer");
        }

        // Verify C++17 AOT backend code generation succeeds on ECS Query + Inline Structs
        compiler::Compilation aotComp;
        for (const auto& stub : api.exportDeclarationStubs()) {
            aotComp.addSource(stub);
        }
        aotComp.addSource({"ecs_system.rs", ecsSrc});
        auto aotBuild = aotComp.build();
        require(!aotBuild.diagnostics.hasErrors(), "ECS Query Compilation::build failed");
        diagnostics::DiagnosticBag aotDiag;
        aot::CppGenerator aotGen;
        auto aotEmitted = aotGen.generate(aotBuild.modules, aotDiag);
        require(!aotDiag.hasErrors() && !aotEmitted.header.empty() && !aotEmitted.source.empty(),
            "C++17 AOT emission failed on ECS Query<Write<Transform>, Read<Velocity>>()");
        require(aotEmitted.source.find("static_get::") != std::string::npos,
            "C++17 AOT source should contain lowered ECS column fetch");
    }

    // 2. Compile-Time Read/Write Safety Diagnostics on Query<Write<T>, Read<U>>()
    {
        game::GameApi api;
        game::GameScriptCompiler compiler(api);

        // 2a. Mutating a field of a Read<Velocity> (in) component must fail with RS8702
        const char* mutateReadSrc = R"RS(
module Evolution.BadQueryMutate;
struct Transform { int x; }
struct Velocity { int vx; }
void BadSystem() {
    foreach (ref var t, in var v in Query<Write<Transform>, Read<Velocity>>()) {
        v.vx = 10;
    }
}
)RS";
        auto badMutate = compiler.compile({{"bad_mutate.rs", mutateReadSrc}});
        require(!badMutate.succeeded(), "mutating Read<Velocity> component must fail compilation");
        bool sawRS8702 = false;
        for (const auto& d : badMutate.diagnostics.items()) {
            if (d.code == "RS8702") sawRS8702 = true;
        }
        require(sawRS8702, "expected diagnostic RS8702 when mutating a field of Read<T> (in) query variable");

        // 2b. Binding ref variable to Read<Velocity> must fail with RS2215
        const char* refOnReadSrc = R"RS(
module Evolution.BadQueryRef;
struct Transform { int x; }
struct Velocity { int vx; }
void BadSystem() {
    foreach (ref var t, ref var v in Query<Write<Transform>, Read<Velocity>>()) {
        t.x = v.vx;
    }
}
)RS";
        auto badRef = compiler.compile({{"bad_ref.rs", refOnReadSrc}});
        require(!badRef.succeeded(), "binding ref to Read<Velocity> must fail compilation");
        bool sawRS2215 = false;
        for (const auto& d : badRef.diagnostics.items()) {
            if (d.code == "RS2215") sawRS2215 = true;
        }
        require(sawRS2215, "expected diagnostic RS2215 when binding ref variable to Read<T>");
    }
}


void testRound2EngineGapsCompletion() {
    using namespace realscript;

    // 1. 3D InlineStructSlot (Vec3, Quat, Enum) zero-heap-allocation verification
    {
        game::GameApi api;
        const char* src = R"RS(
module Evolution.Inline3D;
import RealScript.Math;

enum MotionState {
    Idle = 0,
    Moving = 2,
    Airborne = 5
}

struct TransformSnapshot {
    Vec3 position;
    Quat rotation;
    MotionState state;
    int frame;
}

class Updater {
    public TransformSnapshot Step(TransformSnapshot snap, int nextFrame) {
        snap.position = new Vec3(snap.position.x + Fixed.Of(3), snap.position.y, snap.position.z + Fixed.Of(4));
        snap.state = MotionState.Moving;
        snap.frame = nextFrame;
        return snap;
    }
}
)RS";
        game::GameScriptCompiler compiler(api);
        auto compiled = compiler.compile({{"inline3d.rs", src}});
        require(
            compiled.succeeded(),
            "Inline3D compile failed:\n" + diagnosticsText(compiled.diagnostics));

        game::ScriptRuntime runtime(compiled.program);
        runtime::RuntimeError error;
        const auto enumType = runtime.findType("Evolution.Inline3D::MotionState");
        require(enumType.has_value(), "MotionState enum not found");

        auto snapVal = runtime.createStruct(
            "Evolution.Inline3D::TransformSnapshot",
            {
                game::ScriptRuntime::makeVec3(game::Vec3::fromInts(1, 2, 3)),
                game::ScriptRuntime::makeQuat(game::Quat::identity()),
                runtime::EnumValue{enumType->descriptor.id, 0},
                std::int64_t{10},
            },
            error);
        require(snapVal.has_value(), "createStruct TransformSnapshot failed: " + error.message);
        const auto* st = std::get_if<runtime::StructValue>(&*snapVal);
        require(
            st != nullptr && st->isInline && st->storage == nullptr,
            "TransformSnapshot with Vec3, Quat, Enum, int must be stored inline without heap StructStorage");

        auto updaterObj = runtime.createObject("Evolution.Inline3D::Updater", error);
        require(updaterObj.has_value(), "createObject Updater failed");
        const auto stepMethod = runtime.findMethod(updaterObj->type(), "Step", 2);
        require(stepMethod.has_value(), "Updater.Step not found");

        const auto res = runtime.invoke(*updaterObj, *stepMethod, {*snapVal, std::int64_t{11}});
        require(res.succeeded, "Updater.Step failed: " + res.error.message);
        const auto* outSt = std::get_if<runtime::StructValue>(&res.value);
        require(
            outSt != nullptr && outSt->isInline && outSt->storage == nullptr,
            "returned TransformSnapshot must remain inline with zero heap allocation");
        const auto posField = runtime::structValueFieldAt(*outSt, 0);
        const auto stateField = runtime::structValueFieldAt(*outSt, 2);
        const auto frameField = runtime::structValueFieldAt(*outSt, 3);
        require(
            posField.has_value() &&
                game::ScriptRuntime::readVec3(*posField) == game::Vec3::fromInts(4, 2, 7),
            "inline Vec3 field mutation mismatch");
        require(
            stateField.has_value() &&
                std::holds_alternative<runtime::EnumValue>(*stateField) &&
                std::get<runtime::EnumValue>(*stateField).value == 2,
            "inline Enum field mutation mismatch");
        require(
            frameField.has_value() && std::get<std::int64_t>(*frameField) == 11,
            "inline int field mutation mismatch");
    }

    // 2. ECS Sparse-Set Multi-Entity Join, With<T>/Without<U> Filters, EntityId Binding & ECB
    {
        game::GameApi api;
        const char* ecsSrc = R"RS(
module Evolution.SparseEcs;

struct Pos {
    int x;
    int y;
}

struct Vel {
    int dx;
    int dy;
}

struct ActiveTag {
    int flag;
}

struct FrozenTag {
    int reason;
}

class EcsSystem {
    public int RunFilteredSystem() {
        int visitedSum = 0;
        foreach (int entityId, ref var p, in var v in Query<Write<Pos>, Read<Vel>, With<ActiveTag>, Without<FrozenTag>>()) {
            p.x = p.x + v.dx;
            p.y = p.y + v.dy;
            visitedSum = visitedSum + entityId;
        }
        return visitedSum;
    }
}
)RS";
        game::GameScriptCompiler compiler(api);
        auto compiled = compiler.compile({{"sparse_ecs.rs", ecsSrc}});
        require(
            compiled.succeeded(),
            "SparseEcs compile failed:\n" + diagnosticsText(compiled.diagnostics));

        game::ScriptRuntime runtime(compiled.program);
        runtime::RuntimeError err;
        const auto makePos = [&](std::int64_t x, std::int64_t y) {
            return *runtime.createStruct("Evolution.SparseEcs::Pos", {x, y}, err);
        };
        const auto makeVel = [&](std::int64_t dx, std::int64_t dy) {
            return *runtime.createStruct("Evolution.SparseEcs::Vel", {dx, dy}, err);
        };
        const auto activeVal = *runtime.createStruct("Evolution.SparseEcs::ActiveTag", {std::int64_t{1}}, err);
        const auto frozenVal = *runtime.createStruct("Evolution.SparseEcs::FrozenTag", {std::int64_t{1}}, err);

        // Entity 10: Pos + Vel + ActiveTag (MATCH)
        require(runtime.setEcsEntityComponent(10, "Evolution.SparseEcs::Pos", makePos(100, 200), err), err.message);
        require(runtime.setEcsEntityComponent(10, "Evolution.SparseEcs::Vel", makeVel(5, 6), err), err.message);
        require(runtime.setEcsEntityComponent(10, "Evolution.SparseEcs::ActiveTag", activeVal, err), err.message);

        // Entity 20: Pos + Vel + ActiveTag + FrozenTag (EXCLUDED by Without<FrozenTag>)
        require(runtime.setEcsEntityComponent(20, "Evolution.SparseEcs::Pos", makePos(300, 400), err), err.message);
        require(runtime.setEcsEntityComponent(20, "Evolution.SparseEcs::Vel", makeVel(9, 9), err), err.message);
        require(runtime.setEcsEntityComponent(20, "Evolution.SparseEcs::ActiveTag", activeVal, err), err.message);
        require(runtime.setEcsEntityComponent(20, "Evolution.SparseEcs::FrozenTag", frozenVal, err), err.message);

        // Entity 30: Pos + Vel (EXCLUDED by With<ActiveTag>)
        require(runtime.setEcsEntityComponent(30, "Evolution.SparseEcs::Pos", makePos(500, 600), err), err.message);
        require(runtime.setEcsEntityComponent(30, "Evolution.SparseEcs::Vel", makeVel(7, 8), err), err.message);

        // Entity 40: Pos + Vel + ActiveTag (MATCH)
        require(runtime.setEcsEntityComponent(40, "Evolution.SparseEcs::Pos", makePos(10, 20), err), err.message);
        require(runtime.setEcsEntityComponent(40, "Evolution.SparseEcs::Vel", makeVel(1, 2), err), err.message);
        require(runtime.setEcsEntityComponent(40, "Evolution.SparseEcs::ActiveTag", activeVal, err), err.message);

        auto sysObj = runtime.createObject("Evolution.SparseEcs::EcsSystem", err);
        require(sysObj.has_value(), "createObject EcsSystem failed");
        const auto runMethod = runtime.findMethod(sysObj->type(), "RunFilteredSystem", 0);
        require(runMethod.has_value(), "RunFilteredSystem not found");

        const auto runRes = runtime.invoke(*sysObj, *runMethod, {});
        require(
            runRes.succeeded && std::get<std::int64_t>(runRes.value) == 50,
            "RunFilteredSystem should visit entities 10 and 40 (sum = 50)");

        // Verify writeback updated entities 10 and 40 while leaving entity 20 untouched
        const auto pos10 = runtime.getEcsEntityComponent(10, "Evolution.SparseEcs::Pos", err);
        const auto pos20 = runtime.getEcsEntityComponent(20, "Evolution.SparseEcs::Pos", err);
        const auto pos40 = runtime.getEcsEntityComponent(40, "Evolution.SparseEcs::Pos", err);
        require(pos10.has_value() && pos20.has_value() && pos40.has_value(), "getEcsEntityComponent failed");
        require(
            *runtime::structValueFieldAt(std::get<runtime::StructValue>(*pos10), 0) == runtime::Value{std::int64_t{105}} &&
                *runtime::structValueFieldAt(std::get<runtime::StructValue>(*pos10), 1) == runtime::Value{std::int64_t{206}},
            "Entity 10 Pos writeback mismatch");
        require(
            *runtime::structValueFieldAt(std::get<runtime::StructValue>(*pos20), 0) == runtime::Value{std::int64_t{300}},
            "Entity 20 Pos should not be modified");
        require(
            *runtime::structValueFieldAt(std::get<runtime::StructValue>(*pos40), 0) == runtime::Value{std::int64_t{11}} &&
                *runtime::structValueFieldAt(std::get<runtime::StructValue>(*pos40), 1) == runtime::Value{std::int64_t{22}},
            "Entity 40 Pos writeback mismatch");

        // Test EntityCommandBuffer (ECB) deferred structural mutations
        runtime.queueEcsRemoveComponent(20, "Evolution.SparseEcs::FrozenTag");
        runtime.queueEcsSetComponent(30, "Evolution.SparseEcs::ActiveTag", activeVal);
        runtime.queueEcsDestroyEntity(10);
        require(runtime.pendingEcsCommandCount() == 3, "pendingEcsCommandCount should be 3");
        const auto applied = runtime.flushEcsCommandBuffer(err);
        require(applied == 3 && runtime.pendingEcsCommandCount() == 0, "flushEcsCommandBuffer failed");
        require(!runtime.hasEcsEntityComponent(10, "Evolution.SparseEcs::Pos"), "Entity 10 should be destroyed");
        require(!runtime.hasEcsEntityComponent(20, "Evolution.SparseEcs::FrozenTag"), "Entity 20 FrozenTag should be removed");
        require(runtime.hasEcsEntityComponent(30, "Evolution.SparseEcs::ActiveTag"), "Entity 30 ActiveTag should be added");

        // Re-run system: now entities 20, 30, 40 match (20 + 30 + 40 = 90)
        const auto runRes2 = runtime.invoke(*sysObj, *runMethod, {});
        require(
            runRes2.succeeded && std::get<std::int64_t>(runRes2.value) == 90,
            "After ECB flush, entities 20, 30, 40 should match (sum = 90)");
    }

    // 3. ManagedHeap Shared referenceFields Layout & String Interning + 4. BindingRegistry SymbolId Cache
    {
        game::GameApi api;
        int pingCalls = 0;
        require(
            api.function("Evolution.Host", "Ping", [&pingCalls](int v) -> int {
                ++pingCalls;
                return v + 1;
            }),
            "bind Ping failed");

        const char* heapSrc = R"RS(
module Evolution.HeapOpt;
import Evolution.Host;

class NodeItem {
    public string label;
    public NodeItem next;

    public int Run() {
        string a = "shared_literal_key";
        string b = "shared_literal_key";
        if (a == b) {
            return Ping(41);
        }
        return 0;
    }
}
)RS";
        game::GameScriptCompiler compiler(api);
        auto compiled = compiler.compile({{"heap_opt.rs", heapSrc}});
        require(compiled.succeeded(), "HeapOpt compile failed");

        game::ScriptRuntime runtime(compiled.program);
        runtime::RuntimeError err;
        auto n1 = runtime.createObject("Evolution.HeapOpt::NodeItem", err);
        auto n2 = runtime.createObject("Evolution.HeapOpt::NodeItem", err);
        auto n3 = runtime.createObject("Evolution.HeapOpt::NodeItem", err);
        require(n1.has_value() && n2.has_value() && n3.has_value(), "createObject NodeItem failed");
        require(
            runtime.heap()->sharedTypeLayoutCount() >= 1,
            "ManagedHeap should share referenceFields layout per typeId");

        const auto s1 = runtime.heap()->internString("engine_tag_alpha", &err);
        const auto s2 = runtime.heap()->internString("engine_tag_alpha", &err);
        require(
            s1.has_value() && s2.has_value() && *s1 == *s2,
            "ManagedHeap::internString must return identical ObjectRef for identical strings");
        require(
            runtime.heap()->internedStringCount() >= 1,
            "internedStringCount should track interned strings");

        const auto runMethod = runtime.findMethod(n1->type(), "Run", 0);
        const auto res = runtime.invoke(*n1, *runMethod, {});
        require(
            res.succeeded && std::get<std::int64_t>(res.value) == 42 && pingCalls == 1,
            "NodeItem.Run should return 42");
        require(
            api.bindings()->cachedSymbolCount() >= 1,
            "BindingRegistry should populate O(1) SymbolId resolution cache");
    }

    // 5. Sequence Coroutine `var` Local Type Inference & `yield wait_signal(...)` + `SceneScriptRuntime::emitSignal`
    {
        auto host = std::make_shared<game::GameplayHost>(60);
        game::GameApi api;
        require(game::installGameplayBindings(api, host), "installGameplayBindings failed");

        const char* seqSrc = R"RS(
module Evolution.Cutscene;
import RealScript.Game;

class QuestActor {
    public int stage = 0;
    public int reward = 0;

    sequence RunQuest(long selfId, long bossEntityId) {
        var baseReward = 50;
        var multiplier = 3;
        stage = 1;
        yield wait_signal(bossEntityId, "OnBossDefeated");
        reward = baseReward * multiplier;
        stage = 2;
    }
}
)RS";
        game::GameScriptCompiler compiler(api);
        auto compiled = compiler.compile({{"cutscene.rs", seqSrc}});
        require(
            compiled.succeeded(),
            "Cutscene sequence compile failed:\n" + diagnosticsText(compiled.diagnostics));

        game::ScriptRuntime runtime(compiled.program);
        game::SceneScriptRuntime scene(runtime);
        game::SceneGameplayDriver driver(scene, host);

        require(scene.attachComponent(1, "Evolution.Cutscene::QuestActor"), "attach QuestActor failed");
        const auto startRes = scene.invoke(
            1,
            "RunQuest",
            {runtime::LongValue{1}, runtime::LongValue{99}});
        require(startRes.succeeded, "RunQuest start failed: " + startRes.error.message);

        auto* actorObj = scene.getComponent(1, "QuestActor");
        runtime::RuntimeError err;
        require(
            std::get<std::int64_t>(*runtime.getMember(*actorObj, "stage", err)) == 1 &&
                std::get<std::int64_t>(*runtime.getMember(*actorObj, "reward", err)) == 0,
            "QuestActor should suspend at stage 1 before signal is emitted");

        // Advance 3 ticks without emitting the signal: sequence must stay suspended
        (void)driver.advanceTicks(3);
        require(
            std::get<std::int64_t>(*runtime.getMember(*actorObj, "stage", err)) == 1,
            "QuestActor should remain at stage 1 before OnBossDefeated signal");

        // Emit signal from unrelated entity 50: sequence waiting on bossEntityId=99 must stay suspended (0 listeners woken)
        require(scene.emitSignal(50, "OnBossDefeated") == 0, "emitSignal(50) should wake 0 listeners");
        (void)driver.advanceTicks(1);
        require(
            std::get<std::int64_t>(*runtime.getMember(*actorObj, "stage", err)) == 1,
            "QuestActor must NOT wake up when unrelated entity 50 emits OnBossDefeated");

        // Emit signal from bossEntityId=99 and advance 1 tick
        require(scene.emitSignal(99, "OnBossDefeated") > 0, "emitSignal should publish signal");
        (void)driver.advanceTicks(1);
        require(
            std::get<std::int64_t>(*runtime.getMember(*actorObj, "stage", err)) == 2 &&
                std::get<std::int64_t>(*runtime.getMember(*actorObj, "reward", err)) == 150,
            "QuestActor should resume after wait_signal with hoisted var locals preserved (reward=150)");
    }

    // 6. RealScript.Input Deterministic Snapshot & RealScript.Physics 2D Colliders/Queries/Callbacks
    {
        game::GameApi api;
        game::GameScriptCompiler bootstrapCompiler(api);
        auto bootstrap = bootstrapCompiler.compile({{"empty.rs", "module Empty; void Main() {}"}});
        game::ScriptRuntime dummyRuntime(bootstrap.program);
        game::SceneScriptRuntime scene(dummyRuntime);
        require(
            game::installSceneComponentBindings(api, scene),
            "installSceneComponentBindings failed");

        const char* physInputSrc = R"RS(
module Evolution.PhysicsInput;
import RealScript.Math;
import RealScript.Scene;

class PlayerActor {
    public int jumpCount = 0;
    public int releaseCount = 0;
    public Fixed moveX;
    public long lastRaycastHit = 0;
    public int nearbyCount = 0;
    public long collisionEnterEntity = 0;
    public int collisionStayTicks = 0;
    public long collisionExitEntity = 0;
    public long triggerEnterEntity = 0;
    public long triggerExitEntity = 0;

    public void SampleInputAndSpatial() {
        if (IsActionJustPressed("Jump")) {
            jumpCount = jumpCount + 1;
        }
        if (IsActionJustReleased("Jump")) {
            releaseCount = releaseCount + 1;
        }
        Vec2 axis = GetInputAxis2D("Move");
        moveX = axis.x;
        lastRaycastHit = Raycast2D(
            new Vec2(Fixed.Of(0), Fixed.Of(0)),
            new Vec2(Fixed.Of(1), Fixed.Of(0)),
            Fixed.Of(20));
        nearbyCount = OverlapCircleCount2D(
            new Vec2(Fixed.Of(0), Fixed.Of(0)),
            Fixed.Of(15));
    }

    public void OnCollisionEnter(long otherEntity) {
        collisionEnterEntity = otherEntity;
    }

    public void OnCollisionStay(long otherEntity) {
        collisionStayTicks = collisionStayTicks + 1;
    }

    public void OnCollisionExit(long otherEntity) {
        collisionExitEntity = otherEntity;
    }

    public void OnTriggerEnter(long otherEntity) {
        triggerEnterEntity = otherEntity;
    }

    public void OnTriggerExit(long otherEntity) {
        triggerExitEntity = otherEntity;
    }
}
)RS";
        game::GameScriptCompiler compiler(api);
        auto compiled = compiler.compile({{"phys_input.rs", physInputSrc}});
        require(
            compiled.succeeded(),
            "PhysicsInput compile failed:\n" + diagnosticsText(compiled.diagnostics));

        dummyRuntime.replaceProgram(compiled.program);
        require(scene.attachComponent(1, "Evolution.PhysicsInput::PlayerActor"), "attach PlayerActor 1 failed");
        require(scene.attachComponent(2, "Evolution.PhysicsInput::PlayerActor"), "attach PlayerActor 2 failed");
        require(scene.attachComponent(3, "Evolution.PhysicsInput::PlayerActor"), "attach PlayerActor 3 failed");

        scene.setLocalPosition2D(1, game::Vec2::fromInts(0, 0));
        scene.setLocalPosition2D(2, game::Vec2::fromInts(5, 0));
        scene.setLocalPosition2D(3, game::Vec2::fromInts(25, 0));

        game::SceneCollider2D col1;
        col1.halfExtents = game::Vec2::fromInts(1, 1);
        scene.setEntityCollider2D(1, col1);

        game::SceneCollider2D col2;
        col2.halfExtents = game::Vec2::fromInts(1, 1);
        scene.setEntityCollider2D(2, col2);

        game::SceneCollider2D trig3;
        trig3.isCircle = true;
        trig3.radius = game::Fixed::fromInt(2);
        trig3.isTrigger = true;
        scene.setEntityCollider2D(3, trig3);

        // Input frame 1: press Jump and set Move axis
        scene.setInputAction("Jump", true);
        scene.setInputAxis2D("Move", game::Vec2::fromInts(1, 0));
        const auto digest1 = scene.computeInputDigest();
        require(digest1 != 0, "computeInputDigest must produce non-zero deterministic hash");
        require(scene.invoke(1, "SampleInputAndSpatial").succeeded, "SampleInputAndSpatial frame 1 failed");

        auto* p1 = scene.getComponent(1, "PlayerActor");
        runtime::RuntimeError err;
        require(
            std::get<std::int64_t>(*dummyRuntime.getMember(*p1, "jumpCount", err)) == 1 &&
                std::get<std::int64_t>(*dummyRuntime.getMember(*p1, "releaseCount", err)) == 0 &&
                std::get<runtime::LongValue>(*dummyRuntime.getMember(*p1, "lastRaycastHit", err)).value == 1 &&
                std::get<std::int64_t>(*dummyRuntime.getMember(*p1, "nearbyCount", err)) == 2,
            "Input & spatial query results mismatch on frame 1");

        // Advance input tick while keeping Jump pressed -> JustPressed must be false
        scene.advanceInputTick();
        require(scene.invoke(1, "SampleInputAndSpatial").succeeded, "SampleInputAndSpatial frame 2 failed");
        require(
            std::get<std::int64_t>(*dummyRuntime.getMember(*p1, "jumpCount", err)) == 1,
            "IsActionJustPressed must not fire when button remains held");

        // Release Jump -> JustReleased must fire
        scene.advanceInputTick();
        scene.setInputAction("Jump", false);
        require(scene.invoke(1, "SampleInputAndSpatial").succeeded, "SampleInputAndSpatial frame 3 failed");
        require(
            std::get<std::int64_t>(*dummyRuntime.getMember(*p1, "releaseCount", err)) == 1,
            "IsActionJustReleased should fire on release frame");

        // Physics 2D lifecycle: move entity 2 onto entity 1 (solid collision) and entity 3 onto entity 1 (trigger)
        scene.setLocalPosition2D(2, game::Vec2::fromInts(1, 0));
        scene.setLocalPosition2D(3, game::Vec2::fromInts(1, 0));
        require(scene.stepPhysics2D() > 0, "stepPhysics2D Enter step should fire callbacks");
        require(
            std::get<runtime::LongValue>(*dummyRuntime.getMember(*p1, "collisionEnterEntity", err)).value == 2 &&
                std::get<runtime::LongValue>(*dummyRuntime.getMember(*p1, "triggerEnterEntity", err)).value == 3,
            "OnCollisionEnter and OnTriggerEnter should fire on entity 1");

        // Step again at same positions -> OnCollisionStay
        require(scene.stepPhysics2D() > 0, "stepPhysics2D Stay step should fire callbacks");
        require(
            std::get<std::int64_t>(*dummyRuntime.getMember(*p1, "collisionStayTicks", err)) == 1,
            "OnCollisionStay should increment collisionStayTicks");

        // Separate entities 2 and 3 -> OnCollisionExit & OnTriggerExit
        scene.setLocalPosition2D(2, game::Vec2::fromInts(50, 0));
        scene.setLocalPosition2D(3, game::Vec2::fromInts(50, 0));
        require(scene.stepPhysics2D() > 0, "stepPhysics2D Exit step should fire callbacks");
        require(
            std::get<runtime::LongValue>(*dummyRuntime.getMember(*p1, "collisionExitEntity", err)).value == 2 &&
                std::get<runtime::LongValue>(*dummyRuntime.getMember(*p1, "triggerExitEntity", err)).value == 3,
            "OnCollisionExit and OnTriggerExit should fire on separation");
    }

    // 7. Compile-Time [Deterministic] Verification (RS2495) & Runtime Capability Sandbox (CapabilityDenied)
    {
        game::GameApi api;
        require(
            api.function(
                "Evolution.Net",
                "FetchWallClock",
                []() -> int { return 12345; },
                runtime::BindingDeterminism::NonDeterministic),
            "bind FetchWallClock failed");
        require(
            api.function(
                "Evolution.Net",
                "HttpGetStatus",
                [](std::string url) -> int { return url.empty() ? 400 : 200; },
                runtime::BindingDeterminism::Deterministic,
                "Net.Http"),
            "bind HttpGetStatus failed");

        game::GameScriptCompiler compiler(api);

        // 7a. Calling [NonDeterministic] function from [Deterministic] module must fail with RS2495
        const char* badDetSrc = R"RS(
[Deterministic]
module Evolution.LockstepSim;
import Evolution.Net;

int SimulateTick() {
    return FetchWallClock();
}
)RS";
        auto badDet = compiler.compile({{"bad_det.rs", badDetSrc}});
        require(!badDet.succeeded(), "calling NonDeterministic function in [Deterministic] module must fail");
        bool sawRS2495 = false;
        for (const auto& d : badDet.diagnostics.items()) {
            if (d.code == "RS2495") sawRS2495 = true;
        }
        require(sawRS2495, "expected diagnostic RS2495 for non-deterministic call in [Deterministic] context");

        // 7b. Capability sandbox enforcement at runtime
        const char* capSrc = R"RS(
module Evolution.ModScript;
import Evolution.Net;

class ModRunner {
    public int CheckServer() {
        return HttpGetStatus("https://example.com");
    }
}
)RS";
        auto capCompiled = compiler.compile({{"mod_script.rs", capSrc}});
        require(
            capCompiled.succeeded(),
            "ModScript compile failed:\n" + diagnosticsText(capCompiled.diagnostics));

        game::ScriptRuntime runtime(capCompiled.program);
        runtime::RuntimeError err;
        auto modObj = runtime.createObject("Evolution.ModScript::ModRunner", err);
        require(modObj.has_value(), "createObject ModRunner failed");
        const auto checkMethod = runtime.findMethod(modObj->type(), "CheckServer", 0);
        require(checkMethod.has_value(), "CheckServer method not found");

        runtime::ExecutionOptions deniedOpts;
        deniedOpts.enforceCapabilities = true;
        const auto deniedRes = runtime.invoke(*modObj, *checkMethod, {}, deniedOpts);
        require(
            !deniedRes.succeeded &&
                deniedRes.error.code == runtime::ErrorCode::CapabilityDenied,
            "invoking capability-protected binding without grant must fail with ErrorCode::CapabilityDenied");

        runtime::ExecutionOptions grantedOpts;
        grantedOpts.enforceCapabilities = true;
        grantedOpts.grantedCapabilities.insert("Net.Http");
        const auto grantedRes = runtime.invoke(*modObj, *checkMethod, {}, grantedOpts);
        require(
            grantedRes.succeeded && std::get<std::int64_t>(grantedRes.value) == 200,
            "invoking capability-protected binding with Net.Http grant must succeed");
    }
}

void testRound3AuditFixesAndGodotUnityParity() {
    using namespace realscript;

    // 1. Fixed-point 64-bit exact geometry at large world coordinates (>3000 & 100000 units)
    //    and asymmetric collisionLayer / collisionMask filtering
    {
        game::GameApi api;
        game::GameScriptCompiler compiler(api);
        auto compiled = compiler.compile({{"empty.rs", "module Empty; void Main() {}"}});
        require(compiled.succeeded(), "Empty compile failed");
        game::ScriptRuntime runtime(compiled.program);
        game::SceneScriptRuntime scene(runtime);

        // Place entity 1 at (0, 0) and entity 2 at (3000, 0).
        // Previously, dx=3000 in Q24.8 (raw 768000) overflowed int32_t when squared in Fixed::operator*,
        // producing a negative wrapped distanceSq and false-positive circle overlaps!
        scene.setLocalPosition2D(1, game::Vec2::fromInts(0, 0));
        scene.setLocalPosition2D(2, game::Vec2::fromInts(3000, 0));
        scene.setLocalPosition2D(3, game::Vec2::fromInts(100000, 0));

        game::SceneCollider2D c1;
        c1.isCircle = true;
        c1.radius = game::Fixed::fromInt(10);
        c1.collisionLayer = 1u << 0;
        c1.collisionMask = 1u << 1;
        scene.setEntityCollider2D(1, c1);

        game::SceneCollider2D c2;
        c2.isCircle = true;
        c2.radius = game::Fixed::fromInt(10);
        c2.collisionLayer = 1u << 1;
        c2.collisionMask = 1u << 0;
        scene.setEntityCollider2D(2, c2);

        game::SceneCollider2D c3;
        c3.isCircle = true;
        c3.radius = game::Fixed::fromInt(10);
        scene.setEntityCollider2D(3, c3);

        const auto hitsNearOrigin = scene.overlapCircle2D(
            game::Vec2::fromInts(0, 0), game::Fixed::fromInt(5));
        require(
            hitsNearOrigin.size() == 1 && hitsNearOrigin[0] == 1,
            "overlapCircle2D at origin must NOT false-match entities at x=3000 or x=100000 due to Q24.8 overflow");

        (void)scene.stepPhysics2D();
        require(
            scene.activeOverlaps2D().empty(),
            "stepPhysics2D must NOT report false overlap between circles 3000 units apart");

        // Raycast2D over 3000 units toward entity 2 at (3000, 0) on layer 1
        const auto farRayHit = scene.raycast2D(
            game::Vec2::fromInts(0, 0),
            game::Vec2::fromInts(1, 0),
            game::Fixed::fromInt(3500),
            1u << 1);
        require(
            farRayHit.has_value() && farRayHit->entity == 2 &&
                farRayHit->distance == game::Fixed::fromInt(2990),
            "raycast2D at distance 3000 must hit entity 2 at distance 2990 without Q24.8 overflow");

        // Asymmetric collisionLayer / collisionMask: move entity 2 onto entity 1, verify overlap,
        // then change entity 2's collisionMask so it no longer masks layer 0.
        scene.setLocalPosition2D(2, game::Vec2::fromInts(5, 0));
        (void)scene.stepPhysics2D();
        require(
            scene.activeOverlaps2D().count({1, 2}) == 1,
            "entities 1 and 2 with matching layer/mask should overlap at distance 5");

        c2.collisionMask = 1u << 2; // no longer collides with entity 1 (layer 0)
        scene.setEntityCollider2D(2, c2);
        (void)scene.stepPhysics2D();
        require(
            scene.activeOverlaps2D().empty(),
            "asymmetric collisionLayer/collisionMask mismatch must prevent overlap");
    }

    // 2. Reentrant QueueDestroyEntity in OnCollisionEnter, Entity Naming, Groups, and Scene JSON Snapshot Round-Trip
    {
        game::GameApi api;
        game::GameScriptCompiler bootstrapCompiler(api);
        auto bootstrap = bootstrapCompiler.compile({{"empty.rs", "module Empty; void Main() {}"}});
        game::ScriptRuntime runtime(bootstrap.program);
        game::SceneScriptRuntime scene(runtime);
        require(
            game::installSceneComponentBindings(api, scene),
            "installSceneComponentBindings failed");

        const char* src = R"RS(
module Evolution.ParityRound3;
import RealScript.Scene;

class BulletActor {
    public long hitTarget = 0;
    public int stayTicks = 0;
    public int exitTicks = 0;
    public long spawnedVfx = 0;
    public int enemyGroupCount = 0;
    public bool jumpReleasedAfterRestore = false;

    public void OnCollisionEnter(long other) {
        hitTarget = other;
        enemyGroupCount = GetGroupCount("Enemies");
        if (IsInGroup(other, "DestroyOnHit")) {
            QueueDestroyEntity(other);
            spawnedVfx = SpawnEntity("HitVfx");
            AddToGroup(spawnedVfx, "VfxGroup");
        }
    }

    public void OnCollisionStay(long other) {
        stayTicks = stayTicks + 1;
    }

    public void OnCollisionExit(long other) {
        exitTicks = exitTicks + 1;
    }

    public void CheckInputAfterRestore() {
        if (IsActionJustReleased("Fire")) {
            jumpReleasedAfterRestore = true;
        }
    }
}
)RS";
        game::GameScriptCompiler compiler(api);
        auto compiled = compiler.compile({{"parity_round3.rs", src}});
        require(
            compiled.succeeded(),
            "ParityRound3 compile failed:\n" + diagnosticsText(compiled.diagnostics));
        runtime.replaceProgram(compiled.program);

        require(scene.attachComponent(1, "Evolution.ParityRound3::BulletActor"), "attach 1 failed");
        require(scene.attachComponent(2, "Evolution.ParityRound3::BulletActor"), "attach 2 failed");
        require(scene.attachComponent(3, "Evolution.ParityRound3::BulletActor"), "attach 3 failed");

        scene.setEntityName(1, "PlayerBullet");
        scene.setEntityName(2, "FragileCrate");
        scene.setEntityName(3, "BossArmor");
        scene.addEntityToGroup(2, "Enemies");
        scene.addEntityToGroup(2, "DestroyOnHit");
        scene.addEntityToGroup(3, "Enemies");

        require(scene.findEntityByName("FragileCrate") == 2, "findEntityByName(FragileCrate) should return 2");

        scene.setLocalPosition2D(1, game::Vec2::fromInts(0, 0));
        scene.setLocalPosition2D(2, game::Vec2::fromInts(0, 0));
        scene.setLocalPosition2D(3, game::Vec2::fromInts(0, 0));

        game::SceneCollider2D box;
        box.halfExtents = game::Vec2::fromInts(2, 2);
        scene.setEntityCollider2D(1, box);
        scene.setEntityCollider2D(2, box);
        scene.setEntityCollider2D(3, box);

        // Press "Fire" before snapshot
        scene.setInputAction("Fire", true);
        scene.setInputAxis2D("Aim", game::Vec2::fromInts(3, -4));

        // Step physics: entity 1 collides with 2 and 3.
        // Callback on 1 queues destruction of entity 2 AND spawns "HitVfx" during stepPhysics2D!
        (void)scene.stepPhysics2D();

        // Entity 2 must be flushed at the end of stepPhysics2D, while entity 1, 3, and spawned HitVfx remain
        require(!scene.hasComponent(2, "BulletActor"), "entity 2 queued for destroy must be removed after stepPhysics2D");
        require(scene.findEntityByName("FragileCrate") == 0, "destroyed entity name must be removed");
        const auto vfxId = scene.findEntityByName("HitVfx");
        require(vfxId != 0 && scene.isEntityInGroup(vfxId, "VfxGroup"), "spawned HitVfx entity and group must exist");

        // Advance input tick and release "Fire" so previousActions["Fire"]=true, currentActions["Fire"]=false
        scene.advanceInputTick();
        scene.setInputAction("Fire", false);

        // Export scene JSON while entity 1 & 3 have an active overlap AND "Fire" was just released
        runtime::RuntimeError err;
        const std::string snapshotJson = game::exportSceneJson(scene, err);
        require(!snapshotJson.empty() && err.code == runtime::ErrorCode::None, "exportSceneJson failed: " + err.message);

        // Mutate scene state, then import snapshot back
        (void)scene.detach(1);
        (void)scene.detach(3);
        scene.restoreInputSnapshot({}, {}, {});
        scene.restorePhysicsOverlapState({}, {});

        require(game::importSceneJson(scene, snapshotJson, err), "importSceneJson failed: " + err.message);
        require(scene.findEntityByName("PlayerBullet") == 1, "restored scene must preserve entity name PlayerBullet");
        require(scene.isEntityInGroup(3, "Enemies"), "restored scene must preserve entity groups");
        require(scene.getEntityCollider2D(1).has_value(), "restored scene must preserve 2D colliders");
        require(
            scene.isInputActionJustReleased("Fire") &&
                scene.getInputAxis2D("Aim") == game::Vec2::fromInts(3, -4),
            "restored scene must preserve deterministic input snapshot (JustReleased & 2D axis)");

        // Stepping physics at same positions after restore must fire OnCollisionStay (NOT OnCollisionEnter again!)
        (void)scene.stepPhysics2D();
        auto* b1 = scene.getComponent(1, "BulletActor");
        require(b1 != nullptr, "BulletActor on entity 1 missing after import");
        require(
            std::get<std::int64_t>(*runtime.getMember(*b1, "stayTicks", err)) == 1,
            "restored activeOverlaps2D must trigger OnCollisionStay on next physics step");
    }

    // 3. Unregistered externalResolver capability hardening
    {
        game::GameApi api;
        // Register signature in semantic symbols via api, then run on bare Interpreter with custom externalResolver
        require(
            api.function("Evolution.RawHost", "SecretHook", []() -> int { return 77; }),
            "bind SecretHook failed");
        game::GameScriptCompiler compiler(api);
        const char* rawSrc = R"RS(
module Evolution.RawCheck;
import Evolution.RawHost;

int CallSecret() {
    return SecretHook();
}
)RS";
        auto compiled = compiler.compile({{"raw_check.rs", rawSrc}});
        require(compiled.succeeded(), "RawCheck compile failed");

        // Execute bytecode on an Interpreter WITHOUT BindingRegistry (only raw externalResolver)
        runtime::Interpreter interp(compiled.program.program());
        interp.setExternalResolver(
            [](const bytecode::FunctionReference&,
               runtime::ValueSpan,
               runtime::RuntimeError&) -> std::optional<runtime::Value> {
                return std::int64_t{77};
            });

        runtime::ExecutionOptions deniedOpts;
        deniedOpts.enforceCapabilities = true;
        const auto deniedRes = interp.invoke("Evolution.RawCheck::CallSecret", {}, deniedOpts);
        require(
            !deniedRes.succeeded && deniedRes.error.code == runtime::ErrorCode::CapabilityDenied,
            "unregistered externalResolver must be blocked when enforceCapabilities=true without Host.UnregisteredExternal");

        runtime::ExecutionOptions allowedOpts;
        allowedOpts.enforceCapabilities = true;
        allowedOpts.grantedCapabilities.insert("Host.UnregisteredExternal");
        const auto allowedRes = interp.invoke("Evolution.RawCheck::CallSecret", {}, allowedOpts);
        require(
            allowedRes.succeeded && std::get<std::int64_t>(allowedRes.value) == 77,
            "unregistered externalResolver must succeed when Host.UnregisteredExternal is granted");
    }
}

void testRound4RoadmapP0ToP2All10Items() {
    using namespace realscript;

    // 1. P0-1: Nested Inline Struct (Struct containing another custom Struct) with zero heap StructStorage
    //    & P0-2: Native O(1) ECS Sparse Set (zero per-mutation heap array allocations)
    //    & P0-3: Inline ManagedHeap Slot Storage
    {
        game::GameApi api;
        const char* p0Src = R"RS(
module Evolution.P0NestedStructAndEcs;
import RealScript.Math;

struct Bounds2D {
    public Vec2 minPt;
    public Vec2 maxPt;

    public Bounds2D(Vec2 mn, Vec2 mx) {
        minPt = mn;
        maxPt = mx;
    }
}

struct SpatialBody {
    public Bounds2D box;
    public int mass;

    public SpatialBody(Bounds2D b, int m) {
        box = b;
        mass = m;
    }
}

class SpatialRunner {
    public SpatialBody sampleBody;

    public void Init() {
        Bounds2D b = new Bounds2D(
            new Vec2(Fixed.Of(1), Fixed.Of(2)),
            new Vec2(Fixed.Of(10), Fixed.Of(20)));
        sampleBody = new SpatialBody(b, 50);
    }

    public int MutateNestedStruct() {
        SpatialBody copy = sampleBody;
        copy.mass = 99;
        Bounds2D inner = copy.box;
        inner.maxPt = new Vec2(Fixed.Of(30), Fixed.Of(40));
        copy.box = inner;
        if (sampleBody.mass != 50) return -1;
        if (sampleBody.box.maxPt.x != Fixed.Of(10)) return -2;
        return copy.box.maxPt.x.ToInt() + copy.mass;
    }

    public int RunEcsBatch() {
        int sumMass = 0;
        foreach (int entityId, ref var body in Query<Write<SpatialBody>>()) {
            body.mass = body.mass + entityId;
            sumMass = sumMass + body.mass;
        }
        return sumMass;
    }
}
)RS";
        game::GameScriptCompiler compiler(api);
        auto compiled = compiler.compile({{"p0_test.rs", p0Src}});
        require(
            compiled.succeeded(),
            "Evolution.P0NestedStructAndEcs compile failed:\n" +
                diagnosticsText(compiled.diagnostics));

        game::ScriptRuntime rt(compiled.program);
        runtime::RuntimeError err;
        auto runner = rt.createObject("Evolution.P0NestedStructAndEcs::SpatialRunner", err);
        require(runner.has_value(), "createObject SpatialRunner failed");
        require(
            rt.invoke(*runner, *rt.findMethod(runner->type(), "Init", 0), {}).succeeded,
            "SpatialRunner.Init failed");

        const auto bodyVal = rt.getMember(*runner, "sampleBody", err);
        require(
            bodyVal.has_value() && std::holds_alternative<runtime::StructValue>(*bodyVal),
            "sampleBody must be a StructValue");
        const auto& inlineBody = std::get<runtime::StructValue>(*bodyVal);
        require(
            inlineBody.isInline && inlineBody.storage == nullptr &&
                inlineBody.inlineCount == 2 && inlineBody.inlineSlotCount == 3,
            "P0-1: SpatialBody (nested Bounds2D{Vec2,Vec2} + int mass) must pack inline into 3 slots with storage == nullptr");

        const auto mutateRes = rt.invoke(
            *runner, *rt.findMethod(runner->type(), "MutateNestedStruct", 0), {});
        require(
            mutateRes.succeeded && std::get<std::int64_t>(mutateRes.value) == 129,
            "P0-1: MutateNestedStruct value-copy semantics expected 30 + 99 = 129");

        // P0-2: Verify O(1) Native ECS Sparse Set does NOT allocate managed heap objects per entity insertion!
        const std::size_t allocsBefore = rt.heap()->statistics().objectsAllocated;
        for (std::uint64_t ent = 1; ent <= 50; ++ent) {
            require(
                rt.setEcsEntityComponent(
                    ent, "Evolution.P0NestedStructAndEcs::SpatialBody", *bodyVal, err),
                "setEcsEntityComponent failed: " + err.message);
        }
        const std::size_t allocsAfter = rt.heap()->statistics().objectsAllocated;
        require(
            allocsAfter == allocsBefore,
            "P0-2: inserting 50 ECS components into native Sparse Set must perform ZERO ManagedHeap object allocations");

        const auto ecsRes = rt.invoke(
            *runner, *rt.findMethod(runner->type(), "RunEcsBatch", 0), {});
        // 50 entities * 50 base mass = 2500, plus sum(1..50) = 1275 => 3775
        require(
            ecsRes.succeeded && std::get<std::int64_t>(ecsRes.value) == 3775,
            "P0-2: RunEcsBatch over 50 native Sparse Set entities expected 3775");
    }

    // 2. P1-1: Generational SceneEntityId (ABA protection), P1-2: Spatial Hash Grid Broadphase & CollisionContact2D,
    //    P1-3: Non-shared_ptr Raw Native Pointer Binding, P1-4: Scriptable Assets & Sequence Hot Reload Migration
    {
        struct EngineManagedMotor {
            int rpm = 1000;
            bool customReleased = false;
            void boost(int delta) { rpm += delta; }
            int getRpm() const { return rpm; }
        };

        game::GameApi api;
        api.type<EngineManagedMotor>("Engine.Raw", "EngineManagedMotor")
            .method<&EngineManagedMotor::boost>("Boost")
            .method<&EngineManagedMotor::getRpm>("GetRpm");

        auto host = std::make_shared<game::GameplayHost>(60);
        require(game::installGameplayBindings(api, host), "installGameplayBindings failed");

        game::GameScriptCompiler bootstrapCompiler(api);
        auto bootstrap = bootstrapCompiler.compile({{"empty.rs", "module Empty; void Main() {}"}});
        game::ScriptRuntime rt(bootstrap.program);
        game::SceneScriptRuntime scene(rt);
        require(game::installSceneComponentBindings(api, scene), "installSceneComponentBindings failed");

        const char* p1SrcV1 = R"RS(
module Evolution.P1EngineRuntime;
import Engine.Raw;
import RealScript.Math;
import RealScript.Game;
import RealScript.Scene;

class WeaponDataAsset {
    public string weaponName = "PlasmaRifle";
    public int baseDamage = 40;

    public void OnValidate() {
        if (baseDamage > 100) baseDamage = 100;
    }
}

class ShipController : EngineManagedMotor {
    public WeaponDataAsset equippedWeapon;
    public int shield = 50;
    public int seqStage = 0;
    public Vec2 lastHitNormal;
    public Fixed lastPenetration;

    public void OnCollisionEnter(long other) {
        lastHitNormal = GetCollisionNormal2D(1, other);
        lastPenetration = GetCollisionPenetration2D(1, other);
    }

    sequence ChargeSequence(long selfId) {
        seqStage = 1;
        yield wait_ticks(5);
        seqStage = 2;
    }
}
)RS";
        game::GameScriptCompiler compiler(api);
        auto compiledV1 = compiler.compile({{"p1_v1.rs", p1SrcV1}});
        require(
            compiledV1.succeeded(),
            "P1EngineRuntime V1 compile failed:\n" + diagnosticsText(compiledV1.diagnostics));
        rt.replaceProgram(compiledV1.program);

        // P1-1: Generational Entity Handle ABA protection
        const auto genId1 = scene.spawnGenerationalEntity("TempDrone");
        require(
            game::entityIdGeneration(genId1) == 1 && scene.isEntityAlive(genId1),
            "P1-1: first generational entity should have generation == 1 and be alive");
        const auto slot1 = game::entityIdSlot(genId1);
        require(scene.detach(genId1), "detach genId1 failed");
        require(!scene.isEntityAlive(genId1), "detached genId1 must not be alive");
        const auto genId2 = scene.spawnGenerationalEntity("ReusedSlotDrone");
        require(
            game::entityIdSlot(genId2) == slot1 &&
                game::entityIdGeneration(genId2) == 2 &&
                !scene.isEntityAlive(genId1) &&
                scene.isEntityAlive(genId2),
            "P1-1: reused generational slot must increment generation to 2 and reject stale genId1 handle");
        scene.setEntityName(genId1, "StaleGhost");
        scene.setLocalPosition2D(genId1, game::Vec2::fromInts(99, 99));
        require(
            scene.findEntityByName("StaleGhost") == 0 &&
                !scene.attachComponent(genId1, "Evolution.P1EngineRuntime::ShipController"),
            "P1-1: mutating APIs must reject stale generational handle genId1");
        runtime::RuntimeError importErr;
        const std::string genSceneJson = game::exportSceneJson(scene, importErr);
        require(
            !genSceneJson.empty() &&
                game::importSceneJson(scene, genSceneJson, importErr) &&
                scene.isEntityAlive(genId2),
            "P1-1: exportSceneJson/importSceneJson must preserve generational entity liveness for genId2");
        (void)scene.detach(genId2);

        // P1-3: Non-shared_ptr Raw Native Pointer Binding with custom deleter hook
        EngineManagedMotor stackMotor;
        stackMotor.rpm = 1200;
        bool deleterCalled = false;
        runtime::RuntimeError err;
        auto rawBoundShip = rt.createDerivedRawNative<EngineManagedMotor>(
            "Evolution.P1EngineRuntime::ShipController",
            &stackMotor,
            err,
            {},
            [&](EngineManagedMotor* ptr) {
                ptr->customReleased = true;
                deleterCalled = true;
            });
        require(rawBoundShip.has_value(), "P1-3: createDerivedRawNative failed: " + err.message);
        require(
            rt.invoke(*rawBoundShip, *rt.findMethod(rawBoundShip->type(), "Boost", 1), {std::int64_t{300}}).succeeded &&
                stackMotor.rpm == 1500,
            "P1-3: invoking inherited C++ method on raw pointer binding should update stackMotor.rpm to 1500");
        require(rt.releaseNative(*rawBoundShip) && deleterCalled && stackMotor.customReleased,
            "P1-3: releasing raw native handle should invoke custom non-owning deleter callback");

        // P1-4: Reusable Scriptable Data Asset (`createAsset` / `findAsset`)
        auto weaponAsset = rt.createAsset(
            "assets/weapons/plasma.rsasset",
            "Evolution.P1EngineRuntime::WeaponDataAsset",
            err,
            {{"baseDamage", std::int64_t{250}}});
        require(weaponAsset.has_value(), "P1-4: createAsset failed: " + err.message);
        // OnValidate should have clamped baseDamage from 250 to 100
        auto* lookedUpAsset = rt.findAsset("assets/weapons/plasma.rsasset");
        require(
            lookedUpAsset != nullptr &&
                std::get<std::int64_t>(*rt.getMember(*lookedUpAsset, "baseDamage", err)) == 100,
            "P1-4: createAsset should run OnValidate and register retrievable ScriptObject in asset registry");

        // P1-2: Spatial Hash Grid Broadphase + CollisionContact2D normal & penetration
        require(scene.attachComponent(1, "Evolution.P1EngineRuntime::ShipController"), "attach ship 1");
        require(scene.attachComponent(2, "Evolution.P1EngineRuntime::ShipController"), "attach ship 2");
        scene.setLocalPosition2D(1, game::Vec2::fromInts(0, 0));
        scene.setLocalPosition2D(2, game::Vec2::fromInts(3, 0));

        game::SceneCollider2D circleCol;
        circleCol.isCircle = true;
        circleCol.radius = game::Fixed::fromInt(2); // sumR = 4, dist = 3 => penetration = 1, normal = (1, 0)
        scene.setEntityCollider2D(1, circleCol);
        scene.setEntityCollider2D(2, circleCol);

        // Add 20 distant colliders in far cells to verify Spatial Hash Grid culls distant pairs
        for (game::SceneEntityId farId = 10; farId < 30; ++farId) {
            scene.setLocalPosition2D(farId, game::Vec2::fromInts(static_cast<int>(farId * 100), 500));
            scene.setEntityCollider2D(farId, circleCol);
        }
        require(scene.stepPhysics2D() > 0, "stepPhysics2D should fire overlap callbacks");
        // Total colliders = 22 -> O(N^2) would be 22*21/2 = 231 pairs; Spatial Hash Grid only pairs entities 1 & 2 (1 pair)!
        require(
            scene.lastBroadphaseCandidatePairs2D() == 1,
            "P1-2: Spatial Hash Grid broadphase should cull 20 distant colliders down to 1 candidate pair, got " +
                std::to_string(scene.lastBroadphaseCandidatePairs2D()));

        const auto contact = scene.getCollisionContact2D(1, 2);
        require(
            contact.has_value() &&
                contact->normal == game::Vec2::fromInts(1, 0) &&
                contact->penetration == game::Fixed::fromInt(1),
            "P1-2: CollisionContact2D between circles at (0,0) and (3,0) with r=2 should have normal=(1,0) and penetration=1");

        // P1-2b: Circle-vs-AABB contact manifold + AABB Y-axis raycast normal
        game::SceneCollider2D aabbCol;
        aabbCol.isCircle = false;
        aabbCol.halfExtents = game::Vec2::fromInts(2, 2);
        scene.setLocalPosition2D(3, game::Vec2::fromInts(6, 0)); // box X in [4, 8]; circle 2 at (3,0) r=2 overlaps [4,5] -> penetration 1, normal (1,0)
        scene.setEntityCollider2D(3, aabbCol);
        (void)scene.stepPhysics2D();
        const auto circleBoxContact = scene.getCollisionContact2D(2, 3);
        require(
            circleBoxContact.has_value() &&
                circleBoxContact->normal == game::Vec2::fromInts(1, 0) &&
                circleBoxContact->penetration == game::Fixed::fromInt(1),
            "P1-2b: Circle-vs-AABB CollisionContact2D should compute exact normal=(1,0) and penetration=1");
        const auto yRayHit = scene.raycast2D(
            game::Vec2::fromInts(6, 10), game::Vec2::fromInts(0, -1), game::Fixed::fromInt(20));
        require(
            yRayHit.has_value() && yRayHit->entity == 3 && yRayHit->normal == game::Vec2::fromInts(0, 1),
            "P1-2b: AABB raycast2D along -Y into top face of box at (6,0) must return normal=(0,1)");
        scene.removeEntityCollider2D(3);

        // P1-4b: Hot reload while ChargeSequence is suspended in `yield wait_ticks(5)`
        game::SceneGameplayDriver driver(scene, host);
        require(
            scene.invoke(1, "ChargeSequence", {runtime::LongValue{1}}).succeeded,
            "ChargeSequence start failed");
        (void)driver.advanceTicks(2);
        auto* ship1V1 = scene.getComponent(1, "ShipController");
        require(
            std::get<std::int64_t>(*rt.getMember(*ship1V1, "seqStage", err)) == 1,
            "ChargeSequence should be at seqStage 1 after 2 ticks");

        // Hot-reload V2 that adds a new field `armorBonus` while ChargeSequence is suspended at tick 2/5
        const char* p1SrcV2 = R"RS(
module Evolution.P1EngineRuntime;
import Engine.Raw;
import RealScript.Math;
import RealScript.Game;
import RealScript.Scene;

class WeaponDataAsset {
    public string weaponName = "PlasmaRifle";
    public int baseDamage = 40;
}

class ShipController : EngineManagedMotor {
    public WeaponDataAsset equippedWeapon;
    public int shield = 50;
    public int armorBonus = 25;
    public int seqStage = 0;
    public Vec2 lastHitNormal;
    public Fixed lastPenetration;

    sequence ChargeSequence(long selfId) {
        seqStage = 1;
        yield wait_ticks(5);
        seqStage = 2 + armorBonus;
    }
}
)RS";
        auto compiledV2 = compiler.compile({{"p1_v2.rs", p1SrcV2}});
        require(compiledV2.succeeded(), "P1EngineRuntime V2 compile failed");
        const auto reloadRep = scene.hotReload(compiledV2.program);
        require(reloadRep.succeeded, "P1-4: hotReload during wait_ticks failed");

        // Advance remaining 3 ticks -> migrated $sequence_state_ChargeSequence resumes and sets seqStage = 2 + 25 = 27!
        (void)driver.advanceTicks(3);
        auto* ship1V2 = scene.getComponent(1, "ShipController");
        require(
            std::get<std::int64_t>(*rt.getMember(*ship1V2, "seqStage", err)) == 27,
            "P1-4: ChargeSequence should preserve $sequence_state_* across structural hotReload and resume to set seqStage=27");
    }

    // 3. P2-1: Resilient Parser MemberAccess Recovery (no double-rebuild hack),
    //    P2-2: Collection Indexers (`dict[k]`, `list[i]`), Typed Result Sequences (`sequence<int>`), and Extension Methods (`this T`),
    //    P2-3: LSP `semanticTokens/full`, `inlayHint`, and DAP `setVariable`
    {
        auto host = std::make_shared<game::GameplayHost>(60);
        game::GameApi api;
        require(game::installGameplayBindings(api, host), "installGameplayBindings failed");

        const char* p2Src = R"RS(
module Evolution.P2LanguageAndTools;
import RealScript.Game;

int ClampScore(this int value, int minVal, int maxVal) {
    if (value < minVal) return minVal;
    if (value > maxVal) return maxVal;
    return value;
}

class InventoryManager {
    public int finalScore = 0;
    public int idxCalls = 0;

    public int NextIndex() {
        idxCalls += 1;
        return 1;
    }

    public int TestIndexersAndExtension() {
        List<int> numbers = new List<int>();
        numbers.Add(10);
        numbers.Add(20);
        numbers[1] = numbers[0] + 35; // 45 via List[i] indexer
        numbers[NextIndex()] += 5;    // 50 via compound List[i] assignment; NextIndex() must be called ONCE
        if (idxCalls != 1) return -999;

        Dictionary<string, int> prices = new Dictionary<string, int>();
        prices["sword"] = 120;        // Put via Dictionary[k] indexer
        prices["sword"] += 30;        // 150 via compound Dictionary[k] assignment
        int rawTotal = numbers[1] + prices["sword"]; // 50 + 150 = 200
        return rawTotal.ClampScore(0, 180);          // Extension method on int -> 180
    }

    sequence<int> ComputeBonusSequence(long selfId, int baseVal) {
        yield wait_ticks(2);
        return baseVal * 3;
    }
}
)RS";
        game::GameScriptCompiler compiler(api);
        auto compiled = compiler.compile({{"p2_lang.rs", p2Src}});
        require(
            compiled.succeeded(),
            "P2LanguageAndTools compile failed:\n" + diagnosticsText(compiled.diagnostics));

        game::ScriptRuntime rt(compiled.program);
        game::SceneScriptRuntime scene(rt);
        game::SceneGameplayDriver driver(scene, host);

        require(scene.attachComponent(1, "Evolution.P2LanguageAndTools::InventoryManager"), "attach InventoryManager");
        const auto idxRes = scene.invoke(1, "TestIndexersAndExtension");
        require(
            idxRes.succeeded && std::get<std::int64_t>(idxRes.value) == 180,
            "P2-2: List/Dictionary indexers and int.ClampScore extension method expected 180");

        require(
            scene.invoke(1, "ComputeBonusSequence", {runtime::LongValue{1}, std::int64_t{14}}).succeeded,
            "ComputeBonusSequence start failed");
        (void)driver.advanceTicks(2);
        const auto seqDone = scene.invoke(1, "IsComputeBonusSequenceCompleted");
        const auto seqVal = scene.invoke(1, "GetComputeBonusSequenceResult");
        require(
            seqDone.succeeded && std::get<bool>(seqDone.value) &&
                seqVal.succeeded && std::get<std::int64_t>(seqVal.value) == 42,
            "P2-2: sequence<int> should complete after 2 ticks with GetComputeBonusSequenceResult() == 42");

        // P2-1 & P2-3: Verify LSP semanticTokens/full, inlayHint, and resilient dot-completion without line-blanking hack
        tooling::LspServer lsp;
        std::ostringstream notifications;
        const auto initResp = lsp.handle(tooling::Json::Object{
            {"jsonrpc", "2.0"},
            {"id", 1},
            {"method", "initialize"},
            {"params", tooling::Json::Object{}},
        }, &notifications);
        const auto* caps = initResp.find("result")->find("capabilities");
        require(
            caps && caps->find("semanticTokensProvider") && caps->find("inlayHintProvider"),
            "P2-3: LspServer must advertise semanticTokensProvider and inlayHintProvider");

        tooling::LanguageService ls;
        ls.open("p2_lsp.rs", R"RS(module Evolution.P2Lsp;
class Bag {
    public int coins;
}
int Compute(Bag b) {
    var totalCoins = 100;
    b.
    return totalCoins;
}
)RS", 1);
        const auto compItems = ls.completion("p2_lsp.rs", {6, 6});
        bool foundCoins = false;
        for (const auto& item : compItems) {
            if (item.label == "coins") foundCoins = true;
        }
        require(foundCoins, "P2-1: resilient Parser must resolve 'b.' completion without line-blanking hack");

        const auto semTokens = ls.semanticTokensEncoded("p2_lsp.rs");
        require(!semTokens.empty() && semTokens.size() % 5 == 0,
            "P2-3: semanticTokensEncoded must return 5-tuple encoded token stream");

        const auto hints = ls.inlayHints("p2_lsp.rs");
        require(!hints.empty() && hints.front().label == ": int",
            "P2-3: inlayHints must infer ': int' for 'var totalCoins = 100;'");

        // P2-3: Verify DAP setVariable modifies live frame variable at breakpoint
        const char* dapSrc = R"RS(
module Evolution.DapSetVar;
int RunCalc() {
    int score = 10;
    int bonus = 5;
    return score + bonus;
}
)RS";
        auto dapCompiled = compiler.compile({{"dap_setvar.rs", dapSrc}});
        require(dapCompiled.succeeded(), "dap_setvar.rs failed to compile");

        debug::DapServer dap(dapCompiled.program.program(), dapCompiled.program.heap());
        std::ostringstream dapEvents;
        (void)dap.handle(tooling::Json::Object{
            {"seq", 1},
            {"type", "request"},
            {"command", "setBreakpoints"},
            {"arguments", tooling::Json::Object{
                {"source", tooling::Json::Object{{"path", "dap_setvar.rs"}}},
                {"breakpoints", tooling::Json::Array{
                    tooling::Json::Object{{"line", 6}, {"column", 1}},
                }},
            }},
        }, &dapEvents);

        (void)dap.handle(tooling::Json::Object{
            {"seq", 2},
            {"type", "request"},
            {"command", "launch"},
            {"arguments", tooling::Json::Object{
                {"function", "Evolution.DapSetVar::RunCalc"},
            }},
        }, &dapEvents);

        // Mutate `score` from 10 to 95 via DAP `setVariable`!
        const auto setVarResp = dap.handle(tooling::Json::Object{
            {"seq", 3},
            {"type", "request"},
            {"command", "setVariable"},
            {"arguments", tooling::Json::Object{
                {"variablesReference", 3},
                {"name", "score"},
                {"value", "95"},
            }},
        }, &dapEvents);
        require(
            setVarResp.find("success") && setVarResp.find("success")->boolValue() &&
                setVarResp.find("body")->find("value")->stringValue() == "95",
            "P2-3: DAP setVariable should update 'score' to 95");

        const auto badSetVarResp = dap.handle(tooling::Json::Object{
            {"seq", 4},
            {"type", "request"},
            {"command", "setVariable"},
            {"arguments", tooling::Json::Object{
                {"variablesReference", 3},
                {"name", "score"},
                {"value", "\"not_an_int\""},
            }},
        }, &dapEvents);
        require(
            badSetVarResp.find("success") && !badSetVarResp.find("success")->boolValue(),
            "P2-3: DAP setVariable must reject assigning a string to an int variable");

        (void)dap.handle(tooling::Json::Object{
            {"seq", 5},
            {"type", "request"},
            {"command", "continue"},
        }, &dapEvents);
    }
}

int main() {
    try {
        std::cout << "[1/12] testScriptInheritingNativeAndMathMarshaling..." << std::endl;
        testScriptInheritingNativeAndMathMarshaling();
        std::cout << "[2/12] testMultiComponentSceneAndEditorMode..." << std::endl;
        testMultiComponentSceneAndEditorMode();
        std::cout << "[3/12] testInspectorAttributesJsonAndStatefulHotReload..." << std::endl;
        testInspectorAttributesJsonAndStatefulHotReload();
        std::cout << "[4/12] testCoroutineWaitSeconds..." << std::endl;
        testCoroutineWaitSeconds();
        std::cout << "[5/12] testLspStubsDotCompletionAndSignatureHelp..." << std::endl;
        testLspStubsDotCompletionAndSignatureHelp();
        std::cout << "[6/12] testTieredExecutionDispatcher..." << std::endl;
        testTieredExecutionDispatcher();
        std::cout << "[7/12] testStage1EditorDataflowSignalsAndHotReloadRefs..." << std::endl;
        testStage1EditorDataflowSignalsAndHotReloadRefs();
        std::cout << "[8/12] testStage2CoroutinesAndDebuggerEvaluation..." << std::endl;
        testStage2CoroutinesAndDebuggerEvaluation();
        std::cout << "[9/12] testStage3FastMathAndChromeTracing..." << std::endl;
        testStage3FastMathAndChromeTracing();
        std::cout << "[10/12] testBoostStage1InlineMathAndValueSpanBindings..." << std::endl;
        testBoostStage1InlineMathAndValueSpanBindings();
        std::cout << "[11/12] testBoostStage2ConstStaticStringInterpolationMathAndContainers..." << std::endl;
        testBoostStage2ConstStaticStringInterpolationMathAndContainers();
        std::cout << "[12/13] testBoostStage3LspExactSymbolIdReferencesRenameAndChainedCompletion..." << std::endl;
        testBoostStage3LspExactSymbolIdReferencesRenameAndChainedCompletion();
        std::cout << "[13/14] testCompleteEvolutionMilestones..." << std::endl;
        testCompleteEvolutionMilestones();
        std::cout << "[14/15] testModernEngineParityCompletionGoals..." << std::endl;
        testModernEngineParityCompletionGoals();
        std::cout << "[15/17] testZeroHeapInlineStructAndEcsQueryMirLowering..." << std::endl;
        testZeroHeapInlineStructAndEcsQueryMirLowering();
        std::cout << "[16/17] testRound2EngineGapsCompletion..." << std::endl;
        testRound2EngineGapsCompletion();
        std::cout << "[17/18] testRound3AuditFixesAndGodotUnityParity..." << std::endl;
        testRound3AuditFixesAndGodotUnityParity();
        std::cout << "[18/18] testRound4RoadmapP0ToP2All10Items..." << std::endl;
        testRound4RoadmapP0ToP2All10Items();
        std::cout << "All engine integration evolution tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Engine integration evolution test failed: " << ex.what() << "\n";
        return 1;
    }
}

