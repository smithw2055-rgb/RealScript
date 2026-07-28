# Game Scripting SDK

The Game Scripting SDK connects RealScript's compiler and runtime to a C++17 game object model. It provides compiler-visible host APIs, typed C++ bindings, rooted script instances, scene lifecycle dispatch, queued events, and condition-based triggers.

## Integration model

```text
C++ game objects / ECS / scene graph
                |
                v
        SceneScriptRuntime
                |
                v
          ScriptRuntime
                |
                v
 GameApi + BindingRegistry + NativeHandleRegistry
                |
                v
       RealScript ProgramImage
```

`GameApi` generates ordinary RealScript declarations for host modules. These declarations participate in parsing, type checking, member lookup, metadata, and bytecode generation. Before linking, generated placeholder function bodies are removed, so calls resolve through `BindingRegistry` instead of executing the placeholders. This keeps one compiler pipeline and avoids a second handwritten binding language.

## Register C++ functions and classes

```cpp
#include "realscript/game/GameScripting.h"

struct Character {
    void takeDamage(int amount);
    int health() const;
    void setHealth(int value);
};

realscript::game::GameApi api;
api.type<Character>("Engine.Game", "Character")
    .method<&Character::takeDamage>("TakeDamage")
    .property<&Character::health, &Character::setHealth>("Health");

api.function(
    "Engine.Game",
    "ClampDamage",
    [](int value) { return value < 0 ? 0 : value; });
```

The initial binding surface supports `void`, `bool`, integral, floating-point, `std::string`, `std::string_view`, `std::shared_ptr<T>`, and `T*` for registered native object types. Free functions, const/non-const member functions, properties, and all existing determinism policies are supported.

Bindings use stable names:

```text
Engine.Game::ClampDamage
Engine.Game::Character.TakeDamage
Engine.Game::Character.get_Health
Engine.Game::Character.set_Health
```

Native objects are exposed through generated script wrappers containing a generation-checked `NativeHandle`. Binding trampolines verify wrapper type, registry identity, generation, and C++ type before invocation.

## Compile scripts

```cpp
realscript::game::GameScriptCompiler compiler(api);
auto result = compiler.compile({
    {"controller.rs", R"(
        module Game;
        import Engine.Game;

        class Controller
        {
            Character owner;

            void OnCreate(Character value)
            {
                owner = value;
            }

            void OnDamage(int amount)
            {
                owner.TakeDamage(ClampDamage(amount));
            }
        }
    )"},
});
```

`GameCompileResult` contains the linked `GameProgram`, emitted bytecode modules, and diagnostics. `GameProgram` retains the program image, bindings, managed heap, and native handle registry.

## Create and call script objects

```cpp
realscript::game::ScriptRuntime scripts(result.program);
realscript::runtime::RuntimeError error;

auto controller = scripts.createObject("Game::Controller", error);
auto character = scripts.wrapNative(
    api.canonicalTypeName<Character>(),
    std::make_shared<Character>(),
    error,
    "player");

auto onCreate = scripts.findMethod(controller->type(), "OnCreate", 1);
scripts.invoke(*controller, *onCreate, {character->value()});
```

A `ScriptObject` owns a `PersistentRoot`, so its managed instance remains alive while retained by C++. `ScriptMethod` caches resolved method metadata, avoiding per-frame qualified-name construction. The host API also supports constructor selection, field/property access, static invocation, exact runtime type checks, and execution budgets.

## Scene lifecycle

```cpp
realscript::game::SceneScriptRuntime scene(scripts);
realscript::game::SceneScriptOptions options;
options.owner = character->value();
options.initialMembers["detectionRange"] = 12.0;

scene.attach(1001, "Game::Controller", std::move(options));
scene.start();
scene.fixedUpdate(fixedDeltaTime);
scene.update(deltaTime);
scene.flushEvents();
scene.evaluateTriggers();
scene.lateUpdate(deltaTime);
```

Optional lifecycle methods are resolved and cached when a script is attached:

```text
OnCreate()
OnCreate(owner)
OnStart()
OnEnable()
OnDisable()
OnUpdate(double deltaTime)
OnFixedUpdate(double fixedDeltaTime)
OnLateUpdate(double deltaTime)
OnDestroy()
```

## Events and triggers

Direct or queued event delivery:

```cpp
scene.dispatch({1001, "OnDamage", {std::int64_t{25}}});
scene.enqueue({1001, "OnDamage", {std::int64_t{25}}});
scene.flushEvents();
```

`SceneScriptRuntime::BroadcastTarget` broadcasts to every enabled script implementing a compatible callback.

Condition-based trigger:

```cpp
realscript::game::ScriptTrigger trigger;
trigger.id = "village-entrance";
trigger.target = 1001;
trigger.callback = "OnPlayerEntered";
trigger.arguments = {player->value()};
trigger.once = true;
trigger.condition = [&world] {
    return world.playerEntered("VillageEntrance");
};
scene.addTrigger(std::move(trigger));
```

The engine owns condition evaluation; scripts own the reaction. This keeps spatial queries, quest persistence, networking, and editor-authored scene data outside the language runtime.

## Safety and current limits

Every invocation returns `ExecutionResult`; scene failures are accumulated as `SceneScriptError` values with entity, callback, runtime error, and script stack. Stale native handles, stale managed references, wrong exact types, execution budgets, and Strict/Record/Replay binding policies remain enforced.

The scene layer is engine-neutral and does not prescribe an ECS, serializer, asset format, editor, or event bus. Generic `Get<T>()`, inheritance-based behaviors, interfaces, delegates, and coroutine lifecycle methods depend on future language features.
