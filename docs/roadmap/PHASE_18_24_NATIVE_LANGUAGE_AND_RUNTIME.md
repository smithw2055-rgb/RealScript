# Phase 18–24 Native Language and Runtime Evolution

This roadmap replaces the Phase 11–17 source-expansion implementation with first-class compiler semantics, then completes the remaining language/runtime priorities in dependency order.

The branch for this work is `agent/native-language-and-runtime-evolution`. Tracking issue: #32.

## Definition of native language support

A feature is native only when all applicable layers understand it directly:

1. lexer/token model;
2. syntax tree and parser recovery;
3. declaration and name binding;
4. type checking and flow analysis;
5. Bound tree and Typed MIR;
6. MIR verification and optimization;
7. bytecode lowering, codec, disassembly, and verification;
8. interpreter execution;
9. C++17 AOT generation and execution;
10. optional JIT execution;
11. source/debug metadata, LSP, DAP, rename, and hot reload;
12. Game SDK metadata, serialization, replay, and rollback where applicable.

Text/token rewriting alone does not satisfy this definition.

## Migration rules

- Existing Phase 11–17 behavior remains available while a feature is migrated.
- A feature switches from expansion to native compilation only after differential tests cover both paths.
- The native implementation becomes the default before the old expansion path is removed.
- `LanguageExpansion` is deleted only after every Phase 11–17 feature has a native replacement and compatibility tests pass.
- Generated hidden identifiers must never appear in user diagnostics, debugger frames, symbols, rename results, or public metadata.
- All stable identities must be based on source declarations and canonical types, not generated source names.

## Phase 18 — native Phase 11–17 compiler features

### 18A — structured control flow

- Native syntax/Bound/MIR for `for`, `foreach`, `do/while`, `break`, `continue`, and `switch`.
- Explicit break/continue labels in Bound trees and MIR.
- Single evaluation of switch expressions.
- Enumerator protocol as a compiler-known contract, initially covering arrays and built-in collections.
- Precise flow analysis and unreachable-code diagnostics.

### 18B — delegates, lambdas, and events

- Native delegate/event declarations.
- Lambda syntax trees and closure analysis.
- Method-group conversion and deterministic event ordering.
- First migration may retain bounded captures, but the AST and symbols must be native.

### 18C — interface contracts and source attributes

- Native interface and attribute declarations/usages.
- Attribute binding and typed constant arguments.
- Interface metadata in semantic models, bytecode, AOT manifests, and Game SDK outputs.

### 18D — generics

- Native type-parameter/type-argument syntax and symbols.
- Generic construction and deterministic specialization after semantic binding.
- Stable generic identity independent of generated source names.

### 18E — deterministic sequences

- Native `sequence` and `yield wait_ticks` nodes.
- Typed coroutine state-machine MIR with source sequence points.
- Snapshot and rollback metadata owned by the compiled program.

### 18F — reference modifiers and exact aliases

- Native `ref`, `out`, and `in` parameter/argument symbols.
- Definite assignment and l-value checking.
- Remove generated wrapper classes from source-level metadata.
- Exact-width value types are completed in Phase 23.

### Phase 18 exit criteria

- `LanguageExpansionOptions` is no longer required for Phase 11–17 source.
- `LanguageExpansion.cpp` and generated support declarations can be removed.
- Interpreter/AOT/JIT result, digest, and profile differential tests pass.
- LSP/DAP/hot reload operate on original source constructs.

## Phase 19 — runtime polymorphism

- `public`, `internal`, `protected`, and `private` visibility.
- Interface-typed values, conversions, and deterministic dispatch tables.
- Single class inheritance, `base`, `virtual`, `override`, `abstract`, and `sealed`.
- Stable inherited field/method layouts and verifier checks.
- Save-state and hot-reload compatibility rules for hierarchy changes.

## Phase 20 — first-class delegates and closures

- Delegate runtime values and method references.
- Heap closure objects with precise GC descriptors.
- Local capture by value/reference according to language rules.
- Delegate parameters, returns, combination, removal, and general event storage.
- Snapshot, replay, and hot-reload policies for closures/subscriptions.

## Phase 21 — complete generics and collections

- Generic inference, generic member methods, constraints, and generic interfaces/delegates.
- Deterministic specialization cache shared across modules and backends.
- Growable `List`, `Dictionary`, `HashSet`, `Queue`, and `Stack`.
- Native enumerator protocol and `foreach` lowering.
- Deterministic allocation/capacity policies and failure behavior.

## Phase 22 — complete deterministic coroutine state machines

- Persist locals, temporaries, and control-flow position across yields.
- Loops, branches, nested sequences, `yield break`, cancellation, and return results.
- Snapshot/restore/rollback and hot-reload state migration.
- Deterministic gameplay profiles do not use thread-based `Task` execution.

## Phase 23 — complete value and reference semantics

- Exact `byte`, `sbyte`, `short`, `ushort`, `uint`, `ulong`, `float`, and `char` identities.
- Checked/unchecked operations and conversions across all backends.
- Mutable struct receivers, `ref this`, ref locals/returns/fields/indexers, and `in` rules.
- Nullable value types and boxing/unboxing.
- GC, verifier, native binding, AOT ABI, and bytecode support.

## Phase 24 — language completeness and structured errors

- `var`, conditional/null operators, `is`, `as`, `typeof`, object/collection initializers.
- Optional, named, and `params` arguments.
- Pattern matching and switch expressions.
- Exceptions, `throw`, `try/catch/finally`, and deterministic cleanup semantics.
- Complete LSP/DAP/hot-reload behavior and a published C#-feature compatibility matrix.

## Validation required for every merge

- Parser recovery and malformed-input tests.
- Binder/type/flow diagnostics.
- Typed MIR verification and optimization tests.
- Bytecode codec/verifier/disassembly tests.
- Interpreter execution tests.
- Generated C++17 compilation and AOT execution tests.
- JIT execution where supported.
- Interpreter/AOT/JIT result and deterministic-digest comparison.
- Ubuntu and Windows warnings-as-errors CI.
- Game SDK and serialization/replay tests when the feature affects runtime state.
- English and Chinese documentation updates.
