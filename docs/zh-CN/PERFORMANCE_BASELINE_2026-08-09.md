# RealScript 性能基线与瓶颈评估（2026-08-09）

## 结论

当前最主要的性能问题不是单一的 Interpreter dispatch，而是三层可分离成本：

1. `invoke()` 每次重新验证全部 bytecode 并重建函数/类型索引，宿主调用成本随整个程序规模增长。
2. Strict determinism 和 ProfileCollector 在每条指令上构造包含字符串的事件、哈希并（profiled 时）加锁更新 map。
3. AOT/JIT 仍执行基于 `runtime::Value` 的 compiled MIR，并为每条 MIR 操作调用 `ExecutionContext::consume()`；当前不是 typed native code。

本轮已先修正 benchmark 的 raw 路径和统计能力。关闭 instrumentation 后不再生成逐指令 `TraceEvent`，因此下面的 raw 数字可作为 VM/AOT 本体的第一版基线。

## 测试环境

- commit：`d8889964d2615db089fd98d7f73433f7fedbb7b1`
- OS：Windows 11 Home China，10.0.26200
- CPU：AMD Ryzen 7 6800H，8C/16T
- 内存：约 16 GB
- 电源计划：ASUS Recommended
- 工具链：Visual Studio 2026 18.3.2、MSVC 14.50、CMake 4.1.2-msvc8
- 配置：x64 Release，warnings-as-errors

这些是本地开发机基线，不是发布用跨语言排名。测试没有绑核或锁定 CPU 频率；表中使用多样本中位数和 p95 降低噪声影响。

## Interpreter instrumentation 矩阵

每个工作负载在脚本内部运行 10,000 次循环。时间为单次脚本 `main()` 调用。

| Benchmark | 指令/次 | Raw 中位数 | Strict 中位数 | Profiled 中位数 | Strict / Raw | Profiled / Raw |
|---|---:|---:|---:|---:|---:|---:|
| `integer_loop` | 130,011 | 12.479 ms | 44.912 ms | 55.153 ms | 3.60x | 4.42x |
| `branch_loop` | 180,011 | 16.789 ms | 62.199 ms | 79.580 ms | 3.71x | 4.74x |
| `function_call` | 180,011 | 18.611 ms | 64.031 ms | 85.098 ms | 3.44x | 4.57x |

Raw 的平均成本约 93–103 ns/解释器指令。Strict 增至约 346–356 ns/指令；Profiled 增至约 424–473 ns/指令。

在加入 raw 快路径前，所谓 raw 仍逐事件哈希，三个工作负载约为 52.6、65.9、77.5 ms。修正后分别约为 12.5、16.8、18.6 ms，证明 instrumentation 是第一优先级瓶颈，而不是可忽略的诊断开销。

## `invoke()` 固定成本与程序规模

两个程序的 `main()` 都只执行 2 条指令；第二个程序额外包含 127 个不会被调用的函数。

| 程序规模 | Interpreter raw | EngineRuntime raw |
|---|---:|---:|
| 1 function | 4.041 us | 6.446 us |
| 128 functions | 407.130 us | 440.641 us |

仅增加未调用函数就让 Interpreter 宿主调用变慢约 101 倍。证据与源码一致：`Interpreter::invoke(SymbolId)` 每次调用 `verifyModule()`，随后重建 `unordered_map<SymbolId, FunctionLocation>` 和 type map。

`EngineRuntime` 在此基础上每次创建 Interpreter，并从 ProgramImage 复制 modules。它在 128 函数程序上再增加约 33.5 us，但首要问题仍是 verify/index per invoke。

## P0 Runtime Image 优化结果

基于后续 `af1bbe4` 性能基线完成了 verify/link/index once：

- ProgramImage 在 link 阶段建立稳定的 FunctionLocation 和 type 索引；
- 从裸 bytecode 构造 Interpreter 时只 link 一次，并保存 link error；
- 从 ProgramImage 构造 Interpreter 时只保留 shared image，不再复制 modules；
- `invoke(SymbolId)` 直接解析 FunctionLocation；
- name lookup 使用 ProgramImage 的 name index；
- EngineRuntime 新增 SymbolId 入口，benchmark 不再把 name lookup 混入调用时间。

21 个样本、每样本 20,000 次调用的 Release 复测：

| 程序规模 / 路径 | P0 前 | P0 后 | 改善 |
|---|---:|---:|---:|
| 1 function / Interpreter | 4.041 us | 0.855 us | -78.8% |
| 128 functions / Interpreter | 407.130 us | 1.027 us | -99.75% |
| 1 function / EngineRuntime | 6.446 us | 1.020 us | -84.2% |
| 128 functions / EngineRuntime | 440.641 us | 0.977 us | -99.78% |

128 函数 Interpreter 的宿主调用约加速 396 倍。更重要的是，1/128 函数结果已经落在同一个约 1 us 噪声区间，调用时间不再随未调用函数数线性增长。

长循环复测没有显示吞吐回退：`integer_loop` raw 12.064 ms、`branch_loop` raw 17.156 ms、`function_call` raw 18.082 ms，均与 P0 前基线处于相同波动范围。

Release 定向回归 7/7 通过：Phase 2C、Phase 6 JIT/core/AOT/benchmark smoke、Phase 19 runtime polymorphism/AOT。新增测试覆盖 ProgramImage copy/move 后的函数/类型索引生命周期以及 EngineRuntime SymbolId 直达调用。

广覆盖 Release 验证为 32/35 通过。除已知无法生成的 Phase 20/24 AOT 和依赖 `rsc` 的打包测试外，新发现的三个基线阻塞为：Phase 2A bytecode snapshot 漂移；Phase 21 AOT 指令计数 Interpreter 3,549、AOT 3,540；Phase 23 NullableRoundTrip 指令计数 Interpreter 23、AOT 22。后两项执行结果一致，差异位于既有 AOT/Interpreter accounting parity，不在 P0 的 verify/index/copy 路径。

## Interpreter O0/O1/O2

| Benchmark | O0 | O1 | O2 | O0/O1/O2 指令数 |
|---|---:|---:|---:|---:|
| `integer_loop` | 11.498 ms | 11.596 ms | 12.741 ms | 130,011 |
| `branch_loop` | 16.816 ms | 16.640 ms | 16.672 ms | 180,011 |
| `function_call` | 18.970 ms | 17.795 ms | 18.881 ms | 180,011 |

本组 microbench 中 O2 没有减少执行指令数，也没有稳定优于 O0/O1。优化器当前没有消除循环中的 load/store、常量或控制流成本。应在下一阶段用 MIR 统计和 workload-specific pass 验证 O2 的实际价值。

## AOT、JIT 与 native ceiling

工作负载为同一个 10,000 次整数求和循环。

| Backend / mode | 中位数 | 相对 native |
|---|---:|---:|
| Native C++ Release | 4.8 us | 1.0x |
| AOT raw | 3.140 ms | 654x slower |
| JIT raw | 3.355 ms | 699x slower |
| AOT Strict | 37.787 ms | 7,872x slower |
| JIT Strict | 38.308 ms | 7,981x slower |

AOT raw 比 Interpreter raw 快约 4 倍，但仍远离 native。生成源码保留：

- `std::vector<runtime::Value>` arguments/locals/registers；
- `context.consume("operation")`；
- `context.binary()`、`context.branch()` 等运行时抽象；
- 每次 `Program::invoke()` 新建 ExecutionContext 及 function/type map。

因此当前 JIT 的运行性能与 AOT 基本一致是预期行为：它缓存并加载同一类生成 C++，不是另一套优化执行后端。

JIT cold 编译为 3,617 ms，紧随其后的 cache hit 为 2.61 ms。JIT 性能工作应分别看编译/缓存和运行时，不应把两者混成一个数。

## GC idle tax

对 `integer_loop` 做 7 组交错进程复测：

- `gcWorkBudget=0`：总体中位数 14.008 ms；单次中位数范围 12.943–17.066 ms；
- `gcWorkBudget=8`：总体中位数 14.553 ms；单次中位数范围 12.539–17.090 ms。

表面差异约 +3.9%，但范围高度重叠。当前只能把每指令 `heap->step()` 列为次级可疑点；必须在固定频率、绑核、无后台编译的环境继续确认后才能设置门禁。

## 编译阶段基线

11 个独立进程的阶段耗时中位数：

| 程序规模 | Frontend | O2 optimizer | Bytecode lower + verify |
|---|---:|---:|---:|
| 1 function | 0.699 ms | 0.026 ms | 0.042 ms |
| 128 functions | 7.313 ms | 0.678 ms | 0.589 ms |

这只是 cold single-module micro baseline；增量编译、模块复用、AOT source generation 和 native compile 仍需独立 macro benchmark。

## 瓶颈优先级

### P0：Runtime Image 与宿主调用热路径

状态：已完成。当前 1/128 函数宿主调用均约 1 us，以下架构条件已经满足。

验收条件：

- bytecode verify：0 次 / invoke；
- function/type index build：0 次 / invoke；
- ProgramImage/modules copy：0 次 / invoke；
- entry 预解析为 `SymbolId` 后 O(1) 直达 FunctionLocation；
- 128 函数 `empty_call` 不再随未调用函数数线性增长。

建议让 ProgramImage 持有稳定的 function/type/dispatch/assignability 索引，Interpreter 和 EngineRuntime 只引用该不可变 image。

### P1：确定性与 profiling 成本

状态：下一优化优先级。

Raw 关闭路径已经不再生成逐事件对象。Strict/Profiled 下一步应：

- 用数值 operation/event id 代替热路径字符串；
- 对现有 MIR/bytecode 元数据直接增量哈希；
- ProfileCollector 改为线程本地计数或无锁批量合并；
- timing run 与 profile run 完全分离。

建议第一阶段目标：Strict 相对 raw 小于 1.2x；Profiled 小于 2x。当前分别约 3.4–3.7x 和 4.4–4.7x。

### P2：Typed AOT

CPU-heavy benchmark 已满足启动 Typed AOT 的信号：AOT 虽比 Interpreter 快约 4 倍，但仍比 native 慢数百倍。应让 primitive MIR register/local 直接生成 C++ primitive，只有 object、boxed/polymorphic value 和 ABI 边界使用 `runtime::Value`。

### P3：Interpreter frame、branch 与 dispatch

- `executeFunction()` 每次创建 locals/registers vector；
- 循环分支每次创建 `transferred` vector 做 SSA block argument parallel copy；
- virtual/interface assignability 会分配 visited set 并遍历继承/接口表；
- function-call workload 在相同指令数下比 branch workload 多约 1.8 ms/10,000 calls。

需要增加 direct/virtual/interface、frame allocation、block-argument transfer 三组专门 benchmark 后再实施缓存或 scratch-frame 优化。

### P4：产品级 macrobench

下一批必须覆盖 AI tick、ability/event fanout、10,000 coroutine resume、rollback snapshot/restore、native binding 和 GC allocation/latency。Lua/Luau/LuaJIT 对比应等 P0 完成且 macrobench correctness gate 建立后再加入。

## 本轮工具改动

`rsbench` 现在支持：

- `--mode raw|deterministic|profiled`；
- `--samples` 及 median/p95/p99/min/max；
- 计时前解析 `SymbolId`；
- `--host-path interpreter|engine`；
- `--gc-work`；
- GC 统计、instructions/invocation、ns/instruction；
- frontend/optimizer/bytecode 阶段耗时。

另有 opt-in `REALSCRIPT_BUILD_BENCHMARKS=ON`，构建 `rsbench_aot` 和 `rsbench_jit`。Windows JIT 命令现在能正确处理含空格的完整 MSVC 路径，并把中间 object 写入独立 cache 目录。

## 验证状态

受本次改动直接影响的 Release 回归 6/6 通过：

- Phase 6 JIT/core/AOT；
- Phase 6 benchmark smoke；
- Phase 19 runtime polymorphism/AOT。

全量 Release build 尚未通过，阻塞来自本轮未修改的主线路径：

- Phase 20 AOT：`RS6001`，含 `RS3004` / `RS3082`；
- Phase 24 AOT：`RS6001`，含 `RS3064`；
- `rsc`：`tools/rsc/main.cpp` 使用 `std::filesystem` 但缺少完整声明。
- Phase 2A：bytecode snapshot 与当前编译结果不一致；
- Phase 21/23 AOT：结果一致，但 instruction accounting 与 Interpreter 分别相差 9 和 1。

这些问题应单独修复后再恢复全量门禁（当前 CMake 列出 40 项测试）。本轮没有提交或推送。

## 复现命令

```powershell
$cmake = 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'

& $cmake -S . -B build-perf `
  -G 'Visual Studio 18 2026' -A x64 `
  -DREALSCRIPT_BUILD_TESTS=OFF `
  -DREALSCRIPT_BUILD_BENCHMARKS=ON `
  -DREALSCRIPT_WARNINGS_AS_ERRORS=ON

& $cmake --build build-perf --config Release `
  --target rsbench rsbench_aot rsbench_jit -- /m:1

.\build-perf\Release\rsbench.exe `
  --mode raw --gc-work 8 --samples 9 `
  --warmup 2 --iterations 3 --opt-level 2 --json `
  benchmarks\micro\integer_loop.rs

.\build-perf\Release\rsbench_aot.exe

cmd.exe /d /s /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=amd64 && "C:\Projects\RealScript\build-perf\Release\rsbench_jit.exe"'
```
