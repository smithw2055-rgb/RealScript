#include "realscript/runtime/AsyncRuntime.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <utility>

namespace realscript::runtime {

namespace {

constexpr semantic::SymbolId makeAsyncRecordSymbolId(
    AsyncPayloadKind kind,
    std::uint64_t requestArgumentHash) noexcept {
    const auto folded = static_cast<std::uint32_t>(
        (requestArgumentHash ^ (requestArgumentHash >> 32u)) & 0x0000FFFFu);
    return static_cast<semantic::SymbolId>(
        0xA5000000u | (folded << 8u) | static_cast<std::uint32_t>(kind));
}

constexpr AsyncPayloadKind extractAsyncRecordKind(semantic::SymbolId symbolId) noexcept {
    const auto raw = static_cast<std::uint8_t>(symbolId & 0xFFu);
    if (raw > static_cast<std::uint8_t>(AsyncPayloadKind::Canceled)) {
        return AsyncPayloadKind::Faulted;
    }
    return static_cast<AsyncPayloadKind>(raw);
}

constexpr bool hasAsyncRecordMagic(semantic::SymbolId symbolId) noexcept {
    return (static_cast<std::uint32_t>(symbolId) & 0xFF000000u) == 0xA5000000u;
}

constexpr std::uint32_t nextNonZeroVersion(std::uint32_t current) noexcept {
    const std::uint32_t next = current + 1u;
    return next == 0u ? 1u : next;
}

constexpr std::uint64_t packCombinatorTag(
    std::uint32_t combSlot,
    std::size_t childIndex) noexcept {
    return (static_cast<std::uint64_t>(combSlot) << 32u) |
        static_cast<std::uint64_t>(childIndex & 0xFFFFFFFFu);
}

constexpr void unpackCombinatorTag(
    std::uint64_t packed,
    std::uint32_t& outCombSlot,
    std::size_t& outChildIndex) noexcept {
    outCombSlot = static_cast<std::uint32_t>(packed >> 32u);
    outChildIndex = static_cast<std::size_t>(packed & 0xFFFFFFFFu);
}

Value buildWhenAnyStructValue(
    std::size_t winnerIndex,
    AwaitStatus winnerStatus,
    const Value& winnerValue,
    ErrorCode errorCode) {
    const std::array<Value, 4> fields = {
        Value(static_cast<std::int64_t>(winnerIndex)),
        Value(static_cast<std::int64_t>(static_cast<std::uint8_t>(winnerStatus))),
        winnerValue,
        Value(static_cast<std::int64_t>(static_cast<std::uint16_t>(errorCode))),
    };
    return Value(makeStructValueFromFields(whenAnyResultTypeId(), fields));
}

} // namespace

const char* awaitStatusName(AwaitStatus status) noexcept {
    switch (status) {
    case AwaitStatus::Pending: return "pending";
    case AwaitStatus::Succeeded: return "succeeded";
    case AwaitStatus::Faulted: return "faulted";
    case AwaitStatus::Canceled: return "canceled";
    }
    return "pending";
}

const char* resumeQueueName(ResumeQueue queue) noexcept {
    switch (queue) {
    case ResumeQueue::PreTickIo: return "pre-tick-io";
    case ResumeQueue::FixedTick: return "fixed-tick";
    case ResumeQueue::Update: return "update";
    case ResumeQueue::EndOfFrame: return "end-of-frame";
    }
    return "update";
}

semantic::SymbolId whenAnyResultTypeId() noexcept {
    static const semantic::SymbolId id =
        semantic::stableTypeId("RealScript.Async.WhenAnyResult");
    return id;
}

semantic::SymbolId awaitableVoidTypeId() noexcept {
    static const semantic::SymbolId id =
        semantic::stableTypeId("RealScript.Async::Awaitable");
    return id;
}

semantic::SymbolId asyncExceptionTypeId() noexcept {
    static const semantic::SymbolId id =
        semantic::stableTypeId("RealScript.Async::AsyncException");
    return id;
}

semantic::SymbolId asyncCanceledExceptionTypeId() noexcept {
    static const semantic::SymbolId id =
        semantic::stableTypeId("RealScript.Async::AsyncCanceledException");
    return id;
}

// ============================================================================
// CancellationTokenPool
// ============================================================================

CancellationSourceHandle CancellationTokenPool::createSource() {
    std::uint32_t slotIndex = 0;
    if (!freeSlots_.empty()) {
        slotIndex = freeSlots_.back();
        freeSlots_.pop_back();
    } else {
        slotIndex = static_cast<std::uint32_t>(slots_.size());
        slots_.emplace_back();
    }
    auto& slot = slots_[slotIndex];
    slot.allocated = true;
    slot.canceled = false;
    slot.timedOut = false;
    slot.parentRegistrations.clear();
    ++totalCreated_;
    return CancellationSourceHandle{slotIndex, slot.version};
}

CancellationSourceHandle CancellationTokenPool::createLinkedSource(
    const CancellationToken* parents,
    std::size_t parentCount) {
    const CancellationSourceHandle child = createSource();
    if (!parents || parentCount == 0) {
        return child;
    }

    for (std::size_t i = 0; i < parentCount; ++i) {
        if (isCancellationRequested(parents[i])) {
            if (isTimedOut(parents[i])) {
                cancelByTimeout(child);
            } else {
                cancel(child);
            }
            return child;
        }
    }

    const std::uint64_t packedChild = child.pack64();
    for (std::size_t i = 0; i < parentCount; ++i) {
        if (!parents[i].canBeCanceled()) continue;
        const CancellationRegistration reg = registerCallback(
            parents[i],
            &CancellationTokenPool::onLinkedParentCanceled,
            this,
            packedChild);
        if (isCancellationRequested(token(child))) {
            break;
        }
        if (reg.valid()) {
            slots_[child.slot].parentRegistrations.push_back(reg);
        }
    }
    return child;
}

CancellationSourceHandle CancellationTokenPool::createLinkedSource(
    std::initializer_list<CancellationToken> parents) {
    return createLinkedSource(parents.begin(), parents.size());
}

CancellationToken CancellationTokenPool::token(
    CancellationSourceHandle source) const noexcept {
    if (!isSourceAlive(source)) {
        return CancellationToken::none();
    }
    return CancellationToken{source.slot, source.version};
}

bool CancellationTokenPool::isSourceAlive(
    CancellationSourceHandle source) const noexcept {
    if (!source.valid() || source.slot >= slots_.size()) return false;
    const auto& slot = slots_[source.slot];
    return slot.allocated && slot.version == source.version;
}

bool CancellationTokenPool::isCancellationRequested(
    CancellationToken token) const noexcept {
    if (!token.canBeCanceled()) return false;
    if (token.isPreCanceledSentinel()) return true;
    if (token.ctsSlot >= slots_.size()) return false;
    const auto& slot = slots_[token.ctsSlot];
    if (slot.allocated && slot.version == token.version) {
        return slot.canceled;
    }
    return slot.lastCanceledVersion == token.version;
}

bool CancellationTokenPool::isTimedOut(CancellationToken token) const noexcept {
    if (!token.canBeCanceled() || token.isPreCanceledSentinel()) return false;
    if (token.ctsSlot >= slots_.size()) return false;
    const auto& slot = slots_[token.ctsSlot];
    if (slot.allocated && slot.version == token.version) {
        return slot.timedOut;
    }
    return slot.lastTimedOutVersion == token.version;
}

void CancellationTokenPool::onLinkedParentCanceled(
    void* context,
    std::uint64_t packedChildSource) {
    auto* self = static_cast<CancellationTokenPool*>(context);
    if (!self) return;
    const auto childHandle = CancellationSourceHandle::unpack64(packedChildSource);
    if (self->activeTimeoutCascade_) {
        self->cancelByTimeout(childHandle);
    } else {
        self->cancel(childHandle);
    }
}

void CancellationTokenPool::detachParentRegistrations(CtsSlot& slot) noexcept {
    if (slot.parentRegistrations.empty()) return;
    std::vector<CancellationRegistration> regs;
    regs.swap(slot.parentRegistrations);
    for (const auto& reg : regs) {
        unregisterCallback(reg);
    }
}

std::size_t CancellationTokenPool::cancelImpl(
    CancellationSourceHandle source,
    bool byTimeout) {
    if (!isSourceAlive(source)) return 0;
    auto& slot = slots_[source.slot];
    if (slot.canceled) return 0;

    slot.canceled = true;
    slot.lastCanceledVersion = slot.version;
    if (byTimeout) {
        slot.timedOut = true;
        slot.lastTimedOutVersion = slot.version;
    }
    detachParentRegistrations(slot);

    struct PendingInvoke {
        CancelCallbackFn fn = nullptr;
        void* ctx = nullptr;
        std::uint64_t tag = 0;
    };
    std::array<PendingInvoke, 8> inlineInvoke{};
    std::size_t inlineCount = 0;
    std::vector<PendingInvoke> overflowInvoke;

    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(slot.callbacks.size()); ++i) {
        auto& cb = slot.callbacks[i];
        if (!cb.active || !cb.callback) continue;
        const PendingInvoke item{cb.callback, cb.context, cb.userTag};
        if (inlineCount < inlineInvoke.size()) {
            inlineInvoke[inlineCount++] = item;
        } else {
            overflowInvoke.push_back(item);
        }
        cb.active = false;
        cb.callback = nullptr;
        cb.context = nullptr;
        cb.userTag = 0;
        cb.generation = nextNonZeroVersion(cb.generation);
        slot.freeCallbacks.push_back(i);
    }

    const bool prevCascade = activeTimeoutCascade_;
    struct CascadeRestore {
        bool& target;
        bool saved;
        ~CascadeRestore() { target = saved; }
    } cascadeGuard{activeTimeoutCascade_, prevCascade};

    for (std::size_t i = 0; i < inlineCount; ++i) {
        ++totalCallbacksInvoked_;
        activeTimeoutCascade_ = byTimeout;
        inlineInvoke[i].fn(inlineInvoke[i].ctx, inlineInvoke[i].tag);
    }
    for (const auto& item : overflowInvoke) {
        ++totalCallbacksInvoked_;
        activeTimeoutCascade_ = byTimeout;
        item.fn(item.ctx, item.tag);
    }
    return inlineCount + overflowInvoke.size();
}

std::size_t CancellationTokenPool::cancel(CancellationSourceHandle source) {
    return cancelImpl(source, false);
}

std::size_t CancellationTokenPool::cancelByTimeout(CancellationSourceHandle source) {
    return cancelImpl(source, true);
}

bool CancellationTokenPool::releaseSource(
    CancellationSourceHandle source) noexcept {
    if (!isSourceAlive(source)) return false;
    auto& slot = slots_[source.slot];
    detachParentRegistrations(slot);

    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(slot.callbacks.size()); ++i) {
        auto& cb = slot.callbacks[i];
        if (!cb.active) continue;
        cb.active = false;
        cb.callback = nullptr;
        cb.context = nullptr;
        cb.userTag = 0;
        cb.generation = nextNonZeroVersion(cb.generation);
        slot.freeCallbacks.push_back(i);
    }

    slot.allocated = false;
    slot.canceled = false;
    slot.timedOut = false;
    slot.version = nextNonZeroVersion(slot.version);
    freeSlots_.push_back(source.slot);
    ++totalReleased_;
    return true;
}

CancellationRegistration CancellationTokenPool::registerCallback(
    CancellationToken token,
    CancelCallbackFn callback,
    void* context,
    std::uint64_t userTag) {
    if (!callback || !token.canBeCanceled()) {
        return CancellationRegistration{};
    }
    if (isCancellationRequested(token)) {
        ++totalCallbacksInvoked_;
        callback(context, userTag);
        return CancellationRegistration{};
    }
    if (token.ctsSlot >= slots_.size()) {
        return CancellationRegistration{};
    }
    auto& slot = slots_[token.ctsSlot];
    if (!slot.allocated || slot.version != token.version) {
        return CancellationRegistration{};
    }

    std::uint32_t cbIndex = 0;
    if (!slot.freeCallbacks.empty()) {
        cbIndex = slot.freeCallbacks.back();
        slot.freeCallbacks.pop_back();
    } else {
        cbIndex = static_cast<std::uint32_t>(slot.callbacks.size());
        slot.callbacks.emplace_back();
    }

    auto& entry = slot.callbacks[cbIndex];
    entry.callback = callback;
    entry.context = context;
    entry.userTag = userTag;
    entry.active = true;
    return CancellationRegistration{
        token.ctsSlot,
        token.version,
        cbIndex,
        entry.generation,
    };
}

bool CancellationTokenPool::unregisterCallback(
    CancellationRegistration registration) noexcept {
    if (!registration.valid() || registration.ctsSlot >= slots_.size()) {
        return false;
    }
    auto& slot = slots_[registration.ctsSlot];
    if (!slot.allocated || slot.version != registration.ctsVersion) {
        return false;
    }
    if (registration.callbackIndex >= slot.callbacks.size()) {
        return false;
    }
    auto& entry = slot.callbacks[registration.callbackIndex];
    if (!entry.active || entry.generation != registration.callbackGeneration) {
        return false;
    }
    entry.active = false;
    entry.callback = nullptr;
    entry.context = nullptr;
    entry.userTag = 0;
    entry.generation = nextNonZeroVersion(entry.generation);
    slot.freeCallbacks.push_back(registration.callbackIndex);
    return true;
}

CancellationToken CancellationTokenPool::getOrCreateEntityDestroyToken(
    GenerationalEntityHandle entity,
    EntityDestroyDomain domain) {
    const auto domainIdx = static_cast<std::size_t>(domain);
    if (!entity.valid() || domainIdx >= kEntityDestroyDomainCount) {
        return CancellationToken::preCanceled();
    }
    auto& map = entityDestroySources_[domainIdx];
    const std::uint64_t key = entity.pack64();
    const auto found = map.find(key);
    if (found != map.end() && isSourceAlive(found->second)) {
        return token(found->second);
    }
    const CancellationSourceHandle created = createSource();
    map[key] = created;
    return token(created);
}

bool CancellationTokenPool::hasEntityDestroyToken(
    GenerationalEntityHandle entity,
    EntityDestroyDomain domain) const noexcept {
    const auto domainIdx = static_cast<std::size_t>(domain);
    if (!entity.valid() || domainIdx >= kEntityDestroyDomainCount) return false;
    const auto& map = entityDestroySources_[domainIdx];
    const auto found = map.find(entity.pack64());
    return found != map.end() && isSourceAlive(found->second);
}

bool CancellationTokenPool::cancelEntity(
    GenerationalEntityHandle entity,
    EntityDestroyDomain domain) {
    const auto domainIdx = static_cast<std::size_t>(domain);
    if (!entity.valid() || domainIdx >= kEntityDestroyDomainCount) return false;
    auto& map = entityDestroySources_[domainIdx];
    const std::uint64_t key = entity.pack64();
    const auto found = map.find(key);
    if (found == map.end()) {
        return false;
    }
    const CancellationSourceHandle handle = found->second;
    map.erase(found);
    if (!isSourceAlive(handle)) {
        return false;
    }
    cancel(handle);
    releaseSource(handle);
    return true;
}

void CancellationTokenPool::clear() noexcept {
    slots_.clear();
    freeSlots_.clear();
    for (auto& map : entityDestroySources_) {
        map.clear();
    }
    totalCreated_ = 0;
    totalReleased_ = 0;
    totalCallbacksInvoked_ = 0;
    activeTimeoutCascade_ = false;
}

CancellationPoolStatistics CancellationTokenPool::statistics() const noexcept {
    CancellationPoolStatistics stats;
    stats.capacity = slots_.size();
    stats.freeSources = freeSlots_.size();
    stats.activeSources = stats.capacity >= stats.freeSources
        ? (stats.capacity - stats.freeSources)
        : 0;
    std::size_t totalEntityTokens = 0;
    for (const auto& map : entityDestroySources_) {
        totalEntityTokens += map.size();
    }
    stats.activeEntityTokens = totalEntityTokens;
    stats.totalCreated = totalCreated_;
    stats.totalReleased = totalReleased_;
    stats.totalCallbacksInvoked = totalCallbacksInvoked_;
    return stats;
}

// ============================================================================
// AwaitableCorePool
// ============================================================================

AwaitableCorePool::AwaitableCorePool(
    ManagedHeap* heap,
    CancellationTokenPool* cancellationPool,
    std::size_t initialReserve)
    : heap_(heap),
      cancellationPool_(cancellationPool) {
    if (initialReserve > 0) {
        const std::size_t pagesNeeded = (initialReserve + kPageSize - 1u) / kPageSize;
        pages_.reserve(pagesNeeded);
        for (std::size_t p = 0; p < pagesNeeded; ++p) {
            pages_.push_back(std::make_unique<AwaitableSlot[]>(kPageSize));
            slotCapacity_ += kPageSize;
        }
        freeSlots_.reserve(slotCapacity_);
    }
    for (auto& rq : readyQueues_) {
        rq.items.reserve(64);
    }
}

AwaitableCorePool::~AwaitableCorePool() noexcept {
    clear();
}

void AwaitableCorePool::setHeap(ManagedHeap* heap) noexcept {
    if (heap_ == heap) return;

    // 1. Release all persistent root tokens registered against the old heap
    if (heap_) {
        for (auto& rq : readyQueues_) {
            for (std::size_t i = rq.head; i < rq.items.size(); ++i) {
                releaseRootToken(rq.items[i].pinnedTargetToken);
            }
        }
        for (auto& comb : combinators_) {
            for (auto& token : comb.valueRootTokens) {
                releaseRootToken(token);
            }
        }
        for (std::uint32_t i = 0; i < nextNeverUsedSlot_; ++i) {
            auto& slot = slotAt(i);
            releaseRootToken(slot.resultRootToken);
            releaseRootToken(slot.continuationTargetRootToken);
            if (slot.cold) {
                releaseRootToken(slot.cold->exceptionRootToken);
            }
        }
    }
    activeGcRoots_ = 0;
    heap_ = heap;

    // 2. Re-pin any active traceable values onto the new heap
    if (heap_) {
        for (auto& comb : combinators_) {
            if (!comb.allocated || comb.resolved) continue;
            const std::size_t n = std::min(comb.values.size(), comb.valueRootTokens.size());
            for (std::size_t i = 0; i < n; ++i) {
                pinValueIfNeeded(comb.values[i], comb.valueRootTokens[i]);
            }
        }
        for (std::uint32_t i = 0; i < nextNeverUsedSlot_; ++i) {
            auto& slot = slotAt(i);
            if (!slot.allocated) continue;
            if (slot.status == AwaitStatus::Succeeded && slot.result.isTraceable()) {
                pinValueIfNeeded(slot.result, slot.resultRootToken);
            }
            if (slot.hasContinuation && slot.continuation.scriptTarget.valid()) {
                slot.continuationTargetRootToken =
                    heap_->addPersistentRoot(slot.continuation.scriptTarget);
                if (slot.continuationTargetRootToken != 0) {
                    ++activeGcRoots_;
                }
            }
            if (slot.dirtyCold && slot.cold && slot.cold->exceptionObject.isTraceable()) {
                pinValueIfNeeded(slot.cold->exceptionObject, slot.cold->exceptionRootToken);
            }
        }
    }
}

void AwaitableCorePool::setCancellationPool(
    CancellationTokenPool* cancellationPool) noexcept {
    cancellationPool_ = cancellationPool;
}

bool AwaitableCorePool::canRegisterContinuation(AwaitableHandle handle) const noexcept {
    if (handle.isCompletedVoidSentinel()) return true;
    if (!isValid(handle)) return false;
    return !slotAt(handle.slot).hasContinuation;
}

std::uint32_t AwaitableCorePool::allocateRawSlot(ResumeQueue defaultQueue) {
    std::uint32_t slotIndex = 0;
    if (!freeSlots_.empty()) {
        slotIndex = freeSlots_.back();
        freeSlots_.pop_back();
    } else {
        if (nextNeverUsedSlot_ >= slotCapacity_) {
            pages_.push_back(std::make_unique<AwaitableSlot[]>(kPageSize));
            slotCapacity_ += kPageSize;
        }
        slotIndex = nextNeverUsedSlot_++;
    }
    auto& slot = slotAt(slotIndex);
    slot.allocated = true;
    slot.status = AwaitStatus::Pending;
    slot.defaultQueue = defaultQueue;
    slot.errorCode = ErrorCode::None;
    slot.hasContinuation = false;
    slot.autoRecycleOnComplete = false;
    ++totalAllocated_;
    return slotIndex;
}

namespace {

inline bool valueNeedsManagedHeapRoot(const Value& v) noexcept {
    if (std::holds_alternative<ObjectRef>(v)) {
        return std::get<ObjectRef>(v).valid();
    }
    if (const auto* st = std::get_if<StructValue>(&v)) {
        return !st->isInline && st->storage != nullptr;
    }
    return false;
}

} // namespace

void AwaitableCorePool::releaseRootToken(ManagedHeap::RootToken& token) noexcept {
    if (token != 0) {
        if (heap_) {
            (void)heap_->removePersistentRoot(token);
        }
        token = 0;
        if (activeGcRoots_ > 0) --activeGcRoots_;
    }
}

void AwaitableCorePool::pinValueIfNeeded(
    const Value& value,
    ManagedHeap::RootToken& rootToken) {
    releaseRootToken(rootToken);
    if (heap_ && valueNeedsManagedHeapRoot(value)) {
        rootToken = heap_->addPersistentRoot(value);
        if (rootToken != 0) {
            ++activeGcRoots_;
        }
    }
}

void AwaitableCorePool::detachCancellation(AwaitableSlot& slot) noexcept {
    if (slot.cancellationReg.valid()) {
        if (cancellationPool_) {
            cancellationPool_->unregisterCallback(slot.cancellationReg);
        }
        slot.cancellationReg = CancellationRegistration{};
    }
}

void AwaitableCorePool::recycleSlotAt(std::uint32_t slotIndex) noexcept {
    if (slotIndex >= nextNeverUsedSlot_) return;
    auto& slot = slotAt(slotIndex);
    if (!slot.allocated) return;

    detachCancellation(slot);

    releaseRootToken(slot.resultRootToken);
    releaseRootToken(slot.continuationTargetRootToken);
    if (slot.dirtyCold && slot.cold) {
        slot.cold->resetHot(heap_, activeGcRoots_);
        slot.dirtyCold = false;
    }

    slot.result = Value{};
    if (slot.hasContinuation) {
        slot.hasContinuation = false;
        slot.continuation = AsyncContinuation{};
    }
    slot.autoRecycleOnComplete = false;
    slot.errorCode = ErrorCode::None;
    slot.allocated = false;
    slot.status = AwaitStatus::Pending;
    slot.version = nextNonZeroVersion(slot.version);
    freeSlots_.push_back(slotIndex);
    ++totalRecycled_;
}

void AwaitableCorePool::onTokenCanceledCallback(
    void* context,
    std::uint64_t packedHandle) {
    auto* self = static_cast<AwaitableCorePool*>(context);
    if (!self) return;
    self->trySetCanceled(AwaitableHandle::unpack64(packedHandle));
}

AwaitableHandle AwaitableCorePool::allocatePending(
    ResumeQueue defaultQueue,
    CancellationToken token) {
    const std::uint32_t slotIndex = allocateRawSlot(defaultQueue);
    const AwaitableHandle handle{slotIndex, slotAt(slotIndex).version};

    if (!token.canBeCanceled()) {
        return handle;
    }

    if (token.isPreCanceledSentinel() ||
        (cancellationPool_ && cancellationPool_->isCancellationRequested(token))) {
        trySetCanceled(handle);
        return handle;
    }

    if (cancellationPool_) {
        const CancellationRegistration reg = cancellationPool_->registerCallback(
            token,
            &AwaitableCorePool::onTokenCanceledCallback,
            this,
            handle.pack64());
        auto& s = slotAt(slotIndex);
        if (s.allocated && s.version == handle.version && s.status == AwaitStatus::Pending) {
            s.cancellationReg = reg;
        }
    }
    return handle;
}

AwaitableHandle AwaitableCorePool::fromResult(Value value) {
    if (std::holds_alternative<std::monostate>(value)) {
        ++synchronousFastPathHits_;
        return AwaitableHandle::completedVoid();
    }
    const std::uint32_t slotIndex = allocateRawSlot(ResumeQueue::Update);
    auto& slot = slotAt(slotIndex);
    slot.status = AwaitStatus::Succeeded;
    slot.result = std::move(value);
    if (heap_ && slot.result.isTraceable()) {
        pinValueIfNeeded(slot.result, slot.resultRootToken);
    }
    ++synchronousFastPathHits_;
    return AwaitableHandle{slotIndex, slot.version};
}

AwaitableHandle AwaitableCorePool::fromFaulted(
    ErrorCode code,
    std::string message,
    Value exceptionObject) {
    const std::uint32_t slotIndex = allocateRawSlot(ResumeQueue::Update);
    auto& slot = slotAt(slotIndex);
    slot.status = AwaitStatus::Faulted;
    slot.errorCode = code == ErrorCode::None ? ErrorCode::ScriptException : code;
    if (!slot.cold) slot.cold = std::make_unique<ColdSlotState>();
    slot.dirtyCold = true;
    slot.cold->errorMessage = std::move(message);
    slot.cold->exceptionObject = std::move(exceptionObject);
    if (heap_ && slot.cold->exceptionObject.isTraceable()) {
        pinValueIfNeeded(slot.cold->exceptionObject, slot.cold->exceptionRootToken);
    }
    ++synchronousFastPathHits_;
    return AwaitableHandle{slotIndex, slot.version};
}

AwaitableHandle AwaitableCorePool::fromCanceled() {
    const std::uint32_t slotIndex = allocateRawSlot(ResumeQueue::Update);
    auto& slot = slotAt(slotIndex);
    slot.status = AwaitStatus::Canceled;
    slot.errorCode = ErrorCode::ExecutionTerminated;
    ++synchronousFastPathHits_;
    return AwaitableHandle{slotIndex, slot.version};
}

void AwaitableCorePool::enqueueReadyContinuation(
    const AsyncContinuation& cont,
    AwaitableHandle handle,
    AwaitStatus terminalStatus,
    ManagedHeap::RootToken existingTargetToken) {
    const auto queueIdx = static_cast<std::size_t>(cont.resumeQueue);
    auto& rq = readyQueues_[queueIdx < kResumeQueueCount ? queueIdx : 2u];

    ManagedHeap::RootToken pinToken = existingTargetToken;
    if (pinToken == 0 && heap_ && cont.scriptTarget.valid()) {
        pinToken = heap_->addPersistentRoot(cont.scriptTarget);
        if (pinToken != 0) {
            ++activeGcRoots_;
        }
    }

    rq.items.push_back(ReadyContinuationItem{
        cont.callback,
        cont.context,
        cont.userTag,
        handle,
        terminalStatus,
        pinToken,
    });
}

void AwaitableCorePool::dispatchOrScheduleContinuation(
    AwaitableSlot& slot,
    AwaitableHandle handle,
    AwaitStatus terminalStatus) {
    const bool shouldAutoRecycle = slot.autoRecycleOnComplete;
    if (!slot.hasContinuation) {
        if (shouldAutoRecycle) {
            recycleSlotAt(handle.slot);
        }
        return;
    }

    const AsyncContinuation cont = slot.continuation;
    ManagedHeap::RootToken movedTargetToken = slot.continuationTargetRootToken;
    slot.continuationTargetRootToken = 0;
    slot.hasContinuation = false;
    slot.continuation = AsyncContinuation{};

    if (cont.runInlineOnComplete) {
        if (cont.callback) {
            ++continuationsDispatched_;
            cont.callback(cont.context, handle, terminalStatus, cont.userTag);
        }
        releaseRootToken(movedTargetToken);
        if (shouldAutoRecycle && isValid(handle)) {
            recycleSlotAt(handle.slot);
        }
        return;
    }

    enqueueReadyContinuation(cont, handle, terminalStatus, movedTargetToken);

    if (shouldAutoRecycle && isValid(handle)) {
        recycleSlotAt(handle.slot);
    }
}

bool AwaitableCorePool::trySetResult(AwaitableHandle handle, Value value) {
    if (!isValid(handle) || handle.isCompletedVoidSentinel()) return false;
    auto& slot = slotAt(handle.slot);
    if (slot.status != AwaitStatus::Pending) return false;

    detachCancellation(slot);
    slot.status = AwaitStatus::Succeeded;
    slot.result = std::move(value);
    if (heap_ && slot.result.isTraceable()) {
        pinValueIfNeeded(slot.result, slot.resultRootToken);
    }
    dispatchOrScheduleContinuation(slot, handle, AwaitStatus::Succeeded);
    return true;
}

bool AwaitableCorePool::trySetFaulted(
    AwaitableHandle handle,
    ErrorCode code,
    std::string message,
    Value exceptionObject) {
    if (!isValid(handle) || handle.isCompletedVoidSentinel()) return false;
    auto& slot = slotAt(handle.slot);
    if (slot.status != AwaitStatus::Pending) return false;

    detachCancellation(slot);
    slot.status = AwaitStatus::Faulted;
    slot.errorCode = code == ErrorCode::None ? ErrorCode::ScriptException : code;
    if (!slot.cold) slot.cold = std::make_unique<ColdSlotState>();
    slot.dirtyCold = true;
    slot.cold->errorMessage = std::move(message);
    slot.cold->exceptionObject = std::move(exceptionObject);
    if (heap_ && slot.cold->exceptionObject.isTraceable()) {
        pinValueIfNeeded(slot.cold->exceptionObject, slot.cold->exceptionRootToken);
    }
    dispatchOrScheduleContinuation(slot, handle, AwaitStatus::Faulted);
    return true;
}

bool AwaitableCorePool::trySetCanceled(AwaitableHandle handle) {
    if (!isValid(handle) || handle.isCompletedVoidSentinel()) return false;
    auto& slot = slotAt(handle.slot);
    if (slot.status != AwaitStatus::Pending) return false;

    detachCancellation(slot);
    slot.status = AwaitStatus::Canceled;
    slot.errorCode = ErrorCode::ExecutionTerminated;
    dispatchOrScheduleContinuation(slot, handle, AwaitStatus::Canceled);
    return true;
}

bool AwaitableCorePool::onCompleted(
    AwaitableHandle handle,
    AsyncContinuation continuation) {
    if (handle.isCompletedVoidSentinel()) {
        ++synchronousFastPathHits_;
        if (continuation.runInlineOnComplete) {
            if (continuation.callback) {
                ++continuationsDispatched_;
                continuation.callback(
                    continuation.context,
                    handle,
                    AwaitStatus::Succeeded,
                    continuation.userTag);
            }
        } else {
            enqueueReadyContinuation(
                continuation, handle, AwaitStatus::Succeeded, 0);
        }
        return true;
    }

    if (!isValid(handle)) return false;
    auto& slot = slotAt(handle.slot);
    if (slot.hasContinuation) {
        return false;
    }

    if (slot.status != AwaitStatus::Pending) {
        ++synchronousFastPathHits_;
        slot.continuation = continuation;
        slot.hasContinuation = true;
        dispatchOrScheduleContinuation(slot, handle, slot.status);
        return true;
    }

    slot.continuation = continuation;
    slot.hasContinuation = true;
    if (heap_ && continuation.scriptTarget.valid()) {
        slot.continuationTargetRootToken = heap_->addPersistentRoot(continuation.scriptTarget);
        if (slot.continuationTargetRootToken != 0) {
            ++activeGcRoots_;
        }
    }
    return true;
}

bool AwaitableCorePool::isValid(AwaitableHandle handle) const noexcept {
    if (!handle.valid()) return false;
    if (handle.isCompletedVoidSentinel()) return true;
    if (handle.slot >= nextNeverUsedSlot_) return false;
    const auto& slot = slotAt(handle.slot);
    return slot.allocated && slot.version == handle.version;
}

AwaitStatus AwaitableCorePool::status(AwaitableHandle handle) const noexcept {
    if (handle.isCompletedVoidSentinel()) return AwaitStatus::Succeeded;
    if (!isValid(handle)) return AwaitStatus::Faulted;
    return slotAt(handle.slot).status;
}

bool AwaitableCorePool::isCompleted(AwaitableHandle handle) const noexcept {
    if (handle.isCompletedVoidSentinel()) return true;
    if (!isValid(handle)) return false;
    return slotAt(handle.slot).status != AwaitStatus::Pending;
}

AwaitStatus AwaitableCorePool::getResult(
    AwaitableHandle handle,
    Value& outValue,
    RuntimeError& outError,
    Value* outExceptionObject) {
    if (handle.isCompletedVoidSentinel()) {
        outValue = Value{};
        outError.code = ErrorCode::None;
        if (!outError.message.empty()) outError.message.clear();
        if (outExceptionObject) *outExceptionObject = Value{};
        return AwaitStatus::Succeeded;
    }

    if (!isValid(handle)) {
        outValue = Value{};
        outError.code = ErrorCode::InvalidProgram;
        outError.message =
            "attempted to await or consume an invalid or already-recycled AwaitableHandle";
        if (outExceptionObject) *outExceptionObject = Value{};
        return AwaitStatus::Faulted;
    }

    auto& slot = slotAt(handle.slot);
    if (slot.status == AwaitStatus::Pending) {
        outValue = Value{};
        outError.code = ErrorCode::InvalidProgram;
        outError.message =
            "attempted to call getResult() on an AwaitableHandle that is still Pending";
        if (outExceptionObject) *outExceptionObject = Value{};
        return AwaitStatus::Faulted;
    }

    const AwaitStatus terminal = slot.status;
    if (terminal == AwaitStatus::Succeeded) {
        if (slot.dirtyCold && slot.cold && slot.cold->hasWhenAnyMetadata) {
            outValue = buildWhenAnyStructValue(
                slot.cold->whenAnyWinnerIndex,
                slot.cold->whenAnyWinnerStatus,
                slot.result,
                slot.cold->whenAnyWinnerErrorCode);
        } else {
            outValue = std::move(slot.result);
        }
        outError.code = ErrorCode::None;
        if (!outError.message.empty()) outError.message.clear();
        if (outExceptionObject) *outExceptionObject = Value{};
        recycleSlotAt(handle.slot);
        return AwaitStatus::Succeeded;
    }

    outValue = Value{};
    outError.code = slot.errorCode;
    if (terminal == AwaitStatus::Canceled) {
        outError.message = "async operation was canceled";
        if (outExceptionObject) *outExceptionObject = Value{};
    } else if (slot.cold) {
        outError.message = std::move(slot.cold->errorMessage);
        if (outExceptionObject) {
            *outExceptionObject = std::move(slot.cold->exceptionObject);
        }
    } else {
        outError.message.clear();
        if (outExceptionObject) *outExceptionObject = Value{};
    }

    recycleSlotAt(handle.slot);
    return terminal;
}

AwaitStatus AwaitableCorePool::getWhenAnyResult(
    AwaitableHandle handle,
    WhenAnyOutcome& outOutcome,
    RuntimeError& outError) {
    if (!isValid(handle) || handle.isCompletedVoidSentinel()) {
        outError.code = ErrorCode::InvalidProgram;
        outError.message = "invalid or already-recycled WhenAny AwaitableHandle";
        return AwaitStatus::Faulted;
    }

    auto& slot = slotAt(handle.slot);
    if (slot.status == AwaitStatus::Pending) {
        outError.code = ErrorCode::InvalidProgram;
        outError.message = "WhenAny AwaitableHandle is still Pending";
        return AwaitStatus::Faulted;
    }

    const AwaitStatus terminal = slot.status;
    if (terminal == AwaitStatus::Succeeded && slot.cold && slot.cold->hasWhenAnyMetadata) {
        outOutcome.winnerIndex = slot.cold->whenAnyWinnerIndex;
        outOutcome.winnerStatus = slot.cold->whenAnyWinnerStatus;
        outOutcome.error.code = slot.cold->whenAnyWinnerErrorCode;
        outOutcome.error.message = std::move(slot.cold->whenAnyWinnerErrorMessage);
        outOutcome.value = std::move(slot.result);
        outError.code = ErrorCode::None;
        if (!outError.message.empty()) outError.message.clear();
    } else {
        outOutcome = WhenAnyOutcome{};
        outError.code = slot.errorCode;
        outError.message = terminal == AwaitStatus::Canceled
            ? "async operation was canceled"
            : (slot.cold ? std::move(slot.cold->errorMessage) : std::string{});
    }

    recycleSlotAt(handle.slot);
    return terminal;
}

bool AwaitableCorePool::discard(AwaitableHandle handle) noexcept {
    if (handle.isCompletedVoidSentinel()) return true;
    if (!isValid(handle)) return false;
    auto& slot = slotAt(handle.slot);
    if (slot.status != AwaitStatus::Pending) {
        recycleSlotAt(handle.slot);
        return true;
    }
    slot.autoRecycleOnComplete = true;
    return true;
}

std::size_t AwaitableCorePool::cancelContinuationsForContext(
    const void* context) noexcept {
    if (!context) return 0;
    std::size_t removed = 0;

    for (std::uint32_t i = 0; i < nextNeverUsedSlot_; ++i) {
        auto& slot = slotAt(i);
        if (!slot.allocated || !slot.hasContinuation) continue;
        if (slot.continuation.context != context) continue;

        releaseRootToken(slot.continuationTargetRootToken);
        slot.hasContinuation = false;
        slot.continuation = AsyncContinuation{};
        ++removed;

        if (slot.status != AwaitStatus::Pending) {
            recycleSlotAt(i);
        } else {
            slot.autoRecycleOnComplete = true;
        }
    }

    for (auto& rq : readyQueues_) {
        for (std::size_t i = rq.head; i < rq.items.size(); ++i) {
            auto& item = rq.items[i];
            if (!item.callback || item.context != context) continue;
            item.callback = nullptr;
            item.context = nullptr;
            releaseRootToken(item.pinnedTargetToken);
            if (!item.completedHandle.isCompletedVoidSentinel() &&
                isValid(item.completedHandle)) {
                recycleSlotAt(item.completedHandle.slot);
            }
            ++removed;
        }
    }

    return removed;
}

std::uint32_t AwaitableCorePool::allocateCombinatorSlot() {
    std::uint32_t combIdx = 0;
    if (!freeCombinators_.empty()) {
        combIdx = freeCombinators_.back();
        freeCombinators_.pop_back();
    } else {
        combIdx = static_cast<std::uint32_t>(combinators_.size());
        combinators_.emplace_back();
    }
    auto& comb = combinators_[combIdx];
    comb.allocated = true;
    comb.resolved = false;
    comb.materializeHeapArray = true;
    comb.returnWinnerIndex = false;
    comb.parentHandle = AwaitableHandle::invalid();
    comb.cancelLosersSource = CancellationSourceHandle{};
    comb.parentCancelReg = CancellationRegistration{};
    comb.totalChildren = 0;
    comb.remainingToComplete = 0;
    comb.unconsumedCallbacks = 0;
    comb.values.clear();
    comb.valueRootTokens.clear();
    return combIdx;
}

void AwaitableCorePool::releaseCombinatorValues(CombinatorSlot& comb) noexcept {
    for (auto& token : comb.valueRootTokens) {
        releaseRootToken(token);
    }
    comb.valueRootTokens.clear();
    comb.values.clear();
}

void AwaitableCorePool::recycleCombinatorSlot(std::uint32_t combIndex) noexcept {
    if (combIndex >= combinators_.size()) return;
    auto& comb = combinators_[combIndex];
    if (!comb.allocated) return;

    if (comb.parentCancelReg.valid() && cancellationPool_) {
        cancellationPool_->unregisterCallback(comb.parentCancelReg);
        comb.parentCancelReg = CancellationRegistration{};
    }
    releaseCombinatorValues(comb);
    comb.allocated = false;
    comb.resolved = false;
    comb.parentHandle = AwaitableHandle::invalid();
    comb.cancelLosersSource = CancellationSourceHandle{};
    freeCombinators_.push_back(combIndex);
}

void AwaitableCorePool::onCombinatorCanceledCallback(
    void* context,
    std::uint64_t packedCombSlot) {
    auto* self = static_cast<AwaitableCorePool*>(context);
    if (!self) return;
    const auto combIdx = static_cast<std::uint32_t>(packedCombSlot);
    if (combIdx >= self->combinators_.size()) return;
    auto& comb = self->combinators_[combIdx];
    if (!comb.allocated || comb.resolved) return;

    comb.resolved = true;
    comb.parentCancelReg = CancellationRegistration{};
    self->releaseCombinatorValues(comb);
    const AwaitableHandle parent = comb.parentHandle;
    self->trySetCanceled(parent);
    if (comb.unconsumedCallbacks == 0) {
        self->recycleCombinatorSlot(combIdx);
    }
}

void AwaitableCorePool::onCombinatorChildCompleted(
    void* context,
    AwaitableHandle childHandle,
    AwaitStatus /*childStatus*/,
    std::uint64_t packedCombAndIndex) {
    auto* self = static_cast<AwaitableCorePool*>(context);
    if (!self) return;
    std::uint32_t combIdx = 0;
    std::size_t childIdx = 0;
    unpackCombinatorTag(packedCombAndIndex, combIdx, childIdx);
    if (combIdx >= self->combinators_.size()) return;
    auto& comb = self->combinators_[combIdx];
    if (!comb.allocated) return;

    if (comb.kind == CombinatorKind::WhenAll) {
        self->handleWhenAllChild(combIdx, childIdx, childHandle);
    } else {
        self->handleWhenAnyChild(combIdx, childIdx, childHandle);
    }
}

void AwaitableCorePool::handleWhenAllChild(
    std::uint32_t combSlotIndex,
    std::size_t childIndex,
    AwaitableHandle childHandle) {
    auto& comb = combinators_[combSlotIndex];
    if (comb.unconsumedCallbacks > 0) {
        --comb.unconsumedCallbacks;
    }

    Value childValue{};
    RuntimeError childError{};
    Value childException{};
    const AwaitStatus childStatus = getResult(
        childHandle,
        childValue,
        childError,
        &childException);

    if (!comb.resolved) {
        if (childStatus == AwaitStatus::Faulted) {
            comb.resolved = true;
            if (comb.parentCancelReg.valid() && cancellationPool_) {
                cancellationPool_->unregisterCallback(comb.parentCancelReg);
                comb.parentCancelReg = CancellationRegistration{};
            }
            releaseCombinatorValues(comb);
            trySetFaulted(
                comb.parentHandle,
                childError.code,
                std::move(childError.message),
                std::move(childException));
        } else if (childStatus == AwaitStatus::Canceled) {
            comb.resolved = true;
            if (comb.parentCancelReg.valid() && cancellationPool_) {
                cancellationPool_->unregisterCallback(comb.parentCancelReg);
                comb.parentCancelReg = CancellationRegistration{};
            }
            releaseCombinatorValues(comb);
            trySetCanceled(comb.parentHandle);
        } else {
            if (childIndex < comb.values.size()) {
                if (heap_ && childValue.isTraceable()) {
                    pinValueIfNeeded(childValue, comb.valueRootTokens[childIndex]);
                }
                comb.values[childIndex] = std::move(childValue);
            }
            if (comb.remainingToComplete > 0) {
                --comb.remainingToComplete;
            }
            if (comb.remainingToComplete == 0) {
                comb.resolved = true;
                if (comb.parentCancelReg.valid() && cancellationPool_) {
                    cancellationPool_->unregisterCallback(comb.parentCancelReg);
                    comb.parentCancelReg = CancellationRegistration{};
                }
                if (comb.materializeHeapArray && heap_) {
                    RuntimeError allocErr;
                    const auto arrayRef = heap_->allocateArray(
                        comb.values.size(), Value{}, &allocErr);
                    if (!arrayRef) {
                        releaseCombinatorValues(comb);
                        trySetFaulted(
                            comb.parentHandle,
                            allocErr.code == ErrorCode::None
                                ? ErrorCode::OutOfMemory
                                : allocErr.code,
                            allocErr.message.empty()
                                ? "failed to materialize WhenAll result array"
                                : std::move(allocErr.message));
                    } else {
                        for (std::size_t i = 0; i < comb.values.size(); ++i) {
                            heap_->arraySet(*arrayRef, i, comb.values[i]);
                        }
                        releaseCombinatorValues(comb);
                        trySetResult(comb.parentHandle, Value(*arrayRef));
                    }
                } else {
                    const auto totalChildren = static_cast<std::int64_t>(comb.totalChildren);
                    releaseCombinatorValues(comb);
                    trySetResult(
                        comb.parentHandle,
                        Value(totalChildren));
                }
            }
        }
    }

    if (comb.unconsumedCallbacks == 0) {
        recycleCombinatorSlot(combSlotIndex);
    }
}

void AwaitableCorePool::handleWhenAnyChild(
    std::uint32_t combSlotIndex,
    std::size_t childIndex,
    AwaitableHandle childHandle) {
    auto& comb = combinators_[combSlotIndex];
    if (comb.unconsumedCallbacks > 0) {
        --comb.unconsumedCallbacks;
    }

    Value childValue{};
    RuntimeError childError{};
    Value childException{};
    const AwaitStatus childStatus = getResult(
        childHandle,
        childValue,
        childError,
        &childException);

    if (!comb.resolved) {
        comb.resolved = true;
        if (comb.parentCancelReg.valid() && cancellationPool_) {
            cancellationPool_->unregisterCallback(comb.parentCancelReg);
            comb.parentCancelReg = CancellationRegistration{};
        }
        releaseCombinatorValues(comb);

        if (comb.cancelLosersSource.valid() && cancellationPool_) {
            cancellationPool_->cancel(comb.cancelLosersSource);
        }

        const AwaitableHandle parent = comb.parentHandle;
        if (comb.returnWinnerIndex) {
            trySetResult(parent, Value(static_cast<std::int64_t>(childIndex)));
        } else {
            if (isValid(parent) && !parent.isCompletedVoidSentinel()) {
                auto& parentSlot = slotAt(parent.slot);
                if (!parentSlot.cold) {
                    parentSlot.cold = std::make_unique<ColdSlotState>();
                }
                parentSlot.dirtyCold = true;
                parentSlot.cold->hasWhenAnyMetadata = true;
                parentSlot.cold->whenAnyWinnerIndex = childIndex;
                parentSlot.cold->whenAnyWinnerStatus = childStatus;
                parentSlot.cold->whenAnyWinnerErrorCode = childError.code;
                parentSlot.cold->whenAnyWinnerErrorMessage = childError.message;
            }

            Value payloadForTuple =
                (childStatus == AwaitStatus::Faulted &&
                 std::holds_alternative<std::monostate>(childValue) &&
                 !std::holds_alternative<std::monostate>(childException))
                    ? std::move(childException)
                    : std::move(childValue);
            if (isValid(parent) && !parent.isCompletedVoidSentinel()) {
                slotAt(parent.slot).cold->whenAnyWinnerValue = payloadForTuple;
            }
            trySetResult(parent, std::move(payloadForTuple));
        }
    }

    if (comb.unconsumedCallbacks == 0) {
        recycleCombinatorSlot(combSlotIndex);
    }
}

AwaitableHandle AwaitableCorePool::whenAll(
    const AwaitableHandle* tasks,
    std::size_t count,
    WhenAllOptions options) {
    if (options.token.isPreCanceledSentinel() ||
        (cancellationPool_ && cancellationPool_->isCancellationRequested(options.token))) {
        if (tasks) {
            for (std::size_t i = 0; i < count; ++i) {
                discard(tasks[i]);
            }
        }
        return fromCanceled();
    }

    if (!tasks || count == 0) {
        if (options.materializeHeapArray && heap_) {
            RuntimeError allocErr;
            if (const auto emptyArr = heap_->allocateArray(0, Value{}, &allocErr)) {
                return fromResult(Value(*emptyArr));
            }
            return fromFaulted(allocErr.code, std::move(allocErr.message));
        }
        return fromResult(Value(std::int64_t{0}));
    }

    for (std::size_t i = 0; i < count; ++i) {
        if (!isValid(tasks[i])) {
            for (std::size_t j = 0; j < count; ++j) {
                if (j != i && canRegisterContinuation(tasks[j])) discard(tasks[j]);
            }
            return fromFaulted(
                ErrorCode::InvalidProgram,
                "whenAll received an invalid or already-consumed child AwaitableHandle");
        }
        if (!canRegisterContinuation(tasks[i])) {
            for (std::size_t j = 0; j < count; ++j) {
                if (j != i && canRegisterContinuation(tasks[j])) discard(tasks[j]);
            }
            return fromFaulted(
                ErrorCode::InvalidProgram,
                "whenAll child already has a registered awaiter");
        }
        if (!tasks[i].isCompletedVoidSentinel()) {
            for (std::size_t j = 0; j < i; ++j) {
                if (tasks[j] == tasks[i]) {
                    for (std::size_t k = 0; k < count; ++k) {
                        bool firstOccurrence = true;
                        for (std::size_t m = 0; m < k; ++m) {
                            if (tasks[m] == tasks[k]) {
                                firstOccurrence = false;
                                break;
                            }
                        }
                        if (firstOccurrence && canRegisterContinuation(tasks[k])) {
                            discard(tasks[k]);
                        }
                    }
                    return fromFaulted(
                        ErrorCode::InvalidProgram,
                        "whenAll received duplicate child AwaitableHandle");
                }
            }
        }
    }

    const AwaitableHandle parent = allocatePending(
        options.resumeQueue,
        CancellationToken::none());

    const std::uint32_t combIdx = allocateCombinatorSlot();
    combinators_[combIdx].kind = CombinatorKind::WhenAll;
    combinators_[combIdx].materializeHeapArray = options.materializeHeapArray;
    combinators_[combIdx].parentHandle = parent;
    combinators_[combIdx].totalChildren = count;
    combinators_[combIdx].remainingToComplete = count;
    combinators_[combIdx].unconsumedCallbacks = count;
    combinators_[combIdx].values.resize(count);
    combinators_[combIdx].valueRootTokens.assign(count, 0);

    if (cancellationPool_ && options.token.canBeCanceled()) {
        const CancellationRegistration reg = cancellationPool_->registerCallback(
            options.token,
            &AwaitableCorePool::onCombinatorCanceledCallback,
            this,
            static_cast<std::uint64_t>(combIdx));
        if (combinators_[combIdx].allocated && !combinators_[combIdx].resolved) {
            combinators_[combIdx].parentCancelReg = reg;
        }
    }

    for (std::size_t i = 0; i < count; ++i) {
        AsyncContinuation cont;
        cont.callback = &AwaitableCorePool::onCombinatorChildCompleted;
        cont.context = this;
        cont.userTag = packCombinatorTag(combIdx, i);
        cont.resumeQueue = options.resumeQueue;
        cont.runInlineOnComplete = true;
        (void)onCompleted(tasks[i], cont);
    }

    return parent;
}

AwaitableHandle AwaitableCorePool::whenAll(
    std::initializer_list<AwaitableHandle> tasks,
    WhenAllOptions options) {
    return whenAll(tasks.begin(), tasks.size(), options);
}

AwaitableHandle AwaitableCorePool::whenAny(
    const AwaitableHandle* tasks,
    std::size_t count,
    WhenAnyOptions options) {
    if (!tasks || count == 0) {
        return fromFaulted(
            ErrorCode::InvalidArguments,
            "whenAny requires at least one child AwaitableHandle");
    }

    if (options.token.isPreCanceledSentinel() ||
        (cancellationPool_ && cancellationPool_->isCancellationRequested(options.token))) {
        for (std::size_t i = 0; i < count; ++i) {
            discard(tasks[i]);
        }
        return fromCanceled();
    }

    for (std::size_t i = 0; i < count; ++i) {
        if (!isValid(tasks[i])) {
            for (std::size_t j = 0; j < count; ++j) {
                if (j != i && canRegisterContinuation(tasks[j])) discard(tasks[j]);
            }
            return fromFaulted(
                ErrorCode::InvalidProgram,
                "whenAny received an invalid or already-consumed child AwaitableHandle");
        }
        if (!canRegisterContinuation(tasks[i])) {
            for (std::size_t j = 0; j < count; ++j) {
                if (j != i && canRegisterContinuation(tasks[j])) discard(tasks[j]);
            }
            return fromFaulted(
                ErrorCode::InvalidProgram,
                "whenAny child already has a registered awaiter");
        }
        if (!tasks[i].isCompletedVoidSentinel()) {
            for (std::size_t j = 0; j < i; ++j) {
                if (tasks[j] == tasks[i]) {
                    for (std::size_t k = 0; k < count; ++k) {
                        bool firstOccurrence = true;
                        for (std::size_t m = 0; m < k; ++m) {
                            if (tasks[m] == tasks[k]) {
                                firstOccurrence = false;
                                break;
                            }
                        }
                        if (firstOccurrence && canRegisterContinuation(tasks[k])) {
                            discard(tasks[k]);
                        }
                    }
                    return fromFaulted(
                        ErrorCode::InvalidProgram,
                        "whenAny received duplicate child AwaitableHandle");
                }
            }
        }
    }

    const AwaitableHandle parent = allocatePending(
        options.resumeQueue,
        CancellationToken::none());

    const std::uint32_t combIdx = allocateCombinatorSlot();
    combinators_[combIdx].kind = CombinatorKind::WhenAny;
    combinators_[combIdx].returnWinnerIndex = options.returnWinnerIndex;
    combinators_[combIdx].parentHandle = parent;
    combinators_[combIdx].cancelLosersSource = options.cancelLosersSource;
    combinators_[combIdx].totalChildren = count;
    combinators_[combIdx].remainingToComplete = 1;
    combinators_[combIdx].unconsumedCallbacks = count;

    if (cancellationPool_ && options.token.canBeCanceled()) {
        const CancellationRegistration reg = cancellationPool_->registerCallback(
            options.token,
            &AwaitableCorePool::onCombinatorCanceledCallback,
            this,
            static_cast<std::uint64_t>(combIdx));
        if (combinators_[combIdx].allocated && !combinators_[combIdx].resolved) {
            combinators_[combIdx].parentCancelReg = reg;
        }
    }

    for (std::size_t i = 0; i < count; ++i) {
        AsyncContinuation cont;
        cont.callback = &AwaitableCorePool::onCombinatorChildCompleted;
        cont.context = this;
        cont.userTag = packCombinatorTag(combIdx, i);
        cont.resumeQueue = options.resumeQueue;
        cont.runInlineOnComplete = true;
        (void)onCompleted(tasks[i], cont);
    }

    return parent;
}

AwaitableHandle AwaitableCorePool::whenAny(
    std::initializer_list<AwaitableHandle> tasks,
    WhenAnyOptions options) {
    return whenAny(tasks.begin(), tasks.size(), options);
}

bool AwaitableCorePool::decodeWhenAnyValue(
    const Value& value,
    std::size_t& outWinnerIndex,
    AwaitStatus& outWinnerStatus,
    Value& outWinnerValue) noexcept {
    const auto* structure = std::get_if<StructValue>(&value);
    if (!structure || structure->typeId != whenAnyResultTypeId() ||
        structure->fieldCount() < 3) {
        return false;
    }
    const auto idxField = structValueFieldAt(*structure, 0);
    const auto statusField = structValueFieldAt(*structure, 1);
    const auto valField = structValueFieldAt(*structure, 2);
    if (!idxField || !statusField || !valField) return false;
    const auto* idxInt = std::get_if<std::int64_t>(&*idxField);
    const auto* statusInt = std::get_if<std::int64_t>(&*statusField);
    if (!idxInt || !statusInt || *idxInt < 0) return false;

    outWinnerIndex = static_cast<std::size_t>(*idxInt);
    const auto rawStatus = static_cast<std::uint8_t>(*statusInt);
    outWinnerStatus = rawStatus <= static_cast<std::uint8_t>(AwaitStatus::Canceled)
        ? static_cast<AwaitStatus>(rawStatus)
        : AwaitStatus::Faulted;
    outWinnerValue = *valField;
    return true;
}

std::size_t AwaitableCorePool::drainReadyContinuations(
    ResumeQueue queue,
    std::size_t maxCount) {
    const auto queueIdx = static_cast<std::size_t>(queue);
    if (queueIdx >= kResumeQueueCount) return 0;
    auto& rq = readyQueues_[queueIdx];
    const std::size_t available = rq.size();
    if (available == 0) return 0;

    const std::size_t limit = maxCount == 0 ? available : std::min(maxCount, available);
    std::size_t executed = 0;
    while (executed < limit && rq.head < rq.items.size()) {
        ReadyContinuationItem item = rq.items[rq.head++];
        if (item.callback) {
            ++continuationsDispatched_;
            item.callback(
                item.context,
                item.completedHandle,
                item.terminalStatus,
                item.userTag);
        }
        releaseRootToken(item.pinnedTargetToken);
        ++executed;
    }
    if (rq.head >= rq.items.size()) {
        rq.items.clear();
        rq.head = 0;
    } else if (rq.head > 1024u && rq.head * 2u >= rq.items.size()) {
        rq.items.erase(
            rq.items.begin(),
            rq.items.begin() + static_cast<std::ptrdiff_t>(rq.head));
        rq.head = 0;
    }
    return executed;
}

std::size_t AwaitableCorePool::readyContinuationCount(
    ResumeQueue queue) const noexcept {
    const auto queueIdx = static_cast<std::size_t>(queue);
    if (queueIdx >= kResumeQueueCount) return 0;
    return readyQueues_[queueIdx].size();
}

void AwaitableCorePool::clear() noexcept {
    for (auto& rq : readyQueues_) {
        for (std::size_t i = rq.head; i < rq.items.size(); ++i) {
            releaseRootToken(rq.items[i].pinnedTargetToken);
        }
        rq.items.clear();
        rq.head = 0;
    }
    for (auto& comb : combinators_) {
        releaseCombinatorValues(comb);
    }
    combinators_.clear();
    freeCombinators_.clear();

    for (std::uint32_t i = 0; i < nextNeverUsedSlot_; ++i) {
        auto& slot = slotAt(i);
        releaseRootToken(slot.resultRootToken);
        releaseRootToken(slot.continuationTargetRootToken);
        if (slot.cold) {
            releaseRootToken(slot.cold->exceptionRootToken);
        }
    }
    pages_.clear();
    slotCapacity_ = 0;
    nextNeverUsedSlot_ = 0;
    freeSlots_.clear();
    activeGcRoots_ = 0;
    totalAllocated_ = 0;
    totalRecycled_ = 0;
    synchronousFastPathHits_ = 0;
    continuationsDispatched_ = 0;
}

AwaitablePoolStatistics AwaitableCorePool::statistics() const noexcept {
    AwaitablePoolStatistics stats;
    stats.capacity = slotCapacity_;
    stats.freeListCount = (slotCapacity_ - nextNeverUsedSlot_) + freeSlots_.size();
    stats.activeCount = nextNeverUsedSlot_ >= freeSlots_.size()
        ? (nextNeverUsedSlot_ - freeSlots_.size())
        : 0;
    stats.activeGcRoots = activeGcRoots_;
    stats.activeCombinators = combinators_.size() >= freeCombinators_.size()
        ? (combinators_.size() - freeCombinators_.size())
        : 0;
    stats.totalAllocated = totalAllocated_;
    stats.totalRecycled = totalRecycled_;
    stats.synchronousFastPathHits = synchronousFastPathHits_;
    stats.continuationsDispatched = continuationsDispatched_;
    return stats;
}

// ============================================================================
// AsyncCompletionQueue
// ============================================================================

AsyncRequestId AsyncCompletionQueue::registerRequest(
    AwaitableHandle target,
    std::string operationName,
    std::uint64_t argumentHash,
    BindingDeterminism determinism,
    CancellationToken token) {
    const AsyncRequestId id = nextRequestId_++;
    PendingRequestEntry entry;
    entry.requestId = id;
    entry.target = target;
    entry.operationName = std::move(operationName);
    entry.argumentHash = argumentHash;
    entry.determinism = determinism;
    entry.token = token;
    pendingRequests_.emplace(id, std::move(entry));
    return id;
}

bool AsyncCompletionQueue::cancelRequest(AsyncRequestId requestId) noexcept {
    return pendingRequests_.erase(requestId) > 0;
}

std::size_t AsyncCompletionQueue::pruneStaleRequests(
    const AwaitableCorePool& pool) noexcept {
    std::size_t pruned = 0;
    for (auto it = pendingRequests_.begin(); it != pendingRequests_.end();) {
        if (!pool.isValid(it->second.target) || pool.isCompleted(it->second.target)) {
            it = pendingRequests_.erase(it);
            ++pruned;
        } else {
            ++it;
        }
    }
    return pruned;
}

void AsyncCompletionQueue::submitVoid(AsyncRequestId requestId) {
    AsyncHostPacket pkt;
    pkt.requestId = requestId;
    pkt.kind = AsyncPayloadKind::Void;
    std::lock_guard<std::mutex> lock(inboxMutex_);
    inbox_.push_back(std::move(pkt));
}

void AsyncCompletionQueue::submitScalar(
    AsyncRequestId requestId,
    Value scalarOrInlineStruct) {
    AsyncHostPacket pkt;
    pkt.requestId = requestId;
    if (!isReplayStableValue(scalarOrInlineStruct)) {
        pkt.kind = AsyncPayloadKind::Faulted;
        pkt.errorCode = ErrorCode::InvalidObjectReference;
        pkt.errorMessage =
            "background worker attempted to submit a ManagedHeap or NativeHandle reference";
    } else {
        pkt.kind = AsyncPayloadKind::Scalar;
        pkt.scalarValue = std::move(scalarOrInlineStruct);
    }
    std::lock_guard<std::mutex> lock(inboxMutex_);
    inbox_.push_back(std::move(pkt));
}

void AsyncCompletionQueue::submitString(
    AsyncRequestId requestId,
    std::string utf8String) {
    AsyncHostPacket pkt;
    pkt.requestId = requestId;
    pkt.kind = AsyncPayloadKind::Utf8String;
    pkt.stringPayload = std::move(utf8String);
    std::lock_guard<std::mutex> lock(inboxMutex_);
    inbox_.push_back(std::move(pkt));
}

void AsyncCompletionQueue::submitBytes(
    AsyncRequestId requestId,
    std::vector<std::uint8_t> bytes) {
    AsyncHostPacket pkt;
    pkt.requestId = requestId;
    pkt.kind = AsyncPayloadKind::ByteBuffer;
    pkt.bytesPayload = std::move(bytes);
    std::lock_guard<std::mutex> lock(inboxMutex_);
    inbox_.push_back(std::move(pkt));
}

void AsyncCompletionQueue::submitFailure(
    AsyncRequestId requestId,
    ErrorCode code,
    std::string message) {
    AsyncHostPacket pkt;
    pkt.requestId = requestId;
    pkt.kind = AsyncPayloadKind::Faulted;
    pkt.errorCode = code == ErrorCode::None ? ErrorCode::ScriptException : code;
    pkt.errorMessage = std::move(message);
    std::lock_guard<std::mutex> lock(inboxMutex_);
    inbox_.push_back(std::move(pkt));
}

void AsyncCompletionQueue::submitCanceled(AsyncRequestId requestId) {
    AsyncHostPacket pkt;
    pkt.requestId = requestId;
    pkt.kind = AsyncPayloadKind::Canceled;
    std::lock_guard<std::mutex> lock(inboxMutex_);
    inbox_.push_back(std::move(pkt));
}

std::size_t AsyncCompletionQueue::queuedBackgroundPacketCount() const noexcept {
    std::lock_guard<std::mutex> lock(inboxMutex_);
    return inbox_.size();
}

void AsyncCompletionQueue::reset() noexcept {
    {
        std::lock_guard<std::mutex> lock(inboxMutex_);
        inbox_.clear();
        swapBuffer_.clear();
    }
    pendingRequests_.clear();
    nextRequestId_ = 1;
    drainTickSequence_ = 0;
}

bool AsyncCompletionQueue::applyPacketOnMainThread(
    const PendingRequestEntry& request,
    AsyncHostPacket& packet,
    AwaitableCorePool& pool,
    ManagedHeap* heap,
    AsyncDrainResult& stats,
    RuntimeError& outError) {
    if (!pool.isValid(request.target) || pool.isCompleted(request.target)) {
        ++stats.staleOrCanceledCount;
        return true;
    }

    switch (packet.kind) {
    case AsyncPayloadKind::Void: {
        if (pool.trySetResult(request.target, Value{})) {
            ++stats.completedCount;
        } else {
            ++stats.staleOrCanceledCount;
        }
        return true;
    }
    case AsyncPayloadKind::Scalar: {
        if (pool.trySetResult(request.target, std::move(packet.scalarValue))) {
            ++stats.completedCount;
        } else {
            ++stats.staleOrCanceledCount;
        }
        return true;
    }
    case AsyncPayloadKind::Utf8String: {
        if (heap) {
            RuntimeError allocErr;
            const auto strRef = heap->allocateString(std::move(packet.stringPayload), &allocErr);
            if (!strRef) {
                outError = std::move(allocErr);
                if (outError.code == ErrorCode::None) {
                    outError.code = ErrorCode::OutOfMemory;
                }
                pool.trySetFaulted(request.target, outError.code, outError.message);
                return false;
            }
            if (pool.trySetResult(request.target, Value(*strRef))) {
                ++stats.completedCount;
            } else {
                ++stats.staleOrCanceledCount;
            }
        } else {
            if (pool.trySetResult(request.target, Value(std::move(packet.stringPayload)))) {
                ++stats.completedCount;
            } else {
                ++stats.staleOrCanceledCount;
            }
        }
        return true;
    }
    case AsyncPayloadKind::ByteBuffer: {
        if (!heap) {
            std::string encoded(
                reinterpret_cast<const char*>(packet.bytesPayload.data()),
                packet.bytesPayload.size());
            if (pool.trySetResult(request.target, Value(std::move(encoded)))) {
                ++stats.completedCount;
            } else {
                ++stats.staleOrCanceledCount;
            }
            return true;
        }
        RuntimeError allocErr;
        const auto arrayRef = heap->allocateTypedArray(
            0,
            semantic::PrimitiveType::Int,
            0,
            packet.bytesPayload.size(),
            Value(std::int64_t{0}),
            &allocErr);
        if (!arrayRef) {
            outError = std::move(allocErr);
            if (outError.code == ErrorCode::None) {
                outError.code = ErrorCode::OutOfMemory;
            }
            pool.trySetFaulted(request.target, outError.code, outError.message);
            return false;
        }
        ArrayFastView fastView{};
        if (heap->tryBindArrayFastView(*arrayRef, fastView) && fastView.data) {
            for (std::size_t i = 0; i < packet.bytesPayload.size(); ++i) {
                fastView.data[i] = Value(static_cast<std::int64_t>(packet.bytesPayload[i]));
            }
        } else {
            for (std::size_t i = 0; i < packet.bytesPayload.size(); ++i) {
                heap->arraySet(
                    *arrayRef,
                    i,
                    Value(static_cast<std::int64_t>(packet.bytesPayload[i])));
            }
        }
        if (pool.trySetResult(request.target, Value(*arrayRef))) {
            ++stats.completedCount;
        } else {
            ++stats.staleOrCanceledCount;
        }
        return true;
    }
    case AsyncPayloadKind::Faulted: {
        if (pool.trySetFaulted(
                request.target,
                packet.errorCode,
                std::move(packet.errorMessage))) {
            ++stats.completedCount;
        } else {
            ++stats.staleOrCanceledCount;
        }
        return true;
    }
    case AsyncPayloadKind::Canceled: {
        if (pool.trySetCanceled(request.target)) {
            ++stats.completedCount;
        } else {
            ++stats.staleOrCanceledCount;
        }
        return true;
    }
    }
    return true;
}

AsyncDrainResult AsyncCompletionQueue::drainToMainThread(
    AwaitableCorePool& pool,
    ManagedHeap* heap,
    DeterminismSession* determinism,
    RuntimeError& outError) {
    AsyncDrainResult result{};
    const DeterminismMode mode = determinism ? determinism->mode() : DeterminismMode::Off;
    const std::uint64_t currentTick = drainTickSequence_++;

    if (mode == DeterminismMode::Strict) {
        const PendingRequestEntry* earliestViolation = nullptr;
        for (const auto& [reqId, pending] : pendingRequests_) {
            (void)reqId;
            if (pending.determinism != BindingDeterminism::Deterministic) {
                if (!earliestViolation || pending.requestId < earliestViolation->requestId) {
                    earliestViolation = &pending;
                }
            }
        }
        if (earliestViolation) {
            result.succeeded = false;
            outError.code = ErrorCode::DeterminismViolation;
            outError.message = "async external operation '" + earliestViolation->operationName +
                "' is not allowed in strict deterministic mode";
            return result;
        }
    }

    if (mode == DeterminismMode::Replay) {
        std::size_t batchCount = 0;
        if (!determinism->replayAsyncDrainBatch(currentTick, batchCount, outError)) {
            result.succeeded = false;
            return result;
        }

        for (std::size_t i = 0; i < batchCount; ++i) {
            ExternalCallRecord record;
            if (!determinism->consumeAsyncCompletion(record, outError)) {
                result.succeeded = false;
                return result;
            }
            if (!hasAsyncRecordMagic(record.symbolId)) {
                result.succeeded = false;
                outError.code = ErrorCode::ReplayMismatch;
                outError.message = "replay entry symbolId is not an async completion record for '" +
                    record.name + "'";
                return result;
            }

            const AsyncRequestId reqId = record.argumentHash;
            const auto found = pendingRequests_.find(reqId);
            if (found == pendingRequests_.end()) {
                result.succeeded = false;
                outError.code = ErrorCode::ReplayMismatch;
                outError.message = "replay async completion requestId " +
                    std::to_string(reqId) + " ('" + record.name + "') is not pending";
                return result;
            }

            const PendingRequestEntry pending = std::move(found->second);
            pendingRequests_.erase(found);

            const AsyncPayloadKind kind = extractAsyncRecordKind(record.symbolId);
            const semantic::SymbolId expectedSym = makeAsyncRecordSymbolId(
                kind, pending.argumentHash);
            if (pending.operationName != record.name || expectedSym != record.symbolId) {
                result.succeeded = false;
                outError.code = ErrorCode::ReplayMismatch;
                outError.message = "replay async completion metadata mismatch for '" +
                    pending.operationName + "'";
                return result;
            }

            AsyncHostPacket replayed;
            replayed.requestId = reqId;
            replayed.kind = kind;
            switch (kind) {
            case AsyncPayloadKind::Void:
                break;
            case AsyncPayloadKind::Scalar:
                replayed.scalarValue = std::move(record.result);
                break;
            case AsyncPayloadKind::Utf8String: {
                if (const auto* s = std::get_if<StringValue>(&record.result)) {
                    replayed.stringPayload = s->str();
                } else {
                    result.succeeded = false;
                    outError.code = ErrorCode::ReplayMismatch;
                    outError.message = "replay async string payload corrupted for '" +
                        pending.operationName + "'";
                    return result;
                }
                break;
            }
            case AsyncPayloadKind::ByteBuffer: {
                if (const auto* s = std::get_if<StringValue>(&record.result)) {
                    const std::string& raw = s->str();
                    replayed.bytesPayload.assign(
                        reinterpret_cast<const std::uint8_t*>(raw.data()),
                        reinterpret_cast<const std::uint8_t*>(raw.data()) + raw.size());
                } else {
                    result.succeeded = false;
                    outError.code = ErrorCode::ReplayMismatch;
                    outError.message = "replay async byte-buffer payload corrupted for '" +
                        pending.operationName + "'";
                    return result;
                }
                break;
            }
            case AsyncPayloadKind::Faulted:
                replayed.errorCode = record.errorCode;
                replayed.errorMessage = std::move(record.errorMessage);
                break;
            case AsyncPayloadKind::Canceled:
                break;
            }

            if (!applyPacketOnMainThread(
                    pending, replayed, pool, heap, result, outError)) {
                result.succeeded = false;
                return result;
            }
        }

        // Discard any duplicate packets submitted by deterministic helpers during replay
        {
            std::lock_guard<std::mutex> lock(inboxMutex_);
            inbox_.clear();
        }
        result.staleOrCanceledCount += pruneStaleRequests(pool);
        return result;
    }

    swapBuffer_.clear();
    {
        std::lock_guard<std::mutex> lock(inboxMutex_);
        if (!inbox_.empty()) {
            swapBuffer_.swap(inbox_);
        }
    }

    if (swapBuffer_.size() > 1) {
        std::sort(
            swapBuffer_.begin(),
            swapBuffer_.end(),
            [](const AsyncHostPacket& a, const AsyncHostPacket& b) noexcept {
                return a.requestId < b.requestId;
            });
    }

    struct MatchedItem {
        PendingRequestEntry request;
        AsyncHostPacket packet;
    };
    std::vector<MatchedItem> matched;
    matched.reserve(swapBuffer_.size());

    for (auto& pkt : swapBuffer_) {
        const auto found = pendingRequests_.find(pkt.requestId);
        if (found == pendingRequests_.end()) {
            ++result.staleOrCanceledCount;
            continue;
        }
        if (mode == DeterminismMode::Strict &&
            found->second.determinism != BindingDeterminism::Deterministic) {
            result.succeeded = false;
            outError.code = ErrorCode::DeterminismViolation;
            outError.message = "async external operation '" + found->second.operationName +
                "' is not allowed in strict deterministic mode";
            return result;
        }
        PendingRequestEntry req = std::move(found->second);
        pendingRequests_.erase(found);
        matched.push_back(MatchedItem{std::move(req), std::move(pkt)});
    }

    if (mode == DeterminismMode::Record) {
        if (!determinism->recordAsyncDrainBatch(currentTick, matched.size(), outError)) {
            result.succeeded = false;
            return result;
        }

        for (const auto& item : matched) {
            ExternalCallRecord record;
            record.symbolId = makeAsyncRecordSymbolId(
                item.packet.kind, item.request.argumentHash);
            record.name = item.request.operationName;
            record.argumentHash = item.request.requestId;
            switch (item.packet.kind) {
            case AsyncPayloadKind::Void:
                record.succeeded = true;
                record.result = Value{};
                break;
            case AsyncPayloadKind::Scalar:
                record.succeeded = true;
                record.result = item.packet.scalarValue;
                break;
            case AsyncPayloadKind::Utf8String:
                record.succeeded = true;
                record.result = Value(item.packet.stringPayload);
                break;
            case AsyncPayloadKind::ByteBuffer: {
                record.succeeded = true;
                std::string rawBytes(
                    reinterpret_cast<const char*>(item.packet.bytesPayload.data()),
                    item.packet.bytesPayload.size());
                record.result = Value(std::move(rawBytes));
                break;
            }
            case AsyncPayloadKind::Faulted:
                record.succeeded = false;
                record.errorCode = item.packet.errorCode;
                record.errorMessage = item.packet.errorMessage;
                break;
            case AsyncPayloadKind::Canceled:
                record.succeeded = false;
                record.errorCode = ErrorCode::ExecutionTerminated;
                record.errorMessage = "async operation was canceled";
                break;
            }

            if (!determinism->recordAsyncCompletion(std::move(record), outError)) {
                result.succeeded = false;
                return result;
            }
        }
    }

    for (auto& item : matched) {
        if (!applyPacketOnMainThread(
                item.request, item.packet, pool, heap, result, outError)) {
            result.succeeded = false;
            return result;
        }
    }

    result.staleOrCanceledCount += pruneStaleRequests(pool);
    return result;
}

// ============================================================================
// CoroutineFramePool & RealScript.Async Runtime Bridge
// ============================================================================

CoroutineFramePool::CoroutineFramePool(
    ManagedHeap* heap,
    AsyncRuntimeContext* ownerContext,
    std::size_t initialReserve)
    : heap_(heap), ownerContext_(ownerContext) {
    freeSlots_.reserve(initialReserve);
}

void CoroutineFramePool::setHeap(ManagedHeap* heap) noexcept {
    if (heap_ == heap) return;
    if (heap_) {
        for (auto& slot : slots_) {
            releasePinnedRoots(slot);
        }
    }
    activeGcRoots_ = 0;
    heap_ = heap;
    if (heap_) {
        for (auto& slot : slots_) {
            if (!slot.allocated || !slot.frame.suspended) continue;
            auto pinRange = [&](const std::vector<Value>& vec) {
                for (const auto& v : vec) {
                    if (valueNeedsManagedHeapRoot(v)) {
                        const auto tok = heap_->addPersistentRoot(v);
                        if (tok != 0) {
                            ++activeGcRoots_;
                            slot.pinnedRootTokens.push_back(tok);
                        }
                    }
                }
            };
            pinRange(slot.frame.savedArguments);
            pinRange(slot.frame.savedLocals);
            pinRange(slot.frame.savedRegisters);
        }
    }
}

CoroutineFrameHandle CoroutineFramePool::allocateFrame(
    semantic::SymbolId functionSymbolId,
    semantic::SymbolId returnStructTypeId,
    AwaitableHandle completionHandle,
    ResumeQueue resumeQueue) {
    std::uint32_t idx = 0;
    if (!freeSlots_.empty()) {
        idx = freeSlots_.back();
        freeSlots_.pop_back();
    } else {
        idx = static_cast<std::uint32_t>(slots_.size());
        slots_.emplace_back();
    }
    auto& slot = slots_[idx];
    slot.allocated = true;
    slot.frame.functionSymbolId = functionSymbolId;
    slot.frame.returnStructTypeId = returnStructTypeId;
    slot.frame.completionHandle = completionHandle;
    slot.frame.awaitedHandle = AwaitableHandle::invalid();
    slot.frame.resumeBlockId = 0;
    slot.frame.resumeQueue = resumeQueue;
    slot.frame.suspended = false;
    slot.frame.completed = false;
    slot.frame.usesScalarBuffer = false;
    slot.frame.scalarByteSize = 0;
    slot.frame.resumeCallback = nullptr;
    slot.frame.runnerContext = nullptr;
    slot.frame.runnerOwner.reset();
    slot.frame.overflowScalarBytes.clear();
    slot.frame.savedArguments.clear();
    slot.frame.savedLocals.clear();
    slot.frame.savedRegisters.clear();
    ++totalAllocated_;
    return CoroutineFrameHandle{idx, slot.version};
}

CoroutineFrame* CoroutineFramePool::resolve(CoroutineFrameHandle handle) noexcept {
    if (!handle.valid() || handle.slot >= slots_.size()) return nullptr;
    auto& slot = slots_[handle.slot];
    if (!slot.allocated || slot.version != handle.version) return nullptr;
    return &slot.frame;
}

const CoroutineFrame* CoroutineFramePool::resolve(CoroutineFrameHandle handle) const noexcept {
    if (!handle.valid() || handle.slot >= slots_.size()) return nullptr;
    const auto& slot = slots_[handle.slot];
    if (!slot.allocated || slot.version != handle.version) return nullptr;
    return &slot.frame;
}

void CoroutineFramePool::releasePinnedRoots(FrameSlot& slot) noexcept {
    for (auto& tok : slot.pinnedRootTokens) {
        if (tok != 0) {
            if (heap_) {
                (void)heap_->removePersistentRoot(tok);
            }
            tok = 0;
            if (activeGcRoots_ > 0) --activeGcRoots_;
        }
    }
    slot.pinnedRootTokens.clear();
}

void CoroutineFramePool::snapshotAndPinValues(
    FrameSlot& slot,
    const Value* args,
    std::size_t argCount,
    const Value* locals,
    std::size_t localCount,
    const Value* regs,
    std::size_t regCount) {
    releasePinnedRoots(slot);

    slot.frame.savedArguments.assign(args, args + argCount);
    slot.frame.savedLocals.assign(locals, locals + localCount);
    slot.frame.savedRegisters.assign(regs, regs + regCount);

    if (!heap_) return;
    auto pinRange = [&](const std::vector<Value>& vec) {
        for (const auto& v : vec) {
            if (valueNeedsManagedHeapRoot(v)) {
                const auto tok = heap_->addPersistentRoot(v);
                if (tok != 0) {
                    ++activeGcRoots_;
                    slot.pinnedRootTokens.push_back(tok);
                }
            }
        }
    };
    pinRange(slot.frame.savedArguments);
    pinRange(slot.frame.savedLocals);
    pinRange(slot.frame.savedRegisters);
}

bool CoroutineFramePool::suspendFrame(
    CoroutineFrameHandle handle,
    const Value* args,
    std::size_t argCount,
    const Value* locals,
    std::size_t localCount,
    const Value* regs,
    std::size_t regCount,
    std::uint32_t resumeBlockId,
    AwaitableHandle awaitedHandle,
    CoroutineResumeCallback resumeCallback,
    void* runnerContext,
    std::shared_ptr<const void> runnerOwner) {
    if (!ownerContext_ || !resumeCallback) return false;
    if (!handle.valid() || handle.slot >= slots_.size()) return false;
    auto& slot = slots_[handle.slot];
    if (!slot.allocated || slot.version != handle.version) return false;

    snapshotAndPinValues(slot, args, argCount, locals, localCount, regs, regCount);
    slot.frame.usesScalarBuffer = false;
    slot.frame.scalarByteSize = 0;
    slot.frame.resumeBlockId = resumeBlockId;
    slot.frame.awaitedHandle = awaitedHandle;
    slot.frame.suspended = true;
    slot.frame.completed = false;
    slot.frame.resumeCallback = resumeCallback;
    slot.frame.runnerContext = runnerContext;
    if (runnerOwner) {
        slot.frame.runnerOwner = std::move(runnerOwner);
    }

    AsyncContinuation cont;
    cont.callback = &CoroutineFramePool::onAwaitedCompletedCallback;
    cont.context = this;
    cont.userTag = handle.pack64();
    cont.resumeQueue = slot.frame.resumeQueue;
    cont.runInlineOnComplete = false;
    if (!ownerContext_->awaitables().onCompleted(awaitedHandle, cont)) {
        slot.frame.suspended = false;
        releasePinnedRoots(slot);
        return false;
    }

    ++totalSuspensions_;
    return true;
}

bool CoroutineFramePool::suspendScalarFrame(
    CoroutineFrameHandle handle,
    const void* scalarData,
    std::size_t scalarByteSize,
    const Value* refSlots,
    std::size_t refCount,
    std::uint32_t resumeBlockId,
    AwaitableHandle awaitedHandle,
    CoroutineResumeCallback resumeCallback,
    void* runnerContext,
    std::shared_ptr<const void> runnerOwner) {
    if (!ownerContext_ || !resumeCallback) return false;
    if (!handle.valid() || handle.slot >= slots_.size()) return false;
    auto& slot = slots_[handle.slot];
    if (!slot.allocated || slot.version != handle.version) return false;

    releasePinnedRoots(slot);
    if (!slot.frame.savedArguments.empty()) slot.frame.savedArguments.clear();
    if (!slot.frame.savedRegisters.empty()) slot.frame.savedRegisters.clear();

    slot.frame.usesScalarBuffer = true;
    slot.frame.scalarByteSize = static_cast<std::uint32_t>(scalarByteSize);
    if (scalarByteSize > 0 && scalarData != nullptr) {
        if (scalarByteSize <= CoroutineFrame::kInlineScalarBytes) {
            std::memcpy(slot.frame.inlineScalarBytes.data(), scalarData, scalarByteSize);
        } else {
            slot.frame.overflowScalarBytes.resize(scalarByteSize);
            std::memcpy(slot.frame.overflowScalarBytes.data(), scalarData, scalarByteSize);
        }
    }

    if (refCount > 0 && refSlots != nullptr) {
        slot.frame.savedLocals.assign(refSlots, refSlots + refCount);
        if (heap_) {
            for (const auto& v : slot.frame.savedLocals) {
                if (valueNeedsManagedHeapRoot(v)) {
                    const auto tok = heap_->addPersistentRoot(v);
                    if (tok != 0) {
                        ++activeGcRoots_;
                        slot.pinnedRootTokens.push_back(tok);
                    }
                }
            }
        }
    } else if (!slot.frame.savedLocals.empty()) {
        slot.frame.savedLocals.clear();
    }

    slot.frame.resumeBlockId = resumeBlockId;
    slot.frame.awaitedHandle = awaitedHandle;
    slot.frame.suspended = true;
    slot.frame.completed = false;
    slot.frame.resumeCallback = resumeCallback;
    slot.frame.runnerContext = runnerContext;
    if (runnerOwner) {
        slot.frame.runnerOwner = std::move(runnerOwner);
    }

    AsyncContinuation cont;
    cont.callback = &CoroutineFramePool::onAwaitedCompletedCallback;
    cont.context = this;
    cont.userTag = handle.pack64();
    cont.resumeQueue = slot.frame.resumeQueue;
    cont.runInlineOnComplete = false;
    if (!ownerContext_->awaitables().onCompleted(awaitedHandle, cont)) {
        slot.frame.suspended = false;
        releasePinnedRoots(slot);
        return false;
    }

    ++totalSuspensions_;
    return true;
}

void CoroutineFramePool::onAwaitedCompletedCallback(
    void* context,
    AwaitableHandle /*completedHandle*/,
    AwaitStatus /*terminalStatus*/,
    std::uint64_t packedFrameHandle) {
    auto* self = static_cast<CoroutineFramePool*>(context);
    if (!self || !self->ownerContext_) return;
    const auto handle = CoroutineFrameHandle::unpack64(packedFrameHandle);
    if (!handle.valid() || handle.slot >= self->slots_.size()) return;
    auto& slot = self->slots_[handle.slot];
    if (!slot.allocated || slot.version != handle.version) return;

    slot.frame.suspended = false;
    ++self->totalResumptions_;
    const CoroutineResumeCallback cb = slot.frame.resumeCallback;
    void* const runnerCtx = slot.frame.runnerContext;
    if (cb) {
        cb(runnerCtx, handle, slot.frame, *self->ownerContext_);
    }

    // Re-check slot validity after callback in case the coroutine re-suspended or completed.
    if (handle.slot < self->slots_.size()) {
        auto& postSlot = self->slots_[handle.slot];
        if (postSlot.allocated && postSlot.version == handle.version) {
            if (!postSlot.frame.completed && !postSlot.frame.suspended) {
                if (postSlot.frame.completionHandle.valid() &&
                    self->ownerContext_->awaitables().status(postSlot.frame.completionHandle) ==
                        AwaitStatus::Pending) {
                    (void)self->ownerContext_->awaitables().trySetFaulted(
                        postSlot.frame.completionHandle,
                        ErrorCode::InvalidProgram,
                        "resumed coroutine terminated without resolving completion handle");
                }
            }
            if (postSlot.frame.completed || !postSlot.frame.suspended) {
                self->recycleFrame(handle);
            }
        }
    }
}

void CoroutineFramePool::recycleFrame(CoroutineFrameHandle handle) noexcept {
    if (!handle.valid() || handle.slot >= slots_.size()) return;
    auto& slot = slots_[handle.slot];
    if (!slot.allocated || slot.version != handle.version) return;

    releasePinnedRoots(slot);
    slot.frame.usesScalarBuffer = false;
    slot.frame.scalarByteSize = 0;
    slot.frame.overflowScalarBytes.clear();
    slot.frame.savedArguments.clear();
    slot.frame.savedLocals.clear();
    slot.frame.savedRegisters.clear();
    slot.frame.runnerOwner.reset();
    slot.frame.resumeCallback = nullptr;
    slot.frame.runnerContext = nullptr;
    slot.frame.suspended = false;
    slot.frame.completed = false;
    slot.allocated = false;
    slot.version = nextNonZeroVersion(slot.version);
    freeSlots_.push_back(handle.slot);
    ++totalRecycled_;
}

std::size_t CoroutineFramePool::abortAllSuspendedFrames() {
    if (slots_.empty()) return 0;
    std::vector<CoroutineFrameHandle> activeHandles;
    activeHandles.reserve(slots_.size());
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(slots_.size()); ++i) {
        if (slots_[i].allocated) {
            activeHandles.push_back(CoroutineFrameHandle{i, slots_[i].version});
        }
    }
    if (activeHandles.empty()) return 0;

    if (ownerContext_) {
        (void)ownerContext_->awaitables().cancelContinuationsForContext(this);
    }

    std::size_t aborted = 0;
    for (const auto handle : activeHandles) {
        if (handle.slot >= slots_.size()) continue;
        auto& slot = slots_[handle.slot];
        if (!slot.allocated || slot.version != handle.version) continue;

        const AwaitableHandle completion = slot.frame.completionHandle;
        const AwaitableHandle awaited = slot.frame.awaitedHandle;
        recycleFrame(handle);
        ++aborted;

        if (ownerContext_) {
            if (awaited.valid() && !awaited.isCompletedVoidSentinel()) {
                (void)ownerContext_->awaitables().discard(awaited);
            }
            if (completion.valid() && !completion.isCompletedVoidSentinel()) {
                (void)ownerContext_->awaitables().trySetCanceled(completion);
            }
        }
    }
    return aborted;
}

void CoroutineFramePool::clear() noexcept {
    for (auto& slot : slots_) {
        if (slot.allocated) {
            releasePinnedRoots(slot);
            slot.frame.runnerOwner.reset();
            slot.allocated = false;
            slot.version = nextNonZeroVersion(slot.version);
        }
    }
    slots_.clear();
    freeSlots_.clear();
    activeGcRoots_ = 0;
    totalAllocated_ = 0;
    totalRecycled_ = 0;
    totalSuspensions_ = 0;
    totalResumptions_ = 0;
}

CoroutinePoolStatistics CoroutineFramePool::statistics() const noexcept {
    CoroutinePoolStatistics stats;
    stats.capacity = slots_.size();
    stats.freeListCount = freeSlots_.size();
    stats.activeCount = slots_.size() >= freeSlots_.size()
        ? (slots_.size() - freeSlots_.size())
        : 0;
    stats.activeGcRoots = activeGcRoots_;
    stats.totalAllocated = totalAllocated_;
    stats.totalRecycled = totalRecycled_;
    stats.totalSuspensions = totalSuspensions_;
    stats.totalResumptions = totalResumptions_;
    return stats;
}

namespace {

bool tryExtractPackedInt64(const Value& v, std::uint64_t& outPacked) noexcept {
    if (const auto* lv = std::get_if<LongValue>(&v)) {
        outPacked = static_cast<std::uint64_t>(lv->value);
        return true;
    }
    if (const auto* ulv = std::get_if<ULongValue>(&v)) {
        outPacked = ulv->value;
        return true;
    }
    if (const auto* iv = std::get_if<std::int64_t>(&v)) {
        outPacked = static_cast<std::uint64_t>(*iv);
        return true;
    }
    return false;
}

} // namespace

Value makeAwaitableStructValue(
    semantic::SymbolId structTypeId,
    AwaitableHandle handle) {
    const semantic::SymbolId tid = structTypeId != 0
        ? structTypeId
        : awaitableVoidTypeId();
    const std::array<Value, 1> fields = {
        Value(LongValue{static_cast<std::int64_t>(handle.pack64())}),
    };
    return Value(makeStructValueFromFields(tid, fields));
}

bool tryExtractAwaitableHandle(
    const Value& value,
    AwaitableHandle& outHandle) noexcept {
    const auto* st = std::get_if<StructValue>(&value);
    if (!st || st->fieldCount() < 1) return false;
    const auto field0 = structValueFieldAt(*st, 0);
    if (!field0) return false;
    std::uint64_t packed = 0;
    if (!tryExtractPackedInt64(*field0, packed)) return false;
    outHandle = AwaitableHandle::unpack64(packed);
    return true;
}

std::optional<ObjectRef> materializeAsyncResumeException(
    ManagedHeap* heap,
    AwaitStatus terminalStatus,
    const RuntimeError& faultError,
    const Value& storedExceptionObject,
    semantic::SymbolId fallbackExceptionTypeId) {
    if (const auto* existingObj = std::get_if<ObjectRef>(&storedExceptionObject)) {
        if (existingObj->valid() && (!heap || heap->isAlive(*existingObj))) {
            return *existingObj;
        }
    }
    if (!heap) return std::nullopt;

    const std::string msg = !faultError.message.empty()
        ? faultError.message
        : (terminalStatus == AwaitStatus::Canceled
               ? std::string("async operation was canceled")
               : std::string("async operation faulted"));

    RuntimeError allocErr;
    const auto strRef = heap->allocateString(msg, &allocErr);
    if (!strRef) return std::nullopt;
    PersistentRoot strPin = heap->retain(Value(*strRef));

    const semantic::SymbolId typeId = (terminalStatus == AwaitStatus::Canceled)
        ? asyncCanceledExceptionTypeId()
        : (fallbackExceptionTypeId != 0
               ? fallbackExceptionTypeId
               : asyncExceptionTypeId());

    return heap->allocateObject(
        typeId,
        std::vector<Value>{Value(*strRef)},
        std::vector<std::size_t>{0u},
        &allocErr);
}

bool isAsyncCanceledExceptionObject(
    const ManagedHeap* heap,
    const Value& excValue) noexcept {
    if (!heap) return false;
    const auto* objRef = std::get_if<ObjectRef>(&excValue);
    if (!objRef || !objRef->valid()) return false;
    const auto tid = heap->objectTypeId(*objRef);
    return tid.has_value() && *tid == asyncCanceledExceptionTypeId();
}

bool tryExecuteAsyncIntrinsic(
    std::string_view intrinsicName,
    ValueSpan arguments,
    AsyncRuntimeContext* asyncContext,
    ManagedHeap* heap,
    Value& result,
    std::string& outError) {
    constexpr std::string_view kPrefix = "Async.__";
    if (intrinsicName.rfind(kPrefix, 0) != 0) return false;
    if (!asyncContext) {
        outError = "Async intrinsic requires an active AsyncRuntimeContext";
        return false;
    }
    const std::string_view op = intrinsicName.substr(kPrefix.size());
    auto& pool = asyncContext->awaitables();
    auto& ctsPool = asyncContext->cancellations();

    if (op == "CompletedVoid") {
        result = Value(LongValue{
            static_cast<std::int64_t>(AwaitableHandle::completedVoid().pack64())});
        return true;
    }
    if (op == "FromCanceled") {
        const AwaitableHandle h = pool.fromCanceled();
        result = Value(LongValue{static_cast<std::int64_t>(h.pack64())});
        return true;
    }
    if (op == "FromFaulted") {
        std::string msg = "async operation faulted";
        if (!arguments.empty() && !std::holds_alternative<NullString>(arguments[0])) {
            msg = valueToString(arguments[0], heap);
        }
        const AwaitableHandle h = pool.fromFaulted(ErrorCode::ScriptException, std::move(msg));
        result = Value(LongValue{static_cast<std::int64_t>(h.pack64())});
        return true;
    }
    if (op == "Yield") {
        std::uint64_t packedTok = 0;
        if (!arguments.empty()) {
            (void)tryExtractPackedInt64(arguments[0], packedTok);
        }
        const AwaitableHandle h = asyncContext->enqueueYield(
            CancellationToken::unpack64(packedTok));
        result = Value(LongValue{static_cast<std::int64_t>(h.pack64())});
        return true;
    }
    if (op == "NewPending") {
        std::uint64_t packedTok = 0;
        if (!arguments.empty()) {
            (void)tryExtractPackedInt64(arguments[0], packedTok);
        }
        const AwaitableHandle h = pool.allocatePending(
            ResumeQueue::Update, CancellationToken::unpack64(packedTok));
        result = Value(LongValue{static_cast<std::int64_t>(h.pack64())});
        return true;
    }
    if (op == "CompleteVoid") {
        std::uint64_t packed = 0;
        if (arguments.empty() || !tryExtractPackedInt64(arguments[0], packed)) {
            result = Value(false);
            return true;
        }
        result = Value(pool.trySetResult(AwaitableHandle::unpack64(packed), Value{}));
        return true;
    }
    if (op == "CompleteInt") {
        std::uint64_t packed = 0;
        if (arguments.size() < 2 || !tryExtractPackedInt64(arguments[0], packed)) {
            result = Value(false);
            return true;
        }
        result = Value(pool.trySetResult(AwaitableHandle::unpack64(packed), arguments[1]));
        return true;
    }
    if (op == "CompleteFault") {
        std::uint64_t packed = 0;
        if (arguments.size() < 2 || !tryExtractPackedInt64(arguments[0], packed)) {
            result = Value(false);
            return true;
        }
        std::string msg = std::holds_alternative<NullString>(arguments[1])
            ? std::string("async signal faulted")
            : valueToString(arguments[1], heap);
        result = Value(pool.trySetFaulted(
            AwaitableHandle::unpack64(packed), ErrorCode::ScriptException, std::move(msg)));
        return true;
    }
    if (op == "CompleteCancel") {
        std::uint64_t packed = 0;
        if (arguments.empty() || !tryExtractPackedInt64(arguments[0], packed)) {
            result = Value(false);
            return true;
        }
        result = Value(pool.trySetCanceled(AwaitableHandle::unpack64(packed)));
        return true;
    }
    if (op == "IsCompleted") {
        std::uint64_t packed = 0;
        if (arguments.empty() || !tryExtractPackedInt64(arguments[0], packed)) {
            result = Value(false);
            return true;
        }
        result = Value(pool.isCompleted(AwaitableHandle::unpack64(packed)));
        return true;
    }
    if (op == "Release") {
        std::uint64_t packed = 0;
        if (arguments.empty() || !tryExtractPackedInt64(arguments[0], packed)) {
            result = Value(false);
            return true;
        }
        result = Value(pool.discard(AwaitableHandle::unpack64(packed)));
        return true;
    }
    if (op == "WhenAll") {
        if (arguments.empty() || std::holds_alternative<NullArray>(arguments[0])) {
            outError = "WhenAll tasks array cannot be null";
            return false;
        }
        const auto* arrRef = std::get_if<ObjectRef>(&arguments[0]);
        if (!arrRef || !heap) {
            outError = "WhenAll requires a valid tasks array";
            return false;
        }
        const auto lenOpt = heap->arrayLength(*arrRef);
        if (!lenOpt) {
            outError = "WhenAll array reference is invalid";
            return false;
        }
        std::vector<AwaitableHandle> children;
        children.reserve(*lenOpt);
        for (std::size_t i = 0; i < *lenOpt; ++i) {
            const auto elem = heap->arrayGet(*arrRef, i);
            AwaitableHandle ch{};
            if (!elem || !tryExtractAwaitableHandle(*elem, ch)) {
                outError = "WhenAll element does not hold a valid Awaitable handle";
                return false;
            }
            children.push_back(ch);
        }
        std::uint64_t packedTok = 0;
        if (arguments.size() >= 2) {
            (void)tryExtractPackedInt64(arguments[1], packedTok);
        }
        WhenAllOptions opts{};
        opts.resumeQueue = ResumeQueue::Update;
        opts.token = CancellationToken::unpack64(packedTok);
        opts.materializeHeapArray = false;
        const AwaitableHandle h = pool.whenAll(children.data(), children.size(), opts);
        result = Value(LongValue{static_cast<std::int64_t>(h.pack64())});
        return true;
    }
    if (op == "WhenAnyIndex") {
        if (arguments.empty() || std::holds_alternative<NullArray>(arguments[0])) {
            outError = "WhenAny tasks array cannot be null";
            return false;
        }
        const auto* arrRef = std::get_if<ObjectRef>(&arguments[0]);
        if (!arrRef || !heap) {
            outError = "WhenAny requires a valid tasks array";
            return false;
        }
        const auto lenOpt = heap->arrayLength(*arrRef);
        if (!lenOpt) {
            outError = "WhenAny array reference is invalid";
            return false;
        }
        std::vector<AwaitableHandle> children;
        children.reserve(*lenOpt);
        for (std::size_t i = 0; i < *lenOpt; ++i) {
            const auto elem = heap->arrayGet(*arrRef, i);
            AwaitableHandle ch{};
            if (!elem || !tryExtractAwaitableHandle(*elem, ch)) {
                outError = "WhenAny element does not hold a valid Awaitable handle";
                return false;
            }
            children.push_back(ch);
        }
        std::uint64_t packedTok = 0;
        std::uint64_t packedLosersCts = 0;
        if (arguments.size() >= 2) {
            (void)tryExtractPackedInt64(arguments[1], packedTok);
        }
        if (arguments.size() >= 3) {
            (void)tryExtractPackedInt64(arguments[2], packedLosersCts);
        }
        WhenAnyOptions opts{};
        opts.resumeQueue = ResumeQueue::Update;
        opts.token = CancellationToken::unpack64(packedTok);
        opts.cancelLosersSource = CancellationSourceHandle::unpack64(packedLosersCts);
        opts.returnWinnerIndex = true;
        const AwaitableHandle h = pool.whenAny(children.data(), children.size(), opts);
        result = Value(LongValue{static_cast<std::int64_t>(h.pack64())});
        return true;
    }
    if (op == "CtsCreate") {
        const auto h = ctsPool.createSource();
        result = Value(LongValue{static_cast<std::int64_t>(h.pack64())});
        return true;
    }
    if (op == "CtsCreateLinked") {
        std::uint64_t packedParent = 0;
        if (!arguments.empty()) {
            (void)tryExtractPackedInt64(arguments[0], packedParent);
        }
        const auto parentTok = CancellationToken::unpack64(packedParent);
        const auto h = ctsPool.createLinkedSource(&parentTok, 1u);
        result = Value(LongValue{static_cast<std::int64_t>(h.pack64())});
        return true;
    }
    if (op == "CtsCreateLinked2") {
        std::uint64_t packedA = 0;
        std::uint64_t packedB = 0;
        if (arguments.size() >= 1) (void)tryExtractPackedInt64(arguments[0], packedA);
        if (arguments.size() >= 2) (void)tryExtractPackedInt64(arguments[1], packedB);
        const std::array<CancellationToken, 2> parents{
            CancellationToken::unpack64(packedA),
            CancellationToken::unpack64(packedB),
        };
        const auto h = ctsPool.createLinkedSource(parents.data(), parents.size());
        result = Value(LongValue{static_cast<std::int64_t>(h.pack64())});
        return true;
    }
    if (op == "CtsCreateTimeout") {
        double secs = 0.0;
        if (!arguments.empty()) {
            if (const auto* d = std::get_if<double>(&arguments[0])) secs = *d;
            else if (const auto* f = std::get_if<FloatValue>(&arguments[0])) secs = f->value;
            else if (const auto* i = std::get_if<std::int64_t>(&arguments[0])) secs = static_cast<double>(*i);
        }
        const auto h = asyncContext->createTimeoutSource(secs, false);
        result = Value(LongValue{static_cast<std::int64_t>(h.pack64())});
        return true;
    }
    if (op == "CtsCreateLinkedTimeout") {
        std::uint64_t packedParent = 0;
        double secs = 0.0;
        if (arguments.size() >= 1) (void)tryExtractPackedInt64(arguments[0], packedParent);
        if (arguments.size() >= 2) {
            if (const auto* d = std::get_if<double>(&arguments[1])) secs = *d;
            else if (const auto* f = std::get_if<FloatValue>(&arguments[1])) secs = f->value;
            else if (const auto* i = std::get_if<std::int64_t>(&arguments[1])) secs = static_cast<double>(*i);
        }
        const auto parentTok = CancellationToken::unpack64(packedParent);
        const auto h = asyncContext->createLinkedTimeoutSource(&parentTok, 1u, secs, false);
        result = Value(LongValue{static_cast<std::int64_t>(h.pack64())});
        return true;
    }
    if (op == "CtsToken") {
        std::uint64_t packedCts = 0;
        if (arguments.empty() || !tryExtractPackedInt64(arguments[0], packedCts)) {
            result = Value(LongValue{0});
            return true;
        }
        const auto tok = ctsPool.token(CancellationSourceHandle::unpack64(packedCts));
        result = Value(LongValue{static_cast<std::int64_t>(tok.pack64())});
        return true;
    }
    if (op == "CtsCancel") {
        std::uint64_t packedCts = 0;
        if (arguments.empty() || !tryExtractPackedInt64(arguments[0], packedCts)) {
            result = Value(std::int64_t{0});
            return true;
        }
        const std::size_t n = ctsPool.cancel(CancellationSourceHandle::unpack64(packedCts));
        result = Value(static_cast<std::int64_t>(n));
        return true;
    }
    if (op == "CtsRelease") {
        std::uint64_t packedCts = 0;
        if (arguments.empty() || !tryExtractPackedInt64(arguments[0], packedCts)) {
            result = Value(false);
            return true;
        }
        result = Value(ctsPool.releaseSource(CancellationSourceHandle::unpack64(packedCts)));
        return true;
    }
    if (op == "TokenPreCanceled") {
        result = Value(LongValue{
            static_cast<std::int64_t>(CancellationToken::preCanceled().pack64())});
        return true;
    }
    if (op == "TokenIsCanceled") {
        std::uint64_t packedTok = 0;
        if (arguments.empty() || !tryExtractPackedInt64(arguments[0], packedTok)) {
            result = Value(false);
            return true;
        }
        result = Value(ctsPool.isCancellationRequested(
            CancellationToken::unpack64(packedTok)));
        return true;
    }
    if (op == "TokenIsTimedOut") {
        std::uint64_t packedTok = 0;
        if (arguments.empty() || !tryExtractPackedInt64(arguments[0], packedTok)) {
            result = Value(false);
            return true;
        }
        result = Value(ctsPool.isTimedOut(
            CancellationToken::unpack64(packedTok)));
        return true;
    }
    if (op == "AdvanceTimeouts") {
        double dt = 0.0;
        if (!arguments.empty()) {
            if (const auto* d = std::get_if<double>(&arguments[0])) dt = *d;
            else if (const auto* f = std::get_if<FloatValue>(&arguments[0])) dt = f->value;
            else if (const auto* i = std::get_if<std::int64_t>(&arguments[0])) dt = static_cast<double>(*i);
        }
        const std::size_t fired = asyncContext->advanceTimeouts(dt);
        result = Value(static_cast<std::int64_t>(fired));
        return true;
    }
    if (op == "StepUpdate") {
        std::size_t maxCount = 0;
        if (!arguments.empty()) {
            if (const auto* n = std::get_if<std::int64_t>(&arguments[0]); n && *n > 0) {
                maxCount = static_cast<std::size_t>(*n);
            }
        }
        const std::size_t ran = asyncContext->stepResumeQueue(ResumeQueue::Update, maxCount);
        result = Value(static_cast<std::int64_t>(ran));
        return true;
    }
    if (op == "ReadyUpdateCount") {
        const std::size_t cnt = pool.readyContinuationCount(ResumeQueue::Update);
        result = Value(static_cast<std::int64_t>(cnt));
        return true;
    }
    outError = "unknown Async intrinsic '" + std::string(intrinsicName) + "'";
    return false;
}

// ============================================================================
// AsyncRuntimeContext
// ============================================================================

AsyncRuntimeContext::AsyncRuntimeContext(ManagedHeap* heap)
    : cancellations_(),
      awaitables_(heap, &cancellations_),
      coroutines_(heap, this),
      completionQueue_() {}

void AsyncRuntimeContext::setHeap(ManagedHeap* heap) noexcept {
    awaitables_.setHeap(heap);
    coroutines_.setHeap(heap);
}

AwaitableHandle AsyncRuntimeContext::enqueueYield(CancellationToken token) {
    const AwaitableHandle h = awaitables_.allocatePending(ResumeQueue::Update, token);
    if (awaitables_.status(h) == AwaitStatus::Pending) {
        pendingYields_.push_back(h);
    }
    return h;
}

CancellationSourceHandle AsyncRuntimeContext::createTimeoutSource(
    double timeoutSeconds,
    bool autoReleaseOnFire) {
    const CancellationSourceHandle src = cancellations_.createSource();
    if (std::isnan(timeoutSeconds) || timeoutSeconds <= 0.0) {
        (void)cancellations_.cancelByTimeout(src);
        if (autoReleaseOnFire) {
            (void)cancellations_.releaseSource(src);
        }
        return src;
    }
    if (!std::isfinite(timeoutSeconds)) {
        return src;
    }
    scheduledTimeouts_.push_back(ScheduledTimeout{
        virtualTimeSeconds_ + timeoutSeconds,
        src,
        autoReleaseOnFire,
    });
    return src;
}

CancellationSourceHandle AsyncRuntimeContext::createLinkedTimeoutSource(
    const CancellationToken* parents,
    std::size_t parentCount,
    double timeoutSeconds,
    bool autoReleaseOnFire) {
    const CancellationSourceHandle src =
        cancellations_.createLinkedSource(parents, parentCount);
    const CancellationToken tok = cancellations_.token(src);
    if (cancellations_.isCancellationRequested(tok)) {
        return src;
    }
    if (std::isnan(timeoutSeconds) || timeoutSeconds <= 0.0) {
        (void)cancellations_.cancelByTimeout(src);
        if (autoReleaseOnFire) {
            (void)cancellations_.releaseSource(src);
        }
        return src;
    }
    if (!std::isfinite(timeoutSeconds)) {
        return src;
    }
    scheduledTimeouts_.push_back(ScheduledTimeout{
        virtualTimeSeconds_ + timeoutSeconds,
        src,
        autoReleaseOnFire,
    });
    return src;
}

std::size_t AsyncRuntimeContext::advanceTimeouts(double deltaSeconds) {
    if (std::isfinite(deltaSeconds) && deltaSeconds > 0.0) {
        virtualTimeSeconds_ += deltaSeconds;
    }
    if (scheduledTimeouts_.empty()) {
        return 0;
    }
    std::vector<ScheduledTimeout> due;
    std::size_t writeIdx = 0;
    for (std::size_t i = 0; i < scheduledTimeouts_.size(); ++i) {
        const auto item = scheduledTimeouts_[i];
        if (!cancellations_.isSourceAlive(item.source)) {
            continue;
        }
        if (cancellations_.isCancellationRequested(cancellations_.token(item.source))) {
            continue;
        }
        if (virtualTimeSeconds_ + 1e-12 >= item.deadlineSeconds) {
            due.push_back(item);
            continue;
        }
        if (writeIdx != i) {
            scheduledTimeouts_[writeIdx] = item;
        }
        ++writeIdx;
    }
    scheduledTimeouts_.resize(writeIdx);

    std::size_t firedCount = 0;
    for (const auto& item : due) {
        if (!cancellations_.isSourceAlive(item.source)) continue;
        if (!cancellations_.isCancellationRequested(cancellations_.token(item.source))) {
            (void)cancellations_.cancelByTimeout(item.source);
            ++firedCount;
        }
        if (item.autoReleaseOnFire) {
            (void)cancellations_.releaseSource(item.source);
        }
    }
    return firedCount;
}

bool AsyncRuntimeContext::stepPreTickIo(
    DeterminismSession* determinism,
    RuntimeError& outError,
    std::size_t* outContinuationsRun) {
    const AsyncDrainResult drain = completionQueue_.drainToMainThread(
        awaitables_, awaitables_.heap(), determinism, outError);
    if (!drain.succeeded) {
        if (outContinuationsRun) *outContinuationsRun = 0;
        return false;
    }
    const std::size_t ran = awaitables_.drainReadyContinuations(ResumeQueue::PreTickIo);
    if (outContinuationsRun) *outContinuationsRun = ran;
    return true;
}

std::size_t AsyncRuntimeContext::stepResumeQueue(
    ResumeQueue queue,
    std::size_t maxContinuations) {
    if (queue == ResumeQueue::Update && !pendingYields_.empty()) {
        std::vector<AwaitableHandle> batch;
        batch.swap(pendingYields_);
        for (const auto yh : batch) {
            if (awaitables_.isValid(yh) && awaitables_.status(yh) == AwaitStatus::Pending) {
                (void)awaitables_.trySetResult(yh, Value{});
            }
        }
    }
    std::size_t totalExecuted = 0;
    for (std::size_t pass = 0; pass < 65536u; ++pass) {
        if (maxContinuations != 0 && totalExecuted >= maxContinuations) {
            break;
        }
        const std::size_t passLimit = (maxContinuations != 0)
            ? (maxContinuations - totalExecuted)
            : 0;
        const std::size_t ran = awaitables_.drainReadyContinuations(queue, passLimit);
        if (ran == 0) break;
        totalExecuted += ran;
    }
    return totalExecuted;
}

void AsyncRuntimeContext::reset() noexcept {
    pendingYields_.clear();
    virtualTimeSeconds_ = 0.0;
    scheduledTimeouts_.clear();
    completionQueue_.reset();
    coroutines_.clear();
    awaitables_.clear();
    cancellations_.clear();
    awaitables_.setCancellationPool(&cancellations_);
    coroutines_.setOwnerContext(this);
}

} // namespace realscript::runtime
