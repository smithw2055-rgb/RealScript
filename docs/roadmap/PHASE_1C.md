# Phase 1C — Calls, Modules and Incremental Compilation

## Status

Implemented as the third executable compiler slice after the Draft v0.1 specifications.

## Goal

Move RealScript from isolated single-file functions to a small but coherent compilation model:

- functions are declared before bodies are bound;
- calls can target later declarations and imported modules;
- overload selection is deterministic and diagnosable;
- multiple files can contribute to one module;
- function identity is stable across source ordering and processes;
- unchanged semantic/MIR module products can be reused safely.

The slice deliberately does not introduce objects, generics or the full numeric conversion lattice.

## Compilation pipeline

```text
SourceFile[]
    │
    ▼
Parse all files
    │
    ▼
Group files by module
    │
    ▼
Predeclare function signatures
    │
    ▼
Resolve imports and visible overload sets
    │
    ▼
Bind bodies and run flow analysis
    │
    ▼
Lower and verify MIR
    │
    ▼
BuildSnapshot for the next compilation
```

Binding errors stop the backend pipeline for that module. An invalid call is never passed to the MIR Lowerer as a placeholder expression.

## Function symbols

`FunctionSymbol` now contains:

- stable `SymbolId`;
- declaring module;
- source name;
- return type;
- ordered parameter symbols.

The canonical overload key is:

```text
<module>::<function>(<parameter-types>)
```

The return type is excluded because RealScript does not allow overloading by return type alone.

The current `SymbolId` is a stable 64-bit FNV-1a hash of the canonical key. It is independent of declaration order and runtime addresses. Collision policy can be strengthened before binary format freeze by storing the canonical key alongside serialized IDs.

## Call binding

The parser already produced `CallExpressionSyntax`. Phase 1C adds:

- module-wide function predeclaration;
- forward calls;
- direct recursion support through predeclared symbols;
- visible overload sets from the current module and direct imports;
- argument binding before candidate selection;
- deterministic conversion ranking;
- no-applicable-overload and ambiguous-call diagnostics;
- `BoundCallExpression` and `BoundConversionExpression`.

### Conversion profile

The initial ranking is intentionally small:

| Conversion | Rank |
|---|---:|
| identity | 0 |
| `null` to `string` | 1 |
| all others | unavailable |

This establishes the overload-resolution architecture without freezing numeric promotion before the remaining primitive types are implemented.

## Module model

Multiple source files can declare the same module. Files are ordered by path before declarations are collected so diagnostics and build products remain reproducible.

Imports expose the imported module's function declarations as unqualified candidates. Phase 1C does not yet include:

- private/public access modifiers;
- aliases;
- selective imports;
- qualified call syntax;
- re-export rules;
- package manifests or automatic file discovery.

All files must currently be supplied to `Compilation` or `rsc` explicitly.

## MIR changes

### Calls

The new `call` instruction records:

- callee `SymbolId`;
- canonical display name;
- parameter types;
- ordered argument values;
- result type;
- optional result value for non-void functions.

Example:

```text
%2:int = call @Game.Math::twice[0x...](%1)
```

The verifier rejects zero symbol IDs, argument-count mismatches, argument-type mismatches, void calls that define values, and value-returning calls without results.

### Conversions

`null → string` is explicit in MIR:

```text
%1:null = const.null
%2:string = conv.null.string %1
```

Backends do not need to rediscover overload conversion semantics.

## Incremental model

Each module produces:

- `sourceFingerprint` from sorted source paths and contents;
- `publicFingerprint` from sorted canonical public signatures;
- `dependencyFingerprint` from direct import names and their public fingerprints;
- verified MIR cached in a `BuildSnapshot`.

A module is reused when all three fingerprints match the previous snapshot.

Consequences:

- implementation-only edits rebuild the changed module;
- dependents remain reusable when its public surface is unchanged;
- public-signature edits invalidate direct dependents;
- removed or added source files change the module source fingerprint;
- reordered declarations do not change `SymbolId` or the public fingerprint.

Phase 1C still reparses source files on each build. Incremental syntax trees and persistent on-disk caches remain future work.

## Diagnostics

New diagnostics include:

- `RS2100`: undefined function;
- `RS2107`: no applicable overload;
- `RS2108`: ambiguous call;
- `RS4001`: missing imported module;
- `RS4002`: duplicate function overload;
- `RS3030`–`RS3035`: malformed conversion or call MIR.

## CLI

`rsc` now accepts multiple files:

```bash
rsc lib.rs app.rs
rsc lib.rs app.rs --mir
rsc lib.rs app.rs --symbols
```

`--tokens` remains a single-file operation.

## Tests

Phase 1C adds 11 cases while retaining the Phase 1B suite:

- direct and forward calls;
- exact overload selection;
- `null → string` conversion;
- no applicable overload;
- imported function calls;
- missing imports;
- ambiguity across imports;
- stable IDs across declaration reordering;
- multiple files in one module;
- implementation reuse and public-surface invalidation;
- void calls as expression statements.

Validation configurations:

- GCC 14.2, C++17, warnings-as-errors;
- Clang 17, C++17, warnings-as-errors;
- AddressSanitizer;
- UndefinedBehaviorSanitizer.

## Explicit limitations

Phase 1C does not implement:

- member or virtual calls;
- overloads differentiated only by return type;
- user-defined conversions;
- full integer and floating-point promotion;
- generic function inference;
- visibility or package boundaries;
- transitive re-export semantics;
- incremental parse-tree reuse;
- persistent cache serialization;
- bytecode execution.

## Exit criteria

- calls to later declarations bind successfully;
- current-module and direct-import overload sets are deterministic;
- bad and ambiguous calls produce diagnostics without entering MIR lowering;
- stable IDs are independent of source order;
- same-module files are compiled as one semantic module;
- implementation-only dependency edits do not invalidate dependents;
- public-signature edits invalidate direct dependents;
- all emitted call and conversion MIR passes structural verification;
- Phase 1B MIR snapshots remain compatible.

## Next slice

Phase 2A should establish the executable bytecode contract:

1. typed register bytecode opcodes;
2. function and block layout;
3. constants, locals, branches, calls and returns;
4. deterministic encoder and decoder;
5. textual disassembler;
6. bytecode verifier derived from MIR types;
7. MIR-to-bytecode lowering;
8. round-trip and malformed-module fixtures.
