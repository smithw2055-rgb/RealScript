# RealScript

**English** | [简体中文](README.zh-CN.md)

RealScript is an embedded, strongly typed scripting language and runtime designed for modern game engines. It uses a C#-inspired syntax and targets C++17 hosts through a verified bytecode interpreter, deterministic C++17 AOT generation, and an optional native toolchain JIT.

> **Project status:** the Phase 1–6 compiler/runtime roadmap, Phase 7 Game Scripting SDK, and Phase 8–10 deterministic gameplay runtime are complete and form the RealScript v0.1 alpha integration baseline. The source language, `.rsbc` format, object ABI, native module ABI, gameplay-state format, and GC contracts remain draft and are not frozen for long-term compatibility.

## Highlights

- Dependency-free C++17 and CMake foundation.
- Lexer, parser, binder, flow analysis, stable diagnostics, and incremental module compilation.
- Verified multi-block Typed MIR with explicit locals, block parameters, and O0/O1/O2 optimization levels.
- Typed register bytecode, deterministic `.rsbc` 0.5 encoding, disassembly, defensive decoding, and strict verification.
- Bytecode interpreter with structured errors, script stacks, instruction budgets, recursion budgets, and runtime statistics.
- Classes, constructors, methods, properties, arrays, enums, structs, strings, native handles, and exact type identities.
- Precise shadow-stack roots, generation-checked `ObjectRef` handles, incremental mark/sweep GC, write barriers, heap snapshots, retaining paths, and leak summaries.
- DAP debugger, LSP language server, source-level debug information, and body-only hot reload.
- Deterministic C++17 AOT generation, reusable CMake integration, and a C11/C++ native module query ABI.
- Strict, Record, and Replay execution modes with stable execution digests and host-binding determinism policies.
- Typed C++ game bindings, rooted script objects, scene lifecycle callbacks, events, triggers, and SDK productization.
- Generation-checked gameplay entities, fixed ticks, PCG random streams, deterministic timers/events, script contracts, metadata, and fixed-step scene driving.
- Versioned `RSGS` gameplay snapshots with defensive limits and stable state-hash verification for save, replay, and rollback integration.
- Per-function profiling, stable benchmark output, the `rsbench` CLI, and optional external-toolchain JIT.
- Interpreter/AOT/JIT differential validation.
- Ubuntu and Windows Server 2025 / Visual Studio 2026 GitHub Actions coverage.

## Quick Start

### Build and test

```bash
cmake -S . -B build \
  -DREALSCRIPT_BUILD_TESTS=ON \
  -DREALSCRIPT_WARNINGS_AS_ERRORS=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

### Compile multiple source files

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

Compile, inspect MIR, and list stable symbols:

```bash
rsc math.rs main.rs
rsc math.rs main.rs --mir
rsc math.rs main.rs --symbols
```

Run an entry function:

```bash
rsc math.rs main.rs --run Game.Main::main
```

Run with optimization, deterministic execution, profiling, and a stable digest:

```bash
rsc math.rs main.rs \
  --run Game.Main::main \
  --opt-level 2 \
  --deterministic \
  --profile \
  --digest
```

### Bytecode

```bash
rsc math.rs main.rs --bytecode
rsc math.rs main.rs --emit-bytecode game.rsbc
rsc game.rsbc --disassemble
```

A `.rsbc` module must pass physical decoding and semantic verification before execution.

### C++17 AOT

```bash
rsaot \
  --output-dir build/generated/game \
  --program-name GameScripts \
  --opt-level 2 \
  --opt-report \
  math.rs main.rs
```

The generator emits a header, C++17 source, source-map metadata, and a deterministic manifest. Generated code executes verified Typed MIR directly; it does not embed the bytecode interpreter.

CMake projects can create a native AOT library directly:

```cmake
include(cmake/RealScriptAot.cmake)

realscript_add_aot_library(GameScriptsAot
    PROGRAM_NAME GameScripts
    OPT_LEVEL 2
    SOURCES math.rs main.rs
)

target_link_libraries(game PRIVATE GameScriptsAot)
```

### Game SDK and deterministic gameplay runtime

```cpp
#include "realscript/game/Gameplay.h"

realscript::game::GameApi api;
auto gameplay = std::make_shared<realscript::game::GameplayHost>(60, 1234, 7);
realscript::game::installGameplayBindings(api, gameplay);

// Compile scripts with GameScriptCompiler, attach them through
// SceneScriptRuntime, then drive deterministic ticks with SceneGameplayDriver.
```

Gameplay scheduling state can be encoded with `encodeGameplayHostState()` and restored with `restoreGameplayHostState()`. Script object fields use the existing `ScriptObjectState` productization API; an engine rollback frame should combine both state domains.

### Benchmarking

```bash
rsbench \
  --entry Game.Main::main \
  --warmup 20 \
  --iterations 1000 \
  --opt-level 2 \
  --json \
  math.rs main.rs
```

### Debugger and language server

```bash
rsdebug game.rs common.rs   # Debug Adapter Protocol over stdin/stdout
rslsp                       # Language Server Protocol over stdin/stdout
```

## Command-Line Tools

| Tool | Purpose |
|---|---|
| `rsc` | Compile, validate, inspect MIR/bytecode, run scripts, and emit profiles/digests. |
| `rsaot` | Generate deterministic C++17 AOT sources and manifests. |
| `rsdebug` | Source-level DAP debug adapter. |
| `rslsp` | LSP language server. |
| `rsbench` | Deterministic benchmark and profile tool. |

## Architecture

```text
RealScript source files
        |
        v
Lexer / Parser / Syntax Trees
        |
        v
Compilation / Module Graph / Symbol Predeclaration
        |
        v
Binder / Overload Resolution / Flow Analysis
        |
        v
Verified multi-block Typed MIR
        |
        +--------> Register bytecode -----> Bytecode VM
        +--------> Generated C++17 -------> Platform AOT compiler
        +--------> Toolchain JIT ---------> Shared native module

Shared type system / runtime services / metadata / GC / bindings / debug info
        |
        v
Game Scripting SDK / deterministic gameplay runtime
        |
        v
C++17 game engine
```

All execution backends share the same type system, verified MIR semantics, runtime contracts, and differential tests. A backend must not bypass MIR and reinterpret the source AST independently. Gameplay services remain outside the execution backend so interpreter, AOT, and JIT scripts use the same fixed-tick semantics.

## Source Layout

```text
include/realscript/
  text/          SourceText and TextSpan
  diagnostics/   Stable diagnostics
  syntax/        Tokens, AST, lexer, and parser
  semantic/      Types, symbols, bound trees, binder, and flow analysis
  mir/           Multi-block Typed MIR, lowering, printing, and verification
  compiler/      Multi-file compilation, module graph, and incremental snapshots
  bytecode/      Register bytecode, codec, verifier, and disassembler
  runtime/       ProgramImage, interpreter, execution services, and managed heap
  game/          Host bindings, script objects, scene runtime, gameplay and state codec
  debug/         Debug metadata, debug sessions, and DAP
  tooling/       JSON, language services, and LSP
  hot_reload/    ProgramImage compatibility analysis and atomic replacement
  aot_cpp/       C++17 generator, AOT runtime, and public C ABI
  optimization/  Typed MIR optimizer and optimization statistics
  jit/           Optional toolchain JIT and dynamic module lifetime
src/              Implementations
tools/rsc/        Compiler and runtime CLI
tools/rsaot/      C++17 AOT generator
tools/rsdebug/    DAP adapter
tools/rslsp/      LSP server
tools/rsbench/    Benchmark and profile tool
cmake/             Reusable SDK and AOT integration
tests/             Conformance, regression, differential, and integration tests
docs/en/           Default English documentation
docs/spec/         Detailed Chinese specification set
docs/roadmap/      Implementation roadmap and phase notes
```

## Documentation

The default documentation language is English:

- [Documentation home](docs/README.md)
- [English documentation library](docs/en/README.md)
- [Getting started](docs/en/GETTING_STARTED.md)
- [Architecture](docs/en/ARCHITECTURE.md)
- [Language and type system](docs/en/LANGUAGE_AND_TYPE_SYSTEM.md)
- [Compilation, MIR, and bytecode](docs/en/COMPILATION_AND_BYTECODE.md)
- [Runtime, GC, and embedding](docs/en/RUNTIME_GC_AND_EMBEDDING.md)
- [Game Scripting SDK](docs/en/GAME_SCRIPTING_SDK.md)
- [Deterministic gameplay runtime](docs/en/GAMEPLAY_RUNTIME.md)
- [SDK productization](docs/en/PRODUCTIZATION.md)
- [Debugging, tooling, and hot reload](docs/en/DEBUGGING_TOOLING_AND_HOT_RELOAD.md)
- [AOT, JIT, and performance](docs/en/AOT_JIT_AND_PERFORMANCE.md)
- [Determinism and replay](docs/en/DETERMINISM_AND_REPLAY.md)
- [Project status and roadmap](docs/en/PROJECT_STATUS_AND_ROADMAP.md)

Chinese documentation remains available:

- [Chinese documentation home](docs/zh-CN/README.md)
- [Chinese Game Scripting SDK](docs/zh-CN/GAME_SCRIPTING_SDK.md)
- [Chinese deterministic gameplay runtime](docs/zh-CN/GAMEPLAY_RUNTIME.md)
- [Chinese architecture design](docs/ENGINE_DESIGN.md)
- [Chinese specification index](docs/spec/README.md)

## Completed Roadmap

- [x] Phase 1: language frontend, control flow, calls, modules, and incremental compilation.
- [x] Phase 2: typed register bytecode, interpreter, linking, observability, and embedding.
- [x] Phase 3: managed heap, precise GC, objects, arrays, native handles, members, and value types.
- [x] Phase 4: debug information, DAP, LSP, and body-only hot reload.
- [x] Phase 5: C++17 AOT, native module ABI, source maps, and differential testing.
- [x] Phase 6: deterministic record/replay, MIR optimization, profiling, benchmarking, and optional toolchain JIT.
- [x] Phase 7: typed Game Scripting SDK, script objects, scene lifecycle, events, and triggers.
- [x] Phase 8: deterministic entities, fixed ticks, random streams, timers, events, snapshots, and stable hashes.
- [x] Phase 9: script contracts, metadata, fixed-tick sequences, scene driver, and generated gameplay bindings.
- [x] Phase 10: versioned gameplay-state codec for save, replay, and rollback integration.

## Stability and Deliberate Limits

RealScript v0.1 is an alpha technical and integration baseline, not a frozen 1.0 language or binary platform.

The following remain intentionally unfrozen:

- source-language compatibility;
- `.rsbc` bytecode compatibility;
- object and native module ABI compatibility;
- Game SDK and gameplay-state compatibility;
- GC and embedding contracts;
- cross-toolchain distribution of precompiled AOT modules.

Notable features that remain unimplemented include inheritance, source-language interfaces and virtual dispatch, generics, exceptions, source-language coroutines/`async`, `ref`/`out`, complete `for`/`foreach`/`switch` syntax, source attributes, direct machine-code JIT generation, OSR, PGO, and a rollback networking protocol. The deterministic gameplay runtime supplies host-level contracts, metadata, sequences, snapshots, and fixed-tick services without pretending those source-language features already exist.

## Design References

RealScript combines ideas from AngelScript's embedding and debugging model, Luau's VM performance engineering, Unity IL2CPP's C++ AOT workflow, native JIT toolchains, and the DAP/LSP editor protocols.

## License

A license has not yet been selected. Do not assume that the code or documentation may be redistributed until a license file is committed.
