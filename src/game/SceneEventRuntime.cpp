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

const ScriptMethod* SceneScriptRuntime::findCachedMethod(
    const ScriptType& type,
    const std::string& name,
    std::size_t arity) {
    auto hash = std::hash<std::string>{}(name);
    hash ^= static_cast<std::size_t>(type.descriptor.id) +
        0x9e3779b9u + (hash << 6u) + (hash >> 2u);
    hash ^= arity + 0x9e3779b9u + (hash << 6u) + (hash >> 2u);
    auto& bucket = methodCache_[hash];
    for (auto& entry : bucket) {
        if (entry.typeId == type.descriptor.id && entry.arity == arity &&
            entry.name == name) {
            return entry.method ? &*entry.method : nullptr;
        }
    }
    bucket.push_back(MethodCacheEntry{
        type.descriptor.id,
        name,
        arity,
        runtime_.findMethod(type, name, arity)});
    auto& method = bucket.back().method;
    return method ? &*method : nullptr;
}

runtime::ExecutionResult SceneScriptRuntime::invoke(
    SceneEntityId entity,
    const std::string& callback,
    const std::vector<runtime::Value>& arguments) {
    const auto found = instances_.find(entity);
    if (found == instances_.end() || found->second.empty()) {
        return failedResult(
            runtime::ErrorCode::InvalidArguments,
            "scene entity does not have a script instance");
    }
    for (auto& instance : found->second) {
        const auto* method = findCachedMethod(
            instance.object.type(), callback, arguments.size());
        if (!method) continue;
        auto result = runtime_.invoke(
            instance.object, *method, arguments, executionOptions_);
        if (!result.succeeded) recordError(entity, callback, result.error);
        return result;
    }
    return failedResult(
        runtime::ErrorCode::FunctionNotFound,
        "script callback '" + callback + "' was not found");
}

runtime::ExecutionResult SceneScriptRuntime::invokeComponent(
    SceneEntityId entity,
    const std::string& scriptType,
    const std::string& callback,
    const std::vector<runtime::Value>& arguments) {
    const auto found = instances_.find(entity);
    if (found == instances_.end() || found->second.empty()) {
        return failedResult(
            runtime::ErrorCode::InvalidArguments,
            "scene entity does not have a script instance");
    }
    for (auto& instance : found->second) {
        if (!matchesComponentType(instance.object.type(), scriptType)) continue;
        const auto* method = findCachedMethod(
            instance.object.type(), callback, arguments.size());
        if (!method) {
            return failedResult(
                runtime::ErrorCode::FunctionNotFound,
                "script callback '" + callback + "' was not found");
        }
        auto result = runtime_.invoke(
            instance.object, *method, arguments, executionOptions_);
        if (!result.succeeded) recordError(entity, callback, result.error);
        return result;
    }
    return failedResult(
        runtime::ErrorCode::FunctionNotFound,
        "component '" + scriptType + "' was not found on entity");
}

bool SceneScriptRuntime::dispatch(const ScriptEvent& event) {
    if (event.target != BroadcastTarget) {
        const auto found = instances_.find(event.target);
        if (found == instances_.end() || found->second.empty()) return false;
        bool matchedAny = false;
        bool allSucceeded = true;
        for (auto& instance : found->second) {
            const auto* method = findCachedMethod(
                instance.object.type(), event.callback, event.arguments.size());
            if (!method) continue;
            matchedAny = true;
            auto result = runtime_.invoke(
                instance.object, *method, event.arguments, executionOptions_);
            if (!result.succeeded) {
                recordError(event.target, event.callback, result.error);
                allSucceeded = false;
            }
        }
        return matchedAny && allSucceeded;
    }
    bool succeeded = true;
    for (auto& [entity, components] : instances_) {
        for (auto& instance : components) {
            if (!instance.enabled) continue;
            const auto* method = findCachedMethod(
                instance.object.type(), event.callback, event.arguments.size());
            if (!method) continue;
            auto result = runtime_.invoke(
                instance.object, *method, event.arguments, executionOptions_);
            if (!result.succeeded) {
                recordError(entity, event.callback, result.error);
                succeeded = false;
            }
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
    return (found == instances_.end() || found->second.empty())
        ? nullptr
        : &found->second.front().object;
}

ScriptObject* SceneScriptRuntime::getComponent(
    SceneEntityId entity,
    const std::string& typeOrInterfaceName) {
    const auto found = instances_.find(entity);
    if (found == instances_.end()) return nullptr;
    for (auto& instance : found->second) {
        if (matchesComponentType(instance.object.type(), typeOrInterfaceName)) {
            return &instance.object;
        }
    }
    return nullptr;
}

std::vector<ScriptObject*> SceneScriptRuntime::getComponents(SceneEntityId entity) {
    std::vector<ScriptObject*> result;
    const auto found = instances_.find(entity);
    if (found == instances_.end()) return result;
    result.reserve(found->second.size());
    for (auto& instance : found->second) {
        result.push_back(&instance.object);
    }
    return result;
}

bool SceneScriptRuntime::hasComponent(
    SceneEntityId entity,
    const std::string& typeOrInterfaceName) const {
    const auto found = instances_.find(entity);
    if (found == instances_.end()) return false;
    for (const auto& instance : found->second) {
        if (matchesComponentType(instance.object.type(), typeOrInterfaceName)) {
            return true;
        }
    }
    return false;
}

std::size_t SceneScriptRuntime::componentCount(
    SceneEntityId entity) const noexcept {
    const auto found = instances_.find(entity);
    return found == instances_.end() ? 0 : found->second.size();
}

bool installSceneComponentBindings(
    GameApi& api,
    SceneScriptRuntime& scene,
    std::string moduleName) {
    SceneScriptRuntime* scenePtr = &scene;
    const bool ok =
        api.function(
            moduleName,
            "HasComponent",
            [scenePtr](std::int64_t entityId, std::string typeName) -> bool {
                if (entityId <= 0) return false;
                return scenePtr->hasComponent(
                    static_cast<SceneEntityId>(entityId), typeName);
            }) &&
        api.function(
            moduleName,
            "GetComponentCount",
            [scenePtr](std::int64_t entityId) -> std::int32_t {
                if (entityId <= 0) return 0;
                return static_cast<std::int32_t>(scenePtr->componentCount(
                    static_cast<SceneEntityId>(entityId)));
            }) &&
        api.function(
            moduleName,
            "SetComponentEnabled",
            [scenePtr](std::int64_t entityId, std::string typeName, bool enabled) -> bool {
                if (entityId <= 0) return false;
                return scenePtr->setComponentEnabled(
                    static_cast<SceneEntityId>(entityId), typeName, enabled);
            }) &&
        api.function(
            moduleName,
            "SendComponentEvent",
            [scenePtr](std::int64_t entityId, std::string callback) -> bool {
                if (entityId <= 0) return false;
                return scenePtr->dispatch(
                    ScriptEvent{static_cast<SceneEntityId>(entityId), std::move(callback), {}});
            }) &&
        api.function(
            moduleName,
            "SendComponentEventInt",
            [scenePtr](
                std::int64_t entityId,
                std::string callback,
                std::int32_t payload) -> bool {
                if (entityId <= 0) return false;
                return scenePtr->dispatch(ScriptEvent{
                    static_cast<SceneEntityId>(entityId),
                    std::move(callback),
                    {static_cast<std::int64_t>(payload)}});
            });
    return ok && api.valid();
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
