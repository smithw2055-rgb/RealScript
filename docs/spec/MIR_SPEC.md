# RealScript Typed MIR 规范

> Phase 3B 已实现 `new.object`、`check.notnull`、`load.field`、`store.field` 与精确 Object TypeId；具体边界见 [OBJECT_MODEL_V0.md](OBJECT_MODEL_V0.md)。


- 规范版本：Draft v0.1
- `mir_version`：0.1
- 状态：逻辑格式草案，文本和二进制序列化尚未冻结

## 1. 目的

Typed MIR 是 RealScript 所有执行后端的共同语义边界。

前端必须在进入 MIR 前完成名称绑定、重载解析和主要类型检查；字节码、C++17 AOT 与可选 JIT 必须从符合本规范的 MIR 或语义等价表示生成。

MIR 的主要目标：

- 显式表达控制流、类型、内存访问和异常；
- 消除高级语法糖；
- 为验证、优化、调试和差分测试提供稳定边界；
- 阻止后端继承 C++ 未定义行为；
- 确保解释与编译模式语义一致。

## 2. 基本结构

```text
MirModule
  TypeTable
  ConstantTable
  GlobalMetadata
  Functions[]

MirFunction
  Signature
  Locals[]
  Blocks[]
  ExceptionRegions[]
  DebugScopes[]
```

每个函数由基本块组成。每个基本块：

- 具有稳定块 ID；
- 包含零个或多个普通指令；
- 以且只以一个终结指令结束；
- 不允许从块中部进入；
- 所有前驱和后继必须可计算。

## 3. 值、位置和类型

MIR 区分：

- **值（value）**：不可变 SSA 值或虚拟寄存器结果；
- **位置（place）**：可以加载或存储的地址抽象；
- **对象引用（object reference）**：受 GC 跟踪的引用；
- **宿主句柄（host handle）**：不属于 GC 的代际句柄；
- **受管借用（managed borrow）**：受生命周期限制的 `in/ref/out` 位置引用。

值必须带完整静态类型。禁止使用“运行时再判断”的无类型通用寄存器代替类型系统。

位置可以表示：

```text
local %n
arg %n
field <base, field-id>
static-field <module, field-id>
array-element <array, index>
indirect <borrow>
```

v0.1 不允许任意指针算术位置。

## 4. SSA 与合流

MIR 应当采用 SSA 或接近 SSA 的虚拟寄存器形式。控制流合流使用 `phi`：

```text
bb0:
  br_if %condition, bb1, bb2

bb1:
  %a = const.i32 10
  br bb3

bb2:
  %b = const.i32 20
  br bb3

bb3:
  %result = phi [bb1: %a], [bb2: %b]
  ret %result
```

`phi` 必须：

- 位于块的所有普通指令之前；
- 为每个可达前驱提供一个输入；
- 输入类型完全相同；
- 不引用不支配对应前驱出口的值。

实现也可以内部使用块参数，但序列化或调试工具必须能够映射为等价 `phi` 语义。

## 5. 指令分类

### 5.1 常量和默认值

```text
const.bool
const.i8/i16/i32/i64
const.u8/u16/u32/u64
const.f32/f64
const.char
const.string
const.null
default <type>
```

浮点常量必须保存位模式，不得只保存区域设置相关文本。

### 5.2 数值运算

```text
add.checked.<type>
add.wrap.<type>
sub.checked.<type>
sub.wrap.<type>
mul.checked.<type>
mul.wrap.<type>
div.<type>
rem.<type>
neg.checked.<type>
neg.wrap.<type>
shl.<type>
shr.signed.<type>
shr.unsigned.<type>
bit.and/or/xor/not
```

指令名称必须编码足够语义，禁止让后端根据构建模式猜测溢出规则。

浮点运算必须区分规范 IEEE 模式和未来可能增加的 Fast Math 模式。

### 5.3 比较和逻辑

```text
cmp.eq
cmp.ne
cmp.lt
cmp.le
cmp.gt
cmp.ge
bool.not
```

比较指令必须知道操作数类型和有符号性。引用身份比较、字符串内容比较和结构化值比较必须使用不同 intrinsic 或明确指令。

### 5.4 转换

```text
convert.checked <source, target>
convert.wrap <source, target>
upcast.class
downcast.checked
test.type
cast.interface
box
unbox.checked
nullable.wrap
nullable.has-value
nullable.get-value
```

所有可能失败的转换必须显式表示异常行为。

### 5.5 内存与字段

```text
load <place>
store <place, value>
load.field <object-or-value, field-id>
store.field <object-or-borrow, field-id, value>
load.element <array, index>
store.element <array, index, value>
borrow.in <place>
borrow.ref <place>
borrow.out <place>
```

数组访问必须在 MIR 中保留边界检查，除非优化器已经证明安全并记录消除依据。

对象字段访问必须在需要时显式包含空值检查。后端不得自行决定是否忽略检查。

### 5.6 对象和数组

```text
new.object <type-id, ctor-id, args...>
new.array <element-type, length>
array.length
string.concat
string.equals
```

对象分配是 GC safepoint。数组长度为负必须抛出受控异常。

### 5.7 调用

```text
call.static <function-id>
call.direct <method-id>
call.virtual <slot-id>
call.interface <interface-id, slot-id>
call.delegate
call.native <binding-id>
```

调用指令必须携带：

- 完整函数签名；
- 参数模式；
- 是否可能抛出；
- 是否可能分配；
- 是否是 safepoint；
- Capability 要求；
- Determinism 分类。

编译期去虚化可以把虚调用替换为 direct call，但必须证明动态目标唯一。

### 5.8 控制流终结指令

```text
br <target>
br_if <condition, true-target, false-target>
switch <value, cases..., default>
ret
ret.value
throw
rethrow
unreachable
leave <region, target>
```

每个基本块必须以终结指令结束。`unreachable` 只允许用于已经由前端或优化证明不可达的路径，不能隐藏缺失返回。

## 6. 检查指令

以下检查必须显式存在或经证明消除：

```text
check.not-null
check.bounds
check.overflow
check.divide-nonzero
check.valid-handle
check.capability
check.budget
```

检查失败必须产生规范定义的脚本异常或模块终止状态，不能触发宿主崩溃、C++ UB 或越界访问。

优化器消除检查时必须满足：

- 证明在所有进入路径上成立；
- 不改变异常发生顺序；
- 不跨越可能修改相关状态的调用；
- Debug 构建能够恢复来源信息。

## 7. 异常区域

异常处理使用结构化区域表：

```text
try-region
  protected blocks
  handlers[]
    catch <type-id> -> block
    finally -> block
```

区域必须正确嵌套，禁止部分重叠。

`finally` 的正常和异常退出必须通过显式 `leave`、清理块或语义等价机制表示。后端不能依赖 C++ 析构展开来定义脚本 `finally`。

脚本异常对象必须保持类型身份和脚本影子栈。原生绑定抛出的 C++ 异常不得直接进入 MIR 异常机制；绑定层必须捕获并转换。

## 8. 异步降级

`async` 函数在进入可执行 MIR 前降级为状态机：

```text
AsyncState
  state
  captured parameters
  captured locals
  pending awaiter
  result or exception
```

每个 `await` 产生：

- 完成快速路径；
- 保存状态路径；
- 调度器注册；
- 恢复入口；
- 异常传播路径。

受管借用和栈位置不得跨越挂起点。验证器必须拒绝此类 MIR。

## 9. 源码序列点和调试作用域

每个具有用户可见行为的语句应当关联源码序列点：

```text
sequence-point <file-id, start-line, start-column, end-line, end-column>
```

序列点分为：

- 可停止；
- 隐藏；
- 调用；
- 循环回边；
- 异常边界；
- 异步挂起/恢复。

局部变量必须包含作用域和位置映射。优化后变量可能显示为 `optimized out`，但不得显示错误值。

Debug 模式中的优化必须保留：

- 单步语句顺序；
- 调用栈逻辑结构；
- 异常位置；
- 活跃局部变量的正确值或不可用状态。

## 10. 副作用模型

每条指令必须声明副作用分类：

```text
Pure
ReadMemory
WriteMemory
Allocate
Throw
CallHost
Synchronize
Suspend
ObserveDebug
```

优化器只有在不改变副作用偏序的前提下才能移动或删除指令。

宿主调用默认视为可能读写内存、分配和抛出，除非绑定元数据明确声明更精确效果并经过验证。

## 11. 确定性标记

函数和调用具有 Determinism 分类：

- `Deterministic`；
- `DeterministicWithSeed`；
- `RecordedInput`；
- `PresentationOnly`；
- `NonDeterministic`。

确定性模块不得调用后两类之外未经允许的函数。`RecordedInput` 只能在宿主保证结果记录并参与回放时使用。

MIR 验证必须沿调用图传播最严格分类。

## 12. 验证规则

有效 MIR 必须满足：

- 所有块以终结指令结束；
- 所有引用的块、类型、字段和函数存在；
- SSA 支配关系正确；
- `phi` 输入完整且类型一致；
- 指令操作数和结果类型匹配；
- `ref/in/out` 生命周期有效；
- 异常区域正确嵌套；
- `await` 不保存非法借用；
- 返回值与函数签名一致；
- Capability 和 Determinism 约束满足；
- 不存在跳入受保护区域中部的控制流；
- 所有可达路径有定义行为。

任何验证失败必须阻止生成字节码和 AOT 产物。

## 13. 优化合法性

允许的基础优化包括：

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

任何优化都必须保持：

- 左到右求值的可观察结果；
- 异常类型和最先发生位置；
- `finally` 执行；
- 原生调用顺序；
- 分配身份的可观察语义；
- 调试档位承诺；
- 确定性输出。

AOT 后端不能通过 C++ 编译器优化绕过这些要求。必要时生成代码必须使用 intrinsic、显式检查、受控无符号运算或编译器屏障。

## 14. 文本表示

建议提供人类可读 `.rsmir` 调试格式：

```text
func @Game.Health.ApplyDamage(%self: ref Health, %amount: f32) -> void {
  bb0:
    seqpoint 12:5-12:31
    %0 = load.field %self, Health.current
    %1 = sub.f32 %0, %amount
    store.field %self, Health.current, %1
    ret
}
```

文本格式用于测试快照、调试和工具，不作为首版发布兼容格式。稳定兼容由 `mir_version` 和正式序列化规范决定。

## 15. 差分测试要求

同一 MIR 测试用例应至少比较：

- 参考 MIR 解释器；
- 字节码 VM；
- C++17 AOT；
- 可选 JIT。

比较内容包括：

- 返回值和位模式；
- 异常类型与源码位置；
- 可观察宿主调用序列；
- 分配和 GC 安全性；
- 状态哈希；
- 确定性回放结果。

任何后端差异默认视为实现缺陷，除非规范明确允许实现定义行为。
