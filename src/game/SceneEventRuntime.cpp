#include "realscript/game/GameScripting.h"

#include <algorithm>
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
            if (!consumeFireAndForgetInvocationResult(
                    event.target, *method, result, event.callback)) {
                allSucceeded = false;
            }
        }
        return matchedAny && allSucceeded;
    }
    bool succeeded = true;
    std::vector<SceneEntityId> sortedEntities;
    sortedEntities.reserve(instances_.size());
    for (const auto& [entity, _] : instances_) {
        (void)_;
        sortedEntities.push_back(entity);
    }
    std::sort(sortedEntities.begin(), sortedEntities.end());
    for (const auto entity : sortedEntities) {
        const auto instIt = instances_.find(entity);
        if (instIt == instances_.end()) continue;
        for (auto& instance : instIt->second) {
            if (!instance.enabled) continue;
            const auto* method = findCachedMethod(
                instance.object.type(), event.callback, event.arguments.size());
            if (!method) continue;
            auto result = runtime_.invoke(
                instance.object, *method, event.arguments, executionOptions_);
            if (!consumeFireAndForgetInvocationResult(
                    entity, *method, result, event.callback)) {
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
    for (const auto& [ent, _] : colliders2D_) {
        (void)_;
        if (ent != BroadcastTarget) allIds.insert(ent);
    }
    for (const auto& [ent, _] : colliders3D_) {
        (void)_;
        if (ent != BroadcastTarget) allIds.insert(ent);
    }
    for (const auto& [ent, _] : entityNames_) {
        (void)_;
        if (ent != BroadcastTarget) allIds.insert(ent);
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

void SceneFiberHostAdapter::registerAsyncConditionKey(
    std::uint16_t key16,
    SceneScriptRuntime::AsyncRequestId requestId) {
    asyncConditions_[key16] = requestId;
}

namespace {

SceneEntityId resolveBoundFiberEntity(
    const SceneScriptRuntime& scene,
    runtime::GenerationalEntityHandle handle) noexcept {
    if (handle.slot == 0 ||
        handle.slot == runtime::GenerationalEntityHandle::InvalidSlot) {
        return SceneScriptRuntime::BroadcastTarget;
    }
    const std::uint32_t slotGen = scene.getEntitySlotGeneration(handle.slot);
    if (slotGen != 0) {
        if (slotGen != handle.generation) {
            return SceneScriptRuntime::BroadcastTarget;
        }
        return (static_cast<SceneEntityId>(slotGen) << 32u) | handle.slot;
    }
    return static_cast<SceneEntityId>(handle.slot);
}

} // namespace

void SceneFiberHostAdapter::onHostCommand(
    runtime::FiberContext& fiber,
    std::uint8_t commandId,
    std::uint16_t arg16,
    std::uint32_t arg32) noexcept {
    const auto it = commands_.find(commandId);
    if (it == commands_.end()) return;
    const auto entityId = resolveBoundFiberEntity(scene_, fiber.boundEntity);
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
    if (const auto asyncIt = asyncConditions_.find(key16);
        asyncIt != asyncConditions_.end()) {
        const std::int64_t actual = scene_.isAsyncRequestCompleted(asyncIt->second) ? 1 : 0;
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
    const auto it = conditions_.find(key16);
    if (it == conditions_.end()) return false;
    const auto entityId = resolveBoundFiberEntity(scene_, fiber.boundEntity);
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
            }) &&
        api.function(
            moduleName,
            "SpawnEntity",
            [scenePtr](std::string name) -> std::int64_t {
                return static_cast<std::int64_t>(scenePtr->spawnEntity(name));
            }) &&
        api.function(
            moduleName,
            "QueueDestroyEntity",
            [scenePtr](std::int64_t entityId) -> bool {
                if (entityId <= 0) return false;
                scenePtr->queueDestroyEntity(static_cast<SceneEntityId>(entityId));
                return true;
            }) &&
        api.function(
            moduleName,
            "SetEntityName",
            [scenePtr](std::int64_t entityId, std::string name) -> bool {
                if (entityId <= 0) return false;
                scenePtr->setEntityName(static_cast<SceneEntityId>(entityId), std::move(name));
                return true;
            }) &&
        api.function(
            moduleName,
            "GetEntityName",
            [scenePtr](std::int64_t entityId) -> std::string {
                if (entityId <= 0) return {};
                return scenePtr->getEntityName(static_cast<SceneEntityId>(entityId));
            }) &&
        api.function(
            moduleName,
            "FindEntityByName",
            [scenePtr](std::string name) -> std::int64_t {
                return static_cast<std::int64_t>(scenePtr->findEntityByName(name));
            }) &&
        api.function(
            moduleName,
            "AddToGroup",
            [scenePtr](std::int64_t entityId, std::string group) -> bool {
                if (entityId <= 0 || group.empty()) return false;
                scenePtr->addEntityToGroup(static_cast<SceneEntityId>(entityId), group);
                return true;
            }) &&
        api.function(
            moduleName,
            "RemoveFromGroup",
            [scenePtr](std::int64_t entityId, std::string group) -> bool {
                if (entityId <= 0 || group.empty()) return false;
                return scenePtr->removeEntityFromGroup(static_cast<SceneEntityId>(entityId), group);
            }) &&
        api.function(
            moduleName,
            "IsInGroup",
            [scenePtr](std::int64_t entityId, std::string group) -> bool {
                if (entityId <= 0 || group.empty()) return false;
                return scenePtr->isEntityInGroup(static_cast<SceneEntityId>(entityId), group);
            }) &&
        api.function(
            moduleName,
            "GetGroupCount",
            [scenePtr](std::string group) -> std::int32_t {
                return static_cast<std::int32_t>(scenePtr->getEntitiesInGroup(group).size());
            }) &&
        api.function(
            moduleName,
            "SpawnGenerationalEntity",
            [scenePtr](std::string name) -> std::int64_t {
                return static_cast<std::int64_t>(scenePtr->spawnGenerationalEntity(name));
            }) &&
        api.function(
            moduleName,
            "IsEntityAlive",
            [scenePtr](std::int64_t entityId) -> bool {
                if (entityId <= 0) return false;
                return scenePtr->isEntityAlive(static_cast<SceneEntityId>(entityId));
            }) &&
        api.function(
            moduleName,
            "GetCollisionNormal2D",
            [scenePtr](std::int64_t entityA, std::int64_t entityB) -> Vec2 {
                if (entityA <= 0 || entityB <= 0) return Vec2{};
                const auto contact = scenePtr->getCollisionContact2D(
                    static_cast<SceneEntityId>(entityA),
                    static_cast<SceneEntityId>(entityB));
                return contact ? contact->normal : Vec2{};
            }) &&
        api.function(
            moduleName,
            "GetCollisionPenetration2D",
            [scenePtr](std::int64_t entityA, std::int64_t entityB) -> Fixed {
                if (entityA <= 0 || entityB <= 0) return Fixed::fromRaw(0);
                const auto contact = scenePtr->getCollisionContact2D(
                    static_cast<SceneEntityId>(entityA),
                    static_cast<SceneEntityId>(entityB));
                return contact ? contact->penetration : Fixed::fromRaw(0);
            }) &&
        api.function(
            moduleName,
            "GetCollisionPoint2D",
            [scenePtr](std::int64_t entityA, std::int64_t entityB) -> Vec2 {
                if (entityA <= 0 || entityB <= 0) return Vec2{};
                const auto contact = scenePtr->getCollisionContact2D(
                    static_cast<SceneEntityId>(entityA),
                    static_cast<SceneEntityId>(entityB));
                return contact ? contact->contactPoint : Vec2{};
            }) &&
        api.function(
            moduleName,
            "RaycastAllCount2D",
            [scenePtr](Vec2 origin, Vec2 direction, Fixed maxDistance) -> std::int32_t {
                return static_cast<std::int32_t>(
                    scenePtr->raycastAll2D(origin, direction, maxDistance).size());
            }) &&
        api.function(
            moduleName,
            "Raycast3D",
            [scenePtr](Vec3 origin, Vec3 direction, Fixed maxDistance) -> std::int64_t {
                const auto hit = scenePtr->raycast3D(origin, direction, maxDistance);
                return hit ? static_cast<std::int64_t>(hit->entity) : std::int64_t{0};
            }) &&
        api.function(
            moduleName,
            "OverlapSphereCount3D",
            [scenePtr](Vec3 center, Fixed radius) -> std::int32_t {
                return static_cast<std::int32_t>(
                    scenePtr->overlapSphere3D(center, radius).size());
            }) &&
        api.function(
            moduleName,
            "GetCollisionNormal3D",
            [scenePtr](std::int64_t entityA, std::int64_t entityB) -> Vec3 {
                if (entityA <= 0 || entityB <= 0) return Vec3{};
                const auto contact = scenePtr->getCollisionContact3D(
                    static_cast<SceneEntityId>(entityA),
                    static_cast<SceneEntityId>(entityB));
                return contact ? contact->normal : Vec3{};
            }) &&
        api.function(
            moduleName,
            "CreateAsyncRequest",
            [scenePtr](std::string tag) -> std::int64_t {
                return static_cast<std::int64_t>(
                    scenePtr->createAsyncRequest(std::move(tag)));
            }) &&
        api.function(
            moduleName,
            "CompleteAsyncRequest",
            [scenePtr](std::int64_t requestId, std::int32_t result) -> bool {
                if (requestId <= 0) return false;
                return scenePtr->completeAsyncRequest(
                    static_cast<SceneScriptRuntime::AsyncRequestId>(requestId),
                    static_cast<std::int64_t>(result));
            }) &&
        api.function(
            moduleName,
            "IsAsyncRequestCompleted",
            [scenePtr](std::int64_t requestId) -> bool {
                if (requestId <= 0) return false;
                return scenePtr->isAsyncRequestCompleted(
                    static_cast<SceneScriptRuntime::AsyncRequestId>(requestId));
            }) &&
        api.function(
            moduleName,
            "GetAsyncRequestResultInt",
            [scenePtr](std::int64_t requestId) -> std::int32_t {
                if (requestId <= 0) return 0;
                const auto res = scenePtr->getAsyncRequestResult(
                    static_cast<SceneScriptRuntime::AsyncRequestId>(requestId));
                if (res && std::holds_alternative<std::int64_t>(*res)) {
                    return static_cast<std::int32_t>(std::get<std::int64_t>(*res));
                }
                return 0;
            }) &&
        api.function(
            moduleName,
            "WaitAsyncOperation",
            [scenePtr](
                std::int64_t targetEntity,
                std::string callback,
                std::int64_t requestId) -> std::int64_t {
                if (targetEntity <= 0 || requestId <= 0) return 0;
                const bool okWait = scenePtr->waitAsyncRequest(
                    static_cast<SceneScriptRuntime::AsyncRequestId>(requestId),
                    static_cast<SceneEntityId>(targetEntity),
                    std::move(callback));
                return okWait ? requestId : 0;
            }) &&
        api.function(
            moduleName,
            "EntityDestroyToken",
            [scenePtr](std::int64_t entityId) -> runtime::CancellationToken {
                if (entityId <= 0) {
                    return runtime::CancellationToken::preCanceled();
                }
                return scenePtr->entityDestroyToken(static_cast<SceneEntityId>(entityId));
            }) &&
        api.function(
            moduleName,
            "LoadPrefabSceneAsync",
            [scenePtr](
                std::string prefabKey,
                std::int64_t parentEntity,
                runtime::CancellationToken token) -> TypedAwaitable<std::int64_t> {
                const SceneEntityId parentId =
                    parentEntity > 0 ? static_cast<SceneEntityId>(parentEntity) : SceneEntityId{0};
                return TypedAwaitable<std::int64_t>{
                    scenePtr->loadPrefabSceneAsync(std::move(prefabKey), parentId, token)};
            }) &&
        api.function(
            moduleName,
            "LoadAssetAsync",
            [scenePtr](
                std::string assetPath,
                std::string typeName,
                runtime::CancellationToken token) -> TypedAwaitable<bool> {
                return TypedAwaitable<bool>{
                    scenePtr->loadAssetAsync(std::move(assetPath), std::move(typeName), token)};
            }) &&
        api.function(
            moduleName,
            "HasLoadedAsset",
            [scenePtr](std::string assetPath) -> bool {
                return scenePtr->runtime().findAsset(assetPath) != nullptr;
            }) &&
        api.function(
            moduleName,
            "UnloadLoadedAsset",
            [scenePtr](std::string assetPath) -> bool {
                return scenePtr->runtime().unloadAsset(assetPath);
            }) &&
        api.function(
            moduleName,
            "GetLoadedAssetInt",
            [scenePtr](std::string assetPath, std::string fieldName) -> std::int32_t {
                const auto* asset = scenePtr->runtime().findAsset(assetPath);
                if (!asset) return 0;
                runtime::RuntimeError err;
                const auto val = scenePtr->runtime().getMember(*asset, fieldName, err);
                if (val && std::holds_alternative<std::int64_t>(*val)) {
                    return static_cast<std::int32_t>(std::get<std::int64_t>(*val));
                }
                return 0;
            }) &&
        api.function(
            moduleName,
            "GetLoadedAssetString",
            [scenePtr](std::string assetPath, std::string fieldName) -> std::string {
                const auto* asset = scenePtr->runtime().findAsset(assetPath);
                if (!asset) return {};
                runtime::RuntimeError err;
                const auto val = scenePtr->runtime().getMember(*asset, fieldName, err);
                if (!val) return {};
                if (const auto* sv = std::get_if<std::string>(&*val)) return *sv;
                if (const auto* ref = std::get_if<runtime::ObjectRef>(&*val);
                    ref && scenePtr->runtime().heap()) {
                    if (const auto view = scenePtr->runtime().heap()->stringView(*ref)) {
                        return std::string(*view);
                    }
                }
                return {};
            }) &&
        api.function(
            moduleName,
            "SaveGameAsync",
            [scenePtr](
                std::string slotKey,
                std::string payload,
                runtime::CancellationToken token) -> TypedAwaitable<bool> {
                return TypedAwaitable<bool>{
                    scenePtr->saveGameAsync(std::move(slotKey), std::move(payload), token)};
            }) &&
        api.function(
            moduleName,
            "LoadGameAsync",
            [scenePtr](
                std::string slotKey,
                runtime::CancellationToken token) -> TypedAwaitable<std::string> {
                return TypedAwaitable<std::string>{
                    scenePtr->loadGameAsync(std::move(slotKey), token)};
            }) &&
        api.function(
            moduleName,
            "HttpGetAsync",
            [scenePtr](
                std::string url,
                double timeoutSeconds,
                runtime::CancellationToken token) -> TypedAwaitable<std::string> {
                SceneScriptRuntime::HttpRequestSpec spec;
                spec.method = "GET";
                spec.url = std::move(url);
                spec.timeoutSeconds = timeoutSeconds;
                return TypedAwaitable<std::string>{
                    scenePtr->httpRequestAsync(std::move(spec), false, token)};
            }) &&
        api.function(
            moduleName,
            "HttpPostAsync",
            [scenePtr](
                std::string url,
                std::string body,
                double timeoutSeconds,
                runtime::CancellationToken token) -> TypedAwaitable<std::string> {
                SceneScriptRuntime::HttpRequestSpec spec;
                spec.method = "POST";
                spec.url = std::move(url);
                spec.body = std::move(body);
                spec.timeoutSeconds = timeoutSeconds;
                return TypedAwaitable<std::string>{
                    scenePtr->httpRequestAsync(std::move(spec), false, token)};
            }) &&
        api.function(
            moduleName,
            "HttpSendStatusAsync",
            [scenePtr](
                std::string method,
                std::string url,
                std::string body,
                double timeoutSeconds,
                runtime::CancellationToken token) -> TypedAwaitable<std::int32_t> {
                SceneScriptRuntime::HttpRequestSpec spec;
                spec.method = std::move(method);
                spec.url = std::move(url);
                spec.body = std::move(body);
                spec.timeoutSeconds = timeoutSeconds;
                return TypedAwaitable<std::int32_t>{
                    scenePtr->httpRequestAsync(std::move(spec), true, token)};
            }) &&
        api.function(
            moduleName,
            "GetLastHttpStatus",
            [scenePtr]() -> std::int32_t {
                return static_cast<std::int32_t>(scenePtr->lastHttpStatusCode());
            }) &&
        api.function(
            moduleName,
            "GetLastHttpBody",
            [scenePtr]() -> std::string {
                return scenePtr->lastHttpResponseBody();
            }) &&
        api.function(
            moduleName,
            "GetLastHttpTimedOut",
            [scenePtr]() -> bool {
                return scenePtr->lastHttpTimedOut();
            }) &&
        api.function(
            moduleName,
            "WsConnectAsync",
            [scenePtr](
                std::string url,
                std::int64_t ownerEntity,
                double timeoutSeconds,
                runtime::CancellationToken token) -> TypedAwaitable<std::int64_t> {
                const SceneEntityId owner =
                    ownerEntity > 0 ? static_cast<SceneEntityId>(ownerEntity) : SceneEntityId{0};
                return TypedAwaitable<std::int64_t>{
                    scenePtr->wsConnectAsync(std::move(url), owner, timeoutSeconds, token)};
            }) &&
        api.function(
            moduleName,
            "WsSendAsync",
            [scenePtr](
                std::int64_t wsHandle,
                std::string message,
                double timeoutSeconds,
                runtime::CancellationToken token) -> TypedAwaitable<bool> {
                return TypedAwaitable<bool>{
                    scenePtr->wsSendAsync(wsHandle, std::move(message), timeoutSeconds, token)};
            }) &&
        api.function(
            moduleName,
            "WsReceiveAsync",
            [scenePtr](
                std::int64_t wsHandle,
                double timeoutSeconds,
                runtime::CancellationToken token) -> TypedAwaitable<std::string> {
                return TypedAwaitable<std::string>{
                    scenePtr->wsReceiveAsync(wsHandle, timeoutSeconds, token)};
            }) &&
        api.function(
            moduleName,
            "WsCloseAsync",
            [scenePtr](std::int64_t wsHandle) -> TypedAwaitable<bool> {
                return TypedAwaitable<bool>{scenePtr->wsCloseAsync(wsHandle)};
            }) &&
        api.function(
            moduleName,
            "WsIsConnected",
            [scenePtr](std::int64_t wsHandle) -> bool {
                return scenePtr->isWebSocketConnected(wsHandle);
            });
    return ok && api.valid();
}

void SceneScriptRuntime::onLifecycleCoroutineCompleted(
    void* context,
    runtime::AwaitableHandle handle,
    runtime::AwaitStatus,
    std::uint64_t packedHandle) {
    auto* self = static_cast<SceneScriptRuntime*>(context);
    if (!self) return;
    PendingLifecycleCoroutine meta{};
    bool hasMeta = false;
    const auto it = self->pendingLifecycleCoroutines_.find(packedHandle);
    if (it != self->pendingLifecycleCoroutines_.end()) {
        meta = std::move(it->second);
        self->pendingLifecycleCoroutines_.erase(it);
        hasMeta = true;
    }
    const auto ctx = self->runtime_.asyncRuntime();
    if (!ctx) return;

    runtime::Value unused{};
    runtime::RuntimeError faultErr{};
    const auto terminal = ctx->awaitables().getResult(handle, unused, faultErr);
    if (terminal == runtime::AwaitStatus::Faulted && hasMeta) {
        self->recordError(meta.entity, std::move(meta.callback), faultErr);
    }
}

bool SceneScriptRuntime::consumeFireAndForgetInvocationResult(
    SceneEntityId entity,
    const ScriptMethod& method,
    const runtime::ExecutionResult& result,
    std::string_view callback) {
    if (!result.succeeded) {
        recordError(entity, std::string(callback), result.error);
        return false;
    }

    if (semantic::storageReturnTypeOf(method.descriptor) ==
        semantic::PrimitiveType::Struct) {
        const std::string& retTypeName =
            semantic::storageReturnTypeNameOf(method.descriptor);
        const bool isAwaitable =
            retTypeName == "RealScript.Async::Awaitable" ||
            retTypeName.find("::Awaitable__") != std::string::npos;
        const auto* sVal = std::get_if<runtime::StructValue>(&result.value);
        if (isAwaitable && sVal && sVal->valid() &&
            sVal->typeId == semantic::stableTypeId(retTypeName)) {
            runtime::AwaitableHandle awaitableHandle{};
            if (runtime::tryExtractAwaitableHandle(result.value, awaitableHandle) &&
                awaitableHandle.valid()) {
                if (awaitableHandle.isCompletedVoidSentinel()) {
                    return true;
                }
                if (const auto ctx = runtime_.asyncRuntime()) {
                    auto& pool = ctx->awaitables();
                    const auto st = pool.status(awaitableHandle);
                    if (st == runtime::AwaitStatus::Pending) {
                        const std::uint64_t key = awaitableHandle.pack64();
                        pendingLifecycleCoroutines_[key] = PendingLifecycleCoroutine{
                            entity,
                            std::string(callback),
                        };
                        runtime::AsyncContinuation cont{};
                        cont.callback = &SceneScriptRuntime::onLifecycleCoroutineCompleted;
                        cont.context = this;
                        cont.userTag = key;
                        cont.runInlineOnComplete = true;
                        (void)pool.onCompleted(awaitableHandle, cont);
                        return true;
                    }
                    runtime::Value unused{};
                    runtime::RuntimeError faultErr{};
                    const auto terminal = pool.getResult(awaitableHandle, unused, faultErr);
                    if (terminal == runtime::AwaitStatus::Faulted) {
                        recordError(entity, std::string(callback), faultErr);
                        return false;
                    }
                }
            }
        }
    }
    return true;
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
    return consumeFireAndForgetInvocationResult(
        entity,
        *method,
        result,
        callback ? std::string_view(callback) : std::string_view{});
}

void SceneScriptRuntime::recordError(
    SceneEntityId entity,
    std::string callback,
    const runtime::RuntimeError& error) {
    errors_.push_back({entity, std::move(callback), error});
}

} // namespace realscript::game
