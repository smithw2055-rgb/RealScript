# RealScript Object Model — Implemented Draft v0.1

- implementation slice: Phase 3B
- bytecode dependency: `.rsbc` 0.2
- allocation model: managed non-moving Record objects
- status: implemented draft; object ABI is not frozen

## 1. Class identity

A class is identified by a stable non-zero 64-bit TypeId derived from:

```text
<module>::<type-name>
```

Type identity MUST NOT depend on source order, process addresses or module load addresses. Function parameters, returns and call references with class types MUST retain the exact TypeId in addition to the generic runtime `object` category.

## 2. Implemented declarations

A class contains an ordered list of instance fields. The implemented profile supports fields of:

- `bool`;
- `int`;
- `string`;
- another class type, including the declaring type.

Field indices are declaration-order indices beginning at zero. Duplicate field names and unknown field types are invalid programs.

The implemented profile has no constructors, methods, inheritance, static fields or visibility modifiers.

## 3. Allocation and defaults

`new T()` allocates a managed Record with TypeId `T` and exactly the descriptor field count. Field defaults are deterministic:

- `bool`: `false`;
- `int`: `0`;
- `string`: null string;
- class: null object.

Allocation failure produces `OutOfMemory`.

## 4. Null and identity

A null class value has runtime type category `object` and no ObjectRef. Member access MUST execute a null check before reading or writing a field. Null dereference produces `NullReference`.

Class equality is identity equality:

- the same ObjectRef equals itself;
- distinct live ObjectRefs are not equal;
- two null object values are equal;
- null and a live object are not equal.

Stale or forged ObjectRefs MUST produce `InvalidObjectReference` rather than participating in equality or member access.

## 5. Field access

A field operation carries:

- the owner TypeId/type descriptor index;
- the field index;
- the statically known field type.

The runtime MUST reject an ObjectRef whose stored TypeId differs from the owner TypeId. Field reads and writes MUST remain within the descriptor field count.

## 6. Exact object signatures

For every function/reference signature:

- primitive parameters use TypeId zero;
- class parameters use their stable non-zero TypeId;
- primitive returns use TypeId zero;
- class returns use their stable non-zero TypeId.

The interpreter validates class arguments and return values at execution boundaries. This is required even for a function body that never dereferences the argument.

## 7. GC reference maps

A class descriptor defines an exact reference-field map:

- `string` fields are references;
- class fields are references;
- `bool` and `int` fields are non-references.

The collector traces only reference fields. Stores to reference fields MUST execute the incremental collector write barrier.

## 8. MIR and bytecode operations

The implemented operations are:

```text
new.object TYPE
check.notnull TYPE, object
load.field TYPE.FIELD, object
store.field TYPE.FIELD, object, value
conv.null.object
```

Every operation is type-checked by the compiler and structurally checked by MIR/bytecode verifiers.

## 9. Non-goals

This draft does not define arrays, constructors, methods, inheritance, interfaces, properties, native-resource handles, finalizers, weak references or binary object-layout compatibility.
