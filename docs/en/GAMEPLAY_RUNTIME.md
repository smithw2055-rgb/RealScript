# Deterministic Gameplay Runtime

[Documentation Home](README.md) | [Game Scripting SDK](GAME_SCRIPTING_SDK.md)

The deterministic gameplay runtime extends the existing Game Scripting SDK with fixed-tick services that are useful for RTS, tower-defense, simulation, replay, rollback, and engine-authored gameplay logic.

This layer deliberately stays separate from the parser, binder, MIR, bytecode, and GC. It can therefore be used by bytecode, AOT, and JIT programs without giving each execution backend a different gameplay implementation.

## Included stages

### Phase 8 — Deterministic gameplay primitives

`GameplayPrimitives.h` provides:

- generation-checked `EntityId` values and deterministic slot reuse;
- a fixed-tick clock with catch-up limits and dropped-time accounting;
- independent PCG random streams;
- a tick scheduler ordered by due tick and stable timer id;
- a typed deterministic event queue;
- in-memory snapshots and stable state hashes.

```cpp
#include "realscript/game/Gameplay.h"

realscript::game::DeterministicGameplayRuntime gameplay(60, 1234, 7);
auto entity = gameplay.entities().create();

auto timer = gameplay.scheduler().scheduleAfter(
    gameplay.clock().tick(),
    5,
    entity.packed());
```

No wall-clock timestamp, pointer, unordered-container iteration, or platform-specific random source participates in simulation ordering.

### Phase 9 — Script contracts, metadata, events, and fixed-step driving

`GameplayScripting.h` adds a reusable `GameplayHost` and `SceneGameplayDriver`.

```cpp
auto host = std::make_shared<realscript::game::GameplayHost>(60, seed, stream);

host->scheduleCall(entityId, "OnReloaded", 30);
host->subscribe("unit.died", entityId, "OnUnitDied");
host->publish("unit.died", {std::int32_t{reward}}, 1);

realscript::game::SceneGameplayDriver driver(sceneScripts, host);
driver.advance(frameDeltaSeconds);
```

Per tick, the driver uses this stable order:

1. increment the fixed tick;
2. drain due timers and deterministic events;
3. invoke the corresponding script callbacks;
4. call `SceneScriptRuntime::fixedUpdate`;
5. evaluate scene triggers;
6. flush the existing scene event queue.

`GameplayHost::startSequence` is a deterministic timeline/coroutine substitute. Each sequence is compiled by the host into fixed-tick callbacks. It does not depend on C++ stack suspension, wall-clock time, or backend-specific continuation objects.

```cpp
host->startSequence(entityId, {
    {1, "OnWindup", {}},
    {12, "OnRelease", {}},
    {6, "OnRecover", {}},
});
```

### Script-facing host API

Call `installGameplayBindings` before compiling game scripts:

```cpp
realscript::game::GameApi api;
auto host = std::make_shared<realscript::game::GameplayHost>();
realscript::game::installGameplayBindings(api, host);
```

The default `RealScript.Game` module exposes:

```text
long CurrentTick()
int TickRate()
long CreateEntity()
bool DestroyEntity(long entity)
bool IsEntityAlive(long entity)
int RandomInt(int minimumInclusive, int maximumExclusive)
long Schedule(long target, string callback, int delayTicks)
long ScheduleRepeating(long target, string callback,
                       int delayTicks, int intervalTicks, int repeatCount)
bool CancelTimer(long timer)
int CancelEntityTimers(long target)
long SubscribeOnce(string topic, long target, string callback)
long Publish(string topic, int delayTicks)
```

A repeat count of zero means repeat forever. A zero delay is normalized to the next simulation tick so callbacks cannot recursively re-enter the current scheduler drain.

### Deterministic coroutine sequences

Source-language `sequence` methods lower to explicit state machines backed by `RealScript.Game` scheduling primitives:

- `yield wait_ticks(ticks)` suspends for an exact integer tick count;
- `yield wait_seconds(seconds)` converts seconds using `TickRate()` (clamped to at least 1 tick);
- `yield wait_event(topic)` registers a one-shot event subscription via `SubscribeOnce(topic, entityId, resumeCallback)` and resumes when the topic is published;
- `yield wait_until(condition)` polls `condition` once per simulation tick until it evaluates to `true`.

Disabling or detaching a component in `SceneScriptRuntime` automatically invokes the generated `Cancel<SequenceName>()` methods to cancel pending timers and one-shot event subscriptions.

## Interface-like contracts

In addition to native source-language `interface` declarations, `ScriptContract` provides an engine-level contract that validates required callback names and arities against compiled script metadata.

```cpp
realscript::game::ScriptContract abilityContract{
    "Ability",
    {
        {"CanActivate", 1, true},
        {"Activate", 1, true},
        {"Cancel", 0, false},
    },
};

auto report = realscript::game::validateScriptContract(
    scripts,
    "Game.Abilities::Fireball",
    abilityContract);
```

This is suitable for behavior, ability, quest, AI, and editor validation.

## Attribute-style metadata and Inspector schema

`ScriptMetadataRegistry` stores deterministic type and member annotations for serializers, inspectors, replication, save systems, and editor tooling. Source attributes (`[Export]`, `[SerializeField]`, `[HideInInspector]`, `[Range]`, `[Tooltip]`, `[Header]`, `[ReadOnly]`, `[FormerlySerializedAs]`, `[EntityRef]`, `[ComponentRef]`, `[AssetRef]`, `[Tool]`, `[ExecuteInEditMode]`) are automatically extracted from compiled scripts into `GameProgram::metadata()`, and can also be registered from C++:

```cpp
metadata.addTypeAttribute(
    "Game.Unit::Marine",
    {"Serializable", {{"version", std::int32_t{2}}}});
metadata.addMemberAttribute(
    "Game.Unit::Marine",
    "Health",
    {"Replicated", {{"channel", std::string{"state"}}}});
```

`inspectTypeSchema(program, metadata, typeName)` builds a rich `InspectorTypeSchema` containing field types, default values, range constraints, enum options, recursive nested struct schemas, collection element/key types, reference kinds (`Entity`/`Component`/`Asset`), `hasOnValidate`, and `event` signal signatures.

## Phase 10 — Save, replay, and rollback state

`GameplayStateCodec.h` defines the versioned `RSGS` gameplay-state container.

```cpp
realscript::game::GameplayStateError error;
auto bytes = realscript::game::encodeGameplayHostState(*host, error);

realscript::game::GameplayHost restored;
realscript::game::restoreGameplayHostState(restored, bytes, error);
```

The codec includes:

- entity slots and generations;
- fixed clock and catch-up state;
- random stream state;
- timers and repeat counters;
- pending deterministic events;
- script callback payloads;
- event subscriptions;
- sequence ownership;
- a stable state hash and defensive size limits.

The container uses fixed-width little-endian fields and canonical floating-point encoding. A corrupt, truncated, oversized, unsupported, structurally invalid, or hash-mismatched state is rejected before it can replace the live host state.

The codec captures gameplay scheduling state. Script object fields remain covered by `GameProductization.h` and `ScriptObjectState`; an engine rollback frame should combine both snapshots under one engine-level frame id.

## Threading and ownership

A `GameplayHost` is an engine-owned simulation object. Mutations and `SceneGameplayDriver::step` should run on the simulation thread. Read-only state copies may be moved to replay, diagnostics, or network workers after a snapshot has been taken.

## Language and runtime integration

Source-language features from Phases 18–24 and the engine integration evolution lower directly onto these deterministic gameplay primitives:

- `sequence` methods lower to explicit state machines using `Schedule`, `SubscribeOnce`, and `CancelTimer` for `yield wait_ticks`, `yield wait_seconds`, `yield wait_event`, and `yield wait_until`;
- source `[Attribute]` declarations populate `ScriptMetadataRegistry` and `InspectorTypeSchema` automatically;
- language `event` declarations integrate with `SceneScriptRuntime::connectSignal` and survive stateful hot reload.
