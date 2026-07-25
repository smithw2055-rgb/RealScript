# RealScript Embedding and Observability — Draft v0.1

## Linked image

A program image is an immutable collection of verified bytecode modules with unique stable function identities. A runtime must not execute a module set that failed linking.

## Host binding

A host binding is selected by stable `SymbolId` first and canonical name second. Arguments and return values use the primitive runtime `Value` model. Binding failure produces a structured runtime error.

## Trace semantics

Trace callbacks are optional and synchronous. Event ordering follows interpreter execution order. A callback must not be used as part of script semantics; deterministic simulation should either disable tracing or use a deterministic sink.

## Statistics

Statistics are monotonic within one invocation and cover the complete call tree. Instructions and terminators consume the same execution budget and appear in the final instruction count.

## Threading

A linked image and binding registry may be shared for read-only access. An `EngineRuntime` invocation owns its execution state. Concurrent invocation guarantees remain implementation-defined until the runtime threading profile is specified.
