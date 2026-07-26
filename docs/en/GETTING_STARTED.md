# Getting Started

[Documentation Home](README.md) | [Repository README](../../README.md)

This guide builds the RealScript toolchain, compiles a small multi-file program, runs it through the interpreter, generates C++17 AOT output, and executes a benchmark.

## Requirements

- CMake 3.20 or newer
- A C++17 compiler
- A C11 compiler for the native ABI conformance test
- Python 3 for CI helper scripts

The core compiler and runtime do not depend on third-party libraries. The optional toolchain JIT invokes an external C++ compiler at runtime and loads the resulting shared library.

## Build

```bash
cmake -S . -B build \
  -DREALSCRIPT_BUILD_TESTS=ON \
  -DREALSCRIPT_WARNINGS_AS_ERRORS=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

On a single-config generator, the `--config Debug` and `-C Debug` options may be omitted.

To disable external toolchain JIT integration tests:

```bash
cmake -S . -B build \
  -DREALSCRIPT_BUILD_TESTS=ON \
  -DREALSCRIPT_ENABLE_TOOLCHAIN_JIT_TESTS=OFF
```

## First Program

Create `hello.rs`:

```csharp
module Demo.Hello;

int main()
{
    return 42;
}
```

Validate it:

```bash
build/rsc hello.rs
```

Run it:

```bash
build/rsc hello.rs --run Demo.Hello::main
```

The exact executable location depends on the generator. Visual Studio builds normally place executables under `build/Debug/`.

## Multiple Modules

Create `math.rs`:

```csharp
module Demo.Math;

int twice(int value)
{
    return value * 2;
}
```

Create `app.rs`:

```csharp
module Demo.App;
import Demo.Math;

int main()
{
    return twice(21);
}
```

Compile and run both files:

```bash
build/rsc math.rs app.rs --run Demo.App::main
```

A module may be split across multiple source files. Imports operate on module names, not physical file paths.

## Inspect the Compiler Pipeline

Print verified Typed MIR:

```bash
build/rsc math.rs app.rs --mir
```

List stable symbols:

```bash
build/rsc math.rs app.rs --symbols
```

Print typed register bytecode:

```bash
build/rsc math.rs app.rs --bytecode
```

Write and disassemble a `.rsbc` module:

```bash
build/rsc math.rs app.rs --emit-bytecode demo.rsbc
build/rsc demo.rsbc --disassemble
```

## Optimization and Deterministic Execution

Run optimized MIR with deterministic tracing, a profile, and an execution digest:

```bash
build/rsc math.rs app.rs \
  --run Demo.App::main \
  --opt-level 2 \
  --deterministic \
  --profile \
  --digest
```

Optimization levels:

- `0`: preserve the verified input MIR;
- `1`: local folding and basic control-flow cleanup;
- `2`: bounded fixed-point optimization and conservative dead-value removal.

Optimization never removes observable runtime checks or host calls.

## Generate C++17 AOT Sources

```bash
build/rsaot \
  --output-dir build/generated/demo \
  --program-name DemoScripts \
  --opt-level 2 \
  --opt-report \
  math.rs app.rs
```

The output directory contains:

- a generated public header;
- generated C++17 implementation;
- a deterministic manifest;
- source-map metadata.

The generated code executes Typed MIR semantics directly and links against the RealScript AOT support runtime.

## Add AOT to a CMake Project

```cmake
include(cmake/RealScriptAot.cmake)

realscript_add_aot_library(DemoScriptsAot
    PROGRAM_NAME DemoScripts
    OPT_LEVEL 2
    SOURCES
        math.rs
        app.rs
)

target_link_libraries(game PRIVATE DemoScriptsAot)
```

The custom target tracks script inputs and does not rewrite unchanged generated files.

## Benchmark

```bash
build/rsbench \
  --entry Demo.App::main \
  --warmup 20 \
  --iterations 1000 \
  --opt-level 2 \
  --json \
  math.rs app.rs
```

Benchmark output includes timing data, optimization statistics, a stable semantic digest, and per-function profile counters.

## Editor Integration

Start the language server:

```bash
build/rslsp
```

Start the debug adapter with source files:

```bash
build/rsdebug math.rs app.rs
```

Both processes use standard stdin/stdout protocol framing and are intended to be launched by an editor extension or protocol client.

## Where to Go Next

- [Language and Type System](LANGUAGE_AND_TYPE_SYSTEM.md)
- [Compilation, MIR, and Bytecode](COMPILATION_AND_BYTECODE.md)
- [Runtime, GC, and Embedding](RUNTIME_GC_AND_EMBEDDING.md)
- [AOT, JIT, and Performance](AOT_JIT_AND_PERFORMANCE.md)
