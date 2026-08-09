# RealScript

[English](README.md) | **简体中文**

RealScript 是一门面向现代游戏引擎的嵌入式强类型脚本语言与运行时。它采用接近 C# 的语法，以 C++17 游戏引擎为主要宿主，同时提供解释器、C++17 AOT、可选原生 JIT、源码级调试、语言服务器、热重载和确定性执行能力。

> 当前状态：Phase 1–24 已完成。原生编译器现已覆盖运行时多态、一等委托与闭包、可推断的编译期泛型与可增长集合、确定性协程状态机、精确值/引用语义、常用 C# 风格 Pattern/便利语法和结构化脚本异常。语言、`.rsbc`、对象 ABI、Native Module ABI、Gameplay 状态格式和 GC 契约仍处于 Draft 阶段。

## 已实现能力

- Lexer、Parser、Binder、Flow Analysis 和稳定诊断；
- 多文件模块、函数重载、递归、显式 import 和增量编译；
- 多基本块 Typed MIR、验证器与 O0/O1/O2 优化；
- `.rsbc` 0.9 类型化寄存器字节码（兼容读取 0.6–0.8）、反汇编器与严格验证器；
- 字节码解释器、预算、结构化运行时错误和脚本调用栈；
- class、构造函数、方法、属性、数组、enum、struct 和 Native Handle；
- 精确 Shadow Stack、代际 `ObjectRef`、增量 Mark/Sweep、写屏障和堆诊断；
- DAP 调试器、LSP 语言服务器和函数体热重载；
- C++17 AOT、C11/C++ Native Module 查询 ABI 和 CMake 集成；
- Strict/Record/Replay 确定性执行、稳定摘要和逐函数 Profile；
- 类型化 C++ 游戏绑定、脚本对象、场景生命周期、事件、触发器和 SDK 产品化；
- 代际 Entity、固定 Tick、PCG 随机流、确定性计时器与事件队列；
- Script Contract、Gameplay Metadata、固定 Tick Sequence 和 `SceneGameplayDriver`；
- 版本化 `RSGS` Gameplay 状态格式、稳定 Hash、存档、回放和 Rollback 恢复辅助；
- Phase 11：`for`、集合 `foreach`、`do/while`、`break`、`continue`、`switch`；
- Phase 12：事件签名、方法组、受限 Lambda 和确定性 Event；
- Phase 13：Interface 声明与编译期 Contract 校验；
- Phase 14：源码 Attribute 与 `GameCompileResult`/`GameProgram` 元数据；
- Phase 15：显式泛型特化和固定容量集合；
- Phase 16：`sequence` 与 `yield wait_ticks` 确定性协程 Profile；
- Phase 17：受限 `ref/out/in` 与基础值类型别名；
- Phase 18：上述扩展全部迁入原生 Syntax/Bound/MIR/Bytecode/AOT 流水线；
- Phase 19：单继承、运行时 Interface/Virtual 分派、可见性和稳定对象布局；
- Phase 20：一等 Delegate、方法引用、共享可变捕获、堆闭包、多播和通用 Event 存储；
- Phase 21：泛型推断/约束/成员/Interface/Delegate、可增长集合和 Enumerator；
- Phase 22：可持久化局部与控制流的确定性 Sequence 状态机；
- Phase 23：精确值类型、可变 Struct、ref 位置、Nullable 和 Boxing；
- Phase 24：`var`、空/条件运算、初始化器、灵活参数、Pattern、Switch Expression 和结构化异常；
- `rsbench` 基准工具和可选外部 C++ 工具链 JIT；
- Ubuntu 与 Windows warnings-as-errors 全量 CI。

## 快速开始

```bash
cmake -S . -B build \
  -DREALSCRIPT_BUILD_TESTS=ON \
  -DREALSCRIPT_WARNINGS_AS_ERRORS=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

检查和运行脚本：

```bash
rsc math.rs main.rs
rsc math.rs main.rs --mir
rsc game.rs --run Game.Main::main
rsc game.rs --run Game.Main::main \
  --opt-level 2 --deterministic --profile --digest
```

生成 C++17 AOT：

```bash
rsaot --output-dir build/generated/game \
  --program-name GameScripts \
  --opt-level 2 \
  game.rs common.rs
```

游戏运行时入口：

```cpp
#include "realscript/game/Gameplay.h"

realscript::game::GameApi api;
auto gameplay = std::make_shared<realscript::game::GameplayHost>(60, 1234, 7);
realscript::game::installGameplayBindings(api, gameplay);
```

Gameplay 调度状态由 `encodeGameplayHostState()` / `restoreGameplayHostState()` 保存和恢复；脚本对象字段继续使用 `ScriptObjectState`，引擎级 Rollback Frame 应组合两类状态。

## 中文文档

- [中文文档入口](docs/zh-CN/README.md)
- [游戏脚本 SDK](docs/zh-CN/GAME_SCRIPTING_SDK.md)
- [确定性游戏运行时](docs/zh-CN/GAMEPLAY_RUNTIME.md)
- [Phase 11–17 游戏语言扩展 Profile](docs/roadmap/PHASE_11_17_LANGUAGE_EXPANSION.md)
- [Phase 18–24 原生语言与运行时路线图](docs/roadmap/PHASE_18_24_NATIVE_LANGUAGE_AND_RUNTIME.md)
- [Phase 19 运行时多态](docs/roadmap/PHASE_19_RUNTIME_POLYMORPHISM.md)
- [Phase 20 一等委托与闭包](docs/roadmap/PHASE_20_FIRST_CLASS_DELEGATES.md)
- [Phase 21 完整泛型与集合](docs/roadmap/PHASE_21_COMPLETE_GENERICS_AND_COLLECTIONS.md)
- [Phase 22 确定性协程状态机](docs/roadmap/PHASE_22_DETERMINISTIC_COROUTINE_STATE_MACHINES.md)
- [Phase 23 完整值与引用语义](docs/roadmap/PHASE_23_COMPLETE_VALUE_AND_REFERENCE_SEMANTICS.md)
- [Phase 24 语言完整性与结构化异常](docs/roadmap/PHASE_24_LANGUAGE_COMPLETENESS_AND_STRUCTURED_ERRORS.md)
- [C# 风格特性兼容矩阵](docs/zh-CN/CSHARP_COMPATIBILITY_MATRIX.md)
- [总体架构设计](docs/ENGINE_DESIGN.md)
- [规范文档索引](docs/spec/README.md)

## 当前边界

当前实现是面向游戏的强类型确定性语言，不是完整 CLR/C#。Phase 19–24 已解除旧 Profile 的多态、闭包、泛型/集合、协程、值/引用和结构化异常缺口；主要剩余边界是：

- 泛型采用编译期特化；不支持开放运行时泛型、协变/逆变和 CLR Reflection；
- Sequence 是单线程 `yield wait_ticks` 确定性模型，不提供 `Task`、线程或通用 `async/await`；
- 已支持 ref local/return/field/indexer，但无 unsafe 指针、ref struct、ref property 和完整逃逸分析；
- 已支持脚本对象异常和确定性 `finally`，但无 Filter、`using`、原生异常互操作和异常跨协程暂停；
- 无运算符重载、用户自定义转换、LINQ、`dynamic` 和完整 .NET BCL；
- 直接进程内机器码 JIT、OSR、PGO 与 Rollback 网络协议仍是后续工作。

## License

RealScript 使用 Apache License 2.0 许可证，详见 [LICENSE](LICENSE)。
