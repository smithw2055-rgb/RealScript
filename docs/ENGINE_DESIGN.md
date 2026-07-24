# RealScript 游戏脚本引擎总体设计

- 文档状态：Draft v0.1
- 目标宿主：C++17 游戏引擎
- 主要场景：游戏逻辑、UI 行为、任务系统、关卡脚本、AI、Mod、确定性模拟
- 主要平台：Windows、Linux、macOS，后续扩展移动端和受限 AOT 平台

## 1. 摘要

RealScript 是一门具有 C# 表达习惯、面向游戏领域约束和优化的独立脚本语言。

系统采用统一前端和多后端架构：

1. 源码经过解析、绑定和类型检查，生成 Typed HIR；
2. Typed HIR 降级为 SSA-like Typed MIR；
3. 编辑和调试模式将 MIR 编译为类型化寄存器字节码；
4. 发布模式将 MIR 生成 C++17，再由平台编译器完成 AOT 原生编译；
5. 桌面开发环境可以在后期增加 LLVM ORC JIT；
6. 所有后端共享同一套类型系统、运行时 ABI、对象模型、元数据和调试信息。

RealScript 的目标不是实现完整 C# 或替代 C++，而是覆盖游戏逻辑中最常见、最需要生产效率和安全性的部分，同时保留接近原生代码的发布性能。

## 2. 设计目标

### 2.1 功能目标

- C# 风格的现代强类型语法；
- 类、值类型、接口、枚举、属性、泛型、委托和协程；
- 快速增量编译；
- 字节码解释执行；
- C++17 AOT 编译；
- 可选 JIT；
- 源码级调试；
- 热重载和状态迁移；
- C++ 类型和函数自动绑定；
- 统一反射、序列化、编辑器和调试元数据；
- 确定性脚本模块；
- Mod 沙箱、能力控制和执行预算。

### 2.2 性能目标

- 解释器适合绝大多数游戏编排、状态机、任务和 UI 行为；
- AOT 适合计算密集逻辑和发布版本；
- 脚本到 C++ 的高频调用不经过通用 Variant 装箱；
- 值类型默认不产生 GC 分配；
- GC 可按帧预算增量工作；
- 热重载不要求重启游戏进程；
- 调试功能关闭后不应显著影响发布性能。

### 2.3 工程目标

- 核心运行时使用 C++17；
- 不依赖平台私有 C++ ABI；
- 前端、IR、VM、AOT、工具链模块边界清晰；
- 支持独立编译器命令行和嵌入式编译服务；
- 支持单元测试、语义一致性测试、差分测试和基准测试；
- 从首版开始考虑版本化、存档兼容和网络协议兼容。

## 3. 非目标

首个稳定版本不计划支持：

- 完整 C# 语言兼容；
- CLR、CTS、IL 或 .NET 程序集兼容；
- `dynamic`；
- 任意运行时代码生成；
- 表达式树；
- 完整 LINQ；
- 用户可见的裸指针和任意 `unsafe`；
- C++ 多继承模型；
- 默认接口方法；
- 脚本直接创建操作系统线程；
- 脚本终结器管理 GPU、文件和引擎资源；
- 跨 C++/脚本边界传播原生 C++ 异常。

## 4. 参考项目与取舍

| 项目 | 重点参考 | RealScript 的不同点 |
|---|---|---|
| AngelScript | 强类型嵌入、原生绑定、字节码、反射和调试接口 | 采用更明显的 C# 风格语法、统一 MIR 和 C++17 AOT 后端 |
| Luau | 高性能字节码、增量 GC、类型分析、快速原生调用和可选原生代码生成 | RealScript 使用静态强类型和值类型对象模型，而非 Lua 数据模型 |
| Unity IL2CPP | 中间代码生成 C++ 后再交给平台编译器 | RealScript 同时保留可嵌入字节码 VM 和热重载路径 |
| Wren | 小型 VM、模块、Fiber 和易嵌入设计 | RealScript 使用类型化寄存器字节码，并提供 AOT、强类型和完整工具链 |
| GDScript | 与编辑器、场景和游戏对象模型深度整合 | RealScript 更强调 C++17 宿主、独立运行时和发布期原生化 |
| CoreCLR/.NET | 成熟的 C# 语义、GC、JIT、诊断和宿主接口 | RealScript 控制语言规模、运行时体积、平台限制和热脚本模型 |
| LLVM ORC | 模块化 JIT、惰性编译和原生代码链接 | 只作为可选后端，不成为语言运行时基础依赖 |
| DAP/LSP | 编辑器无关的调试和语言服务协议 | RealScript 将协议适配建立在统一语义模型和调试信息上 |

建议采用“组合参考”而不是选择一个项目直接修改：

- AngelScript：宿主 API、绑定和调试器；
- Luau：VM 性能工程；
- IL2CPP：AOT 发布路线；
- LLVM ORC：可选桌面 JIT；
- DAP/LSP：工具协议；
- 自定义 Typed MIR：统一全部执行模式。

## 5. 总体架构

```text
                         ┌─────────────────────┐
                         │  RealScript Source  │
                         └──────────┬──────────┘
                                    │
                         Lexer / Parser / CST
                                    │
                      Binder / Resolver / Type Checker
                                    │
                              Typed HIR
                                    │
                           SSA-like Typed MIR
                   ┌────────────────┼────────────────┐
                   │                │                │
           Bytecode Backend     C++17 Backend    LLVM Backend
                   │                │                │
          Register Bytecode    Generated C++     LLVM IR
                   │                │                │
            Bytecode VM        Native AOT       Optional JIT
                   └────────────────┼────────────────┘
                                    │
                         Unified Runtime ABI
                                    │
       ┌──────────┬──────────┬──────┴──────┬───────────┬─────────┐
       │ Metadata │ Bindings │ Object/GC   │ Debugging │ Profiler│
       └──────────┴──────────┴─────────────┴───────────┴─────────┘
                                    │
                             C++17 Game Engine
```

### 5.1 架构约束

- 解释器和 AOT 后端不得各自直接解释 AST；
- 语言语义必须在类型系统、MIR 和 Runtime Intrinsic 层定义；
- 字节码 VM 和原生代码调用相同 Runtime ABI；
- 字节码、AOT 和 JIT 需要通过同一套差分测试；
- 反射、绑定、调试和序列化共享 Descriptor；
- 所有外部字节码必须经过验证器；
- 不可信模块只能通过声明的 Capability 调用宿主服务。

## 6. 编译管线

### 6.1 源文件与模块

建议扩展名：

```text
.rs
.rsi     # 可选接口/签名文件
.rsm     # 编译模块包
.rsbc    # 字节码包
.rsmeta  # 可选独立元数据
```

初期只需要 `.rs` 和内部模块缓存格式，避免过早冻结公共二进制格式。

模块示例：

```csharp
module Game.Defense;

import Engine.Math;
import Engine.ECS;
import Game.Shared;
```

模块是以下能力的边界：

- 名称空间和可见性；
- 增量编译；
- 热重载；
- 能力授权；
- AOT 编译单元；
- 元数据版本；
- 存档和网络 Schema 管理。

### 6.2 Lexer 与 Parser

推荐使用手写 Lexer 和递归下降/Pratt Parser：

- 易于生成高质量错误信息；
- 易于处理 C# 风格泛型、Lambda 和表达式优先级；
- 可支持错误恢复；
- 可构建不可变 Green Tree + 带位置的 Red Tree；
- 可服务增量编译和 LSP。

语法树层不负责类型判断，保留不完整和错误节点，使 IDE 在代码尚未完成时仍能提供服务。

### 6.3 Binder 与类型检查

Binder 负责：

- 模块导入；
- 标识符解析；
- 重载候选集；
- 可见性；
- 泛型参数；
- 属性解析；
- 局部作用域；
- 捕获变量分析。

Type Checker 负责：

- 隐式和显式转换；
- 数值提升；
- 空值分析；
- `ref`/`in`/`out` 规则；
- 值类型初始化；
- 泛型约束；
- 接口实现检查；
- 确定性模块 API 检查；
- 异步函数返回类型检查。

### 6.4 Typed HIR

HIR 保留较高级语言结构：

- `foreach`；
- 属性访问；
- 模式匹配；
- Lambda；
- `async/await`；
- 自动属性；
- 插值字符串；
- 空值传播。

HIR 节点已经完成类型绑定，不再保存未解析名称。

### 6.5 Typed MIR

MIR 是所有后端的共同输入，建议具有：

- 显式基本块；
- 显式控制流；
- SSA 值或接近 SSA 的虚拟寄存器；
- 显式加载和存储；
- 显式空值检查；
- 显式边界检查；
- 显式装箱和拆箱；
- 显式虚调用和接口调用；
- 异步状态机降级；
- 异常处理区域；
- 源码序列点。

示例：

```text
bb0:
  %0 = arg.entity
  %1 = call @Entity.IsValid(%0)
  br_false %1, bb2
  br bb1

bb1:
  %2 = call @Entity.Get<Transform>(%0)
  %3 = load_field %2, Transform.position
  ret %3

bb2:
  throw @InvalidEntityException
```

MIR 优化包含：

- 常量折叠；
- 不可达块删除；
- Copy Propagation；
- 局部公共子表达式消除；
- 简单内联；
- 去虚化；
- 泛型单态化；
- 边界检查消除；
- 逃逸分析；
- 装箱消除；
- 只读字段传播。

第一阶段应保持优化器规模可控，优先保证语义和调试信息正确。

## 7. 语言设计

### 7.1 基础类型

建议首版内建：

```text
bool
byte, sbyte
short, ushort
int, uint
long, ulong
float, double
char
string
object
void
```

游戏扩展类型如 `fixed32`、`fixed64`、`half` 可在后续加入。

所有整数运算需要定义：

- Debug 模式是否默认溢出检查；
- `checked`/`unchecked` 语义；
- 除零行为；
- 移位计数规则；
- 有符号和无符号转换规则。

语言不能把这些行为交给 C++ 未定义行为。

### 7.2 值类型 `struct`

`struct` 是真正的值语义：

- 赋值默认复制；
- 参数可按值、`in`、`ref`、`out` 传递；
- 默认构造产生零初始化或语言定义的默认值；
- 默认不单独进入 GC 堆；
- 可以包含 GC 引用字段，但需要 GC 描述图；
- 不允许无限递归布局；
- 可通过属性控制序列化和网络布局，但不能任意破坏运行时安全。

示例：

```csharp
[Component]
struct Health
{
    float current;
    float maximum;

    bool IsAlive => current > 0.0f;
}
```

### 7.3 引用类型 `class`

`class` 由脚本 GC 管理：

- 单继承；
- 多接口；
- 可声明 `sealed`；
- 字段默认初始化；
- 方法默认非虚，显式 `virtual`/`override`；
- 不允许用户定义终结器；
- 引擎资源使用句柄或显式租约，不由 GC 直接销毁。

默认非虚有利于：

- 去虚化；
- AOT 内联；
- 更清晰的对象契约；
- 减少高频逻辑的间接调用。

### 7.4 接口

接口仅描述契约：

```csharp
interface IGameSystem
{
    void Tick(in TickContext context);
}
```

首版限制：

- 不保存字段；
- 不支持默认实现；
- 不支持接口静态抽象成员；
- 不参与对象布局；
- 运行时使用紧凑接口槽表；
- 编译期已知具体类型时允许去虚化。

接口不应成为每帧数十万次细粒度对象调用的主要机制。高频系统应优先使用批处理、值类型和静态分派。

### 7.5 枚举

```csharp
enum TargetPriority : byte
{
    Nearest,
    Strongest,
    Weakest
}
```

枚举具有确定的底层整数类型，不隐式转换为整数，位标志需显式 `[Flags]`。

### 7.6 泛型

首版建议以编译期泛型为主：

- 值类型实例全部单态化；
- 引用类型可选择共享实现或单态化；
- 泛型约束在编译期检查；
- AOT 输出记录实例化集合；
- 不支持运行时构造未知泛型类型。

示例：

```csharp
struct Optional<T>
{
    bool hasValue;
    T value;
}
```

需要限制代码膨胀，并提供实例化统计工具。

### 7.7 可空类型

- 引用类型默认不可空；
- `Entity?`、`Actor?` 显式表示可空；
- 编译器进行流敏感空值分析；
- 值类型使用 `T?` 或 `Optional<T>`；
- 引擎句柄的失效和语言 `null` 必须是两个概念。

### 7.8 委托与 Lambda

委托是类型化函数引用：

- 非捕获 Lambda 不分配；
- 捕获 Lambda 生成闭包对象；
- 对小闭包进行逃逸分析；
- 事件订阅使用弱句柄或显式生命周期 Token，避免长期引用泄漏。

### 7.9 异步与协程

`async GameTask` 被编译为状态机：

```csharp
async GameTask RunWave()
{
    await Time.DelayTicks(30);
    SpawnWave();
}
```

游戏协程不同于操作系统线程：

- 由引擎 Scheduler 驱动；
- 等待对象必须是引擎定义的 Awaitable；
- 支持等待 Tick、事件、资源、动画和任务；
- 确定性模块只能等待确定性事件；
- 取消使用 `CancellationToken`；
- 调试器显示逻辑异步栈。

## 8. 字节码设计

### 8.1 选择寄存器式 VM

推荐类型化寄存器字节码，而不是直接执行 AST 或纯栈式字节码。

原因：

- 降低指令分派次数；
- 与 MIR 的虚拟寄存器模型接近；
- 更容易生成变量位置信息；
- 更容易进行局部优化；
- 更容易映射到 AOT/JIT；
- 可减少临时值在 VM 栈上的移动。

### 8.2 类型化指令

示例：

```text
LOAD_CONST_I32   r0, 10
LOAD_FIELD_F32   r1, r_this, field.current
ADD_F32          r2, r1, r_damage
STORE_FIELD_F32  r_this, field.current, r2
CALL_NATIVE      r3, native.EntityIsValid, r_entity
BR_FALSE         r3, label.invalid
RETURN_VOID
```

建议指令族：

- 常量和移动；
- 算术和位运算；
- 比较；
- 分支和 Switch；
- 字段和数组访问；
- 值类型复制；
- 对象创建；
- 静态、实例、虚和接口调用；
- Native Thunk 调用；
- 装箱和拆箱；
- 异常；
- 协程挂起和恢复；
- 调试 Safepoint；
- Runtime Intrinsic。

### 8.3 编码格式

初期建议使用易调试的固定头 + 可变操作数格式，而不是立刻追求极致压缩。

```cpp
struct InstructionHeader
{
    uint16_t opcode;
    uint8_t operandCount;
    uint8_t flags;
};
```

稳定后可以引入：

- 高频短指令；
- 紧凑寄存器索引；
- 常量池索引压缩；
- Super Instructions；
- Direct Threading 或 Computed Goto（平台允许时）；
- 字节码布局 PGO。

### 8.4 字节码验证器

加载前验证：

- 指令边界；
- Opcode 合法性；
- 寄存器范围和类型；
- 跳转目标；
- 基本块入口；
- 局部值初始化；
- 参数和返回值；
- 字段访问权限；
- 泛型实例；
- 异常区域嵌套；
- 协程状态；
- Capability；
- 最大寄存器、栈深、常量和代码长度。

验证后，解释器热路径无需重复执行大量结构性检查。

### 8.5 调用帧

```cpp
struct VmFrame
{
    FunctionEntry* function;
    uint32_t instructionOffset;
    ValueSlot* registers;
    VmFrame* caller;
    ExceptionRegionState exceptionState;
};
```

`ValueSlot` 应针对 64 位平台优化，并允许大值类型使用栈帧内独立存储区。

## 9. C++17 AOT 后端

### 9.1 管线

```text
Typed MIR
  → MIR optimization
  → C++17 internal source
  → MSVC / Clang / GCC / platform compiler
  → object/library
  → game executable or script module
```

### 9.2 生成代码原则

生成代码是运行时内部实现，不追求人类手写风格，也不暴露为公共 API。

```cpp
void RS_Game_Defense_TurretSystem_Tick(
    rs::ObjectHandle self,
    const rs::TickContext* context,
    rs::ExecutionContext* execution)
{
    // Generated, checked operations.
}
```

AOT 后端需要显式处理：

- 整数溢出；
- 除零；
- 空引用；
- 边界检查；
- 求值顺序；
- 异常；
- GC Safepoint；
- Write Barrier；
- 接口调用；
- 泛型布局；
- 源码调试映射。

### 9.3 稳定 Runtime C ABI

生成代码不直接调用不稳定的引擎 C++ 虚表：

```cpp
struct RealScriptRuntimeApi
{
    uint32_t abiVersion;

    void* (*resolveComponentWrite)(EntityHandle, TypeId);
    const void* (*resolveComponentRead)(EntityHandle, TypeId);

    ObjectHandle (*allocateObject)(ExecutionContext*, TypeId);
    void (*writeBarrier)(ObjectHandle owner, ObjectHandle value);

    void (*throwException)(ExecutionContext*, ExceptionId);
    StringHandle (*concatString)(StringHandle, StringHandle);
};
```

收益：

- 隔离 MSVC/Clang/GCC ABI 差异；
- 引擎内部可以重构；
- 便于动态模块加载；
- 便于 Capability 控制；
- 便于版本检查；
- 便于测试替身和独立运行时。

### 9.4 模块链接

建议每个 AOT 模块导出：

```cpp
extern "C" const RealScriptModuleDescriptor*
RealScript_LoadModule(uint32_t requestedAbiVersion);
```

模块描述包含：

- 模块 GUID；
- Schema 版本；
- Runtime ABI 版本；
- 类型和函数表；
- GC 描述；
- 调试信息；
- Capability 声明；
- 初始化和卸载函数。

## 10. 可选 LLVM ORC JIT

LLVM ORC 适合桌面开发环境：

- 热点函数 JIT；
- 按需编译；
- 快速替换函数入口；
- Profile Guided Recompilation；
- 高性能开发运行。

不建议在第一阶段依赖 LLVM：

- 构建体积和依赖显著增加；
- 调试信息和符号管理复杂；
- 主机和移动平台仍需 AOT；
- C++17 AOT 已经是完整发布后端；
- 过早引入会分散语言和 VM 基础建设。

建议 LLVM 后端只消费稳定 MIR，不影响前端、字节码和 Runtime ABI。

## 11. 统一函数入口与混合执行

```cpp
enum class BackendKind : uint8_t
{
    Bytecode,
    Aot,
    Jit,
    Native
};

struct FunctionEntry
{
    FunctionId id;
    BackendKind backend;
    uint32_t version;
    void* entryPoint;
    const BytecodeFunction* bytecode;
    const FunctionDebugInfo* debugInfo;
};
```

所有调用通过稳定入口或调用桩：

- 编辑中的函数可以是字节码；
- 核心库可以是 AOT；
- 热点函数可以切换到 JIT；
- Native API 使用生成 Thunk；
- 热重载时只原子替换 `FunctionEntry`；
- 活跃旧调用退出后再回收旧版本代码。

禁止业务代码永久缓存裸原生函数地址。

## 12. C++ 绑定

### 12.1 绑定目标

- C++ 类型可暴露给脚本；
- 脚本类型可由引擎查询；
- 高频调用使用静态生成 Thunk；
- 编辑器仍可动态枚举成员；
- 所有绑定可生成文档、LSP 符号和调试视图；
- 绑定签名可进行 ABI 校验。

### 12.2 描述符优先

```cpp
struct NativeMethodDescriptor
{
    MethodId id;
    StringId name;
    TypeId ownerType;
    TypeId returnType;
    Span<const ParameterDescriptor> parameters;
    NativeThunk thunk;
    MethodFlags flags;
};
```

开发者通过普通 C++ 描述 API、模板注册器或外部代码生成工具声明类型，不依赖大规模宏展开。

示意：

```cpp
registry.type<Transform>("Engine.Transform")
    .valueType()
    .field("position", &Transform::position)
    .field("rotation", &Transform::rotation);
```

生产构建中，注册描述可由离线工具生成紧凑静态表。

### 12.3 Native Thunk

```cpp
using NativeThunk = void (*)(NativeCallFrame& frame);
```

Thunk 负责：

- 读取已知类型参数；
- 校验句柄和生命周期；
- 调用 C++ 函数；
- 写入返回值；
- 转换宿主错误；
- 记录 Profiler Span；
- 执行 Capability 检查（不可信模块）。

热路径不使用：

```cpp
Variant Invoke(string name, Vector<Variant> args);
```

通用 Variant 调用只保留给编辑器、调试控制台、序列化迁移和低频反射场景。

### 12.4 引擎对象句柄

脚本不持有裸 C++ 指针：

```cpp
struct EngineObjectHandle
{
    uint32_t index;
    uint32_t generation;
};
```

句柄提供：

- 失效检测；
- 代际安全；
- 序列化控制；
- 跨线程消息传递限制；
- 调试显示；
- 热重载稳定性。

## 13. 元数据：Descriptor + Facet

### 13.1 统一描述符

```text
ModuleDescriptor
TypeDescriptor
FieldDescriptor
PropertyDescriptor
MethodDescriptor
ParameterDescriptor
AttributeDescriptor
```

每个描述符存放稳定身份、名称、签名和基础关系；领域信息通过 Facet 扩展：

```text
TypeDescriptor
  ├─ ScriptTypeFacet
  ├─ NativeBindingFacet
  ├─ SerializationFacet
  ├─ NetworkSchemaFacet
  ├─ EditorFacet
  ├─ DebugFacet
  ├─ GcLayoutFacet
  └─ DeterminismFacet
```

### 13.2 ID 模型

区分三种身份：

```text
RuntimeTypeId
StableSchemaId
DisplayName
```

- `RuntimeTypeId`：基于模块、规范全名和泛型参数生成，面向当前构建；
- `StableSchemaId`：用于存档、资源、网络协议和长期兼容；
- `DisplayName`：用于 UI、日志和文档。

示例：

```csharp
[StableId("b13731f3-47a1-4ee3-b6f0-84a8e5e2e140")]
struct Health
{
    [StableFieldId(1)]
    float current;

    [StableFieldId(2)]
    float maximum;
}
```

Stable ID 可以由工具首次生成并写入模块清单，开发者不必手工维护全部 GUID。

### 13.3 查询

提供：

- 按 ID O(1) 查询；
- 按规范名称索引；
- 按属性/Facet 索引；
- 模块局部 Symbol Table；
- 编辑器使用的模糊名称索引；
- 版本化 Descriptor Snapshot。

运行时高频代码不进行字符串反射查找。

## 14. 对象模型与 GC

### 14.1 所有权分类

| 对象类别 | 管理方式 |
|---|---|
| 脚本 `struct` | 栈、寄存器、内联字段或连续容器 |
| 脚本 `class` | 精确 GC |
| 引擎对象 | 代际句柄，由引擎管理 |
| GPU/文件/音频资源 | 引擎资源句柄和显式租约 |
| 临时编译器对象 | Arena/Region |
| 字符串 | 不可变对象，支持驻留和短字符串优化 |

### 14.2 GC 第一阶段

建议实现精确增量 Mark-Sweep：

- 三色标记；
- Write Barrier；
- 每帧工作预算；
- 大对象区；
- 对象头保存 TypeId 和 GC 状态；
- 类型描述符保存引用字段布局；
- Safepoint 只在受控位置出现；
- 调试器可以枚举对象和引用路径。

### 14.3 后续优化

- 年轻代和分代回收；
- 卡表；
- 逃逸分析；
- 栈上分配；
- 标量替换；
- 短生命周期 Arena；
- 闭包去分配；
- 协程状态压缩。

### 14.4 确定性 GC

确定性模式下，GC 进度由确定性指标驱动：

- 已分配字节数；
- 固定 Tick；
- 固定工作单元数。

不能由墙上时间预算决定何时回收，否则不同机器可能在不同 Tick 触发析构相关观察或资源压力变化。

## 15. 异常和错误

### 15.1 脚本异常

支持：

- `throw`；
- `try/catch/finally`；
- 运行时异常类型；
- 字节码异常表；
- AOT 明确展开或 Runtime Unwind；
- 异步异常传播；
- 调试器异常断点。

### 15.2 C++ 边界

- C++ 异常不得穿过 Runtime C ABI；
- Native Thunk 捕获允许的宿主异常并转换为脚本错误；
- Release 可配置不使用 C++ 异常；
- 预期失败优先使用 Result/Status；
- 非预期脚本异常包含脚本调用栈，不依赖原生 C++ 栈恢复。

## 16. 热重载

### 16.1 流程

```text
Compile new module
      ↓
Validate bytecode and metadata
      ↓
Compare old/new ABI and schema
      ↓
Build new descriptors and function entries
      ↓
Migrate globals and live script objects
      ↓
Atomically publish new module version
      ↓
Retire old code after active calls exit
```

### 16.2 函数热替换

- 函数通过 `FunctionEntry` 调用；
- 新版本增加版本号；
- 未进入的调用立即使用新版本；
- 已进入的旧调用默认执行完成；
- 调试模式可选择在 Safepoint 迁移活动帧；
- 旧模块资源采用 Epoch/RCU 风格延迟回收。

### 16.3 状态迁移

按 Stable Field ID 迁移：

- 同类型字段直接复制；
- 新字段使用默认值；
- 删除字段丢弃；
- 兼容数值类型执行受控转换；
- 不兼容变化调用显式迁移器；
- 迁移失败则回滚模块发布。

示例：

```csharp
[Migration(fromVersion: 2)]
static PlayerState Migrate(PlayerStateV2 old)
{
    return new PlayerState
    {
        health = old.health,
        stamina = 100.0f
    };
}
```

## 17. 调试器

### 17.1 运行时调试信息

每个函数记录：

- 源文件和版本；
- 行列范围；
- Sequence Point；
- 局部变量作用域；
- 变量到字节码寄存器/栈槽/原生位置映射；
- 内联调用链；
- 异常区域；
- 异步恢复点；
- 优化状态。

### 17.2 调试功能

- 源码断点；
- 条件断点；
- 命中次数；
- 单步进入、越过、跳出；
- 异常断点；
- 调用栈；
- 局部变量；
- 对象、数组、字典和 ECS 句柄展开；
- Watch；
- 调试表达式；
- 协程和任务列表；
- 模块与热重载版本；
- 远程调试；
- 确定性回放定位。

### 17.3 DAP

```text
VS Code / Custom Editor / IDE
              │
             DAP
              │
     RealScript Debug Adapter
              │
     Runtime Debug Protocol
              │
      Embedded Script Runtime
```

调试适配器不直接访问 VM 内部对象指针，而使用版本化调试协议。

### 17.4 AOT 调试

AOT 代码插入：

- Sequence Point；
- 脚本 Shadow Stack；
- 循环回边 Safepoint；
- 函数进入/退出记录；
- 可选局部变量 Materialization。

Debug AOT 应限制部分内联和变量合并。优化后不可恢复的变量显示为 `optimized out`，不能伪造错误值。

## 18. LSP 与编辑器服务

LSP 服务复用编译器前端：

- 增量诊断；
- 自动补全；
- Signature Help；
- 跳转定义；
- 查找引用；
- 重命名；
- 语义高亮；
- Inlay Hint；
- Code Action；
- 格式化；
- 模块和依赖图；
- Native API 文档；
- 属性和 Capability 校验。

编译器、LSP 和构建系统必须共享：

- Syntax Tree；
- Symbol；
- Type；
- Descriptor；
- Diagnostic Code。

禁止工具链重新实现一套简化语义分析。

## 19. 确定性模式

### 19.1 模块声明

```csharp
[Deterministic]
module Game.Simulation;
```

编译器和运行时共同限制：

- 禁止墙上时间；
- 禁止系统随机数；
- 禁止任意线程；
- 禁止无序容器不稳定迭代；
- 禁止操作系统文件和网络；
- 禁止未标记为确定性的 Native API；
- 禁止依赖地址的 Hash；
- 禁止非版本化序列化；
- 协程只能等待确定性 Tick 和事件。

### 19.2 时间与随机数

```csharp
void Tick(in TickContext context)
{
    int value = context.rng.NextInt(0, 100);
    FixedTime dt = context.delta;
}
```

RNG 状态必须属于模拟状态或显式子流，不能使用全局隐藏随机源。

### 19.3 浮点

跨架构位级浮点确定性难以保证。RealScript 应提供项目级策略：

1. 表现层允许普通 `float`；
2. 权威模拟使用 `fixed32`/`fixed64`；
3. 或采用受控浮点模式，固定编译选项、舍入、Flush-to-zero 和数学库；
4. 提供确定性向量、角度和三角函数库；
5. 测试多架构回放哈希。

### 19.4 回放和时间旅行

记录：

- 周期性状态快照；
- 输入命令；
- RNG 状态；
- 脚本模块哈希；
- Schema 版本；
- 外部确定性事件。

向后调试采用“恢复最近快照 + 重放到目标 Tick”，不尝试逆转原生机器指令。

## 20. ECS 和游戏引擎集成

### 20.1 批量优先

避免：

```csharp
foreach (var entity in allEntities)
{
    var transform = entity.Get<Transform>();
    var velocity = entity.Get<Velocity>();
    transform.position += velocity.value * dt;
}
```

推荐：

```csharp
foreach (ref var transform, in var velocity
         in Query<Write<Transform>, Read<Velocity>>())
{
    transform.position += velocity.value * dt;
}
```

编译器可将查询降级为 Chunk 批处理，减少：

- 句柄解析；
- 类型查找；
- 脚本/原生边界跨越；
- 随机内存访问；
- 虚调用。

### 20.2 生命周期

- Script System 由 World 持有；
- Entity 使用代际句柄；
- Component View 只在查询作用域有效；
- `ref` 不能逃逸到堆或协程；
- 结构变化命令写入 Command Buffer；
- 确定性系统按显式顺序或依赖图调度。

## 21. Mod 沙箱和安全

### 21.1 Capability

模块清单声明：

```text
capabilities:
  - game.read-world
  - game.submit-commands
  - ui.create-panel
  - storage.mod-scope
```

不可信 Mod 默认不能访问：

- 任意文件；
- 任意网络；
- 操作系统进程；
- 原生动态库；
- 未授权引擎对象；
- 调试器管理接口；
- 其他 Mod 私有存储。

### 21.2 资源预算

每个模块配置：

- 每 Tick 最大指令数；
- 最大调用深度；
- 最大堆大小；
- 最大对象数；
- 最大字符串长度；
- 最大容器容量；
- 最大协程数；
- 最大事件队列；
- 超限策略。

超限策略可为：

- 暂停到下一 Tick；
- 取消当前任务；
- 禁用模块；
- 抛出可捕获异常；
- 记录安全审计事件。

## 22. Profiler 和可观测性

需要同时观察：

- 脚本函数 CPU 时间；
- VM 指令数；
- Native Thunk 次数和耗时；
- 分配字节和对象数；
- GC 阶段和暂停；
- 协程数量和等待原因；
- 热重载耗时；
- AOT/JIT 编译耗时；
- ECS 查询实体数；
- Capability 拒绝和预算超限。

Profiler 数据可导出为：

- 引擎内置 Timeline；
- Chrome Trace Event；
- 统计摘要；
- 自动性能回归报告。

## 23. 性能指标与基准

以下是设计目标，必须由固定硬件、固定编译选项和固定工作负载验证。

| 指标 | 目标 |
|---|---:|
| 字节码纯计算 | 等价优化 C++ 的 8–15 倍耗时以内 |
| 引擎调用占主导逻辑 | C++ 总耗时的 2–3 倍以内 |
| AOT 类型化数值代码 | 与手写 C++ 差距 0–30% |
| AOT Native API 调用 | 接近普通 C ABI 调用 |
| VM 到 Native Thunk | 直接函数指针调用开销的约 2 倍以内 |
| Debug 功能关闭后的额外开销 | 目标低于 1% |
| GC P99 单帧暂停 | 60 FPS 下低于 0.5 ms |
| GC 极端暂停 | 尽量低于 2 ms |
| 典型模块增量编译 | 低于 100 ms |
| 典型模块字节码热重载 | 低于 200 ms |
| 10,000 空闲协程 | 保持低 CPU 占用和紧凑内存 |

基准集：

1. 标量和向量运算；
2. 分支和循环；
3. 结构体复制；
4. 数组和字典；
5. 字符串拼接；
6. 静态、虚和接口调用；
7. Script → Native；
8. Native → Script；
9. ECS Chunk 查询；
10. 小对象分配；
11. 闭包；
12. 协程；
13. GC 吞吐和尾延迟；
14. 热重载；
15. 存档迁移；
16. 多平台确定性回放；
17. 字节码和 AOT 结果差分。

## 24. 代码组织

```text
realscript/
  CMakeLists.txt
  cmake/
  include/realscript/
    compiler/
    runtime/
    metadata/
    debug/
    bindings/
  src/
    common/
    syntax/
    semantic/
    ir/
      hir/
      mir/
      optimizer/
    bytecode/
    vm/
    runtime/
    gc/
    metadata/
    bindings/
    aot_cpp/
    jit_llvm/
    hot_reload/
    debug/
    tooling/
      lsp/
      dap/
    profiler/
  tools/
    rsc/
    rsdump/
    rsdebug/
    rsbench/
  tests/
    unit/
    parser/
    semantic/
    conformance/
    differential/
    hot_reload/
    determinism/
    benchmarks/
  samples/
    hello/
    ecs_system/
    hot_reload/
    debugger/
    deterministic_sim/
  docs/
```

### 24.1 依赖方向

```text
common
  ↑
syntax
  ↑
semantic
  ↑
HIR → MIR → backend
             ↓
     bytecode / aot_cpp / jit_llvm
             ↓
runtime ← metadata ← bindings
   ↑            ↑
 debug       tooling
```

运行时不能依赖编译器前端。发布产品可以只携带 Runtime、Metadata、Bindings 和所需执行后端。

## 25. 命令行工具

### 25.1 `rsc`

```text
rsc check game.rsproj
rsc build game.rsproj --backend=bytecode
rsc build game.rsproj --backend=cpp-aot
rsc emit-mir game.rs --output=game.mir
rsc emit-cpp game.rsproj --output=generated/
rsc migrate-check old.rsmeta new.rsmeta
```

### 25.2 `rsdump`

```text
rsdump bytecode module.rsbc
rsdump metadata module.rsm
rsdump gc-layout Game.Player
rsdump callgraph module.rsm
```

### 25.3 `rsdebug`

```text
rsdebug --dap-port=4711 --runtime=127.0.0.1:4712
```

## 26. 构建配置

| 配置 | 特征 |
|---|---|
| Editor | 编译器服务、字节码、调试、热重载、完整元数据 |
| Debug | 检查、Sequence Point、变量信息、低优化 |
| Profile | 优化 + Profiler + 调用边界统计 |
| Release | C++ AOT、裁剪元数据、关闭调试 Hook |
| Server | 无编辑器依赖、确定性和回放支持 |
| ModHost | 字节码验证、Capability 和资源预算 |

## 27. 实施切片

### Slice 0：工程引导

- C++17/CMake 工程；
- 基础类型、Arena、StringId、SourceSpan；
- 测试框架；
- CI；
- 编译器诊断格式；
- 基准框架。

验收：库和空工具可在主要桌面平台构建。

### Slice 1：语法和最小语义

- Lexer；
- Parser；
- Syntax Tree；
- 模块和函数；
- 基础类型；
- 局部变量；
- `if`、`while`、`return`；
- 诊断和格式化输出。

验收：可解析和检查一组最小程序。

### Slice 2：HIR、MIR 与解释器闭环

- Typed HIR；
- 基础 MIR；
- 寄存器字节码；
- 字节码验证器；
- VM 调用栈；
- 整数、浮点和控制流；
- `rsc run`。

验收：源码可以编译并执行，语义测试稳定。

### Slice 3：值类型和原生绑定

- `struct`；
- 字段和方法；
- 数组和字符串；
- Descriptor；
- C++ 注册 API；
- 生成 Native Thunk；
- Script/Native 双向调用。

验收：脚本可高效更新宿主暴露的值类型。

### Slice 4：对象和 GC

- `class`；
- 单继承；
- 接口；
- 精确 GC；
- Write Barrier；
- 异常；
- 对象调试视图。

验收：对象图可安全运行，GC 压力测试通过。

### Slice 5：调试和 LSP

- 调试信息；
- 断点和单步；
- 变量和调用栈；
- DAP；
- 增量编译服务；
- LSP 基础功能。

验收：VS Code 或测试客户端可完成源码调试。

### Slice 6：热重载

- 模块版本；
- FunctionEntry 替换；
- Descriptor Diff；
- 全局和对象迁移；
- 断点重绑定；
- 回滚。

验收：修改函数和兼容字段后无需重启进程。

### Slice 7：C++17 AOT

- MIR 到 C++17；
- Runtime C ABI；
- 模块导出；
- 编译缓存；
- Source Map；
- 字节码/AOT 差分测试。

验收：同一测试集在 VM 和 AOT 下结果一致。

### Slice 8：确定性和 ECS

- `[Deterministic]`；
- API Effect/Capability 检查；
- 固定 Tick；
- Seeded RNG；
- ECS Chunk 查询；
- 快照和回放哈希。

验收：跨进程长时间回放状态哈希一致。

### Slice 9：高级性能

- 去虚化；
- 泛型单态化；
- 逃逸分析；
- 边界检查消除；
- 分代 GC；
- PGO；
- 可选 LLVM ORC。

验收：达到正式性能目标并建立回归门槛。

## 28. 测试策略

### 28.1 语法和诊断 Golden Tests

输入源码，对比：

- Syntax Tree；
- Diagnostic Code；
- SourceSpan；
- 修复建议。

### 28.2 Conformance Tests

覆盖每条语言语义：

- 数值转换；
- 求值顺序；
- 空值；
- 值类型复制；
- 接口分派；
- 异常；
- 泛型；
- 协程。

### 28.3 Differential Tests

同一 MIR 分别运行：

- Bytecode VM；
- C++ AOT；
- 可选 JIT。

比较：

- 返回值；
- 输出事件；
- 异常；
- 堆状态摘要；
- 确定性哈希。

### 28.4 Fuzzing

- Lexer/Parser Fuzz；
- 字节码验证器 Fuzz；
- 序列化输入 Fuzz；
- 调试协议 Fuzz；
- 热重载 Schema Diff Fuzz。

## 29. 风险与控制

| 风险 | 影响 | 控制方式 |
|---|---|---|
| 语言范围失控 | 无法按期完成 | 明确非目标，按 Slice 冻结特性 |
| VM 与 AOT 语义分叉 | 隐蔽发布 Bug | 统一 MIR、Runtime Intrinsic 和差分测试 |
| C++ 绑定开销过高 | 游戏逻辑性能不足 | 类型化 Thunk、批量 API、ECS Query |
| GC 尾延迟 | 卡顿 | 值类型优先、增量 GC、分配 Profiler |
| 热重载迁移不可靠 | 编辑状态损坏 | Stable ID、事务迁移和回滚 |
| 完整 C# 预期 | 用户困惑 | 明确兼容边界和语言规范 |
| JIT 限制平台 | 发布受阻 | C++ AOT 为主发布后端 |
| 元数据重复 | 系统长期漂移 | Descriptor + Facet 单一来源 |
| 确定性被宿主 API 破坏 | 联机和回放失效 | Capability、Effect 检查和回放哈希 |
| Mod 逃逸沙箱 | 安全问题 | 字节码验证、句柄、预算和最小能力 |

## 30. 首批需要冻结的架构决策

1. 语言不兼容完整 C#；
2. C++17 是核心运行时和 AOT 输出基线；
3. Typed MIR 是所有执行后端共同边界；
4. 编辑/调试使用类型化寄存器字节码；
5. 发布使用 C++17 AOT；
6. LLVM ORC 是可选后期能力；
7. `struct` 是值类型，`class` 是 GC 引用类型；
8. 引擎对象使用代际句柄；
9. Native 高频调用使用生成 Thunk；
10. 元数据使用 Descriptor + Facet；
11. DAP/LSP 是正式工具接口；
12. 确定性模块由语言、编译器和宿主 API 联合约束；
13. 热重载依赖稳定函数入口和 Stable Schema ID；
14. 外部字节码加载前必须验证；
15. VM/AOT/JIT 必须执行差分测试。

## 31. 后续规范文档建议

在进入大规模实现前，继续补充：

- `LANGUAGE_SPEC.md`：词法、语法和语言语义；
- `TYPE_SYSTEM.md`：类型、转换、泛型和可空性；
- `MIR_SPEC.md`：MIR 指令、控制流和验证规则；
- `BYTECODE_SPEC.md`：字节码编码和版本化；
- `RUNTIME_ABI.md`：AOT 模块和宿主 ABI；
- `METADATA_SCHEMA.md`：Descriptor、Facet 和 Stable ID；
- `DEBUG_PROTOCOL.md`：运行时调试协议与 DAP 映射；
- `HOT_RELOAD.md`：ABI 比较、状态迁移和回滚；
- `DETERMINISM.md`：受限 API、数值和回放规范；
- `BENCHMARKS.md`：硬件、测试集和性能门槛。

## 32. 参考链接

- AngelScript: <https://www.angelcode.com/angelscript/>
- Luau: <https://luau.org/>
- Unity IL2CPP: <https://docs.unity3d.com/Manual/IL2CPP.html>
- LLVM ORCv2: <https://llvm.org/docs/ORCv2.html>
- Wren: <https://wren.io/>
- GDScript: <https://docs.godotengine.org/en/stable/tutorials/scripting/gdscript/>
- .NET Native Hosting: <https://learn.microsoft.com/dotnet/core/tutorials/netcore-hosting>
- Debug Adapter Protocol: <https://microsoft.github.io/debug-adapter-protocol/>
- Language Server Protocol: <https://microsoft.github.io/language-server-protocol/>

---

本文档是 RealScript 的总体架构基线。后续实现可以演进内部细节，但若修改第 30 节的冻结决策，应通过明确的架构决策记录说明动机、兼容影响和迁移方案。
