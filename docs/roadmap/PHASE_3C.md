# Phase 3C — Arrays, Native Handles and Heap Diagnostics

## Status

Implemented on top of the Phase 3B language-visible object model.

## Goal

Complete the first practical managed-runtime boundary for embedding RealScript in a C++17 engine:

```text
source arrays
    │
    ▼
exact array types and element operations
    │
    ▼
Typed MIR / .rsbc 0.3 introduction (current producer: 0.4) / verifier / interpreter
    │
    ├── precise GC edges and write barriers
    ├── generation-safe native resource handles
    └── heap snapshots, retaining paths and leak summaries
```

Phase 3C deliberately combines the source-language array slice with the ownership and diagnostics work required before methods, constructors and larger object graphs are added.

## Source-language arrays

The implemented syntax supports single-dimensional arrays of primitive values, strings, classes and opaque native handles:

```csharp
module Game.Inventory;

class Item
{
    int value;
}

int sum()
{
    int[] values = new int[3];
    values[0] = 40;
    values[1] = 2;
    return values[0] + values[1] + values.length;
}

Item first(Item item)
{
    Item[] items = new Item[1];
    items[0] = item;
    return items[0];
}
```

Implemented behavior:

- `T[]` type syntax;
- `new T[length]` allocation;
- `array[index]` reads;
- `array[index] = value` writes;
- read-only `.length`;
- `null -> T[]` conversion and array identity equality;
- array types in locals, fields, parameters, returns and overload signatures;
- deterministic array TypeIds derived from the canonical element type name;
- default values of `false`, `0`, null string, null object, null array or invalid handle according to the element type;
- structured null, negative-length and out-of-range runtime errors.

The first slice does not implement array literals, resizing, slicing, multidimensional or nested array syntax, covariance, `foreach`, spans or unsafe element access.

## Typed MIR

Phase 3C adds the following operations:

```text
conv.null.array
new.array
array.length
load.element
store.element
```

Array instructions carry:

- the exact array result TypeId where a value is produced;
- the primitive element category;
- an exact element TypeId for class or array reference elements.

The MIR verifier checks receiver type, index type, element type, exact reference identity, operand counts and result identity. Array values remain typed references and are never represented as untyped host pointers.

## `.rsbc` 0.3 introduction

Phase 3C advanced the physical bytecode version from 0.2 to 0.3. The current Phase 3E producer emits 0.4 while retaining the array and handle model introduced here.

Version 0.3 adds:

- `array` and `handle` type tags;
- exact local, register and block-parameter TypeIds;
- array element category and element TypeId fields in every instruction record;
- array allocation, length, load, store and null-conversion opcodes;
- verifier rules for exact array signatures and element metadata.

The five-section container remains unchanged: strings, type descriptors, function references, functions and code. Version 0.3 is intentionally not binary compatible with 0.2; the decoder rejects older or newer versions instead of guessing a layout.

## Managed arrays and GC

A managed array header records:

- exact array TypeId;
- element primitive category;
- exact class/array element TypeId when applicable;
- fixed length and payload storage.

Reference-bearing arrays participate in precise tracing. Every array element store validates the element category and exact runtime TypeId before committing the write. Stores of managed references execute the incremental collector write barrier.

`collectFull()` now guarantees a collection that starts from the heap state visible at the call boundary. If an incremental cycle is already active, it is drained first and a fresh complete cycle is then executed. This removes floating garbage from diagnostic and shutdown collections while preserving safe allocation during incremental collection.

## Heap ownership contract

`ObjectRef` now includes a unique `heapId` in addition to slot, generation and object kind. A reference from one `ManagedHeap` is rejected by every other heap even if slot and generation values happen to match.

The ownership rules are:

- a managed object belongs to exactly one heap for its complete lifetime;
- script frames expose arguments, locals and registers through `ShadowStack` precise roots;
- engine-owned references use persistent roots;
- `ManagedHeap::retain()` returns a move-only `PersistentRoot` RAII object;
- persistent roots can be updated or released explicitly;
- stale generations and cross-heap references produce structured errors;
- native resources are never stored as raw C++ pointers in `Value`.

## Native handles

`NativeHandle` is an opaque value containing:

```cpp
slot + generation + registry identity + host type TypeId
```

`NativeHandleRegistry` owns the host-side `shared_ptr<void>` resource table and provides:

- typed creation;
- expected-TypeId validation during resolution;
- generation-safe stale-handle rejection;
- deterministic slot reuse;
- explicit release and registry-wide invalidation;
- live-resource counts and debug names.

`handle` is available as an opaque language/runtime value. It can be stored, passed through functions, returned to the host, compared by identity and stored in arrays or fields. Script code cannot construct, dereference or destroy a native resource. The host validates a returned handle through the registry before use.

`EngineRuntime` owns a default registry and allows an engine to install a shared registry through `setNativeHandles()`.

## Heap diagnostics

Phase 3C adds deterministic diagnostic APIs:

- `HeapSnapshot` with object metadata, roots and labeled edges;
- stable textual snapshot output;
- `retainingPath()` breadth-first root-to-object paths;
- per-object size, kind, TypeId, element metadata and value counts;
- persistent-root and optional shadow-stack root reporting;
- `leakSummary()` for shutdown and test diagnostics;
- live/peak/allocation/reclamation statistics.

Snapshots are observational. They do not pin objects or change collector state.

## Tests

The Phase 3C target covers:

- integer, string and class-reference arrays;
- array fields, parameters and returns;
- default values and exact array TypeIds;
- null, negative length and bounds failures;
- MIR/bytecode generation and canonical 0.3 round trips at introduction time (current snapshots use 0.4);
- verifier rejection of corrupted element metadata;
- object-array GC retention and write barriers;
- RAII persistent roots, snapshots and retaining paths;
- cross-heap reference rejection;
- native handle creation, type checking, stale generation rejection and bytecode round trips;
- incremental allocation stress and exact full-collection cleanup.

## Validation

Required validation for this slice:

- GCC, C++17, warnings-as-errors;
- Clang, C++17, warnings-as-errors;
- complete Phase 1A–3C test suite;
- AddressSanitizer and UndefinedBehaviorSanitizer;
- deterministic bytecode and snapshot fixtures.

## Deliberate limits and next slice

Phase 3C does not add constructors, instance methods, properties, inheritance, interfaces, exceptions, finalizers, weak references or concurrent GC.

The next object-model slice should introduce direct-dispatch instance methods, implicit `this`, constructors and simple properties before inheritance or virtual dispatch is attempted.
