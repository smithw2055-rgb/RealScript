# RealScript English Documentation

[Repository README](../../README.md) | [简体中文文档](../zh-CN/README.md)

This library describes the RealScript v0.2.0 implementation through Phase 24.
It is organized by subsystem and user workflow.

## New Users

1. [Getting Started](GETTING_STARTED.md)
2. [Architecture](ARCHITECTURE.md)
3. [Language and Type System](LANGUAGE_AND_TYPE_SYSTEM.md)
4. [C#-Style Compatibility Matrix](CSHARP_COMPATIBILITY_MATRIX.md)
5. [Phase 11–18 Native Language Profile](NATIVE_LANGUAGE_PHASE_11_18.md)
6. [Game Scripting SDK](GAME_SCRIPTING_SDK.md)
7. [Deterministic Gameplay Runtime](GAMEPLAY_RUNTIME.md)
8. [SDK Productization](PRODUCTIZATION.md)
9. [Project Status and Roadmap](PROJECT_STATUS_AND_ROADMAP.md)

## Compiler and Execution Pipeline

- [Compilation, MIR, and Bytecode](COMPILATION_AND_BYTECODE.md)
- [Runtime, GC, and Embedding](RUNTIME_GC_AND_EMBEDDING.md)
- [Phase 11–18 Native Language Profile](NATIVE_LANGUAGE_PHASE_11_18.md)
- [Game Scripting SDK](GAME_SCRIPTING_SDK.md)
- [Deterministic Gameplay Runtime](GAMEPLAY_RUNTIME.md)
- [SDK Productization](PRODUCTIZATION.md)
- [AOT, JIT, and Performance](AOT_JIT_AND_PERFORMANCE.md)
- [Detailed Performance Baseline (Chinese)](../zh-CN/PERFORMANCE_BASELINE_2026-08-09.md)
- [Phase 18–24 Native Roadmap](../roadmap/PHASE_18_24_NATIVE_LANGUAGE_AND_RUNTIME.md)
- [Phase 24 Language Completeness and Structured Errors](../roadmap/PHASE_24_LANGUAGE_COMPLETENESS_AND_STRUCTURED_ERRORS.md)

## Developer Tools

- [Debugging, Tooling, and Hot Reload](DEBUGGING_TOOLING_AND_HOT_RELOAD.md)
- [Determinism and Replay](DETERMINISM_AND_REPLAY.md)

## Detailed Chinese Specifications

The original detailed documents remain useful for implementation history and subsystem-level constraints:

- [Overall engine design](../ENGINE_DESIGN.md)
- [Game Scripting SDK overview](../zh-CN/GAME_SCRIPTING_SDK.md)
- [Specification index](../spec/README.md)
- [Phase 11–17 implementation profile](../roadmap/PHASE_11_17_LANGUAGE_EXPANSION.md)
- [Implementation roadmap](../roadmap/PHASE_1A.md)

## Versioning

The current documentation describes the v0.2.0 implementation through Phase
24. The source language, MIR, bytecode, runtime ABI, metadata, and debug-info
formats use separate version dimensions. A numbered GitHub release does not
freeze those compatibility contracts; pin an exact release and rebuild AOT
artifacts with the matching SDK.
