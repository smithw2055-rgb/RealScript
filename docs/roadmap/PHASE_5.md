# Phase 5 — C++17 AOT Backend

## Status

Implemented as the first production-oriented native backend on top of the Phase 1–4 compiler, runtime, metadata and debugging foundations.

## Goal

Phase 5 converts verified Typed MIR into ordinary C++17, compiles that source with the platform toolchain, and executes it through the same runtime semantics used by the bytecode interpreter:

```text
RealScript sources
      │
      ▼
Compilation / Typed MIR validation
      │
      ▼
CppGenerator
      ├── realscript_aot_generated.h
      ├── realscript_aot_generated.cpp
      └── realscript_aot_manifest.json
      │
      ▼
MSVC / Clang / GCC / platform compiler
      │
      ▼
Native static library or linked module
      │
      ├── ProgramDescriptor / C++ host wrapper
      └── rs_module_query_v1 / stable C discovery ABI
```

The generated implementation does not embed or dispatch through the bytecode interpreter. It lowers MIR basic blocks directly into C++ control flow and calls explicit AOT runtime intrinsics for observable language operations.

## Phase 5A — MIR-to-C++17 generation

`CppGenerator` accepts verified MIR modules and produces deterministic output. It:

- orders modules, types and functions deterministically;
- preserves stable `SymbolId` and exact `TypeId` identities;
- emits one native implementation per script function;
- represents MIR basic blocks with an explicit block-state `switch`;
- preserves block-parameter parallel-copy semantics;
- emits explicit checked arithmetic, division, null and bounds operations;
- emits direct internal calls by stable function identity;
- routes unresolved calls through `BindingRegistry`;
- rejects malformed MIR, duplicate identities, unsupported instructions and programs without functions;
- computes a stable semantic content hash;
- emits a deterministic JSON manifest.

The generated code uses fixed-width values and runtime helpers instead of relying on C++ signed-overflow, evaluation-order, padding or aliasing behavior.

## Phase 5B — AOT runtime support

`AotRuntime` provides the runtime boundary consumed by generated source:

- `ProgramDescriptor`, type descriptors, enum metadata and function descriptors;
- `Program` and `ExecutionContext` host APIs;
- checked `int` and `long` operations;
- IEEE binary64 operations;
- object, array, string, enum, struct and native-handle values;
- exact runtime signature validation;
- managed allocation, null checks, bounds checks and write barriers;
- precise Shadow Stack frames for arguments, locals and registers;
- incremental GC work at the same instruction safepoints as the interpreter;
- instruction, recursion and GC work budgets;
- runtime errors, script stack traces, tracing and statistics;
- host binding resolution without reflective name lookup on the hot path.

`Program` validates all externally supplied descriptor tables before execution, including module names, type and field layouts, enum members, function signatures and source-map entries.

## Phase 5C — stable module discovery ABI

`RuntimeAbi.h` is a C11/C++ source-compatible public header. Generated modules expose one query symbol:

```c
RsStatusV1 rs_module_query_v1(
    const RsRuntimeApiV1* runtime_api,
    RsModuleExportsV1* out_exports);
```

The query performs size and ABI-version negotiation and returns:

- program name and semantic content hash;
- stable native function entries;
- backend kind and generated-module version;
- opaque backend data;
- the complete read-only `ProgramDescriptor`.

Every function entry uses an opaque execution-context pointer and catches runtime failures inside the AOT boundary. Invalid pointers, incompatible ABI versions and script runtime errors return explicit status codes.

The v1 profile is a **same-SDK native AOT profile**: generated C++ and `AotRuntime` are compiled together with a compatible RealScript SDK. The public discovery and entry table are stable C ABI, while distributing precompiled modules across unrelated C++ standard libraries or compiler ABIs remains outside this phase.

## Phase 5D — typed native thunks

`makeNativeThunk()` adapts ordinary C++ functions to `BindingRegistry` without building a reflective `Variant[]` layer in user code. The initial adapters support:

- `bool`;
- script `int` through `std::int32_t`;
- script `long` through `std::int64_t`;
- `double`;
- host strings;
- enum, struct, object and native-handle runtime values;
- `void` and typed return values.

Argument counts and types are checked, and C++ exceptions are caught and converted into structured runtime errors. Exact script type identity is validated by the generated call signature before entering the thunk.

## Phase 5E — source mapping and tooling

Generated source retains source correlation through:

- deterministic `#line` directives;
- source path, line and column entries in `ProgramDescriptor`;
- stable function identities in the manifest;
- content hashes suitable for cache keys and release manifests.

`rsaot` provides the command-line frontend:

```bash
rsaot \
  --output-dir build/generated/game \
  --program-name GameScripts \
  game.rs common.rs
```

It emits:

```text
realscript_aot_generated.h
realscript_aot_generated.cpp
realscript_aot_manifest.json
```

Unchanged files are not rewritten, preserving downstream compiler caches.

CMake projects can use the integrated helper:

```cmake
include(cmake/RealScriptAot.cmake)

realscript_add_aot_library(GameScriptsAot
    PROGRAM_NAME GameScripts
    SOURCES
        scripts/game.rs
        scripts/common.rs
)

target_link_libraries(game PRIVATE GameScriptsAot)
```

The helper generates, compiles and exposes the generated header and manifest as one normal native target.

## Semantic coverage

The initial AOT backend covers all MIR operations implemented through Phase 3E:

- parameters, locals, block parameters and structured control flow;
- direct and external function calls;
- `bool`, `int`, `long`, `double`, strings and null values;
- classes, constructors, methods, properties and fields;
- enums and value-semantic structs;
- arrays, element access, length and write barriers;
- object and array identity equality;
- numeric widening conversions;
- checked overflow, division-by-zero, null, bounds and allocation failures.

Phase 4 debug tables remain a bytecode/runtime tooling format. Native AOT source maps provide compiler-level source correlation, while native DAP stepping and optimized variable recovery are separate future work.

## Validation

Phase 5 validation includes:

- deterministic generation and semantic content hashes;
- rejection of malformed MIR and invalid descriptor tables;
- generated C++ compilation with strict warnings;
- a C11 compilation test for the public ABI header;
- direct invocation through `RsFunctionEntryV1`;
- typed native-thunk argument and exception tests;
- interpreter/AOT differential execution for successful values and failures;
- matching instruction accounting and script stack traces;
- checked integer/long overflow, division by zero, null references, array bounds and negative lengths;
- recursion and instruction budgets;
- object, array, struct, enum, string, long and double behavior;
- tracing, statistics and incremental GC visibility;
- complete Phase 1–5 regression coverage.

## Deliberate limits

Phase 5 does not yet add:

- inheritance, interfaces or virtual dispatch;
- generics and monomorphization;
- script exceptions, `try`/`catch`/`finally` or native unwind metadata;
- async state machines or coroutines;
- optimized native debugger variable locations;
- precompiled binary portability across unrelated C++ toolchains;
- shared-library loading/unloading lifecycle hooks;
- link-time whole-program optimization or profile-guided optimization;
- a separate LLVM backend.

These are language/runtime or Phase 6 optimization extensions. They do not prevent the current C++17 backend from serving as the release backend for the language subset implemented through Phase 5.
