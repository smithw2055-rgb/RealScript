# AOT, JIT, and Performance

[Documentation Home](README.md) | [Compilation, MIR, and Bytecode](COMPILATION_AND_BYTECODE.md)

RealScript provides two native execution paths built on the same verified Typed MIR semantics: deterministic C++17 AOT generation and an optional external C++ toolchain JIT.

## C++17 AOT Backend

The AOT backend transforms verified, optionally optimized MIR into regular C++17 source code.

```bash
rsaot \
  --output-dir generated \
  --program-name GameScripts \
  --opt-level 2 \
  --opt-report \
  game.rs common.rs
```

Generated output includes:

- a public C++ header;
- C++17 implementation;
- a deterministic manifest;
- module and function descriptors;
- source mapping through generated `#line` directives and metadata.

The generated implementation does not embed or invoke the bytecode interpreter.

## AOT Runtime

Generated code uses the shared AOT support runtime for:

- checked integer arithmetic;
- conversions;
- null and bounds checks;
- object, field, array, enum, and struct operations;
- managed allocation and precise shadow-stack roots;
- host bindings and native handles;
- instruction, recursion, heap, allocation, and GC budgets;
- tracing, profiling, deterministic events, and structured errors.

This preserves observable behavior across interpreter and native execution.

## CMake Integration

```cmake
include(cmake/RealScriptAot.cmake)

realscript_add_aot_library(GameScriptsAot
    PROGRAM_NAME GameScripts
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/scripts"
    OPT_LEVEL 2
    SOURCES
        game.rs
        common.rs
)

target_link_libraries(game PRIVATE GameScriptsAot)
```

The integration:

1. tracks script source dependencies;
2. invokes `rsaot`;
3. compiles generated C++17;
4. links RealScript AOT support;
5. exposes generated headers and manifest files;
6. avoids rewriting generated files when content has not changed.

## Native Module ABI

Generated modules expose two integration surfaces:

- C++ `ProgramDescriptor` and `Program` objects for same-SDK static linking;
- the C11/C++ compatible `rs_module_query_v1` entry for dynamic discovery.

The query ABI provides:

- ABI version negotiation;
- module identity and semantic content hash;
- function entry tables;
- type and source-map descriptors;
- descriptor validation before execution.

The public ABI header can be included from a C11 host.

## Same-SDK Boundary

The v0.2.0 AOT ABI assumes compatible RealScript SDK headers, runtime
implementation, compiler settings, and C++ ABI.

It is not a promise that a precompiled module can be moved between arbitrary standard libraries, compiler versions, or platforms.

## Toolchain JIT

The optional JIT reuses the AOT generator:

```text
Verified MIR
  -> O2 optimization
  -> generated C++17
  -> external host compiler
  -> shared library
  -> dlopen / LoadLibrary
  -> rs_module_query_v1
  -> ABI and descriptor validation
  -> native invocation
```

This path provides real native execution while keeping the core interpreter independent of LLVM or another third-party JIT library.

## JIT Cache

The cache key includes semantic and toolchain inputs such as:

- optimized program hash;
- compiler identity;
- compiler flags;
- SDK identity;
- include paths;
- AOT support-library inputs;
- target platform details.

A cache hit skips recompilation but still reloads the module and repeats ABI and descriptor validation.

## Dynamic Library Lifetime

The JIT module object owns the dynamic library handle. Function descriptors and native entry pointers remain valid only while the loaded module is alive.

The runtime does not unload a library while an owned module may still execute it.

## Optimization

AOT and JIT consume the same optimizer:

- O0: verified unoptimized MIR;
- O1: local folding and control-flow cleanup;
- O2: bounded fixed-point optimization and conservative dead-value removal.

Optimization reports expose counts such as folded constants, removed blocks, and removed instructions.

## Profiling

`ProfileCollector` records stable per-function counters:

- calls and returns;
- executed instructions;
- branches;
- external calls;
- GC work;
- runtime errors;
- maximum call depth.

Text and JSON output use stable function ordering to support automated comparisons.

## Benchmarking

```bash
rsbench \
  --entry Game.Main::main \
  --warmup 20 \
  --iterations 1000 \
  --opt-level 2 \
  --json \
  game.rs common.rs
```

Benchmark output includes:

- warmup and measured iteration counts;
- elapsed-time statistics;
- optimization statistics;
- semantic execution digest;
- per-function profile information.

A zero-warmup run is valid.

## v0.2.0 Release Baseline

The following local Release measurements were collected on Windows 11 with an
AMD Ryzen 7 6800H and Visual Studio 2026. They are regression baselines, not
cross-language rankings. Timings are medians; RAW native-backend measurements
use `gcWorkBudget=0` so collector work is measured separately.

| Workload | Median |
|---|---:|
| Native C++ integer loop | 4.06 µs |
| C++17 AOT RAW | 25.86 µs |
| Toolchain JIT RAW | 25.68 µs |
| AOT Strict | 0.358 ms |
| JIT Strict | 0.347 ms |
| Interpreter integer loop, 130,011 instructions | 6.73 ms |
| Interpreter branch loop, 180,011 instructions | 9.46 ms |
| Interpreter function-call loop, 10,000 calls | 12.99 ms |
| Allocation tick with GC work 8 | 27.75 ms |
| 10,000 coroutine resume | 86.06 ms |
| 10,000 coroutine deterministic replay | 89.45 ms |

The AOT/JIT integer loop improved from the pre-typed baseline of roughly 3 ms
to roughly 26 µs while preserving checked arithmetic, exact finite budgets,
statistics, trace/profile behavior, and cross-backend deterministic digests.
Continuous-allocation GC improved from 5.239 seconds with no completed
collection to 27.75 ms with 460 completed collections and 16,320 bytes live.

For the complete experiment history, rejected prototypes, product macrobench
results, and reproduction commands, see the
[detailed performance report](../zh-CN/PERFORMANCE_BASELINE_2026-08-09.md).

## Differential Validation

Tests compare interpreter, AOT, and JIT behavior for:

- successful return values;
- checked overflow;
- division by zero;
- null references;
- array bounds and negative lengths;
- instruction and recursion budgets;
- script stacks;
- deterministic event digests;
- per-function profiles;
- external compiler execution and JIT cache reuse.

## Current Limits

The toolchain JIT is not LLVM ORC and does not provide:

- direct machine-code emission;
- speculative optimization;
- deoptimization;
- on-stack replacement;
- profile-guided optimization;
- escape analysis;
- advanced bounds-check elimination.

Those capabilities may be added later without changing the verified MIR semantic boundary.
