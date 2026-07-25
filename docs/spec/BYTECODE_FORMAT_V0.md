# RealScript `.rsbc` Physical Format — Draft v0.1

- `bytecode_format_version`: 0.1
- implementation slice: Phase 2A
- byte order: little-endian
- status: implemented draft; binary compatibility is not frozen

## 1. Scope

This document records the exact physical encoding produced and consumed by the Phase 2A implementation. It refines the logical requirements in [BYTECODE_AND_ABI.md](BYTECODE_AND_ABI.md) for the currently implemented primitive language profile.

The v0.1 container intentionally favors straightforward validation and deterministic snapshots over compact size. A future format revision may introduce compressed operands, checksums, signatures, debug sections and GC maps, but must preserve the verified logical instruction semantics.

## 2. Scalar encoding

- `u8`: one byte;
- `u16`, `u32`, `u64`: unsigned little-endian integers;
- `i64`: two's-complement bit pattern encoded as `u64`;
- strings: `u32 byte_length` followed by UTF-8 bytes;
- registers, block IDs, table indices and code offsets: `u32`;
- an absent result register is `0xffffffff`.

The decoder reads fields byte-by-byte. It never casts an unaligned input buffer to a host C++ structure and never stores host pointers or `size_t` values in the file.

## 3. Header

```text
offset  size  field
0       4     magic = "RSBC"
4       2     bytecode_major
6       2     bytecode_minor
8       4     flags, currently zero
12      4     section_count, currently four
16      ...   section directory
```

Phase 2A accepts exactly version `0.1` and four required sections. Unsupported versions are rejected with `RS5001`.

## 4. Section directory

Each entry is 12 bytes:

```text
kind:   u32
offset: u32
size:   u32
```

Current section kinds:

| Kind | Name | Purpose |
|---:|---|---|
| 1 | `STRINGS` | module name, function names and string constants |
| 2 | `FUNCTION_REFERENCES` | direct-call signatures and stable SymbolIds |
| 3 | `FUNCTIONS` | function type/register metadata and code ranges |
| 4 | `CODE` | blocks, instructions and terminators |

The loader rejects missing, duplicate, out-of-range, header-overlapping or mutually overlapping sections.

## 5. Primitive type tags

| Tag | Type |
|---:|---|
| 0 | `void` |
| 1 | `bool` |
| 2 | `int` |
| 3 | `string` |
| 4 | `null` |
| 5 | `object` |

`object` is the managed-reference carrier introduced by Phase 3A. `Error` is never serializable. `void` is valid only where a signature permits it; value registers and parameters cannot have type `void`.

A type vector is encoded as:

```text
count: u32
types[count]: u8
```

## 6. String section

```text
string_count: u32
repeat string_count:
  byte_length: u32
  utf8_bytes[byte_length]
```

Index zero is the module name. The encoder deduplicates strings in deterministic first-use order. The current set includes:

- module name;
- function-reference names;
- function names;
- string literals.

## 7. Function-reference section

```text
reference_count: u32
repeat reference_count:
  symbol_id: u64
  name_string_index: u32
  return_type: u8
  parameter_types: TypeVector
```

A call instruction references this table by `u32` index. The entry carries the full signature needed by the verifier, including references to functions in another `.rsbc` module.

## 8. Function metadata section

```text
function_count: u32
repeat function_count:
  symbol_id: u64
  name_string_index: u32
  return_type: u8
  parameter_types: TypeVector
  local_types: TypeVector
  register_types: TypeVector
  code_offset: u32
  code_size: u32
```

`code_offset` is relative to the start of the `CODE` section. Function code ranges must be in bounds and non-overlapping.

Every register has one static type in `register_types`. Phase 2A preserves MIR value identity as register identity, so registers remain SSA-like except that mutable source variables continue to use explicit local slots.

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
  instructions[instruction_count]

  terminator
```

Block zero is the entry block. Block parameters preserve MIR edge values. A taken edge assigns its argument values to the target block's parameter registers.

## 10. Instruction record

All v0.1 instructions use one generic, easily validated record:

```text
opcode: u8
result_register: u32
operand_count: u32
operands[operand_count]: u32
index: u32
integer_immediate: i64
bool_immediate: u8
string_index: u32
```

Unused generic fields must contain their neutral value. `string_index` must be `0xffffffff` except for `const.string`.

The `index` field means:

- parameter index for `param`;
- local index for `load.local` and `store.local`;
- function-reference index for `call`;
- zero for other current instructions.

This baseline is larger than a production encoding, but makes instruction boundaries independent of opcode-specific host structures and gives the decoder one consistent bounds-checking path.

## 11. Implemented opcodes

| Logical operation | Disassembly |
|---|---|
| parameter load | `param` |
| constants | `const.i32`, `const.bool`, `const.string`, `const.null` |
| mutable locals | `load.local`, `store.local` |
| conversion | `conv.null.string` |
| direct call | `call` |
| unary operations | `neg.i32`, `not.bool` |
| arithmetic | `add.i32`, `sub.i32`, `mul.i32`, `div.checked.i32`, `rem.checked.i32` |
| equality | `eq`, `ne` |
| integer comparison | `lt.i32`, `le.i32`, `gt.i32`, `ge.i32` |

Division and remainder retain checked MIR semantics; the bytecode does not inherit C++ undefined behavior.

## 12. Terminator record

```text
kind: u8
condition_register: u32
value_register: u32
true_or_jump_target: u32
false_target: u32
true_argument_count: u32
true_arguments[]: u32
false_argument_count: u32
false_arguments[]: u32
```

Kinds:

| Tag | Meaning |
|---:|---|
| 0 | invalid / missing terminator |
| 1 | jump |
| 2 | conditional branch |
| 3 | return value |
| 4 | return void |

Unused register fields use `0xffffffff`.

## 13. Canonical encoding

For the same verified bytecode module, the encoder must produce identical bytes. Canonical behavior includes:

- stable source/module declaration order from the compiler;
- stable function and reference order;
- first-use string interning;
- fixed little-endian integer representation;
- no timestamps, process addresses or platform-dependent padding;
- no unspecified C++ structure serialization.

The conformance suite verifies `encode(decode(encode(module)))` byte-for-byte equality and includes full hexadecimal and disassembly snapshots.

## 14. Decoder safety

Before constructing executable bytecode, the decoder rejects:

- bad magic or unsupported version;
- missing, duplicate, overlapping or out-of-range sections;
- truncated strings, vectors, metadata, code and terminators;
- invalid type, opcode or terminator tags;
- invalid string indices;
- out-of-range or overlapping function code ranges;
- impossible collection counts before allocation.

The decoder only reconstructs the typed bytecode structure. The bytecode verifier must still succeed before execution.

## 15. Verifier requirements

The Phase 2A verifier checks at least:

- valid module and function identities;
- valid static types for parameters, locals and registers;
- unique register definitions;
- use-before-definition and cross-block dominance;
- valid block targets and reachability;
- edge argument count and type compatibility;
- instruction operand count and type rules;
- local and parameter indices;
- direct-call reference indices, signatures and results;
- return compatibility;
- one terminator per block.

A future interpreter may convert successfully verified bytecode into a faster internal representation, but it must never execute a module that failed decoding or verification.

## 16. Compatibility boundary

Version 0.1 is implemented but not stable. Before declaring a stable bytecode format, the project still needs decisions for:

- language, MIR, Runtime ABI and metadata version fields in the header;
- module stable ID and content hash;
- required/optional section flags;
- checksums, signatures and compression;
- constant/type/import/export tables beyond the current primitive profile;
- exception tables, GC maps and debug information;
- compact opcode-specific operand formats;
- resource limit policy and fuzzing corpus.

Any incompatible physical change before format freeze may replace v0.1. After freeze, incompatible changes require a major-version increment.
