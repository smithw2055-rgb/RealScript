# RealScript

**English** | [简体中文](README.zh-CN.md)

RealScript is an embedded, strongly typed scripting language and runtime for modern C++17 game engines. It uses a C#-inspired syntax and provides a verified bytecode interpreter, deterministic C++17 AOT generation, an optional external-toolchain JIT, source-level debugging, language tooling, hot reload, typed game bindings, and fixed-tick gameplay services.

> **Project status:** Phase 1–10 provides the compiler/runtime, Game Scripting SDK, and deterministic gameplay runtime. Phase 11–17 adds a bounded C#-style gameplay-language profile with multi-file/module expansion, structured control flow, events/lambdas, compile-time interfaces, source attributes, explicit generics, deterministic sequences, and restricted reference parameters. The language, expansion profile, `.rsbc`, object/Game SDK ABIs, metadata, gameplay-state format, and GC contracts remain draft.

## Highlights

- Dependency-free C++17 and CMake foundation.
- Lexer, parser, binder, flow analysis, diagnostics, multi-file modules, imports, and incremental snapshots.
- Verified multi-block Typed MIR with O0/O1/O2 optimization.
- Typed register bytecode, deterministic `.rsbc` 0.5 encoding, strict verification, and structured runtime errors.
- Classes, constructors, methods, properties, arrays, enums, structs, strings, native handles, and exact type identities.
- Precise roots, generation-checked references, incremental mark/sweep GC, write barriers, heap snapshots, and leak diagnostics.
- DAP debugger, LSP language server, source metadata, and body-only hot reload.
- Deterministic C++17 AOT, native module ABI, record/replay, profiling, benchmarking, and optional toolchain JIT.
- Typed C++ Game SDK, rooted script objects, scene lifecycle, events, triggers, bytecode package loading, and installable SDK targets.
- Fixed ticks, generation-checked gameplay entities, PCG random streams, deterministic timers/events, gameplay snapshots, and `RSGS` save/replay/rollback state.
- Phase 11 structured control flow: `for`, array/fixed-collection `foreach`, `do/while`, `break`, `continue`, and equality-based `switch`.
- Phase 12 deterministic class-local events, method groups, and bounded lambdas.
- Phase 13 interface declarations and compile-time implementation contracts.
- Phase 14 source attributes retained through `Compilation`, `GameCompileResult`, and `GameProgram`.
- Phase 15 explicit deterministic generic specialization and fixed-capacity collection profiles.
- Phase 16 `sequence` / `yield wait_ticks` lowering through the fixed-tick gameplay runtime.
- Phase 17 restricted standalone `ref`, `out`, and `in` calls plus current-runtime value aliases.
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

`math.rs`:

```csharp
module Game.Math;

int twice(int value)
{
    return value * 2;
}
```

`main.rs`:

```csharp
module Game.Main;
import Game.Math;

int main()
{
    return twice(21);
}
```

```bash
rsc math.rs main.rs
rsc math.rs main.rs --mir
rsc math.rs main.rs --symbols
rsc math.rs main.rs --run Game.Main::main
```

Run with deterministic execution, profiling, and a stable digest:

```bash
rsc math.rs main.rs \
  --run Game.Main::main \
  --opt-level 2 \
  --deterministic \
  --profile \
  --digest
```

## Phase 11–17 gameplay-language example

```csharp
module Game.Combat;

delegate void DamageHandler(int amount);

interface IDamageReader
{
    int ReadDamage();
}

[Serializable(version = 1)]
class DamageAccumulator<T> : IDamageReader
{
    T marker;
    int total;
    event DamageHandler Damaged;

    void Add(int amount)
    {
        total = total + amount;
    }

    int ReadDamage()
    {
        return total;
    }

    int Run(int[] values)
    {
        Damaged += Add;
        Damaged += value => total = total + value;

        List<int> accepted = new List<int>(16);
        foreach (int value in values)
        {
            if (value < 0) continue;
            accepted.Add(value);
            Damaged(value);
        }

        switch (accepted.Count())
        {
            case 0:
                return 0;
            default:
                return total;
        }
    }
}
```

Extended source is normalized in deterministic module/path order and then passes through the ordinary parser, binder, Typed MIR, bytecode/interpreter, optimizer, AOT generator, and optional JIT pipeline.

## Deterministic sequence example

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

Sequence callbacks are scheduled through `GameplayHost` and `TickScheduler`, so they participate in fixed-tick ordering, snapshot, replay, and rollback state. Durable state across yields belongs in script object fields.

## C++17 AOT

```bash
rsaot \
  --output-dir build/generated/game \
  --program-name GameScripts \
  --opt-level 2 \
  --opt-report \
  math.rs main.rs
```

The generator emits deterministic C++17 source, a header, source metadata, and a manifest. Generated code executes verified Typed MIR directly and does not embed the bytecode interpreter.

CMake projects can create an AOT library directly:

```cmake
include(cmake/RealScriptAot.cmake)

realscript_add_aot_library(GameScriptsAot
    PROGRAM_NAME GameScripts
    OPT_LEVEL 2
    SOURCES math.rs main.rs
)

target_link_libraries(game PRIVATE GameScriptsAot)
```

## Game SDK and gameplay runtime

```cpp
#include "realscript/game/Gameplay.h"

realscript::game::GameApi api;
auto gameplay = std::make_shared<realscript::game::GameplayHost>(60, 1234, 7);
realscript::game::installGameplayBindings(api, gameplay);

realscript::game::GameScriptCompiler compiler(api);
auto compiled = compiler.compile(sources);

// Source attributes and interface/generic expansion metadata remain available.
const auto& metadata = compiled.program.languageMetadata();
```

Use `SceneScriptRuntime` for lifecycle/event dispatch and `SceneGameplayDriver` for fixed-tick execution. Gameplay scheduling state uses `encodeGameplayHostState()` / `restoreGameplayHostState()`; script object fields use `ScriptObjectState`. An engine rollback frame should combine both state domains.

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
Optional Phase 11–17 deterministic language expansion
        |
        v
Lexer / Parser / Syntax Trees
        |
        v
Compilation / Module Graph / Binder / Flow Analysis
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
- [Phase 11–17 language expansion profile](docs/en/LANGUAGE_EXPANSION_PHASE_11_17.md)
- [Game Scripting SDK](docs/en/GAME_SCRIPTING_SDK.md)
- [Deterministic gameplay runtime](docs/en/GAMEPLAY_RUNTIME.md)
- [SDK productization](docs/en/PRODUCTIZATION.md)
- [Project status and roadmap](docs/en/PROJECT_STATUS_AND_ROADMAP.md)
- [Chinese documentation home](docs/zh-CN/README.md)
- [Chinese Phase 11–17 implementation profile](docs/roadmap/PHASE_11_17_LANGUAGE_EXPANSION.md)

## Completed roadmap

- [x] Phase 1–6: compiler, runtime, GC, tooling, AOT, determinism, optimization, and optional JIT.
- [x] Phase 7: Game Scripting SDK and productization.
- [x] Phase 8–10: deterministic gameplay runtime and state codec.
- [x] Phase 11: structured control flow.
- [x] Phase 12: bounded delegates, lambdas, and events.
- [x] Phase 13: interface contracts.
- [x] Phase 14: source metadata.
- [x] Phase 15: explicit generics and fixed-capacity collections.
- [x] Phase 16: deterministic sequences.
- [x] Phase 17: restricted reference parameters and value aliases.

## Deliberate limits

Phase 11–17 is a deterministic embedded game-language profile, not full CLR/C#:

- no class inheritance or runtime interface/virtual dispatch;
- delegates are not general first-class runtime values;
- lambdas do not capture arbitrary locals into heap closure objects;
- no inferred/open generics, constraints, variance, or automatically growing collections;
- no pattern matching or general enumerator protocol;
- sequences support `yield wait_ticks`, not general `Task`/threads/`async`;
- no ref locals, ref returns, ref fields, ref indexers, or general reference lifetime analysis;
- value aliases reuse current canonical runtime carriers rather than distinct exact ABI types;
- source attributes are not yet serialized in `.rsbc`;
- expanded generated code does not yet have complete exact source-map remapping;
- exceptions, nullable values, boxing, direct machine-code JIT, OSR, PGO, and rollback networking remain future work.

## License

A license has not yet been selected. Do not assume that the code or documentation may be redistributed until a license file is committed.
