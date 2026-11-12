#include "RuntimeInternal.h"

#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace realscript::runtime {

semantic::SymbolId fixedTypeId() noexcept {
    static const semantic::SymbolId id =
        semantic::stableTypeId("RealScript.Math::Fixed");
    return id;
}

semantic::SymbolId fixed16TypeId() noexcept {
    static const semantic::SymbolId id =
        semantic::stableTypeId("RealScript.Math::Fixed16");
    return id;
}

semantic::SymbolId vec2TypeId() noexcept {
    static const semantic::SymbolId id =
        semantic::stableTypeId("RealScript.Math::Vec2");
    return id;
}

semantic::SymbolId vec3TypeId() noexcept {
    static const semantic::SymbolId id =
        semantic::stableTypeId("RealScript.Math::Vec3");
    return id;
}

semantic::SymbolId quatTypeId() noexcept {
    static const semantic::SymbolId id =
        semantic::stableTypeId("RealScript.Math::Quat");
    return id;
}

std::optional<semantic::SymbolId> structValueTypeId(const Value& value) noexcept {
    if (std::holds_alternative<FixedValue>(value)) return fixedTypeId();
    if (std::holds_alternative<Fixed16Value>(value)) return fixed16TypeId();
    if (std::holds_alternative<Vec2Value>(value)) return vec2TypeId();
    if (std::holds_alternative<Vec3Value>(value)) return vec3TypeId();
    if (std::holds_alternative<QuatValue>(value)) return quatTypeId();
    if (const auto* s = std::get_if<StructValue>(&value)) return s->typeId;
    return std::nullopt;
}

std::optional<Value> makeDefaultInlineStructValue(semantic::SymbolId typeId) noexcept {
    if (typeId == fixedTypeId()) return Value{FixedValue{}};
    if (typeId == fixed16TypeId()) return Value{Fixed16Value{}};
    if (typeId == vec2TypeId()) return Value{Vec2Value{}};
    if (typeId == vec3TypeId()) return Value{Vec3Value{}};
    if (typeId == quatTypeId()) return Value{QuatValue{}};
    return std::nullopt;
}

std::optional<InlineStructSlot> encodeInlineStructSlot(
    const Value& value) noexcept {
    InlineStructSlot slot{};
    if (const auto* b = std::get_if<bool>(&value)) {
        slot.kind = InlineStructSlot::Kind::Bool;
        slot.setBits(*b ? 1u : 0u);
        return slot;
    }
    if (const auto* v = std::get_if<ByteValue>(&value)) {
        slot.kind = InlineStructSlot::Kind::Byte;
        slot.setBits(v->value);
        return slot;
    }
    if (const auto* v = std::get_if<SByteValue>(&value)) {
        slot.kind = InlineStructSlot::Kind::SByte;
        slot.setBits(static_cast<std::uint8_t>(v->value));
        return slot;
    }
    if (const auto* v = std::get_if<ShortValue>(&value)) {
        slot.kind = InlineStructSlot::Kind::Short;
        slot.setBits(static_cast<std::uint16_t>(v->value));
        return slot;
    }
    if (const auto* v = std::get_if<UShortValue>(&value)) {
        slot.kind = InlineStructSlot::Kind::UShort;
        slot.setBits(v->value);
        return slot;
    }
    if (const auto* v = std::get_if<std::int64_t>(&value)) {
        slot.kind = InlineStructSlot::Kind::Int;
        slot.setBits(static_cast<std::uint64_t>(*v));
        return slot;
    }
    if (const auto* v = std::get_if<UIntValue>(&value)) {
        slot.kind = InlineStructSlot::Kind::UInt;
        slot.setBits(v->value);
        return slot;
    }
    if (const auto* v = std::get_if<LongValue>(&value)) {
        slot.kind = InlineStructSlot::Kind::Long;
        slot.setBits(static_cast<std::uint64_t>(v->value));
        return slot;
    }
    if (const auto* v = std::get_if<ULongValue>(&value)) {
        slot.kind = InlineStructSlot::Kind::ULong;
        slot.setBits(v->value);
        return slot;
    }
    if (const auto* v = std::get_if<FloatValue>(&value)) {
        slot.kind = InlineStructSlot::Kind::Float;
        std::uint32_t fbits = 0;
        std::memcpy(&fbits, &v->value, sizeof(fbits));
        slot.setBits(fbits);
        return slot;
    }
    if (const auto* v = std::get_if<double>(&value)) {
        slot.kind = InlineStructSlot::Kind::Double;
        std::uint64_t dbits = 0;
        std::memcpy(&dbits, v, sizeof(dbits));
        slot.setBits(dbits);
        return slot;
    }
    if (const auto* v = std::get_if<CharValue>(&value)) {
        slot.kind = InlineStructSlot::Kind::Char;
        slot.setBits(v->value);
        return slot;
    }
    if (const auto* v = std::get_if<FixedValue>(&value)) {
        slot.kind = InlineStructSlot::Kind::Fixed;
        slot.setBits(static_cast<std::uint32_t>(v->raw));
        return slot;
    }
    if (const auto* v = std::get_if<Fixed16Value>(&value)) {
        slot.kind = InlineStructSlot::Kind::Fixed16;
        slot.setBits(static_cast<std::uint64_t>(v->raw));
        return slot;
    }
    if (EnumValue ev{}; value.tryGetEnum(ev)) {
        if (ev.value >= -8388608 && ev.value <= 8388607) {
            slot.kind = InlineStructSlot::Kind::Enum;
            slot.setBits(ev.typeId);
            slot.setAuxInt24(static_cast<std::int32_t>(ev.value));
            return slot;
        }
        return std::nullopt;
    }
    if (const auto* v = std::get_if<Vec2Value>(&value)) {
        slot.kind = InlineStructSlot::Kind::Vec2;
        slot.lo = static_cast<std::uint32_t>(v->x);
        slot.hi = static_cast<std::uint32_t>(v->y);
        return slot;
    }
    if (const auto* v = std::get_if<Vec3Value>(&value)) {
        if (v->z >= -8388608 && v->z <= 8388607) {
            slot.kind = InlineStructSlot::Kind::Vec3;
            slot.lo = static_cast<std::uint32_t>(v->x);
            slot.hi = static_cast<std::uint32_t>(v->y);
            slot.setAuxInt24(v->z);
            return slot;
        }
        return std::nullopt;
    }
    if (QuatValue qv{}; value.tryGetQuat(qv)) {
        const auto inInt16 = [](std::int32_t c) noexcept {
            return c >= std::numeric_limits<std::int16_t>::min() &&
                   c <= std::numeric_limits<std::int16_t>::max();
        };
        if (inInt16(qv.x) && inInt16(qv.y) && inInt16(qv.z) && inInt16(qv.w)) {
            slot.kind = InlineStructSlot::Kind::Quat;
            slot.lo = static_cast<std::uint32_t>(
                          static_cast<std::uint16_t>(static_cast<std::int16_t>(qv.x))) |
                (static_cast<std::uint32_t>(
                     static_cast<std::uint16_t>(static_cast<std::int16_t>(qv.y)))
                 << 16u);
            slot.hi = static_cast<std::uint32_t>(
                          static_cast<std::uint16_t>(static_cast<std::int16_t>(qv.z))) |
                (static_cast<std::uint32_t>(
                     static_cast<std::uint16_t>(static_cast<std::int16_t>(qv.w)))
                 << 16u);
            return slot;
        }
        return std::nullopt;
    }
    if (std::holds_alternative<NullString>(value)) {
        slot.kind = InlineStructSlot::Kind::NullString;
        return slot;
    }
    if (std::holds_alternative<NullObject>(value)) {
        slot.kind = InlineStructSlot::Kind::NullObject;
        return slot;
    }
    if (std::holds_alternative<NullArray>(value)) {
        slot.kind = InlineStructSlot::Kind::NullArray;
        return slot;
    }
    if (const auto* sv = std::get_if<StringValue>(&value)) {
        slot.kind = InlineStructSlot::Kind::String;
        slot.lo = sv->id;
        return slot;
    }

    return std::nullopt;
}

Value decodeInlineStructSlot(const InlineStructSlot& slot) noexcept {
    const auto bits = slot.bits();
    switch (slot.kind) {
    case InlineStructSlot::Kind::Bool:
        return bits != 0;
    case InlineStructSlot::Kind::Byte:
        return ByteValue{static_cast<std::uint8_t>(bits)};
    case InlineStructSlot::Kind::SByte:
        return SByteValue{static_cast<std::int8_t>(static_cast<std::uint8_t>(bits))};
    case InlineStructSlot::Kind::Short:
        return ShortValue{static_cast<std::int16_t>(static_cast<std::uint16_t>(bits))};
    case InlineStructSlot::Kind::UShort:
        return UShortValue{static_cast<std::uint16_t>(bits)};
    case InlineStructSlot::Kind::Int:
        return static_cast<std::int64_t>(bits);
    case InlineStructSlot::Kind::UInt:
        return UIntValue{static_cast<std::uint32_t>(bits)};
    case InlineStructSlot::Kind::Long:
        return LongValue{static_cast<std::int64_t>(bits)};
    case InlineStructSlot::Kind::ULong:
        return ULongValue{bits};
    case InlineStructSlot::Kind::Float: {
        std::uint32_t fbits = slot.lo;
        float val = 0.0f;
        std::memcpy(&val, &fbits, sizeof(val));
        return FloatValue{val};
    }
    case InlineStructSlot::Kind::Double: {
        double val = 0.0;
        std::memcpy(&val, &bits, sizeof(val));
        return val;
    }
    case InlineStructSlot::Kind::Char:
        return CharValue{static_cast<char16_t>(bits)};
    case InlineStructSlot::Kind::Fixed:
        return FixedValue{static_cast<std::int32_t>(slot.lo)};
    case InlineStructSlot::Kind::Fixed16:
        return Fixed16Value{static_cast<std::int64_t>(bits)};
    case InlineStructSlot::Kind::Enum:
        return EnumValue{
            bits,
            static_cast<std::int64_t>(slot.auxInt24()),
        };
    case InlineStructSlot::Kind::Vec2:
        return Vec2Value{
            static_cast<std::int32_t>(slot.lo),
            static_cast<std::int32_t>(slot.hi),
        };
    case InlineStructSlot::Kind::Vec3:
        return Vec3Value{
            static_cast<std::int32_t>(slot.lo),
            static_cast<std::int32_t>(slot.hi),
            slot.auxInt24(),
        };
    case InlineStructSlot::Kind::Quat:
        return QuatValue{
            static_cast<std::int32_t>(static_cast<std::int16_t>(slot.lo & 0xFFFFu)),
            static_cast<std::int32_t>(static_cast<std::int16_t>(slot.lo >> 16u)),
            static_cast<std::int32_t>(static_cast<std::int16_t>(slot.hi & 0xFFFFu)),
            static_cast<std::int32_t>(static_cast<std::int16_t>(slot.hi >> 16u)),
        };
    case InlineStructSlot::Kind::NullString:
        return NullString{};
    case InlineStructSlot::Kind::NullObject:
        return NullObject{};
    case InlineStructSlot::Kind::NullArray:
        return NullArray{};
    case InlineStructSlot::Kind::String:
        return detail::lookupRuntimeStringById(slot.lo);

    case InlineStructSlot::Kind::None:
        break;
    }
    return std::monostate{};
}

StructStorageHandle::StructStorageHandle(std::shared_ptr<const StructStorage> store)
    : raw_(detail::registerHeapStructStorage(std::move(store))) {}

StructStorageHandle::StructStorageHandle(std::shared_ptr<StructStorage> store)
    : raw_(detail::registerHeapStructStorage(
          std::shared_ptr<const StructStorage>(std::move(store)))) {}

const StructStorage* StructStorageHandle::get() const noexcept {
    if (!isStoragePtr()) return nullptr;
    return detail::lookupHeapStructStorage(raw_);
}

const InlineStructArenaEntry& StructValue::inlineEntry() const noexcept {
    return detail::lookupInlineArenaEntry(storage.inlineArenaIndex());
}

std::uint16_t StructValue::nestedFieldMask() const noexcept {
    return isInline ? inlineEntry().nestedFieldMask : 0u;
}

semantic::SymbolId StructValue::nestedTypeId() const noexcept {
    return isInline ? inlineEntry().nestedTypeId : 0u;
}

bool operator==(const StructValue& left, const StructValue& right) {
    if (left.typeId != right.typeId) return false;
    const auto count = left.fieldCount();
    if (count != right.fieldCount()) return false;
    if (left.isInline && right.isInline) {
        if (left.storage.raw_ == right.storage.raw_ &&
            left.inlineSlotCount == right.inlineSlotCount &&
            left.packedOffsetsAndLengths == right.packedOffsetsAndLengths) {
            return true;
        }
        const auto& le = left.inlineEntry();
        const auto& re = right.inlineEntry();
        if (le.nestedTypeId == re.nestedTypeId &&
            left.inlineSlotCount == right.inlineSlotCount &&
            left.packedOffsetsAndLengths == right.packedOffsetsAndLengths &&
            le.nestedFieldMask == re.nestedFieldMask) {
            const std::size_t slots =
                left.inlineSlotCount != 0 ? left.inlineSlotCount : count;
            for (std::size_t i = 0; i < slots; ++i) {
                if (le.slots[i] != re.slots[i]) return false;
            }
            return true;
        }
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

StringValue::StringValue(const std::string& value) {
    *this = detail::internRuntimeString(value);
}

StringValue::StringValue(std::string&& value) {
    *this = detail::internRuntimeString(value);
}

StringValue::StringValue(std::string_view value) {
    *this = detail::internRuntimeString(value);
}

const std::string& StringValue::str() const noexcept {
    return ptr ? *ptr : detail::emptyRuntimePoolString();
}

namespace {

bool tryPackInlineStructFields(
    semantic::SymbolId typeId,
    ValueSpan fields,
    StructValue& outVal) noexcept {
    if (fields.size() > StructValue::MaxInlineFields) return false;
    outVal = StructValue{};
    outVal.typeId = typeId;
    outVal.inlineCount = static_cast<std::uint8_t>(fields.size());
    outVal.isInline = true;
    InlineStructArenaEntry entry{};
    std::size_t slotCursor = 0;
    bool anyNested = false;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (auto slot = encodeInlineStructSlot(fields[i])) {
            if (slotCursor + 1 > StructValue::MaxInlineSlots) return false;
            outVal.packedOffsetsAndLengths |=
                static_cast<std::uint8_t>((slotCursor & 0x3u) << (i * 2));
            entry.nestedFieldMask |= static_cast<std::uint16_t>(1u << (i * 2));
            entry.slots[slotCursor++] = *slot;
            continue;
        }
        if (const auto* nested = std::get_if<StructValue>(&fields[i]);
            nested && nested->isInline && nested->inlineSlotCount > 0 &&
            nested->inlineSlotCount <= 3 && (nested->nestedFieldMask() >> 8u) == 0 &&
            nested->nestedTypeId() == 0 && !anyNested) {
            if (slotCursor + nested->inlineSlotCount <= StructValue::MaxInlineSlots) {
                anyNested = true;
                const auto& nestedEntry = nested->inlineEntry();
                entry.nestedTypeId = nested->typeId;
                outVal.packedOffsetsAndLengths |=
                    static_cast<std::uint8_t>((slotCursor & 0x3u) << (i * 2));
                entry.nestedFieldMask |=
                    static_cast<std::uint16_t>((nested->inlineSlotCount & 0x3u) << (i * 2));
                entry.nestedFieldMask |=
                    static_cast<std::uint16_t>(((i & 0x3u) + 1u) << 8u);
                for (std::size_t k = 0; k < nested->inlineSlotCount; ++k) {
                    entry.slots[slotCursor++] = nestedEntry.slots[k];
                }
                continue;
            }
        }
        return false;
    }
    if (!anyNested) {
        entry.nestedTypeId = 0;
        outVal.packedOffsetsAndLengths = 0;
        entry.nestedFieldMask = 0;
    }
    outVal.inlineSlotCount = static_cast<std::uint8_t>(slotCursor);
    outVal.storage.setInlineArenaIndex(detail::allocateInlineArenaEntry(entry));
    return true;
}

std::optional<std::int32_t> extractRawFixedField(const Value& fieldValue) noexcept {
    if (const auto* f = std::get_if<FixedValue>(&fieldValue)) {
        return f->raw;
    }
    if (const auto* i = std::get_if<std::int64_t>(&fieldValue)) {
        return static_cast<std::int32_t>(*i);
    }
    if (const auto* l = std::get_if<LongValue>(&fieldValue)) {
        return static_cast<std::int32_t>(l->value);
    }
    if (const auto* s = std::get_if<StructValue>(&fieldValue)) {
        if (s->typeId == fixedTypeId() && s->storage && s->storage->fields.size() == 1) {
            if (const auto* rawInt = std::get_if<std::int64_t>(&s->storage->fields[0])) {
                return static_cast<std::int32_t>(*rawInt);
            }
            if (const auto* rawLong = std::get_if<LongValue>(&s->storage->fields[0])) {
                return static_cast<std::int32_t>(rawLong->value);
            }
        }
    }
    return std::nullopt;
}

} // namespace

StructValue makeStructValueFromFields(
    semantic::SymbolId typeId,
    ValueSpan fields) {
    StructValue inlineVal;
    if (tryPackInlineStructFields(typeId, fields, inlineVal)) {
        return inlineVal;
    }
    auto storage = std::make_shared<StructStorage>();
    storage->fields.assign(fields.begin(), fields.end());
    return StructValue{typeId, std::move(storage)};
}

StructValue makeStructValueFromFields(
    semantic::SymbolId typeId,
    std::vector<Value> fields) {
    StructValue inlineVal;
    if (tryPackInlineStructFields(typeId, ValueSpan(fields), inlineVal)) {
        return inlineVal;
    }
    auto storage = std::make_shared<StructStorage>();
    storage->fields = std::move(fields);
    return StructValue{typeId, std::move(storage)};
}

std::optional<Value> structValueFieldAt(
    const StructValue& structure,
    std::size_t index) noexcept {
    if (structure.isInline) {
        if (index >= structure.inlineCount) return std::nullopt;
        const auto& entry = structure.inlineEntry();
        const std::uint8_t highByte =
            static_cast<std::uint8_t>(entry.nestedFieldMask >> 8u);
        if (highByte != 0) {
            const std::size_t nestedIdx = static_cast<std::size_t>(highByte - 1u) & 0x3u;
            const std::uint8_t offset = structure.fieldOffset(index);
            const std::uint8_t len = structure.fieldLength(index);
            if (index == nestedIdx) {
                const semantic::SymbolId nestedType = entry.nestedTypeId;
                if (nestedType == 0 || offset + len > structure.inlineSlotCount) {
                    return std::nullopt;
                }
                StructValue nested;
                nested.typeId = nestedType;
                nested.isInline = true;
                nested.inlineCount = len;
                nested.inlineSlotCount = len;
                InlineStructArenaEntry nestedEntry{};
                for (std::uint8_t k = 0; k < len; ++k) {
                    nestedEntry.slots[k] = entry.slots[offset + k];
                }
                nested.storage.setInlineArenaIndex(
                    detail::allocateInlineArenaEntry(nestedEntry));
                return Value{nested};
            }
            if (offset >= structure.inlineSlotCount) return std::nullopt;
            return decodeInlineStructSlot(entry.slots[offset]);
        }
        return decodeInlineStructSlot(entry.slots[index]);
    }
    if (!structure.storage || index >= structure.storage->fields.size()) {
        return std::nullopt;
    }
    return structure.storage->fields[index];
}

std::optional<Value> loadInlineStructField(
    const Value& value,
    std::size_t fieldIndex) noexcept {
    if (const auto* f = std::get_if<FixedValue>(&value)) {
        if (fieldIndex == 0) return Value{static_cast<std::int64_t>(f->raw)};
        return std::nullopt;
    }
    if (const auto* f16 = std::get_if<Fixed16Value>(&value)) {
        if (fieldIndex == 0) return Value{static_cast<std::int64_t>(f16->raw)};
        return std::nullopt;
    }
    if (const auto* v2 = std::get_if<Vec2Value>(&value)) {
        if (fieldIndex == 0) return Value{FixedValue{v2->x}};
        if (fieldIndex == 1) return Value{FixedValue{v2->y}};
        return std::nullopt;
    }
    if (const auto* v3 = std::get_if<Vec3Value>(&value)) {
        if (fieldIndex == 0) return Value{FixedValue{v3->x}};
        if (fieldIndex == 1) return Value{FixedValue{v3->y}};
        if (fieldIndex == 2) return Value{FixedValue{v3->z}};
        return std::nullopt;
    }
    if (QuatValue q{}; value.tryGetQuat(q)) {
        if (fieldIndex == 0) return Value{FixedValue{q.x}};
        if (fieldIndex == 1) return Value{FixedValue{q.y}};
        if (fieldIndex == 2) return Value{FixedValue{q.z}};
        if (fieldIndex == 3) return Value{FixedValue{q.w}};
        return std::nullopt;
    }
    if (const auto* s = std::get_if<StructValue>(&value); s && s->isInline) {
        return structValueFieldAt(*s, fieldIndex);
    }
    return std::nullopt;
}

std::optional<Value> storeInlineStructField(
    const Value& structure,
    std::size_t fieldIndex,
    const Value& fieldValue) noexcept {
    if (std::holds_alternative<FixedValue>(structure)) {
        if (fieldIndex != 0) return std::nullopt;
        if (const auto* i = std::get_if<std::int64_t>(&fieldValue)) {
            return Value{FixedValue{static_cast<std::int32_t>(*i)}};
        }
        if (const auto* l = std::get_if<LongValue>(&fieldValue)) {
            return Value{FixedValue{static_cast<std::int32_t>(l->value)}};
        }
        return std::nullopt;
    }
    if (std::holds_alternative<Fixed16Value>(structure)) {
        if (fieldIndex != 0) return std::nullopt;
        if (const auto* l = std::get_if<LongValue>(&fieldValue)) {
            return Value{Fixed16Value{l->value}};
        }
        if (const auto* i = std::get_if<std::int64_t>(&fieldValue)) {
            return Value{Fixed16Value{*i}};
        }
        return std::nullopt;
    }
    if (const auto* v2 = std::get_if<Vec2Value>(&structure)) {
        const auto raw = extractRawFixedField(fieldValue);
        if (!raw) return std::nullopt;
        Vec2Value out = *v2;
        if (fieldIndex == 0) {
            out.x = *raw;
            return Value{out};
        }
        if (fieldIndex == 1) {
            out.y = *raw;
            return Value{out};
        }
        return std::nullopt;
    }
    if (const auto* v3 = std::get_if<Vec3Value>(&structure)) {
        const auto raw = extractRawFixedField(fieldValue);
        if (!raw) return std::nullopt;
        Vec3Value out = *v3;
        if (fieldIndex == 0) {
            out.x = *raw;
            return Value{out};
        }
        if (fieldIndex == 1) {
            out.y = *raw;
            return Value{out};
        }
        if (fieldIndex == 2) {
            out.z = *raw;
            return Value{out};
        }
        return std::nullopt;
    }
    if (QuatValue out{}; structure.tryGetQuat(out)) {
        const auto raw = extractRawFixedField(fieldValue);
        if (!raw) return std::nullopt;
        if (fieldIndex == 0) {
            out.x = *raw;
            return Value{out};
        }
        if (fieldIndex == 1) {
            out.y = *raw;
            return Value{out};
        }
        if (fieldIndex == 2) {
            out.z = *raw;
            return Value{out};
        }
        if (fieldIndex == 3) {
            out.w = *raw;
            return Value{out};
        }
        return std::nullopt;
    }
    if (const auto* s = std::get_if<StructValue>(&structure); s && s->isInline) {
        if (fieldIndex < s->inlineCount) {
            std::array<Value, StructValue::MaxInlineFields> unpacked{};
            for (std::size_t i = 0; i < s->inlineCount; ++i) {
                if (i == fieldIndex) {
                    unpacked[i] = fieldValue;
                } else {
                    const auto existing = structValueFieldAt(*s, i);
                    if (!existing) return std::nullopt;
                    unpacked[i] = *existing;
                }
            }
            return Value{makeStructValueFromFields(
                s->typeId,
                ValueSpan(unpacked.data(), s->inlineCount))};
        }
        return std::nullopt;
    }
    return std::nullopt;
}

namespace detail {

std::uint64_t computeStructFieldsHash(const std::vector<Value>& fields) noexcept {
    std::uint64_t h = 14695981039346656037ULL ^ fields.size();
    for (const auto& v : fields) {
        std::uint64_t vw =
            static_cast<std::uint64_t>(v.index()) * 0x9e3779b97f4a7c15ULL;
        if (const auto* bv = std::get_if<bool>(&v)) {
            vw ^= *bv ? 1ULL : 0ULL;
        } else if (const auto* byv = std::get_if<ByteValue>(&v)) {
            vw ^= byv->value;
        } else if (const auto* sbv = std::get_if<SByteValue>(&v)) {
            vw ^= static_cast<std::uint8_t>(sbv->value);
        } else if (const auto* shv = std::get_if<ShortValue>(&v)) {
            vw ^= static_cast<std::uint16_t>(shv->value);
        } else if (const auto* ushv = std::get_if<UShortValue>(&v)) {
            vw ^= ushv->value;
        } else if (const auto* uiv = std::get_if<UIntValue>(&v)) {
            vw ^= uiv->value;
        } else if (const auto* ulv = std::get_if<ULongValue>(&v)) {
            vw ^= ulv->value;
        } else if (const auto* iv = std::get_if<std::int64_t>(&v)) {
            vw ^= static_cast<std::uint64_t>(*iv);
        } else if (const auto* lv = std::get_if<LongValue>(&v)) {
            vw ^= static_cast<std::uint64_t>(lv->value);
        } else if (const auto* flv = std::get_if<FloatValue>(&v)) {
            float norm = (flv->value == 0.0f) ? 0.0f : flv->value;
            std::uint32_t fbits = 0;
            std::memcpy(&fbits, &norm, sizeof(fbits));
            vw ^= fbits;
        } else if (const auto* dv = std::get_if<double>(&v)) {
            double norm = (*dv == 0.0) ? 0.0 : *dv;
            std::uint64_t dbits = 0;
            std::memcpy(&dbits, &norm, sizeof(dbits));
            vw ^= dbits;
        } else if (const auto* chv = std::get_if<CharValue>(&v)) {
            vw ^= chv->value;
        } else if (EnumValue env{}; v.tryGetEnum(env)) {
            vw ^= env.typeId ^
                (static_cast<std::uint64_t>(env.value) * 0x9e3779b97f4a7c15ULL);
        } else if (const auto* fx = std::get_if<FixedValue>(&v)) {
            vw ^= static_cast<std::uint32_t>(fx->raw);
        } else if (const auto* fx16 = std::get_if<Fixed16Value>(&v)) {
            vw ^= static_cast<std::uint64_t>(fx16->raw);
        } else if (const auto* v2 = std::get_if<Vec2Value>(&v)) {
            vw ^= static_cast<std::uint32_t>(v2->x) |
                (static_cast<std::uint64_t>(static_cast<std::uint32_t>(v2->y)) << 32u);
        } else if (const auto* v3 = std::get_if<Vec3Value>(&v)) {
            vw ^= static_cast<std::uint32_t>(v3->x) ^
                (static_cast<std::uint64_t>(static_cast<std::uint32_t>(v3->y)) << 21u) ^
                (static_cast<std::uint64_t>(static_cast<std::uint32_t>(v3->z)) << 42u);
        } else if (QuatValue qt{}; v.tryGetQuat(qt)) {
            vw ^= static_cast<std::uint32_t>(qt.x) ^
                (static_cast<std::uint64_t>(static_cast<std::uint32_t>(qt.y)) << 16u) ^
                (static_cast<std::uint64_t>(static_cast<std::uint32_t>(qt.z)) << 32u) ^
                (static_cast<std::uint64_t>(static_cast<std::uint32_t>(qt.w)) << 48u);
        } else if (const auto* sv = std::get_if<StringValue>(&v)) {
            vw ^= sv->id;
        } else if (const auto* st = std::get_if<StructValue>(&v)) {
            vw ^= st->typeId ^ (static_cast<std::uint64_t>(st->storage.raw_) << 16u);
        } else if (const auto* obj = std::get_if<ObjectRef>(&v)) {
            vw ^= (static_cast<std::uint64_t>(obj->slot) << 32u) ^ obj->generation;
        } else if (NativeHandle nh{}; v.tryGetNativeHandle(nh)) {
            vw ^= nh.typeId ^ (static_cast<std::uint64_t>(nh.slot) << 16u) ^
                nh.generation;
        }
        h ^= vw + 0x9e3779b97f4a7c15ULL + (h << 6u) + (h >> 2u);
    }
    return h;
}

bool isStringLikeValue(const Value& val) noexcept {
    if (std::holds_alternative<std::string>(val) ||
        std::holds_alternative<NullString>(val)) {
        return true;
    }
    if (const auto* ref = std::get_if<ObjectRef>(&val)) {
        return ref->kind == ObjectKind::String;
    }
    return false;
}

std::string extractIntrinsicString(const Value& val, const ManagedHeap* heap) {
    if (std::holds_alternative<NullString>(val) ||
        std::holds_alternative<std::monostate>(val)) {
        return {};
    }
    if (const auto* s = std::get_if<std::string>(&val)) return *s;
    if (const auto* ref = std::get_if<ObjectRef>(&val)) {
        if (heap) {
            if (const auto sv = heap->stringView(*ref)) {
                return std::string(*sv);
            }
        }
    }
    return valueToString(val, heap);
}

bool intrinsicValuesEqual(const Value& a, const Value& b, const ManagedHeap* heap) {
    if (isStringLikeValue(a) || isStringLikeValue(b)) {
        const bool aNull = std::holds_alternative<NullString>(a);
        const bool bNull = std::holds_alternative<NullString>(b);
        if (aNull || bNull) return aNull && bNull;
        return extractIntrinsicString(a, heap) == extractIntrinsicString(b, heap);
    }
    return a == b;
}

bool intrinsicValueLess(const Value& a, const Value& b, const ManagedHeap* heap) {
    if (isStringLikeValue(a) || isStringLikeValue(b)) {
        const bool aNull = std::holds_alternative<NullString>(a);
        const bool bNull = std::holds_alternative<NullString>(b);
        if (aNull != bNull) return aNull;
        if (aNull && bNull) return false;
        return extractIntrinsicString(a, heap) < extractIntrinsicString(b, heap);
    }
    if (a.index() == b.index()) {
        if (const auto* ia = std::get_if<std::int64_t>(&a)) return *ia < std::get<std::int64_t>(b);
        if (const auto* la = std::get_if<LongValue>(&a)) return la->value < std::get<LongValue>(b).value;
        if (const auto* uia = std::get_if<UIntValue>(&a)) return uia->value < std::get<UIntValue>(b).value;
        if (const auto* ula = std::get_if<ULongValue>(&a)) return ula->value < std::get<ULongValue>(b).value;
        if (const auto* bya = std::get_if<ByteValue>(&a)) return bya->value < std::get<ByteValue>(b).value;
        if (const auto* sba = std::get_if<SByteValue>(&a)) return sba->value < std::get<SByteValue>(b).value;
        if (const auto* sha = std::get_if<ShortValue>(&a)) return sha->value < std::get<ShortValue>(b).value;
        if (const auto* usa = std::get_if<UShortValue>(&a)) return usa->value < std::get<UShortValue>(b).value;
        if (const auto* cha = std::get_if<CharValue>(&a)) return cha->value < std::get<CharValue>(b).value;
        if (const auto* ba = std::get_if<bool>(&a)) return (!*ba && std::get<bool>(b));
        if (EnumValue ena{}; a.tryGetEnum(ena)) {
            const auto enb = std::get<EnumValue>(b);
            return ena.typeId != enb.typeId ? (ena.typeId < enb.typeId) : (ena.value < enb.value);
        }
        if (const auto* fxa = std::get_if<FixedValue>(&a)) return fxa->raw < std::get<FixedValue>(b).raw;
        if (const auto* f16a = std::get_if<Fixed16Value>(&a)) return f16a->raw < std::get<Fixed16Value>(b).raw;
        if (const auto* da = std::get_if<double>(&a)) return *da < std::get<double>(b);
        if (const auto* fla = std::get_if<FloatValue>(&a)) return fla->value < std::get<FloatValue>(b).value;
    }
    return valueToString(a, heap) < valueToString(b, heap);
}

} // namespace detail

} // namespace realscript::runtime
