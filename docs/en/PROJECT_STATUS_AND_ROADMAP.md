# Project Status and Roadmap

[Documentation Home](README.md) | [Repository README](../../README.md)

The planned Phase 1–6 roadmap is complete. RealScript now has a coherent v0.1 alpha technical baseline covering the source language, verified compiler pipeline, bytecode runtime, managed memory, debugging, editor tooling, AOT, deterministic execution, optimization, profiling, benchmarking, and optional native JIT.

## Current Status

RealScript should currently be described as:

> A complete alpha reference implementation and integration baseline, not a frozen production 1.0 language or binary platform.

The implementation is suitable for:

- architectural evaluation;
- embedding experiments;
- game-tool prototypes;
- interpreter/AOT/JIT comparison;
- debugger and language-server integration;
- deterministic simulation experiments;
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

## Validation Baseline

The completed baseline has been exercised with:

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

## Unfrozen Compatibility Areas

The following dimensions remain draft:

- source language syntax and static semantics;
- MIR instruction set and verification rules;
- `.rsbc` physical format;
- runtime C/C++ ABI;
- object descriptor and metadata schema;
- debug-information schema;
- GC and embedding ownership contracts;
- cross-toolchain AOT module distribution.

Projects integrating the alpha baseline should pin a specific RealScript revision or SDK version.

## Deliberate Feature Limits

The current baseline does not include:

- inheritance and interfaces;
- virtual or abstract dispatch;
- generics;
- exceptions;
- coroutines or async tasks;
- `ref` and `out` parameters;
- complete structured loop and switch syntax;
- cross-toolchain stable binary distribution;
- direct machine-code JIT generation;
- speculative optimization, deoptimization, OSR, or PGO;
- fixed-tick standard library and rollback networking framework.

## Recommended Next Stage

The original roadmap should remain closed. The next work should be driven by integration into one real application rather than by adding another broad implementation phase.

Recommended integration sequence:

1. embed `EngineRuntime` in a real C++17 application;
2. expose a small, capability-limited host API;
3. load a representative multi-module script package;
4. exercise interpreter and AOT deployment;
5. connect LSP and DAP to an editor;
6. validate hot reload during development;
7. profile realistic workloads with `rsbench` and runtime counters;
8. test deterministic replay against real input streams;
9. collect API, ABI, performance, and usability findings;
10. freeze only the contracts proven by actual integration.

## Possible Future Work

Future RFCs may consider:

- a stable v0.2 language subset;
- inheritance, interfaces, and virtual dispatch;
- generics and monomorphization policy;
- exceptions and structured cleanup;
- richer standard-library containers;
- fixed-tick deterministic runtime services;
- rollback and state snapshot integration;
- persistent incremental build caches;
- advanced optimizer passes;
- LLVM ORC or direct machine-code JIT;
- cross-toolchain native distribution ABI;
- package and dependency management.

These items are not part of the completed Phase 1–6 commitment.

## Release Naming

Until compatibility contracts are frozen, releases should use alpha or preview naming such as `v0.1.0-alpha`. Release notes should identify the exact commit and clearly state same-SDK and compatibility boundaries.
