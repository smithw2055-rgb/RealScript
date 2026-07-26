# Phase 4 — Debugging, Language Services and Hot Reload

## Status

Implemented as the first tooling and live-development layer on top of the Phase 3A–3E compiler and runtime.

## Goal

Phase 4 makes the existing compiler/runtime observable and editable without introducing a second source model:

```text
SourceText / symbols / spans
          │
          ├──► .rsbc 0.5 debug tables
          │       └──► DebugController / DebugSession / DAP
          │
          ├──► LanguageService / LSP
          │
          └──► body-only compatibility analysis / Hot Reload
```

All three surfaces reuse compiler source spans, stable SymbolIds, exact TypeIds and linked ProgramImages.

## Phase 4A — debug information

The compiler emits:

- deterministic source-file tables;
- source content fingerprints and line-start maps;
- function declaration and body ranges;
- instruction and terminator sequence points;
- parameter and local-variable names, slots and exact TypeIds;
- lexical local-variable scope ranges.

The bytecode format advances to `.rsbc 0.5` and adds a sixth `DEBUG` section. Debug metadata is optional for hand-authored in-memory test modules, but compiled `.rsbc` modules carry complete metadata. The decoder and verifier reject missing source references, invalid slots, duplicate sequence locations and malformed ranges.

## Phase 4B — debugger and DAP

`DebugController` binds source breakpoints to executable sequence points and supports:

- stop on entry;
- source breakpoints;
- asynchronous pause requests;
- continue;
- step in;
- step over;
- step out;
- terminate;
- stack-frame capture;
- arguments and locals inspection.

`DebugSession` runs the interpreter on a worker thread. A stop blocks execution until the client selects a resume mode, allowing a UI or protocol adapter to inspect state safely.

`rsdebug` exposes the session through the Debug Adapter Protocol over standard `Content-Length` framing. The initial adapter implements initialize, setBreakpoints, launch, configurationDone, threads, stackTrace, scopes, variables, continue, next, stepIn, stepOut, pause and disconnect/terminate.

## Phase 4C — language server

`LanguageService` owns an incremental workspace and reuses `Compilation` snapshots. It provides:

- diagnostics;
- keyword and symbol completion;
- hover;
- go to definition;
- find references;
- workspace rename edits;
- document symbols.

`rslsp` exposes these operations over JSON-RPC/LSP standard framing. It supports initialize/shutdown/exit, didOpen/didChange/didClose, completion, hover, definition, references, documentSymbol and rename, plus publishDiagnostics notifications.

The current rename/reference implementation uses stable compiler definitions when unambiguous and token-level workspace matching for occurrences. A later incremental-syntax phase can replace the fallback with a complete bound-reference index without changing the public service model.

## Phase 4D — hot reload

The first hot-reload profile is deliberately body-only:

- module set must remain unchanged;
- type set, kind, field layout and enum members must remain unchanged;
- function SymbolIds, names, parameter types and return types must remain unchanged;
- function bodies, local frame layout, sequence points and source text may change.

`prepare()` links and verifies the replacement image before compatibility comparison. `apply()` atomically replaces the `EngineRuntime` ProgramImage only after a compatible plan is produced. Active invocations retain their original shared ProgramImage snapshot; later invocations observe the new image.

Rejected changes return structured issue kinds for invalid bytecode, module-set changes, type-layout changes, function-set changes and function-signature changes.

## Tools

```bash
# Compile sources and start a DAP server on stdin/stdout
rsdebug game.rs scripts/common.rs

# Start the LSP server on stdin/stdout
rslsp
```

Both tools are dependency-free C++17 executables and use the same protocol framing/parser implementation.

## Validation

Phase 4 tests cover:

- source, sequence-point and local metadata round trips plus malformed-range rejection;
- `.rsbc 0.5` verification;
- breakpoint binding, entry stops, stepping, termination and variable capture;
- JSON protocol parsing, non-finite-number handling and bounded framing;
- LSP capabilities, diagnostics and document symbols;
- DAP initialize, launch, stopped events and stack frames;
- compatible function-body replacement;
- rejection of signature-changing hot reload;
- complete Phase 1A–4 regression coverage.

## Deliberate limits

Phase 4 does not yet add:

- expression evaluation while paused;
- mutable variables through DAP;
- conditional breakpoints, logpoints or data breakpoints;
- inline stack frames or optimized variable locations;
- persistent on-disk language-server indexes;
- syntax-aware partial text edits beyond full-document synchronization;
- type-layout migration or additive-field hot reload;
- replacement of currently active frames.

These are extensions of the established interfaces. Phase 5 consumes the stable MIR, runtime semantics and source metadata established here.
