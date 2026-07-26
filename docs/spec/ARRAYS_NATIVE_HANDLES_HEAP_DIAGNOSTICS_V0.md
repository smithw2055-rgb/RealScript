# Arrays, Native Handles and Heap Diagnostics — Implemented Draft v0.1

- implementation slice: Phase 3C
- bytecode dependency: `.rsbc` 0.3
- status: implemented draft; source, ABI and binary compatibility are not frozen

## 1. Scope

This document records the implemented Phase 3C contracts for fixed-length managed arrays, opaque host-resource handles, managed-heap ownership and diagnostic inspection.

These contracts are implementation boundaries rather than a promise of stable external ABI compatibility.

## 2. Array identity

Every source array type has a canonical name:

```text
<canonical element type>[]
```

A stable 64-bit TypeId is derived from that name. Array values carry the exact array TypeId at function, local, field, MIR, bytecode and runtime boundaries.

The first implementation accepts one-dimensional array syntax. The element category may be `bool`, `int`, `string`, a class reference or `handle`. The runtime representation also reserves exact element TypeId metadata for reference-bearing arrays.

## 3. Array allocation and defaults

`new T[length]` evaluates the length once. A negative length fails with `IndexOutOfRange`; an allocation that exceeds configured heap limits fails with `OutOfMemory`.

Elements are initialized as follows:

| Element category | Default |
|---|---|
| `bool` | `false` |
| `int` | `0` |
| `string` | null string |
| class | null object of the declared class type |
| array | null array of the declared array type |
| `handle` | invalid handle |

Array length is immutable after allocation.

## 4. Array access

Element access evaluates receiver, index and assigned value in source order. The interpreter performs:

1. null receiver validation;
2. managed-reference ownership and generation validation;
3. index non-negativity and bounds validation;
4. element category and exact TypeId validation;
5. a write barrier before committing a managed-reference store.

Failed stores do not mutate the array.

Array equality is identity equality. Two null arrays compare equal. Arrays do not use structural element comparison.

## 5. Native handle model

A native handle contains:

- a registry slot;
- a non-zero generation;
- a non-zero registry ownership identity;
- a non-zero host-defined TypeId.

The registry owns a `shared_ptr<void>` resource. Script-visible values never expose the native pointer.

Resolution succeeds only if:

- the handle belongs to that registry;
- the slot is live;
- the generation matches;
- the stored TypeId matches the handle;
- an optional expected TypeId matches.

Release invalidates the generation before the slot can be reused. A stale or invalid handle produces `InvalidNativeHandle`; an expected-type mismatch produces `TypeMismatch`.

The language-level `handle` value is opaque. It supports storage, argument/return transport and identity equality only. Resource operations are host bindings that must resolve and validate the handle through the registry.

## 6. Managed-heap identity

Every `ManagedHeap` has a unique non-zero `heapId`. Every `ObjectRef` contains that identity. Heap APIs reject a reference whose heap identity does not match before inspecting its slot.

This prevents accidental acceptance of a foreign object when two heaps use the same slot and generation numbers.

## 7. Rooting

Precise roots have two sources:

- interpreter `ShadowStack` frames containing argument, local and register vectors;
- persistent roots registered by the embedding host.

`PersistentRoot` is move-only and unregisters its token on destruction or `reset()`. Updating a root requires a live object from the same heap.

A host must retain every managed object that outlives the native call or interpreter invocation that produced it.

## 8. Incremental collection and full collection

Incremental collection uses mark/sweep work units. Objects allocated during an active collection are marked and scanned so they cannot be reclaimed before becoming visible to the mutator.

A write from a marked owner to an unmarked managed object marks the target. If work is discovered during sweep, the collector returns to mark processing.

`collectFull()` has stronger diagnostic semantics than a single incremental cycle: it drains any active cycle and then performs a fresh complete cycle from the current root set. After it returns, all objects unreachable at the call boundary have been reclaimed.

## 9. Snapshots and retaining paths

A `HeapSnapshot` contains:

- heap identity and aggregate statistics;
- every live object's reference, kind, TypeId, element metadata, size and value count;
- labeled managed-reference edges;
- persistent roots and optional shadow-stack roots.

Object and root ordering is deterministic. `toText()` is suitable for fixtures and diagnostics, not as a stable interchange format.

`retainingPath(target)` performs a breadth-first traversal from the current roots and returns one shortest managed-reference path. An empty result means the target is invalid or not retained by the supplied roots.

## 10. Shutdown diagnostics

`leakSummary()` reports live object/byte totals, persistent-root totals and per-kind object counts. It is intended for tests and engine shutdown checks. A non-empty heap is not automatically an error because the embedding host may intentionally keep persistent roots alive.

## 11. Compatibility

`.rsbc` 0.3 is not binary compatible with 0.2. A 0.3 decoder must reject any different version. Source and runtime interfaces remain draft and may change before the first stable release.
