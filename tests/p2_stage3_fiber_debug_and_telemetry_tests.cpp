#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/debug/DapServer.h"
#include "realscript/optimization/Optimizer.h"
#include "realscript/runtime/AsyncRuntime.h"
#include "realscript/runtime/FiberScheduler.h"
#include "realscript/runtime/Runtime.h"

#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace realscript;

namespace {

std::atomic<bool> gTrackHeapAllocs{false};
std::atomic<std::size_t> gObservedHeapAllocs{0};

} // namespace

#if defined(_MSC_VER)
#define RS_TEST_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define RS_TEST_NOINLINE __attribute__((noinline))
#else
#define RS_TEST_NOINLINE
#endif

RS_TEST_NOINLINE void* operator new(std::size_t size) {
    if (gTrackHeapAllocs.load(std::memory_order_relaxed)) {
        gObservedHeapAllocs.fetch_add(1, std::memory_order_relaxed);
    }
    if (void* p = std::malloc(size == 0 ? 1 : size)) {
        return p;
    }
    throw std::bad_alloc();
}

RS_TEST_NOINLINE void operator delete(void* p) noexcept {
    std::free(p);
}

RS_TEST_NOINLINE void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}

namespace {

void require(bool cond, const std::string& msg) {
    if (!cond) {
        throw std::runtime_error("P2-D verification failed: " + msg);
    }
}

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

void testFiberAutoLoweringDebugInfoAndDapIntrospection() {
    std::cout << "[1/3] Testing FiberCompiler::canAutoLowerToFiber, FiberDebugInfo & DapServer virtual Fiber threads...\n";

    // Keep exact line numbers deterministic for 1-based line assertions:
    const std::string fiberSrc =
        "module P2D.Behaviors;\n"                           // line 1
        "import RealScript.Fiber;\n"                        // line 2
        "import RealScript.Math;\n"                         // line 3
        "import RealScript.Async;\n"                        // line 4
        "\n"                                                // line 5
        "public class HeapBox {\n"                          // line 6
        "    public int payload;\n"                         // line 7
        "    public HeapBox(int v) { payload = v; }\n"      // line 8
        "}\n"                                               // line 9
        "\n"                                                // line 10
        "public void PatrolWithFixed() {\n"                 // line 11
        "    Fixed speed = Fixed.Of(2) + Fixed.Half();\n"   // line 12
        "    int step = 0;\n"                               // line 13
        "    while (step < 4) {\n"                          // line 14
        "        step = step + 1;\n"                        // line 15
        "        speed = speed + Fixed.Half();\n"           // line 16
        "        Fiber.Command(1, 0, 10);\n"                // line 17
        "        Fiber.Yield(5);\n"                         // line 18  <-- YieldTicks!
        "    }\n"                                           // line 19
        "}\n"                                               // line 20
        "\n"                                                // line 21
        "public void SubStep() {\n"                         // line 22
        "    int innerCount = 7;\n"                         // line 23
        "    Fiber.Yield(3);\n"                             // line 24  <-- Sub-call YieldTicks!
        "}\n"                                               // line 25
        "\n"                                                // line 26
        "public void CallerRoutine() {\n"                   // line 27
        "    int phase = 2;\n"                              // line 28
        "    Fiber.WaitUntil(2, 42, 3);\n"                  // line 29  <-- Blackboard wait!
        "    SubStep();\n"                                  // line 30  <-- Call site!
        "}\n"                                               // line 31
        "\n"                                                // line 32
        "public async Awaitable PureAsyncPatrol() {\n"      // line 33
        "    int tickCount = 0;\n"                          // line 34
        "    while (tickCount < 3) {\n"                     // line 35
        "        tickCount = tickCount + 1;\n"              // line 36
        "        Fiber.Command(9, 0, 1);\n"                 // line 37
        "        await Awaitable.Yield();\n"                // line 38  <-- Auto-lowered async yield!
        "    }\n"                                           // line 39
        "}\n"                                               // line 40
        "\n"                                                // line 41
        "public int HeapCapturingFunc() {\n"                // line 42
        "    HeapBox b = new HeapBox(99);\n"                // line 43
        "    int[] arr = new int[4];\n"                     // line 44
        "    arr[0] = b.payload;\n"                         // line 45
        "    return arr[0];\n"                              // line 46
        "}\n"                                               // line 47
        "\n"                                                // line 48
        "public double FloatRejectedFunc(double x) {\n"     // line 49
        "    return x * 1.5;\n"                             // line 50
        "}\n";                                              // line 51

    compiler::Compilation comp({{"p2d_fibers.rscript", fiberSrc}});
    auto build = comp.build();
    require(!build.diagnostics.hasErrors(), "p2d_fibers.rscript failed to compile");

    const mir::Module* targetMir = nullptr;
    for (const auto& m : build.modules) {
        if (m.name == "P2D.Behaviors") {
            targetMir = &m;
            break;
        }
    }
    require(targetMir != nullptr, "P2D.Behaviors MIR module missing");

    const auto findMirFn = [&](const std::string& name) -> const mir::Function* {
        for (const auto& f : targetMir->functions) {
            if (f.name == name) return &f;
        }
        return nullptr;
    };

    const auto* patrolFn = findMirFn("PatrolWithFixed");
    const auto* subStepFn = findMirFn("SubStep");
    const auto* callerFn = findMirFn("CallerRoutine");
    const auto* pureAsyncFn = findMirFn("PureAsyncPatrol");
    const auto* heapFn = findMirFn("HeapCapturingFunc");
    const auto* floatFn = findMirFn("FloatRejectedFunc");
    const auto* ctorFn = findMirFn("HeapBox..ctor");
    require(patrolFn && subStepFn && callerFn && pureAsyncFn && heapFn && floatFn && ctorFn,
        "expected MIR functions not found");

    // 1. Verify static auto-lowering eligibility analysis
    require(runtime::FiberCompiler::canAutoLowerToFiber(*targetMir, *patrolFn),
        "PatrolWithFixed (Fixed + int + YieldTicks) must be auto-lowerable");
    require(runtime::FiberCompiler::canAutoLowerToFiber(*targetMir, *subStepFn),
        "SubStep must be auto-lowerable");
    require(runtime::FiberCompiler::canAutoLowerToFiber(*targetMir, *callerFn),
        "CallerRoutine (WaitUntil + intra-module Call) must be auto-lowerable");
    require(runtime::FiberCompiler::canAutoLowerToFiber(*targetMir, *pureAsyncFn),
        "PureAsyncPatrol (async Awaitable with pure scalar + Awaitable.Yield) must be auto-lowerable");
    require(!runtime::FiberCompiler::canAutoLowerToFiber(*targetMir, *heapFn),
        "HeapCapturingFunc (NewObject + NewArray) must be rejected by canAutoLowerToFiber");
    require(!runtime::FiberCompiler::canAutoLowerToFiber(*targetMir, *floatFn),
        "FloatRejectedFunc (IEEE-754 double) must be rejected by canAutoLowerToFiber");
    require(!runtime::FiberCompiler::canAutoLowerToFiber(*targetMir, *ctorFn),
        "Class constructor with `this` Object parameter must be rejected by canAutoLowerToFiber");

    // 2. Compile auto-lowered Fiber module & check FiberDebugInfo
    const runtime::CompiledFiberModule fiberMod =
        runtime::FiberCompiler::compileAutoLoweredModule(*targetMir);
    require(fiberMod.hasFunction("PatrolWithFixed"), "PatrolWithFixed missing in CompiledFiberModule");
    require(fiberMod.hasFunction("CallerRoutine"), "CallerRoutine missing in CompiledFiberModule");
    require(fiberMod.hasFunction("PureAsyncPatrol"), "PureAsyncPatrol missing in CompiledFiberModule");
    require(!fiberMod.hasFunction("HeapCapturingFunc"), "HeapCapturingFunc must not be in compileAutoLoweredModule");
    require(fiberMod.instructionLocations.size() == fiberMod.instructions.size(),
        "instructionLocations must be 1:1 with instructions");

    const auto* patrolMeta = fiberMod.findFunction("PatrolWithFixed");
    require(patrolMeta != nullptr && patrolMeta->autoLowerable, "PatrolWithFixed metadata invalid");
    require(patrolMeta->debugInfo.sourcePath == "p2d_fibers.rscript", "sourcePath mismatch");
    require(patrolMeta->debugInfo.registerNames[0] == "speed", "slot 0 should be 'speed'");
    require(patrolMeta->debugInfo.registerNames[1] == "step", "slot 1 should be 'step'");

    // 3. Step FiberScheduler and inspect via DapServer
    MockFiberHost host;
    runtime::FiberScheduler scheduler(&host);
    const auto ent1 = runtime::GenerationalEntityHandle{7, 1};
    const std::uint32_t fidPatrol = scheduler.spawnFunction(fiberMod, "PatrolWithFixed", ent1, 1);
    const std::uint32_t fidCaller = scheduler.spawnFunction(fiberMod, "CallerRoutine", runtime::GenerationalEntityHandle{8, 2}, 1);
    (void)scheduler.stepTick(10);

    // Spawn a 3rd fiber AFTER tick 10 so it remains in Ready ("Running") status!
    const std::uint32_t fidAsync = scheduler.spawnFunction(fiberMod, "PureAsyncPatrol", runtime::GenerationalEntityHandle{9, 1}, 1);

    bytecode::Lowerer bcLowerer;
    std::vector<bytecode::Module> bcMods;
    for (const auto& m : build.modules) bcMods.push_back(bcLowerer.lower(m));
    runtime::RuntimeError linkErr;
    auto linked = runtime::ProgramImage::link(std::move(bcMods), linkErr);
    require(linked.has_value(), "ProgramImage::link failed");
    auto programPtr = std::make_shared<const runtime::ProgramImage>(std::move(*linked));
    auto heapPtr = std::make_shared<runtime::ManagedHeap>();

    debug::DapServer dap(programPtr, heapPtr);
    dap.bindFiberScheduler(&scheduler, &fiberMod);

    // 3a. Verify DAP `threads`
    const auto threadsResp = dap.handle(tooling::Json::Object{
        {"seq", 1}, {"type", "request"}, {"command", "threads"},
    });
    require(threadsResp.find("success")->boolValue(), "DAP threads failed");
    const auto& threadsArr = threadsResp.find("body")->find("threads")->arrayValue();
    require(threadsArr.size() == 4, "Expected 1 main thread + 3 virtual Fiber threads, got " + std::to_string(threadsArr.size()));
    require(threadsArr[0].find("id")->integerValue() == 1, "Main thread id must be 1");
    require(threadsArr[1].find("id")->integerValue() == 10000 + static_cast<std::int64_t>(fidPatrol), "Fiber #1 threadId mismatch");
    require(threadsArr[1].find("name")->stringValue() == "Fiber #1 (PatrolWithFixed - WaitingTicks)",
        "Unexpected Fiber #1 name: " + threadsArr[1].find("name")->stringValue());
    require(threadsArr[2].find("name")->stringValue() == "Fiber #2 (CallerRoutine - WaitingPredicate)",
        "Unexpected Fiber #2 name: " + threadsArr[2].find("name")->stringValue());
    require(threadsArr[3].find("name")->stringValue() == "Fiber #3 (PureAsyncPatrol - Running)",
        "Unexpected Fiber #3 name: " + threadsArr[3].find("name")->stringValue());

    // 3b. Verify DAP `stackTrace` on Fiber #1 (suspended at `Fiber.Yield(5);` on line 18!)
    const auto st1Resp = dap.handle(tooling::Json::Object{
        {"seq", 2}, {"type", "request"}, {"command", "stackTrace"},
        {"arguments", tooling::Json::Object{{"threadId", 10000 + static_cast<std::int64_t>(fidPatrol)}}},
    });
    const auto& frames1 = st1Resp.find("body")->find("stackFrames")->arrayValue();
    require(frames1.size() == 1, "Fiber #1 should have 1 frame");
    require(frames1[0].find("name")->stringValue() == "PatrolWithFixed", "Fiber #1 frame name mismatch");
    require(frames1[0].find("line")->integerValue() == 18,
        "Fiber #1 suspended line should be 18 (Fiber.Yield(5)), got " +
        std::to_string(frames1[0].find("line")->integerValue()));
    require(frames1[0].find("source")->find("name")->stringValue() == "p2d_fibers.rscript",
        "Fiber #1 source name mismatch");

    // 3c. Verify DAP `scopes` and `variables` on Fiber #1
    const std::int64_t f1FrameId = frames1[0].find("id")->integerValue();
    const auto scopesResp = dap.handle(tooling::Json::Object{
        {"seq", 3}, {"type", "request"}, {"command", "scopes"},
        {"arguments", tooling::Json::Object{{"frameId", f1FrameId}}},
    });
    const auto& scopesArr = scopesResp.find("body")->find("scopes")->arrayValue();
    require(scopesArr.size() == 2, "Expected 2 Fiber scopes (Fiber State + Locals)");
    const std::int64_t stateRef = scopesArr[0].find("variablesReference")->integerValue();
    const std::int64_t localsRef = scopesArr[1].find("variablesReference")->integerValue();

    const auto stateVarsResp = dap.handle(tooling::Json::Object{
        {"seq", 4}, {"type", "request"}, {"command", "variables"},
        {"arguments", tooling::Json::Object{{"variablesReference", stateRef}}},
    });
    const auto& stateVars = stateVarsResp.find("body")->find("variables")->arrayValue();
    bool sawStatus = false, sawRemTicks = false, sawEntity = false;
    for (const auto& v : stateVars) {
        const auto vName = v.find("name")->stringValue();
        const auto vVal = v.find("value")->stringValue();
        if (vName == "status" && vVal == "WaitingTicks") sawStatus = true;
        if (vName == "remainingTicks" && vVal == "5") sawRemTicks = true;
        if (vName == "boundEntity" && vVal == "7:1") sawEntity = true;
    }
    require(sawStatus && sawRemTicks && sawEntity, "Fiber State variables missing status/remainingTicks/boundEntity");

    const auto localVarsResp = dap.handle(tooling::Json::Object{
        {"seq", 5}, {"type", "request"}, {"command", "variables"},
        {"arguments", tooling::Json::Object{{"variablesReference", localsRef}}},
    });
    const auto& localVars = localVarsResp.find("body")->find("variables")->arrayValue();
    bool sawSpeed = false, sawStep = false;
    for (const auto& v : localVars) {
        const auto vName = v.find("name")->stringValue();
        const auto vVal = v.find("value")->stringValue();
        const auto vType = v.find("type")->stringValue();
        // Initial speed = 2.5 (640), + 0.5 (128) = 3.0000 (raw=768)
        if (vName == "speed" && vType == "Fixed" && vVal.find("3.0000 (raw=768)") != std::string::npos) {
            sawSpeed = true;
        }
        if (vName == "step" && vVal == "1") {
            sawStep = true;
        }
    }
    require(sawSpeed && sawStep, "Fiber Locals missing Fixed `speed` (3.0000 raw=768) or `step` (1)");

    // 3d. Wake Fiber #2 (`CallerRoutine`) via blackboard event so it calls `SubStep()` and suspends at line 24!
    require(scheduler.notifyBlackboardEvent(1, 2, 42, 3) == 1, "Expected Fiber #2 to wake on blackboard event");
    (void)scheduler.stepTick(11);

    const auto st2Resp = dap.handle(tooling::Json::Object{
        {"seq", 6}, {"type", "request"}, {"command", "stackTrace"},
        {"arguments", tooling::Json::Object{{"threadId", 10000 + static_cast<std::int64_t>(fidCaller)}}},
    });
    const auto& frames2 = st2Resp.find("body")->find("stackFrames")->arrayValue();
    require(frames2.size() == 2, "Fiber #2 should have 2 stack frames (SubStep + CallerRoutine)");
    require(frames2[0].find("name")->stringValue() == "SubStep" &&
            frames2[0].find("line")->integerValue() == 24,
        "Top frame of Fiber #2 should be SubStep at line 24, got line " +
        std::to_string(frames2[0].find("line")->integerValue()));
    require(frames2[1].find("name")->stringValue() == "CallerRoutine" &&
            frames2[1].find("line")->integerValue() == 30,
        "Caller frame of Fiber #2 should be CallerRoutine at line 30, got line " +
        std::to_string(frames2[1].find("line")->integerValue()));

    // Verify PureAsyncPatrol ran its first tick on FiberScheduler and emitted Command(9, 0, 1)!
    bool sawAsyncCmd = false;
    for (const auto& c : host.commands) {
        if (std::get<0>(c) == fidAsync && std::get<1>(c) == 9) sawAsyncCmd = true;
    }
    require(sawAsyncCmd, "Auto-lowered async coroutine PureAsyncPatrol did not execute on FiberScheduler");
    std::cout << "  [PASS] Auto-lowering, FiberDebugInfo PC->line mapping (2-frame callStack), and DAP virtual threads verified.\n";
}

void testZeroAllocationFrameTelemetryAndDapStreaming() {
    std::cout << "[2/3] Testing zero-allocation FrameTelemetryRing (128-frame wrap, Top-8 hotspots, GC/Heap/Coro/Fiber) & DAP streaming...\n";

    const char* vmSrc = R"RS(
module P2D.TelemetryBench;
import RealScript.Async;

public class BoxedNode {
    public int id;
    public int weight;
    public BoxedNode(int i, int w) { id = i; weight = w; }
}

int WorkFn1(int n) { int s = 0; for (int i = 0; i < n; i = i + 1) { s = s + i; } return s; }
int WorkFn2(int n) { int s = 0; for (int i = 0; i < n; i = i + 1) { s = s + i * 2; } return s; }
int WorkFn3(int n) { int s = 0; for (int i = 0; i < n; i = i + 1) { s = s + i * 3; } return s; }
int WorkFn4(int n) { int s = 0; for (int i = 0; i < n; i = i + 1) { s = s + i + 1; } return s; }
int WorkFn5(int n) { int s = 0; for (int i = 0; i < n; i = i + 1) { s = s + i + 2; } return s; }
int WorkFn6(int n) { int s = 0; for (int i = 0; i < n; i = i + 1) { s = s + i + 3; } return s; }
int WorkFn7(int n) { int s = 0; for (int i = 0; i < n; i = i + 1) { s = s + i + 4; } return s; }
int WorkFn8(int n) { int s = 0; for (int i = 0; i < n; i = i + 1) { s = s + i + 5; } return s; }
int WorkFn9Cold(int n) { return n + 1; }

public async Awaitable BackgroundCoro() {
    await Awaitable.Yield();
    await Awaitable.Yield();
}

public int RunFrameWorkload() {
    int[] compactArr = new int[256];
    compactArr[0] = WorkFn1(100);
    compactArr[1] = WorkFn2(70);
    compactArr[2] = WorkFn3(60);
    compactArr[3] = WorkFn4(50);
    compactArr[4] = WorkFn5(40);
    compactArr[5] = WorkFn6(30);
    compactArr[6] = WorkFn7(20);
    compactArr[7] = WorkFn8(12);
    int cold = WorkFn9Cold(1);

    BoxedNode n1 = new BoxedNode(1, compactArr[0]);
    BoxedNode n2 = new BoxedNode(2, compactArr[1]);
    return n1.weight + n2.weight + cold;
}
)RS";

    compiler::Compilation comp({{"telemetry_bench.rscript", vmSrc}});
    auto build = comp.build();
    require(!build.diagnostics.hasErrors(), "telemetry_bench.rscript compile failed");

    optimization::Options optOpts;
    optOpts.level = optimization::Level::None; // Keep all 9 helper functions un-inlined to test Top-8 ranking!
    auto opt = optimization::Optimizer{}.optimize(std::move(build.modules), build.diagnostics, optOpts);
    bytecode::Lowerer lowerer;
    std::vector<bytecode::Module> bc;
    for (const auto& m : opt.modules) bc.push_back(lowerer.lower(m));
    runtime::RuntimeError linkErr;
    auto linked = runtime::ProgramImage::link(std::move(bc), linkErr);
    require(linked.has_value(), "link failed");
    auto program = std::make_shared<const runtime::ProgramImage>(std::move(*linked));

    runtime::HeapConfig hcfg;
    hcfg.initialCollectionThresholdBytes = 2048; // Trigger incremental GC slices during frame!
    auto heap = std::make_shared<runtime::ManagedHeap>(hcfg);
    auto asyncCtx = std::make_shared<runtime::AsyncRuntimeContext>(heap.get());
    runtime::Interpreter interp(program, heap);
    interp.setAsyncRuntime(asyncCtx);

    // Spawn 1 suspended coroutine so activeCoroutines == 1
    const auto coroLaunch = interp.invoke("P2D.TelemetryBench::BackgroundCoro");
    require(coroLaunch.succeeded, "BackgroundCoro launch failed");
    require(asyncCtx->coroutines().statistics().activeCount == 1, "Expected 1 active suspended coroutine");

    // Spawn 2 active fibers on FiberScheduler
    runtime::FiberScheduler fiberSched;
    std::vector<runtime::CompactInstruction> loopCode = {
        runtime::CompactInstruction(runtime::FiberOpcode::YieldTicks, 0, 0, 2),
        runtime::CompactInstruction(runtime::FiberOpcode::Jump, 0, 0, 0),
    };
    (void)fiberSched.spawnFiber(loopCode, 0, runtime::GenerationalEntityHandle{1, 1});
    (void)fiberSched.spawnFiber(loopCode, 0, runtime::GenerationalEntityHandle{2, 1});

    runtime::FrameTelemetryRing telemetry;

    // 1. Prove ZERO heap allocations across 200 full ring cycles (beginFrame + record* + endFrame + copyRecentSamples)!
    gObservedHeapAllocs.store(0, std::memory_order_relaxed);
    gTrackHeapAllocs.store(true, std::memory_order_seq_cst);
    for (std::uint64_t f = 0; f < 200; ++f) {
        telemetry.beginFrame(f);
        for (std::uint64_t s = 1; s <= 12; ++s) {
            telemetry.recordFunctionInstructions(1000 + s, s * 100 + f);
        }
        telemetry.recordGcSliceNanoseconds(1500 + f);
        telemetry.setHeapLiveBytes(4096, 8192);
        telemetry.recordFiberStep(16, 2);
        (void)telemetry.endFrame(nullptr, nullptr, nullptr);
    }
    std::array<runtime::FrameTelemetrySample, 128> ringOut{};
    const std::size_t copiedCount = telemetry.copyRecentSamples(ringOut.data(), ringOut.size());
    runtime::FrameTelemetrySample latestSnap{};
    const bool hasLatest = telemetry.latestSample(latestSnap);
    gTrackHeapAllocs.store(false, std::memory_order_seq_cst);
    const std::size_t allocsDuringRingOps = gObservedHeapAllocs.load(std::memory_order_relaxed);

    require(allocsDuringRingOps == 0,
        "FrameTelemetryCollector MUST allocate 0 bytes on hot path, observed " + std::to_string(allocsDuringRingOps));
    require(copiedCount == 128 && hasLatest, "128-frame ring wrap-around should return 128 recent frames");
    require(ringOut.front().frameNumber == 72 && ringOut.back().frameNumber == 199 && latestSnap.frameNumber == 199,
        "Ring wrap-around frame numbers mismatch");

    // 2. Now run real Interpreter workload with ActiveFrameTelemetryScope and verify live integration!
    telemetry.clear();
    {
        runtime::ActiveFrameTelemetryScope scope(&telemetry);
        telemetry.beginFrame(500);
        // Allocate enough garbage beforehand so incremental GC steps fire inside Interpreter::invoke
        for (int i = 0; i < 32; ++i) {
            (void)heap->allocateString(std::string(128, 'x'));
        }
        heap->requestCollection();

        runtime::ExecutionOptions opts;
        opts.limits.gcWorkBudget = 4;
        const auto frameRes = interp.invoke("P2D.TelemetryBench::RunFrameWorkload", {}, opts);
        require(frameRes.succeeded, "RunFrameWorkload failed: " + frameRes.error.message);

        // Retain one packed int[] so compactHeapLiveBytes > 0 at endFrame
        auto retainedPacked = heap->allocateTypedArray(
            0, semantic::PrimitiveType::Int, 0, 512, runtime::Value{std::int64_t{7}});
        auto rootPacked = heap->retain(*retainedPacked);
        telemetry.noteCompactHeapObject(*retainedPacked);
        auto retainedBoxed = heap->allocateRecord(8);
        auto rootBoxed = heap->retain(*retainedBoxed);
        (void)rootPacked;
        (void)rootBoxed;

        const std::uint64_t fibInst = fiberSched.stepTick(1);
        telemetry.recordFiberStep(fibInst, static_cast<std::uint32_t>(fiberSched.activeFiberCount()));

        const auto sample = telemetry.endFrame(heap.get(), asyncCtx.get(), &fiberSched);
        require(sample.frameNumber == 500, "frameNumber mismatch");
        require(sample.totalInstructions > 500, "totalInstructions too low");
        require(sample.hotFunctionCount == 8,
            "Expected Top-8 hotspot functions (out of 10 functions), got " + std::to_string(sample.hotFunctionCount));
        for (std::uint8_t i = 1; i < sample.hotFunctionCount; ++i) {
            require(sample.topFunctions[i - 1].instructions >= sample.topFunctions[i].instructions,
                "Top-8 hot functions must be sorted descending by instructions");
        }
        const auto wf1Sym = program->findFunction("P2D.TelemetryBench::WorkFn1");
        require(wf1Sym.has_value() && sample.topFunctions[0].symbolId == *wf1Sym,
            "WorkFn1(100) must be #1 hotspot function");
        require(sample.compactHeapLiveBytes >= 2048,
            "compactHeapLiveBytes should reflect live packed int[512] (>=2048B), got " +
            std::to_string(sample.compactHeapLiveBytes));
        require(sample.boxedHeapLiveBytes > 0, "boxedHeapLiveBytes should be > 0");
        require(sample.gcSliceNanoseconds > 0, "gcSliceNanoseconds should be > 0 when incremental GC stepped");
        require(sample.activeCoroutines == 1, "activeCoroutines should be 1");
        require(sample.activeFibers == 2, "activeFibers should be 2");
    }

    // 3. Verify DapServer `:telemetry` console command & `emitFrameTelemetry` streaming event!
    debug::DapServer dap(program, heap);
    dap.bindFrameTelemetry(&telemetry);

    const auto evalTelText = dap.handle(tooling::Json::Object{
        {"seq", 10}, {"type", "request"}, {"command", "evaluate"},
        {"arguments", tooling::Json::Object{{"expression", ":telemetry"}}},
    });
    require(evalTelText.find("success")->boolValue(), ":telemetry evaluate failed");
    const std::string summaryStr = evalTelText.find("body")->find("result")->stringValue();
    require(summaryStr.find("[Frame #500]") != std::string::npos &&
            summaryStr.find("P2D.TelemetryBench::WorkFn1") != std::string::npos,
        ":telemetry summary missing Frame #500 or WorkFn1 symbol name:\n" + summaryStr);

    const auto evalTelJson = dap.handle(tooling::Json::Object{
        {"seq", 11}, {"type", "request"}, {"command", "evaluate"},
        {"arguments", tooling::Json::Object{{"expression", ":telemetry json"}}},
    });
    require(evalTelJson.find("success")->boolValue(), ":telemetry json evaluate failed");
    std::string parseErr;
    const auto parsedConsoleJson = tooling::Json::parse(
        evalTelJson.find("body")->find("result")->stringValue(), parseErr);
    require(parsedConsoleJson.has_value() && parseErr.empty(), ":telemetry json did not produce valid JSON");

    std::stringstream eventStream;
    dap.emitFrameTelemetry(eventStream);
    std::string rawEventBody;
    require(tooling::readProtocolMessage(eventStream, rawEventBody),
        "Failed to read protocol message emitted by emitFrameTelemetry");
    const auto eventJson = tooling::Json::parse(rawEventBody, parseErr);
    require(eventJson.has_value() &&
            eventJson->find("type")->stringValue() == "event" &&
            eventJson->find("event")->stringValue() == "realscript/frameTelemetry",
        "emitFrameTelemetry must emit event 'realscript/frameTelemetry'");
    const auto* evBody = eventJson->find("body");
    require(evBody &&
            evBody->find("frameNumber")->integerValue() == 500 &&
            evBody->find("activeCoroutines")->integerValue() == 1 &&
            evBody->find("activeFibers")->integerValue() == 2 &&
            evBody->find("topFunctions")->arrayValue().size() == 8,
        "realscript/frameTelemetry body payload mismatch");

    std::cout << "  [PASS] Zero-allocation FrameTelemetryRing (0 heap allocs across 200 frames) & DAP streaming verified.\n";
}

} // namespace

int main() {
    try {
        testFiberAutoLoweringDebugInfoAndDapIntrospection();
        testZeroAllocationFrameTelemetryAndDapStreaming();
        std::cout << "[3/3] ALL P2-D VERIFICATION TESTS PASSED!\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[FAIL] " << ex.what() << "\n";
        return 1;
    }
}
