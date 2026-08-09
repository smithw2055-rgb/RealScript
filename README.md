# RealScript

**English** | [简体中文](README.zh-CN.md)

RealScript is an embedded, strongly typed scripting language and runtime for modern C++17 game engines. It uses a C#-inspired syntax and provides a verified bytecode interpreter, deterministic C++17 AOT generation, an optional external-toolchain JIT, source-level debugging, language tooling, hot reload, typed game bindings, and fixed-tick gameplay services.

> **Project status:** Phase 1–24 is implemented. The native compiler/runtime now covers runtime polymorphism, first-class delegates and closures, inferred compile-time generics and growable collections, deterministic coroutine state machines, exact value/reference semantics, common C#-style patterns and convenience syntax, and structured script exceptions. Language, bytecode, metadata, SDK, gameplay-state, and ABI contracts remain draft.

## Highlights

- Dependency-free C++17 and CMake foundation.
- Lexer, parser, Binder, flow analysis, diagnostics, multi-file modules, imports, and incremental snapshots.
- Verified multi-block Typed MIR with O0/O1/O2 optimization.
- Typed register bytecode, deterministic `.rsbc` 0.9 encoding with 0.6–0.8 legacy decode support, strict verification, and structured runtime errors.
- First-class exact delegate values, static/instance/virtual/interface method references, shared mutable heap closures, multicast/event storage, and deterministic heap rollback.
- Classes, constructors, methods, properties, arrays, enums, structs, strings, native handles, and exact type identities.
- Precise roots, generation-checked references, incremental mark/sweep GC, write barriers, heap snapshots, and leak diagnostics.
- DAP debugger, LSP language server, source metadata, and body-only hot reload.
- Deterministic C++17 AOT, native module ABI, record/replay, profiling, benchmarking, and optional toolchain JIT.
- Typed C++ Game SDK, rooted script objects, scene lifecycle, events, triggers, bytecode package loading, and installable SDK targets.
- Fixed ticks, generation-checked gameplay entities, PCG random streams, deterministic timers/events, gameplay snapshots, and `RSGS` save/replay/rollback state.
- Native Phase 18 structured control flow: `for`, array/indexed-collection `foreach`, `do/while`, `break`, `continue`, and equality-based `switch`.
- Native compile-time interface contracts with exact signatures and module/import visibility.
- Native source Attribute syntax and metadata retained through Compilation and the Game SDK.
- Exact-width values, mutable structs, ref locations, nullable values, boxing, patterns, initializers, flexible arguments, and deterministic `try/catch/finally` semantics.
- Ubuntu and Windows Server 2025 / Visual Studio 2026 warnings-as-errors CI.

## Build and test

```bash
cmake -S . -B build \
  -DREALSCRIPT_BUILD_TESTS=ON \
  -DREALSCRIPT_WARNINGS_AS_ERRORS=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

## Basic script

```csharp
module Game.Main;

int main()
{
    int total = 0;
    for (int value = 0; value < 5; value = value + 1)
    {
        if (value == 1) continue;
        total = total + value;
    }
    return total;
}
```

```bash
rsc game.rs
rsc game.rs --mir
rsc game.rs --symbols
rsc game.rs --run Game.Main::main
```

## Native interface and attribute example

```csharp
module Game.Combat;

interface IDamageReader
{
    int ReadDamage();
}

[Serializable(version = 1)]
class DamageAccumulator : IDamageReader
{
    [Replicated(channel = "state")]
    int total;

    int ReadDamage()
    {
        return total;
    }
}
```

The Parser retains original Attribute and interface syntax nodes. Compilation validates the complete interface method signature and exposes stable module-qualified metadata to the Game SDK.

## Deterministic gameplay example

```csharp
sequence Attack(long target)
{
    PlayWindup();
    yield wait_ticks(12);
    SpawnProjectile();
    yield wait_ticks(6);
    Finish();
}
```

Sequences lower to explicit deterministic state machines. Parameters, locals, temporaries, and control-flow position survive suspension; nesting, cancellation, results, snapshots, replay, rollback, and generated C++17 AOT use the same fixed-tick semantics.

## C++17 AOT

```bash
rsaot \
  --output-dir build/generated/game \
  --program-name GameScripts \
  --opt-level 2 \
  --opt-report \
  game.rs
```

The generator emits deterministic C++17 source, a header, source metadata, and a manifest. Generated code executes verified Typed MIR directly and does not embed the bytecode interpreter.

## Game SDK

```cpp
#include "realscript/game/Gameplay.h"

realscript::game::GameApi api;
auto gameplay = std::make_shared<realscript::game::GameplayHost>(60, 1234, 7);
realscript::game::installGameplayBindings(api, gameplay);

realscript::game::GameScriptCompiler compiler(api);
auto compiled = compiler.compile(sources);
const auto& metadata = compiled.program.languageMetadata();
```

Use `SceneScriptRuntime` for lifecycle/event dispatch and `SceneGameplayDriver` for fixed-tick execution. Gameplay scheduling state uses `encodeGameplayHostState()` / `restoreGameplayHostState()`; script object fields use `ScriptObjectState`.

## Command-line tools

| Tool | Purpose |
|---|---|
| `rsc` | Compile, validate, inspect MIR/bytecode, run scripts, emit module directories, and emit profiles/digests. |
| `rsaot` | Generate deterministic C++17 AOT sources and manifests. |
| `rsdebug` | Source-level Debug Adapter Protocol server. |
| `rslsp` | Language Server Protocol server. |
| `rsbench` | Deterministic benchmark and profile runner. |

## Architecture

```text
RealScript source files
        |
        v
Native Phase 18–24 syntax and semantic pipeline
        |
        v
Parser / Syntax Trees / Compilation / Binder / Flow Analysis
        |
        v
Verified multi-block Typed MIR
        |
        +--------> Register bytecode -----> Bytecode VM
        +--------> Generated C++17 -------> Platform AOT compiler
        +--------> Toolchain JIT ---------> Shared native module

Shared type system / runtime services / metadata / GC / bindings
        |
        v
Game Scripting SDK / deterministic gameplay runtime
        |
        v
C++17 game engine
```

## Documentation

- [English documentation library](docs/en/README.md)
- [Language and type system](docs/en/LANGUAGE_AND_TYPE_SYSTEM.md)
- [C#-style compatibility matrix](docs/en/CSHARP_COMPATIBILITY_MATRIX.md)
- [Phase 11–18 language profile](docs/en/NATIVE_LANGUAGE_PHASE_11_18.md)
- [Phase 18–24 native roadmap](docs/roadmap/PHASE_18_24_NATIVE_LANGUAGE_AND_RUNTIME.md)
- [Phase 19 runtime polymorphism](docs/roadmap/PHASE_19_RUNTIME_POLYMORPHISM.md)
- [Phase 20 first-class delegates and closures](docs/roadmap/PHASE_20_FIRST_CLASS_DELEGATES.md)
- [Phase 21 complete generics and collections](docs/roadmap/PHASE_21_COMPLETE_GENERICS_AND_COLLECTIONS.md)
- [Phase 22 deterministic coroutine state machines](docs/roadmap/PHASE_22_DETERMINISTIC_COROUTINE_STATE_MACHINES.md)
- [Phase 23 value and reference semantics](docs/roadmap/PHASE_23_COMPLETE_VALUE_AND_REFERENCE_SEMANTICS.md)
- [Phase 24 language completeness and structured errors](docs/roadmap/PHASE_24_LANGUAGE_COMPLETENESS_AND_STRUCTURED_ERRORS.md)
- [Game Scripting SDK](docs/en/GAME_SCRIPTING_SDK.md)
- [Deterministic gameplay runtime](docs/en/GAMEPLAY_RUNTIME.md)
- [Project status and roadmap](docs/en/PROJECT_STATUS_AND_ROADMAP.md)

## Completed roadmap

- [x] Phase 1–6: compiler, runtime, GC, tooling, AOT, determinism, optimization, and optional JIT.
- [x] Phase 7: Game Scripting SDK and productization.
- [x] Phase 8–10: deterministic gameplay runtime and state codec.
- [x] Phase 11–17: bounded C#-style gameplay profile.
- [x] Phase 18: native Phase 11–17 syntax, semantics, metadata, tooling, and artifacts.
- [x] Phase 19: runtime polymorphism and visibility.
- [x] Phase 20: first-class delegates, heap closures, multicast, and general events.
- [x] Phase 21: inferred compile-time generics, constraints, growable collections, and enumerators.
- [x] Phase 22: deterministic coroutine state machines.
- [x] Phase 23: exact value types, complete implemented ref semantics, nullable values, and boxing.
- [x] Phase 24: convenience syntax, patterns, structured exceptions, and tooling closure.

## Deliberate limits

RealScript is not CLR-compatible C#. Current limits include:

- single class inheritance and runtime interface/virtual dispatch are supported; multiple class inheritance and default interface implementations are not;
- compile-time generic inference and constraints are supported; open runtime generics and variance are not;
- common constant/null/type/discard patterns are supported; relational/property/list/recursive pattern families are not;
- no general `Task`/threads/`async`;
- ref locals/returns/fields/indexers are supported, but unsafe pointers, ref structs, ref properties, and full escape analysis are not;
- script-object exceptions and deterministic `finally` are supported; filters, `using`, native exception interop, and exceptions across coroutine suspension are not;
- no operator overloads, user-defined conversions, CLR reflection/`dynamic`, LINQ, or the .NET base-class library;
- direct in-process machine-code JIT, OSR, PGO, and rollback networking remain future work.

## License

RealScript is licensed under the Apache License 2.0. See [LICENSE](LICENSE).
