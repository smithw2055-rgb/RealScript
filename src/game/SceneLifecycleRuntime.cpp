#include "realscript/game/GameScripting.h"

#include <utility>

namespace realscript::game {

bool SceneScriptRuntime::attach(
    SceneEntityId entity,
    const std::string& scriptType,
    SceneScriptOptions options) {
    if (entity == BroadcastTarget || instances_.find(entity) != instances_.end()) {
        return false;
    }
    runtime::RuntimeError error;
    auto object = runtime_.createObject(scriptType, error, executionOptions_);
    if (!object) {
        recordError(entity, "OnCreate", error);
        return false;
    }
    for (auto& [name, value] : options.initialMembers) {
        if (!runtime_.setMember(*object, name, std::move(value), error, executionOptions_)) {
            recordError(entity, "SetMember:" + name, error);
            return false;
        }
    }

    Instance instance;
    instance.object = std::move(*object);
    instance.enabled = options.enabled;
    const auto& type = instance.object.type();
    instance.onCreate0 = runtime_.findMethod(type, "OnCreate", 0);
    instance.onCreate1 = runtime_.findMethod(type, "OnCreate", 1);
    instance.onStart = runtime_.findMethod(type, "OnStart", 0);
    instance.onEnable = runtime_.findMethod(type, "OnEnable", 0);
    instance.onDisable = runtime_.findMethod(type, "OnDisable", 0);
    instance.onUpdate = runtime_.findMethod(type, "OnUpdate", 1);
    instance.onFixedUpdate = runtime_.findMethod(type, "OnFixedUpdate", 1);
    instance.onLateUpdate = runtime_.findMethod(type, "OnLateUpdate", 1);
    instance.onDestroy = runtime_.findMethod(type, "OnDestroy", 0);

    auto inserted = instances_.emplace(entity, std::move(instance));
    auto& stored = inserted.first->second;
    const bool hasOwner = !std::holds_alternative<std::monostate>(options.owner);
    if (hasOwner && stored.onCreate1) {
        if (!invokeLifecycle(
                entity, stored, stored.onCreate1, {std::move(options.owner)}, "OnCreate")) {
            instances_.erase(entity);
            return false;
        }
    } else if (!invokeLifecycle(
            entity, stored, stored.onCreate0, {}, "OnCreate")) {
        instances_.erase(entity);
        return false;
    }
    if (stored.enabled && !invokeLifecycle(
            entity, stored, stored.onEnable, {}, "OnEnable")) {
        instances_.erase(entity);
        return false;
    }
    return true;
}

bool SceneScriptRuntime::detach(SceneEntityId entity) {
    const auto found = instances_.find(entity);
    if (found == instances_.end()) return false;
    auto& instance = found->second;
    if (instance.enabled) {
        (void)invokeLifecycle(entity, instance, instance.onDisable, {}, "OnDisable");
    }
    (void)invokeLifecycle(entity, instance, instance.onDestroy, {}, "OnDestroy");
    instances_.erase(found);
    return true;
}

bool SceneScriptRuntime::setEnabled(SceneEntityId entity, bool enabled) {
    const auto found = instances_.find(entity);
    if (found == instances_.end() || found->second.enabled == enabled) return false;
    auto& instance = found->second;
    const auto& callback = enabled ? instance.onEnable : instance.onDisable;
    if (!invokeLifecycle(
            entity,
            instance,
            callback,
            {},
            enabled ? "OnEnable" : "OnDisable")) {
        return false;
    }
    instance.enabled = enabled;
    return true;
}

void SceneScriptRuntime::start() {
    for (auto& [entity, instance] : instances_) {
        if (!instance.enabled || instance.started) continue;
        if (invokeLifecycle(entity, instance, instance.onStart, {}, "OnStart")) {
            instance.started = true;
        }
    }
}

void SceneScriptRuntime::update(double deltaTime) {
    for (auto& [entity, instance] : instances_) {
        if (instance.enabled && instance.started) {
            (void)invokeLifecycle(
                entity, instance, instance.onUpdate, {deltaTime}, "OnUpdate");
        }
    }
}

void SceneScriptRuntime::fixedUpdate(double fixedDeltaTime) {
    for (auto& [entity, instance] : instances_) {
        if (instance.enabled && instance.started) {
            (void)invokeLifecycle(
                entity,
                instance,
                instance.onFixedUpdate,
                {fixedDeltaTime},
                "OnFixedUpdate");
        }
    }
}

void SceneScriptRuntime::lateUpdate(double deltaTime) {
    for (auto& [entity, instance] : instances_) {
        if (instance.enabled && instance.started) {
            (void)invokeLifecycle(
                entity, instance, instance.onLateUpdate, {deltaTime}, "OnLateUpdate");
        }
    }
}

} // namespace realscript::game
