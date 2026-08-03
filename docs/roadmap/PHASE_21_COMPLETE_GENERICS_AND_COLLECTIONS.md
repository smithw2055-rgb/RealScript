# Phase 21 — Complete Generics and Collections

Phase 21 replaces the bounded Phase 18 specialization/collection profile with a
deterministic compile-time specialization model and growable managed collections.
All generated specializations continue through the ordinary native
Syntax/Bound/MIR/bytecode pipeline, so interpreter and C++17 AOT semantics remain
shared.

## Work breakdown

- **21A growable collections and enumerator protocol:** complete.
- **21B generic function inference and member methods:** complete.
- **21C constraints and generic interfaces/delegates:** complete.
- **21D cross-module specialization identity and artifact policy:** complete.
- **21E tooling, hot reload, deterministic failure behavior, and validation:** complete.

## Delivered baseline

- `List`, `Dictionary`, `HashSet`, `Queue`, and `Stack` grow by deterministic
  doubling, with a minimum allocation of four elements.
- Growth copies elements in stable logical order; queue growth normalizes its
  wrapped storage without changing enumeration order.
- Collections expose `GetEnumerator()`. Native `foreach` recognizes the
  `GetEnumerator()` / `MoveNext()` / `Current()` protocol and retains the former
  indexed path only as a compatibility fallback.
- The specialization cache is keyed by declaring module, specialization kind,
  generated name, and exact type arguments, so repeated cross-module consumers
  share one generated specialization.
- Generic free functions and member methods infer exact arguments from typed
  parameters; explicit arguments use the same specialization cache.
- `where T : class`, `struct`, and `new()` constraints are checked before
  specialization. Generic interfaces and delegates become ordinary exact
  runtime descriptors and use the existing dispatch paths.
- Empty access and invalid indices fail through the existing verified managed
  array runtime path; negative initial capacities fail deterministically.

## Exit validation

- inference, explicit specialization, constraints, member methods, generic
  interfaces, and generic delegates;
- collection growth, mutation, enumeration order, and malformed access;
- `.rsbc` metadata round trip and hot-reload compatibility;
- interpreter and generated C++17 AOT execution/result/instruction parity;
- full warnings-as-errors build and repository-wide CTest.

The Phase 21 interpreter and generated C++17 AOT fixtures pass with exact result
and instruction-count parity. The current repository-wide Windows Release
warnings-as-errors matrix passes 37/37 tests.
