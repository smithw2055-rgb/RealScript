# Changelog

All notable RealScript changes are documented here. Compatibility contracts
remain versioned but unfrozen until a future 1.0 release.

## [0.2.0] - 2026-08-10

v0.2.0 advances RealScript from the Phase 1–6 alpha baseline to the complete
Phase 1–24 language, runtime, Game SDK, deterministic gameplay, and performance
baseline.

### Language and compiler

- Added native structured control flow, interfaces, source attributes,
  delegates, closures, multicast events, runtime polymorphism, and visibility.
- Added inferred compile-time generics, constraints, generic members,
  collections, enumerators, nullable values, boxing, exact-width primitives,
  mutable structs, ref locations, patterns, initializers, flexible arguments,
  switch expressions, and structured script exceptions.
- Added deterministic coroutine state machines with persistent locals, nested
  control flow, cancellation, child sequences, results, snapshot, replay,
  rollback, and AOT parity.
- Migrated the Phase 11–17 expansion profile into native syntax, semantic,
  Typed MIR, bytecode, AOT, metadata, tooling, and hot-reload pipelines.

### Runtime and Game SDK

- Added typed C++ bindings, rooted script objects, scene lifecycle, events,
  triggers, bytecode package loading, installable SDK targets, and object-state
  serialization.
- Added generation-checked gameplay entities, fixed ticks, PCG random streams,
  deterministic timers/events, gameplay snapshots, stable hashes, metadata,
  generated bindings, and versioned `RSGS` save/replay/rollback state.
- Added ProgramImage link/verify-once indexes and direct SymbolId invocation.
- Fixed incremental GC sweep progress during continuous allocation while
  preserving birth-cycle objects and bounded work.

### Native execution and performance

- Added typed primitive C++17 AOT lowering, direct CFG labels, failure-aware
  accounting, conservative integer range analysis, and exact RAW/Strict local
  accounting.
- Preserved checked arithmetic, finite instruction budgets, public statistics,
  trace/profile behavior, and interpreter/AOT/JIT deterministic digest parity.
- Reduced the integer-loop AOT/JIT RAW baseline from roughly 3 ms to roughly
  26 µs on the reference machine.
- Reduced continuous-allocation GC from 5.239 s with no completed collection to
  27.75 ms with 460 completed collections on the reference workload.
- Added interpreter integer specialization, cheaper stack materialization,
  scene method caching, one-shot scheduler payload moves, and product-level AI,
  ability, event, allocation, coroutine, snapshot, restore, and replay
  benchmarks.

### Tooling and delivery

- Expanded `rsc`, `rsaot`, `rsbench`, DAP, LSP, source metadata, diagnostics,
  profiling, deterministic digests, and benchmark JSON output.
- Fixed O2 MIR validation for void delegate calls, struct stores, and exception
  handler control-flow edges; aligned interpreter/AOT parity tests on the same
  optimized input.
- Fixed the Windows `rsc` build and made `rsaot --version` plus LSP server
  metadata report the shared product version.
- Added component CMake targets, package version metadata, install-consumer
  validation, expanded Windows/Linux CI, and productized AOT integration.
- Reorganized English and Chinese repository documentation around features,
  quick start, execution backends, performance evidence, compatibility, and
  engine integration.

### Compatibility

- Project version is now 0.2.0.
- Current bytecode output is `.rsbc` 0.9; the decoder accepts 0.6–0.9.
- SDK compatibility, Game SDK package, and script-object-state versions remain
  independently versioned at 1.
- Source, MIR, bytecode, native ABI, metadata, debug-info, and serialized
  gameplay-state contracts are not frozen. Pin the exact release and rebuild
  generated AOT artifacts with the matching SDK.

## [0.1.0-alpha] - 2026-07-26

- Delivered the first complete Phase 1–6 compiler/runtime technical baseline.
- Added verified Typed MIR and register bytecode, interpreter, managed heap,
  DAP/LSP tooling, C++17 AOT, deterministic execution, profiling, benchmarking,
  and optional external-toolchain JIT.

[0.2.0]: https://github.com/smithw2055-rgb/RealScript/compare/v0.1.0-alpha...v0.2.0
[0.1.0-alpha]: https://github.com/smithw2055-rgb/RealScript/releases/tag/v0.1.0-alpha
