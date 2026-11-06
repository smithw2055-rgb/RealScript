#include "realscript/game/GameProductization.h"
#include "realscript/game/GameplayMetadata.h"
#include "realscript/game/GameplayScripting.h"
#include "realscript/tooling/LanguageService.h"
#include "realscript/tooling/LspServer.h"

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
    const auto* retStruct = std::get_if<realscript::runtime::StructValue>(&advExec.value);
    require(
        retStruct && retStruct->storage && retStruct->storage->fields.size() == 2,
        "returned value should be Vec2 StructValue");
    const auto retX = realscript::game::detail::extractFixedStructValue(
        retStruct->storage->fields[0]);
    const auto retY = realscript::game::detail::extractFixedStructValue(
        retStruct->storage->fields[1]);
    require(
        retX.has_value() && retY.has_value() &&
            *retX == realscript::game::Fixed::fromInt(18) &&
            *retY == realscript::game::Fixed::fromInt(-1),
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

} // namespace

int main() {
    try {
        testScriptInheritingNativeAndMathMarshaling();
        testMultiComponentSceneAndEditorMode();
        testInspectorAttributesJsonAndStatefulHotReload();
        testCoroutineWaitSeconds();
        testLspStubsDotCompletionAndSignatureHelp();
        testTieredExecutionDispatcher();
        std::cout << "All engine integration evolution tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Engine integration evolution test failed: " << ex.what() << "\n";
        return 1;
    }
}

