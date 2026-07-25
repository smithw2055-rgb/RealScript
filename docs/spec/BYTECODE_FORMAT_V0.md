# RealScript `.rsbc` Physical Format — Implemented Draft v0.2

- `bytecode_format_version`: 0.2
- implementation slices: Phase 2A–3B
- byte order: little-endian
- status: implemented draft; binary compatibility is not frozen

## 1. Scope

This document records the physical encoding currently produced and consumed by RealScript. Version 0.2 extends the original typed-register format with class type descriptors, exact object TypeIds in function signatures, and object-field instructions.

The format favors deterministic encoding and defensive validation over compactness.

## 2. Scalars

- `u8`: one byte;
- `u16`, `u32`, `u64`: unsigned little-endian integers;
- `i64`: two's-complement bits encoded as `u64`;
- string: `u32 byte_length` plus UTF-8 bytes;
- register, block, table index and code offset: `u32`;
- absent register/index: `0xffffffff` where specified.

The decoder reads fields byte-by-byte and never casts the input to a host C++ structure.

## 3. Header and sections

```text
offset  size  field
0       4     magic = "RSBC"
4       2     major = 0
6       2     minor = 2
8       4     flags = 0
12      4     section_count = 5
16      ...   section directory
```

Each directory entry contains `kind:u32`, `offset:u32`, `size:u32`.

| Kind | Section | Purpose |
|---:|---|---|
| 1 | `STRINGS` | module/type/function/field names and string constants |
| 2 | `TYPES` | class TypeIds and ordered field layouts |
| 3 | `FUNCTION_REFERENCES` | direct-call signatures and stable SymbolIds |
| 4 | `FUNCTIONS` | function, local, register and code-range metadata |
| 5 | `CODE` | blocks, instructions and terminators |

Missing, duplicate, overlapping or out-of-range sections are rejected.

## 4. Type tags

| Tag | Type |
|---:|---|
| 0 | `void` |
| 1 | `bool` |
| 2 | `int` |
| 3 | `string` |
| 4 | `object` |
| 5 | `null` |

`Error` is not serializable. A primitive type vector is `count:u32` followed by `count` tags.

An exact signature TypeId vector is `count:u32` followed by `count` `u64` values. Primitive entries MUST be zero; object entries MUST be non-zero.

## 5. String section

```text
string_count: u32
repeat string_count:
  length: u32
  utf8[length]
```

Index zero is the module name. Strings are interned in deterministic first-use order.

## 6. Type descriptor section

```text
type_count: u32
repeat type_count:
  type_id: u64
  module_name_string: u32
  type_name_string: u32
  field_count: u32
  repeat field_count:
    field_name_string: u32
    field_type: u8
    referenced_type_name_string_or_ffffffff: u32
    field_index: u32
```

TypeIds are non-zero and unique within a module. Field indices are contiguous declaration-order indices. Object fields carry a canonical referenced type name.

## 7. Function-reference section

```text
reference_count: u32
repeat reference_count:
  symbol_id: u64
  name_string: u32
  return_type: u8
  return_type_id: u64
  parameter_types: TypeVector
  parameter_type_ids: TypeIdVector
```

The two parameter vectors MUST have equal length for compiler-produced modules. Exact object identities allow runtime boundary checks for imported and host calls.

## 8. Function metadata section

```text
function_count: u32
repeat function_count:
  symbol_id: u64
  name_string: u32
  return_type: u8
  return_type_id: u64
  parameter_types: TypeVector
  parameter_type_ids: TypeIdVector
  local_types: TypeVector
  register_types: TypeVector
  code_offset: u32
  code_size: u32
```

Code offsets are relative to the `CODE` section. Function code ranges must be in bounds and non-overlapping.

## 9. Function code

```text
block_count: u32
repeat block_count:
  block_id: u32
  parameter_count: u32
  repeat parameter_count:
    target_register: u32
    type: u8
  instruction_count: u32
  instructions[]
  terminator
```

Block zero is the entry block. Edge arguments initialize target block-parameter registers using parallel-copy semantics.

## 10. Instruction record

```text
opcode: u8
result_register: u32
operand_count: u32
operands[]: u32
index: u32
type_index: u32
integer_immediate: i64
bool_immediate: u8
string_index: u32
```

`index` is a parameter/local/reference/field index according to opcode. `type_index` selects the `TYPES` entry for object allocation, null checks and field operations.

Implemented object operations:

| Operation | Meaning |
|---|---|
| `conv.null.object` | convert the untyped null literal to a typed null object value |
| `new.object` | allocate the selected descriptor with default fields |
| `check.notnull` | reject null and verify the runtime TypeId |
| `load.field` | read a descriptor field |
| `store.field` | write a descriptor field and execute the GC barrier when required |

All previous typed-register primitive operations remain unchanged.

## 11. Terminators

The terminator record contains kind, condition/value registers, true/jump and false targets, and both edge-argument vectors. Kinds are jump, conditional branch, value return and void return.

## 12. Canonical and defensive behavior

For the same verified module, encoding must be byte-for-byte deterministic. The decoder validates section boundaries, counts before allocation, tags, type descriptors, signature TypeIds, code ranges, instruction fields and complete section consumption before the interpreter can run the module.
