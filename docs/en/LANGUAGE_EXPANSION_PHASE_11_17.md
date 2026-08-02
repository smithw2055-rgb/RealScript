# Phase 11–18 Language Profile

[Documentation Home](README.md) | [Language and Type System](LANGUAGE_AND_TYPE_SYSTEM.md) | [Gameplay Runtime](GAMEPLAY_RUNTIME.md)

RealScript originally introduced the Phase 11–17 C#-style gameplay profile through a deterministic source-expansion layer. Phase 18 is migrating those features into the native lexer, syntax tree, Binder, flow analysis, Typed MIR, bytecode, tooling metadata, AOT/JIT pipeline, and Game SDK.

This remains an embedded deterministic game-language profile, not CLR compatibility.

## Current compilation model

A `Compilation` still runs the compatibility expansion stage for features not yet migrated. Native features bypass that stage and preserve their original syntax nodes and source spans.

Native as of Phase 18:

- `for`, `foreach`, `do/while`, `break`, `continue`, and `switch`;
- interface declarations and class/struct interface contract lists;
- source Attribute Lists and positional/named Attribute Arguments;
- deterministic `sequence` declarations and top-level `yield wait_ticks` suspension points.

Still using compatibility expansion during Phase 18 migration:

- bounded delegates, lambdas, and class-local events;
- generic declarations and explicit specializations;
- restricted `ref`, `out`, and `in` calls;
- value aliases that do not yet have exact runtime identities.

Declarations remain isolated by `module`; directly imported modules contribute visible interface and remaining expansion declarations.

## Native structured control flow

Implemented directly in the compiler:

- `for`;
- `foreach` over arrays and indexed fixed-capacity collection profiles;
- `do` / `while`;
- loop `break` and `continue`;
- equality-based `switch`, `case`, and `default`;
- nested loop/switch target semantics;
- malformed loop-control diagnostics;
- stable hidden-local debug metadata for foreach collections, indices, and switch discriminants.

Switch cases do not fall through. Pattern matching, guards, and arbitrary enumerator protocols remain future work.

## Bounded delegates, lambdas, and events

Currently available through the compatibility expansion path:

- delegate declarations used as event signatures;
- class-local events;
- deterministic method-group subscription/removal;
- parenthesized and single-parameter lambdas;
- field/`this` capture without arbitrary local closure objects.

Native AST/Bound/MIR migration remains Phase 18 work. First-class delegate runtime values and heap closure objects belong to Phase 20.

## Native interface contracts

Implemented directly in Parser and Compilation:

- interface declarations;
- class/struct implementation lists;
- module/import-aware visibility;
- exact method-name, arity, parameter-type, return-type, and exact-type validation;
- stable canonical implementation metadata in `BuildResult` and Game SDK outputs.

Interface-typed values and runtime interface dispatch are not part of Phase 18; they belong to Phase 19.

## Native source attributes

Implemented directly in Parser and Compilation:

- Attribute Lists on types, interfaces, functions, fields, methods, constructors, properties, enum members, interface methods, and sequences;
- positional and named argument token spans;
- module-qualified canonical targets;
- source file and offset retention;
- public module fingerprint participation;
- metadata retained through `Compilation`, `GameCompileResult`, and `GameProgram`.

Attributes remain metadata records rather than executable Attribute classes. `.rsbc` serialization of this metadata is part of Phase 18 closure.

## Explicit generics and fixed-capacity collections

Currently available through compatibility specialization:

- explicit generic type and function instantiation;
- deterministic monomorphization;
- cross-file/module isolation;
- `List<T>`, `Queue<T>`, `Stack<T>`, `HashSet<T>`, `Dictionary<K,V>`, and `Optional<T>` fixed-capacity profiles.

Native type-parameter/type-argument syntax and semantic specialization remain Phase 18 work.

## Native deterministic sequences

Implemented directly in Parser, Compilation, and Binder:

```csharp
sequence Attack(long target)
{
    PlayWindup();
    yield wait_ticks(12);
    SpawnProjectile();
}
```

The compiler creates an entry method, fixed-tick callback methods, and a typed target-handle field as compiler-owned semantic symbols. Each segment between top-level yields is bound independently. Schedule calls bind directly to imported `RealScript.Game.Schedule`; no script source is generated.

Native sequence metadata is retained through `BuildResult`, `GameCompileResult`, and `GameProgram`. Callbacks continue to use `GameplayHost` and `TickScheduler`, preserving fixed-tick ordering and existing replay/rollback behavior.

Current bounded limits remain:

- exactly one `long target` parameter;
- only top-level `yield wait_ticks(expression)`;
- durable state must live in object fields;
- sequence owners must be classes;
- no `Task`, threads, arbitrary iterator values, or automatic persistence of locals.

Complete local-persisting coroutine state machines, cancellation, nesting, and results belong to Phase 22.

## Restricted reference parameters and aliases

Currently available through compatibility lowering:

- standalone `ref`, `out`, and `in` calls;
- copy-in/copy-out wrappers;
- `out` initialization and `in` write diagnostics;
- source aliases for small/unsigned integers, `float`, and `char` mapped onto current carriers.

Native parameter/argument modifiers remain Phase 18 work. Exact value identities and complete reference semantics belong to Phase 23.

## Backend validation

The native Phase 18 slices use the existing shared backend pipeline. Current coverage includes:

- parser recovery and diagnostics;
- Binder and definite-assignment analysis;
- Typed MIR and bytecode verification;
- interpreter execution;
- fixed-tick Game SDK sequence execution;
- existing AOT/JIT integration through shared MIR;
- Game SDK metadata retention;
- Ubuntu and Windows warnings-as-errors CI.

## Remaining Phase 18 work

- native delegates, lambdas, and events;
- native generic declarations, type arguments, and semantic specialization;
- native `ref`, `out`, and `in` symbols and MIR semantics;
- exact source/tooling support for all migrated nodes;
- `.rsbc` and AOT-manifest serialization of native language metadata;
- removal of the compatibility `LanguageExpansion` implementation after differential closure.

The migration remains explicit so game code depends only on verified implemented behavior.