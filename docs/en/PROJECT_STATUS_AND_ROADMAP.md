# Project Status and Roadmap

[Documentation Home](README.md) | [Repository README](../../README.md)

The Phase 1–6 compiler and execution roadmap, Phase 7 Game Scripting SDK, and Phase 8–10 deterministic gameplay runtime are complete. RealScript now has a coherent v0.1 alpha integration baseline covering the source language, verified compiler pipeline, bytecode runtime, managed memory, debugging, editor tooling, AOT, deterministic execution, typed game-engine embedding, fixed-tick gameplay services, and versioned gameplay-state restoration.

## Current Status

RealScript should currently be described as:

> A complete alpha reference implementation and game-engine integration baseline, not a frozen production 1.0 language or binary platform.

The implementation is suitable for:

- architectural evaluation;
- embedding in C++17 game and simulation engines;
- gameplay, AI, ability, quest, and scene-script prototypes;
- interpreter/AOT/JIT comparison;
- debugger and language-server integration;
- deterministic fixed-tick simulation;
- save, replay, and rollback-state experiments;
- validation through a real host application.

## Completed Phases

### Phase 1 — Language Frontend

- C++17/CMake foundation
- text and diagnostics
- lexer and parser
- semantic binding
- control flow and flow analysis
- multi-block Typed MIR
- functions, overloads, modules, imports, and incremental snapshots

### Phase 2 — Bytecode Runtime

- typed register bytecode
- deterministic codec and disassembler
- defensive decoder and verifier
- interpreter, calls, block arguments, and runtime errors
- linking, native bindings, tracing, statistics, and embedding facade

### Phase 3 — Object and Memory Model

- non-moving managed heap
- generation-checked object references
- precise roots and incremental mark/sweep
- classes, fields, arrays, and exact type descriptors
- native handles and cross-heap ownership checks
- methods, constructors, and properties
- checked `long`, binary64 `double`, enums, and copy-semantic structs
- heap snapshots, retaining paths, and leak summaries

### Phase 4 — Debugging and Tooling

- `.rsbc` source and debug metadata
- sequence points, locals, parameters, and lexical scopes
- DAP debug adapter
- LSP language server
- breakpoints, stepping, stack frames, scopes, and variables
- diagnostics, completion, navigation, references, rename, and symbols
- safe function-body hot reload

### Phase 5 — C++17 AOT

- deterministic C++17 source generation
- AOT support runtime
- source maps and deterministic manifest
- C11/C++ native module query ABI
- typed native entries and descriptor validation
- reusable CMake AOT integration
- interpreter/AOT differential tests

### Phase 6 — Determinism, Optimization, and JIT

- Strict, Record, and Replay execution
- host-binding determinism policies
- stable execution digests
- O0/O1/O2 Typed MIR optimization
- per-function profiles
- `rsbench`
- external C++ toolchain JIT
- shared-library loading and content-addressed cache
- interpreter/AOT/JIT differential validation

### Phase 7 — Game Scripting SDK and Productization

- compiler-visible typed C++ host APIs
- native functions, instance methods, and properties
- generation-checked native object wrappers
- rooted script-object creation and invocation
- scene lifecycle callbacks
- direct, queued, and broadcast events
- engine-owned trigger conditions
- bytecode package loading and object-metadata restoration
- installable SDK targets and script-object state serialization

### Phase 8 — Deterministic Gameplay Primitives

- generation-checked gameplay `EntityId`
- fixed-tick clock with catch-up limits
- independent PCG random streams
- deterministic timer scheduler
- globally unique deterministic event identities
- typed event arguments
- snapshot, restore, and stable hashes

### Phase 9 — Gameplay Scripting Runtime

- script callback timers
- named event subscriptions
- fixed-tick script sequences
- interface-like `ScriptContract` validation
- host-owned deterministic metadata
- fixed-step `SceneGameplayDriver`
- generated `RealScript.Game` host bindings

### Phase 10 — Save, Replay, and Rollback State

- versioned `RSGS` gameplay-state container
- fixed-width little-endian encoding
- canonical floating-point representation
- defensive object, string, count, and byte limits
- stable state-hash verification
- gameplay-host snapshot and restoration helpers
- composition boundary with `ScriptObjectState` for engine-level rollback frames

## Validation Baseline

The compiler/runtime baseline has been exercised with:

- GCC 14.2 Debug with warnings as errors;
- Clang 17 Debug with warnings as errors;
- GCC Release optimization with warnings as errors;
- AddressSanitizer and UndefinedBehaviorSanitizer;
- Ubuntu GitHub Actions;
- Windows Server 2025 / Visual Studio 2026 GitHub Actions;
- generated C++ compilation;
- C11 ABI-header compilation;
- dynamic shared-library loading;
- native ABI query and descriptor validation;
- JIT cache reuse;
- interpreter/AOT/JIT differential tests.

The repository test matrix now also includes Phase 7 Game SDK/productization tests and Phase 8–10 coverage for deterministic ordering, stale entity rejection, globally unique event identities, timer repetition, event delivery, sequence timing, contract validation, snapshot/hash stability, codec round trips, corruption rejection, and decode limits.

## Unfrozen Compatibility Areas

The following dimensions remain draft:

- source-language syntax and static semantics;
- MIR instruction set and verification rules;
- `.rsbc` physical format;
- runtime C/C++ ABI;
- object descriptor and metadata schema;
- debug-information schema;
- GC and embedding ownership contracts;
- Game SDK source and binary contracts;
- `RSGS` gameplay-state format;
- cross-toolchain AOT module distribution.

Projects integrating the alpha baseline should pin a specific RealScript revision or SDK version.

## Deliberate Feature Limits

The current baseline does not include:

- inheritance and source-language interfaces;
- virtual or abstract dispatch;
- generics and generic collections;
- exceptions and structured cleanup;
- source-language `yield`, coroutines, or async tasks;
- `ref` and `out` parameters;
- complete `for`, `foreach`, `switch`, and pattern-matching syntax;
- source attribute syntax;
- cross-toolchain stable binary distribution;
- direct machine-code JIT generation;
- speculative optimization, deoptimization, OSR, or PGO;
- a complete rollback networking, replication, or lockstep transport framework.

The host-level contracts, metadata, sequences, fixed-tick services, and state codec are intentional runtime foundations for later language syntax and engine networking; they are not substitutes for claiming those features are already implemented.

## Recommended Next Stage

The broad technical roadmap should now remain closed. The next work should be driven by integration into one real game engine rather than by adding another general-purpose compiler phase.

Recommended integration sequence:

1. embed `GameApi`, `GameScriptCompiler`, and `SceneScriptRuntime` in a real C++17 engine;
2. install a capability-limited gameplay API;
3. drive scripts through `GameplayHost` and `SceneGameplayDriver`;
4. combine `GameplayHost::State` and `ScriptObjectState` into an engine rollback frame;
5. validate replay and rollback against real deterministic input streams;
6. exercise interpreter and AOT deployment;
7. connect LSP and DAP to the editor;
8. validate hot reload during development;
9. profile realistic workloads with `rsbench` and runtime counters;
10. freeze only the contracts proven by actual integration.

## Possible Future Work

Future RFCs may consider:

- a stable v0.2 language subset;
- inheritance, interfaces, and virtual dispatch;
- generics and monomorphization policy;
- exceptions and structured cleanup;
- delegates, lambdas, and language-level events;
- source attributes and editor metadata generation;
- `for`, `foreach`, `switch`, and pattern matching;
- coroutine state-machine lowering;
- deterministic generic collections;
- rollback networking and replication integration;
- persistent incremental build caches;
- advanced optimizer passes;
- LLVM ORC or direct machine-code JIT;
- cross-toolchain native distribution ABI;
- package and dependency management.

These items are not part of the completed Phase 1–10 integration baseline.

## Release Naming

Until compatibility contracts are frozen, releases should use alpha or preview naming such as `v0.1.0-alpha`. Release notes should identify the exact commit and clearly state source, bytecode, SDK, gameplay-state, and ABI compatibility boundaries.
