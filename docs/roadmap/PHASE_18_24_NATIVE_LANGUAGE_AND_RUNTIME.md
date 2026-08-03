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

### 18A — structured control flow — complete

Implemented and validated:

- native syntax, Bound nodes, and MIR for `for`, `foreach`, `do/while`, `break`, `continue`, and `switch`;
- explicit break/continue target stacks in MIR lowering;
- single evaluation of switch expressions through a compiler-owned typed local;
- native array and indexed-collection `foreach`;
- precise definite-assignment and all-path-return behavior;
- stable original-source debug metadata for compiler-owned locals;
- constant infinite loops without synthetic reachable exits;
- malformed loop-control diagnostics;
- `LanguageExpansionOptions::structuredControlFlow` disabled by default;
- all Phase 1–18A tests passing on Ubuntu and Windows with warnings as errors.

### 18B — delegates, lambdas, and events — pending

- Native delegate/event declarations.
- Lambda syntax trees and closure analysis.
- Method-group conversion and deterministic event ordering.
- First migration may retain bounded captures, but the AST and symbols must be native.

### 18C — interface contracts and source attributes — complete

Implemented and validated:

- native `interface` declarations and class/struct implementation lists;
- module/import-aware interface visibility;
- exact method name, visible arity, parameter type, return type, and exact-type identity validation;
- stable module-qualified interface implementation metadata in `BuildResult` and the Game SDK;
- native Attribute List, Attribute, and Attribute Argument syntax nodes with original token spans;
- declaration attributes on types, interfaces, functions, fields, methods, constructors, properties, enum members, and interface methods;
- positional and named argument text retained from original source;
- canonical metadata targets and source locations;
- native attributes retained in `Compilation`, `GameCompileResult`, and `GameProgram`;
- attributes and interface contracts included in public module fingerprints;
- generic source expansion preserves native attribute token lists while generic specialization remains pending;
- `LanguageExpansionOptions::interfaces` and `sourceAttributes` disabled by default;
- Phase 1–18C tests passing on Ubuntu and Windows with warnings as errors.

### 18D — generics — complete for explicit bounded specialization

Implemented and validated:

- native type-parameter and nested type-argument syntax;
- native explicit generic type/function call syntax;
- compiler-owned specialization units without rewriting user source;
- deterministic concrete names and stable specialization metadata;
- user generic classes, structs, and free functions;
- fixed-capacity `List`, `Queue`, `Stack`, `Optional`, `HashSet`, and `Dictionary` profiles;
- cross-file/module explicit specialization and AOT/JIT reuse through normal MIR;
- `LanguageExpansionOptions::generics` disabled by default.

Inference, constraints, generic member methods, generic interfaces/delegates, and complete collection implementations remain Phase 21 work.

### 18E — deterministic sequences — complete

Implemented and validated:

- native `sequence` declaration and `yield wait_ticks(expression)` syntax nodes;
- original source spans retained for sequence declarations and yield points;
- compiler-owned entry method, callback methods, and target-handle field symbols;
- each top-level yield segment is bound independently, so locals cannot silently cross suspension boundaries;
- generated schedule calls bind directly to imported `RealScript.Game.Schedule` rather than generated script text;
- fixed-tick callbacks continue to use `GameplayHost` and `TickScheduler` for replay/rollback behavior;
- canonical sequence metadata is retained in `BuildResult`, `GameCompileResult`, and `GameProgram`;
- invalid sequence signatures, missing scheduling APIs, nested/out-of-context yields, and non-class owners receive explicit diagnostics;
- `LanguageExpansionOptions::deterministicCoroutines` disabled by default;
- native sequence execution, metadata, error recovery, bytecode verification, and the full repository test matrix pass on Ubuntu and Windows with warnings as errors.

Current bounded semantics remain unchanged: exactly one `long target` parameter, top-level `yield wait_ticks`, and durable state stored in object fields. Complete local-persisting coroutine state machines remain Phase 22.

### 18F — reference modifiers and value aliases — complete

Implemented and validated in the native reference slice:

- native `ref`, `out`, and `in` parameter and argument tokens;
- source-level function signatures retain modifiers and underlying types;
- compiler-owned synthetic reference boxes use existing object/field/GC/bytecode support;
- Binder reads and writes `ref`/`out` parameters through the internal `Value` field;
- call sites perform typed copy-in/copy-out without generated source text;
- forwarding of compatible `ref`/`out` parameters;
- `out` arguments become definitely assigned after a successful call;
- assignment to `in` parameters reports `RS8702`;
- `LanguageExpansionOptions::referenceParameters` disabled by default.

The bounded Phase 18F profile is complete:

- `byte`, `sbyte`, `short`, `ushort`, and `char` resolve natively to the checked `int` carrier;
- `uint` and `ulong` resolve natively to the checked `long` carrier;
- `float` resolves natively to the `double` carrier;
- `LanguageExpansionOptions::valueTypeAliases` is disabled by default;
- aliases preserve original source spans and no longer generate source text.

Reference member/indexer l-values, exact-width identities, checked/unchecked conversions, nullable values, and boxing remain Phase 23 work.

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