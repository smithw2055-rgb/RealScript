#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/debug/DapServer.h"
#include "realscript/debug/DebugInfo.h"
#include "realscript/debug/Debugger.h"
#include "realscript/diagnostics/Diagnostic.h"
#include "realscript/runtime/FiberScheduler.h"
#include "realscript/runtime/Runtime.h"
#include "realscript/text/Text.h"
#include "realscript/tooling/Json.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <new>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace realscript;

[[noreturn]] void failCheck(const char* expr, int line) noexcept {
    std::cerr << "Check failed at app_debug_and_dap_tests.cpp:" << line << ": " << expr << std::endl;
    std::exit(1);
}

#define RS_CHECK(...)                                                                              \
    do {                                                                                           \
        if (!(__VA_ARGS__)) {                                                                      \
            failCheck(#__VA_ARGS__, __LINE__);                                                     \
        }                                                                                          \
    } while (false)

// Zero-alloc telemetry measurement hook. Only active when g_countHeapAllocs is true.
std::atomic<bool> g_countHeapAllocs{false};
std::atomic<std::size_t> g_heapAllocCount{0};

} // namespace

void* operator new(std::size_t size) {
    if (g_countHeapAllocs.load(std::memory_order_relaxed)) {
        g_heapAllocCount.fetch_add(1, std::memory_order_relaxed);
    }
    void* ptr = std::malloc(size == 0 ? 1 : size);
    if (!ptr) {
        throw std::bad_alloc();
    }
    return ptr;
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void operator delete(void* ptr) noexcept {
    std::free(ptr);
}

void operator delete[](void* ptr) noexcept {
    std::free(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept {
    std::free(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept {
    std::free(ptr);
}

namespace {

struct BuiltProgram {
    std::shared_ptr<runtime::ProgramImage> program;
    std::shared_ptr<runtime::ManagedHeap> heap;
    std::vector<mir::Module> mirModules;
    std::vector<bytecode::Module> bytecodeModules;
};

BuiltProgram compileProgram(std::vector<compiler::SourceFile> sources) {
    compiler::Compilation compilation(std::move(sources));
    const auto buildResult = compilation.build();
    if (buildResult.diagnostics.hasErrors()) {
        for (const auto& d : buildResult.diagnostics.items()) {
            std::cerr << "Compile diagnostic [" << d.code << "]: " << d.message << "\n";
        }
    }
    RS_CHECK(!buildResult.diagnostics.hasErrors());

    bytecode::Lowerer lowerer;
    std::vector<bytecode::Module> lowered;
    lowered.reserve(buildResult.modules.size());
    for (const auto& mirMod : buildResult.modules) {
        lowered.push_back(lowerer.lower(mirMod));
    }

    runtime::RuntimeError linkError;
    auto linked = runtime::ProgramImage::link(lowered, linkError);
    if (!linked) {
        std::cerr << "Link error: " << linkError.message << "\n";
    }
    RS_CHECK(linked.has_value());

    BuiltProgram out;
    out.program = std::make_shared<runtime::ProgramImage>(std::move(*linked));
    out.heap = std::make_shared<runtime::ManagedHeap>();
    out.mirModules = buildResult.modules;
    out.bytecodeModules = std::move(lowered);
    return out;
}

const runtime::ProgramImage::FunctionLocation* findFnLocation(
    const runtime::ProgramImage& program,
    const std::string& qualifiedName) {
    const auto symId = program.findFunction(qualifiedName);
    if (!symId) return nullptr;
    return program.resolveFunction(*symId);
}

#ifdef _WIN32
struct WinsockScope {
    WinsockScope() {
        WSADATA wsa{};
        const int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
        RS_CHECK(rc == 0);
        (void)rc;
    }
    ~WinsockScope() { WSACleanup(); }
};
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSock = INVALID_SOCKET;
void closeSock(SocketHandle s) {
    if (s != INVALID_SOCKET) {
        closesocket(s);
    }
}
#else
struct WinsockScope {};
using SocketHandle = int;
constexpr SocketHandle kInvalidSock = -1;
void closeSock(SocketHandle s) {
    if (s >= 0) {
        close(s);
    }
}
#endif

SocketHandle connectLoopback(std::uint16_t port) {
    SocketHandle sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    RS_CHECK(sock != kInvalidSock);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    RS_CHECK(inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1);

    const int rc = connect(sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    RS_CHECK(rc == 0);
    (void)rc;
    return sock;
}

void sendDapFrame(SocketHandle sock, const tooling::Json& msg) {
    const std::string body = msg.dump();
    const std::string framed =
        "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    std::size_t sentTotal = 0;
    while (sentTotal < framed.size()) {
        const auto sent = send(
            sock,
            framed.data() + sentTotal,
            static_cast<int>(framed.size() - sentTotal),
            0);
        RS_CHECK(sent > 0);
        sentTotal += static_cast<std::size_t>(sent);
    }
}

tooling::Json recvDapFrame(SocketHandle sock, std::string& buffer) {
    for (;;) {
        const auto headerEnd = buffer.find("\r\n\r\n");
        if (headerEnd != std::string::npos) {
            const std::string headers = buffer.substr(0, headerEnd);
            const auto clPos = headers.find("Content-Length:");
            RS_CHECK(clPos != std::string::npos);
            const auto lineEnd = headers.find('\n', clPos);
            const std::string lenStr = headers.substr(
                clPos + 15,
                lineEnd == std::string::npos ? std::string::npos : lineEnd - (clPos + 15));
            const auto contentLen = static_cast<std::size_t>(std::stoul(lenStr));
            const auto totalLen = headerEnd + 4 + contentLen;
            if (buffer.size() >= totalLen) {
                const std::string jsonBody = buffer.substr(headerEnd + 4, contentLen);
                buffer.erase(0, totalLen);
                std::string parseErr;
                auto parsed = tooling::Json::parse(jsonBody, parseErr);
                RS_CHECK(parsed.has_value());
                return *parsed;
            }
        }
        char chunk[4096];
        const int n = recv(sock, chunk, static_cast<int>(sizeof(chunk)), 0);
        RS_CHECK(n > 0);
        buffer.append(chunk, static_cast<std::size_t>(n));
    }
}

tooling::Json sendAndAwaitResponse(
    SocketHandle sock,
    std::string& readBuf,
    std::int64_t seq,
    const std::string& command,
    tooling::Json args = tooling::Json::Object{},
    std::vector<tooling::Json>* capturedEvents = nullptr) {
    tooling::Json req = tooling::Json::Object{
        {"seq", seq},
        {"type", "request"},
        {"command", command},
        {"arguments", std::move(args)},
    };
    sendDapFrame(sock, req);
    for (;;) {
        auto msg = recvDapFrame(sock, readBuf);
        const auto* typeNode = msg.find("type");
        if (typeNode && typeNode->stringValue() == "event") {
            if (capturedEvents) {
                capturedEvents->push_back(std::move(msg));
            }
            continue;
        }
        const auto* reqSeq = msg.find("request_seq");
        if (typeNode && typeNode->stringValue() == "response" && reqSeq &&
            reqSeq->integerValue(-1) == seq) {
            return msg;
        }
    }
}

void testDebugInfoEncodingAndVerifierRejection() {
    auto built = compileProgram({{
        "debug_verify.rscript",
        R"(module Debug.Verify;
int addTwice(int x)
{
    int local = x + 3;
    return local + x;
}
)",
    }});

    RS_CHECK(!built.bytecodeModules.empty());
    const auto& mod = built.bytecodeModules.front();
    const auto encoded = bytecode::encodeModule(mod);
    bytecode::Module decoded;
    diagnostics::DiagnosticBag decodeDiags;
    RS_CHECK(bytecode::decodeModule(encoded, decoded, decodeDiags));

    diagnostics::DiagnosticBag verifyDiags;
    RS_CHECK(bytecode::verifyModule(decoded, verifyDiags));

    bytecode::Module corrupt = decoded;
    RS_CHECK(!corrupt.functions.empty());
    RS_CHECK(!corrupt.functions.front().debugInfo.sequencePoints.empty());
    corrupt.functions.front().debugInfo.sequencePoints.front().instructionIndex = 999999u;
    diagnostics::DiagnosticBag corruptDiags;
    RS_CHECK(!bytecode::verifyModule(corrupt, corruptDiags));
    RS_CHECK(corruptDiags.hasErrors());

    std::cout << "[PASS] DebugInfo encoding & verifier rejection\n";
}

void testLineDirectivesMappingAndHiddenStepping() {
    // Line 1: module Debug.LineMap;
    // Line 3: public class State { public int val = 0; }
    // Line 4: #line hidden
    // Line 5: void helperHidden(State st, int x)
    // Line 7:     int doubled = x * 2;
    // Line 8:     st.val = doubled + 1;
    // Line 10: #line default
    // Line 12: int stepTarget(int a)
    // Line 15:     helperHidden(st, a);   <-- single-statement call site
    // Line 16: #line 200 "virtual/generated.rscript"
    // Line 17:     int z = st.val + 10;   <-- 0-based mapped line 199 (1-based 200)
    // Line 18:     return z;              <-- 0-based mapped line 200 (1-based 201)
    // Line 19: #line default
    const std::string source =
        "module Debug.LineMap;\n"
        "\n"
        "public class State { public int val = 0; }\n"
        "#line hidden\n"
        "void helperHidden(State st, int x)\n"
        "{\n"
        "    int doubled = x * 2;\n"
        "    st.val = doubled + 1;\n"
        "}\n"
        "#line default\n"
        "\n"
        "int stepTarget(int a)\n"
        "{\n"
        "    var st = new State();\n"
        "    helperHidden(st, a);\n"
        "#line 200 \"virtual/generated.rscript\"\n"
        "    int z = st.val + 10;\n"
        "    return z;\n"
        "#line default\n"
        "}\n";

    const std::string hiddenOnlySource =
        "module Debug.HiddenOnly;\n"
        "#line hidden\n"
        "public int hiddenFn(int v)\n"
        "{\n"
        "    int t = v + 1;\n"
        "    return t;\n"
        "}\n"
        "#line default\n";

    auto built = compileProgram({
        {"line_map.rscript", source},
        {"hidden_only.rscript", hiddenOnlySource},
    });
    const auto* hiddenLoc = findFnLocation(*built.program, "Debug.LineMap::helperHidden");
    const auto* targetLoc = findFnLocation(*built.program, "Debug.LineMap::stepTarget");
    RS_CHECK(hiddenLoc && targetLoc);
    RS_CHECK(!hiddenLoc->function->debugInfo.sequencePoints.empty());

    bool sawHidden = false;
    for (const auto& sp : hiddenLoc->function->debugInfo.sequencePoints) {
        if (sp.hidden) {
            sawHidden = true;
        }
    }
    RS_CHECK(sawHidden);

    bool sawMapped = false;
    std::uint32_t entryCallLine = 0;
    for (const auto& sp : targetLoc->function->debugInfo.sequencePoints) {
        if (sp.mappedPath == "virtual/generated.rscript") {
            RS_CHECK(sp.range.start.line >= 199u);
            sawMapped = true;
        } else if (!sp.hidden && sp.range.start.line >= 14u && entryCallLine == 0) {
            entryCallLine = static_cast<std::uint32_t>(sp.range.start.line);
        }
    }
    RS_CHECK(sawMapped);
    RS_CHECK(entryCallLine >= 14u);

    // Confirm setting a breakpoint inside #line hidden never binds on hidden lines.
    auto session = std::make_shared<debug::DebugSession>(built.program, built.heap);
    auto controller = session->controller();

    const auto hiddenOnlyBps = controller->setBreakpoints(
        "hidden_only.rscript",
        {text::LinePosition{4u, 0u}, text::LinePosition{5u, 0u}});
    RS_CHECK(hiddenOnlyBps.size() == 2);
    RS_CHECK(!hiddenOnlyBps[0].verified);
    RS_CHECK(!hiddenOnlyBps[1].verified);

    controller->clearBreakpoints();
    const auto snappedBps = controller->setBreakpoints(
        "line_map.rscript",
        {text::LinePosition{6u, 0u}, text::LinePosition{7u, 0u}});
    RS_CHECK(snappedBps.size() == 2);
    RS_CHECK(snappedBps[0].line >= 13u && snappedBps[0].line != 6u);
    RS_CHECK(snappedBps[1].line >= 13u && snappedBps[1].line != 7u);

    // Real breakpoint on `int y = helperHidden(a);`
    controller->clearBreakpoints();
    const auto callBps = controller->setBreakpoints(
        "line_map.rscript",
        {text::LinePosition{entryCallLine, 0u}});
    RS_CHECK(callBps.size() == 1);
    RS_CHECK(callBps[0].verified);

    runtime::ExecutionResult result;
    std::thread worker([&]() {
        runtime::ExecutionOptions opts;
        opts.debugger = controller;
        runtime::Interpreter interp(built.program, built.heap);
        result = interp.invoke(
            "Debug.LineMap::stepTarget",
            {runtime::Value{std::int64_t{5}}},
            opts);
    });

    RS_CHECK(session->waitForStop(std::chrono::seconds(5)));
    auto stop1 = session->currentStop();
    RS_CHECK(stop1.has_value());
    RS_CHECK(!stop1->frames.empty());
    RS_CHECK(stop1->frames[0].sourcePath == "line_map.rscript");
    RS_CHECK(stop1->frames[0].location.start.line == entryCallLine);

    // StepIn into helperHidden(st, a): because helperHidden is `#line hidden`, StepIn MUST NEVER
    // stop inside helperHidden and MUST advance directly onto `#line 200 "virtual/generated.rscript"`.
    std::optional<debug::DebugStop> stop2;
    for (int stepIdx = 0; stepIdx < 8; ++stepIdx) {
        session->resume(debug::ResumeMode::StepIn);
        RS_CHECK(session->waitForStop(std::chrono::seconds(5)));
        auto cur = session->currentStop();
        RS_CHECK(cur.has_value());
        RS_CHECK(!cur->frames.empty());
        RS_CHECK(cur->frames[0].function.find("helperHidden") == std::string::npos);
        RS_CHECK(cur->frames[0].function.find("stepTarget") != std::string::npos);
        if (cur->frames[0].sourcePath == "virtual/generated.rscript") {
            stop2 = std::move(cur);
            break;
        }
        RS_CHECK(cur->frames[0].sourcePath == "line_map.rscript");
        RS_CHECK(cur->frames[0].location.start.line == entryCallLine);
    }
    RS_CHECK(stop2.has_value());
    RS_CHECK(stop2->frames[0].sourcePath == "virtual/generated.rscript");
    RS_CHECK(stop2->frames[0].location.start.line == 199u);

    // Verify DAP `stackTrace` exposes mappedPath and 1-based line 200 on the stopped frame.
    debug::DapServer dap(session, built.program, built.heap);
    std::ostringstream dapEvents;
    const auto stResp = dap.handle(
        tooling::Json::Object{
            {"seq", std::int64_t{1}},
            {"type", "request"},
            {"command", "stackTrace"},
            {"arguments", tooling::Json::Object{{"threadId", std::int64_t{1}}}},
        },
        &dapEvents);
    RS_CHECK(stResp.find("success")->boolValue(false));
    const auto* stackFrames = stResp.find("body")->find("stackFrames");
    RS_CHECK(stackFrames && stackFrames->isArray() && !stackFrames->arrayValue().empty());
    const auto& topFrame = stackFrames->arrayValue().front();
    RS_CHECK(topFrame.find("line")->integerValue(0) == 200);
    RS_CHECK(topFrame.find("source")->find("name")->stringValue() == "virtual/generated.rscript");
    RS_CHECK(topFrame.find("source")->find("path")->stringValue() == "virtual/generated.rscript");

    session->resume(debug::ResumeMode::Continue);
    worker.join();
    RS_CHECK(result.succeeded);
    RS_CHECK(std::get<std::int64_t>(result.value) == 21);

    // Also confirm StepOver skips `#line hidden` helper cleanly onto virtual line 200.
    controller->clearBreakpoints();
    (void)controller->setBreakpoints(
        "line_map.rscript",
        {text::LinePosition{entryCallLine, 0u}});
    runtime::ExecutionResult result2;
    std::thread worker2([&]() {
        runtime::ExecutionOptions opts;
        opts.debugger = controller;
        runtime::Interpreter interp(built.program, built.heap);
        result2 = interp.invoke(
            "Debug.LineMap::stepTarget",
            {runtime::Value{std::int64_t{7}}},
            opts);
    });
    RS_CHECK(session->waitForStop(std::chrono::seconds(5)));
    std::optional<debug::DebugStop> stopOver;
    for (int stepIdx = 0; stepIdx < 8; ++stepIdx) {
        session->resume(debug::ResumeMode::StepOver);
        RS_CHECK(session->waitForStop(std::chrono::seconds(5)));
        auto cur = session->currentStop();
        RS_CHECK(cur.has_value());
        RS_CHECK(!cur->frames.empty());
        RS_CHECK(cur->frames[0].function.find("helperHidden") == std::string::npos);
        if (cur->frames[0].sourcePath == "virtual/generated.rscript") {
            stopOver = std::move(cur);
            break;
        }
        RS_CHECK(cur->frames[0].sourcePath == "line_map.rscript");
        RS_CHECK(cur->frames[0].location.start.line == entryCallLine);
    }
    RS_CHECK(stopOver.has_value());
    RS_CHECK(stopOver->frames[0].sourcePath == "virtual/generated.rscript");
    RS_CHECK(stopOver->frames[0].location.start.line == 199u);
    session->resume(debug::ResumeMode::Continue);
    worker2.join();
    RS_CHECK(result2.succeeded);
    RS_CHECK(std::get<std::int64_t>(result2.value) == 25);

    std::cout << "[PASS] #line mappedPath reporting & #line hidden stepping\n";
}

void testConditionalBreakpointsEvalSandboxAndExceptions() {
    const std::string source =
        "module Debug.EvalSandbox;\n"
        "import RealScript.Math;\n"
        "\n"
        "class Counter {\n"
        "    int total = 0;\n"
        "    public Vec3 pos = Vec3.Of(6, 8, 10);\n"
        "    int doubled { get { return total * 2; } }\n"
        "    int bump(int delta) { total = total + delta; return total; }\n"
        "}\n"
        "\n"
        "class BoomError {\n"
        "    public string msg = \"boom\";\n"
        "}\n"
        "\n"
        "int pureCalc(int v) { return v * 3 + 1; }\n"
        "\n"
        "int runLoop(int limit)\n"
        "{\n"
        "    var counter = new Counter();\n"
        "    int acc = 0;\n"
        "    for (int i = 1; i <= limit; i = i + 1) {\n"
        "        int probe = i;\n"
        "        acc = acc + counter.bump(probe);\n"
        "    }\n"
        "    return acc;\n"
        "}\n"
        "\n"
        "int triggerThrow()\n"
        "{\n"
        "    throw new BoomError();\n"
        "}\n";

    auto built = compileProgram({{"eval_sandbox.rscript", source}});
    const auto* runLoc = findFnLocation(*built.program, "Debug.EvalSandbox::runLoop");
    RS_CHECK(runLoc);
    const auto bumpPos = source.find("int probe = i;");
    RS_CHECK(bumpPos != std::string::npos);
    const std::uint32_t targetAccLine = static_cast<std::uint32_t>(
        std::count(source.begin(), source.begin() + static_cast<std::ptrdiff_t>(bumpPos), '\n'));
    std::uint32_t accLine = 0;
    for (const auto& sp : runLoc->function->debugInfo.sequencePoints) {
        if (sp.range.start.line >= targetAccLine) {
            accLine = static_cast<std::uint32_t>(sp.range.start.line);
            break;
        }
    }
    RS_CHECK(accLine == targetAccLine);

    auto session = std::make_shared<debug::DebugSession>(built.program, built.heap);
    auto controller = session->controller();
    const auto bps = controller->setConditionalBreakpoints(
        "eval_sandbox.rscript",
        {debug::BreakpointSpec{text::LinePosition{accLine, 0u}, "i == 3"}});
    RS_CHECK(bps.size() == 1 && bps[0].verified);

    runtime::ExecutionResult res;
    std::thread t([&]() {
        runtime::ExecutionOptions opts;
        opts.debugger = controller;
        runtime::Interpreter interp(built.program, built.heap);
        res = interp.invoke(
            "Debug.EvalSandbox::runLoop",
            {runtime::Value{std::int64_t{4}}},
            opts);
    });

    RS_CHECK(session->waitForStop(std::chrono::seconds(5)));
    auto stop = session->currentStop();
    RS_CHECK(stop.has_value());
    RS_CHECK(!stop->frames.empty());

    // Pure getter & pure function evaluation succeed inside ReadOnly DAP eval sandbox.
    std::string evalErr;
    const auto pureEval = session->evaluate("counter.doubled + pureCalc(i)", 1u, &evalErr);
    RS_CHECK(pureEval.has_value());
    RS_CHECK(std::get<std::int64_t>(*pureEval) == 16);

    // Inline Vec3 / Fixed introspection via ReadOnly DAP eval sandbox.
    const auto vecEval = session->evaluate("counter.pos.x.ToInt() + counter.pos[2].ToInt()", 1u, &evalErr);
    RS_CHECK(vecEval.has_value());
    RS_CHECK(std::get<std::int64_t>(*vecEval) == 16);

    // Side-effecting method invocation is rejected by the pure-getter sandbox.
    std::string mutateErr;
    const auto mutateEval = session->evaluate("counter.bump(99)", 1u, &mutateErr);
    RS_CHECK(!mutateEval.has_value());
    RS_CHECK(!mutateErr.empty());

    // Confirm DAP `scopes` + `variables` inspect locals without mutating state.
    debug::DapServer dap(session, built.program, built.heap);
    const auto scopesResp = dap.handle(tooling::Json::Object{
        {"seq", std::int64_t{10}},
        {"type", "request"},
        {"command", "scopes"},
        {"arguments", tooling::Json::Object{{"frameId", std::int64_t{1}}}},
    });
    RS_CHECK(scopesResp.find("success")->boolValue(false));
    const auto& scopes = scopesResp.find("body")->find("scopes")->arrayValue();
    RS_CHECK(!scopes.empty());
    const auto varsResp = dap.handle(tooling::Json::Object{
        {"seq", std::int64_t{11}},
        {"type", "request"},
        {"command", "variables"},
        {"arguments", tooling::Json::Object{
            {"variablesReference", scopes[0].find("variablesReference")->integerValue(0)},
        }},
    });
    RS_CHECK(varsResp.find("success")->boolValue(false));

    controller->clearBreakpoints();
    session->resume(debug::ResumeMode::Continue);
    t.join();
    RS_CHECK(res.succeeded);
    // 1 + (1+2) + (1+2+3) + (1+2+3+4) = 1 + 3 + 6 + 10 = 20.
    RS_CHECK(std::get<std::int64_t>(res.value) == 20);

    // Exception breakpoint test.
    controller->clearBreakpoints();
    controller->setExceptionBreakpoints(true, true, true);
    runtime::ExecutionResult exRes;
    std::thread tEx([&]() {
        runtime::ExecutionOptions opts;
        opts.debugger = controller;
        runtime::Interpreter interp(built.program, built.heap);
        exRes = interp.invoke("Debug.EvalSandbox::triggerThrow", {}, opts);
    });
    RS_CHECK(session->waitForStop(std::chrono::seconds(5)));
    auto exStop = session->currentStop();
    RS_CHECK(exStop.has_value());
    session->resume(debug::ResumeMode::Continue);
    tEx.join();
    RS_CHECK(!exRes.succeeded);

    std::cout << "[PASS] Conditional breakpoints, DAP eval sandbox & exception breaks\n";
}

void testFiberIntrospectionAndZeroAllocTelemetry() {
    const std::string fiberSource =
        "module Debug.FiberTest;\n"
        "import RealScript.Fiber;\n"
        "\n"
        "public void patrol()\n"
        "{\n"
        "    int steps = 42;\n"
        "    Fiber.Yield(3);\n"
        "    steps = steps + 1;\n"
        "}\n";

    auto built = compileProgram({{"fiber_debug.rscript", fiberSource}});
    RS_CHECK(!built.mirModules.empty());
    const auto compiledFiber = runtime::FiberCompiler::compileModule(built.mirModules.front());

    runtime::FiberScheduler scheduler;
    const auto pid = scheduler.registerModule(compiledFiber);
    RS_CHECK(pid > 0);
    const auto fid = scheduler.spawnFunction(
        compiledFiber,
        "patrol",
        runtime::GenerationalEntityHandle{100u, 1u});
    RS_CHECK(fid > 0);

    // Step tick 0 so the patrol fiber initializes `steps = 42` and yields for 3 ticks.
    scheduler.stepTick(0);
    RS_CHECK(scheduler.activeFiberCount() == 1);

    debug::DapServer dap(built.program, built.heap);
    dap.bindFiberScheduler(&scheduler, &compiledFiber);

    const auto threadsResp = dap.handle(tooling::Json::Object{
        {"seq", std::int64_t{1}},
        {"type", "request"},
        {"command", "threads"},
    });
    RS_CHECK(threadsResp.find("success")->boolValue(false));
    const auto& threads = threadsResp.find("body")->find("threads")->arrayValue();
    std::int64_t fiberThreadId = 0;
    for (const auto& th : threads) {
        const auto id = th.find("id")->integerValue(0);
        if (id >= 1000) {
            fiberThreadId = id;
            break;
        }
    }
    RS_CHECK(fiberThreadId >= 1000);

    const auto fiberStackResp = dap.handle(tooling::Json::Object{
        {"seq", std::int64_t{2}},
        {"type", "request"},
        {"command", "stackTrace"},
        {"arguments", tooling::Json::Object{{"threadId", fiberThreadId}}},
    });
    RS_CHECK(fiberStackResp.find("success")->boolValue(false));
    const auto& fFrames = fiberStackResp.find("body")->find("stackFrames")->arrayValue();
    RS_CHECK(!fFrames.empty());

    // Zero-alloc telemetry ring verification while runtime records hot frames.
    runtime::FrameTelemetryCollector telemetry;
    runtime::ActiveFrameTelemetryScope telemetryScope(&telemetry);
    dap.bindFrameTelemetry(&telemetry);

    g_heapAllocCount.store(0, std::memory_order_relaxed);
    g_countHeapAllocs.store(true, std::memory_order_release);
    for (std::uint64_t frame = 1; frame <= 8; ++frame) {
        runtime::FrameTelemetrySample sample{};
        sample.frameNumber = frame;
        sample.totalInstructions = frame * 100u;
        sample.hotFunctionCount = 1;
        sample.topFunctions[0] = runtime::HotFunctionTelemetry{1u, frame * 100u, 1.0};
        telemetry.commitSample(sample);
        runtime::FrameTelemetrySample latest{};
        RS_CHECK(telemetry.latestSample(latest));
        RS_CHECK(latest.frameNumber == frame);
    }
    g_countHeapAllocs.store(false, std::memory_order_release);
    RS_CHECK(g_heapAllocCount.load(std::memory_order_relaxed) == 0);

    std::ostringstream out;
    dap.emitFrameTelemetry(out);
    const std::string emitted = out.str();
    RS_CHECK(emitted.find("\"event\":\"realscript/frameTelemetry\"") != std::string::npos);

    std::cout << "[PASS] Fiber DAP introspection & zero-alloc telemetry\n";
}

void testEmbeddedDapSocketAttachAndHotReloadRebind() {
    WinsockScope winsock;

    auto v1 = compileProgram({{
        "attach_v1.rscript",
        R"(module Debug.Attach;
int compute(int x)
{
    int seed = x + 5;
    return seed * 2;
}
)",
    }});

    debug::EmbeddedDapServer embedded(v1.program, v1.heap);
    // Empty bindAddress must safely default to loopback 127.0.0.1 (AUD-04 hardening).
    RS_CHECK(embedded.start(0, ""));
    RS_CHECK(embedded.isRunning());
    RS_CHECK(embedded.boundPort() > 0);
    RS_CHECK(embedded.boundAddress() == "127.0.0.1");

    const SocketHandle client = connectLoopback(embedded.boundPort());
    std::string readBuf;
    std::vector<tooling::Json> events;

    const auto initResp = sendAndAwaitResponse(
        client, readBuf, 1, "initialize",
        tooling::Json::Object{{"adapterID", "realscript"}}, &events);
    RS_CHECK(initResp.find("success")->boolValue(false));

    const auto attachResp = sendAndAwaitResponse(
        client, readBuf, 2, "attach", tooling::Json::Object{}, &events);
    RS_CHECK(attachResp.find("success")->boolValue(false));

    const auto* fnLoc = findFnLocation(*v1.program, "Debug.Attach::compute");
    RS_CHECK(fnLoc && !fnLoc->function->debugInfo.sequencePoints.empty());
    const std::int64_t line1 = static_cast<std::int64_t>(
        fnLoc->function->debugInfo.sequencePoints.front().range.start.line + 1u);

    const auto bpResp = sendAndAwaitResponse(
        client, readBuf, 3, "setBreakpoints",
        tooling::Json::Object{
            {"source", tooling::Json::Object{{"path", "attach_v1.rscript"}}},
            {"breakpoints", tooling::Json::Array{tooling::Json::Object{{"line", line1}}}},
        },
        &events);
    RS_CHECK(bpResp.find("success")->boolValue(false));

    const auto doneResp = sendAndAwaitResponse(
        client, readBuf, 4, "configurationDone", tooling::Json::Object{}, &events);
    RS_CHECK(doneResp.find("success")->boolValue(false));

    runtime::ExecutionResult runRes;
    std::thread runner([&]() {
        runtime::ExecutionOptions opts;
        opts.debugger = embedded.controller();
        runtime::Interpreter interp(v1.program, v1.heap);
        runRes = interp.invoke(
            "Debug.Attach::compute",
            {runtime::Value{std::int64_t{4}}},
            opts);
    });

    RS_CHECK(embedded.session()->waitForStop(std::chrono::seconds(5)));

    const auto stResp = sendAndAwaitResponse(
        client, readBuf, 5, "stackTrace",
        tooling::Json::Object{{"threadId", std::int64_t{1}}}, &events);
    RS_CHECK(stResp.find("success")->boolValue(false));

    const auto clearBpResp = sendAndAwaitResponse(
        client, readBuf, 6, "setBreakpoints",
        tooling::Json::Object{
            {"source", tooling::Json::Object{{"path", "attach_v1.rscript"}}},
            {"breakpoints", tooling::Json::Array{}},
        },
        &events);
    RS_CHECK(clearBpResp.find("success")->boolValue(false));

    const auto contResp = sendAndAwaitResponse(
        client, readBuf, 7, "continue",
        tooling::Json::Object{{"threadId", std::int64_t{1}}}, &events);
    RS_CHECK(contResp.find("success")->boolValue(false));

    runner.join();
    RS_CHECK(runRes.succeeded);
    RS_CHECK(std::get<std::int64_t>(runRes.value) == 18);

    // Live hot-reload rebind (AUD-04): swap ProgramImage on the active EmbeddedDapServer.
    auto v2 = compileProgram({{
        "attach_v2.rscript",
        R"(module Debug.Attach;
public class V2Only {
    public int tag = 99;
}
public int makeAndCompute()
{
    var obj = new V2Only();
    return obj.tag + 1;
}
)",
    }});

    embedded.rebindProgram(v2.program);
    const auto v2TypeId = semantic::stableTypeId("Debug.Attach::V2Only");
    RS_CHECK(v2.program->resolveType(v2TypeId) != nullptr);
    const auto allocated = v1.heap->allocateObject(
        v2TypeId,
        std::vector<runtime::Value>{runtime::Value{std::int64_t{99}}},
        std::vector<std::size_t>{});
    RS_CHECK(allocated.has_value());
    auto rootGuard = v1.heap->retain(*allocated);
    (void)rootGuard;

    const auto snapResp = sendAndAwaitResponse(
        client, readBuf, 8, "realscript/heapSnapshot",
        tooling::Json::Object{{"diffBaseline", false}}, &events);
    RS_CHECK(snapResp.find("success")->boolValue(false));
    const std::string snapDump = snapResp.dump();
    RS_CHECK(snapDump.find("V2Only") != std::string::npos);

    (void)sendAndAwaitResponse(client, readBuf, 9, "disconnect", tooling::Json::Object{}, &events);
    closeSock(client);
    embedded.stop();
    RS_CHECK(!embedded.isRunning());

    std::cout << "[PASS] EmbeddedDapServer TCP attach, loopback bind & live rebindProgram\n";
}

} // namespace

int main() {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    testDebugInfoEncodingAndVerifierRejection();
    testLineDirectivesMappingAndHiddenStepping();
    testConditionalBreakpointsEvalSandboxAndExceptions();
    testFiberIntrospectionAndZeroAllocTelemetry();
    testEmbeddedDapSocketAttachAndHotReloadRebind();
    std::cout << "All app_debug_and_dap_tests passed!\n";
    return 0;
}
