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

- source breakpoint binding (including conditional breakpoints via `BreakpointSpec` and `setConditionalBreakpoints`);
- exception breakpoints (breaking on caught/uncaught exceptions via `setExceptionBreakpoints`);
- reentrant DAP safepoints during nested runtime execution;
- same-line sequence-point re-entry suppression;
- stop on entry;
- pause and continue;
- step in;
- step over;
- step out;
- termination;
- thread-safe stack and variable snapshots;
- heap retaining-path profiler (interactive path-to-root query to diagnose memory leaks);
- side-effect-free expression evaluation (`evaluate`) on any paused frame.

Source breakpoints bind to the nearest executable sequence point when an exact source line does not contain executable code.

`DebugSession` runs the interpreter on a worker thread and uses condition variables to pause safely. Debug clients never inspect partially mutated frame state.

## Debug Adapter Protocol

Start the adapter with the source files that form the program:

```bash
rsdebug game.rscript common.rscript
```

Or listen on a TCP port so an IDE can attach over a socket:

```bash
rsdebug --dap-port 4711 game.rscript common.rscript
```

Game engines can also embed `realscript::debug::EmbeddedDapServer` (`realscript/debug/DapServer.h`) directly in-process to accept live TCP attach connections (`127.0.0.1:<port>`) without restarting the host application.

The adapter communicates through standard DAP `Content-Length` framing over stdin/stdout or TCP sockets.

Implemented request families include:

- `initialize` (advertising `supportsConditionalBreakpoints` and `supportsEvaluateForHovers`);
- `launch` and `attach`;
- `configurationDone`;
- `setBreakpoints` (including per-breakpoint `condition` expressions);
- `threads`;
- `stackTrace`;
- `scopes`;
- `variables`;
- `evaluate` (watch and hover expression evaluation);
- `continue`;
- `next`;
- `stepIn`;
- `stepOut`;
- `pause`;
- `disconnect` and termination.

The adapter emits asynchronous stopped and terminated events. Stopped events do not require a follow-up client request to become visible.

## Stack Frames, Variables, and Watch Evaluation

A paused frame exposes:

- function name and stable identity;
- source path and current range;
- parameters;
- locals visible in the active lexical scope;
- runtime value formatting.

`DebugController::evaluate` and DAP `evaluate` provide a deterministic, side-effect-free expression evaluator supporting locals, parameters, implicit/explicit `this` fields, nested struct and object fields, array indexing (`arr[i]`), `.Length`, literals, arithmetic, comparisons, and boolean logic (`&&`, `||`, `!`). Arbitrary function-call side effects remain disallowed during evaluation.

## Language Server

Start the language server:

```bash
rslsp
```

The server uses standard JSON-RPC/LSP framing over stdin/stdout.

Implemented capabilities include:

- document open, change, and close notifications;
- diagnostics;
- completion (including receiver-type member `.` dot-completion);
- signature help (`textDocument/signatureHelp` with overload parameters and active parameter tracking);
- hover;
- definition;
- references;
- rename;
- document symbols;
- workspace source tracking, `realscript.project.json` discovery, and `.d.rscript` ambient host declaration loading (`GameApi::exportDeclarationStub()`);
- diagnostic publication.

The initial synchronization model uses full-document updates.

## Language Service Architecture and VS Code Extension

`LanguageService` reuses the normal compiler and `BuildSnapshot` model. It does not maintain an independent parser or type checker.

Each source file maintains a cached `ParseSnapshot` (lexed tokens, parsed syntax tree, and public declaration fingerprint). In addition, `LanguageService` maintains an incremental subgraph metadata rebuild cache and an asynchronous request bridge for low-latency IDE response. Body-only edits reuse parsed syntax trees of untouched files and skip rebinding dependent modules whose imported public signatures have not changed.

The repository includes an official Visual Studio Code extension under `tools/vscode-realscript/` providing:

- automatic `rslsp` Language Client startup for `.rscript` and `.d.rscript` files;
- a `realscript` Debug Adapter supporting both `"request": "launch"` (`rsdebug --dap`) and `"request": "attach"` (connecting to `rsdebug --dap-port <port>` or an in-engine `EmbeddedDapServer` over TCP);
- TextMate syntax highlighting and language configuration for RealScript keywords, attributes, `sequence` coroutines, and ECS `Query<...>()` expressions.

## Auto-Binding Generator (`rsbind`)

`rsbind` parses C++ header files and emits type-safe binding code and ECS raw column helpers:

```bash
rsbind --output src/game/GameBindings.gen.cpp include/game/*.h
```

`rsbind` generates:
- host type registrations and member thunks;
- field and template type bindings;
- zero-copy ECS raw column helpers (`bindEcsRawColumnAuto`);
- `.d.rscript` ambient type definition stubs for LSP editor autocompletion.

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

## Publication Semantics and Stateful Scene Hot Reload

`EngineRuntime` publishes a compatible replacement `ProgramImage` atomically.

- Active calls retain the image on which they started.
- New invocations observe the replacement image.
- Old images remain alive while borrowed references or active calls still require them.

For structural changes during editor/gameplay iteration, `ScriptRuntime::replaceProgram` and `SceneScriptRuntime::hotReload` provide stateful instance migration:

- allocates new component instances against the updated `ProgramImage`;
- migrates matching fields, static fields, and fields renamed with `[FormerlySerializedAs("oldName")]`;
- preserves synthetic coroutine (`$sequence_*`) and event (`$event_*`) state fields;
- recursively patches cross-component `ObjectRef` fields, arrays, and delegate targets to point to the newly migrated instances.

## Chrome Tracing Profiler Export

`exportChromeTraceJson(profile, &gcStats)` serializes `ExecutionProfile` function timing/instruction metrics and `GcStatistics` counters into standard Chrome Tracing / Perfetto JSON (`{"traceEvents":[...]}`) for visual flame-graph inspection in `chrome://tracing` or `ui.perfetto.dev`.

## Current Tooling Limits

The current tooling surface intentionally excludes:

- data (watchpoint) breakpoints;
- side-effecting function invocations inside watch expressions;
- partial-document incremental LSP text sync;
- in-place rewriting of actively executing stack frames.

These limits are explicit protocol and ABI boundaries rather than silent partial behavior.
