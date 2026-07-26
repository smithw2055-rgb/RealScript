# Methods, Constructors and Properties — Implemented Draft v0.1

- implementation slice: Phase 3D
- bytecode introduced in: `.rsbc` 0.4; current producer: `.rsbc` 0.5
- dispatch model: direct, statically resolved
- status: implemented draft; member ABI is not frozen

## 1. Member identity

Every method, constructor and property accessor is a function with a stable SymbolId. The canonical key contains module, owner type, member name and visible parameter types. Instance members additionally carry the exact owner TypeId as hidden parameter zero.

## 2. Calls

Overload resolution is completed before MIR lowering. Bytecode contains only direct references; runtime name lookup is not used for script members. A class instance call must null-check and validate the receiver TypeId before entering the callee.

Struct receivers are values. Phase 3E restricts their instance methods to read-only behavior until an addressable-place ABI exists.

## 3. Constructors

Class construction allocates a fully default-initialized object before calling `.ctor`. The newly allocated object is rooted through the active interpreter frame during the call. If construction fails, the partially initialized object is not returned to script code.

Struct construction starts from a zero/default value. A struct constructor receives that value as hidden parameter zero and returns the updated value.

## 4. Properties

Properties are accessor pairs and do not create a distinct runtime invocation mechanism. A getter is `get_<name>()`; a setter is `set_<name>(value)`. Class auto-properties create a synthetic descriptor field. Verifier and GC rules for that field are identical to explicit fields.

## 5. Compatibility

Member SymbolIds and bytecode signatures are deterministic but remain draft. Inheritance, virtual slots and interface maps will require a new dispatch specification rather than changing direct-call meaning implicitly.
