# Phase 1A — C++17 Language Foundation

## Status

Implemented as the first executable vertical slice after the Draft v0.1 specifications.

## Goal

Establish a dependency-free C++17 frontend that turns a small, well-defined RealScript source file into a parsed syntax tree, a semantically checked bound tree, and printable Typed MIR.

This slice intentionally proves architecture boundaries before expanding language coverage.

## Delivered surface

### Text and diagnostics

- immutable `SourceText` with CRLF/LF line mapping;
- half-open `TextSpan` source ranges;
- stable diagnostic codes and source formatting;
- error accumulation instead of fail-fast parsing.

### Lexer

- identifiers and reserved keywords;
- integer, floating-point and string tokens;
- punctuation and precedence-bearing operators;
- line and block comments;
- malformed character, string and comment diagnostics.

### Parser

The initial grammar supports:

```text
compilation-unit  := module-declaration? import-declaration* function-declaration* EOF
module-declaration := "module" qualified-name ";"
import-declaration := "import" qualified-name ";"
function-declaration := type identifier "(" parameter-list? ")" block
statement := block | return-statement | variable-declaration | expression-statement
expression := literal | name | call | parenthesized | unary | binary
```

Functions deliberately use the C#-style `return-type name(parameters)` form.

### Semantic binding

The Phase 1A semantic profile implements:

- `void`, `bool`, `int` and `string` type descriptors;
- function parameters and lexical local scopes;
- local declaration and duplicate-name validation;
- name resolution;
- integer arithmetic and comparison;
- Boolean equality and logical operators;
- return-type validation;
- explicit diagnostics for parsed-but-not-yet-bound features.

The lexer recognizes the wider primitive keyword set from the language specification, but types outside this implementation profile receive a diagnostic instead of silently changing semantics.

### Typed MIR

The initial MIR is a single-basic-block SSA-like form with:

- typed parameter values;
- integer, Boolean and string constants;
- integer arithmetic and comparisons;
- Boolean logical operations;
- local values represented by SSA value IDs;
- explicit value and void returns;
- source spans retained on every instruction.

Division and remainder are named as checked operations so later VM and AOT implementations cannot inherit C++ undefined behavior.

## Tooling

`rsc` provides three initial modes:

```bash
rsc sample.rs           # parse and type-check
rsc sample.rs --tokens  # print tokens
rsc sample.rs --mir     # print Typed MIR
```

## Build and test

```bash
cmake -S . -B build -DREALSCRIPT_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The tests cover source mapping, tokenization, invalid characters, module/import parsing, operator precedence, name diagnostics and MIR lowering.

## Explicit limitations

Phase 1A does not yet implement:

- classes, structs, interfaces or enums;
- fields, member access or assignment;
- function-call binding;
- overload resolution;
- full numeric conversion and promotion;
- control-flow statements;
- multi-block MIR and Phi nodes;
- definite-return control-flow analysis;
- bytecode generation or execution.

These limitations are diagnosed or structurally isolated; they are not hidden behind placeholder runtime behavior.

## Exit criteria

- the project builds as standard C++17 without external dependencies;
- all conformance tests pass;
- valid Phase 1A input reaches printable Typed MIR;
- invalid source produces stable diagnostics without crashes;
- parser, binder and MIR remain separate modules;
- no execution backend reads the syntax tree directly.

## Next slice

Phase 1B should add control-flow binding and multi-block MIR:

1. assignment and mutable locals;
2. `if`/`else` and `while`;
3. control-flow graph construction;
4. definite assignment and all-path return analysis;
5. MIR branches, merge blocks and Phi-like block parameters;
6. structural MIR verifier;
7. snapshot-based conformance fixtures for diagnostics and MIR text.
