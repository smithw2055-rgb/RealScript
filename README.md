# RealScript

RealScript 是一门面向现代游戏引擎的嵌入式强类型脚本语言与运行时。

它采用接近 C# 的表达方式，以 C++17 游戏引擎为主要宿主，长期目标包括：快速字节码解释、C++17 AOT、可选 LLVM ORC JIT、源码级调试、热重载、沙箱 Mod，以及固定 Tick 的确定性模拟。

> 当前状态：Draft v0.1 规范基线、Phase 1A–1C 编译器前端、Phase 2A–2C 字节码运行时，以及 Phase 3A–3C 对象、数组、精确 GC、Native Handle 与堆诊断已经完成。语言、字节码、对象 ABI 和 GC 策略尚未冻结。

## 当前可运行能力

Phase 1A–3C 已经实现：

- 无外部依赖的 C++17/CMake 工程；
- `SourceText`、`TextSpan` 和 CRLF/LF 行列映射；
- 稳定诊断编号和错误累积；
- Lexer、注释、字符串和数值 Token；
- `module`、`import` 和 C# 风格函数解析；
- Pratt 表达式解析与右结合赋值；
- `if` / `else`、`while` 和可变局部变量；
- `void`、`bool`、`int`、`string` 基础类型；
- 确定赋值与全路径返回分析；
- 函数预声明、前向调用和直接递归所需的符号基础；
- 同名函数重载和基础转换排序；
- 同模块多文件聚合及跨模块 `import`；
- 与源码顺序无关的稳定 `SymbolId`；
- 多基本块 Typed MIR、显式局部槽位和块参数；
- `&&` / `||` 短路控制流；
- 直接调用、转换、类型、目标块、支配关系和终结指令 MIR 验证；
- 模块源码、公有签名和依赖指纹；
- 基于上一轮 `BuildSnapshot` 的模块级语义/MIR 复用；
- 类型化寄存器字节码、函数引用表和块参数；
- `.rsbc` 0.3 Section 容器、类型描述符、精确 Local/Register/Block TypeId、确定性编码和防御性解码；
- 字节码反汇编器与语义验证器；
- 类型化寄存器解释器、调用帧和跨模块调用；
- Checked Arithmetic Trap、运行时错误和脚本栈；
- 指令预算、递归预算与外部函数解析边界；
- 可复用 `ProgramImage`、`BindingRegistry`、Trace 和运行统计；
- `ObjectRef` 代际句柄、String/Array/Record 托管对象；
- 精确 Shadow Stack 根、持久根和增量 Mark/Sweep；
- 写屏障、GC 工作预算、回收统计和托管字符串字面量；
- 模块级 `class`、稳定 TypeId 和声明顺序字段布局；
- `new Type()`、字段读写、对象 Null Check 和身份相等；
- 对象 MIR/Bytecode、精确引用字段图和运行时签名 TypeId 校验；
- `T[]`、`new T[length]`、元素读写、`.length` 和数组身份相等；
- 数组 MIR/Bytecode、边界检查、精确元素 TypeId 与数组写屏障；
- `handle` 传递、Native Handle 注册表身份与代际安全和宿主 TypeId 校验；
- 跨堆 `ObjectRef` 隔离、RAII 持久根、Heap Snapshot、Retaining Path 和泄漏摘要；
- `rsc` Token、检查、MIR、Symbol、Bytecode 和二进制输出模式；
- Linux/Windows GitHub Actions 构建；
- 前端、控制流、模块、调用、增量编译和字节码测试。

实现边界见：

- [Phase 1A — C++17 Language Foundation](docs/roadmap/PHASE_1A.md)
- [Phase 1B — Control Flow and Multi-Block MIR](docs/roadmap/PHASE_1B.md)
- [Phase 1C — Calls, Modules and Incremental Compilation](docs/roadmap/PHASE_1C.md)
- [Phase 2A — Typed Register Bytecode](docs/roadmap/PHASE_2A.md)
- [Phase 2B — Bytecode Interpreter and Runtime Baseline](docs/roadmap/PHASE_2B.md)
- [Phase 2C — Linking, Observability and Embedding](docs/roadmap/PHASE_2C.md)
- [Phase 3A — Managed Heap and Precise GC Baseline](docs/roadmap/PHASE_3A.md)
- [Phase 3B — Language-visible Object Model and Fields](docs/roadmap/PHASE_3B.md)
- [Phase 3C — Arrays, Native Handles and Heap Diagnostics](docs/roadmap/PHASE_3C.md)

## 快速开始

### 构建

```bash
cmake -S . -B build \
  -DREALSCRIPT_BUILD_TESTS=ON \
  -DREALSCRIPT_WARNINGS_AS_ERRORS=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

### 多文件模块示例

`math.rs`：

```csharp
module Game.Math;

int twice(int value)
{
    return value * 2;
}

string choose(string value)
{
    return value;
}
```

`main.rs`：

```csharp
module Game.Main;
import Game.Math;

int main()
{
    return twice(21);
}
```

同时检查多个源文件：

```bash
rsc math.rs main.rs
```

打印并验证 Typed MIR：

```bash
rsc math.rs main.rs --mir
```

查看稳定函数符号：

```bash
rsc math.rs main.rs --symbols
```

输出示意：

```text
Game.Math::twice 0x...
Game.Math::choose 0x...
Game.Main::main 0x...
```

函数调用在 MIR 中通过稳定 `SymbolId` 表达：

```text
%argument:int = const.i32 21
%result:int = call @Game.Math::twice[0x...](%argument)
ret %result
```

生成并检查类型化字节码：

```bash
rsc math.rs --bytecode
rsc math.rs --emit-bytecode math.rsbc
rsc math.rsbc --disassemble
```

`.rsbc` 在执行前必须先通过物理解码检查和字节码语义验证。

执行无参数入口函数：

```bash
rsc game.rs --run Game.Main::main
```

Phase 2B 已加入类型化寄存器解释器、函数调用、分支参数、预算与结构化运行时错误。

## Phase 1C 的符号与增量模型

### 稳定函数身份

函数的首版规范键为：

```text
<module>::<name>(<parameter-types>)
```

返回类型不参与重载身份，因此不能只通过返回类型声明两个重载。`SymbolId` 由规范键稳定散列得到，不依赖源码顺序、内存地址或本次进程。

### 重载解析

Phase 1C 的转换排序刻意保持较小：

1. 精确类型匹配；
2. `null → string`；
3. 其他转换不可用。

这足以建立可验证的候选选择框架，同时避免在完整数值类型尚未实现时过早冻结提升规则。

### 模块增量复用

每个模块记录三类指纹：

- `sourceFingerprint`：模块全部源文件的路径和内容；
- `publicFingerprint`：排序后的函数公有签名；
- `dependencyFingerprint`：直接导入模块的公有指纹。

源码实现发生变化时，仅重建该模块；如果公有签名未变，依赖模块可以复用上一轮 MIR。公有签名变化时，直接依赖模块会失效并重新绑定。

当前增量层复用语义和 MIR 结果，Lexer/Parser 的语法树缓存将在后续增量前端切片中补充。

## Phase 2A 的字节码边界

字节码保留多基本块、类型化寄存器、显式局部槽位和边参数。MIR `ValueId` 直接映射到寄存器，调用通过带签名的函数引用表索引表达。

`.rsbc` 0.3 使用 little-endian 固定宽度整数和五个 Section：字符串、类型描述符、函数引用、函数元数据和代码。0.3 增加 array/handle 类型标签、Local/Register/Block TypeId 表及数组元素元数据。编码器不写入时间戳、裸地址或平台结构体填充，因此同一模块能够稳定地产生相同字节。

Decoder 负责文件边界、Section、计数和 Tag；Verifier 继续检查类型、定义支配、分支参数、调用和返回。两层都成功前不得进入执行器。

## 当前实现限制

尚未实现：

- `struct`、`interface`、`enum`；
- 构造函数、成员函数、继承、访问控制和命名导入；
- 完整数值类型、数值提升和用户定义转换；
- 泛型和运行时构造未知泛型实例；
- `break`、`continue`、`for`、`foreach`、`switch`；
- 异常和协程；
- 增量语法树与持久化构建缓存；
- 成员方法、构造函数、属性及相应调用模型；
- 异常表、协程状态、调试 GC Map 和原生 AOT。

未实现能力会得到明确诊断，不会使用占位运行时行为假装支持。

## 目标架构

```text
RealScript Source Files
        │
        ▼
Lexer / Parser / Syntax Trees
        │
        ▼
Compilation / Module Graph / Symbol Predeclaration
        │
        ▼
Binder / Overload Resolution / Flow Analysis
        │
        ▼
Verified Multi-Block Typed MIR
        │
        ├────────► Register Bytecode ─► Bytecode VM
        ├────────► Generated C++17 ───► Platform AOT Compiler
        └────────► LLVM IR ───────────► Optional ORC JIT

Shared Runtime ABI / Metadata / GC / Bindings / Debug Info
        │
        ▼
C++17 Game Engine
```

解释、AOT 和 JIT 必须共享类型系统、MIR、运行时 ABI 和差分测试。任何后端都不得直接绕过 MIR 解释 AST。

## 代码结构

```text
include/realscript/
  text/          SourceText 和 TextSpan
  diagnostics/   稳定诊断模型
  syntax/        Token、AST、Lexer 和 Parser
  semantic/      类型、Symbol、Bound Tree、Binder 和 Flow Analysis
  mir/           多基本块 Typed MIR、Lowerer 和 Verifier
  compiler/      多文件 Compilation、模块图和增量快照
  bytecode/      类型化寄存器字节码、Codec、Verifier 和 Disassembler
  runtime/       ProgramImage、Interpreter、ManagedHeap 和宿主嵌入
src/              对应实现
tools/rsc/        编译器命令行
tests/fixtures/   源码 conformance fixtures
tests/snapshots/  MIR snapshots
docs/spec/        Draft v0.1 规范
docs/roadmap/     实现切片和路线图
```

## 文档

- [总体架构设计](docs/ENGINE_DESIGN.md)
- [规范文档索引](docs/spec/README.md)
- [语言规范 Draft v0.1](docs/spec/LANGUAGE_SPEC.md)
- [Typed MIR 规范 Draft v0.1](docs/spec/MIR_SPEC.md)
- [运行时模型规范 Draft v0.1](docs/spec/RUNTIME_MODEL.md)
- [字节码与原生 ABI 规范 Draft v0.1](docs/spec/BYTECODE_AND_ABI.md)
- [`.rsbc` 0.3 物理格式](docs/spec/BYTECODE_FORMAT_V0.md)
- [Embedding and Observability Draft v0.1](docs/spec/EMBEDDING_AND_OBSERVABILITY_V0.md)
- [Managed Heap and GC Implemented Draft v0.1](docs/spec/MANAGED_HEAP_GC_V0.md)
- [Arrays, Native Handles and Heap Diagnostics Draft v0.1](docs/spec/ARRAYS_NATIVE_HANDLES_HEAP_DIAGNOSTICS_V0.md)
- [Phase 1A 实现说明](docs/roadmap/PHASE_1A.md)
- [Phase 1B 实现说明](docs/roadmap/PHASE_1B.md)
- [Phase 1C 实现说明](docs/roadmap/PHASE_1C.md)
- [Phase 2A 实现说明](docs/roadmap/PHASE_2A.md)
- [Phase 2B 实现说明](docs/roadmap/PHASE_2B.md)
- [Phase 2C 实现说明](docs/roadmap/PHASE_2C.md)
- [Phase 3A 实现说明](docs/roadmap/PHASE_3A.md)
- [Phase 3B 实现说明](docs/roadmap/PHASE_3B.md)
- [Phase 3C 实现说明](docs/roadmap/PHASE_3C.md)

## 路线图

- [x] 总体语言和运行时方向；
- [x] Draft v0.1 语言、MIR、运行时、字节码与 ABI 规范；
- [x] Phase 1A：C++17 工程、Lexer、Parser、Binder 和单块 MIR；
- [x] Phase 1B：赋值、控制流、流分析、多块 MIR、块参数和验证器；
- [x] Phase 1C：函数调用、重载、模块符号和增量编译；
- [x] Phase 2A：类型化寄存器字节码、编码器、反汇编器和验证器；
- [x] Phase 2B：解释器、调用栈、运行时错误和执行预算；
- [x] Phase 2C：链接镜像、宿主绑定、Trace 和嵌入门面；
- [x] Phase 3A：托管引用、精确根和增量 Mark/Sweep；
- [x] Phase 3B：class、对象字段、类型描述符和对象 Bytecode；
- [x] Phase 3C：数组、Native Handle、Heap Snapshot 和泄漏诊断；
- [ ] Phase 4：DAP、LSP 和热重载；
- [ ] Phase 5：C++17 AOT；
- [ ] Phase 6：确定性、性能优化和可选 JIT。

## 主要参考方向

RealScript 组合参考 AngelScript 的嵌入与调试接口、Luau 的 VM 性能工程、Unity IL2CPP 的 C++ AOT 路径、LLVM ORC 的可选 JIT，以及 DAP/LSP 的编辑器协议。

## License

许可证尚未确定。在许可证文件提交前，请勿假定本仓库代码或文档可用于再分发。


## Phase 2C 嵌入接口

Phase 2C 增加可复用的链接镜像、宿主绑定注册表和执行观察接口：

```cpp
auto image = runtime::ProgramImage::link(std::move(modules), error);
auto sharedImage = std::make_shared<runtime::ProgramImage>(std::move(*image));
auto bindings = std::make_shared<runtime::BindingRegistry>();
bindings->bind("Host::log", hostLogFunction);

runtime::EngineRuntime runtime(sharedImage);
runtime.setBindings(bindings);

runtime::ExecutionOptions options;
options.trace = [](const runtime::TraceEvent& event) {
    // 记录函数、指令、分支和外部调用
};
auto result = runtime.invoke("Game.Main::main", {}, options);
```

详细说明：

- [Phase 2C — Linking, Observability and Embedding](docs/roadmap/PHASE_2C.md)
- [Embedding and Observability Draft v0.1](docs/spec/EMBEDDING_AND_OBSERVABILITY_V0.md)


## Phase 3A 托管堆与精确 GC

Phase 3A 建立非移动托管堆，并将脚本字符串字面量迁移为代际 `ObjectRef`：

```cpp
runtime::EngineRuntime runtime(sharedImage);
auto heap = runtime.heap();

auto array = heap->allocateArray(4);
auto text = heap->allocateString("hello");
heap->arraySet(*array, 0, *text);

const auto root = heap->addPersistentRoot(*array);
heap->requestCollection();
// 解释器会在指令 safepoint 按 gcWorkBudget 分步执行 GC
heap->removePersistentRoot(root);
```

活动解释器帧通过 Shadow Stack 精确登记参数、Local 和 Register。Array/Record 写入执行写屏障；旧 `ObjectRef` 通过 generation 检查拒绝复用后的悬空访问。

详细说明：

- [Phase 3A — Managed Heap and Precise GC Baseline](docs/roadmap/PHASE_3A.md)
- [Managed Heap and GC Implemented Draft v0.1](docs/spec/MANAGED_HEAP_GC_V0.md)
- [Arrays, Native Handles and Heap Diagnostics Draft v0.1](docs/spec/ARRAYS_NATIVE_HANDLES_HEAP_DIAGNOSTICS_V0.md)


## Phase 3B 语言可见对象模型

Phase 3B 将 `class`、`new` 和字段访问贯穿源码、Typed MIR、`.rsbc` 与解释器：

```csharp
module Game.Objects;

class Point
{
    int x;
    int y;
}

int main()
{
    Point point = new Point();
    point.x = 40;
    point.y = 2;
    return point.x + point.y;
}
```

每个类拥有由规范名称生成的稳定 TypeId。`.rsbc` 0.3 保存有序字段布局和精确对象签名；解释器在函数入口、返回、Null Check 和字段访问处验证运行时 TypeId。GC 只扫描描述符中的 `string` 与 class 引用字段。

详细说明：

- [Phase 3B — Language-visible Object Model and Fields](docs/roadmap/PHASE_3B.md)
- [Phase 3C — Arrays, Native Handles and Heap Diagnostics](docs/roadmap/PHASE_3C.md)
- [Object Model Implemented Draft v0.1](docs/spec/OBJECT_MODEL_V0.md)

## Phase 3C 数组、Native Handle 与堆诊断

Phase 3C 将固定长度数组贯穿源码、Typed MIR、`.rsbc` 0.3、验证器、解释器和精确 GC：

```csharp
module Game.Arrays;

class Item { int value; }

int main()
{
    Item[] items = new Item[2];
    Item item = new Item();
    item.value = 42;
    items[0] = item;
    return items[0].value + items.length;
}
```

数组保存精确 Array TypeId 和元素类型信息；null、负长度和越界访问产生结构化运行时错误。class/array 元素写入执行类型验证和 GC 写屏障。

宿主资源通过 `NativeHandleRegistry` 暴露为代际安全、带 TypeId 的 opaque `handle`，不会把裸 C++ 指针放入脚本值。`ObjectRef` 同时携带 HeapId，防止跨 Runtime/Heap 误用。

`PersistentRoot`、`HeapSnapshot`、`retainingPath()` 和 `leakSummary()` 为引擎嵌入、压力测试和关闭期泄漏检查提供可观测边界。

详细说明：

- [Phase 3C — Arrays, Native Handles and Heap Diagnostics](docs/roadmap/PHASE_3C.md)
- [Arrays, Native Handles and Heap Diagnostics — Implemented Draft v0.1](docs/spec/ARRAYS_NATIVE_HANDLES_HEAP_DIAGNOSTICS_V0.md)
