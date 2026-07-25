# Phase 2C — Linking, Observability and Embedding

## Status

Implemented after the Phase 2B interpreter baseline.

## Goal

Turn the bytecode interpreter into a reusable game-engine subsystem:

- validate and index modules once;
- reject duplicate stable identities before execution;
- pre-register host functions;
- expose deterministic tracing and runtime statistics;
- provide a small embedding facade that hides interpreter setup.

## ProgramImage

`ProgramImage::link()` verifies every module and builds immutable indexes for stable `SymbolId` and qualified function names. Linking rejects duplicate symbols and duplicate qualified names before an invocation begins.

A linked image can be shared by multiple `EngineRuntime` instances and reused for repeated calls without rebuilding the program index.

## BindingRegistry

Host functions can be registered by stable symbol or canonical name. The registry is immutable from the interpreter's point of view and acts as the initial native-binding boundary before generated thunks are added.

## Observability

Execution optionally reports:

- function enter and exit;
- executed opcodes;
- selected branch targets;
- external calls;
- runtime errors.

Tracing is observational. It does not alter instruction-budget accounting or script semantics.

Runtime statistics include instruction count, function calls, external calls, branches and maximum call depth.

## EngineRuntime facade

`EngineRuntime` owns a shared linked image, accepts a shared binding registry and invokes functions by qualified name. This is the preferred embedding entry point for engine integration; direct `Interpreter` use remains available for low-level tests.

## Tests

Phase 2C covers linked image indexing, duplicate rejection, host binding by name, execution traces, runtime statistics, maximum call depth and deterministic branch/instruction counts.

## Next slice

Phase 3A should establish heap objects, managed references, strings/arrays as runtime objects, precise root maps and the first incremental GC boundary.
