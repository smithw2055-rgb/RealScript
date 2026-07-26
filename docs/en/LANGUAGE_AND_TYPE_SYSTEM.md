# Language and Type System

[Documentation Home](README.md) | [Architecture](ARCHITECTURE.md)

RealScript v0.1 provides a compact C#-inspired language designed for embedding in C++17 game engines. The current language is intentionally smaller than C# and prioritizes explicit, verifiable semantics.

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
- checked 32-bit `int`
- checked 64-bit `long`
- IEEE 754 binary64 `double`
- managed `string`
- opaque native `handle`

Numeric widening currently includes:

```text
int -> long -> double
```

Integer arithmetic traps on overflow. Floating-point operations follow binary64 semantics, with deterministic mode canonicalizing NaN payloads and signed zero for hashing and replay comparisons.

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

Implemented statements include blocks, local declarations, expression statements, assignments, `if`/`else`, `while`, and `return`.

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

Inheritance, interfaces, virtual dispatch, and access modifiers are not implemented.

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

The current ordinary struct instance receiver is read-only. Mutating instance methods, `ref this`, boxing, and nullable value types are future work.

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

## Unsupported Language Features

The v0.1 baseline does not implement:

- inheritance and interfaces;
- virtual or abstract dispatch;
- generics;
- exceptions;
- coroutines or `async`;
- `ref` and `out`;
- `break`, `continue`, `for`, `foreach`, and `switch`;
- unsigned integer types, `float`, `char`, or user-defined conversions;
- mutable struct instance receivers and boxing.

Unsupported syntax or semantics receive explicit diagnostics.
