# RealScript

[English](README.md) | **简体中文**

RealScript 是一门面向现代游戏引擎的嵌入式强类型脚本语言与运行时。它采用接近 C# 的语法，以 C++17 游戏引擎为主要宿主，同时提供解释器、C++17 AOT、可选原生 JIT、源码级调试、语言服务器、热重载和确定性执行能力。

> 当前状态：Phase 1–6 编译器与运行时路线、Phase 7 游戏脚本 SDK，以及 Phase 8–10 确定性游戏运行时已经完成，形成 RealScript v0.1 alpha 集成基线。语言、`.rsbc` 字节码、对象 ABI、Native Module ABI、Gameplay 状态格式和 GC 契约仍处于 Draft 阶段，尚未冻结长期兼容性。

## 已实现能力

- Lexer、Parser、Binder、Flow Analysis 和稳定诊断；
- 多文件模块、函数重载、递归和增量编译；
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
- Script Contract、宿主 Metadata、固定 Tick Sequence 和 `SceneGameplayDriver`；
- 版本化 `RSGS` Gameplay 状态格式、稳定 Hash、存档、回放和 Rollback 恢复辅助；
- `rsbench` 基准工具和可选外部 C++ 工具链 JIT；
- Interpreter/AOT/JIT 差分验证；
- Ubuntu 与 Windows Server 2025 / Visual Studio 2026 CI。

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
- [总体架构设计](docs/ENGINE_DESIGN.md)
- [规范文档索引](docs/spec/README.md)
- [Phase 8–10 游戏运行时路线](docs/roadmap/PHASE_8_10_GAMEPLAY_RUNTIME.md)

英文文档是仓库默认入口；现有详细规范和阶段文档继续保留中文版本。

## 当前边界

尚未实现或尚未冻结的主要能力包括：继承、源码级接口与虚分派、泛型、异常、源码级协程和 `async`、`ref`/`out`、完整 `for`/`foreach`/`switch`、源码 Attribute、完整跨工具链二进制 ABI、直接机器码 JIT、OSR、PGO，以及 Rollback 网络协议。当前 Gameplay Runtime 已提供固定 Tick、Contract、Metadata、Sequence、快照和恢复能力，但不会假装这些 C# 语言特性已经完成。

## License

许可证尚未确定。在正式提交许可证文件前，请勿假定本仓库代码或文档可以再分发。
