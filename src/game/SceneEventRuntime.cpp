#include "realscript/game/GameScripting.h"

#include <set>
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

const ScriptObject* SceneScriptRuntime::getComponent(
    SceneEntityId entity,
    const std::string& typeOrInterfaceName) const {
    const auto found = instances_.find(entity);
    if (found == instances_.end()) return nullptr;
    for (const auto& instance : found->second) {
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

std::vector<const ScriptObject*> SceneScriptRuntime::getComponents(SceneEntityId entity) const {
    std::vector<const ScriptObject*> result;
    const auto found = instances_.find(entity);
    if (found == instances_.end()) return result;
    result.reserve(found->second.size());
    for (const auto& instance : found->second) {
        result.push_back(&instance.object);
    }
    return result;
}

std::vector<SceneEntityId> SceneScriptRuntime::entities() const {
    std::set<SceneEntityId> allIds;
    for (const auto& [ent, _] : instances_) {
        (void)_;
        if (ent != BroadcastTarget) allIds.insert(ent);
    }
    for (const auto& [ent, _] : entityActiveSelf_) {
        (void)_;
        if (ent != BroadcastTarget) allIds.insert(ent);
    }
    for (const auto& [ent, _] : localTransforms_) {
        (void)_;
        if (ent != BroadcastTarget) allIds.insert(ent);
    }
    for (const auto& [child, parent] : parentByEntity_) {
        if (child != BroadcastTarget) allIds.insert(child);
        if (parent != BroadcastTarget) allIds.insert(parent);
    }
    return std::vector<SceneEntityId>(allIds.begin(), allIds.end());
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

void SceneFiberHostAdapter::registerCommandCallback(
    std::uint8_t commandId,
    std::string callbackName,
    std::string componentType) {
    commands_[commandId] = CommandEntry{std::move(callbackName), std::move(componentType)};
}

void SceneFiberHostAdapter::registerConditionMember(
    std::uint16_t key16,
    std::string memberName,
    std::string componentType) {
    conditions_[key16] = ConditionEntry{std::move(memberName), std::move(componentType)};
}

void SceneFiberHostAdapter::onHostCommand(
    runtime::FiberContext& fiber,
    std::uint8_t commandId,
    std::uint16_t arg16,
    std::uint32_t arg32) noexcept {
    const auto it = commands_.find(commandId);
    if (it == commands_.end()) return;
    const auto entityId = static_cast<SceneEntityId>(fiber.boundEntity.slot);
    if (entityId == SceneScriptRuntime::BroadcastTarget) return;

    const std::vector<runtime::Value> args2{
        static_cast<std::int64_t>(arg16),
        static_cast<std::int64_t>(static_cast<std::int32_t>(arg32)),
    };
    const std::vector<runtime::Value> args1{
        static_cast<std::int64_t>(static_cast<std::int32_t>(arg32)),
    };

    if (!it->second.componentType.empty()) {
        auto* comp = scene_.getComponent(entityId, it->second.componentType);
        if (!comp) return;
        if (scene_.runtime().findMethod(comp->type(), it->second.callbackName, 2)) {
            (void)scene_.invokeComponent(
                entityId, it->second.componentType, it->second.callbackName, args2);
        } else if (scene_.runtime().findMethod(comp->type(), it->second.callbackName, 1)) {
            (void)scene_.invokeComponent(
                entityId, it->second.componentType, it->second.callbackName, args1);
        } else {
            (void)scene_.invokeComponent(
                entityId, it->second.componentType, it->second.callbackName, {});
        }
    } else {
        auto* comp = scene_.object(entityId);
        if (!comp) return;
        if (scene_.runtime().findMethod(comp->type(), it->second.callbackName, 2)) {
            (void)scene_.invoke(entityId, it->second.callbackName, args2);
        } else if (scene_.runtime().findMethod(comp->type(), it->second.callbackName, 1)) {
            (void)scene_.invoke(entityId, it->second.callbackName, args1);
        } else {
            (void)scene_.invoke(entityId, it->second.callbackName, {});
        }
    }
}

bool SceneFiberHostAdapter::onEvaluateCondition(
    const runtime::FiberContext& fiber,
    std::uint8_t conditionKind,
    std::uint16_t key16,
    std::uint16_t threshold16) const noexcept {
    const auto it = conditions_.find(key16);
    if (it == conditions_.end()) return false;
    const auto entityId = static_cast<SceneEntityId>(fiber.boundEntity.slot);
    if (entityId == SceneScriptRuntime::BroadcastTarget) return false;

    const ScriptObject* comp = it->second.componentType.empty()
        ? const_cast<SceneScriptRuntime&>(scene_).object(entityId)
        : scene_.getComponent(entityId, it->second.componentType);
    if (!comp) return false;

    runtime::RuntimeError err;
    const auto val = scene_.runtime().getMember(*comp, it->second.memberName, err);
    if (!val) return false;

    std::int64_t actual = 0;
    if (const auto* iv = std::get_if<std::int64_t>(&*val)) {
        actual = *iv;
    } else if (const auto* bv = std::get_if<bool>(&*val)) {
        actual = *bv ? 1 : 0;
    } else if (const auto* lv = std::get_if<runtime::LongValue>(&*val)) {
        actual = lv->value;
    } else {
        return false;
    }

    const auto rhs = static_cast<std::int64_t>(threshold16);
    switch (conditionKind) {
    case 0: return actual >= rhs;
    case 1: return actual > rhs;
    case 2: return actual == rhs;
    case 3: return actual < rhs;
    case 4: return actual <= rhs;
    case 5: return actual != rhs;
    default: return actual >= rhs;
    }
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
            "SetEntityActive",
            [scenePtr](std::int64_t entityId, bool active) -> bool {
                if (entityId <= 0) return false;
                return scenePtr->setEntityActive(
                    static_cast<SceneEntityId>(entityId), active);
            }) &&
        api.function(
            moduleName,
            "IsEntityActiveSelf",
            [scenePtr](std::int64_t entityId) -> bool {
                if (entityId <= 0) return false;
                return scenePtr->isEntityActiveSelf(
                    static_cast<SceneEntityId>(entityId));
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
            }) &&
        api.function(
            moduleName,
            "SetParent",
            [scenePtr](std::int64_t childId, std::int64_t parentId) -> bool {
                if (childId <= 0 || parentId < 0) return false;
                return scenePtr->setParent(
                    static_cast<SceneEntityId>(childId),
                    static_cast<SceneEntityId>(parentId));
            }) &&
        api.function(
            moduleName,
            "GetParent",
            [scenePtr](std::int64_t entityId) -> std::int64_t {
                if (entityId <= 0) return 0;
                return static_cast<std::int64_t>(scenePtr->getParent(
                    static_cast<SceneEntityId>(entityId)));
            }) &&
        api.function(
            moduleName,
            "IsActiveInHierarchy",
            [scenePtr](std::int64_t entityId) -> bool {
                if (entityId <= 0) return false;
                return scenePtr->isActiveInHierarchy(
                    static_cast<SceneEntityId>(entityId));
            }) &&
        api.function(
            moduleName,
            "SetLocalPosition2D",
            [scenePtr](std::int64_t entityId, Vec2 pos) -> bool {
                if (entityId <= 0) return false;
                scenePtr->setLocalPosition2D(
                    static_cast<SceneEntityId>(entityId), pos);
                return true;
            }) &&
        api.function(
            moduleName,
            "GetWorldPosition2D",
            [scenePtr](std::int64_t entityId) -> Vec2 {
                if (entityId <= 0) return Vec2{};
                return scenePtr->getWorldPosition2D(
                    static_cast<SceneEntityId>(entityId));
            }) &&
        api.function(
            moduleName,
            "EmitSignal",
            [scenePtr](std::int64_t sourceEntity, std::string signalName) -> bool {
                if (sourceEntity <= 0 || signalName.empty()) return false;
                return scenePtr->emitSignal(
                    static_cast<SceneEntityId>(sourceEntity), signalName) > 0;
            }) &&
        api.function(
            moduleName,
            "IsActionPressed",
            [scenePtr](std::string action) -> bool {
                return scenePtr->isInputActionPressed(action);
            }) &&
        api.function(
            moduleName,
            "IsActionJustPressed",
            [scenePtr](std::string action) -> bool {
                return scenePtr->isInputActionJustPressed(action);
            }) &&
        api.function(
            moduleName,
            "IsActionJustReleased",
            [scenePtr](std::string action) -> bool {
                return scenePtr->isInputActionJustReleased(action);
            }) &&
        api.function(
            moduleName,
            "GetInputAxis2D",
            [scenePtr](std::string axisName) -> Vec2 {
                return scenePtr->getInputAxis2D(axisName);
            }) &&
        api.function(
            moduleName,
            "Raycast2D",
            [scenePtr](Vec2 origin, Vec2 direction, Fixed maxDistance) -> std::int64_t {
                const auto hit = scenePtr->raycast2D(origin, direction, maxDistance);
                return hit ? static_cast<std::int64_t>(hit->entity) : std::int64_t{0};
            }) &&
        api.function(
            moduleName,
            "OverlapCircleCount2D",
            [scenePtr](Vec2 center, Fixed radius) -> std::int32_t {
                return static_cast<std::int32_t>(
                    scenePtr->overlapCircle2D(center, radius).size());
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
