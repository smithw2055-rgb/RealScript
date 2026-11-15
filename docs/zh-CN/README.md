# RealScript 中文文档索引

[文档首页](../README.md) | [English Docs](../en/README.md) | [中文总览](../../README.zh-CN.md) | [变更日志](../../CHANGELOG.md)

## 核心中文指南

- [RealScript 引擎集成指南](integration_guide.md) — L0/L1 与配套 L2 `RealScript-GamePack` 分层选型、`BindGen` 集成、`StencilJit`（`x86_64` / `ARM64`）、`ParallelJobScheduler`、结构化热重载、`DapServer` 遥测与 `RSG001`–`RSG005` 工程治理
- [C# 兼容性对照矩阵](CSHARP_COMPATIBILITY_MATRIX.md) — 已支持语法、特性与有意裁剪边界（含默认接口方法、关系与 `and`/`or`/`not` 组合模式、`[StackOnly]`、`sequence`、`fixed`）
- [性能基线与演进报告](PERFORMANCE_BASELINE.md) — Interpreter、C++17 Typed AOT、`StencilJit`、O2 优化、分代 GC 与并行 Fiber 实测数据与优化阶段说明

## 英文详尽专题

更底层的编译器、运行时与 SDK 设计文档见 [英文文档索引](../en/README.md)：

- [Architecture Overview](../en/ARCHITECTURE.md) | [Getting Started](../en/GETTING_STARTED.md)
- [Language and Type System](../en/LANGUAGE_AND_TYPE_SYSTEM.md) | [Compilation and Bytecode](../en/COMPILATION_AND_BYTECODE.md)
- [Runtime, GC, and Embedding](../en/RUNTIME_GC_AND_EMBEDDING.md) | [AOT, JIT, and Performance](../en/AOT_JIT_AND_PERFORMANCE.md)
- [Debugging, Tooling, and Hot Reload](../en/DEBUGGING_TOOLING_AND_HOT_RELOAD.md) | [Determinism and Replay](../en/DETERMINISM_AND_REPLAY.md)
- [Productization](../en/PRODUCTIZATION.md) | [Project Status and Roadmap](../en/PROJECT_STATUS_AND_ROADMAP.md)

> **游戏 SDK 与 Gameplay Runtime 文档迁移说明：** 原 `GAME_SCRIPTING_SDK.md` 与 `GAMEPLAY_RUNTIME.md` 已随 L2 代码拆分移至配套 **`RealScript-GamePack`** 仓库（`docs/zh-CN/` 与 `docs/en/`）。
