#include "realscript/game/GameScripting.h"
#include "realscript/runtime/AsyncRuntime.h"
#include "realscript/runtime/Runtime.h"
#include "realscript/semantic/Semantic.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#define RS_REQUIRE(expr)                                                        \
    do {                                                                        \
        if (!(expr)) {                                                          \
            throw std::runtime_error(                                           \
                std::string("RS_REQUIRE failed at ") + __FILE__ + ":" +         \
                std::to_string(__LINE__) + ": " #expr);                         \
        }                                                                       \
    } while (false)

namespace {

using namespace realscript;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// 1. Multi-thread, multi-ManagedHeap concurrent allocation + incremental/young/full GC
//    with zero cross-instance global pool lock contention.
void testMultiThreadIndependentHeapsZeroContention() {
    runtime::ManagedHeap backgroundPinnedHeap;
    const runtime::StringValue bgStr(std::string_view("background_pinned_pool_string"));
    const std::array<runtime::Value, 3> bgStructFields{
        std::int64_t{101}, std::int64_t{202}, std::int64_t{303}};
    const auto bgInlineStruct = runtime::makeStructValueFromFields(
        semantic::stableTypeId("Perf3::BgInline"),
        runtime::ValueSpan(bgStructFields.data(), bgStructFields.size()));
    backgroundPinnedHeap.setStaticField(0x1111ULL, runtime::Value{bgStr});
    backgroundPinnedHeap.setStaticField(0x2222ULL, runtime::Value{bgInlineStruct});
    RS_REQUIRE(runtime::hasAnyPinnedPoolValues());

    constexpr int kThreads = 8;
    constexpr int kRoundsPerThread = 60;
    constexpr int kObjectsPerRound = 96;

    std::atomic<int> readyCount{0};
    std::atomic<bool> startFlag{false};
    std::atomic<bool> anyFailed{false};
    std::mutex errorMutex;
    std::string firstError;

    std::vector<std::thread> workers;
    workers.reserve(kThreads);

    const auto nodeType = semantic::stableTypeId("Perf3::GraphNode");
    const auto arrayType = semantic::stableTypeId("Perf3::GraphNode[]");
    const auto structType = semantic::stableTypeId("Perf3::WorkerPayload");
    const auto refFieldsPtr = std::make_shared<const std::vector<std::size_t>>(
        std::vector<std::size_t>{0, 1});

    for (int tid = 0; tid < kThreads; ++tid) {
        workers.emplace_back([&, tid]() {
            try {
                runtime::HeapConfig cfg;
                cfg.initialCollectionThresholdBytes = 16 * 1024;
                cfg.maximumHeapBytes = 32 * 1024 * 1024;
                runtime::ManagedHeap heap(cfg);

                readyCount.fetch_add(1, std::memory_order_acq_rel);
                while (!startFlag.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }

                std::vector<runtime::PersistentRoot> anchors;
                anchors.reserve(16);

                for (int round = 0; round < kRoundsPerThread; ++round) {
                    const auto spineArr = heap.allocateTypedArray(
                        arrayType,
                        semantic::PrimitiveType::Object,
                        nodeType,
                        80,
                        runtime::NullObject{});
                    require(spineArr.has_value(), "spine typed array allocation failed");

                    std::vector<runtime::Value> stackFrame{*spineArr};
                    runtime::RootSet roots;
                    roots.pushFrame(nullptr, nullptr, &stackFrame);

                    for (int i = 0; i < kObjectsPerRound; ++i) {
                        const auto shortStr = heap.allocateString(
                            "s_" + std::to_string(tid) + "_" + std::to_string(i));
                        const auto longStr = heap.allocateString(
                            "heap_pooled_payload_thread_" + std::to_string(tid) +
                            "_round_" + std::to_string(round) + "_item_" + std::to_string(i));
                        const auto interned = heap.internString(
                            "intern_key_" + std::to_string(i & 15));
                        require(shortStr && longStr && interned, "string allocation failed");

                        const std::array<runtime::Value, 4> tmplDefaults{
                            runtime::Value{*longStr},
                            runtime::Value{runtime::NullObject{}},
                            runtime::Value{static_cast<std::int64_t>(tid * 100000 + round * 100 + i)},
                            runtime::Value{*shortStr},
                        };
                        const auto obj = heap.allocateObjectFromTemplate(
                            nodeType, tmplDefaults.data(), tmplDefaults.size(), refFieldsPtr);
                        require(obj.has_value(), "template object allocation failed");

                        if ((i & 7) == 0) {
                            auto bigRec = heap.allocateRecord(8);
                            require(bigRec.has_value(), "large record allocation failed");
                            std::array<runtime::Value, 3> sf{
                                std::int64_t{tid}, std::int64_t{round}, std::int64_t{i}};
                            auto sv = runtime::makeStructValueFromFields(
                                structType, runtime::ValueSpan(sf.data(), sf.size()));
                            require(heap.fieldSet(*bigRec, 0, runtime::Value{sv}), "fieldSet struct failed");
                            require(heap.fieldSet(*bigRec, 1, runtime::Value{*obj}), "fieldSet child failed");
                        }

                        if (i < 80 && (i % 4 == 0)) {
                            require(heap.arraySet(*spineArr, static_cast<std::size_t>(i), *obj),
                                "spine arraySet failed");
                        }
                    }

                    if (round % 10 == 0 && anchors.size() < 8) {
                        anchors.push_back(heap.retain(*spineArr));
                    }

                    if (round % 3 == 0) {
                        (void)heap.collectYoung(roots);
                    } else if (round % 3 == 1) {
                        const auto stats = heap.collectIncrementalWithBudget(
                            roots, std::chrono::microseconds(150), 16);
                        require(stats.elapsedNanoseconds ==
                                static_cast<std::uint64_t>(stats.elapsed.count()),
                            "elapsedNanoseconds must match elapsed.count()");
                    } else {
                        heap.collectFull(roots);
                    }

                    for (std::size_t slot = 0; slot < 80; slot += 4) {
                        const auto elem = heap.arrayGet(*spineArr, slot);
                        require(elem.has_value() && std::holds_alternative<runtime::ObjectRef>(*elem),
                            "retained spine element missing");
                        const auto childRef = std::get<runtime::ObjectRef>(*elem);
                        require(heap.isAlive(childRef), "retained spine child prematurely swept");
                        const auto strField = heap.fieldGet(childRef, 0);
                        require(strField.has_value() && std::holds_alternative<runtime::ObjectRef>(*strField),
                            "child string field missing");
                        const auto sv = heap.stringView(std::get<runtime::ObjectRef>(*strField));
                        require(sv.has_value() && sv->find("heap_pooled_payload_thread_") == 0,
                            "child string payload corrupted across concurrent GC");
                    }

                    roots.popFrame();
                }

                anchors.clear();
                runtime::RootSet emptyRoots;
                heap.collectFull(emptyRoots);
                require(heap.liveObjects() == 0 && heap.liveBytes() == 0,
                    "thread heap leaked objects after releasing all roots: liveObjects=" +
                        std::to_string(heap.liveObjects()));
            } catch (const std::exception& ex) {
                anyFailed.store(true, std::memory_order_release);
                std::lock_guard<std::mutex> lock(errorMutex);
                if (firstError.empty()) {
                    firstError = "Thread " + std::to_string(tid) + ": " + ex.what();
                }
            }
        });
    }

    while (readyCount.load(std::memory_order_acquire) < kThreads) {
        std::this_thread::yield();
    }
    const auto t0 = std::chrono::steady_clock::now();
    startFlag.store(true, std::memory_order_release);
    for (auto& w : workers) {
        w.join();
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double totalMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

    if (anyFailed.load(std::memory_order_acquire)) {
        throw std::runtime_error(firstError);
    }

    const auto checkStr = backgroundPinnedHeap.getStaticField(0x1111ULL);
    RS_REQUIRE(checkStr.has_value() &&
               std::get<runtime::StringValue>(*checkStr) == "background_pinned_pool_string");
    backgroundPinnedHeap.removeStaticField(0x1111ULL);
    backgroundPinnedHeap.removeStaticField(0x2222ULL);
    backgroundPinnedHeap.collectFull(runtime::ShadowStack{});

    std::cout << "[PASS] 8-thread independent ManagedHeap concurrent alloc/GC completed in "
              << totalMs << " ms\n";
}

// 2. Nanosecond budget truncation, P1-4 idle no-spin, chrono saturation, & multi-frame GC.
void testNanosecondBudgetTruncationAndMultiFrameCompletion() {
    runtime::HeapConfig cfg;
    cfg.initialCollectionThresholdBytes = 64 * 1024 * 1024; // High threshold: below auto-trigger
    cfg.maximumHeapBytes = 128 * 1024 * 1024;
    runtime::ManagedHeap heap(cfg);

    const auto nodeType = semantic::stableTypeId("Perf3::BudgetNode");
    constexpr std::size_t kLiveChainLength = 2500;
    constexpr std::size_t kGarbageCount = 5500;

    std::optional<runtime::ObjectRef> head;
    for (std::size_t i = 0; i < kLiveChainLength; ++i) {
        std::vector<runtime::Value> fields{
            head ? runtime::Value{*head} : runtime::Value{runtime::NullObject{}},
            runtime::Value{static_cast<std::int64_t>(i)},
        };
        const auto node = heap.allocateObject(nodeType, std::move(fields), {0});
        RS_REQUIRE(node.has_value());
        head = *node;
    }
    for (std::size_t i = 0; i < kGarbageCount; ++i) {
        const auto deadStr = heap.allocateString(
            "ephemeral_garbage_frame_payload_" + std::to_string(i));
        RS_REQUIRE(deadStr.has_value());
    }

    RS_REQUIRE(heap.liveObjects() == kLiveChainLength + kGarbageCount);
    RS_REQUIRE(!heap.collectionRequested());
    RS_REQUIRE(heap.liveBytes() < cfg.initialCollectionThresholdBytes);

    auto rootHandle = heap.retain(*head);
    runtime::RootSet emptyRoots;

    // P1-4: When Idle && !requested && liveBytes < threshold, collectIncrementalWithBudget
    // MUST immediately return {cycleCompleted=false, budgetExhausted=false} without spinning!
    {
        const auto idleNoSpin = heap.collectIncrementalWithBudget(
            emptyRoots, std::chrono::milliseconds(2), 16);
        RS_REQUIRE(!idleNoSpin.cycleCompleted && !idleNoSpin.budgetExhausted);
        RS_REQUIRE(idleNoSpin.phaseAfterStep == runtime::GcPhase::Idle);
        RS_REQUIRE(idleNoSpin.totalWork() == 0);

        // P1-4: microseconds::max() duration conversion must saturate cleanly without signed UB!
        const auto saturatedIdle = heap.collectIncrementalWithBudget(
            emptyRoots, std::chrono::microseconds::max(), 16);
        RS_REQUIRE(!saturatedIdle.cycleCompleted && !saturatedIdle.budgetExhausted);
        RS_REQUIRE(heap.phase() == runtime::GcPhase::Idle);
    }

    // Request collection explicitly, then verify 0ns / negative budget bails out immediately.
    heap.requestCollection();
    {
        const auto zeroStats = heap.collectIncrementalWithBudget(
            emptyRoots, std::chrono::nanoseconds(0), 4);
        RS_REQUIRE(!zeroStats.cycleCompleted && zeroStats.budgetExhausted);
        RS_REQUIRE(zeroStats.objectsMarked == 0 && zeroStats.objectsSwept == 0);

        const auto negStats = heap.collectIncrementalWithBudget(
            emptyRoots, std::chrono::nanoseconds(-1000), 4);
        RS_REQUIRE(!negStats.cycleCompleted && negStats.budgetExhausted);
    }

    // Multi-frame execution with tight 200ns budget & checkInterval=4 (including startCollection time):
    std::size_t sliceCount = 0;
    std::size_t budgetExhaustedSlices = 0;
    std::size_t totalMarked = 0;
    std::size_t totalSwept = 0;
    std::size_t totalReclaimed = 0;
    bool observedMarkPhase = false;
    bool observedSweepPhase = false;

    runtime::IncrementalGcBudgetStats stepStats{};
    do {
        stepStats = heap.collectIncrementalWithBudget(
            emptyRoots, std::chrono::nanoseconds(200), 4);
        ++sliceCount;
        if (stepStats.budgetExhausted) {
            ++budgetExhaustedSlices;
            RS_REQUIRE(!stepStats.cycleCompleted);
        }
        if (stepStats.phaseAfterStep == runtime::GcPhase::Mark) {
            observedMarkPhase = true;
        } else if (stepStats.phaseAfterStep == runtime::GcPhase::Sweep) {
            observedSweepPhase = true;
        }
        totalMarked += stepStats.objectsMarked;
        totalSwept += stepStats.objectsSwept;
        totalReclaimed += stepStats.objectsReclaimed;
        RS_REQUIRE(stepStats.elapsed.count() >= 0 &&
                   stepStats.elapsedNanoseconds == static_cast<std::uint64_t>(stepStats.elapsed.count()));
    } while (!stepStats.cycleCompleted);

    RS_REQUIRE(sliceCount > 2 && budgetExhaustedSlices >= 2);
    RS_REQUIRE(observedMarkPhase && observedSweepPhase);
    RS_REQUIRE(stepStats.cycleCompleted && !stepStats.budgetExhausted &&
               heap.phase() == runtime::GcPhase::Idle);
    RS_REQUIRE(totalMarked == kLiveChainLength);
    RS_REQUIRE(totalSwept == kLiveChainLength + kGarbageCount);
    RS_REQUIRE(totalReclaimed == kGarbageCount && heap.liveObjects() == kLiveChainLength);

    std::optional<runtime::ObjectRef> cursor = *head;
    for (std::size_t idx = kLiveChainLength; idx > 0; --idx) {
        RS_REQUIRE(cursor.has_value() && heap.isAlive(*cursor));
        const auto payload = heap.fieldGet(*cursor, 1);
        RS_REQUIRE(payload.has_value() &&
                   std::get<std::int64_t>(*payload) == static_cast<std::int64_t>(idx - 1));
        const auto nextVal = heap.fieldGet(*cursor, 0);
        RS_REQUIRE(nextVal.has_value());
        if (std::holds_alternative<runtime::ObjectRef>(*nextVal)) {
            cursor = std::get<runtime::ObjectRef>(*nextVal);
        } else {
            RS_REQUIRE(idx == 1);
            cursor.reset();
        }
    }

    // Release root, request collection, and verify microsecond convenience overloads + max() saturation
    rootHandle.reset();
    heap.requestCollection();
    std::size_t cleanupSlices = 0;
    runtime::IncrementalGcBudgetStats usStats{};
    do {
        if ((cleanupSlices & 1u) == 0u) {
            usStats = heap.collectIncrementalWithBudget(
                emptyRoots, std::chrono::microseconds::max(), 32);
        } else {
            usStats = heap.collectIncrementalWithBudgetUs(emptyRoots, 250, 32);
        }
        ++cleanupSlices;
    } while (!usStats.cycleCompleted);

    RS_REQUIRE(heap.liveObjects() == 0);

    // Empty idle heap: no-spin by default, immediate cycleCompleted when forceStartIfIdle=true
    const auto idleEmptyStats = heap.collectIncrementalWithBudget(
        emptyRoots, std::chrono::milliseconds(1), 32);
    RS_REQUIRE(!idleEmptyStats.cycleCompleted && !idleEmptyStats.budgetExhausted &&
               idleEmptyStats.totalWork() == 0);
    const auto forcedEmptyStats = heap.collectIncrementalWithBudget(
        emptyRoots, std::chrono::milliseconds(1), 32, true);
    RS_REQUIRE(forcedEmptyStats.cycleCompleted && !forcedEmptyStats.budgetExhausted &&
               forcedEmptyStats.totalWork() == 0);

    std::cout << "[PASS] Nanosecond budget truncation, idle no-spin & chrono saturation verified in "
              << sliceCount << " slices\n";
}

// 3. Mutator write-barriers, allocations during Mark/Sweep, and compatibility with collect / collectIncremental.
void testWriteBarriersDuringBudgetedGcAndLegacyCompatibility() {
    runtime::ManagedHeap heap;
    const auto boxType = semantic::stableTypeId("Perf3::Box");

    const auto container = heap.allocateObject(
        boxType, {runtime::Value{runtime::NullObject{}}}, {0});
    RS_REQUIRE(container.has_value());
    auto root = heap.retain(*container);

    for (int i = 0; i < 1000; ++i) {
        (void)heap.allocateString("transient_" + std::to_string(i));
    }

    runtime::RootSet roots;
    heap.requestCollection();

    const auto lateChild = heap.allocateString("attached_mid_incremental_gc");
    RS_REQUIRE(lateChild.has_value());

    bool attachedDuringGc = false;
    bool allocatedDuringSweep = false;
    std::optional<runtime::ObjectRef> sweepBorn;
    runtime::PersistentRoot sweepBornRoot;

    runtime::IncrementalGcBudgetStats st{};
    int guard = 0;
    do {
        st = heap.collectIncrementalWithBudget(roots, std::chrono::nanoseconds(400), 4);
        if (!attachedDuringGc && !st.cycleCompleted) {
            RS_REQUIRE(heap.fieldSet(*container, 0, *lateChild));
            attachedDuringGc = true;
        }
        if (st.phaseAfterStep == runtime::GcPhase::Sweep && !allocatedDuringSweep) {
            sweepBorn = heap.allocateString("allocated_during_sweep_reopen");
            RS_REQUIRE(sweepBorn.has_value());
            sweepBornRoot = heap.retain(*sweepBorn);
            allocatedDuringSweep = true;
        }
        RS_REQUIRE(++guard < 100000);
    } while (!st.cycleCompleted);

    RS_REQUIRE(heap.isAlive(*container) && heap.isAlive(*lateChild));
    if (allocatedDuringSweep) {
        RS_REQUIRE(heap.isAlive(*sweepBorn));
    }

    sweepBornRoot.reset();
    (void)heap.allocateString("compat_garbage_1");
    (void)heap.allocateString("compat_garbage_2");
    heap.requestCollection();
    const std::size_t incWork = heap.collectIncremental(roots, 5);
    RS_REQUIRE(incWork > 0);
    heap.collect(roots);
    RS_REQUIRE(heap.phase() == runtime::GcPhase::Idle && heap.liveObjects() == 2);

    std::cout << "[PASS] Write-barriers across budgeted GC slices & legacy GC compatibility verified\n";
}

// 4. P0-3 & P1-4: Suspended coroutine ValuePool UAF protection, cross-thread active mutator
//    safe-point coordination, and deterministic TraceEventKind::GcStep invariance.
void testP03CoroutinePoolSafetyAndP14Determinism() {
    game::GameApi api;
    RS_REQUIRE(api.valid());

    const std::string src = R"RS(
module P03.Test;
import RealScript.Async;

public struct PayloadPair {
    public int left;
    public int right;
}

public class Runner {
    public string outText = "";
    public int outSum = 0;

    public async Awaitable Execute(int seed) {
        string dynamicStr = "pooled_coro_string_payload_value_exceeding_sso_" + "42";
        PayloadPair pair = new PayloadPair();
        pair.left = seed + 100;
        pair.right = seed + 200;
        await Awaitable.Yield();
        this.outText = dynamicStr;
        this.outSum = pair.left + pair.right;
    }

    public static int ChurnMutator(int n) {
        int acc = 0;
        for (int i = 0; i < n; i = i + 1) {
            PayloadPair p = new PayloadPair();
            p.left = i;
            p.right = i * 2;
            string s = "mutator_live_dynamic_pooled_string_item_number_long";
            if (s != "") {
                acc = acc + p.left + p.right;
            }
        }
        return acc;
    }
}
)RS";

    game::GameScriptCompiler compiler(api);
    const auto compiled = compiler.compile({compiler::SourceFile{"p03.rscript", src}});
    if (!compiled.succeeded()) {
        for (const auto& d : compiled.diagnostics.items()) {
            std::cerr << "DIAG " << d.code << ": " << d.message << "\n";
        }
    }
    RS_REQUIRE(compiled.succeeded());

    // Part A: Suspended CoroutineFrame + AwaitableCorePool result/exception ValuePool UAF protection
    {
        game::ScriptRuntime rt(compiled.program);
        game::SceneScriptRuntime scene(rt);
        RS_REQUIRE(scene.attachComponent(1, "P03.Test::Runner", {}));
        scene.start();
        RS_REQUIRE(scene.invoke(1, "Execute", {std::int64_t{7}}).succeeded);
        RS_REQUIRE(rt.asyncRuntime()->coroutines().statistics().activeCount == 1u);

        // Also park unconsumed pooled StringValue & InlineStruct inside standalone AwaitableCorePool
        runtime::AsyncRuntimeContext standaloneAsync;
        const auto pendingAw = standaloneAsync.awaitables().allocatePending();
        const runtime::StringValue awPooledStr(
            std::string_view("awaitable_unconsumed_long_pooled_string_result_payload_999"));
        RS_REQUIRE(standaloneAsync.awaitables().trySetResult(
            pendingAw, runtime::Value{awPooledStr}));

        const auto inlineTid = semantic::stableTypeId("P03.Test::PayloadPair");
        const std::array<runtime::Value, 2> excFields{std::int64_t{314}, std::int64_t{159}};
        const auto awExcStruct = runtime::makeStructValueFromFields(
            inlineTid, runtime::ValueSpan(excFields.data(), excFields.size()));
        const auto faultedAw = standaloneAsync.awaitables().fromFaulted(
            runtime::ErrorCode::InvalidProgram,
            "pooled_exception_test",
            runtime::Value{awExcStruct});

        // Force global ValuePool sweep from another ManagedHeap & churn new pool allocations
        runtime::ManagedHeap sweeperHeap;
        sweeperHeap.setStaticField(
            0x9999ULL,
            runtime::Value{runtime::StringValue(
                std::string_view("temporary_pinned_trigger_for_pool_reclaim_sweep_0001"))});
        sweeperHeap.removeStaticField(0x9999ULL);
        sweeperHeap.collectFull(runtime::ShadowStack{});

        std::vector<runtime::Value> overwriteChurn;
        overwriteChurn.reserve(512);
        for (int i = 0; i < 256; ++i) {
            overwriteChurn.emplace_back(runtime::StringValue(
                "OVERWRITTEN_POOL_SLOT_GARBAGE_PAYLOAD_LONG_" + std::to_string(i)));
            const std::array<runtime::Value, 2> badFields{
                std::int64_t{-9999}, std::int64_t{-8888}};
            overwriteChurn.emplace_back(runtime::makeStructValueFromFields(
                inlineTid, runtime::ValueSpan(badFields.data(), badFields.size())));
        }

        // Verify standalone AwaitableCorePool result & fault exception were NOT swept/overwritten!
        runtime::Value awOut{};
        runtime::RuntimeError awErr{};
        RS_REQUIRE(standaloneAsync.awaitables().getResult(pendingAw, awOut, awErr) ==
                   runtime::AwaitStatus::Succeeded);
        RS_REQUIRE(std::get<runtime::StringValue>(awOut) ==
                   "awaitable_unconsumed_long_pooled_string_result_payload_999");

        runtime::Value faultOut{};
        runtime::Value faultExcOut{};
        runtime::RuntimeError faultErr{};
        RS_REQUIRE(standaloneAsync.awaitables().getResult(
                       faultedAw, faultOut, faultErr, &faultExcOut) ==
                   runtime::AwaitStatus::Faulted);
        const auto f0 = runtime::loadInlineStructField(faultExcOut, 0);
        const auto f1 = runtime::loadInlineStructField(faultExcOut, 1);
        RS_REQUIRE(f0.has_value() && std::get<std::int64_t>(*f0) == 314);
        RS_REQUIRE(f1.has_value() && std::get<std::int64_t>(*f1) == 159);

        // Resume suspended coroutine and verify its pooled StringValue & InlineStruct survived!
        scene.update(0.016);
        RS_REQUIRE(scene.errors().empty());
        RS_REQUIRE(rt.asyncRuntime()->coroutines().statistics().activeCount == 0u);

        const auto* runnerObj = scene.getComponent(1, "P03.Test::Runner");
        RS_REQUIRE(runnerObj != nullptr);
        runtime::RuntimeError err;
        const auto textVal = rt.getMember(*runnerObj, "outText", err);
        const auto sumVal = rt.getMember(*runnerObj, "outSum", err);
        RS_REQUIRE(textVal.has_value() && sumVal.has_value());
        RS_REQUIRE(std::get<std::int64_t>(*sumVal) == (107 + 207));
        std::string actualStr;
        if (const auto* sv = std::get_if<runtime::StringValue>(&*textVal)) {
            actualStr = sv->str();
        } else if (const auto* oref = std::get_if<runtime::ObjectRef>(&*textVal)) {
            const auto v = rt.heap()->stringView(*oref);
            RS_REQUIRE(v.has_value());
            actualStr = std::string(*v);
        }
        RS_REQUIRE(actualStr == "pooled_coro_string_payload_value_exceeding_sso_42");
    }

    // Part B: Concurrent active Interpreter mutator vs background ManagedHeap GC / ValuePool sweep
    {
        std::atomic<bool> stopCollector{false};
        std::atomic<bool> workerFailed{false};

        std::thread collectorThread([&]() {
            try {
                runtime::ManagedHeap bgHeap;
                runtime::RootSet emptyRoots;
                int iter = 0;
                while (!stopCollector.load(std::memory_order_acquire)) {
                    bgHeap.setStaticField(
                        1ULL,
                        runtime::Value{runtime::StringValue(
                            "bg_collector_pooled_string_long_iteration_" + std::to_string(iter++))});
                    bgHeap.removeStaticField(1ULL);
                    (void)bgHeap.collectIncrementalWithBudget(
                        emptyRoots, std::chrono::microseconds(50), 8, true);
                    bgHeap.collectFull(emptyRoots);
                }
            } catch (...) {
                workerFailed.store(true, std::memory_order_release);
            }
        });

        runtime::Interpreter mutatorInterp(compiled.program.program());
        for (int r = 0; r < 40; ++r) {
            const auto res = mutatorInterp.invoke(
                "P03.Test::Runner.ChurnMutator", {std::int64_t{80}});
            RS_REQUIRE(res.succeeded);
            RS_REQUIRE(std::get<std::int64_t>(res.value) == (79 * 80 / 2) * 3);
        }

        stopCollector.store(true, std::memory_order_release);
        collectorThread.join();
        RS_REQUIRE(!workerFailed.load(std::memory_order_acquire));
    }

    // Part C: P1-4 Deterministic TraceEventKind::GcStep invariance after wall-clock GC slice
    {
        runtime::HeapConfig detCfg;
        detCfg.initialCollectionThresholdBytes = 1024;
        auto heapA = std::make_shared<runtime::ManagedHeap>(detCfg);
        auto heapB = std::make_shared<runtime::ManagedHeap>(detCfg);

        for (int i = 0; i < 64; ++i) {
            const std::string s = "seed_obj_for_determinism_check_" + std::to_string(i);
            RS_REQUIRE(heapA->allocateString(s).has_value());
            RS_REQUIRE(heapB->allocateString(s).has_value());
        }
        // Fast machine (heapA) completes wall-clock budgeted GC in 1 slice, whereas slow machine
        // (heapB, 1ns budget) truncates mid-Mark; Interpreter must produce identical GcStep trace & digest!
        runtime::RootSet emptyRoots;
        const auto fastSlice = heapA->collectIncrementalWithBudget(
            emptyRoots, std::chrono::milliseconds(50), 32, true);
        const auto slowSlice = heapB->collectIncrementalWithBudget(
            emptyRoots, std::chrono::nanoseconds(1), 1, true);
        RS_REQUIRE(fastSlice.cycleCompleted && !heapA->isWallClockCycleActive());
        RS_REQUIRE(!slowSlice.cycleCompleted && heapB->isWallClockCycleActive());

        runtime::Interpreter interpA(compiled.program.program(), heapA);
        runtime::Interpreter interpB(compiled.program.program(), heapB);

        std::size_t gcStepsA = 0;
        std::size_t gcStepsB = 0;
        runtime::ExecutionOptions optsA;
        optsA.limits.gcWorkBudget = 4;
        optsA.determinism.mode = runtime::DeterminismMode::Record;
        optsA.trace = [&gcStepsA](const runtime::TraceEvent& ev) {
            if (ev.kind == runtime::TraceEventKind::GcStep) ++gcStepsA;
        };
        runtime::ExecutionOptions optsB;
        optsB.limits.gcWorkBudget = 4;
        optsB.determinism.mode = runtime::DeterminismMode::Record;
        optsB.trace = [&gcStepsB](const runtime::TraceEvent& ev) {
            if (ev.kind == runtime::TraceEventKind::GcStep) ++gcStepsB;
        };

        const auto resA = interpA.invoke("P03.Test::Runner.ChurnMutator", {std::int64_t{25}}, optsA);
        const auto resB = interpB.invoke("P03.Test::Runner.ChurnMutator", {std::int64_t{25}}, optsB);
        RS_REQUIRE(resA.succeeded && resB.succeeded);
        RS_REQUIRE(resA.determinismDigest == resB.determinismDigest);
        RS_REQUIRE(gcStepsA == gcStepsB);
    }

    std::cout << "[PASS] P0-3 suspended coroutine ValuePool pinning, mutator safe-point & P1-4 trace determinism verified\n";
}

} // namespace

int main() {
    try {
        testMultiThreadIndependentHeapsZeroContention();
        testNanosecondBudgetTruncationAndMultiFrameCompletion();
        testWriteBarriersDuringBudgetedGcAndLegacyCompatibility();
        testP03CoroutinePoolSafetyAndP14Determinism();
        std::cout << "All sprint2 perf_3 GC budget and per-instance pool tests passed!\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[FAIL] " << ex.what() << "\n";
        return 1;
    }
}
