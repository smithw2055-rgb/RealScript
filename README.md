# RealScript

RealScript 是一门面向现代游戏引擎的嵌入式强类型脚本语言与运行时。

它采用接近 C# 的表达方式，以 C++17 游戏引擎为主要宿主，长期目标包括：快速字节码解释、C++17 AOT、可选 LLVM ORC JIT、源码级调试、热重载、沙箱 Mod，以及固定 Tick 的确定性模拟。

> 当前状态：Draft v0.1 规范基线、Phase 1A 语言基础和 Phase 1B 控制流闭环已经完成。语言、字节码和 ABI 尚未冻结。

## 当前可运行能力

Phase 1A–1B 已经实现：

- 无外部依赖的 C++17/CMake 工程；
- `SourceText`、`TextSpan` 和 CRLF/LF 行列映射；
- 稳定诊断编号和错误累积；
- Lexer、注释、字符串和数值 Token；
- `module`、`import` 和 C# 风格函数解析；
- Pratt 表达式解析与右结合赋值；
- `if` / `else`、`while` 和可变局部变量；
- `void`、`bool`、`int`、`string` 基础类型描述；
- 参数、局部作用域、名称解析和类型检查；
- 确定赋值与全路径返回分析；
- 多基本块 Typed MIR、显式局部槽位和块参数；
- `&&` / `||` 短路控制流；
- 类型、目标块、支配关系和终结指令 MIR 验证；
- `rsc` Token、检查和 MIR 输出模式；
- Linux/Windows GitHub Actions 构建；
- 13 项前端与 MIR conformance cases，包括快照 fixture。

实现边界见：

- [Phase 1A — C++17 Language Foundation](docs/roadmap/PHASE_1A.md)
- [Phase 1B — Control Flow and Multi-Block MIR](docs/roadmap/PHASE_1B.md)

## 快速开始

### 构建

```bash
cmake -S . -B build \
  -DREALSCRIPT_BUILD_TESTS=ON \
  -DREALSCRIPT_WARNINGS_AS_ERRORS=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

### 示例

```csharp
module Demo.Flow;

int clamp(int value, int maximum)
{
    int result;
    if (value < 0)
        result = 0;
    else
        result = value;

    while (result > maximum)
        result = result - 1;

    return result;
}

bool guarded(bool enabled, int value)
{
    return enabled && value > 0;
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

打印并验证 Typed MIR：

```bash
rsc sample.rs --mir
```

短路表达式会生成控制流和块参数：

```text
%left:bool = load.local 0
%false:bool = const.bool false
br %left, bb_rhs, bb_merge(%false)

bb_rhs:
  %right:bool = ...
  jmp bb_merge(%right)

bb_merge(%result:bool):
  ret %result
```

## Phase 1B 的状态模型

源码变量是可变的。当前 MIR 使用类型化 `load.local` / `store.local` 表示可变状态，并使用块参数表示控制流表达式的合流值。

这比在 Lowerer 中直接构造完整 SSA 更简单可靠，同时保留后续 `mem2reg` 优化空间：优化器可以根据支配关系把不逃逸的局部槽位提升为 SSA，而不改变语言语义。

## 当前实现限制

尚未实现：

- `class`、`struct`、`interface`、`enum`；
- 函数调用绑定、重载和跨模块符号解析；
- 完整数值类型、转换和提升；
- `break`、`continue`、`for`、`foreach`、`switch`；
- 异常和协程；
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
Binder / Type Checker / Flow Analysis
       │
       ▼
Typed HIR
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
- [Phase 1A 实现说明](docs/roadmap/PHASE_1A.md)
- [Phase 1B 实现说明](docs/roadmap/PHASE_1B.md)

## 路线图

- [x] 总体语言和运行时方向；
- [x] Draft v0.1 语言、MIR、运行时、字节码与 ABI 规范；
- [x] Phase 1A：C++17 工程、Lexer、Parser、Binder 和单块 MIR；
- [x] Phase 1B：赋值、控制流、流分析、多块 MIR、块参数和验证器；
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
