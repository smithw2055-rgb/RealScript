# Project Status and Roadmap

[Documentation Home](README.md) | [Repository README](../../README.md) | [Phase 11–18 Profile](NATIVE_LANGUAGE_PHASE_11_18.md)

The Phase 1–24 compiler, runtime, Game SDK, deterministic gameplay, and native language/runtime roadmap is complete. Phase 18 replaced the former Phase 11–17 source-expansion profile with native compiler constructs; Phases 19–24 then completed the planned polymorphism, closure, generic/collection, coroutine, value/reference, convenience/pattern, and structured-error slices.

## Current Status

RealScript should currently be described as:

> A complete Phase 24 alpha reference implementation of a native, strongly typed, deterministic C#-style game language and engine-integration baseline—not a frozen production 1.0 language, CLR-compatible platform, or .NET standard library.

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

### Phase 18–24 — native language/runtime completion

- Phase 18 moved all former expansion features into native syntax, binding,
  Typed MIR, bytecode/AOT, source metadata, tooling, and hot reload.
- Phase 19 added visibility, single inheritance, base construction, and runtime
  virtual/interface values and dispatch.
- Phase 20 added first-class delegates, precise heap closures, shared captures,
  multicast, events, snapshots, and backend parity.
- Phase 21 added generic inference/members/constraints/interfaces/delegates,
  deterministic specialization, growable collections, and enumerators.
- Phase 22 added explicit snapshot-safe deterministic coroutine state machines
  with nested control flow, cancellation, child sequences, and results.
- Phase 23 added exact-width values, checked/unchecked conversion, mutable
  structs, ref locations, nullable values, and boxing/unboxing.
- Phase 24 added inference/null/type convenience, initializers, flexible
  arguments, patterns/switch expressions, and deterministic script exceptions.

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

## Historical Phase 11–17 Profile Boundaries

The following list describes the closed Phase 11–17 baseline only. Its language
gaps were addressed by native Phase 18 and the Phase 19–24 roadmap; it must not
be read as the current compatibility matrix.

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

Projects integrating the alpha baseline should pin a specific revision and use
the [current C#-style compatibility matrix](CSHARP_COMPATIBILITY_MATRIX.md).

## Recommended Next Stage

Phases 1–24 are closed. The next work should be driven by integration, hardening,
profiling, and explicit RFCs rather than assuming unimplemented CLR semantics:

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

- open runtime generics, variance, reflection, and `dynamic`;
- additional relational/property/list/recursive pattern families;
- ref-struct/Span-style escape analysis, unsafe pointers, and ref properties;
- catch filters, `using`, native exception interop, and exception-aware
  coroutine suspension;
- operators, user-defined conversions, and broader library APIs;
- rollback networking and replication protocols;
- direct machine-code JIT, OSR, PGO, and cross-toolchain distribution.

## Release Naming

Until compatibility contracts are frozen, releases should use alpha or preview naming such as `v0.1.0-alpha`. Release notes should identify the exact commit and explicitly state source, expansion-profile, bytecode, SDK, gameplay-state, and ABI boundaries.
