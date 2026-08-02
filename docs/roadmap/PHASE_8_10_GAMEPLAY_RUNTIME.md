# Phase 8–10 — Game Language Runtime Foundation

## Status

Implemented as an additive layer over the Phase 7 Game Scripting SDK and RS0 productization API.

## Phase 8

- generation-checked entities;
- fixed-tick clock;
- deterministic PCG random stream;
- deterministic timer scheduler;
- typed event queue;
- snapshot and stable hash.

## Phase 9

- script callback timers;
- named event subscriptions;
- fixed-tick script sequences;
- interface-like script contracts;
- attribute-style host metadata;
- fixed-step scene driver;
- generated `RealScript.Game` host bindings.

## Phase 10

- versioned gameplay-state codec;
- defensive decode limits;
- canonical floating-point representation;
- state hash verification;
- replay/rollback restoration helpers.

## Validation

- C++17;
- GCC warnings-as-errors for deterministic primitives and state codec;
- isolated API-shape compilation for the Game SDK bridge;
- unit coverage for ordering, stale entity rejection, timer repeat behavior,
  event delivery, sequence timing, snapshot/hash stability, codec round trips,
  corruption rejection, and limit enforcement;
- full repository integration test added for `SceneGameplayDriver` and
  `ScriptContract` validation.

## Remaining compiler phases

The runtime is ready for later source-language work, but does not itself implement
interfaces, generics, lambdas/delegates, source attributes, `foreach`, `switch`,
or coroutine state-machine lowering.
