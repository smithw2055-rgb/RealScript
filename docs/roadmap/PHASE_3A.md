# Phase 3A — Managed Heap and Precise GC Baseline

## Status

Implemented as the first managed-object slice after the linked bytecode runtime.

## Goal

Establish a small, testable managed-memory boundary before adding classes and object bytecode:

```text
Runtime Value / ObjectRef
          │
          ▼
Stable Slot + Generation Handle
          │
          ▼
String / Array / Record Heap Objects
          │
          ├── Precise Shadow-Stack Roots
          ├── Persistent Host Roots
          ├── Incremental Mark/Sweep
          ├── Mutation Write Barrier
          └── GC Statistics
```

The slice deliberately avoids changing the source-language object model. It first makes object lifetime, stale-handle rejection and incremental collection correct.

## Managed references

`ObjectRef` contains:

- a stable slot index;
- a generation number;
- the expected object kind.

A reference is valid only when all three values match a live heap slot. Reusing a freed slot increments its generation, so an old reference cannot silently refer to a new object.

No script or host API receives a raw heap pointer.

## Object layouts

The non-moving Phase 3A heap implements:

- `String`: UTF-8 byte storage;
- `Array`: a fixed-length vector of runtime `Value` elements;
- `Record`: a fixed field vector used as the field-layout precursor for later script classes.

Each allocation has an internal header containing object kind, mark state and accounted byte size.

Script `const.string` instructions now allocate managed string objects. Host code can still pass an inline `std::string`; string equality compares content across inline and managed representations.

## Precise roots

Interpreter frames register three exact value ranges in a `ShadowStack`:

- call arguments;
- local slots;
- typed registers.

Only `Value` entries containing `ObjectRef` are traced. Native C++ stack bytes are never conservatively scanned.

Long-lived host references use generation-safe persistent root tokens:

```cpp
const auto token = heap.addPersistentRoot(reference);
// retain across invocations and collections
heap.removePersistentRoot(token);
```

## Incremental collector

The baseline collector is non-moving tri-color-style Mark/Sweep with explicit phases:

1. `Idle`;
2. `Mark`;
3. `Sweep`.

`ManagedHeap::step()` consumes an object/slot work budget rather than wall-clock time. The interpreter calls it at instruction and terminator safepoints using `Limits::gcWorkBudget`.

Roots are rescanned at each incremental step. Array and record writes execute a write barrier. If a marked object gains a reference to an unmarked object during sweep, marking is reopened before collection continues.

Allocations during an active collection are treated as live and scanned before sweep resumes.

## Runtime integration

`Interpreter` and `EngineRuntime` own or share a `ManagedHeap`. Repeated `EngineRuntime` invocations therefore share managed objects and persistent roots.

`RuntimeStatistics::gcWorkPerformed` reports collector work charged during an invocation. Optional tracing emits `gc-step` events without changing script semantics.

Invalid or stale managed arguments produce `InvalidObjectReference`. Heap-limit failures produce `OutOfMemory`.

## Tests

Phase 3A adds coverage for:

- string, array and record layouts;
- transitive object graph retention;
- unreachable-object reclamation;
- persistent roots;
- slot generation reuse and stale handles;
- one-unit incremental budgets;
- write barriers that reopen marking;
- interpreter shadow-stack roots;
- managed script string literals;
- invalid managed arguments;
- allocation/collection stress and statistics.

## Validation

Validated with:

- GCC, C++17, warnings-as-errors;
- Clang, C++17, warnings-as-errors;
- AddressSanitizer;
- UndefinedBehaviorSanitizer;
- the complete Phase 1A–3A test matrix.

## Explicit limitations

Phase 3A does not implement:

- source-language `class`, `struct`, array or field expressions;
- object/array allocation bytecodes;
- type descriptors or inherited field layouts;
- moving or compacting collection;
- generational remembered sets;
- weak references, ephemerons or finalizers;
- concurrent collection;
- GC maps for native AOT frames;
- host resource lifetime ownership.

## Next slice

Phase 3B should add:

1. runtime type descriptors and field layouts;
2. object and array allocation/access MIR and bytecode;
3. language-level reference types and null checks;
4. generated GC reference maps;
5. native resource handle wrappers;
6. heap snapshots and leak diagnostics.
