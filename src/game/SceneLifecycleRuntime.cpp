#include <cstring>
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

int findExecutionOrderAttribute(
    const runtime::ProgramImage* program,
    const std::string& canonicalTypeName) {
    if (!program) return 0;
    for (const auto& module : program->modules()) {
        for (const auto& attr : module.languageMetadata.attributes) {
            if (attr.target == canonicalTypeName &&
                (attr.name == "DefaultExecutionOrder" || attr.name == "ExecutionOrder") &&
                !attr.arguments.empty()) {
                try {
                    return std::stoi(attr.arguments.front().value);
                } catch (...) {
                    return 0;
                }
            }
        }
    }
    return 0;
}

std::vector<std::string> findRequiredComponents(
    const runtime::ProgramImage* program,
    const std::string& canonicalTypeName) {
    std::vector<std::string> required;
    if (!program) return required;
    for (const auto& module : program->modules()) {
        for (const auto& attr : module.languageMetadata.attributes) {
            if (attr.target == canonicalTypeName &&
                attr.name == "RequireComponent" &&
                !attr.arguments.empty()) {
                std::string val = attr.arguments.front().value;
                if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
                    val = val.substr(1, val.size() - 2);
                }
                if (!val.empty()) required.push_back(std::move(val));
            }
        }
    }
    return required;
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

void cancelActiveInstanceSequences(
    const ScriptRuntime& runtime,
    const ScriptObject& object,
    const runtime::ExecutionOptions& options) {
    if (!object.valid()) return;
    static constexpr const char* kPrefix = "$sequence_state_";
    const std::size_t prefixLen = std::strlen(kPrefix);
    for (const auto& field : object.type().descriptor.fields) {
        if (!field.synthetic) continue;
        if (field.name.size() > prefixLen &&
            field.name.rfind(kPrefix, 0) == 0) {
            const std::string seqName = field.name.substr(prefixLen);
            if (const auto cancelMethod = runtime.findMethod(
                    object.type(), "Cancel" + seqName, 0)) {
                (void)runtime.invoke(object, *cancelMethod, {}, options);
            }
        } else if (field.name == "$sequence_timer") {
            for (const auto& method : object.type().descriptor.methods) {
                if (method.synthetic &&
                    method.name.rfind("Cancel", 0) == 0 &&
                    method.parameters.size() == 1) {
                    (void)runtime.invoke(object, ScriptMethod{method}, {}, options);
                }
            }
        }
    }
}

void patchMigratedObjectRefsInHeapObject(
    runtime::ManagedHeap* heap,
    runtime::ObjectRef target,
    const std::unordered_map<std::uint64_t, runtime::ObjectRef>& refMap,
    std::unordered_set<std::uint64_t>& visited) {
    if (!heap || !target.valid()) return;
    const std::uint64_t key =
        (static_cast<std::uint64_t>(target.generation) << 32u) | target.slot;
    if (!visited.insert(key).second) return;
    const auto fCount = heap->fieldCount(target);
    if (!fCount) return;
    for (std::size_t i = 0; i < *fCount; ++i) {
        auto val = heap->fieldGet(target, i);
        if (!val) continue;
        if (const auto* objRef = std::get_if<runtime::ObjectRef>(&*val)) {
            const std::uint64_t refKey =
                (static_cast<std::uint64_t>(objRef->generation) << 32u) | objRef->slot;
            const auto mapped = refMap.find(refKey);
            if (mapped != refMap.end()) {
                (void)heap->fieldSet(target, i, mapped->second);
            } else {
                patchMigratedObjectRefsInHeapObject(heap, *objRef, refMap, visited);
            }
        }
    }
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
    instance.onValidate = runtime_.findMethod(type, "OnValidate", 0);
    instance.executeInEditMode = hasEditModeAttribute(
        runtime_.program().get(), type.canonicalName());
    instance.executionOrder = findExecutionOrderAttribute(
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
    for (const auto& reqType : findRequiredComponents(
             runtime_.program().get(), object->type().canonicalName())) {
        if (!hasComponent(entity, reqType)) {
            error.code = runtime::ErrorCode::InvalidArguments;
            error.message = "missing required component '" + reqType +
                "' for '" + object->type().canonicalName() + "'";
            recordError(entity, "RequireComponent", error);
            return false;
        }
    }
    if (entityActiveSelf_.find(entity) == entityActiveSelf_.end()) {
        entityActiveSelf_[entity] = true;
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
    const auto childrenIt = childrenByEntity_.find(entity);
    if (childrenIt != childrenByEntity_.end()) {
        const auto childrenCopy = childrenIt->second;
        for (const auto child : childrenCopy) {
            (void)detach(child);
        }
        childrenByEntity_.erase(entity);
    }
    const auto parentIt = parentByEntity_.find(entity);
    if (parentIt != parentByEntity_.end()) {
        const auto parentId = parentIt->second;
        parentByEntity_.erase(parentIt);
        const auto pChildrenIt = childrenByEntity_.find(parentId);
        if (pChildrenIt != childrenByEntity_.end()) {
            auto& vec = pChildrenIt->second;
            vec.erase(std::remove(vec.begin(), vec.end(), entity), vec.end());
        }
    }
    entityActiveSelf_.erase(entity);
    localTransforms_.erase(entity);

    const auto found = instances_.find(entity);
    if (found == instances_.end()) return false;
    for (auto& instance : found->second) {
        cancelActiveInstanceSequences(runtime_, instance.object, executionOptions_);
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
            cancelActiveInstanceSequences(runtime_, it->object, executionOptions_);
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
    if (!enabled) {
        cancelActiveInstanceSequences(runtime_, instance.object, executionOptions_);
    }
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
    bool anyComponentEnabled = false;
    for (const auto& comp : found->second) {
        if (comp.enabled) {
            anyComponentEnabled = true;
            break;
        }
    }
    entityActiveSelf_[entity] = anyComponentEnabled;

    // Cascade OnEnable / OnDisable to active child entities in hierarchy
    const auto notifySubtree = [&](const auto& self, SceneEntityId parentNode) -> void {
        const auto chIt = childrenByEntity_.find(parentNode);
        if (chIt == childrenByEntity_.end()) return;
        for (const auto childId : chIt->second) {
            const auto childSelfIt = entityActiveSelf_.find(childId);
            const bool childSelfActive = (childSelfIt == entityActiveSelf_.end()) ? true : childSelfIt->second;
            if (!childSelfActive) continue;
            const auto childInstIt = instances_.find(childId);
            if (childInstIt != instances_.end()) {
                for (auto& childComp : childInstIt->second) {
                    if (!childComp.enabled) continue;
                    if (!enabled) {
                        cancelActiveInstanceSequences(runtime_, childComp.object, executionOptions_);
                    }
                    const auto& cb = enabled ? childComp.onEnable : childComp.onDisable;
                    (void)invokeLifecycle(
                        childId, childComp, cb, {}, enabled ? "OnEnable" : "OnDisable");
                }
            }
            self(self, childId);
        }
    };
    if (isActiveInHierarchy(getParent(entity)) || getParent(entity) == 0) {
        notifySubtree(notifySubtree, entity);
    }
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
        if (!enabled) {
            cancelActiveInstanceSequences(runtime_, instance.object, executionOptions_);
        }
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
    struct TickItem {
        int order = 0;
        SceneEntityId entity = 0;
        std::size_t compIndex = 0;
        Instance* instance = nullptr;
    };
    std::vector<TickItem> items;
    for (auto& [entity, components] : instances_) {
        if (!isActiveInHierarchy(entity)) continue;
        for (std::size_t idx = 0; idx < components.size(); ++idx) {
            auto& instance = components[idx];
            if (!instance.enabled || instance.started ||
                !shouldTickInCurrentMode(instance)) {
                continue;
            }
            items.push_back(TickItem{instance.executionOrder, entity, idx, &instance});
        }
    }
    std::sort(items.begin(), items.end(), [](const TickItem& a, const TickItem& b) {
        if (a.order != b.order) return a.order < b.order;
        if (a.entity != b.entity) return a.entity < b.entity;
        return a.compIndex < b.compIndex;
    });
    for (auto& item : items) {
        if (invokeLifecycle(item.entity, *item.instance, item.instance->onStart, {}, "OnStart")) {
            item.instance->started = true;
        }
    }
}

void SceneScriptRuntime::update(double deltaTime) {
    struct TickItem {
        int order = 0;
        SceneEntityId entity = 0;
        std::size_t compIndex = 0;
        Instance* instance = nullptr;
    };
    std::vector<TickItem> items;
    for (auto& [entity, components] : instances_) {
        if (!isActiveInHierarchy(entity)) continue;
        for (std::size_t idx = 0; idx < components.size(); ++idx) {
            auto& instance = components[idx];
            if (!instance.enabled || !shouldTickInCurrentMode(instance)) continue;
            if (!instance.started && executionMode_ == SceneExecutionMode::Play) {
                continue;
            }
            items.push_back(TickItem{instance.executionOrder, entity, idx, &instance});
        }
    }
    std::sort(items.begin(), items.end(), [](const TickItem& a, const TickItem& b) {
        if (a.order != b.order) return a.order < b.order;
        if (a.entity != b.entity) return a.entity < b.entity;
        return a.compIndex < b.compIndex;
    });
    for (auto& item : items) {
        (void)invokeLifecycle(
            item.entity, *item.instance, item.instance->onUpdate, {deltaTime}, "OnUpdate");
    }
}

void SceneScriptRuntime::fixedUpdate(double fixedDeltaTime) {
    struct TickItem {
        int order = 0;
        SceneEntityId entity = 0;
        std::size_t compIndex = 0;
        Instance* instance = nullptr;
    };
    std::vector<TickItem> items;
    for (auto& [entity, components] : instances_) {
        if (!isActiveInHierarchy(entity)) continue;
        for (std::size_t idx = 0; idx < components.size(); ++idx) {
            auto& instance = components[idx];
            if (!instance.enabled || !shouldTickInCurrentMode(instance)) continue;
            if (!instance.started && executionMode_ == SceneExecutionMode::Play) {
                continue;
            }
            items.push_back(TickItem{instance.executionOrder, entity, idx, &instance});
        }
    }
    std::sort(items.begin(), items.end(), [](const TickItem& a, const TickItem& b) {
        if (a.order != b.order) return a.order < b.order;
        if (a.entity != b.entity) return a.entity < b.entity;
        return a.compIndex < b.compIndex;
    });
    for (auto& item : items) {
        (void)invokeLifecycle(
            item.entity,
            *item.instance,
            item.instance->onFixedUpdate,
            {fixedDeltaTime},
            "OnFixedUpdate");
    }
}

void SceneScriptRuntime::lateUpdate(double deltaTime) {
    struct TickItem {
        int order = 0;
        SceneEntityId entity = 0;
        std::size_t compIndex = 0;
        Instance* instance = nullptr;
    };
    std::vector<TickItem> items;
    for (auto& [entity, components] : instances_) {
        if (!isActiveInHierarchy(entity)) continue;
        for (std::size_t idx = 0; idx < components.size(); ++idx) {
            auto& instance = components[idx];
            if (!instance.enabled || !shouldTickInCurrentMode(instance)) continue;
            if (!instance.started && executionMode_ == SceneExecutionMode::Play) {
                continue;
            }
            items.push_back(TickItem{instance.executionOrder, entity, idx, &instance});
        }
    }
    std::sort(items.begin(), items.end(), [](const TickItem& a, const TickItem& b) {
        if (a.order != b.order) return a.order < b.order;
        if (a.entity != b.entity) return a.entity < b.entity;
        return a.compIndex < b.compIndex;
    });
    for (auto& item : items) {
        (void)invokeLifecycle(
            item.entity, *item.instance, item.instance->onLateUpdate, {deltaTime}, "OnLateUpdate");
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
        runtime::ObjectRef oldRef;
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
            saved.oldRef = instance.object.reference();
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

    std::unordered_map<std::uint64_t, runtime::ObjectRef> migratedRefMap;

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

            if (saved.oldRef.valid()) {
                const std::uint64_t oldKey =
                    (static_cast<std::uint64_t>(saved.oldRef.generation) << 32u) |
                    saved.oldRef.slot;
                migratedRefMap[oldKey] = newObj->reference();
            }

            for (const auto& newField : newType->descriptor.fields) {
                const bool isInstanceField =
                    !newField.synthetic && !newField.staticField && !newField.constField;
                const auto newFieldTypeId = exactFieldTypeId(
                    newField.type, newField.typeName);
                const auto directIt = saved.fields.find(newField.name);
                if (directIt != saved.fields.end() &&
                    runtime_.valueMatches(
                        directIt->second, newField.type, newFieldTypeId)) {
                    (void)runtime_.heap()->fieldSet(
                        newObj->reference(), newField.index, directIt->second, &allocError);
                    if (isInstanceField) {
                        ++report.fieldsMigrated;
                    }
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
                        if (isInstanceField) {
                            ++report.fieldsMigrated;
                            ++report.fieldsRenamed;
                        }
                        continue;
                    }
                }
                if (isInstanceField) {
                    ++report.fieldsDefaulted;
                }
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

    // Patch cross-component ObjectRef fields and delegate targets to point to migrated instances
    if (!migratedRefMap.empty()) {
        std::unordered_set<std::uint64_t> visited;
        for (auto& [entity, components] : instances_) {
            (void)entity;
            for (auto& instance : components) {
                patchMigratedObjectRefsInHeapObject(
                    runtime_.heap().get(),
                    instance.object.reference(),
                    migratedRefMap,
                    visited);
            }
        }
    }

    report.succeeded = report.errors.empty();
    return report;
}

bool SceneScriptRuntime::validate(SceneEntityId entity) {
    const auto found = instances_.find(entity);
    if (found == instances_.end() || found->second.empty()) return false;
    bool allOk = true;
    for (auto& instance : found->second) {
        if (!invokeLifecycle(entity, instance, instance.onValidate, {}, "OnValidate")) {
            allOk = false;
        }
    }
    return allOk;
}

bool SceneScriptRuntime::validateComponent(
    SceneEntityId entity,
    const std::string& scriptType) {
    const auto found = instances_.find(entity);
    if (found == instances_.end()) return false;
    for (auto& instance : found->second) {
        if (!matchesComponentType(instance.object.type(), scriptType)) continue;
        return invokeLifecycle(entity, instance, instance.onValidate, {}, "OnValidate");
    }
    return false;
}

bool SceneScriptRuntime::setInspectorMember(
    SceneEntityId entity,
    const std::string& scriptType,
    const std::string& memberName,
    runtime::Value value) {
    const auto found = instances_.find(entity);
    if (found == instances_.end()) return false;
    runtime::RuntimeError lastError;
    for (auto& instance : found->second) {
        if (!scriptType.empty() &&
            !matchesComponentType(instance.object.type(), scriptType)) {
            continue;
        }
        runtime::RuntimeError error;
        if (!runtime_.setMember(
                instance.object,
                memberName,
                value,
                error,
                executionOptions_)) {
            if (scriptType.empty()) {
                lastError = std::move(error);
                continue;
            }
            recordError(entity, "SetInspectorMember:" + memberName, error);
            return false;
        }
        if (instance.onValidate) {
            if (!invokeLifecycle(
                    entity, instance, instance.onValidate, {}, "OnValidate")) {
                return false;
            }
        }
        return true;
    }
    if (scriptType.empty() && !found->second.empty() &&
        lastError.code != runtime::ErrorCode::None) {
        recordError(entity, "SetInspectorMember:" + memberName, lastError);
    }
    return false;
}

bool SceneScriptRuntime::connectSignal(
    SceneEntityId sourceEntity,
    const std::string& sourceComponent,
    const std::string& signalName,
    SceneEntityId targetEntity,
    const std::string& targetComponent,
    const std::string& targetMethod) {
    ScriptObject* srcObj = sourceComponent.empty()
        ? object(sourceEntity)
        : getComponent(sourceEntity, sourceComponent);
    ScriptObject* dstObj = targetComponent.empty()
        ? object(targetEntity)
        : getComponent(targetEntity, targetComponent);
    if (!srcObj || !dstObj || signalName.empty() || targetMethod.empty()) {
        return false;
    }

    const std::string backingFieldName = "$event_" + signalName + "_delegate";
    const semantic::FieldSymbol* eventField = nullptr;
    for (const auto& field : srcObj->type().descriptor.fields) {
        if (field.name == backingFieldName &&
            field.type == semantic::PrimitiveType::Object) {
            eventField = &field;
            break;
        }
    }
    if (!eventField) return false;

    const auto delegateType = runtime_.findType(eventField->typeName);
    if (!delegateType || !delegateType->descriptor.delegateType) return false;

    const semantic::FunctionSymbol* invokeSig = nullptr;
    for (const auto& method : delegateType->descriptor.methods) {
        if (method.name == "Invoke") {
            invokeSig = &method;
            break;
        }
    }
    if (!invokeSig) return false;
    const std::size_t arity = invokeSig->parameters.size() > 0
        ? invokeSig->parameters.size() - 1u
        : 0u;

    const auto dstMethod = runtime_.findMethod(dstObj->type(), targetMethod, arity);
    if (!dstMethod || !dstMethod->instance()) return false;

    // Construct leaf delegate object matching Interpreter::NewDelegate layout
    const auto symbolId = dstMethod->descriptor.id;
    const auto lowWord = static_cast<std::int64_t>(symbolId & 0xffffffffULL);
    const auto highWord = static_cast<std::int64_t>((symbolId >> 32u) & 0xffffffffULL);
    std::vector<runtime::Value> leafFields{
        dstObj->value(),
        lowWord,
        highWord,
        std::int64_t{0},
        std::int64_t{0},
        std::int64_t{0},
        std::int64_t{0},
        runtime::NullObject{},
    };
    runtime::RuntimeError error;
    const auto leafRef = runtime_.heap()->allocateObject(
        delegateType->descriptor.id, std::move(leafFields), {0u, 7u}, &error);
    if (!leafRef) return false;

    const auto existingVal = runtime_.heap()->fieldGet(
        srcObj->reference(), eventField->index);
    if (!existingVal || std::holds_alternative<runtime::NullObject>(*existingVal)) {
        return runtime_.heap()->fieldSet(
            srcObj->reference(), eventField->index, *leafRef, &error);
    }

    const auto* existingRef = std::get_if<runtime::ObjectRef>(&*existingVal);
    if (!existingRef || !existingRef->valid()) {
        return runtime_.heap()->fieldSet(
            srcObj->reference(), eventField->index, *leafRef, &error);
    }

    // Combine into a multicast node (flags = 4, field[0] = existing, field[7] = leaf)
    std::vector<runtime::Value> combinedFields{
        *existingRef,
        std::int64_t{0},
        std::int64_t{0},
        std::int64_t{4},
        std::int64_t{0},
        std::int64_t{0},
        std::int64_t{0},
        *leafRef,
    };
    const auto combinedRef = runtime_.heap()->allocateObject(
        delegateType->descriptor.id, std::move(combinedFields), {0u, 7u}, &error);
    if (!combinedRef) return false;

    return runtime_.heap()->fieldSet(
        srcObj->reference(), eventField->index, *combinedRef, &error);
}

std::optional<SceneScriptRuntime::ComponentLocation>
SceneScriptRuntime::findObjectLocation(runtime::ObjectRef reference) const {
    if (!reference.valid()) return std::nullopt;
    for (const auto& [entity, components] : instances_) {
        for (const auto& instance : components) {
            if (instance.object.reference() == reference) {
                return ComponentLocation{
                    entity,
                    instance.object.type().canonicalName(),
                };
            }
        }
    }
    return std::nullopt;
}

bool SceneScriptRuntime::setParent(SceneEntityId child, SceneEntityId parent) {
    if (child == BroadcastTarget || child == parent) return false;
    // Prevent cycle in hierarchy
    SceneEntityId cursor = parent;
    while (cursor != BroadcastTarget) {
        if (cursor == child) return false;
        const auto pIt = parentByEntity_.find(cursor);
        cursor = (pIt == parentByEntity_.end()) ? BroadcastTarget : pIt->second;
    }

    const auto oldParentIt = parentByEntity_.find(child);
    if (oldParentIt != parentByEntity_.end()) {
        const auto oldParent = oldParentIt->second;
        if (oldParent == parent) return true;
        const auto chIt = childrenByEntity_.find(oldParent);
        if (chIt != childrenByEntity_.end()) {
            auto& vec = chIt->second;
            vec.erase(std::remove(vec.begin(), vec.end(), child), vec.end());
        }
        parentByEntity_.erase(oldParentIt);
    }

    if (parent != BroadcastTarget) {
        parentByEntity_[child] = parent;
        auto& children = childrenByEntity_[parent];
        if (std::find(children.begin(), children.end(), child) == children.end()) {
            children.push_back(child);
        }
    }
    return true;
}

SceneEntityId SceneScriptRuntime::getParent(SceneEntityId entity) const noexcept {
    const auto it = parentByEntity_.find(entity);
    return it == parentByEntity_.end() ? BroadcastTarget : it->second;
}

std::vector<SceneEntityId> SceneScriptRuntime::getChildren(SceneEntityId entity) const {
    const auto it = childrenByEntity_.find(entity);
    return it == childrenByEntity_.end() ? std::vector<SceneEntityId>{} : it->second;
}

bool SceneScriptRuntime::isActiveInHierarchy(SceneEntityId entity) const noexcept {
    if (entity == BroadcastTarget) return false;
    SceneEntityId cursor = entity;
    while (cursor != BroadcastTarget) {
        const auto activeIt = entityActiveSelf_.find(cursor);
        if (activeIt != entityActiveSelf_.end() && !activeIt->second) {
            return false;
        }
        const auto instIt = instances_.find(cursor);
        if (instIt != instances_.end() && !instIt->second.empty() &&
            !instIt->second.front().enabled && activeIt == entityActiveSelf_.end()) {
            return false;
        }
        const auto pIt = parentByEntity_.find(cursor);
        cursor = (pIt == parentByEntity_.end()) ? BroadcastTarget : pIt->second;
    }
    return true;
}

void SceneScriptRuntime::setLocalTransform(
    SceneEntityId entity,
    const Transform3D& transform) {
    if (entity == BroadcastTarget) return;
    localTransforms_[entity] = transform;
}

Transform3D SceneScriptRuntime::getLocalTransform(
    SceneEntityId entity) const noexcept {
    const auto it = localTransforms_.find(entity);
    return it == localTransforms_.end() ? Transform3D::identity() : it->second;
}

Transform3D SceneScriptRuntime::getWorldTransform(
    SceneEntityId entity) const noexcept {
    if (entity == BroadcastTarget) return Transform3D::identity();
    std::vector<SceneEntityId> chain;
    SceneEntityId cursor = entity;
    while (cursor != BroadcastTarget) {
        chain.push_back(cursor);
        const auto pIt = parentByEntity_.find(cursor);
        cursor = (pIt == parentByEntity_.end()) ? BroadcastTarget : pIt->second;
    }
    Transform3D world = Transform3D::identity();
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        world = Transform3D::combine(world, getLocalTransform(*it));
    }
    return world;
}

void SceneScriptRuntime::setLocalPosition2D(SceneEntityId entity, Vec2 position) {
    if (entity == BroadcastTarget) return;
    Transform3D t = getLocalTransform(entity);
    t.position = Vec3{position.x, position.y, t.position.z};
    localTransforms_[entity] = t;
}

Vec2 SceneScriptRuntime::getWorldPosition2D(SceneEntityId entity) const noexcept {
    const Transform3D w = getWorldTransform(entity);
    return Vec2{w.position.x, w.position.y};
}

ScriptObject* SceneScriptRuntime::getComponentInChildren(
    SceneEntityId entity,
    const std::string& typeOrInterfaceName) {
    if (auto* selfComp = getComponent(entity, typeOrInterfaceName)) {
        return selfComp;
    }
    const auto chIt = childrenByEntity_.find(entity);
    if (chIt == childrenByEntity_.end()) return nullptr;
    for (const auto child : chIt->second) {
        if (auto* found = getComponentInChildren(child, typeOrInterfaceName)) {
            return found;
        }
    }
    return nullptr;
}

ScriptObject* SceneScriptRuntime::getComponentInParent(
    SceneEntityId entity,
    const std::string& typeOrInterfaceName) {
    SceneEntityId cursor = entity;
    while (cursor != BroadcastTarget) {
        if (auto* comp = getComponent(cursor, typeOrInterfaceName)) {
            return comp;
        }
        const auto pIt = parentByEntity_.find(cursor);
        cursor = (pIt == parentByEntity_.end()) ? BroadcastTarget : pIt->second;
    }
    return nullptr;
}

SceneEntityId SceneScriptRuntime::instantiatePrefab(
    const PrefabNodeTemplate& prefab,
    SceneEntityId& nextEntityId,
    SceneEntityId parent,
    const std::map<std::string, runtime::Value>& rootMemberOverrides) {
    if (nextEntityId == 0) nextEntityId = 1;
    const SceneEntityId entityId = nextEntityId++;
    entityActiveSelf_[entityId] = true;
    localTransforms_[entityId] = prefab.localTransform;
    if (parent != BroadcastTarget) {
        (void)setParent(entityId, parent);
    }
    for (std::size_t i = 0; i < prefab.components.size(); ++i) {
        SceneScriptOptions opts = prefab.components[i].options;
        if (!attachComponent(entityId, prefab.components[i].scriptType, std::move(opts))) {
            (void)detach(entityId);
            return BroadcastTarget;
        }
    }
    for (const auto& [k, v] : rootMemberOverrides) {
        if (!setInspectorMember(entityId, std::string{}, k, v)) {
            (void)detach(entityId);
            return BroadcastTarget;
        }
    }
    for (const auto& childPrefab : prefab.children) {
        if (instantiatePrefab(childPrefab, nextEntityId, entityId) == BroadcastTarget) {
            (void)detach(entityId);
            return BroadcastTarget;
        }
    }
    return entityId;
}

} // namespace realscript::game
