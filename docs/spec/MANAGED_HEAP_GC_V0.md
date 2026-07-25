# Managed Heap and GC — Draft v0.1

## Scope

Phase 3A introduces the first precise managed-memory boundary. It adds runtime object references and collection infrastructure without yet exposing class, array or object-allocation syntax in the source language.

## Object references

`ObjectRef` is a stable handle containing:

- slot index;
- generation;
- object kind.

The heap may reuse a reclaimed slot, but increments its generation. A stale reference therefore never aliases a later object.

## Object kinds

The initial heap supports:

- managed UTF-8 string objects;
- arrays of runtime `Value` elements;
- record objects with indexed `Value` fields.

Arrays and records may contain further `ObjectRef` values and therefore form arbitrary graphs and cycles.

## Exact roots

Roots are registered through scoped root descriptors. Current interpreter roots are:

- function arguments;
- typed registers;
- local slots.

The collector scans only `Value` locations registered as roots and object fields known to the heap. It does not conservatively scan native stacks or arbitrary memory.

## Collector

The v0.1 collector is non-moving tri-color mark/sweep:

1. color live slots white;
2. seed gray work from exact roots;
3. trace arrays and record fields;
4. sweep remaining white objects;
5. increment generations for reclaimed slots.

`step(workBudget)` limits the number of mark/sweep units processed per call. Full `collect()` repeatedly steps until idle.

## Mutation safety

Object field and array writes use a black-to-white write barrier during marking. Exact roots are rescanned on incremental steps; if root mutation discovers a white object during sweep, collection returns to marking before continuing.

Objects allocated during an active collection are treated as marked for that cycle.

## Statistics

The heap reports allocations, collections, incremental steps, reclaimed objects and bytes, live objects and bytes, peak bytes and work performed by the last step.

## Deliberate limits

This draft does not include compaction, generations, weak references, finalizers, ephemerons, concurrent collection, object pinning, GC maps in `.rsbc`, source-level object allocation or automatic collection scheduling from allocation thresholds.
