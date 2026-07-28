#include "realscript/game/GameScripting.h"

#include <utility>

namespace realscript::game {
namespace {

runtime::ExecutionResult failedResult(
    runtime::ErrorCode code,
    std::string message) {
    runtime::ExecutionResult result;
    result.error.code = code;
    result.error.message = std::move(message);
    return result;
}

} // namespace

runtime::ExecutionResult SceneScriptRuntime::invoke(
    SceneEntityId entity,
    const std::string& callback,
    const std::vector<runtime::Value>& arguments) {
    const auto found = instances_.find(entity);
    if (found == instances_.end()) {
        return failedResult(
            runtime::ErrorCode::InvalidArguments,
            "scene entity does not have a script instance");
    }
    auto method = runtime_.findMethod(
        found->second.object.type(), callback, arguments.size());
    if (!method) {
        return failedResult(
            runtime::ErrorCode::FunctionNotFound,
            "script callback '" + callback + "' was not found");
    }
    auto result = runtime_.invoke(
        found->second.object, *method, arguments, executionOptions_);
    if (!result.succeeded) recordError(entity, callback, result.error);
    return result;
}

bool SceneScriptRuntime::dispatch(const ScriptEvent& event) {
    if (event.target != BroadcastTarget) {
        return invoke(event.target, event.callback, event.arguments).succeeded;
    }
    bool succeeded = true;
    for (auto& [entity, instance] : instances_) {
        if (!instance.enabled) continue;
        auto method = runtime_.findMethod(
            instance.object.type(), event.callback, event.arguments.size());
        if (!method) continue;
        auto result = runtime_.invoke(
            instance.object, *method, event.arguments, executionOptions_);
        if (!result.succeeded) {
            recordError(entity, event.callback, result.error);
            succeeded = false;
        }
    }
    return succeeded;
}

void SceneScriptRuntime::enqueue(ScriptEvent event) {
    eventQueue_.push_back(std::move(event));
}

std::size_t SceneScriptRuntime::flushEvents() {
    std::vector<ScriptEvent> events;
    events.swap(eventQueue_);
    std::size_t delivered = 0;
    for (const auto& event : events) {
        if (dispatch(event)) ++delivered;
    }
    return delivered;
}

bool SceneScriptRuntime::addTrigger(ScriptTrigger trigger) {
    if (trigger.id.empty() || !trigger.condition) return false;
    return triggers_.emplace(trigger.id, std::move(trigger)).second;
}

bool SceneScriptRuntime::removeTrigger(const std::string& id) {
    return triggers_.erase(id) != 0;
}

std::size_t SceneScriptRuntime::evaluateTriggers() {
    std::size_t fired = 0;
    for (auto& [id, trigger] : triggers_) {
        (void)id;
        if (!trigger.enabled || (trigger.once && trigger.fired) ||
            !trigger.condition()) {
            continue;
        }
        if (dispatch({trigger.target, trigger.callback, trigger.arguments})) {
            trigger.fired = true;
            ++fired;
        }
    }
    return fired;
}

ScriptObject* SceneScriptRuntime::object(SceneEntityId entity) {
    const auto found = instances_.find(entity);
    return found == instances_.end() ? nullptr : &found->second.object;
}

bool SceneScriptRuntime::invokeLifecycle(
    SceneEntityId entity,
    Instance& instance,
    const std::optional<ScriptMethod>& method,
    const std::vector<runtime::Value>& arguments,
    const char* callback) {
    if (!method) return true;
    auto result = runtime_.invoke(
        instance.object, *method, arguments, executionOptions_);
    if (result.succeeded) return true;
    recordError(entity, callback, result.error);
    return false;
}

void SceneScriptRuntime::recordError(
    SceneEntityId entity,
    std::string callback,
    const runtime::RuntimeError& error) {
    errors_.push_back({entity, std::move(callback), error});
}

} // namespace realscript::game
