# Phase 3E — Numeric Values, Enums and Structs

## Status

Implemented after Phase 3D direct member calls.

## Goal

Add the first coherent non-reference value model:

```text
int / long / double
        +
enum identity
        +
struct inline fields and copy semantics
        │
        ▼
exact Typed MIR values
        │
        ▼
.rsbc 0.4 type tags and descriptors
        │
        ▼
interpreter value semantics + precise nested GC scanning
```

## Numeric types

The implemented canonical numeric types are:

| Type | Semantics |
|---|---|
| `int` | checked signed 32-bit integer |
| `long` | checked signed 64-bit integer |
| `double` | IEEE 754 binary64 |

Implicit widening conversions are:

```text
int -> long
int -> double
long -> double
```

Arithmetic and comparisons are lowered to type-specific MIR and bytecode operations. Integer overflow, division by zero and remainder by zero produce structured runtime errors. `double` division follows IEEE 754, including infinity and NaN for zero divisors.

`byte`, `sbyte`, `short`, `ushort`, `uint`, `ulong`, `float`, `char` and decimal/fixed-point types remain unsupported rather than being silently mapped to a type with different range or signedness.

## Enums

```csharp
enum Team
{
    Neutral,
    Player = 5,
    Enemy
}
```

Implemented enum behavior:

- declaration-order members;
- optional signed integer values;
- automatic increment from the previous value;
- duplicate-member diagnostics;
- stable enum TypeId from the canonical type name;
- exact enum identity in locals, parameters, returns, arrays and fields;
- equality only between the same enum type;
- default underlying value zero.

Enums are not implicitly interchangeable with `int`.

## Structs

```csharp
struct Vector2
{
    double x;
    double y;

    Vector2(double x, double y)
    {
        this.x = x;
        this.y = y;
    }

    double LengthSquared()
    {
        return x * x + y * y;
    }
}
```

Implemented behavior:

- module-level struct declarations;
- deterministic declaration-order fields;
- primitive, enum, struct and managed-reference fields;
- exact stable struct TypeId;
- zero-initialized `new StructType()` even when other constructors exist;
- overloaded user constructors;
- read-only instance methods and static methods;
- getter properties;
- direct field mutation on local struct variables;
- copy value semantics;
- struct locals, parameters, returns, fields and arrays;
- structural equality for the same exact struct type.

The interpreter stores an immutable `StructStorage` behind each `StructValue`. Field mutation creates a new storage value and writes it back to the owning local. Copies may share storage until one copy is changed, preserving observable by-value semantics without exposing mutable aliasing.

## Struct mutation boundary

Phase 3E does not yet define `ref`, `out`, addressable places or a mutating receiver ABI. Therefore:

- constructors may initialize `this`;
- a local variable may be changed with `value.field = ...`;
- ordinary struct instance methods are read-only;
- struct property setters are diagnosed;
- mutation through a temporary expression is diagnosed.

This prevents a misleading implementation where a method appears to mutate the caller but only changes a copied receiver. A later `ref this`/place-semantics phase can extend this boundary explicitly.

## Recursive layout safety

A struct is inline and must have finite size. The compiler rejects direct and indirect cycles made only of struct fields, including cross-module cycles. Recursive graphs remain valid through class, array, string or handle references.

## GC integration

Structs are values, not independent heap objects. A struct field may contain managed references, and structs can be nested inside other structs or arrays. Precise root scanning recursively visits every nested `StructValue` field. The embedding host can retain a returned struct with `ManagedHeap::retain(Value)`; validation rejects any nested reference from another heap. Heap snapshots also expose managed edges reached through struct roots and struct-containing objects.

## `.rsbc 0.4`

Version 0.4 adds:

- `long`, `double`, `struct` and `enum` type tags;
- type descriptor kinds for class, struct and enum;
- synthetic-field metadata;
- enum member names and values;
- `f64` instruction immediates;
- numeric conversion and typed arithmetic instructions;
- struct allocation and field load/store operations;
- exact struct/enum TypeIds in signatures, locals, registers and block parameters.

The decoder accepts exactly 0.4 and rejects previous layouts.

## Tests

The Phase 3E target covers:

- checked `long` arithmetic and overflow;
- mixed `int`/`long`/`double` promotion;
- IEEE double division behavior;
- enum explicit/implicit values and identity;
- struct construction, methods and getter properties;
- zero-initialized default struct construction;
- copy-on-write value behavior;
- struct arrays;
- managed references nested in struct roots;
- recursive layout rejection;
- mutating method and setter diagnostics;
- canonical `.rsbc 0.4` round trips;
- verifier rejection of missing exact struct TypeIds.

## Deliberate limits

Phase 3E does not add unsigned integers, binary32 `float`, user-defined numeric conversions, operator overloads, boxing, nullable values, generics, `ref`/`out`, mutable struct receivers, explicit layout or native ABI struct interop.
