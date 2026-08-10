# Determinism and Replay

[Documentation Home](README.md) | [AOT, JIT, and Performance](AOT_JIT_AND_PERFORMANCE.md)

RealScript provides explicit deterministic execution modes for simulation, debugging, reproducibility, and cross-backend validation. Determinism is an opt-in runtime contract rather than an assumption that every host API is deterministic.

## Execution Modes

### Off

Normal execution. Host bindings are invoked without deterministic policy enforcement or replay logging.

### Strict

Only bindings declared `Deterministic` may execute. Recordable or non-deterministic host calls are rejected before invocation.

### Record

Deterministic bindings execute normally. Recordable bindings execute and append a replay entry containing their observable call identity, arguments, and result.

Non-deterministic bindings are rejected.

### Replay

Recordable bindings are not invoked. The runtime consumes the next replay entry, validates it against the current call, and returns the recorded result.

Deterministic bindings may still execute normally.

## Host Binding Policies

Every host binding declares one policy:

| Policy | Strict | Record | Replay |
|---|---:|---:|---:|
| `Deterministic` | Execute | Execute | Execute |
| `Recordable` | Reject | Execute and record | Validate and replay |
| `NonDeterministic` | Reject | Reject | Reject |

Examples of deterministic bindings may include pure math helpers or stable table lookups. Time, operating-system randomness, network input, and uncontrolled I/O normally require recording or remain disallowed.

## Replay Entries

A replay entry identifies:

- the ordered call index;
- binding identity;
- argument count and argument values;
- returned value or structured error;
- stable type information required for validation.

Replay validation rejects:

- missing entries;
- extra unconsumed entries;
- wrong binding order;
- wrong function identity;
- argument count mismatch;
- argument value mismatch;
- incompatible return values;
- truncated logs;
- non-replayable values.

Replay succeeds only when the complete log is consumed.

## Recordability Checks

Record mode validates that arguments and results can be serialized into stable replay values.

Managed object references and native handles are not replay-stable by default because their slots, generations, registries, or heap identities are process-local. Structs are replayable only when every nested field is replay-stable.

The runtime performs required validation before invoking a recordable host function whenever failure would otherwise create an unrecorded side effect.

## Stable Execution Digest

Execution events can feed a stable digest. The digest covers observable semantic information such as:

- function entry and return;
- instruction and terminator order;
- branches;
- external calls;
- structured errors;
- stable primitive, enum, struct, and string values.

The digest deliberately excludes process-local details such as:

- native addresses;
- heap identity numbers;
- native handle registry identity;
- compiler object addresses;
- unordered container iteration artifacts.

## Floating-Point Canonicalization

For deterministic hashing and replay comparison:

- all NaN payloads are canonicalized;
- positive and negative zero are treated as the same deterministic value;
- finite binary64 values retain their exact bits.

This canonicalization affects deterministic comparison and hashing; it does not redefine ordinary floating-point arithmetic.

## Cross-Backend Event Ordering

Interpreter and AOT execution emit the same semantic event sequence for:

- function boundaries;
- ordinary instructions;
- terminators;
- branches;
- host calls;
- errors.

The toolchain JIT executes generated AOT semantics and participates in the same differential tests.

## Profiles and Digests

A deterministic run may produce both:

- an execution digest for semantic equality;
- a per-function profile for operational comparison.

Differential tests compare return values, errors, digests, and profiles across interpreter, AOT, and JIT execution.

## Example

```bash
rsc game.rs \
  --run Game.Main::main \
  --opt-level 2 \
  --deterministic \
  --profile \
  --digest
```

The CLI deterministic option enables the strict deterministic execution profile appropriate for scripts that do not require replayable host input.

## Deterministic Simulation Boundary

The v0.2.0 runtime establishes execution-level determinism, host-call policy,
record/replay validation, and stable digests. A complete fixed-tick game
simulation environment still needs host-level policies for:

- fixed time steps;
- seeded random-number streams;
- stable collection iteration;
- deterministic asset and input ordering;
- world-state serialization;
- module and content hash validation;
- networking and rollback integration.

These systems are expected to build on the current execution contract rather than bypass it.

## Security and Correctness Notes

Record/replay is not a sandbox by itself. A host must still control which bindings are available and validate all external data.

Replay logs should be treated as versioned data tied to a specific language/runtime/module baseline. Compatibility is not guaranteed across unfrozen ABI or semantic changes.
