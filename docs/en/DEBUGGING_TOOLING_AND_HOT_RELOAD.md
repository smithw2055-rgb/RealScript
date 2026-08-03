# Debugging, Tooling, and Hot Reload

[Documentation Home](README.md) | [Runtime, GC, and Embedding](RUNTIME_GC_AND_EMBEDDING.md)

RealScript treats source locations and symbol information as runtime metadata. The compiler, debugger, language service, and hot-reload system share the same source model and stable identities.

## Debug Information

Compiler-produced `.rsbc` 0.9 modules include:

- source file IDs and paths;
- source content fingerprints;
- line-start tables;
- function declaration and body ranges;
- instruction and terminator sequence points;
- parameter and local-variable metadata;
- local slots and exact type IDs;
- lexical scopes.

The bytecode verifier validates source ranges, line tables, sequence targets, local slots, types, and scope boundaries before debugging begins.

## Debug Runtime

`DebugController` binds to a linked program image and manages:

- source breakpoint binding;
- stop on entry;
- pause and continue;
- step in;
- step over;
- step out;
- termination;
- thread-safe stack and variable snapshots.

Source breakpoints bind to the nearest executable sequence point when an exact source line does not contain executable code.

`DebugSession` runs the interpreter on a worker thread and uses condition variables to pause safely. Debug clients never inspect partially mutated frame state.

## Debug Adapter Protocol

Start the adapter with the source files that form the program:

```bash
rsdebug game.rs common.rs
```

The adapter communicates through standard DAP `Content-Length` framing over stdin/stdout.

Implemented request families include:

- `initialize`;
- `launch`;
- `configurationDone`;
- `setBreakpoints`;
- `threads`;
- `stackTrace`;
- `scopes`;
- `variables`;
- `continue`;
- `next`;
- `stepIn`;
- `stepOut`;
- `pause`;
- `disconnect` and termination.

The adapter emits asynchronous stopped and terminated events. Stopped events do not require a follow-up client request to become visible.

## Stack Frames and Variables

A paused frame exposes:

- function name and stable identity;
- source path and current range;
- parameters;
- locals visible in the active lexical scope;
- runtime value formatting.

Expression evaluation with arbitrary side effects is not part of the v0.1 debugger.

## Language Server

Start the language server:

```bash
rslsp
```

The server uses standard JSON-RPC/LSP framing over stdin/stdout.

Implemented capabilities include:

- document open, change, and close notifications;
- diagnostics;
- completion;
- hover;
- definition;
- references;
- rename;
- document symbols;
- workspace source tracking;
- diagnostic publication.

The initial synchronization model uses full-document updates.

## Language Service Architecture

`LanguageService` reuses the normal compiler and `BuildSnapshot` model. It does not maintain an independent parser or type checker.

Workspace changes update source inputs and rebuild only invalidated modules where possible. Symbol definitions and stable identities drive precise navigation when available. Token-level fallback is used for some cross-workspace reference and rename cases.

## Protocol Safety

The JSON and protocol layers enforce defensive limits:

- maximum message size of 16 MiB;
- rejection of duplicate `Content-Length` headers;
- bounded JSON nesting;
- rejection of duplicate object keys;
- rejection of unescaped control characters;
- valid JSON output for non-finite floating-point values.

## Hot Reload

The first hot-reload profile supports function-body replacement while preserving runtime layout compatibility.

A replacement image is linked and verified before it can be published.

Compatible changes may include:

- function bodies;
- local layouts;
- debug metadata;
- source sequence points.

Incompatible changes include:

- module set changes;
- type additions or removals;
- class or struct field-layout changes;
- enum member or value changes;
- function additions or removals;
- function signature changes.

The compatibility analyzer returns structured rejection reasons.

## Publication Semantics

`EngineRuntime` publishes a compatible replacement `ProgramImage` atomically.

- Active calls retain the image on which they started.
- New invocations observe the replacement image.
- Old images remain alive while borrowed references or active calls still require them.

Running frames are not rewritten in place.

## Current Tooling Limits

The v0.1 baseline does not include:

- conditional breakpoints;
- data breakpoints;
- arbitrary side-effecting expression evaluation;
- partial-document LSP synchronization;
- inheritance-aware navigation;
- class-layout migration;
- running-frame replacement;
- coroutine-state migration.

These limits are explicit protocol and ABI boundaries rather than silent partial behavior.
