#include "realscript/aot_cpp/AotCpp.h"
#include "realscript/aot_cpp/AotRuntime.h"
#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/game/Gameplay.h"
#include "realscript/optimization/Optimizer.h"
#include "realscript/runtime/AsyncRuntime.h"
#include "realscript/runtime/Runtime.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace phase28_ns_std {
const realscript::aot::ProgramDescriptor& Phase28StdProgram() noexcept;
} // namespace phase28_ns_std

namespace phase28_ns_trusted {
const realscript::aot::ProgramDescriptor& Phase28TrustedProgram() noexcept;
} // namespace phase28_ns_trusted

namespace {

using namespace realscript;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string diagnosticsText(const diagnostics::DiagnosticBag& bag) {
    std::string out;
    for (const auto& d : bag.items()) {
        out += d.code + ": " + d.message + "\n";
    }
    return out;
}

std::string extractStringValue(
    const runtime::Value& val,
    const runtime::ManagedHeap* heap) {
    if (const auto* s = std::get_if<std::string>(&val)) {
        return *s;
    }
    if (const auto* ref = std::get_if<runtime::ObjectRef>(&val)) {
        if (heap) {
            if (const auto sv = heap->stringView(*ref)) {
                return std::string(*sv);
            }
        }
    }
    return {};
}

std::string readFixtureText() {
    const std::string path =
        std::string(REALSCRIPT_SOURCE_DIR) + "/tests/fixtures/phase28_trusted_async_aot.rscript";
    std::ifstream in(path, std::ios::binary);
    require(in.good(), "failed to open fixture: " + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::vector<bytecode::Module> compilePhase28BytecodeModules(
    compiler::BuildResult* optionalOutBuild = nullptr,
    optimization::Result* optionalOutOpt = nullptr) {
    compiler::Compilation compilation({
        {"tests/fixtures/phase28_trusted_async_aot.rscript", readFixtureText()},
    });
    auto build = compilation.build();
    require(!build.diagnostics.hasErrors(),
        "Phase 28 fixture compile failed:\n" + diagnosticsText(build.diagnostics));
    optimization::Options optOptions;
    optOptions.level = optimization::Level::Aggressive;
    auto optimized = optimization::Optimizer{}.optimize(
        build.modules, build.diagnostics, optOptions);
    require(!build.diagnostics.hasErrors(),
        "Phase 28 fixture optimization failed:\n" + diagnosticsText(build.diagnostics));
    bytecode::Lowerer lowerer;
    std::vector<bytecode::Module> modules;
    for (const auto& m : optimized.modules) {
        modules.push_back(lowerer.lower(m));
    }
    if (optionalOutBuild) *optionalOutBuild = std::move(build);
    if (optionalOutOpt) *optionalOutOpt = std::move(optimized);
    return modules;
}

// ============================================================================
// 1. Timeout-Linked CancellationTokenPool & Deterministic Virtual Clock
// ============================================================================
void testLinkedTimeoutCancellationPool() {
    runtime::ManagedHeap heap;
    runtime::AsyncRuntimeContext ctx(&heap);
    auto& ctsPool = ctx.cancellations();

    // 1a. Multi-parent CreateLinked2
    const auto pA = ctsPool.createSource();
    const auto pB = ctsPool.createSource();
    const std::array<runtime::CancellationToken, 2> parents{{
        ctsPool.token(pA),
        ctsPool.token(pB),
    }};
    const auto child2 = ctsPool.createLinkedSource(parents.data(), parents.size());
    const auto child2Tok = ctsPool.token(child2);
    require(!ctsPool.isCancellationRequested(child2Tok), "child2 prematurely canceled");
    (void)ctsPool.cancel(pB);
    require(ctsPool.isCancellationRequested(child2Tok), "child2 did not cancel from parent B");
    require(!ctsPool.isTimedOut(child2Tok), "manual parent cancel must not set timedOut");
    require(ctsPool.releaseSource(child2), "release child2");
    require(ctsPool.releaseSource(pB), "release pB");
    require(ctsPool.releaseSource(pA), "release pA");

    // 1b. Standalone timeout source + cascading linked child inherits timeout bit
    const auto timeoutSrc = ctx.createTimeoutSource(0.25, false);
    const auto timeoutTok = ctsPool.token(timeoutSrc);
    const auto grandchildSrc = ctsPool.createLinkedSource(&timeoutTok, 1u);
    const auto grandchildTok = ctsPool.token(grandchildSrc);

    const auto pendingTask = ctx.awaitables().allocatePending(
        runtime::ResumeQueue::Update, grandchildTok);
    require(!ctx.awaitables().isCompleted(pendingTask), "pendingTask prematurely completed");

    require(ctx.advanceTimeouts(0.10) == 0, "timeout fired too early at 0.10s");
    require(!ctsPool.isCancellationRequested(timeoutTok), "canceled at 0.10s");
    require(ctx.advanceTimeouts(0.16) == 1, "expected timeout to fire at 0.26s");
    require(ctsPool.isCancellationRequested(timeoutTok), "timeoutTok not canceled");
    require(ctsPool.isTimedOut(timeoutTok), "timeoutTok missing timedOut flag");
    require(ctsPool.isCancellationRequested(grandchildTok), "grandchildTok not canceled");
    require(ctsPool.isTimedOut(grandchildTok), "grandchildTok did not inherit timedOut flag");
    require(ctx.awaitables().status(pendingTask) == runtime::AwaitStatus::Canceled,
        "pendingTask was not auto-canceled by linked timeout");

    runtime::Value unused{};
    runtime::RuntimeError err{};
    (void)ctx.awaitables().getResult(pendingTask, unused, err);
    require(ctsPool.releaseSource(grandchildSrc), "release grandchildSrc");
    require(ctsPool.releaseSource(timeoutSrc), "release timeoutSrc");

    // 1c. Parent cancellation BEFORE deadline keeps isTimedOut == false and prunes timer cleanly
    const auto entitySrc = ctsPool.createSource();
    const auto entityTok = ctsPool.token(entitySrc);
    const auto linkedTimeout = ctx.createLinkedTimeoutSource(&entityTok, 1u, 1.0, false);
    const auto linkedTok = ctsPool.token(linkedTimeout);
    (void)ctsPool.cancel(entitySrc);
    require(ctsPool.isCancellationRequested(linkedTok), "linkedTok should be canceled by entitySrc");
    require(!ctsPool.isTimedOut(linkedTok), "linkedTok should NOT report timedOut on parent cancel");
    require(ctx.advanceTimeouts(2.0) == 0, "already-canceled timeout source should not fire again");
    require(!ctsPool.isTimedOut(linkedTok), "post-deadline advance must not flip timedOut retroactively");

    require(ctsPool.releaseSource(linkedTimeout), "release linkedTimeout");
    require(ctsPool.releaseSource(entitySrc), "release entitySrc");
    require(ctsPool.statistics().activeSources == 0,
        "CancellationTokenPool leaked active sources");
}

// ============================================================================
// 2. HTTP Awaitable Bridge (200/201, 503 Fault, Linked Timeout, Entity Destroy,
//    Background Worker Thread + Determinism Record/Replay)
// ============================================================================
void testHttpAwaitableBridgeAndTimeoutCancellation() {
    const std::string scriptSource = R"RS(
module Phase28.HttpScene;
import RealScript.Async;
import RealScript.Scene;

public class HttpAudit {
    public static bool destroyedCanceled = false;
    public static bool destroyedWasTimeout = true;
}

public class HttpAgent {
    public long selfEntity;
    public string profileBody;
    public int postStatus;
    public string postEchoBody;
    public bool caught503;
    public int statusAfter503;
    public bool caughtTimeout;
    public bool timedOutFlag;
    public int statusAfterTimeout;
    public bool finished;

    public async Awaitable OnStart() {
        CancellationToken destroyTok = EntityDestroyToken(selfEntity);

        profileBody = await HttpGetAsync(
            "https://api.realscript.test/v1/profile",
            1.0,
            destroyTok);

        string postResp = await HttpPostAsync(
            "https://api.realscript.test/v1/score",
            "score=9800",
            1.0,
            destroyTok);
        postStatus = GetLastHttpStatus();
        postEchoBody = postResp;

        try {
            await HttpGetAsync(
                "https://api.realscript.test/v1/maintenance",
                1.0,
                destroyTok);
        } catch (AsyncException err) {
            caught503 = true;
            statusAfter503 = GetLastHttpStatus();
        }

        try {
            await HttpGetAsync(
                "https://api.realscript.test/v1/blackhole",
                0.20,
                destroyTok);
        } catch (AsyncCanceledException ex) {
            caughtTimeout = true;
            timedOutFlag = GetLastHttpTimedOut();
            statusAfterTimeout = GetLastHttpStatus();
        }

        finished = true;
    }
}

public class DestroyMidHttpAgent {
    public long selfEntity;

    public async Awaitable OnStart() {
        CancellationToken destroyTok = EntityDestroyToken(selfEntity);
        try {
            await HttpGetAsync(
                "https://api.realscript.test/v1/slow-asset",
                5.0,
                destroyTok);
        } catch (AsyncCanceledException ex) {
            HttpAudit.destroyedCanceled = true;
            HttpAudit.destroyedWasTimeout = GetLastHttpTimedOut();
        }
    }
}
)RS";

    game::GameApi api;
    game::ScriptRuntime runtime(nullptr, api.bindings(), api.heap(), api.nativeHandles());
    game::SceneScriptRuntime scene(runtime);
    require(game::installSceneComponentBindings(api, scene), "installSceneComponentBindings failed");

    game::GameScriptCompiler compiler(api);
    const auto compiled = compiler.compile({{"phase28_http.rscript", scriptSource}});
    require(compiled.succeeded(), "HTTP script compile failed:\n" + diagnosticsText(compiled.diagnostics));
    runtime.replaceProgram(compiled.program);

    scene.registerHttpEndpoint(
        "GET", "https://api.realscript.test/v1/profile", 200, "{\"user\":\"Nova\",\"tier\":4}", 1);
    scene.registerHttpEndpoint(
        "POST", "https://api.realscript.test/v1/score", 201, "{\"accepted\":true}", 1);
    scene.registerHttpEndpoint(
        "GET", "https://api.realscript.test/v1/maintenance", 503, "Service Unavailable", 1);
    scene.setHttpWorker([](
        const game::SceneScriptRuntime::HttpRequestSpec& /*req*/,
        runtime::AsyncRequestId /*reqId*/,
        runtime::CancellationToken /*tok*/,
        runtime::AsyncCompletionQueue& /*queue*/) {
        // Deliberately leave unresolved URLs pending to test timeout & entity-destroy cancellation
    });

    const auto agentEntity = scene.spawnGenerationalEntity("HttpAgentNode");
    game::SceneScriptOptions opts;
    opts.initialMembers["selfEntity"] = runtime::LongValue{static_cast<std::int64_t>(agentEntity)};
    require(scene.attachComponent(agentEntity, "Phase28.HttpScene::HttpAgent", opts), "attach HttpAgent");

    const auto ephemeralEntity = scene.spawnGenerationalEntity("EphemeralHttpNode");
    game::SceneScriptOptions ephOpts;
    ephOpts.initialMembers["selfEntity"] = runtime::LongValue{static_cast<std::int64_t>(ephemeralEntity)};
    require(scene.attachComponent(ephemeralEntity, "Phase28.HttpScene::DestroyMidHttpAgent", ephOpts),
        "attach DestroyMidHttpAgent");

    scene.start();
    scene.update(0.05);
    // Destroy ephemeralEntity mid-flight while its 5.0s HTTP request is pending!
    scene.queueDestroyEntity(ephemeralEntity);

    for (int step = 0; step < 16; ++step) {
        scene.update(0.06);
    }
    require(scene.errors().empty(), "unexpected scene errors during HTTP pipeline");

    const auto* agentObj = scene.getComponent(agentEntity, "Phase28.HttpScene::HttpAgent");
    require(agentObj != nullptr, "missing HttpAgent component");
    runtime::RuntimeError err;
    require(std::get<bool>(*runtime.getMember(*agentObj, "finished", err)), "HttpAgent did not finish");
    require(extractStringValue(*runtime.getMember(*agentObj, "profileBody", err), runtime.heap().get()) ==
            "{\"user\":\"Nova\",\"tier\":4}",
        "HTTP GET profileBody mismatch");
    require(std::get<std::int64_t>(*runtime.getMember(*agentObj, "postStatus", err)) == 201,
        "HTTP POST status expected 201");
    require(extractStringValue(*runtime.getMember(*agentObj, "postEchoBody", err), runtime.heap().get()) ==
            "{\"accepted\":true}",
        "HTTP POST body mismatch");
    require(std::get<bool>(*runtime.getMember(*agentObj, "caught503", err)),
        "expected HTTP 503 to throw GameError");
    require(std::get<std::int64_t>(*runtime.getMember(*agentObj, "statusAfter503", err)) == 503,
        "expected GetLastHttpStatus() == 503");
    require(std::get<bool>(*runtime.getMember(*agentObj, "caughtTimeout", err)),
        "expected 0.20s HTTP timeout to throw AsyncCanceledException");
    require(std::get<bool>(*runtime.getMember(*agentObj, "timedOutFlag", err)),
        "expected GetLastHttpTimedOut() == true after HTTP timeout");
    require(std::get<std::int64_t>(*runtime.getMember(*agentObj, "statusAfterTimeout", err)) == 408,
        "expected GetLastHttpStatus() == 408 on timeout");

    require(std::get<bool>(*runtime.getStaticMember("Phase28.HttpScene::HttpAudit", "destroyedCanceled", err)),
        "DestroyMidHttpAgent did not catch cancellation upon entity destroy");
    require(!std::get<bool>(*runtime.getStaticMember("Phase28.HttpScene::HttpAudit", "destroyedWasTimeout", err)),
        "entity destroy cancellation must NOT report GetLastHttpTimedOut() == true");

    // Background thread HTTP worker + DeterminismSession Record -> Replay
    std::atomic<int> workerCalls{0};
    auto replayLog = std::make_shared<runtime::ReplayLog>();
    auto runHttpDetPass = [&](bool recordMode) -> std::string {
        game::GameApi detApi;
        game::ScriptRuntime detRt(nullptr, detApi.bindings(), detApi.heap(), detApi.nativeHandles());
        game::SceneScriptRuntime detScene(detRt);
        require(game::installSceneComponentBindings(detApi, detScene), "det install bindings");
        game::GameScriptCompiler detComp(detApi);
        const std::string detSrc = R"RS(
module Phase28.HttpDet;
import RealScript.Async;
import RealScript.Scene;

public class DetClient {
    public long selfEntity;
    public string resultPayload;
    public int statusCode;

    public async Awaitable OnStart() {
        CancellationToken tok = EntityDestroyToken(selfEntity);
        resultPayload = await HttpPostAsync("https://live.worker/auth", "token=xyz", 2.0, tok);
        statusCode = GetLastHttpStatus();
    }
}
)RS";
        const auto detBuilt = detComp.compile({{"det_http.rscript", detSrc}});
        require(detBuilt.succeeded(), "det_http compile failed");
        detRt.replaceProgram(detBuilt.program);

        runtime::DeterminismOptions detOpts;
        detOpts.mode = recordMode
            ? runtime::DeterminismMode::Record
            : runtime::DeterminismMode::Replay;
        detOpts.replayLog = replayLog;
        runtime::DeterminismSession session(detOpts);
        detScene.setDeterminismSession(&session);

        if (recordMode) {
            detScene.setHttpWorker([&workerCalls](
                const game::SceneScriptRuntime::HttpRequestSpec& req,
                runtime::AsyncRequestId reqId,
                runtime::CancellationToken linkedTok,
                runtime::AsyncCompletionQueue& queue) {
                ++workerCalls;
                std::thread([req, reqId, linkedTok, &queue]() {
                    (void)linkedTok;
                    queue.submitString(
                        reqId,
                        game::SceneScriptRuntime::encodeHttpWirePacket(
                            202, "bg-ack:" + req.body));
                }).join();
            });
        }

        const auto ent = detScene.spawnGenerationalEntity("DetNode");
        game::SceneScriptOptions cOpts;
        cOpts.initialMembers["selfEntity"] = runtime::LongValue{static_cast<std::int64_t>(ent)};
        require(detScene.attachComponent(ent, "Phase28.HttpDet::DetClient", cOpts), "attach DetClient");
        detScene.start();
        detScene.update(0.016);
        detScene.update(0.016);
        require(detScene.errors().empty(), "detScene errors");

        const auto* obj = detScene.getComponent(ent, "Phase28.HttpDet::DetClient");
        runtime::RuntimeError mErr;
        require(std::get<std::int64_t>(*detRt.getMember(*obj, "statusCode", mErr)) == 202,
            "expected status 202 in HTTP determinism pass");
        return extractStringValue(*detRt.getMember(*obj, "resultPayload", mErr), detRt.heap().get());
    };

    const std::string recOut = runHttpDetPass(true);
    require(recOut == "bg-ack:token=xyz", "recorded HTTP worker payload mismatch: " + recOut);
    require(workerCalls.load() == 1, "expected 1 real worker call during Record");

    const std::string repOut = runHttpDetPass(false);
    require(repOut == recOut, "replayed HTTP payload diverged from recorded run");
    require(workerCalls.load() == 1, "Replay must NOT invoke live background HTTP worker");
}

// ============================================================================
// 3. WebSocket Full-Duplex Bridge (Async Push, Non-Destructive Receive Timeout,
//    Entity-Destroy Auto-Close & Determinism Record/Replay)
// ============================================================================
void testWebSocketFullDuplexTimeoutAndEntityDestroy() {
    const std::string scriptSource = R"RS(
module Phase28.WsScene;
import RealScript.Async;
import RealScript.Scene;

public class WsAudit {
    public static long peerHandle = 0L;
    public static bool peerRecvCanceledOnDestroy = false;
}

public class WsSessionAgent {
    public long selfEntity;
    public long wsHandle;
    public string firstMsg;
    public bool receiveTimedOut;
    public bool stillConnectedAfterTimeout;
    public string secondMsgAfterTimeout;
    public bool cleanlyClosed;

    public async Awaitable OnStart() {
        CancellationToken destroyTok = EntityDestroyToken(selfEntity);
        wsHandle = await WsConnectAsync(
            "wss://realtime.realscript.test/match",
            selfEntity,
            1.0,
            destroyTok);

        bool sent = await WsSendAsync(wsHandle, "hello-server", 1.0, destroyTok);
        if (!sent) {
            return;
        }

        firstMsg = await WsReceiveAsync(wsHandle, 1.0, destroyTok);

        // Next receive has a short 0.15s per-operation timeout while no frame arrives yet:
        try {
            await WsReceiveAsync(wsHandle, 0.15, destroyTok);
        } catch (AsyncCanceledException ex) {
            receiveTimedOut = true;
            stillConnectedAfterTimeout = WsIsConnected(wsHandle);
        }

        // Now await the next inbound frame pushed after the timeout -- session & queue must be intact!
        secondMsgAfterTimeout = await WsReceiveAsync(wsHandle, 1.0, destroyTok);

        bool closedOk = await WsCloseAsync(wsHandle);
        cleanlyClosed = closedOk && !WsIsConnected(wsHandle);
    }
}

public class WsEphemeralPeer {
    public long selfEntity;

    public async Awaitable OnStart() {
        CancellationToken destroyTok = EntityDestroyToken(selfEntity);
        WsAudit.peerHandle = await WsConnectAsync(
            "wss://realtime.realscript.test/match",
            selfEntity,
            1.0,
            destroyTok);
        try {
            await WsReceiveAsync(WsAudit.peerHandle, 10.0, destroyTok);
        } catch (AsyncCanceledException ex) {
            WsAudit.peerRecvCanceledOnDestroy = true;
        }
    }
}
)RS";

    game::GameApi api;
    game::ScriptRuntime runtime(nullptr, api.bindings(), api.heap(), api.nativeHandles());
    game::SceneScriptRuntime scene(runtime);
    require(game::installSceneComponentBindings(api, scene), "installSceneComponentBindings failed");

    game::GameScriptCompiler compiler(api);
    const auto compiled = compiler.compile({{"phase28_ws.rscript", scriptSource}});
    require(compiled.succeeded(), "WS script compile failed:\n" + diagnosticsText(compiled.diagnostics));
    runtime.replaceProgram(compiled.program);

    scene.registerWebSocketEndpoint("wss://realtime.realscript.test/match", false, 0);

    const auto mainEnt = scene.spawnGenerationalEntity("WsMain");
    game::SceneScriptOptions mainOpts;
    mainOpts.initialMembers["selfEntity"] = runtime::LongValue{static_cast<std::int64_t>(mainEnt)};
    require(scene.attachComponent(mainEnt, "Phase28.WsScene::WsSessionAgent", mainOpts), "attach WsSessionAgent");

    const auto peerEnt = scene.spawnGenerationalEntity("WsPeer");
    game::SceneScriptOptions peerOpts;
    peerOpts.initialMembers["selfEntity"] = runtime::LongValue{static_cast<std::int64_t>(peerEnt)};
    require(scene.attachComponent(peerEnt, "Phase28.WsScene::WsEphemeralPeer", peerOpts), "attach WsEphemeralPeer");

    scene.start();
    const auto* mainObj = scene.getComponent(mainEnt, "Phase28.WsScene::WsSessionAgent");
    require(mainObj != nullptr, "missing WsSessionAgent component");

    // Tick 1: connects complete and WsSendAsync("hello-server") completes
    scene.update(0.02);
    runtime::RuntimeError err;
    const auto mainWsHandle = std::get<runtime::LongValue>(*runtime.getMember(*mainObj, "wsHandle", err)).value;
    const auto peerWsHandle = std::get<runtime::LongValue>(
        *runtime.getStaticMember("Phase28.WsScene::WsAudit", "peerHandle", err)).value;
    require(mainWsHandle > 0 && peerWsHandle > 0, "expected valid WebSocket handles");
    require(scene.isWebSocketConnected(mainWsHandle), "mainWsHandle should be connected");
    require(scene.isWebSocketConnected(peerWsHandle), "peerWsHandle should be connected");
    const auto sentFrames = scene.getWebSocketSentMessages(mainWsHandle);
    require(sentFrames.size() == 1 && sentFrames[0] == "hello-server", "sent WebSocket message mismatch");

    // Push first inbound frame from a background thread!
    std::thread([&scene, mainWsHandle]() {
        (void)scene.pushWebSocketInboundMessage(mainWsHandle, "welcome-frame-1");
    }).join();

    // Tick 2: drains firstMsg -> enters 0.15s WsReceiveAsync
    scene.update(0.02);
    // Destroy peerEnt directly via detach -> must auto-close peerWsHandle and cancel its pending WsReceiveAsync!
    require(scene.detach(peerEnt), "detach peerEnt");
    require(!scene.isWebSocketConnected(peerWsHandle),
        "destroying owner entity must auto-close its WebSocket session");

    // Tick 3 drains firstMsg and starts 0.15s WsReceiveAsync; Ticks 4-5 (+0.20s) fire timeout!
    scene.update(0.02);
    scene.update(0.10);
    scene.update(0.10);

    // Verify receive timed out WITHOUT closing mainWsHandle, then push second frame!
    require(std::get<bool>(*runtime.getMember(*mainObj, "receiveTimedOut", err)),
        "expected 0.15s WsReceiveAsync to time out");
    require(std::get<bool>(*runtime.getMember(*mainObj, "stillConnectedAfterTimeout", err)),
        "WsReceiveAsync timeout must keep WebSocket session open");

    require(scene.pushWebSocketInboundMessage(mainWsHandle, "delayed-frame-2"),
        "pushWebSocketInboundMessage after timeout failed");
    scene.update(0.02);
    scene.update(0.02);

    require(scene.errors().empty(), "unexpected scene errors in WebSocket test");
    require(extractStringValue(*runtime.getMember(*mainObj, "firstMsg", err), runtime.heap().get()) ==
            "welcome-frame-1",
        "firstMsg mismatch");
    require(extractStringValue(*runtime.getMember(*mainObj, "secondMsgAfterTimeout", err), runtime.heap().get()) ==
            "delayed-frame-2",
        "secondMsgAfterTimeout mismatch (frame dropped or waiter corrupted)");
    require(std::get<bool>(*runtime.getMember(*mainObj, "cleanlyClosed", err)),
        "WsCloseAsync did not cleanly close mainWsHandle");
    require(std::get<bool>(
                *runtime.getStaticMember("Phase28.WsScene::WsAudit", "peerRecvCanceledOnDestroy", err)),
        "peer WsReceiveAsync was not canceled when owner entity was destroyed");
}

// ============================================================================
// 4. C++17 AOT Scalar-Unboxed Coroutine Codegen Inspection & Mode Parity
// ============================================================================
void testTrustedReleaseScalarCoroutineCodegenAndParity() {
    compiler::BuildResult build;
    optimization::Result optimized;
    auto modules = compilePhase28BytecodeModules(&build, &optimized);

    // 4a. Inspect generated --trusted-release C++17 code for ScalarCoroPodState & fast paths
    aot::CppGenerator generator;
    aot::GenerationOptions genOpts;
    genOpts.programName = "Phase28Inspect";
    genOpts.trustedRelease = true;
    const auto generated = generator.generate(optimized.modules, build.diagnostics, genOpts);
    require(!build.diagnostics.hasErrors(), "AOT codegen inspection failed");
    require(generated.source.find("struct ScalarCoroPodState") != std::string::npos,
        "expected ScalarCoroPodState in generated scalar async C++17 output");
    require(generated.source.find("context.canSkipAwaitSuspendFast(") != std::string::npos,
        "expected inline canSkipAwaitSuspendFast check in generated C++17 output");
    require(generated.source.find("context.awaitSuspendScalar(") != std::string::npos,
        "expected awaitSuspendScalar call in generated C++17 output");
    require(generated.source.find("context.asyncIntrinsicNewPendingFast(") != std::string::npos,
        "expected zero-boxing asyncIntrinsicNewPendingFast in TrustedRelease codegen");
    require(generated.source.find("_typed<false, false, true>") != std::string::npos,
        "expected _typed<false, false, true> dispatch in TrustedRelease codegen");

    // 4b. Verify exact numerical parity across Interpreter, Standard AOT (instruction parity!),
    //     and Trusted-Release Scalar-Unboxed AOT (0 accounting instructions!)
    runtime::Interpreter interpreter(std::move(modules));
    aot::Program stdAot(phase28_ns_std::Phase28StdProgram());
    aot::Program trustedAot(phase28_ns_trusted::Phase28TrustedProgram());

    runtime::ExecutionOptions trustedOpts;
    trustedOpts.trustedRelease = true;
    trustedOpts.limits.gcWorkBudget = 0;

    auto verifyAllThree = [&](const std::string& fnName,
                              const std::vector<runtime::Value>& args,
                              bool expectLong) {
        const auto vmRes = interpreter.invoke(fnName, args);
        const auto stdRes = stdAot.invoke(fnName, args);
        const auto trRes = trustedAot.invoke(fnName, args, trustedOpts);
        require(vmRes.succeeded, fnName + " failed in VM: " + vmRes.error.message);
        require(stdRes.succeeded, fnName + " failed in Standard AOT: " + stdRes.error.message);
        if (!trRes.succeeded) {
            std::string st;
            for (const auto& f : trRes.error.symbolizedStackTrace) st += "\n  at " + f;
            require(false, fnName + " failed in Trusted AOT: " + trRes.error.message + st);
        }

        if (expectLong) {
            const auto vVm = std::get<runtime::LongValue>(vmRes.value).value;
            const auto vStd = std::get<runtime::LongValue>(stdRes.value).value;
            const auto vTr = std::get<runtime::LongValue>(trRes.value).value;
            require(vVm == vStd && vVm == vTr,
                fnName + " return mismatch: VM=" + std::to_string(vVm) +
                " StdAOT=" + std::to_string(vStd) + " TrustedAOT=" + std::to_string(vTr));
        } else {
            const auto vVm = std::get<std::int64_t>(vmRes.value);
            const auto vStd = std::get<std::int64_t>(stdRes.value);
            const auto vTr = std::get<std::int64_t>(trRes.value);
            require(vVm == vStd && vVm == vTr && vTr == 777,
                fnName + " int return mismatch");
        }

        // Standard AOT must match Interpreter instruction count bit-for-bit!
        require(vmRes.instructionsExecuted == stdRes.instructionsExecuted,
            fnName + " instruction parity mismatch: VM=" +
            std::to_string(vmRes.instructionsExecuted) + " vs StdAOT=" +
            std::to_string(stdRes.instructionsExecuted));

        // Trusted-Release AOT must eliminate 100% of instruction accounting!
        require(trRes.instructionsExecuted == 0,
            fnName + " TrustedRelease executed non-zero accounted instructions: " +
            std::to_string(trRes.instructionsExecuted));
    };

    verifyAllThree("Phase28.TrustedAsync::BenchSyncScalarAsyncNoAlloc", {std::int64_t{64}}, true);
    verifyAllThree("Phase28.TrustedAsync::BenchPooledSuspendResumeCoro", {std::int64_t{32}}, true);
    verifyAllThree("Phase28.TrustedAsync::BenchNestedAsyncChain", {std::int64_t{32}}, true);
    verifyAllThree("Phase28.TrustedAsync::VerifyTimeoutLinkedTokensInAot", {}, false);

    require(trustedAot.asyncRuntime()->coroutines().statistics().activeCount == 0,
        "Trusted AOT leaked active CoroutineFrames");
    require(trustedAot.asyncRuntime()->coroutines().statistics().activeGcRoots == 0,
        "Trusted AOT leaked coroutine GC roots");
    require(trustedAot.asyncRuntime()->awaitables().statistics().activeCount == 0,
        "Trusted AOT leaked active Awaitable slots");
}

// ============================================================================
// 5. Steady-State Microbenchmarks: Flat VM vs Standard AOT vs Trusted-Release Scalar AOT
// ============================================================================
void runStage4AsyncAotBenchmarks() {
    auto modules = compilePhase28BytecodeModules();
    runtime::Interpreter interpreter(std::move(modules));
    aot::Program stdAot(phase28_ns_std::Phase28StdProgram());
    aot::Program trustedAot(phase28_ns_trusted::Phase28TrustedProgram());

    runtime::ExecutionOptions vmFastOpts;
    vmFastOpts.limits.gcWorkBudget = 0;
    vmFastOpts.limits.instructionBudget = 100000000ULL;

    runtime::ExecutionOptions stdAotOpts; // default safe accounting
    stdAotOpts.limits.instructionBudget = 100000000ULL;

    runtime::ExecutionOptions trustedOpts;
    trustedOpts.trustedRelease = true;
    trustedOpts.limits.gcWorkBudget = 0;

#if defined(NDEBUG)
    constexpr std::int64_t kIterations = 25000;
#else
    constexpr std::int64_t kIterations = 1000;
#endif
    constexpr int kSamples = 3;

    struct BenchCase {
        const char* label;
        const char* fnName;
    };
    const BenchCase cases[] = {
        {"Sync Scalar Async/Await", "Phase28.TrustedAsync::BenchSyncScalarAsyncNoAlloc"},
        {"Pooled 2-Suspend 6-Local Coro", "Phase28.TrustedAsync::BenchPooledSuspendResumeCoro"},
        {"3-Level Nested Async Chain", "Phase28.TrustedAsync::BenchNestedAsyncChain"},
    };

    std::cout << "\n[Stage 4 Benchmark] " << kIterations << " iterations per sample (best of "
              << kSamples << "):\n";
    for (const auto& bc : cases) {
        const std::vector<runtime::Value> args{kIterations};

        // Warmup
        (void)interpreter.invoke(bc.fnName, args, vmFastOpts);
        (void)stdAot.invoke(bc.fnName, args, stdAotOpts);
        (void)trustedAot.invoke(bc.fnName, args, trustedOpts);

        auto measureBestNsPerIter = [&](auto&& runner) -> double {
            double bestNs = 1e18;
            for (int s = 0; s < kSamples; ++s) {
                const auto t0 = std::chrono::steady_clock::now();
                const auto res = runner();
                const auto t1 = std::chrono::steady_clock::now();
                require(res.succeeded, std::string(bc.label) + " benchmark failed: " + res.error.message);
                const double elapsedNs =
                    std::chrono::duration<double, std::nano>(t1 - t0).count();
                const double perIter = elapsedNs / static_cast<double>(kIterations);
                if (perIter < bestNs) bestNs = perIter;
            }
            return bestNs;
        };

        const double vmNs = measureBestNsPerIter([&]() {
            return interpreter.invoke(bc.fnName, args, vmFastOpts);
        });
        const double stdNs = measureBestNsPerIter([&]() {
            return stdAot.invoke(bc.fnName, args, stdAotOpts);
        });
        const double trNs = measureBestNsPerIter([&]() {
            return trustedAot.invoke(bc.fnName, args, trustedOpts);
        });

        std::cout << "  - " << std::left << std::setw(30) << bc.label
                  << " | Flat VM: " << std::fixed << std::setprecision(1) << std::setw(7) << vmNs << " ns/op"
                  << " | Std AOT: " << std::setw(6) << stdNs << " ns/op"
                  << " | Trusted Scalar AOT: " << std::setw(6) << trNs << " ns/op"
                  << " (" << std::setprecision(2) << (vmNs / trNs) << "x vs VM, "
                  << (stdNs / trNs) << "x vs Std AOT)\n";
    }
}

// ============================================================================
// 6. Stage 4 Review Regressions:
//    - Re-entrant manual cancel inside timeout callback + NaN/+Inf timeout guards
//    - Pre-canceled HTTP isolation, WS mock latency & worker failure sentMessages,
//      and idle short-timeout WsReceiveAsync waiter pruning
//    - SceneScriptRuntime destruction & clearForSnapshotImport network leak zeroing
//    - Standard/Trusted Scalar AOT GC rooting, Program outliving across 2nd suspend,
//      DivideByZero real ErrorCode + source line stack trace, and uncaught exception
// ============================================================================
void testStage4ReviewRegressions() {
    // 6a. Timeout cascade isolation on re-entrant manual cancel + NaN / +Inf guards
    {
        runtime::ManagedHeap heap;
        runtime::AsyncRuntimeContext ctx(&heap);
        auto& ctsPool = ctx.cancellations();

        struct ReentrantCtx {
            runtime::CancellationTokenPool* pool = nullptr;
            runtime::CancellationSourceHandle unrelatedParent{};
        };

        const auto unrelatedParent = ctsPool.createSource();
        const auto unrelatedParentTok = ctsPool.token(unrelatedParent);
        const auto unrelatedChild = ctsPool.createLinkedSource(&unrelatedParentTok, 1u);
        const auto unrelatedChildTok = ctsPool.token(unrelatedChild);

        const auto timeoutRoot = ctx.createTimeoutSource(0.10, false);
        const auto timeoutRootTok = ctsPool.token(timeoutRoot);
        const auto timeoutChild = ctsPool.createLinkedSource(&timeoutRootTok, 1u);
        const auto timeoutChildTok = ctsPool.token(timeoutChild);

        ReentrantCtx rctx{&ctsPool, unrelatedParent};
        const auto reg = ctsPool.registerCallback(
            timeoutRootTok,
            [](void* raw, std::uint64_t) {
                auto* c = static_cast<ReentrantCtx*>(raw);
                (void)c->pool->cancel(c->unrelatedParent);
            },
            &rctx,
            0);
        require(reg.valid(), "6a expected valid callback registration");

        require(ctx.advanceTimeouts(0.15) == 1, "6a expected timeoutRoot to fire");
        require(ctsPool.isCancellationRequested(timeoutChildTok) &&
                ctsPool.isTimedOut(timeoutChildTok),
            "6a timeoutChild must inherit timedOut == true");
        require(ctsPool.isCancellationRequested(unrelatedParentTok) &&
                !ctsPool.isTimedOut(unrelatedParentTok),
            "6a unrelatedParent canceled manually inside timeout callback must NOT be timedOut");
        require(ctsPool.isCancellationRequested(unrelatedChildTok) &&
                !ctsPool.isTimedOut(unrelatedChildTok),
            "6a unrelatedChild linked to unrelatedParent must NOT be mis-marked timedOut");

        require(ctsPool.releaseSource(timeoutChild), "6a release timeoutChild");
        require(ctsPool.releaseSource(timeoutRoot), "6a release timeoutRoot");
        require(ctsPool.releaseSource(unrelatedChild), "6a release unrelatedChild");
        require(ctsPool.releaseSource(unrelatedParent), "6a release unrelatedParent");

        // NaN timeout -> immediate timeout cancel without stuck timer
        const double nanVal = std::numeric_limits<double>::quiet_NaN();
        const double infVal = std::numeric_limits<double>::infinity();
        const auto nanSrc = ctx.createTimeoutSource(nanVal, false);
        const auto nanTok = ctsPool.token(nanSrc);
        require(ctsPool.isCancellationRequested(nanTok) && ctsPool.isTimedOut(nanTok),
            "6a NaN timeout must cancel immediately by timeout");
        require(ctx.advanceTimeouts(5.0) == 0, "6a NaN timeout must not leave scheduled timer");
        require(ctsPool.releaseSource(nanSrc), "6a release nanSrc");

        // +Inf timeout -> never expires; advanceTimeouts(+Inf / NaN) does not corrupt clock
        const double clockBefore = ctx.virtualTimeSeconds();
        const auto infSrc = ctx.createTimeoutSource(infVal, false);
        const auto infTok = ctsPool.token(infSrc);
        require(!ctsPool.isCancellationRequested(infTok), "6a +Inf timeout must start active");
        require(ctx.advanceTimeouts(infVal) == 0, "6a advanceTimeouts(+Inf) must be ignored");
        require(ctx.advanceTimeouts(nanVal) == 0, "6a advanceTimeouts(NaN) must be ignored");
        require(ctx.virtualTimeSeconds() == clockBefore,
            "6a non-finite delta must not corrupt virtualTimeSeconds");
        require(ctx.advanceTimeouts(1000.0) == 0 && !ctsPool.isCancellationRequested(infTok),
            "6a +Inf timeout source must not expire");
        require(ctsPool.releaseSource(infSrc), "6a release infSrc");
        require(ctsPool.statistics().activeSources == 0, "6a leaked CTS sources");
    }

    // 6b. Pre-canceled HTTP non-clobbering, WS mock latency & worker failure sentMessages,
    //     and repeated short-timeout WsReceiveAsync queue bound
    {
        game::GameApi api;
        game::ScriptRuntime runtime(nullptr, api.bindings(), api.heap(), api.nativeHandles());
        game::SceneScriptRuntime scene(runtime);
        auto& pool = scene.asyncRuntime()->awaitables();
        auto& ctsPool = scene.asyncRuntime()->cancellations();

        scene.registerHttpEndpoint("POST", "https://api.test/commit", 201, "ok-201", 0);
        game::SceneScriptRuntime::HttpRequestSpec okSpec{"POST", "https://api.test/commit", "x=1", 1.0};
        const auto okTask = scene.httpRequestAsync(okSpec, false);
        scene.update(0.016);
        runtime::Value okVal{};
        runtime::RuntimeError okErr{};
        require(pool.getResult(okTask, okVal, okErr) == runtime::AwaitStatus::Succeeded,
            "6b expected initial 201 HTTP request to succeed");
        require(scene.lastHttpStatusCode() == 201 && scene.lastHttpResponseBody() == "ok-201",
            "6b expected lastHttpStatusCode == 201");

        // Pre-canceled request MUST NOT synchronously clobber the completed 201 response on entry!
        const auto manualCancelSrc = ctsPool.createSource();
        (void)ctsPool.cancel(manualCancelSrc);
        game::SceneScriptRuntime::HttpRequestSpec preSpec{"GET", "https://api.test/never", {}, 1.0};
        const auto preTask = scene.httpRequestAsync(preSpec, false, ctsPool.token(manualCancelSrc));
        require(scene.lastHttpStatusCode() == 201 && scene.lastHttpResponseBody() == "ok-201",
            "6b pre-canceled httpRequestAsync clobbered previous completed HTTP status synchronously");

        // Once Update resume queue drains, preTask transitions to Canceled and updates lastHttp*
        (void)scene.stepAsyncResumeQueue(runtime::ResumeQueue::Update);
        require(pool.getResult(preTask, okVal, okErr) == runtime::AwaitStatus::Canceled,
            "6b preTask should complete as Canceled on Update");
        require(scene.lastHttpStatusCode() == 0 && !scene.lastHttpTimedOut(),
            "6b manual pre-canceled request should deliver status 0 and timedOut == false");
        (void)ctsPool.releaseSource(manualCancelSrc);

        // NaN timeout on httpRequestAsync -> delivers timedOut == true, status 408
        game::SceneScriptRuntime::HttpRequestSpec nanSpec{
            "GET", "https://api.test/nan", {}, std::numeric_limits<double>::quiet_NaN()};
        const auto nanTask = scene.httpRequestAsync(nanSpec, false);
        (void)scene.stepAsyncResumeQueue(runtime::ResumeQueue::Update);
        require(pool.getResult(nanTask, okVal, okErr) == runtime::AwaitStatus::Canceled,
            "6b NaN timeout HTTP request should cancel");
        require(scene.lastHttpTimedOut() && scene.lastHttpStatusCode() == 408,
            "6b NaN timeout HTTP request should report timedOut == true and status 408");

        // WebSocket mock latency + failed worker send must not record sentMessages prematurely
        scene.registerWebSocketEndpoint("wss://test.local/echo", true, 0);
        const auto connTask = scene.wsConnectAsync("wss://test.local/echo", 0, 1.0);
        scene.update(0.016);
        runtime::Value wsVal{};
        require(pool.getResult(connTask, wsVal, okErr) == runtime::AwaitStatus::Succeeded,
            "6b wsConnectAsync failed");
        const auto wsHandle = std::get<runtime::LongValue>(wsVal).value;

        scene.setWebSocketSendWorker([](
            game::SceneScriptRuntime::WebSocketHandle,
            const std::string&,
            runtime::AsyncRequestId reqId,
            runtime::CancellationToken,
            runtime::AsyncCompletionQueue& cq) {
            cq.submitFailure(reqId, runtime::ErrorCode::ScriptException, "send_rejected");
        });
        const auto failSendTask = scene.wsSendAsync(wsHandle, "must_not_record", 1.0);
        scene.update(0.016);
        require(pool.getResult(failSendTask, wsVal, okErr) == runtime::AwaitStatus::Faulted,
            "6b expected worker send failure");
        require(scene.getWebSocketSentMessages(wsHandle).empty(),
            "6b failed wsSendAsync worker must NOT append to sentMessages");

        scene.setWebSocketSendWorker(nullptr);
        scene.setAsyncIoLatencyFrames(3);
        const auto canceledLatencySend = scene.wsSendAsync(wsHandle, "canceled_in_flight", 0.05);
        require(scene.getWebSocketSentMessages(wsHandle).empty(),
            "6b deferred mock wsSendAsync must not record sentMessages before latency elapses");
        scene.update(0.10); // triggers 0.05s timeout before 3 latency frames!
        require(pool.getResult(canceledLatencySend, wsVal, okErr) == runtime::AwaitStatus::Canceled,
            "6b expected deferred mock wsSendAsync to time out");
        scene.update(0.016);
        scene.update(0.016);
        require(scene.getWebSocketSentMessages(wsHandle).empty(),
            "6b timed-out deferred wsSendAsync must NOT record sentMessages");

        const auto goodLatencySend = scene.wsSendAsync(wsHandle, "delayed_ok", 1.0);
        scene.update(0.016);
        scene.update(0.016);
        require(scene.getWebSocketSentMessages(wsHandle).empty(),
            "6b goodLatencySend still has 1 frame remaining");
        scene.update(0.016);
        require(pool.getResult(goodLatencySend, wsVal, okErr) == runtime::AwaitStatus::Succeeded,
            "6b goodLatencySend should succeed on frame 3");
        const auto sentAfter = scene.getWebSocketSentMessages(wsHandle);
        require(sentAfter.size() == 1 && sentAfter[0] == "delayed_ok",
            "6b expected delayed_ok in sentMessages");

        // Drain the autoEcho message from goodLatencySend
        scene.setAsyncIoLatencyFrames(0);
        const auto echoRecv = scene.wsReceiveAsync(wsHandle, 1.0);
        scene.update(0.016);
        require(pool.getResult(echoRecv, wsVal, okErr) == runtime::AwaitStatus::Succeeded &&
                extractStringValue(wsVal, runtime.heap().get()) == "echo:delayed_ok",
            "6b expected autoEcho payload from deferred mock send");

        // Repeated short-timeout wsReceiveAsync on idle socket must prune waiters & CTS cleanly
        for (int i = 0; i < 64; ++i) {
            const auto rTask = scene.wsReceiveAsync(wsHandle, 0.05);
            scene.update(0.10);
            require(pool.getResult(rTask, wsVal, okErr) == runtime::AwaitStatus::Canceled,
                "6b idle poll receive should time out");
        }
        require(ctsPool.statistics().activeSources == 0,
            "6b idle wsReceiveAsync timeouts leaked active CTS sources");
        require(scene.pushWebSocketInboundMessage(wsHandle, "fresh_after_64_timeouts"),
            "6b push after 64 timeouts failed");
        const auto freshRecv = scene.wsReceiveAsync(wsHandle, 1.0);
        scene.update(0.016);
        require(pool.getResult(freshRecv, wsVal, okErr) == runtime::AwaitStatus::Succeeded &&
                extractStringValue(wsVal, runtime.heap().get()) == "fresh_after_64_timeouts",
            "6b fresh inbound frame after 64 timeouts was lost");
        const auto closeTask = scene.wsCloseAsync(wsHandle);
        (void)pool.discard(closeTask);
        require(pool.statistics().activeCount == 0 && ctsPool.statistics().activeSources == 0,
            "6b leaked handles or CTS sources");
    }

    // 6c. SceneScriptRuntime destruction & clearForSnapshotImport while HTTP/WS ops are pending
    {
        game::GameApi api;
        game::ScriptRuntime runtime(nullptr, api.bindings(), api.heap(), api.nativeHandles());
        auto& pool = runtime.asyncRuntime()->awaitables();
        auto& ctsPool = runtime.asyncRuntime()->cancellations();

        const auto externalCallerCts = ctsPool.createSource();
        const auto externalCallerTok = ctsPool.token(externalCallerCts);

        runtime::AwaitableHandle orphanHttp{};
        runtime::AwaitableHandle orphanWsConn{};
        runtime::AwaitableHandle orphanWsSend{};
        runtime::AwaitableHandle orphanWsRecv{};
        {
            game::SceneScriptRuntime transientScene(runtime);
            transientScene.registerWebSocketEndpoint("wss://transient.local/live", false, 0);
            const auto readyConn = transientScene.wsConnectAsync("wss://transient.local/live", 0, 1.0);
            transientScene.update(0.016);
            runtime::Value connVal{};
            runtime::RuntimeError connErr{};
            require(pool.getResult(readyConn, connVal, connErr) == runtime::AwaitStatus::Succeeded,
                "6c initial wsConnectAsync failed");
            const auto liveWs = std::get<runtime::LongValue>(connVal).value;

            // Switch to 5-frame deferred latency & stalled workers so all 4 network ops stay pending!
            transientScene.setAsyncIoLatencyFrames(5);
            transientScene.registerHttpEndpoint("GET", "https://transient.local/slow", 200, "ok", 5);
            transientScene.registerWebSocketEndpoint("wss://transient.local/slow", false, 5);

            orphanHttp = transientScene.httpRequestAsync(
                {"GET", "https://transient.local/slow", {}, 5.0}, false, externalCallerTok);
            orphanWsConn = transientScene.wsConnectAsync(
                "wss://transient.local/slow", 0, 5.0, externalCallerTok);
            orphanWsSend = transientScene.wsSendAsync(
                liveWs, "inflight_msg", 5.0, externalCallerTok);
            orphanWsRecv = transientScene.wsReceiveAsync(
                liveWs, 5.0, externalCallerTok);
        }

        // transientScene is now destroyed! Verify zero UAF on timeout advance / external cancel / drain
        require(runtime.asyncRuntime()->advanceTimeouts(10.0) == 0,
            "6c destroyed SceneScriptRuntime left live timeout sources scheduled");
        (void)ctsPool.cancel(externalCallerCts);
        require(ctsPool.releaseSource(externalCallerCts), "6c release externalCallerCts");

        runtime::RuntimeError ioErr{};
        require(runtime.asyncRuntime()->stepPreTickIo(nullptr, ioErr),
            "6c stepPreTickIo after SceneScriptRuntime destruction failed");
        (void)runtime.asyncRuntime()->stepResumeQueue(runtime::ResumeQueue::Update);

        require(pool.status(orphanHttp) == runtime::AwaitStatus::Canceled &&
                pool.status(orphanWsConn) == runtime::AwaitStatus::Canceled &&
                pool.status(orphanWsSend) == runtime::AwaitStatus::Canceled &&
                pool.status(orphanWsRecv) == runtime::AwaitStatus::Canceled,
            "6c all orphan network handles must be Canceled on SceneScriptRuntime destruction");
        (void)pool.discard(orphanHttp);
        (void)pool.discard(orphanWsConn);
        (void)pool.discard(orphanWsSend);
        (void)pool.discard(orphanWsRecv);

        require(pool.statistics().activeCount == 0,
            "6c AwaitableCorePool leaked slots after SceneScriptRuntime destruction");
        require(ctsPool.statistics().activeSources == 0,
            "6c CancellationTokenPool leaked sources after SceneScriptRuntime destruction");

        // Also verify clearForSnapshotImport() cleans up pending network ops cleanly
        game::SceneScriptRuntime resetScene(runtime);
        resetScene.setAsyncIoLatencyFrames(5);
        resetScene.registerHttpEndpoint("GET", "https://reset.local/slow", 200, "ok", 5);
        (void)resetScene.httpRequestAsync({"GET", "https://reset.local/slow", {}, 5.0}, false);
        resetScene.clearForSnapshotImport();
        require(pool.statistics().activeCount == 0 && ctsPool.statistics().activeSources == 0,
            "6c clearForSnapshotImport leaked network handles or CTS sources");
    }

    // 6d. Scalar AOT GC rooting (Standard AOT + Trusted AOT), outliving Program across 2nd suspend,
    //     DivideByZero real ErrorCode + source line stack trace, and uncaught exception propagation
    {
        const auto intAwaitableTid = semantic::stableTypeId("RealScript.Async::Awaitable__int");

        auto testGcAndFaultsOnProgram = [&](bool trustedMode) {
            auto prog = std::make_unique<aot::Program>(
                trustedMode
                    ? phase28_ns_trusted::Phase28TrustedProgram()
                    : phase28_ns_std::Phase28StdProgram());
            auto heap = prog->heap();
            auto asyncRt = prog->asyncRuntime();

            runtime::ExecutionOptions opts;
            opts.trustedRelease = trustedMode;
            opts.limits.gcWorkBudget = trustedMode ? 0 : 1; // aggressive GC in Standard AOT!

            // (i) Reference + Array scalar coroutine across 2 suspensions with full GC at each pause
            const auto gA = asyncRt->awaitables().allocatePending();
            const auto gB = asyncRt->awaitables().allocatePending();
            const auto startRef = prog->invoke(
                "Phase28.TrustedAsync::StartRefCoroHelper",
                {
                    std::int64_t{7},
                    runtime::makeAwaitableStructValue(intAwaitableTid, gA),
                    runtime::makeAwaitableStructValue(intAwaitableTid, gB),
                },
                opts);
            require(startRef.succeeded, "6d StartRefCoroHelper launch failed: " + startRef.error.message);
            runtime::AwaitableHandle refCoroHandle{};
            require(runtime::tryExtractAwaitableHandle(startRef.value, refCoroHandle),
                "6d missing refCoroHandle");

            {
                runtime::ShadowStack emptyStack;
                heap->collectFull(emptyStack);
            }
            require(asyncRt->awaitables().trySetResult(gA, std::int64_t{5}), "6d resolve gA");
            asyncRt->stepResumeQueue(runtime::ResumeQueue::Update);
            {
                runtime::ShadowStack emptyStack;
                heap->collectFull(emptyStack);
            }
            require(asyncRt->awaitables().trySetResult(gB, std::int64_t{9}), "6d resolve gB");
            asyncRt->stepResumeQueue(runtime::ResumeQueue::Update);

            runtime::Value refVal{};
            runtime::RuntimeError refErr{};
            require(asyncRt->awaitables().getResult(refCoroHandle, refVal, refErr) ==
                    runtime::AwaitStatus::Succeeded,
                "6d StartRefCoroHelper did not succeed: " + refErr.message);
            // scratch=[7, 10+(5+7)=22, 20+9=29], total = 7 + 22 + 29 + 7 = 65
            require(std::get<std::int64_t>(refVal) == 65,
                "6d StartRefCoroHelper result mismatch under GC pressure");

            // (ii) Uncaught script exception after resume via finishUncaughtAsyncExceptionScalar
            const auto throwGate = asyncRt->awaitables().allocatePending();
            const auto throwStart = prog->invoke(
                "Phase28.TrustedAsync::ThrowAfterSuspendScalarCoro",
                {runtime::makeAwaitableStructValue(intAwaitableTid, throwGate)},
                opts);
            require(throwStart.succeeded, "6d ThrowAfterSuspendScalarCoro launch failed");
            runtime::AwaitableHandle throwHandle{};
            require(runtime::tryExtractAwaitableHandle(throwStart.value, throwHandle),
                "6d missing throwHandle");
            require(asyncRt->awaitables().trySetResult(throwGate, std::int64_t{-3}),
                "6d resolve throwGate");
            asyncRt->stepResumeQueue(runtime::ResumeQueue::Update);

            runtime::Value throwVal{};
            runtime::RuntimeError throwErr{};
            runtime::Value excObj{};
            require(asyncRt->awaitables().getResult(throwHandle, throwVal, throwErr, &excObj) ==
                    runtime::AwaitStatus::Faulted,
                "6d expected ThrowAfterSuspendScalarCoro to fault");
            require(throwErr.code == runtime::ErrorCode::ScriptException &&
                    throwErr.message.find("scalar_coro_uncaught_boom") != std::string::npos,
                "6d expected ScriptException with message 'scalar_coro_uncaught_boom', got: " +
                throwErr.message);

            // (iii) Multi-suspend runnerOwner lifetime across Program destruction + real DivisionByZero
            const auto divGateA = asyncRt->awaitables().allocatePending();
            const auto divGateB = asyncRt->awaitables().allocatePending();
            const auto divStart = prog->invoke(
                "Phase28.TrustedAsync::FaultDivideAfterTwoSuspends",
                {
                    std::int64_t{0},
                    runtime::makeAwaitableStructValue(intAwaitableTid, divGateA),
                    runtime::makeAwaitableStructValue(intAwaitableTid, divGateB),
                },
                opts);
            require(divStart.succeeded, "6d FaultDivideAfterTwoSuspends launch failed");
            runtime::AwaitableHandle divHandle{};
            require(runtime::tryExtractAwaitableHandle(divStart.value, divHandle),
                "6d missing divHandle");

            // Destroy `prog` BEFORE resolving 1st suspension -> tests runnerOwner across 2nd suspension!
            prog.reset();

            require(asyncRt->awaitables().trySetResult(divGateA, std::int64_t{40}),
                "6d resolve divGateA");
            asyncRt->stepResumeQueue(runtime::ResumeQueue::Update);
            require(!asyncRt->awaitables().isCompleted(divHandle),
                "6d divHandle must suspend a 2nd time on divGateB even after Program was destroyed");

            require(asyncRt->awaitables().trySetResult(divGateB, std::int64_t{2}),
                "6d resolve divGateB");
            asyncRt->stepResumeQueue(runtime::ResumeQueue::Update);

            runtime::Value divVal{};
            runtime::RuntimeError divErr{};
            require(asyncRt->awaitables().getResult(divHandle, divVal, divErr) ==
                    runtime::AwaitStatus::Faulted,
                "6d expected FaultDivideAfterTwoSuspends to fault");
            require(divErr.code == runtime::ErrorCode::DivisionByZero,
                "6d expected ErrorCode::DivisionByZero instead of generic InvalidProgram, got code=" +
                std::to_string(static_cast<int>(divErr.code)) + " msg=" + divErr.message);

            require(asyncRt->coroutines().statistics().activeCount == 0 &&
                    asyncRt->coroutines().statistics().activeGcRoots == 0 &&
                    asyncRt->awaitables().statistics().activeCount == 0,
                "6d scalar AOT leaked frames, GC roots, or awaitable slots");
        };

        testGcAndFaultsOnProgram(false); // Standard AOT
        testGcAndFaultsOnProgram(true);  // TrustedRelease Scalar AOT

        // Also verify Standard AOT generated C++ emits setActiveGeneratedLine & acquireRawColumnCache on resume
        compiler::BuildResult build;
        optimization::Result optimized;
        (void)compilePhase28BytecodeModules(&build, &optimized);
        aot::CppGenerator generator;
        aot::GenerationOptions stdGenOpts;
        stdGenOpts.programName = "Phase28StdInspect";
        stdGenOpts.trustedRelease = false;
        const auto stdGenerated = generator.generate(optimized.modules, build.diagnostics, stdGenOpts);
        require(!build.diagnostics.hasErrors(), "6d Std AOT codegen inspection failed");
        require(stdGenerated.source.find("aot::retainTypedGcSlot(") != std::string::npos,
            "6d expected retainTypedGcSlot in Standard AOT scalar coroutine codegen");
        require(stdGenerated.source.find(".push_back(context.heap()->retain(") == std::string::npos ||
                stdGenerated.source.find("typedLocalGcGuards.push_back(") == std::string::npos,
            "6d Standard AOT must never emit typedLocalGcGuards.push_back on std::array");
        require(stdGenerated.source.find("context.acquireRawColumnCache(typedLocal_") != std::string::npos,
            "6d expected acquireRawColumnCache rebuild in Standard AOT array local handling");
    }
}

} // namespace

int main() {
    try {
        testLinkedTimeoutCancellationPool();
        testHttpAwaitableBridgeAndTimeoutCancellation();
        testWebSocketFullDuplexTimeoutAndEntityDestroy();
        testTrustedReleaseScalarCoroutineCodegenAndParity();
        testStage4ReviewRegressions();
        runStage4AsyncAotBenchmarks();
        std::cout << "[PASS] All Phase 28 Network Async (HTTP/WebSocket + Timeout) & Trusted Scalar AOT tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[FAIL] " << ex.what() << "\n";
        return 1;
    }
}
