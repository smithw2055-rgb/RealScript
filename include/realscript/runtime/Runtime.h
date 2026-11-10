#pragma once

#include "realscript/bytecode/Bytecode.h"
#include "realscript/semantic/Semantic.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
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

enum class ValueTag : std::uint8_t {
    Monostate = 0,
    NullString = 1,
    NullObject = 2,
    NullArray = 3,
    Bool = 4,
    Byte = 5,
    SByte = 6,
    Short = 7,
    UShort = 8,
    Int64 = 9,
    UInt = 10,
    Long = 11,
    ULong = 12,
    Float = 13,
    Double = 14,
    Char = 15,
    EnumInline = 16,
    Fixed = 17,
    Fixed16 = 18,
    Vec2 = 19,
    Vec3 = 20,
    QuatInline = 21,
    Float4Inline = 22,
    NativeHandle = 23,
    // Traceable / Pool-backed tags (>= FirstTraceable)
    EnumArena = 24,
    QuatArena = 25,
    Float4Arena = 26,
    Struct = 27,
    String = 28,
    ObjectRef = 29,

    FirstTraceable = EnumArena,
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
    std::uint32_t heapId = 0;
    ObjectKind kind = ObjectKind::Record;
    std::uint8_t pad0_ = 0;
    std::uint8_t pad1_ = 0;
    std::uint8_t vtag_ = static_cast<std::uint8_t>(ValueTag::ObjectRef);

    constexpr ObjectRef() noexcept = default;
    constexpr ObjectRef(
        std::uint32_t s,
        std::uint32_t gen,
        ObjectKind k,
        std::uint64_t hId) noexcept
        : slot(s),
          generation(gen),
          heapId(static_cast<std::uint32_t>(hId)),
          kind(k),
          pad0_(0),
          pad1_(0),
          vtag_(static_cast<std::uint8_t>(ValueTag::ObjectRef)) {}

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
static_assert(sizeof(ObjectRef) == 16, "ObjectRef must be 16 bytes");

struct NativeHandle {
    static constexpr std::uint32_t InvalidSlot = 0xffffffffu;

    std::uint32_t slot = InvalidSlot;
    std::uint32_t generation : 16;
    std::uint32_t registryId : 16;
    semantic::SymbolId typeId = 0;

    constexpr NativeHandle() noexcept
        : slot(InvalidSlot), generation(0), registryId(0), typeId(0) {}
    constexpr NativeHandle(
        std::uint32_t s,
        std::uint32_t gen,
        semantic::SymbolId tId,
        std::uint64_t regId) noexcept
        : slot(s),
          generation(gen == 0 ? 0u : static_cast<std::uint32_t>(((gen - 1u) % 65535u) + 1u)),
          registryId(regId == 0 ? 0u : static_cast<std::uint32_t>(((regId - 1u) % 65535u) + 1u)),
          typeId(tId) {}

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
static_assert(sizeof(NativeHandle) == 16, "NativeHandle must be 16 bytes");

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

struct Float4Value {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;
    friend constexpr bool operator==(Float4Value left, Float4Value right) noexcept {
        return left.x == right.x && left.y == right.y &&
            left.z == right.z && left.w == right.w;
    }
    friend constexpr bool operator!=(Float4Value left, Float4Value right) noexcept {
        return !(left == right);
    }
};
static_assert(sizeof(Float4Value) == 16, "Float4Value must be 16 bytes");

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
        NullString,
        NullObject,
        NullArray,
        String,
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

class StructStorageHandle {
public:
    static constexpr std::uint32_t InlineBit = 0x80000000u;

    constexpr StructStorageHandle() noexcept = default;
    constexpr StructStorageHandle(std::nullptr_t) noexcept : raw_(0) {}
    StructStorageHandle(std::shared_ptr<const StructStorage> store);
    StructStorageHandle(std::shared_ptr<StructStorage> store);

    [[nodiscard]] constexpr bool isStoragePtr() const noexcept {
        return raw_ != 0 && (raw_ & InlineBit) == 0;
    }
    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return isStoragePtr();
    }
    [[nodiscard]] const StructStorage* get() const noexcept;
    [[nodiscard]] const StructStorage* operator->() const noexcept { return get(); }
    [[nodiscard]] const StructStorage& operator*() const noexcept { return *get(); }
    [[nodiscard]] long use_count() const noexcept { return isStoragePtr() ? 2 : 0; }

    [[nodiscard]] constexpr std::uint32_t inlineArenaIndex() const noexcept {
        return (raw_ & InlineBit) != 0 ? (raw_ & ~InlineBit) : 0u;
    }
    constexpr void setInlineArenaIndex(std::uint32_t idx) noexcept {
        raw_ = InlineBit | (idx & ~InlineBit);
    }

    friend constexpr bool operator==(
        StructStorageHandle left,
        StructStorageHandle right) noexcept {
        return left.isStoragePtr() && right.isStoragePtr() &&
            left.raw_ == right.raw_;
    }
    friend constexpr bool operator!=(
        StructStorageHandle left,
        StructStorageHandle right) noexcept {
        return !(left == right);
    }
    friend constexpr bool operator==(
        StructStorageHandle left,
        std::nullptr_t) noexcept {
        return !left.isStoragePtr();
    }
    friend constexpr bool operator==(
        std::nullptr_t,
        StructStorageHandle right) noexcept {
        return !right.isStoragePtr();
    }
    friend constexpr bool operator!=(
        StructStorageHandle left,
        std::nullptr_t) noexcept {
        return left.isStoragePtr();
    }
    friend constexpr bool operator!=(
        std::nullptr_t,
        StructStorageHandle right) noexcept {
        return right.isStoragePtr();
    }

    std::uint32_t raw_ = 0;
};

struct InlineStructArenaEntry {
    semantic::SymbolId nestedTypeId = 0;
    std::uint16_t nestedFieldMask = 0;
    std::array<InlineStructSlot, 4> slots{};
};

struct StructValue {
    static constexpr std::size_t MaxInlineFields = 4;
    static constexpr std::size_t MaxInlineSlots = 4;

    semantic::SymbolId typeId = 0;
    StructStorageHandle storage{};
    std::uint8_t inlineCount : 4;
    std::uint8_t inlineSlotCount : 4;
    bool isInline = false;
    std::uint8_t packedOffsetsAndLengths = 0; // 2 bits offset per field
    std::uint8_t vtag_ = static_cast<std::uint8_t>(ValueTag::Struct);

    constexpr StructValue() noexcept
        : typeId(0),
          storage{},
          inlineCount(0),
          inlineSlotCount(0),
          isInline(false),
          packedOffsetsAndLengths(0),
          vtag_(static_cast<std::uint8_t>(ValueTag::Struct)) {}
    StructValue(
        semantic::SymbolId id,
        StructStorageHandle store) noexcept
        : typeId(id),
          storage(store),
          inlineCount(0),
          inlineSlotCount(0),
          isInline(false),
          packedOffsetsAndLengths(0),
          vtag_(static_cast<std::uint8_t>(ValueTag::Struct)) {}

    [[nodiscard]] bool valid() const noexcept {
        return typeId != 0 && (isInline || storage != nullptr);
    }
    [[nodiscard]] std::size_t fieldCount() const noexcept;
    [[nodiscard]] const InlineStructArenaEntry& inlineEntry() const noexcept;
    [[nodiscard]] std::uint16_t nestedFieldMask() const noexcept;
    [[nodiscard]] semantic::SymbolId nestedTypeId() const noexcept;

    [[nodiscard]] std::uint8_t fieldOffset(std::size_t idx) const noexcept {
        if (nestedFieldMask() == 0) return static_cast<std::uint8_t>(idx);
        return static_cast<std::uint8_t>((packedOffsetsAndLengths >> (idx * 2)) & 0x3u);
    }
    [[nodiscard]] std::uint8_t fieldLength(std::size_t idx) const noexcept {
        const auto mask = nestedFieldMask();
        if (mask == 0) return 1;
        return static_cast<std::uint8_t>(((mask >> (idx * 2)) & 0x3u));
    }

    friend bool operator==(const StructValue& left, const StructValue& right);
    friend bool operator!=(const StructValue& left, const StructValue& right) {
        return !(left == right);
    }
};
static_assert(sizeof(StructValue) == 16, "StructValue must be 16 bytes");
static_assert(offsetof(StructValue, vtag_) == 15, "StructValue::vtag_ must reside at byte 15");

struct StringValue {
    const std::string* ptr = nullptr;
    std::uint32_t id = 0;
    std::uint8_t pad0_ = 0;
    std::uint8_t pad1_ = 0;
    std::uint8_t pad2_ = 0;
    std::uint8_t vtag_ = static_cast<std::uint8_t>(ValueTag::String);

    constexpr StringValue() noexcept = default;
    StringValue(const std::string& value);
    StringValue(std::string&& value);
    StringValue(std::string_view value);

    [[nodiscard]] const std::string& str() const noexcept;
    [[nodiscard]] std::string_view view() const noexcept { return str(); }
    operator const std::string&() const noexcept { return str(); }
    operator std::string_view() const noexcept { return str(); }
    [[nodiscard]] const char* c_str() const noexcept { return str().c_str(); }
    [[nodiscard]] const char* data() const noexcept { return str().data(); }
    [[nodiscard]] std::size_t size() const noexcept { return str().size(); }
    [[nodiscard]] bool empty() const noexcept { return str().empty(); }
    [[nodiscard]] auto begin() const noexcept { return str().begin(); }
    [[nodiscard]] auto end() const noexcept { return str().end(); }
    [[nodiscard]] bool isAsciiFast() const noexcept;
    [[nodiscard]] std::size_t codePointLength() const noexcept;
    [[nodiscard]] std::string_view codePointSlice(
        std::size_t startCodePoint,
        std::size_t codePointCount = std::string_view::npos) const noexcept;
    [[nodiscard]] std::int64_t codePointIndexOf(std::string_view needle) const noexcept;
    [[nodiscard]] std::int64_t codePointLastIndexOf(std::string_view needle) const noexcept;

    friend bool operator==(const StringValue& left, const StringValue& right) noexcept {
        if (left.id != 0 && left.id == right.id) return true;
        return left.str() == right.str();
    }
    friend bool operator!=(const StringValue& left, const StringValue& right) noexcept {
        return !(left == right);
    }
    friend bool operator==(const StringValue& left, const std::string& right) noexcept {
        return left.str() == right;
    }
    friend bool operator==(const std::string& left, const StringValue& right) noexcept {
        return left == right.str();
    }
    friend bool operator!=(const StringValue& left, const std::string& right) noexcept {
        return left.str() != right;
    }
    friend bool operator!=(const std::string& left, const StringValue& right) noexcept {
        return left != right.str();
    }
    friend bool operator==(const StringValue& left, std::string_view right) noexcept {
        return std::string_view(left.str()) == right;
    }
    friend bool operator==(std::string_view left, const StringValue& right) noexcept {
        return left == std::string_view(right.str());
    }
    friend bool operator==(const StringValue& left, const char* right) noexcept {
        return left.str() == (right ? right : "");
    }
    friend bool operator==(const char* left, const StringValue& right) noexcept {
        return (left ? left : "") == right.str();
    }
    friend bool operator!=(const StringValue& left, const char* right) noexcept {
        return !(left == right);
    }
    friend bool operator!=(const char* left, const StringValue& right) noexcept {
        return !(left == right);
    }
};
static_assert(sizeof(StringValue) == 16, "StringValue must be 16 bytes");
static_assert(offsetof(StringValue, vtag_) == 15, "StringValue::vtag_ must reside at byte 15");
static_assert(offsetof(ObjectRef, vtag_) == 15, "ObjectRef::vtag_ must reside at byte 15");

namespace detail {
[[nodiscard]] std::uint32_t internArenaQuat(QuatValue value) noexcept;
[[nodiscard]] QuatValue lookupArenaQuat(std::uint32_t arenaIdx) noexcept;
[[nodiscard]] std::uint32_t internArenaFloat4(Float4Value value) noexcept;
[[nodiscard]] Float4Value lookupArenaFloat4(std::uint32_t arenaIdx) noexcept;
[[nodiscard]] std::uint32_t internArenaEnum(EnumValue value) noexcept;
[[nodiscard]] EnumValue lookupArenaEnum(std::uint32_t arenaIdx) noexcept;
[[nodiscard]] std::uint32_t internNativeHandleTypeId(semantic::SymbolId typeId) noexcept;
[[nodiscard]] semantic::SymbolId lookupNativeHandleTypeId(std::uint32_t typeIdx) noexcept;

struct SharedValuePoolBridge {
    std::uint32_t version = 1u;
    std::uint32_t (*internArenaQuatFn)(QuatValue) noexcept = nullptr;
    QuatValue (*lookupArenaQuatFn)(std::uint32_t) noexcept = nullptr;
    std::uint32_t (*internArenaFloat4Fn)(Float4Value) noexcept = nullptr;
    Float4Value (*lookupArenaFloat4Fn)(std::uint32_t) noexcept = nullptr;
    std::uint32_t (*internArenaEnumFn)(EnumValue) noexcept = nullptr;
    EnumValue (*lookupArenaEnumFn)(std::uint32_t) noexcept = nullptr;
    std::uint32_t (*internNativeHandleTypeIdFn)(semantic::SymbolId) noexcept = nullptr;
    semantic::SymbolId (*lookupNativeHandleTypeIdFn)(std::uint32_t) noexcept = nullptr;
    StringValue (*internStringFn)(std::string_view) = nullptr;
    StringValue (*stringByIdFn)(std::uint32_t) noexcept = nullptr;
    std::uint32_t (*internInlineArenaFn)(const InlineStructArenaEntry&) noexcept = nullptr;
    const InlineStructArenaEntry* (*lookupInlineArenaFn)(std::uint32_t) noexcept = nullptr;
    std::uint32_t (*registerHeapStructFn)(const void*) = nullptr;
    const void* (*lookupHeapStructFn)(std::uint32_t) noexcept = nullptr;
};

[[nodiscard]] const SharedValuePoolBridge* hostValuePoolBridge() noexcept;
void bindHostValuePoolBridge(const void* bridgePtr) noexcept;

template <typename T>
struct ValueTaggedCell {
    static_assert(sizeof(T) <= 12, "ValueTaggedCell requires sizeof(T) <= 12");
    T payload{};
    std::uint8_t pad[15 - sizeof(T)]{};
    std::uint8_t vtag = 0;
};
} // namespace detail

struct alignas(16) Value {
    struct EnumInlineCell {
        semantic::SymbolId typeId = 0;
        std::int32_t value32 = 0;
        std::uint8_t pad[3]{};
        std::uint8_t vtag = 0;
    };
    struct QuatInlineCell {
        std::int32_t x = 0;
        std::int32_t y = 0;
        std::int32_t z = 0;
        std::uint8_t w0 = 0;
        std::uint8_t w1 = 0;
        std::uint8_t w2 = 0;
        std::uint8_t vtag = 0;
    };
    struct Float4InlineCell {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        std::uint8_t w0 = 0;
        std::uint8_t w1 = 0;
        std::uint8_t w2 = 0;
        std::uint8_t vtag = 0;
    };
    struct NativeHandleCell {
        std::uint32_t slot = NativeHandle::InvalidSlot;
        std::uint16_t generation = 0;
        std::uint16_t registryId = 0;
        std::uint32_t typeIdx = 0;
        std::uint8_t pad[3]{};
        std::uint8_t vtag = 0;
    };
    struct ArenaRefCell {
        std::uint32_t arenaIdx = 0;
        std::uint8_t pad[11]{};
        std::uint8_t vtag = 0;
    };

    union Storage {
        detail::ValueTaggedCell<std::monostate> monostateCell{};
        detail::ValueTaggedCell<NullString> nullStringCell;
        detail::ValueTaggedCell<NullObject> nullObjectCell;
        detail::ValueTaggedCell<NullArray> nullArrayCell;
        detail::ValueTaggedCell<bool> boolCell;
        detail::ValueTaggedCell<ByteValue> byteCell;
        detail::ValueTaggedCell<SByteValue> sbyteCell;
        detail::ValueTaggedCell<ShortValue> shortCell;
        detail::ValueTaggedCell<UShortValue> ushortCell;
        detail::ValueTaggedCell<std::int64_t> int64Cell;
        detail::ValueTaggedCell<UIntValue> uintCell;
        detail::ValueTaggedCell<LongValue> longCell;
        detail::ValueTaggedCell<ULongValue> ulongCell;
        detail::ValueTaggedCell<FloatValue> floatCell;
        detail::ValueTaggedCell<double> doubleCell;
        detail::ValueTaggedCell<CharValue> charCell;
        EnumInlineCell enumInlineCell;
        detail::ValueTaggedCell<FixedValue> fixedCell;
        detail::ValueTaggedCell<Fixed16Value> fixed16Cell;
        detail::ValueTaggedCell<Vec2Value> vec2Cell;
        detail::ValueTaggedCell<Vec3Value> vec3Cell;
        QuatInlineCell quatInlineCell;
        Float4InlineCell float4InlineCell;
        NativeHandleCell nativeHandleCell;
        ArenaRefCell arenaRefCell;
        StructValue structVal;
        StringValue stringVal;
        ObjectRef objectRef;
    } u_{};

    constexpr Value() noexcept = default;
    constexpr Value(const Value&) noexcept = default;
    constexpr Value(Value&&) noexcept = default;
    constexpr Value& operator=(const Value&) noexcept = default;
    constexpr Value& operator=(Value&&) noexcept = default;

    constexpr Value(std::monostate) noexcept : u_{} {}

    constexpr Value(NullString) noexcept {
        u_.nullStringCell = {NullString{}, {}, static_cast<std::uint8_t>(ValueTag::NullString)};
    }
    constexpr Value(NullObject) noexcept {
        u_.nullObjectCell = {NullObject{}, {}, static_cast<std::uint8_t>(ValueTag::NullObject)};
    }
    constexpr Value(NullArray) noexcept {
        u_.nullArrayCell = {NullArray{}, {}, static_cast<std::uint8_t>(ValueTag::NullArray)};
    }

    template <
        typename B,
        std::enable_if_t<std::is_same_v<std::remove_cv_t<std::remove_reference_t<B>>, bool>, int> = 0>
    constexpr Value(B v) noexcept {
        u_.boolCell = {v, {}, static_cast<std::uint8_t>(ValueTag::Bool)};
    }

    constexpr Value(ByteValue v) noexcept {
        u_.byteCell = {v, {}, static_cast<std::uint8_t>(ValueTag::Byte)};
    }
    constexpr Value(SByteValue v) noexcept {
        u_.sbyteCell = {v, {}, static_cast<std::uint8_t>(ValueTag::SByte)};
    }
    constexpr Value(ShortValue v) noexcept {
        u_.shortCell = {v, {}, static_cast<std::uint8_t>(ValueTag::Short)};
    }
    constexpr Value(UShortValue v) noexcept {
        u_.ushortCell = {v, {}, static_cast<std::uint8_t>(ValueTag::UShort)};
    }

    template <
        typename I,
        std::enable_if_t<
            std::is_integral_v<std::remove_cv_t<std::remove_reference_t<I>>> &&
            std::is_signed_v<std::remove_cv_t<std::remove_reference_t<I>>> &&
            !std::is_same_v<std::remove_cv_t<std::remove_reference_t<I>>, bool> &&
            !std::is_same_v<std::remove_cv_t<std::remove_reference_t<I>>, char> &&
            !std::is_same_v<std::remove_cv_t<std::remove_reference_t<I>>, char16_t> &&
            !std::is_same_v<std::remove_cv_t<std::remove_reference_t<I>>, char32_t> &&
            !std::is_same_v<std::remove_cv_t<std::remove_reference_t<I>>, wchar_t>,
            int> = 0>
    constexpr Value(I v) noexcept {
        u_.int64Cell = {static_cast<std::int64_t>(v), {}, static_cast<std::uint8_t>(ValueTag::Int64)};
    }

    constexpr Value(UIntValue v) noexcept {
        u_.uintCell = {v, {}, static_cast<std::uint8_t>(ValueTag::UInt)};
    }
    constexpr Value(LongValue v) noexcept {
        u_.longCell = {v, {}, static_cast<std::uint8_t>(ValueTag::Long)};
    }
    constexpr Value(ULongValue v) noexcept {
        u_.ulongCell = {v, {}, static_cast<std::uint8_t>(ValueTag::ULong)};
    }
    constexpr Value(FloatValue v) noexcept {
        u_.floatCell = {v, {}, static_cast<std::uint8_t>(ValueTag::Float)};
    }

    template <
        typename F,
        std::enable_if_t<std::is_floating_point_v<std::remove_cv_t<std::remove_reference_t<F>>>, int> = 0>
    constexpr Value(F v) noexcept {
        u_.doubleCell = {static_cast<double>(v), {}, static_cast<std::uint8_t>(ValueTag::Double)};
    }

    constexpr Value(CharValue v) noexcept {
        u_.charCell = {v, {}, static_cast<std::uint8_t>(ValueTag::Char)};
    }

    Value(EnumValue v) noexcept {
#if defined(__GNUC__) || defined(__clang__)
        if (__builtin_expect(v.value >= -2147483647LL - 1LL && v.value <= 2147483647LL, 1))
#else
        if (v.value >= -2147483647LL - 1LL && v.value <= 2147483647LL)
#endif
        {
            u_.enumInlineCell = {
                v.typeId,
                static_cast<std::int32_t>(v.value),
                {},
                static_cast<std::uint8_t>(ValueTag::EnumInline)};
        } else {
            u_.arenaRefCell = {
                detail::internArenaEnum(v),
                {},
                static_cast<std::uint8_t>(ValueTag::EnumArena)};
        }
    }

    constexpr Value(FixedValue v) noexcept {
        u_.fixedCell = {v, {}, static_cast<std::uint8_t>(ValueTag::Fixed)};
    }
    constexpr Value(Fixed16Value v) noexcept {
        u_.fixed16Cell = {v, {}, static_cast<std::uint8_t>(ValueTag::Fixed16)};
    }
    constexpr Value(Vec2Value v) noexcept {
        u_.vec2Cell = {v, {}, static_cast<std::uint8_t>(ValueTag::Vec2)};
    }
    constexpr Value(Vec3Value v) noexcept {
        u_.vec3Cell = {v, {}, static_cast<std::uint8_t>(ValueTag::Vec3)};
    }

    Value(QuatValue v) noexcept {
        storeQuat(v);
    }

    Value(Float4Value v) noexcept {
        storeFloat4(v);
    }

    Value(StructValue v) noexcept {
        v.vtag_ = static_cast<std::uint8_t>(ValueTag::Struct);
        u_.structVal = v;
    }

    Value(StringValue v) noexcept {
        v.vtag_ = static_cast<std::uint8_t>(ValueTag::String);
        u_.stringVal = v;
    }

    Value(const std::string& v) : Value(StringValue(v)) {}
    Value(std::string&& v) : Value(StringValue(std::move(v))) {}
    Value(std::string_view v) : Value(StringValue(v)) {}

    template <
        typename S,
        std::enable_if_t<
            std::is_same_v<std::decay_t<S>, const char*> ||
            std::is_same_v<std::decay_t<S>, char*>,
            int> = 0>
    Value(S v) : Value(StringValue(std::string_view(v ? v : ""))) {}

    Value(ObjectRef v) noexcept {
        v.vtag_ = static_cast<std::uint8_t>(ValueTag::ObjectRef);
        u_.objectRef = v;
    }

    Value(NativeHandle v) noexcept {
        const std::uint32_t tIdx = (v.typeId == 0)
            ? 0u
            : detail::internNativeHandleTypeId(v.typeId);
        u_.nativeHandleCell = {
            v.slot,
            static_cast<std::uint16_t>(v.generation),
            static_cast<std::uint16_t>(v.registryId),
            tIdx,
            {},
            static_cast<std::uint8_t>(ValueTag::NativeHandle)};
    }

    // Direct active-member writers (zero temporary, zero STLF stall, strict-aliasing safe)
    void storeInt64(std::int64_t v) noexcept {
        u_.int64Cell = {v, {}, static_cast<std::uint8_t>(ValueTag::Int64)};
    }
    void storeBool(bool v) noexcept {
        u_.boolCell = {v, {}, static_cast<std::uint8_t>(ValueTag::Bool)};
    }
    void storeLong(std::int64_t v) noexcept {
        u_.longCell = {LongValue{v}, {}, static_cast<std::uint8_t>(ValueTag::Long)};
    }
    void storeFloat(float v) noexcept {
        u_.floatCell = {FloatValue{v}, {}, static_cast<std::uint8_t>(ValueTag::Float)};
    }
    void storeDouble(double v) noexcept {
        u_.doubleCell = {v, {}, static_cast<std::uint8_t>(ValueTag::Double)};
    }
    void storeFixed(std::int32_t raw) noexcept {
        u_.fixedCell = {FixedValue{raw}, {}, static_cast<std::uint8_t>(ValueTag::Fixed)};
    }
    void storeVec2(std::int32_t x, std::int32_t y) noexcept {
        u_.vec2Cell = {Vec2Value{x, y}, {}, static_cast<std::uint8_t>(ValueTag::Vec2)};
    }
    void storeVec3(std::int32_t x, std::int32_t y, std::int32_t z) noexcept {
        u_.vec3Cell = {Vec3Value{x, y, z}, {}, static_cast<std::uint8_t>(ValueTag::Vec3)};
    }
    void storeQuat(const QuatValue& v) noexcept {
#if defined(__GNUC__) || defined(__clang__)
        if (__builtin_expect(v.w >= -8388608 && v.w <= 8388607, 1))
#else
        if (v.w >= -8388608 && v.w <= 8388607)
#endif
        {
            const auto uw = static_cast<std::uint32_t>(v.w);
            u_.quatInlineCell = {
                v.x,
                v.y,
                v.z,
                static_cast<std::uint8_t>(uw & 0xFFu),
                static_cast<std::uint8_t>((uw >> 8u) & 0xFFu),
                static_cast<std::uint8_t>((uw >> 16u) & 0xFFu),
                static_cast<std::uint8_t>(ValueTag::QuatInline)};
        } else {
            u_.arenaRefCell = {
                detail::internArenaQuat(v),
                {},
                static_cast<std::uint8_t>(ValueTag::QuatArena)};
        }
    }
    void storeFloat4(const Float4Value& v) noexcept {
        std::uint32_t wBits = 0;
        std::memcpy(&wBits, &v.w, sizeof(wBits));
#if defined(__GNUC__) || defined(__clang__)
        if (__builtin_expect((wBits & 0xFFu) == 0u, 1))
#else
        if ((wBits & 0xFFu) == 0u)
#endif
        {
            u_.float4InlineCell = {
                v.x,
                v.y,
                v.z,
                static_cast<std::uint8_t>((wBits >> 8u) & 0xFFu),
                static_cast<std::uint8_t>((wBits >> 16u) & 0xFFu),
                static_cast<std::uint8_t>((wBits >> 24u) & 0xFFu),
                static_cast<std::uint8_t>(ValueTag::Float4Inline)};
        } else {
            u_.arenaRefCell = {
                detail::internArenaFloat4(v),
                {},
                static_cast<std::uint8_t>(ValueTag::Float4Arena)};
        }
    }

    [[nodiscard]] ValueTag rawTag() const noexcept {
        unsigned char t = 0;
        std::memcpy(&t, reinterpret_cast<const unsigned char*>(&u_) + 15, 1);
        return static_cast<ValueTag>(t);
    }

    [[nodiscard]] bool isTraceable() const noexcept {
        return static_cast<std::uint8_t>(rawTag()) >=
            static_cast<std::uint8_t>(ValueTag::FirstTraceable);
    }

    [[nodiscard]] std::uint32_t arenaPoolIndex() const noexcept {
        std::uint32_t idx = 0;
        std::memcpy(&idx, &u_, sizeof(idx));
        return idx;
    }

    [[nodiscard]] std::size_t index() const noexcept {
        static constexpr std::uint8_t kCanonicalIndex[30] = {
            0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13,
            14, 15, 16, 17, 18, 19, 20, 21, 26, 25, 16, 21, 26, 22, 23, 24,
        };
        const auto t = static_cast<std::uint8_t>(rawTag());
        return t < 30u ? static_cast<std::size_t>(kCanonicalIndex[t]) : 0u;
    }

    [[nodiscard]] bool tryGetInt64(std::int64_t& out) const noexcept {
        if (rawTag() != ValueTag::Int64) return false;
        out = u_.int64Cell.payload;
        return true;
    }
    [[nodiscard]] bool tryGetDouble(double& out) const noexcept {
        if (rawTag() != ValueTag::Double) return false;
        out = u_.doubleCell.payload;
        return true;
    }
    [[nodiscard]] bool tryGetFloat(float& out) const noexcept {
        if (rawTag() != ValueTag::Float) return false;
        out = u_.floatCell.payload.value;
        return true;
    }
    [[nodiscard]] bool tryGetBool(bool& out) const noexcept {
        if (rawTag() != ValueTag::Bool) return false;
        out = u_.boolCell.payload;
        return true;
    }
    [[nodiscard]] bool tryGetLong(std::int64_t& out) const noexcept {
        if (rawTag() != ValueTag::Long) return false;
        out = u_.longCell.payload.value;
        return true;
    }
    [[nodiscard]] bool tryGetFixed(std::int32_t& outRaw) const noexcept {
        if (rawTag() != ValueTag::Fixed) return false;
        outRaw = u_.fixedCell.payload.raw;
        return true;
    }
    [[nodiscard]] bool tryGetVec2(Vec2Value& out) const noexcept {
        if (rawTag() != ValueTag::Vec2) return false;
        out = u_.vec2Cell.payload;
        return true;
    }
    [[nodiscard]] bool tryGetVec3(Vec3Value& out) const noexcept {
        if (rawTag() != ValueTag::Vec3) return false;
        out = u_.vec3Cell.payload;
        return true;
    }
    [[nodiscard]] bool tryGetQuat(QuatValue& out) const noexcept {
        const auto t = rawTag();
#if defined(__GNUC__) || defined(__clang__)
        if (__builtin_expect(t == ValueTag::QuatInline, 1))
#else
        if (t == ValueTag::QuatInline)
#endif
        {
            out.x = u_.quatInlineCell.x;
            out.y = u_.quatInlineCell.y;
            out.z = u_.quatInlineCell.z;
            const std::uint32_t u24 =
                static_cast<std::uint32_t>(u_.quatInlineCell.w0) |
                (static_cast<std::uint32_t>(u_.quatInlineCell.w1) << 8u) |
                (static_cast<std::uint32_t>(u_.quatInlineCell.w2) << 16u);
            out.w = static_cast<std::int32_t>((u24 ^ 0x00800000u) - 0x00800000u);
            return true;
        }
        if (t == ValueTag::QuatArena) {
            out = detail::lookupArenaQuat(u_.arenaRefCell.arenaIdx);
            return true;
        }
        return false;
    }
    [[nodiscard]] bool tryGetFloat4(Float4Value& out) const noexcept {
        const auto t = rawTag();
#if defined(__GNUC__) || defined(__clang__)
        if (__builtin_expect(t == ValueTag::Float4Inline, 1))
#else
        if (t == ValueTag::Float4Inline)
#endif
        {
            out.x = u_.float4InlineCell.x;
            out.y = u_.float4InlineCell.y;
            out.z = u_.float4InlineCell.z;
            const std::uint32_t wBits =
                (static_cast<std::uint32_t>(u_.float4InlineCell.w0) << 8u) |
                (static_cast<std::uint32_t>(u_.float4InlineCell.w1) << 16u) |
                (static_cast<std::uint32_t>(u_.float4InlineCell.w2) << 24u);
            std::memcpy(&out.w, &wBits, sizeof(out.w));
            return true;
        }
        if (t == ValueTag::Float4Arena) {
            out = detail::lookupArenaFloat4(u_.arenaRefCell.arenaIdx);
            return true;
        }
        return false;
    }
    [[nodiscard]] bool tryGetEnum(EnumValue& out) const noexcept {
        const auto t = rawTag();
#if defined(__GNUC__) || defined(__clang__)
        if (__builtin_expect(t == ValueTag::EnumInline, 1))
#else
        if (t == ValueTag::EnumInline)
#endif
        {
            out.typeId = u_.enumInlineCell.typeId;
            out.value = static_cast<std::int64_t>(u_.enumInlineCell.value32);
            return true;
        }
        if (t == ValueTag::EnumArena) {
            out = detail::lookupArenaEnum(u_.arenaRefCell.arenaIdx);
            return true;
        }
        return false;
    }
    [[nodiscard]] bool tryGetNativeHandle(NativeHandle& out) const noexcept {
        if (rawTag() != ValueTag::NativeHandle) return false;
        out.slot = u_.nativeHandleCell.slot;
        out.generation = u_.nativeHandleCell.generation;
        out.registryId = u_.nativeHandleCell.registryId;
        const auto tIdx = u_.nativeHandleCell.typeIdx;
        out.typeId = (tIdx == 0u) ? 0u : detail::lookupNativeHandleTypeId(tIdx);
        return true;
    }

    [[nodiscard]] std::int64_t asInt64Unchecked() const noexcept {
        return u_.int64Cell.payload;
    }
    [[nodiscard]] bool asBoolUnchecked() const noexcept {
        return u_.boolCell.payload;
    }
    [[nodiscard]] const StringValue& asStringUnchecked() const noexcept {
        return u_.stringVal;
    }
    [[nodiscard]] const StructValue& asStructUnchecked() const noexcept {
        return u_.structVal;
    }
    [[nodiscard]] const ObjectRef& asObjectRefUnchecked() const noexcept {
        return u_.objectRef;
    }

    friend bool operator==(const Value& left, const Value& right) {
        const auto lt = left.rawTag();
        const auto rt = right.rawTag();
        if (lt != rt) {
            if ((lt == ValueTag::Float4Inline && rt == ValueTag::Float4Arena) ||
                (lt == ValueTag::Float4Arena && rt == ValueTag::Float4Inline)) {
                Float4Value lf{}, rf{};
                (void)left.tryGetFloat4(lf);
                (void)right.tryGetFloat4(rf);
                return lf == rf;
            }
            return false;
        }
        switch (lt) {
        case ValueTag::Monostate:
        case ValueTag::NullString:
        case ValueTag::NullObject:
        case ValueTag::NullArray:
            return true;
        case ValueTag::Bool:
            return left.u_.boolCell.payload == right.u_.boolCell.payload;
        case ValueTag::Byte:
            return left.u_.byteCell.payload == right.u_.byteCell.payload;
        case ValueTag::SByte:
            return left.u_.sbyteCell.payload == right.u_.sbyteCell.payload;
        case ValueTag::Short:
            return left.u_.shortCell.payload == right.u_.shortCell.payload;
        case ValueTag::UShort:
            return left.u_.ushortCell.payload == right.u_.ushortCell.payload;
        case ValueTag::Int64:
            return left.u_.int64Cell.payload == right.u_.int64Cell.payload;
        case ValueTag::UInt:
            return left.u_.uintCell.payload == right.u_.uintCell.payload;
        case ValueTag::Long:
            return left.u_.longCell.payload == right.u_.longCell.payload;
        case ValueTag::ULong:
            return left.u_.ulongCell.payload == right.u_.ulongCell.payload;
        case ValueTag::Float:
            return left.u_.floatCell.payload == right.u_.floatCell.payload;
        case ValueTag::Double:
            return left.u_.doubleCell.payload == right.u_.doubleCell.payload;
        case ValueTag::Char:
            return left.u_.charCell.payload == right.u_.charCell.payload;
        case ValueTag::EnumInline:
            return left.u_.enumInlineCell.typeId == right.u_.enumInlineCell.typeId &&
                left.u_.enumInlineCell.value32 == right.u_.enumInlineCell.value32;
        case ValueTag::Fixed:
            return left.u_.fixedCell.payload == right.u_.fixedCell.payload;
        case ValueTag::Fixed16:
            return left.u_.fixed16Cell.payload == right.u_.fixed16Cell.payload;
        case ValueTag::Vec2:
            return left.u_.vec2Cell.payload == right.u_.vec2Cell.payload;
        case ValueTag::Vec3:
            return left.u_.vec3Cell.payload == right.u_.vec3Cell.payload;
        case ValueTag::QuatInline:
            return left.u_.quatInlineCell.x == right.u_.quatInlineCell.x &&
                left.u_.quatInlineCell.y == right.u_.quatInlineCell.y &&
                left.u_.quatInlineCell.z == right.u_.quatInlineCell.z &&
                left.u_.quatInlineCell.w0 == right.u_.quatInlineCell.w0 &&
                left.u_.quatInlineCell.w1 == right.u_.quatInlineCell.w1 &&
                left.u_.quatInlineCell.w2 == right.u_.quatInlineCell.w2;
        case ValueTag::Float4Inline: {
            Float4Value lf{}, rf{};
            (void)left.tryGetFloat4(lf);
            (void)right.tryGetFloat4(rf);
            return lf == rf;
        }
        case ValueTag::NativeHandle: {
            if (left.u_.nativeHandleCell.slot != right.u_.nativeHandleCell.slot ||
                left.u_.nativeHandleCell.generation != right.u_.nativeHandleCell.generation ||
                left.u_.nativeHandleCell.registryId != right.u_.nativeHandleCell.registryId) {
                return false;
            }
            if (left.u_.nativeHandleCell.typeIdx == right.u_.nativeHandleCell.typeIdx) {
                return true;
            }
            NativeHandle lh{}, rh{};
            (void)left.tryGetNativeHandle(lh);
            (void)right.tryGetNativeHandle(rh);
            return lh == rh;
        }
        case ValueTag::EnumArena: {
            if (left.u_.arenaRefCell.arenaIdx == right.u_.arenaRefCell.arenaIdx &&
                left.u_.arenaRefCell.arenaIdx != 0u) {
                return true;
            }
            EnumValue le{}, re{};
            (void)left.tryGetEnum(le);
            (void)right.tryGetEnum(re);
            return le == re;
        }
        case ValueTag::QuatArena: {
            if (left.u_.arenaRefCell.arenaIdx == right.u_.arenaRefCell.arenaIdx &&
                left.u_.arenaRefCell.arenaIdx != 0u) {
                return true;
            }
            QuatValue lq{}, rq{};
            (void)left.tryGetQuat(lq);
            (void)right.tryGetQuat(rq);
            return lq == rq;
        }
        case ValueTag::Float4Arena: {
            Float4Value lf{}, rf{};
            (void)left.tryGetFloat4(lf);
            (void)right.tryGetFloat4(rf);
            return lf == rf;
        }
        case ValueTag::Struct:
            return left.u_.structVal == right.u_.structVal;
        case ValueTag::String:
            return left.u_.stringVal == right.u_.stringVal;
        case ValueTag::ObjectRef:
            return left.u_.objectRef == right.u_.objectRef;
        }
        return false;
    }

    friend bool operator!=(const Value& left, const Value& right) {
        return !(left == right);
    }
};
static_assert(sizeof(Value) == 16, "runtime::Value must be 16 bytes");
static_assert(alignof(Value) == 16, "runtime::Value must be 16-byte aligned");
static_assert(std::is_trivially_copyable_v<Value>, "runtime::Value must be trivially copyable");
static_assert(std::is_trivially_destructible_v<Value>, "runtime::Value must be trivially destructible");

namespace detail {
template <typename T>
struct ValueInPlaceTraits {
    static constexpr bool isInPlace = false;
};
#define RS_DEFINE_VALUE_INPLACE_TRAIT(Type, TagEnum, MemberExpr)                  \
    template <>                                                                   \
    struct ValueInPlaceTraits<Type> {                                             \
        static constexpr bool isInPlace = true;                                   \
        static constexpr ValueTag tag = ValueTag::TagEnum;                        \
        static Type* ptr(Value* v) noexcept { return &(v->u_.MemberExpr); }       \
        static const Type* ptr(const Value* v) noexcept {                         \
            return &(v->u_.MemberExpr);                                           \
        }                                                                         \
    }

RS_DEFINE_VALUE_INPLACE_TRAIT(std::monostate, Monostate, monostateCell.payload);
RS_DEFINE_VALUE_INPLACE_TRAIT(NullString, NullString, nullStringCell.payload);
RS_DEFINE_VALUE_INPLACE_TRAIT(NullObject, NullObject, nullObjectCell.payload);
RS_DEFINE_VALUE_INPLACE_TRAIT(NullArray, NullArray, nullArrayCell.payload);
RS_DEFINE_VALUE_INPLACE_TRAIT(bool, Bool, boolCell.payload);
RS_DEFINE_VALUE_INPLACE_TRAIT(ByteValue, Byte, byteCell.payload);
RS_DEFINE_VALUE_INPLACE_TRAIT(SByteValue, SByte, sbyteCell.payload);
RS_DEFINE_VALUE_INPLACE_TRAIT(ShortValue, Short, shortCell.payload);
RS_DEFINE_VALUE_INPLACE_TRAIT(UShortValue, UShort, ushortCell.payload);
RS_DEFINE_VALUE_INPLACE_TRAIT(std::int64_t, Int64, int64Cell.payload);
RS_DEFINE_VALUE_INPLACE_TRAIT(UIntValue, UInt, uintCell.payload);
RS_DEFINE_VALUE_INPLACE_TRAIT(LongValue, Long, longCell.payload);
RS_DEFINE_VALUE_INPLACE_TRAIT(ULongValue, ULong, ulongCell.payload);
RS_DEFINE_VALUE_INPLACE_TRAIT(FloatValue, Float, floatCell.payload);
RS_DEFINE_VALUE_INPLACE_TRAIT(double, Double, doubleCell.payload);
RS_DEFINE_VALUE_INPLACE_TRAIT(CharValue, Char, charCell.payload);
RS_DEFINE_VALUE_INPLACE_TRAIT(FixedValue, Fixed, fixedCell.payload);
RS_DEFINE_VALUE_INPLACE_TRAIT(Fixed16Value, Fixed16, fixed16Cell.payload);
RS_DEFINE_VALUE_INPLACE_TRAIT(Vec2Value, Vec2, vec2Cell.payload);
RS_DEFINE_VALUE_INPLACE_TRAIT(Vec3Value, Vec3, vec3Cell.payload);
RS_DEFINE_VALUE_INPLACE_TRAIT(StructValue, Struct, structVal);
RS_DEFINE_VALUE_INPLACE_TRAIT(StringValue, String, stringVal);
RS_DEFINE_VALUE_INPLACE_TRAIT(ObjectRef, ObjectRef, objectRef);

#undef RS_DEFINE_VALUE_INPLACE_TRAIT

// Transient 64-slot thread-local decode ring for legacy `std::get_if<EnumValue|QuatValue|Float4Value|NativeHandle>`.
// Returned pointers are valid only for immediate read-only inspection within the current expression/statement;
// prefer value-returning `Value::tryGetEnum` / `Value::tryGetQuat` / `Value::tryGetFloat4` / `Value::tryGetNativeHandle` in new code.
template <typename T>
inline const T* tlsDecodedValueSlot(const T& decoded) noexcept {
    struct Ring {
        std::array<T, 64> items{};
        std::uint8_t next = 0;
    };
    thread_local Ring ring{};
    const std::uint8_t idx = ring.next++ & 63u;
    ring.items[idx] = decoded;
    return &ring.items[idx];
}
} // namespace detail

} // namespace realscript::runtime

namespace std {

template <
    typename T,
    std::enable_if_t<realscript::runtime::detail::ValueInPlaceTraits<T>::isInPlace, int> = 0>
inline bool holds_alternative(const realscript::runtime::Value& value) noexcept {
    return value.rawTag() == realscript::runtime::detail::ValueInPlaceTraits<T>::tag;
}

template <
    typename T,
    std::enable_if_t<std::is_same_v<T, realscript::runtime::EnumValue>, int> = 0>
inline bool holds_alternative(const realscript::runtime::Value& value) noexcept {
    const auto t = value.rawTag();
    return t == realscript::runtime::ValueTag::EnumInline ||
        t == realscript::runtime::ValueTag::EnumArena;
}

template <
    typename T,
    std::enable_if_t<std::is_same_v<T, realscript::runtime::QuatValue>, int> = 0>
inline bool holds_alternative(const realscript::runtime::Value& value) noexcept {
    const auto t = value.rawTag();
    return t == realscript::runtime::ValueTag::QuatInline ||
        t == realscript::runtime::ValueTag::QuatArena;
}

template <
    typename T,
    std::enable_if_t<std::is_same_v<T, realscript::runtime::Float4Value>, int> = 0>
inline bool holds_alternative(const realscript::runtime::Value& value) noexcept {
    const auto t = value.rawTag();
    return t == realscript::runtime::ValueTag::Float4Inline ||
        t == realscript::runtime::ValueTag::Float4Arena;
}

template <
    typename T,
    std::enable_if_t<std::is_same_v<T, realscript::runtime::NativeHandle>, int> = 0>
inline bool holds_alternative(const realscript::runtime::Value& value) noexcept {
    return value.rawTag() == realscript::runtime::ValueTag::NativeHandle;
}

template <typename T, std::enable_if_t<std::is_same_v<T, std::string>, int> = 0>
inline bool holds_alternative(const realscript::runtime::Value& value) noexcept {
    return value.rawTag() == realscript::runtime::ValueTag::String;
}

template <
    typename T,
    std::enable_if_t<realscript::runtime::detail::ValueInPlaceTraits<T>::isInPlace, int> = 0>
inline const T* get_if(const realscript::runtime::Value* value) noexcept {
    if (!value || value->rawTag() != realscript::runtime::detail::ValueInPlaceTraits<T>::tag) {
        return nullptr;
    }
    return realscript::runtime::detail::ValueInPlaceTraits<T>::ptr(value);
}

template <
    typename T,
    std::enable_if_t<realscript::runtime::detail::ValueInPlaceTraits<T>::isInPlace, int> = 0>
inline T* get_if(realscript::runtime::Value* value) noexcept {
    if (!value || value->rawTag() != realscript::runtime::detail::ValueInPlaceTraits<T>::tag) {
        return nullptr;
    }
    return realscript::runtime::detail::ValueInPlaceTraits<T>::ptr(value);
}

template <
    typename T,
    std::enable_if_t<std::is_same_v<T, realscript::runtime::EnumValue>, int> = 0>
inline const realscript::runtime::EnumValue* get_if(
    const realscript::runtime::Value* value) noexcept {
    if (!value) return nullptr;
    realscript::runtime::EnumValue decoded{};
    if (!value->tryGetEnum(decoded)) return nullptr;
    return realscript::runtime::detail::tlsDecodedValueSlot(decoded);
}

template <
    typename T,
    std::enable_if_t<std::is_same_v<T, realscript::runtime::EnumValue>, int> = 0>
inline const realscript::runtime::EnumValue* get_if(
    realscript::runtime::Value* value) noexcept {
    return std::get_if<realscript::runtime::EnumValue>(
        static_cast<const realscript::runtime::Value*>(value));
}

template <
    typename T,
    std::enable_if_t<std::is_same_v<T, realscript::runtime::QuatValue>, int> = 0>
inline const realscript::runtime::QuatValue* get_if(
    const realscript::runtime::Value* value) noexcept {
    if (!value) return nullptr;
    realscript::runtime::QuatValue decoded{};
    if (!value->tryGetQuat(decoded)) return nullptr;
    return realscript::runtime::detail::tlsDecodedValueSlot(decoded);
}

template <
    typename T,
    std::enable_if_t<std::is_same_v<T, realscript::runtime::QuatValue>, int> = 0>
inline const realscript::runtime::QuatValue* get_if(
    realscript::runtime::Value* value) noexcept {
    return std::get_if<realscript::runtime::QuatValue>(
        static_cast<const realscript::runtime::Value*>(value));
}

template <
    typename T,
    std::enable_if_t<std::is_same_v<T, realscript::runtime::Float4Value>, int> = 0>
inline const realscript::runtime::Float4Value* get_if(
    const realscript::runtime::Value* value) noexcept {
    if (!value) return nullptr;
    realscript::runtime::Float4Value decoded{};
    if (!value->tryGetFloat4(decoded)) return nullptr;
    return realscript::runtime::detail::tlsDecodedValueSlot(decoded);
}

template <
    typename T,
    std::enable_if_t<std::is_same_v<T, realscript::runtime::Float4Value>, int> = 0>
inline const realscript::runtime::Float4Value* get_if(
    realscript::runtime::Value* value) noexcept {
    return std::get_if<realscript::runtime::Float4Value>(
        static_cast<const realscript::runtime::Value*>(value));
}

template <
    typename T,
    std::enable_if_t<std::is_same_v<T, realscript::runtime::NativeHandle>, int> = 0>
inline const realscript::runtime::NativeHandle* get_if(
    const realscript::runtime::Value* value) noexcept {
    if (!value) return nullptr;
    realscript::runtime::NativeHandle decoded{};
    if (!value->tryGetNativeHandle(decoded)) return nullptr;
    return realscript::runtime::detail::tlsDecodedValueSlot(decoded);
}

template <
    typename T,
    std::enable_if_t<std::is_same_v<T, realscript::runtime::NativeHandle>, int> = 0>
inline const realscript::runtime::NativeHandle* get_if(
    realscript::runtime::Value* value) noexcept {
    return std::get_if<realscript::runtime::NativeHandle>(
        static_cast<const realscript::runtime::Value*>(value));
}

template <typename T, std::enable_if_t<std::is_same_v<T, std::string>, int> = 0>
inline const std::string* get_if(
    const realscript::runtime::Value* value) noexcept {
    if (!value || value->rawTag() != realscript::runtime::ValueTag::String) {
        return nullptr;
    }
    return &value->asStringUnchecked().str();
}

template <typename T, std::enable_if_t<std::is_same_v<T, std::string>, int> = 0>
inline const std::string* get_if(
    realscript::runtime::Value* value) noexcept {
    return std::get_if<std::string>(
        static_cast<const realscript::runtime::Value*>(value));
}

template <
    typename T,
    std::enable_if_t<realscript::runtime::detail::ValueInPlaceTraits<T>::isInPlace, int> = 0>
inline const T& get(const realscript::runtime::Value& value) {
    if (const auto* p = std::get_if<T>(&value)) return *p;
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    throw std::bad_variant_access{};
#else
    std::abort();
#endif
}

template <
    typename T,
    std::enable_if_t<realscript::runtime::detail::ValueInPlaceTraits<T>::isInPlace, int> = 0>
inline T& get(realscript::runtime::Value& value) {
    if (auto* p = std::get_if<T>(&value)) return *p;
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    throw std::bad_variant_access{};
#else
    std::abort();
#endif
}

template <
    typename T,
    std::enable_if_t<realscript::runtime::detail::ValueInPlaceTraits<T>::isInPlace, int> = 0>
inline const T&& get(const realscript::runtime::Value&& value) {
    if (const auto* p = std::get_if<T>(&value)) return std::move(*p);
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    throw std::bad_variant_access{};
#else
    std::abort();
#endif
}

template <
    typename T,
    std::enable_if_t<realscript::runtime::detail::ValueInPlaceTraits<T>::isInPlace, int> = 0>
inline T&& get(realscript::runtime::Value&& value) {
    if (auto* p = std::get_if<T>(&value)) return std::move(*p);
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    throw std::bad_variant_access{};
#else
    std::abort();
#endif
}

template <
    typename T,
    std::enable_if_t<
        std::is_same_v<T, realscript::runtime::EnumValue> ||
        std::is_same_v<T, realscript::runtime::QuatValue> ||
        std::is_same_v<T, realscript::runtime::Float4Value> ||
        std::is_same_v<T, realscript::runtime::NativeHandle>,
        int> = 0>
inline T get(const realscript::runtime::Value& value) {
    T out{};
    if constexpr (std::is_same_v<T, realscript::runtime::EnumValue>) {
        if (value.tryGetEnum(out)) return out;
    } else if constexpr (std::is_same_v<T, realscript::runtime::QuatValue>) {
        if (value.tryGetQuat(out)) return out;
    } else if constexpr (std::is_same_v<T, realscript::runtime::Float4Value>) {
        if (value.tryGetFloat4(out)) return out;
    } else {
        if (value.tryGetNativeHandle(out)) return out;
    }
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    throw std::bad_variant_access{};
#else
    std::abort();
#endif
}

template <
    typename T,
    std::enable_if_t<
        std::is_same_v<T, realscript::runtime::EnumValue> ||
        std::is_same_v<T, realscript::runtime::QuatValue> ||
        std::is_same_v<T, realscript::runtime::Float4Value> ||
        std::is_same_v<T, realscript::runtime::NativeHandle>,
        int> = 0>
inline T get(realscript::runtime::Value& value) {
    return std::get<T>(static_cast<const realscript::runtime::Value&>(value));
}

template <
    typename T,
    std::enable_if_t<
        std::is_same_v<T, realscript::runtime::EnumValue> ||
        std::is_same_v<T, realscript::runtime::QuatValue> ||
        std::is_same_v<T, realscript::runtime::Float4Value> ||
        std::is_same_v<T, realscript::runtime::NativeHandle>,
        int> = 0>
inline T get(const realscript::runtime::Value&& value) {
    return std::get<T>(static_cast<const realscript::runtime::Value&>(value));
}

template <
    typename T,
    std::enable_if_t<
        std::is_same_v<T, realscript::runtime::EnumValue> ||
        std::is_same_v<T, realscript::runtime::QuatValue> ||
        std::is_same_v<T, realscript::runtime::Float4Value> ||
        std::is_same_v<T, realscript::runtime::NativeHandle>,
        int> = 0>
inline T get(realscript::runtime::Value&& value) {
    return std::get<T>(static_cast<const realscript::runtime::Value&>(value));
}

template <typename T, std::enable_if_t<std::is_same_v<T, std::string>, int> = 0>
inline const std::string& get(const realscript::runtime::Value& value) {
    return std::get<realscript::runtime::StringValue>(value).str();
}
template <typename T, std::enable_if_t<std::is_same_v<T, std::string>, int> = 0>
inline const std::string& get(realscript::runtime::Value& value) {
    return std::get<realscript::runtime::StringValue>(
        static_cast<const realscript::runtime::Value&>(value)).str();
}
template <typename T, std::enable_if_t<std::is_same_v<T, std::string>, int> = 0>
inline const std::string& get(const realscript::runtime::Value&& value) {
    return std::get<realscript::runtime::StringValue>(value).str();
}
template <typename T, std::enable_if_t<std::is_same_v<T, std::string>, int> = 0>
inline std::string get(realscript::runtime::Value&& value) {
    return std::get<realscript::runtime::StringValue>(value).str();
}

} // namespace std

namespace realscript::runtime {

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

struct StackTraceLocation {
    semantic::SymbolId symbolId = 0;
    std::string function;
    std::string sourcePath;
    std::uint32_t sourceLine = 0;
    std::uint32_t sourceColumn = 0;
    std::uint32_t generatedLine = 0;

    [[nodiscard]] bool hasSourceLocation() const noexcept {
        return !sourcePath.empty() && sourceLine != 0;
    }

    [[nodiscard]] std::string format() const {
        if (!hasSourceLocation()) {
            return function.empty() ? std::string("<unknown>") : function;
        }
        std::string out = function.empty() ? std::string("<unknown>") : function;
        out += " (" + sourcePath + ":" + std::to_string(sourceLine);
        if (sourceColumn != 0) {
            out += ":" + std::to_string(sourceColumn);
        }
        out += ")";
        return out;
    }
};

struct RuntimeError {
    ErrorCode code = ErrorCode::None;
    std::string message;
    std::vector<std::string> stackTrace;
    std::vector<StackTraceLocation> sourceLocations;
    std::vector<std::string> symbolizedStackTrace;
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

enum class NativeFieldKind : std::uint8_t {
    Int32,
    Int64,
    Float32,
    Float64,
    Bool,
    Fixed,
    Fixed16,
    Vec2,
    Vec3,
    Quat,
};

struct NativeFieldLayout {
    std::size_t byteOffset = 0;
    NativeFieldKind kind = NativeFieldKind::Int32;
    semantic::PrimitiveType scriptType = semantic::PrimitiveType::Int;
    semantic::SymbolId scriptTypeId = 0;
};

struct NativeRawColumnView {
    void* basePtr = nullptr;
    std::size_t count = 0;
    std::size_t strideBytes = 0;
    semantic::SymbolId structTypeId = 0;
    std::vector<NativeFieldLayout> fields;
    bool readOnly = false;
};

class ShadowStack {
public:
    static constexpr std::size_t InlineFrameCapacity = 4;
    static constexpr std::uint32_t InvalidRegistrySlot = 0xffffffffu;

    ShadowStack() noexcept;
    ~ShadowStack() noexcept;
    ShadowStack(const ShadowStack& other);
    ShadowStack& operator=(const ShadowStack& other);
    ShadowStack(ShadowStack&& other) noexcept;
    ShadowStack& operator=(ShadowStack&& other) noexcept;

    void pushFrame(
        const std::vector<Value>* arguments,
        const std::vector<Value>* locals,
        const std::vector<Value>* registers,
        const std::size_t* activeRegisterLimit = nullptr);
    void pushSpanFrame(
        ValueSpan arguments,
        ValueSpan locals,
        ValueSpan registers,
        const std::size_t* activeRegisterLimit = nullptr);
    void popFrame() noexcept;
    void visitRoots(const std::function<void(const Value&)>& visitor) const;

    template <typename Visitor>
    void visitRoots(Visitor&& visitor) const {
        const auto visitValues = [&](
                                     const std::vector<Value>* values,
                                     const Value* rawPtr,
                                     std::size_t rawCount,
                                     const std::size_t* limit) {
            const Value* data = values ? values->data() : rawPtr;
            const std::size_t total = values ? values->size() : rawCount;
            if (!data || total == 0) return;
            const std::size_t count = limit ? std::min(*limit, total) : total;
            for (std::size_t index = 0; index < count; ++index) {
                const Value& v = data[index];
                if (!std::holds_alternative<std::monostate>(v)) {
                    visitor(v);
                }
            }
        };
        const std::size_t inlineCount = std::min(frameCount_, InlineFrameCapacity);
        for (std::size_t i = 0; i < inlineCount; ++i) {
            const auto& frame = inlineFrames_[i];
            visitValues(frame.arguments, frame.argumentsPtr, frame.argumentsCount, nullptr);
            visitValues(frame.locals, frame.localsPtr, frame.localsCount, nullptr);
            visitValues(frame.registers, frame.registersPtr, frame.registersCount, frame.activeRegisterLimit);
        }
        for (const auto& frame : overflowFrames_) {
            visitValues(frame.arguments, frame.argumentsPtr, frame.argumentsCount, nullptr);
            visitValues(frame.locals, frame.localsPtr, frame.localsCount, nullptr);
            visitValues(frame.registers, frame.registersPtr, frame.registersCount, frame.activeRegisterLimit);
        }
    }

    template <typename Visitor>
    void visitTraceableRoots(Visitor&& visitor) const {
        const auto visitValues = [&](
                                     const std::vector<Value>* values,
                                     const Value* rawPtr,
                                     std::size_t rawCount,
                                     const std::size_t* limit) {
            const Value* data = values ? values->data() : rawPtr;
            const std::size_t total = values ? values->size() : rawCount;
            if (!data || total == 0) return;
            const std::size_t count = limit ? std::min(*limit, total) : total;
            for (std::size_t index = 0; index < count; ++index) {
                const Value& v = data[index];
                if (v.isTraceable()) {
                    visitor(v);
                }
            }
        };
        const std::size_t inlineCount = std::min(frameCount_, InlineFrameCapacity);
        for (std::size_t i = 0; i < inlineCount; ++i) {
            const auto& frame = inlineFrames_[i];
            visitValues(frame.arguments, frame.argumentsPtr, frame.argumentsCount, nullptr);
            visitValues(frame.locals, frame.localsPtr, frame.localsCount, nullptr);
            visitValues(frame.registers, frame.registersPtr, frame.registersCount, frame.activeRegisterLimit);
        }
        for (const auto& frame : overflowFrames_) {
            visitValues(frame.arguments, frame.argumentsPtr, frame.argumentsCount, nullptr);
            visitValues(frame.locals, frame.localsPtr, frame.localsCount, nullptr);
            visitValues(frame.registers, frame.registersPtr, frame.registersCount, frame.activeRegisterLimit);
        }
    }

    [[nodiscard]] std::size_t frameCount() const noexcept { return frameCount_; }
    [[nodiscard]] std::uint64_t instanceId() const noexcept { return instanceId_; }

    struct FrameRoots {
        const std::vector<Value>* arguments = nullptr;
        const std::vector<Value>* locals = nullptr;
        const std::vector<Value>* registers = nullptr;
        const Value* argumentsPtr = nullptr;
        std::size_t argumentsCount = 0;
        const Value* localsPtr = nullptr;
        std::size_t localsCount = 0;
        const Value* registersPtr = nullptr;
        std::size_t registersCount = 0;
        const std::size_t* activeRegisterLimit = nullptr;
    };

private:
    friend void registerActiveShadowStack(ShadowStack* stack) noexcept;
    friend void unregisterActiveShadowStack(ShadowStack* stack) noexcept;
    friend void transferActiveShadowStack(ShadowStack* from, ShadowStack* to) noexcept;

    std::array<FrameRoots, InlineFrameCapacity> inlineFrames_{};
    std::vector<FrameRoots> overflowFrames_;
    std::size_t frameCount_ = 0;
    std::uint64_t instanceId_ = 0;
    void* registryShard_ = nullptr;
    std::uint32_t registrySlot_ = InvalidRegistrySlot;
};

struct ArrayFastView {
    Value* data = nullptr;
    std::uint8_t* dirtyCards = nullptr;
    std::uint32_t length = 0;
    semantic::PrimitiveType elementType = semantic::PrimitiveType::Error;
    bool needsRememberedBarrier = false;
    semantic::SymbolId elementTypeId = 0;
};

struct ObjectFastView {
    Value* fields = nullptr;
    std::uint32_t fieldCount = 0;
    semantic::SymbolId typeId = 0;
    bool needsRememberedBarrier = false;
};

class ManagedHeap {
public:
    using RootToken = std::uint64_t;
    static constexpr std::size_t kCardElementShift = 6;
    static constexpr std::size_t kCardElements = std::size_t{1} << kCardElementShift;
    static constexpr std::size_t kCardTableMinLength = kCardElements + 1;

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
    [[nodiscard]] std::optional<ObjectRef> allocateTypedArray(
        semantic::SymbolId arrayTypeId,
        semantic::PrimitiveType elementType,
        semantic::SymbolId elementTypeId,
        std::size_t length,
        Value initialValue,
        RuntimeError* error,
        bool* outSlotsReallocated);
    [[nodiscard]] std::optional<ObjectRef> allocateRecord(
        std::size_t fieldCount,
        RuntimeError* error = nullptr);
    [[nodiscard]] std::optional<ObjectRef> allocateObject(
        semantic::SymbolId typeId,
        std::vector<Value> fields,
        std::vector<std::size_t> referenceFields,
        RuntimeError* error = nullptr);
    [[nodiscard]] std::optional<ObjectRef> allocateObjectFromTemplate(
        semantic::SymbolId typeId,
        const Value* defaultFields,
        std::size_t fieldCount,
        const std::shared_ptr<const std::vector<std::size_t>>& referenceFields,
        RuntimeError* error = nullptr,
        bool* outSlotsReallocated = nullptr);

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
    [[nodiscard]] const Value* arrayGetFast(ObjectRef reference, std::size_t index) const noexcept;
    [[nodiscard]] bool tryBindArrayFastView(ObjectRef reference, ArrayFastView& outView) const noexcept;
    void recordArrayWriteBarrier(ObjectRef arrayRef, std::size_t index, const Value& value) noexcept;
    void recordArrayWriteBarrier(ObjectRef arrayRef, const Value& value) noexcept;
    [[nodiscard]] bool tryBindObjectFastView(ObjectRef reference, ObjectFastView& outView) const noexcept;
    void recordObjectWriteBarrier(ObjectRef objectRef, const Value& value) noexcept;
    [[nodiscard]] const void* slotStorageToken() const noexcept;
    [[nodiscard]] semantic::SymbolId objectTypeIdFast(ObjectRef reference) const noexcept;
    bool arraySetPrevalidated(ObjectRef reference, std::size_t index, const Value& value, bool mayBeReference) noexcept;
    bool arraySet(
        ObjectRef reference,
        std::size_t index,
        Value value,
        RuntimeError* error = nullptr,
        ArrayStoreTypePolicy typePolicy = ArrayStoreTypePolicy::Exact);
    [[nodiscard]] std::optional<std::size_t> fieldCount(ObjectRef reference) const;
    [[nodiscard]] bool isInlineRecordObject(ObjectRef reference) const noexcept;
    [[nodiscard]] std::optional<semantic::SymbolId> objectTypeId(
        ObjectRef reference) const;
    [[nodiscard]] std::optional<Value> fieldGet(ObjectRef reference, std::size_t index) const;
    [[nodiscard]] const Value* fieldGetFast(
        ObjectRef reference,
        std::size_t index,
        semantic::SymbolId* outTypeId = nullptr) const noexcept;
    bool fieldSetFast(ObjectRef reference, std::size_t index, const Value& value) noexcept;
    bool fieldSet(
        ObjectRef reference,
        std::size_t index,
        Value value,
        RuntimeError* error = nullptr);
    bool fieldSetFast(
        ObjectRef reference,
        std::size_t index,
        const Value& value,
        semantic::SymbolId expectedTypeId,
        semantic::PrimitiveType expectedFieldType) noexcept;

    void registerTypeObservableBits(
        semantic::SymbolId typeId,
        std::vector<std::uint8_t> fieldObservableBits);
    void registerProgramObservableTypes(const class ProgramImage& program);
    [[nodiscard]] std::uint64_t dirtyFieldMask(ObjectRef reference) const noexcept;
    void clearDirtyFieldMask(ObjectRef reference) noexcept;
    void addUiObserver(ObjectRef reference) noexcept;
    void removeUiObserver(ObjectRef reference) noexcept;
    [[nodiscard]] std::uint16_t uiObserverCount(ObjectRef reference) const noexcept;
    [[nodiscard]] std::vector<ObjectRef> consumeDirtyObjects();

    static constexpr std::int64_t DelegateWeakTargetFlag = 8;
    [[nodiscard]] std::optional<ObjectRef> allocateWeakDelegate(
        semantic::SymbolId delegateTypeId,
        ObjectRef target,
        semantic::SymbolId functionId,
        std::int64_t baseFlags = 0,
        RuntimeError* error = nullptr);
    [[nodiscard]] Value pruneDelegateChain(const Value& delegateVal);

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
    bool removeStaticField(semantic::SymbolId id) noexcept;
    [[nodiscard]] std::vector<semantic::SymbolId> staticFieldIds() const;

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
    [[nodiscard]] std::vector<std::string> nativeEcsTableNames() const;
    void clearNativeEcsTables() noexcept;
    [[nodiscard]] const std::vector<std::int64_t>* nativeEcsEntityIds(
        std::string_view canonicalTypeName) const noexcept;
    [[nodiscard]] const std::vector<Value>* nativeEcsComponentValues(
        std::string_view canonicalTypeName) const noexcept;
    bool setNativeEcsComponentByDenseIndex(
        std::string_view canonicalTypeName,
        std::size_t denseIndex,
        Value component);

    // Zero-Copy Host C++ Contiguous Struct Column Binding for ECS Query
    [[nodiscard]] std::optional<ObjectRef> allocateRawColumnArray(
        semantic::SymbolId arrayTypeId,
        semantic::SymbolId elementTypeId,
        NativeRawColumnView view,
        RuntimeError* error = nullptr);
    bool updateRawColumnArray(
        ObjectRef reference,
        NativeRawColumnView view) noexcept;
    [[nodiscard]] bool isRawColumnArray(ObjectRef reference) const noexcept;
    [[nodiscard]] bool hasSparseEcsState() const noexcept;
    void notifySparseEcsState() noexcept;
    [[nodiscard]] const NativeRawColumnView* rawColumnView(ObjectRef reference) const noexcept;
    bool readRawColumnFields(
        ObjectRef reference,
        std::size_t index,
        Value* outFields,
        std::size_t maxFields,
        std::uint8_t& outFieldCount) const noexcept;
    bool writeRawColumnFields(
        ObjectRef reference,
        std::size_t index,
        const Value* fields,
        std::size_t fieldCount,
        RuntimeError* error = nullptr) noexcept;

    void requestCollection() noexcept;
    [[nodiscard]] bool collectionRequested() const noexcept;
    [[nodiscard]] GcPhase phase() const noexcept;
    std::size_t step(const ShadowStack& shadowStack, std::size_t workBudget);
    std::size_t stepIdleForDuration(const ShadowStack& shadowStack, double maxMilliseconds);
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

struct WeakRef {
    ObjectRef ref{};

    constexpr WeakRef() noexcept = default;
    constexpr explicit WeakRef(ObjectRef target) noexcept : ref(target) {}

    [[nodiscard]] constexpr bool empty() const noexcept { return !ref.valid(); }
    void reset() noexcept { ref = ObjectRef{}; }
    [[nodiscard]] std::optional<ObjectRef> lock(const ManagedHeap& heap) const noexcept {
        if (!ref.valid() || !heap.isAlive(ref)) return std::nullopt;
        return ref;
    }
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
    [[nodiscard]] NativeHandle createBorrowed(
        semantic::SymbolId typeId,
        void* rawPointer,
        bool indexPointer = false);
    [[nodiscard]] std::optional<NativeHandle> findHandle(
        semantic::SymbolId typeId,
        const void* rawPointer) const noexcept;
    [[nodiscard]] std::optional<ObjectRef> cachedWrapper(
        NativeHandle handle,
        std::uint64_t heapId) const noexcept;
    bool setCachedWrapper(
        NativeHandle handle,
        ObjectRef wrapper,
        std::uint64_t heapId) noexcept;
    [[nodiscard]] std::shared_ptr<void> resolve(
        NativeHandle handle,
        semantic::SymbolId expectedTypeId = 0,
        RuntimeError* error = nullptr) const;
    [[nodiscard]] void* resolveRawFast(
        NativeHandle handle,
        semantic::SymbolId expectedTypeId = 0) const noexcept;
    void visitCachedWrappers(
        std::uint64_t heapId,
        const std::function<void(NativeHandle, ObjectRef, const void*)>& visitor) const;
    [[nodiscard]] bool isAlive(NativeHandle handle) const noexcept;
    bool release(NativeHandle handle) noexcept;
    void clear() noexcept;
    [[nodiscard]] std::size_t liveCount() const noexcept;
    [[nodiscard]] std::uint64_t registryId() const noexcept;
    [[nodiscard]] std::string debugName(NativeHandle handle) const;

private:
    struct Slot {
        std::shared_ptr<void> resource;
        const void* rawPointer = nullptr;
        semantic::SymbolId typeId = 0;
        std::uint32_t generation = 1;
        std::string debugName;
        ObjectRef cachedWrapper{};
        std::uint64_t cachedHeapId = 0;
    };
    struct PointerTypeKey {
        const void* rawPointer = nullptr;
        semantic::SymbolId typeId = 0;
        bool operator==(const PointerTypeKey& other) const noexcept {
            return rawPointer == other.rawPointer && typeId == other.typeId;
        }
    };
    struct PointerTypeKeyHash {
        std::size_t operator()(const PointerTypeKey& key) const noexcept {
            const auto h1 = std::hash<const void*>{}(key.rawPointer);
            const auto h2 = std::hash<semantic::SymbolId>{}(key.typeId);
            return h1 ^ (h2 + 0x9e3779b9u + (h1 << 6u) + (h1 >> 2u));
        }
    };
    std::uint64_t registryId_ = 0;
    std::vector<Slot> slots_;
    std::vector<std::uint32_t> freeSlots_;
    std::unordered_map<PointerTypeKey, std::uint32_t, PointerTypeKeyHash> activeSlotByPointer_;
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

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
struct ExternalCallRecord {
    semantic::SymbolId symbolId = 0;
    std::string name;
    std::uint64_t argumentHash = 0;
    bool succeeded = false;
    Value result;
    ErrorCode errorCode = ErrorCode::None;
    std::string errorMessage;
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

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
    bool trustedRelease = false;
};

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
struct ExecutionResult {
    bool succeeded = false;
    Value value;
    RuntimeError error;
    std::uint64_t instructionsExecuted = 0;
    RuntimeStatistics statistics;
    std::uint64_t determinismDigest = 0;
    std::size_t replayEntriesConsumed = 0;
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

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
    DeterminismMode mode_ = DeterminismMode::Off;
    bool canonicalizeFloatingPoint_ = true;
    bool enforceCapabilities_ = false;
    std::shared_ptr<ReplayLog> replayLog_{};
    std::unique_ptr<std::unordered_set<std::string>> grantedCapabilities_{};
    std::vector<ExternalCallRecord> replayEntries_{};
    std::size_t replayCursor_ = 0;
    std::uint64_t digest_ = 14695981039346656037ULL;
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

class ExecutionDomainContext;
enum class DomainKind : std::uint8_t;

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
    void setExecutionDomainContext(
        ExecutionDomainContext* context,
        DomainKind domainKind) noexcept;
    [[nodiscard]] ExecutionDomainContext* executionDomainContext() const noexcept;
    [[nodiscard]] DomainKind domainKind() const noexcept;

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
    ExecutionDomainContext* domainContext_ = nullptr;
    std::uint8_t domainKind_ = 0;
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
[[nodiscard]] bool utf8IsAsciiFast(std::string_view text) noexcept;
[[nodiscard]] std::size_t utf8CodePointLength(std::string_view text) noexcept;
[[nodiscard]] std::string_view utf8CodePointSlice(
    std::string_view text,
    std::size_t startCodePoint,
    std::size_t codePointCount = std::string_view::npos) noexcept;
[[nodiscard]] std::int64_t utf8CodePointIndexOf(
    std::string_view text,
    std::string_view needle) noexcept;
[[nodiscard]] std::int64_t utf8CodePointLastIndexOf(
    std::string_view text,
    std::string_view needle) noexcept;
[[nodiscard]] std::uint64_t stableValueHash(const Value& value) noexcept;
[[nodiscard]] bool isReplayStableValue(const Value& value) noexcept;
[[nodiscard]] const char* determinismModeName(DeterminismMode mode) noexcept;
[[nodiscard]] const char* bindingDeterminismName(BindingDeterminism determinism) noexcept;
[[nodiscard]] std::string formatProfile(const ExecutionProfile& profile);
[[nodiscard]] std::string profileToJson(const ExecutionProfile& profile);
[[nodiscard]] std::string exportChromeTraceJson(
    const ExecutionProfile& profile,
    const GcStatistics* gcStatistics = nullptr);


struct RuntimeValuePoolStatistics {
    std::size_t stringActiveSlots = 0;
    std::size_t stringPinnedSlots = 0;
    std::size_t stringFreeListSlots = 0;
    std::size_t stringAllocatedPages = 0;
    std::uint64_t stringClockEvictions = 0;
    std::uint64_t stringGcReclaimed = 0;

    std::size_t inlineStructActiveSlots = 0;
    std::size_t inlineStructPinnedSlots = 0;
    std::size_t inlineStructFreeListSlots = 0;
    std::size_t inlineStructAllocatedPages = 0;
    std::uint64_t inlineStructClockEvictions = 0;
    std::uint64_t inlineStructGcReclaimed = 0;

    std::size_t heapStructActiveSlots = 0;
    std::size_t heapStructPinnedSlots = 0;
    std::size_t heapStructFreeListSlots = 0;
    std::size_t heapStructAllocatedPages = 0;
    std::uint64_t heapStructClockEvictions = 0;
    std::uint64_t heapStructGcReclaimed = 0;
};

void pinValueInPools(const Value& value) noexcept;
[[nodiscard]] bool hasAnyPinnedPoolValues() noexcept;
void beginValuePoolGcMark() noexcept;
void markValueInPools(const Value& value) noexcept;
void markStackRootValueInPools(const Value& value) noexcept;
std::size_t finishValuePoolGcSweep() noexcept;
std::size_t reclaimUnreferencedPoolValues(
    const ShadowStack* activeShadowStack = nullptr) noexcept;
[[nodiscard]] RuntimeValuePoolStatistics getRuntimeValuePoolStatistics() noexcept;

[[nodiscard]] bool tryExecuteRuntimeIntrinsic(
    std::string_view name,
    ValueSpan arguments,
    ManagedHeap* heap,
    Value& result,
    std::string& errorMessage);

} // namespace realscript::runtime
