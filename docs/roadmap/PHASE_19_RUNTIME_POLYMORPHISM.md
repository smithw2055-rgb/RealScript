# Phase 19 — Runtime Polymorphism and Object Model

Phase 19 builds on the fully native Phase 18 compiler pipeline. It introduces deterministic runtime polymorphism without adopting CLR reflection or multiple class inheritance.

Tracking issue: #34. Development branch: `agent/phase19-runtime-polymorphism`.

## Scope

### 19A — visibility and inheritance model

- `public`, `internal`, `protected`, and `private` declaration modifiers;
- one optional base class followed by zero or more interfaces;
- deterministic inheritance-cycle rejection;
- base-first inherited field layout with stable field identities;
- inherited member lookup and access checks;
- abstract or sealed class markers.

### 19B — constructors and `base`

- explicit base-constructor invocation;
- implicit parameterless base-constructor invocation when legal;
- `base.member` and `base.Method(...)` binding;
- constructor-order and definitely-assigned validation;
- abstract classes cannot be instantiated.

### 19C — virtual dispatch

- `virtual`, `override`, `abstract`, and `sealed override` methods;
- deterministic virtual slot allocation based on canonical source signatures;
- exact override signature and accessibility validation;
- runtime dispatch by object TypeId and virtual slot;
- verifier checks for slot ownership, target signatures, and abstract entries;
- direct non-virtual and `base` calls remain statically bound.

### 19D — interface values and dispatch

- interface types in fields, locals, parameters, return values, arrays, and null comparisons;
- implicit conversion from implementing class references to interface values;
- deterministic interface method slots and implementation maps;
- runtime interface call dispatch without reflection;
- assignability checks used by conversion, invocation, save-state, and native bindings.

Struct interface contracts remain compile-time-only until Phase 23 boxing support.

### 19E — artifact and runtime closure

- MIR and bytecode dispatch metadata for virtual and interface calls;
- `.rsbc` object-model descriptors and versioned codec validation;
- AOT/JIT execution parity;
- inherited GC descriptors and save-state compatibility;
- hot reload rejects hierarchy, layout, slot, or interface-map changes;
- Game SDK metadata exposes public object-model contracts;
- debugger and DAP show user methods rather than dispatch thunks;
- LSP completion, definition, rename, and diagnostics respect visibility and inheritance.

## Deterministic ABI rules

- Class inheritance is single and acyclic.
- A derived class starts with the complete base-field layout; newly declared fields append in source order.
- Existing virtual slots are inherited unchanged. New virtual methods append in canonical signature order after source declaration validation.
- Overrides reuse the inherited slot and cannot change parameter types, return type, static/instance status, or visibility incompatibly.
- Interface slots are assigned from canonical interface method signatures and remain independent from class virtual slots.
- Dispatch tables and interface maps are serialized into `.rsbc` and included in AOT content hashes and hot-reload fingerprints.
- Compiler-generated thunks are synthetic and never appear in user-facing symbols or debugger frames.

## Explicit non-goals

- multiple class inheritance;
- default interface implementations;
- interface static abstract members;
- generic covariance or contravariance;
- `dynamic` or reflection-based dispatch;
- runtime type emission;
- Phase 20 first-class delegate/closure semantics.

## Merge criteria

- parser recovery and modifier-conflict diagnostics;
- Binder/type/flow tests for visibility, inheritance, overrides, base access, and interface conversions;
- MIR and bytecode verifier coverage;
- interpreter, AOT, and JIT result/digest parity;
- `.rsbc` codec corruption tests;
- GC and save-state tests with inherited layouts;
- hot-reload compatibility tests;
- LSP/DAP/debugger and Game SDK tests;
- Ubuntu and Windows warnings-as-errors builds with all CTest targets passing.
