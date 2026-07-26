# C++17 AOT Backend and Native Module ABI — Implemented Draft v0.1

## 1. Scope

This document defines the implemented Phase 5 native AOT profile. It specifies deterministic MIR-to-C++17 generation, the AOT runtime support contract, native module discovery, typed host thunks, GC rooting, source mapping and cross-backend conformance.

Normative terms follow [README.md](README.md).

## 2. Version identifiers

The initial implementation uses independent versions:

```text
AOT manifest format       1
Runtime C ABI major       1
Runtime C ABI minor       0
Generated module version  1
```

Changing generated C++ layout does not by itself require changing `.rsbc`. Breaking a public C ABI field or semantic meaning MUST increase the Runtime ABI major version. A compatible appended field MAY increase the minor version.

## 3. Input requirements

The generator MUST consume verified Typed MIR. It MUST reject:

- an empty module set;
- a program without any function;
- malformed MIR;
- duplicate incompatible TypeIds;
- duplicate Function SymbolIds;
- unsupported MIR operations;
- an invalid public query symbol.

A module with frontend or semantic diagnostics MUST NOT produce an AOT artifact.

## 4. Deterministic output

For the same canonical MIR and generation options, the generator MUST produce byte-identical header, source and manifest content.

Canonical generation MUST NOT include:

- wall-clock timestamps;
- process IDs;
- native addresses;
- hash-table iteration order;
- platform structure padding.

The semantic content hash is computed from the ordered MIR program and program identity. It is suitable for build caches, release manifests and replay metadata.

## 5. Generated files

The standard output consists of:

```text
realscript_aot_generated.h
realscript_aot_generated.cpp
realscript_aot_manifest.json
```

The header declares the C++ `ProgramDescriptor` accessor and the C query entry. The source contains native function implementations and immutable metadata. The manifest identifies modules, functions, content hash, ABI version and source-map count.

Tools SHOULD avoid rewriting unchanged files so platform compiler caches remain effective.

## 6. Function lowering

Every script function maps to one generated native function with this internal shape:

```cpp
bool function(
    ExecutionContext& context,
    const runtime::Value* arguments,
    std::size_t argumentCount,
    runtime::Value& result);
```

The implementation MUST:

- establish argument, local and register Shadow Stack roots;
- consume one budget unit at every MIR instruction and terminator;
- preserve MIR block-parameter parallel-copy behavior;
- preserve source evaluation order;
- route observable checks through AOT runtime intrinsics;
- return failures through `ExecutionContext`, not escaping C++ exceptions.

Generated code MUST NOT invoke the bytecode interpreter.

## 7. Runtime semantics

AOT observable behavior MUST match the reference interpreter for the same verified MIR, including:

- return values and runtime value kinds;
- checked arithmetic overflow;
- division and remainder by zero;
- null-reference and array-bounds checks;
- exact class, array, enum, struct and handle TypeIds;
- object and array allocation;
- write barriers and precise root visibility;
- instruction and recursion budgets;
- runtime error codes and script stack traces;
- trace and statistics accounting.

The backend MUST NOT inherit C++ signed-overflow or uninitialized-memory behavior as script semantics.

## 8. Descriptor validation

A host-created `Program` MUST reject inconsistent descriptor data before execution. Validation includes:

- ABI version;
- non-empty program and module names;
- module-name uniqueness;
- pointer/count consistency;
- type identity and kind consistency;
- field order, names, storage types and exact TypeIds;
- enum-member names;
- function SymbolId and name uniqueness;
- parameter/return type validity;
- source-map function references and positive locations.

Descriptor memory is borrowed and MUST remain alive for the lifetime of every `Program` and `ExecutionContext` that references it.

## 9. GC and ownership

Generated frames MUST register arguments, locals and registers with the runtime Shadow Stack. Safepoints occur through instruction consumption. Managed allocation and stores MUST use `ManagedHeap` operations and write barriers.

The AOT runtime shares the Phase 3 ownership rules:

- `ObjectRef` values are heap-specific;
- cross-heap references are rejected;
- host-owned persistent values require explicit persistent roots;
- native handles retain host identity and generation checks;
- structs containing managed references are scanned recursively.

## 10. Native module C ABI

`RuntimeAbi.h` MUST compile as C11 and C++. The module exports a query function compatible with `RsModuleQueryV1`.

The host MUST initialize `size`, ABI major and ABI minor fields before querying. The module MUST reject:

- null arguments;
- structures smaller than the required v1 prefix;
- a different major version;
- a host minor version older than the module requirement.

On success, `RsModuleExportsV1` provides the content hash, native function table and opaque program descriptor.

## 11. Native function entries

Each `RsFunctionEntryV1` contains:

- structure size;
- `RS_BACKEND_V1_NATIVE_AOT`;
- stable Function SymbolId;
- generated-module version;
- an opaque native entry point;
- immutable backend descriptor data;
- optional debug metadata.

The entry point MUST validate null pointers and argument layout. It MUST convert script runtime failures to `RS_STATUS_V1_RUNTIME_ERROR`. It MUST NOT let a C++ exception cross the C ABI boundary.

## 12. Same-SDK profile

Phase 5 defines a same-SDK AOT profile. Generated source and `AotRuntime` are built with compatible headers, C++ standard library and compiler ABI. The public query and entry table are C ABI, but the generated implementation internally uses C++ runtime value types.

A future binary-distribution profile may replace this internal dependency with a complete C runtime function table. Such a profile requires a separate ABI version and does not alter Phase 5 source semantics.

## 13. Host bindings

Typed native thunks MUST:

- verify argument count;
- decode supported C++ values without dynamic name lookup;
- catch C++ exceptions;
- produce a structured runtime failure;
- encode the return value.

Generated call signatures MUST validate exact script TypeIds before invoking a thunk. Tools and reflection may use the existing generic binding path, but release hot paths SHOULD use typed thunks.

## 14. Source mapping

The generator SHOULD emit `#line` directives from MIR Sequence Points. It MUST also expose stable source-map entries containing function identity, generated line, source path, source line and source column.

Source mapping affects diagnostics and debugging metadata but MUST NOT change script semantics.

## 15. CMake integration

`realscript_add_aot_library()` defines the standard integrated build path. It MUST:

- depend on `rsaot` and all script sources;
- generate the standard files;
- compile generated C++ as C++17;
- link `RealScript::AotSupport`;
- expose the generated include directory;
- preserve strict warning settings;
- avoid rerunning when inputs are unchanged.

## 16. Conformance

An implementation claiming Phase 5 conformance MUST run successful and failing programs through the interpreter and AOT backends and compare:

- success state;
- runtime error code;
- return value;
- instruction count;
- script stack trace for failures.

The generated source MUST compile under at least GCC or Clang and MSVC-compatible C++17 modes before a release claim is made.

## 17. Deliberate omissions

This version does not specify virtual dispatch, generic specialization, exception unwinding, async state machines, dynamic shared-library lifecycle, optimized native variable locations, cross-toolchain binary compatibility or LLVM IR generation.
