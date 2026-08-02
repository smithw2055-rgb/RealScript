# RealScript English Documentation

[Repository README](../../README.md) | [简体中文文档](../zh-CN/README.md)

This library describes the RealScript v0.1 alpha technical baseline and the bounded Phase 11–17 gameplay-language profile. It is organized by subsystem and user workflow.

## New Users

1. [Getting Started](GETTING_STARTED.md)
2. [Architecture](ARCHITECTURE.md)
3. [Language and Type System](LANGUAGE_AND_TYPE_SYSTEM.md)
4. [Phase 11–17 Language Expansion Profile](LANGUAGE_EXPANSION_PHASE_11_17.md)
5. [Game Scripting SDK](GAME_SCRIPTING_SDK.md)
6. [Deterministic Gameplay Runtime](GAMEPLAY_RUNTIME.md)
7. [SDK Productization](PRODUCTIZATION.md)
8. [Project Status and Roadmap](PROJECT_STATUS_AND_ROADMAP.md)

## Compiler and Execution Pipeline

- [Compilation, MIR, and Bytecode](COMPILATION_AND_BYTECODE.md)
- [Runtime, GC, and Embedding](RUNTIME_GC_AND_EMBEDDING.md)
- [Phase 11–17 Language Expansion Profile](LANGUAGE_EXPANSION_PHASE_11_17.md)
- [Game Scripting SDK](GAME_SCRIPTING_SDK.md)
- [Deterministic Gameplay Runtime](GAMEPLAY_RUNTIME.md)
- [SDK Productization](PRODUCTIZATION.md)
- [AOT, JIT, and Performance](AOT_JIT_AND_PERFORMANCE.md)

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

The current documentation describes the v0.1 alpha baseline plus an additive, bounded language profile. The source language, expansion profile, MIR, bytecode, runtime ABI, metadata, and debug-info formats use separate version dimensions. None should be assumed stable until explicitly frozen.
