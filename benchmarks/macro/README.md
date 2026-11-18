# RealScript product macrobench

These workloads complement the instruction-level microbenchmarks with stable,
game-shaped units of work:

- `ai_tick.rscript`: 10,000 branch-heavy unit updates.
- `ability_tick.rscript`: 5,000 cooldown/status/damage evaluations.
- `event_fanout.rscript`: 1,000 emitters with 10 listener calls each.
- `allocation_tick.rscript`: 10,000 short-lived array allocations.

Run timing and GC passes separately so collector work is not hidden inside the
raw execution number:

```powershell
build-perf\Release\rsbench.exe --mode raw --gc-work 0 --samples 9 `
  --warmup 2 --iterations 3 --opt-level 2 --json `
  benchmarks\macro\ai_tick.rscript

build-perf\Release\rsbench.exe --mode raw --gc-work 8 --samples 9 `
  --warmup 2 --iterations 3 --opt-level 2 --json `
  benchmarks\macro\allocation_tick.rscript
```

Report both time per invocation and workload-normalized throughput; do not use
these local measurements as cross-language rankings.

The opt-in `rsbench_product` target (now hosted in the companion **`RealScript-GamePack`**
repository when configured with `-DREALSCRIPT_GAMEPACK_BUILD_BENCHMARKS=ON`) measures a
10,000-coroutine gameplay tick, including heap/host snapshot, restore, and
deterministic replay:

```powershell
cmake -S C:\Projects\RealScript-GamePack -B build-gamepack -DREALSCRIPT_BUILD_DIR=C:\Projects\RealScript\build -DREALSCRIPT_GAMEPACK_BUILD_BENCHMARKS=ON
cmake --build build-gamepack --config Release --target rsbench_product
build-gamepack\Release\rsbench_product.exe
```
