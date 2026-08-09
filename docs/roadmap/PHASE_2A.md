# Phase 2A — Typed Register Bytecode

## Status

Implemented as the first execution-backend slice after the Phase 1 compiler frontend.

## Goal

Create a complete, non-executing bytecode toolchain boundary:

```text
Verified Typed MIR
        │
        ▼
MIR → Typed Register Bytecode
        │
        ├── Verifier
        ├── Disassembler
        ├── Deterministic Encoder
        └── Defensive Decoder
                │
                ▼
             .rsbc 0.1 (initial baseline)
```

Phase 2A intentionally stops before instruction execution. Phase 2B can build the interpreter on a validated and testable module representation rather than combining file parsing, verification and execution in one step.

## In-memory bytecode model

The model contains:

- module version and name;
- function-reference table with stable `SymbolId` and signatures;
- functions with parameter, local and register type tables;
- basic blocks and typed block parameters;
- register instructions;
- jump, conditional branch and return terminators.

MIR `ValueId` values map directly to bytecode register indices. Mutable source variables remain explicit local slots, while block parameters retain short-circuit and control-flow merge values.

## MIR lowering

The lowerer currently maps:

- MIR parameters to `param`;
- constants to typed constant instructions;
- local loads/stores without semantic rediscovery;
- `null → string` to an explicit conversion instruction;
- calls to function-reference table indices;
- arithmetic and comparisons to type-specific opcodes;
- blocks and edge arguments without flattening short-circuit semantics.

Function references are deduplicated by stable symbol identity, name and full signature. Cross-module calls remain valid external references until a future linker/runtime resolves them.

## `.rsbc` container

The Phase 2A v0.1 container established the original shape. Phase 3B advanced the format to 0.2 with object descriptors; Phase 3C advanced the format to 0.3 with arrays/handles; Phase 3D–3E advance the producer to 0.4 with member/value metadata and operations; Phase 4 advances it to 0.5 with debug metadata. The shared container uses:

- `RSBC` magic;
- a versioned bytecode header (currently 0.5);
- little-endian fixed-width scalar fields;
- a section directory;
- string, function-reference, function metadata and code sections;
- generic instruction records for simple validation;
- relative function code ranges.

The exact format is documented in [BYTECODE_FORMAT_V0.md](../spec/BYTECODE_FORMAT_V0.md).

## Validation layers

### Decoder validation

The decoder performs physical and resource checks before constructing the module:

- header and version;
- section bounds and overlap;
- collection counts before allocation;
- string and type tags;
- code range bounds and overlap;
- opcode and terminator tags;
- complete consumption of each section and function body.

### Semantic bytecode verification

The verifier checks:

- function/reference identity and signature consistency;
- register, local and parameter types;
- unique register definitions;
- definition dominance and use-before-definition;
- branch targets and reachability;
- block argument signatures;
- instruction-specific type rules;
- call and return compatibility;
- terminator completeness.

No Phase 2B execution path may bypass either layer.

## CLI

New commands:

```bash
rsc game.rs --bytecode
rsc game.rs --emit-bytecode game.rsbc
rsc game.rsbc --disassemble
```

`--emit-bytecode` retains the original single-module contract. The completed
productization path also provides `--emit-bytecode-dir`, which writes one
`.rsbc` artifact per compiled module for deterministic loading and linking by
`GameProgramLoader`.

## Tests

Phase 2A adds twelve focused cases:

- MIR-to-bytecode lowering;
- bytecode verifier acceptance;
- canonical encode/decode/re-encode;
- disassembly round trip;
- bad magic;
- unsupported version;
- truncated payload;
- overlapping section;
- oversized collection count;
- invalid register definition;
- invalid call reference;
- invalid branch arguments;
- source-order-independent encoding;
- full disassembly and hexadecimal snapshots.

The repository now runs the Phase 1A/1B suite, Phase 1C suite and Phase 2A suite together.

## Validation configurations

Locally validated with:

- GCC 14.2, C++17, warnings-as-errors;
- Clang 17, C++17, warnings-as-errors;
- AddressSanitizer;
- UndefinedBehaviorSanitizer.

The standard GitHub Actions Linux/Windows matrix remains the remote release gate.

## Explicit limitations

Phase 2A does not implement:

- instruction execution;
- VM stack frames or scheduler;
- exception regions;
- GC maps or object references;
- debug sequence-point sections;
- capability or budget instructions;
- compact operands;
- package-level multi-module containers;
- linking or native binding resolution;
- cryptographic module hashes or signatures;
- stable binary compatibility.

## Next slice

Phase 2B should add:

1. interpreter execution context;
2. typed register frames and local storage;
3. direct call stack and external function resolution boundary;
4. branch edge-argument transfer;
5. checked arithmetic and script runtime errors;
6. instruction and recursion budgets;
7. deterministic execution tests;
8. MIR/bytecode behavior tests for the implemented primitive language profile.
