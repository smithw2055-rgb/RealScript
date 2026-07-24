# RealScript

RealScript 是一门面向现代游戏引擎的嵌入式强类型脚本语言与运行时。

它采用接近 C# 的表达方式，以 C++17 游戏引擎为主要宿主，长期目标包括：快速字节码解释、C++17 AOT、可选 LLVM ORC JIT、源码级调试、热重载、沙箱 Mod，以及固定 Tick 的确定性模拟。

> 当前状态：已经完成 Draft v0.1 规范基线和第一条可编译前端垂直切片。语言、字节码和 ABI 尚未冻结。

## 当前可运行能力

Phase 1A 已经实现：

- 无外部依赖的 C++17/CMake 工程；
- `SourceText`、`TextSpan` 和 CRLF/LF 行列映射；
- 稳定诊断编号和错误累积；
- Lexer、注释、字符串和数值 Token；
- `module`、`import` 和 C# 风格函数解析；
- Pratt 表达式解析和错误恢复；
- `void`、`bool`、`int`、`string` 基础类型描述；
- 参数、局部作用域、名称解析和返回类型检查；
- 单基本块 SSA-like Typed MIR；
- `rsc` Token、检查和 MIR 输出模式；
- Linux/Windows GitHub Actions 构建；
- 7 项前端 conformance tests。

完整边界见 [Phase 1A 实现说明](docs/roadmap/PHASE_1A.md)。

## 快速开始

### 构建

```bash
cmake -S . -B build \
  -DREALSCRIPT_BUILD_TESTS=ON \
  -DREALSCRIPT_WARNINGS_AS_ERRORS=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

### Phase 1A 示例

```csharp
module Demo.Core;
import Engine.Math;

int add(int a, int b)
{
    int doubled = b * 2;
    return a + doubled;
}
```

检查源码：

```bash
rsc sample.rs
```

打印 Token：

```bash
rsc sample.rs --tokens
```

打印 Typed MIR：

```bash
rsc sample.rs --mir
```

示例输出：

```text
module Demo.Core

func @add(int, int) -> int {
bb0:
  %0:int = param 0
  %1:int = param 1
  %2:int = const.i32 2
  %3:int = mul.i32 %1, %2
  %4:int = add.i32 %0, %3
  ret %4
}
```

## 当前实现限制

Phase 1A 只验证前端分层与语义闭环，尚未实现：

- `class`、`struct`、`interface`、`enum`；
- 字段、成员访问、赋值和可变局部变量；
- 函数调用绑定和重载解析；
- 完整数值类型、转换和提升；
- `if`、`while` 与短路 `&&`/`||`；
- 多基本块 MIR、控制流图与 Phi-like 参数；
- 字节码生成、VM 和原生 AOT。

未实现能力会得到明确诊断，不会使用占位运行时行为假装支持。

## 目标架构

```text
RealScript Source
       │
       ▼
Lexer / Parser / Incremental Syntax Tree
       │
       ▼
Binder / Type Checker
       │
       ▼
Typed HIR
       │
       ▼
SSA-like Typed MIR
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

## 设计原则

### C# 风格，但不复制完整 C#

RealScript 重点覆盖游戏脚本所需的类型安全、值类型、对象、接口、泛型、协程和工具体验，不兼容 CLR、CTS、IL 或完整 .NET 生态。

### 值类型优先

`struct` 将作为向量、颜色、变换、ECS 组件和小型配置的主要数据模型；`class` 用于行为对象、任务、流程状态和较小对象图。

### 明确定义语义

整数溢出、除零、求值顺序、空值、边界检查和异常行为必须由语言与 MIR 规范定义，不能继承 C++ 未定义行为。

### 稳定宿主边界

编辑器可以使用通用动态值，高频 C++ 调用则使用自动生成的类型化 Native Thunk 和版本化 Runtime C ABI。

### 工具是一等能力

调试器采用 DAP，语言服务采用 LSP。源码位置、Sequence Point、局部作用域和脚本 Shadow Stack 从编译器早期阶段纳入设计。

## 代码结构

```text
include/realscript/
  text/          SourceText 和 TextSpan
  diagnostics/   稳定诊断模型
  syntax/        Token、AST、Lexer 和 Parser
  semantic/      类型、Symbol、Bound Tree 和 Binder
  mir/           Typed MIR 与 Lowerer
src/              对应实现
tools/rsc/        编译器命令行
tests/            前端 conformance tests
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
- [Phase 1A 实现说明](docs/roadmap/PHASE_1A.md)

## 路线图

- [x] 总体语言和运行时方向；
- [x] Draft v0.1 语言、MIR、运行时、字节码与 ABI 规范；
- [x] C++17 工程骨架；
- [x] SourceText、诊断、Lexer 和 Parser；
- [x] 最小 Binder 与单基本块 Typed MIR；
- [x] `rsc` CLI、测试和跨平台 CI；
- [ ] Phase 1B：赋值、`if`/`while`、CFG、多基本块 MIR 和验证器；
- [ ] Phase 1C：函数调用、模块符号和增量编译；
- [ ] Phase 2：类型化寄存器字节码、验证器和解释器；
- [ ] Phase 3：对象模型、GC 和 C++ Native Binding；
- [ ] Phase 4：DAP、LSP 和热重载；
- [ ] Phase 5：C++17 AOT；
- [ ] Phase 6：确定性、性能优化和可选 JIT。

## 主要参考方向

RealScript 组合参考 AngelScript 的嵌入与调试接口、Luau 的 VM 性能工程、Unity IL2CPP 的 C++ AOT 路径、LLVM ORC 的可选 JIT，以及 DAP/LSP 的编辑器协议。

## License

许可证尚未确定。在许可证文件提交前，请勿假定本仓库代码或文档可用于再分发。
