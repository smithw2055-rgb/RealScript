# C# 风格特性兼容矩阵

[中文文档入口](README.md) | [English](../en/CSHARP_COMPATIBILITY_MATRIX.md)

RealScript 采用 C# 风格语法，但不是 CLR、ECMA C# 或 .NET 标准库实现。下表中的“支持”表示该能力已进入原生编译流水线，并由适用的 RealScript 后端共同理解；“部分支持”表示有意采用了更小、可验证、确定性的语义范围。

## 语言与类型系统

| 能力 | 状态 | RealScript 语义边界 |
|---|---|---|
| 模块、导入、多文件 | 支持 | 使用显式 `module`/`import`；不是 C# Assembly/Namespace 解析。 |
| 函数与重载 | 支持 | 前向调用、递归、确定性转换排序、可选/命名/`params` 参数。 |
| Class 继承 | 支持 | 单继承、基类构造与 `base`、`abstract`/`virtual`/`override`/`sealed`。 |
| Interface | 支持 | Interface 类型值、实现契约和确定性运行时分派；无默认 Interface 实现。 |
| 访问控制 | 支持 | 已支持声明上的 `public`、`internal`、`protected`、`private`。 |
| Delegate 与 Closure | 支持 | 一等委托、方法组、堆闭包、共享可变捕获、多播和通用 Event 存储。 |
| 泛型 | 支持 | 编译期特化、推断、泛型成员、`class`/`struct`/`new()` 与多接口复合 `where` 约束、泛型 Interface/Delegate；无开放运行时泛型和协变/逆变。 |
| 集合与 `foreach` | 支持 | 可增长且确定性的 `List`、`Dictionary`、`HashSet`、`Queue`、`Stack`，具备值类型 `struct` 零分配 `Enumerator`、索引器读写（`list[i]`/`dict[k]`）、`Count`/`Capacity`/`IsEmpty` 属性及 $O(1)$ Swap-and-Pop 桶重链 `Remove`；不是 .NET 集合库或 LINQ。 |
| Struct | 支持 | 值复制、可变 receiver、隐式/ref `this`、嵌套托管引用和结构相等。 |
| 精确基础值类型 | 支持 | 精确有符号/无符号整数宽度、binary32 `float`、binary64 `double`、`char`，以及 checked/unchecked 转换。 |
| Nullable 值类型 | 支持 | 精确 `T?`、空条件提升结果、`HasValue`、`Value`、`GetValueOrDefault`。 |
| Boxing/Unboxing | 支持 | 保留脚本值类型精确身份，并纳入托管堆与 GC。 |
| 引用位置 | 部分支持 | ref 参数、局部、返回、字段、Indexer 与 ref `this`；无 unsafe 指针、ref property、ref struct 和完整逃逸分析。 |
| Attribute | 部分支持 | 原生语法及编译器/Game SDK/产物元数据，自动导入 `ScriptMetadataRegistry` 与 `InspectorTypeSchema`；无可执行 Attribute Class 和完整 `AttributeUsage`。 |
| 用户运算符/转换 | 支持 | 支持自定义 `operator` 重载（`+`、`-`、`*`、`/`、`%`、`==`、`!=`、`<`、`<=`、`>`、`>=`、一元 `-`、`!`、`~`）、复合赋值（`+=`、`-=`、`*=`、`/=`、`%=`）与自定义隐式/显式类型转换运算符（`public static implicit/explicit operator Target(Source)`）。 |
| `const`、`readonly`、静态成员与属性初始化器 | 支持 | 支持编译期 `const`、构造函数内赋值的 `readonly` 字段、`static class`、确定性静态构造函数（`static T()`）、`static` 字段及初始化器，以及跨解释器、字节码与 C++17 AOT 的实例/静态自动属性初始化器（`{ get; set; } = expr;`）。 |

## 表达式与控制流

| 能力 | 状态 | RealScript 语义边界 |
|---|---|---|
| 结构化语句 | 支持 | `if`、`while`、`for`、`foreach`、`do/while`、`break`、`continue`、switch 语句。 |
| 推断与便利语法 | 支持 | `var`、`?:`、`??`、`?.`、`$"..."` 字符串插值、对象/Struct/集合初始化器。 |
| 运行时类型运算 | 支持 | `is`、`as`、`typeof` 和精确运行时可赋值规则。 |
| Pattern | 支持 | 常量、null、类型、discard、模式变量、`when` guard 和穷尽 Switch Expression。 |
| 其他模式族 | 部分支持 | 无关系、属性、位置、列表、递归、`and`/`or`/`not` 和 Range Pattern。 |
| 双轨协程：`sequence`（确定性回滚轨） | 支持 | 专司确定性帧模拟与帧级回滚快照（`yield wait_ticks`、`wait_seconds`、`wait_event`、`wait_signal`、`wait_until`），编译为显式字段状态机并完整纳入 `RSGS` 快照/Rollback，配合 `RealScript.Fiber` 协作式调度。 |
| 双轨协程：`async`/`await`（流式与网络 I/O 轨） | 支持受限模型 | Phase 25–28 引入 `RealScript.Async`（`Awaitable`/`Awaitable<T>`、8B 代际 `AwaitableHandle`、`CancellationToken`/`CancellationTokenSource` 及 `PreTickIo`/`FixedTick`/`Update`/`EndOfFrame` 四阶段主线程恢复队列），专司异步 `OnStart()`、预制体/资产流式加载、存档读写与 HTTP/WebSocket 网络 I/O（随实体销毁自动取消）；不支持裸 OS 线程共享可变状态或 CLR `System.Threading.Tasks.Task`。 |

## 异常与清理

| 能力 | 状态 | RealScript 语义边界 |
|---|---|---|
| `throw`、`try`、`catch`、`finally` | 支持 | 非空脚本 Class 对象、有序 typed/catch-all 匹配、rethrow、跨调用传播。 |
| 控制流清理 | 支持 | `finally` 覆盖正常完成、脚本异常、`return`、`break` 和 `continue`。 |
| `using` 资源确定性释放（`IDisposable` / `ResourceHandle<T>`） | 支持 | `using (...)` 代码块与 `using var` 声明编译期降级为确定性 LIFO `try`/`finally` `Dispose()`，支持 `class`、`interface` 与 `struct` 及宿主引用计数 `ResourceRegistry` 资源桥接（`RealScript.Resource`）。 |
| 运行时 Fault | Host 结构化错误 | 溢出、非法字节码、预算、null/越界和 Host 错误不是可 catch 的脚本异常对象。 |
| Filter 与资源语句 | 部分支持 | 已支持 `using`；无 catch filter、`lock` 和平台/原生异常互操作。 |
| 异常跨 Sequence Yield | 不支持 | Sequence 状态可快照；未把进行中的异常展开序列化到暂停点。 |

## 运行时、后端与工具

| 能力 | 状态 | RealScript 语义边界 |
|---|---|---|
| Interpreter | 支持 | 验证过的类型化寄存器 Bytecode、统一扁平栈帧布局（Unified Flat Frame）、Computed-goto 解释器派发、增量 GC 步进、预算、确定性 Trace 与结构化错误。 |
| C++17 AOT | 支持 | 生成代码与解释器共享 MIR 语义，包括委托、值类型、Pattern 和异常，并支持零预算 Trusted Release（`--trusted-release`）、`#line` 源码行号映射指令、结构体栈标量化与 AVX2 向量化 ECS 快循环。 |
| Toolchain JIT | 平台相关 | 配置后使用外部 C++ 工具链；不是 CLR 风格进程内机器码 JIT。 |
| `.rsbc` | Draft、带版本 | 当前输出 0.9，Decoder 支持 0.6–0.9；格式尚未冻结。 |
| LSP/DAP/Hot Reload | 支持受限模型 | 仅识别 `.rscript` / `.d.rscript` 后缀；支持原始源码符号与 Sequence Point、`ParseSnapshot` 与语法子图增量重构缓存、成员 `.` 补全、`signatureHelp`、`.d.rscript` 宿主声明导出、标准流及内嵌 TCP（`--dap-port` / `EmbeddedDapServer`）DAP 调试、Watch 面板动态 `variablesReference` 对象/结构体/容器树状展开、C++17 AOT `.natvis`/LLDB 调试视图、纯自研单文件 C++ 声明解析器 `rsbind`（内置 `#define`/`#if` 宏求值），以及带状态结构化热重载。 |
| Reflection 与 `dynamic` | 不支持 | 有稳定 TypeId，但无 CLR Reflection、`dynamic`、运行时代码生成和运行时泛型构造。 |
| 标准库 | 专用模型 | 提供确定性游戏/运行时基础能力，不是 .NET BCL。 |

## 准确定位

Phase24 完成后的准确描述是：

> 一门原生、强类型、确定性的 C# 风格游戏脚本语言，具备运行时多态、闭包、编译期泛型、协程状态机、已实现范围内完整的值/引用语义、常用 Pattern/便利语法和结构化脚本异常；但它不是完整 C# 或 .NET 生态的 CLR 兼容实现。
