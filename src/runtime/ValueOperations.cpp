#include "RuntimeInternal.h"

#include <atomic>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
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

semantic::SymbolId fixed64TypeId() noexcept {
    static const semantic::SymbolId id =
        semantic::stableTypeId("RealScript.Math::Fixed64");
    return id;
}

semantic::SymbolId quatTypeId() noexcept {
    static const semantic::SymbolId id =
        semantic::stableTypeId("RealScript.Math::Quat");
    return id;
}

semantic::SymbolId vec2fTypeId() noexcept {
    static const semantic::SymbolId id =
        semantic::stableTypeId("RealScript.Math::Vec2f");
    return id;
}

semantic::SymbolId vec3fTypeId() noexcept {
    static const semantic::SymbolId id =
        semantic::stableTypeId("RealScript.Math::Vec3f");
    return id;
}

semantic::SymbolId quatfTypeId() noexcept {
    static const semantic::SymbolId id =
        semantic::stableTypeId("RealScript.Math::Quatf");
    return id;
}

std::optional<semantic::SymbolId> structValueTypeId(const Value& value) noexcept {
    if (std::holds_alternative<FixedValue>(value)) return fixedTypeId();
    if (std::holds_alternative<Fixed16Value>(value)) return fixed16TypeId();
    if (std::holds_alternative<Fixed64Value>(value)) return fixed64TypeId();
    if (std::holds_alternative<Vec2Value>(value)) return vec2TypeId();
    if (std::holds_alternative<Vec3Value>(value)) return vec3TypeId();
    if (std::holds_alternative<QuatValue>(value)) return quatTypeId();
    if (std::holds_alternative<Vec2fValue>(value)) return vec2fTypeId();
    if (std::holds_alternative<Vec3fValue>(value)) return vec3fTypeId();
    if (std::holds_alternative<QuatfValue>(value)) return quatfTypeId();
    if (const auto* s = std::get_if<StructValue>(&value)) return s->typeId;
    return std::nullopt;
}

std::optional<Value> makeDefaultInlineStructValue(semantic::SymbolId typeId) noexcept {
    if (typeId == fixedTypeId()) return Value{FixedValue{}};
    if (typeId == fixed16TypeId()) return Value{Fixed16Value{}};
    if (typeId == fixed64TypeId()) return Value{Fixed64Value{}};
    if (typeId == vec2TypeId()) return Value{Vec2Value{}};
    if (typeId == vec3TypeId()) return Value{Vec3Value{}};
    if (typeId == quatTypeId()) return Value{QuatValue{}};
    if (typeId == vec2fTypeId()) return Value{Vec2fValue{}};
    if (typeId == vec3fTypeId()) return Value{Vec3fValue{}};
    if (typeId == quatfTypeId()) return Value{QuatfValue{}};
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
    if (const auto* v = std::get_if<Fixed64Value>(&value)) {
        slot.kind = InlineStructSlot::Kind::Fixed64;
        slot.setBits(static_cast<std::uint64_t>(v->raw));
        return slot;
    }
    if (const auto* v = std::get_if<Vec2fValue>(&value)) {
        slot.kind = InlineStructSlot::Kind::Vec2f;
        std::memcpy(&slot.lo, &v->x, sizeof(float));
        std::memcpy(&slot.hi, &v->y, sizeof(float));
        return slot;
    }
    if (const auto* v = std::get_if<Vec3fValue>(&value)) {
        std::uint32_t zBits = 0;
        std::memcpy(&zBits, &v->z, sizeof(float));
        if ((zBits & 0xFFu) == 0u) {
            slot.kind = InlineStructSlot::Kind::Vec3f;
            std::memcpy(&slot.lo, &v->x, sizeof(float));
            std::memcpy(&slot.hi, &v->y, sizeof(float));
            slot.aux0 = static_cast<std::uint8_t>((zBits >> 8u) & 0xFFu);
            slot.aux1 = static_cast<std::uint8_t>((zBits >> 16u) & 0xFFu);
            slot.aux2 = static_cast<std::uint8_t>((zBits >> 24u) & 0xFFu);
            return slot;
        }
        return std::nullopt;
    }
    if (QuatfValue qf{}; value.tryGetQuatf(qf)) {
        if (qf.x == 0.0f && qf.y == 0.0f) {
            slot.kind = InlineStructSlot::Kind::Quatf;
            std::memcpy(&slot.lo, &qf.z, sizeof(float));
            std::memcpy(&slot.hi, &qf.w, sizeof(float));
            return slot;
        }
        return std::nullopt;
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
    case InlineStructSlot::Kind::Fixed64:
        return Fixed64Value{static_cast<std::int64_t>(bits)};
    case InlineStructSlot::Kind::Vec2f: {
        Vec2fValue out{};
        std::memcpy(&out.x, &slot.lo, sizeof(float));
        std::memcpy(&out.y, &slot.hi, sizeof(float));
        return out;
    }
    case InlineStructSlot::Kind::Vec3f: {
        Vec3fValue out{};
        std::memcpy(&out.x, &slot.lo, sizeof(float));
        std::memcpy(&out.y, &slot.hi, sizeof(float));
        const std::uint32_t zBits =
            (static_cast<std::uint32_t>(slot.aux0) << 8u) |
            (static_cast<std::uint32_t>(slot.aux1) << 16u) |
            (static_cast<std::uint32_t>(slot.aux2) << 24u);
        std::memcpy(&out.z, &zBits, sizeof(float));
        return out;
    }
    case InlineStructSlot::Kind::Quatf: {
        QuatfValue out{};
        std::memcpy(&out.z, &slot.lo, sizeof(float));
        std::memcpy(&out.w, &slot.hi, sizeof(float));
        return out;
    }
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

namespace {

std::atomic<std::uint64_t> g_heapStructStorageAllocations{0};

std::atomic<std::uint64_t>& heapStructAllocCounter() noexcept {
    if (const auto* b = detail::externalValuePoolBridge(); b && b->heapStructAllocCounterPtr) {
        return *static_cast<std::atomic<std::uint64_t>*>(b->heapStructAllocCounterPtr);
    }
    return g_heapStructStorageAllocations;
}

struct FlatStructRegistry {
    static constexpr std::uint32_t kBlockBits = 8u;
    static constexpr std::uint32_t kBlockSize = 1u << kBlockBits; // 256
    static constexpr std::uint32_t kMaxBlocks = 256u;             // 65,536 schemas (uint16_t)

    std::mutex mutex;
    std::array<std::atomic<FlatStructSchema*>, kMaxBlocks> blocks{};
    std::vector<std::unique_ptr<FlatStructSchema[]>> ownedBlocks;
    std::atomic<std::uint32_t> nextSchemaId{1u}; // 0 = invalid
    std::unordered_map<semantic::SymbolId, std::uint16_t> latestByTypeId;

    FlatStructRegistry() noexcept {
        for (auto& b : blocks) b.store(nullptr, std::memory_order_relaxed);
    }

    [[nodiscard]] const FlatStructSchema* getByIdLockFree(std::uint16_t id) const noexcept {
        const std::uint32_t limit = nextSchemaId.load(std::memory_order_acquire);
        if (id == 0u || static_cast<std::uint32_t>(id) >= limit) return nullptr;
        const std::uint32_t blkIdx = static_cast<std::uint32_t>(id) >> kBlockBits;
        if (blkIdx >= kMaxBlocks) return nullptr;
        const auto* blk = blocks[blkIdx].load(std::memory_order_acquire);
        if (!blk) return nullptr;
        return &blk[static_cast<std::uint32_t>(id) & (kBlockSize - 1u)];
    }

    [[nodiscard]] std::uint16_t registerOrReuseUnlocked(FlatStructSchema candidate) {
        const auto existingIt = latestByTypeId.find(candidate.structTypeId);
        if (existingIt != latestByTypeId.end()) {
            if (const auto* cur = getByIdLockFree(existingIt->second)) {
                if (cur->flatSlotCount == candidate.flatSlotCount &&
                    cur->containsManagedReferences == candidate.containsManagedReferences &&
                    cur->containsPoolValues == candidate.containsPoolValues &&
                    cur->directFields.size() == candidate.directFields.size()) {
                    bool same = true;
                    for (std::size_t i = 0; i < cur->directFields.size(); ++i) {
                        const auto& a = cur->directFields[i];
                        const auto& b = candidate.directFields[i];
                        if (a.leafOffset != b.leafOffset ||
                            a.leafCount != b.leafCount ||
                            a.type != b.type ||
                            a.typeId != b.typeId ||
                            a.subSchemaId != b.subSchemaId) {
                            same = false;
                            break;
                        }
                    }
                    if (same) {
                        return existingIt->second;
                    }
                }
            }
        }
        const std::uint32_t id32 = nextSchemaId.load(std::memory_order_relaxed);
        if (id32 >= 0xFFFFu) return 0u;
        const std::uint32_t blkIdx = id32 >> kBlockBits;
        if (!blocks[blkIdx].load(std::memory_order_relaxed)) {
            auto newBlk = std::make_unique<FlatStructSchema[]>(kBlockSize);
            FlatStructSchema* raw = newBlk.get();
            ownedBlocks.push_back(std::move(newBlk));
            blocks[blkIdx].store(raw, std::memory_order_release);
        }
        const auto id16 = static_cast<std::uint16_t>(id32);
        candidate.schemaId = id16;
        blocks[blkIdx].load(std::memory_order_relaxed)[id32 & (kBlockSize - 1u)] = std::move(candidate);
        nextSchemaId.store(id32 + 1u, std::memory_order_release);
        latestByTypeId[blocks[blkIdx].load(std::memory_order_relaxed)[id32 & (kBlockSize - 1u)].structTypeId] = id16;
        return id16;
    }
};

FlatStructRegistry& localFlatStructRegistry() noexcept {
    static FlatStructRegistry instance;
    return instance;
}

FlatStructRegistry& globalFlatStructRegistry() noexcept {
    if (const auto* b = detail::externalValuePoolBridge(); b && b->flatStructRegistryPtr) {
        return *static_cast<FlatStructRegistry*>(b->flatStructRegistryPtr);
    }
    return localFlatStructRegistry();
}

[[nodiscard]] bool isBuiltinSingleCellMathStructId(semantic::SymbolId tid) noexcept {
    return tid == fixedTypeId() ||
        tid == fixed16TypeId() ||
        tid == fixed64TypeId() ||
        tid == vec2TypeId() ||
        tid == vec3TypeId() ||
        tid == quatTypeId() ||
        tid == vec2fTypeId() ||
        tid == vec3fTypeId() ||
        tid == quatfTypeId();
}

struct ThreadFlatStackBanks {
    std::array<const std::vector<Value>*, 64> banks{};
};
thread_local ThreadFlatStackBanks tlsFlatStackBanks{};

} // namespace

const FlatStructSchema* getFlatStructSchema(std::uint16_t schemaId) noexcept {
    return globalFlatStructRegistry().getByIdLockFree(schemaId);
}

const FlatStructSchema* findFlatStructSchemaByTypeId(semantic::SymbolId structTypeId) noexcept {
    if (structTypeId == 0 || isBuiltinSingleCellMathStructId(structTypeId)) return nullptr;
    auto& reg = globalFlatStructRegistry();
    std::lock_guard<std::mutex> lock(reg.mutex);
    const auto it = reg.latestByTypeId.find(structTypeId);
    if (it == reg.latestByTypeId.end()) return nullptr;
    return reg.getByIdLockFree(it->second);
}

std::uint64_t heapStructStorageAllocations() noexcept {
    return heapStructAllocCounter().load(std::memory_order_relaxed);
}

void resetHeapStructStorageAllocations() noexcept {
    heapStructAllocCounter().store(0, std::memory_order_relaxed);
}

namespace detail {

void* localFlatStructRegistryPtr() noexcept {
    return &localFlatStructRegistry();
}

void* localHeapStructAllocCounterPtr() noexcept {
    return &g_heapStructStorageAllocations;
}

std::uint8_t acquireFlatStackBank(const std::vector<Value>* stackVec) noexcept {
    if (const auto* b = externalValuePoolBridge(); b && b->acquireFlatStackBankFn) {
        return b->acquireFlatStackBankFn(static_cast<const void*>(stackVec));
    }
    if (!stackVec) return 0u;
    for (std::uint8_t id = 1u; id < 64u; ++id) {
        if (tlsFlatStackBanks.banks[id] == nullptr) {
            tlsFlatStackBanks.banks[id] = stackVec;
            return id;
        }
    }
    return 0u;
}

void releaseFlatStackBank(std::uint8_t bankId) noexcept {
    if (const auto* b = externalValuePoolBridge(); b && b->releaseFlatStackBankFn) {
        b->releaseFlatStackBankFn(bankId);
        return;
    }
    if (bankId > 0u && bankId < 64u) {
        tlsFlatStackBanks.banks[bankId] = nullptr;
    }
}

const Value* lookupFlatStackSlice(
    std::uint8_t bankId,
    std::uint32_t absOffset,
    std::uint16_t slotCount) noexcept {
    if (const auto* b = externalValuePoolBridge(); b && b->lookupFlatStackSliceFn) {
        return static_cast<const Value*>(b->lookupFlatStackSliceFn(bankId, absOffset, slotCount));
    }
    if (bankId == 0u || bankId >= 64u) return nullptr;
    const auto* vec = tlsFlatStackBanks.banks[bankId];
    if (!vec) return nullptr;
    const std::size_t end = static_cast<std::size_t>(absOffset) + static_cast<std::size_t>(slotCount);
    if (end > vec->size()) return nullptr;
    return vec->data() + absOffset;
}

void registerFlatStructTypes(
    const std::vector<std::pair<semantic::SymbolId, std::vector<BlittableFieldInput>>>& structTypes) {
    if (structTypes.empty()) return;
    auto& reg = globalFlatStructRegistry();
    std::lock_guard<std::mutex> lock(reg.mutex);

    std::unordered_map<semantic::SymbolId, const std::vector<BlittableFieldInput>*> pending;
    pending.reserve(structTypes.size());
    for (const auto& [tid, fields] : structTypes) {
        if (tid == 0 || isBuiltinSingleCellMathStructId(tid)) continue;
        pending[tid] = &fields;
    }

    // 0 = unvisited, 1 = visiting, 2 = resolved, 3 = invalid
    std::unordered_map<semantic::SymbolId, int> state;
    const auto makeDefaultLeaf = [](semantic::PrimitiveType pt, semantic::SymbolId tid) noexcept -> Value {
        switch (pt) {
        case semantic::PrimitiveType::Bool: return false;
        case semantic::PrimitiveType::Byte: return ByteValue{};
        case semantic::PrimitiveType::SByte: return SByteValue{};
        case semantic::PrimitiveType::Short: return ShortValue{};
        case semantic::PrimitiveType::UShort: return UShortValue{};
        case semantic::PrimitiveType::Int: return std::int64_t{0};
        case semantic::PrimitiveType::UInt: return UIntValue{};
        case semantic::PrimitiveType::Long: return LongValue{};
        case semantic::PrimitiveType::ULong: return ULongValue{};
        case semantic::PrimitiveType::Float: return FloatValue{};
        case semantic::PrimitiveType::Double: return 0.0;
        case semantic::PrimitiveType::Char: return CharValue{};
        case semantic::PrimitiveType::String: return NullString{};
        case semantic::PrimitiveType::Object: return NullObject{};
        case semantic::PrimitiveType::Array: return NullArray{};
        case semantic::PrimitiveType::Handle: {
            NativeHandle h;
            h.typeId = tid;
            return h;
        }
        case semantic::PrimitiveType::Enum:
            return EnumValue{tid, 0};
        case semantic::PrimitiveType::Struct:
            if (const auto inl = makeDefaultInlineStructValue(tid)) return *inl;
            return std::monostate{};
        default:
            return std::monostate{};
        }
    };

    const auto resolve = [&](auto& self, semantic::SymbolId tid) -> std::uint16_t {
        const auto stIt = state.find(tid);
        if (stIt != state.end()) {
            if (stIt->second == 2) {
                const auto lIt = reg.latestByTypeId.find(tid);
                return lIt != reg.latestByTypeId.end() ? lIt->second : 0u;
            }
            return 0u;
        }
        const auto pIt = pending.find(tid);
        if (pIt == pending.end()) {
            const auto lIt = reg.latestByTypeId.find(tid);
            return lIt != reg.latestByTypeId.end() ? lIt->second : 0u;
        }
        state[tid] = 1;
        const auto& fields = *pIt->second;
        FlatStructSchema schema{};
        schema.structTypeId = tid;
        schema.directFields.reserve(fields.size());

        std::uint32_t cursor = 0;
        for (const auto& f : fields) {
            if (f.type == semantic::PrimitiveType::Void ||
                f.type == semantic::PrimitiveType::Error ||
                f.type == semantic::PrimitiveType::Null) {
                state[tid] = 3;
                return 0u;
            }
            if (f.type == semantic::PrimitiveType::Struct &&
                f.typeId != 0 &&
                !isBuiltinSingleCellMathStructId(f.typeId)) {
                const std::uint16_t subSid = self(self, f.typeId);
                const auto* subSc = reg.getByIdLockFree(subSid);
                if (subSid == 0u || !subSc || cursor + subSc->flatSlotCount > 0x7FFFu) {
                    state[tid] = 3;
                    return 0u;
                }
                schema.directFields.push_back(FlatStructFieldDesc{
                    static_cast<std::uint16_t>(cursor),
                    subSc->flatSlotCount,
                    f.type,
                    f.typeId,
                    subSid,
                });
                if (subSc->containsManagedReferences) schema.containsManagedReferences = true;
                if (subSc->containsPoolValues) schema.containsPoolValues = true;
                for (const auto subRefOff : subSc->referenceLeafOffsets) {
                    schema.referenceLeafOffsets.push_back(
                        static_cast<std::uint16_t>(cursor + subRefOff));
                }
                schema.prototypeLeaves.insert(
                    schema.prototypeLeaves.end(),
                    subSc->prototypeLeaves.begin(),
                    subSc->prototypeLeaves.end());
                cursor += subSc->flatSlotCount;
                continue;
            }
            if (cursor + 1u > 0x7FFFu) {
                state[tid] = 3;
                return 0u;
            }
            schema.directFields.push_back(FlatStructFieldDesc{
                static_cast<std::uint16_t>(cursor),
                1u,
                f.type,
                f.typeId,
                0u,
            });
            if (f.type == semantic::PrimitiveType::Object ||
                f.type == semantic::PrimitiveType::Array ||
                f.type == semantic::PrimitiveType::String) {
                schema.containsManagedReferences = true;
                schema.referenceLeafOffsets.push_back(static_cast<std::uint16_t>(cursor));
            }
            if (f.type == semantic::PrimitiveType::String ||
                f.type == semantic::PrimitiveType::Enum ||
                (f.type == semantic::PrimitiveType::Struct &&
                 (f.typeId == quatTypeId() || f.typeId == quatfTypeId()))) {
                schema.containsPoolValues = true;
            }
            schema.prototypeLeaves.push_back(makeDefaultLeaf(f.type, f.typeId));
            cursor += 1u;
        }
        if (cursor == 0u) {
            schema.flatSlotCount = 1u;
            schema.prototypeLeaves.push_back(Value{});
        } else {
            schema.flatSlotCount = static_cast<std::uint16_t>(cursor);
        }
        const std::uint16_t sid = reg.registerOrReuseUnlocked(std::move(schema));
        state[tid] = (sid != 0u) ? 2 : 3;
        return sid;
    };

    for (const auto& [tid, fields] : structTypes) {
        (void)fields;
        if (tid == 0 || isBuiltinSingleCellMathStructId(tid)) continue;
        (void)resolve(resolve, tid);
    }
}

} // namespace detail

Value reconstructDetachedStructFromLeaves(
    const FlatStructSchema& schema,
    const Value* srcLeaves) {
    if (!srcLeaves || schema.directFields.empty()) {
        return Value{makeStructValueFromFields(schema.structTypeId, ValueSpan{})};
    }
    const std::size_t fCount = schema.directFields.size();
    std::array<Value, 16> stackFields{};
    std::vector<Value> heapFields;
    Value* dst = fCount <= stackFields.size()
        ? stackFields.data()
        : (heapFields.resize(fCount), heapFields.data());

    for (std::size_t i = 0; i < fCount; ++i) {
        const auto& fd = schema.directFields[i];
        if (fd.subSchemaId != 0u) {
            if (const auto* subSc = getFlatStructSchema(fd.subSchemaId)) {
                dst[i] = reconstructDetachedStructFromLeaves(*subSc, srcLeaves + fd.leafOffset);
                continue;
            }
        }
        dst[i] = srcLeaves[fd.leafOffset];
    }
    if (fCount <= stackFields.size()) {
        return Value{makeStructValueFromFields(schema.structTypeId, ValueSpan(stackFields.data(), fCount))};
    }
    return Value{makeStructValueFromFields(schema.structTypeId, std::move(heapFields))};
}

bool extractStructLeavesToBuffer(
    const Value& structVal,
    const FlatStructSchema& schema,
    Value* dstLeaves) noexcept {
    if (!dstLeaves) return false;
    const auto* sv = std::get_if<StructValue>(&structVal);
    if (!sv || !sv->valid() || sv->typeId != schema.structTypeId) {
        return false;
    }
    if (sv->isFrameSlice() && sv->frameSchemaId() == schema.schemaId) {
        if (const Value* slicePtr = detail::lookupFlatStackSlice(
                sv->storage.frameSliceBank(),
                sv->storage.frameSliceOffset(),
                schema.flatSlotCount)) {
            if (slicePtr != dstLeaves) {
                std::memmove(
                    static_cast<void*>(dstLeaves),
                    static_cast<const void*>(slicePtr),
                    static_cast<std::size_t>(schema.flatSlotCount) * sizeof(Value));
            }
            return true;
        }
        return false;
    }
    if (schema.directFields.empty()) {
        dstLeaves[0] = Value{};
        return true;
    }
    const std::size_t fCount = schema.directFields.size();
    if (sv->fieldCount() != fCount) return false;
    for (std::size_t i = 0; i < fCount; ++i) {
        const auto fv = structValueFieldAt(*sv, i);
        if (!fv) return false;
        const auto& fd = schema.directFields[i];
        if (fd.subSchemaId != 0u) {
            const auto* subSc = getFlatStructSchema(fd.subSchemaId);
            if (!subSc || !extractStructLeavesToBuffer(*fv, *subSc, dstLeaves + fd.leafOffset)) {
                return false;
            }
        } else {
            const auto leafPrim = valueType(*fv);
            if (fd.type == semantic::PrimitiveType::String) {
                if (leafPrim == semantic::PrimitiveType::Null) {
                    dstLeaves[fd.leafOffset] = NullString{};
                } else if (leafPrim == semantic::PrimitiveType::String) {
                    dstLeaves[fd.leafOffset] = *fv;
                } else {
                    return false;
                }
            } else if (fd.type == semantic::PrimitiveType::Object) {
                if (leafPrim == semantic::PrimitiveType::Null) {
                    dstLeaves[fd.leafOffset] = NullObject{};
                } else if (leafPrim == semantic::PrimitiveType::Object) {
                    dstLeaves[fd.leafOffset] = *fv;
                } else {
                    return false;
                }
            } else if (fd.type == semantic::PrimitiveType::Array) {
                if (leafPrim == semantic::PrimitiveType::Null) {
                    dstLeaves[fd.leafOffset] = NullArray{};
                } else if (leafPrim == semantic::PrimitiveType::Array) {
                    dstLeaves[fd.leafOffset] = *fv;
                } else {
                    return false;
                }
            } else if (fd.type == semantic::PrimitiveType::Enum) {
                const auto* ev = std::get_if<EnumValue>(&*fv);
                if (!ev || (fd.typeId != 0 && ev->typeId != fd.typeId)) return false;
                dstLeaves[fd.leafOffset] = *fv;
            } else if (fd.type == semantic::PrimitiveType::Handle) {
                NativeHandle nh{};
                if (!fv->tryGetNativeHandle(nh) || (fd.typeId != 0 && nh.typeId != fd.typeId)) return false;
                dstLeaves[fd.leafOffset] = *fv;
            } else if (fd.type == semantic::PrimitiveType::Struct) {
                if (leafPrim != semantic::PrimitiveType::Struct) return false;
                bool okStruct = false;
                if (fd.typeId == 0) {
                    okStruct = true;
                } else if (fd.typeId == fixedTypeId()) {
                    okStruct = std::holds_alternative<FixedValue>(*fv);
                } else if (fd.typeId == fixed16TypeId()) {
                    okStruct = std::holds_alternative<Fixed16Value>(*fv);
                } else if (fd.typeId == fixed64TypeId()) {
                    okStruct = std::holds_alternative<Fixed64Value>(*fv);
                } else if (fd.typeId == vec2TypeId()) {
                    okStruct = std::holds_alternative<Vec2Value>(*fv);
                } else if (fd.typeId == vec3TypeId()) {
                    okStruct = std::holds_alternative<Vec3Value>(*fv);
                } else if (fd.typeId == quatTypeId()) {
                    okStruct = std::holds_alternative<QuatValue>(*fv);
                } else if (fd.typeId == vec2fTypeId()) {
                    okStruct = std::holds_alternative<Vec2fValue>(*fv);
                } else if (fd.typeId == vec3fTypeId()) {
                    okStruct = std::holds_alternative<Vec3fValue>(*fv);
                } else if (fd.typeId == quatfTypeId()) {
                    okStruct = std::holds_alternative<QuatfValue>(*fv);
                } else if (const auto* subSv = std::get_if<StructValue>(&*fv)) {
                    okStruct = subSv->valid() && subSv->typeId == fd.typeId;
                }
                if (!okStruct) return false;
                dstLeaves[fd.leafOffset] = *fv;
            } else {
                if (leafPrim != fd.type) return false;
                dstLeaves[fd.leafOffset] = *fv;
            }
        }
    }
    return true;
}

Value detachFrameSliceValue(const Value& value) {
    if (value.rawTag() != ValueTag::Struct) return value;
    const auto& sv = value.asStructUnchecked();
    if (!sv.isFrameSlice()) return value;
    const auto* sc = getFlatStructSchema(sv.frameSchemaId());
    if (!sc) return Value{StructValue{}};
    const Value* leaves = detail::lookupFlatStackSlice(
        sv.storage.frameSliceBank(),
        sv.storage.frameSliceOffset(),
        sc->flatSlotCount);
    if (!leaves) return Value{StructValue{}};
    return reconstructDetachedStructFromLeaves(*sc, leaves);
}

bool operator==(const StructValue& left, const StructValue& right) {
    if (left.typeId != right.typeId) return false;
    const auto count = left.fieldCount();
    if (count != right.fieldCount()) return false;
    if (left.isFrameSlice() || right.isFrameSlice()) {
        if (left.isFrameSlice() && right.isFrameSlice() &&
            left.storage.raw_ == right.storage.raw_ &&
            left.frameSchemaId() == right.frameSchemaId()) {
            return true;
        }
        for (std::size_t i = 0; i < count; ++i) {
            const auto lf = structValueFieldAt(left, i);
            const auto rf = structValueFieldAt(right, i);
            if (!lf || !rf || !(*lf == *rf)) return false;
        }
        return true;
    }
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
    const std::uint32_t arenaIdx = detail::allocateInlineArenaEntry(entry);
    if (arenaIdx == 0u) return false;
    outVal.inlineSlotCount = static_cast<std::uint8_t>(slotCursor);
    outVal.storage.setInlineArenaIndex(arenaIdx);
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
    heapStructAllocCounter().fetch_add(1, std::memory_order_relaxed);
    std::vector<Value> detachedFields;
    detachedFields.reserve(fields.size());
    for (const auto& f : fields) {
        detachedFields.push_back(detachFrameSliceValue(f));
    }
    auto storage = std::make_shared<StructStorage>();
    storage->fields = std::move(detachedFields);
    return StructValue{typeId, std::move(storage)};
}

StructValue makeStructValueFromFields(
    semantic::SymbolId typeId,
    std::vector<Value> fields) {
    for (auto& f : fields) {
        if (f.rawTag() == ValueTag::Struct && f.asStructUnchecked().isFrameSlice()) {
            f = detachFrameSliceValue(f);
        }
    }
    StructValue inlineVal;
    if (tryPackInlineStructFields(typeId, ValueSpan(fields), inlineVal)) {
        return inlineVal;
    }
    heapStructAllocCounter().fetch_add(1, std::memory_order_relaxed);
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
    if (structure.isFrameSlice()) {
        const auto* sc = getFlatStructSchema(structure.frameSchemaId());
        if (!sc || index >= sc->directFields.size()) return std::nullopt;
        const Value* base = detail::lookupFlatStackSlice(
            structure.storage.frameSliceBank(),
            structure.storage.frameSliceOffset(),
            sc->flatSlotCount);
        if (!base) return std::nullopt;
        const auto& fd = sc->directFields[index];
        if (fd.subSchemaId != 0u) {
            return Value{StructValue::makeFrameSlice(
                fd.typeId,
                structure.storage.frameSliceBank(),
                structure.storage.frameSliceOffset() + fd.leafOffset,
                fd.subSchemaId)};
        }
        return base[fd.leafOffset];
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
    if (const auto* f64 = std::get_if<Fixed64Value>(&value)) {
        if (fieldIndex == 0) return Value{LongValue{f64->raw}};
        return std::nullopt;
    }
    if (const auto* v2f = std::get_if<Vec2fValue>(&value)) {
        if (fieldIndex == 0) return Value{FloatValue{v2f->x}};
        if (fieldIndex == 1) return Value{FloatValue{v2f->y}};
        return std::nullopt;
    }
    if (const auto* v3f = std::get_if<Vec3fValue>(&value)) {
        if (fieldIndex == 0) return Value{FloatValue{v3f->x}};
        if (fieldIndex == 1) return Value{FloatValue{v3f->y}};
        if (fieldIndex == 2) return Value{FloatValue{v3f->z}};
        return std::nullopt;
    }
    if (QuatfValue qf{}; value.tryGetQuatf(qf)) {
        if (fieldIndex == 0) return Value{FloatValue{qf.x}};
        if (fieldIndex == 1) return Value{FloatValue{qf.y}};
        if (fieldIndex == 2) return Value{FloatValue{qf.z}};
        if (fieldIndex == 3) return Value{FloatValue{qf.w}};
        return std::nullopt;
    }
    if (const auto* s = std::get_if<StructValue>(&value);
        s && (s->isInline || s->isFrameSlice())) {
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
    if (std::holds_alternative<Fixed64Value>(structure)) {
        if (fieldIndex != 0) return std::nullopt;
        if (const auto* l = std::get_if<LongValue>(&fieldValue)) {
            return Value{Fixed64Value{l->value}};
        }
        if (const auto* i = std::get_if<std::int64_t>(&fieldValue)) {
            return Value{Fixed64Value{*i}};
        }
        return std::nullopt;
    }
    const auto extractFloatField = [](const Value& fv) noexcept -> std::optional<float> {
        if (const auto* f = std::get_if<FloatValue>(&fv)) return f->value;
        if (const auto* d = std::get_if<double>(&fv)) return static_cast<float>(*d);
        return std::nullopt;
    };
    if (const auto* v2f = std::get_if<Vec2fValue>(&structure)) {
        const auto f = extractFloatField(fieldValue);
        if (!f) return std::nullopt;
        Vec2fValue out = *v2f;
        if (fieldIndex == 0) { out.x = *f; return Value{out}; }
        if (fieldIndex == 1) { out.y = *f; return Value{out}; }
        return std::nullopt;
    }
    if (const auto* v3f = std::get_if<Vec3fValue>(&structure)) {
        const auto f = extractFloatField(fieldValue);
        if (!f) return std::nullopt;
        Vec3fValue out = *v3f;
        if (fieldIndex == 0) { out.x = *f; return Value{out}; }
        if (fieldIndex == 1) { out.y = *f; return Value{out}; }
        if (fieldIndex == 2) { out.z = *f; return Value{out}; }
        return std::nullopt;
    }
    if (QuatfValue out{}; structure.tryGetQuatf(out)) {
        const auto f = extractFloatField(fieldValue);
        if (!f) return std::nullopt;
        if (fieldIndex == 0) { out.x = *f; return Value{out}; }
        if (fieldIndex == 1) { out.y = *f; return Value{out}; }
        if (fieldIndex == 2) { out.z = *f; return Value{out}; }
        if (fieldIndex == 3) { out.w = *f; return Value{out}; }
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
        } else if (const auto* fx64 = std::get_if<Fixed64Value>(&v)) {
            vw ^= static_cast<std::uint64_t>(fx64->raw);
        } else if (const auto* v2f = std::get_if<Vec2fValue>(&v)) {
            const auto normBits = [](float f) noexcept {
                const float n = (f == 0.0f) ? 0.0f : f;
                std::uint32_t b = 0;
                std::memcpy(&b, &n, sizeof(b));
                return b;
            };
            vw ^= normBits(v2f->x) | (static_cast<std::uint64_t>(normBits(v2f->y)) << 32u);
        } else if (const auto* v3f = std::get_if<Vec3fValue>(&v)) {
            const auto normBits = [](float f) noexcept {
                const float n = (f == 0.0f) ? 0.0f : f;
                std::uint32_t b = 0;
                std::memcpy(&b, &n, sizeof(b));
                return b;
            };
            vw ^= normBits(v3f->x) ^
                (static_cast<std::uint64_t>(normBits(v3f->y)) << 21u) ^
                (static_cast<std::uint64_t>(normBits(v3f->z)) << 42u);
        } else if (QuatfValue qf{}; v.tryGetQuatf(qf)) {
            const auto normBits = [](float f) noexcept {
                const float n = (f == 0.0f) ? 0.0f : f;
                std::uint32_t b = 0;
                std::memcpy(&b, &n, sizeof(b));
                return b;
            };
            vw ^= normBits(qf.x) ^
                (static_cast<std::uint64_t>(normBits(qf.y)) << 16u) ^
                (static_cast<std::uint64_t>(normBits(qf.z)) << 32u) ^
                (static_cast<std::uint64_t>(normBits(qf.w)) << 48u);
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
        if (const auto* f64a = std::get_if<Fixed64Value>(&a)) return f64a->raw < std::get<Fixed64Value>(b).raw;
        if (const auto* da = std::get_if<double>(&a)) return *da < std::get<double>(b);
        if (const auto* fla = std::get_if<FloatValue>(&a)) return fla->value < std::get<FloatValue>(b).value;
    }
    return valueToString(a, heap) < valueToString(b, heap);
}

} // namespace detail


void utf8ToUtf16(std::string_view utf8, std::u16string& outUtf16) {
    outUtf16.clear();
    outUtf16.reserve(utf8.size());
    std::size_t i = 0;
    while (i < utf8.size()) {
        const auto b0 = static_cast<unsigned char>(utf8[i]);
        std::uint32_t cp = 0xFFFDu;
        if (b0 < 0x80u) {
            cp = b0;
            ++i;
        } else if ((b0 & 0xE0u) == 0xC0u && i + 1 < utf8.size()) {
            const auto b1 = static_cast<unsigned char>(utf8[i + 1]);
            if ((b1 & 0xC0u) == 0x80u) {
                cp = ((b0 & 0x1Fu) << 6) | (b1 & 0x3Fu);
                if (cp < 0x80u) {
                    cp = 0xFFFDu;
                }
                i += 2;
            } else {
                ++i;
            }
        } else if ((b0 & 0xF0u) == 0xE0u && i + 2 < utf8.size()) {
            const auto b1 = static_cast<unsigned char>(utf8[i + 1]);
            const auto b2 = static_cast<unsigned char>(utf8[i + 2]);
            if ((b1 & 0xC0u) == 0x80u && (b2 & 0xC0u) == 0x80u) {
                cp = ((b0 & 0x0Fu) << 12) | ((b1 & 0x3Fu) << 6) | (b2 & 0x3Fu);
                if (cp < 0x800u || (cp >= 0xD800u && cp <= 0xDFFFu)) {
                    cp = 0xFFFDu;
                }
                i += 3;
            } else {
                ++i;
            }
        } else if ((b0 & 0xF8u) == 0xF0u && i + 3 < utf8.size()) {
            const auto b1 = static_cast<unsigned char>(utf8[i + 1]);
            const auto b2 = static_cast<unsigned char>(utf8[i + 2]);
            const auto b3 = static_cast<unsigned char>(utf8[i + 3]);
            if ((b1 & 0xC0u) == 0x80u && (b2 & 0xC0u) == 0x80u && (b3 & 0xC0u) == 0x80u) {
                cp = ((b0 & 0x07u) << 18) | ((b1 & 0x3Fu) << 12) |
                     ((b2 & 0x3Fu) << 6) | (b3 & 0x3Fu);
                if (cp < 0x10000u || cp > 0x10FFFFu) {
                    cp = 0xFFFDu;
                }
                i += 4;
            } else {
                ++i;
            }
        } else {
            ++i;
        }
        if (cp <= 0xFFFFu) {
            if (cp >= 0xD800u && cp <= 0xDFFFu) {
                cp = 0xFFFDu;
            }
            outUtf16.push_back(static_cast<char16_t>(cp));
        } else if (cp <= 0x10FFFFu) {
            cp -= 0x10000u;
            outUtf16.push_back(static_cast<char16_t>(0xD800u + (cp >> 10)));
            outUtf16.push_back(static_cast<char16_t>(0xDC00u + (cp & 0x3FFu)));
        } else {
            outUtf16.push_back(static_cast<char16_t>(0xFFFDu));
        }
    }
}

void utf16ToUtf8(std::u16string_view utf16, std::string& outUtf8) {
    outUtf8.clear();
    outUtf8.reserve(utf16.size() * 2);
    std::size_t i = 0;
    while (i < utf16.size()) {
        std::uint32_t cp = static_cast<std::uint16_t>(utf16[i++]);
        if (cp >= 0xD800u && cp <= 0xDBFFu && i < utf16.size()) {
            const std::uint32_t low = static_cast<std::uint16_t>(utf16[i]);
            if (low >= 0xDC00u && low <= 0xDFFFu) {
                cp = 0x10000u + (((cp - 0xD800u) << 10) | (low - 0xDC00u));
                ++i;
            } else {
                cp = 0xFFFDu;
            }
        } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
            cp = 0xFFFDu;
        }
        if (cp < 0x80u) {
            outUtf8.push_back(static_cast<char>(cp));
        } else if (cp < 0x800u) {
            outUtf8.push_back(static_cast<char>(0xC0u | (cp >> 6)));
            outUtf8.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else if (cp < 0x10000u) {
            outUtf8.push_back(static_cast<char>(0xE0u | (cp >> 12)));
            outUtf8.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            outUtf8.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else {
            outUtf8.push_back(static_cast<char>(0xF0u | (cp >> 18)));
            outUtf8.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
            outUtf8.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            outUtf8.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        }
    }
}

} // namespace realscript::runtime
