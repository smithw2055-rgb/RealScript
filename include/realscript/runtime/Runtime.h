#pragma once

#include "realscript/bytecode/Bytecode.h"
#include "realscript/semantic/Semantic.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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

struct ByteValue {
    std::uint8_t value = 0;
    friend constexpr bool operator==(ByteValue left, ByteValue right) noexcept {
        return left.value == right.value;
    }
};
struct SByteValue {
    std::int8_t value = 0;
    friend constexpr bool operator==(SByteValue left, SByteValue right) noexcept {
        return left.value == right.value;
    }
};
struct ShortValue {
    std::int16_t value = 0;
    friend constexpr bool operator==(ShortValue left, ShortValue right) noexcept {
        return left.value == right.value;
    }
};
struct UShortValue {
    std::uint16_t value = 0;
    friend constexpr bool operator==(UShortValue left, UShortValue right) noexcept {
        return left.value == right.value;
    }
};
struct UIntValue {
    std::uint32_t value = 0;
    friend constexpr bool operator==(UIntValue left, UIntValue right) noexcept {
        return left.value == right.value;
    }
};
struct ULongValue {
    std::uint64_t value = 0;
    friend constexpr bool operator==(ULongValue left, ULongValue right) noexcept {
        return left.value == right.value;
    }
};
struct FloatValue {
    float value = 0.0f;
    friend constexpr bool operator==(FloatValue left, FloatValue right) noexcept {
        return left.value == right.value;
    }
};
struct CharValue {
    char16_t value = 0;
    friend constexpr bool operator==(CharValue left, CharValue right) noexcept {
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

struct FixedValue {
    std::int32_t raw = 0;
    friend constexpr bool operator==(FixedValue left, FixedValue right) noexcept {
        return left.raw == right.raw;
    }
    friend constexpr bool operator!=(FixedValue left, FixedValue right) noexcept {
        return left.raw != right.raw;
    }
};

struct Fixed16Value {
    std::int64_t raw = 0;
    friend constexpr bool operator==(Fixed16Value left, Fixed16Value right) noexcept {
        return left.raw == right.raw;
    }
    friend constexpr bool operator!=(Fixed16Value left, Fixed16Value right) noexcept {
        return left.raw != right.raw;
    }
};

struct Vec2Value {
    std::int32_t x = 0;
    std::int32_t y = 0;
    friend constexpr bool operator==(Vec2Value left, Vec2Value right) noexcept {
        return left.x == right.x && left.y == right.y;
    }
    friend constexpr bool operator!=(Vec2Value left, Vec2Value right) noexcept {
        return !(left == right);
    }
};

struct Vec3Value {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
    friend constexpr bool operator==(Vec3Value left, Vec3Value right) noexcept {
        return left.x == right.x && left.y == right.y && left.z == right.z;
    }
    friend constexpr bool operator!=(Vec3Value left, Vec3Value right) noexcept {
        return !(left == right);
    }
};

struct QuatValue {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
    std::int32_t w = 0;
    friend constexpr bool operator==(QuatValue left, QuatValue right) noexcept {
        return left.x == right.x && left.y == right.y &&
            left.z == right.z && left.w == right.w;
    }
    friend constexpr bool operator!=(QuatValue left, QuatValue right) noexcept {
        return !(left == right);
    }
};

struct StructStorage;
class ValueSpan;

struct InlineStructSlot {
    enum class Kind : std::uint8_t {
        None = 0,
        Bool,
        Byte,
        SByte,
        Short,
        UShort,
        Int,
        UInt,
        Long,
        ULong,
        Float,
        Double,
        Char,
        Enum,
        Fixed,
        Fixed16,
        Vec2,
        Vec3,
        Quat,
    };

    std::uint32_t lo = 0;
    std::uint32_t hi = 0;
    std::uint8_t aux0 = 0;
    std::uint8_t aux1 = 0;
    std::uint8_t aux2 = 0;
    Kind kind = Kind::None;

    [[nodiscard]] constexpr std::uint64_t bits() const noexcept {
        return (static_cast<std::uint64_t>(hi) << 32u) |
            static_cast<std::uint64_t>(lo);
    }
    constexpr void setBits(std::uint64_t value) noexcept {
        lo = static_cast<std::uint32_t>(value & 0xffffffffULL);
        hi = static_cast<std::uint32_t>(value >> 32u);
    }
    [[nodiscard]] constexpr std::int32_t auxInt24() const noexcept {
        const std::uint32_t u = static_cast<std::uint32_t>(aux0) |
            (static_cast<std::uint32_t>(aux1) << 8u) |
            (static_cast<std::uint32_t>(aux2) << 16u);
        return (u & 0x800000u) != 0
            ? static_cast<std::int32_t>(u | 0xFF000000u)
            : static_cast<std::int32_t>(u);
    }
    constexpr void setAuxInt24(std::int32_t value) noexcept {
        const auto u = static_cast<std::uint32_t>(value);
        aux0 = static_cast<std::uint8_t>(u & 0xFFu);
        aux1 = static_cast<std::uint8_t>((u >> 8u) & 0xFFu);
        aux2 = static_cast<std::uint8_t>((u >> 16u) & 0xFFu);
    }

    friend constexpr bool operator==(
        const InlineStructSlot& left,
        const InlineStructSlot& right) noexcept {
        return left.kind == right.kind &&
            left.lo == right.lo &&
            left.hi == right.hi &&
            left.aux0 == right.aux0 &&
            left.aux1 == right.aux1 &&
            left.aux2 == right.aux2;
    }
    friend constexpr bool operator!=(
        const InlineStructSlot& left,
        const InlineStructSlot& right) noexcept {
        return !(left == right);
    }
};

struct StructValue {
    static constexpr std::size_t MaxInlineFields = 4;
    static constexpr std::size_t MaxInlineSlots = 4;

    semantic::SymbolId typeId = 0;
    semantic::SymbolId nestedTypeId = 0;
    std::shared_ptr<const StructStorage> storage;
    std::uint8_t inlineCount = 0;
    std::uint8_t inlineSlotCount = 0;
    bool isInline = false;
    std::uint8_t packedOffsetsAndLengths = 0; // 2 bits offset per field
    std::uint16_t nestedFieldMask = 0;        // low byte: 2 bits length per field; high byte: 1 + nested field index
    std::array<InlineStructSlot, MaxInlineSlots> inlineFields{};

    StructValue() = default;
    StructValue(
        semantic::SymbolId id,
        std::shared_ptr<const StructStorage> store) noexcept
        : typeId(id), storage(std::move(store)), inlineCount(0), isInline(false) {}

    [[nodiscard]] bool valid() const noexcept {
        return typeId != 0 && (isInline || storage != nullptr);
    }
    [[nodiscard]] std::size_t fieldCount() const noexcept;

    [[nodiscard]] constexpr std::uint8_t fieldOffset(std::size_t idx) const noexcept {
        if (nestedFieldMask == 0) return static_cast<std::uint8_t>(idx);
        return static_cast<std::uint8_t>((packedOffsetsAndLengths >> (idx * 2)) & 0x3u);
    }
    [[nodiscard]] constexpr std::uint8_t fieldLength(std::size_t idx) const noexcept {
        if (nestedFieldMask == 0) return 1;
        return static_cast<std::uint8_t>(((nestedFieldMask >> (idx * 2)) & 0x3u));
    }

    friend bool operator==(const StructValue& left, const StructValue& right);
};

using Value = std::variant<
    std::monostate,
    NullString,
    NullObject,
    NullArray,
    bool,
    ByteValue,
    SByteValue,
    ShortValue,
    UShortValue,
    std::int64_t,
    UIntValue,
    LongValue,
    ULongValue,
    FloatValue,
    double,
    CharValue,
    EnumValue,
    FixedValue,
    Fixed16Value,
    Vec2Value,
    Vec3Value,
    QuatValue,
    StructValue,
    std::string,
    ObjectRef,
    NativeHandle>;

class ValueSpan {
public:
    constexpr ValueSpan() noexcept = default;
    constexpr ValueSpan(const Value* data, std::size_t size) noexcept
        : data_(data), size_(size) {}
    ValueSpan(const std::vector<Value>& values) noexcept
        : data_(values.data()), size_(values.size()) {}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winit-list-lifetime"
#endif
    ValueSpan(std::initializer_list<Value> values) noexcept
        : data_(values.begin()), size_(values.size()) {}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
    template <std::size_t N>
    constexpr ValueSpan(const std::array<Value, N>& values) noexcept
        : data_(values.data()), size_(N) {}

    [[nodiscard]] constexpr const Value* data() const noexcept { return data_; }
    [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }
    [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] constexpr const Value& operator[](std::size_t index) const noexcept {
        return data_[index];
    }
    [[nodiscard]] constexpr const Value& front() const noexcept { return data_[0]; }
    [[nodiscard]] constexpr const Value& back() const noexcept { return data_[size_ - 1]; }
    [[nodiscard]] constexpr const Value* begin() const noexcept { return data_; }
    [[nodiscard]] constexpr const Value* end() const noexcept { return data_ + size_; }

private:
    const Value* data_ = nullptr;
    std::size_t size_ = 0;
};

struct StructStorage {
    std::vector<Value> fields;
};

[[nodiscard]] std::optional<InlineStructSlot> encodeInlineStructSlot(
    const Value& value) noexcept;
[[nodiscard]] Value decodeInlineStructSlot(
    const InlineStructSlot& slot) noexcept;
[[nodiscard]] StructValue makeStructValueFromFields(
    semantic::SymbolId typeId,
    ValueSpan fields);
[[nodiscard]] StructValue makeStructValueFromFields(
    semantic::SymbolId typeId,
    std::vector<Value> fields);
[[nodiscard]] std::optional<Value> structValueFieldAt(
    const StructValue& structure,
    std::size_t index) noexcept;

inline std::size_t StructValue::fieldCount() const noexcept {
    if (isInline) return inlineCount;
    return storage ? storage->fields.size() : 0;
}

inline bool operator==(const StructValue& left, const StructValue& right) {
    if (left.typeId != right.typeId) return false;
    const auto count = left.fieldCount();
    if (count != right.fieldCount()) return false;
    if (left.isInline && right.isInline &&
        left.nestedTypeId == right.nestedTypeId &&
        left.inlineSlotCount == right.inlineSlotCount &&
        left.packedOffsetsAndLengths == right.packedOffsetsAndLengths &&
        left.nestedFieldMask == right.nestedFieldMask) {
        const std::size_t slots = left.inlineSlotCount != 0 ? left.inlineSlotCount : count;
        for (std::size_t i = 0; i < slots; ++i) {
            if (left.inlineFields[i] != right.inlineFields[i]) return false;
        }
        return true;
    }
    if (!left.isInline && !right.isInline) {
        if (left.storage == right.storage) return true;
        if (!left.storage || !right.storage) return false;
        return left.storage->fields == right.storage->fields;
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (!(structValueFieldAt(left, i) == structValueFieldAt(right, i))) {
            return false;
        }
    }
    return true;
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
    ScriptException,
    CapabilityDenied,
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
    std::uint64_t youngCollections = 0;
    std::uint64_t youngObjectsReclaimed = 0;
    std::uint64_t objectsPromoted = 0;
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
    std::string stringValue;
    std::vector<Value> values;
    std::vector<std::size_t> referenceFields;
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
    [[nodiscard]] std::optional<ObjectRef> internString(
        std::string_view value,
        RuntimeError* error = nullptr);
    [[nodiscard]] std::size_t internedStringCount() const noexcept;
    [[nodiscard]] std::size_t sharedTypeLayoutCount() const noexcept;
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

    [[nodiscard]] bool hasStaticField(semantic::SymbolId id) const noexcept;
    [[nodiscard]] std::optional<Value> getStaticField(semantic::SymbolId id) const;
    void setStaticField(semantic::SymbolId id, Value value);

    // O(1) Native Unmanaged Sparse-Set ECS Storage (zero per-mutation array allocation)
    bool setNativeEcsComponent(
        std::string_view canonicalTypeName,
        std::int64_t entityId,
        Value component);
    [[nodiscard]] std::optional<Value> getNativeEcsComponent(
        std::string_view canonicalTypeName,
        std::int64_t entityId) const;
    [[nodiscard]] bool hasNativeEcsComponent(
        std::string_view canonicalTypeName,
        std::int64_t entityId) const noexcept;
    bool removeNativeEcsComponent(
        std::string_view canonicalTypeName,
        std::int64_t entityId);
    bool destroyNativeEcsEntity(std::int64_t entityId);
    [[nodiscard]] bool hasNativeEcsTable(std::string_view canonicalTypeName) const noexcept;
    [[nodiscard]] const std::vector<std::int64_t>* nativeEcsEntityIds(
        std::string_view canonicalTypeName) const noexcept;
    [[nodiscard]] const std::vector<Value>* nativeEcsComponentValues(
        std::string_view canonicalTypeName) const noexcept;
    bool setNativeEcsComponentByDenseIndex(
        std::string_view canonicalTypeName,
        std::size_t denseIndex,
        Value component);

    void requestCollection() noexcept;
    [[nodiscard]] bool collectionRequested() const noexcept;
    [[nodiscard]] GcPhase phase() const noexcept;
    std::size_t step(const ShadowStack& shadowStack, std::size_t workBudget);
    void collectFull(const ShadowStack& shadowStack);
    std::size_t collectYoung(const ShadowStack& shadowStack, std::size_t workBudget = 0);
    [[nodiscard]] std::size_t nurserySize() const noexcept;

    bool readPackedIntArray(
        ObjectRef reference,
        std::vector<std::int64_t>& out,
        RuntimeError* error = nullptr) const;
    bool writePackedIntArray(
        ObjectRef reference,
        const std::int64_t* data,
        std::size_t count,
        RuntimeError* error = nullptr);
    bool readPackedDoubleArray(
        ObjectRef reference,
        std::vector<double>& out,
        RuntimeError* error = nullptr) const;
    bool writePackedDoubleArray(
        ObjectRef reference,
        const double* data,
        std::size_t count,
        RuntimeError* error = nullptr);
    bool readPackedVec2Array(
        ObjectRef reference,
        std::vector<Vec2Value>& out,
        RuntimeError* error = nullptr) const;
    bool writePackedVec2Array(
        ObjectRef reference,
        const Vec2Value* data,
        std::size_t count,
        RuntimeError* error = nullptr);

    [[nodiscard]] const GcStatistics& statistics() const noexcept;
    [[nodiscard]] std::size_t liveObjects() const noexcept;
    [[nodiscard]] std::size_t liveBytes() const noexcept;
    [[nodiscard]] std::uint64_t heapId() const noexcept;
    [[nodiscard]] HeapSnapshot snapshot(
        const ShadowStack* shadowStack = nullptr) const;
    bool restore(
        const HeapSnapshot& snapshot,
        RuntimeError* error = nullptr);
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
    bool enforceCapabilities = false;
    std::unordered_set<std::string> grantedCapabilities;
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

using DeterminismOperationId = std::uint64_t;

constexpr DeterminismOperationId determinismOperationId(
    std::string_view operation) noexcept {
    DeterminismOperationId hash = 1469598103934665603ULL;
    for (const unsigned char value : operation) {
        hash ^= value;
        hash *= 1099511628211ULL;
    }
    return hash;
}

constexpr std::uint64_t DeterminismEventSeed = 1469598103934665603ULL;

constexpr std::uint64_t accumulateDeterminismEvent(
    std::uint64_t digest,
    TraceEventKind kind,
    semantic::SymbolId functionId,
    DeterminismOperationId operationId,
    std::uint64_t instructionIndex,
    std::size_t callDepth) noexcept {
    auto event = operationId ^
        (static_cast<std::uint64_t>(functionId) * 0x9e3779b185ebca87ULL);
    event ^= instructionIndex * 0xc2b2ae3d27d4eb4fULL;
    event ^= static_cast<std::uint64_t>(callDepth) << 48U;
    event ^= (static_cast<std::uint64_t>(kind) + 1U) *
        0x165667b19e3779f9ULL;
    return digest ^ (event + 0x9e3779b97f4a7c15ULL +
        (digest << 6U) + (digest >> 2U));
}

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
    void merge(const ExecutionProfile& profile);
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
    bool enforceCapabilities = false;
    std::unordered_set<std::string> grantedCapabilities;
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
    ValueSpan,
    RuntimeError&)>;

struct ResolvedBinding {
    const ExternalFunction* function = nullptr;
    BindingDeterminism determinism = BindingDeterminism::Deterministic;
    std::string requiredCapability;
};

class BindingRegistry {
public:
    bool bind(
        semantic::SymbolId symbolId,
        ExternalFunction function,
        BindingDeterminism determinism = BindingDeterminism::Deterministic,
        std::string requiredCapability = {});
    bool bind(
        const std::string& canonicalName,
        ExternalFunction function,
        BindingDeterminism determinism = BindingDeterminism::Deterministic,
        std::string requiredCapability = {});
    [[nodiscard]] std::optional<Value> invoke(
        const bytecode::FunctionReference& reference,
        ValueSpan arguments,
        RuntimeError& error) const;
    [[nodiscard]] std::optional<ResolvedBinding> resolve(
        const bytecode::FunctionReference& reference) const;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t cachedSymbolCount() const noexcept;

private:
    struct Entry {
        ExternalFunction function;
        BindingDeterminism determinism = BindingDeterminism::Deterministic;
        std::string requiredCapability;
    };
    std::unordered_map<semantic::SymbolId, Entry> bySymbol_;
    std::unordered_map<std::string, Entry> byName_;
    mutable std::unordered_map<semantic::SymbolId, const Entry*> resolvedBySymbolCache_;
    std::size_t entryCount_ = 0;
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
    void mergeEventDigest(
        std::uint64_t eventDigest,
        std::uint64_t eventCount) noexcept;
    [[nodiscard]] bool invokeExternal(
        const BindingRegistry* bindings,
        const ExternalFunction* fallback,
        const bytecode::FunctionReference& reference,
        ValueSpan arguments,
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

class ProgramImage;

namespace detail {
struct ExecutionMirror;
[[nodiscard]] const ExecutionMirror& executionMirror(
    const ProgramImage& program) noexcept;
}

class ProgramImage {
public:
    struct FunctionLocation {
        const bytecode::Module* module = nullptr;
        const bytecode::Function* function = nullptr;
        std::string qualifiedName;
    };

    ProgramImage(const ProgramImage& other);
    ProgramImage& operator=(const ProgramImage& other);
    ProgramImage(ProgramImage&&) noexcept = default;
    ProgramImage& operator=(ProgramImage&&) noexcept = default;

    static std::optional<ProgramImage> link(
        std::vector<bytecode::Module> modules,
        RuntimeError& error);

    [[nodiscard]] const std::vector<bytecode::Module>& modules() const noexcept;
    [[nodiscard]] std::size_t functionCount() const noexcept;
    [[nodiscard]] std::size_t moduleCount() const noexcept;
    [[nodiscard]] bool contains(semantic::SymbolId symbolId) const noexcept;
    [[nodiscard]] std::optional<semantic::SymbolId> findFunction(
        const std::string& qualifiedName) const;
    [[nodiscard]] const FunctionLocation* resolveFunction(
        semantic::SymbolId symbolId) const noexcept;
    [[nodiscard]] const semantic::TypeSymbol* resolveType(
        semantic::SymbolId typeId) const noexcept;

private:
    friend const detail::ExecutionMirror& detail::executionMirror(
        const ProgramImage& program) noexcept;

    explicit ProgramImage(std::vector<bytecode::Module> modules);

    std::vector<bytecode::Module> modules_;
    std::unordered_map<semantic::SymbolId, FunctionLocation> functions_;
    std::unordered_map<semantic::SymbolId, const semantic::TypeSymbol*> types_;
    std::unordered_map<std::string, semantic::SymbolId> names_;
    std::shared_ptr<const detail::ExecutionMirror> executionMirror_;
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
    std::shared_ptr<const ProgramImage> program_;
    RuntimeError programError_;
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
    [[nodiscard]] ExecutionResult invoke(
        semantic::SymbolId symbolId,
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

[[nodiscard]] semantic::SymbolId fixedTypeId() noexcept;
[[nodiscard]] semantic::SymbolId fixed16TypeId() noexcept;
[[nodiscard]] semantic::SymbolId vec2TypeId() noexcept;
[[nodiscard]] semantic::SymbolId vec3TypeId() noexcept;
[[nodiscard]] semantic::SymbolId quatTypeId() noexcept;
[[nodiscard]] std::optional<semantic::SymbolId> structValueTypeId(const Value& value) noexcept;
[[nodiscard]] std::optional<Value> makeDefaultInlineStructValue(semantic::SymbolId typeId) noexcept;
[[nodiscard]] std::optional<Value> loadInlineStructField(const Value& value, std::size_t fieldIndex) noexcept;
[[nodiscard]] std::optional<Value> storeInlineStructField(
    const Value& structure,
    std::size_t fieldIndex,
    const Value& fieldValue) noexcept;

[[nodiscard]] const char* errorCodeName(ErrorCode code) noexcept;
[[nodiscard]] const char* traceEventKindName(TraceEventKind kind) noexcept;
[[nodiscard]] semantic::PrimitiveType valueType(const Value& value) noexcept;
[[nodiscard]] bool tryConvertNumeric(
    const Value& value,
    semantic::PrimitiveType target,
    Value& result,
    bool checkedArithmetic = true) noexcept;
[[nodiscard]] std::string valueToString(const Value& value);
[[nodiscard]] std::string valueToString(const Value& value, const ManagedHeap* heap);
[[nodiscard]] std::uint64_t stableValueHash(const Value& value) noexcept;
[[nodiscard]] bool isReplayStableValue(const Value& value) noexcept;
[[nodiscard]] const char* determinismModeName(DeterminismMode mode) noexcept;
[[nodiscard]] const char* bindingDeterminismName(BindingDeterminism determinism) noexcept;
[[nodiscard]] std::string formatProfile(const ExecutionProfile& profile);
[[nodiscard]] std::string profileToJson(const ExecutionProfile& profile);
[[nodiscard]] std::string exportChromeTraceJson(
    const ExecutionProfile& profile,
    const GcStatistics* gcStatistics = nullptr);

[[nodiscard]] bool tryExecuteRuntimeIntrinsic(
    std::string_view name,
    ValueSpan arguments,
    ManagedHeap* heap,
    Value& result,
    std::string& errorMessage);

} // namespace realscript::runtime
