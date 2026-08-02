# RealScript

**English** | [简体中文](README.zh-CN.md)

RealScript is an embedded, strongly typed scripting language and runtime for modern C++17 game engines. It uses a C#-inspired syntax and provides a verified bytecode interpreter, deterministic C++17 AOT generation, an optional external-toolchain JIT, source-level debugging, language tooling, hot reload, typed game bindings, and fixed-tick gameplay services.

> **Project status:** Phase 1–17 provides the compiler/runtime, Game Scripting SDK, deterministic gameplay runtime, and bounded C#-style gameplay profile. Phase 18 is migrating that profile from source expansion into native compiler semantics. Native control flow, interface contracts, and source attributes are complete on the Phase 18 branch; delegates/events, generics, sequences, and reference modifiers remain in migration. Language, bytecode, metadata, SDK, gameplay-state, and ABI contracts remain draft.

## Highlights

- Dependency-free C++17 and CMake foundation.
- Lexer, parser, Binder, flow analysis, diagnostics, multi-file modules, imports, and incremental snapshots.
- Verified multi-block Typed MIR with O0/O1/O2 optimization.
- Typed register bytecode, deterministic `.rsbc` 0.5 encoding, strict verification, and structured runtime errors.
- Classes, constructors, methods, properties, arrays, enums, structs, strings, native handles, and exact type identities.
- Precise roots, generation-checked references, incremental mark/sweep GC, write barriers, heap snapshots, and leak diagnostics.
- DAP debugger, LSP language server, source metadata, and body-only hot reload.
- Deterministic C++17 AOT, native module ABI, record/replay, profiling, benchmarking, and optional toolchain JIT.
- Typed C++ Game SDK, rooted script objects, scene lifecycle, events, triggers, bytecode package loading, and installable SDK targets.
- Fixed ticks, generation-checked gameplay entities, PCG random streams, deterministic timers/events, gameplay snapshots, and `RSGS` save/replay/rollback state.
- Native Phase 18 structured control flow: `for`, array/indexed-collection `foreach`, `do/while`, `break`, `continue`, and equality-based `switch`.
- Native compile-time interface contracts with exact signatures and module/import visibility.
- Native source Attribute syntax and metadata retained through Compilation and the Game SDK.
- Compatibility support remains for bounded events/lambdas, explicit generics, deterministic sequences, and restricted reference calls while their native Phase 18 migrations are completed.
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

Sequence callbacks are currently compatibility-lowered through `GameplayHost` and `TickScheduler`, so they participate in fixed-tick ordering, snapshots, replay, and rollback. Native sequence nodes are part of the remaining Phase 18 migration.

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
| `rsc` | Compile, validate, inspect MIR/bytecode, run scripts, and emit profiles/digests. |
| `rsaot` | Generate deterministic C++17 AOT sources and manifests. |
| `rsdebug` | Source-level Debug Adapter Protocol server. |
| `rslsp` | Language Server Protocol server. |
| `rsbench` | Deterministic benchmark and profile runner. |

## Architecture

```text
RealScript source files
        |
        v
Native Phase 18 syntax / remaining compatibility expansion
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
- [Phase 11–18 language profile](docs/en/LANGUAGE_EXPANSION_PHASE_11_17.md)
- [Phase 18–24 native roadmap](docs/roadmap/PHASE_18_24_NATIVE_LANGUAGE_AND_RUNTIME.md)
- [Game Scripting SDK](docs/en/GAME_SCRIPTING_SDK.md)
- [Deterministic gameplay runtime](docs/en/GAMEPLAY_RUNTIME.md)
- [Project status and roadmap](docs/en/PROJECT_STATUS_AND_ROADMAP.md)

## Completed roadmap

- [x] Phase 1–6: compiler, runtime, GC, tooling, AOT, determinism, optimization, and optional JIT.
- [x] Phase 7: Game Scripting SDK and productization.
- [x] Phase 8–10: deterministic gameplay runtime and state codec.
- [x] Phase 11–17: bounded C#-style gameplay profile.
- [x] Phase 18A: native structured control flow.
- [x] Phase 18C: native interface contracts and source attributes.
- [ ] Phase 18B/18D–18F: native events/lambdas, generics, sequences, and reference modifiers.

## Deliberate limits

RealScript is not CLR-compatible C#. Current limits include:

- no class inheritance or runtime interface/virtual dispatch;
- bounded event lambdas are not first-class heap closure objects;
- no inferred/open generics, constraints, variance, or automatically growing collections;
- no pattern matching or general enumerator protocol;
- no general `Task`/threads/`async`;
- no ref locals, ref returns, ref fields, ref indexers, or complete lifetime analysis;
- value aliases reuse current canonical carriers rather than distinct exact ABI types;
- native source attributes are not yet serialized in `.rsbc`;
- exceptions, nullable values, boxing, direct machine-code JIT, OSR, PGO, and rollback networking remain future work.

## License

A license has not yet been selected. Do not assume that the code or documentation may be redistributed until a license file is committed.