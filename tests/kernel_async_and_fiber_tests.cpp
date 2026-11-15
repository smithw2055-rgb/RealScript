#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/optimization/Optimizer.h"
#include "realscript/runtime/AsyncRuntime.h"
#include "realscript/runtime/FiberScheduler.h"
#include "realscript/runtime/ParallelJob.h"
#include "realscript/runtime/Runtime.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {

using namespace realscript;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string formatDiags(const diagnostics::DiagnosticBag& bag) {
    std::ostringstream oss;
    for (const auto& d : bag.items()) {
        oss << d.sourceName << " [" << d.code << "] " << d.message << '\n';
    }
    return oss.str();
}

struct CompiledBundle {
    std::vector<mir::Module> mirModules;
    std::shared_ptr<const runtime::ProgramImage> program;
};

CompiledBundle compileAndLink(
    std::vector<compiler::SourceFile> sources,
    compiler::CompilationOptions compOpts = {},
    optimization::Level optLevel = optimization::Level::Basic) {
    compiler::Compilation comp(std::move(sources), std::move(compOpts));
    auto build = comp.build();
    require(!build.diagnostics.hasErrors(), "Compile failed:\n" + formatDiags(build.diagnostics));

    diagnostics::DiagnosticBag optDiag;
    optimization::Options optOpts;
    optOpts.level = optLevel;
    auto optimized = optimization::Optimizer{}.optimize(std::move(build.modules), optDiag, optOpts);
    require(!optDiag.hasErrors(), "Optimization failed");

    bytecode::Lowerer lowerer;
    std::vector<bytecode::Module> bcMods;
    bcMods.reserve(optimized.modules.size());
    for (const auto& m : optimized.modules) {
        bcMods.push_back(lowerer.lower(m));
    }

    runtime::RuntimeError linkErr;
    auto linked = runtime::ProgramImage::link(std::move(bcMods), linkErr);
    require(linked.has_value(), "Link failed: " + linkErr.message);
    return CompiledBundle{std::move(optimized.modules),
                          std::make_shared<const runtime::ProgramImage>(std::move(*linked))};
}

// ---------------------------------------------------------------------------
// 1. Sequence Coroutines: yield wait_ticks, yield break, locals & rollback
// ---------------------------------------------------------------------------
void testSequenceCoroutinesAndExceptionRollback() {
    const std::string src = R"RS(
module Kernel.SeqTest;

public long Schedule(long target, string callback, int delay) {
    return target + delay;
}

public bool CancelTimer(long timer) {
    return timer != 0;
}

public class SeqFault {
    public string msg;
    public SeqFault(string m) { msg = m; }
}

public class SeqActor {
    public int value = 0;
    public bool cleaned = false;

    public sequence Run(long target, bool earlyExit) {
        int acc = 10;
        yield wait_ticks(1);
        acc = acc + 20;
        if (earlyExit) {
            value = acc;
            yield break;
        }
        yield wait_ticks(1);
        value = acc + 5;
    }

    public sequence ThrowMid(long target, bool fail) {
        try {
            value = 1;
            yield wait_ticks(1);
            if (fail) {
                throw new SeqFault("seq_fault");
            }
            value = 99;
        } finally {
            cleaned = true;
        }
    }

    public int Read() { return value; }
    public bool Cleaned() { return cleaned; }
}

public SeqActor Spawn() { return new SeqActor(); }
)RS";

    auto bundle = compileAndLink({{"seq_test.rscript", src}});
    runtime::EngineRuntime rt(bundle.program);

    // Normal full sequence progression across 2 yields.
    const auto a1Res = rt.invoke("Kernel.SeqTest::Spawn");
    require(a1Res.succeeded, "Spawn failed");
    const auto actor1 = std::get<runtime::ObjectRef>(a1Res.value);
    auto pin1 = rt.heap()->retain(runtime::Value{actor1});
    require(rt.invoke("Kernel.SeqTest::SeqActor.Run", {actor1, runtime::Value{runtime::LongValue{1}}, runtime::Value{false}}).succeeded,
        "SeqActor.Run start failed");
    require(rt.invoke("Kernel.SeqTest::SeqActor.$sequence_Run_1", {actor1}).succeeded,
        "SeqActor.Run step 1 failed");
    require(rt.invoke("Kernel.SeqTest::SeqActor.$sequence_Run_1", {actor1}).succeeded,
        "SeqActor.Run step 2 failed");
    const auto r1 = rt.invoke("Kernel.SeqTest::SeqActor.Read", {actor1});
    require(r1.succeeded && std::get<std::int64_t>(r1.value) == 35,
        "Expected full sequence result 35");

    // `yield break` stops at first step and skips remainder.
    const auto a2Res = rt.invoke("Kernel.SeqTest::Spawn");
    const auto actor2 = std::get<runtime::ObjectRef>(a2Res.value);
    auto pin2 = rt.heap()->retain(runtime::Value{actor2});
    require(rt.invoke("Kernel.SeqTest::SeqActor.Run", {actor2, runtime::Value{runtime::LongValue{1}}, runtime::Value{true}}).succeeded,
        "SeqActor.Run early-exit start failed");
    require(rt.invoke("Kernel.SeqTest::SeqActor.$sequence_Run_1", {actor2}).succeeded,
        "SeqActor.Run early-exit step 1 failed");
    const auto r2 = rt.invoke("Kernel.SeqTest::SeqActor.Read", {actor2});
    require(r2.succeeded && std::get<std::int64_t>(r2.value) == 30,
        "Expected yield break result 30");

    // Exception rollback after yield runs finally block!
    const auto a3Res = rt.invoke("Kernel.SeqTest::Spawn");
    const auto actor3 = std::get<runtime::ObjectRef>(a3Res.value);
    auto pin3 = rt.heap()->retain(runtime::Value{actor3});
    require(rt.invoke("Kernel.SeqTest::SeqActor.ThrowMid", {actor3, runtime::Value{runtime::LongValue{1}}, runtime::Value{true}}).succeeded,
        "SeqActor.ThrowMid start failed");
    const auto faultStep = rt.invoke("Kernel.SeqTest::SeqActor.$sequence_ThrowMid_1", {actor3});
    require(!faultStep.succeeded && faultStep.error.code == runtime::ErrorCode::ScriptException,
        "Expected ScriptException from sequence after yield");
    const auto cleanedRes = rt.invoke("Kernel.SeqTest::SeqActor.Cleaned", {actor3});
    require(cleanedRes.succeeded && std::get<bool>(cleanedRes.value),
        "Finally cleanup did not run on sequence exception rollback");
}

// ---------------------------------------------------------------------------
// 2. FiberScheduler Compact VM, Auto-Lowering, Parallel Parity & Snapshots
// ---------------------------------------------------------------------------
struct MockFiberHost final : public runtime::IFiberHostCallbacks {
    std::vector<std::tuple<std::uint32_t, std::uint8_t, std::uint16_t, std::uint32_t>> commands;

    void onHostCommand(
        runtime::FiberContext& fiber,
        std::uint8_t commandId,
        std::uint16_t arg16,
        std::uint32_t arg32) noexcept override {
        commands.emplace_back(fiber.fiberId, commandId, arg16, arg32);
    }
};

void testFiberSchedulerAutoLoweringAndDeterminism() {
    const std::string src = R"RS(
module Kernel.Fibers;
import RealScript.Fiber;
import RealScript.Math;
import RealScript.Async;

public class HeapObj {
    public int x;
}

public void PatrolRoutine() {
    Fixed speed = Fixed.Of(2);
    int step = 0;
    while (step < 3) {
        step = step + 1;
        speed = speed + Fixed.Half();
        Fiber.Command(7, (int)step, speed.raw);
        Fiber.Yield(2);
    }
}

public void WaitForSignalRoutine() {
    Fiber.WaitUntil(1, 55, 1);
    Fiber.Command(8, 1, 99);
}

public async Awaitable AsyncYieldFiber() {
    int count = 0;
    while (count < 2) {
        count = count + 1;
        Fiber.Command(9, 0, count);
        await Awaitable.Yield();
    }
}

public int NonFiberHeapAlloc() {
    HeapObj o = new HeapObj();
    return o.x;
}
)RS";

    auto bundle = compileAndLink({{"fibers.rscript", src}}, {}, optimization::Level::None);
    const mir::Module* mirMod = nullptr;
    for (const auto& m : bundle.mirModules) {
        if (m.name == "Kernel.Fibers") {
            mirMod = &m;
            break;
        }
    }
    require(mirMod != nullptr, "Kernel.Fibers MIR module missing");

    const auto findMir = [&](const std::string& name) -> const mir::Function* {
        for (const auto& f : mirMod->functions) {
            if (f.name == name) {
                return &f;
            }
        }
        return nullptr;
    };
    const auto* patrol = findMir("PatrolRoutine");
    const auto* asyncYield = findMir("AsyncYieldFiber");
    const auto* nonFiber = findMir("NonFiberHeapAlloc");
    require(patrol && asyncYield && nonFiber, "Expected MIR fiber functions missing");

    require(runtime::FiberCompiler::canAutoLowerToFiber(*mirMod, *patrol),
        "PatrolRoutine must be auto-lowerable");
    require(runtime::FiberCompiler::canAutoLowerToFiber(*mirMod, *asyncYield),
        "AsyncYieldFiber must be auto-lowerable");
    require(!runtime::FiberCompiler::canAutoLowerToFiber(*mirMod, *nonFiber),
        "Heap-allocating function must NOT be auto-lowerable to Fiber VM");

    const auto fiberMod = runtime::FiberCompiler::compileAutoLoweredModule(*mirMod);
    require(fiberMod.hasFunction("PatrolRoutine") && fiberMod.hasFunction("WaitForSignalRoutine") &&
            fiberMod.hasFunction("AsyncYieldFiber"),
        "CompiledFiberModule missing expected lowered functions");

    MockFiberHost seqHost;
    MockFiberHost parHost;
    runtime::FiberScheduler seqSched(&seqHost);
    runtime::FiberScheduler parSched(&parHost);

    const std::uint32_t seqProgId = seqSched.registerModule(fiberMod);
    (void)seqProgId;
    (void)parSched.registerModule(fiberMod);

    for (std::uint32_t i = 0; i < 16; ++i) {
        const auto ent = runtime::GenerationalEntityHandle{i + 1, 1};
        seqSched.spawnFunction(fiberMod, "PatrolRoutine", ent, 1, 0, 100u + i);
        parSched.spawnFunction(fiberMod, "PatrolRoutine", ent, 1, 0, 100u + i);
    }
    const auto waitEnt = runtime::GenerationalEntityHandle{99, 1};
    seqSched.spawnFunction(fiberMod, "WaitForSignalRoutine", waitEnt, 1);
    parSched.spawnFunction(fiberMod, "WaitForSignalRoutine", waitEnt, 1);

    std::vector<std::uint8_t> midSnapshot;
    for (std::uint32_t tick = 1; tick <= 8; ++tick) {
        if (tick == 4) {
            (void)seqSched.notifyBlackboardEvent(1, 1, 55, 1);
            (void)parSched.notifyBlackboardEvent(1, 1, 55, 1);
            midSnapshot = seqSched.captureBinarySnapshot();
        }
        const auto seqIns = seqSched.stepTick(tick);
        const auto parIns = parSched.stepTickParallel(tick, 4);
        require(seqIns == parIns &&
                seqSched.computeLockstepDigest() == parSched.computeLockstepDigest(),
            "FiberScheduler stepTickParallel diverged from sequential stepTick");
    }

    // Verify snapshot restoration determinism from tick 4..8!
    MockFiberHost restoreHost;
    runtime::FiberScheduler restoreSched(&restoreHost);
    restoreSched.registerModule(fiberMod);
    restoreSched.restoreBinarySnapshot(midSnapshot);
    for (std::uint32_t tick = 4; tick <= 8; ++tick) {
        (void)restoreSched.stepTick(tick);
    }
    require(restoreSched.computeLockstepDigest() == seqSched.computeLockstepDigest(),
        "Snapshot restore lockstep digest diverged");
    require(!seqHost.commands.empty(), "Expected host commands from fiber execution");

    // Verify killEntityFibers cleans up entity's active fibers.
    const auto killEnt = runtime::GenerationalEntityHandle{500, 1};
    seqSched.spawnFunction(fiberMod, "AsyncYieldFiber", killEnt, 1);
    require(seqSched.killEntityFibers(killEnt) == 1, "Expected 1 fiber killed for entity");
}

// ---------------------------------------------------------------------------
// 3. ParallelJobScheduler Aliasing, Liveness, Verifier & Multi-Core Parity
// ---------------------------------------------------------------------------
void testParallelJobVerifierAliasingAndDeterminism() {
    const std::string src = R"RS(
module Kernel.ParJob;
import RealScript.Math;

public class DummyHeap {
    public int v;
}

public int BadAlloc(int idx) {
    DummyHeap d = new DummyHeap();
    return d.v + idx;
}

public int BadString(int idx) {
    string s = "oops";
    return s.Length + idx;
}

public void CopySaxpy(int idx, int[] src, int[] dst, int scale) {
    dst[idx] = src[idx] * scale + idx;
}

public long RangeReduceSum(int start, int count, int[] data) {
    long acc = 0L;
    int end = start + count;
    for (int i = start; i < end; i = i + 1) {
        acc = acc + (long)data[i];
    }
    return acc;
}
)RS";

    auto bundle = compileAndLink({{"par_job.rscript", src}}, {}, optimization::Level::Basic);
    const auto heap = std::make_shared<runtime::ManagedHeap>();

    // Static verification checks.
    const auto vBadAlloc = runtime::verifyParallelJobFunction(*bundle.program, "Kernel.ParJob::BadAlloc");
    const auto vBadStr = runtime::verifyParallelJobFunction(*bundle.program, "Kernel.ParJob::BadString");
    require(!vBadAlloc.valid && !vBadStr.valid,
        "ParallelJob verifier must reject heap object and string allocations");

    runtime::ParallelJobVerifyOptions copyOpts;
    const auto vCopy = runtime::verifyParallelJobFunction(*bundle.program, "Kernel.ParJob::CopySaxpy", copyOpts);
    require(vCopy.valid, "Expected CopySaxpy to pass ParallelJob verification");

    const auto intArrTid = semantic::stableTypeId("int[]");
    const auto srcArr = heap->allocateTypedArray(intArrTid, semantic::PrimitiveType::Int, 0, 256, std::int64_t{3});
    const auto dst1 = heap->allocateTypedArray(intArrTid, semantic::PrimitiveType::Int, 0, 256, std::int64_t{0});
    const auto dst4 = heap->allocateTypedArray(intArrTid, semantic::PrimitiveType::Int, 0, 256, std::int64_t{0});
    require(srcArr.has_value() && dst1.has_value() && dst4.has_value(), "allocateTypedArray failed");
    auto pinSrc = heap->retain(runtime::Value{*srcArr});
    auto pinDst1 = heap->retain(runtime::Value{*dst1});
    auto pinDst4 = heap->retain(runtime::Value{*dst4});

    runtime::ParallelJobScheduler sched(4);
    runtime::ParallelJobConfig cfg1;
    cfg1.workerCount = 1;
    cfg1.batchSize = 32;
    cfg1.explicitReadOnlyUserArgIndices = {0};
    cfg1.explicitWriteUserArgIndices = {1};
    cfg1.executionOptions.determinism.mode = runtime::DeterminismMode::Strict;

    // Aliased array must fail cleanly with InvalidArguments!
    const auto aliased = sched.dispatchFor(
        bundle.program,
        heap,
        "Kernel.ParJob::CopySaxpy",
        256,
        {runtime::Value{*srcArr}, runtime::Value{*srcArr}, runtime::Value{std::int64_t{4}}},
        cfg1);
    require(!aliased.succeeded && aliased.error.code == runtime::ErrorCode::InvalidArguments,
        "ParallelJobScheduler must reject aliased src==dst arrays");

    // 1-worker vs 4-worker dispatchFor parity.
    auto cfg4 = cfg1;
    cfg4.workerCount = 4;
    const auto r1 = sched.dispatchFor(
        bundle.program,
        heap,
        "Kernel.ParJob::CopySaxpy",
        256,
        {runtime::Value{*srcArr}, runtime::Value{*dst1}, runtime::Value{std::int64_t{4}}},
        cfg1);
    const auto r4 = sched.dispatchFor(
        bundle.program,
        heap,
        "Kernel.ParJob::CopySaxpy",
        256,
        {runtime::Value{*srcArr}, runtime::Value{*dst4}, runtime::Value{std::int64_t{4}}},
        cfg4);
    require(r1.succeeded && r4.succeeded && r1.determinismDigest == r4.determinismDigest,
        "1-worker vs 4-worker dispatchFor digest parity failed");

    // Deterministic Range Reduction across 1 vs 4 workers.
    runtime::ParallelJobConfig redCfg;
    redCfg.batchSize = 32;
    redCfg.reduceOp = runtime::ParallelReduceOp::Sum;
    redCfg.explicitReadOnlyUserArgIndices = {0};
    redCfg.executionOptions.determinism.mode = runtime::DeterminismMode::Strict;
    redCfg.workerCount = 1;
    const auto red1 = sched.dispatchRange(
        bundle.program, heap, "Kernel.ParJob::RangeReduceSum", 256, {runtime::Value{*dst1}}, redCfg);
    redCfg.workerCount = 4;
    const auto red4 = sched.dispatchRange(
        bundle.program, heap, "Kernel.ParJob::RangeReduceSum", 256, {runtime::Value{*dst4}}, redCfg);
    require(red1.succeeded && red4.succeeded &&
            std::get<runtime::LongValue>(red1.value).value == std::get<runtime::LongValue>(red4.value).value &&
            red1.determinismDigest == red4.determinismDigest,
        "1-worker vs 4-worker dispatchRange reduction diverged");
}

// ---------------------------------------------------------------------------
// 4. AwaitableCorePool ABA Safety, GC Roots & CancellationTokenSource
// ---------------------------------------------------------------------------
void testAwaitablePoolAbaAndCancellation() {
    runtime::ManagedHeap heap;
    runtime::CancellationTokenPool ctsPool;
    runtime::AwaitableCorePool pool(&heap, &ctsPool, 4);

    // Pre-allocated slot reuse + ABA generation check.
    const auto h1 = pool.allocatePending();
    require(h1.valid() && !pool.isCompleted(h1), "Expected valid pending handle");
    const auto s = heap.allocateString("pinned-async-result");
    require(s.has_value(), "allocateString failed");
    require(pool.trySetResult(h1, runtime::Value{*s}), "trySetResult h1 failed");

    runtime::ShadowStack emptyRoots;
    heap.collectFull(emptyRoots);
    require(heap.isAlive(*s), "AwaitableCorePool did not protect result ObjectRef across Full GC");

    runtime::Value out1;
    runtime::RuntimeError err1;
    require(pool.getResult(h1, out1, err1) == runtime::AwaitStatus::Succeeded &&
            std::get<runtime::ObjectRef>(out1) == *s,
        "getResult h1 mismatch");

    // Slot is recycled with incremented version; stale h1 must fail cleanly!
    const auto h2 = pool.allocatePending();
    require(h2.slot == h1.slot && h2.version == h1.version + 1u,
        "Expected recycled slot with bumped ABA generation");
    require(!pool.trySetResult(h1, runtime::Value{std::int64_t{-1}}) && !pool.isCompleted(h2),
        "Stale AwaitableHandle mutated new occupant (ABA bug!)");

    // Cancellation in RealScript code via RealScript.Async.
    const std::string cancelSrc = R"RS(
module Kernel.AsyncCancel;
import RealScript.Async;

public async Awaitable<int> CancelAware(CancellationToken tok) {
    await Awaitable.YieldWithToken(tok);
    return 123;
}
)RS";
    auto bundle = compileAndLink({{"async_cancel.rscript", cancelSrc}});
    runtime::EngineRuntime rt(bundle.program);
    const auto srcHandle = rt.asyncRuntime()->cancellations().createSource();
    const auto tok = rt.asyncRuntime()->cancellations().token(srcHandle);
    std::vector<runtime::Value> tokFields = {runtime::Value{runtime::LongValue{static_cast<std::int64_t>(tok.pack64())}}};
    const runtime::Value tokStruct(runtime::makeStructValueFromFields(
        semantic::stableTypeId("RealScript.Async::CancellationToken"), tokFields));

    const auto launched = rt.invoke("Kernel.AsyncCancel::CancelAware", {tokStruct});
    require(launched.succeeded, "CancelAware launch failed");
    require(rt.asyncRuntime()->cancellations().cancel(srcHandle) >= 1, "cancel failed");
    rt.asyncRuntime()->stepResumeQueue(runtime::ResumeQueue::Update);

    const auto f0 = runtime::structValueFieldAt(std::get<runtime::StructValue>(launched.value), 0);
    require(f0.has_value(), "Missing completion handle on Awaitable<int>");
    const auto taskH = runtime::AwaitableHandle::unpack64(
        static_cast<std::uint64_t>(std::get<runtime::LongValue>(*f0).value));
    runtime::Value dummy;
    runtime::RuntimeError cancelErr;
    require(rt.asyncRuntime()->awaitables().getResult(taskH, dummy, cancelErr) == runtime::AwaitStatus::Canceled,
        "Expected task to transition to Canceled after CancellationToken cancel");
}

// ---------------------------------------------------------------------------
// 5. Duck-Typed GetAwaiter + [Awaitable] + IAwaitScheduler
//    Testing BOTH isCompleted() == true (fast path) AND isCompleted() == false
//    (true suspension + resumeCallback resumption with local preservation!)
// ---------------------------------------------------------------------------
class DualPathAwaitScheduler final : public runtime::IAwaitScheduler {
public:
    bool allowCompleted = true;
    int pendingPayload = 0;
    int suspendCalls = 0;
    int resumeCalls = 0;
    std::function<void()> savedResumeCallback;

    void resetForDelayed(int payload) {
        allowCompleted = false;
        pendingPayload = payload;
        suspendCalls = 0;
        resumeCalls = 0;
        savedResumeCallback = nullptr;
    }

    void completeDelayed() {
        allowCompleted = true;
        if (savedResumeCallback) {
            auto cb = std::move(savedResumeCallback);
            savedResumeCallback = nullptr;
            ++resumeCalls;
            cb();
        }
    }

    bool handlesValue(const runtime::Value& awaited) const noexcept override {
        return awaited.rawTag() == runtime::ValueTag::Struct;
    }

    bool isCompleted(const runtime::Value&) const noexcept override {
        return allowCompleted;
    }

    bool tryGetResult(
        const runtime::Value& awaited,
        bool hasResult,
        runtime::Value& outResult,
        runtime::RuntimeError&) override {
        if (hasResult) {
            if (pendingPayload != 0) {
                outResult = runtime::Value{std::int64_t{pendingPayload}};
            } else {
                const auto f0 = runtime::structValueFieldAt(awaited.asStructUnchecked(), 0);
                outResult = f0.value_or(runtime::Value{std::int64_t{0}});
            }
        }
        return true;
    }

    bool onSuspend(
        const runtime::Value&,
        std::function<void()> resumeCallback,
        runtime::RuntimeError&) override {
        ++suspendCalls;
        savedResumeCallback = std::move(resumeCallback);
        return true;
    }

    bool tryResolveAsyncReturn(
        semantic::SymbolId returnTypeId,
        const runtime::Value* payload,
        runtime::Value& outWrapped,
        runtime::RuntimeError&) override {
        std::vector<runtime::Value> fields;
        fields.push_back(payload ? *payload : runtime::Value{std::int64_t{0}});
        outWrapped = runtime::Value{runtime::makeStructValueFromFields(returnTypeId, std::move(fields))};
        return true;
    }
};

void testDuckTypedAwaitSchedulerFastAndSuspendedPaths() {
    compiler::CompilationOptions noBuiltinAsync;
    noBuiltinAsync.disabledPacks.insert("RealScript.Async");

    const std::string src = R"RS(
module Kernel.CustomAwait;

public class AwaitableAttribute {
    public AwaitableAttribute() {}
}

[Awaitable]
public struct HostTaskInt {
    public int CompletedValue;
    public static HostTaskInt FromResult(int v) {
        HostTaskInt t = new HostTaskInt();
        t.CompletedValue = v;
        return t;
    }
    public bool IsCompleted() { return true; }
    public int GetResult() { return CompletedValue; }
}

public struct DuckAwaiter {
    public int Seed;
    public bool IsCompleted() { return true; }
    public int GetResult() { return Seed; }
}

public struct DuckTask {
    public int Seed;
    public DuckAwaiter GetAwaiter() {
        DuckAwaiter a = new DuckAwaiter();
        a.Seed = Seed;
        return a;
    }
}

public async HostTaskInt ComputeFast(int seed) {
    int kept = seed * 3;
    DuckTask t = new DuckTask();
    t.Seed = 21;
    int delta = await t;
    return kept + delta;
}

public async HostTaskInt ComputeSuspended(int seed) {
    int kept = seed * 10;
    DuckTask t = new DuckTask();
    t.Seed = 1;
    int delta = await t;
    return kept + delta;
}
)RS";

    auto bundle = compileAndLink({{"custom_await.rscript", src}}, std::move(noBuiltinAsync));
    const auto hostTaskTypeId = semantic::stableTypeId("Kernel.CustomAwait::HostTaskInt");

    for (auto mode : {runtime::DeterminismMode::Off, runtime::DeterminismMode::Strict}) {
        DualPathAwaitScheduler scheduler;
        auto asyncCtx = std::make_shared<runtime::AsyncRuntimeContext>();
        runtime::Interpreter interp(bundle.program);
        interp.setAsyncRuntime(asyncCtx);

        // Part A: isCompleted() == true synchronous fast path returning outer HostTaskInt.
        {
            scheduler.allowCompleted = true;
            scheduler.pendingPayload = 0;
            runtime::ExecutionOptions opts;
            opts.awaitScheduler = &scheduler;
            opts.determinism.mode = mode;

            const auto res = interp.invoke(
                "Kernel.CustomAwait::ComputeFast", {runtime::Value{std::int64_t{5}}}, opts);
            require(res.succeeded && res.value.rawTag() == runtime::ValueTag::Struct,
                "Fast path IAwaitScheduler failed: " + res.error.message);
            require(res.value.asStructUnchecked().typeId == hostTaskTypeId,
                "Fast path must return outer HostTaskInt struct type");
            require(scheduler.suspendCalls == 0, "Fast path must not invoke onSuspend");
            const auto f0 = runtime::structValueFieldAt(res.value.asStructUnchecked(), 0);
            require(f0.has_value() && std::get<std::int64_t>(*f0) == (5 * 3 + 21),
                "Fast path returned wrong payload");
        }

        // Part B: isCompleted() == false true suspension -> resumeCallback -> outer completion & locals preserved.
        {
            scheduler.resetForDelayed(42);
            runtime::ExecutionOptions opts;
            opts.awaitScheduler = &scheduler;
            opts.determinism.mode = mode;

            const auto res = interp.invoke(
                "Kernel.CustomAwait::ComputeSuspended", {runtime::Value{std::int64_t{7}}}, opts);
            require(res.succeeded && res.value.rawTag() == runtime::ValueTag::Struct,
                "Suspended IAwaitScheduler call failed: " + res.error.message);
            require(res.value.asStructUnchecked().typeId == hostTaskTypeId,
                "Suspended call must return outer HostTaskInt wrapper");
            require(scheduler.suspendCalls == 1 && static_cast<bool>(scheduler.savedResumeCallback),
                "onSuspend was not called when isCompleted() returned false");

            const auto packedField = runtime::structValueFieldAt(res.value.asStructUnchecked(), 0);
            require(packedField.has_value() && std::holds_alternative<runtime::LongValue>(*packedField),
                "Suspended outer HostTaskInt missing packed AwaitableHandle");
            const auto outerHandle = runtime::AwaitableHandle::unpack64(
                static_cast<std::uint64_t>(std::get<runtime::LongValue>(*packedField).value));
            require(!asyncCtx->awaitables().isCompleted(outerHandle),
                "Outer task must remain pending before resumeCallback() fires");

            // Complete host signal and trigger continuation!
            scheduler.completeDelayed();
            require(scheduler.resumeCalls == 1, "Expected saved resumeCallback execution");
            require(asyncCtx->awaitables().isCompleted(outerHandle),
                "Outer task did not complete after resumeCallback()");

            runtime::Value finalVal;
            runtime::RuntimeError finalErr;
            require(asyncCtx->awaitables().getResult(outerHandle, finalVal, finalErr) ==
                    runtime::AwaitStatus::Succeeded,
                "Outer task getResult failed: " + finalErr.message);
            require(std::get<std::int64_t>(finalVal) == (7 * 10 + 42),
                "Resumed continuation failed to preserve local `kept` across suspension");
        }
    }
}

// ---------------------------------------------------------------------------
// 6. Pillar 2: IAwaitScheduler Faulted / Try-Finally & Delayed Resumption Parity
// ---------------------------------------------------------------------------
class FaultAwareAwaitScheduler final : public runtime::IAwaitScheduler {
public:
    bool allowCompleted = true;
    bool shouldFault = false;
    std::int64_t nextPayload = 0;
    int suspendCalls = 0;
    std::function<void()> savedResume;

    bool handlesValue(const runtime::Value& awaited) const noexcept override {
        return awaited.rawTag() == runtime::ValueTag::Struct;
    }

    bool isCompleted(const runtime::Value&) const noexcept override {
        return allowCompleted;
    }

    bool tryGetResult(
        const runtime::Value&,
        bool hasResult,
        runtime::Value& outResult,
        runtime::RuntimeError& outError) override {
        if (shouldFault) {
            outError.code = runtime::ErrorCode::ScriptException;
            outError.message = "scheduler-faulted";
            return false;
        }
        if (hasResult) {
            outResult = runtime::Value{nextPayload};
        }
        return true;
    }

    bool onSuspend(
        const runtime::Value&,
        std::function<void()> resumeCallback,
        runtime::RuntimeError&) override {
        ++suspendCalls;
        savedResume = std::move(resumeCallback);
        return true;
    }
};

void testAwaitSchedulerFaultAndDelayedResumePaths() {
    compiler::CompilationOptions noBuiltinAsync;
    noBuiltinAsync.disabledPacks.insert("RealScript.Async");

    const std::string src = R"RS(
module Kernel.FaultSched;

public class AwaitableAttribute {
    public AwaitableAttribute() {}
}

[Awaitable]
public struct NativeTaskInt {
    public int Tag;
    public NativeTaskInt(int t) { Tag = t; }
    public static NativeTaskInt FromResult(int v) { return new NativeTaskInt(v); }
    public bool IsCompleted() { return true; }
    public int GetResult() { return Tag; }
}

public async NativeTaskInt RunDelayedOrFault(int mult, int[] cleanupFlag) {
    try {
        int val = await new NativeTaskInt(1);
        return val * mult;
    } finally {
        cleanupFlag[0] = cleanupFlag[0] + 1;
    }
}
)RS";

    const auto bundle = compileAndLink({{"FaultSched.rscript", src}}, noBuiltinAsync);
    auto asyncCtx = std::make_shared<runtime::AsyncRuntimeContext>();
    runtime::Interpreter interp(bundle.program);
    interp.setAsyncRuntime(asyncCtx);
    auto& heap = *interp.heap();

    // 1. Immediate fault propagates cleanly & runs finally block.
    {
        FaultAwareAwaitScheduler sched;
        sched.allowCompleted = true;
        sched.shouldFault = true;
        runtime::ExecutionOptions opts;
        opts.awaitScheduler = &sched;

        auto flagArr = heap.allocateTypedArray(
            semantic::stableTypeId("int[]"), semantic::PrimitiveType::Int, 0, 1, std::int64_t{0});
        require(flagArr.has_value(), "allocate flagArr failed");
        auto pin = heap.retain(runtime::Value{*flagArr});

        const auto res = interp.invoke(
            "Kernel.FaultSched::RunDelayedOrFault",
            {runtime::Value{std::int64_t{3}}, runtime::Value{*flagArr}},
            opts);
        require(heap.arrayGet(*flagArr, 0) == std::optional<runtime::Value>{runtime::Value{std::int64_t{1}}},
            "Expected finally block execution on immediate scheduler fault");
        if (res.succeeded && res.value.rawTag() == runtime::ValueTag::Struct) {
            const auto packed = runtime::structValueFieldAt(res.value.asStructUnchecked(), 0);
            require(packed && std::holds_alternative<runtime::LongValue>(*packed),
                "Missing packed AwaitableHandle on immediate faulted task");
            const auto h = runtime::AwaitableHandle::unpack64(
                static_cast<std::uint64_t>(std::get<runtime::LongValue>(*packed).value));
            runtime::Value outVal;
            runtime::RuntimeError outErr;
            require(asyncCtx->awaitables().getResult(h, outVal, outErr) == runtime::AwaitStatus::Faulted &&
                    outErr.message.find("scheduler-faulted") != std::string::npos,
                "Expected immediate IAwaitScheduler fault propagation");
        } else {
            require(!res.succeeded && res.error.message.find("scheduler-faulted") != std::string::npos,
                "Expected immediate IAwaitScheduler fault propagation");
        }
    }

    // 2. Suspend then resume-into-fault marks outer Awaitable faulted & preserves finally cleanup.
    {
        FaultAwareAwaitScheduler sched;
        sched.allowCompleted = false;
        sched.shouldFault = false;
        runtime::ExecutionOptions opts;
        opts.awaitScheduler = &sched;

        auto flagArr = heap.allocateTypedArray(
            semantic::stableTypeId("int[]"), semantic::PrimitiveType::Int, 0, 1, std::int64_t{0});
        require(flagArr.has_value(), "allocate flagArr failed");
        auto pin = heap.retain(runtime::Value{*flagArr});

        const auto res = interp.invoke(
            "Kernel.FaultSched::RunDelayedOrFault",
            {runtime::Value{std::int64_t{4}}, runtime::Value{*flagArr}},
            opts);
        require(res.succeeded && sched.suspendCalls == 1 && sched.savedResume != nullptr,
            "Expected suspension on delayed fault test");
        const auto packed = runtime::structValueFieldAt(res.value.asStructUnchecked(), 0);
        require(packed && std::holds_alternative<runtime::LongValue>(*packed),
            "Missing packed AwaitableHandle on suspended fault test");
        const auto h = runtime::AwaitableHandle::unpack64(
            static_cast<std::uint64_t>(std::get<runtime::LongValue>(*packed).value));

        sched.allowCompleted = true;
        sched.shouldFault = true;
        sched.savedResume();

        runtime::Value outVal;
        runtime::RuntimeError outErr;
        require(asyncCtx->awaitables().getResult(h, outVal, outErr) == runtime::AwaitStatus::Faulted &&
                outErr.message.find("scheduler-faulted") != std::string::npos,
            "Expected Faulted outer Awaitable after resumed scheduler fault");
        require(heap.arrayGet(*flagArr, 0) == std::optional<runtime::Value>{runtime::Value{std::int64_t{1}}},
            "Expected finally cleanup when resumed IAwaitScheduler faults");
    }
}

void testParallelJobRawColumnsAndNativeBatchKernel() {
    const std::string src = R"RS(
module Kernel.ParRaw;

public class ColStore {
    public static int[] Factors;
}

int PureHostBump(int x);

public void InitFactors(int[] factors) {
    ColStore.Factors = factors;
}

public void RawSaxpy(int idx, int[] input, int[] output, int scale) {
    output[idx] = input[idx] * scale + 3;
}

public void StaticColScale(int idx, int[] output) {
    output[idx] = ColStore.Factors[idx] * 5;
}

public void ExternalFallbackKernel(int idx, int[] input, int[] output) {
    output[idx] = PureHostBump(input[idx]);
}
)RS";

    auto bundle = compileAndLink({{"par_raw_batch.rscript", src}}, {}, optimization::Level::Basic);
    const auto heap = std::make_shared<runtime::ManagedHeap>();

    constexpr std::size_t kCount = 128u;
    std::vector<std::int32_t> hostIn(kCount);
    std::vector<std::int32_t> hostOut(kCount, 0);
    for (std::size_t i = 0; i < kCount; ++i) {
        hostIn[i] = static_cast<std::int32_t>(i + 1u);
    }

    const auto intArrTid = semantic::stableTypeId("int[]");
    runtime::NativeRawColumnView inView{};
    inView.basePtr = hostIn.data();
    inView.count = kCount;
    inView.strideBytes = sizeof(std::int32_t);
    inView.readOnly = true;
    inView.mode = runtime::RawColumnMode::ScalarPacked;
    inView.elementType = semantic::PrimitiveType::Int;
    inView.packedEncoding = runtime::PackedElemEncoding::Int32;

    runtime::NativeRawColumnView outView{};
    outView.basePtr = hostOut.data();
    outView.count = kCount;
    outView.strideBytes = sizeof(std::int32_t);
    outView.readOnly = false;
    outView.mode = runtime::RawColumnMode::ScalarPacked;
    outView.elementType = semantic::PrimitiveType::Int;
    outView.packedEncoding = runtime::PackedElemEncoding::Int32;

    runtime::RuntimeError rawErr;
    const auto rawInRef = heap->allocateRawColumnArray(intArrTid, 0u, inView, &rawErr);
    const auto rawOutRef = heap->allocateRawColumnArray(intArrTid, 0u, outView, &rawErr);
    const auto aliasOutRef = heap->allocateRawColumnArray(intArrTid, 0u, outView, &rawErr);
    require(rawInRef.has_value() && rawOutRef.has_value() && aliasOutRef.has_value(),
        "allocateRawColumnArray failed: " + rawErr.message);
    auto pinIn = heap->retain(runtime::Value{*rawInRef});
    auto pinOut = heap->retain(runtime::Value{*rawOutRef});
    auto pinAlias = heap->retain(runtime::Value{*aliasOutRef});

    runtime::ParallelJobScheduler sched(4);

    // 1. Disallowing allowRawColumnArrays rejects raw column arguments safely
    {
        runtime::ParallelJobConfig disallowed;
        disallowed.workerCount = 4;
        disallowed.allowRawColumnArrays = false;
        const auto failRes = sched.dispatchFor(
            bundle.program,
            heap,
            "Kernel.ParRaw::RawSaxpy",
            kCount,
            {runtime::Value{*rawInRef}, runtime::Value{*rawOutRef}, runtime::Value{std::int64_t{4}}},
            disallowed);
        require(!failRes.succeeded && failRes.error.code == runtime::ErrorCode::CapabilityDenied,
            "Expected CapabilityDenied for RawColumnArray when allowRawColumnArrays is false");
    }

    // 2. Writing into a readOnly RawColumnArray must be rejected
    {
        runtime::ParallelJobConfig roCfg;
        roCfg.workerCount = 4;
        roCfg.allowRawColumnArrays = true;
        const auto roFail = sched.dispatchFor(
            bundle.program,
            heap,
            "Kernel.ParRaw::RawSaxpy",
            kCount,
            {runtime::Value{*rawOutRef}, runtime::Value{*rawInRef}, runtime::Value{std::int64_t{2}}},
            roCfg);
        require(!roFail.succeeded && roFail.error.code == runtime::ErrorCode::CapabilityDenied,
            "Expected CapabilityDenied when writing to a readOnly RawColumnArray");
    }

    // 3. Two distinct ObjectRefs overlapping the same host buffer while written must fail aliasing check
    {
        runtime::ParallelJobConfig aliasCfg;
        aliasCfg.workerCount = 4;
        aliasCfg.allowRawColumnArrays = true;
        const auto aliasFail = sched.dispatchFor(
            bundle.program,
            heap,
            "Kernel.ParRaw::RawSaxpy",
            kCount,
            {runtime::Value{*aliasOutRef}, runtime::Value{*rawOutRef}, runtime::Value{std::int64_t{2}}},
            aliasCfg);
        require(!aliasFail.succeeded && aliasFail.error.code == runtime::ErrorCode::InvalidArguments,
            "Expected InvalidArguments for overlapping host RawColumn buffers");
    }

    // 4. Zero-copy POD auto native batch kernel executes over RawColumn buffers without constructing workerInterpreters
    {
        runtime::ParallelJobConfig cfg;
        cfg.workerCount = 4;
        cfg.batchSize = 32;
        cfg.allowRawColumnArrays = true;
        cfg.executionOptions.determinism.mode = runtime::DeterminismMode::Strict;
        const auto okRes = sched.dispatchFor(
            bundle.program,
            heap,
            "Kernel.ParRaw::RawSaxpy",
            kCount,
            {runtime::Value{*rawInRef}, runtime::Value{*rawOutRef}, runtime::Value{std::int64_t{7}}},
            cfg);
        require(okRes.succeeded, "Auto ParallelNativeBatchKernel failed: " + okRes.error.message);
        require(okRes.usedNativeBatchKernel && okRes.workerInterpretersConstructed == 0u,
            "Expected usedNativeBatchKernel=true and workerInterpretersConstructed=0 on POD/RawColumn batch");
        for (std::size_t i = 0; i < kCount; ++i) {
            require(hostOut[i] == hostIn[i] * 7 + 3,
                "Zero-copy RawSaxpy output mismatch at index " + std::to_string(i));
        }
    }

    // 5. allowReadOnlyStaticColumnLoads verifier gating + read-only static column native execution
    {
        runtime::Interpreter initVm(bundle.program, heap);
        const auto initRes = initVm.invoke("Kernel.ParRaw::InitFactors", {runtime::Value{*rawInRef}});
        require(initRes.succeeded, "InitFactors failed: " + initRes.error.message);

        runtime::ParallelJobVerifyOptions noStaticOpts;
        noStaticOpts.allowRawColumnArrays = true;
        noStaticOpts.allowReadOnlyStaticColumnLoads = false;
        const auto vDeny = runtime::verifyParallelJobFunction(
            *bundle.program, "Kernel.ParRaw::StaticColScale", noStaticOpts);
        require(!vDeny.valid,
            "StaticColScale must fail verification when allowReadOnlyStaticColumnLoads is false");

        runtime::ParallelJobVerifyOptions allowStaticOpts;
        allowStaticOpts.allowRawColumnArrays = true;
        allowStaticOpts.allowReadOnlyStaticColumnLoads = true;
        const auto vAllow = runtime::verifyParallelJobFunction(
            *bundle.program, "Kernel.ParRaw::StaticColScale", allowStaticOpts);
        require(vAllow.valid && vAllow.usesReadOnlyStaticColumnLoads,
            "StaticColScale must pass verification with allowReadOnlyStaticColumnLoads=true");

        std::fill(hostOut.begin(), hostOut.end(), 0);
        runtime::ParallelJobConfig sCfg;
        sCfg.workerCount = 4;
        sCfg.batchSize = 32;
        sCfg.allowRawColumnArrays = true;
        sCfg.allowReadOnlyStaticColumnLoads = true;
        const auto sRes = sched.dispatchFor(
            bundle.program,
            heap,
            "Kernel.ParRaw::StaticColScale",
            kCount,
            {runtime::Value{*rawOutRef}},
            sCfg);
        require(sRes.succeeded && sRes.usedNativeBatchKernel && sRes.workerInterpretersConstructed == 0u,
            "Expected native zero-copy execution for read-only static column load: " + sRes.error.message);
        for (std::size_t i = 0; i < kCount; ++i) {
            require(hostOut[i] == hostIn[i] * 5,
                "StaticColScale output mismatch at index " + std::to_string(i));
        }
    }

    // 6. Explicit ParallelNativeBatchKernel callback receives column bindings & processes raw buffers directly
    {
        std::fill(hostOut.begin(), hostOut.end(), 0);
        std::atomic<std::size_t> seenBatches{0u};
        runtime::ParallelJobConfig cbCfg;
        cbCfg.workerCount = 4;
        cbCfg.batchSize = 32;
        cbCfg.allowRawColumnArrays = true;
        cbCfg.nativeBatchKernel = [&](runtime::ParallelNativeBatchContext& ctx) -> bool {
            if (!ctx.columns || ctx.columns->size() < 2u) return false;
            const auto* inPtr = static_cast<const std::int32_t*>((*ctx.columns)[0].basePtr);
            auto* outPtr = static_cast<std::int32_t*>((*ctx.columns)[1].basePtr);
            for (std::size_t k = 0; k < ctx.count; ++k) {
                const std::size_t idx = ctx.start + k;
                outPtr[idx] = inPtr[idx] * 11;
            }
            seenBatches.fetch_add(1u, std::memory_order_relaxed);
            return true;
        };
        const auto cbRes = sched.dispatchFor(
            bundle.program,
            heap,
            "Kernel.ParRaw::RawSaxpy",
            kCount,
            {runtime::Value{*rawInRef}, runtime::Value{*rawOutRef}, runtime::Value{std::int64_t{1}}},
            cbCfg);
        require(cbRes.succeeded && cbRes.usedNativeBatchKernel &&
                cbRes.workerInterpretersConstructed == 0u && seenBatches.load() == cbRes.numBatches,
            "Explicit ParallelNativeBatchKernel failed");
        for (std::size_t i = 0; i < kCount; ++i) {
            require(hostOut[i] == hostIn[i] * 11, "Explicit nativeBatchKernel output mismatch");
        }
    }

    // 7. Safe fallback to workerInterpreters for unsupported kernels (e.g. calling allowed host extern)
    {
        std::fill(hostOut.begin(), hostOut.end(), 0);
        auto reg = std::make_shared<runtime::BindingRegistry>();
        reg->bind(
            "Kernel.ParRaw::PureHostBump",
            [](const bytecode::FunctionReference&,
               runtime::ValueSpan args,
               runtime::RuntimeError&) -> std::optional<runtime::Value> {
                return runtime::Value{std::get<std::int64_t>(args[0]) + 100};
            });
        runtime::ParallelJobConfig fbCfg;
        fbCfg.workerCount = 2;
        fbCfg.batchSize = 32;
        fbCfg.allowRawColumnArrays = true;
        fbCfg.pureExternalNames.insert("Kernel.ParRaw::PureHostBump");
        const auto fbRes = sched.dispatchFor(
            bundle.program,
            heap,
            "Kernel.ParRaw::ExternalFallbackKernel",
            kCount,
            {runtime::Value{*rawInRef}, runtime::Value{*rawOutRef}},
            fbCfg,
            reg);
        require(fbRes.succeeded && !fbRes.usedNativeBatchKernel &&
                fbRes.workerInterpretersConstructed == 2u,
            "Expected safe fallback constructing workerInterpreters for host-extern kernel: " + fbRes.error.message);
        for (std::size_t i = 0; i < kCount; ++i) {
            require(hostOut[i] == hostIn[i] + 100, "Fallback kernel output mismatch");
        }
    }
}

void testNonGameHostSequenceComposition() {
    const std::string schedSrc = R"RS(
module CustomHost.Scheduler;

public class SchedState {
    public static int ScheduledCount = 0;
    public static int EventSubCount = 0;
    public static int AsyncWaitCount = 0;
    public static int CanceledCount = 0;
    public static string LastCallback = "";
}

public long Schedule(long target, string callback, int delay) {
    SchedState.ScheduledCount = SchedState.ScheduledCount + 1;
    SchedState.LastCallback = callback;
    return target * 100 + delay;
}

public bool CancelTimer(long timer) {
    if (timer != 0) {
        SchedState.CanceledCount = SchedState.CanceledCount + 1;
        return true;
    }
    return false;
}

public long SubscribeOnce(long target, string callback, string topic) {
    SchedState.EventSubCount = SchedState.EventSubCount + 1;
    SchedState.LastCallback = callback;
    return target + 10;
}

public long WaitAsyncOperation(long target, string callback, long asyncOp) {
    SchedState.AsyncWaitCount = SchedState.AsyncWaitCount + 1;
    SchedState.LastCallback = callback;
    return asyncOp + 1;
}

public int ReadScheduledCount() { return SchedState.ScheduledCount; }
public int ReadEventSubCount() { return SchedState.EventSubCount; }
public int ReadAsyncWaitCount() { return SchedState.AsyncWaitCount; }
public int ReadCanceledCount() { return SchedState.CanceledCount; }
public string ReadLastCallback() { return SchedState.LastCallback; }
)RS";

    const std::string workflowSrc = R"RS(
module CustomHost.Workflow;
import CustomHost.Scheduler;

public class NonGameTask {
    public int stage = 0;

    public sequence ExecuteFlow(long target, long asyncOp) {
        stage = 10;
        yield wait_ticks(2);
        stage = 20;
        yield wait_event("custom.done");
        stage = 30;
        yield wait_async(asyncOp);
        stage = 40;
    }

    public int ReadStage() { return stage; }
}

public NonGameTask CreateTask() { return new NonGameTask(); }
)RS";

    auto bundle = compileAndLink({
        {"custom_sched.rscript", schedSrc},
        {"custom_workflow.rscript", workflowSrc},
    });
    runtime::EngineRuntime rt(bundle.program);

    const auto readString = [&](const std::string& fn) {
        const auto res = rt.invoke(fn);
        require(res.succeeded, fn + " failed: " + res.error.message);
        if (const auto* ref = std::get_if<runtime::ObjectRef>(&res.value)) {
            if (const auto view = rt.heap()->stringView(*ref)) {
                return std::string(*view);
            }
        }
        require(false, fn + " did not return string");
        return std::string{};
    };
    const auto readInt = [&](const std::string& fn) {
        const auto res = rt.invoke(fn);
        require(res.succeeded && std::holds_alternative<std::int64_t>(res.value),
            fn + " failed");
        return std::get<std::int64_t>(res.value);
    };

    const auto taskRes = rt.invoke("CustomHost.Workflow::CreateTask");
    require(taskRes.succeeded, "CreateTask failed");
    const auto taskRef = std::get<runtime::ObjectRef>(taskRes.value);
    auto pin = rt.heap()->retain(runtime::Value{taskRef});

    require(rt.invoke("CustomHost.Workflow::NonGameTask.ExecuteFlow",
                {runtime::Value{taskRef}, runtime::Value{runtime::LongValue{7}}, runtime::Value{runtime::LongValue{55}}})
                .succeeded,
        "NonGameTask.ExecuteFlow start failed");
    require(readInt("CustomHost.Scheduler::ReadScheduledCount") == 1,
        "Expected CustomHost.Scheduler::Schedule call on wait_ticks");
    const std::string cb1 = "CustomHost.Workflow::NonGameTask." + readString("CustomHost.Scheduler::ReadLastCallback");
    require(rt.invoke(cb1, {runtime::Value{taskRef}}).succeeded,
        "Resume callback 1 failed: " + cb1);
    require(readInt("CustomHost.Scheduler::ReadEventSubCount") == 1,
        "Expected CustomHost.Scheduler::SubscribeOnce call on wait_event");
    const std::string cb2 = "CustomHost.Workflow::NonGameTask." + readString("CustomHost.Scheduler::ReadLastCallback");
    require(rt.invoke(cb2, {runtime::Value{taskRef}}).succeeded,
        "Resume callback 2 failed: " + cb2);
    require(readInt("CustomHost.Scheduler::ReadAsyncWaitCount") == 1,
        "Expected CustomHost.Scheduler::WaitAsyncOperation call on wait_async");
    const std::string cb3 = "CustomHost.Workflow::NonGameTask." + readString("CustomHost.Scheduler::ReadLastCallback");
    require(rt.invoke(cb3, {runtime::Value{taskRef}}).succeeded,
        "Resume callback 3 failed: " + cb3);

    const auto stageRes = rt.invoke("CustomHost.Workflow::NonGameTask.ReadStage", {runtime::Value{taskRef}});
    require(stageRes.succeeded && std::get<std::int64_t>(stageRes.value) == 40,
        "Expected final stage 40 from non-game host sequence");

    // Starting a new sequence mid-flight invokes CustomHost.Scheduler::CancelTimer on restart.
    require(rt.invoke("CustomHost.Workflow::NonGameTask.ExecuteFlow",
                {runtime::Value{taskRef}, runtime::Value{runtime::LongValue{8}}, runtime::Value{runtime::LongValue{66}}})
                .succeeded,
        "Restarted sequence initial start failed");
    require(rt.invoke("CustomHost.Workflow::NonGameTask.ExecuteFlow",
                {runtime::Value{taskRef}, runtime::Value{runtime::LongValue{8}}, runtime::Value{runtime::LongValue{66}}})
                .succeeded,
        "Second restart while suspended failed");
    require(readInt("CustomHost.Scheduler::ReadCanceledCount") >= 1,
        "Expected CustomHost.Scheduler::CancelTimer call on sequence restart");
}

void testFiberSchedulerHotReloadPreservesSuspendedFibers() {
    const auto compileFiberMod = [](const std::string& secondCmdArgSource) {
        const std::string src =
            "module Kernel.FiberReload;\n"
            "import RealScript.Fiber;\n"
            "public void WorkerRoutine() {\n"
            "    Fiber.Command(1, 0, 10);\n"
            "    Fiber.Yield(3);\n"
            "    Fiber.Command(2, 0, " + secondCmdArgSource + ");\n"
            "}\n";
        auto bundle = compileAndLink({{"fiber_reload.rscript", src}}, {}, optimization::Level::None);
        const mir::Module* mirMod = nullptr;
        for (const auto& m : bundle.mirModules) {
            if (m.name == "Kernel.FiberReload") {
                mirMod = &m;
                break;
            }
        }
        require(mirMod != nullptr, "Kernel.FiberReload MIR module missing");
        return runtime::FiberCompiler::compileAutoLoweredModule(*mirMod);
    };

    const auto modV1 = compileFiberMod("20");
    const auto modV2 = compileFiberMod("99");
    require(modV1.hasFunction("WorkerRoutine") && modV2.hasFunction("WorkerRoutine"),
        "CompiledFiberModule missing WorkerRoutine");

    MockFiberHost host;
    runtime::FiberScheduler sched(&host);
    const std::uint32_t progId = sched.registerModule(modV1);
    const auto ent = runtime::GenerationalEntityHandle{42, 1};
    const std::uint32_t fiberId = sched.spawnFiberByProgram(progId, modV1.entryPc("WorkerRoutine"), ent, 1);
    require(fiberId != 0u, "spawnFiberByProgram failed");

    // Run ticks 1..2: executes first Fiber.Command(1, 0, 10) and suspends on Fiber.Yield(3).
    (void)sched.stepTick(1);
    (void)sched.stepTick(2);
    require(sched.activeFiberCount() == 1u, "Expected 1 suspended fiber before hot reload");
    require(host.commands.size() == 1u && std::get<3>(host.commands[0]) == 10u,
        "Expected initial Fiber.Command(1, 0, 10) before hot reload");

    const auto report = sched.reloadModuleWithReport(progId, modV1, modV2);
    require(report.succeeded && report.preservedFibers == 1u && report.abortedFibers == 0u,
        "Expected FiberScheduler reloadModuleWithReport preservedFibers == 1 and abortedFibers == 0");

    // Step ticks 3..5 so suspended fiber wakes up and runs v2's post-yield Fiber.Command(2, 0, 99).
    for (std::uint32_t tick = 3; tick <= 5; ++tick) {
        (void)sched.stepTick(tick);
    }
    require(host.commands.size() == 2u &&
            std::get<1>(host.commands[1]) == 2u &&
            std::get<3>(host.commands[1]) == 99u,
        "Expected preserved fiber to resume into v2 post-yield instruction (arg32 == 99)");
}

} // namespace

int main() {
    try {
        std::vector<std::pair<const char*, void (*)()>> tests = {
            {"Sequence coroutines yield/break & exception rollback", testSequenceCoroutinesAndExceptionRollback},
            {"FiberScheduler auto-lowering, parallel parity & snapshots", testFiberSchedulerAutoLoweringAndDeterminism},
            {"ParallelJobScheduler verifier, aliasing & multi-core parity", testParallelJobVerifierAliasingAndDeterminism},
            {"AwaitableCorePool ABA safety, GC roots & CancellationTokenSource", testAwaitablePoolAbaAndCancellation},
            {"IAwaitScheduler fast path & true suspension/resumption", testDuckTypedAwaitSchedulerFastAndSuspendedPaths},
            {"Pillar 2: IAwaitScheduler immediate & resumed faults + finally cleanup", testAwaitSchedulerFaultAndDelayedResumePaths},
            {"ParallelJob RawColumnArray zero-copy, allowReadOnlyStaticColumnLoads & ParallelNativeBatchKernel", testParallelJobRawColumnsAndNativeBatchKernel},
            {"Non-game host module sequence lowering without RealScript.Game/Scene", testNonGameHostSequenceComposition},
            {"FiberScheduler reloadModuleWithReport preserves suspended fibers", testFiberSchedulerHotReloadPreservesSuspendedFibers},
        };
        for (const auto& [name, fn] : tests) {
            std::cout << "[RUN ] " << name << '\n';
            fn();
            std::cout << "[PASS] " << name << '\n';
        }
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[FAIL] " << ex.what() << '\n';
        return 1;
    }
}
