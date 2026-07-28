# RealScript English Documentation

[Repository README](../../README.md) | [简体中文文档](../zh-CN/README.md)

This library describes the RealScript v0.1 alpha technical baseline in English. It is organized by subsystem and user workflow.

## New Users

1. [Getting Started](GETTING_STARTED.md)
2. [Architecture](ARCHITECTURE.md)
3. [Language and Type System](LANGUAGE_AND_TYPE_SYSTEM.md)
4. [Game Scripting SDK](GAME_SCRIPTING_SDK.md)
5. [Project Status and Roadmap](PROJECT_STATUS_AND_ROADMAP.md)

## Compiler and Execution Pipeline

- [Compilation, MIR, and Bytecode](COMPILATION_AND_BYTECODE.md)
- [Runtime, GC, and Embedding](RUNTIME_GC_AND_EMBEDDING.md)
- [Game Scripting SDK](GAME_SCRIPTING_SDK.md)
- [AOT, JIT, and Performance](AOT_JIT_AND_PERFORMANCE.md)

## Developer Tools

- [Debugging, Tooling, and Hot Reload](DEBUGGING_TOOLING_AND_HOT_RELOAD.md)
- [Determinism and Replay](DETERMINISM_AND_REPLAY.md)

## Detailed Chinese Specifications

The original detailed documents are still available and remain useful for implementation history and subsystem-level constraints:

- [Overall engine design](../ENGINE_DESIGN.md)
- [Game Scripting SDK overview](../zh-CN/GAME_SCRIPTING_SDK.md)
- [Specification index](../spec/README.md)
- [Implementation roadmap](../roadmap/PHASE_1A.md)

## Versioning

The current documentation describes the v0.1 alpha baseline. The language, MIR, bytecode, runtime ABI, metadata, and debug-info formats use separate version dimensions. None of these dimensions should be assumed stable until explicitly marked as frozen.
