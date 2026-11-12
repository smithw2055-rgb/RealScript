#include "realscript/runtime/Runtime.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace realscript::runtime {
namespace {

void setHeapError(RuntimeError* error, ErrorCode code, std::string message) {
    if (!error) return;
    error->code = code;
    error->message = std::move(message);
}

std::size_t estimateValueStorage(std::size_t count) {
    if (count > std::numeric_limits<std::size_t>::max() / sizeof(Value)) {
        return std::numeric_limits<std::size_t>::max();
    }
    return count * sizeof(Value);
}

std::uint64_t nextIdentity(std::atomic<std::uint64_t>& next) noexcept {
    auto value = next.fetch_add(1, std::memory_order_relaxed);
    if (value == 0) value = next.fetch_add(1, std::memory_order_relaxed);
    return value;
}

std::uint64_t nextHeapId() noexcept {
    static std::atomic<std::uint64_t> next{1};
    return nextIdentity(next);
}

std::uint64_t nextRegistryId() noexcept {
    static std::atomic<std::uint64_t> next{1};
    return nextIdentity(next);
}

const char* objectKindName(ObjectKind kind) noexcept {
    switch (kind) {
    case ObjectKind::String: return "string";
    case ObjectKind::Array: return "array";
    case ObjectKind::Record: return "record";
    }
    return "unknown";
}

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
struct alignas(64) HeapSyncState {
    static constexpr std::uint32_t kInvalidShardSlot = 0xFFFFFFFFu;

    std::mutex mutex;
    void* ownerImpl = nullptr;
    bool (*markPoolFn)(void*) = nullptr;
    std::uint64_t heapIdValue = 0;
    std::uint32_t shardIndex = 0;
    std::atomic<std::uint32_t> shardSlot{kInvalidShardSlot};
    std::atomic<std::uint32_t> stringShardSlot{kInvalidShardSlot};

    HeapSyncState(void* impl, bool (*markFn)(void*), std::uint64_t heapId) noexcept
        : ownerImpl(impl),
          markPoolFn(markFn),
          heapIdValue(heapId),
          shardIndex(static_cast<std::uint32_t>(heapId & 63u)) {}
};

constexpr std::size_t kHeapPoolMarkerShardCount = 64;

struct alignas(64) HeapPoolMarkerShard {
    std::mutex mutex;
    std::vector<std::shared_ptr<HeapSyncState>> entries;
    std::atomic<std::size_t> activeCount{0};
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

std::array<HeapPoolMarkerShard, kHeapPoolMarkerShardCount>& heapPoolMarkerShards() noexcept {
    static auto* shards = new std::array<HeapPoolMarkerShard, kHeapPoolMarkerShardCount>();
    return *shards;
}

std::array<HeapPoolMarkerShard, kHeapPoolMarkerShardCount>& liveStringHeapShards() noexcept {
    static auto* shards = new std::array<HeapPoolMarkerShard, kHeapPoolMarkerShardCount>();
    return *shards;
}

void registerHeapSyncInPoolShard(const std::shared_ptr<HeapSyncState>& sync) {
    if (!sync) return;
    if (sync->shardSlot.load(std::memory_order_relaxed) != HeapSyncState::kInvalidShardSlot) {
        return;
    }
    auto& shard = heapPoolMarkerShards()[sync->shardIndex];
    std::lock_guard<std::mutex> shardLock(shard.mutex);
    if (sync->shardSlot.load(std::memory_order_relaxed) != HeapSyncState::kInvalidShardSlot) {
        return;
    }
    const auto slot = static_cast<std::uint32_t>(shard.entries.size());
    shard.entries.push_back(sync);
    sync->shardSlot.store(slot, std::memory_order_release);
    shard.activeCount.store(shard.entries.size(), std::memory_order_release);
}

void unregisterHeapSyncFromPoolShard(HeapSyncState& sync) noexcept {
    if (sync.shardSlot.load(std::memory_order_relaxed) == HeapSyncState::kInvalidShardSlot) {
        return;
    }
    auto& shard = heapPoolMarkerShards()[sync.shardIndex];
    std::lock_guard<std::mutex> shardLock(shard.mutex);
    const std::uint32_t slot = sync.shardSlot.load(std::memory_order_relaxed);
    if (slot < shard.entries.size() && shard.entries[slot].get() == &sync) {
        auto last = std::move(shard.entries.back());
        shard.entries.pop_back();
        if (slot < shard.entries.size()) {
            last->shardSlot.store(slot, std::memory_order_relaxed);
            shard.entries[slot] = std::move(last);
        }
        shard.activeCount.store(shard.entries.size(), std::memory_order_release);
    }
    sync.shardSlot.store(HeapSyncState::kInvalidShardSlot, std::memory_order_release);
}

void registerHeapSyncInStringShard(const std::shared_ptr<HeapSyncState>& sync) {
    if (!sync) return;
    if (sync->stringShardSlot.load(std::memory_order_relaxed) != HeapSyncState::kInvalidShardSlot) {
        return;
    }
    auto& shard = liveStringHeapShards()[sync->shardIndex];
    std::lock_guard<std::mutex> shardLock(shard.mutex);
    if (sync->stringShardSlot.load(std::memory_order_relaxed) != HeapSyncState::kInvalidShardSlot) {
        return;
    }
    const auto slot = static_cast<std::uint32_t>(shard.entries.size());
    shard.entries.push_back(sync);
    sync->stringShardSlot.store(slot, std::memory_order_release);
    shard.activeCount.store(shard.entries.size(), std::memory_order_release);
}

void unregisterHeapSyncFromStringShard(HeapSyncState& sync) noexcept {
    if (sync.stringShardSlot.load(std::memory_order_relaxed) == HeapSyncState::kInvalidShardSlot) {
        return;
    }
    auto& shard = liveStringHeapShards()[sync.shardIndex];
    std::lock_guard<std::mutex> shardLock(shard.mutex);
    const std::uint32_t slot = sync.stringShardSlot.load(std::memory_order_relaxed);
    if (slot < shard.entries.size() && shard.entries[slot].get() == &sync) {
        auto last = std::move(shard.entries.back());
        shard.entries.pop_back();
        if (slot < shard.entries.size()) {
            last->stringShardSlot.store(slot, std::memory_order_relaxed);
            shard.entries[slot] = std::move(last);
        }
        shard.activeCount.store(shard.entries.size(), std::memory_order_release);
    }
    sync.stringShardSlot.store(HeapSyncState::kInvalidShardSlot, std::memory_order_release);
}

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
struct alignas(64) ValuePoolSweepCoordinator {
    std::mutex sweepMutex;
    std::atomic<std::uint64_t> requestedEpoch{0};
    std::atomic<std::uint64_t> completedEpoch{0};
};

struct alignas(64) MutatorSweepCoordinator {
    static constexpr std::uint64_t kSweepActiveBit = std::uint64_t{1} << 63u;
    static constexpr std::uint64_t kActiveMutatorMask = ~kSweepActiveBit;

    std::atomic<std::uint64_t> state{0};
    std::atomic<bool> deferredSweepPending{false};
    std::mutex waitMutex;
    std::condition_variable cv;
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

ValuePoolSweepCoordinator& valuePoolSweepCoordinator() noexcept {
    static auto* coord = new ValuePoolSweepCoordinator();
    return *coord;
}

MutatorSweepCoordinator& mutatorSweepCoordinator() noexcept {
    static auto* mc = new MutatorSweepCoordinator();
    return *mc;
}

std::uint32_t& tlsMutatorDepthRef() noexcept {
    thread_local std::uint32_t depth = 0;
    return depth;
}

std::uint32_t& tlsHeapSyncLockDepthRef() noexcept {
    thread_local std::uint32_t depth = 0;
    return depth;
}

std::size_t reclaimPoolValuesInternal(
    const ShadowStack* activeShadowStack,
    bool waitIfBusy) noexcept;

class HeapSyncMutexGuard {
public:
    explicit HeapSyncMutexGuard(std::mutex& m, bool flushDeferredOnUnlock = true)
        : mutex_(m), flushDeferred_(flushDeferredOnUnlock) {
        mutex_.lock();
        ++tlsHeapSyncLockDepthRef();
    }

    ~HeapSyncMutexGuard() noexcept {
        mutex_.unlock();
        auto& depth = tlsHeapSyncLockDepthRef();
        if (depth > 0u && --depth == 0u && flushDeferred_ && tlsMutatorDepthRef() == 0u) {
            if (mutatorSweepCoordinator().deferredSweepPending.exchange(
                    false, std::memory_order_acq_rel)) {
                (void)reclaimPoolValuesInternal(nullptr, /*waitIfBusy=*/false);
            }
        }
    }

    HeapSyncMutexGuard(const HeapSyncMutexGuard&) = delete;
    HeapSyncMutexGuard& operator=(const HeapSyncMutexGuard&) = delete;

private:
    std::mutex& mutex_;
    bool flushDeferred_ = true;
};

std::uint64_t nextShadowStackId() noexcept {
    static std::atomic<std::uint64_t> next{1};
    return nextIdentity(next);
}

struct ThreadShadowStackShard {
    std::mutex mutex;
    std::vector<ShadowStack*> stacks;
    bool threadExited = false;

    ThreadShadowStackShard() {
        stacks.reserve(16);
    }
};

std::mutex& shadowStackShardsMutex() noexcept {
    static auto* m = new std::mutex();
    return *m;
}

std::vector<std::shared_ptr<ThreadShadowStackShard>>& shadowStackShards() noexcept {
    static auto* shards = new std::vector<std::shared_ptr<ThreadShadowStackShard>>();
    return *shards;
}

ThreadShadowStackShard* currentThreadShadowStackShard() noexcept {
    struct TlsShardHolder {
        std::shared_ptr<ThreadShadowStackShard> shard;
        TlsShardHolder()
            : shard(std::make_shared<ThreadShadowStackShard>()) {
            std::lock_guard<std::mutex> lock(shadowStackShardsMutex());
            shadowStackShards().push_back(shard);
        }
        ~TlsShardHolder() noexcept {
            if (shard) {
                std::lock_guard<std::mutex> lock(shard->mutex);
                shard->threadExited = true;
            }
        }
    };
    thread_local TlsShardHolder tlsHolder;
    return tlsHolder.shard.get();
}

} // namespace

void registerActiveShadowStack(ShadowStack* stack) noexcept {
    if (!stack || stack->registryShard_ != nullptr) return;
    auto* shard = currentThreadShadowStackShard();
    std::lock_guard<std::mutex> lock(shard->mutex);
    stack->registryShard_ = shard;
    stack->registrySlot_ = static_cast<std::uint32_t>(shard->stacks.size());
    shard->stacks.push_back(stack);
}

void unregisterActiveShadowStack(ShadowStack* stack) noexcept {
    if (!stack || !stack->registryShard_) return;
    auto* shard = static_cast<ThreadShadowStackShard*>(stack->registryShard_);
    std::lock_guard<std::mutex> lock(shard->mutex);
    const std::uint32_t slot = stack->registrySlot_;
    if (slot < shard->stacks.size() && shard->stacks[slot] == stack) {
        ShadowStack* last = shard->stacks.back();
        shard->stacks[slot] = last;
        last->registrySlot_ = slot;
        shard->stacks.pop_back();
    }
    stack->registryShard_ = nullptr;
    stack->registrySlot_ = ShadowStack::InvalidRegistrySlot;
}

void transferActiveShadowStack(ShadowStack* from, ShadowStack* to) noexcept {
    if (!from || !to || from == to) return;
    if (from->registryShard_ != nullptr) {
        auto* shard = static_cast<ThreadShadowStackShard*>(from->registryShard_);
        std::lock_guard<std::mutex> lock(shard->mutex);
        to->inlineFrames_ = from->inlineFrames_;
        to->overflowFrames_ = std::move(from->overflowFrames_);
        to->frameCount_ = from->frameCount_;
        from->inlineFrames_ = {};
        from->frameCount_ = 0;

        const std::uint32_t slot = from->registrySlot_;
        if (slot < shard->stacks.size() && shard->stacks[slot] == from) {
            shard->stacks[slot] = to;
            to->registryShard_ = shard;
            to->registrySlot_ = slot;
        }
        from->registryShard_ = nullptr;
        from->registrySlot_ = ShadowStack::InvalidRegistrySlot;
        return;
    }
    to->inlineFrames_ = from->inlineFrames_;
    to->overflowFrames_ = std::move(from->overflowFrames_);
    to->frameCount_ = from->frameCount_;
    from->inlineFrames_ = {};
    from->frameCount_ = 0;
    if (to->frameCount_ > 0) {
        registerActiveShadowStack(to);
    }
}

ShadowStack::ShadowStack() noexcept
    : instanceId_(nextShadowStackId()) {}

ShadowStack::~ShadowStack() noexcept {
    if (registryShard_ != nullptr) {
        unregisterActiveShadowStack(this);
    }
}

ShadowStack::ShadowStack(const ShadowStack& other)
    : instanceId_(nextShadowStackId()) {
    if (other.registryShard_ != nullptr) {
        auto* otherShard = static_cast<ThreadShadowStackShard*>(other.registryShard_);
        std::lock_guard<std::mutex> lock(otherShard->mutex);
        inlineFrames_ = other.inlineFrames_;
        overflowFrames_ = other.overflowFrames_;
        frameCount_ = other.frameCount_;
    } else {
        inlineFrames_ = other.inlineFrames_;
        overflowFrames_ = other.overflowFrames_;
        frameCount_ = other.frameCount_;
    }
    if (frameCount_ > 0) {
        registerActiveShadowStack(this);
    }
}

ShadowStack& ShadowStack::operator=(const ShadowStack& other) {
    if (this == &other) return *this;
    std::array<FrameRoots, InlineFrameCapacity> inlineCopy{};
    std::vector<FrameRoots> overflowCopy;
    std::size_t countCopy = 0;
    if (other.registryShard_ != nullptr && other.registryShard_ != registryShard_) {
        auto* otherShard = static_cast<ThreadShadowStackShard*>(other.registryShard_);
        std::lock_guard<std::mutex> lock(otherShard->mutex);
        inlineCopy = other.inlineFrames_;
        overflowCopy = other.overflowFrames_;
        countCopy = other.frameCount_;
    } else if (other.registryShard_ == nullptr) {
        inlineCopy = other.inlineFrames_;
        overflowCopy = other.overflowFrames_;
        countCopy = other.frameCount_;
    }
    const bool wasRegistered = (registryShard_ != nullptr);
    if (wasRegistered) {
        auto* shard = static_cast<ThreadShadowStackShard*>(registryShard_);
        {
            std::lock_guard<std::mutex> lock(shard->mutex);
            if (other.registryShard_ == registryShard_) {
                inlineFrames_ = other.inlineFrames_;
                overflowFrames_ = other.overflowFrames_;
                frameCount_ = other.frameCount_;
            } else {
                inlineFrames_ = inlineCopy;
                overflowFrames_ = std::move(overflowCopy);
                frameCount_ = countCopy;
            }
            instanceId_ = nextShadowStackId();
        }
        if (frameCount_ == 0) {
            unregisterActiveShadowStack(this);
        }
        return *this;
    }
    inlineFrames_ = inlineCopy;
    overflowFrames_ = std::move(overflowCopy);
    frameCount_ = countCopy;
    instanceId_ = nextShadowStackId();
    if (frameCount_ > 0) {
        registerActiveShadowStack(this);
    }
    return *this;
}

ShadowStack::ShadowStack(ShadowStack&& other) noexcept
    : instanceId_(nextShadowStackId()) {
    transferActiveShadowStack(&other, this);
    other.instanceId_ = nextShadowStackId();
}

ShadowStack& ShadowStack::operator=(ShadowStack&& other) noexcept {
    if (this == &other) return *this;
    if (registryShard_ != nullptr) {
        unregisterActiveShadowStack(this);
    }
    instanceId_ = nextShadowStackId();
    transferActiveShadowStack(&other, this);
    other.instanceId_ = nextShadowStackId();
    return *this;
}

void ShadowStack::pushFrame(
    const std::vector<Value>* arguments,
    const std::vector<Value>* locals,
    const std::vector<Value>* registers,
    const std::size_t* activeRegisterLimit) {
    if (registryShard_ != nullptr) {
        auto* shard = static_cast<ThreadShadowStackShard*>(registryShard_);
        std::lock_guard<std::mutex> lock(shard->mutex);
        const std::size_t idx = frameCount_;
        if (idx < InlineFrameCapacity) {
            inlineFrames_[idx] = {arguments, locals, registers, activeRegisterLimit};
        } else {
            overflowFrames_.push_back({arguments, locals, registers, activeRegisterLimit});
        }
        frameCount_ = idx + 1;
        return;
    }
    const std::size_t idx = frameCount_;
    if (idx < InlineFrameCapacity) {
        inlineFrames_[idx] = {arguments, locals, registers, activeRegisterLimit};
    } else {
        overflowFrames_.push_back({arguments, locals, registers, activeRegisterLimit});
    }
    frameCount_ = idx + 1;
    if (idx == 0) {
        registerActiveShadowStack(this);
    }
}

void ShadowStack::popFrame() noexcept {
    if (frameCount_ == 0) return;
    if (registryShard_ != nullptr) {
        auto* shard = static_cast<ThreadShadowStackShard*>(registryShard_);
        bool becameEmpty = false;
        {
            std::lock_guard<std::mutex> lock(shard->mutex);
            if (frameCount_ > 0) {
                const std::size_t newCount = frameCount_ - 1;
                if (newCount >= InlineFrameCapacity) {
                    overflowFrames_.pop_back();
                } else {
                    inlineFrames_[newCount] = {};
                }
                frameCount_ = newCount;
                becameEmpty = (newCount == 0);
            }
        }
        if (becameEmpty) {
            unregisterActiveShadowStack(this);
        }
        return;
    }
    const std::size_t newCount = frameCount_ - 1;
    if (newCount >= InlineFrameCapacity) {
        overflowFrames_.pop_back();
    } else {
        inlineFrames_[newCount] = {};
    }
    frameCount_ = newCount;
}

void ShadowStack::visitRoots(const std::function<void(const Value&)>& visitor) const {
    visitRoots<const std::function<void(const Value&)>&>(visitor);
}

enum class SlotStorage : std::uint8_t {
    Empty      = 0,
    SsoString  = 1,
    HeapString = 2,
    SlabValues = 3,
    HeapValues = 4,
    RawColumn  = 5,
};

namespace GcFlag {
    constexpr std::uint8_t Marked          = 1u << 0;
    constexpr std::uint8_t InNursery       = 1u << 1;
    constexpr std::uint8_t InRememberedSet = 1u << 2;
    constexpr std::uint8_t HasCardTable    = 1u << 3;
    constexpr std::uint8_t CustomSize      = 1u << 4;
    constexpr std::uint8_t IsInterned      = 1u << 5;
} // namespace GcFlag

struct TypeLayoutEntry {
    semantic::SymbolId typeId = 0;
    semantic::SymbolId elementTypeId = 0;
    semantic::PrimitiveType elementType = semantic::PrimitiveType::Error;
    bool mayContainReferences = false;
    std::vector<std::size_t> referenceFields;
};

struct ArrayLayoutKey {
    semantic::SymbolId arrayTypeId = 0;
    semantic::PrimitiveType elementType = semantic::PrimitiveType::Error;
    semantic::SymbolId elementTypeId = 0;

    friend constexpr bool operator==(const ArrayLayoutKey& a, const ArrayLayoutKey& b) noexcept {
        return a.arrayTypeId == b.arrayTypeId &&
            a.elementType == b.elementType &&
            a.elementTypeId == b.elementTypeId;
    }
};

struct ArrayLayoutKeyHash {
    std::size_t operator()(const ArrayLayoutKey& k) const noexcept {
        std::size_t h = static_cast<std::size_t>(k.arrayTypeId);
        h ^= static_cast<std::size_t>(k.elementType) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= static_cast<std::size_t>(k.elementTypeId) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
struct alignas(32) CompactSlot {
    static constexpr std::uint32_t kInvalidHandle = 0xFFFFFFFFu;
    static constexpr std::size_t kMaxSsoBytes = 23;
    static constexpr std::size_t kInlineValueCapacity = 4;

    std::uint32_t generation = 1;
    std::uint8_t kindAndStorage = 0;
    std::uint8_t gcFlags = 0;
    std::uint16_t auxMeta = 0;

    union Payload {
        std::array<char, 24> ssoChars;
        struct Structured {
            std::uint32_t layoutId;
            std::uint32_t length;
            Value* data;
            std::uint32_t handle;
            std::uint32_t cardHandle;
        } s;

        constexpr Payload() noexcept
            : s{0, 0, nullptr, kInvalidHandle, kInvalidHandle} {}
    } payload;

    [[nodiscard]] constexpr SlotStorage storage() const noexcept {
        return static_cast<SlotStorage>(kindAndStorage & 0x0Fu);
    }
    [[nodiscard]] constexpr ObjectKind kind() const noexcept {
        return static_cast<ObjectKind>((kindAndStorage >> 4) & 0x0Fu);
    }
    [[nodiscard]] constexpr bool occupied() const noexcept {
        return storage() != SlotStorage::Empty;
    }
    constexpr void setKindAndStorage(ObjectKind k, SlotStorage st) noexcept {
        kindAndStorage = static_cast<std::uint8_t>(
            (static_cast<std::uint8_t>(k) << 4) | (static_cast<std::uint8_t>(st) & 0x0Fu));
    }
    [[nodiscard]] constexpr bool hasFlag(std::uint8_t f) const noexcept {
        return (gcFlags & f) != 0;
    }
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
static_assert(sizeof(CompactSlot) == 32, "CompactSlot must be exactly 32 bytes (2 per cache line)");
static_assert(alignof(CompactSlot) == 32, "CompactSlot must be 32-byte aligned");

class PagedValueSlabPool {
public:
    static constexpr std::size_t kBlocksPerPage = 1024;
    static constexpr std::uint16_t kZeroClass = 0xFFFFu;

    [[nodiscard]] static constexpr std::uint16_t sizeToClass(std::size_t count) noexcept {
        return count <= 1 ? 0 : (count <= 2 ? 1 : 2);
    }
    [[nodiscard]] static constexpr std::size_t classStride(std::uint16_t cls) noexcept {
        return std::size_t{1} << cls;
    }

    Value* allocate(std::size_t count, std::uint16_t& outClass, std::uint32_t& outHandle) {
        if (count == 0) {
            outClass = kZeroClass;
            outHandle = CompactSlot::kInvalidHandle;
            return &emptyValue_;
        }
        const std::uint16_t cls = sizeToClass(count);
        outClass = cls;
        auto& pool = classes_[cls];
        if (!pool.freeList.empty()) {
            outHandle = pool.freeList.back();
            pool.freeList.pop_back();
            return resolve(cls, outHandle);
        }
        const std::size_t stride = classStride(cls);
        if (pool.cursor >= kBlocksPerPage) {
            pool.pages.push_back(std::make_unique<Value[]>(kBlocksPerPage * stride));
            pool.cursor = 0;
        }
        const auto pageIdx = static_cast<std::uint32_t>(pool.pages.size() - 1);
        const auto blockIdx = static_cast<std::uint32_t>(pool.cursor++);
        outHandle = (pageIdx << 10) | blockIdx;
        return &pool.pages[pageIdx][blockIdx * stride];
    }

    void deallocate(std::uint16_t cls, std::uint32_t handle, std::uint32_t usedCount) noexcept {
        (void)usedCount;
        if (cls == kZeroClass || handle == CompactSlot::kInvalidHandle) return;
        classes_[cls].freeList.push_back(handle);
    }

    [[nodiscard]] Value* resolve(std::uint16_t cls, std::uint32_t handle) const noexcept {
        const std::size_t stride = classStride(cls);
        return &classes_[cls].pages[handle >> 10][(handle & 0x3FFu) * stride];
    }

    void clear() noexcept {
        for (auto& c : classes_) {
            c.pages.clear();
            c.freeList.clear();
            c.cursor = kBlocksPerPage;
        }
    }

private:
    struct ClassArena {
        std::vector<std::unique_ptr<Value[]>> pages;
        std::vector<std::uint32_t> freeList;
        std::size_t cursor = kBlocksPerPage;
    };
    std::array<ClassArena, 3> classes_;
    Value emptyValue_{};
};

class LargeValueBufferPool {
public:
    std::uint32_t acquireFill(std::size_t length, const Value& initVal, Value*& outData) {
        const std::uint32_t id = acquireId(length);
        buffers_[id].assign(length, initVal);
        outData = buffers_[id].data();
        return id;
    }

    std::uint32_t acquireCopy(const Value* src, std::size_t length, Value*& outData) {
        const std::uint32_t id = acquireId(length);
        buffers_[id].assign(src, src + length);
        outData = buffers_[id].data();
        return id;
    }

    std::uint32_t acquireMove(std::vector<Value>&& vals, Value*& outData) {
        const std::uint32_t id = acquireId(vals.size());
        if (buffers_[id].capacity() >= vals.size()) {
            buffers_[id].assign(
                std::make_move_iterator(vals.begin()),
                std::make_move_iterator(vals.end()));
        } else {
            buffers_[id] = std::move(vals);
        }
        outData = buffers_[id].data();
        return id;
    }

    void release(std::uint32_t id) noexcept {
        if (id >= buffers_.size()) return;
        auto& buf = buffers_[id];
        const std::size_t cap = buf.capacity();
        buf.clear();
        if (cap > 4096) {
            std::vector<Value>().swap(buf);
            smallFreeIds_.push_back(id);
        } else if (cap <= 64) {
            smallFreeIds_.push_back(id);
        } else {
            largeFreeIds_.push_back(id);
        }
    }

    void clear() noexcept {
        buffers_.clear();
        smallFreeIds_.clear();
        largeFreeIds_.clear();
    }

private:
    std::uint32_t acquireId(std::size_t needed) {
        auto& primary = (needed <= 64) ? smallFreeIds_ : largeFreeIds_;
        if (!primary.empty()) {
            const std::uint32_t id = primary.back();
            primary.pop_back();
            if (needed <= 64 && buffers_[id].capacity() > 256) {
                std::vector<Value>().swap(buffers_[id]);
            }
            return id;
        }
        auto& secondary = (needed <= 64) ? largeFreeIds_ : smallFreeIds_;
        if (!secondary.empty()) {
            const std::uint32_t id = secondary.back();
            secondary.pop_back();
            if (needed <= 64 && buffers_[id].capacity() > 256) {
                std::vector<Value>().swap(buffers_[id]);
            }
            return id;
        }
        const auto id = static_cast<std::uint32_t>(buffers_.size());
        buffers_.emplace_back();
        return id;
    }

    std::vector<std::vector<Value>> buffers_;
    std::vector<std::uint32_t> smallFreeIds_;
    std::vector<std::uint32_t> largeFreeIds_;
};

class HeapStringPool {
public:
    std::uint32_t acquire(std::string&& str) {
        if (!freeIds_.empty()) {
            const std::uint32_t id = freeIds_.back();
            freeIds_.pop_back();
            strings_[id] = std::move(str);
            return id;
        }
        const auto id = static_cast<std::uint32_t>(strings_.size());
        strings_.push_back(std::move(str));
        return id;
    }

    void release(std::uint32_t id) noexcept {
        if (id >= strings_.size()) return;
        auto& s = strings_[id];
        s.clear();
        if (s.capacity() > 4096) {
            std::string().swap(s);
        }
        freeIds_.push_back(id);
    }

    [[nodiscard]] const std::string& get(std::uint32_t id) const noexcept {
        return strings_[id];
    }

    void clear() noexcept {
        strings_.clear();
        freeIds_.clear();
    }

private:
    std::vector<std::string> strings_;
    std::vector<std::uint32_t> freeIds_;
};

class RawColumnPool {
public:
    std::uint32_t acquire(NativeRawColumnView&& view) {
        if (!freeIds_.empty()) {
            const std::uint32_t id = freeIds_.back();
            freeIds_.pop_back();
            views_[id] = std::move(view);
            return id;
        }
        const auto id = static_cast<std::uint32_t>(views_.size());
        views_.push_back(std::move(view));
        return id;
    }

    void release(std::uint32_t id) noexcept {
        if (id >= views_.size()) return;
        views_[id] = NativeRawColumnView{};
        freeIds_.push_back(id);
    }

    [[nodiscard]] NativeRawColumnView& get(std::uint32_t id) noexcept { return views_[id]; }
    [[nodiscard]] const NativeRawColumnView& get(std::uint32_t id) const noexcept { return views_[id]; }

    void clear() noexcept {
        views_.clear();
        freeIds_.clear();
    }

private:
    std::vector<NativeRawColumnView> views_;
    std::vector<std::uint32_t> freeIds_;
};

class CardTableArena {
public:
    struct CardSlice {
        std::uint64_t* words = nullptr;
        std::uint32_t wordCount = 0;

        [[nodiscard]] std::uint8_t* bytePtr() const noexcept {
            return reinterpret_cast<std::uint8_t*>(words);
        }
        void clearAll() noexcept {
            if (words && wordCount > 0) {
                std::memset(words, 0, static_cast<std::size_t>(wordCount) * sizeof(std::uint64_t));
            }
        }
        void markAllDirty() noexcept {
            if (words && wordCount > 0) {
                std::memset(words, 1, static_cast<std::size_t>(wordCount) * sizeof(std::uint64_t));
            }
        }
    };

    std::uint32_t allocate(std::size_t elementCount) {
        const std::size_t cardCount =
            (elementCount + ManagedHeap::kCardElements - 1) >> ManagedHeap::kCardElementShift;
        const auto wordCount = static_cast<std::uint32_t>((cardCount + 7) >> 3);
        auto& bucket = freeByWordCount_[wordCount];
        if (!bucket.empty()) {
            const std::uint32_t handle = bucket.back();
            bucket.pop_back();
            slices_[handle].clearAll();
            return handle;
        }
        std::uint64_t* mem = allocateAlignedWords(wordCount);
        std::memset(mem, 0, static_cast<std::size_t>(wordCount) * sizeof(std::uint64_t));
        const auto handle = static_cast<std::uint32_t>(slices_.size());
        slices_.push_back(CardSlice{mem, wordCount});
        return handle;
    }

    void deallocate(std::uint32_t handle) {
        if (handle >= slices_.size()) return;
        freeByWordCount_[slices_[handle].wordCount].push_back(handle);
    }

    [[nodiscard]] CardSlice& get(std::uint32_t handle) noexcept { return slices_[handle]; }
    [[nodiscard]] const CardSlice& get(std::uint32_t handle) const noexcept { return slices_[handle]; }

    void clear() noexcept {
        pages_.clear();
        oversizedPages_.clear();
        cursor_ = kWordsPerPage;
        slices_.clear();
        freeByWordCount_.clear();
    }

private:
    static constexpr std::size_t kWordsPerPage = 8192; // 64 KB per page

    std::uint64_t* allocateAlignedWords(std::uint32_t wordCount) {
        if (wordCount > kWordsPerPage) {
            oversizedPages_.push_back(std::make_unique<std::uint64_t[]>(wordCount));
            return oversizedPages_.back().get();
        }
        if (cursor_ + wordCount > kWordsPerPage) {
            pages_.push_back(std::make_unique<std::uint64_t[]>(kWordsPerPage));
            cursor_ = 0;
        }
        std::uint64_t* ptr = &pages_.back()[cursor_];
        cursor_ += wordCount;
        return ptr;
    }

    std::vector<std::unique_ptr<std::uint64_t[]>> pages_;
    std::vector<std::unique_ptr<std::uint64_t[]>> oversizedPages_;
    std::size_t cursor_ = kWordsPerPage;
    std::vector<CardSlice> slices_;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> freeByWordCount_;
};

struct ManagedHeap::Impl {
    struct LegacyHeader {
        ObjectKind kind = ObjectKind::Record;
        semantic::SymbolId typeId = 0;
        semantic::PrimitiveType elementType = semantic::PrimitiveType::Error;
        semantic::SymbolId elementTypeId = 0;
        bool marked = false;
        bool inNursery = true;
        bool inRememberedSet = false;
        std::size_t sizeBytes = 0;
    };

    struct LegacyHeapObjectLayout {
        LegacyHeader header;
        std::vector<Value> values;
        std::string stringPayload;
        std::shared_ptr<const std::vector<std::size_t>> referenceFields;
        std::optional<NativeRawColumnView> rawColumnView;
    };
    static constexpr std::size_t BaseObjectSizeBytes = sizeof(LegacyHeapObjectLayout);

    struct NativeEcsColumnTable {
        std::vector<std::int64_t> entityIds;
        std::vector<Value> values;
        bool mayContainReferences = false;
    };

    [[nodiscard]] static bool valueMayContainReferences(const Value& value) noexcept {
        if (!value.isTraceable()) return false;
        const auto t = value.rawTag();
        if (t == ValueTag::ObjectRef) return true;
        if (t == ValueTag::Struct) {
            const auto& s = value.asStructUnchecked();
            return !s.isInline && s.storage != nullptr;
        }
        return false;
    }

    [[nodiscard]] static constexpr bool elementTypeMayContainReferences(
        semantic::PrimitiveType elementType) noexcept {
        return elementType == semantic::PrimitiveType::Object ||
            elementType == semantic::PrimitiveType::Array ||
            elementType == semantic::PrimitiveType::String ||
            elementType == semantic::PrimitiveType::Struct ||
            elementType == semantic::PrimitiveType::Error;
    }

    [[nodiscard]] static bool isValuePoolBacked(const Value& value) noexcept {
        const auto t = value.rawTag();
        return t >= ValueTag::FirstTraceable && t != ValueTag::ObjectRef;
    }

    static bool markPoolCallback(void* rawImpl) {
        auto* self = static_cast<Impl*>(rawImpl);
        const bool foundAny = self->markAllReachablePoolValues();
        if (!foundAny) {
            self->mayContainPoolValues.store(false, std::memory_order_relaxed);
        }
        return foundAny;
    }

    explicit Impl(HeapConfig value)
        : config(value),
          heapId(nextHeapId()),
          syncState(std::make_shared<HeapSyncState>(this, &Impl::markPoolCallback, heapId)),
          nextCollectionThreshold(value.initialCollectionThresholdBytes) {
        typeLayouts.push_back(TypeLayoutEntry{}); // layoutId 0: default empty
        if (config.initialCollectionThresholdBytes == 0) {
            config.initialCollectionThresholdBytes = 1;
            nextCollectionThreshold = 1;
        }
        if (config.maximumHeapBytes < config.initialCollectionThresholdBytes) {
            config.maximumHeapBytes = config.initialCollectionThresholdBytes;
        }
    }

    ~Impl() {
        if (syncState) {
            {
                HeapSyncMutexGuard instanceLock(syncState->mutex, /*flushDeferredOnUnlock=*/false);
                syncState->ownerImpl = nullptr;
            }
            unregisterHeapSyncFromPoolShard(*syncState);
            unregisterHeapSyncFromStringShard(*syncState);
        }
    }

    void noteMaybePoolBackedUnlocked() {
        if (!mayContainPoolValues.load(std::memory_order_relaxed)) {
            mayContainPoolValues.store(true, std::memory_order_release);
            registerHeapSyncInPoolShard(syncState);
        }
    }

    void pinHeapValueIfNeededUnlocked(const Value& value) {
        if (isValuePoolBacked(value)) {
            noteMaybePoolBackedUnlocked();
            pinValueInPools(value);
        }
    }

    [[nodiscard]] static std::size_t slotValueCount(const CompactSlot& slot) noexcept {
        const auto st = slot.storage();
        if (st == SlotStorage::SlabValues || st == SlotStorage::HeapValues) {
            return slot.payload.s.length;
        }
        return 0;
    }

    [[nodiscard]] std::string_view slotStringView(const CompactSlot& slot) const noexcept {
        if (slot.storage() == SlotStorage::SsoString) {
            return std::string_view(slot.payload.ssoChars.data(), slot.auxMeta);
        }
        if (slot.storage() == SlotStorage::HeapString) {
            return std::string_view(stringPool.get(slot.payload.s.handle));
        }
        return {};
    }

    [[nodiscard]] std::size_t slotSizeBytes(std::uint32_t slotIndex, const CompactSlot& slot) const noexcept {
        if (slot.hasFlag(GcFlag::CustomSize)) {
            const auto it = customSlotSizes.find(slotIndex);
            if (it != customSlotSizes.end()) return it->second;
        }
        switch (slot.storage()) {
        case SlotStorage::SsoString:
            return BaseObjectSizeBytes + slot.auxMeta;
        case SlotStorage::HeapString:
            return BaseObjectSizeBytes + slot.payload.s.length;
        case SlotStorage::SlabValues:
        case SlotStorage::HeapValues:
            return BaseObjectSizeBytes + static_cast<std::size_t>(slot.payload.s.length) * sizeof(Value);
        case SlotStorage::RawColumn:
        case SlotStorage::Empty:
            return BaseObjectSizeBytes;
        }
        return BaseObjectSizeBytes;
    }

    void releaseSlotPayload(std::uint32_t slotIndex, CompactSlot& slot) noexcept {
        if (slot.hasFlag(GcFlag::HasCardTable) &&
            slot.payload.s.cardHandle != CompactSlot::kInvalidHandle) {
            cardArena.deallocate(slot.payload.s.cardHandle);
        }
        if (slot.hasFlag(GcFlag::CustomSize)) {
            customSlotSizes.erase(slotIndex);
        }
        switch (slot.storage()) {
        case SlotStorage::Empty:
        case SlotStorage::SsoString:
            break;
        case SlotStorage::HeapString:
            stringPool.release(slot.payload.s.handle);
            break;
        case SlotStorage::SlabValues:
            slabPool.deallocate(slot.auxMeta, slot.payload.s.handle, slot.payload.s.length);
            break;
        case SlotStorage::HeapValues:
            largeBuffers.release(slot.payload.s.handle);
            break;
        case SlotStorage::RawColumn:
            rawColumns.release(slot.payload.s.handle);
            break;
        }
        slot.kindAndStorage = 0;
        slot.gcFlags = 0;
        slot.auxMeta = 0;
        slot.payload = CompactSlot::Payload{};
    }

    bool markAllReachablePoolValues() const {
        bool foundAny = false;
        const auto markOne = [&](const Value& v) {
            if (isValuePoolBacked(v)) {
                foundAny = true;
                markValueInPools(v);
            }
        };
        for (const auto& entry : persistentRoots) markOne(entry.second);
        for (const auto& entry : staticFields) markOne(entry.second);
        for (const auto& [typeName, table] : nativeEcsTables) {
            (void)typeName;
            for (const auto& v : table.values) markOne(v);
        }
        for (const auto& slot : slots) {
            if (!slot.occupied()) continue;
            const auto count = slotValueCount(slot);
            if (count == 0) continue;
            const Value* data = slot.payload.s.data;
            for (std::size_t i = 0; i < count; ++i) {
                markOne(data[i]);
            }
        }
        return foundAny;
    }

    std::uint32_t internArrayLayout(
        semantic::SymbolId arrayTypeId,
        semantic::PrimitiveType elementType,
        semantic::SymbolId elementTypeId) {
        const ArrayLayoutKey key{arrayTypeId, elementType, elementTypeId};
        if (hasLastArrayLayout && key == lastArrayLayoutKey) {
            return lastArrayLayoutId;
        }
        const auto it = arrayLayoutByKey.find(key);
        if (it != arrayLayoutByKey.end()) {
            hasLastArrayLayout = true;
            lastArrayLayoutKey = key;
            lastArrayLayoutId = it->second;
            return it->second;
        }
        TypeLayoutEntry entry;
        entry.typeId = arrayTypeId;
        entry.elementType = elementType;
        entry.elementTypeId = elementTypeId;
        entry.mayContainReferences = elementTypeMayContainReferences(elementType);
        const auto id = static_cast<std::uint32_t>(typeLayouts.size());
        typeLayouts.push_back(std::move(entry));
        arrayLayoutByKey.emplace(key, id);
        hasLastArrayLayout = true;
        lastArrayLayoutKey = key;
        lastArrayLayoutId = id;
        return id;
    }

    std::uint32_t internTypedRecordLayout(
        semantic::SymbolId typeId,
        const std::vector<std::size_t>& refFields) {
        const auto it = sharedRecordLayoutByTypeId.find(typeId);
        if (it != sharedRecordLayoutByTypeId.end() &&
            typeLayouts[it->second].referenceFields == refFields) {
            lastRegisteredLayoutTypeId = typeId;
            lastRegisteredLayoutId = it->second;
            return it->second;
        }
        TypeLayoutEntry entry;
        entry.typeId = typeId;
        entry.mayContainReferences = !refFields.empty();
        entry.referenceFields = refFields;
        const auto id = static_cast<std::uint32_t>(typeLayouts.size());
        typeLayouts.push_back(std::move(entry));
        sharedRecordLayoutByTypeId[typeId] = id;
        lastRegisteredLayoutTypeId = typeId;
        lastRegisteredLayoutId = id;
        return id;
    }

    std::uint32_t internTemplateRecordLayout(
        semantic::SymbolId typeId,
        const std::shared_ptr<const std::vector<std::size_t>>& referenceFields) {
        if (typeId == lastRegisteredLayoutTypeId && lastRegisteredLayoutId != 0) {
            return lastRegisteredLayoutId;
        }
        const auto it = sharedRecordLayoutByTypeId.find(typeId);
        if (it != sharedRecordLayoutByTypeId.end()) {
            lastRegisteredLayoutTypeId = typeId;
            lastRegisteredLayoutId = it->second;
            return it->second;
        }
        TypeLayoutEntry entry;
        entry.typeId = typeId;
        if (referenceFields && !referenceFields->empty()) {
            entry.referenceFields = *referenceFields;
            entry.mayContainReferences = true;
        }
        const auto id = static_cast<std::uint32_t>(typeLayouts.size());
        typeLayouts.push_back(std::move(entry));
        sharedRecordLayoutByTypeId[typeId] = id;
        lastRegisteredLayoutTypeId = typeId;
        lastRegisteredLayoutId = id;
        return id;
    }

    std::uint32_t internUntypedAllRefRecordLayout(std::size_t fieldCount) {
        const auto it = untypedDenseRecordLayouts.find(fieldCount);
        if (it != untypedDenseRecordLayouts.end()) {
            return it->second;
        }
        TypeLayoutEntry entry;
        entry.typeId = 0;
        entry.mayContainReferences = fieldCount > 0;
        entry.referenceFields.resize(fieldCount);
        for (std::size_t i = 0; i < fieldCount; ++i) entry.referenceFields[i] = i;
        const auto id = static_cast<std::uint32_t>(typeLayouts.size());
        typeLayouts.push_back(std::move(entry));
        untypedDenseRecordLayouts.emplace(fieldCount, id);
        return id;
    }

    std::uint32_t internSnapshotUntypedRecordLayout(const std::vector<std::size_t>& refFields) {
        for (const auto existingId : untypedCustomRecordLayouts) {
            if (typeLayouts[existingId].referenceFields == refFields) {
                return existingId;
            }
        }
        TypeLayoutEntry entry;
        entry.typeId = 0;
        entry.mayContainReferences = !refFields.empty();
        entry.referenceFields = refFields;
        const auto id = static_cast<std::uint32_t>(typeLayouts.size());
        typeLayouts.push_back(std::move(entry));
        untypedCustomRecordLayouts.push_back(id);
        return id;
    }

    HeapConfig config;
    std::uint64_t heapId = 0;
    std::shared_ptr<HeapSyncState> syncState;
    std::atomic<bool> mayContainPoolValues{false};
    std::vector<CompactSlot> slots;
    std::vector<std::uint32_t> freeSlots;
    std::vector<ObjectRef> markStack;
    std::vector<ObjectRef> youngWorkQueue;
    std::vector<std::uint32_t> nurserySlots;
    std::vector<ObjectRef> rememberedSet;

    PagedValueSlabPool slabPool;
    LargeValueBufferPool largeBuffers;
    HeapStringPool stringPool;
    RawColumnPool rawColumns;
    CardTableArena cardArena;

    std::vector<TypeLayoutEntry> typeLayouts;
    std::unordered_map<semantic::SymbolId, std::uint32_t> sharedRecordLayoutByTypeId;
    std::unordered_map<ArrayLayoutKey, std::uint32_t, ArrayLayoutKeyHash> arrayLayoutByKey;
    std::unordered_map<std::size_t, std::uint32_t> untypedDenseRecordLayouts;
    std::vector<std::uint32_t> untypedCustomRecordLayouts;
    std::unordered_map<std::uint32_t, std::size_t> customSlotSizes;

    semantic::SymbolId lastRegisteredLayoutTypeId = 0;
    std::uint32_t lastRegisteredLayoutId = 0;
    bool hasLastArrayLayout = false;
    ArrayLayoutKey lastArrayLayoutKey{};
    std::uint32_t lastArrayLayoutId = 0;

    std::unordered_map<RootToken, Value> persistentRoots;
    std::unordered_map<semantic::SymbolId, Value> staticFields;
    std::unordered_map<std::string, NativeEcsColumnTable> nativeEcsTables;
    std::unordered_map<std::string, ObjectRef> internedStrings;
    RootToken nextRootToken = 1;
    GcPhase phase = GcPhase::Idle;
    bool requested = false;
    bool resumeSweep = false;
    bool wallClockCycleActive = false;
    bool hasSparseEcsStaticFields = false;
    std::uint64_t lastScannedShadowStackId = 0;
    std::size_t sweepIndex = 0;
    std::size_t sweepLimit = 0;
    std::size_t nextCollectionThreshold = 0;
    GcStatistics statistics;

    void reopenMarkFromSweep() noexcept {
        if (phase == GcPhase::Sweep) {
            phase = GcPhase::Mark;
            resumeSweep = true;
            lastScannedShadowStackId = 0;
        }
    }

    [[nodiscard]] CompactSlot* getSlot(ObjectRef reference) noexcept {
        if (!reference.valid() || reference.heapId != static_cast<std::uint32_t>(heapId) ||
            reference.slot >= slots.size()) {
            return nullptr;
        }
        auto& slot = slots[reference.slot];
        if (!slot.occupied() || slot.generation != reference.generation ||
            slot.kind() != reference.kind) {
            return nullptr;
        }
        return &slot;
    }

    [[nodiscard]] const CompactSlot* getSlot(ObjectRef reference) const noexcept {
        if (!reference.valid() || reference.heapId != static_cast<std::uint32_t>(heapId) ||
            reference.slot >= slots.size()) {
            return nullptr;
        }
        const auto& slot = slots[reference.slot];
        if (!slot.occupied() || slot.generation != reference.generation ||
            slot.kind() != reference.kind) {
            return nullptr;
        }
        return &slot;
    }

    [[nodiscard]] std::optional<std::uint32_t> acquireSlot(
        std::size_t sizeBytes,
        RuntimeError* error,
        bool* outReallocated = nullptr) {
        if (sizeBytes == std::numeric_limits<std::size_t>::max() ||
            sizeBytes > config.maximumHeapBytes ||
            statistics.liveBytes > config.maximumHeapBytes - sizeBytes) {
            setHeapError(error, ErrorCode::OutOfMemory,
                "managed heap allocation exceeds the configured heap limit");
            requested = true;
            return std::nullopt;
        }
        if (!freeSlots.empty()) {
            const auto slotIndex = freeSlots.back();
            freeSlots.pop_back();
            return slotIndex;
        }
        if (slots.size() >= static_cast<std::size_t>(ObjectRef::InvalidSlot)) {
            setHeapError(error, ErrorCode::OutOfMemory,
                "managed heap exhausted the object handle space");
            return std::nullopt;
        }
        if (slots.size() == slots.capacity()) {
            if (outReallocated) *outReallocated = true;
            if (slots.empty()) {
                slots.reserve(64);
                freeSlots.reserve(64);
                nurserySlots.reserve(64);
            }
        }
        const auto slotIndex = static_cast<std::uint32_t>(slots.size());
        slots.emplace_back();
        return slotIndex;
    }

    [[nodiscard]] ObjectRef finalizeSlotAllocation(
        std::uint32_t slotIndex,
        ObjectKind kind,
        SlotStorage storageKind,
        std::size_t sizeBytes) {
        auto& slot = slots[slotIndex];
        slot.setKindAndStorage(kind, storageKind);
        slot.gcFlags = GcFlag::InNursery | (phase != GcPhase::Idle ? GcFlag::Marked : 0u);
        nurserySlots.push_back(slotIndex);
        const ObjectRef reference{slotIndex, slot.generation, kind, heapId};

        ++statistics.objectsAllocated;
        statistics.bytesAllocated += sizeBytes;
        ++statistics.liveObjects;
        statistics.liveBytes += sizeBytes;
        statistics.peakLiveBytes = std::max(statistics.peakLiveBytes, statistics.liveBytes);
        if (statistics.liveBytes >= nextCollectionThreshold) requested = true;
        if (phase != GcPhase::Idle) {
            markStack.push_back(reference);
            reopenMarkFromSweep();
        }
        return reference;
    }

    [[nodiscard]] std::optional<ObjectRef> allocateStringSlot(
        std::string value,
        std::size_t sizeBytes,
        RuntimeError* error) {
        const auto slotIndex = acquireSlot(sizeBytes, error);
        if (!slotIndex) return std::nullopt;
        registerHeapSyncInStringShard(syncState);
        auto& slot = slots[*slotIndex];
        if (value.size() <= CompactSlot::kMaxSsoBytes) {
            slot.auxMeta = static_cast<std::uint16_t>(value.size());
            if (!value.empty()) {
                std::memcpy(slot.payload.ssoChars.data(), value.data(), value.size());
            }
            slot.payload.ssoChars[value.size()] = '\0';
            return finalizeSlotAllocation(*slotIndex, ObjectKind::String, SlotStorage::SsoString, sizeBytes);
        }
        const auto len = static_cast<std::uint32_t>(value.size());
        const std::uint32_t handle = stringPool.acquire(std::move(value));
        slot.auxMeta = 0;
        slot.payload.s = {0u, len, nullptr, handle, CompactSlot::kInvalidHandle};
        return finalizeSlotAllocation(*slotIndex, ObjectKind::String, SlotStorage::HeapString, sizeBytes);
    }

    [[nodiscard]] std::optional<ObjectRef> allocateArraySlot(
        semantic::SymbolId arrayTypeId,
        semantic::PrimitiveType elementType,
        semantic::SymbolId elementTypeId,
        std::size_t length,
        const Value& initialValue,
        std::size_t sizeBytes,
        RuntimeError* error,
        bool* outReallocated = nullptr) {
        const std::uint32_t layoutId = internArrayLayout(arrayTypeId, elementType, elementTypeId);
        const auto slotIndex = acquireSlot(sizeBytes, error, outReallocated);
        if (!slotIndex) return std::nullopt;
        if (length > 0) {
            pinHeapValueIfNeededUnlocked(initialValue);
        }
        auto& slot = slots[*slotIndex];
        if (length == 0) {
            std::uint16_t cls = 0;
            std::uint32_t handle = CompactSlot::kInvalidHandle;
            Value* ptr = slabPool.allocate(0, cls, handle);
            slot.auxMeta = cls;
            slot.payload.s = {
                layoutId,
                0u,
                ptr,
                handle,
                CompactSlot::kInvalidHandle,
            };
            return finalizeSlotAllocation(*slotIndex, ObjectKind::Array, SlotStorage::SlabValues, sizeBytes);
        }
        if (length <= CompactSlot::kInlineValueCapacity) {
            std::uint16_t cls = 0;
            std::uint32_t handle = CompactSlot::kInvalidHandle;
            Value* ptr = slabPool.allocate(length, cls, handle);
            for (std::size_t i = 0; i < length; ++i) {
                ptr[i] = initialValue;
            }
            slot.auxMeta = cls;
            slot.payload.s = {
                layoutId,
                static_cast<std::uint32_t>(length),
                ptr,
                handle,
                CompactSlot::kInvalidHandle,
            };
            return finalizeSlotAllocation(*slotIndex, ObjectKind::Array, SlotStorage::SlabValues, sizeBytes);
        }
        Value* ptr = nullptr;
        const std::uint32_t handle = largeBuffers.acquireFill(length, initialValue, ptr);
        slot.auxMeta = 0;
        slot.payload.s = {
            layoutId,
            static_cast<std::uint32_t>(length),
            ptr,
            handle,
            CompactSlot::kInvalidHandle,
        };
        return finalizeSlotAllocation(*slotIndex, ObjectKind::Array, SlotStorage::HeapValues, sizeBytes);
    }

    [[nodiscard]] std::optional<ObjectRef> allocateRecordValuesSlot(
        std::uint32_t layoutId,
        std::vector<Value>&& vals,
        std::size_t sizeBytes,
        RuntimeError* error) {
        const auto slotIndex = acquireSlot(sizeBytes, error);
        if (!slotIndex) return std::nullopt;
        for (const auto& v : vals) {
            pinHeapValueIfNeededUnlocked(v);
        }
        auto& slot = slots[*slotIndex];
        const std::size_t count = vals.size();
        if (count <= CompactSlot::kInlineValueCapacity) {
            std::uint16_t cls = 0;
            std::uint32_t handle = CompactSlot::kInvalidHandle;
            Value* ptr = slabPool.allocate(count, cls, handle);
            for (std::size_t i = 0; i < count; ++i) {
                ptr[i] = std::move(vals[i]);
            }
            slot.auxMeta = cls;
            slot.payload.s = {
                layoutId,
                static_cast<std::uint32_t>(count),
                ptr,
                handle,
                CompactSlot::kInvalidHandle,
            };
            return finalizeSlotAllocation(*slotIndex, ObjectKind::Record, SlotStorage::SlabValues, sizeBytes);
        }
        Value* ptr = nullptr;
        const std::uint32_t handle = largeBuffers.acquireMove(std::move(vals), ptr);
        slot.auxMeta = 0;
        slot.payload.s = {
            layoutId,
            static_cast<std::uint32_t>(count),
            ptr,
            handle,
            CompactSlot::kInvalidHandle,
        };
        return finalizeSlotAllocation(*slotIndex, ObjectKind::Record, SlotStorage::HeapValues, sizeBytes);
    }

    void promoteSlotToOldGen(CompactSlot& slot) {
        slot.gcFlags &= static_cast<std::uint8_t>(~(GcFlag::InNursery | GcFlag::Marked));
        ++statistics.objectsPromoted;
        if (slot.kind() == ObjectKind::Array &&
            slot.storage() == SlotStorage::HeapValues &&
            slot.payload.s.length >= ManagedHeap::kCardTableMinLength &&
            !slot.hasFlag(GcFlag::HasCardTable)) {
            const auto& layout = typeLayouts[slot.payload.s.layoutId];
            if (layout.mayContainReferences) {
                slot.payload.s.cardHandle = cardArena.allocate(slot.payload.s.length);
                slot.gcFlags |= GcFlag::HasCardTable;
            }
        }
    }

    void sweepReclaimSlot(std::uint32_t slotIdx, CompactSlot& slot, bool isYoung) {
        if (slot.kind() == ObjectKind::String && slot.hasFlag(GcFlag::IsInterned)) {
            const std::string key(slotStringView(slot));
            const auto it = internedStrings.find(key);
            if (it != internedStrings.end() &&
                it->second.slot == slotIdx &&
                it->second.generation == slot.generation) {
                internedStrings.erase(it);
            }
        }
        const auto bytes = slotSizeBytes(slotIdx, slot);
        releaseSlotPayload(slotIdx, slot);
        ++slot.generation;
        if (slot.generation == 0) slot.generation = 1;
        freeSlots.push_back(slotIdx);
        --statistics.liveObjects;
        statistics.liveBytes -= bytes;
        ++statistics.objectsReclaimed;
        if (isYoung) ++statistics.youngObjectsReclaimed;
        statistics.bytesReclaimed += bytes;
    }

    void mark(ObjectRef reference) {
        auto* slot = getSlot(reference);
        if (!slot || slot->hasFlag(GcFlag::Marked)) return;
        slot->gcFlags |= GcFlag::Marked;
        markStack.push_back(reference);
    }

    void markValue(
        const Value& value,
        std::unordered_set<const StructStorage*>& visitedStructs) {
        if (!value.isTraceable()) return;
        const auto t = value.rawTag();
        if (t == ValueTag::ObjectRef) {
            mark(value.asObjectRefUnchecked());
            return;
        }
        if (t == ValueTag::Struct) {
            const auto& structure = value.asStructUnchecked();
            if (!structure.isInline && structure.storage &&
                visitedStructs.insert(structure.storage.get()).second) {
                for (const auto& field : structure.storage->fields) {
                    markValue(field, visitedStructs);
                }
            }
        }
    }

    void markValue(const Value& value) {
        if (!value.isTraceable()) return;
        const auto t = value.rawTag();
        if (t == ValueTag::ObjectRef) {
            mark(value.asObjectRefUnchecked());
            return;
        }
        if (t == ValueTag::Struct) {
            const auto& structure = value.asStructUnchecked();
            if (!structure.isInline && structure.storage) {
                std::unordered_set<const StructStorage*> visitedStructs;
                visitedStructs.insert(structure.storage.get());
                for (const auto& field : structure.storage->fields) {
                    markValue(field, visitedStructs);
                }
            }
        }
    }

    [[nodiscard]] bool validRootValue(
        const Value& value,
        std::unordered_set<const StructStorage*>& visitedStructs) const {
        if (const auto* reference = std::get_if<ObjectRef>(&value)) {
            return getSlot(*reference) != nullptr;
        }
        if (const auto* structure = std::get_if<StructValue>(&value)) {
            if (!structure->valid()) return false;
            if (structure->isInline || !structure->storage) return true;
            if (!visitedStructs.insert(structure->storage.get()).second) return true;
            for (const auto& field : structure->storage->fields) {
                if (!validRootValue(field, visitedStructs)) return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool validRootValue(const Value& value) const {
        if (const auto* reference = std::get_if<ObjectRef>(&value)) {
            return getSlot(*reference) != nullptr;
        }
        if (const auto* structure = std::get_if<StructValue>(&value)) {
            if (!structure->valid()) return false;
            if (structure->isInline || !structure->storage) return true;
            std::unordered_set<const StructStorage*> visitedStructs;
            visitedStructs.insert(structure->storage.get());
            for (const auto& field : structure->storage->fields) {
                if (!validRootValue(field, visitedStructs)) return false;
            }
        }
        return true;
    }

    void markRoots(const ShadowStack& shadowStack) {
        lastScannedShadowStackId = shadowStack.instanceId();
        shadowStack.visitTraceableRoots([&](const Value& value) { markValue(value); });
        for (const auto& entry : persistentRoots) markValue(entry.second);
        for (const auto& entry : staticFields) markValue(entry.second);
        for (const auto& [typeName, table] : nativeEcsTables) {
            (void)typeName;
            if (!table.mayContainReferences) continue;
            for (const auto& v : table.values) markValue(v);
        }
    }

    void startCollection(const ShadowStack& shadowStack) {
        for (auto& slot : slots) {
            if (slot.occupied()) {
                slot.gcFlags &= static_cast<std::uint8_t>(~GcFlag::Marked);
            }
        }
        markStack.clear();
        sweepIndex = 0;
        sweepLimit = slots.size();
        resumeSweep = false;
        lastScannedShadowStackId = 0;
        phase = GcPhase::Mark;
        ++statistics.collectionsStarted;
        markRoots(shadowStack);
    }

    void scan(ObjectRef reference) {
        const auto* slot = getSlot(reference);
        if (!slot || slot->kind() == ObjectKind::String) return;
        const auto count = slotValueCount(*slot);
        if (count == 0) return;
        const Value* data = slot->payload.s.data;
        const auto& layout = typeLayouts[slot->payload.s.layoutId];
        if (slot->kind() == ObjectKind::Array) {
            if (!layout.mayContainReferences) return;
            for (std::size_t i = 0; i < count; ++i) markValue(data[i]);
            return;
        }
        for (const auto index : layout.referenceFields) {
            if (index < count) markValue(data[index]);
        }
    }

    void writeBarrierObject(ObjectRef owner, const Value& value) noexcept {
        if (!valueMayContainReferences(value)) return;
        auto* slot = getSlot(owner);
        if (!slot) return;
        if (!slot->hasFlag(GcFlag::InNursery) && !slot->hasFlag(GcFlag::InRememberedSet)) {
            slot->gcFlags |= GcFlag::InRememberedSet;
            rememberedSet.push_back(owner);
        }
        if (phase == GcPhase::Idle || !slot->hasFlag(GcFlag::Marked)) return;
        markValue(value);
        if (!markStack.empty()) {
            reopenMarkFromSweep();
        }
    }

    void writeBarrierArrayElement(ObjectRef owner, std::size_t elementIndex, const Value& value) noexcept {
        if (!valueMayContainReferences(value)) return;
        auto* slot = getSlot(owner);
        if (!slot) return;
        if (!slot->hasFlag(GcFlag::InNursery)) {
            if (slot->hasFlag(GcFlag::HasCardTable)) {
                auto* cardBytes = cardArena.get(slot->payload.s.cardHandle).bytePtr();
                cardBytes[elementIndex >> ManagedHeap::kCardElementShift] = 1u;
            }
            if (!slot->hasFlag(GcFlag::InRememberedSet)) {
                slot->gcFlags |= GcFlag::InRememberedSet;
                rememberedSet.push_back(owner);
            }
        }
        if (phase == GcPhase::Idle || !slot->hasFlag(GcFlag::Marked)) return;
        markValue(value);
        if (!markStack.empty()) {
            reopenMarkFromSweep();
        }
    }

    void writeBarrierArrayConservative(ObjectRef owner, const Value& value) noexcept {
        if (!valueMayContainReferences(value)) return;
        auto* slot = getSlot(owner);
        if (!slot) return;
        if (!slot->hasFlag(GcFlag::InNursery)) {
            if (slot->hasFlag(GcFlag::HasCardTable)) {
                cardArena.get(slot->payload.s.cardHandle).markAllDirty();
            }
            if (!slot->hasFlag(GcFlag::InRememberedSet)) {
                slot->gcFlags |= GcFlag::InRememberedSet;
                rememberedSet.push_back(owner);
            }
        }
        if (phase == GcPhase::Idle || !slot->hasFlag(GcFlag::Marked)) return;
        markValue(value);
        if (!markStack.empty()) {
            reopenMarkFromSweep();
        }
    }

    [[nodiscard]] bool finishCollectionUnlocked() {
        phase = GcPhase::Idle;
        requested = false;
        resumeSweep = false;
        wallClockCycleActive = false;
        lastScannedShadowStackId = 0;
        sweepIndex = 0;
        sweepLimit = 0;
        for (const auto slotIdx : nurserySlots) {
            if (slotIdx < slots.size() && slots[slotIdx].occupied() &&
                slots[slotIdx].hasFlag(GcFlag::InNursery)) {
                promoteSlotToOldGen(slots[slotIdx]);
            }
        }
        nurserySlots.clear();
        for (const auto& ref : rememberedSet) {
            if (auto* slot = getSlot(ref)) {
                slot->gcFlags &= static_cast<std::uint8_t>(~GcFlag::InRememberedSet);
                if (slot->hasFlag(GcFlag::HasCardTable)) {
                    cardArena.get(slot->payload.s.cardHandle).clearAll();
                }
            }
        }
        rememberedSet.clear();
        ++statistics.collectionsCompleted;
        const auto doubled = statistics.liveBytes >
                std::numeric_limits<std::size_t>::max() / 2
            ? config.maximumHeapBytes
            : statistics.liveBytes * 2;
        nextCollectionThreshold = std::max(
            config.initialCollectionThresholdBytes,
            std::min(config.maximumHeapBytes, doubled));
        return hasAnyPinnedPoolValues();
    }

    [[nodiscard]] std::optional<semantic::SymbolId> arrayTypeIdUnlocked(ObjectRef reference) const noexcept {
        const auto* slot = getSlot(reference);
        if (!slot || slot->kind() != ObjectKind::Array) return std::nullopt;
        return typeLayouts[slot->payload.s.layoutId].typeId;
    }

    [[nodiscard]] std::optional<semantic::SymbolId> objectTypeIdUnlocked(ObjectRef reference) const noexcept {
        const auto* slot = getSlot(reference);
        if (!slot || slot->kind() != ObjectKind::Record) return std::nullopt;
        const auto tid = typeLayouts[slot->payload.s.layoutId].typeId;
        if (tid == 0) return std::nullopt;
        return tid;
    }

    std::size_t stepUnlocked(
        const ShadowStack& shadowStack,
        std::size_t workBudget,
        bool& outShouldSweepPools) {
        if (workBudget == 0) return 0;
        if (phase == GcPhase::Idle) {
            if (!requested) return 0;
            startCollection(shadowStack);
        } else if (phase == GcPhase::Mark &&
                   lastScannedShadowStackId != shadowStack.instanceId()) {
            markRoots(shadowStack);
        }

        std::size_t performed = 0;
        while (performed < workBudget) {
            if (phase == GcPhase::Sweep && !markStack.empty()) {
                reopenMarkFromSweep();
            }
            if (phase == GcPhase::Mark) {
                if (markStack.empty()) {
                    markRoots(shadowStack);
                    if (!markStack.empty()) continue;
                    phase = GcPhase::Sweep;
                    if (!resumeSweep) sweepIndex = 0;
                    resumeSweep = false;
                    continue;
                }
                const auto reference = markStack.back();
                markStack.pop_back();
                scan(reference);
                ++statistics.markWork;
                ++performed;
                continue;
            }

            if (phase == GcPhase::Sweep) {
                if (sweepIndex >= sweepLimit) {
                    if (finishCollectionUnlocked()) {
                        outShouldSweepPools = true;
                    }
                    break;
                }
                const auto slotIdx = static_cast<std::uint32_t>(sweepIndex++);
                auto& slot = slots[slotIdx];
                if (slot.occupied() && !slot.hasFlag(GcFlag::Marked)) {
                    sweepReclaimSlot(slotIdx, slot, false);
                }
                ++statistics.sweepWork;
                ++performed;
                continue;
            }
            break;
        }
        return performed;
    }

    IncrementalGcBudgetStats collectIncrementalWithBudgetUnlocked(
        const RootSet& roots,
        std::chrono::nanoseconds budget,
        std::size_t checkInterval,
        bool forceStartIfIdle,
        std::chrono::steady_clock::time_point startTime,
        bool& outShouldSweepPools) {
        IncrementalGcBudgetStats stats{};

        if (phase == GcPhase::Idle &&
            !requested &&
            statistics.liveBytes < nextCollectionThreshold &&
            !forceStartIfIdle) {
            stats.cycleCompleted = false;
            stats.budgetExhausted = false;
            stats.phaseAfterStep = GcPhase::Idle;
            return stats;
        }

        if (phase == GcPhase::Idle && statistics.liveObjects == 0 && !requested) {
            stats.cycleCompleted = true;
            stats.budgetExhausted = false;
            stats.phaseAfterStep = GcPhase::Idle;
            return stats;
        }

        if (budget <= std::chrono::nanoseconds::zero()) {
            stats.cycleCompleted = false;
            stats.budgetExhausted = true;
            stats.phaseAfterStep = phase;
            return stats;
        }

        wallClockCycleActive = true;
        bool startedThisCall = false;
        if (phase == GcPhase::Idle) {
            startCollection(roots);
            startedThisCall = true;
        } else if (phase == GcPhase::Mark &&
                   roots.frameCount() > 0 &&
                   lastScannedShadowStackId != roots.instanceId()) {
            markRoots(roots);
        }

        if (startedThisCall) {
            if (markStack.empty() && sweepLimit == 0) {
                if (finishCollectionUnlocked()) {
                    outShouldSweepPools = true;
                }
                stats.cycleCompleted = true;
                stats.phaseAfterStep = phase;
                return stats;
            }
            if ((std::chrono::steady_clock::now() - startTime) >= budget) {
                stats.cycleCompleted = false;
                stats.budgetExhausted = true;
                stats.phaseAfterStep = phase;
                return stats;
            }
        }

        const std::size_t stride = (checkInterval == 0) ? 1u : checkInterval;
        std::size_t sinceLastCheck = 0;

        while (true) {
            if (phase == GcPhase::Sweep && !markStack.empty()) {
                reopenMarkFromSweep();
            }

            if (phase == GcPhase::Mark) {
                if (markStack.empty()) {
                    markRoots(roots);
                    if (!markStack.empty()) continue;
                    phase = GcPhase::Sweep;
                    if (!resumeSweep) sweepIndex = 0;
                    resumeSweep = false;
                    if (sweepIndex >= sweepLimit) {
                        if (finishCollectionUnlocked()) {
                            outShouldSweepPools = true;
                        }
                        stats.cycleCompleted = true;
                        break;
                    }
                    continue;
                }
                const auto reference = markStack.back();
                markStack.pop_back();
                scan(reference);
                ++statistics.markWork;
                ++stats.objectsMarked;
                ++sinceLastCheck;
            } else if (phase == GcPhase::Sweep) {
                if (sweepIndex >= sweepLimit) {
                    if (finishCollectionUnlocked()) {
                        outShouldSweepPools = true;
                    }
                    stats.cycleCompleted = true;
                    break;
                }
                const auto slotIdx = static_cast<std::uint32_t>(sweepIndex++);
                auto& slot = slots[slotIdx];
                if (slot.occupied() && !slot.hasFlag(GcFlag::Marked)) {
                    sweepReclaimSlot(slotIdx, slot, false);
                    ++stats.objectsReclaimed;
                }
                ++statistics.sweepWork;
                ++stats.objectsSwept;
                ++sinceLastCheck;

                if (sweepIndex >= sweepLimit && markStack.empty()) {
                    if (finishCollectionUnlocked()) {
                        outShouldSweepPools = true;
                    }
                    stats.cycleCompleted = true;
                    break;
                }
            } else {
                stats.cycleCompleted = true;
                break;
            }

            if (sinceLastCheck >= stride) {
                sinceLastCheck = 0;
                if ((std::chrono::steady_clock::now() - startTime) >= budget) {
                    stats.budgetExhausted = true;
                    break;
                }
            }
        }

        stats.phaseAfterStep = phase;
        return stats;
    }

    HeapSnapshot snapshotUnlocked(const ShadowStack* shadowStack) const;
};

ManagedHeap::ManagedHeap(HeapConfig config)
    : impl_(std::make_unique<Impl>(config)) {}

ManagedHeap::~ManagedHeap() = default;
ManagedHeap::ManagedHeap(ManagedHeap&&) noexcept = default;
ManagedHeap& ManagedHeap::operator=(ManagedHeap&&) noexcept = default;

std::optional<ObjectRef> ManagedHeap::allocateString(
    std::string value,
    RuntimeError* error) {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    const auto size = Impl::BaseObjectSizeBytes + value.size();
    return impl_->allocateStringSlot(std::move(value), size, error);
}

std::optional<ObjectRef> ManagedHeap::internString(
    std::string_view value,
    RuntimeError* error) {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    const std::string key(value);
    const auto found = impl_->internedStrings.find(key);
    if (found != impl_->internedStrings.end()) {
        if (impl_->getSlot(found->second) != nullptr) {
            if (impl_->phase != GcPhase::Idle) {
                impl_->mark(found->second);
                impl_->reopenMarkFromSweep();
            }
            return found->second;
        }
        impl_->internedStrings.erase(found);
    }
    const auto size = Impl::BaseObjectSizeBytes + key.size();
    auto allocated = impl_->allocateStringSlot(key, size, error);
    if (allocated) {
        impl_->slots[allocated->slot].gcFlags |= GcFlag::IsInterned;
        impl_->internedStrings[key] = *allocated;
    }
    return allocated;
}

std::size_t ManagedHeap::internedStringCount() const noexcept {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    std::size_t aliveCount = 0;
    for (const auto& [k, ref] : impl_->internedStrings) {
        (void)k;
        if (impl_->getSlot(ref) != nullptr) ++aliveCount;
    }
    return aliveCount;
}

std::size_t ManagedHeap::sharedTypeLayoutCount() const noexcept {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    return impl_->sharedRecordLayoutByTypeId.size();
}

std::optional<ObjectRef> ManagedHeap::allocateArray(
    std::size_t length,
    Value initialValue,
    RuntimeError* error) {
    auto elementType = valueType(initialValue);
    if (elementType == semantic::PrimitiveType::Null) {
        elementType = semantic::PrimitiveType::Error;
    }
    return allocateTypedArray(
        0, elementType, 0, length, std::move(initialValue), error);
}

std::optional<ObjectRef> ManagedHeap::allocateTypedArray(
    semantic::SymbolId arrayTypeId,
    semantic::PrimitiveType elementType,
    semantic::SymbolId elementTypeId,
    std::size_t length,
    Value initialValue,
    RuntimeError* error) {
    return allocateTypedArray(
        arrayTypeId, elementType, elementTypeId, length, std::move(initialValue), error, nullptr);
}

std::optional<ObjectRef> ManagedHeap::allocateTypedArray(
    semantic::SymbolId arrayTypeId,
    semantic::PrimitiveType elementType,
    semantic::SymbolId elementTypeId,
    std::size_t length,
    Value initialValue,
    RuntimeError* error,
    bool* outSlotsReallocated) {
    const bool validElement = elementType == semantic::PrimitiveType::Error ||
        elementType == semantic::PrimitiveType::Bool ||
        semantic::isNumericType(elementType) ||
        elementType == semantic::PrimitiveType::String ||
        elementType == semantic::PrimitiveType::Object ||
        elementType == semantic::PrimitiveType::Struct ||
        elementType == semantic::PrimitiveType::Enum ||
        elementType == semantic::PrimitiveType::Array ||
        elementType == semantic::PrimitiveType::Handle;
    if (!validElement ||
        (semantic::isExactType(elementType)
            ? elementTypeId == 0
            : elementTypeId != 0)) {
        setHeapError(error, ErrorCode::InvalidArguments,
            "managed array element type metadata is invalid");
        return std::nullopt;
    }
    if (length > 0 &&
        elementType != semantic::PrimitiveType::Error &&
        valueType(initialValue) != elementType) {
        setHeapError(error, ErrorCode::TypeMismatch,
            "managed array initial value does not match its element type");
        return std::nullopt;
    }
    const auto storage = estimateValueStorage(length);
    if (storage == std::numeric_limits<std::size_t>::max()) {
        setHeapError(error, ErrorCode::OutOfMemory, "managed array is too large");
        return std::nullopt;
    }
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    return impl_->allocateArraySlot(
        arrayTypeId,
        elementType,
        elementTypeId,
        length,
        initialValue,
        Impl::BaseObjectSizeBytes + storage,
        error,
        outSlotsReallocated);
}

std::optional<ObjectRef> ManagedHeap::allocateRecord(
    std::size_t fieldCount,
    RuntimeError* error) {
    const auto storage = estimateValueStorage(fieldCount);
    if (storage == std::numeric_limits<std::size_t>::max()) {
        setHeapError(error, ErrorCode::OutOfMemory, "managed record is too large");
        return std::nullopt;
    }
    std::vector<Value> fields(fieldCount);
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    const std::uint32_t layoutId = impl_->internUntypedAllRefRecordLayout(fieldCount);
    return impl_->allocateRecordValuesSlot(
        layoutId,
        std::move(fields),
        Impl::BaseObjectSizeBytes + storage,
        error);
}

std::optional<ObjectRef> ManagedHeap::allocateObject(
    semantic::SymbolId typeId,
    std::vector<Value> fields,
    std::vector<std::size_t> referenceFields,
    RuntimeError* error) {
    const auto storage = estimateValueStorage(fields.size());
    if (typeId == 0) {
        setHeapError(error, ErrorCode::InvalidArguments,
            "managed object type ID must be non-zero");
        return std::nullopt;
    }
    if (storage == std::numeric_limits<std::size_t>::max()) {
        setHeapError(error, ErrorCode::OutOfMemory, "managed object is too large");
        return std::nullopt;
    }
    for (const auto index : referenceFields) {
        if (index >= fields.size()) {
            setHeapError(error, ErrorCode::InvalidArguments,
                "managed object reference field is out of range");
            return std::nullopt;
        }
    }
    std::sort(referenceFields.begin(), referenceFields.end());
    referenceFields.erase(
        std::unique(referenceFields.begin(), referenceFields.end()),
        referenceFields.end());
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    const std::uint32_t layoutId = impl_->internTypedRecordLayout(typeId, referenceFields);
    return impl_->allocateRecordValuesSlot(
        layoutId,
        std::move(fields),
        Impl::BaseObjectSizeBytes + storage,
        error);
}

std::optional<ObjectRef> ManagedHeap::allocateObjectFromTemplate(
    semantic::SymbolId typeId,
    const Value* defaultFields,
    std::size_t fieldCount,
    const std::shared_ptr<const std::vector<std::size_t>>& referenceFields,
    RuntimeError* error,
    bool* outSlotsReallocated) {
    if (typeId == 0) {
        setHeapError(error, ErrorCode::InvalidArguments,
            "managed object type ID must be non-zero");
        return std::nullopt;
    }
    const auto storage = estimateValueStorage(fieldCount);
    if (storage == std::numeric_limits<std::size_t>::max()) {
        setHeapError(error, ErrorCode::OutOfMemory, "managed object is too large");
        return std::nullopt;
    }
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    const std::uint32_t layoutId = impl_->internTemplateRecordLayout(typeId, referenceFields);
    const std::size_t sizeBytes = Impl::BaseObjectSizeBytes + storage;
    const auto slotIndex = impl_->acquireSlot(sizeBytes, error, outSlotsReallocated);
    if (!slotIndex) return std::nullopt;
    for (std::size_t i = 0; i < fieldCount; ++i) {
        impl_->pinHeapValueIfNeededUnlocked(defaultFields[i]);
    }
    auto& slot = impl_->slots[*slotIndex];
    if (fieldCount <= CompactSlot::kInlineValueCapacity) {
        std::uint16_t cls = 0;
        std::uint32_t handle = CompactSlot::kInvalidHandle;
        Value* ptr = impl_->slabPool.allocate(fieldCount, cls, handle);
        if (fieldCount != 0) {
            std::memcpy(ptr, defaultFields, fieldCount * sizeof(Value));
        }
        slot.auxMeta = cls;
        slot.payload.s = {
            layoutId,
            static_cast<std::uint32_t>(fieldCount),
            ptr,
            handle,
            CompactSlot::kInvalidHandle,
        };
        return impl_->finalizeSlotAllocation(
            *slotIndex, ObjectKind::Record, SlotStorage::SlabValues, sizeBytes);
    }
    Value* ptr = nullptr;
    const std::uint32_t handle = impl_->largeBuffers.acquireCopy(defaultFields, fieldCount, ptr);
    slot.auxMeta = 0;
    slot.payload.s = {
        layoutId,
        static_cast<std::uint32_t>(fieldCount),
        ptr,
        handle,
        CompactSlot::kInvalidHandle,
    };
    return impl_->finalizeSlotAllocation(
        *slotIndex, ObjectKind::Record, SlotStorage::HeapValues, sizeBytes);
}

bool ManagedHeap::isAlive(ObjectRef reference) const noexcept {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    return impl_->getSlot(reference) != nullptr;
}

std::optional<std::string_view> ManagedHeap::stringView(ObjectRef reference) const {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    const auto* slot = impl_->getSlot(reference);
    if (!slot || slot->kind() != ObjectKind::String) return std::nullopt;
    return impl_->slotStringView(*slot);
}

std::optional<std::string> ManagedHeap::tryCopyStringGlobal(ObjectRef reference) {
    if (!reference.valid() || reference.kind != ObjectKind::String) {
        return std::nullopt;
    }
    auto& shard = liveStringHeapShards()[reference.heapId & 63u];
    if (shard.activeCount.load(std::memory_order_acquire) == 0) {
        return std::nullopt;
    }
    std::vector<std::shared_ptr<HeapSyncState>> candidates;
    {
        std::lock_guard<std::mutex> shardLock(shard.mutex);
        for (const auto& entry : shard.entries) {
            if (entry && static_cast<std::uint32_t>(entry->heapIdValue) == reference.heapId) {
                candidates.push_back(entry);
            }
        }
    }
    for (const auto& sync : candidates) {
        HeapSyncMutexGuard instanceLock(sync->mutex);
        auto* impl = static_cast<Impl*>(sync->ownerImpl);
        if (!impl) continue;
        const auto* slot = impl->getSlot(reference);
        if (!slot || slot->kind() != ObjectKind::String) continue;
        return std::string(impl->slotStringView(*slot));
    }
    return std::nullopt;
}

namespace {

[[nodiscard]] constexpr std::size_t nativeFieldKindSize(NativeFieldKind kind) noexcept {
    switch (kind) {
    case NativeFieldKind::Int8: return 1;
    case NativeFieldKind::UInt8: return 1;
    case NativeFieldKind::Int16: return 2;
    case NativeFieldKind::UInt16: return 2;
    case NativeFieldKind::Int32: return 4;
    case NativeFieldKind::UInt32: return 4;
    case NativeFieldKind::Int64: return 8;
    case NativeFieldKind::UInt64: return 8;
    case NativeFieldKind::Float32: return 4;
    case NativeFieldKind::Float64: return 8;
    case NativeFieldKind::Bool: return 1;
    case NativeFieldKind::Fixed: return 4;
    case NativeFieldKind::Fixed16: return 2;
    case NativeFieldKind::Vec2: return 8;
    case NativeFieldKind::Vec3: return 12;
    case NativeFieldKind::Quat: return 16;
    }
    return 0;
}

[[nodiscard]] bool validateRawColumnView(
    const NativeRawColumnView& view,
    semantic::SymbolId elementTypeId) noexcept {
    if (elementTypeId == 0 || view.structTypeId != elementTypeId ||
        view.fields.empty() || view.strideBytes == 0) {
        return false;
    }
    if (view.count > 0 && !view.basePtr) {
        return false;
    }
    for (const auto& f : view.fields) {
        const auto fSize = nativeFieldKindSize(f.kind);
        if (fSize == 0 || f.byteOffset > view.strideBytes ||
            fSize > view.strideBytes - f.byteOffset) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] Value wrapIntegralNativeField(
    std::int64_t signedVal,
    std::uint64_t unsignedVal,
    bool sourceUnsigned,
    semantic::PrimitiveType requestedType,
    semantic::PrimitiveType defaultType) noexcept {
    const auto target = (requestedType == semantic::PrimitiveType::Error)
        ? defaultType
        : requestedType;
    switch (target) {
    case semantic::PrimitiveType::SByte:
        return SByteValue{static_cast<std::int8_t>(sourceUnsigned ? static_cast<std::int64_t>(unsignedVal) : signedVal)};
    case semantic::PrimitiveType::Byte:
        return ByteValue{static_cast<std::uint8_t>(sourceUnsigned ? unsignedVal : static_cast<std::uint64_t>(signedVal))};
    case semantic::PrimitiveType::Short:
        return ShortValue{static_cast<std::int16_t>(sourceUnsigned ? static_cast<std::int64_t>(unsignedVal) : signedVal)};
    case semantic::PrimitiveType::UShort:
        return UShortValue{static_cast<std::uint16_t>(sourceUnsigned ? unsignedVal : static_cast<std::uint64_t>(signedVal))};
    case semantic::PrimitiveType::UInt:
        return UIntValue{static_cast<std::uint32_t>(sourceUnsigned ? unsignedVal : static_cast<std::uint64_t>(signedVal))};
    case semantic::PrimitiveType::Long:
        return LongValue{sourceUnsigned ? static_cast<std::int64_t>(unsignedVal) : signedVal};
    case semantic::PrimitiveType::ULong:
        return ULongValue{sourceUnsigned ? unsignedVal : static_cast<std::uint64_t>(signedVal)};
    case semantic::PrimitiveType::Int:
    default:
        return sourceUnsigned ? static_cast<std::int64_t>(unsignedVal) : signedVal;
    }
}

[[nodiscard]] bool tryExtractIntegralBits(const Value& val, std::uint64_t& outBits) noexcept {
    if (const auto* i = std::get_if<std::int64_t>(&val)) {
        outBits = static_cast<std::uint64_t>(*i);
        return true;
    }
    if (const auto* l = std::get_if<LongValue>(&val)) {
        outBits = static_cast<std::uint64_t>(l->value);
        return true;
    }
    if (const auto* b = std::get_if<ByteValue>(&val)) {
        outBits = static_cast<std::uint64_t>(b->value);
        return true;
    }
    if (const auto* sb = std::get_if<SByteValue>(&val)) {
        outBits = static_cast<std::uint64_t>(static_cast<std::int64_t>(sb->value));
        return true;
    }
    if (const auto* s = std::get_if<ShortValue>(&val)) {
        outBits = static_cast<std::uint64_t>(static_cast<std::int64_t>(s->value));
        return true;
    }
    if (const auto* us = std::get_if<UShortValue>(&val)) {
        outBits = static_cast<std::uint64_t>(us->value);
        return true;
    }
    if (const auto* ui = std::get_if<UIntValue>(&val)) {
        outBits = static_cast<std::uint64_t>(ui->value);
        return true;
    }
    if (const auto* ul = std::get_if<ULongValue>(&val)) {
        outBits = ul->value;
        return true;
    }
    return false;
}

[[nodiscard]] Value readNativeFieldFromBytes(
    const std::uint8_t* elemBase,
    const NativeFieldLayout& layout) noexcept {
    const std::uint8_t* ptr = elemBase + layout.byteOffset;
    switch (layout.kind) {
    case NativeFieldKind::Int8: {
        std::int8_t v = 0;
        std::memcpy(&v, ptr, sizeof(v));
        return wrapIntegralNativeField(v, 0u, false, layout.scriptType, semantic::PrimitiveType::SByte);
    }
    case NativeFieldKind::UInt8: {
        std::uint8_t v = 0;
        std::memcpy(&v, ptr, sizeof(v));
        return wrapIntegralNativeField(0, v, true, layout.scriptType, semantic::PrimitiveType::Byte);
    }
    case NativeFieldKind::Int16: {
        std::int16_t v = 0;
        std::memcpy(&v, ptr, sizeof(v));
        return wrapIntegralNativeField(v, 0u, false, layout.scriptType, semantic::PrimitiveType::Short);
    }
    case NativeFieldKind::UInt16: {
        std::uint16_t v = 0;
        std::memcpy(&v, ptr, sizeof(v));
        return wrapIntegralNativeField(0, v, true, layout.scriptType, semantic::PrimitiveType::UShort);
    }
    case NativeFieldKind::Int32: {
        std::int32_t v = 0;
        std::memcpy(&v, ptr, sizeof(v));
        return wrapIntegralNativeField(v, 0u, false, layout.scriptType, semantic::PrimitiveType::Int);
    }
    case NativeFieldKind::UInt32: {
        std::uint32_t v = 0;
        std::memcpy(&v, ptr, sizeof(v));
        return wrapIntegralNativeField(0, v, true, layout.scriptType, semantic::PrimitiveType::UInt);
    }
    case NativeFieldKind::Int64: {
        std::int64_t v = 0;
        std::memcpy(&v, ptr, sizeof(v));
        return wrapIntegralNativeField(v, 0u, false, layout.scriptType, semantic::PrimitiveType::Int);
    }
    case NativeFieldKind::UInt64: {
        std::uint64_t v = 0;
        std::memcpy(&v, ptr, sizeof(v));
        return wrapIntegralNativeField(0, v, true, layout.scriptType, semantic::PrimitiveType::ULong);
    }
    case NativeFieldKind::Float32: {
        float v = 0.0f;
        std::memcpy(&v, ptr, sizeof(v));
        if (layout.scriptType == semantic::PrimitiveType::Double) {
            return static_cast<double>(v);
        }
        return FloatValue{v};
    }
    case NativeFieldKind::Float64: {
        double v = 0.0;
        std::memcpy(&v, ptr, sizeof(v));
        if (layout.scriptType == semantic::PrimitiveType::Float) {
            return FloatValue{static_cast<float>(v)};
        }
        return v;
    }
    case NativeFieldKind::Bool: {
        bool v = false;
        std::memcpy(&v, ptr, sizeof(v));
        return v;
    }
    case NativeFieldKind::Fixed: {
        std::int32_t raw = 0;
        std::memcpy(&raw, ptr, sizeof(raw));
        return FixedValue{raw};
    }
    case NativeFieldKind::Fixed16: {
        std::int16_t raw = 0;
        std::memcpy(&raw, ptr, sizeof(raw));
        return Fixed16Value{raw};
    }
    case NativeFieldKind::Vec2: {
        Vec2Value v{};
        std::memcpy(&v, ptr, sizeof(v));
        return v;
    }
    case NativeFieldKind::Vec3: {
        Vec3Value v{};
        std::memcpy(&v, ptr, sizeof(v));
        return v;
    }
    case NativeFieldKind::Quat: {
        QuatValue v{};
        std::memcpy(&v, ptr, sizeof(v));
        return v;
    }
    }
    return std::int64_t{0};
}

bool writeNativeFieldToBytes(
    std::uint8_t* elemBase,
    const NativeFieldLayout& layout,
    const Value& val) noexcept {
    std::uint8_t* ptr = elemBase + layout.byteOffset;
    switch (layout.kind) {
    case NativeFieldKind::Int8: {
        std::uint64_t bits = 0;
        if (!tryExtractIntegralBits(val, bits)) return false;
        const auto out = static_cast<std::int8_t>(bits);
        std::memcpy(ptr, &out, sizeof(out));
        return true;
    }
    case NativeFieldKind::UInt8: {
        std::uint64_t bits = 0;
        if (!tryExtractIntegralBits(val, bits)) return false;
        const auto out = static_cast<std::uint8_t>(bits);
        std::memcpy(ptr, &out, sizeof(out));
        return true;
    }
    case NativeFieldKind::Int16: {
        std::uint64_t bits = 0;
        if (!tryExtractIntegralBits(val, bits)) return false;
        const auto out = static_cast<std::int16_t>(bits);
        std::memcpy(ptr, &out, sizeof(out));
        return true;
    }
    case NativeFieldKind::UInt16: {
        std::uint64_t bits = 0;
        if (!tryExtractIntegralBits(val, bits)) return false;
        const auto out = static_cast<std::uint16_t>(bits);
        std::memcpy(ptr, &out, sizeof(out));
        return true;
    }
    case NativeFieldKind::Int32: {
        std::uint64_t bits = 0;
        if (!tryExtractIntegralBits(val, bits)) return false;
        const auto out = static_cast<std::int32_t>(bits);
        std::memcpy(ptr, &out, sizeof(out));
        return true;
    }
    case NativeFieldKind::UInt32: {
        std::uint64_t bits = 0;
        if (!tryExtractIntegralBits(val, bits)) return false;
        const auto out = static_cast<std::uint32_t>(bits);
        std::memcpy(ptr, &out, sizeof(out));
        return true;
    }
    case NativeFieldKind::Int64: {
        std::uint64_t bits = 0;
        if (!tryExtractIntegralBits(val, bits)) return false;
        const auto out = static_cast<std::int64_t>(bits);
        std::memcpy(ptr, &out, sizeof(out));
        return true;
    }
    case NativeFieldKind::UInt64: {
        std::uint64_t bits = 0;
        if (!tryExtractIntegralBits(val, bits)) return false;
        std::memcpy(ptr, &bits, sizeof(bits));
        return true;
    }
    case NativeFieldKind::Float32: {
        float out = 0.0f;
        if (const auto* f = std::get_if<FloatValue>(&val)) {
            out = f->value;
        } else if (const auto* d = std::get_if<double>(&val)) {
            out = static_cast<float>(*d);
        } else {
            return false;
        }
        std::memcpy(ptr, &out, sizeof(out));
        return true;
    }
    case NativeFieldKind::Float64: {
        double out = 0.0;
        if (const auto* d = std::get_if<double>(&val)) {
            out = *d;
        } else if (const auto* f = std::get_if<FloatValue>(&val)) {
            out = static_cast<double>(f->value);
        } else {
            return false;
        }
        std::memcpy(ptr, &out, sizeof(out));
        return true;
    }
    case NativeFieldKind::Bool: {
        const auto* b = std::get_if<bool>(&val);
        if (!b) return false;
        const bool out = *b;
        std::memcpy(ptr, &out, sizeof(out));
        return true;
    }
    case NativeFieldKind::Fixed: {
        if (const auto* fx = std::get_if<FixedValue>(&val)) {
            std::memcpy(ptr, &fx->raw, sizeof(fx->raw));
            return true;
        }
        if (const auto f0 = loadInlineStructField(val, 0)) {
            if (const auto* i = std::get_if<std::int64_t>(&*f0)) {
                const auto raw = static_cast<std::int32_t>(*i);
                std::memcpy(ptr, &raw, sizeof(raw));
                return true;
            }
        }
        return false;
    }
    case NativeFieldKind::Fixed16: {
        if (const auto* fx = std::get_if<Fixed16Value>(&val)) {
            const auto raw16 = static_cast<std::int16_t>(fx->raw);
            std::memcpy(ptr, &raw16, sizeof(raw16));
            return true;
        }
        if (const auto f0 = loadInlineStructField(val, 0)) {
            if (const auto* i = std::get_if<std::int64_t>(&*f0)) {
                const auto raw16 = static_cast<std::int16_t>(*i);
                std::memcpy(ptr, &raw16, sizeof(raw16));
                return true;
            }
        }
        return false;
    }
    case NativeFieldKind::Vec2: {
        if (const auto* v = std::get_if<Vec2Value>(&val)) {
            std::memcpy(ptr, v, sizeof(*v));
            return true;
        }
        return false;
    }
    case NativeFieldKind::Vec3: {
        if (const auto* v = std::get_if<Vec3Value>(&val)) {
            std::memcpy(ptr, v, sizeof(*v));
            return true;
        }
        return false;
    }
    case NativeFieldKind::Quat: {
        QuatValue qv{};
        if (val.tryGetQuat(qv)) {
            std::memcpy(ptr, &qv, sizeof(qv));
            return true;
        }
        return false;
    }
    }
    return false;
}

} // namespace

std::optional<std::size_t> ManagedHeap::arrayLength(ObjectRef reference) const {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    const auto* slot = impl_->getSlot(reference);
    if (!slot || slot->kind() != ObjectKind::Array) return std::nullopt;
    if (slot->storage() == SlotStorage::RawColumn) {
        return impl_->rawColumns.get(slot->payload.s.handle).count;
    }
    return static_cast<std::size_t>(slot->payload.s.length);
}

std::optional<semantic::SymbolId> ManagedHeap::arrayTypeId(ObjectRef reference) const {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    const auto* slot = impl_->getSlot(reference);
    if (!slot || slot->kind() != ObjectKind::Array) return std::nullopt;
    return impl_->typeLayouts[slot->payload.s.layoutId].typeId;
}

std::optional<semantic::PrimitiveType> ManagedHeap::arrayElementType(ObjectRef reference) const {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    const auto* slot = impl_->getSlot(reference);
    if (!slot || slot->kind() != ObjectKind::Array) return std::nullopt;
    return impl_->typeLayouts[slot->payload.s.layoutId].elementType;
}

std::optional<semantic::SymbolId> ManagedHeap::arrayElementTypeId(ObjectRef reference) const {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    const auto* slot = impl_->getSlot(reference);
    if (!slot || slot->kind() != ObjectKind::Array) return std::nullopt;
    return impl_->typeLayouts[slot->payload.s.layoutId].elementTypeId;
}

std::optional<Value> ManagedHeap::arrayGet(ObjectRef reference, std::size_t index) const {
    semantic::SymbolId rawStructTypeId = 0;
    std::size_t rawFieldCount = 0;
    std::array<Value, 16> stackFields{};
    std::vector<Value> heapFields;
    {
        HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
        const auto* slot = impl_->getSlot(reference);
        if (!slot || slot->kind() != ObjectKind::Array) return std::nullopt;
        if (slot->storage() != SlotStorage::RawColumn) {
            if (index >= slot->payload.s.length) return std::nullopt;
            return slot->payload.s.data[index];
        }
        const auto& view = impl_->rawColumns.get(slot->payload.s.handle);
        if (index >= view.count || (!view.basePtr && view.count > 0)) return std::nullopt;
        const auto* elemBase = static_cast<const std::uint8_t*>(view.basePtr) + index * view.strideBytes;
        rawStructTypeId = view.structTypeId;
        rawFieldCount = view.fields.size();
        if (rawFieldCount <= 16) {
            for (std::size_t i = 0; i < rawFieldCount; ++i) {
                stackFields[i] = readNativeFieldFromBytes(elemBase, view.fields[i]);
            }
        } else {
            heapFields.resize(rawFieldCount);
            for (std::size_t i = 0; i < rawFieldCount; ++i) {
                heapFields[i] = readNativeFieldFromBytes(elemBase, view.fields[i]);
            }
        }
    }
    if (rawFieldCount <= 16) {
        return Value{makeStructValueFromFields(rawStructTypeId, ValueSpan(stackFields.data(), rawFieldCount))};
    }
    return Value{makeStructValueFromFields(rawStructTypeId, std::move(heapFields))};
}

const Value* ManagedHeap::arrayGetFast(ObjectRef reference, std::size_t index) const noexcept {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    const auto* slot = impl_->getSlot(reference);
    if (!slot || slot->kind() != ObjectKind::Array ||
        slot->storage() == SlotStorage::RawColumn || index >= slot->payload.s.length) {
        return nullptr;
    }
    return &slot->payload.s.data[index];
}

bool ManagedHeap::tryBindArrayFastView(ObjectRef reference, ArrayFastView& outView) const noexcept {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    auto* slot = impl_->getSlot(reference);
    if (!slot || slot->kind() != ObjectKind::Array ||
        slot->storage() == SlotStorage::RawColumn) {
        return false;
    }
    const std::uint32_t count = slot->payload.s.length;
    if (count > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
        return false;
    }
    const auto& layout = impl_->typeLayouts[slot->payload.s.layoutId];
    if (!impl_->mayContainPoolValues.load(std::memory_order_relaxed) &&
        (layout.elementType == semantic::PrimitiveType::String ||
         layout.elementType == semantic::PrimitiveType::Struct ||
         layout.elementType == semantic::PrimitiveType::Enum ||
         layout.elementType == semantic::PrimitiveType::Error)) {
        impl_->noteMaybePoolBackedUnlocked();
    }
    const bool inNursery = slot->hasFlag(GcFlag::InNursery);
    outView.data = slot->payload.s.data;
    outView.dirtyCards = (!inNursery && slot->hasFlag(GcFlag::HasCardTable))
        ? impl_->cardArena.get(slot->payload.s.cardHandle).bytePtr()
        : nullptr;
    outView.length = count;
    outView.elementType = layout.elementType;
    outView.needsRememberedBarrier = (!inNursery && !slot->hasFlag(GcFlag::InRememberedSet));
    outView.elementTypeId = layout.elementTypeId;
    return true;
}

void ManagedHeap::recordArrayWriteBarrier(
    ObjectRef arrayRef,
    std::size_t index,
    const Value& value) noexcept {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    impl_->writeBarrierArrayElement(arrayRef, index, value);
}

void ManagedHeap::recordArrayWriteBarrier(ObjectRef arrayRef, const Value& value) noexcept {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    impl_->writeBarrierArrayConservative(arrayRef, value);
}

bool ManagedHeap::tryBindObjectFastView(ObjectRef reference, ObjectFastView& outView) const noexcept {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    auto* slot = impl_->getSlot(reference);
    if (!slot || slot->kind() != ObjectKind::Record) {
        return false;
    }
    const auto& layout = impl_->typeLayouts[slot->payload.s.layoutId];
    if (layout.typeId == 0) return false;
    if (!impl_->mayContainPoolValues.load(std::memory_order_relaxed)) {
        impl_->noteMaybePoolBackedUnlocked();
    }
    outView.fields = slot->payload.s.data;
    outView.fieldCount = slot->payload.s.length;
    outView.typeId = layout.typeId;
    outView.needsRememberedBarrier =
        (!slot->hasFlag(GcFlag::InNursery) && !slot->hasFlag(GcFlag::InRememberedSet));
    return true;
}

void ManagedHeap::recordObjectWriteBarrier(ObjectRef objectRef, const Value& value) noexcept {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    impl_->writeBarrierObject(objectRef, value);
}

const void* ManagedHeap::slotStorageToken() const noexcept {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    return impl_->slots.data();
}

semantic::SymbolId ManagedHeap::objectTypeIdFast(ObjectRef reference) const noexcept {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    const auto* slot = impl_->getSlot(reference);
    if (!slot || slot->kind() != ObjectKind::Record) {
        return 0;
    }
    return impl_->typeLayouts[slot->payload.s.layoutId].typeId;
}

bool ManagedHeap::arraySetPrevalidated(
    ObjectRef reference,
    std::size_t index,
    const Value& value,
    bool mayBeReference) noexcept {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    auto* slot = impl_->getSlot(reference);
    if (!slot || slot->kind() != ObjectKind::Array) {
        return false;
    }
    if (slot->storage() == SlotStorage::RawColumn) {
        const auto& view = impl_->rawColumns.get(slot->payload.s.handle);
        if (view.readOnly || index >= view.count || !view.basePtr) return false;
        const auto* s = std::get_if<StructValue>(&value);
        if (!s || !s->valid() || s->typeId != view.structTypeId ||
            s->fieldCount() != view.fields.size()) {
            return false;
        }
        auto* elemBase = static_cast<std::uint8_t*>(view.basePtr) + index * view.strideBytes;
        for (std::size_t i = 0; i < view.fields.size(); ++i) {
            const auto fieldVal = structValueFieldAt(*s, i);
            if (!fieldVal || !writeNativeFieldToBytes(elemBase, view.fields[i], *fieldVal)) {
                return false;
            }
        }
        return true;
    }
    if (index >= slot->payload.s.length) {
        return false;
    }
    if (mayBeReference) {
        impl_->writeBarrierArrayElement(reference, index, value);
    }
    impl_->pinHeapValueIfNeededUnlocked(value);
    slot->payload.s.data[index] = value;
    return true;
}

bool ManagedHeap::arraySet(
    ObjectRef reference,
    std::size_t index,
    Value value,
    RuntimeError* error,
    ArrayStoreTypePolicy typePolicy) {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    auto* slot = impl_->getSlot(reference);
    if (!slot || slot->kind() != ObjectKind::Array) {
        setHeapError(error, ErrorCode::InvalidObjectReference,
            "managed array reference is invalid");
        return false;
    }
    if (slot->storage() == SlotStorage::RawColumn) {
        const auto& view = impl_->rawColumns.get(slot->payload.s.handle);
        if (index >= view.count) {
            setHeapError(error, ErrorCode::IndexOutOfRange,
                "managed array index is out of range");
            return false;
        }
        if (view.readOnly || !view.basePtr) {
            setHeapError(error, ErrorCode::CapabilityDenied,
                "cannot write to a read-only native ECS column view");
            return false;
        }
        const auto* s = std::get_if<StructValue>(&value);
        if (!s || !s->valid() || s->typeId != view.structTypeId ||
            s->fieldCount() != view.fields.size()) {
            setHeapError(error, ErrorCode::TypeMismatch,
                "managed array struct element type mismatch for raw ECS column");
            return false;
        }
        auto* elemBase = static_cast<std::uint8_t*>(view.basePtr) + index * view.strideBytes;
        for (std::size_t i = 0; i < view.fields.size(); ++i) {
            const auto fieldVal = structValueFieldAt(*s, i);
            if (!fieldVal || !writeNativeFieldToBytes(elemBase, view.fields[i], *fieldVal)) {
                setHeapError(error, ErrorCode::TypeMismatch,
                    "raw ECS column field type mismatch during in-place write");
                return false;
            }
        }
        return true;
    }
    const auto valueLen = static_cast<std::size_t>(slot->payload.s.length);
    Value* values = slot->payload.s.data;
    if (index >= valueLen) {
        setHeapError(error, ErrorCode::IndexOutOfRange,
            "managed array index is out of range");
        return false;
    }
    const auto layoutId = slot->payload.s.layoutId;
    const auto expected = impl_->typeLayouts[layoutId].elementType;
    const auto expectedTypeId = impl_->typeLayouts[layoutId].elementTypeId;
    if (expected != semantic::PrimitiveType::Error && valueType(value) != expected) {
        setHeapError(error, ErrorCode::TypeMismatch,
            "managed array element type mismatch");
        return false;
    }
    if (expected == semantic::PrimitiveType::Object &&
        !std::holds_alternative<NullObject>(value)) {
        const auto* child = std::get_if<ObjectRef>(&value);
        const auto actual = child ? impl_->objectTypeIdUnlocked(*child) : std::nullopt;
        if (!actual ||
            (typePolicy == ArrayStoreTypePolicy::Exact &&
             *actual != expectedTypeId)) {
            setHeapError(error, ErrorCode::TypeMismatch,
                "managed array object element type mismatch");
            return false;
        }
    } else if (expected == semantic::PrimitiveType::Array &&
               !std::holds_alternative<NullArray>(value)) {
        const auto* child = std::get_if<ObjectRef>(&value);
        const auto actual = child ? impl_->arrayTypeIdUnlocked(*child) : std::nullopt;
        if (!actual || *actual != expectedTypeId) {
            setHeapError(error, ErrorCode::TypeMismatch,
                "managed array nested array type mismatch");
            return false;
        }
    } else if (expected == semantic::PrimitiveType::Struct) {
        const auto actualTypeId = structValueTypeId(value);
        const auto* structure = std::get_if<StructValue>(&value);
        if (!actualTypeId || *actualTypeId != expectedTypeId ||
            (structure && !structure->valid())) {
            setHeapError(error, ErrorCode::TypeMismatch,
                "managed array struct element type mismatch");
            return false;
        }
    } else if (expected == semantic::PrimitiveType::Enum) {
        EnumValue enumeration{};
        if (!value.tryGetEnum(enumeration) ||
            enumeration.typeId != expectedTypeId) {
            setHeapError(error, ErrorCode::TypeMismatch,
                "managed array enum element type mismatch");
            return false;
        }
    } else if (expected == semantic::PrimitiveType::Handle) {
        NativeHandle handle{};
        if (!value.tryGetNativeHandle(handle) ||
            (expectedTypeId != 0 &&
             handle.typeId != expectedTypeId)) {
            setHeapError(error, ErrorCode::TypeMismatch,
                "managed array native handle type mismatch");
            return false;
        }
    }
    impl_->pinHeapValueIfNeededUnlocked(value);
    impl_->writeBarrierArrayElement(reference, index, value);
    values[index] = std::move(value);
    return true;
}

std::optional<semantic::SymbolId> ManagedHeap::objectTypeId(ObjectRef reference) const {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    const auto* slot = impl_->getSlot(reference);
    if (!slot || slot->kind() != ObjectKind::Record) return std::nullopt;
    const auto tid = impl_->typeLayouts[slot->payload.s.layoutId].typeId;
    if (tid == 0) return std::nullopt;
    return tid;
}

std::optional<std::size_t> ManagedHeap::fieldCount(ObjectRef reference) const {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    const auto* slot = impl_->getSlot(reference);
    if (!slot || slot->kind() != ObjectKind::Record) return std::nullopt;
    return static_cast<std::size_t>(slot->payload.s.length);
}

bool ManagedHeap::isInlineRecordObject(ObjectRef reference) const noexcept {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    const auto* slot = impl_->getSlot(reference);
    return slot && slot->kind() == ObjectKind::Record &&
        slot->storage() == SlotStorage::SlabValues;
}

std::optional<Value> ManagedHeap::fieldGet(ObjectRef reference, std::size_t index) const {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    const auto* slot = impl_->getSlot(reference);
    if (!slot || slot->kind() != ObjectKind::Record) return std::nullopt;
    if (index >= slot->payload.s.length) return std::nullopt;
    return slot->payload.s.data[index];
}

const Value* ManagedHeap::fieldGetFast(
    ObjectRef reference,
    std::size_t index,
    semantic::SymbolId* outTypeId) const noexcept {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    const auto* slot = impl_->getSlot(reference);
    if (!slot || slot->kind() != ObjectKind::Record ||
        index >= slot->payload.s.length) {
        return nullptr;
    }
    const auto tid = impl_->typeLayouts[slot->payload.s.layoutId].typeId;
    if (tid == 0) return nullptr;
    if (outTypeId) *outTypeId = tid;
    return &slot->payload.s.data[index];
}

bool ManagedHeap::fieldSet(
    ObjectRef reference,
    std::size_t index,
    Value value,
    RuntimeError* error) {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    auto* slot = impl_->getSlot(reference);
    if (!slot || slot->kind() != ObjectKind::Record) {
        setHeapError(error, ErrorCode::InvalidObjectReference,
            "managed record reference is invalid");
        return false;
    }
    if (index >= slot->payload.s.length) {
        setHeapError(error, ErrorCode::InvalidArguments,
            "managed field index is out of range");
        return false;
    }
    const auto& refFields = impl_->typeLayouts[slot->payload.s.layoutId].referenceFields;
    if (Impl::valueMayContainReferences(value) &&
        !refFields.empty() &&
        std::binary_search(refFields.begin(), refFields.end(), index)) {
        impl_->writeBarrierObject(reference, value);
    }
    impl_->pinHeapValueIfNeededUnlocked(value);
    slot->payload.s.data[index] = std::move(value);
    return true;
}

bool ManagedHeap::fieldSetFast(
    ObjectRef reference,
    std::size_t index,
    const Value& value,
    semantic::SymbolId expectedTypeId,
    semantic::PrimitiveType expectedFieldType) noexcept {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    auto* slot = impl_->getSlot(reference);
    if (!slot || slot->kind() != ObjectKind::Record ||
        index >= slot->payload.s.length ||
        valueType(value) != expectedFieldType) {
        return false;
    }
    const auto& layout = impl_->typeLayouts[slot->payload.s.layoutId];
    if (layout.typeId != expectedTypeId) return false;
    if (const auto* childRef = std::get_if<ObjectRef>(&value);
        childRef && !impl_->getSlot(*childRef)) {
        return false;
    }
    if (Impl::valueMayContainReferences(value) &&
        !layout.referenceFields.empty() &&
        std::binary_search(layout.referenceFields.begin(), layout.referenceFields.end(), index)) {
        impl_->writeBarrierObject(reference, value);
    }
    impl_->pinHeapValueIfNeededUnlocked(value);
    slot->payload.s.data[index] = value;
    return true;
}

ManagedHeap::RootToken ManagedHeap::addPersistentRoot(ObjectRef reference) {
    return addPersistentRoot(Value{reference});
}

ManagedHeap::RootToken ManagedHeap::addPersistentRoot(Value value) {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    if (!impl_->validRootValue(value)) return 0;
    if (impl_->phase != GcPhase::Idle) {
        impl_->markValue(value);
        if (!impl_->markStack.empty()) {
            impl_->reopenMarkFromSweep();
        }
    }
    auto token = impl_->nextRootToken++;
    if (token == 0) token = impl_->nextRootToken++;
    impl_->pinHeapValueIfNeededUnlocked(value);
    impl_->persistentRoots.emplace(token, std::move(value));
    return token;
}

bool ManagedHeap::updatePersistentRoot(
    RootToken token,
    ObjectRef reference) {
    return updatePersistentRoot(token, Value{reference});
}

bool ManagedHeap::updatePersistentRoot(RootToken token, Value value) {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    if (token == 0 || !impl_->validRootValue(value)) return false;
    const auto found = impl_->persistentRoots.find(token);
    if (found == impl_->persistentRoots.end()) return false;
    if (impl_->phase != GcPhase::Idle) {
        impl_->markValue(value);
        if (!impl_->markStack.empty()) {
            impl_->reopenMarkFromSweep();
        }
    }
    impl_->pinHeapValueIfNeededUnlocked(value);
    found->second = std::move(value);
    return true;
}

bool ManagedHeap::removePersistentRoot(RootToken token) noexcept {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    return impl_->persistentRoots.erase(token) != 0;
}

PersistentRoot ManagedHeap::retain(ObjectRef reference) {
    return retain(Value{reference});
}

PersistentRoot ManagedHeap::retain(Value value) {
    const auto token = addPersistentRoot(value);
    return PersistentRoot(token == 0 ? nullptr : this, token, std::move(value));
}

bool ManagedHeap::hasStaticField(semantic::SymbolId id) const noexcept {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    return impl_->staticFields.find(id) != impl_->staticFields.end();
}

std::optional<Value> ManagedHeap::getStaticField(semantic::SymbolId id) const {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    const auto found = impl_->staticFields.find(id);
    if (found == impl_->staticFields.end()) return std::nullopt;
    return found->second;
}

void ManagedHeap::setStaticField(semantic::SymbolId id, Value value) {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    if (impl_->phase != GcPhase::Idle) {
        impl_->markValue(value);
        if (!impl_->markStack.empty()) {
            impl_->reopenMarkFromSweep();
        }
    }
    impl_->pinHeapValueIfNeededUnlocked(value);
    impl_->staticFields[id] = std::move(value);
}

bool ManagedHeap::removeStaticField(semantic::SymbolId id) noexcept {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    return impl_->staticFields.erase(id) != 0;
}

std::vector<semantic::SymbolId> ManagedHeap::staticFieldIds() const {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    std::vector<semantic::SymbolId> ids;
    ids.reserve(impl_->staticFields.size());
    for (const auto& [id, val] : impl_->staticFields) {
        (void)val;
        ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

bool ManagedHeap::setNativeEcsComponent(
    std::string_view canonicalTypeName,
    std::int64_t entityId,
    Value component) {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    if (impl_->phase != GcPhase::Idle) {
        impl_->markValue(component);
        if (!impl_->markStack.empty()) {
            impl_->reopenMarkFromSweep();
        }
    }
    impl_->pinHeapValueIfNeededUnlocked(component);
    auto& table = impl_->nativeEcsTables[std::string(canonicalTypeName)];
    if (Impl::valueMayContainReferences(component)) {
        table.mayContainReferences = true;
    }
    const auto it = std::lower_bound(table.entityIds.begin(), table.entityIds.end(), entityId);
    const auto idx = static_cast<std::size_t>(std::distance(table.entityIds.begin(), it));
    if (it != table.entityIds.end() && *it == entityId) {
        table.values[idx] = std::move(component);
        return true;
    }
    table.entityIds.insert(it, entityId);
    table.values.insert(table.values.begin() + static_cast<std::ptrdiff_t>(idx), std::move(component));
    return true;
}

std::optional<Value> ManagedHeap::getNativeEcsComponent(
    std::string_view canonicalTypeName,
    std::int64_t entityId) const {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    const auto found = impl_->nativeEcsTables.find(std::string(canonicalTypeName));
    if (found == impl_->nativeEcsTables.end()) return std::nullopt;
    const auto& table = found->second;
    const auto it = std::lower_bound(table.entityIds.begin(), table.entityIds.end(), entityId);
    if (it == table.entityIds.end() || *it != entityId) return std::nullopt;
    const auto idx = static_cast<std::size_t>(std::distance(table.entityIds.begin(), it));
    return table.values[idx];
}

bool ManagedHeap::hasNativeEcsComponent(
    std::string_view canonicalTypeName,
    std::int64_t entityId) const noexcept {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    const auto found = impl_->nativeEcsTables.find(std::string(canonicalTypeName));
    if (found == impl_->nativeEcsTables.end()) return false;
    const auto& table = found->second;
    return std::binary_search(table.entityIds.begin(), table.entityIds.end(), entityId);
}

bool ManagedHeap::removeNativeEcsComponent(
    std::string_view canonicalTypeName,
    std::int64_t entityId) {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    const auto found = impl_->nativeEcsTables.find(std::string(canonicalTypeName));
    if (found == impl_->nativeEcsTables.end()) return false;
    auto& table = found->second;
    const auto it = std::lower_bound(table.entityIds.begin(), table.entityIds.end(), entityId);
    if (it == table.entityIds.end() || *it != entityId) return false;
    const auto idx = static_cast<std::size_t>(std::distance(table.entityIds.begin(), it));
    table.entityIds.erase(it);
    table.values.erase(table.values.begin() + static_cast<std::ptrdiff_t>(idx));
    return true;
}

bool ManagedHeap::destroyNativeEcsEntity(std::int64_t entityId) {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    bool removedAny = false;
    for (auto& [typeName, table] : impl_->nativeEcsTables) {
        (void)typeName;
        const auto it = std::lower_bound(table.entityIds.begin(), table.entityIds.end(), entityId);
        if (it != table.entityIds.end() && *it == entityId) {
            const auto idx = static_cast<std::size_t>(std::distance(table.entityIds.begin(), it));
            table.entityIds.erase(it);
            table.values.erase(table.values.begin() + static_cast<std::ptrdiff_t>(idx));
            removedAny = true;
        }
    }
    return removedAny;
}

bool ManagedHeap::hasNativeEcsTable(std::string_view canonicalTypeName) const noexcept {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    const auto found = impl_->nativeEcsTables.find(std::string(canonicalTypeName));
    return found != impl_->nativeEcsTables.end();
}

std::vector<std::string> ManagedHeap::nativeEcsTableNames() const {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    std::vector<std::string> names;
    names.reserve(impl_->nativeEcsTables.size());
    for (const auto& [name, table] : impl_->nativeEcsTables) {
        (void)table;
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

void ManagedHeap::clearNativeEcsTables() noexcept {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    impl_->nativeEcsTables.clear();
}

const std::vector<std::int64_t>* ManagedHeap::nativeEcsEntityIds(
    std::string_view canonicalTypeName) const noexcept {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    const auto found = impl_->nativeEcsTables.find(std::string(canonicalTypeName));
    return found != impl_->nativeEcsTables.end() ? &found->second.entityIds : nullptr;
}

const std::vector<Value>* ManagedHeap::nativeEcsComponentValues(
    std::string_view canonicalTypeName) const noexcept {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    const auto found = impl_->nativeEcsTables.find(std::string(canonicalTypeName));
    return found != impl_->nativeEcsTables.end() ? &found->second.values : nullptr;
}

bool ManagedHeap::setNativeEcsComponentByDenseIndex(
    std::string_view canonicalTypeName,
    std::size_t denseIndex,
    Value component) {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    const auto found = impl_->nativeEcsTables.find(std::string(canonicalTypeName));
    if (found == impl_->nativeEcsTables.end() || denseIndex >= found->second.values.size()) {
        return false;
    }
    if (impl_->phase != GcPhase::Idle) {
        impl_->markValue(component);
        if (!impl_->markStack.empty()) {
            impl_->reopenMarkFromSweep();
        }
    }
    if (Impl::valueMayContainReferences(component)) {
        found->second.mayContainReferences = true;
    }
    impl_->pinHeapValueIfNeededUnlocked(component);
    found->second.values[denseIndex] = std::move(component);
    return true;
}

std::optional<ObjectRef> ManagedHeap::allocateRawColumnArray(
    semantic::SymbolId arrayTypeId,
    semantic::SymbolId elementTypeId,
    NativeRawColumnView view,
    RuntimeError* error) {
    if (!validateRawColumnView(view, elementTypeId)) {
        setHeapError(error, ErrorCode::InvalidArguments,
            "raw ECS column view metadata is invalid");
        return std::nullopt;
    }
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    const std::uint32_t layoutId = impl_->internArrayLayout(
        arrayTypeId, semantic::PrimitiveType::Struct, elementTypeId);
    const auto slotIndex = impl_->acquireSlot(Impl::BaseObjectSizeBytes, error);
    if (!slotIndex) return std::nullopt;
    auto& slot = impl_->slots[*slotIndex];
    const std::uint32_t rawHandle = impl_->rawColumns.acquire(std::move(view));
    slot.auxMeta = 0;
    slot.payload.s = {layoutId, 0u, nullptr, rawHandle, CompactSlot::kInvalidHandle};
    return impl_->finalizeSlotAllocation(
        *slotIndex,
        ObjectKind::Array,
        SlotStorage::RawColumn,
        Impl::BaseObjectSizeBytes);
}

bool ManagedHeap::updateRawColumnArray(
    ObjectRef reference,
    NativeRawColumnView view) noexcept {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    auto* slot = impl_->getSlot(reference);
    if (!slot || slot->kind() != ObjectKind::Array ||
        slot->storage() != SlotStorage::RawColumn) {
        return false;
    }
    const auto elementTypeId = impl_->typeLayouts[slot->payload.s.layoutId].elementTypeId;
    if (!validateRawColumnView(view, elementTypeId)) {
        return false;
    }
    impl_->rawColumns.get(slot->payload.s.handle) = std::move(view);
    return true;
}

bool ManagedHeap::isRawColumnArray(ObjectRef reference) const noexcept {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    const auto* slot = impl_->getSlot(reference);
    return slot && slot->kind() == ObjectKind::Array &&
        slot->storage() == SlotStorage::RawColumn;
}

bool ManagedHeap::hasSparseEcsState() const noexcept {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    return !impl_->nativeEcsTables.empty() || impl_->hasSparseEcsStaticFields;
}

void ManagedHeap::notifySparseEcsState() noexcept {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    impl_->hasSparseEcsStaticFields = true;
}

const NativeRawColumnView* ManagedHeap::rawColumnView(ObjectRef reference) const noexcept {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    const auto* slot = impl_->getSlot(reference);
    if (!slot || slot->kind() != ObjectKind::Array ||
        slot->storage() != SlotStorage::RawColumn) {
        return nullptr;
    }
    return &impl_->rawColumns.get(slot->payload.s.handle);
}

bool ManagedHeap::readRawColumnFields(
    ObjectRef reference,
    std::size_t index,
    Value* outFields,
    std::size_t maxFields,
    std::uint8_t& outFieldCount) const noexcept {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    const auto* slot = impl_->getSlot(reference);
    if (!slot || slot->kind() != ObjectKind::Array ||
        slot->storage() != SlotStorage::RawColumn) {
        return false;
    }
    const auto& view = impl_->rawColumns.get(slot->payload.s.handle);
    if (index >= view.count || !view.basePtr || view.fields.size() > maxFields) {
        return false;
    }
    const auto* elemBase = static_cast<const std::uint8_t*>(view.basePtr) + index * view.strideBytes;
    const auto count = view.fields.size();
    for (std::size_t i = 0; i < count; ++i) {
        outFields[i] = readNativeFieldFromBytes(elemBase, view.fields[i]);
    }
    outFieldCount = static_cast<std::uint8_t>(count);
    return true;
}

bool ManagedHeap::writeRawColumnFields(
    ObjectRef reference,
    std::size_t index,
    const Value* fields,
    std::size_t fieldCount,
    RuntimeError* error) noexcept {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    auto* slot = impl_->getSlot(reference);
    if (!slot || slot->kind() != ObjectKind::Array ||
        slot->storage() != SlotStorage::RawColumn) {
        return false;
    }
    const auto& view = impl_->rawColumns.get(slot->payload.s.handle);
    if (index >= view.count) {
        setHeapError(error, ErrorCode::IndexOutOfRange,
            "array element index is out of range");
        return false;
    }
    if (view.readOnly || !view.basePtr) {
        setHeapError(error, ErrorCode::CapabilityDenied,
            "cannot write to a read-only native ECS column view");
        return false;
    }
    if (fieldCount != view.fields.size()) {
        setHeapError(error, ErrorCode::TypeMismatch,
            "raw ECS column field count mismatch");
        return false;
    }
    auto* elemBase = static_cast<std::uint8_t*>(view.basePtr) + index * view.strideBytes;
    for (std::size_t i = 0; i < fieldCount; ++i) {
        if (!writeNativeFieldToBytes(elemBase, view.fields[i], fields[i])) {
            setHeapError(error, ErrorCode::TypeMismatch,
                "raw ECS column field type mismatch");
            return false;
        }
    }
    return true;
}

void ManagedHeap::requestCollection() noexcept {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    impl_->requested = true;
}
bool ManagedHeap::collectionRequested() const noexcept {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    return impl_->requested;
}
GcPhase ManagedHeap::phase() const noexcept {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    return impl_->phase;
}

std::size_t ManagedHeap::step(
    const ShadowStack& shadowStack,
    std::size_t workBudget) {
    bool shouldSweepPools = false;
    std::size_t performed = 0;
    {
        HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
        performed = impl_->stepUnlocked(shadowStack, workBudget, shouldSweepPools);
    }
    if (shouldSweepPools) {
        (void)reclaimPoolValuesInternal(&shadowStack, /*waitIfBusy=*/false);
    }
    return performed;
}

std::size_t ManagedHeap::collectIncremental(
    const RootSet& roots,
    std::size_t workBudget) {
    return step(roots, workBudget);
}

IncrementalGcBudgetStats ManagedHeap::collectIncrementalWithBudget(
    const RootSet& roots,
    std::chrono::nanoseconds budget,
    std::size_t checkInterval,
    bool forceStartIfIdle) {
    const auto startTime = std::chrono::steady_clock::now();
    bool shouldSweepPools = false;
    IncrementalGcBudgetStats stats{};
    {
        HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
        stats = impl_->collectIncrementalWithBudgetUnlocked(
            roots, budget, checkInterval, forceStartIfIdle, startTime, shouldSweepPools);
    }
    if (shouldSweepPools) {
        (void)reclaimPoolValuesInternal(&roots, /*waitIfBusy=*/false);
    }
    const auto endTime = std::chrono::steady_clock::now();
    stats.elapsed = detail::clampDurationToNanoseconds(endTime - startTime);
    stats.elapsedNanoseconds = static_cast<std::uint64_t>(
        std::max<std::chrono::nanoseconds::rep>(0, stats.elapsed.count()));
    return stats;
}

IncrementalGcBudgetStats ManagedHeap::collectIncrementalWithBudget(
    const RootSet& roots,
    std::chrono::microseconds budget,
    std::size_t checkInterval,
    bool forceStartIfIdle) {
    return collectIncrementalWithBudget(
        roots,
        detail::clampDurationToNanoseconds(budget),
        checkInterval,
        forceStartIfIdle);
}

IncrementalGcBudgetStats ManagedHeap::collectIncrementalWithBudgetUs(
    const RootSet& roots,
    std::uint64_t budgetMicroseconds,
    std::size_t checkInterval,
    bool forceStartIfIdle) {
    constexpr auto kMaxUs = static_cast<std::uint64_t>(
        std::chrono::nanoseconds::max().count() / 1000);
    const auto clampedNs = (budgetMicroseconds >= kMaxUs)
        ? std::chrono::nanoseconds::max()
        : std::chrono::nanoseconds(static_cast<std::chrono::nanoseconds::rep>(budgetMicroseconds * 1000ULL));
    return collectIncrementalWithBudget(roots, clampedNs, checkInterval, forceStartIfIdle);
}

bool ManagedHeap::isWallClockCycleActive() const noexcept {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    return impl_->wallClockCycleActive && impl_->phase != GcPhase::Idle;
}

void ManagedHeap::finishWallClockCycleForDeterminism(const ShadowStack& shadowStack) {
    bool shouldSweepPools = false;
    {
        HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
        if (!impl_->wallClockCycleActive || impl_->phase == GcPhase::Idle) {
            impl_->wallClockCycleActive = false;
            return;
        }
        while (impl_->phase != GcPhase::Idle) {
            const auto budget = std::max<std::size_t>(64, impl_->slots.size() + 1);
            (void)impl_->stepUnlocked(shadowStack, budget, shouldSweepPools);
        }
        impl_->wallClockCycleActive = false;
    }
    if (shouldSweepPools) {
        (void)reclaimPoolValuesInternal(&shadowStack, /*waitIfBusy=*/false);
    }
}

void ManagedHeap::collectFull(const ShadowStack& shadowStack) {
    bool shouldSweepPools = false;
    {
        HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
        const auto drainUnlocked = [&]() {
            while (impl_->phase != GcPhase::Idle || impl_->requested) {
                const auto budget = std::max<std::size_t>(64, impl_->slots.size() + 1);
                (void)impl_->stepUnlocked(shadowStack, budget, shouldSweepPools);
            }
        };
        if (impl_->phase != GcPhase::Idle) drainUnlocked();
        impl_->requested = true;
        drainUnlocked();
    }
    if (shouldSweepPools) {
        (void)reclaimPoolValuesInternal(&shadowStack, /*waitIfBusy=*/false);
    }
}

void ManagedHeap::collect(const RootSet& roots) {
    collectFull(roots);
}

std::size_t ManagedHeap::nurserySize() const noexcept {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    return impl_->nurserySlots.size();
}

std::size_t ManagedHeap::collectYoung(
    const ShadowStack& shadowStack,
    std::size_t workBudget) {
    bool shouldSweepPools = false;
    std::size_t stepResult = 0;
    bool handledIncremental = false;
    {
        HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
        if (impl_->phase != GcPhase::Idle) {
            handledIncremental = true;
            if (workBudget != 0) {
                stepResult = impl_->stepUnlocked(shadowStack, workBudget, shouldSweepPools);
            } else {
                while (impl_->phase != GcPhase::Idle) {
                    const auto budget = std::max<std::size_t>(64, impl_->slots.size() + 1);
                    stepResult += impl_->stepUnlocked(shadowStack, budget, shouldSweepPools);
                }
            }
        } else {
            if (impl_->nurserySlots.empty()) return 0;

    for (const auto slotIdx : impl_->nurserySlots) {
        if (slotIdx < impl_->slots.size() && impl_->slots[slotIdx].occupied()) {
            impl_->slots[slotIdx].gcFlags &= static_cast<std::uint8_t>(~GcFlag::Marked);
        }
    }

    auto& youngQueue = impl_->youngWorkQueue;
    youngQueue.clear();
    std::optional<std::unordered_set<const StructStorage*>> visitedStructs;
    const auto markYoungValue = [&](const auto& self, const Value& val) -> void {
        if (!val.isTraceable()) return;
        const auto t = val.rawTag();
        if (t == ValueTag::ObjectRef) {
            const auto& ref = val.asObjectRefUnchecked();
            auto* slot = impl_->getSlot(ref);
            if (slot && slot->hasFlag(GcFlag::InNursery) && !slot->hasFlag(GcFlag::Marked)) {
                slot->gcFlags |= GcFlag::Marked;
                youngQueue.push_back(ref);
            }
            return;
        }
        if (t == ValueTag::Struct) {
            const auto& structure = val.asStructUnchecked();
            if (!structure.isInline && structure.storage) {
                if (!visitedStructs.has_value()) visitedStructs.emplace();
                if (visitedStructs->insert(structure.storage.get()).second) {
                    for (const auto& field : structure.storage->fields) {
                        self(self, field);
                    }
                }
            }
        }
    };

    shadowStack.visitTraceableRoots([&](const Value& val) { markYoungValue(markYoungValue, val); });
    for (const auto& entry : impl_->persistentRoots) markYoungValue(markYoungValue, entry.second);
    for (const auto& entry : impl_->staticFields) markYoungValue(markYoungValue, entry.second);
    for (const auto& [typeName, table] : impl_->nativeEcsTables) {
        (void)typeName;
        if (!table.mayContainReferences) continue;
        for (const auto& v : table.values) markYoungValue(markYoungValue, v);
    }

    // Card-Table-accelerated Remembered Set scan:
    for (const auto& oldRef : impl_->rememberedSet) {
        auto* oldSlot = impl_->getSlot(oldRef);
        if (!oldSlot || oldSlot->kind() == ObjectKind::String) continue;
        const auto count = Impl::slotValueCount(*oldSlot);
        if (count == 0) continue;
        const Value* data = oldSlot->payload.s.data;
        const auto& layout = impl_->typeLayouts[oldSlot->payload.s.layoutId];

        if (oldSlot->kind() == ObjectKind::Array) {
            if (!layout.mayContainReferences) continue;
            if (oldSlot->hasFlag(GcFlag::HasCardTable)) {
                auto& slice = impl_->cardArena.get(oldSlot->payload.s.cardHandle);
                for (std::uint32_t w = 0; w < slice.wordCount; ++w) {
                    if (slice.words[w] == 0) continue; // Skip 8 clean cards = 512 Values!
                    auto* cardBytes = reinterpret_cast<std::uint8_t*>(&slice.words[w]);
                    for (std::uint32_t b = 0; b < 8; ++b) {
                        if (cardBytes[b] == 0) continue;
                        cardBytes[b] = 0;
                        const std::size_t cardIdx = (static_cast<std::size_t>(w) << 3) + b;
                        const std::size_t begin = cardIdx << ManagedHeap::kCardElementShift;
                        const std::size_t end = std::min(count, begin + ManagedHeap::kCardElements);
                        for (std::size_t i = begin; i < end; ++i) {
                            markYoungValue(markYoungValue, data[i]);
                        }
                    }
                }
                continue;
            }
            for (std::size_t i = 0; i < count; ++i) {
                markYoungValue(markYoungValue, data[i]);
            }
        } else {
            for (const auto idx : layout.referenceFields) {
                if (idx < count) markYoungValue(markYoungValue, data[idx]);
            }
        }
    }

    while (!youngQueue.empty()) {
        const auto ref = youngQueue.back();
        youngQueue.pop_back();
        const auto* slot = impl_->getSlot(ref);
        if (!slot || slot->kind() == ObjectKind::String) continue;
        const auto count = Impl::slotValueCount(*slot);
        if (count != 0) {
            const Value* data = slot->payload.s.data;
            const auto& layout = impl_->typeLayouts[slot->payload.s.layoutId];
            if (slot->kind() == ObjectKind::Array) {
                if (layout.mayContainReferences) {
                    for (std::size_t i = 0; i < count; ++i) markYoungValue(markYoungValue, data[i]);
                }
            } else {
                for (const auto idx : layout.referenceFields) {
                    if (idx < count) markYoungValue(markYoungValue, data[idx]);
                }
            }
        }
        ++impl_->statistics.markWork;
    }

    std::size_t processed = 0;
    for (const auto slotIdx : impl_->nurserySlots) {
        if (slotIdx >= impl_->slots.size()) continue;
        auto& slot = impl_->slots[slotIdx];
        if (!slot.occupied() || !slot.hasFlag(GcFlag::InNursery)) continue;
        ++processed;
        if (!slot.hasFlag(GcFlag::Marked)) {
            impl_->sweepReclaimSlot(slotIdx, slot, true);
        } else {
            impl_->promoteSlotToOldGen(slot);
        }
        ++impl_->statistics.sweepWork;
    }

    impl_->nurserySlots.clear();
    for (const auto& ref : impl_->rememberedSet) {
        if (auto* slot = impl_->getSlot(ref)) {
            slot->gcFlags &= static_cast<std::uint8_t>(~GcFlag::InRememberedSet);
        }
    }
    impl_->rememberedSet.clear();
            ++impl_->statistics.youngCollections;
            if (impl_->statistics.liveBytes < impl_->nextCollectionThreshold) {
                impl_->requested = false;
            }
            return processed;
        }
    }
    if (handledIncremental && shouldSweepPools) {
        (void)reclaimPoolValuesInternal(&shadowStack, /*waitIfBusy=*/false);
    }
    return stepResult;
}

bool ManagedHeap::readPackedIntArray(
    ObjectRef reference,
    std::vector<std::int64_t>& out,
    RuntimeError* error) const {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    const auto* slot = impl_->getSlot(reference);
    if (!slot || slot->kind() != ObjectKind::Array) {
        setHeapError(error, ErrorCode::InvalidObjectReference, "managed array reference is invalid");
        return false;
    }
    const auto len = Impl::slotValueCount(*slot);
    const Value* values = slot->payload.s.data;
    out.resize(len);
    for (std::size_t i = 0; i < len; ++i) {
        if (const auto* iv = std::get_if<std::int64_t>(&values[i])) {
            out[i] = *iv;
        } else if (const auto* lv = std::get_if<LongValue>(&values[i])) {
            out[i] = lv->value;
        } else {
            setHeapError(error, ErrorCode::TypeMismatch, "array element is not an integer");
            return false;
        }
    }
    return true;
}

bool ManagedHeap::writePackedIntArray(
    ObjectRef reference,
    const std::int64_t* data,
    std::size_t count,
    RuntimeError* error) {
    if (count > 0 && !data) {
        setHeapError(error, ErrorCode::InvalidArguments, "bulk write source buffer is null");
        return false;
    }
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    auto* slot = impl_->getSlot(reference);
    if (!slot || slot->kind() != ObjectKind::Array) {
        setHeapError(error, ErrorCode::InvalidObjectReference, "managed array reference is invalid");
        return false;
    }
    const auto elemType = impl_->typeLayouts[slot->payload.s.layoutId].elementType;
    if (elemType != semantic::PrimitiveType::Int &&
        elemType != semantic::PrimitiveType::Long &&
        elemType != semantic::PrimitiveType::Error) {
        setHeapError(error, ErrorCode::TypeMismatch, "array element is not an integer");
        return false;
    }
    const auto len = Impl::slotValueCount(*slot);
    if (count > len) {
        setHeapError(error, ErrorCode::IndexOutOfRange, "bulk write exceeds array length");
        return false;
    }
    Value* values = slot->payload.s.data;
    const bool asLong = elemType == semantic::PrimitiveType::Long;
    for (std::size_t i = 0; i < count; ++i) {
        values[i] = asLong ? Value{LongValue{data[i]}} : Value{data[i]};
    }
    return true;
}

bool ManagedHeap::readPackedDoubleArray(
    ObjectRef reference,
    std::vector<double>& out,
    RuntimeError* error) const {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    const auto* slot = impl_->getSlot(reference);
    if (!slot || slot->kind() != ObjectKind::Array) {
        setHeapError(error, ErrorCode::InvalidObjectReference, "managed array reference is invalid");
        return false;
    }
    const auto len = Impl::slotValueCount(*slot);
    const Value* values = slot->payload.s.data;
    out.resize(len);
    for (std::size_t i = 0; i < len; ++i) {
        if (const auto* dv = std::get_if<double>(&values[i])) {
            out[i] = *dv;
        } else if (const auto* fv = std::get_if<FloatValue>(&values[i])) {
            out[i] = static_cast<double>(fv->value);
        } else {
            setHeapError(error, ErrorCode::TypeMismatch, "array element is not a floating-point number");
            return false;
        }
    }
    return true;
}

bool ManagedHeap::writePackedDoubleArray(
    ObjectRef reference,
    const double* data,
    std::size_t count,
    RuntimeError* error) {
    if (count > 0 && !data) {
        setHeapError(error, ErrorCode::InvalidArguments, "bulk write source buffer is null");
        return false;
    }
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    auto* slot = impl_->getSlot(reference);
    if (!slot || slot->kind() != ObjectKind::Array) {
        setHeapError(error, ErrorCode::InvalidObjectReference, "managed array reference is invalid");
        return false;
    }
    const auto elemType = impl_->typeLayouts[slot->payload.s.layoutId].elementType;
    if (elemType != semantic::PrimitiveType::Double &&
        elemType != semantic::PrimitiveType::Float &&
        elemType != semantic::PrimitiveType::Error) {
        setHeapError(error, ErrorCode::TypeMismatch, "array element is not a floating-point number");
        return false;
    }
    const auto len = Impl::slotValueCount(*slot);
    if (count > len) {
        setHeapError(error, ErrorCode::IndexOutOfRange, "bulk write exceeds array length");
        return false;
    }
    Value* values = slot->payload.s.data;
    const bool asFloat = elemType == semantic::PrimitiveType::Float;
    for (std::size_t i = 0; i < count; ++i) {
        values[i] = asFloat ? Value{FloatValue{static_cast<float>(data[i])}} : Value{data[i]};
    }
    return true;
}

bool ManagedHeap::readPackedVec2Array(
    ObjectRef reference,
    std::vector<Vec2Value>& out,
    RuntimeError* error) const {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    const auto* slot = impl_->getSlot(reference);
    if (!slot || slot->kind() != ObjectKind::Array) {
        setHeapError(error, ErrorCode::InvalidObjectReference, "managed array reference is invalid");
        return false;
    }
    const auto len = Impl::slotValueCount(*slot);
    const Value* values = slot->payload.s.data;
    out.resize(len);
    for (std::size_t i = 0; i < len; ++i) {
        if (const auto* v2 = std::get_if<Vec2Value>(&values[i])) {
            out[i] = *v2;
        } else {
            const auto fx = loadInlineStructField(values[i], 0);
            const auto fy = loadInlineStructField(values[i], 1);
            if (fx && fy && std::holds_alternative<FixedValue>(*fx) && std::holds_alternative<FixedValue>(*fy)) {
                out[i] = Vec2Value{std::get<FixedValue>(*fx).raw, std::get<FixedValue>(*fy).raw};
            } else {
                setHeapError(error, ErrorCode::TypeMismatch, "array element is not a Vec2");
                return false;
            }
        }
    }
    return true;
}

bool ManagedHeap::writePackedVec2Array(
    ObjectRef reference,
    const Vec2Value* data,
    std::size_t count,
    RuntimeError* error) {
    if (count > 0 && !data) {
        setHeapError(error, ErrorCode::InvalidArguments, "bulk write source buffer is null");
        return false;
    }
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    auto* slot = impl_->getSlot(reference);
    if (!slot || slot->kind() != ObjectKind::Array) {
        setHeapError(error, ErrorCode::InvalidObjectReference, "managed array reference is invalid");
        return false;
    }
    const auto& layout = impl_->typeLayouts[slot->payload.s.layoutId];
    if (layout.elementType != semantic::PrimitiveType::Error &&
        !(layout.elementType == semantic::PrimitiveType::Struct &&
          layout.elementTypeId == vec2TypeId())) {
        setHeapError(error, ErrorCode::TypeMismatch, "array element is not a Vec2");
        return false;
    }
    const auto len = Impl::slotValueCount(*slot);
    if (count > len) {
        setHeapError(error, ErrorCode::IndexOutOfRange, "bulk write exceeds array length");
        return false;
    }
    Value* values = slot->payload.s.data;
    for (std::size_t i = 0; i < count; ++i) {
        values[i] = Value{data[i]};
    }
    return true;
}

const GcStatistics& ManagedHeap::statistics() const noexcept {
    return impl_->statistics;
}
std::size_t ManagedHeap::liveObjects() const noexcept {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    return impl_->statistics.liveObjects;
}
std::size_t ManagedHeap::liveBytes() const noexcept {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    return impl_->statistics.liveBytes;
}

std::uint64_t ManagedHeap::heapId() const noexcept { return impl_->heapId; }

HeapSnapshot ManagedHeap::Impl::snapshotUnlocked(const ShadowStack* shadowStack) const {
    HeapSnapshot result;
    result.heapId = heapId;
    result.statistics = statistics;
    result.objects.reserve(statistics.liveObjects);
    for (std::size_t slotIndex = 0; slotIndex < slots.size(); ++slotIndex) {
        const auto& slot = slots[slotIndex];
        if (!slot.occupied()) continue;
        HeapObjectInfo info;
        info.reference = {
            static_cast<std::uint32_t>(slotIndex),
            slot.generation,
            slot.kind(),
            heapId,
        };
        info.kind = slot.kind();
        info.sizeBytes = slotSizeBytes(static_cast<std::uint32_t>(slotIndex), slot);
        if (slot.kind() == ObjectKind::String) {
            info.stringValue = std::string(slotStringView(slot));
        } else {
            const auto& layout = typeLayouts[slot.payload.s.layoutId];
            info.typeId = layout.typeId;
            info.elementType = layout.elementType;
            info.elementTypeId = layout.elementTypeId;
            const auto count = Impl::slotValueCount(slot);
            info.valueCount = count;
            info.values.resize(count);
            for (std::size_t i = 0; i < count; ++i) {
                info.values[i] = slot.payload.s.data[i];
            }
            const auto& values = info.values;
            if (!layout.referenceFields.empty()) {
                info.referenceFields = layout.referenceFields;
            }
            const auto addEdge = [&](std::size_t index) {
                if (index >= values.size()) return;
                std::unordered_set<const StructStorage*> visitedStructs;
                const auto addValueEdges = [&](const auto& self,
                                               const Value& value,
                                               const std::string& label) -> void {
                    if (const auto* target = std::get_if<ObjectRef>(&value);
                        target && getSlot(*target)) {
                        info.edges.push_back({*target, label});
                        return;
                    }
                    if (const auto* structure = std::get_if<StructValue>(&value);
                        structure && structure->storage &&
                        visitedStructs.insert(structure->storage.get()).second) {
                        for (std::size_t field = 0;
                             field < structure->storage->fields.size(); ++field) {
                            self(self, structure->storage->fields[field],
                                label + ".struct[" + std::to_string(field) + "]");
                        }
                    }
                };
                addValueEdges(addValueEdges, values[index],
                    (slot.kind() == ObjectKind::Array ? "element[" : "field[") +
                        std::to_string(index) + "]");
            };
            if (slot.kind() == ObjectKind::Array) {
                for (std::size_t index = 0; index < values.size(); ++index) {
                    addEdge(index);
                }
            } else {
                for (const auto index : layout.referenceFields) addEdge(index);
            }
        }
        result.objects.push_back(std::move(info));
    }
    const auto appendRoots = [&](const Value& value,
                                 std::uint64_t token,
                                 const std::string& kind) {
        std::unordered_set<const StructStorage*> visitedStructs;
        const auto addRoots = [&](const auto& self, const Value& current) -> void {
            if (const auto* reference = std::get_if<ObjectRef>(&current);
                reference && getSlot(*reference)) {
                result.roots.push_back({token, *reference, kind});
                return;
            }
            if (const auto* structure = std::get_if<StructValue>(&current);
                structure && structure->storage &&
                visitedStructs.insert(structure->storage.get()).second) {
                for (const auto& field : structure->storage->fields) {
                    self(self, field);
                }
            }
        };
        addRoots(addRoots, value);
    };
    for (const auto& [token, value] : persistentRoots) {
        appendRoots(value, token, "persistent");
    }
    for (const auto& [symId, value] : staticFields) {
        appendRoots(value, symId, "static");
    }
    {
        std::uint64_t ecsRootIndex = 0;
        for (const auto& [typeName, table] : nativeEcsTables) {
            (void)typeName;
            if (!table.mayContainReferences) continue;
            for (const auto& v : table.values) {
                appendRoots(v, ++ecsRootIndex, "ecs");
            }
        }
    }
    if (shadowStack) {
        std::uint64_t index = 0;
        shadowStack->visitRoots([&](const Value& value) {
            appendRoots(value, index++, "shadow");
        });
    }
    std::sort(result.objects.begin(), result.objects.end(),
        [](const HeapObjectInfo& left, const HeapObjectInfo& right) {
            return left.reference.slot < right.reference.slot;
        });
    std::sort(result.roots.begin(), result.roots.end(),
        [](const HeapRootInfo& left, const HeapRootInfo& right) {
            if (left.kind != right.kind) return left.kind < right.kind;
            if (left.token != right.token) return left.token < right.token;
            return left.reference.slot < right.reference.slot;
        });
    return result;
}

HeapSnapshot ManagedHeap::snapshot(const ShadowStack* shadowStack) const {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    return impl_->snapshotUnlocked(shadowStack);
}

bool ManagedHeap::restore(
    const HeapSnapshot& snapshot,
    RuntimeError* error) {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    if (snapshot.heapId != impl_->heapId) {
        setHeapError(error, ErrorCode::InvalidObjectReference,
            "heap snapshot belongs to a different managed heap");
        return false;
    }
    if (impl_->phase != GcPhase::Idle) {
        setHeapError(error, ErrorCode::InvalidProgram,
            "heap snapshot cannot be restored during collection");
        return false;
    }

    std::size_t slotCount = 0;
    std::size_t liveBytes = 0;
    std::unordered_map<std::uint32_t, const HeapObjectInfo*> infos;
    for (const auto& info : snapshot.objects) {
        if (!info.reference.valid() ||
            info.reference.heapId != static_cast<std::uint32_t>(impl_->heapId) ||
            info.reference.kind != info.kind ||
            info.reference.slot == ObjectRef::InvalidSlot ||
            info.sizeBytes > impl_->config.maximumHeapBytes ||
            liveBytes > impl_->config.maximumHeapBytes - info.sizeBytes) {
            setHeapError(error, ErrorCode::InvalidProgram,
                "heap snapshot contains invalid object metadata");
            return false;
        }
        if (!infos.emplace(info.reference.slot, &info).second) {
            setHeapError(error, ErrorCode::InvalidProgram,
                "heap snapshot contains duplicate object slots");
            return false;
        }
        if (info.kind == ObjectKind::String) {
            if (!info.values.empty() || info.valueCount != 0) {
                setHeapError(error, ErrorCode::InvalidProgram,
                    "heap snapshot string payload is invalid");
                return false;
            }
        } else {
            if (info.values.size() != info.valueCount) {
                setHeapError(error, ErrorCode::InvalidProgram,
                    "heap snapshot value count is invalid");
                return false;
            }
            for (const auto field : info.referenceFields) {
                if (field >= info.values.size()) {
                    setHeapError(error, ErrorCode::InvalidProgram,
                        "heap snapshot reference-field map is invalid");
                    return false;
                }
            }
        }
        liveBytes += info.sizeBytes;
        slotCount = std::max(
            slotCount,
            static_cast<std::size_t>(info.reference.slot) + 1u);
    }

    const auto validValue = [&](const auto& self,
                                const Value& value,
                                std::unordered_set<const StructStorage*>& visited) -> bool {
        if (const auto* reference = std::get_if<ObjectRef>(&value)) {
            const auto found = infos.find(reference->slot);
            return reference->heapId == static_cast<std::uint32_t>(impl_->heapId) &&
                found != infos.end() &&
                found->second->reference == *reference;
        }
        if (const auto* structure = std::get_if<StructValue>(&value);
            structure && structure->storage &&
            visited.insert(structure->storage.get()).second) {
            for (const auto& field : structure->storage->fields) {
                if (!self(self, field, visited)) return false;
            }
        }
        return true;
    };
    for (const auto& info : snapshot.objects) {
        for (const auto& value : info.values) {
            std::unordered_set<const StructStorage*> visited;
            if (!validValue(validValue, value, visited)) {
                setHeapError(error, ErrorCode::InvalidProgram,
                    "heap snapshot contains a dangling object reference");
                return false;
            }
        }
    }

    std::unordered_map<RootToken, Value> roots;
    RootToken nextRootToken = 1;
    for (const auto& root : snapshot.roots) {
        if (root.kind != "persistent") continue;
        const auto found = infos.find(root.reference.slot);
        if (root.token == 0 || found == infos.end() ||
            found->second->reference != root.reference ||
            !roots.emplace(root.token, root.reference).second) {
            setHeapError(error, ErrorCode::InvalidProgram,
                "heap snapshot contains invalid persistent roots");
            return false;
        }
        nextRootToken = std::max(nextRootToken, root.token + 1u);
    }

    // All validation passed; rebuild pools and slots cleanly
    impl_->slabPool.clear();
    impl_->largeBuffers.clear();
    impl_->stringPool.clear();
    impl_->rawColumns.clear();
    impl_->cardArena.clear();
    impl_->customSlotSizes.clear();
    impl_->typeLayouts.assign(1, TypeLayoutEntry{});
    impl_->sharedRecordLayoutByTypeId.clear();
    impl_->arrayLayoutByKey.clear();
    impl_->untypedDenseRecordLayouts.clear();
    impl_->untypedCustomRecordLayouts.clear();
    impl_->lastRegisteredLayoutTypeId = 0;
    impl_->lastRegisteredLayoutId = 0;
    impl_->hasLastArrayLayout = false;
    impl_->lastArrayLayoutKey = {};
    impl_->lastArrayLayoutId = 0;
    impl_->slots.assign(slotCount, CompactSlot{});

    for (const auto& info : snapshot.objects) {
        const std::uint32_t slotIdx = info.reference.slot;
        auto& slot = impl_->slots[slotIdx];
        slot.generation = info.reference.generation;
        slot.gcFlags = 0; // Restored objects reside in Old Gen (inNursery = false)

        if (info.kind == ObjectKind::String) {
            if (info.stringValue.size() <= CompactSlot::kMaxSsoBytes) {
                slot.setKindAndStorage(ObjectKind::String, SlotStorage::SsoString);
                slot.auxMeta = static_cast<std::uint16_t>(info.stringValue.size());
                if (!info.stringValue.empty()) {
                    std::memcpy(slot.payload.ssoChars.data(), info.stringValue.data(), info.stringValue.size());
                }
                slot.payload.ssoChars[info.stringValue.size()] = '\0';
            } else {
                slot.setKindAndStorage(ObjectKind::String, SlotStorage::HeapString);
                slot.auxMeta = 0;
                std::string copy = info.stringValue;
                const auto len = static_cast<std::uint32_t>(copy.size());
                const std::uint32_t strHandle = impl_->stringPool.acquire(std::move(copy));
                slot.payload.s = {0u, len, nullptr, strHandle, CompactSlot::kInvalidHandle};
            }
        } else {
            std::uint32_t layoutId = 0;
            if (info.kind == ObjectKind::Array) {
                layoutId = impl_->internArrayLayout(info.typeId, info.elementType, info.elementTypeId);
            } else if (info.typeId != 0) {
                layoutId = impl_->internTypedRecordLayout(info.typeId, info.referenceFields);
            } else {
                layoutId = impl_->internSnapshotUntypedRecordLayout(info.referenceFields);
            }

            const std::size_t count = info.values.size();
            if (count <= CompactSlot::kInlineValueCapacity) {
                std::uint16_t cls = 0;
                std::uint32_t handle = CompactSlot::kInvalidHandle;
                Value* ptr = impl_->slabPool.allocate(count, cls, handle);
                for (std::size_t fi = 0; fi < count; ++fi) {
                    ptr[fi] = info.values[fi];
                }
                slot.setKindAndStorage(info.kind, SlotStorage::SlabValues);
                slot.auxMeta = cls;
                slot.payload.s = {
                    layoutId,
                    static_cast<std::uint32_t>(count),
                    ptr,
                    handle,
                    CompactSlot::kInvalidHandle,
                };
            } else {
                Value* ptr = nullptr;
                const std::uint32_t handle = impl_->largeBuffers.acquireCopy(info.values.data(), count, ptr);
                std::uint32_t cardHandle = CompactSlot::kInvalidHandle;
                if (info.kind == ObjectKind::Array &&
                    count >= ManagedHeap::kCardTableMinLength &&
                    impl_->typeLayouts[layoutId].mayContainReferences) {
                    cardHandle = impl_->cardArena.allocate(count);
                    slot.gcFlags |= GcFlag::HasCardTable;
                }
                slot.setKindAndStorage(info.kind, SlotStorage::HeapValues);
                slot.auxMeta = 0;
                slot.payload.s = {
                    layoutId,
                    static_cast<std::uint32_t>(count),
                    ptr,
                    handle,
                    cardHandle,
                };
            }
        }

        const std::size_t defaultSize = impl_->slotSizeBytes(slotIdx, slot);
        if (info.sizeBytes != defaultSize) {
            slot.gcFlags |= GcFlag::CustomSize;
            impl_->customSlotSizes[slotIdx] = info.sizeBytes;
        }
    }

    std::vector<std::uint32_t> freeSlots;
    for (std::size_t index = 0; index < impl_->slots.size(); ++index) {
        const auto& slot = impl_->slots[index];
        if (!slot.occupied()) {
            freeSlots.push_back(static_cast<std::uint32_t>(index));
        } else {
            const auto count = Impl::slotValueCount(slot);
            for (std::size_t fi = 0; fi < count; ++fi) {
                impl_->pinHeapValueIfNeededUnlocked(slot.payload.s.data[fi]);
            }
        }
    }
    for (const auto& [token, val] : roots) {
        (void)token;
        impl_->pinHeapValueIfNeededUnlocked(val);
    }
    impl_->freeSlots = std::move(freeSlots);
    impl_->persistentRoots = std::move(roots);
    impl_->nextRootToken = nextRootToken;
    impl_->internedStrings.clear();
    impl_->markStack.clear();
    impl_->nurserySlots.clear();
    impl_->rememberedSet.clear();
    impl_->phase = GcPhase::Idle;
    impl_->requested = false;
    impl_->sweepIndex = 0;
    impl_->resumeSweep = false;
    impl_->wallClockCycleActive = false;
    impl_->sweepLimit = 0;
    impl_->statistics = snapshot.statistics;
    impl_->statistics.liveObjects = snapshot.objects.size();
    impl_->statistics.liveBytes = liveBytes;
    impl_->statistics.peakLiveBytes = std::max(
        impl_->statistics.peakLiveBytes, liveBytes);
    impl_->nextCollectionThreshold = std::max(
        impl_->config.initialCollectionThresholdBytes,
        std::min(
            impl_->config.maximumHeapBytes,
            liveBytes > impl_->config.maximumHeapBytes / 2u
                ? impl_->config.maximumHeapBytes
                : std::max<std::size_t>(1u, liveBytes * 2u)));
    if (error) *error = {};
    return true;
}

std::vector<ObjectRef> ManagedHeap::retainingPath(
    ObjectRef target,
    const ShadowStack* shadowStack) const {
    HeapSnapshot image;
    {
        HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
        if (!impl_->getSlot(target)) return {};
        image = impl_->snapshotUnlocked(shadowStack);
    }
    std::unordered_map<std::uint32_t, const HeapObjectInfo*> objects;
    for (const auto& object : image.objects) {
        objects.emplace(object.reference.slot, &object);
    }
    std::deque<ObjectRef> queue;
    std::unordered_map<std::uint32_t, ObjectRef> parent;
    std::unordered_set<std::uint32_t> visited;
    for (const auto& root : image.roots) {
        if (visited.insert(root.reference.slot).second) {
            queue.push_back(root.reference);
            parent.emplace(root.reference.slot, ObjectRef{});
        }
    }
    while (!queue.empty()) {
        const auto current = queue.front();
        queue.pop_front();
        if (current == target) {
            std::vector<ObjectRef> path;
            auto cursor = current;
            while (cursor.valid()) {
                path.push_back(cursor);
                const auto found = parent.find(cursor.slot);
                if (found == parent.end()) break;
                cursor = found->second;
            }
            std::reverse(path.begin(), path.end());
            return path;
        }
        const auto found = objects.find(current.slot);
        if (found == objects.end()) continue;
        for (const auto& edge : found->second->edges) {
            if (visited.insert(edge.target.slot).second) {
                parent.emplace(edge.target.slot, current);
                queue.push_back(edge.target);
            }
        }
    }
    return {};
}

std::string ManagedHeap::leakSummary() const {
    HeapSnapshot image;
    {
        HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
        image = impl_->snapshotUnlocked(nullptr);
    }
    std::size_t strings = 0;
    std::size_t arrays = 0;
    std::size_t records = 0;
    for (const auto& object : image.objects) {
        switch (object.kind) {
        case ObjectKind::String: ++strings; break;
        case ObjectKind::Array: ++arrays; break;
        case ObjectKind::Record: ++records; break;
        }
    }
    std::ostringstream out;
    out << "heap " << image.heapId << ": " << image.objects.size()
        << " live objects, " << image.statistics.liveBytes << " bytes, "
        << image.roots.size() << " persistent roots"
        << " [string=" << strings << ", array=" << arrays
        << ", record=" << records << ']';
    if (!image.roots.empty()) {
        out << "\nroots:";
        for (const auto& root : image.roots) {
            out << "\n  #" << root.token << " -> " << root.reference.slot
                << ':' << root.reference.generation;
        }
    }
    return out.str();
}

std::string HeapSnapshot::toText() const {
    std::ostringstream out;
    out << "heap " << heapId << " objects=" << objects.size()
        << " bytes=" << statistics.liveBytes << " roots=" << roots.size() << '\n';
    for (const auto& root : roots) {
        out << "root " << root.kind << '#' << root.token << " -> o"
            << root.reference.slot << ':' << root.reference.generation << '\n';
    }
    for (const auto& object : objects) {
        out << 'o' << object.reference.slot << ':' << object.reference.generation
            << ' ' << objectKindName(object.kind) << " bytes=" << object.sizeBytes;
        if (object.typeId != 0) out << " type=0x" << std::hex << object.typeId << std::dec;
        if (object.kind == ObjectKind::Array) {
            out << " element=" << semantic::primitiveTypeName(object.elementType)
                << " count=" << object.valueCount;
            if (object.elementTypeId != 0) {
                out << " elementType=0x" << std::hex << object.elementTypeId << std::dec;
            }
        }
        out << '\n';
        for (const auto& edge : object.edges) {
            out << "  " << edge.label << " -> o" << edge.target.slot << ':'
                << edge.target.generation << '\n';
        }
    }
    return out.str();
}

PersistentRoot::~PersistentRoot() { reset(); }

PersistentRoot::PersistentRoot(PersistentRoot&& other) noexcept
    : heap_(other.heap_), token_(other.token_), value_(std::move(other.value_)) {
    other.heap_ = nullptr;
    other.token_ = 0;
    other.value_ = {};
}

PersistentRoot& PersistentRoot::operator=(PersistentRoot&& other) noexcept {
    if (this == &other) return *this;
    reset();
    heap_ = other.heap_;
    token_ = other.token_;
    value_ = std::move(other.value_);
    other.heap_ = nullptr;
    other.token_ = 0;
    other.value_ = {};
    return *this;
}

bool PersistentRoot::valid() const noexcept {
    return heap_ && token_ != 0;
}

ObjectRef PersistentRoot::reference() const noexcept {
    const auto* reference = std::get_if<ObjectRef>(&value_);
    return reference ? *reference : ObjectRef{};
}

const Value& PersistentRoot::value() const noexcept { return value_; }

bool PersistentRoot::update(ObjectRef reference) {
    return update(Value{reference});
}

bool PersistentRoot::update(Value value) {
    if (!heap_ || token_ == 0 ||
        !heap_->updatePersistentRoot(token_, value)) {
        return false;
    }
    value_ = std::move(value);
    return true;
}

void PersistentRoot::reset() noexcept {
    if (heap_ && token_ != 0) (void)heap_->removePersistentRoot(token_);
    heap_ = nullptr;
    token_ = 0;
    value_ = {};
}

PersistentRoot::PersistentRoot(
    ManagedHeap* heap,
    ManagedHeap::RootToken token,
    Value value)
    : heap_(heap), token_(token), value_(std::move(value)) {}

NativeHandleRegistry::NativeHandleRegistry()
    : registryId_(nextRegistryId()) {}

NativeHandle NativeHandleRegistry::create(
    semantic::SymbolId typeId,
    std::shared_ptr<void> resource,
    std::string debugNameValue) {
    if (typeId == 0 || !resource) return {};
    const void* rawPtr = resource.get();
    std::uint32_t slotIndex = 0;
    if (!freeSlots_.empty()) {
        slotIndex = freeSlots_.back();
        freeSlots_.pop_back();
    } else {
        if (slots_.size() >= static_cast<std::size_t>(NativeHandle::InvalidSlot)) {
            return {};
        }
        slotIndex = static_cast<std::uint32_t>(slots_.size());
        slots_.push_back({});
    }
    auto& slot = slots_[slotIndex];
    if (slot.generation == 0 || slot.generation > 0xFFFFu) slot.generation = 1;
    slot.resource = std::move(resource);
    slot.rawPointer = rawPtr;
    slot.typeId = typeId;
    slot.debugName = std::move(debugNameValue);
    slot.cachedWrapper = {};
    slot.cachedHeapId = 0;
    if (rawPtr) {
        activeSlotByPointer_[PointerTypeKey{rawPtr, typeId}] = slotIndex;
    }
    return {slotIndex, slot.generation, typeId, registryId_};
}

std::optional<NativeHandle> NativeHandleRegistry::findHandle(
    semantic::SymbolId typeId,
    const void* rawPointer) const noexcept {
    if (typeId == 0 || !rawPointer) return std::nullopt;
    const auto it = activeSlotByPointer_.find(PointerTypeKey{rawPointer, typeId});
    if (it == activeSlotByPointer_.end() || it->second >= slots_.size()) {
        return std::nullopt;
    }
    const auto& slot = slots_[it->second];
    if (!slot.resource || slot.rawPointer != rawPointer || slot.typeId != typeId) {
        return std::nullopt;
    }
    return NativeHandle{it->second, slot.generation, typeId, registryId_};
}

std::optional<ObjectRef> NativeHandleRegistry::cachedWrapper(
    NativeHandle handle,
    std::uint64_t heapId) const noexcept {
    if (!isAlive(handle)) return std::nullopt;
    const auto& slot = slots_[handle.slot];
    if (!slot.cachedWrapper.valid() || slot.cachedHeapId != heapId) {
        return std::nullopt;
    }
    return slot.cachedWrapper;
}

bool NativeHandleRegistry::setCachedWrapper(
    NativeHandle handle,
    ObjectRef wrapper,
    std::uint64_t heapId) noexcept {
    if (!isAlive(handle)) return false;
    auto& slot = slots_[handle.slot];
    slot.cachedWrapper = wrapper;
    slot.cachedHeapId = heapId;
    return true;
}

std::shared_ptr<void> NativeHandleRegistry::resolve(
    NativeHandle handle,
    semantic::SymbolId expectedTypeId,
    RuntimeError* error) const {
    if (!handle.valid() || handle.registryId != ((registryId_ == 0) ? 0u : (((registryId_ - 1u) % 65535u) + 1u)) ||
        handle.slot >= slots_.size()) {
        setHeapError(error, ErrorCode::InvalidNativeHandle,
            "native handle is invalid");
        return {};
    }
    const auto& slot = slots_[handle.slot];
    if (!slot.resource || slot.generation != handle.generation ||
        slot.typeId != handle.typeId) {
        setHeapError(error, ErrorCode::InvalidNativeHandle,
            "native handle is stale");
        return {};
    }
    if (expectedTypeId != 0 && slot.typeId != expectedTypeId) {
        setHeapError(error, ErrorCode::TypeMismatch,
            "native handle type does not match the expected host type");
        return {};
    }
    return slot.resource;
}

bool NativeHandleRegistry::isAlive(NativeHandle handle) const noexcept {
    return handle.valid() && handle.registryId == ((registryId_ == 0) ? 0u : (((registryId_ - 1u) % 65535u) + 1u)) &&
        handle.slot < slots_.size() &&
        slots_[handle.slot].resource &&
        slots_[handle.slot].generation == handle.generation &&
        slots_[handle.slot].typeId == handle.typeId;
}

bool NativeHandleRegistry::release(NativeHandle handle) noexcept {
    if (!isAlive(handle)) return false;
    auto& slot = slots_[handle.slot];
    if (slot.rawPointer) {
        const auto it = activeSlotByPointer_.find(
            PointerTypeKey{slot.rawPointer, slot.typeId});
        if (it != activeSlotByPointer_.end() && it->second == handle.slot) {
            activeSlotByPointer_.erase(it);
        }
    }
    slot.resource.reset();
    slot.rawPointer = nullptr;
    slot.typeId = 0;
    slot.debugName.clear();
    slot.cachedWrapper = {};
    slot.cachedHeapId = 0;
    ++slot.generation;
    if (slot.generation == 0 || slot.generation > 0xFFFFu) slot.generation = 1;
    freeSlots_.push_back(handle.slot);
    return true;
}

void NativeHandleRegistry::clear() noexcept {
    activeSlotByPointer_.clear();
    freeSlots_.clear();
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        auto& slot = slots_[index];
        if (slot.resource) {
            slot.resource.reset();
            slot.rawPointer = nullptr;
            slot.typeId = 0;
            slot.debugName.clear();
            slot.cachedWrapper = {};
            slot.cachedHeapId = 0;
            ++slot.generation;
            if (slot.generation == 0 || slot.generation > 0xFFFFu) slot.generation = 1;
        }
        freeSlots_.push_back(static_cast<std::uint32_t>(index));
    }
}

std::size_t NativeHandleRegistry::liveCount() const noexcept {
    return static_cast<std::size_t>(std::count_if(
        slots_.begin(), slots_.end(),
        [](const Slot& slot) { return static_cast<bool>(slot.resource); }));
}

std::uint64_t NativeHandleRegistry::registryId() const noexcept {
    return registryId_;
}

std::string NativeHandleRegistry::debugName(NativeHandle handle) const {
    return isAlive(handle) ? slots_[handle.slot].debugName : std::string{};
}

void NativeHandleRegistry::registerCppType(
    std::type_index cppType,
    semantic::SymbolId typeId) {
    if (typeId != 0) {
        cppTypeIds_[cppType] = typeId;
    }
}

semantic::SymbolId NativeHandleRegistry::findCppTypeId(
    std::type_index cppType) const noexcept {
    const auto it = cppTypeIds_.find(cppType);
    return it != cppTypeIds_.end() ? it->second : 0;
}


namespace detail {

std::shared_ptr<void> createValuePoolRootSync(
    void* owner,
    ValuePoolRootMarkFn markFn) {
    return std::make_shared<HeapSyncState>(owner, markFn, nextHeapId());
}

void activateValuePoolRootSync(const std::shared_ptr<void>& syncHandle) {
    if (!syncHandle) return;
    registerHeapSyncInPoolShard(std::static_pointer_cast<HeapSyncState>(syncHandle));
}

void deactivateValuePoolRootSync(const std::shared_ptr<void>& syncHandle) noexcept {
    if (!syncHandle) return;
    unregisterHeapSyncFromPoolShard(*static_cast<HeapSyncState*>(syncHandle.get()));
}

void detachValuePoolRootSyncOwner(const std::shared_ptr<void>& syncHandle) noexcept {
    if (!syncHandle) return;
    auto* sync = static_cast<HeapSyncState*>(syncHandle.get());
    {
        HeapSyncMutexGuard lk(sync->mutex, /*flushDeferredOnUnlock=*/false);
        sync->ownerImpl = nullptr;
    }
    unregisterHeapSyncFromPoolShard(*sync);
}

void lockValuePoolRootSync(const std::shared_ptr<void>& syncHandle) {
    if (!syncHandle) return;
    static_cast<HeapSyncState*>(syncHandle.get())->mutex.lock();
    ++tlsHeapSyncLockDepthRef();
}

void unlockValuePoolRootSync(const std::shared_ptr<void>& syncHandle) noexcept {
    if (!syncHandle) return;
    static_cast<HeapSyncState*>(syncHandle.get())->mutex.unlock();
    auto& depth = tlsHeapSyncLockDepthRef();
    if (depth > 0u && --depth == 0u && tlsMutatorDepthRef() == 0u) {
        if (mutatorSweepCoordinator().deferredSweepPending.exchange(
                false, std::memory_order_acq_rel)) {
            (void)reclaimPoolValuesInternal(nullptr, /*waitIfBusy=*/false);
        }
    }
}

void enterInterpreterMutator() noexcept {
    auto& depth = tlsMutatorDepthRef();
    if (depth++ > 0u) {
        return;
    }
    auto& mc = mutatorSweepCoordinator();
    std::uint64_t cur = mc.state.load(std::memory_order_relaxed);
    while (true) {
        if ((cur & MutatorSweepCoordinator::kSweepActiveBit) == 0u) {
            if (mc.state.compare_exchange_weak(
                    cur,
                    cur + 1u,
                    std::memory_order_acquire,
                    std::memory_order_relaxed)) {
                return;
            }
            continue;
        }
        std::unique_lock<std::mutex> lk(mc.waitMutex);
        mc.cv.wait(lk, [&]() {
            return (mc.state.load(std::memory_order_acquire) &
                    MutatorSweepCoordinator::kSweepActiveBit) == 0u;
        });
        cur = mc.state.load(std::memory_order_relaxed);
    }
}

void leaveInterpreterMutator() noexcept {
    auto& depth = tlsMutatorDepthRef();
    if (depth == 0u || --depth > 0u) {
        return;
    }
    auto& mc = mutatorSweepCoordinator();
    const std::uint64_t prev = mc.state.fetch_sub(1u, std::memory_order_acq_rel);
    const std::uint64_t remaining =
        (prev & MutatorSweepCoordinator::kActiveMutatorMask) - 1u;
    if (remaining == 0u) {
        if ((prev & MutatorSweepCoordinator::kSweepActiveBit) != 0u) {
            {
                std::lock_guard<std::mutex> lk(mc.waitMutex);
            }
            mc.cv.notify_all();
        } else if (mc.deferredSweepPending.exchange(false, std::memory_order_acq_rel)) {
            (void)reclaimPoolValuesInternal(nullptr, /*waitIfBusy=*/false);
        }
    }
}

} // namespace detail

namespace {

std::size_t reclaimPoolValuesInternal(
    const ShadowStack* activeShadowStack,
    bool waitIfBusy) noexcept {
    if (!hasAnyPinnedPoolValues()) return 0u;
    auto& mc = mutatorSweepCoordinator();
    if (tlsHeapSyncLockDepthRef() > 0u) {
        mc.deferredSweepPending.store(true, std::memory_order_release);
        return 0u;
    }
    const std::uint64_t selfContribution = (tlsMutatorDepthRef() > 0u) ? 1ULL : 0ULL;

    // Never cross-thread scan concurrently mutating ShadowStacks or ObjectFastViews
    // when another mutator thread is active. Defer to the mutator safe point instead.
    if (!waitIfBusy || selfContribution > 0u) {
        if ((mc.state.load(std::memory_order_acquire) &
             MutatorSweepCoordinator::kActiveMutatorMask) > selfContribution) {
            mc.deferredSweepPending.store(true, std::memory_order_seq_cst);
            if ((mc.state.load(std::memory_order_seq_cst) &
                 MutatorSweepCoordinator::kActiveMutatorMask) > selfContribution) {
                return 0u;
            }
        }
    }

    auto& coord = valuePoolSweepCoordinator();
    (void)coord.requestedEpoch.fetch_add(1, std::memory_order_acq_rel);

    std::unique_lock<std::mutex> sweepLock(coord.sweepMutex, std::defer_lock);
    if (waitIfBusy && selfContribution == 0u) {
        sweepLock.lock();
    } else if (!sweepLock.try_lock()) {
        mc.deferredSweepPending.store(true, std::memory_order_release);
        return 0u;
    }

    if (!waitIfBusy || selfContribution > 0u) {
        std::uint64_t cur = mc.state.load(std::memory_order_acquire);
        while (true) {
            if ((cur & MutatorSweepCoordinator::kActiveMutatorMask) > selfContribution ||
                (cur & MutatorSweepCoordinator::kSweepActiveBit) != 0u) {
                mc.deferredSweepPending.store(true, std::memory_order_seq_cst);
                cur = mc.state.load(std::memory_order_seq_cst);
                if ((cur & MutatorSweepCoordinator::kActiveMutatorMask) > selfContribution ||
                    (cur & MutatorSweepCoordinator::kSweepActiveBit) != 0u) {
                    return 0u;
                }
            }
            if (mc.state.compare_exchange_weak(
                    cur,
                    cur | MutatorSweepCoordinator::kSweepActiveBit,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                break;
            }
        }
    } else {
        const std::uint64_t prev = mc.state.fetch_or(
            MutatorSweepCoordinator::kSweepActiveBit,
            std::memory_order_acq_rel);
        if ((prev & MutatorSweepCoordinator::kActiveMutatorMask) != 0u) {
            std::unique_lock<std::mutex> lk(mc.waitMutex);
            mc.cv.wait(lk, [&]() {
                return (mc.state.load(std::memory_order_acquire) &
                        MutatorSweepCoordinator::kActiveMutatorMask) == 0u;
            });
        }
    }

    mc.deferredSweepPending.store(false, std::memory_order_relaxed);
    struct SweepActiveBitGuard {
        MutatorSweepCoordinator& mcRef;
        ~SweepActiveBitGuard() noexcept {
            {
                std::lock_guard<std::mutex> lk(mcRef.waitMutex);
                mcRef.state.fetch_and(
                    ~MutatorSweepCoordinator::kSweepActiveBit,
                    std::memory_order_release);
            }
            mcRef.cv.notify_all();
        }
    } sweepBitGuard{mc};

    std::size_t totalReclaimed = 0u;
    for (int pass = 0; pass < 2; ++pass) {
        const std::uint64_t targetEpoch = coord.requestedEpoch.load(std::memory_order_acquire);
        if (!hasAnyPinnedPoolValues()) {
            coord.completedEpoch.store(targetEpoch, std::memory_order_release);
            break;
        }

        beginValuePoolGcMark();
        if (activeShadowStack) {
            activeShadowStack->visitRoots([](const Value& value) {
                markStackRootValueInPools(value);
            });
        }
        {
            std::vector<std::shared_ptr<ThreadShadowStackShard>> shardsSnapshot;
            {
                std::lock_guard<std::mutex> listLock(shadowStackShardsMutex());
                auto& shards = shadowStackShards();
                shards.erase(
                    std::remove_if(
                        shards.begin(),
                        shards.end(),
                        [](const std::shared_ptr<ThreadShadowStackShard>& shard) {
                            if (!shard) return true;
                            std::lock_guard<std::mutex> shardLock(shard->mutex);
                            return shard->threadExited && shard->stacks.empty();
                        }),
                    shards.end());
                shardsSnapshot = shards;
            }
            for (const auto& shard : shardsSnapshot) {
                if (!shard) continue;
                std::lock_guard<std::mutex> shardLock(shard->mutex);
                for (const ShadowStack* stack : shard->stacks) {
                    if (stack && stack != activeShadowStack) {
                        stack->visitRoots([](const Value& value) {
                            markStackRootValueInPools(value);
                        });
                    }
                }
            }
        }

        std::vector<std::shared_ptr<HeapSyncState>> heapSyncSnapshot;
        {
            auto& markerShards = heapPoolMarkerShards();
            for (auto& shard : markerShards) {
                if (shard.activeCount.load(std::memory_order_acquire) == 0) continue;
                std::lock_guard<std::mutex> shardLock(shard.mutex);
                heapSyncSnapshot.insert(
                    heapSyncSnapshot.end(),
                    shard.entries.begin(),
                    shard.entries.end());
            }
        }
        for (const auto& sync : heapSyncSnapshot) {
            if (!sync) continue;
            HeapSyncMutexGuard instanceLock(sync->mutex, /*flushDeferredOnUnlock=*/false);
            if (sync->ownerImpl && sync->markPoolFn) {
                (void)sync->markPoolFn(sync->ownerImpl);
            }
        }

        totalReclaimed += finishValuePoolGcSweep();
        coord.completedEpoch.store(targetEpoch, std::memory_order_release);
        if (coord.requestedEpoch.load(std::memory_order_acquire) == targetEpoch) {
            break;
        }
    }
    return totalReclaimed;
}

} // namespace

std::size_t reclaimUnreferencedPoolValues(
    const ShadowStack* activeShadowStack) noexcept {
    return reclaimPoolValuesInternal(activeShadowStack, /*waitIfBusy=*/true);
}

} // namespace realscript::runtime
