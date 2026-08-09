# RealScript 性能基线与瓶颈评估（2026-08-09）

## 结论

基线最初显示，主要性能问题不是单一的 Interpreter dispatch，而是三层可分离成本：

1. `invoke()` 每次重新验证全部 bytecode 并重建函数/类型索引，宿主调用成本随整个程序规模增长。
2. Strict determinism 和 ProfileCollector 在每条指令上构造包含字符串的事件、哈希并（profiled 时）加锁更新 map。
3. P2 前 AOT/JIT 仍执行基于 `runtime::Value` 的 compiled MIR，并为每条 MIR 操作调用 `ExecutionContext::consume()`，不是 typed native code。

当前已完成 P0 Runtime Image、P1 instrumentation 热路径优化，以及 P2 Typed AOT 前两个阶段。整数/布尔 primitive MIR 已直接生成 C++ 标量，Typed CFG 使用直接 block labels，RAW accounting 按错误边界安全合并；整数循环 AOT RAW 从记录的 3.033 ms 降到 41.64 us。下一阶段的主要目标是扩大 Typed AOT 覆盖、用范围分析消除可证明安全的 checked arithmetic、优化 Interpreter frame/branch/dispatch，以及建立产品级 macrobench。

## 测试环境

- 初始基线 commit：`d8889964d2615db089fd98d7f73433f7fedbb7b1`
- P2 第二阶段工作树基点：`253f463`（第二阶段改动尚未提交）
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

## P1 确定性与 profiling 优化结果

P1 将执行事件拆成两个通道：

- Strict 使用稳定的函数 `SymbolId`、opcode/terminator operation id、分支目标、指令序号和调用深度，在执行上下文内联累计结构化摘要；invoke 结束时只向 `DeterminismSession` 合并一次；
- trace sink 仍收到原有完整字符串 `TraceEvent`，但未配置 sink 时不再构造字符串事件；
- ProfileCollector 在每次 invoke 内按函数 id 累计数值计数，结束时一次加锁批量合并，不再逐事件执行 mutex + `map<string>` 更新；
- AOT generator 为每条 MIR operation 直接生成稳定的数值 operation id，Interpreter、AOT、JIT 使用同一事件编码。

21 个样本、每样本 3 次调用的最终 Release 复测：

| Benchmark | P1 前 Strict | P1 后 Strict | P1 后 Strict/Raw | P1 前 Profiled | P1 后 Profiled | P1 后 Profiled/Raw |
|---|---:|---:|---:|---:|---:|---:|
| `integer_loop` | 44.912 ms | 13.468 ms | 1.11x | 55.153 ms | 16.305 ms | 1.35x |
| `branch_loop` | 62.199 ms | 18.954 ms | 1.15x | 79.580 ms | 21.536 ms | 1.31x |
| `function_call` | 64.031 ms | 20.015 ms | 1.05x | 85.098 ms | 25.472 ms | 1.34x |

Strict 绝对时间降低约 69%–70%，Profiled 降低约 70%–73%。同进程的模式比值显著低于 Strict `<1.2x`、Profiled `<2x` 的目标。

Compiled MIR 的整数循环也接近目标：AOT raw/Strict 为 3.033/3.395 ms（1.12x），JIT raw/Strict 为 2.931/3.347 ms（1.14x）。与 P1 前 AOT Strict 37.787 ms、JIT Strict 38.308 ms 相比，绝对时间均降低约 91%。

确定性摘要现在使用结构化数值事件编码，因此摘要数值与 P1 前构建不兼容；同一构建内的重复执行以及 Interpreter/AOT/JIT 之间仍保持一致。Release 定向验证 7/7 通过，覆盖 Strict、Record/Replay、trace/profile、JIT、AOT 和 runtime polymorphism。最终广覆盖验证仍为 32/35，通过/失败集合与 P0 完全一致，没有新增回归。

## Interpreter O0/O1/O2

| Benchmark | O0 | O1 | O2 | O0/O1/O2 指令数 |
|---|---:|---:|---:|---:|
| `integer_loop` | 11.498 ms | 11.596 ms | 12.741 ms | 130,011 |
| `branch_loop` | 16.816 ms | 16.640 ms | 16.672 ms | 180,011 |
| `function_call` | 18.970 ms | 17.795 ms | 18.881 ms | 180,011 |

本组 microbench 中 O2 没有减少执行指令数，也没有稳定优于 O0/O1。优化器当前没有消除循环中的 load/store、常量或控制流成本。应在下一阶段用 MIR 统计和 workload-specific pass 验证 O2 的实际价值。

## P2 前 AOT、JIT 与 native ceiling

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

## P2 Typed AOT 第一阶段结果

生成器现在对整函数做保守资格分析。只包含 `int`/`bool` 参数、局部变量、SSA value、checked 整数算术、比较和基本控制流的函数，生成 `std::int64_t`/`bool` 标量 local/register；对象、数组、调用、异常处理、long/double 和其他未覆盖 opcode 保持原有 `runtime::Value` 通用路径。ABI 边界仍负责 value boxing/unboxing，错误类型通过 C ABI 返回 `TypeMismatch`。

RAW 且无 determinism/profile/trace、`gcWorkBudget=0` 时，Typed AOT 使用编译期选择的轻量 accounting 路径；它仍逐 MIR 指令检查 instruction budget、累计 instruction/branch statistics。Strict、trace 和 profile 继续走原有详细事件路径，因此跨后端 determinism digest/profile 保持一致。

最终 Release 复测中，每个进程内部为 11 个样本、每样本 5 次调用；下表使用多进程中位数的中位数：

| Backend / mode | P1 记录值 | P2 第一阶段 | 改善 | 相对 native |
|---|---:|---:|---:|---:|
| Native C++ Release | 约 4.06 us | 4.06 us | — | 1.0x |
| AOT RAW | 3.033 ms | 360.98 us | 8.40x | 88.9x slower |
| JIT RAW | 2.931 ms | 367.34 us | 7.98x | 90.5x slower |
| AOT Strict | 3.395 ms | 1.223 ms | 2.78x | 301x slower |
| JIT Strict | 3.347 ms | 1.215 ms | 2.75x | 299x slower |

这里的总改善不能全部归因于 scalar lowering：P1 的 AOT/JIT 驱动虽然标记为 RAW，实际仍使用默认 `gcWorkBudget=8`。P2 已把 RAW/Strict 驱动都显式改为 0；在保留 `gcWorkBudget=8` 时，Typed AOT RAW 实测约 1.30–1.41 ms，说明 scalar lowering 本身已带来约 2.2 倍改善，关闭 GC idle work 并启用轻量 accounting 后才达到最终约 361 us。GC idle tax 仍应作为独立实验，不应混入 RAW 后端对比。

P2 后 AOT 与 JIT RAW 已基本持平，说明 JIT 没有额外运行时瓶颈；距离 native 仍约 89–91 倍。当前主要剩余成本是每 MIR 指令的预算/统计更新、`switch (currentBlock)` 控制流状态机以及 checked arithmetic。下一阶段应优先研究保持精确 instruction-budget 语义的 block-level 批量 accounting 和结构化 CFG 生成，再扩大到 call、long/double 与混合 boxed value。

## P2 Typed AOT 第二阶段结果

第二阶段针对第一阶段确认的两个热点做了独立 A/B 测量和实现：

- Typed CFG 从 `for + switch(currentBlock)` dispatcher 改为验证后 block id 的直接 C++ labels/goto；通用 `runtime::Value` 路径保持原实现。
- RAW accounting 将连续无失败操作合并，并让每个 batch 结束在 parameter validation、checked arithmetic、除零或溢出等可能失败的操作上；当 batch 超出剩余预算时，instruction count 精确推进到 budget 后报错。
- Strict/profile/trace 继续逐操作调用 `context.consume(operationId, name)`，不参与 RAW 合并。

分步结果表明，直接 labels 将 AOT RAW 从 360.98 us 降到约 284.98 us（1.27x）；failure-aware accounting batching 再降到 41.64 us（6.84x）。最终五个独立进程的中位数汇总如下：

| Backend / mode | P2 第一阶段 | P2 第二阶段 | 本阶段改善 | 相对 native |
|---|---:|---:|---:|---:|
| Native C++ Release | 4.06 us | 4.12 us | — | 1.0x |
| AOT RAW | 360.98 us | 41.64 us | 8.67x | 10.1x slower |
| JIT RAW | 367.34 us | 41.86 us | 8.78x | 10.2x slower |
| AOT Strict | 1.223 ms | 1.082 ms | 1.13x | 263x slower |
| JIT Strict | 1.215 ms | 1.220 ms | noise | 296x slower |

相对 P1 记录值，AOT/JIT RAW 的累计改善分别约为 72.8 倍和 70.0 倍。AOT 与 JIT RAW 仍保持同一性能区间，说明两者继续执行同一生成后端。Strict 的绝对时间没有回退，但由于 RAW 大幅加速，Strict/RAW 比率重新扩大到约 26–29 倍；详细逐操作 determinism 事件现在是 Strict 模式的明确主导成本。

预算与错误优先级新增边界验证：`failDivision(0)` 在 budget 4 时返回 `InstructionBudgetExceeded` 且计数为 4，在 budget 5 时返回 `DivisionByZero` 且计数为 5；RAW 与详细路径的成功调用指令数一致。新增多 block typed fixture 同时覆盖直接分支、branch statistics，以及 Interpreter/AOT Strict digest/profile parity。

距离 native 剩余约 10 倍。整数循环的主要剩余成本是每次循环仍有多个 failure-boundary accounting 更新和 checked overflow 分支；下一步适合加入 MIR range analysis，消除可证明不溢出的检查，并研究“整 block 预留 + 预算不足慢路径”以把常见路径压到每 block 一次 accounting，同时继续保持有限预算的精确错误顺序。

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

状态：已完成。

已完成：

- 用数值 operation/event id 代替热路径字符串；
- 对现有 MIR/bytecode 元数据直接增量哈希；
- ProfileCollector 改为线程本地计数或无锁批量合并；
- benchmark 的 timing 模式与 profile 模式保持显式分离。

第一阶段目标为 Strict 相对 raw 小于 1.2x、Profiled 小于 2x。最终 Interpreter 三组为 Strict 1.05–1.15x、Profiled 1.31–1.35x；AOT/JIT Strict 为 1.12x/1.14x。

### P2：Typed AOT

状态：第二阶段已完成。

- `int`/`bool` primitive MIR register/local 直接生成 C++ 标量；
- checked 算术、比较、分支和 SSA block transfer 不再调用 `context.binary()` 或复制 `runtime::Value`；
- ABI 边界保留参数校验与 result boxing；
- RAW 使用轻量但仍精确的 instruction/branch accounting；
- Strict/profile/trace 保持详细路径和跨后端事件一致性；
- 不满足资格的函数整函数回退到原有通用生成器；
- Typed CFG 使用直接 block labels，移除每次分支后的 dispatcher；
- RAW accounting 按可能失败的操作边界合并，保留精确预算计数和错误优先级。

前两个阶段把整数循环 AOT RAW 从约 3.033 ms 降至 41.64 us，与 JIT RAW 基本持平，距离 native 缩小到约 10 倍。下一阶段应增加 range analysis、整 block 常见路径预留，并扩大 primitive/call 覆盖；Strict 的逐操作事件成本需要作为独立性能目标处理。

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

受本次改动直接影响的 Release 定向回归 7/7 通过：

- Phase 2C；
- Phase 6 JIT/core/AOT；
- Phase 6 benchmark smoke；
- Phase 19 runtime polymorphism/AOT。

纳入范围的完整 Release 重建通过；广覆盖测试为 32/35，通过/失败集合与 P0/P1 一致。未纳入项与失败均来自本轮未修改的主线路径：

- Phase 20 AOT：`RS6001`，含 `RS3004` / `RS3082`；
- Phase 24 AOT：`RS6001`，含 `RS3064`；
- `rsc`：`tools/rsc/main.cpp` 使用 `std::filesystem` 但缺少完整声明。
- Phase 2A：bytecode snapshot 与当前编译结果不一致；
- Phase 21/23 AOT：结果一致，但 instruction accounting 与 Interpreter 分别相差 9 和 1。

这些问题应单独修复后再恢复全量门禁（当前 CMake 列出 40 项测试）。Typed/Generic 混合生成、直接 block labels、failure-aware RAW batch、预算/运行时错误优先级、C ABI 错误参数、Strict digest/profile parity 均已有回归覆盖。本轮没有提交或推送。

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

`rsbench_aot` 和 `rsbench_jit` 的 RAW/Strict 驱动均显式使用 `gcWorkBudget=0`；GC idle tax 请用 `rsbench --gc-work` 单独测量。
