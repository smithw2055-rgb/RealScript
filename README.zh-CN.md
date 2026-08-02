# RealScript

[English](README.md) | **简体中文**

RealScript 是一门面向现代游戏引擎的嵌入式强类型脚本语言与运行时。它采用接近 C# 的语法，以 C++17 游戏引擎为主要宿主，同时提供解释器、C++17 AOT、可选原生 JIT、源码级调试、语言服务器、热重载和确定性执行能力。

> 当前状态：Phase 1–10 编译器、Game SDK 与确定性 Gameplay Runtime 已完成；Phase 11–17 受限游戏语言扩展 Profile 也已贯通多文件/多模块编译、Bytecode、Interpreter、C++17 AOT 与 Game SDK 元数据。语言、Expansion Profile、`.rsbc`、对象 ABI、Native Module ABI、Gameplay 状态格式和 GC 契约仍处于 Draft 阶段。

## 已实现能力

- Lexer、Parser、Binder、Flow Analysis 和稳定诊断；
- 多文件模块、函数重载、递归、显式 import 和增量编译；
- 多基本块 Typed MIR、验证器与 O0/O1/O2 优化；
- `.rsbc` 0.5 类型化寄存器字节码、反汇编器与严格验证器；
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
- [总体架构设计](docs/ENGINE_DESIGN.md)
- [规范文档索引](docs/spec/README.md)

## 当前边界

Phase 11–17 是面向游戏的受限 Profile，不是完整 CLR/C#：

- Interface 只做编译期实现校验，没有 Interface 类型值和虚分派；
- Delegate/Event 不是通用一等对象，Lambda 不捕获任意局部变量；
- 泛型要求显式类型参数，集合固定容量，无开放泛型、约束和协变/逆变；
- Sequence 只支持 `yield wait_ticks`，跨 yield 的持久状态必须放在对象字段；
- `ref/out/in` 仅支持受限独立调用，无 ref local/ref return/ref field；
- `byte/uint/float/char` 等当前映射到已有 `int/long/double` 载体，并非独立 ABI 类型；
- Attribute 尚未写入 `.rsbc`，展开代码尚无完整精确源码映射；
- 继承、运行时多态、异常、完整闭包、Nullable、Boxing、直接机器码 JIT、OSR、PGO 与 Rollback 网络协议仍是后续工作。

## License

许可证尚未确定。在正式提交许可证文件前，请勿假定本仓库代码或文档可以再分发。
