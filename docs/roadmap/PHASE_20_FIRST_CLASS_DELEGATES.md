# Phase 20 — First-Class Delegates and Closures

Phase 20 replaces the bounded Phase 18 event-slot lowering with deterministic,
first-class delegate values. Delegate identity and invocation remain explicit in
MIR, bytecode, artifacts, snapshots, and backend output.

## Implementation status

- **20A delegate descriptors and method references:** complete.
- **20B delegate invocation, parameters, and returns:** complete.
- **20C heap closures and local capture:** complete.
- **20D multicast delegates and general event storage:** complete.
- **20E artifacts, lifecycle policies, tooling, and backend parity:** complete.

## Delivered closure

- Exact sealed delegate descriptors and `Invoke` signatures, including
  `ref`, `out`, and `in` modifiers, are retained in `.rsbc` 0.7.
- Static, instance, virtual, and interface method references lower to explicit
  delegate opcodes shared by the interpreter and generated C++17 backend.
- Lambda captures use compiler-owned heap closures. Mutable locals and
  parameters are promoted to shared managed cells, so delegate copies and
  independently-created closures observe the same storage.
- Multicast combination/removal follows deterministic invocation-list order
  and last-contiguous-match removal. Events store that delegate value in one
  synthetic object field; external code can add/remove accessible events while
  only the declaring object can invoke them.
- Managed-heap snapshots retain scalar payloads, strings, arrays, closure
  fields, invocation-list edges, and persistent roots and can atomically roll
  the same heap back for deterministic replay.
- Closure and delegate layout changes are rejected by hot reload, while
  body-only edits retain stable lambda identities. Debug frames use `<lambda>`
  rather than compiler helper names, and synthetic closure/cell symbols remain
  outside language-service completion surfaces.

## Validation

- `realscript.phase20.delegates` covers descriptor/artifact round trips,
  method-reference dispatch, shared captures, multicast removal, general
  events, subscriber access, exact parameter modifiers, snapshot rollback,
  and hot-reload policy.
- `realscript.phase20.aot` compiles and executes the same delegate, closure,
  event, multicast, and reference-parameter behavior as native Windows C++17
  and compares result plus deterministic instruction accounting with the
  interpreter.

## Scope

### 20A — delegate descriptors and method references

- delegate declarations participate in exact named-type identity;
- static, instance, virtual, and interface method groups convert to compatible
  delegate values;
- a delegate value records its exact delegate TypeId, target receiver, callable
  SymbolId, and dispatch mode;
- null delegates are represented by the ordinary null-object value.

### 20B — invocation, parameters, and returns

- delegate locals, fields, parameters, and return values;
- direct invocation with exact signature and conversion checks;
- interpreter, generated C++ AOT, and Toolchain JIT use the same invocation
  metadata and error behavior;
- verifier rejects malformed delegate creation or invocation metadata.

### 20C — heap closures and capture

- compiler-owned closure descriptors with precise reference-field maps;
- deterministic capture ordering by source identity;
- local and parameter capture with shared mutable cells where language rules
  require reference capture;
- `this` and field capture without exposing synthetic symbols in tooling;
- nested closures retain captured managed references through GC.

### 20D — multicast delegates and general events

- deterministic combination and removal semantics;
- invocation-list order is preserved and the last non-void result is returned;
- events use delegate-backed storage rather than compile-time subscription
  slots;
- event access checks distinguish owner-side invocation from subscriber-side
  add/remove operations.

### 20E — artifact and runtime closure

- delegate and closure descriptors are persisted in a versioned `.rsbc`
  section and included in canonical hashes;
- snapshots and replay preserve invocation-list and capture state;
- hot reload accepts body-only edits and rejects incompatible delegate or
  closure-layout changes;
- debugger/DAP display source lambdas and user methods rather than closure
  helpers;
- LSP completion, definition, rename, signature help, and diagnostics understand
  delegate values and captures;
- Windows and Ubuntu warnings-as-errors builds pass the full CTest suite.

## Deterministic ABI rules

- Delegate signatures use exact parameter modifiers, exact named TypeIds, and
  return type identity.
- Capture fields are ordered by the captured symbol's stable source identity;
  invocation-list entries retain source-order combination.
- Delegate equality and removal compare callable identity, dispatch mode,
  receiver identity, and closure identity; removal deletes the last matching
  contiguous invocation subsequence.
- Compiler-owned closure and capture-cell types are synthetic and excluded from
  user-facing symbol and stack-frame surfaces.

## Explicit non-goals

- expression trees or runtime code generation;
- reflection-based invocation;
- variance (Phase 21 generic delegate work);
- cross-thread delegate invocation or asynchronous handlers (Phase 24).
