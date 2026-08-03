# Phase 19 — Runtime Polymorphism and Object Model

Phase 19 builds on the fully native Phase 18 compiler pipeline. It introduces deterministic runtime polymorphism without adopting CLR reflection or multiple class inheritance.

Tracking issue: #34. Development branch: `agent/phase19-runtime-polymorphism`.

## Implementation status

- **19A frontend and inheritance model:** implemented.
- **19B base access and inherited runtime layout:** implemented for base identity, base-first fields, inherited member lookup, derived-to-base assignability, and statically bound `base` method calls.
- **19C virtual dispatch:** implemented for `virtual`, `override`, `abstract`, and `sealed override`; stable slots execute through the interpreter, generated C++ AOT, and Toolchain JIT.
- **19D interface values and dispatch:** implemented for interface-typed storage, class-to-interface conversion, deterministic interface slots, inherited implementation maps, interpreter dispatch, generated C++ AOT, and Toolchain JIT.
- **19E artifact and runtime closure:** implemented for base-constructor execution, full visibility enforcement, `.rsbc` 0.7 object-model persistence, inherited save-state/Game SDK metadata, compiled AOT parity, and LSP/DAP/debugger surfaces.
- **Phase status:** complete. The VS 18 2026 Release warnings-as-errors build and the current 37-target CTest matrix pass.

Direct non-virtual and `base` calls remain statically bound. Interface values reuse managed object references and preserve their exact interface TypeId in semantic, MIR, and bytecode signatures. Virtual and interface dispatch metadata is present in the interpreter, AOT, JIT, verifiers, printers, disassembler, content hashes, hot-reload compatibility checks, and `.rsbc` 0.7 artifacts. `.rsbc` 0.6 remains readable as a legacy format.

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

Implemented:

- interface descriptors participate in exact named-type identity for fields, locals, parameters, return values, arrays, and `null`;
- implementing class references convert implicitly to interface values without wrapper allocation;
- interface slots are assigned by canonical method signature, independently of source order and class virtual slots;
- every class descriptor carries deterministic `(Interface TypeId, slot -> function SymbolId)` implementation maps, including inherited implementations;
- interpreter, generated C++ AOT, and Toolchain JIT dispatch by the receiver's runtime class descriptor;
- semantic, MIR, bytecode, and runtime assignability checks accept class-to-interface conversions only when a matching implementation map exists;
- hot reload and AOT content hashes include interface descriptors, slots, maps, and call-site dispatch metadata.

Struct interface contracts remain compile-time-only until Phase 23 boxing support. Class interface maps and inherited object metadata are persisted in `.rsbc` 0.7 and consumed directly by the Game SDK.

### 19E — artifact and runtime closure

Implemented and validated:

- MIR and bytecode dispatch metadata for virtual and interface calls;
- `.rsbc` 0.7 object-model descriptors, canonical round trips, legacy 0.6 reads, and corruption validation;
- interpreter and compiled C++ AOT result parity on Windows, with Toolchain JIT coverage on supported non-MSVC hosts;
- base constructors execute before derived constructor bodies, including implicit parameterless base calls;
- inherited GC descriptors, object snapshots, and save-state-compatible field layouts;
- hot reload rejects hierarchy, layout, slot, or interface-map changes;
- Game SDK metadata exposes inherited fields, constructors, methods, properties, and accessibility;
- debugger and DAP retain original user functions and source sequence points without dispatch thunks;
- LSP completion, definition, rename, and compiler diagnostics respect visibility and inheritance.

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
