#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/optimization/Optimizer.h"
#include "realscript/runtime/AsyncRuntime.h"
#include "realscript/runtime/FiberScheduler.h"
#include "realscript/runtime/ParallelJob.h"
#include "realscript/runtime/Runtime.h"

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
