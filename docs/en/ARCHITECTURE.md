# Architecture

[Documentation Home](README.md) | [Getting Started](GETTING_STARTED.md)

RealScript is organized around one verified semantic pipeline shared by the interpreter, C++17 AOT backend, and optional native JIT. The architecture avoids backend-specific language behavior and keeps game-engine embedding explicit.

## Design Goals

- C#-inspired syntax without requiring a managed host runtime.
- C++17 as the host and implementation baseline.
- Predictable runtime cost and explicit resource budgets.
- Strong static typing and exact runtime type identities.
- A verifiable intermediate representation shared by all execution backends.
- Precise managed memory without scanning native stacks conservatively.
- Source-level debugging, editor tooling, and safe body-only hot reload.
- Deterministic execution support suitable for replay and simulation workflows.
- Optional native execution without making an external compiler a core dependency.

## End-to-End Pipeline

```text
Source files
    |
    v
SourceText / Lexer / Parser / Syntax Trees
    |
    v
Compilation / Module Graph / Symbol Predeclaration
    |
    v
Binding / Overload Resolution / Flow Analysis
    |
    v
Verified multi-block Typed MIR
    |
    +----> O0/O1/O2 Optimizer ----> MIR Verifier
    |
    +----> Typed register bytecode ----> Bytecode Verifier ----> Interpreter
    |
    +----> Deterministic C++17 source --> Platform compiler ----> AOT module
    |
    +----> Toolchain JIT ----------------> Shared library ------> Native module

Shared runtime services, type metadata, GC, bindings, debug information,
determinism, profiling, and error semantics
```

The source AST is not interpreted directly by individual backends. All executable paths consume verified MIR or artifacts derived from it.

## Major Layers

### Text and Syntax

`SourceText` owns source content and line mapping. The lexer produces stable tokens and diagnostics. The parser creates syntax trees using Pratt expression parsing and explicit statement productions.

Line mapping handles LF and CRLF inputs. Source spans are preserved through semantic binding, MIR lowering, bytecode debug metadata, and generated source maps.

### Semantic Compilation

The compilation layer:

- groups files by declared module;
- resolves imports through the module graph;
- predeclares functions and types before binding bodies;
- assigns stable `SymbolId` and `TypeId` values;
- performs overload resolution and conversion ranking;
- runs definite-assignment and all-path-return analysis;
- produces bound semantic bodies and Typed MIR;
- reuses previous module results through `BuildSnapshot` fingerprints.

Stable identities do not depend on source order, memory addresses, or a particular process execution.

### Typed MIR

Typed MIR is the central executable model. It contains:

- explicit basic blocks;
- typed SSA-like values;
- explicit local slots;
- block parameters for control-flow edges;
- exact runtime type IDs where primitive tags are insufficient;
- explicit null, bounds, arithmetic, allocation, field, array, and call operations;
- source spans and debug sequence points;
- terminators for return, branch, and conditional branch behavior.

The MIR verifier checks definition dominance, operand types, block arguments, control-flow targets, calls, returns, exact type identities, and operation-specific invariants.

### Bytecode

The bytecode backend maps MIR values to typed registers and MIR locals to explicit local slots. Current `.rsbc` 0.9 uses deterministic fixed-width encoding and separate sections for strings, types, references, functions, code, debug information, language metadata, and the object model. Exception regions are encoded with function bodies.

A module is executable only after defensive decoding and semantic verification both succeed.

### Runtime

`ProgramImage` links verified modules into a reusable immutable execution image. `EngineRuntime` owns or references shared runtime services such as:

- `BindingRegistry`;
- managed heap;
- native handle registry;
- trace callbacks;
- profile collection;
- deterministic execution state;
- hot-reload image publication.

The interpreter uses explicit frames and typed values. Errors are returned as structured results instead of relying on undefined C++ behavior.

### Managed Memory

The heap is non-moving and uses generation-checked `ObjectRef` handles. Precise roots come from interpreter or AOT shadow stacks and persistent host roots. Arrays and records use write barriers during incremental mark/sweep collection.

Heap identity prevents references from crossing unrelated runtimes accidentally.

### Native Execution

The AOT generator emits deterministic C++17 and a native module descriptor. The optional toolchain JIT reuses the same generator, compiles a shared library, loads it, negotiates the module query ABI, validates descriptors, and then invokes native entries.

AOT and JIT are same-SDK paths. Cross-toolchain binary distribution is not frozen.

### Debugging and Tooling

Debug information is a first-class part of `.rsbc` 0.9 and includes source files, ranges, sequence points, locals, parameters, and lexical scopes.

The DAP adapter and LSP server reuse compiler and runtime services rather than maintaining separate language models.

### Determinism and Profiling

Execution events feed a stable digest and per-function profile counters. Host bindings declare whether they are deterministic, recordable, or non-deterministic. Record and Replay modes serialize the observable results of approved external calls.

## Shared Semantic Invariants

Every backend must preserve:

- left-to-right evaluation order;
- checked integer overflow behavior;
- null and bounds checks;
- exact type validation;
- host-call order;
- allocation and write-barrier semantics;
- instruction and recursion budgets;
- structured error codes and script stacks;
- deterministic event ordering when enabled.

Differential tests compare interpreter, AOT, and JIT results, errors, digests, and profiles.

## Version Dimensions

RealScript separates:

- source language version;
- MIR version;
- bytecode format version;
- runtime ABI version;
- metadata schema version;
- debug-information version.

Changing one dimension does not automatically require changing every other dimension.

## Current Architectural Boundaries

The v0.2.0 Phase 24 implementation includes inheritance, runtime
interfaces/virtual dispatch, compile-time generics, deterministic coroutine
state machines, and script exceptions, but their source/artifact/ABI contracts
are not frozen. Cross-toolchain AOT distribution and an in-process direct
machine-code JIT remain future design areas. See the
[compatibility matrix](CSHARP_COMPATIBILITY_MATRIX.md) for language boundaries.
