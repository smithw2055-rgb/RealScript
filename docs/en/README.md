# RealScript Documentation (English)

[Documentation Home](../README.md) | [中文文档](../zh-CN/README.md) | [Repository README](../../README.md) | [Changelog](../../CHANGELOG.md)

## Start Here

1. [Getting Started](GETTING_STARTED.md) — Build, test, CLI workflows (`rsc`, `rsaot`, `rsbind`, `rsdebug`, `rslsp`), and CMake slice consumption
2. [Architecture Overview](ARCHITECTURE.md) — L0/L1/L2 repository layering, acyclic physical slices, compiler-runtime boundary, and execution tiers
3. [Project Status and Roadmap](PROJECT_STATUS_AND_ROADMAP.md) — Verified capabilities in `v0.98.0+`, recent hardening (`AUD-01`–`AUD-05`), and post-baseline roadmap

## Language and Compatibility

- [Language and Type System](LANGUAGE_AND_TYPE_SYSTEM.md) — Modules, classes, structs, enums, interfaces (including default interface methods), delegates, generics, pattern matching, deterministic `sequence` coroutines, and `[StackOnly]` spans
- [C# Compatibility Matrix](CSHARP_COMPATIBILITY_MATRIX.md) — Supported C# constructs, RealScript extensions (`sequence`, `[StackOnly]`, `[Signal]`, `[RelayCommand]`, `[Blittable]`, `fixed`), and deliberate omissions

## Compiler, Runtime, and Performance

- [Compilation and Bytecode](COMPILATION_AND_BYTECODE.md) — Frontend, Typed MIR, O2 passes (SROA, LICM, small-function inlining), `ModuleBuildCache`, `.rsbc` (`0.6`–`0.9`), and 16-byte `Value` / 8-byte `Instruction` representation
- [Runtime, GC, and Embedding](RUNTIME_GC_AND_EMBEDDING.md) — `EngineRuntime`, `ManagedHeap` generational nursery & incremental GC, safepoints, `NativeHandleRegistry`, `ParallelJobScheduler`, and L0 embedding (`RealScript::Runtime`, `RealScript::SlimRuntime`)
- [AOT, JIT, and Performance](AOT_JIT_AND_PERFORMANCE.md) — Interpreter fast path, in-process W^X `StencilJit` (`x86_64` & `ARM64` native stencils + threaded fallback + `functionDiagnostics()`), C++17 `AotRuntime`, `rsaot --opt-remarks`, and native parallel batch kernels

## Tooling, Determinism, and Productization

- [Debugging, Tooling, and Hot Reload](DEBUGGING_TOOLING_AND_HOT_RELOAD.md) — `rslsp` incremental sync / `codeLens` / `inlayHint` / `semanticTokens`, `GovernanceLinter` (`RSG001`–`RSG005`), `rsc --format`/`--lint`, `rsdebug` / `DapServer` fiber & zero-alloc telemetry, `GovernanceFuzzer`, and structural hot reload
- [Determinism and Replay](DETERMINISM_AND_REPLAY.md) — `DeterminismMode::Strict` / `Record` / `Replay`, cross-backend digest parity, and lockstep simulation boundaries
- [Productization](PRODUCTIZATION.md) — Versioned contracts (`Version.h`), modular L0/L1 CMake exports, `-DREALSCRIPT_SLIM_RUNTIME=ON`, `ModuleBuildCache`, `.rscache`, and companion L2 `RealScript-GamePack` packaging

> **Game / Scene / Gameplay SDK Docs:** `GAME_SCRIPTING_SDK.md` and `GAMEPLAY_RUNTIME.md` now live in the companion **`RealScript-GamePack`** repository (`docs/en/`).
