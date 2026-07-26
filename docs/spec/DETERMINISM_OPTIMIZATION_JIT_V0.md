# Determinism, Optimization, Profiling and Optional JIT — Implemented Draft v0.1

## 1. Scope

This document defines the Phase 6 implementation contract for deterministic execution, Typed MIR optimization, profiling and optional native JIT compilation.

It is subordinate to the language, MIR, runtime and AOT specifications. When an optimization or JIT behavior conflicts with an existing observable semantic rule, the existing semantic rule wins.

## 2. Determinism modes

An execution MUST select exactly one `DeterminismMode`.

### 2.1 Off

The runtime MAY invoke any registered binding. It MUST still preserve checked language semantics and execution budgets.

### 2.2 Strict

A binding MUST be declared `Deterministic`. A call to a `Recordable` or `NonDeterministic` binding MUST fail with `DeterminismViolation` before the host function is invoked.

### 2.3 Record

The runtime MUST invoke the host function and append one ordered `ExternalCallRecord` for each non-deterministic or recordable call. The record MUST contain:

- stable symbol ID and canonical name;
- stable argument hash;
- success/failure state;
- replay-stable result or structured error.

Record mode MUST reject execution when no replay log is supplied. It MUST reject object references and native handles in replayed arguments/results.

### 2.4 Replay

The runtime MUST consume entries in order. It MUST validate symbol ID, canonical name and argument hash before returning the recorded result. It MUST NOT invoke the host function for a matched replay entry.

Missing, extra or mismatched entries MUST affect the execution digest and MUST produce `ReplayMismatch` where execution cannot continue.

## 3. Stable execution digest

The execution digest MUST be independent of:

- native addresses;
- heap-instance IDs;
- native-handle registry IDs;
- unordered-container iteration;
- timestamp or wall-clock data;
- platform structure padding.

The digest includes stable trace events, result/error state and execution statistics. Function-exit display strings are not hashed because they may contain diagnostic process-local IDs; the actual return `Value` is hashed separately.

Floating-point hashing SHOULD canonicalize all NaNs and SHOULD treat positive/negative zero identically in deterministic mode.

## 4. Binding policy

`BindingRegistry` MUST retain the determinism declaration with each binding and MUST expose it to all execution backends. Interpreter, AOT and JIT MUST apply the same policy before invoking the host.

A legacy external resolver without policy metadata is treated as non-deterministic.

## 5. Optimizer legality

The optimizer MUST accept and emit valid Typed MIR. It MUST preserve:

- evaluation order;
- checked overflow and division traps;
- null and bounds checks;
- external-call order and count;
- allocation and reference-store behavior;
- write barriers and GC root visibility;
- instruction/recursion/GC work budgets, except where an explicitly selected optimized profile documents accounting changes;
- source correlation sufficient for generated diagnostics and native source maps.

The implemented O1/O2 passes MAY fold pure constants, remove unreachable blocks and remove unused pure values. They MUST NOT fold an operation when the compile-time evaluation would overflow or otherwise trap at runtime.

Typed null conversions MUST remain explicit MIR conversions; a typed null MUST NOT be rewritten into a bare `const.null` with a reference result type.

After removal, value IDs MUST be compacted and every operand, terminator value and debug sequence-point index MUST be updated consistently.

## 6. Optimization levels

- O0 performs no semantic optimization.
- O1 performs one local simplification/CFG cleanup profile.
- O2 repeats supported simplifications to a bounded fixed point and performs conservative dead-value elimination.

The maximum iteration count is implementation configurable and MUST be bounded.

## 7. Profiling

Profiles MUST be aggregated in stable function-name order. Profile collection MUST be thread-safe. Text and JSON renderings MUST be deterministic for the same event sequence.

Wall-clock measurements are not part of deterministic conformance. Benchmark tools MUST report semantic digest/result information beside timing data.

## 8. Optional toolchain JIT

The JIT MUST start from verified MIR and SHOULD reuse the C++17 AOT generator. Before execution it MUST:

- complete MIR optimization and verification;
- compile a native shared library successfully;
- load the configured query symbol;
- negotiate the Runtime ABI;
- validate the complete `ProgramDescriptor`.

A cache key MUST derive from generated semantic content, not source timestamps. A cache hit MUST still perform library loading, ABI negotiation and descriptor validation.

The JIT module object owns its dynamic-library handle. It MUST destroy the `aot::Program` before unloading the library.

## 9. Cross-backend requirements

For equivalent MIR and execution options, Interpreter and AOT/JIT MUST agree on:

- returned values or structured errors;
- deterministic digest;
- per-function profile attribution;
- host-call Record/Replay behavior;
- checked traps and script stack traces.

Instruction accounting may differ only when an optimized MIR profile is explicitly selected. Tests comparing exact accounting MUST use the same optimized MIR in both backends.

## 10. Security and resource boundaries

JIT compiler paths and output directories are explicit host configuration. RealScript source code MUST NOT be able to select arbitrary compiler arguments or filesystem paths through the script language.

Generated source and libraries are untrusted until compiler success, ABI negotiation and descriptor validation all complete.

The external toolchain JIT is optional and MAY be disabled at build/test time. Disabling it MUST NOT disable the interpreter, optimizer, deterministic profile or AOT generator.
