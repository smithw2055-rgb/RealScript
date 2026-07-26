# Debug Information, Tooling and Hot Reload — Implemented Draft v0.1

- implementation phase: Phase 4
- bytecode dependency: `.rsbc 0.5`
- status: implemented draft

## Debug source identity

Each compiled module carries a deterministic source table. A source entry contains a module-local SourceFileId, path, content fingerprint and line-start offsets. Sequence points and variable scopes reference SourceFileId plus both byte spans and zero-based line/column ranges.

A debugger must use the source table from the executing ProgramImage. SourceFileIds are module-local and must not be treated as globally unique.

## Sequence points

A sequence point identifies:

- basic block;
- instruction index, or the block terminator position;
- source range.

The verifier requires the target block/instruction to exist and rejects duplicate bytecode locations. Multiple bytecode locations may map to the same source expression.

## Local-variable metadata

Each entry carries name, local slot, primitive category, exact TypeId, parameter flag, declaration range and lexical scope range. The slot and exact type must match function metadata. Parameters are represented in the same local-slot namespace used by the interpreter.

## Debug execution

Execution may stop only at verified sequence points. The interpreter captures frames from the current call stack without copying or exposing native addresses. A frame exposes function identity, source location, arguments and in-scope locals as runtime Values.

Resume modes are Continue, StepIn, StepOver, StepOut and Terminate. Step operations are defined by changed source location and call depth rather than raw instruction count.

## DAP transport

The DAP adapter uses standard `Content-Length` framing and request/response/event envelopes. Line and column fields are converted between DAP one-based values and internal zero-based positions. Unsupported requests fail explicitly rather than being silently ignored. Protocol frames are capped at 16 MiB, duplicate `Content-Length` headers are rejected, and non-finite runtime numbers are serialized as JSON `null` rather than invalid JSON tokens.

## Language service

The language service rebuilds the open workspace through the normal compiler and can reuse the previous BuildSnapshot. Diagnostics, symbol details and definitions therefore share the same parsing and semantic rules as command-line compilation.

The LSP adapter uses JSON-RPC 2.0 with the same bounded standard framing and zero-based positions. Full-document synchronization is the implemented baseline.

## Hot reload compatibility

The implemented profile allows function-body replacement only. Compatibility requires stable module set, exact type layouts, enum values, function set and function signatures. Local-variable layout and debug metadata may change because active invocations hold the old ProgramImage and new invocations use the replacement image.

The replacement image is linked and bytecode-verified before compatibility analysis. An incompatible image never changes the running EngineRuntime.

## Concurrency and ownership

ProgramImages are immutable and shared. EngineRuntime snapshots the current image before each invocation. Replacing the current image is atomic with respect to that snapshot operation. ManagedHeap and native-handle ownership remain unchanged across a body-only reload.

## Compatibility

`.rsbc 0.5` is not binary compatible with 0.4 because it adds a mandatory physical DEBUG section to encoded modules. In-memory modules may omit debug metadata for embedding tests, but encoded compiler output always includes the section and exact debug table counts.
