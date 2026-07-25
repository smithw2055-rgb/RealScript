# Phase 2B — Bytecode Interpreter and Runtime Baseline

## Status

Implemented as the first executing runtime slice after the verified Phase 2A bytecode toolchain.

## Goal

```text
.rsbc / verified bytecode modules
              │
              ▼
        Interpreter
              │
              ├── typed register frames
              ├── local storage
              ├── calls and returns
              ├── branch edge transfer
              ├── checked traps
              └── deterministic budgets
```

Execution never bypasses bytecode verification.

## Runtime values

The primitive runtime profile supports:

- `bool`;
- 32-bit language integers represented in a 64-bit host carrier;
- strings;
- raw `null` temporaries;
- a distinct null-string reference produced by `null → string`.

The separate null-string representation preserves the statically selected string overload while retaining null identity.

## Frames and control flow

Each call allocates:

- one register array sized from `registerTypes`;
- one local array sized from `localTypes`;
- the current basic block;
- the function arguments.

Branch edge arguments are copied to a temporary vector before target block parameters are written. This gives parallel-copy semantics and prevents source registers from being overwritten during loop and merge transfers.

## Calls

Direct calls resolve by stable `SymbolId` across all loaded modules. References not found in loaded bytecode modules are delegated to an optional host resolver.

The host resolver receives the verified function-reference signature, ordered arguments and a writable runtime-error object. An unresolved reference produces a deterministic runtime failure.

## Checked execution

The interpreter reports structured errors for:

- division or remainder by zero;
- 32-bit integer overflow;
- invalid arguments or runtime types;
- unresolved external functions;
- missing or duplicate function identities;
- instruction budget exhaustion;
- recursion-depth exhaustion;
- invalid program state detected defensively at runtime.

Runtime errors include a script-level function stack.

## Budgets

Every instruction and every terminator consumes one instruction-budget unit. Calls share one budget across the entire invocation tree. Recursion depth is checked before a frame is entered.

The same verified module, arguments and limits produce the same value, error and executed-instruction count.

## CLI

```bash
rsc game.rs --run Game.Main::main
```

The initial CLI entry accepts a no-argument function and prints its primitive return value. Embedders use the C++ API for typed arguments, limits and host resolution.

## Tests

Phase 2B covers:

- arithmetic and direct calls;
- short-circuit block parameters;
- loop execution;
- cross-module calls;
- host external resolution;
- unresolved externals;
- division traps;
- integer overflow;
- instruction budgets;
- recursion limits and stack traces;
- deterministic result and instruction counts.

## Explicit limitations

Phase 2B does not implement objects, GC, exceptions, async tasks, native binding generation, debugger sequence points, suspension/resume, persistent runtime snapshots or concurrent execution.

## Next slice

Phase 2C should add execution tracing and differential reference tests, reusable module linking, richer host binding contracts, runtime statistics and a stable embedding facade before the object/GC phase.
