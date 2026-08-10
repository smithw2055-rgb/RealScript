# RealScript

[![RealScript CI](https://github.com/smithw2055-rgb/RealScript/actions/workflows/ci.yml/badge.svg)](https://github.com/smithw2055-rgb/RealScript/actions/workflows/ci.yml)
[![GitHub Release](https://img.shields.io/github/v/release/smithw2055-rgb/RealScript)](https://github.com/smithw2055-rgb/RealScript/releases)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)

[English](README.md) | **简体中文**

RealScript 是一门面向现代 C++17 游戏引擎的嵌入式强类型脚本语言与运行时。
它采用接近 C# 的源码语法，同时提供经过验证的字节码、确定性执行、C++17 AOT、
可选外部工具链 JIT、游戏绑定、固定 Tick 协程、回放和 Rollback。

> **v0.2.0 状态：** Phase 1–24 实现已经完成，可作为同版本 SDK 的游戏引擎集成
> 基线。源码语言、`.rsbc`、Native ABI、元数据和 Gameplay 状态格式均已版本化，
> 但尚未冻结长期兼容性。RealScript 不是 CLR 兼容的 C# 实现。

## 为什么使用 RealScript

- **统一的验证语义：** Interpreter、AOT 和 JIT 共享 checked arithmetic、错误、
  预算、Profile 与确定性事件行为。
- **面向游戏模拟：** 固定 Tick、PCG 随机流、确定性 Timer/Event、可序列化协程、
  Snapshot、Replay 与 Rollback。
- **原生引擎集成：** 类型化 C++ 绑定、托管脚本对象、Native Handle、CMake 包和
  生成式 C++17 模块。
- **完整开发工具：** 编译/运行器、AOT 生成器、DAP 调试器、LSP、热重载、Profiler
  和基准工具。
- **核心无第三方依赖：** 编译器和运行时只要求 C++17 与 CMake；可选 JIT 调用已安装
  的外部 C++ 编译器。

## 语言与运行时特性

| 领域 | v0.2.0 已实现能力 |
|---|---|
| 语言 | Module/import、重载、Class、单继承、Interface、Virtual Dispatch、Delegate、Closure、Event、Generic、Collection、Enum、可变 Struct、Nullable、Boxing、Pattern、Initializer、灵活参数与结构化异常。 |
| 控制流 | `if`、`while`、`for`、`foreach`、`do/while`、`switch`、Switch Expression、`break`、`continue`、`try/catch/finally` 和确定性 `sequence`/`yield wait_ticks`。 |
| 编译器 | Lexer、Parser、Binder、Flow Analysis、稳定诊断、经过验证的多基本块 Typed MIR，以及 O0/O1/O2 优化。 |
| 字节码 | 类型化寄存器 VM、确定性 `.rsbc` 0.9 输出、0.6–0.8 兼容读取、防御性验证和结构化脚本调用栈。 |
| 内存 | 代际 ObjectRef、精确根、增量 Mark/Sweep、写屏障、有界 GC Work、Snapshot、Retaining Path 和泄漏诊断。 |
| 原生执行 | 确定性 C++17 AOT、C11 Module Query ABI、Source Map、类型化 Native Thunk、可选工具链 JIT 和内容寻址缓存。 |
| 确定性 | Off/Strict/Record/Replay、指令/递归/分配/堆预算、稳定 Digest、外部调用回放与跨后端差分验证。 |
| Game SDK | 类型化 Binding、Rooted Object、Scene 生命周期、Event/Trigger、固定 Tick Gameplay Host、Sequence 调度、`RSGS` 状态编码及 Save/Replay/Rollback 辅助。 |
| 工具链 | `rsc`、`rsaot`、`rsdebug`、`rslsp`、`rsbench`、DAP、LSP、Profile、源码元数据和函数体兼容热重载。 |

完整的支持/部分支持/不支持清单见
[C# 风格兼容矩阵](docs/zh-CN/CSHARP_COMPATIBILITY_MATRIX.md)。

## 快速上手

### 环境要求

- CMake 3.20 或更新版本
- C++17 编译器
- 构建 Native ABI 一致性测试时需要 C11 编译器
- 仓库 CI 辅助脚本需要 Python 3

### 构建与测试

```bash
cmake -S . -B build \
  -DREALSCRIPT_BUILD_TESTS=ON \
  -DREALSCRIPT_WARNINGS_AS_ERRORS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Visual Studio generator 通常把工具放在 `build/Release/`；单配置 generator 可以省略
`--config Release` 和 `-C Release`。

### 编写并运行脚本

创建 `hello.rs`：

```csharp
module Demo.Hello;

class Counter
{
    int value;

    int Add(int amount)
    {
        value = value + amount;
        return value;
    }
}

int main()
{
    int total = 0;
    for (int value = 0; value < 10; value = value + 1)
    {
        total = total + value;
    }
    return total;
}
```

验证、检查并运行：

```bash
build/rsc hello.rs
build/rsc hello.rs --mir
build/rsc hello.rs --bytecode
build/rsc hello.rs --run Demo.Hello::main --opt-level 2
```

### 生成 C++17 AOT

```bash
build/rsaot \
  --output-dir build/generated/hello \
  --program-name HelloScripts \
  --opt-level 2 \
  --opt-report \
  hello.rs
```

输出包括公共 Header、C++17 实现、确定性 Manifest、源码元数据和 Native Function
Descriptor。生成代码直接执行经过验证的 Typed MIR 语义，不嵌入 Bytecode VM。

### 嵌入 Game SDK

```cpp
#include "realscript/game/Gameplay.h"

realscript::game::GameApi api;
auto gameplay = std::make_shared<realscript::game::GameplayHost>(60, 1234, 7);
realscript::game::installGameplayBindings(api, gameplay);

realscript::game::GameScriptCompiler compiler(api);
auto compiled = compiler.compile(sources);
realscript::game::ScriptRuntime scripts(compiled.program);
realscript::game::SceneScriptRuntime scene(scripts);
```

多文件 Module、Bytecode Artifact、CMake AOT、调试与编辑器接入见
[英文快速上手](docs/en/GETTING_STARTED.md)。

## 执行后端

| 后端 | 适合场景 | 说明 |
|---|---|---|
| Bytecode Interpreter | 开发、调试、热重载、冷代码和低频脚本 | 启动快、工具完整；CPU 密集循环慢于原生执行。 |
| C++17 AOT | 发布版 Gameplay 和性能敏感的确定性代码 | 生成可移植 C++，同时保留 checked runtime 语义。 |
| Toolchain JIT | 桌面开发，以及不单独执行 AOT 构建时的原生执行 | 复用 AOT 生成器，调用外部编译器并缓存 Shared Module。 |

## 性能指标

以下数据来自 AMD Ryzen 7 6800H、Windows 11、Visual Studio 2026 的本地 Release
构建。它们用于回归比较，不是跨语言排名。时间采用中位数；RAW 显式使用
`gcWorkBudget=0`，GC 单独测量。

| Workload | 结果 |
|---|---:|
| Native C++ 整数循环 | 4.06 µs |
| RealScript C++17 AOT RAW | 25.86 µs |
| RealScript Toolchain JIT RAW | 25.68 µs |
| AOT Strict 确定性模式 | 0.358 ms |
| JIT Strict 确定性模式 | 0.347 ms |
| Interpreter 整数循环，130,011 条指令 | 6.73 ms |
| Interpreter 分支循环，180,011 条指令 | 9.46 ms |
| Interpreter 函数调用循环，10,000 次调用 | 12.99 ms |
| Allocation Tick，GC Work 8 | 27.75 ms；460 次 Collection；Live 16,320 B |
| 10,000 个协程 Resume | 86.06 ms；8.61 µs/Callback |
| 10,000 个协程确定性 Replay | 89.45 ms |

v0.2.0 的关键性能改动包括：

- Typed AOT/JIT Scalar Lowering 与 Range-Proven Checked Arithmetic；
- 直接 Typed CFG Label 与 Failure-Aware Accounting；
- 保持精确预算和 Digest 的局部 RAW/Strict Accounting；
- Interpreter 整数专用路径和低成本调用栈物化；
- 持续分配时仍能有界完成的 Incremental Sweep；
- Scene Method Descriptor Cache 和更少的 Scheduler Payload Copy。

详细方法、优化历史、被撤销的实验和复现命令见
[完整性能报告](docs/zh-CN/PERFORMANCE_BASELINE_2026-08-09.md)与
[AOT/JIT 性能指南](docs/en/AOT_JIT_AND_PERFORMANCE.md)。

## 确定性 Gameplay

```csharp
sequence Attack(long target)
{
    PlayWindup();
    yield wait_ticks(12);
    SpawnProjectile(target);
    yield wait_ticks(6);
    Finish();
}
```

Sequence 会降低为显式状态机；参数、Local、Temporary、嵌套进度和控制流位置可以跨越
Suspend、Snapshot、Restore、Replay、Rollback 与 C++17 AOT。

## 命令行工具

| 工具 | 用途 |
|---|---|
| `rsc` | 编译、验证、检查 MIR/Bytecode、运行脚本并输出 Package、Profile 和 Digest。 |
| `rsaot` | 生成确定性 C++17 AOT 源码与 Manifest。 |
| `rsdebug` | 启动源码级 Debug Adapter Protocol Server。 |
| `rslsp` | 启动 Language Server Protocol Server。 |
| `rsbench` | 运行可复现 Timing、GC、Digest 与 Profile 基准。 |

## 文档

- [中文文档入口](docs/zh-CN/README.md)
- [快速上手](docs/en/GETTING_STARTED.md)
- [语言与类型系统](docs/en/LANGUAGE_AND_TYPE_SYSTEM.md)
- [C# 风格兼容矩阵](docs/zh-CN/CSHARP_COMPATIBILITY_MATRIX.md)
- [总体架构](docs/en/ARCHITECTURE.md)
- [编译、MIR 与 Bytecode](docs/en/COMPILATION_AND_BYTECODE.md)
- [Runtime、GC 与嵌入](docs/en/RUNTIME_GC_AND_EMBEDDING.md)
- [AOT、JIT 与性能](docs/en/AOT_JIT_AND_PERFORMANCE.md)
- [确定性与回放](docs/en/DETERMINISM_AND_REPLAY.md)
- [Game Scripting SDK](docs/zh-CN/GAME_SCRIPTING_SDK.md)
- [确定性 Gameplay Runtime](docs/zh-CN/GAMEPLAY_RUNTIME.md)
- [产品状态与路线图](docs/en/PROJECT_STATUS_AND_ROADMAP.md)
- [版本变更记录](CHANGELOG.md)

## 明确边界

RealScript 不是 CLR 兼容 C#。v0.2.0 明确不提供：

- 多类继承、默认 Interface 实现、开放运行时泛型或 Generic Variance；
- 通用 `Task`、线程或不受限的 `async/await`；
- Unsafe Pointer、Ref Struct、Ref Property 或完整 Escape Analysis；
- Exception Filter、`using`、Native Exception Interop，或异常跨越协程暂停点；
- Operator Overload、用户自定义转换、LINQ、`dynamic`、反射代码生成或 .NET BCL；
- 直接进程内机器码 JIT、OSR、PGO 或 Rollback Networking Protocol；
- Source、`.rsbc`、AOT ABI、SDK Metadata 和 Gameplay State 的长期冻结兼容性。

游戏引擎集成应固定到准确的 RealScript Release 或 Commit，并使用匹配 SDK 重新生成
AOT Artifact。

## License

RealScript 使用 Apache License 2.0，详见 [LICENSE](LICENSE)。
