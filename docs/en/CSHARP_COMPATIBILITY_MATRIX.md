# C#-Style Feature Compatibility Matrix

[Documentation Home](README.md) | [Language and Type System](LANGUAGE_AND_TYPE_SYSTEM.md) | [简体中文](../zh-CN/CSHARP_COMPATIBILITY_MATRIX.md)

RealScript uses C#-inspired syntax but is not the CLR, ECMA C#, or the .NET
library. “Supported” below means the feature is understood by the native
compiler pipeline and all applicable RealScript backends. “Partial” describes a
deliberately smaller deterministic profile.

## Language and type system

| Area | Status | RealScript contract |
|---|---|---|
| Modules, imports, multiple files | Supported | Explicit module/import model; no C# assembly or namespace resolution. |
| Functions and overloads | Supported | Forward calls, recursion, deterministic conversion ranking, optional/named/`params` arguments. |
| Class inheritance | Supported | Single inheritance, base constructors/access, `abstract`, `virtual`, `override`, `sealed`. |
| Interfaces | Supported | Interface values, inheritance contracts, deterministic runtime interface dispatch. No default interface bodies. |
| Accessibility | Supported | `public`, `internal`, `protected`, and `private` on supported declarations. |
| Delegates and closures | Supported | First-class exact delegate values, method groups, heap closures, shared mutable captures, multicast, events. |
| Generics | Supported | Compile-time specialization, inference, generic members, class/struct/`new()` constraints, generic interfaces/delegates. No open runtime generics or variance. |
| Collections and `foreach` | Supported | Growable deterministic `List`, `Dictionary`, `HashSet`, `Queue`, `Stack`; native enumerator protocol. Not the .NET collection library or LINQ. |
| Structs | Supported | Copy semantics, mutable receivers, implicit/ref `this`, nested managed references, structural equality. |
| Exact primitive identities | Supported | Exact signed/unsigned integer widths, binary32 `float`, binary64 `double`, and `char`; checked/unchecked conversions. |
| Nullable value types | Supported | Exact `T?`, lifted null-conditional results, `HasValue`, `Value`, `GetValueOrDefault`. |
| Boxing/unboxing | Supported | Exact script value-type identity with managed-heap/GC support. |
| Reference locations | Partial | Ref parameters, locals, returns, fields, indexers, and ref `this`. No unsafe pointers, ref properties, ref structs, or full escape analysis. |
| Attributes | Partial | Native syntax and versioned metadata for compiler/Game SDK/artifacts. No executable attribute classes or full `AttributeUsage`. |
| User operators/conversions | Not supported | Built-in deterministic operators and conversions only. |
| `const`, `readonly`, static fields | Not supported | Static methods/properties are supported; these field forms are not in the Phase 24 surface. |

## Expressions and control flow

| Area | Status | RealScript contract |
|---|---|---|
| Structured statements | Supported | `if`, `while`, `for`, `foreach`, `do/while`, `break`, `continue`, switch statements. |
| Inference and convenience | Supported | `var`, `?:`, `??`, `?.`, object/struct/collection initializers. |
| Runtime type operators | Supported | `is`, `as`, `typeof`, exact runtime assignability. |
| Patterns | Supported | Constant, null, type, and discard patterns; variables and `when` guards; exhaustive switch expressions. |
| Pattern families | Partial | No relational, property, positional, list, recursive, `and`/`or`/`not`, or range patterns. |
| Coroutines | Supported profile | Deterministic single-threaded `sequence`, `yield wait_ticks`, persisted state, nesting, cancellation, results, snapshots/rollback. |
| `async`/`await`, `Task`, threads | Not supported | Deliberately outside the deterministic gameplay execution model. |

## Errors and resource cleanup

| Area | Status | RealScript contract |
|---|---|---|
| `throw`, `try`, `catch`, `finally` | Supported | Non-null script class objects, ordered typed/catch-all matching, rethrow, cross-call propagation. |
| Cleanup exits | Supported | `finally` runs for normal completion, script exceptions, `return`, `break`, and `continue`. |
| Runtime faults | Structured host errors | Overflow, invalid bytecode, quota, null/bounds, and host failures are not catchable script exception objects. |
| Filters and resource statements | Not supported | No catch filters, `using`, `lock`, or native/platform exception interop. |
| Exceptions across sequence yields | Not supported | Sequence state is snapshot-safe; pending exception unwinding is not serialized across a suspension. |

## Runtime, backends, and tooling

| Area | Status | RealScript contract |
|---|---|---|
| Interpreter | Supported | Verified typed-register bytecode with budgets, GC, deterministic tracing, and structured errors. |
| C++17 AOT | Supported | Generated native code executes the same MIR semantics, including delegates, value types, patterns, and exceptions. |
| Toolchain JIT | Platform-dependent | External C++ toolchain JIT where configured; it is not an in-process CLR-style machine-code JIT. |
| `.rsbc` artifacts | Draft, versioned | Current output is 0.9; decoder supports 0.6–0.9. Format is not frozen. |
| LSP/DAP/hot reload | Supported profile | Original-source symbols/sequence points, completion/rename, body-only hot reload with layout/metadata compatibility checks. |
| Reflection and dynamic | Not supported | Stable compile-time/runtime TypeIds exist, but no CLR reflection, `dynamic`, runtime code emission, or runtime generic construction. |
| Standard library | Purpose-built | Deterministic game/runtime primitives and collections, not the .NET BCL. |

## Positioning

The accurate Phase 24 description is:

> A native, strongly typed, deterministic C#-style game language with runtime
> polymorphism, closures, compile-time generics, coroutine state machines,
> complete implemented value/reference semantics, common patterns/convenience,
> and structured script exceptions — not a CLR-compatible implementation of
> full C# or the .NET ecosystem.
