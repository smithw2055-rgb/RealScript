# RealScript Documentation

- [English Documentation](en/README.md) — Architecture, compiler, runtime, GC, AOT/JIT, tooling, C# compatibility, and productization
- [中文文档](zh-CN/README.md) — 引擎集成指南、C# 兼容性对照、性能基线
- [Repository README (English)](../README.md) | [仓库总览 (中文)](../README.zh-CN.md) | [Changelog](../CHANGELOG.md) | [Macro Benchmarks](../benchmarks/macro/README.md)

## Repository Architecture (L0 / L1 / L2)

RealScript is organized into strict acyclic layers:

- **L0 Kernel & L1 SDK (`RealScript`)**:
  - **L0 (`REALSCRIPT_SLIM_RUNTIME=ON` compatible core)**: `Foundation`, `Bytecode`, `Runtime`, `AotRuntime`, `BindGenRuntime`, `HotReload`, `StencilJit`, `SlimRuntime`, `Kernel`
  - **L0 Compiler & Native Backends**: `Compiler`, `NativeBackends`, `AotSupport` (`rsc`, `rsaot`)
  - **L1 Optional Tooling**: `Debug` (`rsdebug`), `Tooling` (`rslsp`), `Extensibility`, `BindGen` (`rsbind`), `Frontend`, `Full`
- **L2 Game Engine SDK (`RealScript-GamePack`)**: Standalone companion repository (`RealScript::GameRuntime`, `RealScript::GameCompiler`, `RealScript::GameSdk`, `rsbench_product`) providing `SceneScriptRuntime`, zero-copy ECS column bindings, `GameplayHost`, `ReflectionRegistry`, `ModHost`, and `RSGS` save/rollback serialization. See `RealScript-GamePack/README.md` and `RealScript-GamePack/docs/`.
