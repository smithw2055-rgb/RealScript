#include "realscript/debug/DapServer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace realscript::debug {
namespace {

const tooling::Json* nested(
    const tooling::Json& value,
    const std::initializer_list<const char*>& keys) {
    const tooling::Json* current = &value;
    for (const auto* key : keys) current = current ? current->find(key) : nullptr;
    return current;
}

ResumeMode commandMode(const std::string& command) {
    if (command == "next") return ResumeMode::StepOver;
    if (command == "stepIn") return ResumeMode::StepIn;
    if (command == "stepOut") return ResumeMode::StepOut;
    return ResumeMode::Continue;
}

} // namespace

DapServer::DapServer(
    std::shared_ptr<const runtime::ProgramImage> program,
    std::shared_ptr<runtime::ManagedHeap> heap)
    : program_(std::move(program)),
      heap_(heap ? std::move(heap) : std::make_shared<runtime::ManagedHeap>()),
      session_(std::make_unique<DebugSession>(program_, heap_)) {}

tooling::Json DapServer::response(
    const tooling::Json& request,
    bool success,
    tooling::Json body,
    std::string message) {
    tooling::Json::Object result{
        {"seq", static_cast<std::int64_t>(sequence_++)},
        {"type", "response"},
        {"request_seq", request.find("seq") ? request.find("seq")->integerValue() : 0},
        {"success", success},
        {"command", request.find("command") ? request.find("command")->stringValue() : ""},
        {"body", std::move(body)},
    };
    if (!message.empty()) result.emplace("message", std::move(message));
    return result;
}


void DapServer::rebuildVariableReferences() {
    variables_.clear();
    const auto stop = session_->currentStop();
    if (!stop) return;
    for (const auto& frame : stop->frames) {
        variables_[static_cast<std::int64_t>(frame.id) * 2] = frame.arguments;
        variables_[static_cast<std::int64_t>(frame.id) * 2 + 1] = frame.locals;
    }
}

void DapServer::emitStopped(std::ostream& output) {
    rebuildVariableReferences();
    const auto stop = session_->currentStop();
    if (!stop) return;
    tooling::writeProtocolMessage(output, tooling::Json::Object{
        {"seq", static_cast<std::int64_t>(sequence_++)},
        {"type", "event"},
        {"event", "stopped"},
        {"body", tooling::Json::Object{
            {"reason", stopReasonName(stop->reason)},
            {"threadId", 1},
            {"allThreadsStopped", true},
            {"hitBreakpointIds", stop->breakpointId == 0
                ? tooling::Json(tooling::Json::Array{})
                : tooling::Json(tooling::Json::Array{static_cast<std::int64_t>(stop->breakpointId)})},
        }},
    });
}

void DapServer::emitTerminated(std::ostream& output) {
    tooling::writeProtocolMessage(output, tooling::Json::Object{
        {"seq", static_cast<std::int64_t>(sequence_++)},
        {"type", "event"},
        {"event", "terminated"},
        {"body", tooling::Json::Object{}},
    });
}

tooling::Json DapServer::handle(
    const tooling::Json& request,
    std::ostream* events) {
    const auto command = request.find("command")
        ? request.find("command")->stringValue() : std::string{};
    const auto* arguments = request.find("arguments");

    if (command == "initialize") {
        if (events) {
            tooling::writeProtocolMessage(*events, tooling::Json::Object{
                {"seq", static_cast<std::int64_t>(sequence_++)},
                {"type", "event"},
                {"event", "initialized"},
                {"body", tooling::Json::Object{}},
            });
        }
        return response(request, true, tooling::Json::Object{
            {"supportsConfigurationDoneRequest", true},
            {"supportsTerminateRequest", true},
            {"supportsSetVariable", false},
            {"supportsEvaluateForHovers", false},
        });
    }
    if (command == "setBreakpoints") {
        const auto path = nested(request, {"arguments", "source", "path"})
            ? nested(request, {"arguments", "source", "path"})->stringValue() : std::string{};
        std::vector<text::LinePosition> positions;
        if (const auto* values = arguments ? arguments->find("breakpoints") : nullptr) {
            for (const auto& value : values->arrayValue()) {
                const auto line = value.find("line")
                    ? value.find("line")->integerValue(1) : 1;
                const auto column = value.find("column")
                    ? value.find("column")->integerValue(1) : 1;
                positions.push_back({
                    static_cast<std::size_t>(std::max<std::int64_t>(1, line) - 1),
                    static_cast<std::size_t>(std::max<std::int64_t>(1, column) - 1),
                });
            }
        }
        tooling::Json::Array breakpoints;
        for (const auto& breakpoint : session_->controller()->setBreakpoints(path, positions)) {
            if (breakpoint.sourcePath != path) continue;
            breakpoints.emplace_back(tooling::Json::Object{
                {"id", static_cast<std::int64_t>(breakpoint.id)},
                {"verified", breakpoint.verified},
                {"line", static_cast<std::int64_t>(breakpoint.line + 1)},
                {"column", static_cast<std::int64_t>(breakpoint.column + 1)},
            });
        }
        return response(request, true, tooling::Json::Object{{"breakpoints", tooling::Json(std::move(breakpoints))}});
    }
    if (command == "launch") {
        const auto function = arguments && arguments->find("function")
            ? arguments->find("function")->stringValue() : std::string{};
        const auto stopOnEntry = arguments && arguments->find("stopOnEntry")
            ? arguments->find("stopOnEntry")->boolValue(false) : false;
        std::vector<runtime::Value> launchArguments;
        if (const auto* values = arguments ? arguments->find("args") : nullptr) {
            for (const auto& value : values->arrayValue()) {
                if (value.isBool()) launchArguments.emplace_back(value.boolValue());
                else if (value.isNumber()) {
                    const auto number = value.numberValue();
                    if (std::isfinite(number) && std::floor(number) == number) {
                        launchArguments.emplace_back(value.integerValue());
                    } else {
                        launchArguments.emplace_back(number);
                    }
                }
                else if (value.isString()) launchArguments.emplace_back(value.stringValue());
                else launchArguments.emplace_back(std::monostate{});
            }
        }
        if (!session_->launch(function, std::move(launchArguments), stopOnEntry)) {
            return response(request, false, nullptr, "debug session is already running or has no program");
        }
        const auto result = response(request, true, tooling::Json::Object{});
        if (events) {
            if (session_->waitForStop(std::chrono::seconds{10})) emitStopped(*events);
            else if (session_->finished()) emitTerminated(*events);
        }
        return result;
    }
    if (command == "configurationDone") return response(request, true, tooling::Json::Object{});
    if (command == "threads") {
        return response(request, true, tooling::Json::Object{{"threads", tooling::Json::Array{
            tooling::Json::Object{{"id", 1}, {"name", "RealScript main thread"}},
        }}});
    }
    if (command == "stackTrace") {
        tooling::Json::Array frames;
        const auto stop = session_->currentStop();
        if (stop) {
            for (const auto& frame : stop->frames) {
                const auto path = frame.sourcePath;
                frames.emplace_back(tooling::Json::Object{
                    {"id", static_cast<std::int64_t>(frame.id)},
                    {"name", frame.function},
                    {"line", static_cast<std::int64_t>(frame.location.start.line + 1)},
                    {"column", static_cast<std::int64_t>(frame.location.start.column + 1)},
                    {"source", tooling::Json::Object{{"name", path}, {"path", path}}},
                });
            }
        }
        return response(request, true, tooling::Json::Object{
            {"stackFrames", tooling::Json(std::move(frames))},
            {"totalFrames", stop ? static_cast<std::int64_t>(stop->frames.size()) : 0},
        });
    }
    if (command == "scopes") {
        const auto frameId = arguments && arguments->find("frameId")
            ? arguments->find("frameId")->integerValue() : 0;
        return response(request, true, tooling::Json::Object{{"scopes", tooling::Json::Array{
            tooling::Json::Object{{"name", "Arguments"}, {"variablesReference", frameId * 2}, {"expensive", false}},
            tooling::Json::Object{{"name", "Locals"}, {"variablesReference", frameId * 2 + 1}, {"expensive", false}},
        }}});
    }
    if (command == "variables") {
        const auto reference = arguments && arguments->find("variablesReference")
            ? arguments->find("variablesReference")->integerValue() : 0;
        tooling::Json::Array values;
        const auto found = variables_.find(reference);
        if (found != variables_.end()) {
            for (const auto& variable : found->second) {
                values.emplace_back(tooling::Json::Object{
                    {"name", variable.name},
                    {"value", runtime::valueToString(variable.value, heap_.get())},
                    {"type", semantic::primitiveTypeName(variable.type)},
                    {"variablesReference", 0},
                });
            }
        }
        return response(request, true, tooling::Json::Object{{"variables", tooling::Json(std::move(values))}});
    }
    if (command == "continue" || command == "next" ||
        command == "stepIn" || command == "stepOut") {
        session_->resume(commandMode(command));
        const auto result = response(request, true, tooling::Json::Object{{"allThreadsContinued", true}});
        if (events) {
            if (session_->waitForStop(std::chrono::seconds{10})) emitStopped(*events);
            else if (session_->finished()) emitTerminated(*events);
        }
        return result;
    }
    if (command == "pause") {
        session_->pause();
        const auto result = response(request, true, tooling::Json::Object{});
        if (events && session_->waitForStop(std::chrono::seconds{10})) {
            emitStopped(*events);
        }
        return result;
    }
    if (command == "disconnect" || command == "terminate") {
        session_->terminate();
        session_->join();
        if (events) emitTerminated(*events);
        return response(request, true, tooling::Json::Object{});
    }
    return response(request, false, nullptr, "unsupported DAP command: " + command);
}

int DapServer::run(std::istream& input, std::ostream& output) {
    std::mutex outputMutex;
    std::thread monitor;
    std::atomic<bool> suppressMonitor{false};

    const auto joinMonitor = [&] {
        if (monitor.joinable()) monitor.join();
    };
    const auto startMonitor = [&] {
        joinMonitor();
        suppressMonitor.store(false);
        monitor = std::thread([&] {
            const auto stopped = session_->waitForStop();
            if (suppressMonitor.load()) return;
            if (!stopped) session_->join();
            std::lock_guard<std::mutex> lock(outputMutex);
            if (stopped) emitStopped(output);
            else emitTerminated(output);
        });
    };

    std::string body;
    while (tooling::readProtocolMessage(input, body)) {
        std::string error;
        auto request = tooling::Json::parse(body, error);
        if (!request) continue;
        const auto* commandValue = request->find("command");
        const auto command = commandValue ? commandValue->stringValue() : std::string{};
        const auto asynchronous = command == "launch" || command == "continue" ||
            command == "next" || command == "stepIn" ||
            command == "stepOut" || command == "pause";
        const auto terminal = command == "disconnect" || command == "terminate";

        if (terminal) suppressMonitor.store(true);
        std::ostringstream events;
        const auto responseValue = handle(*request, asynchronous ? nullptr : &events);
        if (terminal) joinMonitor();
        {
            std::lock_guard<std::mutex> lock(outputMutex);
            tooling::writeProtocolMessage(output, responseValue);
            output << events.str();
            if (terminal) emitTerminated(output);
            output.flush();
        }
        const auto succeeded = responseValue.find("success") &&
            responseValue.find("success")->boolValue();
        if (asynchronous && succeeded) startMonitor();
        if (terminal) break;
    }

    if (monitor.joinable()) {
        suppressMonitor.store(true);
        session_->terminate();
        session_->join();
        joinMonitor();
    }
    return 0;
}

} // namespace realscript::debug
