#include <cstring>
#include "realscript/game/GameScripting.h"
#include "realscript/game/GameplayMetadata.h"

#include <algorithm>
#include <set>
#include <unordered_map>
#include <unordered_set>
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
    std::unordered_set<std::uint64_t>& visited);

bool patchMigratedObjectRefsInValue(
    runtime::ManagedHeap* heap,
    runtime::Value& val,
    const std::unordered_map<std::uint64_t, runtime::ObjectRef>& refMap,
    std::unordered_set<std::uint64_t>& visited) {
    if (auto* objRef = std::get_if<runtime::ObjectRef>(&val)) {
        if (!objRef->valid()) return false;
        const std::uint64_t refKey =
            (static_cast<std::uint64_t>(objRef->generation) << 32u) | objRef->slot;
        const auto mapped = refMap.find(refKey);
        if (mapped != refMap.end()) {
            *objRef = mapped->second;
            return true;
        }
        patchMigratedObjectRefsInHeapObject(heap, *objRef, refMap, visited);
        return false;
    }
    if (auto* structVal = std::get_if<runtime::StructValue>(&val)) {
        if (!structVal->storage) return false;
        auto cloned = std::make_shared<runtime::StructStorage>(*structVal->storage);
        bool changed = false;
        for (auto& subField : cloned->fields) {
            if (patchMigratedObjectRefsInValue(heap, subField, refMap, visited)) {
                changed = true;
            }
        }
        if (changed) {
            structVal->storage = std::move(cloned);
        }
        return changed;
    }
    return false;
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

    if (target.kind == runtime::ObjectKind::Array) {
        const auto aLen = heap->arrayLength(target);
        if (!aLen) return;
        for (std::size_t i = 0; i < *aLen; ++i) {
            auto elem = heap->arrayGet(target, i);
            if (!elem) continue;
            if (patchMigratedObjectRefsInValue(heap, *elem, refMap, visited)) {
                (void)heap->arraySet(target, i, *elem);
            }
        }
        return;
    }

    const auto fCount = heap->fieldCount(target);
    if (!fCount) return;
    for (std::size_t i = 0; i < *fCount; ++i) {
        auto val = heap->fieldGet(target, i);
        if (!val) continue;
        if (patchMigratedObjectRefsInValue(heap, *val, refMap, visited)) {
            (void)heap->fieldSet(target, i, *val);
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
    instance.onCollisionEnter = runtime_.findMethod(type, "OnCollisionEnter", 1);
    instance.onCollisionStay = runtime_.findMethod(type, "OnCollisionStay", 1);
    instance.onCollisionExit = runtime_.findMethod(type, "OnCollisionExit", 1);
    instance.onTriggerEnter = runtime_.findMethod(type, "OnTriggerEnter", 1);
    instance.onTriggerStay = runtime_.findMethod(type, "OnTriggerStay", 1);
    instance.onTriggerExit = runtime_.findMethod(type, "OnTriggerExit", 1);
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

void SceneScriptRuntime::rebuildTickOrderIfNeeded() const {
    if (!tickOrderDirty_) return;
    cachedTickOrder_.clear();
    for (const auto& [entity, components] : instances_) {
        for (std::size_t idx = 0; idx < components.size(); ++idx) {
            cachedTickOrder_.push_back(
                TickRef{components[idx].executionOrder, entity, idx});
        }
    }
    std::sort(
        cachedTickOrder_.begin(),
        cachedTickOrder_.end(),
        [](const TickRef& a, const TickRef& b) {
            if (a.order != b.order) return a.order < b.order;
            if (a.entity != b.entity) return a.entity < b.entity;
            return a.compIndex < b.compIndex;
        });
    tickOrderDirty_ = false;
}

void SceneScriptRuntime::invalidateTransformSubtree(SceneEntityId entity) {
    if (entity == BroadcastTarget) return;
    worldTransformCache_.erase(entity);
    const auto chIt = childrenByEntity_.find(entity);
    if (chIt == childrenByEntity_.end()) return;
    for (const auto child : chIt->second) {
        invalidateTransformSubtree(child);
    }
}

bool SceneScriptRuntime::attachComponentWithoutLifecycle(
    SceneEntityId entity,
    const std::string& scriptType,
    SceneScriptOptions options,
    std::size_t& outCompIndex) {
    if (entity == BroadcastTarget) return false;
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
    outCompIndex = list.size();
    list.push_back(std::move(instance));
    tickOrderDirty_ = true;
    return true;
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
    tickOrderDirty_ = true;
    auto& stored = list.back();
    const bool hasOwner = !std::holds_alternative<std::monostate>(options.owner);
    if (hasOwner && stored.onCreate1) {
        if (!invokeLifecycle(
                entity, stored, stored.onCreate1, {std::move(options.owner)}, "OnCreate")) {
            list.pop_back();
            if (list.empty()) instances_.erase(entity);
            tickOrderDirty_ = true;
            return false;
        }
    } else if (!invokeLifecycle(
            entity, stored, stored.onCreate0, {}, "OnCreate")) {
        list.pop_back();
        if (list.empty()) instances_.erase(entity);
        tickOrderDirty_ = true;
        return false;
    }
    if (stored.enabled && isActiveInHierarchy(entity) && !invokeLifecycle(
            entity, stored, stored.onEnable, {}, "OnEnable")) {
        list.pop_back();
        if (list.empty()) instances_.erase(entity);
        tickOrderDirty_ = true;
        return false;
    }
    return true;
}

bool SceneScriptRuntime::detach(SceneEntityId entity) {
    if (entity == BroadcastTarget) return false;
    const auto childrenIt = childrenByEntity_.find(entity);
    if (childrenIt != childrenByEntity_.end()) {
        const auto childrenCopy = childrenIt->second;
        for (const auto child : childrenCopy) {
            (void)detach(child);
        }
        childrenByEntity_.erase(entity);
    }
    const bool wasActiveInHierarchy = isActiveInHierarchy(entity);
    const auto parentIt = parentByEntity_.find(entity);
    const bool hadHierarchyOrTransform =
        parentIt != parentByEntity_.end() ||
        entityActiveSelf_.find(entity) != entityActiveSelf_.end() ||
        localTransforms_.find(entity) != localTransforms_.end();
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
    worldTransformCache_.erase(entity);
    (void)removeEntityCollider2D(entity);

    if (fiberScheduler_) {
        (void)fiberScheduler_->killEntityFibersBySlot(
            static_cast<std::uint32_t>(entity));
    }

    removeSignalsForEntityOrComponent(entity, {});

    const auto found = instances_.find(entity);
    if (found == instances_.end()) {
        return hadHierarchyOrTransform;
    }
    for (auto& instance : found->second) {
        cancelActiveInstanceSequences(runtime_, instance.object, executionOptions_);
        if (instance.enabled && wasActiveInHierarchy) {
            (void)invokeLifecycle(entity, instance, instance.onDisable, {}, "OnDisable");
        }
        (void)invokeLifecycle(entity, instance, instance.onDestroy, {}, "OnDestroy");
    }
    instances_.erase(found);
    tickOrderDirty_ = true;
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
            const std::string canonicalRemoved = it->object.type().canonicalName();
            removeSignalsForEntityOrComponent(entity, canonicalRemoved);
            cancelActiveInstanceSequences(runtime_, it->object, executionOptions_);
            if (it->enabled && isActiveInHierarchy(entity)) {
                (void)invokeLifecycle(entity, *it, it->onDisable, {}, "OnDisable");
            }
            (void)invokeLifecycle(entity, *it, it->onDestroy, {}, "OnDestroy");
            list.erase(it);
            if (list.empty()) instances_.erase(found);
            tickOrderDirty_ = true;
            return true;
        }
    }
    return false;
}

bool SceneScriptRuntime::isEntityActiveSelf(SceneEntityId entity) const noexcept {
    if (entity == BroadcastTarget) return false;
    const auto it = entityActiveSelf_.find(entity);
    if (it != entityActiveSelf_.end()) return it->second;
    const auto instIt = instances_.find(entity);
    if (instIt != instances_.end()) {
        return !instIt->second.empty() && instIt->second.front().enabled;
    }
    return true;
}

bool SceneScriptRuntime::setEntityActive(SceneEntityId entity, bool active) {
    if (entity == BroadcastTarget) return false;
    const bool currentSelf = isEntityActiveSelf(entity);
    if (entityActiveSelf_.find(entity) != entityActiveSelf_.end() &&
        currentSelf == active) {
        return false;
    }
    const bool parentActive =
        getParent(entity) == BroadcastTarget || isActiveInHierarchy(getParent(entity));
    const bool wasActiveInHierarchy = currentSelf && parentActive;

    entityActiveSelf_[entity] = active;
    tickOrderDirty_ = true;

    const bool nowActiveInHierarchy = active && parentActive;
    if (wasActiveInHierarchy == nowActiveInHierarchy) {
        return true;
    }

    const auto notifyNodeComponents = [&](SceneEntityId nodeId, bool becomingActive) -> bool {
        if (!becomingActive && fiberScheduler_) {
            (void)fiberScheduler_->killEntityFibersBySlot(
                static_cast<std::uint32_t>(nodeId));
        }
        const auto instIt = instances_.find(nodeId);
        if (instIt == instances_.end()) return true;
        bool ok = true;
        for (auto& comp : instIt->second) {
            if (!comp.enabled) continue;
            if (!becomingActive) {
                cancelActiveInstanceSequences(runtime_, comp.object, executionOptions_);
            }
            const auto& cb = becomingActive ? comp.onEnable : comp.onDisable;
            if (!invokeLifecycle(
                    nodeId,
                    comp,
                    cb,
                    {},
                    becomingActive ? "OnEnable" : "OnDisable")) {
                ok = false;
            }
        }
        return ok;
    };

    (void)notifyNodeComponents(entity, nowActiveInHierarchy);

    const auto notifySubtree = [&](const auto& self, SceneEntityId parentNode) -> void {
        const auto chIt = childrenByEntity_.find(parentNode);
        if (chIt == childrenByEntity_.end()) return;
        for (const auto childId : chIt->second) {
            if (!isEntityActiveSelf(childId)) continue;
            (void)notifyNodeComponents(childId, nowActiveInHierarchy);
            self(self, childId);
        }
    };
    notifySubtree(notifySubtree, entity);
    return true;
}

bool SceneScriptRuntime::setEnabled(SceneEntityId entity, bool enabled) {
    if (entity == BroadcastTarget) return false;
    const auto found = instances_.find(entity);
    if (found == instances_.end() || found->second.empty()) {
        // Pure transform/hierarchy node without components: delegate to setEntityActive
        return setEntityActive(entity, enabled);
    }
    if (found->second.front().enabled == enabled) {
        return false;
    }
    const bool wasEntityActive = isEntityActiveSelf(entity);
    const bool parentActive =
        getParent(entity) == BroadcastTarget || isActiveInHierarchy(getParent(entity));
    auto& instance = found->second.front();
    if (!enabled) {
        cancelActiveInstanceSequences(runtime_, instance.object, executionOptions_);
        if (fiberScheduler_) {
            (void)fiberScheduler_->killEntityFibersBySlot(
                static_cast<std::uint32_t>(entity));
        }
    }
    const auto& callback = enabled ? instance.onEnable : instance.onDisable;
    if (parentActive) {
        if (!invokeLifecycle(
                entity,
                instance,
                callback,
                {},
                enabled ? "OnEnable" : "OnDisable")) {
            return false;
        }
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
    tickOrderDirty_ = true;

    // Cascade OnEnable / OnDisable to active child entities only if effective hierarchy state changed
    const auto notifySubtree = [&](const auto& self, SceneEntityId parentNode) -> void {
        const auto chIt = childrenByEntity_.find(parentNode);
        if (chIt == childrenByEntity_.end()) return;
        for (const auto childId : chIt->second) {
            if (!isEntityActiveSelf(childId)) continue;
            if (!anyComponentEnabled && fiberScheduler_) {
                (void)fiberScheduler_->killEntityFibersBySlot(
                    static_cast<std::uint32_t>(childId));
            }
            const auto childInstIt = instances_.find(childId);
            if (childInstIt != instances_.end()) {
                for (auto& childComp : childInstIt->second) {
                    if (!childComp.enabled) continue;
                    if (!anyComponentEnabled) {
                        cancelActiveInstanceSequences(runtime_, childComp.object, executionOptions_);
                    }
                    const auto& cb = anyComponentEnabled ? childComp.onEnable : childComp.onDisable;
                    (void)invokeLifecycle(
                        childId,
                        childComp,
                        cb,
                        {},
                        anyComponentEnabled ? "OnEnable" : "OnDisable");
                }
            }
            self(self, childId);
        }
    };
    if (parentActive && wasEntityActive != anyComponentEnabled) {
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
        if (isActiveInHierarchy(entity)) {
            if (!invokeLifecycle(
                    entity,
                    instance,
                    callback,
                    {},
                    enabled ? "OnEnable" : "OnDisable")) {
                return false;
            }
        }
        instance.enabled = enabled;
        tickOrderDirty_ = true;
        return true;
    }
    return false;
}

bool SceneScriptRuntime::isComponentEnabled(
    SceneEntityId entity,
    const std::string& scriptType) const noexcept {
    const auto found = instances_.find(entity);
    if (found == instances_.end()) return false;
    for (const auto& instance : found->second) {
        if (matchesComponentType(instance.object.type(), scriptType)) {
            return instance.enabled;
        }
    }
    return false;
}

void SceneScriptRuntime::start() {
    rebuildTickOrderIfNeeded();
    const auto snapshot = cachedTickOrder_;
    for (const auto& ref : snapshot) {
        if (!isActiveInHierarchy(ref.entity)) continue;
        const auto it = instances_.find(ref.entity);
        if (it == instances_.end() || ref.compIndex >= it->second.size()) continue;
        auto& instance = it->second[ref.compIndex];
        if (!instance.enabled || instance.started ||
            !shouldTickInCurrentMode(instance)) {
            continue;
        }
        if (invokeLifecycle(ref.entity, instance, instance.onStart, {}, "OnStart")) {
            instance.started = true;
        }
    }
}

void SceneScriptRuntime::update(double deltaTime) {
    rebuildTickOrderIfNeeded();
    const auto snapshot = cachedTickOrder_;
    for (const auto& ref : snapshot) {
        if (!isActiveInHierarchy(ref.entity)) continue;
        const auto it = instances_.find(ref.entity);
        if (it == instances_.end() || ref.compIndex >= it->second.size()) continue;
        auto& instance = it->second[ref.compIndex];
        if (!instance.enabled || !shouldTickInCurrentMode(instance)) continue;
        if (!instance.started && executionMode_ == SceneExecutionMode::Play) {
            continue;
        }
        (void)invokeLifecycle(
            ref.entity, instance, instance.onUpdate, {deltaTime}, "OnUpdate");
    }
}

void SceneScriptRuntime::fixedUpdate(double fixedDeltaTime) {
    rebuildTickOrderIfNeeded();
    const auto snapshot = cachedTickOrder_;
    for (const auto& ref : snapshot) {
        if (!isActiveInHierarchy(ref.entity)) continue;
        const auto it = instances_.find(ref.entity);
        if (it == instances_.end() || ref.compIndex >= it->second.size()) continue;
        auto& instance = it->second[ref.compIndex];
        if (!instance.enabled || !shouldTickInCurrentMode(instance)) continue;
        if (!instance.started && executionMode_ == SceneExecutionMode::Play) {
            continue;
        }
        (void)invokeLifecycle(
            ref.entity,
            instance,
            instance.onFixedUpdate,
            {fixedDeltaTime},
            "OnFixedUpdate");
    }
}

void SceneScriptRuntime::lateUpdate(double deltaTime) {
    rebuildTickOrderIfNeeded();
    const auto snapshot = cachedTickOrder_;
    for (const auto& ref : snapshot) {
        if (!isActiveInHierarchy(ref.entity)) continue;
        const auto it = instances_.find(ref.entity);
        if (it == instances_.end() || ref.compIndex >= it->second.size()) continue;
        auto& instance = it->second[ref.compIndex];
        if (!instance.enabled || !shouldTickInCurrentMode(instance)) continue;
        if (!instance.started && executionMode_ == SceneExecutionMode::Play) {
            continue;
        }
        (void)invokeLifecycle(
            ref.entity, instance, instance.onLateUpdate, {deltaTime}, "OnLateUpdate");
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
    tickOrderDirty_ = true;

    std::unordered_map<std::uint64_t, runtime::ObjectRef> migratedRefMap;
    std::unordered_map<std::string, std::optional<ScriptObject>> prototypeCache;

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
                    auto protoIt = prototypeCache.find(saved.canonicalTypeName);
                    if (protoIt == prototypeCache.end()) {
                        runtime::RuntimeError protoErr;
                        protoIt = prototypeCache
                                      .emplace(
                                          saved.canonicalTypeName,
                                          runtime_.createPrototypeObject(*newType, protoErr))
                                      .first;
                    }
                    if (protoIt->second) {
                        if (auto protoVal = runtime_.heap()->fieldGet(
                                protoIt->second->reference(), newField.index)) {
                            (void)runtime_.heap()->fieldSet(
                                newObj->reference(), newField.index, *protoVal, &allocError);
                        }
                    }
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

    // Patch cross-component ObjectRef fields (including inside Arrays and Structs) and delegate targets
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

    // Rebind wired signal connections to updated method SymbolIds and migrated ObjectRefs
    if (!signalConnections_.empty()) {
        std::set<std::tuple<SceneEntityId, std::string, std::string>> uniqueSources;
        for (const auto& conn : signalConnections_) {
            uniqueSources.emplace(conn.sourceEntity, conn.sourceComponent, conn.signalName);
        }
        for (const auto& [srcEnt, srcComp, sigName] : uniqueSources) {
            if (rebuildSignalDelegateChain(srcEnt, srcComp, sigName)) {
                ++report.signalsRebound;
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

bool SceneScriptRuntime::rebuildSignalDelegateChain(
    SceneEntityId sourceEntity,
    const std::string& sourceComponent,
    const std::string& signalName) {
    ScriptObject* srcObj = sourceComponent.empty()
        ? object(sourceEntity)
        : getComponent(sourceEntity, sourceComponent);
    if (!srcObj || signalName.empty()) return false;

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

    runtime::RuntimeError error;
    std::optional<runtime::ObjectRef> chainHead;

    for (const auto& conn : signalConnections_) {
        if (conn.sourceEntity != sourceEntity || conn.signalName != signalName) {
            continue;
        }
        if (!sourceComponent.empty() && !conn.sourceComponent.empty() &&
            !matchesComponentType(srcObj->type(), conn.sourceComponent)) {
            continue;
        }
        ScriptObject* dstObj = conn.targetComponent.empty()
            ? object(conn.targetEntity)
            : getComponent(conn.targetEntity, conn.targetComponent);
        if (!dstObj) continue;

        const auto dstMethod = runtime_.findMethod(dstObj->type(), conn.targetMethod, arity);
        if (!dstMethod || !dstMethod->instance()) continue;

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
        const auto leafRef = runtime_.heap()->allocateObject(
            delegateType->descriptor.id, std::move(leafFields), {0u, 7u}, &error);
        if (!leafRef) return false;

        if (!chainHead) {
            chainHead = *leafRef;
        } else {
            std::vector<runtime::Value> combinedFields{
                *chainHead,
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
            chainHead = *combinedRef;
        }
    }

    if (!chainHead) {
        return runtime_.heap()->fieldSet(
            srcObj->reference(), eventField->index, runtime::NullObject{}, &error);
    }
    return runtime_.heap()->fieldSet(
        srcObj->reference(), eventField->index, *chainHead, &error);
}

void SceneScriptRuntime::removeSignalsForEntityOrComponent(
    SceneEntityId entity,
    const std::string& removedComponentType) {
    if (signalConnections_.empty()) return;

    const auto matchesRemoved = [&](SceneEntityId ent, const std::string& compName) {
        if (ent != entity) return false;
        if (removedComponentType.empty()) return true;
        if (compName.empty()) return false;
        if (compName == removedComponentType) return true;
        if (const auto* compObj = getComponent(entity, compName)) {
            return compObj->type().canonicalName() == removedComponentType;
        }
        return false;
    };

    std::set<std::tuple<SceneEntityId, std::string, std::string>> affectedSources;
    for (auto it = signalConnections_.begin(); it != signalConnections_.end();) {
        const bool srcRemoved = matchesRemoved(it->sourceEntity, it->sourceComponent);
        const bool dstRemoved = matchesRemoved(it->targetEntity, it->targetComponent);
        if (srcRemoved || dstRemoved) {
            if (!srcRemoved) {
                affectedSources.emplace(
                    it->sourceEntity, it->sourceComponent, it->signalName);
            }
            it = signalConnections_.erase(it);
        } else {
            ++it;
        }
    }

    for (const auto& [srcEnt, srcComp, sigName] : affectedSources) {
        (void)rebuildSignalDelegateChain(srcEnt, srcComp, sigName);
    }
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

    const std::string resolvedSrcComp = srcObj->type().canonicalName();
    const std::string resolvedDstComp = dstObj->type().canonicalName();

    for (const auto& existing : signalConnections_) {
        if (existing.sourceEntity == sourceEntity &&
            existing.sourceComponent == resolvedSrcComp &&
            existing.signalName == signalName &&
            existing.targetEntity == targetEntity &&
            existing.targetComponent == resolvedDstComp &&
            existing.targetMethod == targetMethod) {
            return true;
        }
    }

    signalConnections_.push_back(SignalConnection{
        sourceEntity,
        resolvedSrcComp,
        signalName,
        targetEntity,
        resolvedDstComp,
        targetMethod,
    });
    return rebuildSignalDelegateChain(sourceEntity, resolvedSrcComp, signalName);
}

bool SceneScriptRuntime::disconnectSignal(
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
    const std::string resolvedSrcComp = srcObj ? srcObj->type().canonicalName() : sourceComponent;
    const std::string resolvedDstComp = dstObj ? dstObj->type().canonicalName() : targetComponent;

    bool removedAny = false;
    for (auto it = signalConnections_.begin(); it != signalConnections_.end();) {
        const bool srcMatch = it->sourceEntity == sourceEntity &&
            (sourceComponent.empty() || it->sourceComponent == resolvedSrcComp ||
             it->sourceComponent == sourceComponent);
        const bool sigMatch = it->signalName == signalName;
        const bool dstMatch = it->targetEntity == targetEntity &&
            (targetComponent.empty() || it->targetComponent == resolvedDstComp ||
             it->targetComponent == targetComponent) &&
            (targetMethod.empty() || it->targetMethod == targetMethod);
        if (srcMatch && sigMatch && dstMatch) {
            it = signalConnections_.erase(it);
            removedAny = true;
        } else {
            ++it;
        }
    }
    if (!removedAny) return false;
    return rebuildSignalDelegateChain(sourceEntity, resolvedSrcComp, signalName);
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

    if (entityActiveSelf_.find(child) == entityActiveSelf_.end()) {
        entityActiveSelf_[child] = true;
    }
    if (parent != BroadcastTarget &&
        entityActiveSelf_.find(parent) == entityActiveSelf_.end()) {
        entityActiveSelf_[parent] = true;
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
    invalidateTransformSubtree(child);
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
    if (entityActiveSelf_.find(entity) == entityActiveSelf_.end()) {
        entityActiveSelf_[entity] = true;
    }
    localTransforms_[entity] = transform;
    invalidateTransformSubtree(entity);
}

Transform3D SceneScriptRuntime::getLocalTransform(
    SceneEntityId entity) const noexcept {
    const auto it = localTransforms_.find(entity);
    return it == localTransforms_.end() ? Transform3D::identity() : it->second;
}

Transform3D SceneScriptRuntime::getWorldTransform(
    SceneEntityId entity) const noexcept {
    if (entity == BroadcastTarget) return Transform3D::identity();
    const auto cachedIt = worldTransformCache_.find(entity);
    if (cachedIt != worldTransformCache_.end()) {
        return cachedIt->second;
    }
    const SceneEntityId parentId = getParent(entity);
    const Transform3D world = (parentId == BroadcastTarget)
        ? getLocalTransform(entity)
        : Transform3D::combine(getWorldTransform(parentId), getLocalTransform(entity));
    worldTransformCache_[entity] = world;
    return world;
}

void SceneScriptRuntime::setLocalPosition2D(SceneEntityId entity, Vec2 position) {
    if (entity == BroadcastTarget) return;
    if (entityActiveSelf_.find(entity) == entityActiveSelf_.end()) {
        entityActiveSelf_[entity] = true;
    }
    Transform3D t = getLocalTransform(entity);
    t.position = Vec3{position.x, position.y, t.position.z};
    localTransforms_[entity] = t;
    invalidateTransformSubtree(entity);
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

    struct PendingActivation {
        int order = 0;
        SceneEntityId entity = 0;
        std::size_t compIndex = 0;
        runtime::Value owner;
    };
    std::vector<PendingActivation> activations;

    // Phase 1: Allocate all entities, transforms, hierarchy links, and component instances
    const auto allocateTree = [&](
        const auto& self,
        const PrefabNodeTemplate& node,
        SceneEntityId parentId,
        bool isRoot) -> SceneEntityId {
        const SceneEntityId entityId = nextEntityId++;
        entityActiveSelf_[entityId] = true;
        localTransforms_[entityId] = node.localTransform;
        if (parentId != BroadcastTarget) {
            (void)setParent(entityId, parentId);
        }
        for (std::size_t i = 0; i < node.components.size(); ++i) {
            SceneScriptOptions opts = node.components[i].options;
            runtime::Value ownerCopy = opts.owner;
            std::size_t compIdx = 0;
            if (!attachComponentWithoutLifecycle(
                    entityId, node.components[i].scriptType, std::move(opts), compIdx)) {
                return BroadcastTarget;
            }
            const int order = instances_[entityId][compIdx].executionOrder;
            activations.push_back(PendingActivation{
                order, entityId, compIdx, std::move(ownerCopy)});
        }
        // Validate [RequireComponent] after all components on this entity have been allocated
        const auto instIt = instances_.find(entityId);
        if (instIt != instances_.end()) {
            for (const auto& comp : instIt->second) {
                for (const auto& reqType : findRequiredComponents(
                         runtime_.program().get(), comp.object.type().canonicalName())) {
                    if (!hasComponent(entityId, reqType)) {
                        runtime::RuntimeError error;
                        error.code = runtime::ErrorCode::InvalidArguments;
                        error.message = "missing required component '" + reqType +
                            "' for '" + comp.object.type().canonicalName() + "'";
                        recordError(entityId, "RequireComponent", error);
                        return BroadcastTarget;
                    }
                }
            }
        }
        if (isRoot && !rootMemberOverrides.empty()) {
            for (const auto& [k, v] : rootMemberOverrides) {
                bool applied = false;
                if (instIt != instances_.end()) {
                    for (auto& comp : instIt->second) {
                        runtime::RuntimeError err;
                        if (runtime_.setMember(comp.object, k, v, err, executionOptions_)) {
                            applied = true;
                            break;
                        }
                    }
                }
                if (!applied) {
                    runtime::RuntimeError err;
                    err.code = runtime::ErrorCode::InvalidArguments;
                    err.message = "override member '" + k + "' not found on root prefab entity";
                    recordError(entityId, "SetInspectorMember:" + k, err);
                    return BroadcastTarget;
                }
            }
        }
        for (const auto& childPrefab : node.children) {
            if (self(self, childPrefab, entityId, false) == BroadcastTarget) {
                return BroadcastTarget;
            }
        }
        return entityId;
    };

    const SceneEntityId rootEntityId = allocateTree(allocateTree, prefab, parent, true);
    if (rootEntityId == BroadcastTarget) {
        return BroadcastTarget;
    }

    // Phase 2: Sort all newly instantiated components by (ExecutionOrder, EntityId, CompIndex)
    // and invoke OnCreate followed by OnEnable
    std::sort(
        activations.begin(),
        activations.end(),
        [](const PendingActivation& a, const PendingActivation& b) {
            if (a.order != b.order) return a.order < b.order;
            if (a.entity != b.entity) return a.entity < b.entity;
            return a.compIndex < b.compIndex;
        });

    for (auto& item : activations) {
        auto& stored = instances_[item.entity][item.compIndex];
        const bool hasOwner = !std::holds_alternative<std::monostate>(item.owner);
        if (hasOwner && stored.onCreate1) {
            if (!invokeLifecycle(
                    item.entity, stored, stored.onCreate1, {std::move(item.owner)}, "OnCreate")) {
                (void)detach(rootEntityId);
                return BroadcastTarget;
            }
        } else if (!invokeLifecycle(
                item.entity, stored, stored.onCreate0, {}, "OnCreate")) {
            (void)detach(rootEntityId);
            return BroadcastTarget;
        }
        if (stored.onValidate && !rootMemberOverrides.empty() && item.entity == rootEntityId) {
            if (!invokeLifecycle(
                    item.entity, stored, stored.onValidate, {}, "OnValidate")) {
                (void)detach(rootEntityId);
                return BroadcastTarget;
            }
        }
    }

    for (auto& item : activations) {
        auto& stored = instances_[item.entity][item.compIndex];
        if (stored.enabled && isActiveInHierarchy(item.entity)) {
            if (!invokeLifecycle(
                    item.entity, stored, stored.onEnable, {}, "OnEnable")) {
                (void)detach(rootEntityId);
                return BroadcastTarget;
            }
        }
    }

    tickOrderDirty_ = true;
    invalidateTransformSubtree(rootEntityId);
    return rootEntityId;
}

std::uint32_t SceneScriptRuntime::spawnEntityFiber(
    SceneEntityId entity,
    const runtime::CompiledFiberModule& module,
    const std::string& functionName,
    std::uint8_t ownerGroupId,
    std::uint32_t seed) {
    if (!fiberScheduler_ || entity == BroadcastTarget || !isActiveInHierarchy(entity)) {
        return 0;
    }
    return fiberScheduler_->spawnFunction(
        module,
        functionName,
        runtime::GenerationalEntityHandle{static_cast<std::uint32_t>(entity), 1u},
        ownerGroupId,
        0u,
        seed);
}

std::uint64_t SceneScriptRuntime::stepFibers(std::uint32_t currentTick) {
    if (!fiberScheduler_) return 0;
    return fiberScheduler_->stepTick(currentTick);
}

std::size_t SceneScriptRuntime::emitSignal(
    SceneEntityId sourceEntity,
    const std::string& sourceComponent,
    const std::string& signalName,
    const std::vector<runtime::Value>& arguments) {
    if (sourceEntity == BroadcastTarget || signalName.empty()) return 0;
    ScriptObject* srcObj = sourceComponent.empty()
        ? object(sourceEntity)
        : getComponent(sourceEntity, sourceComponent);
    const std::string resolvedSrcComp = srcObj ? srcObj->type().canonicalName() : sourceComponent;

    std::size_t invokedCount = 0;
    const auto connectionsSnapshot = signalConnections_;
    for (const auto& conn : connectionsSnapshot) {
        if (conn.sourceEntity != sourceEntity || conn.signalName != signalName) {
            continue;
        }
        if (!sourceComponent.empty() && conn.sourceComponent != resolvedSrcComp &&
            conn.sourceComponent != sourceComponent) {
            continue;
        }
        const auto res = invokeComponent(
            conn.targetEntity, conn.targetComponent, conn.targetMethod, arguments);
        if (res.succeeded) ++invokedCount;
    }

    if (signalTopicPublisher_) {
        signalTopicPublisher_("$signal::" + std::to_string(sourceEntity) + "::" + signalName);
        signalTopicPublisher_(signalName);
        ++invokedCount;
    }
    return invokedCount;
}

void SceneScriptRuntime::setInputAction(const std::string& action, bool pressed) {
    if (action.empty()) return;
    currentInputActions_[action] = pressed;
}

void SceneScriptRuntime::setInputAxis2D(const std::string& axisName, Vec2 value) {
    if (axisName.empty()) return;
    inputAxes2D_[axisName] = value;
}

void SceneScriptRuntime::advanceInputTick() {
    previousInputActions_ = currentInputActions_;
}

bool SceneScriptRuntime::isInputActionPressed(const std::string& action) const noexcept {
    const auto it = currentInputActions_.find(action);
    return it != currentInputActions_.end() && it->second;
}

bool SceneScriptRuntime::isInputActionJustPressed(const std::string& action) const noexcept {
    const bool curr = isInputActionPressed(action);
    const auto prevIt = previousInputActions_.find(action);
    const bool prev = prevIt != previousInputActions_.end() && prevIt->second;
    return curr && !prev;
}

bool SceneScriptRuntime::isInputActionJustReleased(const std::string& action) const noexcept {
    const bool curr = isInputActionPressed(action);
    const auto prevIt = previousInputActions_.find(action);
    const bool prev = prevIt != previousInputActions_.end() && prevIt->second;
    return !curr && prev;
}

Vec2 SceneScriptRuntime::getInputAxis2D(const std::string& axisName) const noexcept {
    const auto it = inputAxes2D_.find(axisName);
    return it != inputAxes2D_.end() ? it->second : Vec2{};
}

std::uint64_t SceneScriptRuntime::computeInputDigest() const noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    const auto mixByte = [&](std::uint8_t b) {
        hash ^= b;
        hash *= 1099511628211ULL;
    };
    const auto mixU32 = [&](std::uint32_t v) {
        mixByte(static_cast<std::uint8_t>(v & 0xFFu));
        mixByte(static_cast<std::uint8_t>((v >> 8u) & 0xFFu));
        mixByte(static_cast<std::uint8_t>((v >> 16u) & 0xFFu));
        mixByte(static_cast<std::uint8_t>((v >> 24u) & 0xFFu));
    };
    const auto mixStr = [&](const std::string& s) {
        for (const unsigned char ch : s) mixByte(ch);
        mixByte(0xFFu);
    };
    for (const auto& [name, state] : currentInputActions_) {
        mixStr(name);
        mixByte(state ? 1u : 0u);
        const auto prevIt = previousInputActions_.find(name);
        const bool prev = prevIt != previousInputActions_.end() && prevIt->second;
        mixByte(prev ? 1u : 0u);
    }
    for (const auto& [axis, vec] : inputAxes2D_) {
        mixStr(axis);
        mixU32(static_cast<std::uint32_t>(vec.x.raw));
        mixU32(static_cast<std::uint32_t>(vec.y.raw));
    }
    return hash;
}

void SceneScriptRuntime::setEntityCollider2D(SceneEntityId entity, SceneCollider2D collider) {
    if (entity == BroadcastTarget) return;
    if (entityActiveSelf_.find(entity) == entityActiveSelf_.end()) {
        entityActiveSelf_[entity] = true;
    }
    colliders2D_[entity] = collider;
}

bool SceneScriptRuntime::removeEntityCollider2D(SceneEntityId entity) {
    for (auto it = activeOverlaps2D_.begin(); it != activeOverlaps2D_.end();) {
        if (it->first == entity || it->second == entity) {
            activeOverlapWasTrigger2D_.erase(*it);
            it = activeOverlaps2D_.erase(it);
        } else {
            ++it;
        }
    }
    return colliders2D_.erase(entity) != 0;
}

namespace {

constexpr Fixed fixedAbs(Fixed v) noexcept {
    return v.raw < 0 ? Fixed::fromRaw(-v.raw) : v;
}

constexpr Fixed fixedClamp(Fixed v, Fixed lo, Fixed hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

constexpr Fixed fixedSqrt(Fixed v) noexcept {
    if (v.raw <= 0) return Fixed::fromRaw(0);
    return Fixed::fromRaw(static_cast<std::int32_t>(
        deterministicSqrt64(static_cast<std::int64_t>(v.raw) << 8)));
}

bool testOverlap2D(
    Vec2 posA, const SceneCollider2D& colA,
    Vec2 posB, const SceneCollider2D& colB) noexcept {
    if ((colA.layerMask & colB.layerMask) == 0) return false;
    if (colA.isCircle && colB.isCircle) {
        const Vec2 delta = posB - posA;
        const Fixed sumR = colA.radius + colB.radius;
        return delta.lengthSq() <= (sumR * sumR);
    }
    if (!colA.isCircle && !colB.isCircle) {
        const Fixed dx = fixedAbs(posB.x - posA.x);
        const Fixed dy = fixedAbs(posB.y - posA.y);
        return dx <= (colA.halfExtents.x + colB.halfExtents.x) &&
               dy <= (colA.halfExtents.y + colB.halfExtents.y);
    }
    const Vec2 circlePos = colA.isCircle ? posA : posB;
    const Fixed radius = colA.isCircle ? colA.radius : colB.radius;
    const Vec2 boxPos = colA.isCircle ? posB : posA;
    const Vec2 halfExt = colA.isCircle ? colB.halfExtents : colA.halfExtents;
    const Fixed clampedX = fixedClamp(
        circlePos.x, boxPos.x - halfExt.x, boxPos.x + halfExt.x);
    const Fixed clampedY = fixedClamp(
        circlePos.y, boxPos.y - halfExt.y, boxPos.y + halfExt.y);
    const Vec2 diff{circlePos.x - clampedX, circlePos.y - clampedY};
    return diff.lengthSq() <= (radius * radius);
}

} // namespace

std::optional<RaycastHit2D> SceneScriptRuntime::raycast2D(
    Vec2 origin,
    Vec2 direction,
    Fixed maxDistance,
    std::uint32_t layerMask) const {
    const Fixed dirLen = direction.length();
    if (dirLen == Fixed::fromRaw(0) || maxDistance <= Fixed::fromRaw(0)) {
        return std::nullopt;
    }
    const Vec2 dir = direction / dirLen;

    std::optional<RaycastHit2D> closest;
    for (const auto& [entity, col] : colliders2D_) {
        if ((col.layerMask & layerMask) == 0 || !isActiveInHierarchy(entity)) {
            continue;
        }
        const Vec2 center = getWorldPosition2D(entity);
        if (col.isCircle) {
            const Vec2 toCenter = center - origin;
            const Fixed proj = Vec2::dot(toCenter, dir);
            if (proj < -col.radius || proj > maxDistance + col.radius) continue;
            const Vec2 closestPt = origin + dir * proj;
            const Fixed distSq = (center - closestPt).lengthSq();
            const Fixed rSq = col.radius * col.radius;
            if (distSq > rSq) continue;
            const Fixed offset = fixedSqrt(rSq - distSq);
            if (proj + offset < Fixed::fromRaw(0)) continue;
            Fixed hitDist = proj - offset;
            if (hitDist < Fixed::fromRaw(0)) hitDist = Fixed::fromRaw(0);
            if (hitDist > maxDistance) continue;
            if (!closest || hitDist < closest->distance ||
                (hitDist == closest->distance && entity < closest->entity)) {
                closest = RaycastHit2D{entity, origin + dir * hitDist, hitDist};
            }
        } else {
            const Vec2 minB = center - col.halfExtents;
            const Vec2 maxB = center + col.halfExtents;
            Fixed tMin = Fixed::fromRaw(0);
            Fixed tMax = maxDistance;
            bool hit = true;
            for (int axis = 0; axis < 2; ++axis) {
                const Fixed o = axis == 0 ? origin.x : origin.y;
                const Fixed d = axis == 0 ? dir.x : dir.y;
                const Fixed bMin = axis == 0 ? minB.x : minB.y;
                const Fixed bMax = axis == 0 ? maxB.x : maxB.y;
                if (d == Fixed::fromRaw(0)) {
                    if (o < bMin || o > bMax) {
                        hit = false;
                        break;
                    }
                } else {
                    Fixed t1 = (bMin - o) / d;
                    Fixed t2 = (bMax - o) / d;
                    if (t1 > t2) std::swap(t1, t2);
                    if (t1 > tMin) tMin = t1;
                    if (t2 < tMax) tMax = t2;
                    if (tMin > tMax) {
                        hit = false;
                        break;
                    }
                }
            }
            if (!hit || tMin > maxDistance) continue;
            if (!closest || tMin < closest->distance ||
                (tMin == closest->distance && entity < closest->entity)) {
                closest = RaycastHit2D{entity, origin + dir * tMin, tMin};
            }
        }
    }
    return closest;
}

std::vector<SceneEntityId> SceneScriptRuntime::overlapCircle2D(
    Vec2 center,
    Fixed radius,
    std::uint32_t layerMask) const {
    std::vector<SceneEntityId> result;
    SceneCollider2D queryCircle;
    queryCircle.isCircle = true;
    queryCircle.radius = radius;
    queryCircle.layerMask = layerMask;
    for (const auto& [entity, col] : colliders2D_) {
        if (!isActiveInHierarchy(entity)) continue;
        if (testOverlap2D(center, queryCircle, getWorldPosition2D(entity), col)) {
            result.push_back(entity);
        }
    }
    return result;
}

std::size_t SceneScriptRuntime::stepPhysics2D() {
    std::set<std::pair<SceneEntityId, SceneEntityId>> currentOverlaps;
    std::map<std::pair<SceneEntityId, SceneEntityId>, bool> currentIsTrigger;

    for (auto itA = colliders2D_.begin(); itA != colliders2D_.end(); ++itA) {
        if (!isActiveInHierarchy(itA->first)) continue;
        const Vec2 posA = getWorldPosition2D(itA->first);
        auto itB = itA;
        ++itB;
        for (; itB != colliders2D_.end(); ++itB) {
            if (!isActiveInHierarchy(itB->first)) continue;
            const Vec2 posB = getWorldPosition2D(itB->first);
            if (testOverlap2D(posA, itA->second, posB, itB->second)) {
                const auto pair = std::make_pair(itA->first, itB->first);
                currentOverlaps.insert(pair);
                currentIsTrigger[pair] = itA->second.isTrigger || itB->second.isTrigger;
            }
        }
    }

    std::size_t callbacksFired = 0;
    const auto dispatchPhysicsCallback = [&](
        SceneEntityId selfEntity,
        SceneEntityId otherEntity,
        bool isTrigger,
        int phase /* 0=Enter, 1=Stay, 2=Exit */) {
        const auto instIt = instances_.find(selfEntity);
        if (instIt == instances_.end()) return;
        for (auto& instance : instIt->second) {
            if (!instance.enabled || !shouldTickInCurrentMode(instance)) continue;
            const std::optional<ScriptMethod>* slot = nullptr;
            const char* cbName = "";
            if (isTrigger) {
                if (phase == 0) { slot = &instance.onTriggerEnter; cbName = "OnTriggerEnter"; }
                else if (phase == 1) { slot = &instance.onTriggerStay; cbName = "OnTriggerStay"; }
                else { slot = &instance.onTriggerExit; cbName = "OnTriggerExit"; }
            } else {
                if (phase == 0) { slot = &instance.onCollisionEnter; cbName = "OnCollisionEnter"; }
                else if (phase == 1) { slot = &instance.onCollisionStay; cbName = "OnCollisionStay"; }
                else { slot = &instance.onCollisionExit; cbName = "OnCollisionExit"; }
            }
            if (!slot || !*slot) continue;
            const auto& param = (*slot)->descriptor.parameters.back();
            runtime::Value argVal = param.type == semantic::PrimitiveType::Long
                ? runtime::Value{runtime::LongValue{static_cast<std::int64_t>(otherEntity)}}
                : runtime::Value{static_cast<std::int64_t>(otherEntity)};
            if (invokeLifecycle(selfEntity, instance, *slot, {std::move(argVal)}, cbName)) {
                ++callbacksFired;
            }
        }
    };

    // Enter & Stay
    for (const auto& pair : currentOverlaps) {
        const bool isTrig = currentIsTrigger[pair];
        const int phase = activeOverlaps2D_.find(pair) == activeOverlaps2D_.end() ? 0 : 1;
        dispatchPhysicsCallback(pair.first, pair.second, isTrig, phase);
        dispatchPhysicsCallback(pair.second, pair.first, isTrig, phase);
    }
    // Exit
    for (const auto& pair : activeOverlaps2D_) {
        if (currentOverlaps.find(pair) == currentOverlaps.end()) {
            const auto trigIt = activeOverlapWasTrigger2D_.find(pair);
            const bool isTrig = trigIt != activeOverlapWasTrigger2D_.end() && trigIt->second;
            dispatchPhysicsCallback(pair.first, pair.second, isTrig, 2);
            dispatchPhysicsCallback(pair.second, pair.first, isTrig, 2);
        }
    }

    activeOverlaps2D_ = std::move(currentOverlaps);
    activeOverlapWasTrigger2D_ = std::move(currentIsTrigger);
    return callbacksFired;
}

} // namespace realscript::game
