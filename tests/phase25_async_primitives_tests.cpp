#include "realscript/compiler/Compilation.h"
#include "realscript/runtime/AsyncRuntime.h"
#include "realscript/runtime/Runtime.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace realscript::runtime;

void check(bool condition, const char* expr, int line) {
    if (!condition) {
        std::cerr << "[FAIL] phase25_async_primitives_tests.cpp:" << line
                  << " assertion failed: " << expr << '\n';
        std::abort();
    }
}

#define RS_CHECK(expr) check(static_cast<bool>(expr), #expr, __LINE__)

struct ContinuationCounter {
    std::uint64_t count = 0;
    AwaitStatus lastStatus = AwaitStatus::Pending;
    std::uint64_t lastTag = 0;

    static void onResume(
        void* ctx,
        AwaitableHandle /*handle*/,
        AwaitStatus status,
        std::uint64_t tag) {
        auto* self = static_cast<ContinuationCounter*>(ctx);
        ++self->count;
        self->lastStatus = status;
        self->lastTag = tag;
    }
};

void testZeroAllocSteadyStateAndBenchmark() {
    ManagedHeap heap;
    CancellationTokenPool ctsPool;
    AwaitableCorePool pool(&heap, &ctsPool, 64);

    const GcStatistics gcBefore = heap.statistics();
    ContinuationCounter counter{};

    constexpr std::size_t kIterations = 100'000;
    const auto t0 = std::chrono::steady_clock::now();
    std::int64_t checksum = 0;

    for (std::size_t i = 0; i < kIterations; ++i) {
        const AwaitableHandle h = pool.allocatePending(ResumeQueue::Update);
        AsyncContinuation cont{};
        cont.callback = &ContinuationCounter::onResume;
        cont.context = &counter;
        cont.userTag = i;
        cont.resumeQueue = ResumeQueue::Update;
        RS_CHECK(pool.onCompleted(h, cont));

        RS_CHECK(pool.trySetResult(h, Value(static_cast<std::int64_t>(i * 3 + 1))));
        RS_CHECK(pool.drainReadyContinuations(ResumeQueue::Update) == 1u);

        Value outVal{};
        RuntimeError outErr{};
        const AwaitStatus st = pool.getResult(h, outVal, outErr);
        RS_CHECK(st == AwaitStatus::Succeeded);
        RS_CHECK(outErr.code == ErrorCode::None);
        checksum += std::get<std::int64_t>(outVal);
    }
    const auto t1 = std::chrono::steady_clock::now();

    // Baseline comparison: traditional heap-allocated Task + continuation closure + GC root pin
    ManagedHeap baselineHeap;
    ShadowStack emptyStack;
    std::uint64_t baselineResumes = 0;
    const auto b0 = std::chrono::steady_clock::now();
    std::int64_t baselineChecksum = 0;
    for (std::size_t i = 0; i < kIterations; ++i) {
        const auto taskObj = baselineHeap.allocateRecord(3);
        const auto contObj = baselineHeap.allocateRecord(2);
        RS_CHECK(taskObj.has_value() && contObj.has_value());
        PersistentRoot taskPin = baselineHeap.retain(*taskObj);
        baselineHeap.fieldSet(*contObj, 0, Value(static_cast<std::int64_t>(i)));
        baselineHeap.fieldSet(*taskObj, 2, Value(*contObj));
        baselineHeap.fieldSet(*taskObj, 0, Value(static_cast<std::int64_t>(1)));
        baselineHeap.fieldSet(*taskObj, 1, Value(static_cast<std::int64_t>(i * 3 + 1)));
        if (baselineHeap.fieldGet(*taskObj, 2).has_value()) {
            ++baselineResumes;
        }
        const auto loaded = baselineHeap.fieldGet(*taskObj, 1);
        RS_CHECK(loaded.has_value());
        baselineChecksum += std::get<std::int64_t>(*loaded);
        taskPin.reset();
        if ((i & 1023u) == 1023u) {
            baselineHeap.collectYoung(emptyStack);
        }
    }
    const auto b1 = std::chrono::steady_clock::now();

    RS_CHECK(checksum == baselineChecksum);
    RS_CHECK(counter.count == kIterations && baselineResumes == kIterations);

    const GcStatistics gcAfter = heap.statistics();
    RS_CHECK(gcAfter.objectsAllocated == gcBefore.objectsAllocated);
    RS_CHECK(gcAfter.bytesAllocated == gcBefore.bytesAllocated);

    const AwaitablePoolStatistics poolStats = pool.statistics();
    RS_CHECK(poolStats.capacity == 256u);
    RS_CHECK(poolStats.activeCount == 0u);
    RS_CHECK(poolStats.freeListCount == 256u);
    RS_CHECK(poolStats.activeGcRoots == 0u);
    RS_CHECK(poolStats.totalAllocated == kIterations);
    RS_CHECK(poolStats.totalRecycled == kIterations);

    const double pooledNs = std::chrono::duration<double, std::nano>(t1 - t0).count() /
        static_cast<double>(kIterations);
    const double heapNs = std::chrono::duration<double, std::nano>(b1 - b0).count() /
        static_cast<double>(kIterations);
    std::cout << "[BENCH] Pooled AwaitableCore: " << pooledNs << " ns/op (0 B heap) vs "
              << "ManagedHeap state record: " << heapNs << " ns/op ("
              << baselineHeap.statistics().bytesAllocated << " B total allocated)\n";
}

void testGenerationalAbaSafetyAndSingleAwaiter() {
    AwaitableCorePool pool(nullptr, nullptr, 4);

    // Synchronous void sentinel fast path (zero slot allocation)
    const AwaitableHandle voidHandle = pool.fromResult(Value{});
    RS_CHECK(voidHandle.isCompletedVoidSentinel());
    RS_CHECK(pool.isCompleted(voidHandle));
    {
        Value v{std::int64_t{99}};
        RuntimeError err{};
        RS_CHECK(pool.getResult(voidHandle, v, err) == AwaitStatus::Succeeded);
        RS_CHECK(std::holds_alternative<std::monostate>(v));
    }

    // Slot reuse + ABA version protection
    const AwaitableHandle h1 = pool.allocatePending();
    RS_CHECK(h1.valid());
    RS_CHECK(!pool.isCompleted(h1));

    // Calling getResult while still Pending must fail without recycling h1
    {
        Value prematureVal{};
        RuntimeError prematureErr{};
        RS_CHECK(pool.getResult(h1, prematureVal, prematureErr) == AwaitStatus::Faulted);
        RS_CHECK(prematureErr.code == ErrorCode::InvalidProgram);
        RS_CHECK(pool.isValid(h1));
    }

    // Single-awaiter invariant
    ContinuationCounter c1{};
    ContinuationCounter c2{};
    AsyncContinuation cont1{};
    cont1.callback = &ContinuationCounter::onResume;
    cont1.context = &c1;
    cont1.userTag = 101;
    RS_CHECK(pool.onCompleted(h1, cont1));

    AsyncContinuation cont2 = cont1;
    cont2.context = &c2;
    RS_CHECK(!pool.onCompleted(h1, cont2));

    RS_CHECK(pool.trySetResult(h1, Value(std::int64_t{777})));
    // Second trySetResult on already-completed handle must be rejected
    RS_CHECK(!pool.trySetResult(h1, Value(std::int64_t{888})));

    RS_CHECK(pool.drainReadyContinuations(ResumeQueue::Update) == 1u);
    RS_CHECK(c1.count == 1u && c1.lastTag == 101u);
    RS_CHECK(c2.count == 0u);

    Value out1{};
    RuntimeError err1{};
    RS_CHECK(pool.getResult(h1, out1, err1) == AwaitStatus::Succeeded);
    RS_CHECK(std::get<std::int64_t>(out1) == 777);

    // Double consumption of h1 must fail with InvalidProgram
    Value outDup{};
    RuntimeError errDup{};
    RS_CHECK(pool.getResult(h1, outDup, errDup) == AwaitStatus::Faulted);
    RS_CHECK(errDup.code == ErrorCode::InvalidProgram);

    // Next allocation reuses exact LIFO slot with incremented generation
    const AwaitableHandle h2 = pool.allocatePending();
    RS_CHECK(h2.slot == h1.slot);
    RS_CHECK(h2.version == h1.version + 1u);

    // Stale h1 operations must NOT affect h2
    RS_CHECK(!pool.trySetResult(h1, Value(std::int64_t{-1})));
    RS_CHECK(!pool.trySetCanceled(h1));
    RS_CHECK(!pool.isCompleted(h2));

    RS_CHECK(pool.trySetFaulted(h2, ErrorCode::ScriptException, "boom"));
    Value out2{};
    RuntimeError err2{};
    RS_CHECK(pool.getResult(h2, out2, err2) == AwaitStatus::Faulted);
    RS_CHECK(err2.code == ErrorCode::ScriptException);
    RS_CHECK(err2.message == "boom");
}

void testManagedHeapGcRootPinningAndRelease() {
    ManagedHeap heap;
    ShadowStack emptyStack;
    AwaitableCorePool pool(&heap, nullptr, 8);

    const AwaitableHandle pendingStr = pool.allocatePending();
    const AwaitableHandle pendingExc = pool.allocatePending();

    ObjectRef savedStrRef{};
    ObjectRef savedExcRef{};
    {
        const auto s = heap.allocateString("streamed_level_chunk_alpha");
        const auto e = heap.allocateString("network_timeout_exception_payload");
        RS_CHECK(s.has_value() && e.has_value());
        savedStrRef = *s;
        savedExcRef = *e;

        RS_CHECK(pool.trySetResult(pendingStr, Value(savedStrRef)));
        RS_CHECK(pool.trySetFaulted(
            pendingExc,
            ErrorCode::ScriptException,
            "timeout",
            Value(savedExcRef)));
    }

    // Both ObjectRefs live ONLY inside AwaitableCorePool; force full GC!
    RS_CHECK(pool.statistics().activeGcRoots == 2u);
    heap.collectFull(emptyStack);
    RS_CHECK(heap.isAlive(savedStrRef));
    RS_CHECK(heap.isAlive(savedExcRef));
    RS_CHECK(heap.stringView(savedStrRef) == "streamed_level_chunk_alpha");

    // Consume both awaitables and drop returned Values -> roots must be released immediately
    {
        Value val{};
        RuntimeError err{};
        RS_CHECK(pool.getResult(pendingStr, val, err) == AwaitStatus::Succeeded);
        RS_CHECK(std::get<ObjectRef>(val) == savedStrRef);

        Value excObj{};
        RS_CHECK(pool.getResult(pendingExc, val, err, &excObj) == AwaitStatus::Faulted);
        RS_CHECK(std::get<ObjectRef>(excObj) == savedExcRef);
    }

    RS_CHECK(pool.statistics().activeGcRoots == 0u);
    heap.collectFull(emptyStack);
    RS_CHECK(!heap.isAlive(savedStrRef));
    RS_CHECK(!heap.isAlive(savedExcRef));

    // Verify setHeap() safely detaches roots from old heap before old heap destruction
    {
        auto transientHeap = std::make_unique<ManagedHeap>();
        pool.setHeap(transientHeap.get());
        const auto transientObj = transientHeap->allocateString("transient_heap_string");
        RS_CHECK(transientObj.has_value());
        const AwaitableHandle hTransient = pool.fromResult(Value(*transientObj));
        RS_CHECK(pool.statistics().activeGcRoots == 1u);

        // Switch back to main heap BEFORE destroying transientHeap -> must not UAF on recycle!
        pool.setHeap(&heap);
        transientHeap.reset();
        RS_CHECK(pool.discard(hTransient));
        RS_CHECK(pool.statistics().activeGcRoots == 0u);
    }
}

void testHierarchicalCancellationAndEntityDestroyTokens() {
    CancellationTokenPool ctsPool;
    AwaitableCorePool pool(nullptr, &ctsPool, 16);

    // Pre-canceled fast path
    const AwaitableHandle preCanceled = pool.allocatePending(
        ResumeQueue::Update, CancellationToken::preCanceled());
    RS_CHECK(pool.status(preCanceled) == AwaitStatus::Canceled);
    {
        Value v{};
        RuntimeError err{};
        RS_CHECK(pool.getResult(preCanceled, v, err) == AwaitStatus::Canceled);
    }

    // 3-level hierarchy: Root -> Scene -> Request
    const CancellationSourceHandle rootCts = ctsPool.createSource();
    const CancellationSourceHandle sceneCts =
        ctsPool.createLinkedSource({ctsPool.token(rootCts)});
    const CancellationSourceHandle reqCts =
        ctsPool.createLinkedSource({ctsPool.token(sceneCts)});

    const AwaitableHandle taskA = pool.allocatePending(
        ResumeQueue::Update, ctsPool.token(reqCts));
    const AwaitableHandle taskB = pool.allocatePending(
        ResumeQueue::Update, ctsPool.token(sceneCts));
    RS_CHECK(!pool.isCompleted(taskA));
    RS_CHECK(!pool.isCompleted(taskB));

    // Canceling root cascades to sceneCts -> reqCts -> both pending tasks!
    ctsPool.cancel(rootCts);
    RS_CHECK(ctsPool.isCancellationRequested(ctsPool.token(rootCts)));
    RS_CHECK(ctsPool.isCancellationRequested(ctsPool.token(sceneCts)));
    RS_CHECK(ctsPool.isCancellationRequested(ctsPool.token(reqCts)));
    RS_CHECK(pool.status(taskA) == AwaitStatus::Canceled);
    RS_CHECK(pool.status(taskB) == AwaitStatus::Canceled);

    {
        Value v{};
        RuntimeError e{};
        RS_CHECK(pool.getResult(taskA, v, e) == AwaitStatus::Canceled);
        RS_CHECK(pool.getResult(taskB, v, e) == AwaitStatus::Canceled);
    }
    RS_CHECK(ctsPool.releaseSource(reqCts));
    RS_CHECK(ctsPool.releaseSource(sceneCts));
    RS_CHECK(ctsPool.releaseSource(rootCts));

    // Entity Destroy Token: verify O(1) callback slot reuse across 5,000 short awaits
    // and deterministic bulk cancellation on entity teardown.
    const GenerationalEntityHandle bossEntity{12u, 3u};
    RS_CHECK(!ctsPool.hasEntityDestroyToken(bossEntity));
    const CancellationToken destroyToken =
        ctsPool.getOrCreateEntityDestroyToken(bossEntity);
    RS_CHECK(ctsPool.hasEntityDestroyToken(bossEntity));
    RS_CHECK(!ctsPool.isCancellationRequested(destroyToken));

    for (int i = 0; i < 5000; ++i) {
        const AwaitableHandle step = pool.allocatePending(
            ResumeQueue::FixedTick, destroyToken);
        RS_CHECK(pool.trySetResult(step, Value(static_cast<std::int64_t>(i))));
        Value v{};
        RuntimeError err{};
        RS_CHECK(pool.getResult(step, v, err) == AwaitStatus::Succeeded);
    }

    // Spawn 3 concurrent entity coroutines awaiting on destroyToken, then destroy entity
    const AwaitableHandle eTask1 = pool.allocatePending(ResumeQueue::Update, destroyToken);
    const AwaitableHandle eTask2 = pool.allocatePending(ResumeQueue::Update, destroyToken);
    const AwaitableHandle eTask3 = pool.allocatePending(ResumeQueue::FixedTick, destroyToken);

    RS_CHECK(ctsPool.cancelEntity(bossEntity));
    RS_CHECK(!ctsPool.hasEntityDestroyToken(bossEntity));
    // Lingering token copy still reports cancellation requested during cleanup!
    RS_CHECK(ctsPool.isCancellationRequested(destroyToken));
    RS_CHECK(pool.status(eTask1) == AwaitStatus::Canceled);
    RS_CHECK(pool.status(eTask2) == AwaitStatus::Canceled);
    RS_CHECK(pool.status(eTask3) == AwaitStatus::Canceled);

    RS_CHECK(pool.discard(eTask1));
    RS_CHECK(pool.discard(eTask2));
    RS_CHECK(pool.discard(eTask3));
    RS_CHECK(pool.statistics().activeCount == 0u);
    RS_CHECK(ctsPool.statistics().activeSources == 0u);
}

void testWhenAllAndWhenAnyCombinators() {
    ManagedHeap heap;
    ShadowStack emptyStack;
    CancellationTokenPool ctsPool;
    AwaitableCorePool pool(&heap, &ctsPool, 32);

    // 1. WhenAll out-of-order completion with mixed scalar + GC-rooted heap string
    {
        const AwaitableHandle c0 = pool.allocatePending();
        const AwaitableHandle c1 = pool.allocatePending();
        const AwaitableHandle c2 = pool.allocatePending();

        const AwaitableHandle all = pool.whenAll({c0, c1, c2});
        RS_CHECK(!pool.isCompleted(all));

        // Complete c2 first, then c1 (with a heap string + full GC!), then c0
        RS_CHECK(pool.trySetResult(c2, Value(3.5)));
        RS_CHECK(!pool.isCompleted(all));

        {
            const auto heapStr = heap.allocateString("asset_bundle_manifest");
            RS_CHECK(heapStr.has_value());
            RS_CHECK(pool.trySetResult(c1, Value(*heapStr)));
        }
        // Force full GC while c1's string sits inside WhenAll's CombinatorSlot!
        heap.collectFull(emptyStack);
        RS_CHECK(!pool.isCompleted(all));

        RS_CHECK(pool.trySetResult(c0, Value(std::int64_t{42})));
        RS_CHECK(pool.isCompleted(all));

        Value allVal{};
        RuntimeError allErr{};
        RS_CHECK(pool.getResult(all, allVal, allErr) == AwaitStatus::Succeeded);
        const ObjectRef arrRef = std::get<ObjectRef>(allVal);
        RS_CHECK(heap.arrayLength(arrRef) == 3u);
        RS_CHECK(std::get<std::int64_t>(*heap.arrayGet(arrRef, 0)) == 42);
        const ObjectRef elem1Ref = std::get<ObjectRef>(*heap.arrayGet(arrRef, 1));
        RS_CHECK(heap.stringView(elem1Ref) == "asset_bundle_manifest");
        RS_CHECK(std::get<double>(*heap.arrayGet(arrRef, 2)) == 3.5);

        RS_CHECK(pool.statistics().activeCount == 0u);
        RS_CHECK(pool.statistics().activeCombinators == 0u);
        RS_CHECK(pool.statistics().activeGcRoots == 0u);
    }

    // 2. WhenAll short-circuit on fault + immediate GC root release + automatic slot reclamation
    {
        const AwaitableHandle fastHeapChild = pool.allocatePending();
        const AwaitableHandle failingB = pool.allocatePending();
        const AwaitableHandle slowC = pool.allocatePending();

        const AwaitableHandle all = pool.whenAll({fastHeapChild, failingB, slowC});
        ObjectRef transientStr{};
        {
            const auto s = heap.allocateString("transient_sibling_result");
            RS_CHECK(s.has_value());
            transientStr = *s;
            RS_CHECK(pool.trySetResult(fastHeapChild, Value(transientStr)));
        }
        RS_CHECK(pool.statistics().activeGcRoots == 1u);

        RS_CHECK(pool.trySetFaulted(failingB, ErrorCode::InvalidArguments, "bad_shader_id"));

        // Must short-circuit immediately AND release fastHeapChild's GC root even while slowC is Pending!
        RS_CHECK(pool.isCompleted(all));
        RS_CHECK(pool.statistics().activeGcRoots == 0u);
        heap.collectFull(emptyStack);
        RS_CHECK(!heap.isAlive(transientStr));

        Value allVal{};
        RuntimeError allErr{};
        RS_CHECK(pool.getResult(all, allVal, allErr) == AwaitStatus::Faulted);
        RS_CHECK(allErr.code == ErrorCode::InvalidArguments);
        RS_CHECK(allErr.message == "bad_shader_id");

        // Combinator stays tracked ONLY until slowC finishes, then recycles everything!
        RS_CHECK(pool.statistics().activeCombinators == 1u);
        RS_CHECK(pool.trySetResult(slowC, Value(std::int64_t{2})));
        RS_CHECK(pool.statistics().activeCount == 0u);
        RS_CHECK(pool.statistics().activeCombinators == 0u);
        RS_CHECK(pool.statistics().activeGcRoots == 0u);
    }

    // 2b. Atomic pre-validation on duplicate or already-awaited child handles
    {
        const AwaitableHandle dupChild = pool.allocatePending();
        const AwaitableHandle dupAll = pool.whenAll({dupChild, dupChild});
        Value dupVal{};
        RuntimeError dupErr{};
        RS_CHECK(pool.getResult(dupAll, dupVal, dupErr) == AwaitStatus::Faulted);
        RS_CHECK(dupErr.code == ErrorCode::InvalidProgram);
        RS_CHECK(pool.statistics().activeCombinators == 0u);
        // dupChild was marked auto-recycle on complete; completing it recycles its slot immediately
        RS_CHECK(pool.trySetResult(dupChild, Value(std::int64_t{1})));
        RS_CHECK(pool.statistics().activeCount == 0u);
    }

    // 3. WhenAny race + automatic loser cancellation via cancelLosersSource
    {
        const CancellationSourceHandle raceCts = ctsPool.createSource();
        const CancellationToken raceToken = ctsPool.token(raceCts);

        const AwaitableHandle httpRequest = pool.allocatePending(ResumeQueue::Update, raceToken);
        const AwaitableHandle timeoutTimer = pool.allocatePending(ResumeQueue::Update, raceToken);
        const AwaitableHandle manualSlowTask = pool.allocatePending(ResumeQueue::Update);

        WhenAnyOptions opts;
        opts.resumeQueue = ResumeQueue::Update;
        opts.cancelLosersSource = raceCts;

        const AwaitableHandle race = pool.whenAny(
            {timeoutTimer, httpRequest, manualSlowTask}, opts);
        RS_CHECK(!pool.isCompleted(race));

        // httpRequest (index 1) wins!
        RS_CHECK(pool.trySetResult(httpRequest, Value(std::int64_t{200})));
        RS_CHECK(pool.isCompleted(race));
        // raceCts must have been auto-canceled, immediately canceling timeoutTimer!
        RS_CHECK(ctsPool.isCancellationRequested(raceToken));

        WhenAnyOutcome outcome{};
        RuntimeError raceErr{};
        RS_CHECK(pool.getWhenAnyResult(race, outcome, raceErr) == AwaitStatus::Succeeded);
        RS_CHECK(outcome.winnerIndex == 1u);
        RS_CHECK(outcome.winnerStatus == AwaitStatus::Succeeded);
        RS_CHECK(std::get<std::int64_t>(outcome.value) == 200);

        // manualSlowTask wasn't bound to raceToken and finishes 3 frames later: must self-recycle!
        RS_CHECK(pool.trySetResult(manualSlowTask, Value(std::int64_t{999})));
        RS_CHECK(ctsPool.releaseSource(raceCts));

        RS_CHECK(pool.statistics().activeCount == 0u);
        RS_CHECK(pool.statistics().activeCombinators == 0u);
        RS_CHECK(pool.statistics().activeGcRoots == 0u);
    }

    // 4. Zero-ManagedHeap-allocation WhenAll mode
    {
        const std::uint64_t heapAllocsBefore = heap.statistics().objectsAllocated;
        WhenAllOptions scalarOpts;
        scalarOpts.materializeHeapArray = false;
        for (int iter = 0; iter < 500; ++iter) {
            const AwaitableHandle a = pool.allocatePending();
            const AwaitableHandle b = pool.allocatePending();
            const AwaitableHandle joined = pool.whenAll({a, b}, scalarOpts);
            RS_CHECK(pool.trySetResult(b, Value(std::int64_t{10})));
            RS_CHECK(pool.trySetResult(a, Value(std::int64_t{20})));
            Value v{};
            RuntimeError err{};
            RS_CHECK(pool.getResult(joined, v, err) == AwaitStatus::Succeeded);
            RS_CHECK(std::get<std::int64_t>(v) == 2);
        }
        RS_CHECK(heap.statistics().objectsAllocated == heapAllocsBefore);
        RS_CHECK(pool.statistics().activeCount == 0u);
    }
}

void testMultiThreadedAsyncCompletionQueue() {
    ManagedHeap heap;
    ShadowStack emptyStack;
    AsyncRuntimeContext asyncCtx(&heap);

    constexpr std::size_t kPerWorker = 8;
    constexpr std::size_t kWorkers = 4;
    constexpr std::size_t kTotalRequests = kPerWorker * kWorkers;

    std::vector<AwaitableHandle> handles(kTotalRequests);
    std::vector<AsyncRequestId> reqIds(kTotalRequests);

    for (std::size_t i = 0; i < kTotalRequests; ++i) {
        handles[i] = asyncCtx.awaitables().allocatePending(ResumeQueue::PreTickIo);
        reqIds[i] = asyncCtx.completionQueue().registerRequest(
            handles[i],
            "WorkerOp." + std::to_string(i % kWorkers),
            static_cast<std::uint64_t>(i));
    }

    // Also test background thread safety guard when accidentally submitting an ObjectRef
    const auto illegalHeapObj = heap.allocateString("main_thread_only_obj");
    RS_CHECK(illegalHeapObj.has_value());
    const AwaitableHandle guardHandle =
        asyncCtx.awaitables().allocatePending(ResumeQueue::PreTickIo);
    const AsyncRequestId guardReqId = asyncCtx.completionQueue().registerRequest(
        guardHandle, "IllegalHeapRefOp", 999u);

    std::vector<std::thread> workers;
    workers.reserve(kWorkers);
    for (std::size_t w = 0; w < kWorkers; ++w) {
        workers.emplace_back([&, w]() {
            for (std::size_t j = 0; j < kPerWorker; ++j) {
                // Reverse submission order inside each worker to exercise canonical sorting
                const std::size_t idx = w * kPerWorker + (kPerWorker - 1u - j);
                const AsyncRequestId reqId = reqIds[idx];
                switch (w) {
                case 0:
                    asyncCtx.completionQueue().submitScalar(
                        reqId, Value(static_cast<std::int64_t>(idx * 10)));
                    break;
                case 1:
                    asyncCtx.completionQueue().submitString(
                        reqId, "json_payload_" + std::to_string(idx));
                    break;
                case 2:
                    asyncCtx.completionQueue().submitBytes(
                        reqId,
                        std::vector<std::uint8_t>{
                            static_cast<std::uint8_t>(idx & 0xFFu),
                            0xA5u,
                            0x5Au,
                        });
                    break;
                default:
                    if ((j & 1u) == 0u) {
                        asyncCtx.completionQueue().submitFailure(
                            reqId, ErrorCode::ExternalFunctionUnresolved, "http_503");
                    } else {
                        asyncCtx.completionQueue().submitCanceled(reqId);
                    }
                    break;
                }
            }
            if (w == 0) {
                asyncCtx.completionQueue().submitScalar(guardReqId, Value(*illegalHeapObj));
            }
        });
    }

    for (auto& t : workers) {
        t.join();
    }

    RuntimeError drainErr{};
    std::size_t continuationsRun = 0;
    RS_CHECK(asyncCtx.stepPreTickIo(nullptr, drainErr, &continuationsRun));
    RS_CHECK(drainErr.code == ErrorCode::None);
    RS_CHECK(asyncCtx.completionQueue().pendingRequestCount() == 0u);

    // Run full GC while materialized strings & byte arrays sit inside AwaitableCorePool!
    heap.collectFull(emptyStack);

    for (std::size_t i = 0; i < kTotalRequests; ++i) {
        const std::size_t w = i / kPerWorker;
        Value val{};
        RuntimeError err{};
        const AwaitStatus st = asyncCtx.awaitables().getResult(handles[i], val, err);
        if (w == 0) {
            RS_CHECK(st == AwaitStatus::Succeeded);
            RS_CHECK(std::get<std::int64_t>(val) == static_cast<std::int64_t>(i * 10));
        } else if (w == 1) {
            RS_CHECK(st == AwaitStatus::Succeeded);
            const ObjectRef sRef = std::get<ObjectRef>(val);
            RS_CHECK(heap.stringView(sRef) == "json_payload_" + std::to_string(i));
        } else if (w == 2) {
            RS_CHECK(st == AwaitStatus::Succeeded);
            const ObjectRef bRef = std::get<ObjectRef>(val);
            RS_CHECK(heap.arrayLength(bRef) == 3u);
            RS_CHECK(std::get<std::int64_t>(*heap.arrayGet(bRef, 0)) ==
                     static_cast<std::int64_t>(i & 0xFFu));
            RS_CHECK(std::get<std::int64_t>(*heap.arrayGet(bRef, 1)) == 0xA5);
            RS_CHECK(std::get<std::int64_t>(*heap.arrayGet(bRef, 2)) == 0x5A);
        } else {
            const std::size_t j = (kPerWorker - 1u) - (i % kPerWorker);
            if ((j & 1u) == 0u) {
                RS_CHECK(st == AwaitStatus::Faulted);
                RS_CHECK(err.code == ErrorCode::ExternalFunctionUnresolved);
                RS_CHECK(err.message == "http_503");
            } else {
                RS_CHECK(st == AwaitStatus::Canceled);
            }
        }
    }

    {
        Value guardVal{};
        RuntimeError guardErr{};
        RS_CHECK(asyncCtx.awaitables().getResult(guardHandle, guardVal, guardErr) ==
                 AwaitStatus::Faulted);
        RS_CHECK(guardErr.code == ErrorCode::InvalidObjectReference);
    }

    RS_CHECK(asyncCtx.awaitables().statistics().activeCount == 0u);
    RS_CHECK(asyncCtx.awaitables().statistics().activeGcRoots == 0u);
}

void testDeterministicRecordReplayAndStrictMode() {
    RuntimeError linkErr{};
    auto linked = ProgramImage::link(std::vector<realscript::bytecode::Module>{}, linkErr);
    RS_CHECK(linked.has_value());
    const auto program = std::make_shared<const ProgramImage>(std::move(*linked));

    auto replayLog = std::make_shared<ReplayLog>();
    std::uint64_t recordedDigest = 0;

    // ------------------------------------------------------------------------
    // Phase A: Record a 5-frame run where req2 (fast disk) finishes on Frame 1,
    // while req1 (slow HTTP) and req3 (timeout fault) finish on Frame 3!
    // ------------------------------------------------------------------------
    {
        EngineRuntime engine(program);
        RS_CHECK(engine.asyncRuntime() != nullptr);
        auto& asyncCtx = *engine.asyncRuntime();

        DeterminismOptions detOpts;
        detOpts.mode = DeterminismMode::Record;
        detOpts.replayLog = replayLog;
        DeterminismSession recordSession(detOpts);

        const AwaitableHandle hHttp = asyncCtx.awaitables().allocatePending();
        const AwaitableHandle hDisk = asyncCtx.awaitables().allocatePending();
        const AwaitableHandle hPing = asyncCtx.awaitables().allocatePending();
        const AwaitableHandle hSim = asyncCtx.awaitables().allocatePending();

        const AsyncRequestId reqHttp = asyncCtx.completionQueue().registerRequest(
            hHttp, "Http.GetProfile", 0x1001u, BindingDeterminism::Recordable);
        const AsyncRequestId reqDisk = asyncCtx.completionQueue().registerRequest(
            hDisk, "SaveGame.ReadHeader", 0x2002u, BindingDeterminism::Recordable);
        const AsyncRequestId reqPing = asyncCtx.completionQueue().registerRequest(
            hPing, "Matchmaker.Ping", 0x3003u, BindingDeterminism::Recordable);
        const AsyncRequestId reqSim = asyncCtx.completionQueue().registerRequest(
            hSim, "Sim.NavPath", 0x4004u, BindingDeterminism::Deterministic);

        // Frame 0: nothing completed yet
        RuntimeError stepErr{};
        RS_CHECK(asyncCtx.stepPreTickIo(&recordSession, stepErr));
        RS_CHECK(!asyncCtx.awaitables().isCompleted(hHttp));
        RS_CHECK(!asyncCtx.awaitables().isCompleted(hDisk));
        RS_CHECK(!asyncCtx.awaitables().isCompleted(hPing));
        RS_CHECK(!asyncCtx.awaitables().isCompleted(hSim));

        // Frame 1: background thread completes reqDisk & deterministic reqSim first
        std::thread diskThread([&]() {
            asyncCtx.completionQueue().submitBytes(
                reqDisk, std::vector<std::uint8_t>{0xCAu, 0xFEu, 0x01u, 0x02u});
            asyncCtx.completionQueue().submitScalar(reqSim, Value(std::int64_t{77}));
        });
        diskThread.join();

        RS_CHECK(asyncCtx.stepPreTickIo(&recordSession, stepErr));
        RS_CHECK(asyncCtx.awaitables().isCompleted(hDisk));
        RS_CHECK(asyncCtx.awaitables().isCompleted(hSim));
        RS_CHECK(!asyncCtx.awaitables().isCompleted(hHttp));
        RS_CHECK(!asyncCtx.awaitables().isCompleted(hPing));

        // Frame 2: idle frame
        RS_CHECK(asyncCtx.stepPreTickIo(&recordSession, stepErr));
        RS_CHECK(!asyncCtx.awaitables().isCompleted(hHttp));

        // Frame 3: two background threads submit reqPing and reqHttp concurrently
        std::thread netThreadA([&]() {
            asyncCtx.completionQueue().submitFailure(
                reqPing, ErrorCode::ScriptException, "gateway_timeout");
        });
        std::thread netThreadB([&]() {
            asyncCtx.completionQueue().submitString(reqHttp, "{\"player\":\"Aria\",\"lv\":40}");
        });
        netThreadA.join();
        netThreadB.join();

        RS_CHECK(asyncCtx.stepPreTickIo(&recordSession, stepErr));
        RS_CHECK(asyncCtx.awaitables().isCompleted(hHttp));
        RS_CHECK(asyncCtx.awaitables().isCompleted(hPing));

        // Frame 4: post-completion frame
        RS_CHECK(asyncCtx.stepPreTickIo(&recordSession, stepErr));

        // Verify values on Record run
        Value vDisk{}, vHttp{}, vPing{}, vSim{};
        RuntimeError eDisk{}, eHttp{}, ePing{}, eSim{};
        RS_CHECK(asyncCtx.awaitables().getResult(hDisk, vDisk, eDisk) == AwaitStatus::Succeeded);
        RS_CHECK(asyncCtx.awaitables().getResult(hSim, vSim, eSim) == AwaitStatus::Succeeded);
        RS_CHECK(std::get<std::int64_t>(vSim) == 77);
        RS_CHECK(asyncCtx.awaitables().getResult(hHttp, vHttp, eHttp) == AwaitStatus::Succeeded);
        RS_CHECK(asyncCtx.awaitables().getResult(hPing, vPing, ePing) == AwaitStatus::Faulted);
        RS_CHECK(ePing.message == "gateway_timeout");

        RuntimeStatistics dummyStats{};
        RuntimeError finishErr{};
        RS_CHECK(recordSession.finish(true, Value(std::int64_t{40}), finishErr, dummyStats));
        recordedDigest = recordSession.digest();
        RS_CHECK(recordedDigest != 0u);
    }

    // ------------------------------------------------------------------------
    // Phase B: Replay WITHOUT any background threads; verify exact frame-level
    // wakeup ticks, reconstructed ManagedHeap payloads, and bit-exact digest!
    // ------------------------------------------------------------------------
    {
        EngineRuntime replayEngine(program);
        auto& asyncCtx = *replayEngine.asyncRuntime();

        DeterminismOptions detOpts;
        detOpts.mode = DeterminismMode::Replay;
        detOpts.replayLog = replayLog;
        DeterminismSession replaySession(detOpts);

        const AwaitableHandle hHttp = asyncCtx.awaitables().allocatePending();
        const AwaitableHandle hDisk = asyncCtx.awaitables().allocatePending();
        const AwaitableHandle hPing = asyncCtx.awaitables().allocatePending();
        const AwaitableHandle hSim = asyncCtx.awaitables().allocatePending();

        (void)asyncCtx.completionQueue().registerRequest(
            hHttp, "Http.GetProfile", 0x1001u, BindingDeterminism::Recordable);
        (void)asyncCtx.completionQueue().registerRequest(
            hDisk, "SaveGame.ReadHeader", 0x2002u, BindingDeterminism::Recordable);
        (void)asyncCtx.completionQueue().registerRequest(
            hPing, "Matchmaker.Ping", 0x3003u, BindingDeterminism::Recordable);
        (void)asyncCtx.completionQueue().registerRequest(
            hSim, "Sim.NavPath", 0x4004u, BindingDeterminism::Deterministic);

        RuntimeError stepErr{};
        // Frame 0: all still pending
        RS_CHECK(asyncCtx.stepPreTickIo(&replaySession, stepErr));
        RS_CHECK(!asyncCtx.awaitables().isCompleted(hDisk));
        RS_CHECK(!asyncCtx.awaitables().isCompleted(hSim));
        RS_CHECK(!asyncCtx.awaitables().isCompleted(hHttp));
        RS_CHECK(!asyncCtx.awaitables().isCompleted(hPing));

        // Frame 1: ONLY hDisk and hSim wake up!
        RS_CHECK(asyncCtx.stepPreTickIo(&replaySession, stepErr));
        RS_CHECK(asyncCtx.awaitables().isCompleted(hDisk));
        RS_CHECK(asyncCtx.awaitables().isCompleted(hSim));
        RS_CHECK(!asyncCtx.awaitables().isCompleted(hHttp));
        RS_CHECK(!asyncCtx.awaitables().isCompleted(hPing));

        // Frame 2: hHttp and hPing still pending!
        RS_CHECK(asyncCtx.stepPreTickIo(&replaySession, stepErr));
        RS_CHECK(!asyncCtx.awaitables().isCompleted(hHttp));
        RS_CHECK(!asyncCtx.awaitables().isCompleted(hPing));

        // Frame 3: hHttp and hPing wake up together!
        RS_CHECK(asyncCtx.stepPreTickIo(&replaySession, stepErr));
        RS_CHECK(asyncCtx.awaitables().isCompleted(hHttp));
        RS_CHECK(asyncCtx.awaitables().isCompleted(hPing));

        // Frame 4
        RS_CHECK(asyncCtx.stepPreTickIo(&replaySession, stepErr));

        Value vDisk{}, vHttp{}, vPing{}, vSim{};
        RuntimeError eDisk{}, eHttp{}, ePing{}, eSim{};
        RS_CHECK(asyncCtx.awaitables().getResult(hDisk, vDisk, eDisk) == AwaitStatus::Succeeded);
        const ObjectRef diskArr = std::get<ObjectRef>(vDisk);
        RS_CHECK(replayEngine.heap()->arrayLength(diskArr) == 4u);
        RS_CHECK(std::get<std::int64_t>(*replayEngine.heap()->arrayGet(diskArr, 0)) == 0xCA);
        RS_CHECK(std::get<std::int64_t>(*replayEngine.heap()->arrayGet(diskArr, 1)) == 0xFE);

        RS_CHECK(asyncCtx.awaitables().getResult(hSim, vSim, eSim) == AwaitStatus::Succeeded);
        RS_CHECK(std::get<std::int64_t>(vSim) == 77);

        RS_CHECK(asyncCtx.awaitables().getResult(hHttp, vHttp, eHttp) == AwaitStatus::Succeeded);
        const ObjectRef httpStr = std::get<ObjectRef>(vHttp);
        RS_CHECK(replayEngine.heap()->stringView(httpStr) == "{\"player\":\"Aria\",\"lv\":40}");

        RS_CHECK(asyncCtx.awaitables().getResult(hPing, vPing, ePing) == AwaitStatus::Faulted);
        RS_CHECK(ePing.code == ErrorCode::ScriptException);
        RS_CHECK(ePing.message == "gateway_timeout");

        RuntimeStatistics dummyStats{};
        RuntimeError finishErr{};
        RS_CHECK(replaySession.finish(true, Value(std::int64_t{40}), finishErr, dummyStats));
        RS_CHECK(replaySession.digest() == recordedDigest);
    }

    // ------------------------------------------------------------------------
    // Phase C: Tampered request metadata during Replay -> ErrorCode::ReplayMismatch
    // ------------------------------------------------------------------------
    {
        EngineRuntime mismatchEngine(program);
        auto& asyncCtx = *mismatchEngine.asyncRuntime();

        DeterminismOptions detOpts;
        detOpts.mode = DeterminismMode::Replay;
        detOpts.replayLog = replayLog;
        DeterminismSession mismatchSession(detOpts);

        const AwaitableHandle hHttp = asyncCtx.awaitables().allocatePending();
        const AwaitableHandle hDisk = asyncCtx.awaitables().allocatePending();
        (void)asyncCtx.completionQueue().registerRequest(
            hHttp, "Http.GetProfile", 0x1001u, BindingDeterminism::Recordable);
        // Divergent argument hash on SaveGame.ReadHeader!
        (void)asyncCtx.completionQueue().registerRequest(
            hDisk, "SaveGame.ReadHeader", 0x9999u, BindingDeterminism::Recordable);

        RuntimeError err{};
        RS_CHECK(asyncCtx.stepPreTickIo(&mismatchSession, err)); // Frame 0 (0 completions)
        RS_CHECK(!asyncCtx.stepPreTickIo(&mismatchSession, err)); // Frame 1 -> ReplayMismatch!
        RS_CHECK(err.code == ErrorCode::ReplayMismatch);
    }

    // ------------------------------------------------------------------------
    // Phase D: DeterminismMode::Strict rejects non-deterministic async I/O,
    // while permitting BindingDeterminism::Deterministic operations
    // ------------------------------------------------------------------------
    {
        EngineRuntime strictEngine(program);
        auto& asyncCtx = *strictEngine.asyncRuntime();

        DeterminismOptions strictOpts;
        strictOpts.mode = DeterminismMode::Strict;
        DeterminismSession strictSession(strictOpts);

        const AwaitableHandle detHandle = asyncCtx.awaitables().allocatePending();
        const AsyncRequestId detReq = asyncCtx.completionQueue().registerRequest(
            detHandle, "Sim.DeterministicSubstep", 1u, BindingDeterminism::Deterministic);
        asyncCtx.completionQueue().submitScalar(detReq, Value(std::int64_t{12}));

        RuntimeError err{};
        RS_CHECK(asyncCtx.stepPreTickIo(&strictSession, err));
        Value detVal{};
        RS_CHECK(asyncCtx.awaitables().getResult(detHandle, detVal, err) ==
                 AwaitStatus::Succeeded);
        RS_CHECK(std::get<std::int64_t>(detVal) == 12);

        // Canceled request auto-prunes on drain without leaking in pendingRequests_
        const CancellationSourceHandle cancelCts = asyncCtx.cancellations().createSource();
        const AwaitableHandle canceledHandle = asyncCtx.awaitables().allocatePending(
            ResumeQueue::PreTickIo, asyncCtx.cancellations().token(cancelCts));
        (void)asyncCtx.completionQueue().registerRequest(
            canceledHandle, "Sim.CanceledSubstep", 9u, BindingDeterminism::Deterministic);
        RS_CHECK(asyncCtx.completionQueue().pendingRequestCount() == 1u);
        asyncCtx.cancellations().cancel(cancelCts);
        RS_CHECK(asyncCtx.stepPreTickIo(&strictSession, err));
        RS_CHECK(asyncCtx.completionQueue().pendingRequestCount() == 0u);
        RS_CHECK(asyncCtx.awaitables().discard(canceledHandle));

        const AwaitableHandle nondetHandle1 = asyncCtx.awaitables().allocatePending();
        const AwaitableHandle nondetHandle2 = asyncCtx.awaitables().allocatePending();
        (void)asyncCtx.completionQueue().registerRequest(
            nondetHandle1, "Http.FirstViolation", 2u, BindingDeterminism::Recordable);
        (void)asyncCtx.completionQueue().registerRequest(
            nondetHandle2, "Http.SecondViolation", 3u, BindingDeterminism::Recordable);
        RS_CHECK(!asyncCtx.stepPreTickIo(&strictSession, err));
        RS_CHECK(err.code == ErrorCode::DeterminismViolation);
        RS_CHECK(err.message.find("Http.FirstViolation") != std::string::npos);
    }
}

} // namespace

int main() {
    testZeroAllocSteadyStateAndBenchmark();
    testGenerationalAbaSafetyAndSingleAwaiter();
    testManagedHeapGcRootPinningAndRelease();
    testHierarchicalCancellationAndEntityDestroyTokens();
    testWhenAllAndWhenAnyCombinators();
    testMultiThreadedAsyncCompletionQueue();
    testDeterministicRecordReplayAndStrictMode();
    std::cout << "All Phase 25 unified async primitive tests passed!\n";
    return 0;
}
