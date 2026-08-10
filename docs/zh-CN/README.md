# RealScript 中文文档

[English Documentation](../README.md) | **简体中文**

仓库默认使用英文 README 和英文主题文档。现有中文架构、规范和阶段文档继续保留原路径，作为更详细的实现历史与设计记录。

## 中文入口

- [仓库中文 README](../../README.zh-CN.md)
- [游戏脚本 SDK](GAME_SCRIPTING_SDK.md)
- [确定性游戏运行时](GAMEPLAY_RUNTIME.md)
- [C# 风格特性兼容矩阵](CSHARP_COMPATIBILITY_MATRIX.md)
- [性能基线、瓶颈与优化结果](PERFORMANCE_BASELINE_2026-08-09.md)
- [Phase 18–24 原生语言与运行时路线图](../roadmap/PHASE_18_24_NATIVE_LANGUAGE_AND_RUNTIME.md)
- [Phase 11–17 游戏语言扩展 Profile](../roadmap/PHASE_11_17_LANGUAGE_EXPANSION.md)
- [总体架构设计](../ENGINE_DESIGN.md)
- [规范文档索引](../spec/README.md)

## 规范文档

- [语言规范](../spec/LANGUAGE_SPEC.md)
- [Typed MIR 规范](../spec/MIR_SPEC.md)
- [运行时模型](../spec/RUNTIME_MODEL.md)
- [字节码与 ABI](../spec/BYTECODE_AND_ABI.md)
- [`.rsbc` 0.5 物理格式](../spec/BYTECODE_FORMAT_V0.md)
- [嵌入与可观测性](../spec/EMBEDDING_AND_OBSERVABILITY_V0.md)
- [托管堆与 GC](../spec/MANAGED_HEAP_GC_V0.md)
- [对象模型](../spec/OBJECT_MODEL_V0.md)
- [数组、Native Handle 与堆诊断](../spec/ARRAYS_NATIVE_HANDLES_HEAP_DIAGNOSTICS_V0.md)
- [方法、构造函数与属性](../spec/MEMBERS_PROPERTIES_V0.md)
- [数值、枚举与结构体](../spec/NUMERIC_ENUM_STRUCT_V0.md)
- [调试、工具链与热重载](../spec/DEBUG_TOOLING_HOT_RELOAD_V0.md)
- [C++17 AOT](../spec/CXX17_AOT_V0.md)
- [确定性、优化与 JIT](../spec/DETERMINISM_OPTIMIZATION_JIT_V0.md)

## Phase 1–17 实现说明

- [Phase 1A](../roadmap/PHASE_1A.md)
- [Phase 1B](../roadmap/PHASE_1B.md)
- [Phase 1C](../roadmap/PHASE_1C.md)
- [Phase 2A](../roadmap/PHASE_2A.md)
- [Phase 2B](../roadmap/PHASE_2B.md)
- [Phase 2C](../roadmap/PHASE_2C.md)
- [Phase 3A](../roadmap/PHASE_3A.md)
- [Phase 3B](../roadmap/PHASE_3B.md)
- [Phase 3C](../roadmap/PHASE_3C.md)
- [Phase 3D](../roadmap/PHASE_3D.md)
- [Phase 3E](../roadmap/PHASE_3E.md)
- [Phase 4](../roadmap/PHASE_4.md)
- [Phase 5](../roadmap/PHASE_5.md)
- [Phase 6](../roadmap/PHASE_6.md)
- [Phase 8–10 游戏运行时基础](../roadmap/PHASE_8_10_GAMEPLAY_RUNTIME.md)
- [Phase 11–17 游戏语言扩展 Profile](../roadmap/PHASE_11_17_LANGUAGE_EXPANSION.md)

Phase 7 的 Game SDK 与产品化接口见上方专题文档；Phase 8–10 覆盖固定 Tick、Gameplay Host、状态快照与 Rollback；Phase 11–17 提供受限、确定性的 C# 风格游戏语言扩展，并明确保留运行时接口分派、完整闭包、开放泛型和完整引用语义等边界。

英文文档按使用主题组织，中文文档按规范与历史实现阶段组织。两者描述同一个
v0.2.0 实现基线；规范文档中的 Draft v0.1 是独立的规范版本，不等同于产品版本。
