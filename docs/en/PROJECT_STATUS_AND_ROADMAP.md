# Project Status and Roadmap

[Documentation Home](README.md) | [Repository README](../../README.md) | [Phase 11–17 Profile](LANGUAGE_EXPANSION_PHASE_11_17.md)

The Phase 1–10 compiler, Game SDK, and deterministic gameplay-runtime roadmap is complete. Phase 11–17 adds a bounded deterministic game-language profile that is integrated with multi-file/module compilation, Typed MIR, bytecode, the interpreter, C++17 AOT generation, and Game SDK metadata.

## Current Status

RealScript should currently be described as:

> A complete alpha reference implementation and game-engine integration baseline with a bounded C#-style gameplay-language profile, not a frozen production 1.0 language or CLR-compatible platform.

The implementation is suitable for:

- embedding in C++17 game and simulation engines;
- gameplay, AI, ability, quest, and scene-script prototypes;
- deterministic fixed-tick simulation;
- save, replay, and rollback-state experiments;
- interpreter/AOT/JIT comparison;
- debugger and language-server integration;
- editor metadata and Game SDK integration;
- validation through a real host application.

## Completed Phases

### Phase 1–6 — compiler and execution baseline

- lexer, parser, binder, flow analysis, and multi-file modules;
- verified multi-block Typed MIR and O0/O1/O2 optimization;
- deterministic register bytecode, verifier, interpreter, and embedding;
- managed heap, precise roots, classes, arrays, members, enums, and structs;
- DAP, LSP, debug metadata, and body-only hot reload;
- deterministic C++17 AOT, native module ABI, profiling, record/replay, benchmark tooling, and optional external-toolchain JIT.

### Phase 7 — Game Scripting SDK and productization

- compiler-visible typed C++ host APIs;
- native functions, methods, properties, and generation-checked wrappers;
- rooted script objects and scene lifecycle callbacks;
- direct/queued/broadcast events and engine-owned triggers;
- bytecode package loading, installable SDK targets, and script-object state serialization.

### Phase 8–10 — deterministic gameplay runtime

- generation-checked gameplay entities;
- fixed ticks, PCG random streams, deterministic timers/events, snapshots, and stable hashes;
- gameplay contracts, host metadata, fixed-step scene driving, and generated gameplay bindings;
- versioned `RSGS` state encoding for save, replay, and rollback composition.

### Phase 11 — structured gameplay control flow

- `for`, array/fixed-collection `foreach`, and `do/while`;
- `break`, `continue`, and equality-based `switch/case/default`;
- deterministic nested loop/switch lowering and diagnostics.

### Phase 12 — delegates, lambdas, and events

- delegate declarations as event signatures;
- class-local deterministic events;
- method groups, parenthesized lambdas, and single-parameter lambdas;
- stable generated handler methods and subscription order.

### Phase 13 — interface contracts

- interface declarations and implementation lists;
- compile-time method-name/arity validation;
- implementation metadata for tools and the Game SDK.

### Phase 14 — source attributes

- positional and named attribute arguments;
- module-qualified targets;
- metadata retained by `Compilation`, `GameCompileResult`, and `GameProgram`.

### Phase 15 — explicit generics and collections

- deterministic explicit monomorphization;
- same-module cross-file sharing and imported-module visibility;
- module isolation for same-name declarations;
- fixed-capacity `List`, `Queue`, `Stack`, `Optional`, `HashSet`, and `Dictionary` profiles.

### Phase 16 — deterministic sequences

- `sequence` methods;
- `yield wait_ticks(...)` lowering to fixed-tick callbacks;
- integration with `GameplayHost`, snapshots, replay, and rollback scheduling.

### Phase 17 — reference/value profile

- restricted standalone `ref`, `out`, and `in` calls;
- declaring-module-owned exact wrapper types and deterministic writeback;
- current-core aliases for smaller/unsigned numeric names, `float`, and `char`.

## Validation Baseline

The repository matrix covers:

- GCC/Clang/MSVC C++17 builds with warnings as errors;
- Ubuntu and Windows GitHub Actions;
- bytecode verification and interpreter execution;
- generated C++17 AOT source and native AOT tests;
- C11 native-module ABI validation;
- optional toolchain JIT and interpreter/AOT/JIT differential tests;
- Phase 7–10 Game SDK/gameplay-state integration;
- Phase 11–17 single-file, cross-file, and cross-module execution;
- module import/isolation, nested switch/loop behavior, fixed-collection enumeration, event lambdas, reference writeback, deterministic sequences, AOT generation, and Game SDK metadata retention.

## Explicit Profile Boundaries

Phase 11–17 is a bounded game-language profile. It does not implement full CLR/C# semantics:

- interfaces are compile-time contracts, not runtime interface values or virtual dispatch;
- delegates are not general first-class runtime objects;
- lambdas do not capture arbitrary locals into heap closure objects;
- generics use explicit compile-time specialization without inference, open runtime generics, constraints, or variance;
- collections are fixed-capacity and deterministic;
- sequences support `yield wait_ticks`; durable cross-yield state belongs in object fields;
- `ref/out/in` is restricted to standalone calls; no ref locals/returns/fields/indexers;
- `byte`/`uint`/`float`/`char` names map to current canonical carriers rather than distinct runtime ABI identities;
- attributes are available to source/Game SDK metadata but are not yet serialized into `.rsbc`;
- expanded generated code does not yet have a complete exact source-map remapping layer.

## Unfrozen Compatibility Areas

The following remain draft:

- source language and language-expansion syntax;
- MIR instruction set and verification rules;
- `.rsbc` and `RSGS` physical formats;
- object, Game SDK, metadata, and native module ABIs;
- GC and embedding ownership contracts;
- cross-toolchain AOT module distribution.

Projects integrating the alpha baseline should pin a specific revision and explicitly select the implemented gameplay-language profile.

## Recommended Next Stage

The broad Phase 1–17 roadmap should now remain closed. The next work should be driven by integration into a real game engine:

1. embed `GameApi`, `GameScriptCompiler`, `SceneScriptRuntime`, and `GameplayHost`;
2. expose a capability-limited host API;
3. build representative gameplay, AI, ability, quest, and mod scripts;
4. combine gameplay and script-object state into engine rollback frames;
5. validate interpreter and native AOT deployment;
6. connect LSP/DAP and metadata to the editor;
7. profile realistic workloads;
8. freeze only contracts proven by integration.

## Possible Future RFCs

Future work may consider:

- class inheritance and runtime interface/virtual dispatch;
- first-class delegates and complete closure objects;
- inferred/open generics, constraints, variance, and growable collections;
- pattern matching and general enumerator protocols;
- native coroutine state machines with persisted locals;
- exact-width unsigned, binary32 `float`, and Unicode `char` identities;
- complete reference lifetime semantics, exceptions, nullable values, and boxing;
- precise source maps for generated expansion code;
- `.rsbc` attribute metadata;
- rollback networking and replication protocols;
- direct machine-code JIT, OSR, PGO, and cross-toolchain distribution.

## Release Naming

Until compatibility contracts are frozen, releases should use alpha or preview naming such as `v0.1.0-alpha`. Release notes should identify the exact commit and explicitly state source, expansion-profile, bytecode, SDK, gameplay-state, and ABI boundaries.
