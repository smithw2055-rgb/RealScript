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
long CreateEntity()
bool DestroyEntity(long entity)
bool IsEntityAlive(long entity)
int RandomInt(int minimumInclusive, int maximumExclusive)
long Schedule(long target, string callback, int delayTicks)
long ScheduleRepeating(long target, string callback,
                       int delayTicks, int intervalTicks, int repeatCount)
bool CancelTimer(long timer)
long Publish(string topic, int delayTicks)
```

A repeat count of zero means repeat forever. A zero delay is normalized to the next simulation tick so callbacks cannot recursively re-enter the current scheduler drain.

## Interface-like contracts

RealScript still has no source-language `interface` declaration. `ScriptContract` provides an engine-level contract that validates required callback names and arities against compiled script metadata.

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

This is suitable for behavior, ability, quest, AI, and editor validation. It does not add inheritance, virtual dispatch, or interface values to the language.

## Attribute-style metadata

`ScriptMetadataRegistry` stores deterministic type and member annotations for serializers, inspectors, replication, save systems, and editor tooling.

```cpp
metadata.addTypeAttribute(
    "Game.Unit::Marine",
    {"Serializable", {{"version", std::int32_t{2}}}});
metadata.addMemberAttribute(
    "Game.Unit::Marine",
    "Health",
    {"Replicated", {{"channel", std::string{"state"}}}});
```

Metadata is host-owned in this stage. Source syntax such as `[Serializable]` remains future compiler work.

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

## Deliberate language boundary

These phases productize the game runtime without pretending that all requested C# features have been implemented. The following remain parser/compiler work:

- source-language interfaces and virtual dispatch;
- generics and generic collections;
- lambdas, delegates, and language `event` declarations;
- `for`, `foreach`, `switch`, and pattern matching;
- source attributes;
- `yield`, coroutine state-machine lowering, and `async`;
- `ref`, `out`, mutable struct receivers, and boxing.

The runtime contracts, metadata, named events, and fixed-tick sequences establish stable engine semantics that those future syntax features can lower to rather than creating a second runtime later.
