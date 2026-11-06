#include "realscript/game/GameScripting.h"
#include "realscript/game/GameplayMetadata.h"

#include <unordered_map>
#include <utility>

namespace realscript::game {
namespace {

semantic::SymbolId exactFieldTypeId(
    semantic::PrimitiveType type,
    const std::string& typeName) {
    return semantic::isExactType(type) && !typeName.empty()
        ? semantic::stableTypeId(typeName)
        : 0;
}

bool hasEditModeAttribute(
    const runtime::ProgramImage* program,
    const std::string& canonicalTypeName) {
    if (!program) return false;
    for (const auto& module : program->modules()) {
        for (const auto& attr : module.languageMetadata.attributes) {
            if (attr.target == canonicalTypeName &&
                (attr.name == "Tool" || attr.name == "ExecuteInEditMode")) {
                return true;
            }
        }
    }
    return false;
}

std::string findRenamedFieldSource(
    const ScriptMetadataRegistry& metadata,
    const std::string& canonicalTypeName,
    const std::string& newFieldName) {
    const auto* attr = metadata.findMemberAttribute(
        canonicalTypeName, newFieldName, "FormerlySerializedAs");
    if (!attr) return {};
    static const char* const kKeys[] = {"oldName", "name", "arg0"};
    for (const char* key : kKeys) {
        const auto found = attr->arguments.find(key);
        if (found != attr->arguments.end()) {
            if (const auto* str = std::get_if<std::string>(&found->second)) {
                return *str;
            }
        }
    }
    for (const auto& entry : attr->arguments) {
        if (const auto* str = std::get_if<std::string>(&entry.second)) {
            return *str;
        }
    }
    return {};
}

} // namespace

void SceneScriptRuntime::bindLifecycleMethods(Instance& instance) const {
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
    instance.executeInEditMode = hasEditModeAttribute(
        runtime_.program().get(), type.canonicalName());
}

bool SceneScriptRuntime::shouldTickInCurrentMode(
    const Instance& instance) const noexcept {
    if (executionMode_ == SceneExecutionMode::Play) return true;
    return instance.executeInEditMode;
}

bool SceneScriptRuntime::matchesComponentType(
    const ScriptType& componentType,
    const std::string& query) const {
    if (query.empty()) return false;
    if (componentType.canonicalName() == query ||
        componentType.descriptor.name == query) {
        return true;
    }
    if (const auto targetType = runtime_.findType(query)) {
        return runtime_.isTypeAssignable(
            componentType.descriptor.id, targetType->descriptor.id);
    }
    if (runtime_.program()) {
        for (const auto& module : runtime_.program()->modules()) {
            for (const auto& candidate : module.types) {
                if (candidate.name == query &&
                    runtime_.isTypeAssignable(
                        componentType.descriptor.id, candidate.id)) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool SceneScriptRuntime::attach(
    SceneEntityId entity,
    const std::string& scriptType,
    SceneScriptOptions options) {
    if (entity == BroadcastTarget || instances_.find(entity) != instances_.end()) {
        return false;
    }
    return attachComponent(entity, scriptType, std::move(options));
}

bool SceneScriptRuntime::attachComponent(
    SceneEntityId entity,
    const std::string& scriptType,
    SceneScriptOptions options) {
    if (entity == BroadcastTarget) {
        return false;
    }
    const auto existingEntity = instances_.find(entity);
    if (existingEntity != instances_.end()) {
        for (const auto& comp : existingEntity->second) {
            if (comp.object.type().canonicalName() == scriptType ||
                comp.object.type().descriptor.name == scriptType) {
                return false;
            }
        }
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
    bindLifecycleMethods(instance);

    auto& list = instances_[entity];
    list.push_back(std::move(instance));
    auto& stored = list.back();
    const bool hasOwner = !std::holds_alternative<std::monostate>(options.owner);
    if (hasOwner && stored.onCreate1) {
        if (!invokeLifecycle(
                entity, stored, stored.onCreate1, {std::move(options.owner)}, "OnCreate")) {
            list.pop_back();
            if (list.empty()) instances_.erase(entity);
            return false;
        }
    } else if (!invokeLifecycle(
            entity, stored, stored.onCreate0, {}, "OnCreate")) {
        list.pop_back();
        if (list.empty()) instances_.erase(entity);
        return false;
    }
    if (stored.enabled && !invokeLifecycle(
            entity, stored, stored.onEnable, {}, "OnEnable")) {
        list.pop_back();
        if (list.empty()) instances_.erase(entity);
        return false;
    }
    return true;
}

bool SceneScriptRuntime::detach(SceneEntityId entity) {
    const auto found = instances_.find(entity);
    if (found == instances_.end()) return false;
    for (auto& instance : found->second) {
        if (instance.enabled) {
            (void)invokeLifecycle(entity, instance, instance.onDisable, {}, "OnDisable");
        }
        (void)invokeLifecycle(entity, instance, instance.onDestroy, {}, "OnDestroy");
    }
    instances_.erase(found);
    return true;
}

bool SceneScriptRuntime::detachComponent(
    SceneEntityId entity,
    const std::string& scriptType) {
    const auto found = instances_.find(entity);
    if (found == instances_.end()) return false;
    auto& list = found->second;
    for (auto it = list.begin(); it != list.end(); ++it) {
        if (matchesComponentType(it->object.type(), scriptType)) {
            if (it->enabled) {
                (void)invokeLifecycle(entity, *it, it->onDisable, {}, "OnDisable");
            }
            (void)invokeLifecycle(entity, *it, it->onDestroy, {}, "OnDestroy");
            list.erase(it);
            if (list.empty()) instances_.erase(found);
            return true;
        }
    }
    return false;
}

bool SceneScriptRuntime::setEnabled(SceneEntityId entity, bool enabled) {
    const auto found = instances_.find(entity);
    if (found == instances_.end() || found->second.empty() ||
        found->second.front().enabled == enabled) {
        return false;
    }
    auto& instance = found->second.front();
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

bool SceneScriptRuntime::setComponentEnabled(
    SceneEntityId entity,
    const std::string& scriptType,
    bool enabled) {
    const auto found = instances_.find(entity);
    if (found == instances_.end()) return false;
    for (auto& instance : found->second) {
        if (!matchesComponentType(instance.object.type(), scriptType)) continue;
        if (instance.enabled == enabled) return false;
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
    return false;
}

void SceneScriptRuntime::start() {
    for (auto& [entity, components] : instances_) {
        for (auto& instance : components) {
            if (!instance.enabled || instance.started ||
                !shouldTickInCurrentMode(instance)) {
                continue;
            }
            if (invokeLifecycle(entity, instance, instance.onStart, {}, "OnStart")) {
                instance.started = true;
            }
        }
    }
}

void SceneScriptRuntime::update(double deltaTime) {
    for (auto& [entity, components] : instances_) {
        for (auto& instance : components) {
            if (!instance.enabled || !shouldTickInCurrentMode(instance)) continue;
            if (!instance.started && executionMode_ == SceneExecutionMode::Play) {
                continue;
            }
            (void)invokeLifecycle(
                entity, instance, instance.onUpdate, {deltaTime}, "OnUpdate");
        }
    }
}

void SceneScriptRuntime::fixedUpdate(double fixedDeltaTime) {
    for (auto& [entity, components] : instances_) {
        for (auto& instance : components) {
            if (!instance.enabled || !shouldTickInCurrentMode(instance)) continue;
            if (!instance.started && executionMode_ == SceneExecutionMode::Play) {
                continue;
            }
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
    for (auto& [entity, components] : instances_) {
        for (auto& instance : components) {
            if (!instance.enabled || !shouldTickInCurrentMode(instance)) continue;
            if (!instance.started && executionMode_ == SceneExecutionMode::Play) {
                continue;
            }
            (void)invokeLifecycle(
                entity, instance, instance.onLateUpdate, {deltaTime}, "OnLateUpdate");
        }
    }
}

SceneHotReloadReport SceneScriptRuntime::hotReload(
    const GameProgram& newProgram,
    const ScriptMetadataRegistry* metadata) {
    SceneHotReloadReport report;
    if (!newProgram.valid()) {
        report.errors.push_back("hot reload target GameProgram is invalid");
        return report;
    }

    ScriptMetadataRegistry mergedMetadata;
    if (metadata) {
        mergedMetadata = *metadata;
    }
    mergedMetadata.populateFromProgram(newProgram);

    struct SavedComponentState {
        std::string canonicalTypeName;
        bool enabled = true;
        bool started = false;
        std::unordered_map<std::string, runtime::Value> fields;
    };
    std::map<SceneEntityId, std::vector<SavedComponentState>> savedEntities;

    for (const auto& [entity, components] : instances_) {
        auto& savedList = savedEntities[entity];
        savedList.reserve(components.size());
        for (const auto& instance : components) {
            SavedComponentState saved;
            saved.canonicalTypeName = instance.object.type().canonicalName();
            saved.enabled = instance.enabled;
            saved.started = instance.started;
            for (const auto& field : instance.object.type().descriptor.fields) {
                auto val = runtime_.heap()->fieldGet(
                    instance.object.reference(), field.index);
                if (val) {
                    saved.fields.emplace(field.name, std::move(*val));
                }
            }
            savedList.push_back(std::move(saved));
        }
    }

    runtime_.replaceProgram(newProgram);
    methodCache_.clear();

    for (auto& [entity, components] : instances_) {
        const auto& savedList = savedEntities[entity];
        for (std::size_t idx = 0; idx < components.size(); ++idx) {
            auto& instance = components[idx];
            const auto& saved = savedList[idx];
            const auto newType = runtime_.findType(saved.canonicalTypeName);
            if (!newType) {
                report.errors.push_back(
                    "script type '" + saved.canonicalTypeName +
                    "' was removed in hot-reloaded program");
                continue;
            }
            runtime::RuntimeError allocError;
            auto newObj = runtime_.allocateUninitializedObject(*newType, allocError);
            if (!newObj) {
                report.errors.push_back(
                    "failed to allocate migrated instance of '" +
                    saved.canonicalTypeName + "': " + allocError.message);
                continue;
            }

            for (const auto& newField : newType->descriptor.fields) {
                const auto newFieldTypeId = exactFieldTypeId(
                    newField.type, newField.typeName);
                const auto directIt = saved.fields.find(newField.name);
                if (directIt != saved.fields.end() &&
                    runtime_.valueMatches(
                        directIt->second, newField.type, newFieldTypeId)) {
                    (void)runtime_.heap()->fieldSet(
                        newObj->reference(), newField.index, directIt->second, &allocError);
                    ++report.fieldsMigrated;
                    continue;
                }

                const std::string oldFieldName = findRenamedFieldSource(
                    mergedMetadata, saved.canonicalTypeName, newField.name);
                if (!oldFieldName.empty()) {
                    const auto renamedIt = saved.fields.find(oldFieldName);
                    if (renamedIt != saved.fields.end() &&
                        runtime_.valueMatches(
                            renamedIt->second, newField.type, newFieldTypeId)) {
                        (void)runtime_.heap()->fieldSet(
                            newObj->reference(),
                            newField.index,
                            renamedIt->second,
                            &allocError);
                        ++report.fieldsMigrated;
                        ++report.fieldsRenamed;
                        continue;
                    }
                }
                ++report.fieldsDefaulted;
            }

            instance.object = std::move(*newObj);
            instance.enabled = saved.enabled;
            instance.started = saved.started;
            bindLifecycleMethods(instance);
            if (mergedMetadata.findTypeAttribute(saved.canonicalTypeName, "Tool") ||
                mergedMetadata.findTypeAttribute(
                    saved.canonicalTypeName, "ExecuteInEditMode")) {
                instance.executeInEditMode = true;
            }
            ++report.instancesMigrated;
        }
    }

    report.succeeded = report.errors.empty();
    return report;
}

} // namespace realscript::game
