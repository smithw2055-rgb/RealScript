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


std::int64_t DapServer::registerExpandableValue(const runtime::Value& value) {
    if (const auto* objRef = std::get_if<runtime::ObjectRef>(&value)) {
        if (!heap_ || !objRef->valid() || !heap_->isAlive(*objRef)) return 0;
        if (objRef->kind == runtime::ObjectKind::String) return 0;
        if (objRef->kind == runtime::ObjectKind::Array) {
            const auto len = heap_->arrayLength(*objRef).value_or(0);
            const auto elemType = heap_->arrayElementType(*objRef).value_or(semantic::PrimitiveType::Error);
            const auto elemTypeId = heap_->arrayElementTypeId(*objRef).value_or(0);
            std::vector<DebugVariable> children;
            children.reserve(std::min<std::size_t>(len, 256));
            for (std::size_t i = 0; i < std::min<std::size_t>(len, 256); ++i) {
                if (const auto elem = heap_->arrayGet(*objRef, i)) {
                    children.push_back(DebugVariable{
                        "[" + std::to_string(i) + "]",
                        *elem,
                        elemType != semantic::PrimitiveType::Error ? elemType : runtime::valueType(*elem),
                        elemTypeId,
                    });
                }
            }
            const auto refId = nextChildRef_++;
            variables_[refId] = std::move(children);
            childOwnerObjects_[refId] = *objRef;
            return refId;
        }
        const auto typeId = heap_->objectTypeId(*objRef).value_or(0);
        const auto* typeDesc = (program_ && typeId != 0) ? program_->resolveType(typeId) : nullptr;
        const auto fCount = heap_->fieldCount(*objRef).value_or(0);
        std::vector<DebugVariable> children;
        for (std::size_t i = 0; i < fCount; ++i) {
            if (typeDesc && i < typeDesc->fields.size()) {
                if (typeDesc->fields[i].staticField ||
                    typeDesc->fields[i].constField ||
                    typeDesc->fields[i].synthetic) {
                    continue;
                }
            }
            const auto fVal = heap_->fieldGet(*objRef, i);
            if (!fVal) continue;
            std::string fName = "field_" + std::to_string(i);
            semantic::PrimitiveType fType = runtime::valueType(*fVal);
            if (typeDesc && i < typeDesc->fields.size()) {
                fName = typeDesc->fields[i].name;
                fType = typeDesc->fields[i].type;
            }
            children.push_back(DebugVariable{std::move(fName), *fVal, fType, 0});
        }
        if (children.empty()) return 0;
        const auto refId = nextChildRef_++;
        variables_[refId] = std::move(children);
        childOwnerObjects_[refId] = *objRef;
        return refId;
    }
    if (const auto* stVal = std::get_if<runtime::StructValue>(&value)) {
        if (!stVal->valid() || stVal->fieldCount() == 0) return 0;
        const auto* typeDesc = (program_ && stVal->typeId != 0) ? program_->resolveType(stVal->typeId) : nullptr;
        std::vector<DebugVariable> children;
        for (std::size_t i = 0; i < stVal->fieldCount(); ++i) {
            if (typeDesc && i < typeDesc->fields.size()) {
                if (typeDesc->fields[i].staticField ||
                    typeDesc->fields[i].constField ||
                    typeDesc->fields[i].synthetic) {
                    continue;
                }
            }
            const auto fVal = runtime::structValueFieldAt(*stVal, i);
            if (!fVal) continue;
            std::string fName = "field_" + std::to_string(i);
            semantic::PrimitiveType fType = runtime::valueType(*fVal);
            if (typeDesc && i < typeDesc->fields.size()) {
                fName = typeDesc->fields[i].name;
                fType = typeDesc->fields[i].type;
            }
            children.push_back(DebugVariable{std::move(fName), *fVal, fType, 0});
        }
        if (children.empty()) return 0;
        const auto refId = nextChildRef_++;
        variables_[refId] = std::move(children);
        return refId;
    }
    return 0;
}

void DapServer::rebuildVariableReferences() {
    variables_.clear();
    childOwnerObjects_.clear();
    nextChildRef_ = 1000;
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
            {"supportsSetVariable", true},
            {"supportsConditionalBreakpoints", true},
            {"supportsEvaluateForHovers", true},
        });
    }
    if (command == "setBreakpoints") {
        const auto path = nested(request, {"arguments", "source", "path"})
            ? nested(request, {"arguments", "source", "path"})->stringValue() : std::string{};
        std::vector<BreakpointSpec> specs;
        if (const auto* values = arguments ? arguments->find("breakpoints") : nullptr) {
            for (const auto& value : values->arrayValue()) {
                const auto line = value.find("line")
                    ? value.find("line")->integerValue(1) : 1;
                const auto column = value.find("column")
                    ? value.find("column")->integerValue(1) : 1;
                const auto condition = value.find("condition")
                    ? value.find("condition")->stringValue() : std::string{};
                specs.push_back(BreakpointSpec{
                    {
                        static_cast<std::size_t>(std::max<std::int64_t>(1, line) - 1),
                        static_cast<std::size_t>(std::max<std::int64_t>(1, column) - 1),
                    },
                    condition,
                });
            }
        }
        tooling::Json::Array breakpoints;
        for (const auto& breakpoint : session_->controller()->setConditionalBreakpoints(path, specs)) {
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
    if (command == "evaluate") {
        const auto expression = arguments && arguments->find("expression")
            ? arguments->find("expression")->stringValue() : std::string{};
        const auto frameId = arguments && arguments->find("frameId")
            ? static_cast<std::uint32_t>(std::max<std::int64_t>(1, arguments->find("frameId")->integerValue(1)))
            : 1u;
        std::string errorMessage;
        const auto evaluated = session_->evaluate(expression, frameId, &errorMessage);
        if (!evaluated) {
            return response(request, false, nullptr,
                errorMessage.empty() ? "failed to evaluate expression" : errorMessage);
        }
        return response(request, true, tooling::Json::Object{
            {"result", runtime::valueToString(*evaluated, heap_.get())},
            {"type", semantic::primitiveTypeName(runtime::valueType(*evaluated))},
            {"variablesReference", 0},
        });
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
            const auto listCopy = found->second;
            for (const auto& variable : listCopy) {
                const auto childRef = registerExpandableValue(variable.value);
                values.emplace_back(tooling::Json::Object{
                    {"name", variable.name},
                    {"value", runtime::valueToString(variable.value, heap_.get())},
                    {"type", semantic::primitiveTypeName(variable.type)},
                    {"variablesReference", childRef},
                });
            }
        }
        return response(request, true, tooling::Json::Object{{"variables", tooling::Json(std::move(values))}});
    }
    if (command == "setVariable") {
        const auto reference = arguments && arguments->find("variablesReference")
            ? arguments->find("variablesReference")->integerValue() : 2;
        const auto name = arguments && arguments->find("name")
            ? arguments->find("name")->stringValue() : std::string{};
        const auto valExpr = arguments && arguments->find("value")
            ? arguments->find("value")->stringValue() : std::string{};
        if (reference >= 1000) {
            const auto ownerIt = childOwnerObjects_.find(reference);
            if (ownerIt == childOwnerObjects_.end() || !heap_ || !heap_->isAlive(ownerIt->second)) {
                return response(request, false, nullptr,
                    "cannot mutate non-heap child variable reference");
            }
            std::string errMsg;
            const auto evaluated = session_->evaluate(valExpr, 1u, &errMsg);
            if (!evaluated) {
                return response(request, false, nullptr,
                    errMsg.empty() ? "failed to evaluate new value" : errMsg);
            }
            const auto ownerRef = ownerIt->second;
            runtime::RuntimeError rtErr;
            if (ownerRef.kind == runtime::ObjectKind::Array) {
                if (name.size() < 3 || name.front() != '[' || name.back() != ']') {
                    return response(request, false, nullptr, "invalid array element name: " + name);
                }
                const auto idx = static_cast<std::size_t>(
                    std::strtoull(name.substr(1, name.size() - 2).c_str(), nullptr, 10));
                if (!heap_->arraySet(ownerRef, idx, *evaluated, &rtErr)) {
                    return response(request, false, nullptr, rtErr.message);
                }
            } else {
                const auto typeId = heap_->objectTypeId(ownerRef).value_or(0);
                const auto* typeDesc = (program_ && typeId != 0) ? program_->resolveType(typeId) : nullptr;
                std::size_t targetIdx = std::numeric_limits<std::size_t>::max();
                if (typeDesc) {
                    for (std::size_t i = 0; i < typeDesc->fields.size(); ++i) {
                        const auto& fld = typeDesc->fields[i];
                        if (fld.staticField || fld.constField) continue;
                        if (fld.name == name) {
                            targetIdx = i;
                            break;
                        }
                    }
                }
                if (targetIdx == std::numeric_limits<std::size_t>::max() ||
                    !heap_->fieldSet(ownerRef, targetIdx, *evaluated, &rtErr)) {
                    return response(request, false, nullptr,
                        rtErr.message.empty() ? ("unknown field: " + name) : rtErr.message);
                }
            }
            if (auto it = variables_.find(reference); it != variables_.end()) {
                for (auto& childVar : it->second) {
                    if (childVar.name == name) {
                        childVar.value = *evaluated;
                        break;
                    }
                }
            }
            if (const auto stop = session_->currentStop()) {
                for (const auto& frame : stop->frames) {
                    variables_[static_cast<std::int64_t>(frame.id) * 2] = frame.arguments;
                    variables_[static_cast<std::int64_t>(frame.id) * 2 + 1] = frame.locals;
                }
            }
            const auto childRef = registerExpandableValue(*evaluated);
            return response(request, true, tooling::Json::Object{
                {"value", runtime::valueToString(*evaluated, heap_.get())},
                {"type", semantic::primitiveTypeName(runtime::valueType(*evaluated))},
                {"variablesReference", childRef},
            });
        }
        const auto frameId = static_cast<std::uint32_t>(std::max<std::int64_t>(1, reference / 2));
        runtime::Value updatedVal{};
        std::string errMsg;
        if (!session_->setVariable(name, valExpr, frameId, &updatedVal, &errMsg)) {
            return response(request, false, nullptr,
                errMsg.empty() ? "failed to set variable" : errMsg);
        }
        rebuildVariableReferences();
        const auto childRef = registerExpandableValue(updatedVal);
        return response(request, true, tooling::Json::Object{
            {"value", runtime::valueToString(updatedVal, heap_.get())},
            {"type", semantic::primitiveTypeName(runtime::valueType(updatedVal))},
            {"variablesReference", childRef},
        });
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
