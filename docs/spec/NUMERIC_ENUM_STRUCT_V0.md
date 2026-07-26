# Numeric, Enum and Struct Values — Implemented Draft v0.1

- implementation slice: Phase 3E
- bytecode dependency: `.rsbc` 0.4
- status: implemented draft; value ABI is not frozen

## 1. Canonical numeric values

The implemented storage categories are checked signed `int` (32-bit), checked signed `long` (64-bit) and IEEE 754 `double` (binary64). Widening is limited to `int -> long`, `int -> double` and `long -> double`.

Integer failures are explicit runtime errors. Floating operations preserve IEEE values and do not inherit C++ undefined behavior.

## 2. Enum identity

An enum value consists of exact enum TypeId plus signed 64-bit member value. The TypeId participates in assignment, calls, equality and verifier checks. No implicit enum/integer conversion is implemented.

## 3. Struct representation

A `StructValue` contains exact TypeId and immutable ordered field storage. Struct assignment copies the value. Implementations may share immutable storage internally, but mutating one local must not change another copied local.

All struct fields have deterministic declaration-order indices. Direct or indirect all-struct layout cycles are invalid programs.

## 4. Defaults

Numeric defaults are zero. Enum default is underlying zero. Struct default recursively initializes every field according to its declared type. Class/string/array fields are typed null values and handles are invalid handles.

## 5. Precise references

The runtime recursively scans struct fields when a struct occurs in shadow-stack roots, persistent host roots, arrays or managed-object fields. `ManagedHeap::retain(Value)` can therefore keep a returned struct and all of its nested managed references alive. Root validation recursively rejects nested references that belong to another heap. The struct value itself is not separately allocated in the managed heap.

## 6. Mutation

Constructors may update their local `this` and return the resulting struct. Direct assignment to a local struct field performs value replacement. Ordinary struct instance methods are read-only in this version, and setters are rejected, because `ref this` and addressable-place semantics are not yet implemented.

## 7. Compatibility

The `.rsbc 0.4` representation is not compatible with 0.3. Struct layout, native ABI passing and boxing policy remain draft.
