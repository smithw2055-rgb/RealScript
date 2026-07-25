# RealScript Managed Heap and GC — Implemented Draft v0.1

- implementation slice: Phase 3A
- collector: non-moving incremental Mark/Sweep
- reference representation: slot + generation + object kind
- status: implemented draft; object ABI is not frozen

## 1. Safety boundary

A managed reference MUST NOT expose a C++ heap address. A reference is valid only if:

- its slot exists;
- the slot contains a live object;
- the generation matches;
- the object kind matches.

A freed slot MUST advance its generation before reuse. Invalid references MUST be rejected rather than dereferenced.

## 2. Implemented object kinds

| Kind | Payload |
|---|---|
| `String` | UTF-8 bytes |
| `Array` | fixed-length `Value[]` |
| `Record` | fixed field `Value[]` |

The internal object header records kind, mark state and accounted size. Header layout is an implementation detail and MUST NOT be serialized into `.rsbc` or exposed through the embedding API.

## 3. Runtime values

`Value` can carry an `ObjectRef`. In the primitive language profile, only `ObjectRef(String)` has a source-language primitive type (`string`). Array and Record references remain host/runtime values until later language slices define their static types.

Script string literals MUST allocate managed String objects. Inline host strings MAY remain supported at the embedding boundary. Equality between inline and managed strings MUST compare text content.

## 4. Precise roots

The interpreter MUST register exact root ranges for every active frame:

- arguments;
- locals;
- registers.

The collector MUST inspect only `Value` entries that contain an `ObjectRef`; it MUST NOT scan arbitrary native stack memory.

Persistent host references MUST use explicit root tokens or a later equivalent handle API.

## 5. Collection phases

The implemented phases are:

```text
Idle → Mark → Sweep → Idle
```

Starting a collection clears mark state, records current roots and enters Mark. Mark scans Array and Record payload values transitively. Sweep reclaims every unmarked slot and advances its generation.

## 6. Incremental work

`step(roots, budget)` MUST perform no more than `budget` object scans or slot sweeps. A zero budget MUST perform no work.

Scheduling is based on deterministic work units, not elapsed wall time. Interpreter safepoints call `step` using `Limits::gcWorkBudget`.

## 7. Mutation during collection

Roots MUST be rescanned at incremental safepoints.

Writing an `ObjectRef` into a marked Array or Record MUST execute a write barrier. If marking had already transitioned to Sweep, discovering a new white target MUST reopen Mark before sweeping continues.

Objects allocated during an active collection MUST be retained for that cycle and their reference payloads MUST be scanned.

## 8. Statistics

The runtime exposes cumulative counters for:

- allocations and allocated bytes;
- collections started/completed;
- reclaimed objects/bytes;
- mark and sweep work;
- current live objects/bytes;
- peak live bytes.

Statistics are observational and MUST NOT affect reachability or script results.

## 9. Non-goals

This draft does not define moving references, generational barriers, weak references, finalizers, concurrent collection, native AOT stack maps or source-language object instructions.
