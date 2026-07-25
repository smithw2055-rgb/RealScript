# RealScript Primitive Execution Semantics — Draft v0.1

## Entry validation

A runtime invocation MUST verify every loaded bytecode module before executing any instruction. Function identities MUST be unique across the loaded module set.

## Instruction order

Instructions execute in source order within a basic block. The block terminator executes after all block instructions. Each instruction and terminator consumes exactly one budget unit.

## Integer semantics

The current `int` profile is signed 32-bit. Addition, subtraction, multiplication, negation and the `INT_MIN / -1` case trap on overflow. Division and remainder trap on a zero divisor. `INT_MIN % -1` returns zero.

## Branch transfer

Edge arguments are read before any target block parameter is written. Transfer therefore has parallel-copy semantics.

## Calls

Arguments are evaluated before a callee frame is entered. A direct bytecode function is selected by `SymbolId`. Missing symbols may be resolved by the host callback; otherwise execution fails.

Instruction budgets are shared by callers and callees. Recursion limits count active script frames.

## Determinism

For verified bytecode, fixed module ordering, arguments, host resolver behavior and limits, execution MUST produce the same result or runtime error and the same instruction count.
