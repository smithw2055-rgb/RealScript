#pragma once

#include "realscript/runtime/Runtime.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace realscript::runtime::detail {

[[nodiscard]] inline std::uint64_t fnv1a64(std::string_view text) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char ch : text) {
        hash ^= static_cast<std::uint64_t>(ch);
        hash *= 1099511628211ULL;
    }
    return hash;
}

// Shared Value / Struct / String operations (implemented in ValueOperations.cpp)
[[nodiscard]] std::uint64_t computeStructFieldsHash(
    const std::vector<Value>& fields) noexcept;
[[nodiscard]] bool isStringLikeValue(const Value& val) noexcept;
[[nodiscard]] std::string extractIntrinsicString(
    const Value& val,
    const ManagedHeap* heap);
[[nodiscard]] bool intrinsicValuesEqual(
    const Value& a,
    const Value& b,
    const ManagedHeap* heap);
[[nodiscard]] bool intrinsicValueLess(
    const Value& a,
    const Value& b,
    const ManagedHeap* heap);

// Shared sharded value-pool service accessors (implemented in RuntimeServices.cpp)
[[nodiscard]] const std::string& emptyRuntimePoolString() noexcept;
[[nodiscard]] StringValue internRuntimeString(
    std::string_view text,
    bool pinEntry = false);
[[nodiscard]] StringValue lookupRuntimeStringById(std::uint32_t id) noexcept;

[[nodiscard]] std::uint32_t allocateInlineArenaEntry(
    const InlineStructArenaEntry& entry) noexcept;
[[nodiscard]] const InlineStructArenaEntry& lookupInlineArenaEntry(
    std::uint32_t idx) noexcept;

[[nodiscard]] std::uint32_t registerHeapStructStorage(
    std::shared_ptr<const StructStorage> store,
    bool pinEntry = false);
[[nodiscard]] const StructStorage* lookupHeapStructStorage(
    std::uint32_t id) noexcept;

void registerFlatStructTypes(
    const std::vector<std::pair<semantic::SymbolId, std::vector<BlittableFieldInput>>>& structTypes);
[[nodiscard]] std::uint8_t acquireFlatStackBank(const std::vector<Value>* stackVec) noexcept;
void releaseFlatStackBank(std::uint8_t bankId) noexcept;
[[nodiscard]] const Value* lookupFlatStackSlice(
    std::uint8_t bankId,
    std::uint32_t absOffset,
    std::uint16_t slotCount) noexcept;

[[nodiscard]] const SharedValuePoolBridge* externalValuePoolBridge() noexcept;
[[nodiscard]] void* localFlatStructRegistryPtr() noexcept;
[[nodiscard]] void* localBlittableRegistryPtr() noexcept;
[[nodiscard]] void* localHeapPoolMarkerShardsPtr() noexcept;
[[nodiscard]] void* localLiveStringHeapShardsPtr() noexcept;
[[nodiscard]] void* localHeapStructAllocCounterPtr() noexcept;

// 30-bit pool ID layout:
//   bits 29..24 (6 bits): domainId (0 = defaultDomain, 1..63 = dynamic/recycled)
//   bits 23..20 (4 bits): shardIdx (0..15)
//   bits 19..0 (20 bits): localIdx (1..1,048,575; 0 reserved invalid)
inline constexpr std::uint32_t kPoolDomainBits = 6u;
inline constexpr std::uint32_t kMaxPoolDomains = 1u << kPoolDomainBits; // 64
inline constexpr std::uint32_t kPoolDomainMask = kMaxPoolDomains - 1u;
inline constexpr std::uint32_t kPoolShardBits = 4u;
inline constexpr std::uint32_t kPoolShardCount = 1u << kPoolShardBits; // 16
inline constexpr std::uint32_t kPoolShardMask = kPoolShardCount - 1u;
inline constexpr std::uint32_t kPoolLocalBits = 20u;
inline constexpr std::uint32_t kPoolLocalMask = (1u << kPoolLocalBits) - 1u;
inline constexpr std::uint32_t kPoolShardShift = kPoolLocalBits;                  // 20
inline constexpr std::uint32_t kPoolDomainShift = kPoolLocalBits + kPoolShardBits; // 24

[[nodiscard]] constexpr std::uint32_t encodeDomainPoolId(
    std::uint8_t domainId,
    std::uint32_t shardIdx,
    std::uint32_t localSlot) noexcept {
    return (static_cast<std::uint32_t>(domainId & kPoolDomainMask) << kPoolDomainShift) |
        ((shardIdx & kPoolShardMask) << kPoolShardShift) |
        ((localSlot + 1u) & kPoolLocalMask);
}

[[nodiscard]] constexpr bool decodeDomainPoolId(
    std::uint32_t id,
    std::uint8_t& outDomainId,
    std::uint32_t& outShardIdx,
    std::uint32_t& outLocalSlot) noexcept {
    if ((id & 0xC0000000u) != 0u) return false;
    const std::uint32_t rawLocal = id & kPoolLocalMask;
    if (rawLocal == 0u) return false;
    outDomainId = static_cast<std::uint8_t>((id >> kPoolDomainShift) & kPoolDomainMask);
    outShardIdx = (id >> kPoolShardShift) & kPoolShardMask;
    outLocalSlot = rawLocal - 1u;
    return true;
}

struct DomainGcRuntimeState {
    virtual ~DomainGcRuntimeState() = default;
};

struct ValuePoolDomainAccess {
    [[nodiscard]] static ValuePoolDomain::Impl* impl(ValuePoolDomain* domain) noexcept;
    [[nodiscard]] static const ValuePoolDomain::Impl* impl(const ValuePoolDomain* domain) noexcept;
    [[nodiscard]] static ValuePoolDomain::Impl* currentDomainImpl() noexcept;
    [[nodiscard]] static std::shared_ptr<ValuePoolDomain> tryLockDomainById(
        std::uint8_t domainId) noexcept;
    [[nodiscard]] static std::uint8_t domainIdOf(
        const ValuePoolDomain::Impl* domainImpl) noexcept;
    [[nodiscard]] static std::uint64_t domainEpochOf(
        const ValuePoolDomain::Impl* domainImpl) noexcept;

    using GcStateFactoryFn = std::shared_ptr<DomainGcRuntimeState> (*)(std::uint8_t domainId);
    [[nodiscard]] static std::shared_ptr<DomainGcRuntimeState> getOrCreateGcState(
        ValuePoolDomain::Impl* domainImpl,
        GcStateFactoryFn factory);
    [[nodiscard]] static std::shared_ptr<DomainGcRuntimeState> getGcStateByDomainId(
        std::uint8_t domainId) noexcept;

    [[nodiscard]] static bool poolGcMarkInProgress(
        const ValuePoolDomain::Impl* domainImpl) noexcept;
    [[nodiscard]] static bool anyPoolHasPinnedSlots(
        const ValuePoolDomain::Impl* domainImpl) noexcept;
    static void beginPoolGcMarkForImpl(ValuePoolDomain::Impl* domainImpl) noexcept;
    static void markValueInDomain(
        ValuePoolDomain::Impl* domainImpl,
        const Value& value) noexcept;
    static void markStackRootValueInDomain(
        ValuePoolDomain::Impl* domainImpl,
        const Value& value) noexcept;
    static std::size_t finishPoolGcSweepForImpl(
        ValuePoolDomain::Impl* domainImpl) noexcept;
    static void incrementCollectionCount(
        ValuePoolDomain::Impl* domainImpl) noexcept;
};

[[nodiscard]] std::uint8_t currentThreadDomainIdFast() noexcept;
std::size_t reclaimDomainPoolValues(
    ValuePoolDomain::Impl* domainImpl,
    const ShadowStack* activeShadowStack,
    bool waitIfBusy) noexcept;
std::size_t reclaimDomainPoolValuesById(
    std::uint8_t domainId,
    const ShadowStack* activeShadowStack,
    bool waitIfBusy) noexcept;

std::size_t hostReclaimPoolValuesBridge(const void* activeShadowStack, bool waitIfBusy) noexcept;
void hostRegisterShadowStackBridge(void* stack) noexcept;
void hostUnregisterShadowStackBridge(void* stack) noexcept;
void hostTransferShadowStackBridge(void* from, void* to) noexcept;
void hostEnterHeapSyncLockBridge() noexcept;
void hostLeaveHeapSyncLockBridge(bool flushDeferred) noexcept;

} // namespace realscript::runtime::detail
