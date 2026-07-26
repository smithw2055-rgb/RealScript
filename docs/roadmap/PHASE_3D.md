# Phase 3D — Methods, Constructors and Properties

## Status

Implemented on top of the Phase 3C array, native-handle and heap-diagnostics runtime.

## Goal

Add direct-dispatch class behavior without introducing inheritance or a second call ABI:

```text
class members
    │
    ▼
stable member SymbolIds + implicit this
    │
    ▼
ordinary Typed MIR call
    │
    ▼
.rsbc 0.4 function references
    │
    ▼
existing interpreter call frames
```

A member function is represented as an ordinary typed function whose first parameter is the exact owner type. This keeps free functions, static methods, instance methods, constructors and property accessors on one verified call path.

## Source language

### Instance methods

```csharp
class Counter
{
    int value;

    int Add(int amount)
    {
        value = value + amount;
        return value;
    }
}
```

Implemented behavior:

- implicit `this` in instance methods;
- explicit `this.member` access;
- unqualified instance field/property reads and field assignments;
- overload resolution using the existing deterministic conversion ranking;
- exact owner TypeId in the hidden receiver parameter;
- null receiver checks before every class instance call;
- direct dispatch only.

### Static methods

```csharp
class Math
{
    static int Twice(int value)
    {
        return value * 2;
    }
}
```

Static methods have no hidden receiver and are called with `Type.Member(...)`. `this` is diagnosed in a static context.

### Constructors

```csharp
class Counter
{
    int value;

    Counter(int initial)
    {
        this.value = initial;
    }
}
```

Class construction performs allocation first, then invokes the selected `.ctor` with the new object as parameter zero. Constructor overloads use the same ranking rules as functions and methods. Classes with no declared constructors retain an implicit parameterless allocation path.

### Properties

Explicit properties lower to accessor methods:

```csharp
int Value
{
    get { return value; }
    set { this.value = value; }
}
```

Auto-properties receive a deterministic synthetic backing field:

```csharp
int Value { get; set; }
```

Implemented property forms:

- instance getter and setter;
- static explicit getter and setter;
- getter-only and setter-only properties;
- class auto-properties;
- read/write diagnostics when an accessor is absent.

Synthetic fields participate in descriptor layout, GC maps and bytecode verification but are marked as synthetic in metadata.

## Symbols and ABI

Member identities use the canonical form:

```text
<module>::<owner>.<member>(<visible parameters>)
```

The hidden receiver is omitted from the readable canonical signature but remains present in MIR, bytecode and runtime parameter tables. Constructor identities use `.ctor` as the member name. Property accessors use `get_<name>` and `set_<name>`.

No virtual slot, vtable or interface dispatch metadata is introduced in this phase.

## MIR and bytecode

Phase 3D intentionally reuses `call.direct` / `call` rather than adding method-specific opcodes. The resolved function reference records:

- stable member SymbolId;
- owner-qualified name;
- exact return TypeId when applicable;
- exact hidden receiver TypeId;
- visible parameter categories and TypeIds.

A class receiver is lowered through `check.notnull` before the call. This guarantees a null instance call fails even when the method body does not otherwise access `this`.

## Cross-module behavior

Imported type descriptors include methods, constructors and properties. A dependent module can construct an imported class, invoke members and use property accessors while retaining exact owner identity in its function-reference table.

Member signatures are included in module public fingerprints. Changing a public method, constructor or property invalidates direct dependent modules.

## Tests

The Phase 3D target covers:

- instance and static methods;
- implicit and explicit `this` access;
- method overload ranking;
- constructors and constructor overload selection;
- explicit and auto-properties;
- static properties;
- cross-module construction and member calls;
- null receiver traps;
- canonical `.rsbc 0.4` round trips;
- exact hidden receiver TypeIds.

## Deliberate limits

Phase 3D does not add:

- inheritance or base calls;
- virtual/abstract methods;
- interfaces or dynamic dispatch;
- access modifiers;
- static fields;
- operator overloads;
- delegates, closures or extension methods;
- constructor chaining or field initializers.

These require separate object-layout and dispatch decisions and are not simulated with placeholder behavior.
