#include "RuntimeInternal.h"

#include "realscript/runtime/AsyncRuntime.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace realscript::runtime {

namespace {

constexpr std::uint32_t kPoolShardBits = 6u;
constexpr std::uint32_t kPoolShardCount = 1u << kPoolShardBits; // 64 shards
constexpr std::uint32_t kPoolShardMask = kPoolShardCount - 1u;
constexpr std::uint32_t kPoolLocalBits = 20u;
constexpr std::uint32_t kPoolLocalMask = (1u << kPoolLocalBits) - 1u;
constexpr std::uint32_t kInvalidLocalSlot = 0xFFFFFFFFu;

constexpr std::uint8_t kSlotFlagOccupied  = 0x01u;
constexpr std::uint8_t kSlotFlagClock     = 0x02u;
constexpr std::uint8_t kSlotFlagPinned    = 0x04u;
constexpr std::uint8_t kSlotFlagGcMarked  = 0x08u;
constexpr std::uint8_t kSlotFlagStackRoot = 0x10u;

std::atomic<bool> g_poolGcMarkInProgress{false};
std::atomic<bool> g_anyPoolHasPinnedSlots{false};
std::atomic<const detail::SharedValuePoolBridge*> g_externalPoolBridge{nullptr};

inline void noteShardPinned(std::atomic<bool>& shardFlag) noexcept {
    if (!shardFlag.load(std::memory_order_relaxed)) {
        shardFlag.store(true, std::memory_order_release);
    }
    if (!g_anyPoolHasPinnedSlots.load(std::memory_order_relaxed)) {
        g_anyPoolHasPinnedSlots.store(true, std::memory_order_release);
    }
}

inline void noteShardMarked(std::atomic<bool>& shardFlag) noexcept {
    if (!shardFlag.load(std::memory_order_relaxed)) {
        shardFlag.store(true, std::memory_order_release);
    }
}

[[nodiscard]] inline std::uint8_t activePinFlags() noexcept {
    return kSlotFlagPinned | kSlotFlagClock |
        (g_poolGcMarkInProgress.load(std::memory_order_acquire) ? kSlotFlagGcMarked : 0u);
}

[[nodiscard]] constexpr std::uint32_t encodeShardedPoolId(
    std::uint32_t shardIdx,
    std::uint32_t localSlot) noexcept {
    return ((shardIdx & kPoolShardMask) << kPoolLocalBits) |
           ((localSlot + 1u) & kPoolLocalMask);
}

[[nodiscard]] constexpr bool decodeShardedPoolId(
    std::uint32_t id,
    std::uint32_t& outShardIdx,
    std::uint32_t& outLocalSlot) noexcept {
    const std::uint32_t rawLocal = id & kPoolLocalMask;
    if (id == 0u || (id & 0x80000000u) != 0u || rawLocal == 0u) return false;
    outShardIdx = (id >> kPoolLocalBits) & kPoolShardMask;
    outLocalSlot = rawLocal - 1u;
    return true;
}

inline void refreshClockBitIfUnset(std::atomic<std::uint8_t>& flagByte) noexcept {
    const std::uint8_t cur = flagByte.load(std::memory_order_relaxed);
    if ((cur & kSlotFlagClock) == 0u) {
        flagByte.fetch_or(kSlotFlagClock, std::memory_order_relaxed);
    }
}

// ----------------------------------------------------------------------------
// 1. ShardedRuntimeStringPool
// ----------------------------------------------------------------------------
struct RuntimeStringPool {
    static constexpr std::uint32_t kPageShift = 8u;
    static constexpr std::uint32_t kPageSize = 1u << kPageShift; // 256 strings/page
    static constexpr std::uint32_t kPageMask = kPageSize - 1u;
    static constexpr std::uint32_t kSoftMaxPagesPerShard = 8u;   // 2,048 ephemeral strings/shard (131,072 total)
    static constexpr std::uint32_t kHardMaxPagesPerShard = 64u;  // 16,384 max if all pinned
    static constexpr std::uint32_t kSoftMaxSlotsPerShard = kSoftMaxPagesPerShard * kPageSize;
    static constexpr std::uint32_t kHardMaxSlotsPerShard = kHardMaxPagesPerShard * kPageSize;
    static constexpr std::uint32_t kRecentGuardWindow = 64u;

    struct StringPage {
        std::string strings[kPageSize][2];
        std::atomic<std::uint8_t> activeBank[kPageSize]{};
        std::atomic<std::uint8_t> flags[kPageSize]{};
    };

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
    struct alignas(64) Shard {
        std::mutex mutex;
        std::atomic<StringPage*> pages[kHardMaxPagesPerShard]{};
        std::vector<std::unique_ptr<StringPage>> ownedPages;
        std::unordered_map<std::string_view, std::uint32_t> indexByView;
        std::vector<std::uint32_t> freeList;
        std::uint32_t highWaterMark = 0u;
        std::uint32_t clockHand = 0u;
        std::uint32_t lastAllocatedSlot = kInvalidLocalSlot;
        std::atomic<std::uint32_t> epoch{1u};
        std::atomic<bool> mayHavePinnedOrMarked{false};
        std::atomic<bool> needsPinReclaim{false};
        std::atomic<std::uint64_t> clockEvictions{0u};
        std::atomic<std::uint64_t> gcReclaimed{0u};
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

    std::string emptyString_{};
    Shard shards_[kPoolShardCount]{};

    RuntimeStringPool() = default;

    [[nodiscard]] const std::string& empty() const noexcept {
        return emptyString_;
    }

    [[nodiscard]] std::uint32_t tryClockEvictLocked(Shard& shard, bool enforceGuardWindow) {
        const std::uint32_t limit = shard.highWaterMark;
        if (limit == 0u) return kInvalidLocalSlot;
        const std::uint32_t maxScans = limit * 2u;
        for (std::uint32_t scan = 0u; scan < maxScans; ++scan) {
            const std::uint32_t candidate = shard.clockHand;
            shard.clockHand = (shard.clockHand + 1u) % limit;
            if (enforceGuardWindow && shard.lastAllocatedSlot != kInvalidLocalSlot) {
                const std::uint32_t dist = (shard.lastAllocatedSlot >= candidate)
                    ? (shard.lastAllocatedSlot - candidate)
                    : (limit - (candidate - shard.lastAllocatedSlot));
                if (dist < kRecentGuardWindow) continue;
            }

            StringPage* page = shard.pages[candidate >> kPageShift].load(std::memory_order_relaxed);
            if (!page) continue;
            const std::uint32_t offset = candidate & kPageMask;
            auto& fl = page->flags[offset];
            const std::uint8_t f = fl.load(std::memory_order_relaxed);
            if ((f & (kSlotFlagPinned | kSlotFlagGcMarked | kSlotFlagStackRoot)) != 0u) {
                continue;
            }
            if ((f & kSlotFlagClock) != 0u) {
                fl.fetch_and(static_cast<std::uint8_t>(~kSlotFlagClock), std::memory_order_relaxed);
                continue;
            }
            const std::uint8_t curBank = page->activeBank[offset].load(std::memory_order_relaxed) & 1u;
            const std::string& victimStr = page->strings[offset][curBank];
            if (!victimStr.empty()) {
                shard.indexByView.erase(std::string_view(victimStr));
            }
            shard.epoch.fetch_add(1u, std::memory_order_release);
            shard.clockEvictions.fetch_add(1u, std::memory_order_relaxed);
            return candidate;
        }
        return kInvalidLocalSlot;
    }

    [[nodiscard]] std::uint32_t acquireSlotLocked(Shard& shard) {
        if (!shard.freeList.empty()) {
            const std::uint32_t slot = shard.freeList.back();
            shard.freeList.pop_back();
            return slot;
        }
        if (shard.highWaterMark < kSoftMaxSlotsPerShard) {
            return allocateFreshSlotLocked(shard);
        }
        const std::uint32_t evicted = tryClockEvictLocked(shard, true);
        if (evicted != kInvalidLocalSlot) {
            return evicted;
        }
        shard.needsPinReclaim.store(true, std::memory_order_relaxed);
        if (shard.highWaterMark < kHardMaxSlotsPerShard) {
            return allocateFreshSlotLocked(shard);
        }
        return tryClockEvictLocked(shard, false);
    }

    [[nodiscard]] std::uint32_t allocateFreshSlotLocked(Shard& shard) {
        const std::uint32_t slot = shard.highWaterMark++;
        const std::uint32_t pageIdx = slot >> kPageShift;
        if (!shard.pages[pageIdx].load(std::memory_order_relaxed)) {
            auto newPage = std::make_unique<StringPage>();
            StringPage* rawPage = newPage.get();
            shard.ownedPages.push_back(std::move(newPage));
            shard.pages[pageIdx].store(rawPage, std::memory_order_release);
        }
        return slot;
    }

    StringValue intern(std::string_view text, bool pinEntry = false) {
        if (text.empty()) {
            StringValue sv;
            sv.id = 0u;
            sv.ptr = &emptyString_;
            return sv;
        }

        const std::uint64_t h = detail::fnv1a64(text);
        const std::uint32_t shardIdx = static_cast<std::uint32_t>((h >> 58u) & kPoolShardMask);
        Shard& shard = shards_[shardIdx];
        if (shard.needsPinReclaim.exchange(false, std::memory_order_relaxed)) {
            (void)reclaimUnreferencedPoolValues();
        }

        struct TlsStringSlot {
            std::uint64_t hash = 0;
            std::uint32_t id = 0;
            std::uint32_t length = 0;
            const std::string* ptr = nullptr;
            std::uint32_t epoch = 0;
        };
        thread_local std::array<TlsStringSlot, 512> tlsCache{};
        auto& tls = tlsCache[h & 511u];
        const std::uint32_t curEpoch = shard.epoch.load(std::memory_order_acquire);
        if (tls.id != 0u && tls.epoch == curEpoch && tls.hash == h &&
            tls.length == text.size() && tls.ptr && *tls.ptr == text) {
            std::uint32_t sIdx = 0u, lSlot = 0u;
            if (decodeShardedPoolId(tls.id, sIdx, lSlot)) {
                if (StringPage* p = shard.pages[lSlot >> kPageShift].load(std::memory_order_relaxed)) {
                    auto& fl = p->flags[lSlot & kPageMask];
                    refreshClockBitIfUnset(fl);
                    if (pinEntry) {
                        fl.fetch_or(activePinFlags(), std::memory_order_relaxed);
                        noteShardPinned(shard.mayHavePinnedOrMarked);
                    }
                }
            }
            StringValue sv;
            sv.id = tls.id;
            sv.ptr = tls.ptr;
            return sv;
        }

        std::lock_guard<std::mutex> lock(shard.mutex);
        const auto found = shard.indexByView.find(text);
        if (found != shard.indexByView.end()) {
            const std::uint32_t localSlot = found->second;
            StringPage* page = shard.pages[localSlot >> kPageShift].load(std::memory_order_relaxed);
            const std::uint32_t offset = localSlot & kPageMask;
            const std::uint8_t bank = page->activeBank[offset].load(std::memory_order_relaxed) & 1u;
            const std::string& stored = page->strings[offset][bank];
            const std::uint8_t addFlags = kSlotFlagOccupied | kSlotFlagClock | (pinEntry ? activePinFlags() : 0u);
            page->flags[offset].fetch_or(addFlags, std::memory_order_relaxed);
            if (pinEntry) {
                noteShardPinned(shard.mayHavePinnedOrMarked);
            }

            const std::uint32_t id = encodeShardedPoolId(shardIdx, localSlot);
            tls.hash = h;
            tls.id = id;
            tls.length = static_cast<std::uint32_t>(stored.size());
            tls.ptr = &stored;
            tls.epoch = shard.epoch.load(std::memory_order_relaxed);

            StringValue sv;
            sv.id = id;
            sv.ptr = &stored;
            return sv;
        }

        const std::uint32_t localSlot = acquireSlotLocked(shard);
        if (localSlot == kInvalidLocalSlot) {
            StringValue sv;
            sv.id = 0u;
            sv.ptr = &emptyString_;
            return sv;
        }
        shard.lastAllocatedSlot = localSlot;
        StringPage* page = shard.pages[localSlot >> kPageShift].load(std::memory_order_relaxed);
        const std::uint32_t offset = localSlot & kPageMask;
        const std::uint8_t nextBank = (page->activeBank[offset].load(std::memory_order_relaxed) ^ 1u) & 1u;
        std::string& stored = page->strings[offset][nextBank];
        stored.assign(text.data(), text.size()); // Reuses string capacity on inactive bank!
        page->activeBank[offset].store(nextBank, std::memory_order_release);
        const std::uint8_t initFlags = kSlotFlagOccupied | kSlotFlagClock | (pinEntry ? activePinFlags() : 0u);
        page->flags[offset].store(initFlags, std::memory_order_release);
        if (pinEntry) {
            noteShardPinned(shard.mayHavePinnedOrMarked);
        }
        shard.indexByView.emplace(std::string_view(stored), localSlot);

        const std::uint32_t id = encodeShardedPoolId(shardIdx, localSlot);
        tls.hash = h;
        tls.id = id;
        tls.length = static_cast<std::uint32_t>(stored.size());
        tls.ptr = &stored;
        tls.epoch = shard.epoch.load(std::memory_order_relaxed);

        StringValue sv;
        sv.id = id;
        sv.ptr = &stored;
        return sv;
    }

    // 100% Lock-Free Read Path
    [[nodiscard]] StringValue getById(std::uint32_t id) noexcept {
        std::uint32_t shardIdx = 0u;
        std::uint32_t localSlot = 0u;
        if (!decodeShardedPoolId(id, shardIdx, localSlot)) {
            StringValue sv;
            sv.id = 0u;
            sv.ptr = &emptyString_;
            return sv;
        }
        const std::uint32_t pageIdx = localSlot >> kPageShift;
        if (pageIdx < kHardMaxPagesPerShard) {
            Shard& shard = shards_[shardIdx];
            if (StringPage* page = shard.pages[pageIdx].load(std::memory_order_acquire)) {
                const std::uint32_t offset = localSlot & kPageMask;
                refreshClockBitIfUnset(page->flags[offset]);
                const std::uint8_t bank = page->activeBank[offset].load(std::memory_order_acquire) & 1u;
                const std::string& stored = page->strings[offset][bank];
                StringValue sv;
                sv.id = id;
                sv.ptr = &stored;
                return sv;
            }
        }
        StringValue sv;
        sv.id = 0u;
        sv.ptr = &emptyString_;
        return sv;
    }

    void pinById(std::uint32_t id) noexcept {
        std::uint32_t shardIdx = 0u, localSlot = 0u;
        if (!decodeShardedPoolId(id, shardIdx, localSlot)) return;
        const std::uint32_t pageIdx = localSlot >> kPageShift;
        if (pageIdx < kHardMaxPagesPerShard) {
            Shard& shard = shards_[shardIdx];
            if (StringPage* page = shard.pages[pageIdx].load(std::memory_order_acquire)) {
                page->flags[localSlot & kPageMask].fetch_or(
                    activePinFlags(), std::memory_order_relaxed);
                noteShardPinned(shard.mayHavePinnedOrMarked);
            }
        }
    }

    void markById(std::uint32_t id) noexcept {
        std::uint32_t shardIdx = 0u, localSlot = 0u;
        if (!decodeShardedPoolId(id, shardIdx, localSlot)) return;
        const std::uint32_t pageIdx = localSlot >> kPageShift;
        if (pageIdx < kHardMaxPagesPerShard) {
            Shard& shard = shards_[shardIdx];
            if (StringPage* page = shard.pages[pageIdx].load(std::memory_order_acquire)) {
                page->flags[localSlot & kPageMask].fetch_or(
                    kSlotFlagGcMarked | kSlotFlagPinned, std::memory_order_relaxed);
                noteShardPinned(shard.mayHavePinnedOrMarked);
            }
        }
    }

    void markStackRootById(std::uint32_t id) noexcept {
        std::uint32_t shardIdx = 0u, localSlot = 0u;
        if (!decodeShardedPoolId(id, shardIdx, localSlot)) return;
        const std::uint32_t pageIdx = localSlot >> kPageShift;
        if (pageIdx < kHardMaxPagesPerShard) {
            Shard& shard = shards_[shardIdx];
            if (StringPage* page = shard.pages[pageIdx].load(std::memory_order_acquire)) {
                page->flags[localSlot & kPageMask].fetch_or(
                    kSlotFlagStackRoot | kSlotFlagClock, std::memory_order_relaxed);
                noteShardMarked(shard.mayHavePinnedOrMarked);
            }
        }
    }
};

RuntimeStringPool& runtimeStringPool() {
    static RuntimeStringPool pool;
    return pool;
}


// ----------------------------------------------------------------------------
// 2. ShardedInlineStructPool
// ----------------------------------------------------------------------------
struct InlineStructEntryEq {
    bool operator()(const InlineStructArenaEntry& a, const InlineStructArenaEntry& b) const noexcept {
        return a.nestedTypeId == b.nestedTypeId &&
            a.nestedFieldMask == b.nestedFieldMask &&
            a.slots == b.slots;
    }
};

struct InlineStructEntryHash {
    std::size_t operator()(const InlineStructArenaEntry& e) const noexcept {
        std::uint64_t h = e.nestedTypeId ^ (static_cast<std::uint64_t>(e.nestedFieldMask) * 0x9e3779b97f4a7c15ULL);
        for (const auto& s : e.slots) {
            const std::uint64_t w = s.bits() ^
                (static_cast<std::uint64_t>(s.aux0) << 40u) ^
                (static_cast<std::uint64_t>(s.aux1) << 48u) ^
                (static_cast<std::uint64_t>(s.aux2) << 56u) ^
                static_cast<std::uint64_t>(s.kind);
            h ^= w + 0x9e3779b97f4a7c15ULL + (h << 6u) + (h >> 2u);
        }
        return static_cast<std::size_t>(h);
    }
};

const InlineStructArenaEntry g_zeroInlineEntry{};

struct InlineStructPool {
    static constexpr std::uint32_t kPageShift = 9u;
    static constexpr std::uint32_t kPageSize = 1u << kPageShift; // 512 entries/page
    static constexpr std::uint32_t kPageMask = kPageSize - 1u;
    static constexpr std::uint32_t kSoftMaxPagesPerShard = 4u;   // 2,048 slots/shard (131,072 total)
    static constexpr std::uint32_t kHardMaxPagesPerShard = 64u;
    static constexpr std::uint32_t kSoftMaxSlotsPerShard = kSoftMaxPagesPerShard * kPageSize;
    static constexpr std::uint32_t kHardMaxSlotsPerShard = kHardMaxPagesPerShard * kPageSize;
    static constexpr std::uint32_t kRecentGuardWindow = 64u;

    struct InlinePage {
        InlineStructArenaEntry entries[kPageSize][2]{};
        std::atomic<std::uint8_t> activeBank[kPageSize]{};
        std::atomic<std::uint8_t> flags[kPageSize]{};
    };

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
    struct alignas(64) Shard {
        std::mutex mutex;
        std::atomic<InlinePage*> pages[kHardMaxPagesPerShard]{};
        std::vector<std::unique_ptr<InlinePage>> ownedPages;
        std::unordered_map<
            InlineStructArenaEntry,
            std::uint32_t,
            InlineStructEntryHash,
            InlineStructEntryEq> dedupMap;
        std::vector<std::uint32_t> freeList;
        std::uint32_t highWaterMark = 0u;
        std::uint32_t clockHand = 0u;
        std::uint32_t lastAllocatedSlot = kInvalidLocalSlot;
        std::atomic<std::uint32_t> epoch{1u};
        std::atomic<bool> mayHavePinnedOrMarked{false};
        std::atomic<bool> needsPinReclaim{false};
        std::atomic<std::uint64_t> clockEvictions{0u};
        std::atomic<std::uint64_t> gcReclaimed{0u};
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

    Shard shards_[kPoolShardCount]{};

    static void pinChildSlots(const InlineStructArenaEntry& entry) noexcept {
        for (const auto& s : entry.slots) {
            if (s.kind == InlineStructSlot::Kind::String && s.lo != 0u) {
                runtimeStringPool().pinById(s.lo);
            }
        }
    }

    [[nodiscard]] std::uint32_t allocateFreshSlotLocked(Shard& shard) {
        const std::uint32_t slot = shard.highWaterMark++;
        const std::uint32_t pageIdx = slot >> kPageShift;
        if (!shard.pages[pageIdx].load(std::memory_order_relaxed)) {
            auto newPage = std::make_unique<InlinePage>();
            InlinePage* rawPage = newPage.get();
            shard.ownedPages.push_back(std::move(newPage));
            shard.pages[pageIdx].store(rawPage, std::memory_order_release);
        }
        return slot;
    }

    [[nodiscard]] std::uint32_t tryClockEvictLocked(Shard& shard, bool enforceGuardWindow) {
        const std::uint32_t limit = shard.highWaterMark;
        if (limit == 0u) return kInvalidLocalSlot;
        const std::uint32_t maxScans = limit * 2u;
        for (std::uint32_t scan = 0u; scan < maxScans; ++scan) {
            const std::uint32_t candidate = shard.clockHand;
            shard.clockHand = (shard.clockHand + 1u) % limit;
            if (enforceGuardWindow && shard.lastAllocatedSlot != kInvalidLocalSlot) {
                const std::uint32_t dist = (shard.lastAllocatedSlot >= candidate)
                    ? (shard.lastAllocatedSlot - candidate)
                    : (limit - (candidate - shard.lastAllocatedSlot));
                if (dist < kRecentGuardWindow) continue;
            }

            InlinePage* page = shard.pages[candidate >> kPageShift].load(std::memory_order_relaxed);
            if (!page) continue;
            const std::uint32_t offset = candidate & kPageMask;
            auto& fl = page->flags[offset];
            const std::uint8_t f = fl.load(std::memory_order_relaxed);
            if ((f & (kSlotFlagPinned | kSlotFlagGcMarked | kSlotFlagStackRoot)) != 0u) {
                continue;
            }
            if ((f & kSlotFlagClock) != 0u) {
                fl.fetch_and(static_cast<std::uint8_t>(~kSlotFlagClock), std::memory_order_relaxed);
                continue;
            }
            const std::uint8_t curBank = page->activeBank[offset].load(std::memory_order_relaxed) & 1u;
            const InlineStructArenaEntry& victim = page->entries[offset][curBank];
            shard.dedupMap.erase(victim);
            shard.epoch.fetch_add(1u, std::memory_order_release);
            shard.clockEvictions.fetch_add(1u, std::memory_order_relaxed);
            return candidate;
        }
        return kInvalidLocalSlot;
    }

    [[nodiscard]] std::uint32_t acquireSlotLocked(Shard& shard) {
        if (!shard.freeList.empty()) {
            const std::uint32_t slot = shard.freeList.back();
            shard.freeList.pop_back();
            return slot;
        }
        if (shard.highWaterMark < kSoftMaxSlotsPerShard) {
            return allocateFreshSlotLocked(shard);
        }
        const std::uint32_t evicted = tryClockEvictLocked(shard, true);
        if (evicted != kInvalidLocalSlot) {
            return evicted;
        }
        shard.needsPinReclaim.store(true, std::memory_order_relaxed);
        if (shard.highWaterMark < kHardMaxSlotsPerShard) {
            return allocateFreshSlotLocked(shard);
        }
        return tryClockEvictLocked(shard, false);
    }

    std::uint32_t intern(const InlineStructArenaEntry& entry, bool pinEntry = false) noexcept {
        const std::size_t h = InlineStructEntryHash{}(entry);
        const std::uint32_t shardIdx = static_cast<std::uint32_t>((h >> 10u) & kPoolShardMask);
        Shard& shard = shards_[shardIdx];
        if (shard.needsPinReclaim.exchange(false, std::memory_order_relaxed)) {
            (void)reclaimUnreferencedPoolValues();
        }

        struct TlsCacheSlot {
            InlineStructArenaEntry key{};
            std::uint32_t idx = 0;
            std::uint32_t epoch = 0;
        };
        thread_local std::array<TlsCacheSlot, 1024> tlsCache{};
        auto& slot = tlsCache[h & 1023u];
        const std::uint32_t curEpoch = shard.epoch.load(std::memory_order_acquire);
        if (slot.idx != 0u && slot.epoch == curEpoch && InlineStructEntryEq{}(slot.key, entry)) {
            std::uint32_t sIdx = 0u, lSlot = 0u;
            if (decodeShardedPoolId(slot.idx, sIdx, lSlot)) {
                if (InlinePage* page = shard.pages[lSlot >> kPageShift].load(std::memory_order_relaxed)) {
                    auto& fl = page->flags[lSlot & kPageMask];
                    refreshClockBitIfUnset(fl);
                    if (pinEntry) {
                        fl.fetch_or(activePinFlags(), std::memory_order_relaxed);
                        noteShardPinned(shard.mayHavePinnedOrMarked);
                        pinChildSlots(entry);
                    }
                }
            }
            return slot.idx;
        }

        std::lock_guard<std::mutex> lock(shard.mutex);
        const auto found = shard.dedupMap.find(entry);
        if (found != shard.dedupMap.end()) {
            const std::uint32_t localSlot = found->second;
            if (InlinePage* page = shard.pages[localSlot >> kPageShift].load(std::memory_order_relaxed)) {
                const std::uint8_t addFlags = kSlotFlagOccupied | kSlotFlagClock | (pinEntry ? activePinFlags() : 0u);
                page->flags[localSlot & kPageMask].fetch_or(addFlags, std::memory_order_relaxed);
            }
            if (pinEntry) {
                noteShardPinned(shard.mayHavePinnedOrMarked);
                pinChildSlots(entry);
            }
            const std::uint32_t id = encodeShardedPoolId(shardIdx, localSlot);
            slot.key = entry;
            slot.idx = id;
            slot.epoch = shard.epoch.load(std::memory_order_relaxed);
            return id;
        }

        const std::uint32_t localSlot = acquireSlotLocked(shard);
        if (localSlot == kInvalidLocalSlot) {
            return 0u;
        }
        shard.lastAllocatedSlot = localSlot;
        InlinePage* page = shard.pages[localSlot >> kPageShift].load(std::memory_order_relaxed);
        const std::uint32_t offset = localSlot & kPageMask;
        const std::uint8_t nextBank = (page->activeBank[offset].load(std::memory_order_relaxed) ^ 1u) & 1u;
        page->entries[offset][nextBank] = entry;
        page->activeBank[offset].store(nextBank, std::memory_order_release);
        const std::uint8_t initFlags = kSlotFlagOccupied | kSlotFlagClock | (pinEntry ? activePinFlags() : 0u);
        page->flags[offset].store(initFlags, std::memory_order_release);
        if (pinEntry) {
            noteShardPinned(shard.mayHavePinnedOrMarked);
            pinChildSlots(entry);
        }
        shard.dedupMap.emplace(entry, localSlot);

        const std::uint32_t id = encodeShardedPoolId(shardIdx, localSlot);
        slot.key = entry;
        slot.idx = id;
        slot.epoch = shard.epoch.load(std::memory_order_relaxed);
        return id;
    }

    // 100% Lock-Free Read Path
    [[nodiscard]] const InlineStructArenaEntry& get(std::uint32_t idx) noexcept {
        std::uint32_t shardIdx = 0u;
        std::uint32_t localSlot = 0u;
        if (!decodeShardedPoolId(idx, shardIdx, localSlot)) {
            return g_zeroInlineEntry;
        }
        const std::uint32_t pageIdx = localSlot >> kPageShift;
        if (pageIdx < kHardMaxPagesPerShard) {
            Shard& shard = shards_[shardIdx];
            if (InlinePage* page = shard.pages[pageIdx].load(std::memory_order_acquire)) {
                const std::uint32_t offset = localSlot & kPageMask;
                refreshClockBitIfUnset(page->flags[offset]);
                const std::uint8_t bank = page->activeBank[offset].load(std::memory_order_acquire) & 1u;
                return page->entries[offset][bank];
            }
        }
        return g_zeroInlineEntry;
    }

    void pinById(std::uint32_t idx) noexcept {
        std::uint32_t shardIdx = 0u, localSlot = 0u;
        if (!decodeShardedPoolId(idx, shardIdx, localSlot)) return;
        const std::uint32_t pageIdx = localSlot >> kPageShift;
        if (pageIdx < kHardMaxPagesPerShard) {
            Shard& shard = shards_[shardIdx];
            if (InlinePage* page = shard.pages[pageIdx].load(std::memory_order_acquire)) {
                const std::uint32_t offset = localSlot & kPageMask;
                page->flags[offset].fetch_or(
                    activePinFlags(), std::memory_order_relaxed);
                noteShardPinned(shard.mayHavePinnedOrMarked);
                const std::uint8_t bank = page->activeBank[offset].load(std::memory_order_acquire) & 1u;
                pinChildSlots(page->entries[offset][bank]);
            }
        }
    }

    void markById(std::uint32_t idx) noexcept {
        std::uint32_t shardIdx = 0u, localSlot = 0u;
        if (!decodeShardedPoolId(idx, shardIdx, localSlot)) return;
        const std::uint32_t pageIdx = localSlot >> kPageShift;
        if (pageIdx < kHardMaxPagesPerShard) {
            Shard& shard = shards_[shardIdx];
            if (InlinePage* page = shard.pages[pageIdx].load(std::memory_order_acquire)) {
                const std::uint32_t offset = localSlot & kPageMask;
                page->flags[offset].fetch_or(
                    kSlotFlagGcMarked | kSlotFlagPinned, std::memory_order_relaxed);
                noteShardPinned(shard.mayHavePinnedOrMarked);
                const std::uint8_t bank = page->activeBank[offset].load(std::memory_order_acquire) & 1u;
                const auto& entry = page->entries[offset][bank];
                for (const auto& s : entry.slots) {
                    if (s.kind == InlineStructSlot::Kind::String && s.lo != 0u) {
                        runtimeStringPool().markById(s.lo);
                    }
                }
            }
        }
    }

    void markStackRootById(std::uint32_t idx) noexcept {
        std::uint32_t shardIdx = 0u, localSlot = 0u;
        if (!decodeShardedPoolId(idx, shardIdx, localSlot)) return;
        const std::uint32_t pageIdx = localSlot >> kPageShift;
        if (pageIdx < kHardMaxPagesPerShard) {
            Shard& shard = shards_[shardIdx];
            if (InlinePage* page = shard.pages[pageIdx].load(std::memory_order_acquire)) {
                const std::uint32_t offset = localSlot & kPageMask;
                page->flags[offset].fetch_or(
                    kSlotFlagStackRoot | kSlotFlagClock, std::memory_order_relaxed);
                noteShardMarked(shard.mayHavePinnedOrMarked);
                const std::uint8_t bank = page->activeBank[offset].load(std::memory_order_acquire) & 1u;
                const auto& entry = page->entries[offset][bank];
                for (const auto& s : entry.slots) {
                    if (s.kind == InlineStructSlot::Kind::String && s.lo != 0u) {
                        runtimeStringPool().markStackRootById(s.lo);
                    }
                }
            }
        }
    }
};

InlineStructPool& inlineStructPool() {
    static InlineStructPool pool;
    return pool;
}


struct NativeHandleTypeTable {
    static constexpr std::uint32_t kFastCapacity = 4096u;
    std::atomic<semantic::SymbolId> fastSlots[kFastCapacity]{};
    std::atomic<std::uint32_t> fastCount{0u};
    std::mutex overflowMutex;
    std::vector<semantic::SymbolId> overflowTypes;
    std::unordered_map<semantic::SymbolId, std::uint32_t> overflowMap;

    std::uint32_t intern(semantic::SymbolId typeId) noexcept {
        if (typeId == 0u) return 0u;
        struct TlsEntry {
            semantic::SymbolId typeId = 0u;
            std::uint32_t idx = 0u;
        };
        thread_local std::array<TlsEntry, 16> tlsCache{};
        auto& tls = tlsCache[(typeId ^ (typeId >> 17u)) & 15u];
        if (tls.typeId == typeId && tls.idx != 0u) {
            return tls.idx;
        }
        const std::uint32_t cur = std::min<std::uint32_t>(
            fastCount.load(std::memory_order_acquire), kFastCapacity);
        for (std::uint32_t i = 0u; i < cur; ++i) {
            if (fastSlots[i].load(std::memory_order_acquire) == typeId) {
                tls = {typeId, i + 1u};
                return i + 1u;
            }
        }
        std::lock_guard<std::mutex> lock(overflowMutex);
        const std::uint32_t curLocked = std::min<std::uint32_t>(
            fastCount.load(std::memory_order_relaxed), kFastCapacity);
        for (std::uint32_t i = 0u; i < curLocked; ++i) {
            if (fastSlots[i].load(std::memory_order_relaxed) == typeId) {
                tls = {typeId, i + 1u};
                return i + 1u;
            }
        }
        if (curLocked < kFastCapacity) {
            fastSlots[curLocked].store(typeId, std::memory_order_release);
            fastCount.store(curLocked + 1u, std::memory_order_release);
            tls = {typeId, curLocked + 1u};
            return curLocked + 1u;
        }
        const auto found = overflowMap.find(typeId);
        if (found != overflowMap.end()) {
            tls = {typeId, found->second};
            return found->second;
        }
        const std::uint32_t idx = kFastCapacity + 1u +
            static_cast<std::uint32_t>(overflowTypes.size());
        overflowTypes.push_back(typeId);
        overflowMap.emplace(typeId, idx);
        tls = {typeId, idx};
        return idx;
    }

    semantic::SymbolId lookup(std::uint32_t idx) noexcept {
        if (idx == 0u) return 0u;
        if (idx <= kFastCapacity) {
            return fastSlots[idx - 1u].load(std::memory_order_acquire);
        }
        std::lock_guard<std::mutex> lock(overflowMutex);
        const std::size_t off = static_cast<std::size_t>(idx - kFastCapacity - 1u);
        return off < overflowTypes.size() ? overflowTypes[off] : 0u;
    }
};

NativeHandleTypeTable& nativeHandleTypeTable() noexcept {
    static NativeHandleTypeTable table;
    return table;
}

// ----------------------------------------------------------------------------
// 3. ShardedHeapStructStoragePool
// ----------------------------------------------------------------------------
struct HeapStructStoragePool {
    static constexpr std::uint32_t kPageShift = 7u;
    static constexpr std::uint32_t kPageSize = 1u << kPageShift; // 128 StructStorage slots/page
    static constexpr std::uint32_t kPageMask = kPageSize - 1u;
    static constexpr std::uint32_t kSoftMaxPagesPerShard = 4u;   // 512 slots/shard (32,768 total)
    static constexpr std::uint32_t kHardMaxPagesPerShard = 64u;
    static constexpr std::uint32_t kSoftMaxSlotsPerShard = kSoftMaxPagesPerShard * kPageSize;
    static constexpr std::uint32_t kHardMaxSlotsPerShard = kHardMaxPagesPerShard * kPageSize;
    static constexpr std::uint32_t kRecentGuardWindow = 32u;

    struct HeapStructPage {
        StructStorage storages[kPageSize][2]{};
        std::atomic<std::uint8_t> activeBank[kPageSize]{};
        std::uint64_t hashes[kPageSize]{};
        std::atomic<std::uint8_t> flags[kPageSize]{};
    };

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
    struct alignas(64) Shard {
        std::mutex mutex;
        std::atomic<HeapStructPage*> pages[kHardMaxPagesPerShard]{};
        std::vector<std::unique_ptr<HeapStructPage>> ownedPages;
        std::unordered_multimap<std::uint64_t, std::uint32_t> dedupByHash;
        std::vector<std::uint32_t> freeList;
        std::uint32_t highWaterMark = 0u;
        std::uint32_t clockHand = 0u;
        std::uint32_t lastAllocatedSlot = kInvalidLocalSlot;
        std::atomic<std::uint32_t> epoch{1u};
        std::atomic<bool> mayHavePinnedOrMarked{false};
        std::atomic<bool> needsPinReclaim{false};
        std::atomic<std::uint64_t> clockEvictions{0u};
        std::atomic<std::uint64_t> gcReclaimed{0u};
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

    Shard shards_[kPoolShardCount]{};

    [[nodiscard]] std::uint32_t allocateFreshSlotLocked(Shard& shard) {
        const std::uint32_t slot = shard.highWaterMark++;
        const std::uint32_t pageIdx = slot >> kPageShift;
        if (!shard.pages[pageIdx].load(std::memory_order_relaxed)) {
            auto newPage = std::make_unique<HeapStructPage>();
            HeapStructPage* rawPage = newPage.get();
            shard.ownedPages.push_back(std::move(newPage));
            shard.pages[pageIdx].store(rawPage, std::memory_order_release);
        }
        return slot;
    }

    void removeDedupEntryLocked(Shard& shard, std::uint64_t hash, std::uint32_t slot) {
        auto range = shard.dedupByHash.equal_range(hash);
        for (auto it = range.first; it != range.second; ++it) {
            if (it->second == slot) {
                shard.dedupByHash.erase(it);
                break;
            }
        }
    }

    [[nodiscard]] std::uint32_t tryClockEvictLocked(Shard& shard, bool enforceGuardWindow) {
        const std::uint32_t limit = shard.highWaterMark;
        if (limit == 0u) return kInvalidLocalSlot;
        const std::uint32_t maxScans = limit * 2u;
        for (std::uint32_t scan = 0u; scan < maxScans; ++scan) {
            const std::uint32_t candidate = shard.clockHand;
            shard.clockHand = (shard.clockHand + 1u) % limit;
            if (enforceGuardWindow && shard.lastAllocatedSlot != kInvalidLocalSlot) {
                const std::uint32_t dist = (shard.lastAllocatedSlot >= candidate)
                    ? (shard.lastAllocatedSlot - candidate)
                    : (limit - (candidate - shard.lastAllocatedSlot));
                if (dist < kRecentGuardWindow) continue;
            }

            HeapStructPage* page = shard.pages[candidate >> kPageShift].load(std::memory_order_relaxed);
            if (!page) continue;
            const std::uint32_t offset = candidate & kPageMask;
            auto& fl = page->flags[offset];
            const std::uint8_t f = fl.load(std::memory_order_relaxed);
            if ((f & (kSlotFlagPinned | kSlotFlagGcMarked | kSlotFlagStackRoot)) != 0u) {
                continue;
            }
            if ((f & kSlotFlagClock) != 0u) {
                fl.fetch_and(static_cast<std::uint8_t>(~kSlotFlagClock), std::memory_order_relaxed);
                continue;
            }
            removeDedupEntryLocked(shard, page->hashes[offset], candidate);
            shard.epoch.fetch_add(1u, std::memory_order_release);
            shard.clockEvictions.fetch_add(1u, std::memory_order_relaxed);
            return candidate;
        }
        return kInvalidLocalSlot;
    }

    [[nodiscard]] std::uint32_t acquireSlotLocked(Shard& shard) {
        if (!shard.freeList.empty()) {
            const std::uint32_t slot = shard.freeList.back();
            shard.freeList.pop_back();
            return slot;
        }
        if (shard.highWaterMark < kSoftMaxSlotsPerShard) {
            return allocateFreshSlotLocked(shard);
        }
        const std::uint32_t evicted = tryClockEvictLocked(shard, true);
        if (evicted != kInvalidLocalSlot) {
            return evicted;
        }
        shard.needsPinReclaim.store(true, std::memory_order_relaxed);
        if (shard.highWaterMark < kHardMaxSlotsPerShard) {
            return allocateFreshSlotLocked(shard);
        }
        return tryClockEvictLocked(shard, false);
    }

    std::uint32_t registerStorage(std::shared_ptr<const StructStorage> store, bool pinEntry = false) {
        if (!store) return 0u;
        const std::uint64_t h = detail::computeStructFieldsHash(store->fields);
        const std::uint32_t shardIdx = static_cast<std::uint32_t>((h >> 58u) & kPoolShardMask);
        Shard& shard = shards_[shardIdx];
        if (shard.needsPinReclaim.exchange(false, std::memory_order_relaxed)) {
            (void)reclaimUnreferencedPoolValues();
        }

        std::lock_guard<std::mutex> lock(shard.mutex);
        auto range = shard.dedupByHash.equal_range(h);
        for (auto it = range.first; it != range.second; ++it) {
            const std::uint32_t localSlot = it->second;
            HeapStructPage* page = shard.pages[localSlot >> kPageShift].load(std::memory_order_relaxed);
            if (page) {
                const std::uint32_t offset = localSlot & kPageMask;
                const std::uint8_t bank = page->activeBank[offset].load(std::memory_order_relaxed) & 1u;
                if (page->storages[offset][bank].fields == store->fields) {
                    const std::uint8_t addFlags = kSlotFlagOccupied | kSlotFlagClock | (pinEntry ? activePinFlags() : 0u);
                    page->flags[offset].fetch_or(addFlags, std::memory_order_relaxed);
                    if (pinEntry) {
                        noteShardPinned(shard.mayHavePinnedOrMarked);
                        for (const auto& f : store->fields) {
                            pinValueInPools(f);
                        }
                    }
                    return encodeShardedPoolId(shardIdx, localSlot);
                }
            }
        }

        const std::uint32_t localSlot = acquireSlotLocked(shard);
        if (localSlot == kInvalidLocalSlot) {
            return 0u;
        }
        shard.lastAllocatedSlot = localSlot;
        HeapStructPage* page = shard.pages[localSlot >> kPageShift].load(std::memory_order_relaxed);
        const std::uint32_t offset = localSlot & kPageMask;
        const std::uint8_t nextBank = (page->activeBank[offset].load(std::memory_order_relaxed) ^ 1u) & 1u;
        page->storages[offset][nextBank].fields.assign(store->fields.begin(), store->fields.end());
        page->activeBank[offset].store(nextBank, std::memory_order_release);
        page->hashes[offset] = h;
        const std::uint8_t initFlags = kSlotFlagOccupied | kSlotFlagClock | (pinEntry ? activePinFlags() : 0u);
        page->flags[offset].store(initFlags, std::memory_order_release);
        if (pinEntry) {
            noteShardPinned(shard.mayHavePinnedOrMarked);
            for (const auto& f : store->fields) {
                pinValueInPools(f);
            }
        }
        shard.dedupByHash.emplace(h, localSlot);
        return encodeShardedPoolId(shardIdx, localSlot);
    }

    // 100% Lock-Free Read Path (No std::mutex on StructStorageHandle::get()!)
    [[nodiscard]] const StructStorage* get(std::uint32_t id) noexcept {
        std::uint32_t shardIdx = 0u;
        std::uint32_t localSlot = 0u;
        if (!decodeShardedPoolId(id, shardIdx, localSlot)) {
            return nullptr;
        }
        const std::uint32_t pageIdx = localSlot >> kPageShift;
        if (pageIdx < kHardMaxPagesPerShard) {
            Shard& shard = shards_[shardIdx];
            if (HeapStructPage* page = shard.pages[pageIdx].load(std::memory_order_acquire)) {
                const std::uint32_t offset = localSlot & kPageMask;
                refreshClockBitIfUnset(page->flags[offset]);
                const std::uint8_t bank = page->activeBank[offset].load(std::memory_order_acquire) & 1u;
                return &page->storages[offset][bank];
            }
        }
        return nullptr;
    }

    void pinById(std::uint32_t id) noexcept {
        std::uint32_t shardIdx = 0u, localSlot = 0u;
        if (!decodeShardedPoolId(id, shardIdx, localSlot)) return;
        const std::uint32_t pageIdx = localSlot >> kPageShift;
        if (pageIdx < kHardMaxPagesPerShard) {
            Shard& shard = shards_[shardIdx];
            if (HeapStructPage* page = shard.pages[pageIdx].load(std::memory_order_acquire)) {
                const std::uint32_t offset = localSlot & kPageMask;
                const std::uint8_t prev = page->flags[offset].fetch_or(
                    activePinFlags(), std::memory_order_relaxed);
                noteShardPinned(shard.mayHavePinnedOrMarked);
                if ((prev & kSlotFlagPinned) == 0u) {
                    const std::uint8_t bank = page->activeBank[offset].load(std::memory_order_acquire) & 1u;
                    for (const auto& f : page->storages[offset][bank].fields) {
                        pinValueInPools(f);
                    }
                }
            }
        }
    }

    void markById(std::uint32_t id) noexcept {
        std::uint32_t shardIdx = 0u, localSlot = 0u;
        if (!decodeShardedPoolId(id, shardIdx, localSlot)) return;
        const std::uint32_t pageIdx = localSlot >> kPageShift;
        if (pageIdx < kHardMaxPagesPerShard) {
            Shard& shard = shards_[shardIdx];
            if (HeapStructPage* page = shard.pages[pageIdx].load(std::memory_order_acquire)) {
                const std::uint32_t offset = localSlot & kPageMask;
                const std::uint8_t prev = page->flags[offset].fetch_or(
                    kSlotFlagGcMarked | kSlotFlagPinned, std::memory_order_relaxed);
                noteShardPinned(shard.mayHavePinnedOrMarked);
                if ((prev & kSlotFlagGcMarked) == 0u) {
                    const std::uint8_t bank = page->activeBank[offset].load(std::memory_order_acquire) & 1u;
                    for (const auto& f : page->storages[offset][bank].fields) {
                        markValueInPools(f);
                    }
                }
            }
        }
    }

    void markStackRootById(std::uint32_t id) noexcept {
        std::uint32_t shardIdx = 0u, localSlot = 0u;
        if (!decodeShardedPoolId(id, shardIdx, localSlot)) return;
        const std::uint32_t pageIdx = localSlot >> kPageShift;
        if (pageIdx < kHardMaxPagesPerShard) {
            Shard& shard = shards_[shardIdx];
            if (HeapStructPage* page = shard.pages[pageIdx].load(std::memory_order_acquire)) {
                const std::uint32_t offset = localSlot & kPageMask;
                const std::uint8_t prev = page->flags[offset].fetch_or(
                    kSlotFlagStackRoot | kSlotFlagClock, std::memory_order_relaxed);
                noteShardMarked(shard.mayHavePinnedOrMarked);
                if ((prev & kSlotFlagStackRoot) == 0u) {
                    const std::uint8_t bank = page->activeBank[offset].load(std::memory_order_acquire) & 1u;
                    for (const auto& f : page->storages[offset][bank].fields) {
                        markStackRootValueInPools(f);
                    }
                }
            }
        }
    }
};

HeapStructStoragePool& heapStructPool() {
    static HeapStructStoragePool pool;
    return pool;
}

} // namespace

namespace detail {

const std::string& emptyRuntimePoolString() noexcept {
    return runtimeStringPool().empty();
}

StringValue internRuntimeString(std::string_view text, bool pinEntry) {
    if (!pinEntry) {
        if (const auto* bridge = g_externalPoolBridge.load(std::memory_order_relaxed)) {
            if (bridge->internStringFn) {
                return bridge->internStringFn(text);
            }
        }
    }
    return runtimeStringPool().intern(text, pinEntry);
}

StringValue lookupRuntimeStringById(std::uint32_t id) noexcept {
    if (const auto* bridge = g_externalPoolBridge.load(std::memory_order_relaxed)) {
        if (bridge->stringByIdFn) return bridge->stringByIdFn(id);
    }
    return runtimeStringPool().getById(id);
}

std::uint32_t allocateInlineArenaEntry(const InlineStructArenaEntry& entry) noexcept {
    if (const auto* bridge = g_externalPoolBridge.load(std::memory_order_relaxed)) {
        if (bridge->internInlineArenaFn) return bridge->internInlineArenaFn(entry);
    }
    return inlineStructPool().intern(entry);
}

const InlineStructArenaEntry& lookupInlineArenaEntry(std::uint32_t idx) noexcept {
    if (const auto* bridge = g_externalPoolBridge.load(std::memory_order_relaxed)) {
        if (bridge->lookupInlineArenaFn) {
            if (const auto* ptr = bridge->lookupInlineArenaFn(idx)) {
                return *ptr;
            }
        }
    }
    return inlineStructPool().get(idx);
}

std::uint32_t registerHeapStructStorage(
    std::shared_ptr<const StructStorage> store,
    bool pinEntry) {
    if (!pinEntry) {
        if (const auto* bridge = g_externalPoolBridge.load(std::memory_order_relaxed)) {
            if (bridge->registerHeapStructFn) {
                return bridge->registerHeapStructFn(&store);
            }
        }
    }
    return heapStructPool().registerStorage(std::move(store), pinEntry);
}

const StructStorage* lookupHeapStructStorage(std::uint32_t id) noexcept {
    if (const auto* bridge = g_externalPoolBridge.load(std::memory_order_relaxed)) {
        if (bridge->lookupHeapStructFn) {
            return static_cast<const StructStorage*>(bridge->lookupHeapStructFn(id));
        }
    }
    return heapStructPool().get(id);
}

} // namespace detail

EngineRuntime::EngineRuntime(std::shared_ptr<const ProgramImage> program)
    : program_(std::move(program)),
      heap_(std::make_shared<ManagedHeap>()),
      nativeHandles_(std::make_shared<NativeHandleRegistry>()),
      asyncRuntime_(std::make_shared<AsyncRuntimeContext>(heap_.get())) {}

void EngineRuntime::setBindings(std::shared_ptr<const BindingRegistry> bindings) {
    bindings_ = std::move(bindings);
}

void EngineRuntime::setHeap(std::shared_ptr<ManagedHeap> heap) {
    auto oldHeap = std::move(heap_);
    heap_ = heap ? std::move(heap) : std::make_shared<ManagedHeap>();
    if (asyncRuntime_) {
        asyncRuntime_->setHeap(heap_.get());
    }
}

std::shared_ptr<ManagedHeap> EngineRuntime::heap() const noexcept { return heap_; }

void EngineRuntime::setNativeHandles(
    std::shared_ptr<NativeHandleRegistry> handles) {
    nativeHandles_ = handles
        ? std::move(handles)
        : std::make_shared<NativeHandleRegistry>();
}

std::shared_ptr<NativeHandleRegistry> EngineRuntime::nativeHandles() const noexcept {
    return nativeHandles_;
}

void EngineRuntime::setAsyncRuntime(
    std::shared_ptr<AsyncRuntimeContext> asyncRuntime) {
    asyncRuntime_ = asyncRuntime
        ? std::move(asyncRuntime)
        : std::make_shared<AsyncRuntimeContext>(heap_.get());
    asyncRuntime_->setHeap(heap_.get());
}

std::shared_ptr<AsyncRuntimeContext> EngineRuntime::asyncRuntime() const noexcept {
    return asyncRuntime_;
}

ExecutionResult EngineRuntime::invoke(
    const std::string& qualifiedName,
    const std::vector<Value>& arguments,
    ExecutionOptions options) const {
    auto program = programSnapshot();
    if (!program) {
        ExecutionResult result;
        result.error.code = ErrorCode::InvalidProgram;
        result.error.message = "runtime has no linked program image";
        return result;
    }
    const auto entry = program->findFunction(qualifiedName);
    if (!entry) {
        ExecutionResult result;
        result.error.code = ErrorCode::FunctionNotFound;
        result.error.message =
            "entry function '" + qualifiedName + "' was not found";
        return result;
    }
    Interpreter interpreter(std::move(program), heap_);
    interpreter.setBindingRegistry(bindings_);
    interpreter.setAsyncRuntime(asyncRuntime_);
    return interpreter.invoke(*entry, arguments, std::move(options));
}

ExecutionResult EngineRuntime::invoke(
    semantic::SymbolId symbolId,
    const std::vector<Value>& arguments,
    ExecutionOptions options) const {
    auto program = programSnapshot();
    if (!program) {
        ExecutionResult result;
        result.error.code = ErrorCode::InvalidProgram;
        result.error.message = "runtime has no linked program image";
        return result;
    }
    Interpreter interpreter(std::move(program), heap_);
    interpreter.setBindingRegistry(bindings_);
    interpreter.setAsyncRuntime(asyncRuntime_);
    return interpreter.invoke(symbolId, arguments, std::move(options));
}

const ProgramImage& EngineRuntime::program() const noexcept {
    std::lock_guard<std::mutex> lock(programMutex_);
    return *program_;
}

std::shared_ptr<const ProgramImage> EngineRuntime::programSnapshot() const noexcept {
    std::lock_guard<std::mutex> lock(programMutex_);
    return program_;
}

void EngineRuntime::replaceProgram(std::shared_ptr<const ProgramImage> program) {
    if (!program) return;
    std::lock_guard<std::mutex> lock(programMutex_);
    if (program_) retiredPrograms_.push_back(program_);
    program_ = std::move(program);
}

bool EngineRuntime::replaceProgram(
    std::shared_ptr<const ProgramImage> program,
    const std::vector<TypeLayoutMigration>& migrations,
    LayoutMigrationStats* outStats,
    RuntimeError* error) {
    if (!program) {
        if (error) {
            error->code = ErrorCode::InvalidProgram;
            error->message = "replacement program image is null";
        }
        return false;
    }
    std::lock_guard<std::mutex> lock(programMutex_);
    if (heap_ && !migrations.empty()) {
        if (!heap_->migrateTypeLayouts(migrations, outStats, error)) {
            return false;
        }
    }
    if (program_) retiredPrograms_.push_back(program_);
    program_ = std::move(program);
    return true;
}

const char* traceEventKindName(TraceEventKind kind) noexcept {
    switch (kind) {
    case TraceEventKind::FunctionEnter: return "function-enter";
    case TraceEventKind::FunctionExit: return "function-exit";
    case TraceEventKind::Instruction: return "instruction";
    case TraceEventKind::Branch: return "branch";
    case TraceEventKind::ExternalCall: return "external-call";
    case TraceEventKind::GcStep: return "gc-step";
    case TraceEventKind::RuntimeError: return "runtime-error";
    }
    return "unknown";
}


namespace {

void splitStringBy(std::string_view input, char delim, std::vector<std::string>& out) {
    out.clear();
    if (input.empty()) return;
    std::size_t start = 0;
    while (start <= input.size()) {
        const auto pos = input.find(delim, start);
        const auto piece = pos == std::string_view::npos
            ? input.substr(start)
            : input.substr(start, pos - start);
        if (!piece.empty()) {
            out.emplace_back(piece);
        }
        if (pos == std::string_view::npos) break;
        start = pos + 1;
    }
}

void parseEcsQuerySpec(
    std::string_view spec,
    std::vector<std::string>& cols,
    std::vector<std::string>& withs,
    std::vector<std::string>& withouts) {
    cols.clear();
    withs.clear();
    withouts.clear();
    const auto p1 = spec.find(';');
    if (p1 == std::string_view::npos) {
        splitStringBy(spec, ',', cols);
        return;
    }
    splitStringBy(spec.substr(0, p1), ',', cols);
    const auto rest = spec.substr(p1 + 1);
    const auto p2 = rest.find(';');
    if (p2 == std::string_view::npos) {
        splitStringBy(rest, ',', withs);
        return;
    }
    splitStringBy(rest.substr(0, p2), ',', withs);
    splitStringBy(rest.substr(p2 + 1), ',', withouts);
}

bool readSparseIds(
    ManagedHeap* heap,
    const std::string& typeName,
    std::vector<std::int64_t>& outIds,
    ObjectRef* outValsRef = nullptr) {
    outIds.clear();
    if (!heap) return false;
    if (const auto* nativeIds = heap->nativeEcsEntityIds(typeName)) {
        outIds = *nativeIds;
        if (outValsRef) *outValsRef = ObjectRef{};
        return true;
    }
    const auto idsVal = heap->getStaticField(
        semantic::stableTypeId("$ecs_sparse_ids::" + typeName));
    if (!idsVal) return false;
    const auto* idsRef = std::get_if<ObjectRef>(&*idsVal);
    if (!idsRef || !idsRef->valid()) return false;
    const auto len = heap->arrayLength(*idsRef);
    if (!len) return false;
    outIds.reserve(*len);
    for (std::size_t i = 0; i < *len; ++i) {
        const auto v = heap->arrayGet(*idsRef, i);
        if (v && std::holds_alternative<std::int64_t>(*v)) {
            outIds.push_back(std::get<std::int64_t>(*v));
        }
    }
    if (outValsRef) {
        const auto valsVal = heap->getStaticField(
            semantic::stableTypeId("$ecs_sparse_vals::" + typeName));
        if (valsVal && std::holds_alternative<ObjectRef>(*valsVal)) {
            *outValsRef = std::get<ObjectRef>(*valsVal);
        }
    }
    return true;
}

bool hasAnySparseEcsTable(
    ManagedHeap* heap,
    const std::vector<std::string>& cols,
    const std::vector<std::string>& withs,
    const std::vector<std::string>& withouts) {
    if (!heap) return false;
    for (const auto& list : {cols, withs, withouts}) {
        for (const auto& t : list) {
            if (heap->hasNativeEcsTable(t)) {
                return true;
            }
            if (heap->hasStaticField(semantic::stableTypeId("$ecs_sparse_ids::" + t))) {
                heap->notifySparseEcsState();
                return true;
            }
        }
    }
    return false;
}

std::vector<std::int64_t> computeMatchedEcsEntities(
    ManagedHeap* heap,
    const std::vector<std::string>& cols,
    const std::vector<std::string>& withs,
    const std::vector<std::string>& withouts) {
    std::vector<std::int64_t> matched;
    bool initialized = false;
    for (const auto& list : {cols, withs}) {
        for (const auto& reqType : list) {
            std::vector<std::int64_t> typeIds;
            if (!readSparseIds(heap, reqType, typeIds)) {
                return {};
            }
            if (!initialized) {
                matched = std::move(typeIds);
                initialized = true;
            } else {
                std::vector<std::int64_t> next;
                for (const auto id : matched) {
                    if (std::binary_search(typeIds.begin(), typeIds.end(), id)) {
                        next.push_back(id);
                    }
                }
                matched = std::move(next);
            }
            if (matched.empty()) return {};
        }
    }
    for (const auto& exType : withouts) {
        std::vector<std::int64_t> exIds;
        if (!readSparseIds(heap, exType, exIds) || exIds.empty()) continue;
        std::vector<std::int64_t> next;
        for (const auto id : matched) {
            if (!std::binary_search(exIds.begin(), exIds.end(), id)) {
                next.push_back(id);
            }
        }
        matched = std::move(next);
        if (matched.empty()) return {};
    }
    return matched;
}

bool makeIntrinsicStringResult(
    std::string text,
    ManagedHeap* heap,
    Value& result,
    std::string& errorMessage) {
    if (heap) {
        RuntimeError err;
        const auto ref = heap->allocateString(std::move(text), &err);
        if (!ref) {
            errorMessage = err.message.empty()
                ? "out of memory allocating string"
                : err.message;
            return false;
        }
        result = *ref;
        return true;
    }
    result = std::move(text);
    return true;
}

} // namespace

bool tryExecuteRuntimeIntrinsic(
    std::string_view name,
    ValueSpan arguments,
    ManagedHeap* heap,
    Value& result,
    std::string& errorMessage) {
    constexpr std::string_view prefix = "$intrinsic::";
    if (name.rfind(prefix, 0) != 0) return false;
    const std::string_view op = name.substr(prefix.size());

    constexpr std::string_view staticHasPrefix = "static_has::";
    if (op.rfind(staticHasPrefix, 0) == 0) {
        const std::string idStr(op.substr(staticHasPrefix.size()));
        const auto id = static_cast<semantic::SymbolId>(
            std::strtoull(idStr.c_str(), nullptr, 10));
        result = heap ? heap->hasStaticField(id) : false;
        return true;
    }

    constexpr std::string_view staticGetPrefix = "static_get::";
    if (op.rfind(staticGetPrefix, 0) == 0) {
        const std::string_view subOp = op.substr(staticGetPrefix.size());
        constexpr std::string_view ecsColPrefix = "ecs_col::";
        if (subOp.rfind(ecsColPrefix, 0) == 0) {
            const auto rest = subOp.substr(ecsColPrefix.size());
            struct CachedEcsColSpec {
                bool valid = false;
                std::size_t colIdx = 0;
                std::string specStr;
                std::string targetType;
                std::vector<std::string> cols;
                std::vector<std::string> withs;
                std::vector<std::string> withouts;
                semantic::SymbolId denseId = 0;
                semantic::SymbolId emptyFallbackId = 0;
                semantic::SymbolId arrTypeId = 0;
                semantic::SymbolId elemTypeId = 0;
                semantic::PrimitiveType elemPrim = semantic::PrimitiveType::Struct;
                semantic::SymbolId wbMapId = 0;
                semantic::SymbolId wbTargetId = 0;
            };
            thread_local std::unordered_map<std::string, CachedEcsColSpec> colCache;
            const std::string restKey(rest);
            auto cacheIt = colCache.find(restKey);
            if (cacheIt == colCache.end()) {
                CachedEcsColSpec entry;
                const auto sep = rest.find("::");
                if (sep != std::string_view::npos) {
                    entry.colIdx = static_cast<std::size_t>(
                        std::strtoull(std::string(rest.substr(0, sep)).c_str(), nullptr, 10));
                    const auto specView = rest.substr(sep + 2);
                    entry.specStr = std::string(specView);
                    parseEcsQuerySpec(specView, entry.cols, entry.withs, entry.withouts);
                    if (entry.colIdx < entry.cols.size()) {
                        entry.valid = true;
                        entry.targetType = entry.cols[entry.colIdx];
                        entry.denseId = semantic::stableTypeId("$ecs_column::" + entry.targetType);
                        entry.emptyFallbackId = semantic::stableTypeId("$ecs_empty_col::" + entry.targetType);
                        entry.elemPrim = semantic::PrimitiveType::Struct;
                        entry.elemTypeId = semantic::stableTypeId(entry.targetType);
                        entry.arrTypeId = semantic::stableTypeId(
                            semantic::arrayTypeName(entry.elemPrim, entry.targetType));
                        entry.wbMapId = semantic::stableTypeId("$ecs_wb_map::" + restKey);
                        entry.wbTargetId = semantic::stableTypeId("$ecs_wb_target::" + restKey);
                    }
                }
                cacheIt = colCache.emplace(restKey, std::move(entry)).first;
            }
            const auto& cached = cacheIt->second;
            if (!cached.valid) {
                errorMessage = "malformed ecs_col intrinsic";
                return false;
            }
            const auto getOrCreateEmptyColArray = [&]() -> Value {
                if (!arguments.empty() && std::holds_alternative<ObjectRef>(arguments[0])) {
                    return arguments[0];
                }
                if (!heap) return NullArray{};
                if (auto existingEmpty = heap->getStaticField(cached.emptyFallbackId)) {
                    return *existingEmpty;
                }
                RuntimeError allocErr;
                Value defaultElem = makeDefaultInlineStructValue(cached.elemTypeId)
                    .value_or(Value{StructValue{cached.elemTypeId, std::make_shared<StructStorage>()}});
                if (const auto emptyRef = heap->allocateTypedArray(
                        cached.arrTypeId, cached.elemPrim, cached.elemTypeId, 0, defaultElem, &allocErr)) {
                    heap->setStaticField(cached.emptyFallbackId, *emptyRef);
                    return *emptyRef;
                }
                return NullArray{};
            };
            const auto& targetType = cached.targetType;
            const auto& cols = cached.cols;
            const auto& withs = cached.withs;
            const auto& withouts = cached.withouts;
            const auto colIdx = cached.colIdx;
            const std::string_view spec = cached.specStr;
            if (!hasAnySparseEcsTable(heap, cols, withs, withouts) &&
                withs.empty() && withouts.empty()) {
                if (heap) {
                    if (auto existing = heap->getStaticField(cached.denseId)) {
                        result = std::move(*existing);
                        return true;
                    }
                }
                result = getOrCreateEmptyColArray();
                if (heap && std::holds_alternative<ObjectRef>(result)) {
                    heap->setStaticField(cached.denseId, result);
                }
                return true;
            }
            if (!heap) {
                result = getOrCreateEmptyColArray();
                return true;
            }
            semantic::SymbolId arrTypeId = cached.arrTypeId;
            semantic::PrimitiveType elemPrim = cached.elemPrim;
            semantic::SymbolId elemTypeId = cached.elemTypeId;
            if (!arguments.empty() && std::holds_alternative<ObjectRef>(arguments[0])) {
                const auto tmplRef = std::get<ObjectRef>(arguments[0]);
                arrTypeId = heap->arrayTypeId(tmplRef).value_or(arrTypeId);
                elemPrim = heap->arrayElementType(tmplRef).value_or(elemPrim);
                elemTypeId = heap->arrayElementTypeId(tmplRef).value_or(elemTypeId);
            }
            const auto matched = computeMatchedEcsEntities(heap, cols, withs, withouts);
            if (matched.empty()) {
                result = getOrCreateEmptyColArray();
                return true;
            }
            const auto* nativeIds = heap->nativeEcsEntityIds(targetType);
            const auto* nativeVals = heap->nativeEcsComponentValues(targetType);
            std::vector<std::int64_t> sparseIds;
            ObjectRef sparseValsRef{};
            if (!nativeIds || !nativeVals) {
                if (!readSparseIds(heap, targetType, sparseIds, &sparseValsRef) ||
                    !sparseValsRef.valid()) {
                    result = getOrCreateEmptyColArray();
                    return true;
                }
            }
            std::vector<Value> matchedVals;
            matchedVals.reserve(matched.size());
            std::vector<std::int64_t> matchedSrcIndices;
            matchedSrcIndices.reserve(matched.size());
            for (const auto entId : matched) {
                if (nativeIds && nativeVals) {
                    const auto it = std::lower_bound(nativeIds->begin(), nativeIds->end(), entId);
                    if (it != nativeIds->end() && *it == entId) {
                        const auto idx = static_cast<std::size_t>(std::distance(nativeIds->begin(), it));
                        if (idx < nativeVals->size()) {
                            matchedVals.push_back((*nativeVals)[idx]);
                            matchedSrcIndices.push_back(static_cast<std::int64_t>(idx));
                        }
                    }
                } else {
                    const auto it = std::lower_bound(sparseIds.begin(), sparseIds.end(), entId);
                    if (it != sparseIds.end() && *it == entId) {
                        const auto idx = static_cast<std::size_t>(std::distance(sparseIds.begin(), it));
                        if (auto v = heap->arrayGet(sparseValsRef, idx)) {
                            matchedVals.push_back(std::move(*v));
                            matchedSrcIndices.push_back(static_cast<std::int64_t>(idx));
                        }
                    }
                }
            }
            if (matchedVals.empty()) {
                result = getOrCreateEmptyColArray();
                return true;
            }
            RuntimeError allocErr;
            const auto outArr = heap->allocateTypedArray(
                arrTypeId, elemPrim, elemTypeId, matchedVals.size(), matchedVals[0], &allocErr);
            if (!outArr) {
                errorMessage = allocErr.message;
                return false;
            }
            for (std::size_t i = 0; i < matchedVals.size(); ++i) {
                (void)heap->arraySet(*outArr, i, matchedVals[i], &allocErr);
            }
            const auto intArrTypeId = semantic::stableTypeId(
                semantic::arrayTypeName(semantic::PrimitiveType::Int, {}));
            if (const auto mapArr = heap->allocateTypedArray(
                    intArrTypeId,
                    semantic::PrimitiveType::Int,
                    0,
                    matchedSrcIndices.size(),
                    std::int64_t{0},
                    &allocErr)) {
                for (std::size_t i = 0; i < matchedSrcIndices.size(); ++i) {
                    (void)heap->arraySet(*mapArr, i, matchedSrcIndices[i], &allocErr);
                }
                const std::string wbMapKey =
                    "$ecs_wb_map::" + std::to_string(colIdx) + "::" + std::string(spec);
                const std::string wbTargetKey =
                    "$ecs_wb_target::" + std::to_string(colIdx) + "::" + std::string(spec);
                heap->setStaticField(semantic::stableTypeId(wbMapKey), *mapArr);
                heap->setStaticField(semantic::stableTypeId(wbTargetKey), sparseValsRef);
                heap->notifySparseEcsState();
            }
            result = *outArr;
            return true;
        }

        constexpr std::string_view ecsEntPrefix = "ecs_ent::";
        if (subOp.rfind(ecsEntPrefix, 0) == 0) {
            const auto spec = subOp.substr(ecsEntPrefix.size());
            std::vector<std::string> cols, withs, withouts;
            parseEcsQuerySpec(spec, cols, withs, withouts);
            const auto arrTypeId = (!arguments.empty() && std::holds_alternative<ObjectRef>(arguments[0]) && heap)
                ? heap->arrayTypeId(std::get<ObjectRef>(arguments[0])).value_or(0)
                : semantic::stableTypeId(semantic::arrayTypeName(semantic::PrimitiveType::Int, {}));
            const auto getOrCreateEmptyEntArray = [&]() -> Value {
                if (!arguments.empty() && std::holds_alternative<ObjectRef>(arguments[0])) {
                    return arguments[0];
                }
                if (!heap) return NullArray{};
                const auto emptyEntId = semantic::stableTypeId("$ecs_empty_ent_arr");
                if (auto existingEmpty = heap->getStaticField(emptyEntId)) {
                    return *existingEmpty;
                }
                RuntimeError allocErr;
                if (const auto emptyRef = heap->allocateTypedArray(
                        arrTypeId, semantic::PrimitiveType::Int, 0, 0, std::int64_t{0}, &allocErr)) {
                    heap->setStaticField(emptyEntId, *emptyRef);
                    return *emptyRef;
                }
                return NullArray{};
            };
            if (!heap) {
                result = getOrCreateEmptyEntArray();
                return true;
            }
            std::vector<std::int64_t> matched;
            if (!hasAnySparseEcsTable(heap, cols, withs, withouts) &&
                withs.empty() && withouts.empty() && !cols.empty()) {
                const auto denseId = semantic::stableTypeId("$ecs_column::" + cols[0]);
                if (auto existing = heap->getStaticField(denseId)) {
                    if (const auto* arrRef = std::get_if<ObjectRef>(&*existing)) {
                        const auto len = heap->arrayLength(*arrRef).value_or(0);
                        matched.resize(len);
                        for (std::size_t i = 0; i < len; ++i) {
                            matched[i] = static_cast<std::int64_t>(i);
                        }
                    }
                }
            } else {
                matched = computeMatchedEcsEntities(heap, cols, withs, withouts);
            }
            if (matched.empty()) {
                result = getOrCreateEmptyEntArray();
                return true;
            }
            RuntimeError allocErr;
            const auto outArr = heap->allocateTypedArray(
                arrTypeId,
                semantic::PrimitiveType::Int,
                0,
                matched.size(),
                std::int64_t{0},
                &allocErr);
            if (!outArr) {
                errorMessage = allocErr.message;
                return false;
            }
            for (std::size_t i = 0; i < matched.size(); ++i) {
                (void)heap->arraySet(*outArr, i, matched[i], &allocErr);
            }
            result = *outArr;
            return true;
        }

        const std::string idStr(subOp);
        const auto id = static_cast<semantic::SymbolId>(
            std::strtoull(idStr.c_str(), nullptr, 10));
        if (heap) {
            if (auto existing = heap->getStaticField(id)) {
                result = std::move(*existing);
                return true;
            }
        }
        result = arguments.empty() ? Value{} : arguments[0];
        if (heap) {
            heap->setStaticField(id, result);
        }
        return true;
    }

    constexpr std::string_view ecsWritebackPrefix = "ecs_writeback::";
    if (op.rfind(ecsWritebackPrefix, 0) == 0) {
        const auto rest = op.substr(ecsWritebackPrefix.size());
        if (arguments.size() < 2 || !heap) {
            result = std::monostate{};
            return true;
        }
        const auto* idxPtr = std::get_if<std::int64_t>(&arguments[0]);
        if (!idxPtr || *idxPtr < 0) {
            result = std::monostate{};
            return true;
        }
        struct CachedWritebackSpec {
            bool valid = false;
            std::size_t colIdx = 0;
            std::string compKey;
            std::vector<std::string> cols;
            std::vector<std::string> withs;
            std::vector<std::string> withouts;
            semantic::SymbolId wbMapId = 0;
            semantic::SymbolId wbTargetId = 0;
            semantic::SymbolId sparseIdsId = 0;
        };
        thread_local std::unordered_map<std::string, CachedWritebackSpec> wbCache;
        const std::string restKey(rest);
        auto cacheIt = wbCache.find(restKey);
        if (cacheIt == wbCache.end()) {
            CachedWritebackSpec entry;
            const auto sep = rest.find("::");
            if (sep != std::string_view::npos) {
                entry.colIdx = static_cast<std::size_t>(
                    std::strtoull(std::string(rest.substr(0, sep)).c_str(), nullptr, 10));
                const auto spec = rest.substr(sep + 2);
                parseEcsQuerySpec(spec, entry.cols, entry.withs, entry.withouts);
                if (entry.colIdx < entry.cols.size()) {
                    entry.valid = true;
                    entry.compKey = entry.cols[entry.colIdx];
                    entry.wbMapId = semantic::stableTypeId("$ecs_wb_map::" + restKey);
                    entry.wbTargetId = semantic::stableTypeId("$ecs_wb_target::" + restKey);
                    entry.sparseIdsId = semantic::stableTypeId("$ecs_sparse_ids::" + entry.compKey);
                }
            }
            cacheIt = wbCache.emplace(restKey, std::move(entry)).first;
        }
        const auto& cached = cacheIt->second;
        if (!cached.valid) {
            result = std::monostate{};
            return true;
        }
        // Fast O(1) exit for dense columns and zero-copy host C++ raw columns (StoreElement already mutated in-place)
        if (!heap->hasSparseEcsState() &&
            !heap->hasStaticField(cached.wbMapId) &&
            !heap->hasStaticField(cached.sparseIdsId)) {
            result = std::monostate{};
            return true;
        }
        const auto idx = static_cast<std::size_t>(*idxPtr);
        const std::string& compKey = cached.compKey;
        const auto& cols = cached.cols;
        const auto& withs = cached.withs;
        const auto& withouts = cached.withouts;
        if (const auto mapVal = heap->getStaticField(cached.wbMapId);
            mapVal && std::holds_alternative<ObjectRef>(*mapVal)) {
            const auto mapRef = std::get<ObjectRef>(*mapVal);
            if (const auto srcIdxVal = heap->arrayGet(mapRef, idx);
                srcIdxVal && std::holds_alternative<std::int64_t>(*srcIdxVal)) {
                const auto srcIdx = static_cast<std::size_t>(std::get<std::int64_t>(*srcIdxVal));
                if (heap->hasNativeEcsTable(compKey)) {
                    (void)heap->setNativeEcsComponentByDenseIndex(compKey, srcIdx, arguments[1]);
                    result = std::monostate{};
                    return true;
                }
                const auto targetVal = heap->getStaticField(cached.wbTargetId);
                if (targetVal && std::holds_alternative<ObjectRef>(*targetVal)) {
                    const auto targetRef = std::get<ObjectRef>(*targetVal);
                    RuntimeError setErr;
                    (void)heap->arraySet(targetRef, srcIdx, arguments[1], &setErr);
                    result = std::monostate{};
                    return true;
                }
            }
        }
        if (heap->hasNativeEcsTable(compKey)) {
            const auto matched = computeMatchedEcsEntities(heap, cols, withs, withouts);
            if (idx < matched.size()) {
                (void)heap->setNativeEcsComponent(compKey, matched[idx], arguments[1]);
            }
            result = std::monostate{};
            return true;
        }
        if (!heap->hasStaticField(cached.sparseIdsId)) {
            result = std::monostate{};
            return true;
        }
        heap->notifySparseEcsState();
        const auto matched = computeMatchedEcsEntities(heap, cols, withs, withouts);
        if (idx < matched.size()) {
            const auto entId = matched[idx];
            std::vector<std::int64_t> sparseIds;
            ObjectRef sparseValsRef{};
            if (readSparseIds(heap, compKey, sparseIds, &sparseValsRef) &&
                sparseValsRef.valid()) {
                const auto it = std::lower_bound(sparseIds.begin(), sparseIds.end(), entId);
                if (it != sparseIds.end() && *it == entId) {
                    const auto srcIdx = static_cast<std::size_t>(
                        std::distance(sparseIds.begin(), it));
                    RuntimeError setErr;
                    (void)heap->arraySet(sparseValsRef, srcIdx, arguments[1], &setErr);
                }
            }
        }
        result = std::monostate{};
        return true;
    }

    constexpr std::string_view staticSetPrefix = "static_set::";
    if (op.rfind(staticSetPrefix, 0) == 0) {
        const std::string idStr(op.substr(staticSetPrefix.size()));
        const auto id = static_cast<semantic::SymbolId>(
            std::strtoull(idStr.c_str(), nullptr, 10));
        if (heap && !arguments.empty()) {
            heap->setStaticField(id, arguments[0]);
        }
        result = std::monostate{};
        return true;
    }

    if (op == "stable_hash" || op.rfind("stable_hash::", 0) == 0) {
        if (arguments.empty()) {
            result = std::int64_t{0};
            return true;
        }
        std::uint64_t h = 1469598103934665603ULL;
        const auto mixU64 = [&](std::uint64_t v) {
            for (unsigned shift = 0; shift < 64; shift += 8) {
                h ^= static_cast<std::uint8_t>((v >> shift) & 0xffu);
                h *= 1099511628211ULL;
            }
        };
        const auto mixStr = [&](std::string_view s) {
            mixU64(s.size());
            for (unsigned char ch : s) {
                h ^= ch;
                h *= 1099511628211ULL;
            }
        };
        const auto hashVal = [&](const auto& self, const Value& v) -> void {
            mixU64(v.index());
            if (const auto* b = std::get_if<bool>(&v)) mixU64(*b ? 1u : 0u);
            else if (const auto* i = std::get_if<std::int64_t>(&v)) mixU64(static_cast<std::uint64_t>(*i));
            else if (const auto* l = std::get_if<LongValue>(&v)) mixU64(static_cast<std::uint64_t>(l->value));
            else if (const auto* ui = std::get_if<UIntValue>(&v)) mixU64(ui->value);
            else if (const auto* ul = std::get_if<ULongValue>(&v)) mixU64(ul->value);
            else if (const auto* by = std::get_if<ByteValue>(&v)) mixU64(by->value);
            else if (const auto* sb = std::get_if<SByteValue>(&v)) mixU64(static_cast<std::uint8_t>(sb->value));
            else if (const auto* sh = std::get_if<ShortValue>(&v)) mixU64(static_cast<std::uint16_t>(sh->value));
            else if (const auto* us = std::get_if<UShortValue>(&v)) mixU64(us->value);
            else if (const auto* ch = std::get_if<CharValue>(&v)) mixU64(ch->value);
            else if (EnumValue en{}; v.tryGetEnum(en)) { mixU64(en.typeId); mixU64(static_cast<std::uint64_t>(en.value)); }
            else if (const auto* fx = std::get_if<FixedValue>(&v)) mixU64(static_cast<std::uint32_t>(fx->raw));
            else if (const auto* f16 = std::get_if<Fixed16Value>(&v)) mixU64(static_cast<std::uint64_t>(f16->raw));
            else if (const auto* v2 = std::get_if<Vec2Value>(&v)) { mixU64(static_cast<std::uint32_t>(v2->x)); mixU64(static_cast<std::uint32_t>(v2->y)); }
            else if (const auto* v3 = std::get_if<Vec3Value>(&v)) { mixU64(static_cast<std::uint32_t>(v3->x)); mixU64(static_cast<std::uint32_t>(v3->y)); mixU64(static_cast<std::uint32_t>(v3->z)); }
            else if (QuatValue q{}; v.tryGetQuat(q)) { mixU64(static_cast<std::uint32_t>(q.x)); mixU64(static_cast<std::uint32_t>(q.y)); mixU64(static_cast<std::uint32_t>(q.z)); mixU64(static_cast<std::uint32_t>(q.w)); }
            else if (const auto* f64 = std::get_if<Fixed64Value>(&v)) mixU64(static_cast<std::uint64_t>(f64->raw));
            else if (const auto* v2f = std::get_if<Vec2fValue>(&v)) {
                auto mixF = [&](float f) { std::uint32_t b = 0; const float n = (f == 0.0f) ? 0.0f : f; std::memcpy(&b, &n, 4); mixU64(b); };
                mixF(v2f->x); mixF(v2f->y);
            }
            else if (const auto* v3f = std::get_if<Vec3fValue>(&v)) {
                auto mixF = [&](float f) { std::uint32_t b = 0; const float n = (f == 0.0f) ? 0.0f : f; std::memcpy(&b, &n, 4); mixU64(b); };
                mixF(v3f->x); mixF(v3f->y); mixF(v3f->z);
            }
            else if (QuatfValue qf{}; v.tryGetQuatf(qf)) {
                auto mixF = [&](float f) { std::uint32_t b = 0; const float n = (f == 0.0f) ? 0.0f : f; std::memcpy(&b, &n, 4); mixU64(b); };
                mixF(qf.x); mixF(qf.y); mixF(qf.z); mixF(qf.w);
            }
            else if (const auto* d = std::get_if<double>(&v)) {
                std::uint64_t bits = 0;
                const double norm = (*d == 0.0) ? 0.0 : *d;
                std::memcpy(&bits, &norm, sizeof(bits));
                mixU64(bits);
            } else if (const auto* fl = std::get_if<FloatValue>(&v)) {
                std::uint32_t bits = 0;
                const float norm = (fl->value == 0.0f) ? 0.0f : fl->value;
                std::memcpy(&bits, &norm, sizeof(bits));
                mixU64(bits);
            } else if (const auto* s = std::get_if<std::string>(&v)) {
                mixStr(*s);
            } else if (const auto* o = std::get_if<ObjectRef>(&v)) {
                if (o->kind == ObjectKind::String && heap) {
                    if (const auto sv = heap->stringView(*o)) {
                        mixStr(*sv);
                        return;
                    }
                }
                mixU64((static_cast<std::uint64_t>(o->generation) << 32u) | o->slot);
            } else if (const auto* st = std::get_if<StructValue>(&v)) {
                mixU64(st->typeId);
                for (std::size_t idx = 0; idx < st->fieldCount(); ++idx) {
                    if (const auto fld = structValueFieldAt(*st, idx)) {
                        self(self, *fld);
                    }
                }
            }
        };
        hashVal(hashVal, arguments[0]);
        result = static_cast<std::int64_t>(static_cast<std::int32_t>(h & 0x7fffffffULL));
        return true;
    }

    if (op == "string_concat") {
        if (arguments.size() != 2) {
            errorMessage = "string_concat requires 2 arguments";
            return false;
        }
        return makeIntrinsicStringResult(
            detail::extractIntrinsicString(arguments[0], heap) +
                detail::extractIntrinsicString(arguments[1], heap),
            heap,
            result,
            errorMessage);
    }

    constexpr std::string_view toStringPrefix = "to_string::";
    if (op.rfind(toStringPrefix, 0) == 0) {
        if (arguments.empty()) {
            errorMessage = "to_string requires 1 argument";
            return false;
        }
        const auto& val = arguments[0];
        if (const auto* d = std::get_if<double>(&val)) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%g", *d);
            return makeIntrinsicStringResult(buf, heap, result, errorMessage);
        }
        if (const auto* f = std::get_if<FloatValue>(&val)) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(f->value));
            return makeIntrinsicStringResult(buf, heap, result, errorMessage);
        }
        return makeIntrinsicStringResult(
            valueToString(val, heap), heap, result, errorMessage);
    }

    if (op.rfind("array_", 0) == 0) {
        if (arguments.empty() || std::holds_alternative<NullArray>(arguments[0])) {
            errorMessage = "cannot invoke array method on null array";
            return false;
        }
        const auto* arrRef = std::get_if<ObjectRef>(&arguments[0]);
        const auto lenOpt = (arrRef && heap) ? heap->arrayLength(*arrRef) : std::nullopt;
        if (!lenOpt) {
            errorMessage = "invalid array reference";
            return false;
        }
        const std::size_t len = *lenOpt;

        if (op.rfind("array_index_of::", 0) == 0) {
            if (arguments.size() < 2) {
                errorMessage = "array_index_of requires 2 arguments";
                return false;
            }
            std::int64_t found = -1;
            for (std::size_t i = 0; i < len; ++i) {
                const auto elem = heap->arrayGet(*arrRef, i);
                if (elem && detail::intrinsicValuesEqual(*elem, arguments[1], heap)) {
                    found = static_cast<std::int64_t>(i);
                    break;
                }
            }
            result = found;
            return true;
        }
        if (op.rfind("array_last_index_of::", 0) == 0) {
            if (arguments.size() < 2) {
                errorMessage = "array_last_index_of requires 2 arguments";
                return false;
            }
            std::int64_t found = -1;
            for (std::size_t i = len; i > 0; --i) {
                const auto elem = heap->arrayGet(*arrRef, i - 1);
                if (elem && detail::intrinsicValuesEqual(*elem, arguments[1], heap)) {
                    found = static_cast<std::int64_t>(i - 1);
                    break;
                }
            }
            result = found;
            return true;
        }
        if (op.rfind("array_contains::", 0) == 0) {
            if (arguments.size() < 2) {
                errorMessage = "array_contains requires 2 arguments";
                return false;
            }
            bool found = false;
            for (std::size_t i = 0; i < len; ++i) {
                const auto elem = heap->arrayGet(*arrRef, i);
                if (elem && detail::intrinsicValuesEqual(*elem, arguments[1], heap)) {
                    found = true;
                    break;
                }
            }
            result = found;
            return true;
        }
        if (op.rfind("array_reverse::", 0) == 0) {
            for (std::size_t i = 0; i < len / 2; ++i) {
                const std::size_t j = len - 1 - i;
                const auto leftVal = heap->arrayGet(*arrRef, i);
                const auto rightVal = heap->arrayGet(*arrRef, j);
                if (leftVal && rightVal) {
                    RuntimeError setErr;
                    (void)heap->arraySet(*arrRef, i, *rightVal, &setErr);
                    (void)heap->arraySet(*arrRef, j, *leftVal, &setErr);
                }
            }
            result = std::monostate{};
            return true;
        }
        if (op.rfind("array_sort::", 0) == 0 || op.rfind("array_sort_range::", 0) == 0) {
            std::size_t sortLen = len;
            if (op.rfind("array_sort_range::", 0) == 0 && arguments.size() >= 2) {
                const auto rawCount = std::holds_alternative<std::int64_t>(arguments[1])
                    ? std::get<std::int64_t>(arguments[1])
                    : 0;
                const auto clampedCount = std::clamp<std::int64_t>(
                    rawCount, 0, static_cast<std::int64_t>(len));
                sortLen = static_cast<std::size_t>(clampedCount);
            }
            if (sortLen > 1) {
                std::vector<Value> elems;
                elems.reserve(sortLen);
                for (std::size_t i = 0; i < sortLen; ++i) {
                    elems.push_back(heap->arrayGet(*arrRef, i).value_or(Value{}));
                }
                std::stable_sort(
                    elems.begin(),
                    elems.end(),
                    [heap](const Value& a, const Value& b) {
                        return detail::intrinsicValueLess(a, b, heap);
                    });
                RuntimeError setErr;
                for (std::size_t i = 0; i < sortLen; ++i) {
                    (void)heap->arraySet(*arrRef, i, elems[i], &setErr);
                }
            }
            result = std::monostate{};
            return true;
        }
    }

    if (!arguments.empty() && std::holds_alternative<NullString>(arguments[0])) {
        errorMessage = "cannot invoke string method on null string";
        return false;
    }

    if (op == "string_length") {
        result = static_cast<std::int64_t>(
            detail::extractIntrinsicString(arguments[0], heap).size());
        return true;
    }
    if (op == "string_is_empty") {
        result = detail::extractIntrinsicString(arguments[0], heap).empty();
        return true;
    }
    if (op == "string_contains") {
        const auto s = detail::extractIntrinsicString(arguments[0], heap);
        const auto sub = detail::extractIntrinsicString(arguments[1], heap);
        result = (s.find(sub) != std::string::npos);
        return true;
    }
    if (op == "string_starts_with") {
        const auto s = detail::extractIntrinsicString(arguments[0], heap);
        const auto p = detail::extractIntrinsicString(arguments[1], heap);
        result = (s.size() >= p.size() && s.compare(0, p.size(), p) == 0);
        return true;
    }
    if (op == "string_ends_with") {
        const auto s = detail::extractIntrinsicString(arguments[0], heap);
        const auto sfx = detail::extractIntrinsicString(arguments[1], heap);
        result = (s.size() >= sfx.size() &&
                  s.compare(s.size() - sfx.size(), sfx.size(), sfx) == 0);
        return true;
    }
    if (op == "string_index_of") {
        const auto s = detail::extractIntrinsicString(arguments[0], heap);
        const auto sub = detail::extractIntrinsicString(arguments[1], heap);
        const auto pos = s.find(sub);
        result = (pos == std::string::npos)
            ? std::int64_t{-1}
            : static_cast<std::int64_t>(pos);
        return true;
    }
    if (op == "string_last_index_of") {
        const auto s = detail::extractIntrinsicString(arguments[0], heap);
        const auto sub = detail::extractIntrinsicString(arguments[1], heap);
        const auto pos = s.rfind(sub);
        result = (pos == std::string::npos)
            ? std::int64_t{-1}
            : static_cast<std::int64_t>(pos);
        return true;
    }
    if (op == "string_substring_1") {
        const auto s = detail::extractIntrinsicString(arguments[0], heap);
        const auto rawStart = std::holds_alternative<std::int64_t>(arguments[1])
            ? std::get<std::int64_t>(arguments[1])
            : 0;
        const auto clampedStart = std::clamp<std::int64_t>(
            rawStart, 0, static_cast<std::int64_t>(s.size()));
        return makeIntrinsicStringResult(
            s.substr(static_cast<std::size_t>(clampedStart)),
            heap,
            result,
            errorMessage);
    }
    if (op == "string_substring_2") {
        const auto s = detail::extractIntrinsicString(arguments[0], heap);
        const auto rawStart = std::holds_alternative<std::int64_t>(arguments[1])
            ? std::get<std::int64_t>(arguments[1])
            : 0;
        const auto rawLen = std::holds_alternative<std::int64_t>(arguments[2])
            ? std::get<std::int64_t>(arguments[2])
            : 0;
        const auto clampedStart = std::clamp<std::int64_t>(
            rawStart, 0, static_cast<std::int64_t>(s.size()));
        const auto clampedLen = std::clamp<std::int64_t>(
            rawLen, 0, static_cast<std::int64_t>(s.size()) - clampedStart);
        return makeIntrinsicStringResult(
            s.substr(
                static_cast<std::size_t>(clampedStart),
                static_cast<std::size_t>(clampedLen)),
            heap,
            result,
            errorMessage);
    }
    if (op == "string_trim") {
        const auto s = detail::extractIntrinsicString(arguments[0], heap);
        const auto isSpace = [](unsigned char c) {
            return c == ' ' || c == '\t' || c == '\r' ||
                   c == '\n' || c == '\v' || c == '\f';
        };
        std::size_t first = 0;
        while (first < s.size() && isSpace(static_cast<unsigned char>(s[first]))) {
            ++first;
        }
        std::size_t last = s.size();
        while (last > first && isSpace(static_cast<unsigned char>(s[last - 1]))) {
            --last;
        }
        return makeIntrinsicStringResult(
            s.substr(first, last - first), heap, result, errorMessage);
    }
    if (op == "string_trim_start") {
        const auto s = detail::extractIntrinsicString(arguments[0], heap);
        const auto isSpace = [](unsigned char c) {
            return c == ' ' || c == '\t' || c == '\r' ||
                   c == '\n' || c == '\v' || c == '\f';
        };
        std::size_t first = 0;
        while (first < s.size() && isSpace(static_cast<unsigned char>(s[first]))) {
            ++first;
        }
        return makeIntrinsicStringResult(
            s.substr(first), heap, result, errorMessage);
    }
    if (op == "string_trim_end") {
        const auto s = detail::extractIntrinsicString(arguments[0], heap);
        const auto isSpace = [](unsigned char c) {
            return c == ' ' || c == '\t' || c == '\r' ||
                   c == '\n' || c == '\v' || c == '\f';
        };
        std::size_t last = s.size();
        while (last > 0 && isSpace(static_cast<unsigned char>(s[last - 1]))) {
            --last;
        }
        return makeIntrinsicStringResult(
            s.substr(0, last), heap, result, errorMessage);
    }
    if (op == "string_split") {
        if (!heap) {
            errorMessage = "string_split requires a managed heap";
            return false;
        }
        const auto s = detail::extractIntrinsicString(arguments[0], heap);
        const auto sep = detail::extractIntrinsicString(arguments[1], heap);
        std::vector<std::string> parts;
        if (sep.empty()) {
            parts.push_back(s);
        } else {
            std::size_t start = 0;
            while (true) {
                const auto pos = s.find(sep, start);
                if (pos == std::string::npos) {
                    parts.push_back(s.substr(start));
                    break;
                }
                parts.push_back(s.substr(start, pos - start));
                start = pos + sep.size();
            }
        }
        const auto arrTypeId = semantic::stableTypeId(
            semantic::arrayTypeName(semantic::PrimitiveType::String, ""));
        RuntimeError allocErr;
        const auto defaultStrRef = heap->allocateString("", &allocErr);
        if (!defaultStrRef) {
            errorMessage = allocErr.message.empty()
                ? "out of memory allocating string array"
                : allocErr.message;
            return false;
        }
        const auto arrRef = heap->allocateTypedArray(
            arrTypeId,
            semantic::PrimitiveType::String,
            0,
            parts.size(),
            *defaultStrRef,
            &allocErr);
        if (!arrRef) {
            errorMessage = allocErr.message.empty()
                ? "out of memory allocating string array"
                : allocErr.message;
            return false;
        }
        for (std::size_t i = 0; i < parts.size(); ++i) {
            const auto elemStrRef = heap->allocateString(std::move(parts[i]), &allocErr);
            if (!elemStrRef || !heap->arraySet(*arrRef, i, *elemStrRef, &allocErr)) {
                errorMessage = allocErr.message.empty()
                    ? "out of memory populating string array"
                    : allocErr.message;
                return false;
            }
        }
        result = *arrRef;
        return true;
    }
    if (op == "string_replace") {
        auto s = detail::extractIntrinsicString(arguments[0], heap);
        const auto from = detail::extractIntrinsicString(arguments[1], heap);
        const auto to = detail::extractIntrinsicString(arguments[2], heap);
        if (!from.empty()) {
            std::size_t pos = 0;
            while ((pos = s.find(from, pos)) != std::string::npos) {
                s.replace(pos, from.size(), to);
                pos += to.size();
            }
        }
        return makeIntrinsicStringResult(std::move(s), heap, result, errorMessage);
    }
    if (op == "string_to_upper") {
        auto s = detail::extractIntrinsicString(arguments[0], heap);
        for (auto& ch : s) {
            if (ch >= 'a' && ch <= 'z') {
                ch = static_cast<char>(ch - 'a' + 'A');
            }
        }
        return makeIntrinsicStringResult(std::move(s), heap, result, errorMessage);
    }
    if (op == "string_to_lower") {
        auto s = detail::extractIntrinsicString(arguments[0], heap);
        for (auto& ch : s) {
            if (ch >= 'A' && ch <= 'Z') {
                ch = static_cast<char>(ch - 'A' + 'a');
            }
        }
        return makeIntrinsicStringResult(std::move(s), heap, result, errorMessage);
    }

    errorMessage = "unknown runtime intrinsic '" + std::string(name) + "'";
    return false;
}


bool hasAnyPinnedPoolValues() noexcept {
    return g_anyPoolHasPinnedSlots.load(std::memory_order_acquire);
}

namespace detail {

namespace {
struct ArenaScalarOverflowTable {
    std::mutex mutex;
    std::vector<InlineStructArenaEntry> entries;
    std::unordered_map<
        InlineStructArenaEntry,
        std::uint32_t,
        InlineStructEntryHash,
        InlineStructEntryEq> map;

    std::uint32_t intern(const InlineStructArenaEntry& entry) noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = map.find(entry);
        if (found != map.end()) return found->second;
        const std::uint32_t id = 0x80000000u | static_cast<std::uint32_t>(entries.size() + 1u);
        entries.push_back(entry);
        map.emplace(entry, id);
        return id;
    }

    const InlineStructArenaEntry& get(std::uint32_t id) noexcept {
        const std::size_t off = static_cast<std::size_t>((id & 0x7FFFFFFFu) - 1u);
        std::lock_guard<std::mutex> lock(mutex);
        return off < entries.size() ? entries[off] : g_zeroInlineEntry;
    }
};

ArenaScalarOverflowTable& arenaScalarOverflowTable() noexcept {
    static ArenaScalarOverflowTable table;
    return table;
}

StringValue hostInternStringBridge(std::string_view text) {
    return runtimeStringPool().intern(text);
}
StringValue hostStringByIdBridge(std::uint32_t id) noexcept {
    return runtimeStringPool().getById(id);
}
std::uint32_t hostInternInlineArenaBridge(const InlineStructArenaEntry& entry) noexcept {
    return inlineStructPool().intern(entry);
}
const InlineStructArenaEntry* hostLookupInlineArenaBridge(std::uint32_t idx) noexcept {
    return &inlineStructPool().get(idx);
}
std::uint32_t hostRegisterHeapStructBridge(const void* storeSharedPtrAddr) {
    const auto* sp = static_cast<const std::shared_ptr<const StructStorage>*>(storeSharedPtrAddr);
    return sp ? heapStructPool().registerStorage(*sp) : 0u;
}
const void* hostLookupHeapStructBridge(std::uint32_t id) noexcept {
    return heapStructPool().get(id);
}
} // namespace

std::uint32_t internArenaQuat(QuatValue value) noexcept {
    if (const auto* bridge = g_externalPoolBridge.load(std::memory_order_relaxed)) {
        if (bridge->internArenaQuatFn) return bridge->internArenaQuatFn(value);
    }
    InlineStructArenaEntry entry{};
    entry.nestedFieldMask = 0x8001u;
    entry.slots[0].kind = InlineStructSlot::Kind::Quat;
    entry.slots[0].lo = static_cast<std::uint32_t>(value.x);
    entry.slots[0].hi = static_cast<std::uint32_t>(value.y);
    entry.slots[1].kind = InlineStructSlot::Kind::Quat;
    entry.slots[1].lo = static_cast<std::uint32_t>(value.z);
    entry.slots[1].hi = static_cast<std::uint32_t>(value.w);
    const std::uint32_t id = inlineStructPool().intern(entry);
    return id != 0u ? id : arenaScalarOverflowTable().intern(entry);
}

QuatValue lookupArenaQuat(std::uint32_t arenaIdx) noexcept {
    if (const auto* bridge = g_externalPoolBridge.load(std::memory_order_relaxed)) {
        if (bridge->lookupArenaQuatFn) return bridge->lookupArenaQuatFn(arenaIdx);
    }
    const auto& entry = (arenaIdx & 0x80000000u) != 0u
        ? arenaScalarOverflowTable().get(arenaIdx)
        : inlineStructPool().get(arenaIdx);
    return QuatValue{
        static_cast<std::int32_t>(entry.slots[0].lo),
        static_cast<std::int32_t>(entry.slots[0].hi),
        static_cast<std::int32_t>(entry.slots[1].lo),
        static_cast<std::int32_t>(entry.slots[1].hi),
    };
}

std::uint32_t internArenaQuatf(QuatfValue value) noexcept {
    InlineStructArenaEntry entry{};
    entry.nestedFieldMask = 0x8003u;
    entry.slots[0].kind = InlineStructSlot::Kind::Quatf;
    std::memcpy(&entry.slots[0].lo, &value.x, sizeof(float));
    std::memcpy(&entry.slots[0].hi, &value.y, sizeof(float));
    entry.slots[1].kind = InlineStructSlot::Kind::Quatf;
    std::memcpy(&entry.slots[1].lo, &value.z, sizeof(float));
    std::memcpy(&entry.slots[1].hi, &value.w, sizeof(float));
    if (const auto* bridge = g_externalPoolBridge.load(std::memory_order_relaxed)) {
        if (bridge->internInlineArenaFn) {
            const std::uint32_t bid = bridge->internInlineArenaFn(entry);
            if (bid != 0u) return bid;
        }
    }
    const std::uint32_t id = inlineStructPool().intern(entry);
    return id != 0u ? id : arenaScalarOverflowTable().intern(entry);
}

QuatfValue lookupArenaQuatf(std::uint32_t arenaIdx) noexcept {
    if ((arenaIdx & 0x80000000u) == 0u) {
        if (const auto* bridge = g_externalPoolBridge.load(std::memory_order_relaxed)) {
            if (bridge->lookupInlineArenaFn) {
                if (const auto* p = bridge->lookupInlineArenaFn(arenaIdx)) {
                    QuatfValue out{};
                    std::memcpy(&out.x, &p->slots[0].lo, sizeof(float));
                    std::memcpy(&out.y, &p->slots[0].hi, sizeof(float));
                    std::memcpy(&out.z, &p->slots[1].lo, sizeof(float));
                    std::memcpy(&out.w, &p->slots[1].hi, sizeof(float));
                    return out;
                }
            }
        }
    }
    const auto& entry = (arenaIdx & 0x80000000u) != 0u
        ? arenaScalarOverflowTable().get(arenaIdx)
        : inlineStructPool().get(arenaIdx);
    QuatfValue out{};
    std::memcpy(&out.x, &entry.slots[0].lo, sizeof(float));
    std::memcpy(&out.y, &entry.slots[0].hi, sizeof(float));
    std::memcpy(&out.z, &entry.slots[1].lo, sizeof(float));
    std::memcpy(&out.w, &entry.slots[1].hi, sizeof(float));
    return out;
}

std::uint32_t internArenaEnum(EnumValue value) noexcept {
    if (const auto* bridge = g_externalPoolBridge.load(std::memory_order_relaxed)) {
        if (bridge->internArenaEnumFn) return bridge->internArenaEnumFn(value);
    }
    InlineStructArenaEntry entry{};
    entry.nestedTypeId = value.typeId;
    entry.nestedFieldMask = 0x8002u;
    entry.slots[0].kind = InlineStructSlot::Kind::Enum;
    entry.slots[0].setBits(static_cast<std::uint64_t>(value.value));
    const std::uint32_t id = inlineStructPool().intern(entry);
    return id != 0u ? id : arenaScalarOverflowTable().intern(entry);
}

EnumValue lookupArenaEnum(std::uint32_t arenaIdx) noexcept {
    if (const auto* bridge = g_externalPoolBridge.load(std::memory_order_relaxed)) {
        if (bridge->lookupArenaEnumFn) return bridge->lookupArenaEnumFn(arenaIdx);
    }
    const auto& entry = (arenaIdx & 0x80000000u) != 0u
        ? arenaScalarOverflowTable().get(arenaIdx)
        : inlineStructPool().get(arenaIdx);
    return EnumValue{
        entry.nestedTypeId,
        static_cast<std::int64_t>(entry.slots[0].bits()),
    };
}

std::uint32_t internNativeHandleTypeId(semantic::SymbolId typeId) noexcept {
    if (const auto* bridge = g_externalPoolBridge.load(std::memory_order_relaxed)) {
        if (bridge->internNativeHandleTypeIdFn) return bridge->internNativeHandleTypeIdFn(typeId);
    }
    return nativeHandleTypeTable().intern(typeId);
}

semantic::SymbolId lookupNativeHandleTypeId(std::uint32_t typeIdx) noexcept {
    if (const auto* bridge = g_externalPoolBridge.load(std::memory_order_relaxed)) {
        if (bridge->lookupNativeHandleTypeIdFn) return bridge->lookupNativeHandleTypeIdFn(typeIdx);
    }
    return nativeHandleTypeTable().lookup(typeIdx);
}

const SharedValuePoolBridge* hostValuePoolBridge() noexcept {
    if (const auto* existing = g_externalPoolBridge.load(std::memory_order_acquire)) {
        return existing;
    }
    static const SharedValuePoolBridge kHostBridge{
        1u,
        &internArenaQuat,
        &lookupArenaQuat,
        &internArenaEnum,
        &lookupArenaEnum,
        &internNativeHandleTypeId,
        &lookupNativeHandleTypeId,
        &hostInternStringBridge,
        &hostStringByIdBridge,
        &hostInternInlineArenaBridge,
        &hostLookupInlineArenaBridge,
        &hostRegisterHeapStructBridge,
        &hostLookupHeapStructBridge,
    };
    return &kHostBridge;
}

void bindHostValuePoolBridge(const void* bridgePtr) noexcept {
    const auto* incoming = static_cast<const SharedValuePoolBridge*>(bridgePtr);
    if (!incoming || incoming->version != 1u || incoming == hostValuePoolBridge()) {
        return;
    }
    g_externalPoolBridge.store(incoming, std::memory_order_release);
}

} // namespace detail

void pinValueInPools(const Value& value) noexcept {
    if (!value.isTraceable()) return;
    switch (value.rawTag()) {
    case ValueTag::String: {
        const auto& sv = value.asStringUnchecked();
        if (sv.id != 0u) runtimeStringPool().pinById(sv.id);
        return;
    }
    case ValueTag::Struct: {
        const auto& st = value.asStructUnchecked();
        if (st.isInline) {
            const std::uint32_t idx = st.storage.inlineArenaIndex();
            if (idx != 0u) inlineStructPool().pinById(idx);
        } else if (st.storage.isStoragePtr()) {
            heapStructPool().pinById(st.storage.raw_);
        }
        return;
    }
    case ValueTag::EnumArena:
    case ValueTag::QuatArena:
    case ValueTag::QuatfArena: {
        const std::uint32_t idx = value.arenaPoolIndex();
        if (idx != 0u) inlineStructPool().pinById(idx);
        return;
    }
    default:
        return;
    }
}

void beginValuePoolGcMark() noexcept {
    g_poolGcMarkInProgress.store(true, std::memory_order_release);
    g_anyPoolHasPinnedSlots.store(false, std::memory_order_relaxed);
    constexpr std::uint8_t clearMask =
        static_cast<std::uint8_t>(~(kSlotFlagGcMarked | kSlotFlagStackRoot));
    auto& strPool = runtimeStringPool();
    for (std::uint32_t s = 0u; s < kPoolShardCount; ++s) {
        auto& shard = strPool.shards_[s];
        if (!shard.mayHavePinnedOrMarked.load(std::memory_order_acquire)) continue;
        if (!shard.pages[0].load(std::memory_order_relaxed)) continue;
        std::lock_guard<std::mutex> lock(shard.mutex);
        for (std::uint32_t slot = 0u; slot < shard.highWaterMark; ++slot) {
            if (auto* page = shard.pages[slot >> RuntimeStringPool::kPageShift].load(std::memory_order_relaxed)) {
                page->flags[slot & RuntimeStringPool::kPageMask].fetch_and(
                    clearMask, std::memory_order_relaxed);
            }
        }
    }
    auto& inlPool = inlineStructPool();
    for (std::uint32_t s = 0u; s < kPoolShardCount; ++s) {
        auto& shard = inlPool.shards_[s];
        if (!shard.mayHavePinnedOrMarked.load(std::memory_order_acquire)) continue;
        if (!shard.pages[0].load(std::memory_order_relaxed)) continue;
        std::lock_guard<std::mutex> lock(shard.mutex);
        for (std::uint32_t slot = 0u; slot < shard.highWaterMark; ++slot) {
            if (auto* page = shard.pages[slot >> InlineStructPool::kPageShift].load(std::memory_order_relaxed)) {
                page->flags[slot & InlineStructPool::kPageMask].fetch_and(
                    clearMask, std::memory_order_relaxed);
            }
        }
    }
    auto& hpPool = heapStructPool();
    for (std::uint32_t s = 0u; s < kPoolShardCount; ++s) {
        auto& shard = hpPool.shards_[s];
        if (!shard.mayHavePinnedOrMarked.load(std::memory_order_acquire)) continue;
        if (!shard.pages[0].load(std::memory_order_relaxed)) continue;
        std::lock_guard<std::mutex> lock(shard.mutex);
        for (std::uint32_t slot = 0u; slot < shard.highWaterMark; ++slot) {
            if (auto* page = shard.pages[slot >> HeapStructStoragePool::kPageShift].load(std::memory_order_relaxed)) {
                page->flags[slot & HeapStructStoragePool::kPageMask].fetch_and(
                    clearMask, std::memory_order_relaxed);
            }
        }
    }
}

void markValueInPools(const Value& value) noexcept {
    if (!value.isTraceable()) return;
    switch (value.rawTag()) {
    case ValueTag::String: {
        const auto& sv = value.asStringUnchecked();
        if (sv.id != 0u) runtimeStringPool().markById(sv.id);
        return;
    }
    case ValueTag::Struct: {
        const auto& st = value.asStructUnchecked();
        if (st.isInline) {
            const std::uint32_t idx = st.storage.inlineArenaIndex();
            if (idx != 0u) inlineStructPool().markById(idx);
        } else if (st.storage.isStoragePtr()) {
            heapStructPool().markById(st.storage.raw_);
        }
        return;
    }
    case ValueTag::EnumArena:
    case ValueTag::QuatArena:
    case ValueTag::QuatfArena: {
        const std::uint32_t idx = value.arenaPoolIndex();
        if (idx != 0u) inlineStructPool().markById(idx);
        return;
    }
    default:
        return;
    }
}

void markStackRootValueInPools(const Value& value) noexcept {
    if (!value.isTraceable()) return;
    switch (value.rawTag()) {
    case ValueTag::String: {
        const auto& sv = value.asStringUnchecked();
        if (sv.id != 0u) runtimeStringPool().markStackRootById(sv.id);
        return;
    }
    case ValueTag::Struct: {
        const auto& st = value.asStructUnchecked();
        if (st.isInline) {
            const std::uint32_t idx = st.storage.inlineArenaIndex();
            if (idx != 0u) inlineStructPool().markStackRootById(idx);
        } else if (st.storage.isStoragePtr()) {
            heapStructPool().markStackRootById(st.storage.raw_);
        }
        return;
    }
    case ValueTag::EnumArena:
    case ValueTag::QuatArena:
    case ValueTag::QuatfArena: {
        const std::uint32_t idx = value.arenaPoolIndex();
        if (idx != 0u) inlineStructPool().markStackRootById(idx);
        return;
    }
    default:
        return;
    }
}

std::size_t finishValuePoolGcSweep() noexcept {
    std::size_t reclaimed = 0u;
    bool anyPoolPinned = false;
    constexpr std::uint8_t clearMarkBits =
        static_cast<std::uint8_t>(~(kSlotFlagGcMarked | kSlotFlagStackRoot));
    constexpr std::uint8_t clearPinAndMarkBits =
        static_cast<std::uint8_t>(~(kSlotFlagGcMarked | kSlotFlagStackRoot | kSlotFlagPinned));

    auto& strPool = runtimeStringPool();
    for (std::uint32_t s = 0u; s < kPoolShardCount; ++s) {
        auto& shard = strPool.shards_[s];
        if (!shard.mayHavePinnedOrMarked.load(std::memory_order_acquire)) continue;
        if (!shard.pages[0].load(std::memory_order_relaxed)) continue;
        std::lock_guard<std::mutex> lock(shard.mutex);
        shard.mayHavePinnedOrMarked.store(false, std::memory_order_relaxed);
        bool anyReclaimed = false;
        bool shardStillPinned = false;
        for (std::uint32_t slot = 0u; slot < shard.highWaterMark; ++slot) {
            auto* page = shard.pages[slot >> RuntimeStringPool::kPageShift].load(std::memory_order_relaxed);
            if (!page) continue;
            const std::uint32_t offset = slot & RuntimeStringPool::kPageMask;
            const std::uint8_t f = page->flags[offset].load(std::memory_order_relaxed);
            if ((f & kSlotFlagOccupied) == 0u) continue;
            if ((f & kSlotFlagGcMarked) != 0u) {
                page->flags[offset].fetch_and(clearMarkBits, std::memory_order_relaxed);
                shardStillPinned = true;
                continue;
            }
            if ((f & kSlotFlagStackRoot) != 0u) {
                page->flags[offset].fetch_and(clearPinAndMarkBits, std::memory_order_relaxed);
                page->flags[offset].fetch_or(kSlotFlagClock, std::memory_order_relaxed);
                continue;
            }
            if ((f & kSlotFlagPinned) == 0u) {
                continue;
            }
            const std::uint8_t bank = page->activeBank[offset].load(std::memory_order_relaxed) & 1u;
            if (!page->strings[offset][bank].empty()) {
                shard.indexByView.erase(std::string_view(page->strings[offset][bank]));
            }
            page->flags[offset].store(0u, std::memory_order_release);
            shard.freeList.push_back(slot);
            shard.gcReclaimed.fetch_add(1u, std::memory_order_relaxed);
            anyReclaimed = true;
            ++reclaimed;
        }
        if (shardStillPinned) {
            shard.mayHavePinnedOrMarked.store(true, std::memory_order_release);
            anyPoolPinned = true;
        }
        if (anyReclaimed) {
            shard.epoch.fetch_add(1u, std::memory_order_release);
        }
    }

    auto& inlPool = inlineStructPool();
    for (std::uint32_t s = 0u; s < kPoolShardCount; ++s) {
        auto& shard = inlPool.shards_[s];
        if (!shard.mayHavePinnedOrMarked.load(std::memory_order_acquire)) continue;
        if (!shard.pages[0].load(std::memory_order_relaxed)) continue;
        std::lock_guard<std::mutex> lock(shard.mutex);
        shard.mayHavePinnedOrMarked.store(false, std::memory_order_relaxed);
        bool anyReclaimed = false;
        bool shardStillPinned = false;
        for (std::uint32_t slot = 0u; slot < shard.highWaterMark; ++slot) {
            auto* page = shard.pages[slot >> InlineStructPool::kPageShift].load(std::memory_order_relaxed);
            if (!page) continue;
            const std::uint32_t offset = slot & InlineStructPool::kPageMask;
            const std::uint8_t f = page->flags[offset].load(std::memory_order_relaxed);
            if ((f & kSlotFlagOccupied) == 0u) continue;
            if ((f & kSlotFlagGcMarked) != 0u) {
                page->flags[offset].fetch_and(clearMarkBits, std::memory_order_relaxed);
                shardStillPinned = true;
                continue;
            }
            if ((f & kSlotFlagStackRoot) != 0u) {
                page->flags[offset].fetch_and(clearPinAndMarkBits, std::memory_order_relaxed);
                page->flags[offset].fetch_or(kSlotFlagClock, std::memory_order_relaxed);
                continue;
            }
            if ((f & kSlotFlagPinned) == 0u) {
                continue;
            }
            const std::uint8_t bank = page->activeBank[offset].load(std::memory_order_relaxed) & 1u;
            shard.dedupMap.erase(page->entries[offset][bank]);
            page->flags[offset].store(0u, std::memory_order_release);
            shard.freeList.push_back(slot);
            shard.gcReclaimed.fetch_add(1u, std::memory_order_relaxed);
            anyReclaimed = true;
            ++reclaimed;
        }
        if (shardStillPinned) {
            shard.mayHavePinnedOrMarked.store(true, std::memory_order_release);
            anyPoolPinned = true;
        }
        if (anyReclaimed) {
            shard.epoch.fetch_add(1u, std::memory_order_release);
        }
    }

    auto& hpPool = heapStructPool();
    for (std::uint32_t s = 0u; s < kPoolShardCount; ++s) {
        auto& shard = hpPool.shards_[s];
        if (!shard.mayHavePinnedOrMarked.load(std::memory_order_acquire)) continue;
        if (!shard.pages[0].load(std::memory_order_relaxed)) continue;
        std::lock_guard<std::mutex> lock(shard.mutex);
        shard.mayHavePinnedOrMarked.store(false, std::memory_order_relaxed);
        bool anyReclaimed = false;
        bool shardStillPinned = false;
        for (std::uint32_t slot = 0u; slot < shard.highWaterMark; ++slot) {
            auto* page = shard.pages[slot >> HeapStructStoragePool::kPageShift].load(std::memory_order_relaxed);
            if (!page) continue;
            const std::uint32_t offset = slot & HeapStructStoragePool::kPageMask;
            const std::uint8_t f = page->flags[offset].load(std::memory_order_relaxed);
            if ((f & kSlotFlagOccupied) == 0u) continue;
            if ((f & kSlotFlagGcMarked) != 0u) {
                page->flags[offset].fetch_and(clearMarkBits, std::memory_order_relaxed);
                shardStillPinned = true;
                continue;
            }
            if ((f & kSlotFlagStackRoot) != 0u) {
                page->flags[offset].fetch_and(clearPinAndMarkBits, std::memory_order_relaxed);
                page->flags[offset].fetch_or(kSlotFlagClock, std::memory_order_relaxed);
                continue;
            }
            if ((f & kSlotFlagPinned) == 0u) {
                continue;
            }
            hpPool.removeDedupEntryLocked(shard, page->hashes[offset], slot);
            page->hashes[offset] = 0u;
            page->flags[offset].store(0u, std::memory_order_release);
            shard.freeList.push_back(slot);
            shard.gcReclaimed.fetch_add(1u, std::memory_order_relaxed);
            anyReclaimed = true;
            ++reclaimed;
        }
        if (shardStillPinned) {
            shard.mayHavePinnedOrMarked.store(true, std::memory_order_release);
            anyPoolPinned = true;
        }
        if (anyReclaimed) {
            shard.epoch.fetch_add(1u, std::memory_order_release);
        }
    }

    if (anyPoolPinned) {
        g_anyPoolHasPinnedSlots.store(true, std::memory_order_release);
    }
    g_poolGcMarkInProgress.store(false, std::memory_order_release);
    return reclaimed;
}

RuntimeValuePoolStatistics getRuntimeValuePoolStatistics() noexcept {
    RuntimeValuePoolStatistics stats{};

    auto& strPool = runtimeStringPool();
    for (std::uint32_t s = 0u; s < kPoolShardCount; ++s) {
        auto& shard = strPool.shards_[s];
        std::lock_guard<std::mutex> lock(shard.mutex);
        stats.stringActiveSlots += shard.indexByView.size();
        stats.stringFreeListSlots += shard.freeList.size();
        stats.stringAllocatedPages += shard.ownedPages.size();
        stats.stringClockEvictions += shard.clockEvictions.load(std::memory_order_relaxed);
        stats.stringGcReclaimed += shard.gcReclaimed.load(std::memory_order_relaxed);
        for (std::uint32_t slot = 0u; slot < shard.highWaterMark; ++slot) {
            if (auto* page = shard.pages[slot >> RuntimeStringPool::kPageShift].load(std::memory_order_relaxed)) {
                const std::uint8_t f = page->flags[slot & RuntimeStringPool::kPageMask].load(std::memory_order_relaxed);
                if ((f & kSlotFlagOccupied) != 0u && (f & kSlotFlagPinned) != 0u) {
                    ++stats.stringPinnedSlots;
                }
            }
        }
    }

    auto& inlPool = inlineStructPool();
    for (std::uint32_t s = 0u; s < kPoolShardCount; ++s) {
        auto& shard = inlPool.shards_[s];
        std::lock_guard<std::mutex> lock(shard.mutex);
        stats.inlineStructActiveSlots += shard.dedupMap.size();
        stats.inlineStructFreeListSlots += shard.freeList.size();
        stats.inlineStructAllocatedPages += shard.ownedPages.size();
        stats.inlineStructClockEvictions += shard.clockEvictions.load(std::memory_order_relaxed);
        stats.inlineStructGcReclaimed += shard.gcReclaimed.load(std::memory_order_relaxed);
        for (std::uint32_t slot = 0u; slot < shard.highWaterMark; ++slot) {
            if (auto* page = shard.pages[slot >> InlineStructPool::kPageShift].load(std::memory_order_relaxed)) {
                const std::uint8_t f = page->flags[slot & InlineStructPool::kPageMask].load(std::memory_order_relaxed);
                if ((f & kSlotFlagOccupied) != 0u && (f & kSlotFlagPinned) != 0u) {
                    ++stats.inlineStructPinnedSlots;
                }
            }
        }
    }

    auto& hpPool = heapStructPool();
    for (std::uint32_t s = 0u; s < kPoolShardCount; ++s) {
        auto& shard = hpPool.shards_[s];
        std::lock_guard<std::mutex> lock(shard.mutex);
        stats.heapStructActiveSlots += shard.dedupByHash.size();
        stats.heapStructFreeListSlots += shard.freeList.size();
        stats.heapStructAllocatedPages += shard.ownedPages.size();
        stats.heapStructClockEvictions += shard.clockEvictions.load(std::memory_order_relaxed);
        stats.heapStructGcReclaimed += shard.gcReclaimed.load(std::memory_order_relaxed);
        for (std::uint32_t slot = 0u; slot < shard.highWaterMark; ++slot) {
            if (auto* page = shard.pages[slot >> HeapStructStoragePool::kPageShift].load(std::memory_order_relaxed)) {
                const std::uint8_t f = page->flags[slot & HeapStructStoragePool::kPageMask].load(std::memory_order_relaxed);
                if ((f & kSlotFlagOccupied) != 0u && (f & kSlotFlagPinned) != 0u) {
                    ++stats.heapStructPinnedSlots;
                }
            }
        }
    }

    return stats;
}

} // namespace realscript::runtime
