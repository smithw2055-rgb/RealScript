# RealScript 字节码与原生 ABI 规范

- 规范版本：Draft v0.1
- `bytecode_format_version`：0.1
- `runtime_abi_version`：0.1
- 状态：逻辑结构和安全约束已定义，最终物理编码尚未冻结

## 1. 范围

本文档定义：

- `.rsbc` 字节码模块的逻辑结构；
- 类型化寄存器字节码的执行约束；
- 模块验证器必须执行的安全检查；
- 字节码 VM 的调用约定、异常和调试信息；
- C++17 AOT 生成代码与 Runtime 之间的稳定 C ABI；
- Native Binding 与 Native Thunk；
- 版本、热重载和跨平台兼容规则。

字节码物理编码可以在 v0.1 实现期间调整，但不得削弱本文规定的验证与语义要求。

## 2. 设计原则

1. 字节码必须在执行前完成完整验证；
2. VM 指令必须类型化，不能把动态标签检查作为主要执行模型；
3. 字节码和 AOT 必须共享 Runtime ABI 与语义 intrinsic；
4. 外部模块不能直接保存或调用任意进程地址；
5. C++ 边界必须通过稳定 C ABI，而不是跨编译器 C++ ABI；
6. 高频调用使用生成的类型化 thunk，反射调用使用通用慢路径；
7. 调试、异常和 profiler 使用相同 FunctionId 与序列点；
8. 所有格式均显式版本化并可拒绝不兼容模块。

## 3. `.rsbc` 模块容器

### 3.1 文件头

建议首版文件头：

```text
RsbcHeader
  magic[4]                  // "RSBC"
  bytecode_major: u16
  bytecode_minor: u16
  language_major: u16
  language_minor: u16
  mir_major: u16
  mir_minor: u16
  runtime_abi_major: u16
  runtime_abi_minor: u16
  metadata_schema_major: u16
  metadata_schema_minor: u16
  flags: u32
  section_count: u32
  module_stable_id[16]
  content_hash[32]
```

整数以 little-endian 编码。加载器必须按字节读取，不得把未对齐缓冲区直接解释为宿主结构体。

`content_hash` 覆盖规范化模块内容，用于缓存、签名、回放和多人同步校验。具体哈希算法由格式版本规定，建议 v0.1 使用 SHA-256。

### 3.2 Section Directory

```text
RsbcSectionEntry
  kind: u32
  flags: u32
  offset: u64
  size: u64
  uncompressed_size: u64
  alignment: u32
  checksum: u32
```

加载器必须检查：

- offset/size 不溢出；
- section 位于文件范围内；
- section 不非法重叠；
- alignment 合法；
- 压缩后大小受资源限制；
- checksum 和内容哈希匹配。

### 3.3 标准 Section

```text
MODULE_INFO
STRING_TABLE
TYPE_TABLE
MEMBER_TABLE
SIGNATURE_TABLE
CONSTANT_TABLE
IMPORT_TABLE
EXPORT_TABLE
FUNCTION_TABLE
BYTECODE_BODIES
EXCEPTION_TABLE
GC_MAPS
DEBUG_INFO
CAPABILITIES
DETERMINISM_INFO
HOT_RELOAD_SCHEMA
NATIVE_BINDINGS
SIGNATURE
```

未知非关键 section 可以忽略。未知且带 `REQUIRED` 标记的 section 必须导致加载失败。

## 4. 模块身份和依赖

`MODULE_INFO` 至少包含：

- 模块稳定 ID；
- 模块显示名称；
- 模块版本；
- 编译器版本；
- 构建档位；
- 源内容哈希；
- 依赖模块列表；
- 所需 Runtime features；
- Capability 请求；
- Determinism 分类。

依赖项使用模块稳定 ID 和兼容版本范围，不只使用名称。

加载器必须在执行前：

1. 解析完整依赖图；
2. 检查循环依赖；
3. 选择兼容模块版本；
4. 验证导入签名；
5. 检查 Capability；
6. 检查确定性档位；
7. 完成全部字节码验证。

## 5. 标识符和表

字节码内部不得以源代码字符串执行成员查找。指令引用索引化表项：

```text
TypeIndex
FieldIndex
FunctionIndex
SignatureIndex
ConstantIndex
StringIndex
BindingIndex
```

表项必须包含稳定身份和当前模块快速索引。快速索引只在模块实例内有效。

所有索引在验证时必须进行范围检查。执行器可以在成功验证后使用省略重复范围检查的内部表示。

## 6. 函数体

逻辑函数体：

```text
BytecodeFunction
  FunctionId
  SignatureIndex
  register_count
  parameter_count
  local_count
  max_call_args
  flags
  instructions[]
  exception_regions[]
  gc_map_entries[]
  sequence_points[]
  local_debug_entries[]
```

寄存器在函数内编号。每个寄存器具有验证器推导或显式记录的静态类型。

建议按用途区分：

- 参数寄存器；
- 局部/临时寄存器；
- 异常寄存器；
- 返回寄存器。

函数进入时：

- 参数寄存器由调用约定初始化；
- 需要默认初始化的局部按类型初始化；
- 未初始化临时寄存器不得读取；
- `out` 参数必须在正常返回前赋值。

## 7. 指令编码

v0.1 逻辑指令采用：

```text
Instruction
  opcode
  operand_format
  flags
  operands[]
```

最终物理格式可以采用固定 32 位首字、扩展操作数字或压缩变长编码。无论物理格式如何，都必须：

- 可以安全跳过或拒绝未知 opcode；
- 可以验证完整指令边界；
- 跳转只能指向指令起点；
- 操作数宽度不依赖宿主指针宽度；
- 反汇编器能无损显示逻辑指令。

建议保留 `opcode` 8 或 16 位空间，并按类别分段，避免首版耗尽编码空间。

## 8. 指令集类别

### 8.1 数据移动

```text
MOV
LOAD_CONST
LOAD_DEFAULT
LOAD_FIELD
STORE_FIELD
LOAD_ELEMENT
STORE_ELEMENT
LOAD_LENGTH
```

### 8.2 数值

```text
ADD_I32_WRAP
ADD_I32_CHECKED
ADD_I64_WRAP
ADD_I64_CHECKED
ADD_F32
ADD_F64
SUB_*
MUL_*
DIV_*
REM_*
NEG_*
SHL_*
SHR_*
BIT_AND/OR/XOR/NOT
```

类型和溢出语义必须编码在 opcode 或不可歧义的操作数中。

### 8.3 比较与分支

```text
CMP_EQ_*
CMP_LT_*
CMP_LE_*
BR
BR_TRUE
BR_FALSE
SWITCH
```

字符串内容比较、引用身份比较和宿主句柄比较使用不同 opcode 或 intrinsic。

### 8.4 转换和类型

```text
CONV_CHECKED
CONV_WRAP
IS_TYPE
CAST_CLASS
CAST_INTERFACE
BOX
UNBOX
NULLABLE_WRAP
NULLABLE_HAS_VALUE
NULLABLE_GET_VALUE
```

### 8.5 对象和调用

```text
NEW_OBJECT
NEW_ARRAY
CALL_STATIC
CALL_DIRECT
CALL_VIRTUAL
CALL_INTERFACE
CALL_DELEGATE
CALL_NATIVE
RET
RET_VALUE
```

### 8.6 检查和异常

```text
CHECK_NOT_NULL
CHECK_BOUNDS
CHECK_VALID_HANDLE
CHECK_CAPABILITY
CHECK_BUDGET
THROW
RETHROW
LEAVE
END_FINALLY
```

### 8.7 调试和调度

```text
SAFEPOINT
SEQUENCE_POINT
YIELD_CHECK
AWAIT_PREPARE
AWAIT_SUSPEND
AWAIT_RESUME
```

部分调试指令可以在加载后转换为 side table，但逻辑序列点必须保留。

## 9. 控制流

分支目标使用函数内指令索引或字节偏移，格式版本必须选择一种且保持一致。

验证规则：

- 目标必须位于当前函数；
- 目标必须是指令起点；
- 不允许跳入异常区域、`finally` 或受保护作用域中部；
- 每条可达路径必须到达返回、抛出或合法循环；
- 返回类型和函数签名必须匹配；
- `leave` 必须执行所需清理区域。

加载器应把字节码构造成基本块图以完成验证和可选快速解释布局。

## 10. 类型验证

验证器对每个函数执行数据流分析：

- 寄存器初始化状态；
- 寄存器静态类型；
- 借用生命周期；
- 可空性要求；
- `out` 参数赋值；
- 调用参数模式；
- 返回值；
- 异常边界；
- GC 引用位置；
- Capability 和 Determinism 传播。

控制流合流处，寄存器状态必须具有唯一兼容类型。不得通过把不兼容类型合并为 `object` 绕过显式装箱。

验证失败模块必须被拒绝，不能“尽量执行”。

## 11. 字节码安全验证清单

加载器必须检查至少以下项目：

- 文件和 section 边界；
- 表索引；
- 字符串和常量编码；
- 类型布局无递归溢出；
- 函数签名存在且合法；
- 指令边界和 opcode；
- 跳转目标；
- 寄存器数量和索引；
- 初始化和类型数据流；
- 字段访问权限；
- 数组元素类型；
- 借用不逃逸；
- GC map 完整；
- 异常区域正确嵌套；
- 调用深度和模块大小限制；
- Capability；
- Determinism；
- 导入/导出签名；
- Runtime ABI 版本；
- 数字压缩和解压资源上限；
- 签名或可信来源策略。

已验证模块可以被转换为内部快速格式，但缓存必须绑定原始内容哈希、验证器版本和运行时版本。

## 12. VM 调用约定

逻辑调用约定：

1. 调用方按签名顺序准备参数；
2. 按值参数复制；
3. `in/ref/out` 传递受管位置引用；
4. 目标对象作为隐藏 `this` 参数；
5. 调用创建新脚本帧；
6. 正常返回写入返回寄存器；
7. 异常返回写入 ExecutionContext 异常状态并开始展开。

VM 帧示例：

```text
VmFrame
  caller
  FunctionEntry
  instruction pointer
  registers
  borrow metadata
  exception region cursor
  debug scope cursor
```

寄存器存储可以按类型分区或使用统一槽，但 GC 和验证器必须知道精确类型。

## 13. GC Map 与 Safepoint

每个 safepoint 必须能够确定：

- 哪些寄存器包含对象引用；
- 哪些寄存器包含含引用结构体；
- 哪些栈/帧位置包含引用；
- 哪些临时 Native Thunk 参数已经注册为根；
- 哪些借用指向移动对象内部。

VM 可以从验证类型推导 GC map。AOT 必须生成 stack map 或影子栈记录。

在可能移动 GC 中，跨 safepoint 的对象内部借用必须使用可重定位描述，或在受控范围 pin 对象。禁止无界 pin。

## 14. 异常表

每个函数的异常表包含：

```text
ExceptionRegion
  protected_start
  protected_end
  handlers[]

Handler
  kind: catch | finally
  catch_type
  target
```

区间使用半开范围。区域必须正确嵌套。

异常匹配使用脚本类型系统。VM 和 AOT 必须生成相同 `catch` 选择和 `finally` 顺序。

## 15. Debug Info

调试 section 至少可以描述：

- 源文件表和内容哈希；
- 函数到源码范围；
- 可停止序列点；
- 隐藏序列点；
- 局部变量名、类型、作用域；
- 寄存器/栈位置表达式；
- 内联调用链；
- 异步状态机与原函数映射；
- 热重载版本映射。

Release 模块可以剥离局部变量名，但应保留 FunctionId、异常位置和可选 profiler 映射。

## 16. Runtime C ABI

### 16.1 ABI 基本规则

- 使用 `extern "C"`；
- 使用固定宽度整数；
- 不跨边界传递 STL 类型；
- 不跨边界传递 C++ 引用、虚类、异常或 RTTI；
- 结构体显式声明 `size` 和 `version`；
- 所有句柄为不透明整数或不透明指针类型；
- 所有字符串携带长度；
- 所有数组使用 pointer + count，且所有权明确；
- 函数返回统一状态码，脚本异常保存在 ExecutionContext。

### 16.2 基础类型

```cpp
extern "C" {

using RsRuntimeHandle = void*;
using RsContextHandle = void*;
using RsObjectHandle = std::uint64_t;
using RsStringHandle = std::uint64_t;
using RsTypeId = std::uint64_t;
using RsFunctionId = std::uint64_t;
using RsBindingId = std::uint64_t;

struct RsSlice {
    const void* data;
    std::uint64_t size;
};

enum class RsStatus : std::uint32_t {
    Ok = 0,
    ScriptException,
    InvalidArgument,
    InvalidHandle,
    CapabilityDenied,
    BudgetExceeded,
    AbiMismatch,
    InternalError
};

}
```

实际公共头文件必须可由 C 编译器使用时，应避免 C++ `enum class`，改用 `uint32_t` 常量。上例只用于表达语义。

### 16.3 版本化函数表

```cpp
struct RsRuntimeApiV1 {
    std::uint32_t size;
    std::uint16_t abi_major;
    std::uint16_t abi_minor;
    void* user_data;

    RsStatus (*object_alloc)(
        RsContextHandle,
        RsTypeId,
        RsObjectHandle* out_object);

    RsStatus (*string_create_utf8)(
        RsContextHandle,
        const char* data,
        std::uint64_t size,
        RsStringHandle* out_string);

    RsStatus (*throw_exception)(
        RsContextHandle,
        RsObjectHandle exception_object);

    RsStatus (*host_handle_validate)(
        RsContextHandle,
        std::uint64_t handle,
        RsTypeId expected_type);

    void (*gc_write_barrier)(
        RsContextHandle,
        RsObjectHandle owner,
        RsObjectHandle value);
};
```

生成模块只能访问函数表中由 `size` 和版本确认存在的成员。

ABI minor 版本只能追加向后兼容字段；破坏现有字段语义、顺序或类型必须提升 major。

### 16.4 所有权

每个 ABI 参数必须标注概念所有权：

- borrowed：调用期间有效；
- retained：接收方需要显式 retain/release；
- transferred：所有权转移；
- rooted：作为 GC 根保持；
- weak：不保持存活。

公共头文件和生成绑定元数据必须使用统一注解或命名约定。

## 17. AOT 模块接口

生成的 AOT 模块导出单一入口：

```cpp
extern "C" RsStatus rs_module_query_v1(
    const RsRuntimeApiV1* runtime_api,
    RsModuleExportsV1* out_exports);
```

`RsModuleExportsV1` 包含：

- 模块身份和版本；
- 所需 ABI；
- 类型和函数描述表；
- 模块初始化/关闭函数；
- FunctionEntry 初始化表；
- GC layout/stack map 注册；
- Debug Info 注册；
- 热重载 Schema；
- 内容哈希和确定性认证信息。

加载顺序：

1. 动态库或静态对象被宿主加载；
2. 调用 query；
3. 校验 ABI、模块 ID 和内容哈希；
4. 注册只读描述；
5. 解析导入；
6. 注册 GC/Debug 信息；
7. 创建模块实例；
8. 执行受控初始化。

初始化失败必须完整回滚已经注册的模块私有状态。

## 18. FunctionEntry

所有跨模块或可热重载调用通过稳定入口：

```cpp
struct RsFunctionEntryV1 {
    std::uint32_t size;
    std::uint32_t backend_kind;
    RsFunctionId function_id;
    std::uint64_t version;
    void* entry_point;
    const void* backend_data;
    const void* debug_info;
};
```

调用方不得永久缓存 `entry_point` 并绕过版本入口。运行时可以使用 inline cache，但必须在模块 epoch 变化时失效。

`backend_kind` 可以是：

```text
Bytecode
NativeAot
NativeJit
HostBinding
Invalidated
```

## 19. Native Binding

每个绑定具有生成描述：

```text
NativeBindingDescriptor
  BindingId
  script signature
  C++ target metadata
  parameter marshal plan
  return marshal plan
  Capability requirement
  Thread requirement
  Determinism classification
  Effects
  exception mapping
```

绑定注册使用稳定 BindingId，不使用运行时字符串查找作为热路径。

### 19.1 类型化 Native Thunk

逻辑 thunk 签名：

```cpp
using RsNativeThunkV1 = RsStatus (*)(
    RsContextHandle context,
    const RsCallFrameV1* call,
    RsReturnSlotV1* result);
```

Thunk 必须：

1. 验证或信任已验证签名布局；
2. 建立临时 GC roots；
3. 检查 Capability；
4. 检查线程/阶段；
5. 验证宿主句柄；
6. 解包类型化参数；
7. 调用 C++ 目标；
8. 捕获并映射允许的 C++ 异常；
9. 写入返回值或脚本异常；
10. 释放临时 pin、lease 和借用。

高频 thunk 不得创建参数 `Variant[]`、执行名称查找或使用通用反射分派。

### 19.2 通用反射调用

低频工具路径可以提供：

```cpp
RsStatus rs_invoke_dynamic(
    RsContextHandle,
    RsFunctionId,
    const RsScriptValue* args,
    std::uint32_t arg_count,
    RsScriptValue* result);
```

该路径必须执行完整类型检查，且不作为性能基准的热调用路径。

## 20. C++ AOT 代码生成约束

生成代码必须：

- 使用固定宽度类型；
- 通过无符号运算或 intrinsic 实现 wrap 溢出；
- 显式检查 checked 溢出；
- 显式检查除零、空值和边界；
- 不依赖有符号溢出；
- 不违反 strict aliasing；
- 不读取未初始化内存或填充字节；
- 不依赖参数求值顺序；
- 不让脚本异常作为 C++ 异常跨模块传播；
- 在 safepoint 更新影子栈或 stack map；
- 通过 Runtime ABI 访问可变运行时服务。

建议生成简单、规则化的 C++，而不是追求可供人手维护的源代码美观。

## 21. Capability 与可信等级

模块可以处于：

- `TrustedBuiltin`；
- `TrustedSigned`；
- `ProjectScript`；
- `SandboxedMod`；
- `Untrusted`。

可信等级影响：

- 是否每次 Native Call 执行 Capability 检查；
- 是否允许 AOT/JIT；
- 预算上限；
- 可加载 Binding 集；
- 文件、网络和平台 API；
- 调试器访问。

即使是可信模块，也不得绕过 ABI 版本和类型安全检查。

## 22. 热重载兼容

新旧模块比较：

- 模块稳定 ID；
- 导出函数签名；
- 类型 StableSchemaId；
- 字段 StableFieldId 和类型；
- 接口槽；
- 泛型实例；
- 状态机 Schema；
- Capability；
- Runtime ABI；
- Determinism 分类。

兼容变化可以直接替换 FunctionEntry。布局变化必须执行迁移。接口或导出 ABI 破坏必须拒绝在线替换，除非所有依赖模块同步重载。

## 23. 版本兼容规则

### 23.1 Language Version

编译器可以支持多个源语言版本。模块必须记录实际版本。

### 23.2 Bytecode Format

- major 不同：默认拒绝；
- minor 较新：只有加载器声明支持所有 required feature 时接受；
- 未知 required section/opcode：拒绝。

### 23.3 Runtime ABI

- major 必须相同；
- 生成模块要求的 minor 不得高于 Runtime 提供值，除非通过 feature query 确认兼容；
- 函数表以 `size` 防止越界访问。

### 23.4 Metadata Schema

工具可以读取已知子集，但执行器不得在缺失运行所需 Facet 时加载模块。

## 24. 跨平台要求

字节码：

- 固定字节序；
- 固定整数宽度；
- 不保存裸地址；
- 不保存宿主 `size_t`；
- 浮点常量按 IEEE 位模式；
- 对齐只用于文件 section，不等于对象布局。

AOT：

- 每个平台重新通过平台 C++ 编译器生成目标代码；
- Runtime C ABI 按目标平台调用约定实现；
- 不把某平台 AOT 对象作为另一平台可移植产物；
- 主机平台限制 JIT 时必须使用 AOT 或字节码。

## 25. 测试要求

至少建立以下测试：

- 畸形文件、截断、整数溢出和 section 重叠；
- 未知 opcode 与 required feature；
- 非法跳转和异常区域；
- 未初始化寄存器；
- 类型混淆和伪造引用；
- 借用跨 `await`；
- 错误 GC map；
- Capability 绕过；
- 递归和分配预算；
- Native Thunk 参数/返回值；
- C++ 异常映射；
- ABI major/minor 组合；
- 字节码与 AOT 差分；
- 热重载入口失效；
- 不同平台相同逻辑状态哈希。

验证器应接受 fuzzing。任意输入都只能产生“成功加载”或“受控拒绝”，不能崩溃、越界、无限分配或执行未验证代码。

## 26. v0.1 未冻结项

- 最终指令物理编码；
- section 压缩算法；
- 模块签名格式和证书策略；
- Runtime handle 是整数还是不透明指针；
- AOT stack map 的平台格式；
- 接口槽使用顺序索引还是 StableMemberId 间接；
- 引用类型泛型共享代码 ABI；
- Debug Info 是否独立 `.rsdbg` 存储；
- JIT 代码缓存和签名策略。

未冻结项不得影响已经确定的类型安全、验证、语义一致性和稳定 C ABI 原则。
