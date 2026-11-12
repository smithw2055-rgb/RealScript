# RealScript

[![RealScript CI](https://github.com/smithw2055-rgb/RealScript/actions/workflows/ci.yml/badge.svg)](https://github.com/smithw2055-rgb/RealScript/actions/workflows/ci.yml)
[![GitHub Release](https://img.shields.io/github/v/release/smithw2055-rgb/RealScript)](https://github.com/smithw2055-rgb/RealScript/releases)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)

[English](README.md) | **简体中文**

RealScript 是一门面向现代 C++17 游戏引擎的嵌入式强类型脚本语言与运行时。
它采用接近 C# 的源码语法，同时提供经过验证的字节码、确定性执行、C++17 AOT、
可选外部工具链 JIT、游戏绑定、固定 Tick 协程、回放和 Rollback。

> **v0.98.0 状态：** Phase 1–24 实现与游戏引擎深度集成演进（Game Engine Integration Evolution）已全部完成，提供了与现代引擎（如 Godot / Unity）深度对齐的完整脚本生态基线。源码语言、`.rsbc`、Native ABI、元数据和 Gameplay 状态格式均已版本化，但尚未冻结长期兼容性。RealScript 不是 CLR 兼容的 C# 实现。

## 为什么使用 RealScript

- **统一的验证语义：** Interpreter、AOT 和 JIT 共享 checked arithmetic、错误、
  预算、Profile 与确定性事件行为。
- **面向游戏模拟：** 固定 Tick、PCG 随机流、确定性 Timer/Event、可序列化协程、
  Snapshot、Replay 与 Rollback。
- **原生引擎集成：** 类型化 C++ 绑定、托管脚本对象、Native Handle、零拷贝 ECS 列绑定、C++ 绑定代码自动生成器（`rsbind`）、CMake 包和生成式 C++17 模块。
- **完整开发工具：** 编译/运行器、AOT 生成器、自动绑定生成器、DAP 调试器、LSP、VS Code 扩展、热重载、Profiler
  和基准工具。
- **核心无第三方依赖：** 编译器和运行时只要求 C++17 与 CMake；可选 JIT 调用已安装
  的外部 C++ 编译器。

## 语言与运行时特性

| 领域 | v0.98.0 已实现能力 |
|---|---|
| 语言 | Module/import、重载、运算符重载、复合赋值、Class、`static class`、静态构造函数（`static T()`）、`readonly` 字段、自动属性初始化器（`public int Hp { get; set; } = 100;`）、单继承（含脚本类直接继承 C++ 绑定类型）、Interface、Virtual Dispatch、Delegate、Closure、Event、Generic、Collection、Enum、可变 Struct、Nullable、Boxing、Pattern、Initializer、灵活参数与结构化异常。 |
| 控制流与双轨协程 | `if`、`while`、`for`、`foreach`（零堆分配 `struct` 迭代器快路径）、`do/while`、`switch`、Switch Expression、`break`、`continue`、`try/catch/finally`、确定性 `using (...)` / `using var` 资源释放（`IDisposable` / `ResourceHandle<T>`）；**双轨协程分工**：`sequence` 专司确定性帧模拟与 `RSGS` 快照回滚（`yield wait_ticks`/`wait_seconds`/`wait_event`/`wait_signal`/`wait_until`），`async`/`await`（`RealScript.Async`：`Awaitable<T>`、`CancellationToken`、四阶段主线程恢复队列）专司异步 `OnStart()`、资产/预制体流式加载与 HTTP/WebSocket 网络 I/O，辅以 `RealScript.Fiber` 协作式纤程。 |
| 编译器 | Lexer、Parser、Binder、Flow Analysis、`[Deterministic]` 静态校验、稳定诊断、经过验证的多基本块 Typed MIR、自然循环 LICM 变量加载外提、跨模块 `-O2` 自动属性与 `void` 小函数内联，以及 O0/O1/O2 优化。 |
| 字节码 | 类型化寄存器 VM、统一扁平栈帧布局（Unified Flat Frame）、Computed-goto 线程化派发（含 `double` 运算/超级指令与对象/结构体字段读写 `FlatOpcode` 快路径）、16 字节（16B）紧凑 `Value` 与 8B `Instruction`、32 位拓宽 `BytecodeString` 及无锁池、整数循环指令快化（Quickening）、确定性 `.rsbc` 0.9 输出、0.6–0.8 兼容读取、防御性验证和结构化脚本调用栈。 |
| 内存 | 代际 ObjectRef、12B 内联结构体槽（`InlineStructSlot`）、HeapObject 小缓冲区优化（SBO）、`ValuePoolArena` 池化对象 GC 回收、精确根、带阴影栈保护的增量 Mark/Sweep GC 步进、写屏障、有界 GC Work、Snapshot、Retaining Path 和泄漏诊断。 |
| 原生执行 | 确定性 C++17 AOT、零预算 Trusted Release AOT（`--trusted-release`）、`#line` 源码行号映射指令、自定义 `struct` 栈标量化（`AotStruct_<id>`）、AVX2/SIMD 向量化 ECS 循环重构（`__restrict` + `#pragma loop(ivdep)`）、C11 Module Query ABI、Source Map、类型化 Native Thunk、原生快速路径与对象标识缓存（Identity Cache）、可选工具链 JIT、内容寻址缓存与 Interpreter/JIT/AOT 分层执行。 |
| 确定性 | Off/Strict/Record/Replay、指令/递归/分配/堆预算、Mod 能力沙箱（Capability Sandboxing）、稳定 Digest、外部调用回放与跨后端差分验证。 |
| Game SDK | 类型化 Binding（`structType<S>` 自定义结构体封送、`constructor<Args...>`、`staticMethod<M>`、同名重载绑定、`std::vector<T>` <-> `T[]` 数组封送）、C++ 头文件自动生成绑定（`rsbind`，支持 `RS_EXPORT_STRUCT`、`RS_EXPORT_CONSTRUCTOR`、`static_cast` 重载消歧、静态方法与模板容器类型）、数学原语直接封送与快路径（`Fixed`/`Fixed16`/`Vec2`/`Vec3`/`Quat`）、C++ 宿主连续内存零拷贝 ECS 列绑定（`bindEcsRawColumnAuto<T>`）、`Query<Write<T>, Read<U>, With<V>, Without<W>>()` 与 `EntityCommandBuffer`、2D/3D 定点物理（`stepPhysics2D`/`stepPhysics3D`）、空间查询（Raycast/Overlap）、确定性 `RealScript.Input`、单实体多组件、代际实体句柄、Editor/Play 模式（`[Tool]`/`[ExecuteInEditMode]`）、`OnValidate`、深度 `InspectorTypeSchema` 反射、宿主 `connectSignal`、JSON 场景/预制体序列化与 `RSGS` 状态编码。 |
| 工具链 | `rsc`、`rsaot`、`rsbind`、`rsdebug`、`rslsp`、`rsbench`、DAP（标准流与内嵌 TCP `--dap-port` 附加调试、条件与异常断点、重入安全点、堆保留路径分析器、无副作用 Watch 求值）、LSP（`ParseSnapshot` 增量缓存、子图元数据增量重构缓存、异步请求桥接、成员 `.` 补全、`signatureHelp`、`.d.rscript` 宿主声明导出）、`vscode-realscript` 官方 VS Code 插件、Chrome Tracing 性能导出，以及带状态结构化热重载（`[FormerlySerializedAs]`、静态字段重命名迁移与跨组件引用迁移）。 |

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

创建 `hello.rscript`：

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
build/rsc hello.rscript
build/rsc hello.rscript --mir
build/rsc hello.rscript --bytecode
build/rsc hello.rscript --run Demo.Hello::main --opt-level 2
```

### 生成 C++17 AOT

```bash
build/rsaot \
  --output-dir build/generated/hello \
  --program-name HelloScripts \
  --opt-level 2 \
  --opt-report \
  hello.rscript
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

v0.98.0 的关键性能改动包括：

- 统一扁平栈帧布局（Unified Flat Frame）与 Computed-goto 解释器线程化派发；
- 零预算 Trusted Release AOT 模式（`--trusted-release`）与自定义结构体栈标量化；
- C++ 宿主连续结构体零拷贝 ECS 原始列绑定（`bindEcsRawColumnAuto`）与 AVX2 SIMD 循环重构；
- 32 位拓宽 `BytecodeString` 及无锁分页池、16 字节（16B）/8B 紧凑值与指令表示；
- 带阴影栈安全保护的增量 GC 步进与池化值回收；
- Typed AOT/JIT Scalar Lowering 与 Range-Proven Checked Arithmetic；
- 解释器整数特化与快化循环超级指令（Quickening）；
- 缓存场景方法描述符、原生快速路径（Native Fast-Path）与对象标识缓存（Identity Cache）。

详细方法、优化历史、被撤销的实验和复现命令见
[完整性能报告](docs/zh-CN/PERFORMANCE_BASELINE.md)与
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

RealScript 采用**双轨协程分工架构**：

- **`sequence`（确定性回滚轨）**：降低为显式对象字段状态机；参数、Local、Temporary、嵌套进度和控制流位置可完整跨越 Suspend、Snapshot、Restore、Replay、Rollback 与 C++17 AOT。
- **`async` / `await`（流式加载与网络 I/O 轨）**：基于 8 字节代际句柄 `Awaitable<T>`、`CancellationToken` 与四阶段主线程队列（`PreTickIo` / `FixedTick` / `Update` / `EndOfFrame`），用于异步生命周期 `OnStart()`、场景/资源流式加载、存档读写与 HTTP/WebSocket 请求，并在实体销毁时自动触发取消令牌。

## 命令行工具

| 工具 | 用途 |
|---|---|
| `rsc` | 编译、验证、检查 MIR/Bytecode、运行脚本并输出 Package、Profile 和 Digest。 |
| `rsaot` | 生成确定性 C++17 AOT 源码、行号映射与 Manifest。 |
| `rsbind` | 解析 C++ 头文件，自动生成类型安全绑定、方法 Thunk 与 ECS 连续列辅助代码。 |
| `rsdebug` | 启动源码级 Debug Adapter Protocol Server（支持 stdio 及 `--dap-port` TCP 附加调试）。 |
| `rslsp` | 启动 Language Server Protocol Server（支持增量缓存与快速子图查询）。 |
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

RealScript 不是 CLR 兼容 C#。v0.98.0 明确不提供：

- 多类继承、默认 Interface 实现、开放运行时泛型或 Generic Variance；
- 裸操作系统多线程共享可变状态或 CLR `System.Threading.Tasks.Task`（流式加载与网络 I/O 请使用受主线程帧队列约束的 `RealScript.Async` `Awaitable<T>`，帧回滚请使用 `sequence`）；
- Unsafe Pointer、Ref Struct、Ref Property 或完整 Escape Analysis；
- Exception Filter、`lock`、Native Exception Interop，或异常跨越协程暂停点；
- LINQ、`dynamic`、反射代码生成或 .NET BCL；
- 直接进程内机器码 JIT、OSR、PGO 或 Rollback Networking Protocol；
- Source、`.rsbc`、AOT ABI、SDK Metadata 和 Gameplay State 的长期冻结兼容性。

游戏引擎集成应固定到准确的 RealScript Release 或 Commit，并使用匹配 SDK 重新生成
AOT Artifact。

## License

RealScript 使用 Apache License 2.0，详见 [LICENSE](LICENSE)。

### Sprint 2 全栈架构演进要点
- **每实例独立堆池与纳秒时间片 GC 预算**：每个 `ManagedHeap` 独立持有槽位/缓冲区对象池与同步状态，常规堆分配与 GC 标记清扫无跨实例锁竞争（仅在进程共享 `ValuePool` 触发滞留回收时执行极短安全点扫描），并新增 `collectIncrementalWithBudget` 帧剩余纳秒/微秒预算控制。
- **扁平化生命周期与无损协程热重载**：`SceneScriptRuntime` 采用连续 `FlatLifecycleSlot` 热路径直调，热重载基于 64-bit 状态机指纹自动保留未变更的在途协程帧。
- **AST 卫生泛型、AOT 共享折叠与 `using`/`Resource`**：泛型特化保留精确源文件行列号与跨模块词法卫生性，引用类型泛型共享底层 AOT 实现；支持块级/声明级 `using` 确定性 `Dispose()` 与统一引用计数 `ResourceHandle<T>`。
- **分模块磁盘增量缓存与 Git 友好场景序列化**：`rsc`/`rsaot --cache-dir` 基于 `publicFingerprint` 隔离私有实现变更；引入 128-bit `AssetGuid` 与单行单属性有序场景/预制体文本格式。
