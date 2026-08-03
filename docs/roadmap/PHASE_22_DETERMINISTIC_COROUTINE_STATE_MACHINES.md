# Phase 22 — Deterministic Coroutine State Machines

Phase 22 evolves `sequence` from fixed top-level callback splitting into an
explicit, snapshot-safe deterministic state-machine model. Execution remains
single-threaded and fixed-tick; it does not adopt thread-based `Task` semantics.

## Work breakdown

- **22A persisted parameters, locals, and temporaries:** complete.
- **22B structured branch/loop/switch suspension states:** complete.
- **22C `yield break`, cancellation, nesting, and results:** complete.
- **22D snapshot/restore/rollback and hot-reload migration:** complete.
- **22E debugger/source mapping and backend validation:** complete.

## Delivered state machine

- Locals declared by a sequence are promoted to compiler-owned fields with
  exact primitive/object/struct/enum/array descriptors. Continuation callbacks
  read and write those fields transparently, so values survive a wait.
- `yield break` terminates the generated callback chain and makes following
  statements unreachable from the sequence.
- A generated `Cancel<SequenceName>()` method owns the current deterministic
  timer handle and cancels pending continuation dispatch through `CancelTimer`.
- An explicit program counter represents suspension inside nested blocks,
  branches, loops, and switches. Locals and intermediate values needed after a
  yield are promoted to exact compiler-owned state fields.
- Nested sequence calls suspend the parent until the child completes. Declared
  sequence results are retained and may be consumed by the parent state machine.
- Managed-heap snapshots retain promoted fields, while `GameplayHost` snapshots
  retain the scheduled callback. Restoring both replays the same continuation
  and result.
- Promoted fields are synthetic and excluded from normal tooling surfaces; their
  identities participate in save-state and hot-reload layout compatibility.
- DAP sequence points remain tied to the original `sequence` and `yield`
  locations. The generated C++17 AOT backend executes the same continuation
  transitions as the interpreter.

## Validation

- `realscript.phase22.coroutines` covers persisted values, nested control flow,
  cancellation, `yield break`, child sequences, results, replay, rollback, and
  hot-reload migration rules.
- `realscript.phase22.aot` compiles and executes the state-machine fixture in
  generated C++17 and compares the observable result with the interpreter.
- Phase 22 remains intentionally single-threaded. `Task`, threads, and general
  `async`/`await` are outside this deterministic gameplay model.
