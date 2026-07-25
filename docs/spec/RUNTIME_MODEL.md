# RealScript 运行时模型规范

> Phase 3B 的语言可见对象、运行时 TypeId 和精确字段引用图见 [OBJECT_MODEL_V0.md](OBJECT_MODEL_V0.md)。


- 规范版本：Draft v0.1
- 适用版本：`language_version = 0.1`
- 状态：对象模型和行为约束草案，物理布局尚未冻结

## 1. 范围

本规范定义 RealScript 可观察的运行时行为，包括：

- 执行上下文和模块实例；
- 值、对象、数组、字符串和宿主句柄；
- 类、结构体和接口分派；
- 精确 GC 与 safepoint；
- 异常和脚本调用栈；
- 任务、协程和调度；
- 线程与宿主进入规则；
- Capability、执行预算和沙箱；
- 热重载、对象迁移和版本身份；
- 确定性运行档位。

具体内存布局可以因平台和后端不同，但不得改变语言级语义或 Runtime ABI 承诺。

## 2. Runtime、Execution Context 与 Module Instance

一个宿主进程可以创建一个或多个 `Runtime`。每个 Runtime 拥有：

```text
Runtime
  RuntimeApi
  TypeRegistry
  BindingRegistry
  GC Heap(s)
  Scheduler
  DebugService
  Profiler
  ModuleLoader
```

脚本代码在 `ExecutionContext` 中运行：

```text
ExecutionContext
  Runtime reference
  Current module instance
  Script shadow stack
  Exception state
  Capability set
  Budget counters
  Determinism context
  Thread ownership
```

模块源码被编译为 `ModuleDefinition`，加载后产生 `ModuleInstance`。定义可以跨上下文共享只读代码和元数据；可变状态必须属于模块实例或宿主服务。

v0.1 不允许隐式可变模块全局变量。模块级持久状态必须由显式状态对象承载。

## 3. 值表示

运行时概念上支持以下值类别：

```text
ScalarValue
StructValue
ObjectReference
InterfaceReference
DelegateValue
ManagedBorrow
HostHandle
Null
```

实现可以使用寄存器、栈槽、内联字节或句柄表表示，但必须满足：

- 值类型赋值具有复制语义；
- 引用类型赋值复制引用身份；
- GC 可以精确识别每个活跃引用；
- 调试器可以通过类型元数据解释值；
- 不允许把原始宿主指针伪装为脚本整数或普通引用。

通用 `ScriptValue` 只用于反射、编辑器、调试或低频动态调用。高频字节码和 Native Thunk 不应强制装箱为 `ScriptValue`。

## 4. 类型身份

运行时类型至少具有：

- `RuntimeTypeId`：当前构建中的快速身份；
- `StableSchemaId`：存档、资源、网络和热重载兼容身份；
- `QualifiedName`：诊断和工具显示；
- `ModuleVersion`；
- `LayoutVersion`；
- `TypeDescriptor`。

`RuntimeTypeId` 可以由模块 GUID、规范化限定名和泛型实参生成，不保证跨构建稳定。

需要持久兼容的类型必须具有显式或工具生成的 `StableSchemaId`。重命名不得依赖旧名称哈希继续兼容。

泛型封闭类型的身份由泛型定义身份和有序类型实参共同决定。

## 5. 结构体运行模型

结构体是内联值：

- 可以位于 VM 寄存器、调用帧、数组、类字段或宿主提供的受控存储；
- 赋值复制全部逻辑字段；
- 包含引用字段时，类型描述符必须提供 GC 引用图；
- 不允许自引用内联递归；
- `readonly struct` 不允许通过其实例位置写入字段。

物理填充字节不属于结构体值语义：

- 相等、哈希、序列化和状态哈希不得读取未初始化填充；
- AOT 后端不得使用包含未定义填充的 `memcmp` 实现默认值相等；
- 跨 ABI 传递应使用生成签名或规范化布局，而不是假设宿主 C++ 结构体布局相同。

## 6. 类和对象

脚本类实例由 GC 管理。概念对象头包含：

```text
ObjectHeader
  RuntimeTypeId
  GC state
  object flags
  optional sync/debug data
  instance fields...
```

对象头物理布局不属于 v0.1 公共 ABI。生成代码必须通过 Runtime ABI、内联缓存描述或同版本运行时私有约定访问。

对象创建必须：

1. 验证类型可实例化；
2. 分配足够空间；
3. 将全部字段置为语言默认值；
4. 建立临时 GC 安全引用；
5. 执行基类构造；
6. 执行当前类型字段初始化和构造函数；
7. 在成功后返回对象。

构造函数抛出时，对象不得暴露为已完成实例。GC 可以回收该分配，但必须保持构造过程中已经发布资源的宿主清理协议。

对象身份在生命周期内稳定。移动 GC 可以改变物理地址，但所有脚本和宿主引用必须通过可更新引用、句柄或固定协议保持逻辑身份。

## 7. 接口引用和分派

接口引用概念上包含：

```text
InterfaceReference
  object reference
  interface dispatch descriptor
```

实现可以使用对象引用加接口 ID、fat pointer 或缓存槽，但必须：

- 保持对象身份；
- 支持空值；
- 使用接口定义的稳定槽次序；
- 在无兼容实现时抛出受控转换异常；
- 允许编译器在证明具体类型唯一时去虚化。

接口槽属于元数据兼容面。向已发布接口中间插入成员会改变槽布局，必须提升兼容版本或使用稳定 MemberId 间接层。

## 8. 数组

数组是固定长度、零基、连续逻辑元素集合。

```text
ArrayObject
  element type
  length
  elements...
```

要求：

- 长度为非负 `int`；
- 创建时元素全部默认初始化；
- 每次未证明安全的访问执行边界检查；
- 数组长度不可改变；
- 引用数组写入必须检查元素类型兼容，除非静态类型保证安全；
- GC 必须追踪引用元素和含引用结构体元素。

v0.1 不要求多维矩形数组。标准库可以基于一维数组提供二维容器。

大数组可以进入大对象区，但这不得改变语义。

## 9. 字符串

`string` 是不可变引用类型，逻辑内容为 Unicode 标量序列。

运行时内部可以使用 UTF-8、UTF-16 或分片表示，但必须保证：

- 字符串相等按逻辑内容比较；
- 哈希算法在要求确定性的场景中稳定且版本化；
- 索引 API 必须明确以字节、代码点还是文本元素为单位；
- v0.1 核心 `Length` 应定义为 Unicode 标量数量，避免暴露内部编码；
- 区域设置相关比较只能通过显式 Culture API；
- Deterministic Profile 只允许 ordinal 或规范指定的比较。

字符串驻留是实现优化。除非使用显式 intern API，程序不得依赖引用身份判断内容相等。

## 10. 委托和闭包

委托逻辑上包含：

```text
DelegateValue
  function identity
  optional target object
  optional closure environment
```

静态函数委托不需要目标对象。实例方法委托保持目标对象活跃。

闭包环境由捕获变量构成：

- 按值捕获保存值副本；
- 引用类型变量按引用值捕获；
- 需要共享可变局部时，编译器生成受管 cell；
- `ref/in/out` 和栈借用不得进入可逃逸闭包；
- 不逃逸闭包可以栈上或内联优化，但不得改变可观察身份。

## 11. 宿主对象和代际句柄

C++ 引擎对象默认不由脚本 GC 拥有。脚本通过代际句柄引用宿主资源：

```text
HostHandle
  kind/type
  index
  generation
  optional world/runtime id
```

句柄访问必须验证：

- 类型；
- generation；
- 所属 Runtime/World；
- Capability；
- 线程或阶段限制。

失效句柄不是 `null`。调用要求有效对象的 API 时必须抛出 `InvalidHandleException` 或返回显式结果类型，具体由绑定签名决定。

禁止脚本长期保存裸 C++ 指针。临时借用只能在 Native Thunk 调用期间或明确 pin/lease 生命周期内有效。

## 12. GC

> Phase 3A 的已实现非移动增量 Mark/Sweep 边界、ObjectRef 代际句柄和精确根规则见 [MANAGED_HEAP_GC_V0.md](MANAGED_HEAP_GC_V0.md)。本文其余条款仍描述长期完整运行时目标。


### 12.1 基本要求

v0.1 运行时必须提供精确 GC：

- 精确根集合；
- 精确对象字段描述；
- 写屏障；
- safepoint；
- 增量工作预算；
- 受控大对象处理；
- 无用户可见终结器。

实现可以使用非移动 Mark-Sweep、移动压缩或分代 GC，但不能暴露物理地址稳定性。

### 12.2 根集合

GC 根至少包括：

- VM 活跃调用帧和寄存器；
- AOT 脚本影子栈/stack map；
- 模块状态对象；
- 调度中的任务和闭包；
- 调试器临时句柄；
- 宿主显式注册的强根；
- 进行中的异常对象。

宿主保存脚本对象必须使用 `StrongHandle`、`WeakHandle` 或等价 API，不得保存不受跟踪的原始地址。

### 12.3 Safepoint

以下位置默认是 safepoint：

- 对象或数组分配；
- 脚本函数调用；
- 可能分配的原生调用；
- 循环回边；
- `await` 挂起和恢复；
- 显式调试/预算检查点。

AOT 后端必须为 safepoint 提供活跃引用位置图。

### 12.4 写屏障

任何把受管引用写入堆对象、数组或持久模块状态的操作必须执行与 GC 算法匹配的写屏障。优化器只有在证明目标不需要屏障时才能消除。

### 12.5 暂停和预算

增量 GC 应由宿主每帧提供工作预算。Deterministic Profile 中工作量必须由分配字节、对象数量或 Tick 等确定性计数驱动，不能以墙上微秒作为唯一触发条件。

GC 可以在不同实现中选择不同收集时机，但不得改变可观察脚本语义。由于没有用户终结器，回收时机不应成为程序逻辑输入。

## 13. 弱引用和资源生命周期

v0.1 可以提供 `Weak<T>`：

- 不保持目标对象存活；
- 读取时返回 `T?`；
- 目标回收后稳定返回 `null`；
- 不保证何时从非空变为空。

GPU、音频、文件、网络连接和实体生命周期必须使用显式宿主资源句柄、租约或作用域对象。GC 只能回收脚本包装对象，不能单独定义资源释放时机。

## 14. 调用帧和脚本影子栈

每次脚本函数调用产生逻辑脚本帧：

```text
ScriptFrame
  FunctionId
  ModuleVersion
  instruction/native location
  caller
  argument/local location map
  current sequence point
```

字节码 VM 可以直接使用 VM 帧。AOT 代码必须维护影子栈或提供等价可遍历结构。

影子栈用于：

- 异常栈；
- DAP 调试；
- GC stack map 关联；
- profiler；
- 热重载活动版本判断。

Release 构建可以压缩信息，但必须保留异常和采样分析所需的函数身份。

## 15. 异常运行模型

脚本异常状态包含：

```text
ScriptException
  object reference
  throw sequence point
  logical stack trace
  optional native binding context
```

异常传播必须遵循语言规范中的 `catch/finally` 规则。

Native Thunk 不得允许任意 C++ 异常越过边界。绑定包装必须：

1. 捕获声明允许的宿主异常；
2. 映射为脚本异常或错误结果；
3. 记录绑定 ID；
4. 清理临时 pin、lease 和借用；
5. 返回运行时统一异常状态。

内存不足、预算超限或验证错误可以触发不可捕获的模块终止，但宿主进程仍应保持可控，除非宿主明确配置为 fail-fast。

## 16. 任务和协程调度

任务由 Runtime Scheduler 驱动。默认模型为游戏线程协作式调度，而不是每任务一个系统线程。

任务状态：

```text
Created
Scheduled
Running
Suspended
Completed
Faulted
Cancelled
```

挂起任务必须保存：

- 状态机对象；
- 恢复函数 ID 和版本；
- 捕获局部；
- 等待原因；
- ExecutionContext；
- Capability 和 Determinism 上下文。

恢复必须在兼容执行上下文中发生。跨线程恢复只能由声明线程安全的调度器执行。

取消是协作式操作。v0.1 建议通过 `CancellationToken` 标准库类型表达，不允许异步强制终止任意脚本帧。

## 17. 线程模型

默认每个 ExecutionContext 由一个宿主线程拥有。宿主进入脚本前必须通过运行时 enter/leave API 建立线程状态。

v0.1 规则：

- 脚本不能直接创建 OS 线程；
- Runtime 可以有多个上下文并行执行，但共享对象必须遵守运行时同步策略；
- 普通脚本对象默认不是线程安全的；
- GC 必须知道所有进入 Runtime 的线程；
- Native Binding 必须声明主线程、渲染线程、任意线程或特定阶段要求；
- 违反线程要求必须在 Debug 中诊断，在所有构建中安全失败。

Deterministic Profile 默认单线程权威模拟。并行任务必须通过确定性 job graph 和稳定归并规则实现。

## 18. Capability 与沙箱

模块加载时获得 CapabilitySet。每个 Native Binding 声明所需能力。

调用检查可以在编译期、模块验证期和运行时分层完成，但不可信模块的运行时边界不得省略最终检查，除非模块已经被可信签名和隔离加载。

建议能力层级：

```text
engine.ecs.read
engine.ecs.write
engine.scene.spawn
engine.audio.play
engine.ui.modify
filesystem.assets.read
filesystem.user.write
network.client
network.server
platform.clipboard
```

能力必须最小授权。导入一个模块不自动继承其全部宿主权限。

## 19. 执行预算

不可信或 Mod 模块必须支持：

- 指令/基本块预算；
- 原生调用预算；
- 分配字节预算；
- 活跃对象/任务上限；
- 最大递归深度；
- 每 Tick 恢复次数；
- 字符串和容器最大容量；
- 可选总墙上时间监控。

权威限制应使用确定性计数。墙上时间只作为宿主保护，不作为可回放脚本语义。

预算超限产生 `BudgetExceeded` 终止状态或可配置脚本异常。默认不允许脚本捕获后无限继续消耗预算。

## 20. 元数据与反射

运行时使用 Descriptor + Facet：

```text
TypeDescriptor
  ScriptTypeFacet
  NativeBindingFacet
  SerializationFacet
  DebugFacet
  EditorFacet
  DeterminismFacet
```

反射只暴露被元数据允许的成员。v0.1 不支持通过反射生成新代码、修改类型布局或绕过访问控制。

反射调用使用 `ScriptValue` 等通用接口时必须执行完整类型和 Capability 检查。

## 21. 热重载

### 21.1 稳定身份

可热重载实体必须使用稳定 ID：

- Module Stable ID；
- Type StableSchemaId；
- Field StableFieldId；
- Function StableFunctionId；
- 可选 StateMachine StableStateId。

显示名称不等于稳定身份。

### 21.2 函数替换

所有可替换函数通过稳定 `FunctionEntry` 间接调用：

```text
FunctionEntry
  FunctionId
  BackendKind
  entry point or bytecode body
  DebugInfo
  version
  active call count/epoch
```

加载新模块版本后，运行时原子替换入口。已经进入旧函数的调用可以运行到安全退出；旧代码在无活动帧后回收。

### 21.3 对象迁移

对象迁移按 StableFieldId 执行：

- 相同兼容字段复制；
- 新字段使用默认值或迁移表达式；
- 删除字段丢弃；
- 允许的数值扩大转换按规范执行；
- 不兼容字段要求显式迁移函数；
- 任一必要迁移失败则整个模块重载回滚。

迁移不得读取未初始化的新对象字段。

### 21.4 活动协程

v0.1 将活动协程迁移分级：

- Level 0：存在旧版本挂起协程时拒绝重载；
- Level 1：仅函数体改变且状态布局兼容时重绑定；
- Level 2：通过 StableStateId 和显式迁移转换状态机。

首个实现应至少支持 Level 0，并清晰报告阻塞重载的任务。

## 22. 确定性运行档位

Deterministic Context 包含：

```text
TickIndex
FixedDelta
SeededRng streams
Recorded inputs
Deterministic scheduler state
Module/content hashes
State hash service
```

必须禁止或控制：

- 墙上时间；
- OS 随机数；
- 无序容器遍历；
- 区域设置；
- 文件系统枚举顺序；
- 网络到达时间；
- 非稳定线程竞态；
- Fast Math；
- 未认证原生函数。

GC 调度、任务唤醒和事件分发必须使用稳定计数和顺序。

状态哈希应基于逻辑字段和稳定 ID，不得包含对象地址、填充字节、哈希表随机种子或调试数据。

## 23. 运行时失败分类

运行时必须区分：

- 可捕获脚本异常；
- 无效宿主句柄；
- Capability 拒绝；
- 预算超限；
- 字节码验证失败；
- ABI 不兼容；
- 热重载迁移失败；
- Runtime 内部错误。

前七类必须可控地返回宿主，不应导致未定义行为。内部错误可以触发 fail-fast，但必须尽可能输出模块、函数、版本和序列点信息。

## 24. 实现验证

运行时符合性测试至少覆盖：

- 值复制与引用身份；
- 含引用结构体的 GC；
- 数组边界和协变检查；
- 移动/非移动 GC 下的宿主句柄；
- 异常经过字节码、AOT 和原生绑定；
- 挂起协程保持对象存活；
- Capability 与预算无法绕过；
- 热重载成功、拒绝和回滚；
- 确定性状态哈希和回放；
- 调试器查看优化前后局部变量。
