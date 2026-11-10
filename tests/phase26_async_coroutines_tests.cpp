#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/optimization/Optimizer.h"
#include "realscript/runtime/AsyncRuntime.h"
#include "realscript/runtime/Runtime.h"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace realscript;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::vector<bytecode::Module> compileModules(
    const std::string& source,
    optimization::Level optLevel = optimization::Level::Aggressive) {
    compiler::Compilation compilation({{"async_test.rs", source}});
    auto build = compilation.build();
    if (build.diagnostics.hasErrors()) {
        std::string msg = "Compilation failed:";
        for (const auto& d : build.diagnostics.items()) {
            msg += "\n  [" + d.code + "] " + d.message;
        }
        throw std::runtime_error(msg);
    }
    optimization::Options opts;
    opts.level = optLevel;
    auto optimized = optimization::Optimizer{}.optimize(
        build.modules, build.diagnostics, opts);
    require(!build.diagnostics.hasErrors(), "Optimizer reported errors");

    bytecode::Lowerer lowerer;
    std::vector<bytecode::Module> out;
    for (const auto& m : optimized.modules) {
        out.push_back(lowerer.lower(m));
    }
    return out;
}

bool hasDiagnosticCode(const std::string& source, const std::string& expectedCode) {
    compiler::Compilation compilation({{"diag_test.rs", source}});
    auto build = compilation.build();
    for (const auto& entry : build.diagnostics.items()) {
        if (entry.code == expectedCode) {
            return true;
        }
    }
    return false;
}

// -----------------------------------------------------------------------------
// 1. Frontend Diagnostics RS2600 - RS2605
// -----------------------------------------------------------------------------
void testFrontendDiagnostics() {
    // RS2600: invalid return type
    require(hasDiagnosticCode(R"RS(
module Diag.T0;
public async int BadReturn() {
    return 1;
}
)RS", "RS2600"), "expected RS2600 for non-Awaitable return type");

    // RS2601: ref return on async function
    require(hasDiagnosticCode(R"RS(
module Diag.T1;
public async ref Awaitable BadRefReturn(Awaitable[] xs) {
    return ref xs[0];
}
)RS", "RS2601"), "expected RS2601 for async ref return");

    // RS2602: ref / out parameter on async function
    require(hasDiagnosticCode(R"RS(
module Diag.T2;
public async Awaitable BadRefParam(ref int x) {
    await Awaitable.Completed();
}
)RS", "RS2602"), "expected RS2602 for async ref parameter");

    require(hasDiagnosticCode(R"RS(
module Diag.T2b;
public async Awaitable BadOutParam(out int x) {
    x = 1;
    await Awaitable.Completed();
}
)RS", "RS2602"), "expected RS2602 for async out parameter");

    // RS2603: async constructor & abstract async method
    require(hasDiagnosticCode(R"RS(
module Diag.T3a;
public class Foo {
    public async Foo() {}
}
)RS", "RS2603"), "expected RS2603 for async constructor");

    require(hasDiagnosticCode(R"RS(
module Diag.T3b;
public abstract class Base {
    public abstract async Awaitable Run();
}
)RS", "RS2603"), "expected RS2603 for abstract async method");

    // RS2604: await outside async function AND inside synchronous lambda inside async function
    require(hasDiagnosticCode(R"RS(
module Diag.T4a;
public int SyncFn() {
    await Awaitable.Completed();
    return 0;
}
)RS", "RS2604"), "expected RS2604 for await in sync function");

    require(hasDiagnosticCode(R"RS(
module Diag.T4b;
public delegate int IntMap(int x);
public async Awaitable<int> OuterAsync() {
    Awaitable<int> pending = Awaitable<int>.FromResult(5);
    IntMap fn = (v) => (await pending) + v;
    return fn(3);
}
)RS", "RS2604"), "expected RS2604 for await inside synchronous lambda");

    // RS2605: awaiting non-awaitable type
    require(hasDiagnosticCode(R"RS(
module Diag.T5;
public async Awaitable BadOperand() {
    await 123;
}
)RS", "RS2605"), "expected RS2605 for awaiting int");
}

// -----------------------------------------------------------------------------
// 2. Zero-Allocation Synchronous Fast Path & Steady-State Frame Pool Reuse
// -----------------------------------------------------------------------------
void testZeroAllocFastPathAndPoolBenchmark() {
    const std::string src = R"RS(
module Phase26.Bench;

public async Awaitable<int> SyncCompute(int x) {
    int a = await Awaitable<int>.FromResult(x + 3);
    await Awaitable.Completed();
    return a * 2;
}

public async Awaitable RunSyncLoopAsync(int iterations) {
    int acc = 0;
    for (int i = 0; i < iterations; i = i + 1) {
        int val = await SyncCompute(i);
        if (val > 0) { acc = acc + 1; }
    }
}

public int RunSyncLoop(int iterations) {
    Awaitable outer = RunSyncLoopAsync(iterations);
    return iterations;
}

public async Awaitable SuspendOnce(Awaitable sig, int[] counter) {
    int localScalar = counter[0] + 1;
    await sig;
    counter[0] = localScalar;
}

public int RunPooledSuspendLoop(int iterations) {
    int[] counter = new int[1];
    counter[0] = 0;
    for (int i = 0; i < iterations; i = i + 1) {
        Awaitable sig = Async.NewSignal();
        Awaitable task = SuspendOnce(sig, counter);
        Async.ResolveSignal(sig);
        Async.StepUpdate();
        task.Release();
    }
    return counter[0];
}

public async Awaitable ProtectedSuspendOnce(Awaitable sig, int[] counter) {
    try {
        await sig;
        counter[0] = counter[0] + 1;
    } finally {
        counter[0] = counter[0] + 0;
    }
}

public int RunProtectedTryFinallySuspendLoop(int iterations) {
    int[] counter = new int[1];
    counter[0] = 0;
    for (int i = 0; i < iterations; i = i + 1) {
        Awaitable sig = Async.NewSignal();
        Awaitable task = ProtectedSuspendOnce(sig, counter);
        Async.ResolveSignal(sig);
        Async.StepUpdate();
        task.Release();
    }
    return counter[0];
}
)RS";

    auto modules = compileModules(src);
    runtime::Interpreter vm(std::move(modules));
    runtime::Limits benchLimits{};
    benchLimits.instructionBudget = 500'000'000ULL;

    // Warmup synchronous path once
    const std::vector<runtime::Value> warmup10 = {std::int64_t{10}};
    require(std::get<std::int64_t>(
                vm.invoke("Phase26.Bench::RunSyncLoop", warmup10, benchLimits).value) == 10,
        "sync warmup failed");

    const auto heapBeforeSync = vm.heap()->statistics().objectsAllocated;
    const auto coroBeforeSync = vm.asyncRuntime()->coroutines().statistics().totalAllocated;

    constexpr std::int64_t kSyncIters = 50000;
    const std::vector<runtime::Value> argsSync = {kSyncIters};
    const auto t0 = std::chrono::steady_clock::now();
    const auto syncRes = vm.invoke("Phase26.Bench::RunSyncLoop", argsSync, benchLimits);
    const auto t1 = std::chrono::steady_clock::now();
    require(syncRes.succeeded && std::get<std::int64_t>(syncRes.value) == kSyncIters,
        "RunSyncLoop failed: " + syncRes.error.message);

    const auto heapAfterSync = vm.heap()->statistics().objectsAllocated;
    const auto coroAfterSync = vm.asyncRuntime()->coroutines().statistics().totalAllocated;
    require(heapAfterSync == heapBeforeSync,
        "synchronous async fast path allocated ManagedHeap objects!");
    require(coroAfterSync == coroBeforeSync,
        "synchronous async fast path allocated CoroutineFramePool slots!");

    // Warmup suspend+resume path so pool establishes 1 warm frame
    const std::vector<runtime::Value> warmup8 = {std::int64_t{8}};
    require(std::get<std::int64_t>(
                vm.invoke("Phase26.Bench::RunPooledSuspendLoop", warmup8, benchLimits).value) == 8,
        "pooled warmup failed");
    const auto heapBeforeSuspend = vm.heap()->statistics().objectsAllocated;

    constexpr std::int64_t kSuspendIters = 50000;
    const std::vector<runtime::Value> argsSusp = {kSuspendIters};
    const auto t2 = std::chrono::steady_clock::now();
    const auto suspRes = vm.invoke("Phase26.Bench::RunPooledSuspendLoop", argsSusp, benchLimits);
    const auto t3 = std::chrono::steady_clock::now();
    require(suspRes.succeeded && std::get<std::int64_t>(suspRes.value) == kSuspendIters,
        "RunPooledSuspendLoop failed: " + suspRes.error.message);

    const auto heapAfterSuspend = vm.heap()->statistics().objectsAllocated;
    const auto coroStats = vm.asyncRuntime()->coroutines().statistics();
    const auto awaitStats = vm.asyncRuntime()->awaitables().statistics();
    // Only the 1 `new int[1]` array outside the loop was allocated on ManagedHeap!
    require(heapAfterSuspend - heapBeforeSuspend == 1,
        "steady-state suspend/resume allocated unexpected ManagedHeap objects: " +
            std::to_string(heapAfterSuspend - heapBeforeSuspend));
    require(coroStats.capacity <= 2 && coroStats.activeCount == 0 && coroStats.activeGcRoots == 0,
        "CoroutineFramePool capacity grew unexpectedly or leaked active frames");
    require(awaitStats.capacity <= 256 && awaitStats.activeCount == 0 && awaitStats.activeGcRoots == 0,
        "AwaitableCorePool capacity grew beyond 1 page or leaked active handles");

    // Measure cross-await try/finally suspend+resume (still 0 per-op heap allocs!)
    (void)vm.invoke("Phase26.Bench::RunProtectedTryFinallySuspendLoop", warmup8, benchLimits);
    const auto heapBeforeProt = vm.heap()->statistics().objectsAllocated;
    const auto t4 = std::chrono::steady_clock::now();
    const auto protRes = vm.invoke("Phase26.Bench::RunProtectedTryFinallySuspendLoop", argsSusp, benchLimits);
    const auto t5 = std::chrono::steady_clock::now();
    require(protRes.succeeded && std::get<std::int64_t>(protRes.value) == kSuspendIters,
        "RunProtectedTryFinallySuspendLoop failed: " + protRes.error.message);
    const auto heapAfterProt = vm.heap()->statistics().objectsAllocated;
    require(heapAfterProt - heapBeforeProt == 1,
        "cross-await try/finally suspend/resume allocated unexpected ManagedHeap objects!");

    const double syncNs = std::chrono::duration<double, std::nano>(t1 - t0).count() /
        static_cast<double>(kSyncIters);
    const double suspNs = std::chrono::duration<double, std::nano>(t3 - t2).count() /
        static_cast<double>(kSuspendIters);
    const double protNs = std::chrono::duration<double, std::nano>(t5 - t4).count() /
        static_cast<double>(kSuspendIters);

    std::cout << std::fixed << std::setprecision(1)
              << "[bench] sync async fast-path: " << syncNs << " ns/op (0 heap allocs, 0 frame allocs)\n"
              << "[bench] pooled async suspend+resume: " << suspNs << " ns/op (0 per-op heap allocs, pool cap="
              << coroStats.capacity << ")\n"
              << "[bench] cross-await try/finally suspend+resume: " << protNs
              << " ns/op (0 per-op heap allocs)\n";
}

// -----------------------------------------------------------------------------
// 3. GC Survival Across Suspension, Block VM Parity & RSBC Binary Round-Trip
// -----------------------------------------------------------------------------
void testGcSurvivalBlockVmAndCodecRoundTrip() {
    const std::string src = R"RS(
module Phase26.GcAndCodec;

public class Payload {
    public string title;
    public int weight;

    public Payload(string t, int w) {
        title = t;
        weight = w;
    }
}

public class FaultBoom {
    public string reason;
    public FaultBoom(string r) {
        reason = r;
    }
}

public async Awaitable<int> GcStressCoroutine(Awaitable gate, string prefix, int baseWeight) {
    Payload localObj = new Payload(prefix + "-alive", baseWeight);
    string dynamicStr = localObj.title + ":verified";
    try {
        await gate;
        return localObj.weight + dynamicStr.Length;
    } finally {
        await Awaitable.Completed();
    }
}

public async Awaitable CaptureResult(Awaitable gate, int[] box) {
    try {
        int r = await GcStressCoroutine(gate, "asset", 100);
        box[0] = r;
    } catch (AsyncCanceledException ex) {
        box[0] = -404;
    } catch (FaultBoom fb) {
        box[0] = -500 - fb.reason.Length;
    }
}
)RS";

    auto compiled = compileModules(src);

    // Round-trip every module through binary RSBC codec!
    std::vector<bytecode::Module> decodedModules;
    for (const auto& mod : compiled) {
        const auto bytes = bytecode::encodeModule(mod);
        bytecode::Module decodedMod;
        diagnostics::DiagnosticBag decodeDiag;
        require(bytecode::decodeModule(bytes, decodedMod, decodeDiag) && !decodeDiag.hasErrors(),
            "RSBC decode failed for async module");
        decodedModules.push_back(std::move(decodedMod));
    }

    for (bool useBlockVmViaTrace : {false, true}) {
        runtime::Interpreter vm(decodedModules);
        runtime::ExecutionOptions execOpts{};
        if (useBlockVmViaTrace) {
            execOpts.trace = [](const runtime::TraceEvent&) {};
        }

        const auto gate = vm.asyncRuntime()->awaitables().allocatePending();
        const auto gateVal = runtime::makeAwaitableStructValue(
            runtime::awaitableVoidTypeId(), gate);
        const auto boxOpt = vm.heap()->allocateTypedArray(semantic::stableTypeId("int[]"), semantic::PrimitiveType::Int, 0, 1, runtime::Value(std::int64_t{0}));
        require(boxOpt.has_value(), "box allocation failed");
        const auto boxPin = vm.heap()->retain(runtime::Value(*boxOpt));

        const std::vector<runtime::Value> callArgs = {gateVal, runtime::Value(*boxOpt)};
        const auto launch = vm.invoke("Phase26.GcAndCodec::CaptureResult", callArgs, execOpts);
        require(launch.succeeded, "CaptureResult launch failed: " + launch.error.message);

        // Allocate garbage and force full mark-sweep GC while both coroutines are suspended!
        for (int i = 0; i < 200; ++i) {
            (void)vm.heap()->allocateString("ephemeral-garbage-" + std::to_string(i));
        }
        {
            runtime::ShadowStack emptyStack;
            vm.heap()->collectFull(emptyStack);
        }
        require(vm.asyncRuntime()->coroutines().statistics().activeGcRoots > 0,
            "suspended coroutine with live ObjectRef locals must pin GC roots");

        require(vm.asyncRuntime()->awaitables().trySetResult(gate), "resolve gate failed");
        vm.asyncRuntime()->stepResumeQueue(runtime::ResumeQueue::Update);

        const auto resVal = vm.heap()->arrayGet(*boxOpt, 0);
        // "asset-alive:verified" has length 20 -> 100 + 20 = 120
        require(resVal && std::get<std::int64_t>(*resVal) == 120,
            std::string(useBlockVmViaTrace ? "[BlockVM]" : "[FlatVM]") +
                " GC survival result mismatch");
        require(vm.asyncRuntime()->coroutines().statistics().activeGcRoots == 0 &&
                vm.asyncRuntime()->coroutines().statistics().activeCount == 0,
            "GC roots or coroutine frames leaked after completion");
    }
}

// -----------------------------------------------------------------------------
// 4. Uncaught Exception -> Faulted / Canceled Awaitable + Background I/O Replay
// -----------------------------------------------------------------------------
void testUncaughtStatesAndDeterministicBackgroundIo() {
    const std::string src = R"RS(
module Phase26.IoAndFaults;

public class CustomCrash {
    public string message;
    public CustomCrash(string m) {
        message = m;
    }
}

public async Awaitable ThrowCustomAfterYield() {
    await Awaitable.Yield();
    throw new CustomCrash("shader-compile-failed");
}

public async Awaitable CancelViaToken(CancellationToken tok) {
    await Awaitable.YieldWithToken(tok);
}

public async Awaitable ConsumeHostString(Awaitable<string> ioTask, int[] outLen) {
    try {
        string text = await ioTask;
        outLen[0] = text.Length;
    } catch (AsyncException ex) {
        outLen[0] = -ex.message.Length;
    }
}
)RS";

    auto modules = compileModules(src);

    // A. Uncaught custom exception resolves Awaitable to Faulted (never crashes VM)
    {
        runtime::Interpreter vm(modules);
        const std::vector<runtime::Value> noArgs{};
        const auto r = vm.invoke("Phase26.IoAndFaults::ThrowCustomAfterYield", noArgs);
        require(r.succeeded, "async throw after yield should not fail synchronous invoke");
        runtime::AwaitableHandle h{};
        require(runtime::tryExtractAwaitableHandle(r.value, h), "invalid Awaitable struct");
        require(vm.asyncRuntime()->awaitables().status(h) == runtime::AwaitStatus::Pending,
            "expected Pending before StepUpdate");

        vm.asyncRuntime()->stepResumeQueue(runtime::ResumeQueue::Update);
        require(vm.asyncRuntime()->awaitables().status(h) == runtime::AwaitStatus::Faulted,
            "uncaught CustomCrash must transition Awaitable to Faulted");

        runtime::Value unusedVal{};
        runtime::RuntimeError faultErr{};
        runtime::Value excObj{};
        const auto st = vm.asyncRuntime()->awaitables().getResult(h, unusedVal, faultErr, &excObj);
        require(st == runtime::AwaitStatus::Faulted &&
                faultErr.message == "shader-compile-failed" &&
                std::holds_alternative<runtime::ObjectRef>(excObj),
            "Faulted Awaitable did not preserve custom exception message and ObjectRef");
    }

    // B. Uncaught cancellation resolves Awaitable to Canceled
    {
        runtime::Interpreter vm(modules);
        const auto cts = vm.asyncRuntime()->cancellations().createSource();
        const auto tok = vm.asyncRuntime()->cancellations().token(cts);
        const std::array<runtime::Value, 1> tokFields = {
            runtime::Value(runtime::LongValue{static_cast<std::int64_t>(tok.pack64())}),
        };
        const runtime::Value tokStruct(runtime::makeStructValueFromFields(
            semantic::stableTypeId("RealScript.Async::CancellationToken"), tokFields));

        const std::vector<runtime::Value> cancelArgs = {tokStruct};
        const auto r = vm.invoke("Phase26.IoAndFaults::CancelViaToken", cancelArgs);
        require(r.succeeded, "CancelViaToken launch failed");
        runtime::AwaitableHandle h{};
        require(runtime::tryExtractAwaitableHandle(r.value, h), "invalid Awaitable struct");

        (void)vm.asyncRuntime()->cancellations().cancel(cts);
        vm.asyncRuntime()->stepResumeQueue(runtime::ResumeQueue::Update);
        require(vm.asyncRuntime()->awaitables().status(h) == runtime::AwaitStatus::Canceled,
            "uncaught token cancellation must transition Awaitable to Canceled");
        (void)vm.asyncRuntime()->awaitables().discard(h);
        (void)vm.asyncRuntime()->cancellations().releaseSource(cts);
    }

    // C. Background C++ worker thread -> AsyncCompletionQueue -> Deterministic Record & Replay!
    auto sharedReplayLog = std::make_shared<runtime::ReplayLog>();
    std::uint64_t recordedDigest = 0;
    {
        runtime::Interpreter vm(modules);
        runtime::DeterminismOptions detOpts;
        detOpts.mode = runtime::DeterminismMode::Record;
        detOpts.replayLog = sharedReplayLog;
        runtime::DeterminismSession recordSession(detOpts);

        const auto ioHandle = vm.asyncRuntime()->awaitables().allocatePending(
            runtime::ResumeQueue::Update);
        const auto reqId = vm.asyncRuntime()->completionQueue().registerRequest(
            ioHandle, "Asset.LoadShaderText", 0xCAFEULL);

        const auto ioStruct = runtime::makeAwaitableStructValue(
            semantic::stableTypeId("RealScript.Async::Awaitable__string"), ioHandle);
        const auto outArr = vm.heap()->allocateTypedArray(semantic::stableTypeId("int[]"), semantic::PrimitiveType::Int, 0, 1, runtime::Value(std::int64_t{0}));
        const auto arrPin = vm.heap()->retain(runtime::Value(*outArr));

        const std::vector<runtime::Value> ioArgs = {ioStruct, runtime::Value(*outArr)};
        const auto launch = vm.invoke("Phase26.IoAndFaults::ConsumeHostString", ioArgs);
        require(launch.succeeded, "ConsumeHostString launch failed");

        std::thread bgWorker([&]() {
            vm.asyncRuntime()->completionQueue().submitString(
                reqId, "void main() { gl_Position = vec4(1.0); }");
        });
        bgWorker.join();

        runtime::RuntimeError ioErr{};
        require(vm.asyncRuntime()->stepPreTickIo(&recordSession, ioErr),
            "stepPreTickIo record failed: " + ioErr.message);
        vm.asyncRuntime()->stepResumeQueue(runtime::ResumeQueue::Update);

        const auto lenVal = vm.heap()->arrayGet(*outArr, 0);
        require(lenVal && std::get<std::int64_t>(*lenVal) == 40,
            "background string completion length mismatch in Record session");
        recordedDigest = recordSession.digest();
        require(recordedDigest != 0ULL, "expected non-zero determinism digest");
    }

    // Replay session WITHOUT spawning any background thread!
    {
        runtime::Interpreter vm(modules);
        runtime::DeterminismOptions detOpts;
        detOpts.mode = runtime::DeterminismMode::Replay;
        detOpts.replayLog = sharedReplayLog;
        runtime::DeterminismSession replaySession(detOpts);

        const auto ioHandle = vm.asyncRuntime()->awaitables().allocatePending(
            runtime::ResumeQueue::Update);
        (void)vm.asyncRuntime()->completionQueue().registerRequest(
            ioHandle, "Asset.LoadShaderText", 0xCAFEULL);

        const auto ioStruct = runtime::makeAwaitableStructValue(
            semantic::stableTypeId("RealScript.Async::Awaitable__string"), ioHandle);
        const auto outArr = vm.heap()->allocateTypedArray(semantic::stableTypeId("int[]"), semantic::PrimitiveType::Int, 0, 1, runtime::Value(std::int64_t{0}));
        const auto arrPin = vm.heap()->retain(runtime::Value(*outArr));

        const std::vector<runtime::Value> ioArgs = {ioStruct, runtime::Value(*outArr)};
        const auto launch = vm.invoke("Phase26.IoAndFaults::ConsumeHostString", ioArgs);
        require(launch.succeeded, "ConsumeHostString replay launch failed");

        runtime::RuntimeError ioErr{};
        require(vm.asyncRuntime()->stepPreTickIo(&replaySession, ioErr),
            "stepPreTickIo replay failed: " + ioErr.message);
        vm.asyncRuntime()->stepResumeQueue(runtime::ResumeQueue::Update);

        const auto lenVal = vm.heap()->arrayGet(*outArr, 0);
        require(lenVal && std::get<std::int64_t>(*lenVal) == 40 &&
                replaySession.digest() == recordedDigest,
            "deterministic Replay did not reproduce exact async coroutine result and digest");
    }
}

// -----------------------------------------------------------------------------
// 5. Stage 2 Audit Regressions (Fatal RuntimeError, Capabilities, Cross-Module,
//    Qualified Generic Syntax, Multi-Wave Budget & Heap Migration)
// -----------------------------------------------------------------------------
void testStage2AuditRegressions() {
    // A. Resumed coroutine hitting a fatal non-exception RuntimeError (DivisionByZero)
    //    must transition completionHandle to Faulted and not corrupt caller's StepUpdate()
    {
        const std::string src = R"RS(
module Phase26.AuditFault;

public async Awaitable<int> DivideByZeroAfterYield(int denom) {
    await Awaitable.Yield();
    return 100 / denom;
}

public async Awaitable CatchDivideByZeroAsync(int[] outBox) {
    try {
        int v = await DivideByZeroAfterYield(0);
        outBox[0] = v;
    } catch (AsyncException ex) {
        outBox[0] = 42;
    }
}

public int StepAndInspectFault() {
    int[] box = new int[1];
    box[0] = -1;
    Awaitable outer = CatchDivideByZeroAsync(box);
    if (outer.IsCompleted()) {
        return -1;
    }
    int ran = Async.StepUpdate();
    if (ran < 1 || !outer.IsCompleted()) {
        return -2;
    }
    outer.Release();
    return box[0];
}
)RS";
        auto modules = compileModules(src);
        runtime::Interpreter vm(std::move(modules));
        const auto res = vm.invoke("Phase26.AuditFault::StepAndInspectFault", {});
        require(res.succeeded && std::get<std::int64_t>(res.value) == 42,
            "DivideByZero inside resumed coroutine failed to fault cleanly or polluted caller: " +
                res.error.message);
        require(vm.asyncRuntime()->coroutines().statistics().activeCount == 0 &&
                vm.asyncRuntime()->awaitables().statistics().activeCount == 0,
            "DivideByZero coroutine leaked frame or awaitable handle");
    }

    // B. Host-driven resumption outside invoke() must preserve ExecutionOptions (Capabilities)
    {
        const std::string hostSrc = R"RS(
module Phase26.HostExtern;

public int RestrictedCall(int x) {
    return x;
}
)RS";
        const std::string callerSrc = R"RS(
module Phase26.AuditCaps;
import Phase26.HostExtern;

public async Awaitable<int> ResumeIntoRestrictedExtern() {
    await Awaitable.Yield();
    return RestrictedCall(7);
}
)RS";
        compiler::Compilation compilation({
            {"host_extern.rs", hostSrc},
            {"caller_caps.rs", callerSrc},
        });
        auto build = compilation.build();
        require(!build.diagnostics.hasErrors(), "AuditCaps compilation failed");
        bytecode::Lowerer lowerer;
        std::vector<bytecode::Module> runtimeModules;
        for (const auto& m : build.modules) {
            if (m.name == "Phase26.HostExtern") continue; // Leave RestrictedCall external!
            runtimeModules.push_back(lowerer.lower(m));
        }

        runtime::Interpreter vm(std::move(runtimeModules));
        vm.setExternalResolver(
            [](const bytecode::FunctionReference&,
               runtime::ValueSpan args,
               runtime::RuntimeError&) -> std::optional<runtime::Value> {
                return args.empty() ? runtime::Value(std::int64_t{0}) : args[0];
            });

        runtime::ExecutionOptions strictCaps{};
        strictCaps.enforceCapabilities = true; // Network.Http NOT granted!

        const auto launch = vm.invoke(
            "Phase26.AuditCaps::ResumeIntoRestrictedExtern", {}, strictCaps);
        require(launch.succeeded, "launch before await should succeed");
        runtime::AwaitableHandle h{};
        require(runtime::tryExtractAwaitableHandle(launch.value, h), "invalid Awaitable<int>");
        require(vm.asyncRuntime()->awaitables().status(h) == runtime::AwaitStatus::Pending,
            "expected Pending before host stepResumeQueue");

        // Host steps resume queue outside invoke() - must still enforce strictCaps!
        vm.asyncRuntime()->stepResumeQueue(runtime::ResumeQueue::Update);
        require(vm.asyncRuntime()->awaitables().status(h) == runtime::AwaitStatus::Faulted,
            "host-driven resumption did not enforce capability check after await");

        runtime::Value unused{};
        runtime::RuntimeError faultErr{};
        (void)vm.asyncRuntime()->awaitables().getResult(h, unused, faultErr);
        require(faultErr.code == runtime::ErrorCode::CapabilityDenied,
            "expected CapabilityDenied on unauthorized extern after await");
    }

    // C. Cross-module Awaitable<UserType> specialization & multi-wave StepUpdate
    {
        const std::string modASrc = R"RS(
module Phase26.CrossA;

public class SharedItem {
    public int id;
    public SharedItem(int i) {
        id = i;
    }
}

public async Awaitable<SharedItem> ProduceItem(int baseId) {
    await Awaitable.Yield();
    return await Awaitable<SharedItem>.FromResult(new SharedItem(baseId + 5));
}
)RS";
        const std::string modBSrc = R"RS(
module Phase26.CrossB;
import Phase26.CrossA;

public async Awaitable<int> ConsumeFromModuleB(int[] box) {
    Awaitable<SharedItem> task = ProduceItem(37);
    SharedItem item = await task;
    box[0] = item.id;
    return item.id;
}

public int RunCrossModuleTest() {
    int[] box = new int[1];
    box[0] = 0;
    Awaitable<int> top = ConsumeFromModuleB(box);
    // Wave 1 resumes ProduceItem -> resolves task -> Wave 2 resumes ConsumeFromModuleB in one StepUpdate!
    Async.StepUpdate();
    if (!top.IsCompleted()) {
        return -1;
    }
    top.Release();
    return box[0];
}
)RS";
        compiler::Compilation compilation({
            {"cross_a.rs", modASrc},
            {"cross_b.rs", modBSrc},
        });
        auto build = compilation.build();
        require(!build.diagnostics.hasErrors(),
            "Cross-module Awaitable<SharedItem> compilation failed");
        optimization::Options optOpts;
        optOpts.level = optimization::Level::Aggressive;
        auto optimized = optimization::Optimizer{}.optimize(
            build.modules, build.diagnostics, optOpts);
        require(!build.diagnostics.hasErrors(), "Cross-module optimization failed");
        bytecode::Lowerer lowerer;
        std::vector<bytecode::Module> modules;
        for (const auto& m : optimized.modules) {
            modules.push_back(lowerer.lower(m));
        }

        runtime::Interpreter vm(std::move(modules));
        const auto res = vm.invoke("Phase26.CrossB::RunCrossModuleTest", {});
        require(res.succeeded && std::get<std::int64_t>(res.value) == 42,
            "Cross-module Awaitable<SharedItem> execution failed: " + res.error.message);
    }

    // D. CoroutineFramePool::setHeap root detachment and re-attachment while suspended
    {
        runtime::ManagedHeap heap1;
        runtime::AsyncRuntimeContext ctx(&heap1);

        const auto strRef = heap1.allocateString("pinned-across-heap-switch");
        require(strRef.has_value(), "failed to allocate test string");
        const runtime::Value localVal(*strRef);

        const auto completion = ctx.awaitables().allocatePending();
        const auto awaited = ctx.awaitables().allocatePending();
        const auto frameHandle = ctx.coroutines().allocateFrame(
            1, runtime::awaitableVoidTypeId(), completion, runtime::ResumeQueue::Update);

        require(ctx.coroutines().suspendFrame(
            frameHandle,
            nullptr, 0,
            &localVal, 1,
            nullptr, 0,
            1,
            awaited,
            [](void*,
               runtime::CoroutineFrameHandle,
               runtime::CoroutineFrame&,
               runtime::AsyncRuntimeContext&) {},
            nullptr),
            "suspendFrame failed");
        require(ctx.coroutines().statistics().activeGcRoots == 1,
            "expected 1 active GC root on heap1");

        ctx.setHeap(nullptr);
        require(ctx.coroutines().statistics().activeGcRoots == 0,
            "expected 0 active GC roots after setHeap(nullptr)");

        ctx.setHeap(&heap1);
        require(ctx.coroutines().statistics().activeGcRoots == 1,
            "expected 1 re-pinned GC root after setHeap(&heap1)");
    }
}

} // namespace

int main() {
    try {
        testFrontendDiagnostics();
        testZeroAllocFastPathAndPoolBenchmark();
        testGcSurvivalBlockVmAndCodecRoundTrip();
        testUncaughtStatesAndDeterministicBackgroundIo();
        testStage2AuditRegressions();
        std::cout << "All Phase 26 async coroutines, diagnostics, GC, codec & replay tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Phase 26 async coroutines test failure: " << ex.what() << "\n";
        return 1;
    }
}
