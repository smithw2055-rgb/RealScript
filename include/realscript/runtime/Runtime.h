#pragma once

#include "realscript/bytecode/Bytecode.h"
#include "realscript/semantic/Semantic.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace realscript::debug { class DebugController; }

namespace realscript::runtime {

struct NullString {
    friend constexpr bool operator==(NullString, NullString) noexcept { return true; }
};

struct NullObject {
    friend constexpr bool operator==(NullObject, NullObject) noexcept { return true; }
};

struct NullArray {
    friend constexpr bool operator==(NullArray, NullArray) noexcept { return true; }
};

enum class ObjectKind : std::uint8_t {
    String,
    Array,
    Record,
};

struct ObjectRef {
    static constexpr std::uint32_t InvalidSlot = 0xffffffffu;

    std::uint32_t slot = InvalidSlot;
    std::uint32_t generation = 0;
    ObjectKind kind = ObjectKind::Record;
    std::uint64_t heapId = 0;

    [[nodiscard]] constexpr bool valid() const noexcept {
        return slot != InvalidSlot && generation != 0 && heapId != 0;
    }

    friend constexpr bool operator==(ObjectRef left, ObjectRef right) noexcept {
        return left.slot == right.slot &&
            left.generation == right.generation &&
            left.kind == right.kind &&
            left.heapId == right.heapId;
    }
    friend constexpr bool operator!=(ObjectRef left, ObjectRef right) noexcept {
        return !(left == right);
    }
};

struct NativeHandle {
    static constexpr std::uint32_t InvalidSlot = 0xffffffffu;

    std::uint32_t slot = InvalidSlot;
    std::uint32_t generation = 0;
    semantic::SymbolId typeId = 0;
    std::uint64_t registryId = 0;

    [[nodiscard]] constexpr bool valid() const noexcept {
        return slot != InvalidSlot && generation != 0 &&
            typeId != 0 && registryId != 0;
    }

    friend constexpr bool operator==(NativeHandle left, NativeHandle right) noexcept {
        return left.slot == right.slot &&
            left.generation == right.generation &&
            left.typeId == right.typeId &&
            left.registryId == right.registryId;
    }
    friend constexpr bool operator!=(NativeHandle left, NativeHandle right) noexcept {
        return !(left == right);
    }
};

struct LongValue {
    std::int64_t value = 0;
    friend constexpr bool operator==(LongValue left, LongValue right) noexcept {
        return left.value == right.value;
    }
};

struct EnumValue {
    semantic::SymbolId typeId = 0;
    std::int64_t value = 0;
    friend constexpr bool operator==(EnumValue left, EnumValue right) noexcept {
        return left.typeId == right.typeId && left.value == right.value;
    }
};

struct StructStorage;

struct StructValue {
    semantic::SymbolId typeId = 0;
    std::shared_ptr<const StructStorage> storage;

    friend bool operator==(const StructValue& left, const StructValue& right);
};

using Value = std::variant<
    std::monostate,
    NullString,
    NullObject,
    NullArray,
    bool,
    std::int64_t,
    LongValue,
    double,
    EnumValue,
    StructValue,
    std::string,
    ObjectRef,
    NativeHandle>;

struct StructStorage {
    std::vector<Value> fields;
};

inline bool operator==(const StructValue& left, const StructValue& right) {
    if (left.typeId != right.typeId) return false;
    if (left.storage == right.storage) return true;
    if (!left.storage || !right.storage) return false;
    return left.storage->fields == right.storage->fields;
}

enum class ErrorCode {
    None,
    FunctionNotFound,
    InvalidArguments,
    TypeMismatch,
    DivisionByZero,
    IntegerOverflow,
    InstructionBudgetExceeded,
    RecursionLimitExceeded,
    ExternalFunctionUnresolved,
    DuplicateSymbol,
    InvalidObjectReference,
    InvalidNativeHandle,
    NullReference,
    IndexOutOfRange,
    OutOfMemory,
    InvalidProgram,
    ExecutionTerminated,
    DeterminismViolation,
    ReplayMismatch,
};

struct RuntimeError {
    ErrorCode code = ErrorCode::None;
    std::string message;
    std::vector<std::string> stackTrace;
};

struct HeapConfig {
    std::size_t initialCollectionThresholdBytes = 64 * 1024;
    std::size_t maximumHeapBytes = 64 * 1024 * 1024;
};

enum class GcPhase {
    Idle,
    Mark,
    Sweep,
};

struct GcStatistics {
    std::uint64_t objectsAllocated = 0;
    std::uint64_t bytesAllocated = 0;
    std::uint64_t collectionsStarted = 0;
    std::uint64_t collectionsCompleted = 0;
    std::uint64_t objectsReclaimed = 0;
    std::uint64_t bytesReclaimed = 0;
    std::uint64_t markWork = 0;
    std::uint64_t sweepWork = 0;
    std::size_t liveObjects = 0;
    std::size_t liveBytes = 0;
    std::size_t peakLiveBytes = 0;
};

struct HeapEdge {
    ObjectRef target;
    std::string label;
};

struct HeapObjectInfo {
    ObjectRef reference;
    ObjectKind kind = ObjectKind::Record;
    semantic::SymbolId typeId = 0;
    semantic::PrimitiveType elementType = semantic::PrimitiveType::Error;
    semantic::SymbolId elementTypeId = 0;
    std::size_t sizeBytes = 0;
    std::size_t valueCount = 0;
    std::vector<HeapEdge> edges;
};

struct HeapRootInfo {
    std::uint64_t token = 0;
    ObjectRef reference;
    std::string kind;
};

struct HeapSnapshot {
    std::uint64_t heapId = 0;
    GcStatistics statistics;
    std::vector<HeapObjectInfo> objects;
    std::vector<HeapRootInfo> roots;

    [[nodiscard]] std::string toText() const;
};

class PersistentRoot;

enum class ArrayStoreTypePolicy {
    Exact,
    PrevalidatedAssignableObject,
};

class ShadowStack {
public:
    void pushFrame(
        const std::vector<Value>* arguments,
        const std::vector<Value>* locals,
        const std::vector<Value>* registers);
    void popFrame() noexcept;
    void visitRoots(const std::function<void(const Value&)>& visitor) const;
    [[nodiscard]] std::size_t frameCount() const noexcept;

private:
    struct FrameRoots {
        const std::vector<Value>* arguments = nullptr;
        const std::vector<Value>* locals = nullptr;
        const std::vector<Value>* registers = nullptr;
    };
    std::vector<FrameRoots> frames_;
};

class ManagedHeap {
public:
    using RootToken = std::uint64_t;

    explicit ManagedHeap(HeapConfig config = {});
    ~ManagedHeap();
    ManagedHeap(ManagedHeap&&) noexcept;
    ManagedHeap& operator=(ManagedHeap&&) noexcept;
    ManagedHeap(const ManagedHeap&) = delete;
    ManagedHeap& operator=(const ManagedHeap&) = delete;

    [[nodiscard]] std::optional<ObjectRef> allocateString(
        std::string value,
        RuntimeError* error = nullptr);
    [[nodiscard]] std::optional<ObjectRef> allocateArray(
        std::size_t length,
        Value initialValue = {},
        RuntimeError* error = nullptr);
    [[nodiscard]] std::optional<ObjectRef> allocateTypedArray(
        semantic::SymbolId arrayTypeId,
        semantic::PrimitiveType elementType,
        semantic::SymbolId elementTypeId,
        std::size_t length,
        Value initialValue,
        RuntimeError* error = nullptr);
    [[nodiscard]] std::optional<ObjectRef> allocateRecord(
        std::size_t fieldCount,
        RuntimeError* error = nullptr);
    [[nodiscard]] std::optional<ObjectRef> allocateObject(
        semantic::SymbolId typeId,
        std::vector<Value> fields,
        std::vector<std::size_t> referenceFields,
        RuntimeError* error = nullptr);

    [[nodiscard]] bool isAlive(ObjectRef reference) const noexcept;
    [[nodiscard]] std::optional<std::string_view> stringView(ObjectRef reference) const;
    [[nodiscard]] std::optional<std::size_t> arrayLength(ObjectRef reference) const;
    [[nodiscard]] std::optional<semantic::SymbolId> arrayTypeId(
        ObjectRef reference) const;
    [[nodiscard]] std::optional<semantic::PrimitiveType> arrayElementType(
        ObjectRef reference) const;
    [[nodiscard]] std::optional<semantic::SymbolId> arrayElementTypeId(
        ObjectRef reference) const;
    [[nodiscard]] std::optional<Value> arrayGet(ObjectRef reference, std::size_t index) const;
    bool arraySet(
        ObjectRef reference,
        std::size_t index,
        Value value,
        RuntimeError* error = nullptr,
        ArrayStoreTypePolicy typePolicy = ArrayStoreTypePolicy::Exact);
    [[nodiscard]] std::optional<std::size_t> fieldCount(ObjectRef reference) const;
    [[nodiscard]] std::optional<semantic::SymbolId> objectTypeId(
        ObjectRef reference) const;
    [[nodiscard]] std::optional<Value> fieldGet(ObjectRef reference, std::size_t index) const;
    bool fieldSet(
        ObjectRef reference,
        std::size_t index,
        Value value,
        RuntimeError* error = nullptr);

    [[nodiscard]] RootToken addPersistentRoot(ObjectRef reference);
    [[nodiscard]] RootToken addPersistentRoot(Value value);
    bool updatePersistentRoot(RootToken token, ObjectRef reference);
    bool updatePersistentRoot(RootToken token, Value value);
    bool removePersistentRoot(RootToken token) noexcept;
    [[nodiscard]] PersistentRoot retain(ObjectRef reference);
    [[nodiscard]] PersistentRoot retain(Value value);

    void requestCollection() noexcept;
    [[nodiscard]] bool collectionRequested() const noexcept;
    [[nodiscard]] GcPhase phase() const noexcept;
    std::size_t step(const ShadowStack& shadowStack, std::size_t workBudget);
    void collectFull(const ShadowStack& shadowStack);

    [[nodiscard]] const GcStatistics& statistics() const noexcept;
    [[nodiscard]] std::size_t liveObjects() const noexcept;
    [[nodiscard]] std::size_t liveBytes() const noexcept;
    [[nodiscard]] std::uint64_t heapId() const noexcept;
    [[nodiscard]] HeapSnapshot snapshot(
        const ShadowStack* shadowStack = nullptr) const;
    [[nodiscard]] std::vector<ObjectRef> retainingPath(
        ObjectRef target,
        const ShadowStack* shadowStack = nullptr) const;
    [[nodiscard]] std::string leakSummary() const;

private:
    friend class PersistentRoot;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class PersistentRoot {
public:
    PersistentRoot() = default;
    ~PersistentRoot();
    PersistentRoot(PersistentRoot&& other) noexcept;
    PersistentRoot& operator=(PersistentRoot&& other) noexcept;
    PersistentRoot(const PersistentRoot&) = delete;
    PersistentRoot& operator=(const PersistentRoot&) = delete;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] ObjectRef reference() const noexcept;
    [[nodiscard]] const Value& value() const noexcept;
    bool update(ObjectRef reference);
    bool update(Value value);
    void reset() noexcept;

private:
    friend class ManagedHeap;
    PersistentRoot(ManagedHeap* heap, ManagedHeap::RootToken token, Value value);

    ManagedHeap* heap_ = nullptr;
    ManagedHeap::RootToken token_ = 0;
    Value value_{};
};

class NativeHandleRegistry {
public:
    NativeHandleRegistry();
    NativeHandleRegistry(const NativeHandleRegistry&) = delete;
    NativeHandleRegistry& operator=(const NativeHandleRegistry&) = delete;
    NativeHandleRegistry(NativeHandleRegistry&&) = delete;
    NativeHandleRegistry& operator=(NativeHandleRegistry&&) = delete;

    [[nodiscard]] NativeHandle create(
        semantic::SymbolId typeId,
        std::shared_ptr<void> resource,
        std::string debugName = {});
    [[nodiscard]] std::shared_ptr<void> resolve(
        NativeHandle handle,
        semantic::SymbolId expectedTypeId = 0,
        RuntimeError* error = nullptr) const;
    [[nodiscard]] bool isAlive(NativeHandle handle) const noexcept;
    bool release(NativeHandle handle) noexcept;
    void clear() noexcept;
    [[nodiscard]] std::size_t liveCount() const noexcept;
    [[nodiscard]] std::uint64_t registryId() const noexcept;
    [[nodiscard]] std::string debugName(NativeHandle handle) const;

private:
    struct Slot {
        std::shared_ptr<void> resource;
        semantic::SymbolId typeId = 0;
        std::uint32_t generation = 1;
        std::string debugName;
    };
    std::uint64_t registryId_ = 0;
    std::vector<Slot> slots_;
    std::vector<std::uint32_t> freeSlots_;
};

enum class DeterminismMode : std::uint8_t {
    Off,
    Strict,
    Record,
    Replay,
};

enum class BindingDeterminism : std::uint8_t {
    Deterministic,
    Recordable,
    NonDeterministic,
};

struct ExternalCallRecord {
    semantic::SymbolId symbolId = 0;
    std::string name;
    std::uint64_t argumentHash = 0;
    bool succeeded = false;
    Value result;
    ErrorCode errorCode = ErrorCode::None;
    std::string errorMessage;
};

class ReplayLog {
public:
    void append(ExternalCallRecord record);
    [[nodiscard]] std::vector<ExternalCallRecord> entries() const;
    [[nodiscard]] std::size_t size() const;
    void clear();

private:
    mutable std::mutex mutex_;
    std::vector<ExternalCallRecord> entries_;
};

struct DeterminismOptions {
    DeterminismMode mode = DeterminismMode::Off;
    std::shared_ptr<ReplayLog> replayLog;
    bool canonicalizeFloatingPoint = true;
};

struct Limits {
    std::uint64_t instructionBudget = 1'000'000;
    std::size_t recursionLimit = 256;
    std::size_t gcWorkBudget = 8;
};

struct RuntimeStatistics {
    std::uint64_t instructionsExecuted = 0;
    std::uint64_t functionCalls = 0;
    std::uint64_t externalCalls = 0;
    std::uint64_t branchesTaken = 0;
    std::size_t maximumCallDepth = 0;
    std::uint64_t gcWorkPerformed = 0;
};

enum class TraceEventKind {
    FunctionEnter,
    FunctionExit,
    Instruction,
    Branch,
    ExternalCall,
    GcStep,
    RuntimeError,
};

struct TraceEvent {
    TraceEventKind kind = TraceEventKind::Instruction;
    std::string function;
    std::string operation;
    std::uint64_t instructionIndex = 0;
    std::size_t callDepth = 0;
};

using TraceSink = std::function<void(const TraceEvent&)>;

struct FunctionProfile {
    std::string function;
    std::uint64_t calls = 0;
    std::uint64_t returns = 0;
    std::uint64_t instructions = 0;
    std::uint64_t branches = 0;
    std::uint64_t externalCalls = 0;
    std::uint64_t gcSteps = 0;
    std::uint64_t runtimeErrors = 0;
    std::size_t maximumCallDepth = 0;
};

struct ExecutionProfile {
    std::vector<FunctionProfile> functions;
    std::uint64_t totalEvents = 0;
};

class ProfileCollector {
public:
    void record(const TraceEvent& event);
    [[nodiscard]] ExecutionProfile snapshot() const;
    void reset();

private:
    mutable std::mutex mutex_;
    std::map<std::string, FunctionProfile> functions_;
    std::uint64_t totalEvents_ = 0;
};

struct ExecutionOptions {
    Limits limits;
    TraceSink trace;
    std::shared_ptr<ProfileCollector> profile;
    DeterminismOptions determinism;
    std::shared_ptr<debug::DebugController> debugger;
};

struct ExecutionResult {
    bool succeeded = false;
    Value value;
    RuntimeError error;
    std::uint64_t instructionsExecuted = 0;
    RuntimeStatistics statistics;
    std::uint64_t determinismDigest = 0;
    std::size_t replayEntriesConsumed = 0;
};

using ExternalFunction = std::function<std::optional<Value>(
    const bytecode::FunctionReference&,
    const std::vector<Value>&,
    RuntimeError&)>;

struct ResolvedBinding {
    const ExternalFunction* function = nullptr;
    BindingDeterminism determinism = BindingDeterminism::Deterministic;
};

class BindingRegistry {
public:
    bool bind(
        semantic::SymbolId symbolId,
        ExternalFunction function,
        BindingDeterminism determinism = BindingDeterminism::Deterministic);
    bool bind(
        const std::string& canonicalName,
        ExternalFunction function,
        BindingDeterminism determinism = BindingDeterminism::Deterministic);
    [[nodiscard]] std::optional<Value> invoke(
        const bytecode::FunctionReference& reference,
        const std::vector<Value>& arguments,
        RuntimeError& error) const;
    [[nodiscard]] std::optional<ResolvedBinding> resolve(
        const bytecode::FunctionReference& reference) const;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    struct Entry {
        ExternalFunction function;
        BindingDeterminism determinism = BindingDeterminism::Deterministic;
    };
    std::unordered_map<semantic::SymbolId, Entry> bySymbol_;
    std::unordered_map<std::string, Entry> byName_;
};

class DeterminismSession {
public:
    explicit DeterminismSession(DeterminismOptions options = {});
    ~DeterminismSession();
    DeterminismSession(DeterminismSession&&) noexcept;
    DeterminismSession& operator=(DeterminismSession&&) noexcept;
    DeterminismSession(const DeterminismSession&) = delete;
    DeterminismSession& operator=(const DeterminismSession&) = delete;

    void observe(const TraceEvent& event);
    [[nodiscard]] bool invokeExternal(
        const BindingRegistry* bindings,
        const ExternalFunction* fallback,
        const bytecode::FunctionReference& reference,
        const std::vector<Value>& arguments,
        std::optional<Value>& result,
        RuntimeError& error);
    [[nodiscard]] bool finish(
        bool succeeded,
        const Value& value,
        RuntimeError& error,
        const RuntimeStatistics& statistics);
    [[nodiscard]] std::uint64_t digest() const noexcept;
    [[nodiscard]] std::size_t replayEntriesConsumed() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class ProgramImage {
public:
    static std::optional<ProgramImage> link(
        std::vector<bytecode::Module> modules,
        RuntimeError& error);

    [[nodiscard]] const std::vector<bytecode::Module>& modules() const noexcept;
    [[nodiscard]] std::size_t functionCount() const noexcept;
    [[nodiscard]] std::size_t moduleCount() const noexcept;
    [[nodiscard]] bool contains(semantic::SymbolId symbolId) const noexcept;
    [[nodiscard]] std::optional<semantic::SymbolId> findFunction(
        const std::string& qualifiedName) const;

private:
    explicit ProgramImage(std::vector<bytecode::Module> modules);

    std::vector<bytecode::Module> modules_;
    std::unordered_map<semantic::SymbolId, std::string> functions_;
    std::unordered_map<std::string, semantic::SymbolId> names_;
};

class Interpreter {
public:
    explicit Interpreter(std::vector<bytecode::Module> modules);
    explicit Interpreter(std::shared_ptr<const ProgramImage> program);
    Interpreter(
        std::shared_ptr<const ProgramImage> program,
        std::shared_ptr<ManagedHeap> heap);

    void setExternalResolver(ExternalFunction resolver);
    void setBindingRegistry(std::shared_ptr<const BindingRegistry> bindings);
    void setHeap(std::shared_ptr<ManagedHeap> heap);
    [[nodiscard]] std::shared_ptr<ManagedHeap> heap() const noexcept;

    [[nodiscard]] ExecutionResult invoke(
        semantic::SymbolId symbolId,
        const std::vector<Value>& arguments = {},
        Limits limits = {}) const;
    [[nodiscard]] ExecutionResult invoke(
        semantic::SymbolId symbolId,
        const std::vector<Value>& arguments,
        ExecutionOptions options) const;
    [[nodiscard]] ExecutionResult invoke(
        const std::string& qualifiedName,
        const std::vector<Value>& arguments = {},
        Limits limits = {}) const;
    [[nodiscard]] ExecutionResult invoke(
        const std::string& qualifiedName,
        const std::vector<Value>& arguments,
        ExecutionOptions options) const;

private:
    std::vector<bytecode::Module> modules_;
    std::shared_ptr<const ProgramImage> program_;
    ExternalFunction externalResolver_;
    std::shared_ptr<const BindingRegistry> bindings_;
    std::shared_ptr<ManagedHeap> heap_;
};

class EngineRuntime {
public:
    explicit EngineRuntime(std::shared_ptr<const ProgramImage> program);

    void setBindings(std::shared_ptr<const BindingRegistry> bindings);
    void setHeap(std::shared_ptr<ManagedHeap> heap);
    [[nodiscard]] std::shared_ptr<ManagedHeap> heap() const noexcept;
    void setNativeHandles(std::shared_ptr<NativeHandleRegistry> handles);
    [[nodiscard]] std::shared_ptr<NativeHandleRegistry> nativeHandles() const noexcept;
    [[nodiscard]] ExecutionResult invoke(
        const std::string& qualifiedName,
        const std::vector<Value>& arguments = {},
        ExecutionOptions options = {}) const;
    [[nodiscard]] const ProgramImage& program() const noexcept;
    [[nodiscard]] std::shared_ptr<const ProgramImage> programSnapshot() const noexcept;
    void replaceProgram(std::shared_ptr<const ProgramImage> program);

private:
    mutable std::mutex programMutex_;
    std::shared_ptr<const ProgramImage> program_;
    std::vector<std::shared_ptr<const ProgramImage>> retiredPrograms_;
    std::shared_ptr<const BindingRegistry> bindings_;
    std::shared_ptr<ManagedHeap> heap_;
    std::shared_ptr<NativeHandleRegistry> nativeHandles_;
};

[[nodiscard]] const char* errorCodeName(ErrorCode code) noexcept;
[[nodiscard]] const char* traceEventKindName(TraceEventKind kind) noexcept;
[[nodiscard]] semantic::PrimitiveType valueType(const Value& value) noexcept;
[[nodiscard]] std::string valueToString(const Value& value);
[[nodiscard]] std::string valueToString(const Value& value, const ManagedHeap* heap);
[[nodiscard]] std::uint64_t stableValueHash(const Value& value) noexcept;
[[nodiscard]] bool isReplayStableValue(const Value& value) noexcept;
[[nodiscard]] const char* determinismModeName(DeterminismMode mode) noexcept;
[[nodiscard]] const char* bindingDeterminismName(BindingDeterminism determinism) noexcept;
[[nodiscard]] std::string formatProfile(const ExecutionProfile& profile);
[[nodiscard]] std::string profileToJson(const ExecutionProfile& profile);

} // namespace realscript::runtime
