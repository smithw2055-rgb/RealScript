# RealScript 规范文档

- 规范版本：Draft v0.1
- 语言版本：0.1
- 文档状态：设计基线，尚未冻结二进制兼容性
- 适用实现：编译器、字节码 VM、C++17 AOT 后端、调试器、语言服务器和宿主集成

## 1. 规范目的

本目录把总体架构中的设计意图转化为可以实现、验证和测试的规范性约束。

总体设计回答“系统为什么这样设计”；本目录回答：

- 源代码应当如何解析和绑定；
- 类型、表达式和语句具有怎样的确定语义；
- HIR/MIR 与后端之间必须保持哪些不变量；
- 字节码模块如何验证和执行；
- C++ 宿主如何通过稳定 ABI 与脚本交互；
- GC、异常、协程、热重载和确定性模式如何工作；
- 编译器、VM、AOT 和调试器如何判定是否符合规范。

## 2. 规范性术语

本文档采用以下关键词：

- **必须（MUST）**：符合规范的实现不可违反；
- **禁止（MUST NOT）**：符合规范的实现不可执行该行为；
- **应当（SHOULD）**：除非存在明确且记录在案的原因，否则应遵循；
- **不应（SHOULD NOT）**：通常不应采用，偏离时需要说明；
- **可以（MAY）**：可选能力，不影响最低符合性；
- **实现定义（implementation-defined）**：实现可以选择，但必须公开并保持稳定；
- **未指定（unspecified）**：实现无需公开选择，程序不得依赖具体结果；
- **无效程序（ill-formed program）**：编译器必须诊断且不得生成可执行模块；
- **验证失败模块**：字节码加载器必须拒绝执行。

RealScript 不允许以 C++ 未定义行为作为脚本语义。所有可观察结果必须由语言规范、MIR 规范或运行时 intrinsic 明确定义。

## 3. 文档结构

| 文档 | 内容 | 当前状态 |
|---|---|---|
| [LANGUAGE_SPEC.md](LANGUAGE_SPEC.md) | 源文件、模块、类型、转换、表达式、语句、对象、泛型、异常、协程、属性和确定性约束 | Draft v0.1 |
| [MIR_SPEC.md](MIR_SPEC.md) | Typed MIR、控制流、类型规则、检查、异常区、序列点和优化合法性 | Draft v0.1 |
| [RUNTIME_MODEL.md](RUNTIME_MODEL.md) | 值与对象模型、GC、字符串、数组、句柄、任务、线程、调试、预算和热重载 | Draft v0.1 |
| [BYTECODE_AND_ABI.md](BYTECODE_AND_ABI.md) | 字节码逻辑模型、验证器、调用约定、Runtime C ABI、Native Thunk 和版本兼容 | Draft v0.1 |
| [BYTECODE_FORMAT_V0.md](BYTECODE_FORMAT_V0.md) | Phase 2A–3B 已实现的 `.rsbc` 0.2 物理编码、类型描述符、指令记录和安全边界 | Implemented Draft v0.1 |
| [EMBEDDING_AND_OBSERVABILITY_V0.md](EMBEDDING_AND_OBSERVABILITY_V0.md) | Phase 2C 链接镜像、宿主绑定、Trace、统计与嵌入门面 | Implemented Draft v0.1 |
| [MANAGED_HEAP_GC_V0.md](MANAGED_HEAP_GC_V0.md) | Phase 3A ObjectRef、托管对象、精确根、写屏障与增量 Mark/Sweep | Implemented Draft v0.1 |
| [OBJECT_MODEL_V0.md](OBJECT_MODEL_V0.md) | Phase 3B class、稳定 TypeId、字段布局、对象操作与精确引用图 | Implemented Draft v0.1 |
| [../ENGINE_DESIGN.md](../ENGINE_DESIGN.md) | 总体架构、参考项目、性能目标和实施路线 | Draft v0.1 |

## 4. 符合性档位

实现可以声明以下符合性档位：

### 4.1 Core Language

必须实现：

- 词法与语法；
- 模块和名称绑定；
- 基础类型、枚举、结构体、类和接口；
- 函数、控制流和异常语义；
- 类型检查与诊断；
- Typed MIR 输出或等价的可验证中间表示。

### 4.2 Bytecode Runtime

在 Core Language 之上必须实现：

- 类型化字节码生成；
- 模块结构和完整验证；
- 调用栈、异常、对象和 GC；
- 资源预算和安全失败；
- 规范要求的调试序列点。

### 4.3 Native AOT

在 Core Language 之上必须实现：

- 与字节码一致的求值顺序、溢出、空值、边界和异常行为；
- Runtime C ABI；
- 类型化 Native Thunk；
- 字节码/AOT 差分测试；
- 不依赖宿主编译器未定义行为。

### 4.4 Deterministic Profile

必须额外实现：

- 固定 Tick 时间来源；
- 显式种子随机数；
- 稳定容器迭代规则；
- 能力白名单；
- 确定性 GC 调度触发；
- 回放输入、模块哈希和状态哈希验证。

## 5. 版本维度

RealScript 分离以下版本号：

- `language_version`：源语言语法和静态语义；
- `mir_version`：MIR 指令和验证规则；
- `bytecode_format_version`：`.rsbc` 物理格式；
- `runtime_abi_version`：生成代码与运行时函数表；
- `metadata_schema_version`：Descriptor 与 Facet 序列化格式；
- `debug_info_version`：序列点、变量位置和内联信息格式。

修改一个维度不得无理由提升其他版本。模块加载器必须逐项检查兼容性，而不是只检查单个“引擎版本”。

## 6. 语义优先级

出现冲突时，优先级从高到低为：

1. 对应版本的正式规范；
2. 规范附带的一致性测试；
3. 参考解释器的可观察行为；
4. 总体设计文档；
5. 单个后端或平台的当前实现。

在规范冻结前，参考解释器用于发现设计问题，但其缺陷不自动成为语言特性。

## 7. 变更流程

任何影响可观察行为的变更应当：

1. 提交设计说明或 RFC；
2. 更新规范文本；
3. 增加正向、负向和差分测试；
4. 标明兼容性影响；
5. 决定是否提升语言、MIR、字节码或 ABI 版本；
6. 在实现完成后再将条款从 Draft 标记为 Stable。

## 8. v0.1 尚未冻结的事项

- Unicode 标识符是否进入首个稳定版本；
- 整数运算在 Debug 构建下的默认 checked 策略；
- 引用类型泛型共享代码与完全单态化的边界；
- `.rsbc` 0.2 已实现物理编码是否直接演进为首个稳定格式；
- 异常过滤器和 `finally` 的首版范围；
- `async` 的取消和结构化并发 API；
- 热重载中活动协程状态迁移的支持等级；
- 固定点基础类型进入核心语言还是标准库。

这些问题可以改变实现策略，但不得破坏已经写明的跨后端语义一致性原则。
