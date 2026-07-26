# Phase 6 — Determinism, Optimization, Profiling and Optional JIT

## Status

Implemented as the final planned optimization and execution-engine phase on top of the Phase 1–5 language, bytecode, runtime, tooling and C++17 AOT foundations.

## Goal

Phase 6 keeps one observable RealScript semantic model while adding explicit execution profiles around it:

```text
Verified Typed MIR
      │
      ├── O0 / O1 / O2 optimizer
      │        │
      │        ├── register bytecode interpreter
      │        ├── C++17 AOT release backend
      │        └── optional external-toolchain JIT
      │
      ├── deterministic Record / Replay
      ├── stable execution digest
      └── per-function profiling and rsbench
```

No optimizer or native backend may bypass MIR validation, remove observable traps, change checked arithmetic, reorder host calls, alter GC-visible reference stores, or weaken execution budgets.

## Phase 6A — deterministic execution profile

`ExecutionOptions::determinism` selects one of four modes:

- `Off`: ordinary execution; a stable digest is still available for diagnostics;
- `Strict`: only bindings declared deterministic may execute;
- `Record`: recordable and non-deterministic host calls are executed and appended to a `ReplayLog`;
- `Replay`: recorded host-call outcomes are consumed without invoking the host again.

Each host binding declares a `BindingDeterminism` policy:

- `Deterministic`;
- `Recordable`;
- `NonDeterministic`.

The runtime records stable function, instruction, branch, external-call, GC and error events. The final digest includes:

- execution-event kind and stable operation identity;
- stable function identity and call depth;
- instruction position;
- canonical result or error data;
- instruction, call, branch, external-call and GC statistics.

Process-local heap and native-handle registry identities are excluded from the digest. Floating-point hashing canonicalizes NaN payloads and signed zero when requested.

Replay entries validate the function identity and a stable hash of all arguments. Managed object references and native handles are rejected as replay payloads because their ownership and process identity cannot be serialized safely by this initial profile.

## Phase 6B — MIR optimizer

`optimization::Optimizer` consumes verified Typed MIR and emits verified Typed MIR. Three levels are available:

- `O0`: validation-preserving pass-through;
- `O1`: local constant propagation/folding, branch folding and unreachable-block removal;
- `O2`: iterative O1 plus conservative dead-value elimination and value-ID compaction.

Implemented transformations include:

- checked constant folding for `int` and `long` without folding overflow traps away;
- IEEE binary64 arithmetic and comparisons;
- Boolean and null equality folding;
- numeric widening conversion folding;
- local constant load folding when the store/load relationship is unambiguous;
- conditional branch folding;
- unreachable block removal;
- removal of unused, side-effect-free values;
- dense `ValueId` reassignment after removal;
- regenerated sequence-point tables after CFG/instruction changes.

Calls, allocations, null checks, bounds checks, stores and other observable operations are retained unless a later proof system explicitly establishes their removal as legal.

Every module is verified before and after optimization. Invalid input or invalid output produces a stable diagnostic and cannot proceed to bytecode, AOT or JIT generation.

## Phase 6C — profiling and benchmarks

`ProfileCollector` attributes stable event counts by function:

- calls and returns;
- instructions;
- branches;
- external calls;
- GC steps;
- runtime errors;
- maximum call depth.

Profiles can be rendered as deterministic text or JSON. The same collector is supported by the interpreter and AOT runtime.

`rsbench` provides a repeatable benchmark harness:

```bash
rsbench \
  --entry Game.Main::main \
  --warmup 20 \
  --iterations 1000 \
  --opt-level 2 \
  --json \
  game.rs common.rs
```

It reports elapsed time, nanoseconds per invocation, aggregate execution digest, final value, optimizer statistics and the per-function profile. Timing itself is informational; semantic validation uses the digest, value, error and statistics rather than asserting wall-clock thresholds in conformance tests.

## Phase 6D — optional C++ toolchain JIT

The initial optional JIT deliberately reuses the proven C++17 AOT path rather than introducing a second native code generator:

1. optimize verified MIR;
2. generate deterministic C++17;
3. compile a shared library with a configured platform compiler;
4. load it with `LoadLibrary` or `dlopen`;
5. discover it through `rs_module_query_v1`;
6. validate its `ProgramDescriptor`;
7. execute through the existing AOT runtime;
8. cache the library by semantic content hash.

`jit::ToolchainJit` accepts explicit compiler, include, support-library and cache paths. A cache hit skips native compilation but still loads and validates the module.

This is a same-SDK optional JIT profile. It provides native execution and tiering infrastructure without making an external compiler a mandatory runtime dependency. It is not an LLVM ORC implementation, does not patch running frames, and does not unload code while active calls can still reference it.

## Tool integration

### `rsc`

```bash
rsc game.rs --mir --opt-level 2 --opt-report
rsc game.rs --run Game.Main::main \
  --opt-level 2 --deterministic --profile --digest
```

### `rsaot`

```bash
rsaot --output-dir generated \
  --program-name GameScripts \
  --opt-level 2 --opt-report \
  game.rs common.rs
```

### CMake

```cmake
realscript_add_aot_library(GameScripts
    PROGRAM_NAME GameScripts
    OPT_LEVEL 2
    SOURCES game.rs common.rs
)
```

The CMake helper defaults to `OPT_LEVEL 0` for compatibility with projects that require byte-for-byte instruction accounting. Release projects opt into O1/O2 explicitly.

## Validation

Phase 6 validation covers:

- optimizer input/output verification;
- checked-overflow preservation;
- constant, branch and unreachable-block folding;
- interpreter equivalence before and after optimization;
- interpreter/AOT event ordering, execution digest and profile parity;
- deterministic strict-mode host-binding rejection;
- host-call Record/Replay without a second host invocation;
- replay mismatch and truncated-log errors;
- stable repeated-run digest and profile output;
- real external C++ compilation and shared-library loading;
- stable C ABI module query and descriptor validation;
- JIT cache reuse by semantic content hash;
- `rsbench` command-line smoke execution;
- complete Phase 1–6 regression coverage.

## Deliberate limits

Phase 6 does not add:

- LLVM ORC or machine-code emission directly from MIR;
- speculative optimization, deoptimization or on-stack replacement;
- profile-guided inlining or link-time whole-program optimization;
- escape analysis, scalar replacement or bounds-check elimination;
- portable replay serialization for object/native-handle graphs;
- fixed-tick clock and random-number standard-library services;
- inheritance, interfaces, virtual dispatch, generics or script exceptions;
- a stable cross-toolchain binary JIT cache format.

The public optimizer, determinism, profile and JIT interfaces are designed so these capabilities can be added without introducing a second language semantics.
