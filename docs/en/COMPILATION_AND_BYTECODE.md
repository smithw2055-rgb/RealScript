# Compilation, MIR, and Bytecode

[Documentation Home](README.md) | [Architecture](ARCHITECTURE.md)

RealScript uses a staged compiler with verified Typed MIR as the semantic boundary between the source language and every executable backend.

## Compilation Stages

```text
SourceText
  -> Lexer
  -> Parser
  -> Syntax Tree
  -> Module Graph and Predeclaration
  -> Binder and Overload Resolution
  -> Flow Analysis
  -> Typed MIR Lowering
  -> MIR Verification
  -> Optional MIR Optimization
  -> Bytecode / C++17 AOT / Toolchain JIT
```

Diagnostics accumulate across stages so users can receive multiple useful errors from one compilation.

## Module Compilation

Files are grouped by their declared module name. Compilation performs:

1. module discovery;
2. import graph construction;
3. top-level symbol predeclaration;
4. body binding;
5. flow analysis;
6. MIR lowering and verification.

Predeclaration enables forward calls and direct recursion.

## Stable Symbols and Types

Functions use a canonical identity based on module, name, and parameter types:

```text
<module>::<name>(<parameter-types>)
```

The return type does not distinguish overloads.

`SymbolId` and `TypeId` values are stable hashes of canonical identities. They do not depend on source order, process addresses, or compiler allocation order.

Exact type IDs are preserved where primitive tags alone are insufficient, including classes, arrays, structs, enums, and native handles.

## Incremental Compilation

Each module records:

- a source fingerprint;
- a public-signature fingerprint;
- a dependency fingerprint.

If an implementation changes without changing public signatures, dependent module MIR may be reused from the previous `BuildSnapshot`. If a public signature changes, direct dependents are rebound.

The v0.1 baseline reuses semantic and MIR results. Persistent parser caches are future work.

## Typed MIR

A function contains explicit basic blocks. A block contains instructions followed by one terminator.

MIR values are typed and identified by `ValueId`. Locals use explicit slots. Control-flow edges carry block arguments into block parameters.

Representative MIR:

```text
block 0:
  %0:int = const.i32 21
  %1:int = call @Game.Math::twice(%0)
  ret %1
```

MIR makes observable behavior explicit:

- arithmetic operations and checked overflow;
- conversions;
- local loads and stores;
- object and array allocation;
- null and bounds checks;
- field and element operations;
- direct and host calls;
- branch targets and block arguments;
- returns and error-producing operations.

## MIR Verification

The verifier rejects malformed MIR before bytecode generation, AOT generation, optimization, or execution. Checks include:

- unique value definitions;
- dominance of uses by definitions;
- operand and result types;
- exact `TypeId` compatibility;
- local slot types;
- branch target existence;
- block argument count and type compatibility;
- call signatures;
- return types;
- object, field, array, and struct invariants;
- valid terminators.

A backend must only consume verified MIR.

## Optimization Levels

### O0

Preserves verified MIR structure as closely as possible.

### O1

Applies local and control-flow simplifications such as:

- constant folding;
- local-load folding where safe;
- constant conditional-branch folding;
- unreachable-block removal;
- value renumbering.

### O2

Runs bounded fixed-point optimization and conservative removal of unused pure values.

The optimizer verifies MIR before and after transformation. It preserves:

- checked overflow traps;
- divide-by-zero behavior;
- null and bounds checks;
- allocation and GC effects;
- external-call order;
- write barriers;
- debug mapping;
- execution budgets and observable errors.

## Typed Register Bytecode

The bytecode backend maps MIR values to typed registers and locals to explicit local slots. Block parameters become typed edge arguments.

Bytecode functions retain:

- stable function identity;
- parameter and return types;
- local and register types;
- exact runtime type IDs;
- basic blocks and terminators;
- source debug metadata.

## `.rsbc` 0.5

The current physical format is deterministic, little-endian, and section based. It contains six logical areas:

1. strings;
2. type descriptors;
3. function references;
4. function metadata;
5. code;
6. debug information.

Encoding avoids timestamps, native addresses, compiler padding, and platform-dependent structure layouts.

The format includes descriptors for classes, arrays, structs, enums, and native handles, plus exact local/register/block type metadata.

## Defensive Loading

Loading has two required stages:

### Physical Decode

The decoder validates:

- magic and version;
- section bounds;
- record counts;
- tag values;
- fixed-width integer reads;
- string and table indices;
- truncated or overlapping data.

### Semantic Verification

The bytecode verifier validates:

- control flow and register definitions;
- instruction operand types;
- calls and returns;
- descriptor identity and field layouts;
- exact type tables;
- null and bounds-check requirements;
- source ranges, sequence points, and local scopes.

No module may execute unless both stages succeed.

## Debug Information

`.rsbc` 0.5 stores:

- source-file identities and content fingerprints;
- line-start tables;
- function declaration and body ranges;
- instruction and terminator sequence points;
- parameters and local variables;
- local slots and exact types;
- lexical scopes.

Hand-authored in-memory bytecode used by tests may omit debug information, but compiler output normally includes it.

## Inspecting Output

```bash
rsc source.rs --mir
rsc source.rs --symbols
rsc source.rs --bytecode
rsc source.rs --emit-bytecode program.rsbc
rsc program.rsbc --disassemble
```

For backend details, continue with [AOT, JIT, and Performance](AOT_JIT_AND_PERFORMANCE.md).
