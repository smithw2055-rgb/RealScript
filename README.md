# RealScript

RealScript 是一门面向现代游戏引擎的嵌入式脚本语言与运行时。

它采用接近 C# 的强类型语法，以 C++17 游戏引擎为主要宿主，同时提供快速解释执行、发布期原生编译、完整调试工具链、热重载和确定性模拟支持。

> 项目当前处于架构设计与基础设施引导阶段，语言和运行时接口尚未稳定。

## 项目目标

RealScript 希望在以下需求之间取得平衡：

- 接近 C# 的清晰、现代、易学习语法；
- 与 C++17 引擎低成本集成；
- 编辑期快速编译和字节码解释执行；
- 发布期生成 C++17 并由平台编译器完成 AOT 原生编译；
- 可选的桌面 LLVM ORC JIT 后端；
- 源码断点、单步、变量查看、远程调试和性能分析；
- 脚本模块热重载与状态迁移；
- RTS、塔防、城建和 Roguelite 所需的固定 Tick、回放与确定性模式；
- 面向 Mod 的字节码校验、权限控制和资源预算；
- Windows、Linux、macOS、移动端及受限 AOT 平台的长期可移植性。

RealScript **不是完整 C# 实现**，也不以兼容 .NET 生态为目标。它是一门具有 C# 表达习惯、针对游戏运行时约束和优化的独立语言。

## 语言示例

```csharp
module Game.Defense;

[Component]
struct Health
{
    float current;
    float maximum;

    bool IsAlive => current > 0.0f;
}

interface IGameSystem
{
    void Tick(in TickContext context);
}

[Deterministic]
sealed class TurretSystem : IGameSystem
{
    private Query<Write<Transform>, Read<Turret>, Read<Target>> turrets;

    void Tick(in TickContext context)
    {
        foreach (ref var transform, in var turret, in var target in turrets)
        {
            if (!target.entity.IsValid)
                continue;

            transform.rotation = Math.LookAt(
                transform.position,
                target.entity.Get<Transform>().position);
        }
    }

    async GameTask RunWave()
    {
        await Time.DelayTicks(30);
        SpawnWave();
    }
}
```

## 总体架构

```text
RealScript source
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
      ├──────────────► Register Bytecode ─► Bytecode VM
      │
      ├──────────────► Generated C++17 ───► Platform AOT Compiler
      │
      └──────────────► LLVM IR ───────────► Optional ORC JIT
                                      
All backends share:
Runtime ABI / Metadata / GC / Bindings / Debug Info / Profiler
      │
      ▼
C++17 Game Engine
```

解释器和编译器不会分别直接处理 AST。它们共享同一套类型系统、HIR、MIR、运行时 ABI 和语义一致性测试，从架构上避免两种执行模式逐渐产生行为差异。

## 执行模式

| 构建模式 | 执行后端 | 主要用途 |
|---|---|---|
| Edit | 类型化寄存器字节码 | 快速编译、热重载、交互开发 |
| Debug | 字节码或低优化 AOT | 断点、单步、变量和异常调试 |
| Profile | 字节码统计、AOT 或可选 JIT | 热点识别和性能分析 |
| Release | C++17 AOT | 正式发布和受限平台部署 |
| Mod Sandbox | 已校验字节码 | 用户脚本和 Mod |
| Deterministic Server | 确定性字节码或 AOT | 联机同步、回放和自动测试 |

## 核心设计原则

### 1. C# 风格，但不复制完整 C#

首版聚焦 `module`、`class`、`struct`、`interface`、属性、强类型枚举、泛型、委托、协程和可空引用等游戏开发高价值能力。

`dynamic`、运行时代码生成、完整反射、任意指针、默认接口实现和完整 LINQ 不属于首版目标。

### 2. 值类型优先

`struct` 是 RealScript 的高性能基础：

- 可内联存储；
- 默认不进入 GC 堆；
- 适合向量、颜色、变换、ECS 组件和小型配置；
- 支持无装箱参数传递；
- AOT 后端生成受控布局的 C++ 值类型。

`class` 用于行为对象、流程状态、任务、UI ViewModel 和较小的对象图。

### 3. 统一中间表示

Typed MIR 是解释、AOT、JIT、优化、调试信息和静态分析的共同边界。语言语义由 MIR 和运行时 intrinsic 明确定义，不继承 C++ 的未定义行为。

### 4. 统一元数据

类型、方法、字段和属性使用统一的 Descriptor + Facet 模型：

```text
TypeDescriptor
  ├─ ScriptTypeFacet
  ├─ NativeBindingFacet
  ├─ SerializationFacet
  ├─ DebugFacet
  ├─ EditorFacet
  └─ DeterminismFacet
```

反射、绑定、序列化、编辑器和调试器共享元数据来源，不建立相互漂移的独立注册表。

### 5. C++ 边界无动态装箱

编辑器和反射工具可以使用通用 `ScriptValue`，高频运行路径则使用生成的类型化 Native Thunk，避免字符串查找、参数数组分配、装箱和重复类型判断。

### 6. 调试与语言工具是一等能力

- 调试器采用 Debug Adapter Protocol（DAP）；
- 代码补全、诊断、跳转和重构采用 Language Server Protocol（LSP）；
- 字节码和 AOT 共用源码序列点、局部变量作用域和脚本影子调用栈；
- 调试协议从运行时早期阶段纳入设计，而不是项目末期补充。

### 7. 确定性由语言和 API 共同保证

`[Deterministic]` 模块禁止直接读取墙上时间、系统随机数、任意线程、无序容器遍历和未声明的原生 API。时间、随机数、异步等待和资源访问必须通过确定性服务完成。

## 计划中的代码结构

```text
realscript/
  include/realscript/
  src/
    syntax/          # Lexer、Parser、增量语法树
    semantic/        # 符号、绑定、类型检查和泛型
    ir/
      hir/
      mir/
      optimizer/
    bytecode/        # 指令、编码器、反汇编器和验证器
    vm/              # 解释器、调用栈和协程
    runtime/         # 对象、字符串、数组、异常和 ABI
    gc/              # 精确增量 GC
    bindings/        # C++ 描述符和绑定生成
    aot_cpp/         # C++17 AOT 后端
    jit_llvm/        # 可选 LLVM ORC 后端
    metadata/        # Descriptor + Facet
    debug/           # 调试运行时和 DAP
    tooling/lsp/     # 语言服务器
    hot_reload/      # ABI 比较和状态迁移
    profiler/        # CPU、分配和调用边界分析
  tools/
    rsc/             # 编译器命令行
    rsdump/          # MIR/字节码/元数据检查工具
    rsdebug/         # 调试适配器
  tests/
    conformance/
    differential/
    benchmarks/
  docs/
```

## 初步性能目标

这些数字是未来基准测试的验收目标，不是当前实现数据。

| 指标 | 初步目标 |
|---|---:|
| 字节码纯计算 | 等价优化 C++ 耗时的 8–15 倍以内 |
| 引擎调用占主导的脚本逻辑 | C++ 总耗时的 2–3 倍以内 |
| C++ AOT 类型化数值代码 | 与手写 C++ 差距控制在 0–30% |
| AOT 到原生 API 调用 | 接近普通 C ABI 函数调用 |
| GC P99 单帧暂停 | 60 FPS 下低于 0.5 ms |
| 典型模块增量编译 | 低于 100 ms |
| 典型模块字节码热重载 | 低于 200 ms |

基准测试将覆盖向量计算、ECS 批处理、脚本到 C++ 调用、接口分派、字符串、容器、分配、协程、GC 延迟、热重载和确定性回放，而不是只使用 Fibonacci 一类微型测试。

## 路线图

### Phase 1 — Language Foundation

- Lexer、Parser 和增量语法树；
- 模块、符号、类型和控制流；
- Typed HIR 与 Typed MIR；
- 语言一致性测试基础设施。

### Phase 2 — Bytecode Runtime

- 类型化寄存器字节码；
- 编码器、反汇编器和验证器；
- 解释器、调用栈和异常；
- 字符串、数组和资源预算。

### Phase 3 — Object Model and Native Binding

- `struct`、`class`、`interface` 和泛型；
- 精确 GC；
- Descriptor + Facet 元数据；
- 自动生成的 C++ Native Thunk。

### Phase 4 — Debugging and Tooling

- 调试信息格式；
- 断点、单步、变量、调用栈和 Watch；
- DAP 调试适配器；
- LSP 语言服务器；
- 字节码热重载。

### Phase 5 — C++17 AOT

- MIR 优化；
- C++17 代码生成；
- 稳定 Runtime C ABI；
- 模块编译缓存；
- 字节码/AOT 差分测试。

### Phase 6 — Determinism and Advanced Performance

- 确定性模块和固定点类型；
- ECS 批量访问；
- 去虚化、泛型单态化和逃逸分析；
- 分代 GC 与 PGO；
- 快照、回放和时间旅行调试；
- 可选 LLVM ORC JIT。

## 设计文档

完整设计见：

- [RealScript 游戏脚本引擎总体设计](docs/ENGINE_DESIGN.md)

## 主要参考项目

RealScript 不直接复制任何单一项目，而是重点研究下列系统的不同长处：

- [AngelScript](https://www.angelcode.com/angelscript/)：强类型嵌入式脚本、C++ 绑定、字节码和调试接口；
- [Luau](https://luau.org/)：高性能字节码、增量 GC、类型分析和可选原生代码生成；
- [Unity IL2CPP](https://docs.unity3d.com/Manual/IL2CPP.html)：中间代码到 C++ 的 AOT 发布路径；
- [LLVM ORC](https://llvm.org/docs/ORCv2.html)：模块化 JIT 和按需编译；
- [Wren](https://wren.io/)：紧凑、易嵌入的 VM 和 Fiber 模型；
- [Godot GDScript](https://docs.godotengine.org/en/stable/tutorials/scripting/gdscript/)：脚本语言与游戏编辑器的深度整合；
- [.NET Native Hosting](https://learn.microsoft.com/dotnet/core/tutorials/netcore-hosting)：托管运行时嵌入与宿主边界；
- [Debug Adapter Protocol](https://microsoft.github.io/debug-adapter-protocol/)；
- [Language Server Protocol](https://microsoft.github.io/language-server-protocol/)。

## 当前状态

- [x] 确立总体语言和运行时方向；
- [x] 确立字节码解释 + C++17 AOT 双后端；
- [x] 确立统一 HIR/MIR、Runtime ABI 和元数据模型；
- [x] 确立 DAP/LSP、热重载和确定性目标；
- [ ] 完成语言规范 v0.1；
- [ ] 冻结首版 MIR 和字节码格式；
- [ ] 建立 C++17 工程骨架；
- [ ] 实现最小编译器与解释器闭环；
- [ ] 建立基准测试与差分测试。

## License

许可证尚未确定。在许可证文件提交前，请勿假定本仓库代码或文档可用于再分发。
