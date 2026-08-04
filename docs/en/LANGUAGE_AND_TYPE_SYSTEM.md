# Language and Type System

[Documentation Home](README.md) | [Architecture](ARCHITECTURE.md)

RealScript v0.1 provides a compact C#-inspired language designed for embedding in C++17 game engines. This page describes the native Phase 24 implementation. The language is intentionally smaller than C# and prioritizes explicit, verifiable, deterministic semantics. For an exact supported/partial/unsupported inventory, see the [C#-style compatibility matrix](CSHARP_COMPATIBILITY_MATRIX.md).

## Source Files and Modules

Every source file declares a module:

```csharp
module Game.Combat;
```

A file may import another module:

```csharp
module Game.Main;
import Game.Combat;
```

Multiple files may contribute declarations to the same module. Imports resolve module names rather than physical paths.

## Functions

```csharp
int add(int left, int right)
{
    return left + right;
}
```

Supported function behavior includes:

- forward calls;
- direct recursion;
- cross-file and cross-module calls;
- overloads by parameter types;
- stable function identity through canonical signatures;
- `void` functions;
- structured runtime errors and script stack frames.

Return types do not participate in overload identity.

## Primitive Types

The implemented primitive model includes:

- `void`
- `bool`
- exact `byte`, `sbyte`, `short`, `ushort`, `int`, `uint`, `long`, and `ulong`
- IEEE 754 binary32 `float` and binary64 `double`
- exact `char`
- managed `string`
- opaque native `handle`

Numeric widening currently includes:

```text
smaller integers -> wider compatible integers -> float/double
```

Integral arithmetic and narrowing conversions are checked by default;
`checked(...)` and `unchecked(...)` select trapping or wrapping behavior.
Floating-point values preserve binary32/binary64 identity, with deterministic
mode canonicalizing NaN payloads and signed zero for hashing and replay
comparisons.

## Variables and Control Flow

```csharp
int sum(int limit)
{
    int index = 0;
    int result = 0;

    while (index < limit)
    {
        result = result + index;
        index = index + 1;
    }

    if (result > 10)
    {
        return result;
    }
    else
    {
        return 10;
    }
}
```

Implemented statements include blocks, inferred or explicit local declarations, expression statements, assignments, `if`/`else`, `while`, `for`, `foreach`, `do/while`, `break`, `continue`, switch statements, `return`, `throw`, and `try/catch/finally`.

The compiler performs definite-assignment and all-path-return analysis.

`&&` and `||` use short-circuit control flow rather than eager Boolean evaluation.

## Classes

```csharp
class Counter
{
    int value;

    Counter(int initial)
    {
        this.value = initial;
    }

    int Add(int amount)
    {
        value = value + amount;
        return value;
    }
}
```

Classes provide:

- deterministic declaration-order field layouts;
- stable exact `TypeId` identity;
- object allocation;
- instance and static methods;
- implicit `this` for instance members;
- overloaded constructors;
- direct member dispatch;
- null receiver checks;
- exact owner-type validation.

Classes support single inheritance, base constructor calls and `base` member
access, runtime interface values, deterministic virtual/interface dispatch,
`abstract`/`virtual`/`override`/`sealed`, and
`public`/`internal`/`protected`/`private` accessibility.

## Properties

Explicit property accessors:

```csharp
class Player
{
    int health;

    int Health
    {
        get { return health; }
        set { health = value; }
    }
}
```

Class auto-properties use deterministic synthetic backing fields. Accessors receive stable symbols and compile as normal direct calls.

## Arrays

```csharp
int sumFirstTwo()
{
    int[] values = new int[2];
    values[0] = 20;
    values[1] = 22;
    return values[0] + values[1] + values.length;
}
```

Arrays are fixed-length managed objects with:

- exact array and element type identities;
- runtime negative-length rejection;
- null checks;
- bounds checks;
- typed element loads and stores;
- write barriers for managed references;
- identity equality.

## Enums

```csharp
enum Team
{
    Neutral,
    Player = 5,
    Enemy
}
```

Enums have exact `TypeId` identity. Explicit member values and automatic incrementing are supported. Equality requires compatible enum types.

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

Structs provide deterministic field layouts and copy value semantics. They may appear in locals, parameters, returns, object fields, arrays, and nested structs.

Local field updates use copy-on-write storage internally. Managed references nested inside structs participate in precise GC scanning.

Struct instance methods may mutate the original location through implicit or
explicit ref `this`. Ref locals/returns/fields/indexers, nullable value types,
and exact boxing/unboxing are supported by MIR, bytecode, GC, bindings, and AOT.

## Null Values

RealScript distinguishes typed null categories for strings, objects, and arrays. Null conversions are explicit in MIR and bytecode so exact runtime types remain verifiable.

Dereferencing a null object or array produces a structured runtime error.

## Equality

Implemented equality includes:

- primitive value equality;
- managed and host string content equality;
- object identity equality;
- array identity equality;
- exact enum equality;
- structural struct equality;
- compatible null/reference equality.

## Native Handles

`handle` values represent host-owned resources through `NativeHandleRegistry`. A handle contains registry identity, slot, generation, and exact host type identity. Raw C++ pointers are not exposed directly as script values.

Stale, wrong-type, and cross-registry handles are rejected.

## Overload Resolution

The compiler collects candidates by name and applies conversion ranking. The implemented conversion set is intentionally small and deterministic. Ambiguous or invalid calls produce diagnostics instead of runtime dispatch.

## Delegates, Generics, and Collections

Delegates are exact first-class values. Static, instance, virtual, and interface
method groups can create delegates; lambdas use precise heap closures and shared
mutable capture cells. Multicast combination/removal and delegate-backed events
preserve source order.

Generics use deterministic compile-time specialization. Type inference, generic
member methods, `class`/`struct`/`new()` constraints, generic interfaces, and
generic delegates are supported. Growable deterministic collections implement
the native `GetEnumerator`/`MoveNext`/`Current` protocol used by `foreach`.

## Expressions and Patterns

Phase 24 includes `var`, lazy `?:` and `??`, null-conditional `?.`, `is`, `as`,
`typeof`, object/struct/collection initializers, and optional/named/`params`
arguments with source-order evaluation. Constant, null, type, and discard
patterns support variables and `when` guards in switch statements and exhaustive
switch expressions.

## Deterministic Sequences

`sequence` and `yield wait_ticks(...)` compile to explicit single-threaded state
machines. Locals and control-flow position survive yields; nesting,
cancellation, results, snapshots, replay, rollback, hot reload, and C++17 AOT
share the same fixed-tick model.

## Script Exceptions

`throw`, rethrow, typed/catch-all clauses, and `try/catch/finally` use explicit
MIR and bytecode exception regions. A thrown value is a non-null script class
object. `finally` runs for normal completion, script exception propagation,
`return`, `break`, and `continue`. Host/runtime faults remain structured runtime
errors rather than catchable script objects.

## Unsupported Language Features

The Phase 24 implementation deliberately does not provide:

- multiple class inheritance or default interface implementations;
- open runtime generics, variance, reflection, or `dynamic`;
- relational/property/list/recursive pattern families;
- `Task`, threads, or general `async`/`await`;
- unsafe pointers, ref structs, ref properties, or full byref escape analysis;
- exception filters, `using`, native exception interop, or exception unwinding
  serialized across sequence suspension;
- operator overloads, user-defined conversions, LINQ, or the .NET base-class
  library.

Unsupported syntax or semantics receive explicit diagnostics.
