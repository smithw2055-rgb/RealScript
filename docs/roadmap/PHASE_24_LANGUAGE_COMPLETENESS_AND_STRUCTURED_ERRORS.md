# Phase 24 — Language Completeness and Structured Errors

Phase 24 completes the C#-style convenience and structured-error slice defined
by the Phase 18–24 roadmap. The features are native Syntax, Bound, Typed MIR,
bytecode, interpreter, and generated C++17 AOT semantics; no source rewriting is
used.

## Work breakdown

- **24A inference and null/conditional/type operators:** complete.
- **24B initializers and flexible argument binding:** complete.
- **24C patterns and switch expressions:** complete.
- **24D exceptions and deterministic cleanup:** complete.
- **24E artifacts, LSP/DAP/hot reload, docs, and backend parity:** complete.

## Delivered language features

- `var`; lazy `?:` and `??`; null-conditional `?.`; `is`, `as`, and `typeof`;
- object, struct, collection, and dictionary-entry initializers;
- optional, named, and `params` arguments with source-order evaluation;
- constant, null, type, and discard patterns, pattern variables, `when` guards,
  type-pattern switch statements, and exhaustive switch expressions;
- `throw`, rethrow, ordered typed/catch-all clauses, and `try/catch/finally`.

## Exception model

- A thrown value must be a non-null script class object. Catch matching uses the
  same deterministic runtime assignability rules as inheritance and interfaces.
- Exception regions are explicit Typed MIR and `.rsbc` 0.9 handler tables.
  Verifiers validate protected blocks, handler targets, catch types, and locals.
- Exceptions cross ordinary and delegate calls. `finally` executes on normal
  completion, script exception propagation, `return`, `break`, and `continue`.
- An uncaught script value becomes `runtime::ErrorCode::ScriptException` at the
  host boundary. Runtime faults such as budget exhaustion, invalid bytecode, or
  arithmetic traps remain structured host runtime errors and are not catchable
  script objects.
- Generated C++17 uses the same explicit pending-exception channel and handler
  ordering as the interpreter; it does not rely on platform C++ exception
  unwinding.

## Tooling and artifacts

- MIR printing and bytecode disassembly expose `throw` and handler tables.
- `.rsbc` 0.9 canonically encodes handler metadata while the decoder retains
  legacy 0.6–0.8 support.
- LSP completion includes Phase 24 and prior native keywords. Catch/finally
  blocks retain original-source DAP sequence points.
- Hot-reload body fingerprints include catch type, protected blocks, handler
  target, and exception-local identity.
- The current supported and unsupported C# surface is published in the English
  and Chinese compatibility matrices.

## Validation

- `realscript.phase24.language-completeness` covers diagnostics, Typed MIR,
  codec round trips, verifier/disassembly output, interpreter execution,
  LSP/DAP metadata, hot reload, caught/rethrown/uncaught exceptions, and cleanup
  on all supported exits.
- `realscript.phase24.aot` compiles generated C++17 and compares results and
  deterministic instruction accounting with the interpreter, including nested
  exception propagation and `finally`.

## Deliberate limits

This phase does not add exception filters, `using`, `lock`, platform/native
exception interop, exception serialization across coroutine suspension, or the
complete .NET base-class library.
