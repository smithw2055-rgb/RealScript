# RealScript

[![RealScript CI](https://github.com/smithw2055-rgb/RealScript/actions/workflows/ci.yml/badge.svg)](https://github.com/smithw2055-rgb/RealScript/actions/workflows/ci.yml)
[![GitHub release](https://img.shields.io/github/v/release/smithw2055-rgb/RealScript)](https://github.com/smithw2055-rgb/RealScript/releases)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)

**English** | [简体中文](README.zh-CN.md)

RealScript is an embedded, strongly typed scripting language and runtime for
modern C++17 game engines. It combines C#-inspired source syntax with verified
bytecode, deterministic execution, C++17 AOT, an optional external-toolchain
JIT, game-oriented bindings, fixed-tick coroutines, replay, and rollback.

> **v0.2.0 status:** The Phase 1–24 implementation is complete and usable as a
> same-SDK engine integration baseline. The source language, `.rsbc` format,
> native ABI, metadata, and gameplay-state contracts remain versioned but are
> not frozen for long-term compatibility. RealScript is not CLR-compatible C#.

## Why RealScript

- **One verified semantic model:** interpreter, AOT, and JIT share checked
  arithmetic, errors, budgets, profiles, and deterministic event behavior.
- **Built for simulation:** fixed ticks, seeded PCG streams, deterministic
  timers/events, serializable coroutines, snapshots, replay, and rollback.
- **Native engine integration:** typed C++ bindings, managed script objects,
  native handles, CMake package targets, and generated C++17 modules.
- **Useful development tools:** compiler/runner, AOT generator, DAP debugger,
  LSP server, hot reload, profiler, and benchmark runner.
- **No third-party core dependency:** the compiler and runtime require C++17
  and CMake; the optional JIT invokes an installed C++ compiler.

## Language and runtime features

| Area | Implemented in v0.2.0 |
|---|---|
| Language | Modules/imports, overloads, classes, single inheritance, interfaces, virtual dispatch, delegates, closures, events, generics, collections, enums, mutable structs, nullable values, boxing, patterns, initializers, flexible arguments, and structured exceptions. |
| Control flow | `if`, `while`, `for`, `foreach`, `do/while`, `switch`, switch expressions, `break`, `continue`, `try/catch/finally`, and deterministic `sequence`/`yield wait_ticks`. |
| Compiler | Lexer, parser, Binder, flow analysis, stable diagnostics, verified multi-block Typed MIR, and O0/O1/O2 optimization. |
| Bytecode | Typed register VM, deterministic `.rsbc` 0.9 output, 0.6–0.8 legacy decoding, defensive validation, and structured script stacks. |
| Memory | Generation-checked object references, precise roots, incremental mark/sweep GC, write barriers, bounded GC work, snapshots, retaining paths, and leak diagnostics. |
| Native execution | Deterministic C++17 AOT, C11 module-query ABI, source maps, typed native thunks, optional toolchain JIT, and content-addressed JIT cache. |
| Determinism | Off/Strict/Record/Replay modes, instruction/recursion/allocation/heap budgets, stable digests, external-call replay, and cross-backend differential validation. |
| Game SDK | Typed bindings, rooted objects, scene lifecycle, events/triggers, fixed-tick gameplay host, sequence scheduling, `RSGS` state encoding, save/replay/rollback helpers, and installable CMake targets. |
| Tooling | `rsc`, `rsaot`, `rsdebug`, `rslsp`, `rsbench`, DAP, LSP, profiling, source metadata, and body-compatible hot reload. |

For the exact supported/partial/unsupported language inventory, see the
[C#-style compatibility matrix](docs/en/CSHARP_COMPATIBILITY_MATRIX.md).

## Quick start

### Requirements

- CMake 3.20 or newer
- a C++17 compiler
- a C11 compiler when building the native ABI conformance test
- Python 3 for repository CI helpers

### Build and test

```bash
cmake -S . -B build \
  -DREALSCRIPT_BUILD_TESTS=ON \
  -DREALSCRIPT_WARNINGS_AS_ERRORS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Visual Studio generators normally place tools under `build/Release/`. With a
single-config generator, omit `--config Release` and `-C Release`.

### Write and run a script

Create `hello.rs`:

```csharp
module Demo.Hello;

class Counter
{
    int value;

    int Add(int amount)
    {
        value = value + amount;
        return value;
    }
}

int main()
{
    int total = 0;
    for (int value = 0; value < 10; value = value + 1)
    {
        total = total + value;
    }
    return total;
}
```

Validate, inspect, and run it:

```bash
build/rsc hello.rs
build/rsc hello.rs --mir
build/rsc hello.rs --bytecode
build/rsc hello.rs --run Demo.Hello::main --opt-level 2
```

### Generate C++17 AOT

```bash
build/rsaot \
  --output-dir build/generated/hello \
  --program-name HelloScripts \
  --opt-level 2 \
  --opt-report \
  hello.rs
```

Generated output contains a public header, C++17 implementation, deterministic
manifest, source metadata, and native function descriptors. It executes
verified Typed MIR semantics directly and does not embed the bytecode VM.

### Embed the Game SDK

```cpp
#include "realscript/game/Gameplay.h"

realscript::game::GameApi api;
auto gameplay = std::make_shared<realscript::game::GameplayHost>(60, 1234, 7);
realscript::game::installGameplayBindings(api, gameplay);

realscript::game::GameScriptCompiler compiler(api);
auto compiled = compiler.compile(sources);
realscript::game::ScriptRuntime scripts(compiled.program);
realscript::game::SceneScriptRuntime scene(scripts);
```

See [Getting Started](docs/en/GETTING_STARTED.md) for multi-file modules,
bytecode artifacts, CMake AOT integration, debugging, and editor tooling.

## Execution backends

| Backend | Best use | Notes |
|---|---|---|
| Bytecode interpreter | Development, debugging, hot reload, and cold or low-frequency scripts | Fast startup and full tooling; slower than native execution for CPU-heavy loops. |
| C++17 AOT | Shipping gameplay and performance-sensitive deterministic code | Portable generated C++ with the same checked runtime semantics. |
| Toolchain JIT | Desktop development and native execution without a separate AOT build step | Reuses AOT generation, invokes an external compiler, and caches the shared module. |

## Performance snapshot

The following are local Release measurements from an AMD Ryzen 7 6800H on
Windows 11 with Visual Studio 2026. They are regression baselines, not
cross-language rankings. Times are medians; RAW uses `gcWorkBudget=0` so GC work
is measured separately.

| Workload | Result |
|---|---:|
| Native C++ integer loop | 4.06 µs |
| RealScript C++17 AOT RAW | 25.86 µs |
| RealScript toolchain JIT RAW | 25.68 µs |
| AOT Strict deterministic | 0.358 ms |
| JIT Strict deterministic | 0.347 ms |
| Interpreter integer loop, 130,011 instructions | 6.73 ms |
| Interpreter branch loop, 180,011 instructions | 9.46 ms |
| Interpreter function-call loop, 10,000 calls | 12.99 ms |
| Allocation tick, GC work 8 | 27.75 ms; 460 collections; 16,320 B live |
| 10,000 coroutine resume | 86.06 ms; 8.61 µs/callback |
| 10,000 coroutine deterministic replay | 89.45 ms |

Key v0.2.0 performance changes:

- typed AOT/JIT scalar lowering and range-proven checked arithmetic;
- direct typed CFG labels and failure-aware accounting;
- local RAW/Strict accounting with exact budget and digest preservation;
- interpreter integer specialization and cheaper call-stack materialization;
- bounded incremental sweep progress during continuous allocation;
- cached scene method descriptors and lower scheduler payload-copy cost.

Read the [AOT/JIT performance guide](docs/en/AOT_JIT_AND_PERFORMANCE.md) and the
[full benchmark report](docs/zh-CN/PERFORMANCE_BASELINE_2026-08-09.md) for
methodology, optimization history, rejected experiments, and reproduction
commands.

## Deterministic gameplay

```csharp
sequence Attack(long target)
{
    PlayWindup();
    yield wait_ticks(12);
    SpawnProjectile(target);
    yield wait_ticks(6);
    Finish();
}
```

Sequences lower to explicit state machines. Their parameters, locals,
temporaries, nested progress, and control-flow position survive suspension,
snapshot, restore, replay, rollback, and C++17 AOT generation.

## Command-line tools

| Tool | Purpose |
|---|---|
| `rsc` | Compile, validate, inspect MIR/bytecode, run scripts, emit packages, profiles, and digests. |
| `rsaot` | Generate deterministic C++17 AOT sources and manifests. |
| `rsdebug` | Run the source-level Debug Adapter Protocol server. |
| `rslsp` | Run the Language Server Protocol server. |
| `rsbench` | Run reproducible timing, GC, digest, and profile benchmarks. |

## Documentation

- [English documentation home](docs/en/README.md)
- [Getting Started](docs/en/GETTING_STARTED.md)
- [Language and Type System](docs/en/LANGUAGE_AND_TYPE_SYSTEM.md)
- [C#-Style Compatibility Matrix](docs/en/CSHARP_COMPATIBILITY_MATRIX.md)
- [Architecture](docs/en/ARCHITECTURE.md)
- [Compilation, MIR, and Bytecode](docs/en/COMPILATION_AND_BYTECODE.md)
- [Runtime, GC, and Embedding](docs/en/RUNTIME_GC_AND_EMBEDDING.md)
- [AOT, JIT, and Performance](docs/en/AOT_JIT_AND_PERFORMANCE.md)
- [Determinism and Replay](docs/en/DETERMINISM_AND_REPLAY.md)
- [Game Scripting SDK](docs/en/GAME_SCRIPTING_SDK.md)
- [Deterministic Gameplay Runtime](docs/en/GAMEPLAY_RUNTIME.md)
- [SDK Productization](docs/en/PRODUCTIZATION.md)
- [Project Status and Roadmap](docs/en/PROJECT_STATUS_AND_ROADMAP.md)
- [Changelog](CHANGELOG.md)
- [中文文档入口](docs/zh-CN/README.md)

## Deliberate limits

RealScript is not CLR-compatible C#. v0.2.0 deliberately does not provide:

- multiple class inheritance, default interface implementations, open runtime
  generics, or generic variance;
- general `Task`, threads, or unrestricted `async/await`;
- unsafe pointers, ref structs, ref properties, or complete escape analysis;
- exception filters, `using`, native exception interop, or exceptions across a
  coroutine suspension point;
- operator overloads, user-defined conversions, LINQ, `dynamic`, reflection
  code generation, or the .NET base-class library;
- direct in-process machine-code JIT, OSR, PGO, or a rollback networking
  protocol;
- frozen long-term compatibility for source, `.rsbc`, AOT ABI, SDK metadata,
  or serialized gameplay state.

Pin an exact RealScript release or commit for engine integration and rebuild
generated AOT artifacts with the matching SDK.

## License

RealScript is licensed under the Apache License 2.0. See [LICENSE](LICENSE).
