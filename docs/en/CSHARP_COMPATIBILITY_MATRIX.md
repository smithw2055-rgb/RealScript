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
| Generics | Supported | Compile-time specialization, inference, generic members, `class`/`struct`/`new()` and multi-interface `where` constraints, generic interfaces/delegates. No open runtime generics or variance. |
| Collections and `foreach` | Supported | Growable deterministic `List`, `Dictionary`, `HashSet`, `Queue`, `Stack` with zero-allocation value-type `struct` enumerators, `this[i]` indexers, `Count`/`Capacity`/`IsEmpty` properties, and $O(1)$ Swap-and-Pop `Remove`. Not the .NET collection library or LINQ. |
| Structs | Supported | Copy semantics, mutable receivers, implicit/ref `this`, nested managed references, structural equality. |
| Exact primitive identities | Supported | Exact signed/unsigned integer widths, binary32 `float`, binary64 `double`, and `char`; checked/unchecked conversions. |
| Nullable value types | Supported | Exact `T?`, lifted null-conditional results, `HasValue`, `Value`, `GetValueOrDefault`. |
| Boxing/unboxing | Supported | Exact script value-type identity with managed-heap/GC support. |
| Reference locations | Partial | Ref parameters, locals, returns, fields, indexers, and ref `this`. No unsafe pointers, ref properties, ref structs, or full escape analysis. |
| Attributes | Partial | Native syntax and versioned metadata for compiler/Game SDK/artifacts, auto-ingested into `ScriptMetadataRegistry` and `InspectorTypeSchema`. No executable attribute classes or full `AttributeUsage`. |
| User operators/conversions | Supported | User-defined `operator` overloads (`+`, `-`, `*`, `/`, `%`, `==`, `!=`, `<`, `<=`, `>`, `>=`, unary `-`, `!`, `~`), compound assignments (`+=`, `-=`, `*=`, `/=`, `%=`), and user-defined `implicit`/`explicit` conversion operators (`public static implicit/explicit operator Target(Source)`). |
| `const`, `readonly`, static members, property initializers | Supported | Compile-time `const`, constructor-assigned `readonly` fields, `static class`, deterministic `static` constructors (`static T()`), `static` fields/initializers, and instance/`static` auto-property initializers (`{ get; set; } = expr;`) are supported across interpreter, bytecode, and C++17 AOT. |

## Expressions and control flow

| Area | Status | RealScript contract |
|---|---|---|
| Structured statements | Supported | `if`, `while`, `for`, `foreach`, `do/while`, `break`, `continue`, switch statements. |
| Inference and convenience | Supported | `var`, `?:`, `??`, `?.`, `$"..."` string interpolation, object/struct/collection initializers. |
| Runtime type operators | Supported | `is`, `as`, `typeof`, exact runtime assignability. |
| Patterns | Supported | Constant, null, type, and discard patterns; variables and `when` guards; exhaustive switch expressions. |
| Pattern families | Partial | No relational, property, positional, list, recursive, `and`/`or`/`not`, or range patterns. |
| Dual-track coroutines: `sequence` (rollback track) | Supported | Purpose-built for deterministic tick simulation, `RSGS` binary state snapshots, and frame-level rollback (`yield wait_ticks`, `wait_seconds`, `wait_event`, `wait_signal`, `wait_until`), lowered to explicit object-field state machines alongside `RealScript.Fiber`. |
| Dual-track coroutines: `async`/`await` (streaming & I/O track) | Supported profile | Phases 25–28 provide `RealScript.Async` (`Awaitable`/`Awaitable<T>`, 8-byte generational `AwaitableHandle`, `CancellationToken`/`CancellationTokenSource`, and 4 main-thread frame queues: `PreTickIo`, `FixedTick`, `Update`, `EndOfFrame`) for async `OnStart()`, prefab/asset streaming, save-game I/O, and HTTP/WebSocket bridges with automatic entity-destroy cancellation. Raw OS threads and CLR `Task` remain excluded. |

## Errors and resource cleanup

| Area | Status | RealScript contract |
|---|---|---|
| `throw`, `try`, `catch`, `finally` | Supported | Non-null script class objects, ordered typed/catch-all matching, rethrow, cross-call propagation. |
| Cleanup exits | Supported | `finally` runs for normal completion, script exceptions, `return`, `break`, and `continue`. |
| `using` resource disposal (`IDisposable` / `ResourceHandle<T>`) | Supported | `using (...)` blocks and `using var` declarations lower to deterministic LIFO `try`/`finally` `Dispose()` across `class`, `interface`, and `struct` with host ref-counted `ResourceRegistry` bridging (`RealScript.Resource`). |
| Runtime faults | Structured host errors | Overflow, invalid bytecode, quota, null/bounds, and host failures are not catchable script exception objects. |
| Filters and resource statements | Partial | `using` is supported; catch filters, `lock`, and native/platform exception interop are not supported. |
| Exceptions across sequence yields | Not supported | Sequence state is snapshot-safe; pending exception unwinding is not serialized across a suspension. |

## Runtime, backends, and tooling

| Area | Status | RealScript contract |
|---|---|---|
| Interpreter | Supported | Verified typed-register bytecode with unified flat-frame layout, computed-goto dispatch, incremental GC stepping, budgets, deterministic tracing, and structured errors. |
| C++17 AOT | Supported | Generated native code executes the same MIR semantics, including delegates, value types, patterns, and exceptions, plus Zero-Budget Trusted Release (`--trusted-release`), `#line` source mapping directives, struct stack scalarization, and vectorized ECS fast-loops. |
| Toolchain JIT | Platform-dependent | External C++ toolchain JIT where configured; it is not an in-process CLR-style machine-code JIT. |
| `.rsbc` artifacts | Draft, versioned | Current output is 0.9; decoder supports 0.6–0.9. Format is not frozen. |
| LSP/DAP/hot reload | Supported profile | Exclusive `.rscript` / `.d.rscript` file extensions; original-source symbols/sequence points, incremental `ParseSnapshot` and subgraph metadata caching, member `.` completion, `signatureHelp`, `.d.rscript` host stubs, stdio & embedded TCP (`--dap-port` / `EmbeddedDapServer`) DAP debugging, hierarchical Watch `variablesReference` object/struct/collection expansion, C++17 AOT `.natvis` & LLDB formatters, zero-dependency single-file C++ declaration parser in `rsbind` (`-D`/`-U` macro evaluation), and stateful scene hot reload. |
| Reflection and dynamic | Not supported | Stable compile-time/runtime TypeIds exist, but no CLR reflection, `dynamic`, runtime code emission, or runtime generic construction. |
| Standard library | Purpose-built | Deterministic game/runtime primitives and collections, not the .NET BCL. |

## Positioning

The accurate Phase 24 description is:

> A native, strongly typed, deterministic C#-style game language with runtime
> polymorphism, closures, compile-time generics, coroutine state machines,
> complete implemented value/reference semantics, common patterns/convenience,
> and structured script exceptions — not a CLR-compatible implementation of
> full C# or the .NET ecosystem.
