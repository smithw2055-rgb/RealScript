# Phase 3A — Managed Heap and Incremental GC

## Status

Implemented as the first managed-memory slice after the primitive interpreter and embedding boundary.

## Added

- stable generation-checked `ObjectRef` handles;
- managed String, Array and Record objects;
- object graph tracing including cycles;
- exact scoped roots;
- interpreter arguments, registers and locals registered as roots;
- non-moving tri-color mark/sweep;
- bounded incremental `step()` processing;
- black-to-white write barrier;
- root mutation handling during incremental sweep;
- stale-reference protection and slot reuse;
- heap allocation, live, peak and reclaim statistics;
- `object` bytecode type tag for host-produced managed references.

## Integration boundary

Source syntax still exposes only the primitive Phase 2 profile. Managed references currently enter bytecode execution through verified object-typed function references and host bindings. The interpreter can pass them through registers, locals, calls and returns while the heap retains exact roots.

## Tests

Phase 3A tests cover:

- managed strings and payload access;
- stale generations after slot reuse;
- scoped roots;
- transitive array marking;
- cyclic graph reclamation;
- incremental budgets;
- write barriers;
- root mutation during collection;
- interpreter register/local roots during host-triggered GC;
- pressure allocation and statistics.

## Next slice

Phase 3B should add source-level arrays and object records, allocation/load/store bytecode instructions, automatic allocation-triggered GC scheduling, serialized GC maps and host handles.
