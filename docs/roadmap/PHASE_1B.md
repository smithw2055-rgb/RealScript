# Phase 1B — Control Flow and Multi-Block MIR

## Status

Implemented as the second executable frontend slice, stacked on Phase 1A.

## Goal

Extend the initial expression-and-return frontend into a real structured control-flow compiler while preserving identical semantics for future bytecode and C++17 AOT backends.

The slice establishes:

- mutable locals and assignment;
- `if` / `else` and `while`;
- structured definite-assignment analysis;
- all-path return analysis;
- multi-block Typed MIR;
- short-circuit Boolean lowering;
- block parameters for merge values;
- a structural and type-aware MIR verifier;
- snapshot-based conformance fixtures.

## Language surface

### Assignment

Assignment is a right-associative expression with the lowest precedence:

```csharp
int value;
value = 1;
return value = value + 1;
```

Assignments are type checked and may target locals or parameters. An assignment evaluates its right-hand side first, stores the value, and then produces that value as the result of the expression.

### Conditional statements

```csharp
if (condition)
{
    value = 1;
}
else
{
    value = 2;
}
```

The condition must be `bool`. Each embedded branch owns a lexical scope, including branches written without braces.

### Loops

```csharp
while (value > 0)
{
    value = value - 1;
}
```

The condition must be `bool`. `break`, `continue`, and loop labels remain outside this slice.

## Definite assignment

Locals may now be declared without an initializer:

```csharp
int result;
```

A flow analysis pass tracks assignment state through structured statements.

Rules implemented in this slice:

- parameters are definitely assigned at function entry;
- a declaration with an initializer assigns the local after the initializer is evaluated;
- a declaration without an initializer leaves the local unassigned;
- assignment marks the target assigned after evaluating the right-hand side;
- both continuing branches of an `if` must assign a value for it to be definitely assigned after the merge;
- assignments performed only in the right side of `&&` or `||` are not definitely assigned afterward;
- loop-body assignments are not assumed after a loop because the body may execute zero times;
- reading an unassigned local produces `RS2300`.

Example rejected by the compiler:

```csharp
int choose(bool flag)
{
    int value;
    if (flag)
        value = 1;
    return value; // RS2300
}
```

## All-path return analysis

The old “saw at least one return” check has been replaced by reachability analysis.

A non-void function is valid when its endpoint is unreachable, including:

- both branches of an `if` return;
- a literal `while (true)` loop has no `break` capability;
- prior statements already terminate the path.

A reachable non-void endpoint produces `RS2001`.

## MIR state model

Phase 1A represented each variable as one SSA value. That model cannot correctly represent mutation across branches and loop back-edges without SSA construction.

Phase 1B therefore uses two complementary mechanisms:

1. **Explicit local slots** for mutable source variables:
   - `load.local`;
   - `store.local`;
   - a typed local table per function.
2. **Block parameters** for values created by control-flow expressions, currently short-circuit Boolean merges.

This is intentional. It keeps lowering direct and semantically reliable. A later optimization pass can promote eligible local slots to SSA values using dominance frontiers without changing the language frontend or runtime semantics.

## Basic blocks and terminators

Each MIR block contains:

- zero or more typed block parameters;
- zero or more typed instructions;
- exactly one terminator.

Supported terminators:

```text
jmp bbN(args...)
br %condition, bbTrue(args...), bbFalse(args...)
ret %value
ret.void
```

`if` creates branch and merge blocks. `while` creates condition, body, and exit blocks. A literal `while (true)` has no synthetic exit block.

## Short-circuit lowering

`&&` and `||` are no longer rejected. They lower to control flow rather than eager Boolean instructions.

Conceptually:

```text
%left = ...
br %left, bb_rhs, bb_merge(false)

bb_rhs:
  %right = ...
  jmp bb_merge(%right)

bb_merge(%result:bool):
  ...
```

This preserves source evaluation order and prevents side effects in the right operand when the expression short-circuits.

## MIR verifier

`verifyModule` rejects malformed or unsafe MIR before it can reach an execution backend.

Checks include:

- entry block presence and unique block IDs;
- unique value definitions;
- valid instruction operand counts;
- valid local indices and local types;
- valid branch targets;
- branch argument count and type matching block parameters;
- Boolean branch conditions;
- return type compatibility;
- reachable blocks;
- use-before-definition inside a block;
- cross-block dominance of value definitions;
- arithmetic, comparison, equality, and constant result types;
- a terminator on every block.

The verifier also handles malformed operand arrays without indexing invalid memory.

## Tooling

The existing CLI automatically verifies generated MIR before printing it:

```bash
rsc sample.rs --mir
```

Invalid source is rejected during parsing, binding, or flow analysis. Invalid generated MIR is rejected with `RS30xx` diagnostics.

## Tests

The frontend test executable now covers:

- source line mapping;
- control-flow keywords;
- parsing assignment, `if`, `else`, and `while`;
- partial-branch definite-assignment failure;
- both-branch definite-assignment success;
- all-path return success and failure;
- loop CFG lowering;
- short-circuit block parameters;
- short-circuit definite-assignment behavior;
- literal infinite-loop endpoint analysis;
- missing MIR targets;
- malformed MIR operand arrays;
- a source-to-MIR snapshot fixture.

Validation performed locally with:

- GCC 14.2, C++17, warnings-as-errors;
- Clang, C++17, warnings-as-errors;
- AddressSanitizer and UndefinedBehaviorSanitizer.

## Explicit limitations

Phase 1B still does not implement:

- function-call binding or overload resolution;
- module-to-module symbol resolution;
- `break`, `continue`, `for`, `foreach`, or `switch`;
- user-defined types;
- complete numeric types and conversions;
- exceptions;
- local-slot-to-SSA promotion;
- bytecode generation or execution.

## Exit criteria

- assignment and mutable locals have defined evaluation semantics;
- definite-assignment results are correct across branches, loops, and short-circuit expressions;
- non-void return validation uses path reachability;
- valid control flow lowers to terminated multi-block MIR;
- short-circuit logic uses block parameters rather than eager operators;
- generated MIR passes the structural verifier;
- malformed MIR is rejected without crashes;
- snapshot fixtures make MIR format changes visible in review.

## Next slice

Phase 1C should establish callable and incremental program structure:

1. predeclare function signatures before binding bodies;
2. bind direct function calls and overload candidates;
3. define implicit conversion ranking for the implemented primitive profile;
4. build module symbol tables and resolve imports;
5. separate per-file syntax trees from a compilation object;
6. introduce stable symbol IDs and dependency fingerprints;
7. add incremental rebind tests and multi-file conformance fixtures.
