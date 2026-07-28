#include "realscript/game/GameScripting.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string diagnosticsText(
    const realscript::diagnostics::DiagnosticBag& diagnostics) {
    std::string result;
    for (const auto& diagnostic : diagnostics.items()) {
        if (!result.empty()) result.push_back('\n');
        result += diagnostic.code + ": " + diagnostic.message;
    }
    return result;
}

struct Character {
    int health = 100;

    void takeDamage(int amount) { health -= amount; }
    int healthValue() const { return health; }
    void setHealth(int value) { health = value; }
};

realscript::game::GameApi makeApi() {
    realscript::game::GameApi api;
    api.type<Character>("Engine.Game", "Character")
        .method<&Character::takeDamage>("TakeDamage")
        .property<&Character::healthValue, &Character::setHealth>("Health");
    require(api.function(
        "Engine.Game",
        "Add",
        [](int left, int right) { return left + right; }),
        "failed to register native Add function");
    require(api.valid(), "game API registration failed");
    return api;
}

const char* controllerSource = R"(
module Game.Controller;
import Engine.Game;

class Controller
{
    Character owner;

    void OnCreate(Character value)
    {
        owner = value;
    }

    int Apply(int damage)
    {
        owner.TakeDamage(damage);
        return owner.Health + Add(1, 2);
    }
}
)";

void testTypedHostBindingsAndScriptObjects() {
    auto api = makeApi();
    realscript::game::GameScriptCompiler compiler(api);
    auto compiled = compiler.compile({{"controller.rs", controllerSource}});
    require(compiled.succeeded(),
        "game script compilation failed:\n" + diagnosticsText(compiled.diagnostics));
    for (const auto& module : compiled.modules) {
        for (const auto& function : module.functions) {
            require(function.name != "__rs_native_Character_set_Health",
                "native property setter trampoline remained in the program image");
        }
    }

    realscript::game::ScriptRuntime runtime(compiled.program);
    realscript::runtime::RuntimeError error;
    auto controller = runtime.createObject("Game.Controller::Controller", error);
    require(controller.has_value(), "failed to create Controller: " + error.message);

    auto nativeCharacter = std::make_shared<Character>();
    auto character = runtime.wrapNative(
        api.canonicalTypeName<Character>(),
        nativeCharacter,
        error,
        "player");
    require(character.has_value(), "failed to wrap Character: " + error.message);

    const auto onCreate = runtime.findMethod(controller->type(), "OnCreate", 1);
    require(onCreate.has_value(), "OnCreate was not resolved");
    const auto created = runtime.invoke(*controller, *onCreate, {character->value()});
    require(created.succeeded, "OnCreate failed: " + created.error.message);

    const auto apply = runtime.findMethod(controller->type(), "Apply", 1);
    require(apply.has_value(), "Apply was not resolved");
    const auto result = runtime.invoke(
        *controller, *apply, {std::int64_t{25}});
    require(result.succeeded, "Apply failed: " + result.error.message);
    require(std::get<std::int64_t>(result.value) == 78,
        "native method/property/free-function result was incorrect");
    require(nativeCharacter->health == 75,
        "script did not mutate the C++ Character instance");

    require(runtime.setMember(
        *character, "Health", std::int64_t{64}, error),
        "native property setter failed: " + error.message);
    require(nativeCharacter->health == 64,
        "native property setter left health at " +
        std::to_string(nativeCharacter->health));
    const auto health = runtime.getMember(*character, "Health", error);
    require(health.has_value(), "native property getter returned no value: " + error.message);
    require(std::get<std::int64_t>(*health) == 64,
        "native property getter returned " +
        std::to_string(std::get<std::int64_t>(*health)));
}

const char* sceneSource = R"(
module Game.Scene;

class Behavior
{
    int ticks;

    void OnStart()
    {
        ticks = ticks + 1;
    }

    void OnUpdate(double deltaTime)
    {
        if (deltaTime >= 0.0)
        {
            ticks = ticks + 1;
        }
    }

    void OnEntered(int amount)
    {
        ticks = ticks + amount;
    }

    int Read()
    {
        return ticks;
    }
}
)";

void testSceneLifecycleEventsAndTriggers() {
    realscript::game::GameApi api;
    realscript::game::GameScriptCompiler compiler(api);
    auto compiled = compiler.compile({{"scene.rs", sceneSource}});
    require(compiled.succeeded(),
        "scene script compilation failed:\n" + diagnosticsText(compiled.diagnostics));

    realscript::game::ScriptRuntime runtime(compiled.program);
    realscript::game::SceneScriptRuntime scene(runtime);
    require(scene.attach(10, "Game.Scene::Behavior"),
        "failed to attach scene script");
    scene.start();
    scene.update(0.016);
    scene.enqueue({10, "OnEntered", {std::int64_t{2}}});
    require(scene.flushEvents() == 1, "queued script event was not delivered");

    bool condition = true;
    realscript::game::ScriptTrigger trigger;
    trigger.id = "enter-zone";
    trigger.target = 10;
    trigger.callback = "OnEntered";
    trigger.arguments = {std::int64_t{3}};
    trigger.condition = [&condition] { return condition; };
    require(scene.addTrigger(std::move(trigger)), "failed to add script trigger");
    require(scene.evaluateTriggers() == 1, "script trigger did not fire");
    require(scene.evaluateTriggers() == 0, "one-shot script trigger fired twice");

    const auto read = scene.invoke(10, "Read");
    require(read.succeeded && std::get<std::int64_t>(read.value) == 7,
        "scene lifecycle/event/trigger state was incorrect");
    require(scene.errors().empty(), "scene runtime recorded unexpected errors");
    require(scene.detach(10), "failed to detach scene script");
}

void testStaleNativeHandlesAreRejected() {
    auto api = makeApi();
    realscript::game::GameScriptCompiler compiler(api);
    auto compiled = compiler.compile({{"controller.rs", controllerSource}});
    require(compiled.succeeded(),
        "stale-handle fixture compilation failed:\n" + diagnosticsText(compiled.diagnostics));

    realscript::game::ScriptRuntime runtime(compiled.program);
    realscript::runtime::RuntimeError error;
    auto controller = runtime.createObject("Game.Controller::Controller", error);
    auto character = runtime.wrapNative(
        api.canonicalTypeName<Character>(),
        std::make_shared<Character>(),
        error,
        "temporary");
    require(controller && character, "failed to create stale-handle fixture objects");

    const auto onCreate = runtime.findMethod(controller->type(), "OnCreate", 1);
    const auto apply = runtime.findMethod(controller->type(), "Apply", 1);
    require(onCreate && apply, "failed to resolve stale-handle fixture methods");
    require(runtime.invoke(*controller, *onCreate, {character->value()}).succeeded,
        "failed to assign native wrapper to controller");

    const auto handleValue = runtime.getMember(*character, "__native", error);
    require(handleValue && std::holds_alternative<realscript::runtime::NativeHandle>(*handleValue),
        "native wrapper handle field was not available");
    require(runtime.nativeHandles()->release(
        std::get<realscript::runtime::NativeHandle>(*handleValue)),
        "failed to release native handle");

    const auto result = runtime.invoke(*controller, *apply, {std::int64_t{1}});
    require(!result.succeeded &&
            result.error.code == realscript::runtime::ErrorCode::InvalidNativeHandle,
        "stale native handle was not rejected");
}

} // namespace

int main() {
    int failures = 0;
    const auto run = [&](const char* name, auto test) {
        try {
            test();
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
        }
    };

    run("typed host bindings and script objects", testTypedHostBindingsAndScriptObjects);
    run("scene lifecycle, events, and triggers", testSceneLifecycleEventsAndTriggers);
    run("stale native handles", testStaleNativeHandlesAreRejected);
    return failures == 0 ? 0 : 1;
}
