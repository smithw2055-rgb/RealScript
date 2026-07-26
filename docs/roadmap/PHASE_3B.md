# Phase 3B — Language-visible Object Model and Fields

## Status

Implemented as the first source-language object slice on top of the Phase 3A managed heap.

## Goal

Expose a deliberately small reference-type model through the complete compiler and runtime pipeline:

```text
class / new / member access
          │
          ▼
Type symbols and stable TypeId
          │
          ▼
Typed MIR object operations
          │
          ▼
.rsbc 0.2 type descriptors and field operations
          │
          ▼
Managed Record allocation, null checks and precise GC maps
```

Phase 3B focuses on classes with fields and parameterless allocation. Arrays, methods, inheritance and native handles remain separate slices.

## Source language

The implemented grammar supports:

```csharp
class Point
{
    int x;
    int y;
}

int main()
{
    Point value = new Point();
    value.x = 40;
    value.y = 2;
    return value.x + value.y;
}
```

Implemented capabilities:

- module-level `class` declarations;
- ordered instance fields;
- primitive and class-typed fields;
- recursive object graphs;
- `new Type()` with no constructor arguments;
- member reads and writes, including chained receivers;
- `null → class` conversion;
- object identity equality and null equality;
- object types in locals, parameters, returns and overload signatures.

Fields use deterministic declaration-order layout. Constructors, methods, access modifiers, static members and inheritance are not yet accepted.

## Type identity and descriptors

Every class receives a stable 64-bit `TypeId` from its canonical name:

```text
<module>::<type>
```

The compiler emits a descriptor containing:

- TypeId;
- module and source name;
- ordered field names;
- primitive field kind;
- canonical referenced type name for object fields;
- field index.

Function signatures and call references carry exact object TypeIds in addition to the primitive `object` category. This prevents a host or another module from passing an instance of `B` where `A` is required.

## MIR and bytecode

New typed operations:

```text
new.object
check.notnull
load.field
store.field
conv.null.object
```

`check.notnull` also validates the runtime TypeId. Field instructions carry an owner type descriptor index and a field index.

This slice originally advanced the physical bytecode format to `.rsbc` 0.2 and added a `TYPES` section. The current Phase 3E producer emits `.rsbc` 0.4, retaining these object descriptors while adding arrays, handles and exact register TypeIds.

## Runtime semantics

`new Type()` allocates a managed Record whose header stores the descriptor TypeId. Field defaults are:

| Field type | Default |
|---|---|
| `int` | `0` |
| `bool` | `false` |
| `string` | null string |
| class reference | null object |

Dereferencing a null object produces `NullReference`. Invalid generation handles continue to produce `InvalidObjectReference`.

Object equality is reference identity. Two null object values are equal. A null object and a live object are not equal.

## Precise GC layout

The runtime derives the Record reference map from the descriptor:

- `string` and class fields are traced;
- primitive `int` and `bool` fields are not traced;
- writes to reference fields execute the Phase 3A write barrier;
- recursive and cyclic class graphs are supported.

The heap remains non-moving, so existing generation-safe ObjectRefs remain stable throughout collection.

## Tests

Phase 3B adds coverage for:

- class parsing and descriptor emission;
- object MIR and bytecode operations;
- field assignment/read execution;
- deterministic field defaults;
- chained field assignment;
- null-reference traps;
- recursive object graphs and precise reference maps;
- object identity and null equality;
- exact object TypeId argument checks;
- `.rsbc` encode/decode canonical round trips;
- verifier rejection of invalid field descriptors.

## Validation

Validated with:

- GCC, C++17, warnings-as-errors;
- Clang, C++17, warnings-as-errors;
- AddressSanitizer;
- UndefinedBehaviorSanitizer;
- the complete Phase 1A–3B test matrix.

## Explicit limitations

Phase 3B does not implement:

- constructors or constructor arguments;
- methods, virtual dispatch or interfaces;
- access modifiers, static fields or properties;
- inheritance or layout extension;
- arrays in source syntax;
- object initializers;
- weak references or finalizers;
- native-resource ownership wrappers;
- moving or generational collection;
- object metadata compatibility freeze.

## Next slice

Phase 3C should add:

1. source-language arrays and element operations;
2. array type descriptors and bounds checks;
3. native resource handles with explicit ownership policies;
4. heap snapshots and retained-path diagnostics;
5. generated binding metadata for object/reference signatures.
