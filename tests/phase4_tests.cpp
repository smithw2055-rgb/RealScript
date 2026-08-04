#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/debug/Debugger.h"
#include "realscript/debug/DapServer.h"
#include "realscript/hot_reload/HotReload.h"
#include "realscript/runtime/Runtime.h"
#include "realscript/tooling/LanguageService.h"
#include "realscript/tooling/LspServer.h"
#include "realscript/tooling/Json.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<realscript::bytecode::Module> compile(
    const std::vector<realscript::compiler::SourceFile>& files) {
    realscript::compiler::Compilation compilation;
    for (const auto& file : files) compilation.addSource(file);
    auto result = compilation.build();
    if (result.diagnostics.hasErrors()) {
        std::string message = "source compilation failed";
        for (const auto& diagnostic : result.diagnostics.items()) {
            message += "\n" + diagnostic.code + ": " + diagnostic.message;
        }
        throw std::runtime_error(message);
    }
    realscript::bytecode::Lowerer lowerer;
    std::vector<realscript::bytecode::Module> modules;
    for (const auto& mir : result.modules) {
        auto module = lowerer.lower(mir);
        realscript::diagnostics::DiagnosticBag diagnostics;
        if (!realscript::bytecode::verifyModule(module, diagnostics)) {
            std::string message = "Phase 4 bytecode verification failed";
            for (const auto& diagnostic : diagnostics.items()) {
                message += "\n" + diagnostic.code + ": " + diagnostic.message;
            }
            throw std::runtime_error(message);
        }
        modules.push_back(std::move(module));
    }
    return modules;
}

std::shared_ptr<const realscript::runtime::ProgramImage> link(
    std::vector<realscript::bytecode::Module> modules) {
    realscript::runtime::RuntimeError error;
    auto image = realscript::runtime::ProgramImage::link(std::move(modules), error);
    require(image.has_value(), "program image failed to link: " + error.message);
    return std::make_shared<const realscript::runtime::ProgramImage>(std::move(*image));
}

const char* debugSource = R"(
module Phase4.Debug;
int add(int value)
{
    int doubled = value * 2;
    return doubled + 1;
}
)";

void testDebugMetadataRoundTrip() {
    auto modules = compile({{"debug.rs", debugSource}});
    require(modules.size() == 1, "expected one debug module");
    const auto& module = modules.front();
    require(module.version.minor == 7, "Phase 19 must use .rsbc 0.7");
    require(module.sourceFiles.size() == 1 &&
            module.sourceFiles.front().path == "debug.rs",
        "debug source table is missing");
    require(module.functions.size() == 1, "debug function is missing");
    const auto& info = module.functions.front().debugInfo;
    require(!info.sequencePoints.empty(), "sequence points were not emitted");
    require(info.locals.size() >= 2, "parameters and locals were not emitted");
    require(std::any_of(info.locals.begin(), info.locals.end(),
            [](const auto& local) { return local.name == "doubled" && !local.parameter; }),
        "local debug metadata is missing");
    for (const auto& local : info.locals) {
        if (local.name == "doubled") {
            require(local.scope.span.length != 0 &&
                    local.scope.span.start <= local.declaration.span.start,
                "local lexical scope metadata is invalid");
        }
    }

    realscript::bytecode::Module decoded;
    realscript::diagnostics::DiagnosticBag diagnostics;
    require(realscript::bytecode::decodeModule(
            realscript::bytecode::encodeModule(module), decoded, diagnostics),
        "debug bytecode round trip failed");
    require(realscript::bytecode::verifyModule(decoded, diagnostics),
        "decoded debug metadata failed verification");
    require(decoded.sourceFiles.size() == module.sourceFiles.size() &&
            decoded.functions.front().debugInfo.sequencePoints.size() ==
                info.sequencePoints.size() &&
            decoded.functions.front().debugInfo.locals.size() == info.locals.size(),
        "debug metadata changed during codec round trip");
}


void testDebugVerifierRejectsCorruption() {
    auto modules = compile({{"debug.rs", debugSource}});
    auto invalidPoint = modules.front();
    invalidPoint.functions.front().debugInfo.sequencePoints.front().blockId = 999;
    realscript::diagnostics::DiagnosticBag pointDiagnostics;
    require(!realscript::bytecode::verifyModule(invalidPoint, pointDiagnostics),
        "invalid sequence point passed verification");
    require(std::any_of(pointDiagnostics.items().begin(), pointDiagnostics.items().end(),
            [](const auto& item) { return item.code == "RS5162"; }),
        "invalid sequence point did not report RS5162");

    auto invalidLocal = modules.front();
    invalidLocal.functions.front().debugInfo.locals.front().slot = 999;
    realscript::diagnostics::DiagnosticBag localDiagnostics;
    require(!realscript::bytecode::verifyModule(invalidLocal, localDiagnostics),
        "invalid local debug slot passed verification");
    require(std::any_of(localDiagnostics.items().begin(), localDiagnostics.items().end(),
            [](const auto& item) { return item.code == "RS5163"; }),
        "invalid local debug slot did not report RS5163");

    auto invalidSource = modules.front();
    invalidSource.sourceFiles.front().lineStarts = {1};
    realscript::diagnostics::DiagnosticBag sourceDiagnostics;
    require(!realscript::bytecode::verifyModule(invalidSource, sourceDiagnostics),
        "invalid debug source table passed verification");
    require(std::any_of(sourceDiagnostics.items().begin(), sourceDiagnostics.items().end(),
            [](const auto& item) { return item.code == "RS5160"; }),
        "invalid debug source table did not report RS5160");

    auto invalidRange = modules.front();
    invalidRange.functions.front().debugInfo.sequencePoints.front().range.start.column += 1;
    realscript::diagnostics::DiagnosticBag rangeDiagnostics;
    require(!realscript::bytecode::verifyModule(invalidRange, rangeDiagnostics),
        "inconsistent debug source range passed verification");
    require(std::any_of(rangeDiagnostics.items().begin(), rangeDiagnostics.items().end(),
            [](const auto& item) { return item.code == "RS5162"; }),
        "inconsistent debug source range did not report RS5162");
}

void testDebuggerSession() {
    auto modules = compile({{"debug.rs", debugSource}});
    const auto firstPoint = modules.front().functions.front().debugInfo.sequencePoints.front();
    const auto firstLine = firstPoint.range.start.line;
    auto program = link(std::move(modules));
    realscript::debug::DebugSession session(program);
    auto breakpoints = session.controller()->setBreakpoints(
        "debug.rs", {{firstLine, 0}});
    require(breakpoints.size() == 1 && breakpoints.front().verified,
        "source breakpoint did not bind to an executable line");
    require(session.launch("Phase4.Debug::add", {std::int64_t{4}}, true),
        "debug launch failed");
    require(session.waitForStop(std::chrono::seconds{2}),
        "debugger did not stop on entry");
    auto stop = session.currentStop();
    require(stop && stop->reason == realscript::debug::StopReason::Entry &&
            !stop->frames.empty(),
        "entry stop did not capture a stack frame");
    require(!stop->frames.front().arguments.empty() &&
            stop->frames.front().arguments.front().name == "value" &&
            std::get<std::int64_t>(stop->frames.front().arguments.front().value) == 4,
        "debugger did not capture function arguments");

    session.controller()->clearBreakpoints();
    session.stepIn();
    require(session.waitForStop(std::chrono::seconds{2}),
        "step-in did not reach another source location");
    stop = session.currentStop();
    require(stop && stop->reason == realscript::debug::StopReason::Step,
        "step-in stop reason is incorrect");
    session.continueExecution();
    session.join();
    const auto result = session.result();
    require(result && result->succeeded &&
            std::get<std::int64_t>(result->value) == 9,
        "debugged execution returned the wrong result");
}

void testDebuggerTermination() {
    auto program = link(compile({{"loop.rs", R"(
module Phase4.Terminate;
int run()
{
    int value = 0;
    while (true) value = value + 1;
}
)"}}));
    realscript::debug::DebugSession session(program);
    realscript::runtime::Limits limits;
    limits.instructionBudget = std::numeric_limits<std::uint64_t>::max();
    require(session.launch("Phase4.Terminate::run", {}, false, limits),
        "debug termination launch failed");
    session.terminate();
    session.join();
    const auto result = session.result();
    require(result && !result->succeeded &&
            result->error.code == realscript::runtime::ErrorCode::ExecutionTerminated,
        "terminating a running debug session did not stop execution safely");
}

void testLanguageService() {
    realscript::tooling::LanguageService service;
    service.open("math.rs", R"(
module Phase4.Math;
int twice(int value) { return value * 2; }
)");
    service.open("app.rs", R"(
module Phase4.App;
import Phase4.Math;
int main() { return twice(21); }
)");
    require(service.diagnostics("app.rs").empty(),
        "valid workspace produced diagnostics");
    const auto completion = service.completion("app.rs", {3, 10});
    require(std::any_of(completion.begin(), completion.end(),
            [](const auto& item) { return item.label == "twice"; }),
        "completion did not include imported functions");

    const auto definition = service.definition("app.rs", {3, 21});
    require(definition && definition->path == "math.rs",
        "go-to-definition did not resolve the imported function");
    const auto hover = service.hover("app.rs", {3, 21});
    require(hover && hover->contents.find("twice") != std::string::npos,
        "hover did not describe the function");
    const auto references = service.references("app.rs", {3, 21}, true);
    require(references.size() == 2,
        "find-references did not include definition and call");
    const auto edits = service.rename("app.rs", {3, 21}, "doubleValue");
    require(edits.size() == 2 &&
            std::all_of(edits.begin(), edits.end(), [](const auto& edit) {
                return edit.replacement == "doubleValue";
            }),
        "rename did not produce workspace edits");
    require(!service.documentSymbols("math.rs").empty(),
        "document symbols are missing");

    service.open("native_tools.rs", R"(
module Phase4.NativeTools;
delegate void ChangedHandler(int amount);
class Counter
{
    int total;
    event ChangedHandler Changed;
    void Add(int amount) { total = total + amount; }
    int Run() { Changed += Add; Changed(3); return total; }
}
T Identity<T>(T value) { return value; }
int main()
{
    Counter counter = new Counter();
    return Identity<int>(counter.Run());
}
)");
    require(service.diagnostics("native_tools.rs").empty(),
        "native language tooling fixture produced diagnostics");
    const auto nativeSymbols = service.documentSymbols("native_tools.rs");
    require(!nativeSymbols.empty(),
        "native language document symbols are missing");
    require(std::all_of(
            nativeSymbols.begin(), nativeSymbols.end(),
            [](const auto& symbol) {
                return symbol.name.find("$event_") == std::string::npos &&
                    symbol.name.find("$sequence_") == std::string::npos &&
                    symbol.name.find("__int") == std::string::npos;
            }),
        "LSP exposed compiler-owned native language symbols");

    service.update("app.rs", "module Phase4.App; int main() { return missing; }", 2);
    require(!service.diagnostics("app.rs").empty(),
        "language service did not publish semantic diagnostics after an update");
}


void testProtocolAdapters() {
    std::string error;
    const auto parsed = realscript::tooling::Json::parse(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize"})", error);
    require(parsed && parsed->find("method") &&
            parsed->find("method")->stringValue() == "initialize",
        "JSON protocol parser failed: " + error);
    const auto roundTrip = realscript::tooling::Json::parse(parsed->dump(), error);
    require(roundTrip && roundTrip->find("id") &&
            roundTrip->find("id")->integerValue() == 1,
        "JSON protocol round trip failed");
    require(realscript::tooling::Json(
                std::numeric_limits<double>::infinity()).dump() == "null",
        "JSON writer emitted a non-finite number");
    std::string framedBody;
    std::istringstream oversized(
        "Content-Length: 16777217\r\n\r\n");
    require(!realscript::tooling::readProtocolMessage(oversized, framedBody),
        "protocol reader accepted an oversized message");
    std::istringstream duplicateLength(
        "Content-Length: 2\r\nContent-Length: 2\r\n\r\n{}");
    require(!realscript::tooling::readProtocolMessage(duplicateLength, framedBody),
        "protocol reader accepted duplicate Content-Length headers");
    error.clear();
    require(!realscript::tooling::Json::parse(
                R"({"value":1,"value":2})", error),
        "JSON parser accepted duplicate object keys");
    std::string deeplyNested(258, '[');
    deeplyNested += '0';
    deeplyNested.append(258, ']');
    error.clear();
    require(!realscript::tooling::Json::parse(deeplyNested, error),
        "JSON parser accepted excessive nesting");
    error.clear();
    require(!realscript::tooling::Json::parse("\"line\nfeed\"", error),
        "JSON parser accepted an unescaped control character");

    realscript::tooling::LspServer lsp;
    auto initialized = lsp.handle(*parsed);
    require(initialized.find("result") &&
            initialized.find("result")->find("capabilities"),
        "LSP initialize did not advertise capabilities");
    realscript::tooling::Json open = realscript::tooling::Json::Object{
        {"jsonrpc", "2.0"},
        {"method", "textDocument/didOpen"},
        {"params", realscript::tooling::Json::Object{
            {"textDocument", realscript::tooling::Json::Object{
                {"uri", "file://protocol.rs"},
                {"version", 1},
                {"text", "module Phase4.Protocol; int main() { return 1; }"},
            }},
        }},
    };
    std::ostringstream notifications;
    (void)lsp.handle(open, &notifications);
    require(notifications.str().find("publishDiagnostics") != std::string::npos,
        "LSP did not publish diagnostics after opening a document");
    realscript::tooling::Json symbolsRequest = realscript::tooling::Json::Object{
        {"jsonrpc", "2.0"}, {"id", 2},
        {"method", "textDocument/documentSymbol"},
        {"params", realscript::tooling::Json::Object{
            {"textDocument", realscript::tooling::Json::Object{{"uri", "file://protocol.rs"}}},
        }},
    };
    const auto symbolsResponse = lsp.handle(symbolsRequest);
    require(symbolsResponse.find("result") &&
            !symbolsResponse.find("result")->arrayValue().empty(),
        "LSP documentSymbol returned no symbols");

    auto program = link(compile({{"dap.rs", R"(
module Phase4.Dap;
int main()
{
    int value = 3;
    return value + 4;
}
)"}}));
    realscript::debug::DapServer dap(program);
    realscript::tooling::Json initialize = realscript::tooling::Json::Object{
        {"seq", 1}, {"type", "request"}, {"command", "initialize"},
    };
    require(dap.handle(initialize).find("success")->boolValue(),
        "DAP initialize failed");
    realscript::tooling::Json launchRequest = realscript::tooling::Json::Object{
        {"seq", 2}, {"type", "request"}, {"command", "launch"},
        {"arguments", realscript::tooling::Json::Object{
            {"function", "Phase4.Dap::main"}, {"stopOnEntry", true},
        }},
    };
    std::ostringstream events;
    const auto launchResponse = dap.handle(launchRequest, &events);
    require(launchResponse.find("success")->boolValue() &&
            events.str().find("\"event\":\"stopped\"") != std::string::npos,
        "DAP launch did not produce a stopped event");
    realscript::tooling::Json stackRequest = realscript::tooling::Json::Object{
        {"seq", 3}, {"type", "request"}, {"command", "stackTrace"},
        {"arguments", realscript::tooling::Json::Object{{"threadId", 1}}},
    };
    const auto stackResponse = dap.handle(stackRequest);
    require(stackResponse.find("body") &&
            !stackResponse.find("body")->find("stackFrames")->arrayValue().empty(),
        "DAP stackTrace returned no frames");
    realscript::tooling::Json disconnect = realscript::tooling::Json::Object{
        {"seq", 4}, {"type", "request"}, {"command", "disconnect"},
    };
    (void)dap.handle(disconnect);

    auto nativeProgram = link(compile({{"native_dap.rs", R"(
module Phase4.NativeDap;
delegate void ChangedHandler(int amount);
class Counter
{
    int total;
    event ChangedHandler Changed;
    void Add(int amount) { total = total + amount; }
    int Run() { Changed += Add; Changed(3); return total; }
}
T Identity<T>(T value) { return value; }
int main()
{
    Counter counter = new Counter();
    return Identity<int>(counter.Run());
}
)"}}));
    realscript::debug::DapServer nativeDap(nativeProgram);
    require(nativeDap.handle(initialize).find("success")->boolValue(),
        "native DAP initialize failed");
    realscript::tooling::Json nativeLaunch = realscript::tooling::Json::Object{
        {"seq", 5}, {"type", "request"}, {"command", "launch"},
        {"arguments", realscript::tooling::Json::Object{
            {"function", "Phase4.NativeDap::main"}, {"stopOnEntry", true},
        }},
    };
    std::ostringstream nativeEvents;
    require(nativeDap.handle(nativeLaunch, &nativeEvents)
            .find("success")->boolValue(),
        "native DAP launch failed");
    realscript::tooling::Json nativeStackRequest =
        realscript::tooling::Json::Object{
            {"seq", 6}, {"type", "request"},
            {"command", "stackTrace"},
            {"arguments", realscript::tooling::Json::Object{{"threadId", 1}}},
        };
    const auto nativeStack = nativeDap.handle(nativeStackRequest);
    const auto& nativeFrames = nativeStack.find("body")
        ->find("stackFrames")->arrayValue();
    require(!nativeFrames.empty() && nativeFrames.front().find("name"),
        "native DAP stackTrace returned no frame name");
    const auto nativeFrameName =
        nativeFrames.front().find("name")->stringValue();
    require(nativeFrameName.find("main") != std::string::npos &&
            nativeFrameName.find("$event_") == std::string::npos &&
            nativeFrameName.find("__int") == std::string::npos,
        "DAP exposed a compiler-owned native language frame");
    (void)nativeDap.handle(disconnect);
}

void testHotReload() {
    auto initialModules = compile({{"reload.rs", R"(
module Phase4.Reload;
int value() { return 1; }
)"}});
    auto program = link(initialModules);
    realscript::runtime::EngineRuntime runtime(program);
    const auto before = runtime.invoke("Phase4.Reload::value");
    require(before.succeeded && std::get<std::int64_t>(before.value) == 1,
        "initial program returned the wrong result");

    auto replacement = compile({{"reload.rs", R"(
module Phase4.Reload;
int value() { return 2; }
)"}});
    auto plan = realscript::hot_reload::apply(runtime, std::move(replacement));
    require(plan.compatible && plan.changedFunctions.size() == 1,
        "body-only hot reload was rejected");
    const auto after = runtime.invoke("Phase4.Reload::value");
    require(after.succeeded && std::get<std::int64_t>(after.value) == 2,
        "new invocations did not observe the reloaded body");

    auto incompatible = compile({{"reload.rs", R"(
module Phase4.Reload;
long value() { return 2; }
)"}});
    auto rejected = realscript::hot_reload::prepare(
        *runtime.programSnapshot(), std::move(incompatible));
    require(!rejected.compatible && !rejected.issues.empty() &&
            rejected.issues.front().kind ==
                realscript::hot_reload::ReloadIssueKind::FunctionSignatureChanged,
        "signature-changing hot reload was not rejected");

    auto initialType = compile({{"layout.rs", R"(
module Phase4.Layout;
class Item { int value; }
int read(Item item) { return item.value; }
)"}});
    auto typeProgram = link(initialType);
    auto changedType = compile({{"layout.rs", R"(
module Phase4.Layout;
class Item { int value; int extra; }
int read(Item item) { return item.value; }
)"}});
    const auto layoutPlan = realscript::hot_reload::prepare(
        *typeProgram, std::move(changedType));
    require(!layoutPlan.compatible && std::any_of(
            layoutPlan.issues.begin(), layoutPlan.issues.end(), [](const auto& issue) {
                return issue.kind == realscript::hot_reload::ReloadIssueKind::TypeLayoutChanged;
            }),
        "type-layout-changing hot reload was not rejected");

    auto metadataInitial = compile({{"metadata_reload.rs", R"(
module Phase4.MetadataReload;
[Replicated(channel = "state")]
class Item { int value; }
int read(Item item) { return item.value; }
)"}});
    auto metadataProgram = link(metadataInitial);
    auto metadataChanged = compile({{"metadata_reload.rs", R"(
module Phase4.MetadataReload;
[Replicated(channel = "effects")]
class Item { int value; }
int read(Item item) { return item.value; }
)"}});
    const auto metadataPlan = realscript::hot_reload::prepare(
        *metadataProgram, std::move(metadataChanged));
    require(!metadataPlan.compatible && std::any_of(
            metadataPlan.issues.begin(), metadataPlan.issues.end(),
            [](const auto& issue) {
                return issue.kind ==
                    realscript::hot_reload::ReloadIssueKind::LanguageMetadataChanged;
            }),
        "language-metadata-changing hot reload was not rejected");
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, void(*)()>> tests = {
        {"Debug metadata round trip", testDebugMetadataRoundTrip},
        {"Debug verifier corruption", testDebugVerifierRejectsCorruption},
        {"Debugger session", testDebuggerSession},
        {"Debugger termination", testDebuggerTermination},
        {"Language service", testLanguageService},
        {"Protocol adapters", testProtocolAdapters},
        {"Hot reload", testHotReload},
    };
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
            return 1;
        }
    }
    return 0;
}
