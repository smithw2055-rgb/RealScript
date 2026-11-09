# Changelog

All notable RealScript changes are documented here. Compatibility contracts
remain versioned but unfrozen until a future 1.0 release.

## [Unreleased]

## [0.98.0] - 2026-11-09

v0.98.0 represents the major Game Engine Integration Evolution release, bringing deep parity with modern engine scripting workflows (Godot/Unity), flat-frame high-throughput VM execution, automated C++ binding generation (`rsbind`), zero-budget trusted AOT, native fast-paths, advanced DAP/LSP tooling, and zero-copy ECS architecture.

### Virtual Machine & Runtime Architecture

- Added **Unified Flat-Frame Slot Layout**: consolidated local variables, evaluation stack, and frame overhead into contiguous memory, significantly reducing call-frame allocation and cache thrashing.
- Implemented **Computed-Goto Interpreter Dispatch**: high-performance threaded code dispatch in the bytecode VM loop with fallback to direct-switch, including dedicated `FlatOpcode` fast paths for `double` arithmetic/comparisons (`ConstantDouble`, `AddDouble`, `SubDouble`, `MulDouble`, `DivDouble`, `LessDouble`, `LessOrEqualDouble`, `GreaterDouble`, `GreaterOrEqualDouble`), 4-instruction `double` local arithmetic superinstructions (`SuperBinaryLocalDoubleConstStore[Elided]`, `SuperBinaryLocalDoubleLocalStore[Elided]`), scalar conversion/array-length fast paths (`ConstantBool`, `ConstantLong`, `ArrayLengthFast`, `ConvertIntToLongFast`, `ConvertIntToDoubleFast`, `ConvertLongToDoubleFast`), and object/inline-struct field loads and stores (`LoadFieldFast`, `StoreFieldFast`, `LoadStructFieldFast`, `StoreStructFieldFast`).
- Widened `BytecodeString` to 32-bit with lock-free paged string and register slice pools for low contention in multi-threaded execution, and added `SmallRegisterVector::assign` batch slice interning (`internedSliceCount`, `internedStringCount`) to eliminate intermediate `2..N-1` register slice allocations during lowering and decoding.
- Added **Incremental GC Stepping & Shadow-Stack-Safe Value Pool Reclamation**: integrated bounded GC step ticking in the flat-VM loop, safeguarding pooled values and preventing long pause times during continuous allocation.
- Introduced **Small Buffer Optimization (SBO) for HeapObject**: optimized short strings and small inline objects to eliminate heap allocations for transient operations.
- Added **Native Fast-Path & Identity Cache**: fast O(1) bi-directional C++ host object mapping and bypassing reflection thunks for hot-path method calls.
- Flattened `SceneScriptRuntime` dispatch for direct invocation without intermediary bridge wrappers.
- Compacted `runtime::Value` to 24 bytes (`PooledStringValue`, `PooledStructValue`, `PooledClosureValue`) and `bytecode::Instruction` to 8 bytes, added quickened integer-loop bytecode superinstructions (`AddIntLocalConst`, `SubIntLocalConst`, `LessIntLocalConstBranch`), and integrated `ValuePoolArena::collectGarbage` into `ManagedHeap::collect`.

### C++17 AOT & Native Performance

- Added **Zero-Budget Trusted Release AOT Mode** (`GenerationOptions::trustedRelease`, `ExecutionOptions::trustedRelease`, `rsaot --trusted-release`, and CMake `realscript_add_aot_library(... TRUSTED_RELEASE)`), compiling out instruction/branch/GC accounting guards via `if constexpr (!TrustedRelease)`.
- Added **Custom Struct Stack Scalarization** (`AotStruct_<typeId>`), decomposing value-type structs up to 16 scalar/inline-math fields into stack-allocated C++ structs.
- Added **`#line` Source Mapping Directives & Symbolized Stack Traces**: C++17 AOT codegen emits `#line` markers and static `programSourceMap` tables (`AotLineMapping`), and both AOT and the Bytecode Interpreter populate `RuntimeError::sourceLocations` and `RuntimeError::symbolizedStackTrace` (`function (path:line:col)`).
- Implemented **AVX2 / SIMD Vectorized ECS Loop Reconstruction**: auto-detects and transforms ECS query loops into vectorized C++ inner loops (`__restrict` native column pointers and `#pragma loop(ivdep)` / `#pragma clang loop vectorize(enable)`).
- Added natural-loop **LICM (Loop-Invariant Code Motion) Load Hoisting** and **Cross-Module `-O2` Auto-Property, `void`, & Small-Function Inlining** (`CheckNotNull`, `LoadField`, `StoreField`, `ConvertNumeric`, `ReturnVoid`) in the MIR/bytecode optimizer.

### Game SDK & ECS Integration

- Added **Zero-Copy Host C++ Contiguous Struct ECS Raw Column Binding** (`ObjectKind::RawColumnArray`, `NativeFieldLayout`, `NativeFieldKind`, `ScriptRuntime::bindEcsRawColumn`, and `bindEcsRawColumnAuto<HostStruct>`) with in-place `HeapSlot` reuse on per-frame rebind across both the Bytecode Interpreter and C++17 AOT.
- Added **`rsbind` Auto-Binding Generator CLI Tool & Custom C++ Value-Type Struct Marshaling**: parses C++ headers (including `RS_EXPORT_STRUCT`, `RS_EXPORT_CONSTRUCTOR`, `static` methods, `static_cast<...>` overload disambiguation, and `std::vector<T>` / template parameter lists) to automatically generate type-safe bindings (`GameApi::structType<S>`, `NativeStructBuilder<S>::field<&S::f>`, `NativeTypeBuilder::constructor<Args...>`, `staticMethod<M>`, same-name function/method/constructor overloading via signature-keyed `__ov*` native thunks, and `std::vector<T>` <-> `T[]` marshaling), method thunks, and ECS raw column helpers.
- Extended `Query<...>()` ECS chunk queries with multi-entity Sparse-Set intersection joins (`setEcsEntityComponent`, `getEcsEntityComponent`, `hasEcsEntityComponent`, `removeEcsEntityComponent`, `destroyEcsEntity`), `With<T>` / `Without<U>` filter clauses, optional leading `int entityId` iteration bindings, and `EntityCommandBuffer` (ECB) deferred structural mutation queues.
- Extended `RealScript.Physics` and `SceneScriptRuntime` with 3D fixed-point colliders (`Box3D`, `Sphere3D`, `Capsule3D`), `stepPhysics3D`, spatial queries (`raycast3D`, `raycastAll3D`, `overlapSphere3D`), 3D collision/trigger callbacks (`OnCollisionEnter3D`/`Stay3D`/`Exit3D`, `OnTriggerEnter3D`/`Stay3D`/`Exit3D`), generational entity handles, and deferred entity destruction.
- Upgraded 12-byte `InlineStructSlot` bit-packing to support `Vec3`, `Quat`, and `Enum` fields inline inside `runtime::StructValue` without increasing `sizeof(InlineStructSlot)` (12B) or `sizeof(Value)` (24B), with in-place slot updates in `Opcode::StoreStructField`.
- Added `[ExportGroup]` / `[ExportSubgroup]` / `[ExportCategory]` inspector schema grouping, and `[FormerlySerializedAs]` static field rename migration.

### Language & Type System

- Added **User-Defined `implicit` and `explicit` Conversion Operators** (`public static implicit/explicit operator Target(Source)`), **Multi-Interface Generic Constraints** (`where T : Base, IFoo, IBar, new()`), **Auto-Property Initializers** (`public int Hp { get; set; } = 100;`), and **Static Constructors** (`static ClassName()`).
- Enforced `static class` and constructor-assigned `readonly` fields across compiler, VM, and AOT.
- Extended standard library with math, bitwise, and string intrinsics.

### Debugging, Tooling & LSP Ecosystem

- Added **Embedded TCP Socket DAP Server** (`EmbeddedDapServer` in `realscript/debug/DapServer.h` and `rsdebug --dap-port <port>` attach mode) for in-engine live debugging.
- Added **DAP Exception Breakpoints**: break on caught/uncaught script exceptions with reentrant DAP safepoints.
- Added **DAP Heap Retaining-Path Profiler**: interactive path-to-root inspection for locating script memory leaks.
- Enhanced **LSP Language Server (`rslsp`)**: incremental `ParseSnapshot` cache, incremental subgraph metadata rebuild cache, fast symbol-id navigation, member dot-completion, signature help, and async request bridging.
- Published official **VS Code Extension (`vscode-realscript`)**: complete syntax highlighting, LSP client integration, and DAP launch/attach debugger configurations.
- Extended `DebugController`, `DebugSession`, and `DapServer` with conditional breakpoints (`BreakpointSpec`, `setConditionalBreakpoints`), hot-reload `requestedLine` / `requestedColumn` breakpoint preservation (preventing downward drift on rebind), same-line sequence-point re-entry suppression, and side-effect-free watch/hover expression evaluation.
- Extended `GameApi::exportDeclarationStub()` (`.d.rs`), `LanguageService` (`realscript.project.json`, ambient sources, member `.` dot-completion, and `signatureHelp`), and `LspServer` (`textDocument/signatureHelp`).
- Added `exportChromeTraceJson` to export `ExecutionProfile` and `GcStatistics` in Chrome Tracing / Perfetto (`traceEvents`) JSON format.

### Compatibility

- Project version is now **0.98.0**.
- Current bytecode output is `.rsbc` 0.9; the decoder accepts 0.6–0.9.
- SDK compatibility, Game SDK package, and script-object-state versions remain independently versioned at 1.
- Optimized `ManagedHeap` by sharing `referenceFields` layout vectors across
  instances of the same `typeId` (`sharedTypeLayoutCount`) and adding global
  string interning (`internString`, `internedStringCount`) used by
  `Opcode::ConstantString` in both the Bytecode Interpreter and C++17 AOT.
- Added $O(1)$ `SymbolId` resolution caching (`cachedSymbolCount`) in
  `BindingRegistry` to eliminate string hashing overhead on repeated external
  calls.
- Upgraded `sequence` coroutines to infer `var` local variable types across
  suspension points (`makeSequenceLocalField`) and support 1- and 2-argument
  `yield wait_signal(...)` bridged with `SceneScriptRuntime::emitSignal`.
- Added deterministic `RealScript.Input` snapshot APIs (`setInputAction`,
  `setInputAxis2D`, `advanceInputTick`, `IsActionPressed`,
  `IsActionJustPressed`, `IsActionJustReleased`, `GetInputAxis2D`,
  `computeInputDigest`) and `RealScript.Physics` 2D fixed-point colliders,
  spatial queries (`raycast2D`/`Raycast2D`,
  `overlapCircle2D`/`OverlapCircleCount2D`), and `OnCollisionEnter`/`Stay`/`Exit`
  & `OnTriggerEnter`/`Stay`/`Exit` lifecycle callbacks (`stepPhysics2D`).
- Implemented compile-time `[Deterministic]` / `[NonDeterministic]` static
  verification (`RS2495`) across modules, types, and functions, and runtime
  Mod capability sandboxing (`ErrorCode::CapabilityDenied`,
  `ExecutionOptions::enforceCapabilities`, `grantedCapabilities`).
- Implemented zero-heap-allocation inline storage (`InlineStructSlot`) inside
  `runtime::StructValue` for custom value-type `struct`s with up to 4 fields
  (`bool`, `byte`, `sbyte`, `short`, `ushort`, `int`, `uint`, `long`, `ulong`,
  `float`, `double`, `char`, `Enum`, `Fixed`, `Fixed16`, `Vec2`, `Vec3`, `Quat`),
  eliminating `std::shared_ptr<StructStorage>` and `std::vector<Value>` heap
  allocations across the Bytecode Interpreter, C++17 AOT runtime, JSON
  serialization, and `RSGS` binary state snapshots.
- Implemented compile-time AST/Semantic/MIR lowering of ECS chunk queries
  (`foreach (ref var transform, in var velocity in Query<Write<Transform>, Read<Velocity>>())`),
  including compile-time read/write safety checks (`RS2214`, `RS2215`, `RS2216`,
  and `RS8702` preventing mutation of `Read<T>` / `in` component fields),
  contiguous SoA column iteration, automatic write-back for `Write<T>`
  components, and C++ host helpers (`ScriptRuntime::createStruct`,
  `setEcsComponentColumn`, `getEcsComponentColumn`).

- Decoupled entity active state (`setEntityActive` / `isEntityActiveSelf`) from
  per-component enabled state (`setComponentEnabled` / `isComponentEnabled`),
  allowing empty transform parent nodes and multi-component entities to cascade
  `OnEnable` / `OnDisable` accurately across hierarchy subtrees.
- Upgraded `SceneScriptRuntime::instantiatePrefab` to a two-phase pipeline
  (Phase 1: hierarchy, transform, component allocation, and `[RequireComponent]`
  validation across the entire subtree; Phase 2: `OnCreate` and `OnEnable`
  activation sorted by `[ExecutionOrder]`).
- Added dirty-cached tick ordering (`rebuildTickOrderIfNeeded`) and dirty-cached
  world transforms (`worldTransformCache_` with subtree invalidation) to eliminate
  per-frame sorting and parent-chain walks.
- Added persistent signal connection tracking (`SignalConnection`),
  `disconnectSignal`, automatic signal disconnection when a target or source
  entity/component is detached, and automatic signal rebinding across hot reloads.
- Added whole-scene JSON serialization (`exportSceneJson` and `importSceneJson`)
  preserving entities, hierarchy, local transforms, multi-component fields,
  cross-entity `ObjectRef` references (including inside structs), and wired
  signal connections.
- Extended `SceneScriptRuntime::hotReload` to recursively patch migrated
  `ObjectRef` instances inside `Array` elements and `StructValue` fields,
  initialize newly added fields from class prototype defaults
  (`createPrototypeObject`), and added `ReloadOptions::allowStructuralChanges`
  to `realscript::hot_reload::prepare`.
- Bridged `SceneScriptRuntime` with `FiberScheduler` (`SceneFiberHostAdapter`,
  `spawnEntityFiber`, `stepFibers`, and `killEntityFibersBySlot` on entity
  detach/deactivation).
- Upgraded `GameApi::generateBindingsFromCppHeader` to support multi-line
  declarations, comments, `RS_EXPORT_ENUM`, `RS_EXPORT_PROPERTY`, `const`
  qualifiers, and default parameter values.

- Added script class inheritance from C++ `NativeType` wrappers with polymorphic
  native handle unwrapping and explicit native lifecycle release (`releaseNative`,
  `rebindNative`).
- Extended `SceneScriptRuntime` to support multi-component entities
  (`attachComponent`, `detachComponent`, `getComponent`, `getComponents`),
  `SceneExecutionMode` (`Play` vs. `Editor` with `[Tool]` and
  `[ExecuteInEditMode]`), and scene component bindings (`GetComponent`,
  `HasComponent`, `SetComponentEnabled`).
- Added `OnValidate()` lifecycle support and `setInspectorMember()` to trigger
  automatic validation when modifying component fields from the editor.
- Added host/editor signal wiring (`SceneScriptRuntime::connectSignal`) to
  connect script `event` delegates across entities and components.
- Auto-ingested compiler source attributes into `ScriptMetadataRegistry` and
  added deep `InspectorTypeSchema` reflection covering `[Export]`/`[SerializeField]`,
  `[HideInInspector]`, `[Range]`, `[Tooltip]`, `[Header]`, `[ReadOnly]`,
  `[EntityRef]`/`[ComponentRef]`/`[AssetRef]`, enum options, recursive nested
  struct fields, collection element/key types, and `event` signal signatures.
- Added human-readable JSON scene/prefab serialization (`exportScriptObjectJson`
  and `importScriptObjectJson`) with support for nested structs, arrays, and
  cross-entity component `ObjectRef` references (`{"$entity", "$component"}`).
- Added stateful structural hot reload (`ScriptRuntime::replaceProgram` and
  `SceneScriptRuntime::hotReload`) supporting added/removed fields,
  `[FormerlySerializedAs]` field renames, synthetic coroutine/event state
  preservation, and automatic cross-component `ObjectRef` and delegate target
  migration.

### Coroutines, math, fibers, and runtime

- Extended `sequence` coroutines with `yield wait_seconds(expr)`,
  `yield wait_event(topicExpr)` (via one-shot `GameplayHost::subscribeOnce`),
  and `yield wait_until(conditionExpr)`, plus automatic active sequence
  cancellation when a component or entity is disabled or detached.
- Added user-defined `operator` overloading (`+`, `-`, `*`, `/`, `%`, `==`,
  `!=`, `<`, `<=`, `>`, `>=`, unary `-`, `!`, `~`), compound assignments
  (`+=`, `-=`, `*=`, `/=`, `%=`), and `RealScript.Math` (`Fixed`, `Fixed16`,
  `Vec2`, `Vec3`, `Rect`, `Transform2D`).
- Added direct C++ binding marshaling and zero-allocation `ScriptRuntime`
  helpers (`makeFixed`, `makeVec2`, `makeVec3`, `readFixed`, `readVec2`,
  `readVec3`, `getMemberVec2`, `setMemberVec2`) for `Fixed`/`Fixed16`/`Vec2`/`Vec3`.
- Added `FiberScheduler` (`RealScript.Fiber` lowering), zero-copy SoA bridges,
  and tiered execution dispatch (`TieredExecutionPolicy`: Interpreter, JIT, AOT)
  with hot-loop/invocation auto-promotion.
- Optimized the bytecode interpreter with an execution mirror, arena frames,
  and inline call caches.

### Debugging, LSP, and observability

- Extended `DebugController`, `DebugSession`, and `DapServer` with conditional
  breakpoints (`BreakpointSpec`, `setConditionalBreakpoints`), same-line
  sequence-point re-entry suppression, and side-effect-free watch/hover
  expression evaluation (`evaluate` supporting locals, parameters, `this`
  fields, nested struct/object fields, array indexing, `.Length`, arithmetic,
  comparisons, and boolean operators).
- Extended `GameApi::exportDeclarationStub()` (`.d.rs`), `LanguageService`
  (`realscript.project.json`, ambient sources, member `.` dot-completion, and
  `signatureHelp`), and `LspServer` (`textDocument/signatureHelp`).
- Added `exportChromeTraceJson` to export `ExecutionProfile` and `GcStatistics`
  in Chrome Tracing / Perfetto (`traceEvents`) JSON format.

## [0.2.0] - 2026-08-10

v0.2.0 advances RealScript from the Phase 1–6 alpha baseline to the complete
Phase 1–24 language, runtime, Game SDK, deterministic gameplay, and performance
baseline.

### Language and compiler

- Added native structured control flow, interfaces, source attributes,
  delegates, closures, multicast events, runtime polymorphism, and visibility.
- Added inferred compile-time generics, constraints, generic members,
  collections, enumerators, nullable values, boxing, exact-width primitives,
  mutable structs, ref locations, patterns, initializers, flexible arguments,
  switch expressions, and structured script exceptions.
- Added deterministic coroutine state machines with persistent locals, nested
  control flow, cancellation, child sequences, results, snapshot, replay,
  rollback, and AOT parity.
- Migrated the Phase 11–17 expansion profile into native syntax, semantic,
  Typed MIR, bytecode, AOT, metadata, tooling, and hot-reload pipelines.

### Runtime and Game SDK

- Added typed C++ bindings, rooted script objects, scene lifecycle, events,
  triggers, bytecode package loading, installable SDK targets, and object-state
  serialization.
- Added generation-checked gameplay entities, fixed ticks, PCG random streams,
  deterministic timers/events, gameplay snapshots, stable hashes, metadata,
  generated bindings, and versioned `RSGS` save/replay/rollback state.
- Added ProgramImage link/verify-once indexes and direct SymbolId invocation.
- Fixed incremental GC sweep progress during continuous allocation while
  preserving birth-cycle objects and bounded work.

### Native execution and performance

- Added typed primitive C++17 AOT lowering, direct CFG labels, failure-aware
  accounting, conservative integer range analysis, and exact RAW/Strict local
  accounting.
- Preserved checked arithmetic, finite instruction budgets, public statistics,
  trace/profile behavior, and interpreter/AOT/JIT deterministic digest parity.
- Reduced the integer-loop AOT/JIT RAW baseline from roughly 3 ms to roughly
  26 µs on the reference machine.
- Reduced continuous-allocation GC from 5.239 s with no completed collection to
  27.75 ms with 460 completed collections on the reference workload.
- Added interpreter integer specialization, cheaper stack materialization,
  scene method caching, one-shot scheduler payload moves, and product-level AI,
  ability, event, allocation, coroutine, snapshot, restore, and replay
  benchmarks.

### Tooling and delivery

- Expanded `rsc`, `rsaot`, `rsbench`, DAP, LSP, source metadata, diagnostics,
  profiling, deterministic digests, and benchmark JSON output.
- Fixed O2 MIR validation for void delegate calls, struct stores, and exception
  handler control-flow edges; aligned interpreter/AOT parity tests on the same
  optimized input.
- Fixed the Windows `rsc` build and made `rsaot --version` plus LSP server
  metadata report the shared product version.
- Added component CMake targets, package version metadata, install-consumer
  validation, expanded Windows/Linux CI, and productized AOT integration.
- Reorganized English and Chinese repository documentation around features,
  quick start, execution backends, performance evidence, compatibility, and
  engine integration.

### Compatibility

- Project version is now 0.2.0.
- Current bytecode output is `.rsbc` 0.9; the decoder accepts 0.6–0.9.
- SDK compatibility, Game SDK package, and script-object-state versions remain
  independently versioned at 1.
- Source, MIR, bytecode, native ABI, metadata, debug-info, and serialized
  gameplay-state contracts are not frozen. Pin the exact release and rebuild
  generated AOT artifacts with the matching SDK.

## [0.1.0-alpha] - 2026-07-26

- Delivered the first complete Phase 1–6 compiler/runtime technical baseline.
- Added verified Typed MIR and register bytecode, interpreter, managed heap,
  DAP/LSP tooling, C++17 AOT, deterministic execution, profiling, benchmarking,
  and optional external-toolchain JIT.

[0.98.0]: https://github.com/smithw2055-rgb/RealScript/compare/v0.2.0...v0.98.0
[0.2.0]: https://github.com/smithw2055-rgb/RealScript/compare/v0.1.0-alpha...v0.2.0
[0.1.0-alpha]: https://github.com/smithw2055-rgb/RealScript/releases/tag/v0.1.0-alpha
