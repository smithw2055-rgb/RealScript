# Phase 11–17 — 游戏语言扩展 Profile

## 状态

已实现为现有编译器之前的确定性两阶段语言展开层。扩展源码在进入 Parser 之前完成规范化，随后继续使用统一的 Binder、Typed MIR、Bytecode、Interpreter、C++17 AOT 与可选 JIT 后端。

该阶段的目标是形成适合游戏玩法脚本的受限 C# 风格 Profile，而不是兼容 CLR 或完整 C#。

## 编译模型

`Compilation` 对全部源码执行：

1. 按模块收集 delegate、interface、generic、Attribute 和 `ref/out/in` 签名；
2. 按稳定的模块名和文件路径顺序执行语句 Lowering 与特化代码生成。

声明默认只在所属 `module` 内可见；显式 `import` 后，当前模块可以使用被导入模块的语言扩展声明。同一泛型特化与引用包装类型只在声明模块生成一次。

未使用扩展语法的普通 Phase 1–10 源码保持原文本，不改变已有源码指纹、增量缓存和调试坐标。

## Phase 11：结构化控制流

支持：

- `for`；
- 数组与固定容量集合的 `foreach`；
- `do/while`；
- `break`、`continue`；
- 基于相等比较的 `switch/case/default`；
- 嵌套 loop/switch 的确定性控制流。

限制：无 fallthrough、pattern matching、case guard 和通用 Enumerator 协议。

## Phase 12：Delegate、Lambda 与 Event

支持：

- delegate 声明作为事件签名；
- 类内 event；
- 方法组订阅/取消订阅；
- `(T x) => ...` 与 `x => ...`；
- 按稳定订阅顺序派发。

限制：delegate 不是一等运行时对象；Lambda 只支持参数、`this` 和字段，不支持捕获任意局部变量的堆闭包。

## Phase 13：Interface Contract

支持：

- interface 声明；
- class/struct 实现列表；
- 按方法名和参数数量进行编译期一致性检查；
- 实现关系写入语言元数据。

限制：没有 interface 类型变量、运行时接口分派、继承、virtual/abstract 和默认接口实现。

## Phase 14：源码 Attribute

支持：

- `[Serializable]`；
- `[Replicated(channel = "state")]`；
- 位置参数与命名参数；
- `Module::Target` 稳定目标名；
- `Compilation`、`GameCompileResult` 与 `GameProgram` 持续保存元数据。

限制：不实例化 Attribute 类；元数据暂未写入 `.rsbc` 物理格式。

## Phase 15：泛型与集合

支持：

- 显式泛型类型和泛型函数实例化；
- 按模块执行确定性 monomorphization；
- 跨文件共享特化；
- 显式 import 的跨模块泛型使用；
- 固定容量 `List<T>`、`Queue<T>`、`Stack<T>`、`HashSet<T>`、`Dictionary<K,V>` 和 `Optional<T>`；
- 集合 `foreach`。

限制：不支持泛型推断、开放泛型运行时对象、约束、协变/逆变和自动扩容集合。

## Phase 16：确定性 Sequence

支持：

```csharp
sequence Attack(long target)
{
    Windup();
    yield wait_ticks(12);
    Fire();
}
```

Sequence Lower 到普通实例方法和 `GameplayHost`/`TickScheduler` 任务，因此能够参与固定 Tick、存档、回放和 Rollback。

限制：仅支持 `yield wait_ticks`；跨 yield 的状态必须放在对象字段中，不保存任意局部变量；不支持 `Task`、线程和通用 `async/await`。

## Phase 17：引用参数和值类型别名

支持：

- 受限独立函数调用中的 `ref/out/in`；
- 声明模块拥有的稳定包装类型；
- 调用完成后的写回；
- `out` 默认初始化；
- `in` 写入诊断；
- `byte/sbyte/short/ushort/uint/ulong/float/char` 源码别名。

限制：无 ref local/ref return/ref field/ref indexer；别名映射到现有 `int/long/double` 载体，不形成新的精确 ABI 类型；无 boxing 和 nullable value。

## 验证

覆盖：

- 单文件完整 Profile 执行；
- 同模块跨文件声明共享；
- 跨模块 import 与同名声明隔离；
- 嵌套 switch/loop；
- 集合 foreach；
- Lambda/Event；
- `ref/out/in` 写回；
- 确定性 Sequence；
- 扩展源码生成 C++17 AOT；
- Game SDK 元数据保留；
- Ubuntu 与 Windows warnings-as-errors 全量 CTest。

## 后续原生编译器工作

仍未实现：

- 继承和运行时接口/虚分派；
- 一等 delegate 和完整 closure；
- 开放泛型、约束和动态集合标准库；
- Pattern Matching；
- 保存任意局部变量的原生协程状态机；
- 精确 `uint/ulong/float/char` 类型身份；
- 完整引用生命周期、异常、Nullable 与 Boxing；
- 展开代码的精确源码映射与 Attribute 的 `.rsbc` 序列化。
