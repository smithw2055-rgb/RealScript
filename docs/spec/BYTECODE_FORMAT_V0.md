# RealScript `.rsbc` Physical Format — Implemented Draft v0.3

- `bytecode_format_version`: 0.3
- implementation slices: Phase 2A–3C
- byte order: little-endian
- status: implemented draft; binary compatibility is not frozen

## 1. Scope

This document records the physical encoding currently produced and consumed by RealScript. Version 0.3 extends the typed-register and object-descriptor format with array and native-handle type tags, exact register TypeIds and array instruction metadata.

The format favors deterministic encoding and defensive validation over compactness. A decoder accepts exactly version 0.3; it never attempts layout inference for another version.

## 2. Scalars

- `u8`: one byte;
- `u16`, `u32`, `u64`: unsigned little-endian integers;
- `i64`: two's-complement bits encoded as `u64`;
- string: `u32 byte_length` plus UTF-8 bytes;
- register, block, table index and code offset: `u32`;
- absent register/index: `0xffffffff` where specified.

The decoder reads fields byte-by-byte and never casts input bytes to a host C++ structure.

## 3. Header and sections

```text
offset  size  field
0       4     magic = "RSBC"
4       2     major = 0
6       2     minor = 3
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
| 6 | `array` |
| 7 | `handle` |

`Error` is not a serializable storage type. An optional instruction element type uses `0xff` to represent “not present”.

A primitive type vector is `count:u32` followed by `count` tags.

An exact TypeId vector is `count:u32` followed by `count` `u64` values:

- object and array entries MUST be non-zero in function signatures and compiler-produced value registers;
- primitive, string, null and untyped handle entries MUST be zero;
- host handle identity is carried by the runtime `NativeHandle::typeId` and validated at the embedding boundary.

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

TypeIds are non-zero and unique within a module. Field indices are contiguous declaration-order indices. Object and array fields carry a canonical referenced type name. Primitive and handle fields do not.

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

The two parameter vectors MUST have equal length for compiler-produced modules. Exact object and array identities allow verifier and runtime boundary checks for imported and host calls.

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
  local_type_ids: TypeIdVector
  register_types: TypeVector
  register_type_ids: TypeIdVector
  code_offset: u32
  code_size: u32
```

Version 0.3 adds `local_type_ids` and `register_type_ids`. Each vector has the same count as its corresponding type vector and preserves exact object/array identity after MIR lowering.

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
    type_id: u64
  instruction_count: u32
  instructions[]
  terminator
```

Block zero is the entry block. Edge arguments initialize target block-parameter registers using parallel-copy semantics. Object and array block parameters carry exact non-zero TypeIds.

## 10. Instruction record

```text
opcode: u8
result_register: u32
operand_count: u32
operands[]: u32
index: u32
type_index: u32
element_type_or_ff: u8
element_type_id: u64
integer_immediate: i64
bool_immediate: u8
string_index_or_ffffffff: u32
```

`index` is a parameter, local, function-reference or field index according to the opcode. `type_index` selects a `TYPES` entry for object allocation, null checks and field operations.

`element_type` and `element_type_id` are populated only by array allocation and element operations. Primitive/handle element types require a zero element TypeId; object or array element types require the exact non-zero TypeId.

Implemented reference operations include:

| Operation | Meaning |
|---|---|
| `conv.null.object` | convert an untyped null literal to a typed null object |
| `conv.null.array` | convert an untyped null literal to a typed null array |
| `new.object` | allocate a selected class descriptor |
| `new.array` | allocate a fixed-length typed array |
| `check.notnull` | reject null and verify an object TypeId |
| `array.length` | read an array's immutable length |
| `load.field` / `store.field` | access a descriptor field |
| `load.element` / `store.element` | access a checked typed array element |

All previous typed-register primitive and control-flow operations remain available.

## 11. Terminators

The terminator record contains kind, condition/value registers, jump/true and false targets, and both edge-argument vectors. Kinds are jump, conditional branch, value return and void return.

## 12. Verifier requirements

Before execution, the verifier checks at least:

- version, required sections and complete section consumption;
- scalar, count, string-index and code-range bounds;
- unique type descriptors and function identities;
- exact object/array signature and register TypeIds;
- local/register storage categories;
- register definitions, dominance and block-argument types;
- instruction operand counts and opcode-specific fields;
- object descriptor and field indices;
- array receiver, index, result and element metadata;
- call signatures, returns and reachability.

Malformed bytecode is rejected before the interpreter can observe it.

## 13. Canonical behavior and compatibility

For the same verified module, encoding is byte-for-byte deterministic. The encoder writes no timestamps, native addresses or platform structure padding.

Version 0.3 is not binary compatible with 0.2 because function metadata and instruction records changed. Future incompatible changes must advance the version and remain fail-closed.
