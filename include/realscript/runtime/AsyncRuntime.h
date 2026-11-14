#pragma once

#include "realscript/runtime/AsyncHandles.h"
#include "realscript/runtime/FiberScheduler.h"
#include "realscript/runtime/Runtime.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace realscript::runtime {

/**
 * O(1) ABA-safe registration handle used to detach cancellation callbacks when an
 * `Awaitable` completes before cancellation is requested.
 */
struct CancellationRegistration {
    static constexpr std::uint32_t InvalidIndex = 0xFFFFFFFFu;

    std::uint32_t ctsSlot = CancellationToken::InvalidSlot;
    std::uint32_t ctsVersion = 0;
    std::uint32_t callbackIndex = InvalidIndex;
    std::uint32_t callbackGeneration = 0;

    [[nodiscard]] constexpr bool valid() const noexcept {
        return ctsSlot != CancellationToken::InvalidSlot &&
            ctsVersion != 0u &&
            callbackIndex != InvalidIndex &&
            callbackGeneration != 0u;
    }
};

struct CancellationPoolStatistics {
    std::size_t capacity = 0;
    std::size_t activeSources = 0;
    std::size_t freeSources = 0;
    std::size_t activeEntityTokens = 0;
    std::uint64_t totalCreated = 0;
    std::uint64_t totalReleased = 0;
    std::uint64_t totalCallbacksInvoked = 0;
};

/**
 * Isolated keyspace domains for `CancellationTokenPool` entity-destroy tokens,
 * preventing handle collisions when `SceneScriptRuntime` and `GameplayHost`
 * share a single `AsyncRuntimeContext`.
 */
enum class EntityDestroyDomain : std::uint8_t {
    SceneGenerational = 0,
    SceneNonGenerational = 1,
    GameplayCore = 2,
    Custom = 3,
};

inline constexpr std::size_t kEntityDestroyDomainCount = 4;

/**
 * Pooled Generational `CancellationTokenSource` & Entity Destroy Token Manager.
 */
class CancellationTokenPool {
public:
    using CancelCallbackFn = void (*)(void* context, std::uint64_t userTag);

    CancellationTokenPool() = default;
    ~CancellationTokenPool() = default;
    CancellationTokenPool(const CancellationTokenPool&) = delete;
    CancellationTokenPool& operator=(const CancellationTokenPool&) = delete;
    CancellationTokenPool(CancellationTokenPool&&) noexcept = default;
    CancellationTokenPool& operator=(CancellationTokenPool&&) noexcept = default;

    [[nodiscard]] CancellationSourceHandle createSource();
    [[nodiscard]] CancellationSourceHandle createLinkedSource(
        const CancellationToken* parents,
        std::size_t parentCount);
    [[nodiscard]] CancellationSourceHandle createLinkedSource(
        std::initializer_list<CancellationToken> parents);

    [[nodiscard]] CancellationToken token(CancellationSourceHandle source) const noexcept;
    [[nodiscard]] bool isSourceAlive(CancellationSourceHandle source) const noexcept;
    [[nodiscard]] bool isCancellationRequested(CancellationToken token) const noexcept;
    [[nodiscard]] bool isTimedOut(CancellationToken token) const noexcept;

    std::size_t cancel(CancellationSourceHandle source);
    std::size_t cancelByTimeout(CancellationSourceHandle source);
    bool releaseSource(CancellationSourceHandle source) noexcept;

    [[nodiscard]] CancellationRegistration registerCallback(
        CancellationToken token,
        CancelCallbackFn callback,
        void* context,
        std::uint64_t userTag = 0);
    bool unregisterCallback(CancellationRegistration registration) noexcept;

    [[nodiscard]] CancellationToken getOrCreateEntityDestroyToken(
        GenerationalEntityHandle entity,
        EntityDestroyDomain domain = EntityDestroyDomain::SceneGenerational);
    [[nodiscard]] bool hasEntityDestroyToken(
        GenerationalEntityHandle entity,
        EntityDestroyDomain domain = EntityDestroyDomain::SceneGenerational) const noexcept;
    bool cancelEntity(
        GenerationalEntityHandle entity,
        EntityDestroyDomain domain = EntityDestroyDomain::SceneGenerational);

    void clear() noexcept;
    [[nodiscard]] CancellationPoolStatistics statistics() const noexcept;

private:
    struct CallbackSlot {
        CancelCallbackFn callback = nullptr;
        void* context = nullptr;
        std::uint64_t userTag = 0;
        std::uint32_t generation = 1;
        bool active = false;
    };

    struct CtsSlot {
        std::uint32_t version = 1;
        std::uint32_t lastCanceledVersion = 0;
        std::uint32_t lastTimedOutVersion = 0;
        bool allocated = false;
        bool canceled = false;
        bool timedOut = false;
        std::vector<CallbackSlot> callbacks;
        std::vector<std::uint32_t> freeCallbacks;
        std::vector<CancellationRegistration> parentRegistrations;
    };

    static void onLinkedParentCanceled(void* context, std::uint64_t packedChildSource);
    std::size_t cancelImpl(CancellationSourceHandle source, bool byTimeout);
    void detachParentRegistrations(CtsSlot& slot) noexcept;

    std::deque<CtsSlot> slots_;
    std::vector<std::uint32_t> freeSlots_;
    std::array<
        std::unordered_map<std::uint64_t, CancellationSourceHandle>,
        kEntityDestroyDomainCount> entityDestroySources_;
    std::uint64_t totalCreated_ = 0;
    std::uint64_t totalReleased_ = 0;
    std::uint64_t totalCallbacksInvoked_ = 0;
    bool activeTimeoutCascade_ = false;
};

/**
 * Zero-heap-allocation continuation descriptor stored inline in `AwaitableCore`
 * and queued onto deterministic `ResumeQueue` phase buffers.
 */
struct AsyncContinuation {
    using CallbackFn = void (*)(
        void* context,
        AwaitableHandle completedHandle,
        AwaitStatus status,
        std::uint64_t userTag);

    CallbackFn callback = nullptr;
    void* context = nullptr;
    std::uint64_t userTag = 0;
    ObjectRef scriptTarget{};
    ResumeQueue resumeQueue = ResumeQueue::Update;
    bool runInlineOnComplete = false;

    [[nodiscard]] constexpr bool empty() const noexcept {
        return callback == nullptr && !scriptTarget.valid();
    }
};

struct AwaitablePoolStatistics {
    std::size_t capacity = 0;
    std::size_t activeCount = 0;
    std::size_t freeListCount = 0;
    std::size_t activeGcRoots = 0;
    std::size_t activeCombinators = 0;
    std::uint64_t totalAllocated = 0;
    std::uint64_t totalRecycled = 0;
    std::uint64_t synchronousFastPathHits = 0;
    std::uint64_t continuationsDispatched = 0;
};

struct WhenAllOptions {
    ResumeQueue resumeQueue = ResumeQueue::Update;
    CancellationToken token = CancellationToken::none();
    bool materializeHeapArray = true;
};

struct WhenAnyOptions {
    ResumeQueue resumeQueue = ResumeQueue::Update;
    CancellationToken token = CancellationToken::none();
    CancellationSourceHandle cancelLosersSource{};
    bool returnWinnerIndex = false;
};

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
struct WhenAnyOutcome {
    std::size_t winnerIndex = 0;
    AwaitStatus winnerStatus = AwaitStatus::Pending;
    Value value{};
    RuntimeError error{};
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

/**
 * Zero-Allocation Pooled `AwaitableCorePool` (inspired by Unity 6 `Awaitable` / C# `IValueTaskSource`
 * and Godot 4 unified signal/coroutine resumption).
 *
 * Hot-path layout separates cold fault/combinator metadata (`ColdSlotState`) from the compact
 * 2-cache-line `AwaitableSlot` and uses contiguous flat ring buffers for `ResumeQueue` dispatch.
 */
class AwaitableCorePool {
public:
    explicit AwaitableCorePool(
        ManagedHeap* heap = nullptr,
        CancellationTokenPool* cancellationPool = nullptr,
        std::size_t initialReserve = 64);
    ~AwaitableCorePool() noexcept;
    AwaitableCorePool(const AwaitableCorePool&) = delete;
    AwaitableCorePool& operator=(const AwaitableCorePool&) = delete;
    AwaitableCorePool(AwaitableCorePool&&) = delete;
    AwaitableCorePool& operator=(AwaitableCorePool&&) = delete;

    void setHeap(ManagedHeap* heap) noexcept;
    [[nodiscard]] ManagedHeap* heap() const noexcept { return heap_; }

    void setCancellationPool(CancellationTokenPool* cancellationPool) noexcept;
    [[nodiscard]] CancellationTokenPool* cancellationPool() const noexcept {
        return cancellationPool_;
    }

    [[nodiscard]] AwaitableHandle allocatePending(
        ResumeQueue defaultQueue = ResumeQueue::Update,
        CancellationToken token = CancellationToken::none());

    [[nodiscard]] AwaitableHandle fromResult(Value value = {});
    [[nodiscard]] AwaitableHandle fromFaulted(
        ErrorCode code,
        std::string message,
        Value exceptionObject = {});
    [[nodiscard]] AwaitableHandle fromCanceled();

    bool trySetResult(AwaitableHandle handle, Value value = {});
    bool trySetFaulted(
        AwaitableHandle handle,
        ErrorCode code,
        std::string message,
        Value exceptionObject = {});
    bool trySetCanceled(AwaitableHandle handle);

    bool onCompleted(AwaitableHandle handle, AsyncContinuation continuation);

    [[nodiscard]] bool isValid(AwaitableHandle handle) const noexcept;
    [[nodiscard]] AwaitStatus status(AwaitableHandle handle) const noexcept;
    [[nodiscard]] bool isCompleted(AwaitableHandle handle) const noexcept;

    AwaitStatus getResult(
        AwaitableHandle handle,
        Value& outValue,
        RuntimeError& outError,
        Value* outExceptionObject = nullptr);

    AwaitStatus getWhenAnyResult(
        AwaitableHandle handle,
        WhenAnyOutcome& outOutcome,
        RuntimeError& outError);

    bool discard(AwaitableHandle handle) noexcept;
    std::size_t cancelContinuationsForContext(const void* context) noexcept;

    [[nodiscard]] AwaitableHandle whenAll(
        const AwaitableHandle* tasks,
        std::size_t count,
        WhenAllOptions options = {});
    [[nodiscard]] AwaitableHandle whenAll(
        std::initializer_list<AwaitableHandle> tasks,
        WhenAllOptions options = {});

    [[nodiscard]] AwaitableHandle whenAny(
        const AwaitableHandle* tasks,
        std::size_t count,
        WhenAnyOptions options = {});
    [[nodiscard]] AwaitableHandle whenAny(
        std::initializer_list<AwaitableHandle> tasks,
        WhenAnyOptions options = {});

    [[nodiscard]] static bool decodeWhenAnyValue(
        const Value& value,
        std::size_t& outWinnerIndex,
        AwaitStatus& outWinnerStatus,
        Value& outWinnerValue) noexcept;

    std::size_t drainReadyContinuations(ResumeQueue queue, std::size_t maxCount = 0);
    [[nodiscard]] std::size_t readyContinuationCount(ResumeQueue queue) const noexcept;

    void reconcileValuesOnHotReload(
        const std::function<bool(Value&)>& patchOrValidateValue,
        const std::function<bool(ObjectRef&)>& patchOrValidateObjectRef);

    void clear() noexcept;
    [[nodiscard]] AwaitablePoolStatistics statistics() const noexcept;

private:
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
    struct ColdSlotState {
        std::string errorMessage;
        Value exceptionObject{};
        ManagedHeap::RootToken exceptionRootToken = 0;
        Value whenAnyWinnerValue{};
        std::size_t whenAnyWinnerIndex = 0;
        AwaitStatus whenAnyWinnerStatus = AwaitStatus::Pending;
        ErrorCode whenAnyWinnerErrorCode = ErrorCode::None;
        std::string whenAnyWinnerErrorMessage;
        bool hasWhenAnyMetadata = false;

        void resetHot(ManagedHeap* heap, std::size_t& activeGcRoots) noexcept {
            if (!errorMessage.empty()) errorMessage.clear();
            exceptionObject = Value{};
            if (exceptionRootToken != 0) {
                if (heap) {
                    (void)heap->removePersistentRoot(exceptionRootToken);
                }
                exceptionRootToken = 0;
                if (activeGcRoots > 0) --activeGcRoots;
            }
            if (hasWhenAnyMetadata) {
                whenAnyWinnerValue = Value{};
                whenAnyWinnerIndex = 0;
                whenAnyWinnerStatus = AwaitStatus::Pending;
                whenAnyWinnerErrorCode = ErrorCode::None;
                if (!whenAnyWinnerErrorMessage.empty()) {
                    whenAnyWinnerErrorMessage.clear();
                }
                hasWhenAnyMetadata = false;
            }
        }
    };

    struct AwaitableSlot {
        Value result{};
        AsyncContinuation continuation{};
        CancellationRegistration cancellationReg{};
        ManagedHeap::RootToken resultRootToken = 0;
        ManagedHeap::RootToken continuationTargetRootToken = 0;
        std::unique_ptr<ColdSlotState> cold{};
        std::uint32_t version = 1;
        ErrorCode errorCode = ErrorCode::None;
        AwaitStatus status = AwaitStatus::Pending;
        ResumeQueue defaultQueue = ResumeQueue::Update;
        bool allocated = false;
        bool hasContinuation = false;
        bool autoRecycleOnComplete = false;
        bool dirtyCold = false;
    };
    static_assert(sizeof(AwaitableSlot) == 128, "AwaitableSlot must fit in 2 cache lines (128 bytes)");

    struct ReadyContinuationItem {
        AsyncContinuation::CallbackFn callback = nullptr;
        void* context = nullptr;
        std::uint64_t userTag = 0;
        AwaitableHandle completedHandle{};
        AwaitStatus terminalStatus = AwaitStatus::Pending;
        ManagedHeap::RootToken pinnedTargetToken = 0;
    };

    struct FlatReadyQueue {
        std::vector<ReadyContinuationItem> items;
        std::size_t head = 0;

        [[nodiscard]] std::size_t size() const noexcept {
            return items.size() - head;
        }
        [[nodiscard]] bool empty() const noexcept {
            return head >= items.size();
        }
    };

    enum class CombinatorKind : std::uint8_t {
        WhenAll = 0,
        WhenAny = 1,
    };

    struct CombinatorSlot {
        bool allocated = false;
        bool resolved = false;
        CombinatorKind kind = CombinatorKind::WhenAll;
        bool materializeHeapArray = true;
        bool returnWinnerIndex = false;
        AwaitableHandle parentHandle{};
        CancellationSourceHandle cancelLosersSource{};
        CancellationRegistration parentCancelReg{};
        std::size_t totalChildren = 0;
        std::size_t remainingToComplete = 0;
        std::size_t unconsumedCallbacks = 0;
        std::vector<Value> values;
        std::vector<ManagedHeap::RootToken> valueRootTokens;
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

    static constexpr std::size_t kPageShift = 8;
    static constexpr std::size_t kPageSize = std::size_t{1} << kPageShift; // 256 slots/page
    static constexpr std::size_t kPageMask = kPageSize - 1u;

    [[nodiscard]] inline AwaitableSlot& slotAt(std::uint32_t idx) noexcept {
        return pages_[idx >> kPageShift][idx & kPageMask];
    }
    [[nodiscard]] inline const AwaitableSlot& slotAt(std::uint32_t idx) const noexcept {
        return pages_[idx >> kPageShift][idx & kPageMask];
    }

    [[nodiscard]] bool canRegisterContinuation(AwaitableHandle handle) const noexcept;
    [[nodiscard]] std::uint32_t allocateRawSlot(ResumeQueue defaultQueue);
    void recycleSlotAt(std::uint32_t slotIndex) noexcept;
    void releaseRootToken(ManagedHeap::RootToken& token) noexcept;
    void pinValueIfNeeded(const Value& value, ManagedHeap::RootToken& rootToken);
    void detachCancellation(AwaitableSlot& slot) noexcept;
    void enqueueReadyContinuation(
        const AsyncContinuation& cont,
        AwaitableHandle handle,
        AwaitStatus terminalStatus,
        ManagedHeap::RootToken existingTargetToken);
    void dispatchOrScheduleContinuation(
        AwaitableSlot& slot,
        AwaitableHandle handle,
        AwaitStatus terminalStatus);

    [[nodiscard]] std::uint32_t allocateCombinatorSlot();
    void releaseCombinatorValues(CombinatorSlot& comb) noexcept;
    void recycleCombinatorSlot(std::uint32_t combIndex) noexcept;

    static void onTokenCanceledCallback(void* context, std::uint64_t packedHandle);
    static void onCombinatorChildCompleted(
        void* context,
        AwaitableHandle childHandle,
        AwaitStatus childStatus,
        std::uint64_t packedCombAndIndex);
    static void onCombinatorCanceledCallback(void* context, std::uint64_t packedCombSlot);

    void handleWhenAllChild(
        std::uint32_t combSlotIndex,
        std::size_t childIndex,
        AwaitableHandle childHandle);
    void handleWhenAnyChild(
        std::uint32_t combSlotIndex,
        std::size_t childIndex,
        AwaitableHandle childHandle);

    static bool markPoolRootsCallback(void* rawSelf);
    void pinPoolBackedValueIfNeeded(const Value& value);

    ManagedHeap* heap_ = nullptr;
    CancellationTokenPool* cancellationPool_ = nullptr;
    std::shared_ptr<void> poolRootSync_;
    bool poolRootSyncActive_ = false;
    std::vector<std::unique_ptr<AwaitableSlot[]>> pages_;
    std::size_t slotCapacity_ = 0;
    std::uint32_t nextNeverUsedSlot_ = 0;
    std::vector<std::uint32_t> freeSlots_;
    std::deque<CombinatorSlot> combinators_;
    std::vector<std::uint32_t> freeCombinators_;
    std::array<FlatReadyQueue, kResumeQueueCount> readyQueues_{};

    std::size_t activeGcRoots_ = 0;
    std::uint64_t totalAllocated_ = 0;
    std::uint64_t totalRecycled_ = 0;
    std::uint64_t synchronousFastPathHits_ = 0;
    std::uint64_t continuationsDispatched_ = 0;
};

using AsyncRequestId = std::uint64_t;

enum class AsyncPayloadKind : std::uint8_t {
    Void = 0,
    Scalar = 1,
    Utf8String = 2,
    ByteBuffer = 3,
    Faulted = 4,
    Canceled = 5,
};

struct AsyncDrainResult {
    bool succeeded = true;
    std::size_t completedCount = 0;
    std::size_t staleOrCanceledCount = 0;
};

class AsyncCompletionQueue {
public:
    AsyncCompletionQueue();
    ~AsyncCompletionQueue() noexcept;
    AsyncCompletionQueue(const AsyncCompletionQueue&) = delete;
    AsyncCompletionQueue& operator=(const AsyncCompletionQueue&) = delete;

    [[nodiscard]] AsyncRequestId registerRequest(
        AwaitableHandle target,
        std::string operationName,
        std::uint64_t argumentHash = 0,
        BindingDeterminism determinism = BindingDeterminism::Recordable,
        CancellationToken token = CancellationToken::none());

    bool cancelRequest(AsyncRequestId requestId) noexcept;
    std::size_t pruneStaleRequests(const AwaitableCorePool& pool) noexcept;

    void submitVoid(AsyncRequestId requestId);
    void submitScalar(AsyncRequestId requestId, Value scalarOrInlineStruct);
    void submitString(AsyncRequestId requestId, std::string utf8String);
    void submitBytes(AsyncRequestId requestId, std::vector<std::uint8_t> bytes);
    void submitFailure(AsyncRequestId requestId, ErrorCode code, std::string message);
    void submitCanceled(AsyncRequestId requestId);

    [[nodiscard]] AsyncDrainResult drainToMainThread(
        AwaitableCorePool& pool,
        ManagedHeap* heap,
        DeterminismSession* determinism,
        RuntimeError& outError);

    [[nodiscard]] std::size_t pendingRequestCount() const noexcept {
        return pendingRequests_.size();
    }
    [[nodiscard]] std::size_t queuedBackgroundPacketCount() const noexcept;
    [[nodiscard]] std::uint64_t drainTickSequence() const noexcept {
        return drainTickSequence_;
    }

    void reset() noexcept;

private:
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
    struct AsyncHostPacket {
        AsyncRequestId requestId = 0;
        AsyncPayloadKind kind = AsyncPayloadKind::Void;
        Value scalarValue{};
        std::string stringPayload;
        std::vector<std::uint8_t> bytesPayload;
        ErrorCode errorCode = ErrorCode::None;
        std::string errorMessage;
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

    struct PendingRequestEntry {
        AsyncRequestId requestId = 0;
        AwaitableHandle target{};
        std::string operationName;
        std::uint64_t argumentHash = 0;
        BindingDeterminism determinism = BindingDeterminism::Recordable;
        CancellationToken token = CancellationToken::none();
    };

    bool applyPacketOnMainThread(
        const PendingRequestEntry& request,
        AsyncHostPacket& packet,
        AwaitableCorePool& pool,
        ManagedHeap* heap,
        AsyncDrainResult& stats,
        RuntimeError& outError);

    static bool markPoolRootsCallback(void* rawSelf);

    std::shared_ptr<void> poolRootSync_;
    mutable std::mutex inboxMutex_;
    std::vector<AsyncHostPacket> inbox_;
    std::vector<AsyncHostPacket> swapBuffer_;
    std::vector<Value> inFlightDrainValues_;

    std::unordered_map<AsyncRequestId, PendingRequestEntry> pendingRequests_;
    AsyncRequestId nextRequestId_ = 1;
    std::uint64_t drainTickSequence_ = 0;
};

class AsyncRuntimeContext;
struct CoroutineFrame;

using CoroutineResumeCallback = void (*)(
    void* runnerContext,
    CoroutineFrameHandle frameHandle,
    CoroutineFrame& frame,
    AsyncRuntimeContext& asyncContext);

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
struct CoroutineFrame {
    static constexpr std::size_t kInlineScalarBytes = 256;

    semantic::SymbolId functionSymbolId = 0;
    semantic::SymbolId returnStructTypeId = 0;
    AwaitableHandle completionHandle{};
    AwaitableHandle awaitedHandle{};
    std::uint32_t resumeBlockId = 0;
    ResumeQueue resumeQueue = ResumeQueue::Update;
    bool suspended = false;
    bool completed = false;
    bool invalidated = false;
    bool usesScalarBuffer = false;
    std::uint32_t scalarByteSize = 0;
    CoroutineResumeCallback resumeCallback = nullptr;
    void* runnerContext = nullptr;
    std::shared_ptr<const void> runnerOwner{};
    alignas(16) std::array<std::uint8_t, kInlineScalarBytes> inlineScalarBytes{};
    std::vector<std::uint8_t> overflowScalarBytes;
    std::vector<Value> savedArguments;
    std::vector<Value> savedLocals;
    std::vector<Value> savedRegisters;

    [[nodiscard]] const void* scalarBufferData() const noexcept {
        return scalarByteSize <= kInlineScalarBytes
            ? static_cast<const void*>(inlineScalarBytes.data())
            : static_cast<const void*>(overflowScalarBytes.data());
    }
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

struct CoroutinePoolStatistics {
    std::size_t capacity = 0;
    std::size_t activeCount = 0;
    std::size_t freeListCount = 0;
    std::size_t activeGcRoots = 0;
    std::uint64_t totalAllocated = 0;
    std::uint64_t totalRecycled = 0;
    std::uint64_t totalSuspensions = 0;
    std::uint64_t totalResumptions = 0;
};

class CoroutineFramePool {
public:
    explicit CoroutineFramePool(
        ManagedHeap* heap = nullptr,
        AsyncRuntimeContext* ownerContext = nullptr,
        std::size_t initialReserve = 32);
    ~CoroutineFramePool() noexcept;
    CoroutineFramePool(const CoroutineFramePool&) = delete;
    CoroutineFramePool& operator=(const CoroutineFramePool&) = delete;

    void setHeap(ManagedHeap* heap) noexcept;
    [[nodiscard]] ManagedHeap* heap() const noexcept { return heap_; }

    void setOwnerContext(AsyncRuntimeContext* ownerContext) noexcept {
        ownerContext_ = ownerContext;
    }

    [[nodiscard]] CoroutineFrameHandle allocateFrame(
        semantic::SymbolId functionSymbolId,
        semantic::SymbolId returnStructTypeId,
        AwaitableHandle completionHandle,
        ResumeQueue resumeQueue = ResumeQueue::Update);

    [[nodiscard]] CoroutineFrame* resolve(CoroutineFrameHandle handle) noexcept;
    [[nodiscard]] const CoroutineFrame* resolve(CoroutineFrameHandle handle) const noexcept;

    bool suspendFrame(
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
        std::shared_ptr<const void> runnerOwner = {},
        bool runInlineOnComplete = false);

    bool suspendScalarFrame(
        CoroutineFrameHandle handle,
        const void* scalarData,
        std::size_t scalarByteSize,
        const Value* refSlots,
        std::size_t refCount,
        std::uint32_t resumeBlockId,
        AwaitableHandle awaitedHandle,
        CoroutineResumeCallback resumeCallback,
        void* runnerContext,
        std::shared_ptr<const void> runnerOwner = {},
        bool runInlineOnComplete = false);

    template <typename Visitor>
    void forEachActiveFrame(Visitor&& visitor) {
        const std::size_t count = slots_.size();
        for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(count); ++i) {
            auto& slot = slots_[i];
            if (slot.allocated) {
                visitor(CoroutineFrameHandle{i, slot.version}, slot.frame);
            }
        }
    }

    template <typename Visitor>
    void forEachActiveFrame(Visitor&& visitor) const {
        const std::size_t count = slots_.size();
        for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(count); ++i) {
            const auto& slot = slots_[i];
            if (slot.allocated) {
                visitor(CoroutineFrameHandle{i, slot.version}, slot.frame);
            }
        }
    }

    void notePoolRootsPotentiallyModified();

    void recycleFrame(CoroutineFrameHandle handle) noexcept;
    std::size_t abortAllSuspendedFrames();
    void clear() noexcept;
    [[nodiscard]] CoroutinePoolStatistics statistics() const noexcept;

private:
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
    struct FrameSlot {
        CoroutineFrame frame{};
        std::vector<ManagedHeap::RootToken> pinnedRootTokens;
        std::uint32_t version = 1;
        bool allocated = false;
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

    void releasePinnedRoots(FrameSlot& slot) noexcept;
    void snapshotAndPinValues(
        FrameSlot& slot,
        const Value* args,
        std::size_t argCount,
        const Value* locals,
        std::size_t localCount,
        const Value* regs,
        std::size_t regCount);

    static void onAwaitedCompletedCallback(
        void* context,
        AwaitableHandle completedHandle,
        AwaitStatus terminalStatus,
        std::uint64_t packedFrameHandle);

    static bool markPoolRootsCallback(void* rawSelf);

    ManagedHeap* heap_ = nullptr;
    AsyncRuntimeContext* ownerContext_ = nullptr;
    std::shared_ptr<void> poolRootSync_;
    bool poolRootSyncActive_ = false;
    std::deque<FrameSlot> slots_;
    std::vector<std::uint32_t> freeSlots_;
    std::size_t activeGcRoots_ = 0;
    std::uint64_t totalAllocated_ = 0;
    std::uint64_t totalRecycled_ = 0;
    std::uint64_t totalSuspensions_ = 0;
    std::uint64_t totalResumptions_ = 0;
};

[[nodiscard]] Value makeAwaitableStructValue(
    semantic::SymbolId structTypeId,
    AwaitableHandle handle);

[[nodiscard]] bool tryExtractAwaitableHandle(
    const Value& value,
    AwaitableHandle& outHandle) noexcept;

[[nodiscard]] std::optional<ObjectRef> materializeAsyncResumeException(
    ManagedHeap* heap,
    AwaitStatus terminalStatus,
    const RuntimeError& faultError,
    const Value& storedExceptionObject,
    semantic::SymbolId fallbackExceptionTypeId = 0);

[[nodiscard]] bool isAsyncCanceledExceptionObject(
    const ManagedHeap* heap,
    const Value& excValue) noexcept;

bool tryExecuteAsyncIntrinsic(
    std::string_view intrinsicName,
    ValueSpan arguments,
    AsyncRuntimeContext* asyncContext,
    ManagedHeap* heap,
    Value& result,
    std::string& outError);

class AsyncRuntimeContext {
public:
    explicit AsyncRuntimeContext(ManagedHeap* heap = nullptr);

    void setHeap(ManagedHeap* heap) noexcept;

    [[nodiscard]] CancellationTokenPool& cancellations() noexcept { return cancellations_; }
    [[nodiscard]] const CancellationTokenPool& cancellations() const noexcept {
        return cancellations_;
    }

    [[nodiscard]] AwaitableCorePool& awaitables() noexcept { return awaitables_; }
    [[nodiscard]] const AwaitableCorePool& awaitables() const noexcept { return awaitables_; }

    [[nodiscard]] CoroutineFramePool& coroutines() noexcept { return coroutines_; }
    [[nodiscard]] const CoroutineFramePool& coroutines() const noexcept { return coroutines_; }

    [[nodiscard]] AwaitableHandle enqueueYield(CancellationToken token = CancellationToken::none());

    [[nodiscard]] double virtualTimeSeconds() const noexcept { return virtualTimeSeconds_; }
    [[nodiscard]] CancellationSourceHandle createTimeoutSource(
        double timeoutSeconds,
        bool autoReleaseOnFire = false);
    [[nodiscard]] CancellationSourceHandle createLinkedTimeoutSource(
        const CancellationToken* parents,
        std::size_t parentCount,
        double timeoutSeconds,
        bool autoReleaseOnFire = false);
    std::size_t advanceTimeouts(double deltaSeconds);

    [[nodiscard]] AsyncCompletionQueue& completionQueue() noexcept { return completionQueue_; }
    [[nodiscard]] const AsyncCompletionQueue& completionQueue() const noexcept {
        return completionQueue_;
    }

    [[nodiscard]] bool stepPreTickIo(
        DeterminismSession* determinism,
        RuntimeError& outError,
        std::size_t* outContinuationsRun = nullptr);

    std::size_t stepResumeQueue(ResumeQueue queue, std::size_t maxContinuations = 0);
    void reset() noexcept;

    [[nodiscard]] void* activeInterpreterState() const noexcept { return activeInterpreterState_; }
    void setActiveInterpreterState(void* state) noexcept { activeInterpreterState_ = state; }

    [[nodiscard]] void* activeAotContext() const noexcept { return activeAotContext_; }
    void setActiveAotContext(void* ctx) noexcept { activeAotContext_ = ctx; }

private:
    CancellationTokenPool cancellations_;
    AwaitableCorePool awaitables_;
    CoroutineFramePool coroutines_;
    AsyncCompletionQueue completionQueue_;
    struct ScheduledTimeout {
        double deadlineSeconds = 0.0;
        CancellationSourceHandle source{};
        bool autoReleaseOnFire = false;
    };

    void* activeInterpreterState_ = nullptr;
    void* activeAotContext_ = nullptr;
    std::vector<AwaitableHandle> pendingYields_;
    double virtualTimeSeconds_ = 0.0;
    std::vector<ScheduledTimeout> scheduledTimeouts_;
};

} // namespace realscript::runtime
