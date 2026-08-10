# Runtime, GC, and Embedding

[Documentation Home](README.md) | [Compilation, MIR, and Bytecode](COMPILATION_AND_BYTECODE.md)

The RealScript runtime is designed for embedding in a C++17 engine. It exposes explicit ownership, structured failures, resource budgets, precise managed roots, and stable host binding boundaries.

## Program Images

`ProgramImage` is an immutable linked view of verified script modules. Linking resolves function references, validates module compatibility, and prepares descriptors used by the interpreter, AOT runtime, debugger, and hot-reload system.

A program image can be shared across multiple invocations. Hot reload publishes a replacement image atomically while active calls retain their original image.

## Engine Runtime

`EngineRuntime` is the main embedding facade. It coordinates:

- the current `ProgramImage`;
- native function bindings;
- managed heap ownership;
- native handle registration;
- execution options and budgets;
- trace callbacks;
- deterministic execution state;
- profiling;
- hot reload.

Conceptual host setup:

```cpp
auto image = runtime::ProgramImage::link(std::move(modules), error);
auto sharedImage = std::make_shared<runtime::ProgramImage>(std::move(*image));
auto bindings = std::make_shared<runtime::BindingRegistry>();

runtime::EngineRuntime engine(sharedImage);
engine.setBindings(bindings);

auto result = engine.invoke("Game.Main::main", {}, options);
```

## Runtime Values

The runtime `Value` model supports:

- no value / void state;
- typed null string, object, and array values;
- Boolean values;
- checked integer values;
- 64-bit long values;
- binary64 doubles;
- enum values with exact type identity;
- copy-semantic struct values;
- host strings and managed strings;
- generation-checked object references;
- native resource handles.

Type validation occurs at function boundaries and relevant operations.

## Interpreter

The interpreter executes typed registers, local slots, basic blocks, and explicit terminators. Each frame records:

- function identity;
- arguments;
- locals;
- registers;
- return state;
- source/debug location;
- precise GC roots.

Execution produces a structured result containing either a value or a runtime error.

## Structured Runtime Errors

Examples include:

- division by zero;
- checked integer overflow;
- null reference;
- array bounds violation;
- negative array length;
- type mismatch;
- stale object reference;
- stale or wrong-type native handle;
- unresolved native binding;
- instruction budget exhaustion;
- recursion budget exhaustion;
- heap or allocation limit exhaustion;
- deterministic replay mismatch;
- explicit execution termination.

Errors carry script stack information instead of relying on C++ undefined behavior or process termination.

## Execution Budgets

Execution options may limit:

- instruction count;
- recursion depth;
- heap size;
- allocation pressure;
- incremental GC work;
- tracing or profiling behavior.

Budgets are part of observable runtime semantics and are preserved by interpreter, AOT, and JIT paths.

## Managed Heap

The managed heap is non-moving. Script-visible managed references use `ObjectRef`, which contains:

- heap identity;
- slot index;
- generation;
- object kind.

Generation checks reject stale references after a slot is reused. Heap identity prevents accidental cross-runtime access.

Managed payloads include strings, arrays, records, and language-visible object instances.

## Precise Roots

The collector does not scan arbitrary native memory. Roots come from:

- interpreter frame arguments, locals, and registers;
- AOT shadow-stack frames;
- persistent host roots;
- explicitly retained runtime values.

Struct values are scanned recursively when their descriptors contain managed references.

## Incremental Mark/Sweep

Collection proceeds through explicit incremental work units. The runtime can request collection and perform bounded work at instruction safepoints.

Important properties:

- deterministic slot and work-list behavior;
- exact root rescanning;
- safe allocation during an active collection;
- generation-safe slot reuse;
- full collection support;
- collection statistics.

## Write Barriers

Arrays, records, object fields, and struct-containing managed references use write barriers. A black object receiving a white reference during an active collection is made visible to the collector.

## Persistent Roots

Hosts can retain script values through RAII-style persistent roots. Root validation recursively rejects nested object references that belong to another heap.

Persistent roots may hold arrays, objects, strings, or structs that contain managed references.

## Heap Diagnostics

The runtime can produce deterministic heap snapshots containing:

- roots;
- objects;
- outgoing edges;
- object sizes;
- per-type counts and sizes.

Additional diagnostics include:

- retaining-path discovery;
- leak summaries;
- string, array, and record counts;
- shutdown-time ownership checks.

## Native Bindings

`BindingRegistry` maps stable script-visible binding names to host functions. A binding receives typed runtime values and returns a typed result or structured error.

Phase 6 adds a determinism policy to each binding:

- `Deterministic`;
- `Recordable`;
- `NonDeterministic`.

This policy controls Strict, Record, and Replay execution behavior.

## Native Handles

Host-owned resources use `NativeHandleRegistry` rather than raw pointers. A handle records:

- registry identity;
- slot;
- generation;
- exact host `TypeId`.

The registry rejects stale handles, wrong types, and cross-registry access.

## Tracing and Profiling

Execution may emit events for:

- function entry and return;
- instructions and terminators;
- branches;
- native calls;
- GC work;
- runtime errors.

The same event ordering is used by interpreter and AOT execution so deterministic digests and profiles remain comparable.

## Embedding Stability

The v0.2.0 native and AOT contracts are designed for same-SDK integration.
Hosts should not assume that precompiled modules remain binary compatible
across unrelated compiler toolchains or future RealScript versions until the
ABI is explicitly frozen.
