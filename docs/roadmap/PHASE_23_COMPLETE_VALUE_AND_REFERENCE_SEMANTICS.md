# Phase 23 — Complete Value and Reference Semantics

Phase 23 replaces the former carrier aliases and restricted reference-call
profile with exact runtime identities, mutable value receivers, general
reference locations, nullable values, and boxing.

## Work breakdown

- **23A exact numeric and character identities:** complete.
- **23B checked/unchecked arithmetic and conversions:** complete.
- **23C mutable structs and reference locations:** complete.
- **23D nullable values and boxing/unboxing:** complete.
- **23E artifacts, bindings, GC, and backend validation:** complete.

## Delivered semantics

- `byte`, `sbyte`, `short`, `ushort`, `uint`, `ulong`, `float`, and `char`
  retain distinct semantic, MIR, bytecode, runtime-value, native-binding, and
  AOT ABI identities.
- Integral arithmetic and narrowing conversions are checked by default;
  `checked(...)` and `unchecked(...)` explicitly select trapping or wrapping
  behavior across the interpreter and generated C++17 backend.
- Mutable struct instance methods update the original location through implicit
  `ref this`. Ref locals and ref returns may alias locals, fields, and indexers;
  `in` receivers use defensive-copy rules where mutation would otherwise leak.
- `T?` is an exact nullable value with `HasValue`, `Value`, and
  `GetValueOrDefault`. Boxing stores the exact value type in a managed object;
  unboxing validates that identity.
- Struct, nullable, and boxed managed references participate in precise GC,
  snapshots, bytecode verification, native bindings, and generated AOT calls.

## Validation

- `realscript.phase23.value-semantics` covers exact identities, overflow modes,
  conversions, mutable receivers, reference aliases, nullable values, boxing,
  snapshots, and native Game SDK marshaling.
- `realscript.phase23.aot` executes the same exact-width, checked/unchecked,
  mutable-struct, nullable, boxing, and defensive-copy cases in the interpreter
  and compiled C++17 output.

## Deliberate limits

Reference safety is enforced for the implemented local/field/indexer locations,
but RealScript does not claim C# `Span<T>`/ref-struct escape analysis, unsafe
pointers, ref properties, or the full CLR byref ABI.
