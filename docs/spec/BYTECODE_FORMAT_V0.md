# RealScript `.rsbc` Physical Format — Implemented Draft v0.5

- `bytecode_format_version`: 0.5
- implementation slices: Phase 2A–4
- byte order: little-endian
- status: implemented draft; binary compatibility is not frozen

## 1. Scope

This document records the physical encoding currently produced and consumed by RealScript. Version 0.5 retains the Phase 3D–3E member/value format and adds deterministic source files, line maps, sequence points, local-variable metadata and lexical scopes for debugger and tooling integration.

The encoder is deterministic and the decoder accepts exactly version 0.5. It never infers the layout of another version.

## 2. Scalars

- `u8`, `u16`, `u32`, `u64`: unsigned little-endian integers;
- `i64`: two's-complement bits encoded as `u64`;
- `f64`: IEEE 754 binary64 bits encoded through byte copying, not native-structure casting;
- string: `u32 byte_length` plus UTF-8 bytes;
- register, block, table index and code offset: `u32`;
- absent register/index: `0xffffffff` where specified.

The decoder reads fields byte-by-byte and never casts input bytes to a host C++ structure.

## 3. Header and sections

```text
offset  size  field
0       4     magic = "RSBC"
4       2     major = 0
6       2     minor = 5
8       4     flags = 0
12      4     section_count = 6
16      ...   section directory
```

Each directory entry contains `kind:u32`, `offset:u32`, `size:u32`.

| Kind | Section | Purpose |
|---:|---|---|
| 1 | `STRINGS` | module/type/function/member/field names and string constants |
| 2 | `TYPES` | class, struct and enum descriptors |
| 3 | `FUNCTION_REFERENCES` | direct-call signatures and stable SymbolIds |
| 4 | `FUNCTIONS` | function, local, register and code-range metadata |
| 5 | `CODE` | blocks, instructions and terminators |
| 6 | `DEBUG` | source files, line maps, sequence points, parameters, locals and scopes |

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
| 8 | `long` |
| 9 | `double` |
| 10 | `struct` |
| 11 | `enum` |

`Error` is not serializable. An optional instruction element type uses `0xff` for “not present”.

A TypeId is required for `object`, `array`, `struct` and `enum`. Primitive, string, null and untyped handle entries require zero. Type and TypeId vectors must have identical counts.

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
  kind: u8                 # class=0, struct=1, enum=2
  module_name_string: u32
  type_name_string: u32
  field_count: u32
  repeat field_count:
    field_name_string: u32
    field_type: u8
    referenced_type_name_string_or_ffffffff: u32
    field_index: u32
    synthetic: u8
  enum_member_count: u32
  repeat enum_member_count:
    member_name_string: u32
    member_value: i64
```

TypeIds are non-zero and unique. Field indices are contiguous declaration-order indices. Exact fields carry canonical referenced type names. Auto-property backing fields set `synthetic=1`. Enum descriptors carry member names and values; class and struct descriptors carry no enum members.

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

Free functions, static methods, instance methods, constructors and property accessors use the same reference format. An instance member includes exact owner type as parameter zero.

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

Every exact type in parameters, locals and registers has a non-zero TypeId. Code ranges are relative to the `CODE` section, in bounds and non-overlapping.

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

Block zero is the entry. Edge arguments initialize block-parameter registers using parallel-copy semantics. Exact block parameters require exact TypeIds.

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
double_immediate: f64
bool_immediate: u8
string_index_or_ffffffff: u32
```

`index` is a parameter, local, function-reference, field or element-related index according to the opcode. `type_index` selects a descriptor for object/struct allocation and field checks.

Implemented Phase 3D–3E additions include:

| Operation family | Meaning |
|---|---|
| `conv.int.long`, `conv.int.double`, `conv.long.double` | numeric widening |
| `const.f64` | binary64 literal |
| `new.struct` | create recursively default-initialized struct value |
| `load.struct.field`, `store.struct.field` | immutable/value-replacement struct access |
| `*.long` | checked signed 64-bit arithmetic and comparisons |
| `*.double` | IEEE binary64 arithmetic and comparisons |
| `call` | free or owner-qualified direct member invocation |

Object, array and Phase 2A–3C primitive/control-flow operations remain available.

## 11. Terminators

The terminator record contains kind, condition/value registers, jump/true and false targets, and both edge-argument vectors. Kinds are jump, conditional branch, value return and void return.


## 12. Debug section

```text
source_count: u32
repeat source_count:
  source_file_id: u32
  path_string: u32
  content_hash: u64
  line_start_count: u32
  line_starts[]: u32

function_debug_count: u32
repeat function_debug_count:
  function_symbol_id: u64
  source_file_id: u32
  declaration_range: SourceRange
  body_range: SourceRange
  sequence_point_count: u32
  sequence_points[]
  local_count: u32
  locals[]
```

A `SourceRange` stores SourceFileId, byte span and zero-based start/end line positions. A sequence point stores block, instruction position, terminator flag and SourceRange. A local stores name, slot, type, exact TypeId, parameter flag, declaration range and lexical scope range.

SourceFileIds are module-local. Line starts are serialized so a decoded module does not need source contents to bind breakpoints or render locations.

## 13. Verifier requirements

Before execution, the verifier checks at least:

- exact version and complete section consumption;
- scalar, count, string-index and code-range bounds;
- unique type descriptors, legal descriptor kinds and field layouts;
- function/reference identity and exact signature agreement;
- complete TypeId vectors for parameters, locals, registers and blocks;
- register definitions, dominance and edge-argument types;
- opcode operand counts and result categories;
- object/struct descriptor and field indices;
- array receiver, index and element metadata;
- numeric conversion and typed operation categories;
- direct-call arguments/results, returns and reachability;
- source-file identity, line maps, sequence-point targets and duplicate locations;
- local slots, exact local TypeIds, parameter flags and source/scope references.

Malformed bytecode is rejected before execution.

## 14. Canonical behavior and compatibility

For the same verified module, encoding is byte-for-byte deterministic. The encoder writes no timestamps, native addresses or platform structure padding.

Version 0.5 is not binary compatible with 0.4 because the physical section directory and debug tables changed. The 0.4 member/value instruction and descriptor model is retained. Future incompatible changes must advance the version and remain fail-closed.
