#include "RuntimeInternal.h"

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

std::uint32_t compactNativeRegistryId(std::uint64_t registryId) noexcept {
    return registryId == 0
        ? 0u
        : static_cast<std::uint32_t>(((registryId - 1u) % 65535u) + 1u);
}

std::mutex& nativeHandleRegistryDirMutex() noexcept {
    static std::mutex dirMutex;
    return dirMutex;
}

std::vector<NativeHandleRegistry*>& nativeHandleRegistries() noexcept {
    static std::vector<NativeHandleRegistry*> registries;
    return registries;
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
    std::atomic<std::uint32_t> parallelLockFreeDepth{0};

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

std::array<HeapPoolMarkerShard, kHeapPoolMarkerShardCount>& localHeapPoolMarkerShards() noexcept {
    static auto* shards = new std::array<HeapPoolMarkerShard, kHeapPoolMarkerShardCount>();
    return *shards;
}

std::array<HeapPoolMarkerShard, kHeapPoolMarkerShardCount>& heapPoolMarkerShards() noexcept {
    if (const auto* b = detail::externalValuePoolBridge(); b && b->heapPoolMarkerShardsPtr) {
        return *static_cast<std::array<HeapPoolMarkerShard, kHeapPoolMarkerShardCount>*>(b->heapPoolMarkerShardsPtr);
    }
    return localHeapPoolMarkerShards();
}

std::array<HeapPoolMarkerShard, kHeapPoolMarkerShardCount>& localLiveStringHeapShards() noexcept {
    static auto* shards = new std::array<HeapPoolMarkerShard, kHeapPoolMarkerShardCount>();
    return *shards;
}

std::array<HeapPoolMarkerShard, kHeapPoolMarkerShardCount>& liveStringHeapShards() noexcept {
    if (const auto* b = detail::externalValuePoolBridge(); b && b->liveStringHeapShardsPtr) {
        return *static_cast<std::array<HeapPoolMarkerShard, kHeapPoolMarkerShardCount>*>(b->liveStringHeapShardsPtr);
    }
    return localLiveStringHeapShards();
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

void enterHeapSyncLockLocal() noexcept {
    if (const auto* b = detail::externalValuePoolBridge(); b && b->enterHeapSyncLockFn) {
        b->enterHeapSyncLockFn();
        return;
    }
    ++tlsHeapSyncLockDepthRef();
}

void leaveHeapSyncLockLocal(bool flushDeferred) noexcept {
    if (const auto* b = detail::externalValuePoolBridge(); b && b->leaveHeapSyncLockFn) {
        b->leaveHeapSyncLockFn(flushDeferred);
        return;
    }
    auto& depth = tlsHeapSyncLockDepthRef();
    if (depth > 0u && --depth == 0u && flushDeferred && tlsMutatorDepthRef() == 0u) {
        if (mutatorSweepCoordinator().deferredSweepPending.exchange(
                false, std::memory_order_acq_rel)) {
            (void)reclaimPoolValuesInternal(nullptr, /*waitIfBusy=*/false);
        }
    }
}

class HeapSyncMutexGuard {
public:
    explicit HeapSyncMutexGuard(std::mutex& m, bool flushDeferredOnUnlock = true)
        : mutex_(m), flushDeferred_(flushDeferredOnUnlock) {
        mutex_.lock();
        enterHeapSyncLockLocal();
    }

    ~HeapSyncMutexGuard() noexcept {
        mutex_.unlock();
        leaveHeapSyncLockLocal(flushDeferred_);
    }

    HeapSyncMutexGuard(const HeapSyncMutexGuard&) = delete;
    HeapSyncMutexGuard& operator=(const HeapSyncMutexGuard&) = delete;

private:
    std::mutex& mutex_;
    bool flushDeferred_ = true;
};

class HeapReadOrDisjointGuard {
public:
    explicit HeapReadOrDisjointGuard(HeapSyncState& sync)
        : sync_(sync),
          locked_(sync.parallelLockFreeDepth.load(std::memory_order_acquire) == 0u) {
        if (locked_) {
            sync_.mutex.lock();
            enterHeapSyncLockLocal();
        }
    }

    ~HeapReadOrDisjointGuard() noexcept {
        if (locked_) {
            sync_.mutex.unlock();
            leaveHeapSyncLockLocal(/*flushDeferred=*/true);
        }
    }

    HeapReadOrDisjointGuard(const HeapReadOrDisjointGuard&) = delete;
    HeapReadOrDisjointGuard& operator=(const HeapReadOrDisjointGuard&) = delete;

private:
    HeapSyncState& sync_;
    bool locked_ = true;
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
    if (const auto* b = detail::externalValuePoolBridge(); b && b->registerShadowStackFn) {
        b->registerShadowStackFn(stack);
        return;
    }
    if (!stack || stack->registryShard_ != nullptr) return;
    auto* shard = currentThreadShadowStackShard();
    std::lock_guard<std::mutex> lock(shard->mutex);
    stack->registryShard_ = shard;
    stack->registrySlot_ = static_cast<std::uint32_t>(shard->stacks.size());
    shard->stacks.push_back(stack);
}

void unregisterActiveShadowStack(ShadowStack* stack) noexcept {
    if (const auto* b = detail::externalValuePoolBridge(); b && b->unregisterShadowStackFn) {
        b->unregisterShadowStackFn(stack);
        return;
    }
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
    if (const auto* b = detail::externalValuePoolBridge(); b && b->transferShadowStackFn) {
        b->transferShadowStackFn(from, to);
        return;
    }
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
    Empty        = 0,
    SsoString    = 1,
    HeapString   = 2,
    SlabValues   = 3,
    HeapValues   = 4,
    RawColumn    = 5,
    PackedB8     = 6,  // 1 byte/element (bool, byte, sbyte -> 16x compression)
    PackedB16    = 7,  // 2 bytes/element (short, ushort, char -> 8x compression)
    PackedI32    = 8,  // 4 bytes/element (int, uint, float, enum -> 4x compression)
    PackedI64    = 9,  // 8 bytes/element (long, ulong, double, wide-int/enum -> 2x compression)
    PackedStruct = 10, // Contiguous flat math vectors & blittable user structs (0 GC scan)
    FlatStruct   = 11, // Contiguous multi-Value-slot user structs (supports ObjectRef/String/Array + CardTable)
};

[[nodiscard]] constexpr bool isPackedStorage(SlotStorage st) noexcept {
    return st >= SlotStorage::PackedB8 && st <= SlotStorage::PackedStruct;
}

[[nodiscard]] constexpr std::uint16_t packPackedAuxMeta(
    PackedElemEncoding encoding,
    std::uint8_t elemShift) noexcept {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(encoding) |
        (static_cast<std::uint16_t>(elemShift & 0x03u) << 8u));
}

[[nodiscard]] constexpr PackedElemEncoding unpackPackedEncoding(std::uint16_t auxMeta) noexcept {
    return static_cast<PackedElemEncoding>(auxMeta & 0xFFu);
}

[[nodiscard]] constexpr std::uint8_t unpackPackedShift(std::uint16_t auxMeta) noexcept {
    return static_cast<std::uint8_t>((auxMeta >> 8u) & 0x03u);
}

enum class BlittableLeafKind : std::uint8_t {
    Bool,
    Byte,
    SByte,
    Short,
    UShort,
    Char,
    Int32,
    UInt32,
    Float32,
    Enum32,
    Int64,
    UInt64,
    Float64,
    Fixed24_8,
    Fixed48_16,
    Fixed32_32,
    Vec2,
    Vec3,
    Quat,
    Vec2f,
    Vec3f,
    Quatf,
    NestedStruct,
};

struct BlittableFieldEntry {
    BlittableLeafKind kind = BlittableLeafKind::Int32;
    std::uint32_t byteOffset = 0;
    semantic::SymbolId fieldTypeId = 0;
    std::uint32_t nestedSchemaId = 0;

    friend bool operator==(const BlittableFieldEntry& a, const BlittableFieldEntry& b) noexcept {
        return a.kind == b.kind &&
            a.byteOffset == b.byteOffset &&
            a.fieldTypeId == b.fieldTypeId &&
            a.nestedSchemaId == b.nestedSchemaId;
    }
};

struct BlittableStructSchema {
    std::uint32_t schemaId = 0;
    semantic::SymbolId structTypeId = 0;
    std::uint32_t strideBytes = 0;
    std::uint32_t alignBytes = 1;
    std::vector<BlittableFieldEntry> fields;
};

struct BlittableStructRegistry {
    static constexpr std::uint32_t kBlockBits = 10u;
    static constexpr std::uint32_t kBlockSize = 1u << kBlockBits; // 1024
    static constexpr std::uint32_t kMaxBlocks = 256u;             // 262,144 schemas

    std::mutex mutex;
    std::array<std::atomic<BlittableStructSchema*>, kMaxBlocks> blocks{};
    std::vector<std::unique_ptr<BlittableStructSchema[]>> ownedBlocks;
    std::atomic<std::uint32_t> nextSchemaId{1u}; // 0 = invalid
    std::unordered_map<semantic::SymbolId, std::uint32_t> latestByTypeId;

    BlittableStructRegistry() noexcept {
        for (auto& b : blocks) b.store(nullptr, std::memory_order_relaxed);
    }

    [[nodiscard]] const BlittableStructSchema* getByIdLockFree(std::uint32_t id) const noexcept {
        const std::uint32_t limit = nextSchemaId.load(std::memory_order_acquire);
        if (id == 0u || id >= limit) return nullptr;
        const std::uint32_t blkIdx = id >> kBlockBits;
        if (blkIdx >= kMaxBlocks) return nullptr;
        const auto* blk = blocks[blkIdx].load(std::memory_order_acquire);
        if (!blk) return nullptr;
        return &blk[id & (kBlockSize - 1u)];
    }

    [[nodiscard]] std::uint32_t findLatestSchemaId(semantic::SymbolId typeId) noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = latestByTypeId.find(typeId);
        return it != latestByTypeId.end() ? it->second : 0u;
    }

    std::uint32_t publishUnlocked(
        semantic::SymbolId typeId,
        std::uint32_t strideBytes,
        std::uint32_t alignBytes,
        std::vector<BlittableFieldEntry> fields) {
        const auto existingIt = latestByTypeId.find(typeId);
        if (existingIt != latestByTypeId.end() && existingIt->second != 0u) {
            if (const auto* prev = getByIdLockFree(existingIt->second)) {
                if (prev->strideBytes == strideBytes &&
                    prev->alignBytes == alignBytes &&
                    prev->fields == fields) {
                    return existingIt->second;
                }
            }
        }
        const std::uint32_t id = nextSchemaId.load(std::memory_order_relaxed);
        const std::uint32_t blkIdx = id >> kBlockBits;
        if (blkIdx >= kMaxBlocks) return 0u;
        auto* blk = blocks[blkIdx].load(std::memory_order_relaxed);
        if (!blk) {
            auto alloc = std::make_unique<BlittableStructSchema[]>(kBlockSize);
            blk = alloc.get();
            ownedBlocks.push_back(std::move(alloc));
            blocks[blkIdx].store(blk, std::memory_order_release);
        }
        auto& slot = blk[id & (kBlockSize - 1u)];
        slot.schemaId = id;
        slot.structTypeId = typeId;
        slot.strideBytes = strideBytes;
        slot.alignBytes = alignBytes;
        slot.fields = std::move(fields);
        nextSchemaId.store(id + 1u, std::memory_order_release);
        latestByTypeId[typeId] = id;
        return id;
    }

    void markNonBlittableUnlocked(semantic::SymbolId typeId) {
        latestByTypeId.erase(typeId);
    }
};

BlittableStructRegistry& localBlittableRegistry() noexcept {
    static BlittableStructRegistry instance;
    return instance;
}

BlittableStructRegistry& globalBlittableRegistry() noexcept {
    if (const auto* b = detail::externalValuePoolBridge(); b && b->blittableRegistryPtr) {
        return *static_cast<BlittableStructRegistry*>(b->blittableRegistryPtr);
    }
    return localBlittableRegistry();
}

[[nodiscard]] inline bool isBuiltinMathStructId(semantic::SymbolId tid) noexcept {
    return tid != 0 && (
        tid == fixedTypeId() ||
        tid == fixed16TypeId() ||
        tid == fixed64TypeId() ||
        tid == vec2TypeId() ||
        tid == vec3TypeId() ||
        tid == quatTypeId() ||
        tid == vec2fTypeId() ||
        tid == vec3fTypeId() ||
        tid == quatfTypeId());
}

[[nodiscard]] inline std::uint32_t alignUpU32(std::uint32_t offset, std::uint32_t align) noexcept {
    return (offset + align - 1u) & ~(align - 1u);
}

[[nodiscard]] inline bool classifyLeafFieldSpec(
    semantic::PrimitiveType fType,
    semantic::SymbolId fTypeId,
    BlittableLeafKind& outKind,
    std::uint32_t& outSize,
    std::uint32_t& outAlign) noexcept {
    switch (fType) {
    case semantic::PrimitiveType::Bool:   outKind = BlittableLeafKind::Bool;    outSize = 1; outAlign = 1; return true;
    case semantic::PrimitiveType::Byte:   outKind = BlittableLeafKind::Byte;    outSize = 1; outAlign = 1; return true;
    case semantic::PrimitiveType::SByte:  outKind = BlittableLeafKind::SByte;   outSize = 1; outAlign = 1; return true;
    case semantic::PrimitiveType::Short:  outKind = BlittableLeafKind::Short;   outSize = 2; outAlign = 2; return true;
    case semantic::PrimitiveType::UShort: outKind = BlittableLeafKind::UShort;  outSize = 2; outAlign = 2; return true;
    case semantic::PrimitiveType::Char:   outKind = BlittableLeafKind::Char;    outSize = 2; outAlign = 2; return true;
    case semantic::PrimitiveType::Int:    outKind = BlittableLeafKind::Int32;   outSize = 4; outAlign = 4; return true;
    case semantic::PrimitiveType::UInt:   outKind = BlittableLeafKind::UInt32;  outSize = 4; outAlign = 4; return true;
    case semantic::PrimitiveType::Float:  outKind = BlittableLeafKind::Float32; outSize = 4; outAlign = 4; return true;
    case semantic::PrimitiveType::Enum:
        if (fTypeId == 0) return false;
        outKind = BlittableLeafKind::Enum32;
        outSize = 4;
        outAlign = 4;
        return true;
    case semantic::PrimitiveType::Long:   outKind = BlittableLeafKind::Int64;   outSize = 8; outAlign = 8; return true;
    case semantic::PrimitiveType::ULong:  outKind = BlittableLeafKind::UInt64;  outSize = 8; outAlign = 8; return true;
    case semantic::PrimitiveType::Double: outKind = BlittableLeafKind::Float64; outSize = 8; outAlign = 8; return true;
    case semantic::PrimitiveType::Struct:
        if (fTypeId == fixedTypeId())   { outKind = BlittableLeafKind::Fixed24_8;  outSize = 4;  outAlign = 4; return true; }
        if (fTypeId == fixed16TypeId()) { outKind = BlittableLeafKind::Fixed48_16; outSize = 8;  outAlign = 8; return true; }
        if (fTypeId == fixed64TypeId()) { outKind = BlittableLeafKind::Fixed32_32; outSize = 8;  outAlign = 8; return true; }
        if (fTypeId == vec2TypeId())    { outKind = BlittableLeafKind::Vec2;       outSize = 8;  outAlign = 4; return true; }
        if (fTypeId == vec3TypeId())    { outKind = BlittableLeafKind::Vec3;       outSize = 12; outAlign = 4; return true; }
        if (fTypeId == quatTypeId())    { outKind = BlittableLeafKind::Quat;       outSize = 16; outAlign = 4; return true; }
        if (fTypeId == vec2fTypeId())   { outKind = BlittableLeafKind::Vec2f;      outSize = 8;  outAlign = 4; return true; }
        if (fTypeId == vec3fTypeId())   { outKind = BlittableLeafKind::Vec3f;      outSize = 12; outAlign = 4; return true; }
        if (fTypeId == quatfTypeId())   { outKind = BlittableLeafKind::Quatf;      outSize = 16; outAlign = 4; return true; }
        return false;
    default:
        return false;
    }
}

[[nodiscard]] std::uint32_t tryInferBlittableSchemaFromSampleUnlocked(
    BlittableStructRegistry& reg,
    const StructValue& sv,
    std::uint32_t depth = 0) {
    if (!sv.valid() || sv.typeId == 0 || isBuiltinMathStructId(sv.typeId) || depth > 16u) {
        return 0u;
    }
    const auto existingIt = reg.latestByTypeId.find(sv.typeId);
    if (existingIt != reg.latestByTypeId.end() && existingIt->second != 0u) {
        return existingIt->second;
    }
    const std::size_t fCount = sv.fieldCount();
    if (fCount == 0u || fCount > 256u) return 0u;

    std::vector<BlittableFieldEntry> entries;
    entries.reserve(fCount);
    std::uint32_t offset = 0u;
    std::uint32_t maxAlign = 1u;

    for (std::size_t i = 0; i < fCount; ++i) {
        const auto fvOpt = structValueFieldAt(sv, i);
        if (!fvOpt) return 0u;
        const Value& fv = *fvOpt;
        const auto prim = valueType(fv);
        if (prim == semantic::PrimitiveType::Struct) {
            const auto childTidOpt = structValueTypeId(fv);
            if (!childTidOpt || *childTidOpt == 0) return 0u;
            BlittableLeafKind mKind{};
            std::uint32_t mSize = 0, mAlign = 1;
            if (classifyLeafFieldSpec(prim, *childTidOpt, mKind, mSize, mAlign)) {
                offset = alignUpU32(offset, mAlign);
                maxAlign = std::max(maxAlign, mAlign);
                entries.push_back(BlittableFieldEntry{mKind, offset, *childTidOpt, 0u});
                offset += mSize;
                continue;
            }
            const auto* childSv = std::get_if<StructValue>(&fv);
            if (!childSv || !childSv->valid()) return 0u;
            const std::uint32_t childSchemaId =
                tryInferBlittableSchemaFromSampleUnlocked(reg, *childSv, depth + 1u);
            if (childSchemaId == 0u) return 0u;
            const auto* childSchema = reg.getByIdLockFree(childSchemaId);
            if (!childSchema || childSchema->strideBytes == 0u) return 0u;
            offset = alignUpU32(offset, childSchema->alignBytes);
            maxAlign = std::max(maxAlign, childSchema->alignBytes);
            entries.push_back(BlittableFieldEntry{
                BlittableLeafKind::NestedStruct, offset, *childTidOpt, childSchemaId});
            offset += childSchema->strideBytes;
            continue;
        }
        semantic::SymbolId fTid = 0;
        if (prim == semantic::PrimitiveType::Enum) {
            EnumValue ev{};
            if (!fv.tryGetEnum(ev) || ev.typeId == 0 ||
                ev.value < std::numeric_limits<std::int32_t>::min() ||
                ev.value > std::numeric_limits<std::int32_t>::max()) {
                return 0u;
            }
            fTid = ev.typeId;
        } else if (prim == semantic::PrimitiveType::Int) {
            std::int64_t iv = 0;
            if (!fv.tryGetInt64(iv) ||
                iv < std::numeric_limits<std::int32_t>::min() ||
                iv > std::numeric_limits<std::int32_t>::max()) {
                return 0u;
            }
        }
        BlittableLeafKind lKind{};
        std::uint32_t lSize = 0, lAlign = 1;
        if (!classifyLeafFieldSpec(prim, fTid, lKind, lSize, lAlign)) {
            return 0u;
        }
        offset = alignUpU32(offset, lAlign);
        maxAlign = std::max(maxAlign, lAlign);
        entries.push_back(BlittableFieldEntry{lKind, offset, fTid, 0u});
        offset += lSize;
    }
    const std::uint32_t stride = alignUpU32(offset, maxAlign);
    if (stride == 0u || stride > (1u << 20u)) return 0u;
    return reg.publishUnlocked(sv.typeId, stride, maxAlign, std::move(entries));
}

[[nodiscard]] Value decodeBlittableStructBytes(
    const BlittableStructRegistry& reg,
    const std::uint8_t* elemBase,
    const BlittableStructSchema& schema) noexcept {
    const std::size_t fCount = schema.fields.size();
    std::array<Value, 16> stackFields{};
    std::vector<Value> heapFields;
    Value* dst = (fCount <= stackFields.size()) ? stackFields.data() : nullptr;
    if (!dst) {
        heapFields.resize(fCount);
        dst = heapFields.data();
    }
    for (std::size_t i = 0; i < fCount; ++i) {
        const auto& f = schema.fields[i];
        const std::uint8_t* p = elemBase + f.byteOffset;
        switch (f.kind) {
        case BlittableLeafKind::Bool:
            dst[i] = Value{p[0] != 0u};
            break;
        case BlittableLeafKind::Byte:
            dst[i] = Value{ByteValue{p[0]}};
            break;
        case BlittableLeafKind::SByte:
            dst[i] = Value{SByteValue{static_cast<std::int8_t>(p[0])}};
            break;
        case BlittableLeafKind::Short: {
            std::int16_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            dst[i] = Value{ShortValue{v}};
            break;
        }
        case BlittableLeafKind::UShort: {
            std::uint16_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            dst[i] = Value{UShortValue{v}};
            break;
        }
        case BlittableLeafKind::Char: {
            std::uint16_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            dst[i] = Value{CharValue{v}};
            break;
        }
        case BlittableLeafKind::Int32: {
            std::int32_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            dst[i] = Value{static_cast<std::int64_t>(v)};
            break;
        }
        case BlittableLeafKind::UInt32: {
            std::uint32_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            dst[i] = Value{UIntValue{v}};
            break;
        }
        case BlittableLeafKind::Float32: {
            float v = 0.0f;
            std::memcpy(&v, p, sizeof(v));
            dst[i] = Value{FloatValue{v}};
            break;
        }
        case BlittableLeafKind::Enum32: {
            std::int32_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            dst[i] = Value{EnumValue{f.fieldTypeId, static_cast<std::int64_t>(v)}};
            break;
        }
        case BlittableLeafKind::Int64: {
            std::int64_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            dst[i] = Value{LongValue{v}};
            break;
        }
        case BlittableLeafKind::UInt64: {
            std::uint64_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            dst[i] = Value{ULongValue{v}};
            break;
        }
        case BlittableLeafKind::Float64: {
            double v = 0.0;
            std::memcpy(&v, p, sizeof(v));
            dst[i] = Value{v};
            break;
        }
        case BlittableLeafKind::Fixed24_8: {
            std::int32_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            dst[i] = Value{FixedValue{v}};
            break;
        }
        case BlittableLeafKind::Fixed48_16: {
            std::int64_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            dst[i] = Value{Fixed16Value{v}};
            break;
        }
        case BlittableLeafKind::Fixed32_32: {
            std::int64_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            dst[i] = Value{Fixed64Value{v}};
            break;
        }
        case BlittableLeafKind::Vec2: {
            Vec2Value v{};
            std::memcpy(&v, p, sizeof(v));
            dst[i] = Value{v};
            break;
        }
        case BlittableLeafKind::Vec3: {
            Vec3Value v{};
            std::memcpy(&v, p, sizeof(v));
            dst[i] = Value{v};
            break;
        }
        case BlittableLeafKind::Quat: {
            QuatValue v{};
            std::memcpy(&v, p, sizeof(v));
            dst[i] = Value{v};
            break;
        }
        case BlittableLeafKind::Vec2f: {
            Vec2fValue v{};
            std::memcpy(&v, p, sizeof(v));
            dst[i] = Value{v};
            break;
        }
        case BlittableLeafKind::Vec3f: {
            Vec3fValue v{};
            std::memcpy(&v, p, sizeof(v));
            dst[i] = Value{v};
            break;
        }
        case BlittableLeafKind::Quatf: {
            QuatfValue v{};
            std::memcpy(&v, p, sizeof(v));
            dst[i] = Value{v};
            break;
        }
        case BlittableLeafKind::NestedStruct: {
            const auto* childSchema = reg.getByIdLockFree(f.nestedSchemaId);
            if (childSchema) {
                dst[i] = decodeBlittableStructBytes(reg, p, *childSchema);
            } else {
                dst[i] = Value{StructValue{f.fieldTypeId, std::make_shared<StructStorage>()}};
            }
            break;
        }
        }
    }
    if (fCount <= stackFields.size()) {
        return Value{makeStructValueFromFields(schema.structTypeId, ValueSpan(stackFields.data(), fCount))};
    }
    return Value{makeStructValueFromFields(schema.structTypeId, std::move(heapFields))};
}

[[nodiscard]] bool encodeBlittableStructBytesInPlace(
    const BlittableStructRegistry& reg,
    std::uint8_t* elemBase,
    const BlittableStructSchema& schema,
    const Value& value,
    bool& outNeedsDemote) noexcept {
    const auto* sv = std::get_if<StructValue>(&value);
    if (!sv || !sv->valid() || sv->typeId != schema.structTypeId) {
        return false;
    }
    const std::size_t fCount = schema.fields.size();
    if (sv->fieldCount() != fCount) {
        outNeedsDemote = true;
        return false;
    }
    for (std::size_t i = 0; i < fCount; ++i) {
        const auto fvOpt = structValueFieldAt(*sv, i);
        if (!fvOpt) return false;
        const Value& fv = *fvOpt;
        const auto& f = schema.fields[i];
        std::uint8_t* p = elemBase + f.byteOffset;
        switch (f.kind) {
        case BlittableLeafKind::Bool: {
            bool b = false;
            if (!fv.tryGetBool(b)) { outNeedsDemote = true; return false; }
            p[0] = b ? 1u : 0u;
            break;
        }
        case BlittableLeafKind::Byte: {
            const auto* v = std::get_if<ByteValue>(&fv);
            if (!v) { outNeedsDemote = true; return false; }
            p[0] = v->value;
            break;
        }
        case BlittableLeafKind::SByte: {
            const auto* v = std::get_if<SByteValue>(&fv);
            if (!v) { outNeedsDemote = true; return false; }
            p[0] = static_cast<std::uint8_t>(v->value);
            break;
        }
        case BlittableLeafKind::Short: {
            const auto* v = std::get_if<ShortValue>(&fv);
            if (!v) { outNeedsDemote = true; return false; }
            std::memcpy(p, &v->value, sizeof(v->value));
            break;
        }
        case BlittableLeafKind::UShort: {
            const auto* v = std::get_if<UShortValue>(&fv);
            if (!v) { outNeedsDemote = true; return false; }
            std::memcpy(p, &v->value, sizeof(v->value));
            break;
        }
        case BlittableLeafKind::Char: {
            const auto* v = std::get_if<CharValue>(&fv);
            if (!v) { outNeedsDemote = true; return false; }
            std::memcpy(p, &v->value, sizeof(v->value));
            break;
        }
        case BlittableLeafKind::Int32: {
            std::int64_t iv = 0;
            if (!fv.tryGetInt64(iv)) { outNeedsDemote = true; return false; }
            if (iv < std::numeric_limits<std::int32_t>::min() ||
                iv > std::numeric_limits<std::int32_t>::max()) {
                outNeedsDemote = true;
                return false;
            }
            const auto i32 = static_cast<std::int32_t>(iv);
            std::memcpy(p, &i32, sizeof(i32));
            break;
        }
        case BlittableLeafKind::UInt32: {
            const auto* v = std::get_if<UIntValue>(&fv);
            if (!v) { outNeedsDemote = true; return false; }
            std::memcpy(p, &v->value, sizeof(v->value));
            break;
        }
        case BlittableLeafKind::Float32: {
            float fl = 0.0f;
            if (!fv.tryGetFloat(fl)) { outNeedsDemote = true; return false; }
            std::memcpy(p, &fl, sizeof(fl));
            break;
        }
        case BlittableLeafKind::Enum32: {
            EnumValue ev{};
            if (!fv.tryGetEnum(ev) || ev.typeId != f.fieldTypeId) {
                outNeedsDemote = true;
                return false;
            }
            if (ev.value < std::numeric_limits<std::int32_t>::min() ||
                ev.value > std::numeric_limits<std::int32_t>::max()) {
                outNeedsDemote = true;
                return false;
            }
            const auto e32 = static_cast<std::int32_t>(ev.value);
            std::memcpy(p, &e32, sizeof(e32));
            break;
        }
        case BlittableLeafKind::Int64: {
            std::int64_t lv = 0;
            if (!fv.tryGetLong(lv)) { outNeedsDemote = true; return false; }
            std::memcpy(p, &lv, sizeof(lv));
            break;
        }
        case BlittableLeafKind::UInt64: {
            const auto* v = std::get_if<ULongValue>(&fv);
            if (!v) { outNeedsDemote = true; return false; }
            std::memcpy(p, &v->value, sizeof(v->value));
            break;
        }
        case BlittableLeafKind::Float64: {
            double dv = 0.0;
            if (!fv.tryGetDouble(dv)) { outNeedsDemote = true; return false; }
            std::memcpy(p, &dv, sizeof(dv));
            break;
        }
        case BlittableLeafKind::Fixed24_8: {
            const auto* v = std::get_if<FixedValue>(&fv);
            if (!v) { outNeedsDemote = true; return false; }
            std::memcpy(p, &v->raw, sizeof(v->raw));
            break;
        }
        case BlittableLeafKind::Fixed48_16: {
            const auto* v = std::get_if<Fixed16Value>(&fv);
            if (!v) { outNeedsDemote = true; return false; }
            std::memcpy(p, &v->raw, sizeof(v->raw));
            break;
        }
        case BlittableLeafKind::Fixed32_32: {
            const auto* v = std::get_if<Fixed64Value>(&fv);
            if (!v) { outNeedsDemote = true; return false; }
            std::memcpy(p, &v->raw, sizeof(v->raw));
            break;
        }
        case BlittableLeafKind::Vec2: {
            const auto* v = std::get_if<Vec2Value>(&fv);
            if (!v) { outNeedsDemote = true; return false; }
            std::memcpy(p, v, sizeof(*v));
            break;
        }
        case BlittableLeafKind::Vec3: {
            const auto* v = std::get_if<Vec3Value>(&fv);
            if (!v) { outNeedsDemote = true; return false; }
            std::memcpy(p, v, sizeof(*v));
            break;
        }
        case BlittableLeafKind::Quat: {
            QuatValue qv{};
            if (!fv.tryGetQuat(qv)) { outNeedsDemote = true; return false; }
            std::memcpy(p, &qv, sizeof(qv));
            break;
        }
        case BlittableLeafKind::Vec2f: {
            const auto* v = std::get_if<Vec2fValue>(&fv);
            if (!v) { outNeedsDemote = true; return false; }
            std::memcpy(p, v, sizeof(*v));
            break;
        }
        case BlittableLeafKind::Vec3f: {
            const auto* v = std::get_if<Vec3fValue>(&fv);
            if (!v) { outNeedsDemote = true; return false; }
            std::memcpy(p, v, sizeof(*v));
            break;
        }
        case BlittableLeafKind::Quatf: {
            QuatfValue qv{};
            if (!fv.tryGetQuatf(qv)) { outNeedsDemote = true; return false; }
            std::memcpy(p, &qv, sizeof(qv));
            break;
        }
        case BlittableLeafKind::NestedStruct: {
            const auto* childSchema = reg.getByIdLockFree(f.nestedSchemaId);
            if (!childSchema ||
                !encodeBlittableStructBytesInPlace(reg, p, *childSchema, fv, outNeedsDemote)) {
                return false;
            }
            break;
        }
        }
    }
    return true;
}

[[nodiscard]] bool encodeBlittableStructBytes(
    const BlittableStructRegistry& reg,
    std::uint8_t* elemBase,
    const BlittableStructSchema& schema,
    const Value& value,
    bool& outNeedsDemote) noexcept {
    if (schema.strideBytes == 0u || !elemBase) {
        return false;
    }
    std::array<std::uint8_t, 256> stackBuf{};
    std::vector<std::uint8_t> heapBuf;
    std::uint8_t* const staging = (schema.strideBytes <= stackBuf.size())
        ? stackBuf.data()
        : (heapBuf.resize(schema.strideBytes, 0u), heapBuf.data());
    if (!encodeBlittableStructBytesInPlace(reg, staging, schema, value, outNeedsDemote)) {
        return false;
    }
    std::memcpy(elemBase, staging, schema.strideBytes);
    return true;
}

[[nodiscard]] inline std::size_t packedElementStride(
    PackedElemEncoding enc,
    std::uint8_t elemShift,
    std::uint32_t blittableSchemaId) noexcept {
    switch (enc) {
    case PackedElemEncoding::Bool:
    case PackedElemEncoding::Byte:
    case PackedElemEncoding::SByte:
    case PackedElemEncoding::Short:
    case PackedElemEncoding::UShort:
    case PackedElemEncoding::Char:
    case PackedElemEncoding::Int32:
    case PackedElemEncoding::UInt32:
    case PackedElemEncoding::Float32:
    case PackedElemEncoding::EnumInline:
    case PackedElemEncoding::Int64:
    case PackedElemEncoding::UInt64:
    case PackedElemEncoding::Float64:
    case PackedElemEncoding::Int64Wide:
    case PackedElemEncoding::EnumWide:
        return std::size_t{1} << elemShift;
    case PackedElemEncoding::Fixed24_8:
        return 4u;
    case PackedElemEncoding::Fixed48_16:
    case PackedElemEncoding::Fixed32_32:
    case PackedElemEncoding::Vec2:
    case PackedElemEncoding::Vec2f:
        return 8u;
    case PackedElemEncoding::Vec3:
    case PackedElemEncoding::Vec3f:
        return 12u;
    case PackedElemEncoding::Quat:
    case PackedElemEncoding::Quatf:
        return 16u;
    case PackedElemEncoding::BlittableStruct: {
        const auto* s = globalBlittableRegistry().getByIdLockFree(blittableSchemaId);
        return s ? static_cast<std::size_t>(s->strideBytes) : 0u;
    }
    case PackedElemEncoding::None:
        break;
    }
    return 0u;
}

struct PackedStorageSpec {
    SlotStorage storage = SlotStorage::Empty;
    PackedElemEncoding encoding = PackedElemEncoding::None;
    std::uint8_t elemShift = 0;
    std::uint32_t strideBytes = 0;
    std::uint32_t schemaId = 0xFFFFFFFFu;
};

[[nodiscard]] inline bool tryClassifyPackedArray(
    semantic::PrimitiveType elementType,
    semantic::SymbolId elementTypeId,
    const Value* initialSample,
    PackedStorageSpec& outSpec) noexcept {
    switch (elementType) {
    case semantic::PrimitiveType::Bool:
        outSpec = {SlotStorage::PackedB8, PackedElemEncoding::Bool, 0, 1u, 0xFFFFFFFFu};
        return true;
    case semantic::PrimitiveType::Byte:
        outSpec = {SlotStorage::PackedB8, PackedElemEncoding::Byte, 0, 1u, 0xFFFFFFFFu};
        return true;
    case semantic::PrimitiveType::SByte:
        outSpec = {SlotStorage::PackedB8, PackedElemEncoding::SByte, 0, 1u, 0xFFFFFFFFu};
        return true;
    case semantic::PrimitiveType::Short:
        outSpec = {SlotStorage::PackedB16, PackedElemEncoding::Short, 1, 2u, 0xFFFFFFFFu};
        return true;
    case semantic::PrimitiveType::UShort:
        outSpec = {SlotStorage::PackedB16, PackedElemEncoding::UShort, 1, 2u, 0xFFFFFFFFu};
        return true;
    case semantic::PrimitiveType::Char:
        outSpec = {SlotStorage::PackedB16, PackedElemEncoding::Char, 1, 2u, 0xFFFFFFFFu};
        return true;
    case semantic::PrimitiveType::Int: {
        std::int64_t initI64 = 0;
        if (initialSample && initialSample->tryGetInt64(initI64) &&
            (initI64 < std::numeric_limits<std::int32_t>::min() ||
             initI64 > std::numeric_limits<std::int32_t>::max())) {
            outSpec = {SlotStorage::PackedI64, PackedElemEncoding::Int64Wide, 3, 8u, 0xFFFFFFFFu};
        } else {
            outSpec = {SlotStorage::PackedI32, PackedElemEncoding::Int32, 2, 4u, 0xFFFFFFFFu};
        }
        return true;
    }
    case semantic::PrimitiveType::UInt:
        outSpec = {SlotStorage::PackedI32, PackedElemEncoding::UInt32, 2, 4u, 0xFFFFFFFFu};
        return true;
    case semantic::PrimitiveType::Float:
        outSpec = {SlotStorage::PackedI32, PackedElemEncoding::Float32, 2, 4u, 0xFFFFFFFFu};
        return true;
    case semantic::PrimitiveType::Enum: {
        if (elementTypeId == 0) return false;
        EnumValue ev{};
        if (initialSample && initialSample->tryGetEnum(ev) &&
            (ev.value < std::numeric_limits<std::int32_t>::min() ||
             ev.value > std::numeric_limits<std::int32_t>::max())) {
            outSpec = {SlotStorage::PackedI64, PackedElemEncoding::EnumWide, 3, 8u, 0xFFFFFFFFu};
        } else {
            outSpec = {SlotStorage::PackedI32, PackedElemEncoding::EnumInline, 2, 4u, 0xFFFFFFFFu};
        }
        return true;
    }
    case semantic::PrimitiveType::Long:
        outSpec = {SlotStorage::PackedI64, PackedElemEncoding::Int64, 3, 8u, 0xFFFFFFFFu};
        return true;
    case semantic::PrimitiveType::ULong:
        outSpec = {SlotStorage::PackedI64, PackedElemEncoding::UInt64, 3, 8u, 0xFFFFFFFFu};
        return true;
    case semantic::PrimitiveType::Double:
        outSpec = {SlotStorage::PackedI64, PackedElemEncoding::Float64, 3, 8u, 0xFFFFFFFFu};
        return true;
    case semantic::PrimitiveType::Struct: {
        if (elementTypeId == 0) return false;
        if (elementTypeId == fixedTypeId()) {
            outSpec = {SlotStorage::PackedStruct, PackedElemEncoding::Fixed24_8, 0, 4u, 0xFFFFFFFFu};
            return true;
        }
        if (elementTypeId == fixed16TypeId()) {
            outSpec = {SlotStorage::PackedStruct, PackedElemEncoding::Fixed48_16, 0, 8u, 0xFFFFFFFFu};
            return true;
        }
        if (elementTypeId == fixed64TypeId()) {
            outSpec = {SlotStorage::PackedStruct, PackedElemEncoding::Fixed32_32, 0, 8u, 0xFFFFFFFFu};
            return true;
        }
        if (elementTypeId == vec2TypeId()) {
            outSpec = {SlotStorage::PackedStruct, PackedElemEncoding::Vec2, 0, 8u, 0xFFFFFFFFu};
            return true;
        }
        if (elementTypeId == vec3TypeId()) {
            outSpec = {SlotStorage::PackedStruct, PackedElemEncoding::Vec3, 0, 12u, 0xFFFFFFFFu};
            return true;
        }
        if (elementTypeId == quatTypeId()) {
            outSpec = {SlotStorage::PackedStruct, PackedElemEncoding::Quat, 0, 16u, 0xFFFFFFFFu};
            return true;
        }
        if (elementTypeId == vec2fTypeId()) {
            outSpec = {SlotStorage::PackedStruct, PackedElemEncoding::Vec2f, 0, 8u, 0xFFFFFFFFu};
            return true;
        }
        if (elementTypeId == vec3fTypeId()) {
            outSpec = {SlotStorage::PackedStruct, PackedElemEncoding::Vec3f, 0, 12u, 0xFFFFFFFFu};
            return true;
        }
        if (elementTypeId == quatfTypeId()) {
            outSpec = {SlotStorage::PackedStruct, PackedElemEncoding::Quatf, 0, 16u, 0xFFFFFFFFu};
            return true;
        }
        auto& reg = globalBlittableRegistry();
        std::uint32_t sid = reg.findLatestSchemaId(elementTypeId);
        if (sid == 0u && initialSample) {
            if (const auto* sv = std::get_if<StructValue>(initialSample)) {
                std::lock_guard<std::mutex> regLock(reg.mutex);
                sid = tryInferBlittableSchemaFromSampleUnlocked(reg, *sv);
            }
        }
        if (sid == 0u) return false;
        const auto* schema = reg.getByIdLockFree(sid);
        if (!schema || schema->strideBytes == 0u) return false;
        if (initialSample) {
            std::array<std::uint8_t, 256> scratch{};
            std::vector<std::uint8_t> heapScratch;
            std::uint8_t* probe = schema->strideBytes <= scratch.size()
                ? scratch.data()
                : (heapScratch.resize(schema->strideBytes), heapScratch.data());
            bool needsDemote = false;
            if (!encodeBlittableStructBytes(reg, probe, *schema, *initialSample, needsDemote)) {
                return false;
            }
        }
        outSpec = {
            SlotStorage::PackedStruct,
            PackedElemEncoding::BlittableStruct,
            0,
            schema->strideBytes,
            sid,
        };
        return true;
    }
    default:
        return false;
    }
}

[[nodiscard]] inline Value readPackedElementValue(
    const void* basePtr,
    std::size_t index,
    PackedElemEncoding encoding,
    semantic::SymbolId elemTypeId,
    std::uint32_t blittableSchemaId = 0xFFFFFFFFu) noexcept {
    const auto* bytes = static_cast<const std::uint8_t*>(basePtr);
    switch (encoding) {
    case PackedElemEncoding::Bool:
        return Value{bytes[index] != 0u};
    case PackedElemEncoding::Byte:
        return Value{ByteValue{bytes[index]}};
    case PackedElemEncoding::SByte:
        return Value{SByteValue{static_cast<std::int8_t>(bytes[index])}};
    case PackedElemEncoding::Short:
        return Value{ShortValue{reinterpret_cast<const std::int16_t*>(bytes)[index]}};
    case PackedElemEncoding::UShort:
        return Value{UShortValue{reinterpret_cast<const std::uint16_t*>(bytes)[index]}};
    case PackedElemEncoding::Char:
        return Value{CharValue{reinterpret_cast<const std::uint16_t*>(bytes)[index]}};
    case PackedElemEncoding::Int32:
        return Value{static_cast<std::int64_t>(reinterpret_cast<const std::int32_t*>(bytes)[index])};
    case PackedElemEncoding::UInt32:
        return Value{UIntValue{reinterpret_cast<const std::uint32_t*>(bytes)[index]}};
    case PackedElemEncoding::Float32:
        return Value{FloatValue{reinterpret_cast<const float*>(bytes)[index]}};
    case PackedElemEncoding::EnumInline:
        return Value{EnumValue{elemTypeId, static_cast<std::int64_t>(reinterpret_cast<const std::int32_t*>(bytes)[index])}};
    case PackedElemEncoding::Int64:
        return Value{LongValue{reinterpret_cast<const std::int64_t*>(bytes)[index]}};
    case PackedElemEncoding::UInt64:
        return Value{ULongValue{reinterpret_cast<const std::uint64_t*>(bytes)[index]}};
    case PackedElemEncoding::Float64:
        return Value{reinterpret_cast<const double*>(bytes)[index]};
    case PackedElemEncoding::Int64Wide:
        return Value{reinterpret_cast<const std::int64_t*>(bytes)[index]};
    case PackedElemEncoding::EnumWide:
        return Value{EnumValue{elemTypeId, reinterpret_cast<const std::int64_t*>(bytes)[index]}};
    case PackedElemEncoding::Fixed24_8:
        return Value{FixedValue{reinterpret_cast<const std::int32_t*>(bytes)[index]}};
    case PackedElemEncoding::Fixed48_16:
        return Value{Fixed16Value{reinterpret_cast<const std::int64_t*>(bytes)[index]}};
    case PackedElemEncoding::Fixed32_32:
        return Value{Fixed64Value{reinterpret_cast<const std::int64_t*>(bytes)[index]}};
    case PackedElemEncoding::Vec2: {
        Vec2Value v{};
        std::memcpy(&v, bytes + index * sizeof(Vec2Value), sizeof(v));
        return Value{v};
    }
    case PackedElemEncoding::Vec3: {
        Vec3Value v{};
        std::memcpy(&v, bytes + index * sizeof(Vec3Value), sizeof(v));
        return Value{v};
    }
    case PackedElemEncoding::Quat: {
        QuatValue v{};
        std::memcpy(&v, bytes + index * sizeof(QuatValue), sizeof(v));
        return Value{v};
    }
    case PackedElemEncoding::Vec2f: {
        Vec2fValue v{};
        std::memcpy(&v, bytes + index * sizeof(Vec2fValue), sizeof(v));
        return Value{v};
    }
    case PackedElemEncoding::Vec3f: {
        Vec3fValue v{};
        std::memcpy(&v, bytes + index * sizeof(Vec3fValue), sizeof(v));
        return Value{v};
    }
    case PackedElemEncoding::Quatf: {
        QuatfValue v{};
        std::memcpy(&v, bytes + index * sizeof(QuatfValue), sizeof(v));
        return Value{v};
    }
    case PackedElemEncoding::BlittableStruct: {
        const auto& reg = globalBlittableRegistry();
        const auto* schema = reg.getByIdLockFree(blittableSchemaId);
        if (!schema) return Value{};
        return decodeBlittableStructBytes(
            reg, bytes + index * static_cast<std::size_t>(schema->strideBytes), *schema);
    }
    case PackedElemEncoding::None:
        break;
    }
    return Value{};
}

[[nodiscard]] inline bool writePackedElementValue(
    void* basePtr,
    std::size_t index,
    PackedElemEncoding encoding,
    semantic::SymbolId elemTypeId,
    const Value& value,
    bool& outNeedsWidenTo64,
    std::uint32_t blittableSchemaId = 0xFFFFFFFFu) noexcept {
    outNeedsWidenTo64 = false;
    auto* bytes = static_cast<std::uint8_t*>(basePtr);
    switch (encoding) {
    case PackedElemEncoding::Bool: {
        bool b = false;
        if (!value.tryGetBool(b)) return false;
        bytes[index] = b ? 1u : 0u;
        return true;
    }
    case PackedElemEncoding::Byte: {
        const auto* v = std::get_if<ByteValue>(&value);
        if (!v) return false;
        bytes[index] = v->value;
        return true;
    }
    case PackedElemEncoding::SByte: {
        const auto* v = std::get_if<SByteValue>(&value);
        if (!v) return false;
        bytes[index] = static_cast<std::uint8_t>(v->value);
        return true;
    }
    case PackedElemEncoding::Short: {
        const auto* v = std::get_if<ShortValue>(&value);
        if (!v) return false;
        reinterpret_cast<std::int16_t*>(bytes)[index] = v->value;
        return true;
    }
    case PackedElemEncoding::UShort: {
        const auto* v = std::get_if<UShortValue>(&value);
        if (!v) return false;
        reinterpret_cast<std::uint16_t*>(bytes)[index] = v->value;
        return true;
    }
    case PackedElemEncoding::Char: {
        const auto* v = std::get_if<CharValue>(&value);
        if (!v) return false;
        reinterpret_cast<std::uint16_t*>(bytes)[index] = v->value;
        return true;
    }
    case PackedElemEncoding::Int32: {
        std::int64_t iv = 0;
        if (!value.tryGetInt64(iv)) return false;
        if (iv < std::numeric_limits<std::int32_t>::min() ||
            iv > std::numeric_limits<std::int32_t>::max()) {
            outNeedsWidenTo64 = true;
            return false;
        }
        reinterpret_cast<std::int32_t*>(bytes)[index] = static_cast<std::int32_t>(iv);
        return true;
    }
    case PackedElemEncoding::UInt32: {
        const auto* v = std::get_if<UIntValue>(&value);
        if (!v) return false;
        reinterpret_cast<std::uint32_t*>(bytes)[index] = v->value;
        return true;
    }
    case PackedElemEncoding::Float32: {
        float fv = 0.0f;
        if (!value.tryGetFloat(fv)) return false;
        reinterpret_cast<float*>(bytes)[index] = fv;
        return true;
    }
    case PackedElemEncoding::EnumInline: {
        EnumValue ev{};
        if (!value.tryGetEnum(ev) || ev.typeId != elemTypeId) return false;
        if (ev.value < std::numeric_limits<std::int32_t>::min() ||
            ev.value > std::numeric_limits<std::int32_t>::max()) {
            outNeedsWidenTo64 = true;
            return false;
        }
        reinterpret_cast<std::int32_t*>(bytes)[index] = static_cast<std::int32_t>(ev.value);
        return true;
    }
    case PackedElemEncoding::Int64: {
        std::int64_t lv = 0;
        if (!value.tryGetLong(lv)) return false;
        reinterpret_cast<std::int64_t*>(bytes)[index] = lv;
        return true;
    }
    case PackedElemEncoding::UInt64: {
        const auto* v = std::get_if<ULongValue>(&value);
        if (!v) return false;
        reinterpret_cast<std::uint64_t*>(bytes)[index] = v->value;
        return true;
    }
    case PackedElemEncoding::Float64: {
        double dv = 0.0;
        if (!value.tryGetDouble(dv)) return false;
        reinterpret_cast<double*>(bytes)[index] = dv;
        return true;
    }
    case PackedElemEncoding::Int64Wide: {
        std::int64_t iv = 0;
        if (!value.tryGetInt64(iv)) return false;
        reinterpret_cast<std::int64_t*>(bytes)[index] = iv;
        return true;
    }
    case PackedElemEncoding::EnumWide: {
        EnumValue ev{};
        if (!value.tryGetEnum(ev) || ev.typeId != elemTypeId) return false;
        reinterpret_cast<std::int64_t*>(bytes)[index] = ev.value;
        return true;
    }
    case PackedElemEncoding::Fixed24_8: {
        const auto* v = std::get_if<FixedValue>(&value);
        if (!v) return false;
        reinterpret_cast<std::int32_t*>(bytes)[index] = v->raw;
        return true;
    }
    case PackedElemEncoding::Fixed48_16: {
        const auto* v = std::get_if<Fixed16Value>(&value);
        if (!v) return false;
        reinterpret_cast<std::int64_t*>(bytes)[index] = v->raw;
        return true;
    }
    case PackedElemEncoding::Fixed32_32: {
        const auto* v = std::get_if<Fixed64Value>(&value);
        if (!v) return false;
        reinterpret_cast<std::int64_t*>(bytes)[index] = v->raw;
        return true;
    }
    case PackedElemEncoding::Vec2: {
        const auto* v = std::get_if<Vec2Value>(&value);
        if (!v) return false;
        std::memcpy(bytes + index * sizeof(Vec2Value), v, sizeof(*v));
        return true;
    }
    case PackedElemEncoding::Vec3: {
        const auto* v = std::get_if<Vec3Value>(&value);
        if (!v) return false;
        std::memcpy(bytes + index * sizeof(Vec3Value), v, sizeof(*v));
        return true;
    }
    case PackedElemEncoding::Quat: {
        QuatValue qv{};
        if (!value.tryGetQuat(qv)) return false;
        std::memcpy(bytes + index * sizeof(QuatValue), &qv, sizeof(qv));
        return true;
    }
    case PackedElemEncoding::Vec2f: {
        const auto* v = std::get_if<Vec2fValue>(&value);
        if (!v) return false;
        std::memcpy(bytes + index * sizeof(Vec2fValue), v, sizeof(*v));
        return true;
    }
    case PackedElemEncoding::Vec3f: {
        const auto* v = std::get_if<Vec3fValue>(&value);
        if (!v) return false;
        std::memcpy(bytes + index * sizeof(Vec3fValue), v, sizeof(*v));
        return true;
    }
    case PackedElemEncoding::Quatf: {
        QuatfValue qv{};
        if (!value.tryGetQuatf(qv)) return false;
        std::memcpy(bytes + index * sizeof(QuatfValue), &qv, sizeof(qv));
        return true;
    }
    case PackedElemEncoding::BlittableStruct: {
        const auto& reg = globalBlittableRegistry();
        const auto* schema = reg.getByIdLockFree(blittableSchemaId);
        if (!schema) return false;
        return encodeBlittableStructBytes(
            reg,
            bytes + index * static_cast<std::size_t>(schema->strideBytes),
            *schema,
            value,
            outNeedsWidenTo64);
    }
    case PackedElemEncoding::None:
        break;
    }
    return false;
}

namespace {

bool decodeBlittableLeavesRecursive(
    const BlittableStructRegistry& reg,
    const std::uint8_t* elemBase,
    const BlittableStructSchema& bSchema,
    const FlatStructSchema& fSchema,
    Value* dstLeaves) noexcept {
    if (bSchema.fields.size() != fSchema.directFields.size()) return false;
    for (std::size_t i = 0; i < bSchema.fields.size(); ++i) {
        const auto& bf = bSchema.fields[i];
        const auto& ff = fSchema.directFields[i];
        const std::uint8_t* p = elemBase + bf.byteOffset;
        Value& outLeaf = dstLeaves[ff.leafOffset];
        switch (bf.kind) {
        case BlittableLeafKind::Bool:
            outLeaf = Value{p[0] != 0u};
            break;
        case BlittableLeafKind::Byte:
            outLeaf = Value{ByteValue{p[0]}};
            break;
        case BlittableLeafKind::SByte:
            outLeaf = Value{SByteValue{static_cast<std::int8_t>(p[0])}};
            break;
        case BlittableLeafKind::Short: {
            std::int16_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            outLeaf = Value{ShortValue{v}};
            break;
        }
        case BlittableLeafKind::UShort: {
            std::uint16_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            outLeaf = Value{UShortValue{v}};
            break;
        }
        case BlittableLeafKind::Char: {
            std::uint16_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            outLeaf = Value{CharValue{v}};
            break;
        }
        case BlittableLeafKind::Int32: {
            std::int32_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            outLeaf = Value{static_cast<std::int64_t>(v)};
            break;
        }
        case BlittableLeafKind::UInt32: {
            std::uint32_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            outLeaf = Value{UIntValue{v}};
            break;
        }
        case BlittableLeafKind::Float32: {
            float v = 0.0f;
            std::memcpy(&v, p, sizeof(v));
            outLeaf = Value{FloatValue{v}};
            break;
        }
        case BlittableLeafKind::Enum32: {
            std::int32_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            outLeaf = Value{EnumValue{bf.fieldTypeId, static_cast<std::int64_t>(v)}};
            break;
        }
        case BlittableLeafKind::Int64: {
            std::int64_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            outLeaf = Value{LongValue{v}};
            break;
        }
        case BlittableLeafKind::UInt64: {
            std::uint64_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            outLeaf = Value{ULongValue{v}};
            break;
        }
        case BlittableLeafKind::Float64: {
            double v = 0.0;
            std::memcpy(&v, p, sizeof(v));
            outLeaf = Value{v};
            break;
        }
        case BlittableLeafKind::Fixed24_8: {
            std::int32_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            outLeaf = Value{FixedValue{v}};
            break;
        }
        case BlittableLeafKind::Fixed48_16: {
            std::int64_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            outLeaf = Value{Fixed16Value{v}};
            break;
        }
        case BlittableLeafKind::Fixed32_32: {
            std::int64_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            outLeaf = Value{Fixed64Value{v}};
            break;
        }
        case BlittableLeafKind::Vec2: {
            Vec2Value v{};
            std::memcpy(&v, p, sizeof(v));
            outLeaf = Value{v};
            break;
        }
        case BlittableLeafKind::Vec3: {
            Vec3Value v{};
            std::memcpy(&v, p, sizeof(v));
            outLeaf = Value{v};
            break;
        }
        case BlittableLeafKind::Quat: {
            QuatValue v{};
            std::memcpy(&v, p, sizeof(v));
            outLeaf = Value{v};
            break;
        }
        case BlittableLeafKind::Vec2f: {
            Vec2fValue v{};
            std::memcpy(&v, p, sizeof(v));
            outLeaf = Value{v};
            break;
        }
        case BlittableLeafKind::Vec3f: {
            Vec3fValue v{};
            std::memcpy(&v, p, sizeof(v));
            outLeaf = Value{v};
            break;
        }
        case BlittableLeafKind::Quatf: {
            QuatfValue v{};
            std::memcpy(&v, p, sizeof(v));
            outLeaf = Value{v};
            break;
        }
        case BlittableLeafKind::NestedStruct: {
            const auto* childB = reg.getByIdLockFree(bf.nestedSchemaId);
            const auto* childF = getFlatStructSchema(ff.subSchemaId);
            if (!childB || !childF ||
                !decodeBlittableLeavesRecursive(reg, p, *childB, *childF, dstLeaves + ff.leafOffset)) {
                return false;
            }
            break;
        }
        }
    }
    return true;
}

} // namespace

bool decodePackedStructToLeaves(
    const void* basePtr,
    std::size_t index,
    std::uint32_t blittableSchemaId,
    const FlatStructSchema& flatSchema,
    Value* dstLeaves) noexcept {
    if (!basePtr || !dstLeaves) return false;
    const auto& reg = globalBlittableRegistry();
    const auto* bSchema = reg.getByIdLockFree(blittableSchemaId);
    if (!bSchema || bSchema->structTypeId != flatSchema.structTypeId) return false;
    const auto* elemBytes = static_cast<const std::uint8_t*>(basePtr) +
        index * static_cast<std::size_t>(bSchema->strideBytes);
    if (bSchema->fields.empty()) {
        dstLeaves[0] = Value{};
        return true;
    }
    return decodeBlittableLeavesRecursive(reg, elemBytes, *bSchema, flatSchema, dstLeaves);
}

bool writePackedStructLeafSlice(
    void* basePtr,
    std::size_t index,
    std::uint32_t blittableSchemaId,
    const FlatStructSchema& flatSchema,
    std::uint16_t targetLeafOffset,
    std::uint16_t subSchemaId,
    const Value& rhs,
    bool& outNeedsDemote) noexcept {
    outNeedsDemote = false;
    if (!basePtr) return false;
    const auto& reg = globalBlittableRegistry();
    const auto* curB = reg.getByIdLockFree(blittableSchemaId);
    const FlatStructSchema* curF = &flatSchema;
    if (!curB || curB->structTypeId != curF->structTypeId || curB->strideBytes == 0u) return false;
    std::uint8_t* ptr = static_cast<std::uint8_t*>(basePtr) +
        index * static_cast<std::size_t>(curB->strideBytes);
    std::uint16_t remLeaf = targetLeafOffset;

    for (;;) {
        if (curB->fields.size() != curF->directFields.size()) return false;
        bool advanced = false;
        for (std::size_t i = 0; i < curF->directFields.size(); ++i) {
            const auto& ff = curF->directFields[i];
            const auto& bf = curB->fields[i];
            if (remLeaf < ff.leafOffset || remLeaf >= ff.leafOffset + ff.leafCount) {
                continue;
            }
            ptr += bf.byteOffset;
            remLeaf = static_cast<std::uint16_t>(remLeaf - ff.leafOffset);
            if (remLeaf == 0u && ff.subSchemaId == subSchemaId) {
                if (subSchemaId != 0u) {
                    const auto* subB = reg.getByIdLockFree(bf.nestedSchemaId);
                    if (!subB) return false;
                    return encodeBlittableStructBytes(reg, ptr, *subB, rhs, outNeedsDemote);
                }
                // Scalar / inline-math leaf write directly into packed bytes
                switch (bf.kind) {
                case BlittableLeafKind::Bool: {
                    bool bv = false;
                    if (!rhs.tryGetBool(bv)) { outNeedsDemote = true; return false; }
                    ptr[0] = bv ? 1u : 0u;
                    return true;
                }
                case BlittableLeafKind::Byte: {
                    const auto* v = std::get_if<ByteValue>(&rhs);
                    if (!v) { outNeedsDemote = true; return false; }
                    ptr[0] = v->value;
                    return true;
                }
                case BlittableLeafKind::SByte: {
                    const auto* v = std::get_if<SByteValue>(&rhs);
                    if (!v) { outNeedsDemote = true; return false; }
                    ptr[0] = static_cast<std::uint8_t>(v->value);
                    return true;
                }
                case BlittableLeafKind::Short: {
                    const auto* v = std::get_if<ShortValue>(&rhs);
                    if (!v) { outNeedsDemote = true; return false; }
                    std::memcpy(ptr, &v->value, sizeof(v->value));
                    return true;
                }
                case BlittableLeafKind::UShort: {
                    const auto* v = std::get_if<UShortValue>(&rhs);
                    if (!v) { outNeedsDemote = true; return false; }
                    std::memcpy(ptr, &v->value, sizeof(v->value));
                    return true;
                }
                case BlittableLeafKind::Char: {
                    const auto* v = std::get_if<CharValue>(&rhs);
                    if (!v) { outNeedsDemote = true; return false; }
                    std::memcpy(ptr, &v->value, sizeof(v->value));
                    return true;
                }
                case BlittableLeafKind::Int32: {
                    std::int64_t iv = 0;
                    if (!rhs.tryGetInt64(iv) ||
                        iv < std::numeric_limits<std::int32_t>::min() ||
                        iv > std::numeric_limits<std::int32_t>::max()) {
                        outNeedsDemote = true;
                        return false;
                    }
                    const auto i32 = static_cast<std::int32_t>(iv);
                    std::memcpy(ptr, &i32, sizeof(i32));
                    return true;
                }
                case BlittableLeafKind::UInt32: {
                    const auto* v = std::get_if<UIntValue>(&rhs);
                    if (!v) { outNeedsDemote = true; return false; }
                    std::memcpy(ptr, &v->value, sizeof(v->value));
                    return true;
                }
                case BlittableLeafKind::Float32: {
                    float fl = 0.0f;
                    if (!rhs.tryGetFloat(fl)) { outNeedsDemote = true; return false; }
                    std::memcpy(ptr, &fl, sizeof(fl));
                    return true;
                }
                case BlittableLeafKind::Enum32: {
                    EnumValue ev{};
                    if (!rhs.tryGetEnum(ev) || ev.typeId != bf.fieldTypeId ||
                        ev.value < std::numeric_limits<std::int32_t>::min() ||
                        ev.value > std::numeric_limits<std::int32_t>::max()) {
                        outNeedsDemote = true;
                        return false;
                    }
                    const auto e32 = static_cast<std::int32_t>(ev.value);
                    std::memcpy(ptr, &e32, sizeof(e32));
                    return true;
                }
                case BlittableLeafKind::Int64: {
                    std::int64_t lv = 0;
                    if (!rhs.tryGetLong(lv)) { outNeedsDemote = true; return false; }
                    std::memcpy(ptr, &lv, sizeof(lv));
                    return true;
                }
                case BlittableLeafKind::UInt64: {
                    const auto* v = std::get_if<ULongValue>(&rhs);
                    if (!v) { outNeedsDemote = true; return false; }
                    std::memcpy(ptr, &v->value, sizeof(v->value));
                    return true;
                }
                case BlittableLeafKind::Float64: {
                    double dv = 0.0;
                    if (!rhs.tryGetDouble(dv)) { outNeedsDemote = true; return false; }
                    std::memcpy(ptr, &dv, sizeof(dv));
                    return true;
                }
                case BlittableLeafKind::Fixed24_8: {
                    const auto* v = std::get_if<FixedValue>(&rhs);
                    if (!v) { outNeedsDemote = true; return false; }
                    std::memcpy(ptr, &v->raw, sizeof(v->raw));
                    return true;
                }
                case BlittableLeafKind::Fixed48_16: {
                    const auto* v = std::get_if<Fixed16Value>(&rhs);
                    if (!v) { outNeedsDemote = true; return false; }
                    std::memcpy(ptr, &v->raw, sizeof(v->raw));
                    return true;
                }
                case BlittableLeafKind::Fixed32_32: {
                    const auto* v = std::get_if<Fixed64Value>(&rhs);
                    if (!v) { outNeedsDemote = true; return false; }
                    std::memcpy(ptr, &v->raw, sizeof(v->raw));
                    return true;
                }
                case BlittableLeafKind::Vec2: {
                    const auto* v = std::get_if<Vec2Value>(&rhs);
                    if (!v) { outNeedsDemote = true; return false; }
                    std::memcpy(ptr, v, sizeof(*v));
                    return true;
                }
                case BlittableLeafKind::Vec3: {
                    const auto* v = std::get_if<Vec3Value>(&rhs);
                    if (!v) { outNeedsDemote = true; return false; }
                    std::memcpy(ptr, v, sizeof(*v));
                    return true;
                }
                case BlittableLeafKind::Quat: {
                    QuatValue qv{};
                    if (!rhs.tryGetQuat(qv)) { outNeedsDemote = true; return false; }
                    std::memcpy(ptr, &qv, sizeof(qv));
                    return true;
                }
                case BlittableLeafKind::Vec2f: {
                    const auto* v = std::get_if<Vec2fValue>(&rhs);
                    if (!v) { outNeedsDemote = true; return false; }
                    std::memcpy(ptr, v, sizeof(*v));
                    return true;
                }
                case BlittableLeafKind::Vec3f: {
                    const auto* v = std::get_if<Vec3fValue>(&rhs);
                    if (!v) { outNeedsDemote = true; return false; }
                    std::memcpy(ptr, v, sizeof(*v));
                    return true;
                }
                case BlittableLeafKind::Quatf: {
                    QuatfValue qv{};
                    if (!rhs.tryGetQuatf(qv)) { outNeedsDemote = true; return false; }
                    std::memcpy(ptr, &qv, sizeof(qv));
                    return true;
                }
                case BlittableLeafKind::NestedStruct:
                    return false;
                }
                return false;
            }
            if (bf.kind != BlittableLeafKind::NestedStruct || ff.subSchemaId == 0u) {
                return false;
            }
            curB = reg.getByIdLockFree(bf.nestedSchemaId);
            curF = getFlatStructSchema(ff.subSchemaId);
            if (!curB || !curF) return false;
            advanced = true;
            break;
        }
        if (!advanced) return false;
    }
}

void registerBlittableStructTypes(
    const std::vector<std::pair<semantic::SymbolId, std::vector<BlittableFieldInput>>>& structTypes) {
    if (structTypes.empty()) return;
    detail::registerFlatStructTypes(structTypes);
    auto& reg = globalBlittableRegistry();
    std::lock_guard<std::mutex> lock(reg.mutex);

    std::unordered_map<semantic::SymbolId, const std::vector<BlittableFieldInput>*> pending;
    pending.reserve(structTypes.size());
    for (const auto& [tid, fields] : structTypes) {
        if (tid == 0 || isBuiltinMathStructId(tid)) continue;
        pending[tid] = &fields;
    }

    // 0 = unvisited, 1 = visiting (cycle), 2 = resolved blittable, 3 = resolved non-blittable
    std::unordered_map<semantic::SymbolId, int> state;
    const auto resolve = [&](auto& self, semantic::SymbolId tid) -> std::uint32_t {
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
        if (fields.empty() || fields.size() > 256u) {
            reg.markNonBlittableUnlocked(tid);
            state[tid] = 3;
            return 0u;
        }
        std::vector<BlittableFieldEntry> entries;
        entries.reserve(fields.size());
        std::uint32_t offset = 0u;
        std::uint32_t maxAlign = 1u;
        for (const auto& f : fields) {
            if (f.type == semantic::PrimitiveType::Struct &&
                f.typeId != 0 &&
                !isBuiltinMathStructId(f.typeId)) {
                const std::uint32_t childSid = self(self, f.typeId);
                if (childSid == 0u) {
                    reg.markNonBlittableUnlocked(tid);
                    state[tid] = 3;
                    return 0u;
                }
                const auto* childSchema = reg.getByIdLockFree(childSid);
                if (!childSchema || childSchema->strideBytes == 0u) {
                    reg.markNonBlittableUnlocked(tid);
                    state[tid] = 3;
                    return 0u;
                }
                offset = alignUpU32(offset, childSchema->alignBytes);
                maxAlign = std::max(maxAlign, childSchema->alignBytes);
                entries.push_back(BlittableFieldEntry{
                    BlittableLeafKind::NestedStruct, offset, f.typeId, childSid});
                offset += childSchema->strideBytes;
                continue;
            }
            BlittableLeafKind lKind{};
            std::uint32_t lSize = 0, lAlign = 1;
            if (!classifyLeafFieldSpec(f.type, f.typeId, lKind, lSize, lAlign)) {
                reg.markNonBlittableUnlocked(tid);
                state[tid] = 3;
                return 0u;
            }
            offset = alignUpU32(offset, lAlign);
            maxAlign = std::max(maxAlign, lAlign);
            entries.push_back(BlittableFieldEntry{lKind, offset, f.typeId, 0u});
            offset += lSize;
        }
        const std::uint32_t stride = alignUpU32(offset, maxAlign);
        if (stride == 0u || stride > (1u << 20u)) {
            reg.markNonBlittableUnlocked(tid);
            state[tid] = 3;
            return 0u;
        }
        const std::uint32_t sid = reg.publishUnlocked(tid, stride, maxAlign, std::move(entries));
        if (sid == 0u) {
            reg.markNonBlittableUnlocked(tid);
            state[tid] = 3;
            return 0u;
        }
        state[tid] = 2;
        return sid;
    };

    for (const auto& [tid, fields] : structTypes) {
        (void)fields;
        if (tid == 0 || isBuiltinMathStructId(tid)) continue;
        (void)resolve(resolve, tid);
    }
}

bool isBlittableStructType(semantic::SymbolId structTypeId) noexcept {
    if (structTypeId == 0) return false;
    if (isBuiltinMathStructId(structTypeId)) return true;
    return globalBlittableRegistry().findLatestSchemaId(structTypeId) != 0u;
}

std::optional<Value> makeDefaultBlittableStructValue(
    semantic::SymbolId structTypeId) noexcept {
    if (structTypeId == 0) return std::nullopt;
    if (structTypeId == fixedTypeId()) return Value{FixedValue{}};
    if (structTypeId == fixed16TypeId()) return Value{Fixed16Value{}};
    if (structTypeId == fixed64TypeId()) return Value{Fixed64Value{}};
    if (structTypeId == vec2TypeId()) return Value{Vec2Value{}};
    if (structTypeId == vec3TypeId()) return Value{Vec3Value{}};
    if (structTypeId == quatTypeId()) return Value{QuatValue{}};
    if (structTypeId == vec2fTypeId()) return Value{Vec2fValue{}};
    if (structTypeId == vec3fTypeId()) return Value{Vec3fValue{}};
    if (structTypeId == quatfTypeId()) return Value{QuatfValue{}};
    auto& reg = globalBlittableRegistry();
    const std::uint32_t sid = reg.findLatestSchemaId(structTypeId);
    if (sid == 0u) return std::nullopt;
    const auto* schema = reg.getByIdLockFree(sid);
    if (!schema || schema->strideBytes == 0u) return std::nullopt;
    std::array<std::uint8_t, 256> stackZero{};
    std::vector<std::uint8_t> heapZero;
    const std::uint8_t* const zeroPtr = (schema->strideBytes <= stackZero.size())
        ? stackZero.data()
        : (heapZero.resize(schema->strideBytes, 0u), heapZero.data());
    return decodeBlittableStructBytes(reg, zeroPtr, *schema);
}

Value readPackedStructElement(
    const void* basePtr,
    std::size_t index,
    PackedElemEncoding encoding,
    semantic::SymbolId elemTypeId,
    std::uint32_t blittableSchemaId) noexcept {
    return readPackedElementValue(basePtr, index, encoding, elemTypeId, blittableSchemaId);
}

bool writePackedStructElement(
    void* basePtr,
    std::size_t index,
    PackedElemEncoding encoding,
    semantic::SymbolId elemTypeId,
    std::uint32_t blittableSchemaId,
    const Value& value,
    bool& outNeedsDemote) noexcept {
    return writePackedElementValue(
        basePtr, index, encoding, elemTypeId, value, outNeedsDemote, blittableSchemaId);
}

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

alignas(16) static std::uint8_t gEmptyPackedSentinel[16] = {};

class PackedByteBufferPool {
public:
    static constexpr std::size_t kAlignment = 16;

    struct AlignedBlock {
        std::uint8_t* ptr = nullptr;
        std::size_t cap = 0;

        AlignedBlock() noexcept = default;
        ~AlignedBlock() { reset(); }
        AlignedBlock(const AlignedBlock&) = delete;
        AlignedBlock& operator=(const AlignedBlock&) = delete;
        AlignedBlock(AlignedBlock&& other) noexcept
            : ptr(std::exchange(other.ptr, nullptr)),
              cap(std::exchange(other.cap, 0)) {}
        AlignedBlock& operator=(AlignedBlock&& other) noexcept {
            if (this != &other) {
                reset();
                ptr = std::exchange(other.ptr, nullptr);
                cap = std::exchange(other.cap, 0);
            }
            return *this;
        }

        void ensureZeroed(std::size_t byteSize) {
            if (byteSize == 0) return;
            if (cap < byteSize || cap > byteSize * 4u) {
                reset();
                ptr = static_cast<std::uint8_t*>(
                    ::operator new(byteSize, std::align_val_t{kAlignment}));
                cap = byteSize;
            }
            std::memset(ptr, 0, byteSize);
        }

        void reset() noexcept {
            if (ptr) {
                ::operator delete(ptr, cap, std::align_val_t{kAlignment});
                ptr = nullptr;
                cap = 0;
            }
        }
    };

    std::uint32_t acquireZeroed(std::size_t byteSize, Value*& outAlignedPtr) {
        if (byteSize == 0) {
            outAlignedPtr = reinterpret_cast<Value*>(gEmptyPackedSentinel);
            return CompactSlot::kInvalidHandle;
        }
        std::uint32_t id = 0;
        if (!freeIds_.empty()) {
            id = freeIds_.back();
            freeIds_.pop_back();
        } else {
            id = static_cast<std::uint32_t>(blocks_.size());
            blocks_.emplace_back();
        }
        blocks_[id].ensureZeroed(byteSize);
        outAlignedPtr = reinterpret_cast<Value*>(blocks_[id].ptr);
        return id;
    }

    void release(std::uint32_t id) noexcept {
        if (id >= blocks_.size()) return;
        if (blocks_[id].cap > 65536u) {
            blocks_[id].reset();
        }
        freeIds_.push_back(id);
    }

    void clear() noexcept {
        blocks_.clear();
        freeIds_.clear();
    }

private:
    std::vector<AlignedBlock> blocks_;
    std::vector<std::uint32_t> freeIds_;
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
            if (s.isFrameSlice()) {
                const auto* sc = getFlatStructSchema(s.frameSchemaId());
                return sc ? sc->containsManagedReferences : true;
            }
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
        if (!value.isTraceable() || value.rawTag() == ValueTag::ObjectRef) return false;
        if (value.rawTag() == ValueTag::Struct && value.asStructUnchecked().isFrameSlice()) {
            return false;
        }
        return true;
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
        if (!mayContainPoolValues.exchange(true, std::memory_order_acq_rel)) {
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
        if (st == SlotStorage::FlatStruct) {
            const auto* sc = getFlatStructSchema(slot.auxMeta);
            return sc ? static_cast<std::size_t>(slot.payload.s.length) * sc->flatSlotCount : 0u;
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
        case SlotStorage::FlatStruct:
            return BaseObjectSizeBytes + slotValueCount(slot) * sizeof(Value);
        case SlotStorage::PackedB8:
        case SlotStorage::PackedB16:
        case SlotStorage::PackedI32:
        case SlotStorage::PackedI64:
            return BaseObjectSizeBytes +
                (static_cast<std::size_t>(slot.payload.s.length) << unpackPackedShift(slot.auxMeta));
        case SlotStorage::PackedStruct:
            return BaseObjectSizeBytes +
                static_cast<std::size_t>(slot.payload.s.length) *
                packedElementStride(
                    unpackPackedEncoding(slot.auxMeta), 0u, slot.payload.s.cardHandle);
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
        case SlotStorage::FlatStruct:
            if (slot.payload.s.handle != CompactSlot::kInvalidHandle) {
                largeBuffers.release(slot.payload.s.handle);
            }
            break;
        case SlotStorage::PackedB8:
        case SlotStorage::PackedB16:
        case SlotStorage::PackedI32:
        case SlotStorage::PackedI64:
        case SlotStorage::PackedStruct:
            packedBuffers.release(slot.payload.s.handle);
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
    std::atomic<std::uint64_t> gcAndLayoutEpoch{1};
    std::vector<CompactSlot> slots;
    std::vector<std::uint32_t> freeSlots;
    std::vector<ObjectRef> markStack;
    std::vector<ObjectRef> youngWorkQueue;
    std::vector<std::uint32_t> nurserySlots;
    std::vector<ObjectRef> rememberedSet;

    PagedValueSlabPool slabPool;
    LargeValueBufferPool largeBuffers;
    PackedByteBufferPool packedBuffers;
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
    std::vector<const ShadowStack*> activeShadowStacks;
    std::unordered_map<semantic::SymbolId, Value> staticFields;
    std::unordered_map<std::string, NativeEcsColumnTable> nativeEcsTables;
    std::unordered_map<std::string, ObjectRef> internedStrings;
    bool hasDirtyTrackingTypes = false;
    std::unordered_map<semantic::SymbolId, std::vector<std::uint8_t>> typeDirtyBitMaps;
    std::unordered_map<semantic::SymbolId, semantic::SymbolId> typeBaseIds;
    std::unordered_map<std::uint32_t, std::uint64_t> observerDirtyMasks;
    NativeWrapperSweepCallback nativeWrapperSweepCallback;

    struct PendingWrapperSweep {
        NativeHandle handle{};
        std::uint64_t heapId = 0;
        ObjectRef wrapper{};
    };
    std::vector<PendingWrapperSweep> pendingWrapperSweeps;

    void flushPendingWrapperSweeps() {
        std::vector<PendingWrapperSweep> pending;
        NativeWrapperSweepCallback callback;
        {
            HeapSyncMutexGuard heapLock(syncState->mutex);
            if (pendingWrapperSweeps.empty()) {
                return;
            }
            pending.swap(pendingWrapperSweeps);
            callback = nativeWrapperSweepCallback;
        }
        if (!callback) {
            return;
        }
        for (const auto& entry : pending) {
            if (!NativeHandleRegistry::shouldDispatchSweepNotification(
                    entry.handle, entry.heapId, entry.wrapper)) {
                continue;
            }
            callback(entry.handle, entry.heapId, entry.wrapper);
        }
    }

    void markDirtyForFieldUnlocked(
        ObjectRef reference,
        semantic::SymbolId concreteTypeId,
        std::size_t fieldIndex) noexcept {
        if (!hasDirtyTrackingTypes || concreteTypeId == 0) {
            return;
        }
        semantic::SymbolId cur = concreteTypeId;
        for (int depth = 0; depth < 64 && cur != 0; ++depth) {
            const auto mapIt = typeDirtyBitMaps.find(cur);
            if (mapIt != typeDirtyBitMaps.end() && fieldIndex < mapIt->second.size()) {
                const std::uint8_t bit = mapIt->second[fieldIndex];
                if (bit < 64u) {
                    observerDirtyMasks[reference.slot] |= (std::uint64_t{1} << bit);
                }
            }
            const auto baseIt = typeBaseIds.find(cur);
            if (baseIt == typeBaseIds.end() || baseIt->second == cur) {
                break;
            }
            cur = baseIt->second;
        }
    }
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

    bool widenPackedSlotTo64Unlocked(std::uint32_t slotIndex, CompactSlot& slot) {
        const auto oldEnc = unpackPackedEncoding(slot.auxMeta);
        if (oldEnc != PackedElemEncoding::Int32 && oldEnc != PackedElemEncoding::EnumInline) {
            return false;
        }
        const std::size_t len = slot.payload.s.length;
        const std::size_t oldBytes = len << 2u;
        const std::size_t newBytes = len << 3u;
        const std::size_t delta = newBytes - oldBytes;
        if (!slot.hasFlag(GcFlag::CustomSize) &&
            statistics.liveBytes + delta > config.maximumHeapBytes) {
            return false;
        }
        Value* newAlignedPtr = nullptr;
        const std::uint32_t newHandle = packedBuffers.acquireZeroed(newBytes, newAlignedPtr);
        const auto* src32 = reinterpret_cast<const std::int32_t*>(slot.payload.s.data);
        auto* dst64 = reinterpret_cast<std::int64_t*>(newAlignedPtr);
        for (std::size_t i = 0; i < len; ++i) {
            dst64[i] = static_cast<std::int64_t>(src32[i]);
        }
        packedBuffers.release(slot.payload.s.handle);
        const auto newEnc = (oldEnc == PackedElemEncoding::Int32)
            ? PackedElemEncoding::Int64Wide
            : PackedElemEncoding::EnumWide;
        slot.setKindAndStorage(ObjectKind::Array, SlotStorage::PackedI64);
        slot.auxMeta = packPackedAuxMeta(newEnc, 3u);
        slot.payload.s.data = newAlignedPtr;
        slot.payload.s.handle = newHandle;
        if (!slot.hasFlag(GcFlag::CustomSize)) {
            statistics.bytesAllocated += delta;
            statistics.liveBytes += delta;
            statistics.peakLiveBytes = std::max(statistics.peakLiveBytes, statistics.liveBytes);
        }
        (void)slotIndex;
        gcAndLayoutEpoch.fetch_add(1, std::memory_order_release);
        return true;
    }

    bool demotePackedStructSlotToValuesUnlocked(std::uint32_t slotIndex, CompactSlot& slot) {
        if (slot.storage() != SlotStorage::PackedStruct) return false;
        const std::size_t len = slot.payload.s.length;
        const std::size_t valStorage = estimateValueStorage(len);
        if (valStorage == std::numeric_limits<std::size_t>::max()) return false;
        const std::size_t oldTotalBytes = slotSizeBytes(slotIndex, slot);
        const std::size_t newTotalBytes = BaseObjectSizeBytes + valStorage;
        if (!slot.hasFlag(GcFlag::CustomSize) &&
            newTotalBytes > oldTotalBytes &&
            statistics.liveBytes + (newTotalBytes - oldTotalBytes) > config.maximumHeapBytes) {
            return false;
        }
        const auto enc = unpackPackedEncoding(slot.auxMeta);
        const auto elemTid = typeLayouts[slot.payload.s.layoutId].elementTypeId;
        const std::uint32_t oldSchemaId = slot.payload.s.cardHandle;
        std::vector<Value> materialized(len);
        for (std::size_t i = 0; i < len; ++i) {
            materialized[i] = readPackedElementValue(
                slot.payload.s.data, i, enc, elemTid, oldSchemaId);
            pinHeapValueIfNeededUnlocked(materialized[i]);
        }
        const std::uint32_t oldBufHandle = slot.payload.s.handle;
        const std::uint32_t layoutId = slot.payload.s.layoutId;
        if (len <= CompactSlot::kInlineValueCapacity) {
            std::uint16_t cls = 0;
            std::uint32_t handle = CompactSlot::kInvalidHandle;
            Value* ptr = slabPool.allocate(len, cls, handle);
            for (std::size_t i = 0; i < len; ++i) {
                ptr[i] = std::move(materialized[i]);
            }
            packedBuffers.release(oldBufHandle);
            slot.setKindAndStorage(ObjectKind::Array, SlotStorage::SlabValues);
            slot.auxMeta = cls;
            slot.payload.s = {
                layoutId,
                static_cast<std::uint32_t>(len),
                ptr,
                handle,
                CompactSlot::kInvalidHandle,
            };
        } else {
            Value* ptr = nullptr;
            const std::uint32_t handle = largeBuffers.acquireMove(std::move(materialized), ptr);
            std::uint32_t cardHandle = CompactSlot::kInvalidHandle;
            if (!slot.hasFlag(GcFlag::InNursery) &&
                len >= ManagedHeap::kCardTableMinLength &&
                typeLayouts[layoutId].mayContainReferences) {
                cardHandle = cardArena.allocate(len);
                slot.gcFlags |= GcFlag::HasCardTable;
            }
            packedBuffers.release(oldBufHandle);
            slot.setKindAndStorage(ObjectKind::Array, SlotStorage::HeapValues);
            slot.auxMeta = 0;
            slot.payload.s = {
                layoutId,
                static_cast<std::uint32_t>(len),
                ptr,
                handle,
                cardHandle,
            };
        }
        if (!slot.hasFlag(GcFlag::CustomSize)) {
            if (newTotalBytes >= oldTotalBytes) {
                const std::size_t delta = newTotalBytes - oldTotalBytes;
                statistics.bytesAllocated += delta;
                statistics.liveBytes += delta;
                statistics.peakLiveBytes = std::max(statistics.peakLiveBytes, statistics.liveBytes);
                if (statistics.liveBytes >= nextCollectionThreshold) requested = true;
            } else {
                const std::size_t delta = oldTotalBytes - newTotalBytes;
                statistics.liveBytes = (statistics.liveBytes >= delta) ? (statistics.liveBytes - delta) : 0;
                statistics.bytesReclaimed += delta;
            }
        }
        gcAndLayoutEpoch.fetch_add(1, std::memory_order_release);
        return true;
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
        PackedStorageSpec packedSpec{};
        const Value* packedInitSample =
            (length > 0 && initialValue.rawTag() != ValueTag::Monostate)
                ? &initialValue
                : nullptr;
        if (length <= static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) &&
            tryClassifyPackedArray(
                elementType,
                elementTypeId,
                packedInitSample,
                packedSpec) &&
            packedSpec.strideBytes > 0u &&
            length <= (std::numeric_limits<std::size_t>::max() - BaseObjectSizeBytes) /
                static_cast<std::size_t>(packedSpec.strideBytes)) {
            const std::size_t packedBytes = length * static_cast<std::size_t>(packedSpec.strideBytes);
            const std::size_t packedTotalBytes = BaseObjectSizeBytes + packedBytes;
            const auto slotIndex = acquireSlot(packedTotalBytes, error, outReallocated);
            if (!slotIndex) return std::nullopt;
            Value* alignedPtr = nullptr;
            const std::uint32_t bufHandle = packedBuffers.acquireZeroed(packedBytes, alignedPtr);
            if (packedInitSample != nullptr) {
                bool dummyWiden = false;
                Value zeroSample = readPackedElementValue(
                    alignedPtr, 0, packedSpec.encoding, elementTypeId, packedSpec.schemaId);
                if (!(zeroSample == initialValue)) {
                    for (std::size_t i = 0; i < length; ++i) {
                        (void)writePackedElementValue(
                            alignedPtr,
                            i,
                            packedSpec.encoding,
                            elementTypeId,
                            initialValue,
                            dummyWiden,
                            packedSpec.schemaId);
                    }
                }
            }
            auto& slot = slots[*slotIndex];
            slot.auxMeta = packPackedAuxMeta(packedSpec.encoding, packedSpec.elemShift);
            slot.payload.s = {
                layoutId,
                static_cast<std::uint32_t>(length),
                alignedPtr,
                bufHandle,
                packedSpec.schemaId,
            };
            return finalizeSlotAllocation(*slotIndex, ObjectKind::Array, packedSpec.storage, packedTotalBytes);
        }
        if (elementType == semantic::PrimitiveType::Struct &&
            elementTypeId != 0 &&
            length <= static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
            if (const auto* flatSchema = findFlatStructSchemaByTypeId(elementTypeId);
                flatSchema != nullptr &&
                flatSchema->schemaId != 0u &&
                flatSchema->flatSlotCount >= 1u &&
                length <= (std::numeric_limits<std::size_t>::max() - BaseObjectSizeBytes) /
                    (static_cast<std::size_t>(flatSchema->flatSlotCount) * sizeof(Value))) {
                const std::size_t stride = flatSchema->flatSlotCount;
                const std::size_t totalVals = length * stride;
                const std::size_t flatTotalBytes = BaseObjectSizeBytes + totalVals * sizeof(Value);
                const auto slotIndex = acquireSlot(flatTotalBytes, error, outReallocated);
                if (!slotIndex) return std::nullopt;
                if (flatSchema->containsPoolValues) {
                    noteMaybePoolBackedUnlocked();
                }
                Value* ptr = nullptr;
                std::uint32_t bufHandle = CompactSlot::kInvalidHandle;
                if (totalVals > 0) {
                    std::vector<Value> elemProto(stride);
                    if (!extractStructLeavesToBuffer(initialValue, *flatSchema, elemProto.data())) {
                        elemProto = flatSchema->prototypeLeaves;
                    }
                    for (const auto& leaf : elemProto) {
                        pinHeapValueIfNeededUnlocked(leaf);
                    }
                    std::vector<Value> initBuf(totalVals);
                    for (std::size_t i = 0; i < length; ++i) {
                        std::memcpy(
                            static_cast<void*>(initBuf.data() + i * stride),
                            static_cast<const void*>(elemProto.data()),
                            stride * sizeof(Value));
                    }
                    bufHandle = largeBuffers.acquireMove(std::move(initBuf), ptr);
                }
                auto& slot = slots[*slotIndex];
                slot.auxMeta = flatSchema->schemaId;
                slot.payload.s = {
                    layoutId,
                    static_cast<std::uint32_t>(length),
                    ptr,
                    bufHandle,
                    CompactSlot::kInvalidHandle,
                };
                return finalizeSlotAllocation(*slotIndex, ObjectKind::Array, SlotStorage::FlatStruct, flatTotalBytes);
            }
        }
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
        for (auto& v : vals) {
            if (v.rawTag() == ValueTag::Struct && v.asStructUnchecked().isFrameSlice()) {
                v = detachFrameSliceValue(v);
            }
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
            (slot.storage() == SlotStorage::HeapValues || slot.storage() == SlotStorage::FlatStruct) &&
            slot.payload.s.length >= ManagedHeap::kCardTableMinLength &&
            !slot.hasFlag(GcFlag::HasCardTable)) {
            const auto& layout = typeLayouts[slot.payload.s.layoutId];
            bool needsCards = layout.mayContainReferences;
            if (slot.storage() == SlotStorage::FlatStruct) {
                if (const auto* sc = getFlatStructSchema(slot.auxMeta)) {
                    needsCards = sc->containsManagedReferences;
                }
            }
            if (needsCards) {
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
        if (hasDirtyTrackingTypes && !observerDirtyMasks.empty()) {
            observerDirtyMasks.erase(slotIdx);
        }
        if (nativeWrapperSweepCallback && slot.kind() == ObjectKind::Record &&
            slot.payload.s.data != nullptr) {
            const ObjectRef sweptRef(
                slotIdx,
                slot.generation,
                ObjectKind::Record,
                heapId);
            for (std::uint32_t fi = 0; fi < slot.payload.s.length; ++fi) {
                const auto& fv = slot.payload.s.data[fi];
                if (const auto* nh = std::get_if<NativeHandle>(&fv); nh && nh->valid()) {
                    const bool duplicate = std::any_of(
                        pendingWrapperSweeps.begin(),
                        pendingWrapperSweeps.end(),
                        [&](const PendingWrapperSweep& existing) {
                            return existing.handle == *nh &&
                                existing.heapId == heapId &&
                                existing.wrapper == sweptRef;
                        });
                    if (!duplicate) {
                        pendingWrapperSweeps.push_back(
                            PendingWrapperSweep{*nh, heapId, sweptRef});
                    }
                }
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
        for (const ShadowStack* activeStack : activeShadowStacks) {
            if (activeStack && activeStack != &shadowStack) {
                activeStack->visitTraceableRoots([&](const Value& value) { markValue(value); });
            }
        }
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
            if (slot->storage() == SlotStorage::FlatStruct) {
                const auto* sc = getFlatStructSchema(slot->auxMeta);
                if (!sc || !sc->containsManagedReferences) return;
                const std::size_t elemCount = slot->payload.s.length;
                const std::size_t stride = sc->flatSlotCount;
                for (std::size_t e = 0; e < elemCount; ++e) {
                    const Value* elemBase = data + e * stride;
                    for (const auto refOff : sc->referenceLeafOffsets) {
                        markValue(elemBase[refOff]);
                    }
                }
                return;
            }
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

    void writeBarrierFlatStructElement(ObjectRef owner, std::size_t elementIndex) noexcept {
        auto* slot = getSlot(owner);
        if (!slot || slot->storage() != SlotStorage::FlatStruct) return;
        const auto* sc = getFlatStructSchema(slot->auxMeta);
        if (!sc || !sc->containsManagedReferences || elementIndex >= slot->payload.s.length) return;
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
        const Value* elemBase = slot->payload.s.data + elementIndex * static_cast<std::size_t>(sc->flatSlotCount);
        for (const auto refOff : sc->referenceLeafOffsets) {
            markValue(elemBase[refOff]);
        }
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
        gcAndLayoutEpoch.fetch_add(1, std::memory_order_release);
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
                   (roots.frameCount() > 0 || !activeShadowStacks.empty()) &&
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
    const auto* flatSchemaPre =
        (elementType == semantic::PrimitiveType::Struct && elementTypeId != 0)
            ? findFlatStructSchemaByTypeId(elementTypeId)
            : nullptr;
    const bool isDefaultFlatStructSentinel =
        flatSchemaPre != nullptr && initialValue.rawTag() == ValueTag::Monostate;
    if (length > 0 &&
        !isDefaultFlatStructSentinel &&
        elementType != semantic::PrimitiveType::Error &&
        valueType(initialValue) != elementType) {
        setHeapError(error, ErrorCode::TypeMismatch,
            "managed array initial value does not match its element type");
        return std::nullopt;
    }
    PackedStorageSpec preSpec{};
    std::size_t storage = 0;
    const Value* preSample =
        (length > 0 && initialValue.rawTag() != ValueTag::Monostate) ? &initialValue : nullptr;
    if (length <= static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) &&
        tryClassifyPackedArray(elementType, elementTypeId, preSample, preSpec) &&
        preSpec.strideBytes > 0u) {
        if (length > std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(preSpec.strideBytes)) {
            storage = std::numeric_limits<std::size_t>::max();
        } else {
            storage = length * static_cast<std::size_t>(preSpec.strideBytes);
        }
    } else if (flatSchemaPre != nullptr &&
               length <= static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        const std::size_t elemBytes = static_cast<std::size_t>(flatSchemaPre->flatSlotCount) * sizeof(Value);
        if (elemBytes > 0 && length > std::numeric_limits<std::size_t>::max() / elemBytes) {
            storage = std::numeric_limits<std::size_t>::max();
        } else {
            storage = length * elemBytes;
        }
    } else {
        storage = estimateValueStorage(length);
    }
    if (storage == std::numeric_limits<std::size_t>::max() ||
        storage > std::numeric_limits<std::size_t>::max() - Impl::BaseObjectSizeBytes) {
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
    HeapReadOrDisjointGuard heapLock(*impl_->syncState);
    return impl_->getSlot(reference) != nullptr;
}

std::optional<std::string_view> ManagedHeap::stringView(ObjectRef reference) const {
    HeapReadOrDisjointGuard heapLock(*impl_->syncState);
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
    case NativeFieldKind::Fixed64: return 8;
    case NativeFieldKind::Vec2: return 8;
    case NativeFieldKind::Vec3: return 12;
    case NativeFieldKind::Quat: return 16;
    case NativeFieldKind::Vec2f: return 8;
    case NativeFieldKind::Vec3f: return 12;
    case NativeFieldKind::Quatf: return 16;
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
    case NativeFieldKind::Fixed64: {
        std::int64_t raw = 0;
        std::memcpy(&raw, ptr, sizeof(raw));
        return Fixed64Value{raw};
    }
    case NativeFieldKind::Vec2f: {
        Vec2fValue v{};
        std::memcpy(&v, ptr, sizeof(v));
        return v;
    }
    case NativeFieldKind::Vec3f: {
        Vec3fValue v{};
        std::memcpy(&v, ptr, sizeof(v));
        return v;
    }
    case NativeFieldKind::Quatf: {
        QuatfValue v{};
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
    case NativeFieldKind::Fixed64: {
        if (const auto* fx = std::get_if<Fixed64Value>(&val)) {
            std::memcpy(ptr, &fx->raw, sizeof(fx->raw));
            return true;
        }
        if (const auto f0 = loadInlineStructField(val, 0)) {
            if (const auto* i = std::get_if<std::int64_t>(&*f0)) {
                std::memcpy(ptr, i, sizeof(*i));
                return true;
            }
        }
        return false;
    }
    case NativeFieldKind::Vec2f: {
        if (const auto* v = std::get_if<Vec2fValue>(&val)) {
            std::memcpy(ptr, v, sizeof(*v));
            return true;
        }
        return false;
    }
    case NativeFieldKind::Vec3f: {
        if (const auto* v = std::get_if<Vec3fValue>(&val)) {
            std::memcpy(ptr, v, sizeof(*v));
            return true;
        }
        return false;
    }
    case NativeFieldKind::Quatf: {
        QuatfValue qv{};
        if (val.tryGetQuatf(qv)) {
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
    HeapReadOrDisjointGuard heapLock(*impl_->syncState);
    const auto* slot = impl_->getSlot(reference);
    if (!slot || slot->kind() != ObjectKind::Array) return std::nullopt;
    if (slot->storage() == SlotStorage::RawColumn) {
        return impl_->rawColumns.get(slot->payload.s.handle).count;
    }
    return static_cast<std::size_t>(slot->payload.s.length);
}

std::optional<semantic::SymbolId> ManagedHeap::arrayTypeId(ObjectRef reference) const {
    HeapReadOrDisjointGuard heapLock(*impl_->syncState);
    const auto* slot = impl_->getSlot(reference);
    if (!slot || slot->kind() != ObjectKind::Array) return std::nullopt;
    return impl_->typeLayouts[slot->payload.s.layoutId].typeId;
}

std::optional<semantic::PrimitiveType> ManagedHeap::arrayElementType(ObjectRef reference) const {
    HeapReadOrDisjointGuard heapLock(*impl_->syncState);
    const auto* slot = impl_->getSlot(reference);
    if (!slot || slot->kind() != ObjectKind::Array) return std::nullopt;
    return impl_->typeLayouts[slot->payload.s.layoutId].elementType;
}

std::optional<semantic::SymbolId> ManagedHeap::arrayElementTypeId(ObjectRef reference) const {
    HeapReadOrDisjointGuard heapLock(*impl_->syncState);
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
        HeapReadOrDisjointGuard heapLock(*impl_->syncState);
        const auto* slot = impl_->getSlot(reference);
        if (!slot || slot->kind() != ObjectKind::Array) return std::nullopt;
        if (isPackedStorage(slot->storage())) {
            if (index >= slot->payload.s.length) return std::nullopt;
            const auto elemTid = impl_->typeLayouts[slot->payload.s.layoutId].elementTypeId;
            return readPackedElementValue(
                slot->payload.s.data,
                index,
                unpackPackedEncoding(slot->auxMeta),
                elemTid,
                slot->payload.s.cardHandle);
        }
        if (slot->storage() == SlotStorage::FlatStruct) {
            if (index >= slot->payload.s.length) return std::nullopt;
            const auto* sc = getFlatStructSchema(slot->auxMeta);
            if (!sc) return std::nullopt;
            return reconstructDetachedStructFromLeaves(
                *sc,
                slot->payload.s.data + index * static_cast<std::size_t>(sc->flatSlotCount));
        }
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

bool ManagedHeap::isPackedArray(ObjectRef reference) const noexcept {
    HeapReadOrDisjointGuard heapLock(*impl_->syncState);
    const auto* slot = impl_->getSlot(reference);
    return slot && slot->kind() == ObjectKind::Array && isPackedStorage(slot->storage());
}

bool ManagedHeap::isFlatStructArray(ObjectRef reference) const noexcept {
    HeapReadOrDisjointGuard heapLock(*impl_->syncState);
    const auto* slot = impl_->getSlot(reference);
    return slot && slot->kind() == ObjectKind::Array && slot->storage() == SlotStorage::FlatStruct;
}

std::optional<std::size_t> ManagedHeap::flatStructArrayStride(ObjectRef reference) const noexcept {
    HeapReadOrDisjointGuard heapLock(*impl_->syncState);
    const auto* slot = impl_->getSlot(reference);
    if (!slot || slot->kind() != ObjectKind::Array || slot->storage() != SlotStorage::FlatStruct) {
        return std::nullopt;
    }
    const auto* sc = getFlatStructSchema(slot->auxMeta);
    if (!sc) return std::nullopt;
    return static_cast<std::size_t>(sc->flatSlotCount);
}

std::optional<std::size_t> ManagedHeap::packedElementStrideBytes(ObjectRef reference) const noexcept {
    HeapReadOrDisjointGuard heapLock(*impl_->syncState);
    const auto* slot = impl_->getSlot(reference);
    if (!slot || slot->kind() != ObjectKind::Array || !isPackedStorage(slot->storage())) {
        return std::nullopt;
    }
    const std::size_t stride = packedElementStride(
        unpackPackedEncoding(slot->auxMeta),
        unpackPackedShift(slot->auxMeta),
        slot->payload.s.cardHandle);
    if (stride == 0u) return std::nullopt;
    return stride;
}

std::optional<std::size_t> ManagedHeap::objectSizeBytes(ObjectRef reference) const noexcept {
    HeapReadOrDisjointGuard heapLock(*impl_->syncState);
    const auto* slot = impl_->getSlot(reference);
    if (!slot) return std::nullopt;
    return impl_->slotSizeBytes(reference.slot, *slot);
}

const Value* ManagedHeap::arrayGetFast(ObjectRef reference, std::size_t index) const noexcept {
    HeapReadOrDisjointGuard heapLock(*impl_->syncState);
    const auto* slot = impl_->getSlot(reference);
    if (!slot || slot->kind() != ObjectKind::Array ||
        slot->storage() == SlotStorage::RawColumn ||
        slot->storage() == SlotStorage::FlatStruct ||
        isPackedStorage(slot->storage()) ||
        index >= slot->payload.s.length) {
        return nullptr;
    }
    return &slot->payload.s.data[index];
}

bool ManagedHeap::tryBindPackedArrayFastView(
    ObjectRef reference,
    PackedArrayFastView& outView) const noexcept {
    HeapReadOrDisjointGuard heapLock(*impl_->syncState);
    auto* slot = impl_->getSlot(reference);
    if (!slot || slot->kind() != ObjectKind::Array || !isPackedStorage(slot->storage())) {
        outView = {};
        return false;
    }
    const std::uint32_t count = slot->payload.s.length;
    if (count > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
        outView = {};
        return false;
    }
    const auto& layout = impl_->typeLayouts[slot->payload.s.layoutId];
    const auto enc = unpackPackedEncoding(slot->auxMeta);
    const auto shift = unpackPackedShift(slot->auxMeta);
    const std::uint32_t schemaId = (enc == PackedElemEncoding::BlittableStruct)
        ? slot->payload.s.cardHandle
        : 0u;
    outView.rawData = slot->payload.s.data;
    outView.length = count;
    outView.encoding = enc;
    outView.elementType = layout.elementType;
    outView.elementTypeId = layout.elementTypeId;
    outView.strideBytes = static_cast<std::uint32_t>(packedElementStride(enc, shift, schemaId));
    outView.blittableSchemaId = schemaId;
    return outView.strideBytes != 0u;
}

bool ManagedHeap::tryBindPackedInt32Array(
    ObjectRef reference,
    std::int32_t*& outData,
    std::uint32_t& outLength) const noexcept {
    HeapReadOrDisjointGuard heapLock(*impl_->syncState);
    auto* slot = impl_->getSlot(reference);
    if (!slot || slot->kind() != ObjectKind::Array || slot->storage() != SlotStorage::PackedI32) {
        outData = nullptr;
        outLength = 0;
        return false;
    }
    const auto enc = unpackPackedEncoding(slot->auxMeta);
    if (enc != PackedElemEncoding::Int32 && enc != PackedElemEncoding::EnumInline) {
        outData = nullptr;
        outLength = 0;
        return false;
    }
    outData = reinterpret_cast<std::int32_t*>(slot->payload.s.data);
    outLength = slot->payload.s.length;
    return true;
}

bool ManagedHeap::tryBindPackedDoubleArray(
    ObjectRef reference,
    double*& outData,
    std::uint32_t& outLength) const noexcept {
    HeapReadOrDisjointGuard heapLock(*impl_->syncState);
    auto* slot = impl_->getSlot(reference);
    if (!slot || slot->kind() != ObjectKind::Array || slot->storage() != SlotStorage::PackedI64 ||
        unpackPackedEncoding(slot->auxMeta) != PackedElemEncoding::Float64) {
        outData = nullptr;
        outLength = 0;
        return false;
    }
    outData = reinterpret_cast<double*>(slot->payload.s.data);
    outLength = slot->payload.s.length;
    return true;
}

bool ManagedHeap::tryBindFlatStructArrayFastView(
    ObjectRef reference,
    FlatStructArrayFastView& outView) const noexcept {
    HeapReadOrDisjointGuard heapLock(*impl_->syncState);
    auto* slot = impl_->getSlot(reference);
    if (!slot || slot->kind() != ObjectKind::Array || slot->storage() != SlotStorage::FlatStruct) {
        outView = {};
        return false;
    }
    const std::uint32_t count = slot->payload.s.length;
    if (count > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
        outView = {};
        return false;
    }
    const auto* sc = getFlatStructSchema(slot->auxMeta);
    if (!sc || sc->flatSlotCount == 0u) {
        outView = {};
        return false;
    }
    if (sc->containsPoolValues && !impl_->mayContainPoolValues.load(std::memory_order_relaxed)) {
        impl_->noteMaybePoolBackedUnlocked();
    }
    const bool inNursery = slot->hasFlag(GcFlag::InNursery);
    outView.data = slot->payload.s.data;
    outView.dirtyCards = (!inNursery && slot->hasFlag(GcFlag::HasCardTable))
        ? impl_->cardArena.get(slot->payload.s.cardHandle).bytePtr()
        : nullptr;
    outView.length = count;
    outView.flatStride = sc->flatSlotCount;
    outView.flatSchemaId = sc->schemaId;
    outView.elementTypeId = impl_->typeLayouts[slot->payload.s.layoutId].elementTypeId;
    outView.containsManagedReferences = sc->containsManagedReferences;
    outView.containsPoolValues = sc->containsPoolValues;
    outView.needsRememberedBarrier = (!inNursery && !slot->hasFlag(GcFlag::InRememberedSet));
    return true;
}

void ManagedHeap::recordFlatStructArrayWriteBarrier(
    ObjectRef arrayRef,
    std::size_t elementIndex) noexcept {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    impl_->writeBarrierFlatStructElement(arrayRef, elementIndex);
}

std::uint64_t ManagedHeap::gcEpoch() const noexcept {
    return impl_->gcAndLayoutEpoch.load(std::memory_order_acquire);
}

void ManagedHeap::pinFlatStructArrayElement(
    ObjectRef arrayRef,
    std::size_t elementIndex) noexcept {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    auto* slot = impl_->getSlot(arrayRef);
    if (!slot || slot->kind() != ObjectKind::Array || slot->storage() != SlotStorage::FlatStruct) return;
    const auto* sc = getFlatStructSchema(slot->auxMeta);
    if (!sc || !sc->containsPoolValues || elementIndex >= slot->payload.s.length) return;
    const Value* elemBase = slot->payload.s.data + elementIndex * static_cast<std::size_t>(sc->flatSlotCount);
    for (std::uint16_t k = 0; k < sc->flatSlotCount; ++k) {
        impl_->pinHeapValueIfNeededUnlocked(elemBase[k]);
    }
}

bool ManagedHeap::tryBindArrayFastView(ObjectRef reference, ArrayFastView& outView) const noexcept {
    HeapReadOrDisjointGuard heapLock(*impl_->syncState);
    auto* slot = impl_->getSlot(reference);
    if (!slot || slot->kind() != ObjectKind::Array ||
        slot->storage() == SlotStorage::RawColumn ||
        slot->storage() == SlotStorage::FlatStruct ||
        isPackedStorage(slot->storage())) {
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
    HeapReadOrDisjointGuard heapLock(*impl_->syncState);
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
    HeapReadOrDisjointGuard heapLock(*impl_->syncState);
    return impl_->slots.data();
}

semantic::SymbolId ManagedHeap::objectTypeIdFast(ObjectRef reference) const noexcept {
    HeapReadOrDisjointGuard heapLock(*impl_->syncState);
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
    if (impl_->syncState->parallelLockFreeDepth.load(std::memory_order_acquire) > 0u) {
        auto* slot = impl_->getSlot(reference);
        if (slot && slot->kind() == ObjectKind::Array &&
            isPackedStorage(slot->storage()) &&
            index < slot->payload.s.length) {
            const auto elemTid = impl_->typeLayouts[slot->payload.s.layoutId].elementTypeId;
            bool needsWiden = false;
            return writePackedElementValue(
                slot->payload.s.data,
                index,
                unpackPackedEncoding(slot->auxMeta),
                elemTid,
                value,
                needsWiden,
                slot->payload.s.cardHandle);
        }
    }
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
    if (isPackedStorage(slot->storage())) {
        const auto elemTid = impl_->typeLayouts[slot->payload.s.layoutId].elementTypeId;
        bool needsWiden = false;
        if (writePackedElementValue(
                slot->payload.s.data,
                index,
                unpackPackedEncoding(slot->auxMeta),
                elemTid,
                value,
                needsWiden,
                slot->payload.s.cardHandle)) {
            return true;
        }
        if (needsWiden) {
            if (slot->storage() == SlotStorage::PackedStruct) {
                if (impl_->demotePackedStructSlotToValuesUnlocked(reference.slot, *slot)) {
                    if (mayBeReference) {
                        impl_->writeBarrierArrayElement(reference, index, value);
                    }
                    impl_->pinHeapValueIfNeededUnlocked(value);
                    slot->payload.s.data[index] = value;
                    return true;
                }
            } else if (impl_->widenPackedSlotTo64Unlocked(reference.slot, *slot)) {
                return writePackedElementValue(
                    slot->payload.s.data,
                    index,
                    unpackPackedEncoding(slot->auxMeta),
                    elemTid,
                    value,
                    needsWiden,
                    slot->payload.s.cardHandle);
            }
        }
        return false;
    }
    if (slot->storage() == SlotStorage::FlatStruct) {
        const auto* sc = getFlatStructSchema(slot->auxMeta);
        if (!sc) return false;
        Value* elemDst = slot->payload.s.data + index * static_cast<std::size_t>(sc->flatSlotCount);
        if (!extractStructLeavesToBuffer(value, *sc, elemDst)) {
            return false;
        }
        if (sc->containsPoolValues) {
            for (std::uint16_t k = 0; k < sc->flatSlotCount; ++k) {
                impl_->pinHeapValueIfNeededUnlocked(elemDst[k]);
            }
        }
        if (sc->containsManagedReferences) {
            impl_->writeBarrierFlatStructElement(reference, index);
        }
        return true;
    }
    Value stored = (value.rawTag() == ValueTag::Struct && value.asStructUnchecked().isFrameSlice())
        ? detachFrameSliceValue(value)
        : value;
    if (mayBeReference) {
        impl_->writeBarrierArrayElement(reference, index, stored);
    }
    impl_->pinHeapValueIfNeededUnlocked(stored);
    slot->payload.s.data[index] = std::move(stored);
    return true;
}

bool ManagedHeap::arraySet(
    ObjectRef reference,
    std::size_t index,
    Value value,
    RuntimeError* error,
    ArrayStoreTypePolicy typePolicy) {
    if (impl_->syncState->parallelLockFreeDepth.load(std::memory_order_acquire) > 0u) {
        auto* slot = impl_->getSlot(reference);
        if (slot && slot->kind() == ObjectKind::Array &&
            isPackedStorage(slot->storage())) {
            const auto valueLen = static_cast<std::size_t>(slot->payload.s.length);
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
            if (expected == semantic::PrimitiveType::Enum) {
                EnumValue enumeration{};
                if (!value.tryGetEnum(enumeration) || enumeration.typeId != expectedTypeId) {
                    setHeapError(error, ErrorCode::TypeMismatch,
                        "managed array enum element type mismatch");
                    return false;
                }
            }
            bool needsWiden = false;
            if (writePackedElementValue(
                    slot->payload.s.data,
                    index,
                    unpackPackedEncoding(slot->auxMeta),
                    expectedTypeId,
                    value,
                    needsWiden,
                    slot->payload.s.cardHandle)) {
                return true;
            }
            setHeapError(error, ErrorCode::TypeMismatch,
                needsWiden
                    ? "cannot widen 32-bit packed array during lock-free parallel execution"
                    : "managed array element type mismatch");
            return false;
        }
    }
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
    if (isPackedStorage(slot->storage())) {
        bool needsWiden = false;
        if (writePackedElementValue(
                slot->payload.s.data,
                index,
                unpackPackedEncoding(slot->auxMeta),
                expectedTypeId,
                value,
                needsWiden,
                slot->payload.s.cardHandle)) {
            return true;
        }
        if (needsWiden) {
            if (slot->storage() == SlotStorage::PackedStruct) {
                if (impl_->demotePackedStructSlotToValuesUnlocked(reference.slot, *slot)) {
                    impl_->pinHeapValueIfNeededUnlocked(value);
                    impl_->writeBarrierArrayElement(reference, index, value);
                    slot->payload.s.data[index] = std::move(value);
                    return true;
                }
            } else if (impl_->widenPackedSlotTo64Unlocked(reference.slot, *slot)) {
                if (writePackedElementValue(
                        slot->payload.s.data,
                        index,
                        unpackPackedEncoding(slot->auxMeta),
                        expectedTypeId,
                        value,
                        needsWiden,
                        slot->payload.s.cardHandle)) {
                    return true;
                }
            }
            setHeapError(error, ErrorCode::OutOfMemory,
                "managed packed array widening exceeded heap budget");
            return false;
        }
        setHeapError(error, ErrorCode::TypeMismatch,
            "managed array element type mismatch");
        return false;
    }
    if (slot->storage() == SlotStorage::FlatStruct) {
        const auto* sc = getFlatStructSchema(slot->auxMeta);
        if (!sc) {
            setHeapError(error, ErrorCode::InvalidProgram, "missing flat struct schema for array");
            return false;
        }
        Value* elemDst = values + index * static_cast<std::size_t>(sc->flatSlotCount);
        if (!extractStructLeavesToBuffer(value, *sc, elemDst)) {
            setHeapError(error, ErrorCode::TypeMismatch, "managed array flat struct element mismatch");
            return false;
        }
        if (sc->containsPoolValues) {
            for (std::uint16_t k = 0; k < sc->flatSlotCount; ++k) {
                impl_->pinHeapValueIfNeededUnlocked(elemDst[k]);
            }
        }
        if (sc->containsManagedReferences) {
            impl_->writeBarrierFlatStructElement(reference, index);
        }
        return true;
    }
    if (value.rawTag() == ValueTag::Struct && value.asStructUnchecked().isFrameSlice()) {
        value = detachFrameSliceValue(value);
    }
    if (Impl::valueMayContainReferences(value)) {
        impl_->writeBarrierArrayElement(reference, index, value);
    }
    impl_->pinHeapValueIfNeededUnlocked(value);
    values[index] = std::move(value);
    return true;
}

std::optional<semantic::SymbolId> ManagedHeap::objectTypeId(ObjectRef reference) const {
    HeapReadOrDisjointGuard heapLock(*impl_->syncState);
    const auto* slot = impl_->getSlot(reference);
    if (!slot || slot->kind() != ObjectKind::Record) return std::nullopt;
    const auto tid = impl_->typeLayouts[slot->payload.s.layoutId].typeId;
    if (tid == 0) return std::nullopt;
    return tid;
}

std::optional<std::size_t> ManagedHeap::fieldCount(ObjectRef reference) const {
    HeapReadOrDisjointGuard heapLock(*impl_->syncState);
    const auto* slot = impl_->getSlot(reference);
    if (!slot || slot->kind() != ObjectKind::Record) return std::nullopt;
    return static_cast<std::size_t>(slot->payload.s.length);
}

bool ManagedHeap::isInlineRecordObject(ObjectRef reference) const noexcept {
    HeapReadOrDisjointGuard heapLock(*impl_->syncState);
    const auto* slot = impl_->getSlot(reference);
    return slot && slot->kind() == ObjectKind::Record &&
        slot->storage() == SlotStorage::SlabValues;
}

std::optional<Value> ManagedHeap::fieldGet(ObjectRef reference, std::size_t index) const {
    HeapReadOrDisjointGuard heapLock(*impl_->syncState);
    const auto* slot = impl_->getSlot(reference);
    if (!slot || slot->kind() != ObjectKind::Record) return std::nullopt;
    if (index >= slot->payload.s.length) return std::nullopt;
    return slot->payload.s.data[index];
}

const Value* ManagedHeap::fieldGetFast(
    ObjectRef reference,
    std::size_t index,
    semantic::SymbolId* outTypeId) const noexcept {
    HeapReadOrDisjointGuard heapLock(*impl_->syncState);
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
    if (value.rawTag() == ValueTag::Struct && value.asStructUnchecked().isFrameSlice()) {
        value = detachFrameSliceValue(value);
    }
    const auto& refFields = impl_->typeLayouts[slot->payload.s.layoutId].referenceFields;
    if (Impl::valueMayContainReferences(value) &&
        !refFields.empty() &&
        std::binary_search(refFields.begin(), refFields.end(), index)) {
        impl_->writeBarrierObject(reference, value);
    }
    impl_->pinHeapValueIfNeededUnlocked(value);
    slot->payload.s.data[index] = std::move(value);
    if (impl_->hasDirtyTrackingTypes) {
        const auto tid = impl_->typeLayouts[slot->payload.s.layoutId].typeId;
        impl_->markDirtyForFieldUnlocked(reference, tid, index);
    }
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
    const Value stored = (value.rawTag() == ValueTag::Struct && value.asStructUnchecked().isFrameSlice())
        ? detachFrameSliceValue(value)
        : value;
    if (Impl::valueMayContainReferences(stored) &&
        !layout.referenceFields.empty() &&
        std::binary_search(layout.referenceFields.begin(), layout.referenceFields.end(), index)) {
        impl_->writeBarrierObject(reference, stored);
    }
    impl_->pinHeapValueIfNeededUnlocked(stored);
    slot->payload.s.data[index] = stored;
    if (impl_->hasDirtyTrackingTypes) {
        impl_->markDirtyForFieldUnlocked(reference, expectedTypeId, index);
    }
    return true;
}

void ManagedHeap::registerTypeParent(
    semantic::SymbolId typeId,
    semantic::SymbolId baseTypeId) {
    if (typeId == 0 || baseTypeId == 0 || typeId == baseTypeId) return;
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    impl_->typeBaseIds[typeId] = baseTypeId;
}

void ManagedHeap::registerTypeHierarchy(const ProgramImage& program) {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    for (const auto& [tid, ts] : program.types_) {
        if (ts && tid != 0 && ts->baseTypeId != 0 && ts->baseTypeId != tid) {
            impl_->typeBaseIds[tid] = ts->baseTypeId;
        }
    }
}

void ManagedHeap::notifyFieldMutated(
    ObjectRef reference,
    std::size_t fieldIndex) noexcept {
    if (!hasDirtyTracking_) return;
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    if (!impl_->hasDirtyTrackingTypes) return;
    auto* slot = impl_->getSlot(reference);
    if (!slot || slot->kind() != ObjectKind::Record ||
        fieldIndex >= slot->payload.s.length) {
        return;
    }
    const auto tid = impl_->typeLayouts[slot->payload.s.layoutId].typeId;
    impl_->markDirtyForFieldUnlocked(reference, tid, fieldIndex);
}

void ManagedHeap::enableTypeDirtyTracking(
    semantic::SymbolId typeId,
    std::vector<std::uint8_t> fieldToBitMap) {
    if (typeId == 0) return;
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    impl_->typeDirtyBitMaps[typeId] = std::move(fieldToBitMap);
    impl_->hasDirtyTrackingTypes = !impl_->typeDirtyBitMaps.empty();
    hasDirtyTracking_ = impl_->hasDirtyTrackingTypes;
}

bool ManagedHeap::markDirtyBit(ObjectRef reference, std::uint8_t bitIndex) noexcept {
    if (bitIndex >= 64u) return false;
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    if (!impl_->getSlot(reference)) return false;
    impl_->hasDirtyTrackingTypes = true;
    hasDirtyTracking_ = true;
    impl_->observerDirtyMasks[reference.slot] |= (std::uint64_t{1} << bitIndex);
    return true;
}

std::uint64_t ManagedHeap::peekDirtyMask(ObjectRef reference) const noexcept {
    HeapReadOrDisjointGuard heapLock(*impl_->syncState);
    if (!impl_->hasDirtyTrackingTypes || !impl_->getSlot(reference)) return 0u;
    const auto it = impl_->observerDirtyMasks.find(reference.slot);
    return it != impl_->observerDirtyMasks.end() ? it->second : 0u;
}

std::uint64_t ManagedHeap::consumeDirtyMask(ObjectRef reference) noexcept {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    if (!impl_->hasDirtyTrackingTypes || !impl_->getSlot(reference)) return 0u;
    const auto it = impl_->observerDirtyMasks.find(reference.slot);
    if (it == impl_->observerDirtyMasks.end()) return 0u;
    const std::uint64_t mask = it->second;
    impl_->observerDirtyMasks.erase(it);
    return mask;
}

namespace {

struct CrossHeapStringCopy {
    bool isNull = false;
    std::string text;
};

struct SanitizedValueTree {
    Value baseValue{};
    bool isCopiedString = false;
    std::size_t stringIndex = 0;
    bool isRebuiltStruct = false;
    semantic::SymbolId structTypeId = 0;
    std::vector<SanitizedValueTree> fields;

    [[nodiscard]] Value materialize(const std::vector<Value>& copiedStringValues) const {
        if (isCopiedString) {
            return copiedStringValues[stringIndex];
        }
        if (isRebuiltStruct) {
            std::vector<Value> builtFields;
            builtFields.reserve(fields.size());
            for (const auto& child : fields) {
                builtFields.push_back(child.materialize(copiedStringValues));
            }
            return Value{makeStructValueFromFields(structTypeId, std::move(builtFields))};
        }
        return baseValue;
    }
};

template <typename HandleObjectRefFn>
bool buildSanitizedValueTree(
    const Value& val,
    std::size_t depth,
    HandleObjectRefFn&& handleObjectRef,
    std::vector<CrossHeapStringCopy>& copiedStrings,
    SanitizedValueTree& outNode,
    RuntimeError* error) {
    constexpr std::size_t kMaxCrossHeapStructDepth = 32;
    if (const auto* ref = std::get_if<ObjectRef>(&val)) {
        return handleObjectRef(*ref, copiedStrings, outNode, error);
    }
    if (const auto* sv = std::get_if<StringValue>(&val)) {
        outNode.isCopiedString = true;
        outNode.stringIndex = copiedStrings.size();
        copiedStrings.push_back(CrossHeapStringCopy{false, sv->str()});
        return true;
    }
    if (const auto* st = std::get_if<StructValue>(&val)) {
        if (!st->valid() || st->isInline || !st->storage) {
            outNode.baseValue = val;
            return true;
        }
        if (depth >= kMaxCrossHeapStructDepth) {
            setHeapError(
                error,
                ErrorCode::CrossHeapAccessViolation,
                "nested struct depth exceeds cross-heap policy limit");
            return false;
        }
        const std::size_t count = st->fieldCount();
        outNode.isRebuiltStruct = true;
        outNode.structTypeId = st->typeId;
        outNode.fields.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            const auto fvOpt = structValueFieldAt(*st, i);
            if (!fvOpt) {
                setHeapError(
                    error,
                    ErrorCode::InvalidObjectReference,
                    "struct field unavailable during cross-heap transfer");
                return false;
            }
            if (!buildSanitizedValueTree(
                    *fvOpt,
                    depth + 1,
                    handleObjectRef,
                    copiedStrings,
                    outNode.fields[i],
                    error)) {
                return false;
            }
        }
        return true;
    }
    outNode.baseValue = val;
    return true;
}

} // namespace

bool ManagedHeap::readFieldWithPolicy(
    const ManagedHeap& targetHeap,
    ObjectRef target,
    std::size_t fieldIndex,
    CrossHeapAccessPolicy policy,
    Value& outValue,
    RuntimeError* error) const {
    const bool crossHeap = (&targetHeap != this) && (targetHeap.heapId() != heapId());
    if (crossHeap && policy == CrossHeapAccessPolicy::Deny) {
        setHeapError(
            error,
            ErrorCode::CrossHeapAccessViolation,
            "cross-heap field read denied by active CrossHeapAccessPolicy");
        return false;
    }
    const auto valOpt = targetHeap.fieldGet(target, fieldIndex);
    if (!valOpt) {
        setHeapError(
            error,
            ErrorCode::InvalidObjectReference,
            "target object is dead or field index is out of range");
        return false;
    }
    if (!crossHeap || policy != CrossHeapAccessPolicy::ReadOnlySanitizeStrings) {
        outValue = *valOpt;
        return true;
    }

    const auto targetHeapId = targetHeap.heapId();
    std::vector<CrossHeapStringCopy> copiedStrings;
    SanitizedValueTree root;
    const auto handleReadRef = [&](
        ObjectRef ref,
        std::vector<CrossHeapStringCopy>& pending,
        SanitizedValueTree& node,
        RuntimeError* err) -> bool {
        if (ref.kind != ObjectKind::String) {
            setHeapError(
                err,
                ErrorCode::CrossHeapAccessViolation,
                "cross-heap non-string ObjectRef rejected under ReadOnlySanitizeStrings");
            return false;
        }
        if (ref.heapId != 0 && ref.heapId != targetHeapId) {
            setHeapError(
                err,
                ErrorCode::CrossHeapAccessViolation,
                "foreign heap string reference cannot cross heaps");
            return false;
        }
        node.isCopiedString = true;
        node.stringIndex = pending.size();
        if (const auto sv = targetHeap.stringView(ref)) {
            pending.push_back(CrossHeapStringCopy{false, std::string(*sv)});
        } else {
            pending.push_back(CrossHeapStringCopy{true, {}});
        }
        return true;
    };
    if (!buildSanitizedValueTree(*valOpt, 0, handleReadRef, copiedStrings, root, error)) {
        return false;
    }
    if (copiedStrings.empty()) {
        outValue = *valOpt;
        return true;
    }

    std::vector<Value> allocatedStringValues(copiedStrings.size());
    {
        HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
        for (std::size_t i = 0; i < copiedStrings.size(); ++i) {
            if (copiedStrings[i].isNull) {
                allocatedStringValues[i] = Value{NullString{}};
                continue;
            }
            const auto size = Impl::BaseObjectSizeBytes + copiedStrings[i].text.size();
            auto ref = impl_->allocateStringSlot(std::move(copiedStrings[i].text), size, error);
            if (!ref) {
                return false;
            }
            allocatedStringValues[i] = Value{*ref};
        }
    }
    outValue = root.materialize(allocatedStringValues);
    return true;
}

bool ManagedHeap::writeFieldWithPolicy(
    ManagedHeap& targetHeap,
    ObjectRef target,
    std::size_t fieldIndex,
    Value newValue,
    CrossHeapAccessPolicy policy,
    RuntimeError* error) const {
    const bool crossHeap = (&targetHeap != this) && (targetHeap.heapId() != heapId());
    if (crossHeap && policy != CrossHeapAccessPolicy::AllowReadWrite) {
        setHeapError(
            error,
            ErrorCode::CrossHeapAccessViolation,
            "cross-heap field mutation rejected; dispatch a command envelope instead");
        return false;
    }
    if (!targetHeap.fieldGet(target, fieldIndex)) {
        setHeapError(
            error,
            ErrorCode::InvalidObjectReference,
            "target object is dead or field index is out of range");
        return false;
    }

    const auto targetHeapId = targetHeap.heapId();
    const auto callerHeapId = heapId();
    std::vector<CrossHeapStringCopy> copiedStrings;
    SanitizedValueTree root;
    const auto handleWriteRef = [&](
        ObjectRef ref,
        std::vector<CrossHeapStringCopy>& pending,
        SanitizedValueTree& node,
        RuntimeError* err) -> bool {
        if (ref.kind != ObjectKind::String) {
            if (ref.heapId != 0 && ref.heapId != targetHeapId) {
                setHeapError(
                    err,
                    ErrorCode::CrossHeapAccessViolation,
                    "foreign ObjectRef cannot be stored into targetHeap");
                return false;
            }
            node.baseValue = Value{ref};
            return true;
        }
        if (ref.heapId == targetHeapId) {
            node.baseValue = Value{ref};
            return true;
        }
        if (ref.heapId == callerHeapId && crossHeap) {
            const auto sv = stringView(ref);
            if (!sv) {
                setHeapError(
                    err,
                    ErrorCode::InvalidObjectReference,
                    "caller string reference is invalid");
                return false;
            }
            node.isCopiedString = true;
            node.stringIndex = pending.size();
            pending.push_back(CrossHeapStringCopy{false, std::string(*sv)});
            return true;
        }
        setHeapError(
            err,
            ErrorCode::CrossHeapAccessViolation,
            "foreign string ObjectRef cannot be stored into targetHeap");
        return false;
    };
    if (!buildSanitizedValueTree(newValue, 0, handleWriteRef, copiedStrings, root, error)) {
        return false;
    }
    if (copiedStrings.empty()) {
        return targetHeap.fieldSet(target, fieldIndex, std::move(newValue), error);
    }

    std::vector<Value> targetStringValues;
    targetStringValues.reserve(copiedStrings.size());
    for (auto& entry : copiedStrings) {
        auto allocated = targetHeap.allocateString(std::move(entry.text), error);
        if (!allocated) {
            return false;
        }
        targetStringValues.push_back(Value{*allocated});
    }
    return targetHeap.fieldSet(
        target, fieldIndex, root.materialize(targetStringValues), error);
}

void ManagedHeap::setNativeWrapperSweepCallback(NativeWrapperSweepCallback callback) {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    impl_->nativeWrapperSweepCallback = std::move(callback);
}

ManagedHeap::RootToken ManagedHeap::addPersistentRoot(ObjectRef reference) {
    return addPersistentRoot(Value{reference});
}

ManagedHeap::RootToken ManagedHeap::addPersistentRoot(Value value) {
    if (value.rawTag() == ValueTag::Struct && value.asStructUnchecked().isFrameSlice()) {
        value = detachFrameSliceValue(value);
    }
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
    HeapReadOrDisjointGuard heapLock(*impl_->syncState);
    return impl_->staticFields.find(id) != impl_->staticFields.end();
}

std::optional<Value> ManagedHeap::getStaticField(semantic::SymbolId id) const {
    HeapReadOrDisjointGuard heapLock(*impl_->syncState);
    const auto found = impl_->staticFields.find(id);
    if (found == impl_->staticFields.end()) return std::nullopt;
    return found->second;
}

void ManagedHeap::setStaticField(semantic::SymbolId id, Value value) {
    if (value.rawTag() == ValueTag::Struct && value.asStructUnchecked().isFrameSlice()) {
        value = detachFrameSliceValue(value);
    }
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
    if (component.rawTag() == ValueTag::Struct && component.asStructUnchecked().isFrameSlice()) {
        component = detachFrameSliceValue(component);
    }
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
    if (component.rawTag() == ValueTag::Struct && component.asStructUnchecked().isFrameSlice()) {
        component = detachFrameSliceValue(component);
    }
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
    impl_->flushPendingWrapperSweeps();
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
    impl_->flushPendingWrapperSweeps();
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
    impl_->flushPendingWrapperSweeps();
}

void ManagedHeap::pushActiveShadowStack(const ShadowStack* stack) noexcept {
    if (!impl_ || !stack) return;
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    impl_->activeShadowStacks.push_back(stack);
}

void ManagedHeap::popActiveShadowStack(const ShadowStack* stack) noexcept {
    if (!impl_ || !stack) return;
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    auto& stacks = impl_->activeShadowStacks;
    for (std::size_t i = stacks.size(); i > 0; --i) {
        if (stacks[i - 1] == stack) {
            stacks.erase(stacks.begin() + static_cast<std::ptrdiff_t>(i - 1));
            break;
        }
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
    impl_->flushPendingWrapperSweeps();
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
    for (const ShadowStack* activeStack : impl_->activeShadowStacks) {
        if (activeStack && activeStack != &shadowStack) {
            activeStack->visitTraceableRoots([&](const Value& val) { markYoungValue(markYoungValue, val); });
        }
    }
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
            if (oldSlot->storage() == SlotStorage::FlatStruct) {
                const auto* sc = getFlatStructSchema(oldSlot->auxMeta);
                if (!sc || !sc->containsManagedReferences) continue;
                const std::size_t elemCount = oldSlot->payload.s.length;
                const std::size_t stride = sc->flatSlotCount;
                if (oldSlot->hasFlag(GcFlag::HasCardTable)) {
                    auto& slice = impl_->cardArena.get(oldSlot->payload.s.cardHandle);
                    for (std::uint32_t w = 0; w < slice.wordCount; ++w) {
                        if (slice.words[w] == 0) continue;
                        auto* cardBytes = reinterpret_cast<std::uint8_t*>(&slice.words[w]);
                        for (std::uint32_t b = 0; b < 8; ++b) {
                            if (cardBytes[b] == 0) continue;
                            cardBytes[b] = 0;
                            const std::size_t cardIdx = (static_cast<std::size_t>(w) << 3) + b;
                            const std::size_t begin = cardIdx << ManagedHeap::kCardElementShift;
                            const std::size_t end = std::min(elemCount, begin + ManagedHeap::kCardElements);
                            for (std::size_t e = begin; e < end; ++e) {
                                const Value* elemBase = data + e * stride;
                                for (const auto refOff : sc->referenceLeafOffsets) {
                                    markYoungValue(markYoungValue, elemBase[refOff]);
                                }
                            }
                        }
                    }
                    continue;
                }
                for (std::size_t e = 0; e < elemCount; ++e) {
                    const Value* elemBase = data + e * stride;
                    for (const auto refOff : sc->referenceLeafOffsets) {
                        markYoungValue(markYoungValue, elemBase[refOff]);
                    }
                }
                continue;
            }
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
            impl_->gcAndLayoutEpoch.fetch_add(1, std::memory_order_release);
            if (impl_->statistics.liveBytes < impl_->nextCollectionThreshold) {
                impl_->requested = false;
            }
            stepResult = processed;
        }
    }
    if (handledIncremental && shouldSweepPools) {
        (void)reclaimPoolValuesInternal(&shadowStack, /*waitIfBusy=*/false);
    }
    impl_->flushPendingWrapperSweeps();
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
    if (isPackedStorage(slot->storage())) {
        const std::size_t len = slot->payload.s.length;
        const auto enc = unpackPackedEncoding(slot->auxMeta);
        out.resize(len);
        if (enc == PackedElemEncoding::Int32) {
            const auto* src = reinterpret_cast<const std::int32_t*>(slot->payload.s.data);
            for (std::size_t i = 0; i < len; ++i) out[i] = static_cast<std::int64_t>(src[i]);
            return true;
        }
        if (enc == PackedElemEncoding::Int64 || enc == PackedElemEncoding::Int64Wide) {
            if (len > 0) {
                std::memcpy(out.data(), slot->payload.s.data, len * sizeof(std::int64_t));
            }
            return true;
        }
        setHeapError(error, ErrorCode::TypeMismatch, "array element is not an integer");
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
    if (isPackedStorage(slot->storage())) {
        const std::size_t len = slot->payload.s.length;
        if (count > len) {
            setHeapError(error, ErrorCode::IndexOutOfRange, "bulk write exceeds array length");
            return false;
        }
        auto enc = unpackPackedEncoding(slot->auxMeta);
        if (enc == PackedElemEncoding::Int32) {
            bool anyWide = false;
            for (std::size_t i = 0; i < count; ++i) {
                if (data[i] < std::numeric_limits<std::int32_t>::min() ||
                    data[i] > std::numeric_limits<std::int32_t>::max()) {
                    anyWide = true;
                    break;
                }
            }
            if (anyWide) {
                if (!impl_->widenPackedSlotTo64Unlocked(reference.slot, *slot)) {
                    setHeapError(error, ErrorCode::OutOfMemory, "packed array widening failed");
                    return false;
                }
                enc = unpackPackedEncoding(slot->auxMeta);
            } else {
                auto* dst = reinterpret_cast<std::int32_t*>(slot->payload.s.data);
                for (std::size_t i = 0; i < count; ++i) dst[i] = static_cast<std::int32_t>(data[i]);
                return true;
            }
        }
        if (enc == PackedElemEncoding::Int64 || enc == PackedElemEncoding::Int64Wide) {
            if (count > 0) {
                std::memcpy(static_cast<void*>(slot->payload.s.data), data, count * sizeof(std::int64_t));
            }
            return true;
        }
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
    if (isPackedStorage(slot->storage())) {
        const std::size_t len = slot->payload.s.length;
        const auto enc = unpackPackedEncoding(slot->auxMeta);
        out.resize(len);
        if (enc == PackedElemEncoding::Float64) {
            if (len > 0) {
                std::memcpy(out.data(), slot->payload.s.data, len * sizeof(double));
            }
            return true;
        }
        if (enc == PackedElemEncoding::Float32) {
            const auto* src = reinterpret_cast<const float*>(slot->payload.s.data);
            for (std::size_t i = 0; i < len; ++i) out[i] = static_cast<double>(src[i]);
            return true;
        }
        setHeapError(error, ErrorCode::TypeMismatch, "array element is not a floating-point number");
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
    if (isPackedStorage(slot->storage())) {
        const std::size_t len = slot->payload.s.length;
        if (count > len) {
            setHeapError(error, ErrorCode::IndexOutOfRange, "bulk write exceeds array length");
            return false;
        }
        const auto enc = unpackPackedEncoding(slot->auxMeta);
        if (enc == PackedElemEncoding::Float64) {
            if (count > 0) {
                std::memcpy(static_cast<void*>(slot->payload.s.data), data, count * sizeof(double));
            }
            return true;
        }
        if (enc == PackedElemEncoding::Float32) {
            auto* dst = reinterpret_cast<float*>(slot->payload.s.data);
            for (std::size_t i = 0; i < count; ++i) dst[i] = static_cast<float>(data[i]);
            return true;
        }
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
    if (isPackedStorage(slot->storage())) {
        if (unpackPackedEncoding(slot->auxMeta) != PackedElemEncoding::Vec2) {
            setHeapError(error, ErrorCode::TypeMismatch, "array element is not a Vec2");
            return false;
        }
        const std::size_t len = slot->payload.s.length;
        out.resize(len);
        if (len > 0) {
            std::memcpy(out.data(), static_cast<const void*>(slot->payload.s.data), len * sizeof(Vec2Value));
        }
        return true;
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
    if (isPackedStorage(slot->storage())) {
        if (unpackPackedEncoding(slot->auxMeta) != PackedElemEncoding::Vec2) {
            setHeapError(error, ErrorCode::TypeMismatch, "array element is not a Vec2");
            return false;
        }
        if (count > slot->payload.s.length) {
            setHeapError(error, ErrorCode::IndexOutOfRange, "bulk write exceeds array length");
            return false;
        }
        if (count > 0) {
            std::memcpy(static_cast<void*>(slot->payload.s.data), data, count * sizeof(Vec2Value));
        }
        return true;
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

#define RS_DEFINE_PACKED_MATH_BULK_IO(MethodSuffix, ValueStruct, EncodingEnum, TypeIdFn, LabelStr, TryGetExpr) bool ManagedHeap::readPacked##MethodSuffix##Array(                                                                 ObjectRef reference,                                                                                           std::vector<ValueStruct>& out,                                                                                 RuntimeError* error) const {                                                                                   HeapSyncMutexGuard heapLock(impl_->syncState->mutex);                                                          const auto* slot = impl_->getSlot(reference);                                                                  if (!slot || slot->kind() != ObjectKind::Array) {                                                                  setHeapError(error, ErrorCode::InvalidObjectReference, "managed array reference is invalid");                  return false;                                                                                              }                                                                                                              if (isPackedStorage(slot->storage())) {                                                                            if (unpackPackedEncoding(slot->auxMeta) != PackedElemEncoding::EncodingEnum) {                                     setHeapError(error, ErrorCode::TypeMismatch, "array element is not a " LabelStr);                              return false;                                                                                              }                                                                                                              const std::size_t len = slot->payload.s.length;                                                                out.resize(len);                                                                                               if (len > 0) {                                                                                                     std::memcpy(out.data(), static_cast<const void*>(slot->payload.s.data), len * sizeof(ValueStruct));        }                                                                                                              return true;                                                                                               }                                                                                                              const auto len = Impl::slotValueCount(*slot);                                                                  const Value* values = slot->payload.s.data;                                                                    out.resize(len);                                                                                               for (std::size_t i = 0; i < len; ++i) {                                                                            ValueStruct elem{};                                                                                            const Value& cur = values[i];                                                                                  if (!(TryGetExpr)) {                                                                                               setHeapError(error, ErrorCode::TypeMismatch, "array element is not a " LabelStr);                              return false;                                                                                              }                                                                                                              out[i] = elem;                                                                                             }                                                                                                              return true;                                                                                               }                                                                                                              bool ManagedHeap::writePacked##MethodSuffix##Array(                                                                ObjectRef reference,                                                                                           const ValueStruct* data,                                                                                       std::size_t count,                                                                                             RuntimeError* error) {                                                                                         if (count > 0 && !data) {                                                                                          setHeapError(error, ErrorCode::InvalidArguments, "bulk write source buffer is null");                          return false;                                                                                              }                                                                                                              HeapSyncMutexGuard heapLock(impl_->syncState->mutex);                                                          auto* slot = impl_->getSlot(reference);                                                                        if (!slot || slot->kind() != ObjectKind::Array) {                                                                  setHeapError(error, ErrorCode::InvalidObjectReference, "managed array reference is invalid");                  return false;                                                                                              }                                                                                                              const auto& layout = impl_->typeLayouts[slot->payload.s.layoutId];                                             if (layout.elementType != semantic::PrimitiveType::Error &&                                                        !(layout.elementType == semantic::PrimitiveType::Struct && layout.elementTypeId == TypeIdFn())) {              setHeapError(error, ErrorCode::TypeMismatch, "array element is not a " LabelStr);                              return false;                                                                                              }                                                                                                              if (isPackedStorage(slot->storage())) {                                                                            if (unpackPackedEncoding(slot->auxMeta) != PackedElemEncoding::EncodingEnum) {                                     setHeapError(error, ErrorCode::TypeMismatch, "array element is not a " LabelStr);                              return false;                                                                                              }                                                                                                              if (count > slot->payload.s.length) {                                                                              setHeapError(error, ErrorCode::IndexOutOfRange, "bulk write exceeds array length");                            return false;                                                                                              }                                                                                                              if (count > 0) {                                                                                                   std::memcpy(static_cast<void*>(slot->payload.s.data), data, count * sizeof(ValueStruct));                  }                                                                                                              return true;                                                                                               }                                                                                                              const auto len = Impl::slotValueCount(*slot);                                                                  if (count > len) {                                                                                                 setHeapError(error, ErrorCode::IndexOutOfRange, "bulk write exceeds array length");                            return false;                                                                                              }                                                                                                              Value* values = slot->payload.s.data;                                                                          for (std::size_t i = 0; i < count; ++i) {                                                                          values[i] = Value{data[i]};                                                                                }                                                                                                              return true;                                                                                               }

RS_DEFINE_PACKED_MATH_BULK_IO(Vec3, Vec3Value, Vec3, vec3TypeId, "Vec3", cur.tryGetVec3(elem))
RS_DEFINE_PACKED_MATH_BULK_IO(Quat, QuatValue, Quat, quatTypeId, "Quat", cur.tryGetQuat(elem))
RS_DEFINE_PACKED_MATH_BULK_IO(Fixed64, Fixed64Value, Fixed32_32, fixed64TypeId, "Fixed64", cur.tryGetFixed64(elem.raw))
RS_DEFINE_PACKED_MATH_BULK_IO(Vec2f, Vec2fValue, Vec2f, vec2fTypeId, "Vec2f", cur.tryGetVec2f(elem))
RS_DEFINE_PACKED_MATH_BULK_IO(Vec3f, Vec3fValue, Vec3f, vec3fTypeId, "Vec3f", cur.tryGetVec3f(elem))
RS_DEFINE_PACKED_MATH_BULK_IO(Quatf, QuatfValue, Quatf, quatfTypeId, "Quatf", cur.tryGetQuatf(elem))

#undef RS_DEFINE_PACKED_MATH_BULK_IO

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
            if (isPackedStorage(slot.storage())) {
                const std::size_t pCount = slot.payload.s.length;
                const auto enc = unpackPackedEncoding(slot.auxMeta);
                info.valueCount = pCount;
                info.values.resize(pCount);
                for (std::size_t i = 0; i < pCount; ++i) {
                    info.values[i] = readPackedElementValue(
                        slot.payload.s.data, i, enc, layout.elementTypeId, slot.payload.s.cardHandle);
                }
            } else if (slot.storage() == SlotStorage::FlatStruct) {
                const std::size_t elemCount = slot.payload.s.length;
                const auto* sc = getFlatStructSchema(slot.auxMeta);
                info.valueCount = elemCount;
                info.values.resize(elemCount);
                if (sc) {
                    for (std::size_t i = 0; i < elemCount; ++i) {
                        info.values[i] = reconstructDetachedStructFromLeaves(
                            *sc,
                            slot.payload.s.data + i * static_cast<std::size_t>(sc->flatSlotCount));
                    }
                }
            } else {
                const auto count = Impl::slotValueCount(slot);
                info.valueCount = count;
                info.values.resize(count);
                for (std::size_t i = 0; i < count; ++i) {
                    info.values[i] = slot.payload.s.data[i];
                }
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
    impl_->packedBuffers.clear();
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
            PackedStorageSpec restorePacked{};
            bool canPackRestore = info.kind == ObjectKind::Array &&
                count <= static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) &&
                tryClassifyPackedArray(
                    info.elementType,
                    info.elementTypeId,
                    count > 0 ? &info.values[0] : nullptr,
                    restorePacked) &&
                restorePacked.strideBytes > 0u;
            if (canPackRestore && restorePacked.encoding == PackedElemEncoding::BlittableStruct) {
                const auto& reg = globalBlittableRegistry();
                const auto* schema = reg.getByIdLockFree(restorePacked.schemaId);
                if (!schema) {
                    canPackRestore = false;
                } else {
                    std::vector<std::uint8_t> probe(schema->strideBytes);
                    for (const auto& v : info.values) {
                        bool needsDemote = false;
                        if (!encodeBlittableStructBytes(reg, probe.data(), *schema, v, needsDemote)) {
                            canPackRestore = false;
                            break;
                        }
                    }
                }
            }
            if (canPackRestore) {
                if (restorePacked.encoding == PackedElemEncoding::Int32) {
                    for (const auto& v : info.values) {
                        std::int64_t iv = 0;
                        if (v.tryGetInt64(iv) &&
                            (iv < std::numeric_limits<std::int32_t>::min() ||
                             iv > std::numeric_limits<std::int32_t>::max())) {
                            restorePacked = {SlotStorage::PackedI64, PackedElemEncoding::Int64Wide, 3, 8u, 0xFFFFFFFFu};
                            break;
                        }
                    }
                } else if (restorePacked.encoding == PackedElemEncoding::EnumInline) {
                    for (const auto& v : info.values) {
                        EnumValue ev{};
                        if (v.tryGetEnum(ev) &&
                            (ev.value < std::numeric_limits<std::int32_t>::min() ||
                             ev.value > std::numeric_limits<std::int32_t>::max())) {
                            restorePacked = {SlotStorage::PackedI64, PackedElemEncoding::EnumWide, 3, 8u, 0xFFFFFFFFu};
                            break;
                        }
                    }
                }
                const std::size_t pBytes = count * static_cast<std::size_t>(restorePacked.strideBytes);
                Value* alignedPtr = nullptr;
                const std::uint32_t handle = impl_->packedBuffers.acquireZeroed(pBytes, alignedPtr);
                bool dummyWiden = false;
                for (std::size_t fi = 0; fi < count; ++fi) {
                    (void)writePackedElementValue(
                        alignedPtr,
                        fi,
                        restorePacked.encoding,
                        info.elementTypeId,
                        info.values[fi],
                        dummyWiden,
                        restorePacked.schemaId);
                }
                slot.setKindAndStorage(ObjectKind::Array, restorePacked.storage);
                slot.auxMeta = packPackedAuxMeta(restorePacked.encoding, restorePacked.elemShift);
                slot.payload.s = {
                    layoutId,
                    static_cast<std::uint32_t>(count),
                    alignedPtr,
                    handle,
                    restorePacked.schemaId,
                };
            } else if (info.kind == ObjectKind::Array &&
                       info.elementType == semantic::PrimitiveType::Struct &&
                       info.elementTypeId != 0 &&
                       count <= static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) &&
                       findFlatStructSchemaByTypeId(info.elementTypeId) != nullptr) {
                const auto* sc = findFlatStructSchemaByTypeId(info.elementTypeId);
                const std::size_t stride = sc->flatSlotCount;
                const std::size_t totalVals = count * stride;
                Value* ptr = nullptr;
                std::uint32_t handle = CompactSlot::kInvalidHandle;
                if (totalVals > 0) {
                    std::vector<Value> flatBuf(totalVals);
                    for (std::size_t ei = 0; ei < count; ++ei) {
                        if (!extractStructLeavesToBuffer(info.values[ei], *sc, flatBuf.data() + ei * stride)) {
                            std::memcpy(
                                static_cast<void*>(flatBuf.data() + ei * stride),
                                static_cast<const void*>(sc->prototypeLeaves.data()),
                                stride * sizeof(Value));
                        }
                    }
                    handle = impl_->largeBuffers.acquireMove(std::move(flatBuf), ptr);
                }
                std::uint32_t cardHandle = CompactSlot::kInvalidHandle;
                if (count >= ManagedHeap::kCardTableMinLength && sc->containsManagedReferences) {
                    cardHandle = impl_->cardArena.allocate(count);
                    slot.gcFlags |= GcFlag::HasCardTable;
                }
                slot.setKindAndStorage(ObjectKind::Array, SlotStorage::FlatStruct);
                slot.auxMeta = sc->schemaId;
                slot.payload.s = {
                    layoutId,
                    static_cast<std::uint32_t>(count),
                    ptr,
                    handle,
                    cardHandle,
                };
            } else if (count <= CompactSlot::kInlineValueCapacity) {
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
    : registryId_(nextRegistryId()) {
    std::lock_guard<std::mutex> dirLock(nativeHandleRegistryDirMutex());
    nativeHandleRegistries().push_back(this);
}

NativeHandleRegistry::~NativeHandleRegistry() {
    std::lock_guard<std::mutex> dirLock(nativeHandleRegistryDirMutex());
    auto& registries = nativeHandleRegistries();
    registries.erase(
        std::remove(registries.begin(), registries.end(), this),
        registries.end());
}

NativeHandle NativeHandleRegistry::create(
    semantic::SymbolId typeId,
    std::shared_ptr<void> resource,
    std::string debugNameValue) {
    if (typeId == 0 || !resource) return {};
    std::lock_guard<std::mutex> lock(mutex_);
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
    slot.cachedWrappers.clear();
    slot.cachedWrapperHeapIds.clear();
    if (rawPtr) {
        activeSlotByPointer_[PointerTypeKey{rawPtr, typeId}] = slotIndex;
    }
    return {slotIndex, slot.generation, typeId, registryId_};
}

std::optional<NativeHandle> NativeHandleRegistry::findHandle(
    semantic::SymbolId typeId,
    const void* rawPointer) const noexcept {
    if (typeId == 0 || !rawPointer) return std::nullopt;
    std::lock_guard<std::mutex> lock(mutex_);
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
    std::lock_guard<std::mutex> lock(mutex_);
    if (!isAliveUnlocked(handle)) return std::nullopt;
    const auto& slot = slots_[handle.slot];
    for (const auto& [hid, ref] : slot.cachedWrappers) {
        if (hid == heapId && ref.valid()) {
            return ref;
        }
    }
    return std::nullopt;
}

bool NativeHandleRegistry::setCachedWrapper(
    NativeHandle handle,
    ObjectRef wrapper,
    std::uint64_t heapId) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!isAliveUnlocked(handle)) return false;
    auto& slot = slots_[handle.slot];
    if (std::find(
            slot.cachedWrapperHeapIds.begin(),
            slot.cachedWrapperHeapIds.end(),
            heapId) == slot.cachedWrapperHeapIds.end()) {
        slot.cachedWrapperHeapIds.push_back(heapId);
    }
    for (auto& [hid, ref] : slot.cachedWrappers) {
        if (hid == heapId) {
            ref = wrapper;
            return true;
        }
    }
    slot.cachedWrappers.emplace_back(heapId, wrapper);
    return true;
}

bool NativeHandleRegistry::removeCachedWrapper(
    NativeHandle handle,
    std::uint64_t heapId) noexcept {
    return removeCachedWrapperImpl(handle, heapId, nullptr);
}

bool NativeHandleRegistry::removeCachedWrapper(
    NativeHandle handle,
    std::uint64_t heapId,
    ObjectRef expectedWrapper) noexcept {
    return removeCachedWrapperImpl(handle, heapId, &expectedWrapper);
}

bool NativeHandleRegistry::removeCachedWrapperImpl(
    NativeHandle handle,
    std::uint64_t heapId,
    const ObjectRef* expectedWrapper) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!isAliveUnlocked(handle)) return false;
    auto& wrappers = slots_[handle.slot].cachedWrappers;
    const auto before = wrappers.size();
    wrappers.erase(
        std::remove_if(
            wrappers.begin(),
            wrappers.end(),
            [&](const auto& entry) {
                if (entry.first != heapId) return false;
                return expectedWrapper == nullptr || entry.second == *expectedWrapper;
            }),
        wrappers.end());
    return wrappers.size() != before;
}

std::shared_ptr<void> NativeHandleRegistry::resolve(
    NativeHandle handle,
    semantic::SymbolId expectedTypeId,
    RuntimeError* error) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!handle.valid() || handle.registryId != compactNativeRegistryId(registryId_) ||
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

bool NativeHandleRegistry::isAliveUnlocked(NativeHandle handle) const noexcept {
    return handle.valid() && handle.registryId == compactNativeRegistryId(registryId_) &&
        handle.slot < slots_.size() &&
        slots_[handle.slot].resource &&
        slots_[handle.slot].generation == handle.generation &&
        slots_[handle.slot].typeId == handle.typeId;
}

bool NativeHandleRegistry::isAlive(NativeHandle handle) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return isAliveUnlocked(handle);
}

bool NativeHandleRegistry::shouldDispatchSweepNotification(
    NativeHandle handle,
    std::uint64_t heapId,
    ObjectRef sweptWrapper) noexcept {
    if (!handle.valid()) return false;
    std::lock_guard<std::mutex> dirLock(nativeHandleRegistryDirMutex());
    for (NativeHandleRegistry* reg : nativeHandleRegistries()) {
        if (!reg || compactNativeRegistryId(reg->registryId_) != handle.registryId) {
            continue;
        }
        std::lock_guard<std::mutex> regLock(reg->mutex_);
        if (!reg->isAliveUnlocked(handle)) {
            continue;
        }
        const auto& slot = reg->slots_[handle.slot];
        const bool heapUsedCache = std::find(
            slot.cachedWrapperHeapIds.begin(),
            slot.cachedWrapperHeapIds.end(),
            heapId) != slot.cachedWrapperHeapIds.end();
        if (!heapUsedCache) {
            return true;
        }
        for (const auto& [hid, cachedRef] : slot.cachedWrappers) {
            if (hid == heapId && cachedRef == sweptWrapper) {
                return true;
            }
        }
        return false;
    }
    return false;
}

bool NativeHandleRegistry::release(NativeHandle handle) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!isAliveUnlocked(handle)) return false;
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
    slot.cachedWrappers.clear();
    slot.cachedWrapperHeapIds.clear();
    ++slot.generation;
    if (slot.generation == 0 || slot.generation > 0xFFFFu) slot.generation = 1;
    freeSlots_.push_back(handle.slot);
    return true;
}

void NativeHandleRegistry::clear() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    activeSlotByPointer_.clear();
    freeSlots_.clear();
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        auto& slot = slots_[index];
        if (slot.resource) {
            slot.resource.reset();
            slot.rawPointer = nullptr;
            slot.typeId = 0;
            slot.debugName.clear();
            slot.cachedWrappers.clear();
            slot.cachedWrapperHeapIds.clear();
            ++slot.generation;
            if (slot.generation == 0 || slot.generation > 0xFFFFu) slot.generation = 1;
        }
        freeSlots_.push_back(static_cast<std::uint32_t>(index));
    }
}

std::size_t NativeHandleRegistry::liveCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<std::size_t>(std::count_if(
        slots_.begin(), slots_.end(),
        [](const Slot& slot) { return static_cast<bool>(slot.resource); }));
}

std::uint64_t NativeHandleRegistry::registryId() const noexcept {
    return registryId_;
}

std::string NativeHandleRegistry::debugName(NativeHandle handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return isAliveUnlocked(handle) ? slots_[handle.slot].debugName : std::string{};
}

void NativeHandleRegistry::registerCppType(
    std::type_index cppType,
    semantic::SymbolId typeId) {
    if (typeId != 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        cppTypeIds_[cppType] = typeId;
    }
}

semantic::SymbolId NativeHandleRegistry::findCppTypeId(
    std::type_index cppType) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = cppTypeIds_.find(cppType);
    return it != cppTypeIds_.end() ? it->second : 0;
}


void ManagedHeap::enterParallelLockFreeRegion() noexcept {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    if (!impl_->mayContainPoolValues.load(std::memory_order_relaxed)) {
        impl_->noteMaybePoolBackedUnlocked();
    }
    impl_->syncState->parallelLockFreeDepth.fetch_add(1u, std::memory_order_release);
}

void ManagedHeap::leaveParallelLockFreeRegion() noexcept {
    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);
    const auto prev = impl_->syncState->parallelLockFreeDepth.load(std::memory_order_relaxed);
    if (prev > 0u) {
        impl_->syncState->parallelLockFreeDepth.store(prev - 1u, std::memory_order_release);
    }
}

bool ManagedHeap::inParallelLockFreeRegion() const noexcept {
    return impl_->syncState->parallelLockFreeDepth.load(std::memory_order_acquire) > 0u;
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
    enterHeapSyncLockLocal();
}

void unlockValuePoolRootSync(const std::shared_ptr<void>& syncHandle) noexcept {
    if (!syncHandle) return;
    static_cast<HeapSyncState*>(syncHandle.get())->mutex.unlock();
    leaveHeapSyncLockLocal(/*flushDeferred=*/true);
}

void enterInterpreterMutator() noexcept {
    if (const auto* b = externalValuePoolBridge(); b && b->enterMutatorFn) {
        b->enterMutatorFn();
        return;
    }
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
    if (const auto* b = externalValuePoolBridge(); b && b->leaveMutatorFn) {
        b->leaveMutatorFn();
        return;
    }
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

std::size_t hostReclaimPoolValuesBridge(
    const void* activeShadowStack,
    bool waitIfBusy) noexcept {
    return reclaimPoolValuesInternal(
        static_cast<const ShadowStack*>(activeShadowStack),
        waitIfBusy);
}

void hostRegisterShadowStackBridge(void* stack) noexcept {
    registerActiveShadowStack(static_cast<ShadowStack*>(stack));
}

void hostUnregisterShadowStackBridge(void* stack) noexcept {
    unregisterActiveShadowStack(static_cast<ShadowStack*>(stack));
}

void hostTransferShadowStackBridge(void* from, void* to) noexcept {
    transferActiveShadowStack(static_cast<ShadowStack*>(from), static_cast<ShadowStack*>(to));
}

void hostEnterHeapSyncLockBridge() noexcept {
    enterHeapSyncLockLocal();
}

void hostLeaveHeapSyncLockBridge(bool flushDeferred) noexcept {
    leaveHeapSyncLockLocal(flushDeferred);
}

} // namespace detail

namespace {

std::size_t reclaimPoolValuesInternal(
    const ShadowStack* activeShadowStack,
    bool waitIfBusy) noexcept {
    if (const auto* b = detail::externalValuePoolBridge(); b && b->reclaimPoolValuesFn) {
        return b->reclaimPoolValuesFn(activeShadowStack, waitIfBusy);
    }
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

bool ManagedHeap::migrateTypeLayouts(
    const std::vector<TypeLayoutMigration>& migrations,
    LayoutMigrationStats* outStats,
    RuntimeError* error) {
    LayoutMigrationStats localStats{};
    if (migrations.empty()) {
        if (outStats) *outStats = localStats;
        return true;
    }

    HeapSyncMutexGuard heapLock(impl_->syncState->mutex);

    std::unordered_map<semantic::SymbolId, const TypeLayoutMigration*> classMigrations;
    std::unordered_map<semantic::SymbolId, const TypeLayoutMigration*> structMigrations;
    std::unordered_set<semantic::SymbolId> removedTypeIds;
    std::unordered_set<semantic::SymbolId> delegateTypeIds;
    std::unordered_map<semantic::SymbolId, DelegateMethodSlotUpdate> delegateUpdates;
    std::unordered_set<semantic::SymbolId> arrayLayoutIdsSet;
    for (const auto& [arrKey, arrLayoutId] : impl_->arrayLayoutByKey) {
        (void)arrKey;
        arrayLayoutIdsSet.insert(arrLayoutId);
    }

    for (const auto& mig : migrations) {
        for (const auto& upd : mig.delegateSlotUpdates) {
            if (upd.methodSymbolId != 0) {
                delegateUpdates[upd.methodSymbolId] = upd;
            }
        }
        if (mig.typeId == 0) continue;
        if (mig.removedType) {
            removedTypeIds.insert(mig.typeId);
            continue;
        }
        if (mig.delegateType) {
            delegateTypeIds.insert(mig.typeId);
            continue;
        }
        if (mig.addedType || mig.interfaceType) {
            continue;
        }
        if (mig.typeKind == semantic::TypeKind::Class && mig.instanceLayoutChanged) {
            classMigrations.emplace(mig.typeId, &mig);
        } else if (mig.typeKind == semantic::TypeKind::Struct && mig.instanceLayoutChanged) {
            structMigrations.emplace(mig.typeId, &mig);
        }
    }

    const auto containsYoungReference = [&](const auto& self,
                                            const Value& value,
                                            std::unordered_set<const StructStorage*>* visited) noexcept -> bool {
        if (!value.isTraceable()) return false;
        const auto tag = value.rawTag();
        if (tag == ValueTag::ObjectRef) {
            const auto& ref = value.asObjectRefUnchecked();
            const auto* childSlot = impl_->getSlot(ref);
            return childSlot != nullptr && childSlot->hasFlag(GcFlag::InNursery);
        }
        if (tag == ValueTag::Struct) {
            const auto& st = value.asStructUnchecked();
            if (!st.isInline && st.storage) {
                std::unordered_set<const StructStorage*> localVisited;
                auto* activeSet = visited ? visited : &localVisited;
                if (activeSet->insert(st.storage.get()).second) {
                    for (const auto& f : st.storage->fields) {
                        if (self(self, f, activeSet)) return true;
                    }
                }
            }
        }
        return false;
    };

    const auto isObjectValueAssignable = [&](const Value& val,
                                             semantic::SymbolId expectedTid,
                                             const std::vector<semantic::SymbolId>& assignableIds) noexcept -> bool {
        if (std::holds_alternative<NullObject>(val)) return true;
        const auto* ref = std::get_if<ObjectRef>(&val);
        if (!ref || ref->kind != ObjectKind::Record || !impl_->getSlot(*ref)) {
            return false;
        }
        const auto actualTid = impl_->objectTypeIdUnlocked(*ref);
        if (!actualTid || removedTypeIds.count(*actualTid) != 0) {
            return false;
        }
        if (expectedTid == 0) return true;
        if (*actualTid == expectedTid) return true;
        return std::binary_search(assignableIds.begin(), assignableIds.end(), *actualTid);
    };

    const auto isArrayValueCompatible = [&](const Value& val,
                                            semantic::SymbolId expectedArrTid) noexcept -> bool {
        if (std::holds_alternative<NullArray>(val)) return true;
        const auto* ref = std::get_if<ObjectRef>(&val);
        if (!ref || ref->kind != ObjectKind::Array) return false;
        const auto* arrSlot = impl_->getSlot(*ref);
        if (!arrSlot) return false;
        const auto& arrLayout = impl_->typeLayouts[arrSlot->payload.s.layoutId];
        if (arrLayout.elementTypeId != 0 && removedTypeIds.count(arrLayout.elementTypeId) != 0) {
            return false;
        }
        if (expectedArrTid == 0) return true;
        return arrLayout.typeId == expectedArrTid;
    };

    const auto migrateEmbeddedValue = [&](const auto& self,
                                          const Value& val,
                                          std::size_t depth,
                                          bool& outChanged) -> Value {
        if (depth > 64) return val;
        if (const auto* ref = std::get_if<ObjectRef>(&val)) {
            const auto* targetSlot = impl_->getSlot(*ref);
            if (!targetSlot) {
                outChanged = true;
                if (ref->kind == ObjectKind::Array) return Value{NullArray{}};
                if (ref->kind == ObjectKind::String) return Value{NullString{}};
                return Value{NullObject{}};
            }
            if (ref->kind == ObjectKind::Record) {
                const auto tid = impl_->typeLayouts[targetSlot->payload.s.layoutId].typeId;
                if (tid != 0 && removedTypeIds.count(tid) != 0) {
                    outChanged = true;
                    return Value{NullObject{}};
                }
            }
            return val;
        }
        if (const auto* st = std::get_if<StructValue>(&val)) {
            const auto sIt = structMigrations.find(st->typeId);
            if (sIt != structMigrations.end()) {
                const auto& smig = *sIt->second;
                const std::size_t oldCount = st->fieldCount();
                std::vector<Value> oldStructFields(oldCount);
                for (std::size_t i = 0; i < oldCount; ++i) {
                    if (const auto fv = structValueFieldAt(*st, i)) {
                        oldStructFields[i] = *fv;
                    }
                }
                std::vector<Value> newStructFields;
                newStructFields.reserve(smig.fieldOps.size());
                for (const auto& fop : smig.fieldOps) {
                    Value resolved = fop.defaultValue;
                    if (fop.kind == FieldMigrationKind::Copy && fop.sourceIndex < oldCount) {
                        bool dummyChanged = false;
                        Value candidate = self(self, oldStructFields[fop.sourceIndex], depth + 1, dummyChanged);
                        bool ok = true;
                        if (fop.targetType == semantic::PrimitiveType::Object) {
                            ok = isObjectValueAssignable(candidate, fop.targetTypeId, fop.assignableTypeIds);
                        } else if (fop.targetType == semantic::PrimitiveType::Array) {
                            ok = isArrayValueCompatible(candidate, fop.targetTypeId);
                        } else if (fop.targetType != semantic::PrimitiveType::Error &&
                                   valueType(candidate) != fop.targetType) {
                            ok = false;
                        }
                        if (ok) {
                            resolved = std::move(candidate);
                            ++localStats.fieldsCopied;
                            if (fop.renamed) ++localStats.fieldsRenamed;
                        } else {
                            ++localStats.fieldsDefaulted;
                        }
                    } else if (fop.kind == FieldMigrationKind::ConvertNumeric &&
                               fop.sourceIndex < oldCount) {
                        Value converted{};
                        if (tryConvertNumeric(
                                oldStructFields[fop.sourceIndex],
                                fop.targetType,
                                converted,
                                fop.checkedArithmetic)) {
                            resolved = std::move(converted);
                            ++localStats.fieldsConverted;
                            if (fop.renamed) ++localStats.fieldsRenamed;
                        } else {
                            ++localStats.fieldsDefaulted;
                        }
                    } else {
                        ++localStats.fieldsDefaulted;
                    }
                    newStructFields.push_back(std::move(resolved));
                }
                outChanged = true;
                return Value{makeStructValueFromFields(st->typeId, std::move(newStructFields))};
            }
            if (!st->isInline && st->storage && (!structMigrations.empty() || !removedTypeIds.empty())) {
                bool anyInnerChanged = false;
                std::vector<Value> updatedFields;
                updatedFields.reserve(st->storage->fields.size());
                for (const auto& inner : st->storage->fields) {
                    bool elemChanged = false;
                    updatedFields.push_back(self(self, inner, depth + 1, elemChanged));
                    anyInnerChanged = anyInnerChanged || elemChanged;
                }
                if (anyInnerChanged) {
                    outChanged = true;
                    return Value{makeStructValueFromFields(st->typeId, std::move(updatedFields))};
                }
            } else if (st->isInline && st->nestedTypeId() != 0 &&
                       (!structMigrations.empty() || !removedTypeIds.empty())) {
                const std::size_t fCount = st->fieldCount();
                bool anyInnerChanged = false;
                std::vector<Value> updatedFields;
                updatedFields.reserve(fCount);
                for (std::size_t i = 0; i < fCount; ++i) {
                    Value inner{};
                    if (const auto fv = structValueFieldAt(*st, i)) {
                        inner = *fv;
                    }
                    bool elemChanged = false;
                    updatedFields.push_back(self(self, inner, depth + 1, elemChanged));
                    anyInnerChanged = anyInnerChanged || elemChanged;
                }
                if (anyInnerChanged) {
                    outChanged = true;
                    return Value{makeStructValueFromFields(st->typeId, std::move(updatedFields))};
                }
            }
        }
        return val;
    };

    const auto evaluateFieldOp = [&](const FieldMigrationOp& op,
                                     const Value* oldData,
                                     std::size_t oldCount) -> Value {
        if (op.kind == FieldMigrationKind::Copy) {
            if (op.sourceIndex >= oldCount) {
                ++localStats.fieldsDefaulted;
                return op.defaultValue;
            }
            bool dummyChanged = false;
            Value candidate = migrateEmbeddedValue(
                migrateEmbeddedValue, oldData[op.sourceIndex], 0, dummyChanged);
            bool valid = true;
            if (op.targetType == semantic::PrimitiveType::Object) {
                valid = isObjectValueAssignable(candidate, op.targetTypeId, op.assignableTypeIds);
            } else if (op.targetType == semantic::PrimitiveType::Array) {
                valid = isArrayValueCompatible(candidate, op.targetTypeId);
            } else if (op.targetType != semantic::PrimitiveType::Error &&
                       valueType(candidate) != op.targetType) {
                valid = false;
            }
            if (!valid) {
                ++localStats.fieldsDefaulted;
                return op.defaultValue;
            }
            ++localStats.fieldsCopied;
            if (op.renamed) ++localStats.fieldsRenamed;
            return candidate;
        }
        if (op.kind == FieldMigrationKind::ConvertNumeric) {
            if (op.sourceIndex >= oldCount) {
                ++localStats.fieldsDefaulted;
                return op.defaultValue;
            }
            Value converted{};
            if (tryConvertNumeric(
                    oldData[op.sourceIndex],
                    op.targetType,
                    converted,
                    op.checkedArithmetic)) {
                ++localStats.fieldsConverted;
                if (op.renamed) ++localStats.fieldsRenamed;
                return converted;
            }
            ++localStats.fieldsDefaulted;
            return op.defaultValue;
        }
        ++localStats.fieldsDefaulted;
        return op.defaultValue;
    };

    struct StagedRecordMigration {
        std::uint32_t slotIdx = 0;
        semantic::SymbolId typeId = 0;
        std::size_t oldBytes = 0;
        std::size_t newBytes = 0;
        std::vector<Value> newFields;
    };

    std::vector<StagedRecordMigration> stagedRecords;
    std::size_t totalBytesAdded = 0;
    std::size_t totalBytesRemoved = 0;

    for (std::uint32_t slotIdx = 0; slotIdx < static_cast<std::uint32_t>(impl_->slots.size()); ++slotIdx) {
        const auto& slot = impl_->slots[slotIdx];
        if (!slot.occupied() || slot.kind() != ObjectKind::Record) continue;
        ++localStats.objectsVisited;

        const auto tid = impl_->typeLayouts[slot.payload.s.layoutId].typeId;
        if (tid == 0) continue;

        if (removedTypeIds.count(tid) != 0) {
            const std::size_t oldBytes = impl_->slotSizeBytes(slotIdx, slot);
            const std::size_t newBytes = Impl::BaseObjectSizeBytes;
            if (oldBytes > newBytes) totalBytesRemoved += (oldBytes - newBytes);
            stagedRecords.push_back(StagedRecordMigration{
                slotIdx, tid, oldBytes, newBytes, {}});
            continue;
        }

        const auto mIt = classMigrations.find(tid);
        if (mIt == classMigrations.end()) continue;
        const auto& mig = *mIt->second;

        const std::size_t newFieldStorage = estimateValueStorage(mig.newFieldCount);
        if (newFieldStorage == std::numeric_limits<std::size_t>::max() ||
            newFieldStorage > std::numeric_limits<std::size_t>::max() - Impl::BaseObjectSizeBytes) {
            setHeapError(error, ErrorCode::OutOfMemory, "migrated object layout is too large");
            return false;
        }

        const Value* oldData = slot.payload.s.data;
        const std::size_t oldCount = Impl::slotValueCount(slot);
        std::vector<Value> remapped;
        remapped.reserve(mig.newFieldCount);
        for (std::size_t fi = 0; fi < mig.newFieldCount; ++fi) {
            if (fi < mig.fieldOps.size()) {
                remapped.push_back(evaluateFieldOp(mig.fieldOps[fi], oldData, oldCount));
            } else {
                ++localStats.fieldsDefaulted;
                remapped.emplace_back();
            }
        }

        const std::size_t oldBytes = impl_->slotSizeBytes(slotIdx, slot);
        const std::size_t newBytes = Impl::BaseObjectSizeBytes + newFieldStorage;
        if (newBytes >= oldBytes) {
            const std::size_t diff = newBytes - oldBytes;
            if (totalBytesAdded > std::numeric_limits<std::size_t>::max() - diff) {
                setHeapError(error, ErrorCode::OutOfMemory, "migrated heap size overflowed");
                return false;
            }
            totalBytesAdded += diff;
        } else {
            totalBytesRemoved += (oldBytes - newBytes);
        }

        stagedRecords.push_back(StagedRecordMigration{
            slotIdx, tid, oldBytes, newBytes, std::move(remapped)});
    }

    if (totalBytesAdded > totalBytesRemoved) {
        const std::size_t netIncrease = totalBytesAdded - totalBytesRemoved;
        if (netIncrease > impl_->config.maximumHeapBytes ||
            impl_->statistics.liveBytes > impl_->config.maximumHeapBytes - netIncrease) {
            setHeapError(
                error,
                ErrorCode::OutOfMemory,
                "managed heap type layout migration exceeds the configured heap limit");
            impl_->requested = true;
            return false;
        }
    }

    // Update sharedTypeLayouts (typeLayouts & sharedRecordLayoutByTypeId) for all migrated/added/removed types
    std::unordered_map<semantic::SymbolId, std::uint32_t> resolvedLayoutIdByType;
    const auto ensureCanonicalLayout = [&](semantic::SymbolId tid,
                                           std::vector<std::size_t> refs,
                                           std::size_t fieldLimit,
                                           bool forceCreate) -> std::uint32_t {
        refs.erase(
            std::remove_if(
                refs.begin(),
                refs.end(),
                [fieldLimit](std::size_t idx) { return idx >= fieldLimit; }),
            refs.end());
        std::sort(refs.begin(), refs.end());
        refs.erase(std::unique(refs.begin(), refs.end()), refs.end());

        std::uint32_t canonicalId = 0;
        const auto sIt = impl_->sharedRecordLayoutByTypeId.find(tid);
        if (sIt != impl_->sharedRecordLayoutByTypeId.end()) {
            canonicalId = sIt->second;
            impl_->typeLayouts[canonicalId].referenceFields = refs;
            impl_->typeLayouts[canonicalId].mayContainReferences = !refs.empty();
        } else if (forceCreate) {
            TypeLayoutEntry entry;
            entry.typeId = tid;
            entry.mayContainReferences = !refs.empty();
            entry.referenceFields = refs;
            canonicalId = static_cast<std::uint32_t>(impl_->typeLayouts.size());
            impl_->typeLayouts.push_back(std::move(entry));
            impl_->sharedRecordLayoutByTypeId[tid] = canonicalId;
        }

        for (std::uint32_t lid = 1; lid < static_cast<std::uint32_t>(impl_->typeLayouts.size()); ++lid) {
            if (lid == canonicalId || arrayLayoutIdsSet.count(lid) != 0) continue;
            auto& entry = impl_->typeLayouts[lid];
            if (entry.typeId == tid) {
                entry.referenceFields = refs;
                entry.mayContainReferences = !refs.empty();
                if (canonicalId == 0) {
                    canonicalId = lid;
                    impl_->sharedRecordLayoutByTypeId[tid] = lid;
                }
            }
        }
        return canonicalId;
    };

    std::unordered_set<semantic::SymbolId> stagedTypeIds;
    for (const auto& item : stagedRecords) stagedTypeIds.insert(item.typeId);

    for (const auto& mig : migrations) {
        if (mig.typeId == 0 || mig.delegateType || mig.interfaceType ||
            mig.typeKind != semantic::TypeKind::Class) {
            continue;
        }
        if (mig.removedType) {
            const auto lid = ensureCanonicalLayout(
                mig.typeId, {}, 0u, stagedTypeIds.count(mig.typeId) != 0);
            if (lid != 0) resolvedLayoutIdByType[mig.typeId] = lid;
            continue;
        }
        const bool needCreate = (stagedTypeIds.count(mig.typeId) != 0);
        const auto lid = ensureCanonicalLayout(
            mig.typeId, mig.newReferenceFields, mig.newFieldCount, needCreate);
        if (lid != 0) resolvedLayoutIdByType[mig.typeId] = lid;
    }
    impl_->lastRegisteredLayoutTypeId = 0;
    impl_->lastRegisteredLayoutId = 0;

    // Commit in-place CompactSlot storage remapping while preserving ObjectRef (slot, generation, heapId) 100%
    std::unordered_set<std::uint32_t> migratedSlotIndices;
    migratedSlotIndices.reserve(stagedRecords.size());

    for (auto& staged : stagedRecords) {
        migratedSlotIndices.insert(staged.slotIdx);
        auto& slot = impl_->slots[staged.slotIdx];
        const std::uint32_t targetLayoutId = resolvedLayoutIdByType[staged.typeId];
        const std::size_t newCount = staged.newFields.size();

        for (const auto& fv : staged.newFields) {
            impl_->pinHeapValueIfNeededUnlocked(fv);
            if (impl_->phase != GcPhase::Idle && slot.hasFlag(GcFlag::Marked)) {
                impl_->markValue(fv);
            }
        }
        if (impl_->phase != GcPhase::Idle && !impl_->markStack.empty()) {
            impl_->reopenMarkFromSweep();
        }

        if (slot.hasFlag(GcFlag::CustomSize)) {
            impl_->customSlotSizes.erase(staged.slotIdx);
            slot.gcFlags &= static_cast<std::uint8_t>(~GcFlag::CustomSize);
        }

        const SlotStorage oldStorage = slot.storage();
        const std::uint16_t oldAuxMeta = slot.auxMeta;
        const std::uint32_t oldHandle = slot.payload.s.handle;
        const std::uint32_t oldLength = slot.payload.s.length;

        if (newCount <= CompactSlot::kInlineValueCapacity) {
            const std::uint16_t desiredCls = (newCount == 0)
                ? PagedValueSlabPool::kZeroClass
                : PagedValueSlabPool::sizeToClass(newCount);
            if (oldStorage == SlotStorage::SlabValues &&
                newCount > 0 &&
                oldLength > 0 &&
                oldAuxMeta == desiredCls) {
                Value* ptr = slot.payload.s.data;
                for (std::size_t fi = 0; fi < newCount; ++fi) {
                    ptr[fi] = std::move(staged.newFields[fi]);
                }
                const std::size_t stride = PagedValueSlabPool::classStride(desiredCls);
                for (std::size_t fi = newCount; fi < stride; ++fi) {
                    ptr[fi] = Value{};
                }
                slot.payload.s.layoutId = targetLayoutId;
                slot.payload.s.length = static_cast<std::uint32_t>(newCount);
            } else {
                std::uint16_t newCls = 0;
                std::uint32_t newHandle = CompactSlot::kInvalidHandle;
                Value* newPtr = impl_->slabPool.allocate(newCount, newCls, newHandle);
                for (std::size_t fi = 0; fi < newCount; ++fi) {
                    newPtr[fi] = std::move(staged.newFields[fi]);
                }
                if (oldStorage == SlotStorage::SlabValues) {
                    impl_->slabPool.deallocate(oldAuxMeta, oldHandle, oldLength);
                } else if (oldStorage == SlotStorage::HeapValues) {
                    impl_->largeBuffers.release(oldHandle);
                    ++localStats.heapToInlineTransitions;
                }
                if (slot.hasFlag(GcFlag::HasCardTable) &&
                    slot.payload.s.cardHandle != CompactSlot::kInvalidHandle) {
                    impl_->cardArena.deallocate(slot.payload.s.cardHandle);
                    slot.gcFlags &= static_cast<std::uint8_t>(~GcFlag::HasCardTable);
                }
                slot.setKindAndStorage(ObjectKind::Record, SlotStorage::SlabValues);
                slot.auxMeta = newCls;
                slot.payload.s = {
                    targetLayoutId,
                    static_cast<std::uint32_t>(newCount),
                    newPtr,
                    newHandle,
                    CompactSlot::kInvalidHandle,
                };
            }
        } else {
            Value* newPtr = nullptr;
            const std::uint32_t newHandle = impl_->largeBuffers.acquireMove(
                std::move(staged.newFields), newPtr);
            if (oldStorage == SlotStorage::SlabValues) {
                impl_->slabPool.deallocate(oldAuxMeta, oldHandle, oldLength);
                ++localStats.inlineToSlabOrHeapTransitions;
            } else if (oldStorage == SlotStorage::HeapValues) {
                impl_->largeBuffers.release(oldHandle);
            }
            if (slot.hasFlag(GcFlag::HasCardTable) &&
                slot.payload.s.cardHandle != CompactSlot::kInvalidHandle) {
                impl_->cardArena.deallocate(slot.payload.s.cardHandle);
                slot.gcFlags &= static_cast<std::uint8_t>(~GcFlag::HasCardTable);
            }
            slot.setKindAndStorage(ObjectKind::Record, SlotStorage::HeapValues);
            slot.auxMeta = 0;
            slot.payload.s = {
                targetLayoutId,
                static_cast<std::uint32_t>(newCount),
                newPtr,
                newHandle,
                CompactSlot::kInvalidHandle,
            };
        }

        // Old-Gen -> Young-Gen generational write barrier & CardTable dirtying
        if (!slot.hasFlag(GcFlag::InNursery)) {
            const auto& layoutRefs = impl_->typeLayouts[targetLayoutId].referenceFields;
            bool hasYoungChild = false;
            for (const auto refIdx : layoutRefs) {
                if (refIdx < newCount &&
                    containsYoungReference(containsYoungReference, slot.payload.s.data[refIdx], nullptr)) {
                    hasYoungChild = true;
                    if (slot.hasFlag(GcFlag::HasCardTable) &&
                        slot.payload.s.cardHandle != CompactSlot::kInvalidHandle) {
                        auto* cardBytes = impl_->cardArena.get(slot.payload.s.cardHandle).bytePtr();
                        cardBytes[refIdx >> ManagedHeap::kCardElementShift] = 1u;
                    }
                }
            }
            if (hasYoungChild && !slot.hasFlag(GcFlag::InRememberedSet)) {
                slot.gcFlags |= GcFlag::InRememberedSet;
                impl_->rememberedSet.push_back(
                    ObjectRef{staged.slotIdx, slot.generation, ObjectKind::Record, impl_->heapId});
                ++localStats.oldGenBarriersMarked;
            }
        }

        if (staged.newBytes >= staged.oldBytes) {
            const std::size_t delta = staged.newBytes - staged.oldBytes;
            impl_->statistics.liveBytes += delta;
            impl_->statistics.bytesAllocated += delta;
            impl_->statistics.peakLiveBytes = std::max(
                impl_->statistics.peakLiveBytes, impl_->statistics.liveBytes);
        } else {
            const std::size_t delta = staged.oldBytes - staged.newBytes;
            impl_->statistics.liveBytes = (impl_->statistics.liveBytes >= delta)
                ? (impl_->statistics.liveBytes - delta)
                : 0;
            impl_->statistics.bytesReclaimed += delta;
        }
        if (impl_->statistics.liveBytes >= impl_->nextCollectionThreshold) {
            impl_->requested = true;
        }
        ++localStats.objectsMigrated;
    }

    if (!structMigrations.empty() || !removedTypeIds.empty()) {
        std::vector<std::pair<semantic::SymbolId, std::vector<BlittableFieldInput>>> updatedStructs;
        for (const auto& mig : migrations) {
            if (mig.typeKind != semantic::TypeKind::Struct || mig.typeId == 0) continue;
            if (mig.removedType) {
                std::lock_guard<std::mutex> regLock(globalBlittableRegistry().mutex);
                globalBlittableRegistry().markNonBlittableUnlocked(mig.typeId);
                continue;
            }
            std::vector<BlittableFieldInput> fInputs;
            fInputs.reserve(mig.newFieldCount);
            for (std::size_t fi = 0; fi < mig.newFieldCount; ++fi) {
                if (fi < mig.fieldOps.size()) {
                    fInputs.push_back(BlittableFieldInput{
                        mig.fieldOps[fi].targetType,
                        mig.fieldOps[fi].targetTypeId});
                } else {
                    fInputs.push_back(BlittableFieldInput{semantic::PrimitiveType::Error, 0});
                }
            }
            updatedStructs.emplace_back(mig.typeId, std::move(fInputs));
        }
        if (!updatedStructs.empty()) {
            registerBlittableStructTypes(updatedStructs);
        }
    }

    const auto schemaTouchesMigratedStruct = [&](auto& self, std::uint32_t sid, std::uint32_t depth) -> bool {
        if (sid == 0u || sid == CompactSlot::kInvalidHandle || depth > 16u) return false;
        const auto* sc = globalBlittableRegistry().getByIdLockFree(sid);
        if (!sc) return false;
        if (structMigrations.count(sc->structTypeId) != 0 ||
            removedTypeIds.count(sc->structTypeId) != 0) {
            return true;
        }
        for (const auto& f : sc->fields) {
            if (f.kind == BlittableLeafKind::NestedStruct &&
                self(self, f.nestedSchemaId, depth + 1u)) {
                return true;
            }
        }
        return false;
    };

    // Rebind live Delegate dispatch slots & migrate embedded StructValues / removed ObjectRefs in remaining slots
    if (!delegateUpdates.empty() || !structMigrations.empty() || !removedTypeIds.empty()) {
        for (std::uint32_t slotIdx = 0; slotIdx < static_cast<std::uint32_t>(impl_->slots.size()); ++slotIdx) {
            if (migratedSlotIndices.count(slotIdx) != 0) continue;
            auto& slot = impl_->slots[slotIdx];
            if (!slot.occupied()) continue;
            if (slot.kind() == ObjectKind::Array &&
                slot.storage() == SlotStorage::PackedStruct &&
                unpackPackedEncoding(slot.auxMeta) == PackedElemEncoding::BlittableStruct &&
                schemaTouchesMigratedStruct(schemaTouchesMigratedStruct, slot.payload.s.cardHandle, 0u)) {
                const auto& arrLayout = impl_->typeLayouts[slot.payload.s.layoutId];
                const std::size_t pCount = slot.payload.s.length;
                const std::uint32_t oldSid = slot.payload.s.cardHandle;
                std::vector<Value> elems(pCount);
                bool anyChanged = false;
                for (std::size_t idx = 0; idx < pCount; ++idx) {
                    Value orig = readPackedElementValue(
                        slot.payload.s.data,
                        idx,
                        PackedElemEncoding::BlittableStruct,
                        arrLayout.elementTypeId,
                        oldSid);
                    bool ch = false;
                    elems[idx] = migrateEmbeddedValue(migrateEmbeddedValue, orig, 0, ch);
                    anyChanged = anyChanged || ch;
                }
                PackedStorageSpec newSpec{};
                bool canRepack = tryClassifyPackedArray(
                    semantic::PrimitiveType::Struct,
                    arrLayout.elementTypeId,
                    pCount > 0 ? &elems[0] : nullptr,
                    newSpec) &&
                    newSpec.encoding == PackedElemEncoding::BlittableStruct &&
                    newSpec.strideBytes > 0u;
                if (canRepack) {
                    const auto* newSc = globalBlittableRegistry().getByIdLockFree(newSpec.schemaId);
                    if (!newSc) {
                        canRepack = false;
                    } else {
                        std::vector<std::uint8_t> probe(newSc->strideBytes);
                        for (const auto& ev : elems) {
                            bool dummy = false;
                            if (!encodeBlittableStructBytes(globalBlittableRegistry(), probe.data(), *newSc, ev, dummy)) {
                                canRepack = false;
                                break;
                            }
                        }
                    }
                }
                const std::size_t oldSlotBytes = impl_->slotSizeBytes(slotIdx, slot);
                if (canRepack) {
                    const std::size_t newBufBytes = pCount * static_cast<std::size_t>(newSpec.strideBytes);
                    Value* newAlignedPtr = nullptr;
                    const std::uint32_t newHandle = impl_->packedBuffers.acquireZeroed(newBufBytes, newAlignedPtr);
                    bool dummy = false;
                    for (std::size_t idx = 0; idx < pCount; ++idx) {
                        (void)writePackedElementValue(
                            newAlignedPtr,
                            idx,
                            PackedElemEncoding::BlittableStruct,
                            arrLayout.elementTypeId,
                            elems[idx],
                            dummy,
                            newSpec.schemaId);
                    }
                    impl_->packedBuffers.release(slot.payload.s.handle);
                    slot.payload.s.data = newAlignedPtr;
                    slot.payload.s.handle = newHandle;
                    slot.payload.s.cardHandle = newSpec.schemaId;
                    const std::size_t newSlotBytes = impl_->slotSizeBytes(slotIdx, slot);
                    if (!slot.hasFlag(GcFlag::CustomSize)) {
                        if (newSlotBytes >= oldSlotBytes) {
                            const std::size_t d = newSlotBytes - oldSlotBytes;
                            impl_->statistics.liveBytes += d;
                            impl_->statistics.bytesAllocated += d;
                            impl_->statistics.peakLiveBytes = std::max(
                                impl_->statistics.peakLiveBytes, impl_->statistics.liveBytes);
                        } else {
                            const std::size_t d = oldSlotBytes - newSlotBytes;
                            impl_->statistics.liveBytes = (impl_->statistics.liveBytes >= d)
                                ? (impl_->statistics.liveBytes - d)
                                : 0;
                            impl_->statistics.bytesReclaimed += d;
                        }
                    }
                } else if (impl_->demotePackedStructSlotToValuesUnlocked(slotIdx, slot)) {
                    Value* data = slot.payload.s.data;
                    const ObjectRef ownerRef{slotIdx, slot.generation, ObjectKind::Array, impl_->heapId};
                    for (std::size_t idx = 0; idx < pCount; ++idx) {
                        impl_->pinHeapValueIfNeededUnlocked(elems[idx]);
                        impl_->writeBarrierArrayElement(ownerRef, idx, elems[idx]);
                        data[idx] = std::move(elems[idx]);
                    }
                }
                continue;
            }
            if (slot.kind() == ObjectKind::Array && slot.storage() == SlotStorage::FlatStruct) {
                const auto* oldSc = getFlatStructSchema(slot.auxMeta);
                const auto& arrLayout = impl_->typeLayouts[slot.payload.s.layoutId];
                const std::size_t elemCount = slot.payload.s.length;
                if (oldSc && elemCount > 0) {
                    std::vector<Value> elems(elemCount);
                    bool anyElemChanged = false;
                    for (std::size_t idx = 0; idx < elemCount; ++idx) {
                        Value orig = reconstructDetachedStructFromLeaves(
                            *oldSc,
                            slot.payload.s.data + idx * static_cast<std::size_t>(oldSc->flatSlotCount));
                        bool ch = false;
                        elems[idx] = migrateEmbeddedValue(migrateEmbeddedValue, orig, 0, ch);
                        anyElemChanged = anyElemChanged || ch;
                    }
                    const auto* newSc = findFlatStructSchemaByTypeId(arrLayout.elementTypeId);
                    if (anyElemChanged || (newSc && newSc->schemaId != oldSc->schemaId)) {
                        const std::size_t oldSlotBytes = impl_->slotSizeBytes(slotIdx, slot);
                        if (newSc && newSc->schemaId != 0u) {
                            const std::size_t newStride = newSc->flatSlotCount;
                            const std::size_t totalVals = elemCount * newStride;
                            std::vector<Value> newBuf(totalVals);
                            const ObjectRef ownerRef{slotIdx, slot.generation, ObjectKind::Array, impl_->heapId};
                            for (std::size_t idx = 0; idx < elemCount; ++idx) {
                                if (!extractStructLeavesToBuffer(elems[idx], *newSc, newBuf.data() + idx * newStride)) {
                                    std::memcpy(
                                        static_cast<void*>(newBuf.data() + idx * newStride),
                                        static_cast<const void*>(newSc->prototypeLeaves.data()),
                                        newStride * sizeof(Value));
                                }
                                for (std::size_t k = 0; k < newStride; ++k) {
                                    impl_->pinHeapValueIfNeededUnlocked(newBuf[idx * newStride + k]);
                                }
                            }
                            Value* newPtr = nullptr;
                            const std::uint32_t newHandle = impl_->largeBuffers.acquireMove(std::move(newBuf), newPtr);
                            if (slot.payload.s.handle != CompactSlot::kInvalidHandle) {
                                impl_->largeBuffers.release(slot.payload.s.handle);
                            }
                            slot.auxMeta = newSc->schemaId;
                            slot.payload.s.data = newPtr;
                            slot.payload.s.handle = newHandle;
                            impl_->gcAndLayoutEpoch.fetch_add(1, std::memory_order_release);
                            if (!slot.hasFlag(GcFlag::InNursery) && newSc->containsManagedReferences) {
                                if (elemCount >= ManagedHeap::kCardTableMinLength &&
                                    !slot.hasFlag(GcFlag::HasCardTable)) {
                                    slot.payload.s.cardHandle = impl_->cardArena.allocate(elemCount);
                                    slot.gcFlags |= GcFlag::HasCardTable;
                                }
                                for (std::size_t idx = 0; idx < elemCount; ++idx) {
                                    impl_->writeBarrierFlatStructElement(ownerRef, idx);
                                }
                            }
                            const std::size_t newSlotBytes = impl_->slotSizeBytes(slotIdx, slot);
                            if (!slot.hasFlag(GcFlag::CustomSize)) {
                                if (newSlotBytes >= oldSlotBytes) {
                                    const std::size_t d = newSlotBytes - oldSlotBytes;
                                    impl_->statistics.liveBytes += d;
                                    impl_->statistics.bytesAllocated += d;
                                    impl_->statistics.peakLiveBytes = std::max(
                                        impl_->statistics.peakLiveBytes, impl_->statistics.liveBytes);
                                } else {
                                    const std::size_t d = oldSlotBytes - newSlotBytes;
                                    impl_->statistics.liveBytes = (impl_->statistics.liveBytes >= d)
                                        ? (impl_->statistics.liveBytes - d)
                                        : 0;
                                    impl_->statistics.bytesReclaimed += d;
                                }
                            }
                        }
                    }
                }
                continue;
            }
            const std::size_t count = Impl::slotValueCount(slot);
            if (count == 0) continue;
            Value* data = slot.payload.s.data;
            const auto& layout = impl_->typeLayouts[slot.payload.s.layoutId];

            if (slot.kind() == ObjectKind::Record &&
                count == 8u &&
                !delegateUpdates.empty() &&
                (delegateTypeIds.count(layout.typeId) != 0 ||
                 (layout.referenceFields.size() == 2u &&
                  layout.referenceFields[0] == 0u &&
                  layout.referenceFields[1] == 7u))) {
                std::int64_t flags = -1;
                std::int64_t lo = -1;
                std::int64_t hi = -1;
                if (data[3].tryGetInt64(flags) &&
                    flags >= 0 && flags <= 2 &&
                    data[1].tryGetInt64(lo) &&
                    data[2].tryGetInt64(hi) &&
                    lo >= 0 && hi >= 0 &&
                    lo <= std::numeric_limits<std::uint32_t>::max() &&
                    hi <= std::numeric_limits<std::uint32_t>::max()) {
                    const semantic::SymbolId symId =
                        static_cast<std::uint64_t>(static_cast<std::uint32_t>(lo)) |
                        (static_cast<std::uint64_t>(static_cast<std::uint32_t>(hi)) << 32u);
                    const auto uIt = delegateUpdates.find(symId);
                    if (uIt != delegateUpdates.end() && !uIt->second.removed) {
                        data[3] = Value{uIt->second.dispatchFlags};
                        data[4] = Value{static_cast<std::int64_t>(uIt->second.dispatchSlot)};
                        ++localStats.delegatesRebound;
                    }
                }
            }

            if (structMigrations.empty() && removedTypeIds.empty()) continue;

            if (slot.kind() == ObjectKind::Array) {
                if (!layout.mayContainReferences) continue;
                for (std::size_t idx = 0; idx < count; ++idx) {
                    bool changed = false;
                    Value updated = migrateEmbeddedValue(migrateEmbeddedValue, data[idx], 0, changed);
                    if (!changed) continue;
                    impl_->pinHeapValueIfNeededUnlocked(updated);
                    const ObjectRef ownerRef{slotIdx, slot.generation, ObjectKind::Array, impl_->heapId};
                    impl_->writeBarrierArrayElement(ownerRef, idx, updated);
                    data[idx] = std::move(updated);
                }
            } else if (slot.kind() == ObjectKind::Record) {
                for (std::size_t idx = 0; idx < count; ++idx) {
                    bool changed = false;
                    Value updated = migrateEmbeddedValue(migrateEmbeddedValue, data[idx], 0, changed);
                    if (!changed) continue;
                    impl_->pinHeapValueIfNeededUnlocked(updated);
                    const ObjectRef ownerRef{slotIdx, slot.generation, ObjectKind::Record, impl_->heapId};
                    impl_->writeBarrierObject(ownerRef, updated);
                    data[idx] = std::move(updated);
                }
            }
        }
    }

    // Migrate Native ECS component tables if any struct layouts changed
    if (!structMigrations.empty() || !removedTypeIds.empty()) {
        for (auto& [tableName, table] : impl_->nativeEcsTables) {
            (void)tableName;
            for (auto& compVal : table.values) {
                bool changed = false;
                Value updated = migrateEmbeddedValue(migrateEmbeddedValue, compVal, 0, changed);
                if (!changed) continue;
                impl_->pinHeapValueIfNeededUnlocked(updated);
                if (Impl::valueMayContainReferences(updated)) {
                    table.mayContainReferences = true;
                }
                if (impl_->phase != GcPhase::Idle) {
                    impl_->markValue(updated);
                }
                compVal = std::move(updated);
            }
        }
        if (impl_->phase != GcPhase::Idle && !impl_->markStack.empty()) {
            impl_->reopenMarkFromSweep();
        }
    }

    // Migrate & clean up staticFields (snapshot source values first so swap-renames never clobber)
    std::unordered_map<semantic::SymbolId, Value> oldStaticSnapshot;
    std::unordered_set<semantic::SymbolId> activeNewStaticIds;
    for (const auto& mig : migrations) {
        for (const auto& sop : mig.staticFieldOps) {
            if (sop.targetFieldId != 0) {
                activeNewStaticIds.insert(sop.targetFieldId);
            }
            if (sop.sourceFieldId != 0) {
                const auto found = impl_->staticFields.find(sop.sourceFieldId);
                if (found != impl_->staticFields.end()) {
                    oldStaticSnapshot.emplace(sop.sourceFieldId, found->second);
                }
            }
        }
    }

    const auto storeStaticUnlocked = [&](semantic::SymbolId sid, Value val) {
        if (impl_->phase != GcPhase::Idle) {
            impl_->markValue(val);
            if (!impl_->markStack.empty()) {
                impl_->reopenMarkFromSweep();
            }
        }
        impl_->pinHeapValueIfNeededUnlocked(val);
        impl_->staticFields[sid] = std::move(val);
    };

    for (const auto& mig : migrations) {
        for (const auto& sop : mig.staticFieldOps) {
            if (sop.targetFieldId == 0) continue;
            const auto srcIt = (sop.sourceFieldId != 0)
                ? oldStaticSnapshot.find(sop.sourceFieldId)
                : oldStaticSnapshot.end();
            if (srcIt != oldStaticSnapshot.end()) {
                const Value& oldVal = srcIt->second;
                if (sop.kind == FieldMigrationKind::Copy) {
                    bool dummyChanged = false;
                    Value candidate = migrateEmbeddedValue(migrateEmbeddedValue, oldVal, 0, dummyChanged);
                    bool valid = true;
                    if (sop.targetType == semantic::PrimitiveType::Object) {
                        valid = isObjectValueAssignable(
                            candidate, sop.targetTypeId, sop.assignableTypeIds);
                    } else if (sop.targetType == semantic::PrimitiveType::Array) {
                        valid = isArrayValueCompatible(candidate, sop.targetTypeId);
                    } else if (sop.targetType != semantic::PrimitiveType::Error &&
                               valueType(candidate) != sop.targetType) {
                        valid = false;
                    }
                    if (valid) {
                        storeStaticUnlocked(sop.targetFieldId, std::move(candidate));
                        ++localStats.staticFieldsMigrated;
                        if (sop.renamed) ++localStats.fieldsRenamed;
                    } else {
                        storeStaticUnlocked(sop.targetFieldId, sop.defaultValue);
                    }
                } else if (sop.kind == FieldMigrationKind::ConvertNumeric) {
                    Value converted{};
                    if (tryConvertNumeric(
                            oldVal, sop.targetType, converted, sop.checkedArithmetic)) {
                        storeStaticUnlocked(sop.targetFieldId, std::move(converted));
                        ++localStats.staticFieldsMigrated;
                        ++localStats.fieldsConverted;
                        if (sop.renamed) ++localStats.fieldsRenamed;
                    } else {
                        storeStaticUnlocked(sop.targetFieldId, sop.defaultValue);
                    }
                } else {
                    storeStaticUnlocked(sop.targetFieldId, sop.defaultValue);
                }
            } else if (sop.hasConstantInitializer &&
                       impl_->staticFields.find(sop.targetFieldId) == impl_->staticFields.end()) {
                storeStaticUnlocked(sop.targetFieldId, sop.defaultValue);
            }
        }
    }

    for (const auto& mig : migrations) {
        for (const auto deadSid : mig.removedStaticFieldIds) {
            if (activeNewStaticIds.count(deadSid) != 0) continue;
            if (impl_->staticFields.erase(deadSid) != 0) {
                ++localStats.staticFieldsRemoved;
            }
        }
    }

    if (!structMigrations.empty() || !removedTypeIds.empty()) {
        for (auto& [sid, sval] : impl_->staticFields) {
            (void)sid;
            bool changed = false;
            Value updated = migrateEmbeddedValue(migrateEmbeddedValue, sval, 0, changed);
            if (!changed) continue;
            if (impl_->phase != GcPhase::Idle) {
                impl_->markValue(updated);
                if (!impl_->markStack.empty()) {
                    impl_->reopenMarkFromSweep();
                }
            }
            impl_->pinHeapValueIfNeededUnlocked(updated);
            sval = std::move(updated);
        }
    }

    if (outStats) *outStats = localStats;
    return true;
}

namespace detail {

void* localBlittableRegistryPtr() noexcept {
    return &localBlittableRegistry();
}

void* localHeapPoolMarkerShardsPtr() noexcept {
    return &localHeapPoolMarkerShards();
}

void* localLiveStringHeapShardsPtr() noexcept {
    return &localLiveStringHeapShards();
}

} // namespace detail

std::size_t reclaimUnreferencedPoolValues(
    const ShadowStack* activeShadowStack) noexcept {
    return reclaimPoolValuesInternal(activeShadowStack, /*waitIfBusy=*/true);
}

} // namespace realscript::runtime
