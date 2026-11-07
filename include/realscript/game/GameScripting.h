#pragma once

#include "realscript/aot_cpp/AotRuntime.h"
#include "realscript/game/GameBindings.h"
#include "realscript/jit/Jit.h"
#include "realscript/runtime/FiberScheduler.h"

namespace realscript::game {

class ScriptMetadataRegistry;

enum class ScriptExecutionBackend {
    Interpreter,
    Jit,
    Aot,
};

enum class SceneExecutionMode {
    Play,
    Editor,
};

struct GameLanguageMetadata {
    std::vector<compiler::LanguageAttributeRecord> attributes;
    std::vector<compiler::LanguageInterfaceImplementation> interfaces;
    std::vector<compiler::LanguageGenericInstantiation> genericInstantiations;
    std::vector<compiler::LanguageSequenceRecord> sequences;

    [[nodiscard]] bool empty() const noexcept {
        return attributes.empty() && interfaces.empty() &&
            genericInstantiations.empty() && sequences.empty();
    }
};

class GameProgram {
public:
    GameProgram() = default;

    [[nodiscard]] bool valid() const noexcept { return program_ != nullptr; }
    [[nodiscard]] std::shared_ptr<const runtime::ProgramImage> program() const noexcept {
        return program_;
    }
    [[nodiscard]] std::shared_ptr<const runtime::BindingRegistry> bindings() const noexcept {
        return bindings_;
    }
    [[nodiscard]] std::shared_ptr<runtime::ManagedHeap> heap() const noexcept {
        return heap_;
    }
    [[nodiscard]] std::shared_ptr<runtime::NativeHandleRegistry> nativeHandles() const noexcept {
        return nativeHandles_;
    }
    [[nodiscard]] const GameLanguageMetadata& languageMetadata() const noexcept {
        return languageMetadata_;
    }

private:
    friend class GameScriptCompiler;
    std::shared_ptr<const runtime::ProgramImage> program_;
    std::shared_ptr<const runtime::BindingRegistry> bindings_;
    std::shared_ptr<runtime::ManagedHeap> heap_;
    std::shared_ptr<runtime::NativeHandleRegistry> nativeHandles_;
    GameLanguageMetadata languageMetadata_;
};

struct GameCompileResult {
    GameProgram program;
    std::vector<bytecode::Module> modules;
    diagnostics::DiagnosticBag diagnostics;
    GameLanguageMetadata languageMetadata;

    [[nodiscard]] bool succeeded() const noexcept {
        return program.valid() && !diagnostics.hasErrors();
    }
};

class GameScriptCompiler {
public:
    explicit GameScriptCompiler(const GameApi& api) : api_(api) {}

    [[nodiscard]] GameCompileResult compile(
        const std::vector<compiler::SourceFile>& sources) const;

private:
    const GameApi& api_;
};

struct ScriptType {
    semantic::TypeSymbol descriptor;

    [[nodiscard]] bool valid() const noexcept { return descriptor.id != 0; }
    [[nodiscard]] std::string canonicalName() const {
        return semantic::canonicalTypeName(descriptor);
    }
};

struct ScriptMethod {
    semantic::FunctionSymbol descriptor;

    [[nodiscard]] bool valid() const noexcept { return descriptor.id != 0; }
    [[nodiscard]] bool instance() const noexcept {
        return descriptor.method && !descriptor.staticMethod;
    }
    [[nodiscard]] std::size_t visibleArity() const noexcept {
        return descriptor.parameters.size() - (instance() ? 1u : 0u);
    }
};

class ScriptObject {
public:
    ScriptObject() = default;
    ScriptObject(ScriptObject&&) noexcept = default;
    ScriptObject& operator=(ScriptObject&&) noexcept = default;
    ScriptObject(const ScriptObject&) = delete;
    ScriptObject& operator=(const ScriptObject&) = delete;

    [[nodiscard]] bool valid() const noexcept {
        return type_.valid() && reference_.valid() && root_.valid();
    }
    [[nodiscard]] const ScriptType& type() const noexcept { return type_; }
    [[nodiscard]] runtime::ObjectRef reference() const noexcept { return reference_; }
    [[nodiscard]] runtime::Value value() const { return runtime::Value{reference_}; }

private:
    friend class ScriptRuntime;
    ScriptObject(
        ScriptType type,
        runtime::ObjectRef reference,
        runtime::PersistentRoot root)
        : type_(std::move(type)), reference_(reference), root_(std::move(root)) {}

    ScriptType type_;
    runtime::ObjectRef reference_;
    runtime::PersistentRoot root_;
};

class ScriptRuntime {
public:
    explicit ScriptRuntime(const GameProgram& program);
    ScriptRuntime(
        std::shared_ptr<const runtime::ProgramImage> program,
        std::shared_ptr<const runtime::BindingRegistry> bindings,
        std::shared_ptr<runtime::ManagedHeap> heap,
        std::shared_ptr<runtime::NativeHandleRegistry> nativeHandles);

    [[nodiscard]] std::optional<ScriptType> findType(
        const std::string& canonicalName) const;
    [[nodiscard]] std::optional<ScriptMethod> findMethod(
        const ScriptType& type,
        const std::string& name,
        std::size_t visibleArity) const;
    [[nodiscard]] std::optional<ScriptMethod> findConstructor(
        const ScriptType& type,
        const std::vector<runtime::Value>& arguments) const;

    [[nodiscard]] std::optional<ScriptObject> createObject(
        const ScriptType& type,
        const std::vector<runtime::Value>& constructorArguments,
        runtime::RuntimeError& error,
        runtime::ExecutionOptions options = {}) const;
    [[nodiscard]] std::optional<ScriptObject> createObject(
        const std::string& canonicalTypeName,
        runtime::RuntimeError& error,
        runtime::ExecutionOptions options = {}) const;
    [[nodiscard]] std::optional<ScriptObject> createPrototypeObject(
        const ScriptType& type,
        runtime::RuntimeError& error) const;

    template <typename T>
    [[nodiscard]] std::optional<ScriptObject> wrapRawNative(
        const std::string& canonicalTypeName,
        T* rawObject,
        runtime::RuntimeError& error,
        std::function<void(T*)> customDeleter = {},
        std::string debugName = {}) const {
        if (!rawObject) {
            error.code = runtime::ErrorCode::NullReference;
            error.message = "cannot wrap a null raw native pointer";
            return std::nullopt;
        }
        std::shared_ptr<T> aliasing(
            rawObject,
            [del = std::move(customDeleter)](T* ptr) {
                if (del && ptr) del(ptr);
            });
        return wrapNative<T>(canonicalTypeName, std::move(aliasing), error, std::move(debugName));
    }

    template <typename T>
    [[nodiscard]] std::optional<ScriptObject> createDerivedRawNative(
        const std::string& canonicalTypeName,
        T* rawObject,
        runtime::RuntimeError& error,
        const std::vector<runtime::Value>& constructorArguments = {},
        std::function<void(T*)> customDeleter = {},
        runtime::ExecutionOptions options = {},
        std::string debugName = {}) const {
        if (!rawObject) {
            error.code = runtime::ErrorCode::NullReference;
            error.message = "cannot bind a null raw native pointer";
            return std::nullopt;
        }
        std::shared_ptr<T> aliasing(
            rawObject,
            [del = std::move(customDeleter)](T* ptr) {
                if (del && ptr) del(ptr);
            });
        return createDerivedNative<T>(
            canonicalTypeName,
            std::move(aliasing),
            error,
            constructorArguments,
            std::move(options),
            std::move(debugName));
    }

    template <typename T>
    [[nodiscard]] std::optional<ScriptObject> wrapNative(
        const std::string& canonicalTypeName,
        std::shared_ptr<T> object,
        runtime::RuntimeError& error,
        std::string debugName = {}) const {
        const auto type = findType(canonicalTypeName);
        if (!type || type->descriptor.kind != semantic::TypeKind::Class ||
            type->descriptor.fields.empty() ||
            type->descriptor.fields.front().type != semantic::PrimitiveType::Handle) {
            error.code = runtime::ErrorCode::TypeMismatch;
            error.message = "native wrapper type must be a generated class with a leading handle field";
            return std::nullopt;
        }
        if (!object) {
            error.code = runtime::ErrorCode::NullReference;
            error.message = "cannot wrap a null native object";
            return std::nullopt;
        }
        const auto handleTypeId = nativeBaseTypeId(*type);
        auto handle = nativeHandles_->create(
            handleTypeId != 0 ? handleTypeId : type->descriptor.id,
            std::static_pointer_cast<void>(std::move(object)),
            std::move(debugName));
        std::vector<runtime::Value> fields(type->descriptor.fields.size());
        fields.front() = handle;
        auto reference = heap_->allocateObject(
            type->descriptor.id, std::move(fields), {}, &error);
        if (!reference) {
            nativeHandles_->release(handle);
            return std::nullopt;
        }
        auto root = heap_->retain(*reference);
        if (!root.valid()) {
            nativeHandles_->release(handle);
            error.code = runtime::ErrorCode::InvalidObjectReference;
            error.message = "failed to retain native wrapper object";
            return std::nullopt;
        }
        return ScriptObject{*type, *reference, std::move(root)};
    }

    template <typename T>
    bool bindNative(
        ScriptObject& object,
        std::shared_ptr<T> nativeObject,
        runtime::RuntimeError& error,
        std::string debugName = {}) const {
        if (!object.valid()) {
            error.code = runtime::ErrorCode::InvalidObjectReference;
            error.message = "cannot bind a native object to an invalid script object";
            return false;
        }
        if (!nativeObject) {
            error.code = runtime::ErrorCode::NullReference;
            error.message = "cannot bind a null native object";
            return false;
        }
        const auto handleTypeId = nativeBaseTypeId(object.type());
        if (handleTypeId == 0) {
            error.code = runtime::ErrorCode::TypeMismatch;
            error.message = "script object does not inherit from a native handle wrapper type";
            return false;
        }
        auto handle = nativeHandles_->create(
            handleTypeId,
            std::static_pointer_cast<void>(std::move(nativeObject)),
            std::move(debugName));
        if (!heap_->fieldSet(object.reference(), 0, handle, &error)) {
            nativeHandles_->release(handle);
            return false;
        }
        return true;
    }

    template <typename T>
    [[nodiscard]] std::optional<ScriptObject> createDerivedNative(
        const std::string& canonicalTypeName,
        std::shared_ptr<T> nativeObject,
        runtime::RuntimeError& error,
        const std::vector<runtime::Value>& constructorArguments = {},
        runtime::ExecutionOptions options = {},
        std::string debugName = {}) const {
        if (!nativeObject) {
            error.code = runtime::ErrorCode::NullReference;
            error.message = "cannot bind a null native object";
            return std::nullopt;
        }
        const auto type = findType(canonicalTypeName);
        if (!type) {
            error.code = runtime::ErrorCode::FunctionNotFound;
            error.message = "script type '" + canonicalTypeName + "' was not found";
            return std::nullopt;
        }
        const auto handleTypeId = nativeBaseTypeId(*type);
        if (handleTypeId == 0) {
            error.code = runtime::ErrorCode::TypeMismatch;
            error.message = "script type '" + canonicalTypeName +
                "' does not inherit from a native handle wrapper type";
            return std::nullopt;
        }
        auto object = allocateUninitializedObject(*type, error);
        if (!object) return std::nullopt;
        if (!bindNative(*object, std::move(nativeObject), error, std::move(debugName))) {
            return std::nullopt;
        }
        if (!runObjectConstructor(*object, constructorArguments, error, std::move(options))) {
            releaseNative(*object);
            return std::nullopt;
        }
        return object;
    }

    [[nodiscard]] bool isNativeAlive(const ScriptObject& object) const noexcept;
    bool releaseNative(ScriptObject& object) const noexcept;
    [[nodiscard]] bool isTypeAssignable(
        semantic::SymbolId actualTypeId,
        semantic::SymbolId expectedTypeId) const noexcept;
    void replaceProgram(const GameProgram& program);

    void setAotBackend(const aot::ProgramDescriptor* descriptor);
    void setJitBackend(std::shared_ptr<jit::Module> jitModule);
    void clearNativeBackend() noexcept;
    [[nodiscard]] ScriptExecutionBackend activeBackend() const noexcept {
        return backend_;
    }

    [[nodiscard]] runtime::ExecutionResult invoke(
        const ScriptObject& receiver,
        const ScriptMethod& method,
        const std::vector<runtime::Value>& arguments = {},
        runtime::ExecutionOptions options = {}) const;
    [[nodiscard]] runtime::ExecutionResult invokeStatic(
        const ScriptMethod& method,
        const std::vector<runtime::Value>& arguments = {},
        runtime::ExecutionOptions options = {}) const;

    bool setMember(
        ScriptObject& object,
        const std::string& name,
        runtime::Value value,
        runtime::RuntimeError& error,
        runtime::ExecutionOptions options = {}) const;
    [[nodiscard]] std::optional<runtime::Value> getMember(
        const ScriptObject& object,
        const std::string& name,
        runtime::RuntimeError& error,
        runtime::ExecutionOptions options = {}) const;

    [[nodiscard]] static runtime::Value makeFixed(Fixed value);
    [[nodiscard]] static runtime::Value makeVec2(Vec2 value);
    [[nodiscard]] static runtime::Value makeVec3(Vec3 value);
    [[nodiscard]] static runtime::Value makeQuat(Quat value);
    [[nodiscard]] static std::optional<Fixed> readFixed(
        const runtime::Value& value) noexcept;
    [[nodiscard]] static std::optional<Vec2> readVec2(
        const runtime::Value& value) noexcept;
    [[nodiscard]] static std::optional<Vec3> readVec3(
        const runtime::Value& value) noexcept;
    [[nodiscard]] static std::optional<Quat> readQuat(
        const runtime::Value& value) noexcept;

    [[nodiscard]] std::optional<Vec2> getMemberVec2(
        const ScriptObject& object,
        const std::string& name,
        runtime::RuntimeError& error,
        runtime::ExecutionOptions options = {}) const;
    bool setMemberVec2(
        ScriptObject& object,
        const std::string& name,
        Vec2 value,
        runtime::RuntimeError& error,
        runtime::ExecutionOptions options = {}) const;

    bool readArrayInt(
        runtime::ObjectRef arrayRef,
        std::vector<std::int64_t>& out,
        runtime::RuntimeError& error) const;
    bool writeArrayInt(
        runtime::ObjectRef arrayRef,
        const std::vector<std::int64_t>& values,
        runtime::RuntimeError& error) const;
    bool readArrayVec2(
        runtime::ObjectRef arrayRef,
        std::vector<Vec2>& out,
        runtime::RuntimeError& error) const;
    bool writeArrayVec2(
        runtime::ObjectRef arrayRef,
        const std::vector<Vec2>& values,
        runtime::RuntimeError& error) const;

    [[nodiscard]] std::optional<runtime::Value> createStruct(
        const std::string& canonicalTypeName,
        const std::vector<runtime::Value>& fieldValues,
        runtime::RuntimeError& error) const;
    bool setEcsComponentColumn(
        const std::string& canonicalTypeName,
        const std::vector<runtime::Value>& components,
        runtime::RuntimeError& error) const;
    bool getEcsComponentColumn(
        const std::string& canonicalTypeName,
        std::vector<runtime::Value>& outComponents,
        runtime::RuntimeError& error) const;

    bool setEcsEntityComponent(
        std::uint64_t entity,
        const std::string& canonicalTypeName,
        const runtime::Value& component,
        runtime::RuntimeError& error) const;
    [[nodiscard]] std::optional<runtime::Value> getEcsEntityComponent(
        std::uint64_t entity,
        const std::string& canonicalTypeName,
        runtime::RuntimeError& error) const;
    [[nodiscard]] bool hasEcsEntityComponent(
        std::uint64_t entity,
        const std::string& canonicalTypeName) const;
    bool removeEcsEntityComponent(
        std::uint64_t entity,
        const std::string& canonicalTypeName,
        runtime::RuntimeError& error) const;
    bool destroyEcsEntity(
        std::uint64_t entity,
        runtime::RuntimeError& error) const;

    void queueEcsSetComponent(
        std::uint64_t entity,
        const std::string& canonicalTypeName,
        runtime::Value component) const;
    void queueEcsRemoveComponent(
        std::uint64_t entity,
        const std::string& canonicalTypeName) const;
    void queueEcsDestroyEntity(std::uint64_t entity) const;
    [[nodiscard]] std::size_t pendingEcsCommandCount() const noexcept {
        return ecsCommandBuffer_.size();
    }
    std::size_t flushEcsCommandBuffer(runtime::RuntimeError& error) const;

    // P1-4: Reusable Scriptable Data Assets (ScriptableObject / Resource parity)
    [[nodiscard]] std::optional<ScriptObject> createAsset(
        const std::string& assetPath,
        const std::string& canonicalTypeName,
        runtime::RuntimeError& error,
        const std::map<std::string, runtime::Value>& initialFields = {});
    [[nodiscard]] ScriptObject* findAsset(const std::string& assetPath);
    [[nodiscard]] const ScriptObject* findAsset(const std::string& assetPath) const;
    bool unloadAsset(const std::string& assetPath);
    [[nodiscard]] std::size_t assetCount() const noexcept { return assets_.size(); }

    [[nodiscard]] std::shared_ptr<const runtime::ProgramImage> program() const noexcept {
        return program_;
    }
    [[nodiscard]] std::shared_ptr<runtime::ManagedHeap> heap() const noexcept {
        return heap_;
    }
    [[nodiscard]] std::shared_ptr<runtime::NativeHandleRegistry> nativeHandles() const noexcept {
        return nativeHandles_;
    }

private:
    friend class SceneScriptRuntime;
    [[nodiscard]] semantic::SymbolId nativeBaseTypeId(
        const ScriptType& type) const noexcept;
    [[nodiscard]] std::optional<ScriptObject> allocateUninitializedObject(
        const ScriptType& type,
        runtime::RuntimeError& error) const;
    bool runObjectConstructor(
        ScriptObject& object,
        const std::vector<runtime::Value>& constructorArguments,
        runtime::RuntimeError& error,
        runtime::ExecutionOptions options) const;
    [[nodiscard]] const semantic::TypeSymbol* findTypeById(
        semantic::SymbolId typeId) const;
    [[nodiscard]] runtime::Value defaultValue(
        semantic::PrimitiveType type,
        semantic::SymbolId typeId,
        std::unordered_set<semantic::SymbolId>& visiting) const;
    [[nodiscard]] bool containsManagedReferences(
        semantic::SymbolId typeId,
        std::unordered_set<semantic::SymbolId>& visiting) const;
    [[nodiscard]] bool valueMatches(
        const runtime::Value& value,
        semantic::PrimitiveType type,
        semantic::SymbolId typeId) const;
    [[nodiscard]] runtime::ExecutionResult invokeSymbol(
        semantic::SymbolId symbolId,
        const std::vector<runtime::Value>& arguments,
        runtime::ExecutionOptions options) const;

    std::shared_ptr<const runtime::ProgramImage> program_;
    std::shared_ptr<const runtime::BindingRegistry> bindings_;
    std::shared_ptr<runtime::ManagedHeap> heap_;
    std::shared_ptr<runtime::NativeHandleRegistry> nativeHandles_;
    runtime::Interpreter interpreter_;
    ScriptExecutionBackend backend_ = ScriptExecutionBackend::Interpreter;
    std::shared_ptr<jit::Module> jitModule_;
    std::shared_ptr<aot::Program> nativeProgram_;

    struct EcsQueuedCommand {
        enum class Kind : std::uint8_t {
            SetComponent,
            RemoveComponent,
            DestroyEntity,
        };
        Kind kind = Kind::SetComponent;
        std::uint64_t entity = 0;
        std::string canonicalTypeName;
        runtime::Value component;
    };
    mutable std::vector<EcsQueuedCommand> ecsCommandBuffer_;
    std::map<std::string, ScriptObject> assets_;
};

using SceneEntityId = std::uint64_t;

[[nodiscard]] constexpr SceneEntityId makeGenerationalEntityId(
    std::uint32_t slot,
    std::uint32_t generation = 0) noexcept {
    return (static_cast<SceneEntityId>(generation) << 32u) |
        static_cast<SceneEntityId>(slot);
}

[[nodiscard]] constexpr std::uint32_t entityIdSlot(SceneEntityId id) noexcept {
    return static_cast<std::uint32_t>(id & 0xFFFFFFFFULL);
}

[[nodiscard]] constexpr std::uint32_t entityIdGeneration(SceneEntityId id) noexcept {
    return static_cast<std::uint32_t>(id >> 32u);
}

struct SceneHotReloadReport {
    bool succeeded = false;
    std::size_t instancesMigrated = 0;
    std::size_t fieldsMigrated = 0;
    std::size_t fieldsRenamed = 0;
    std::size_t fieldsDefaulted = 0;
    std::size_t signalsRebound = 0;
    std::vector<std::string> errors;
};

struct SceneScriptError {
    SceneEntityId entity = 0;
    std::string callback;
    runtime::RuntimeError error;
};

struct SceneScriptOptions {
    bool enabled = true;
    std::map<std::string, runtime::Value> initialMembers;
    runtime::Value owner;
};

struct PrefabComponentTemplate {
    std::string scriptType;
    SceneScriptOptions options;
};

struct PrefabNodeTemplate {
    std::string name;
    Transform3D localTransform{};
    std::vector<PrefabComponentTemplate> components;
    std::vector<PrefabNodeTemplate> children;
};

struct SceneCollider2D {
    Vec2 halfExtents{Fixed::fromInt(1), Fixed::fromInt(1)};
    Fixed radius{Fixed::fromInt(1)};
    bool isCircle = false;
    bool isTrigger = false;
    std::uint32_t layerMask = 0xFFFFFFFFu;
    std::uint32_t collisionLayer = 0xFFFFFFFFu;
    std::uint32_t collisionMask = 0xFFFFFFFFu;
};

struct RaycastHit2D {
    SceneEntityId entity = 0;
    Vec2 point{};
    Vec2 normal{};
    Fixed distance{};
};

struct CollisionContact2D {
    SceneEntityId entityA = 0;
    SceneEntityId entityB = 0;
    Vec2 normal{};
    Vec2 contactPoint{};
    Fixed penetration{};
    bool isTrigger = false;
};

struct SceneCollider3D {
    Vec3 halfExtents{Fixed::fromInt(1), Fixed::fromInt(1), Fixed::fromInt(1)};
    Fixed radius{Fixed::fromInt(1)};
    bool isSphere = false;
    bool isTrigger = false;
    std::uint32_t layerMask = 0xFFFFFFFFu;
    std::uint32_t collisionLayer = 0xFFFFFFFFu;
    std::uint32_t collisionMask = 0xFFFFFFFFu;
};

struct RaycastHit3D {
    SceneEntityId entity = 0;
    Vec3 point{};
    Vec3 normal{};
    Fixed distance{};
};

struct CollisionContact3D {
    SceneEntityId entityA = 0;
    SceneEntityId entityB = 0;
    Vec3 normal{};
    Vec3 contactPoint{};
    Fixed penetration{};
    bool isTrigger = false;
};

struct ScriptEvent {
    SceneEntityId target = 0;
    std::string callback;
    std::vector<runtime::Value> arguments;
};

struct ScriptTrigger {
    std::string id;
    SceneEntityId target = 0;
    std::string callback;
    std::vector<runtime::Value> arguments;
    std::function<bool()> condition;
    bool once = true;
    bool enabled = true;
    bool fired = false;
};

class SceneScriptRuntime {
public:
    static constexpr SceneEntityId BroadcastTarget = 0;

    struct SignalConnection {
        SceneEntityId sourceEntity = 0;
        std::string sourceComponent;
        std::string signalName;
        SceneEntityId targetEntity = 0;
        std::string targetComponent;
        std::string targetMethod;
    };

    struct ComponentLocation {
        SceneEntityId entity = 0;
        std::string canonicalTypeName;
    };

    explicit SceneScriptRuntime(ScriptRuntime& runtime)
        : runtime_(runtime) {}

    bool attach(
        SceneEntityId entity,
        const std::string& scriptType,
        SceneScriptOptions options = {});
    bool attachComponent(
        SceneEntityId entity,
        const std::string& scriptType,
        SceneScriptOptions options = {});
    bool detach(SceneEntityId entity);
    bool detachComponent(SceneEntityId entity, const std::string& scriptType);
    bool setEnabled(SceneEntityId entity, bool enabled);
    bool setEntityActive(SceneEntityId entity, bool active);
    [[nodiscard]] bool isEntityActiveSelf(SceneEntityId entity) const noexcept;
    bool setComponentEnabled(
        SceneEntityId entity,
        const std::string& scriptType,
        bool enabled);
    [[nodiscard]] bool isComponentEnabled(
        SceneEntityId entity,
        const std::string& scriptType) const noexcept;
    void start();
    void update(double deltaTime);
    void fixedUpdate(double fixedDeltaTime);
    void lateUpdate(double deltaTime);

    void setExecutionMode(SceneExecutionMode mode) noexcept {
        executionMode_ = mode;
    }
    [[nodiscard]] SceneExecutionMode executionMode() const noexcept {
        return executionMode_;
    }

    [[nodiscard]] SceneHotReloadReport hotReload(
        const GameProgram& newProgram,
        const ScriptMetadataRegistry* metadata = nullptr);

    bool validate(SceneEntityId entity);
    bool validateComponent(
        SceneEntityId entity,
        const std::string& scriptType);
    bool setInspectorMember(
        SceneEntityId entity,
        const std::string& scriptType,
        const std::string& memberName,
        runtime::Value value);

    bool connectSignal(
        SceneEntityId sourceEntity,
        const std::string& sourceComponent,
        const std::string& signalName,
        SceneEntityId targetEntity,
        const std::string& targetComponent,
        const std::string& targetMethod);
    bool disconnectSignal(
        SceneEntityId sourceEntity,
        const std::string& sourceComponent,
        const std::string& signalName,
        SceneEntityId targetEntity,
        const std::string& targetComponent,
        const std::string& targetMethod);
    [[nodiscard]] const std::vector<SignalConnection>& signalConnections() const noexcept {
        return signalConnections_;
    }
    std::size_t emitSignal(
        SceneEntityId sourceEntity,
        const std::string& sourceComponent,
        const std::string& signalName,
        const std::vector<runtime::Value>& arguments = {});
    std::size_t emitSignal(
        SceneEntityId sourceEntity,
        const std::string& signalName,
        const std::vector<runtime::Value>& arguments = {}) {
        return emitSignal(sourceEntity, {}, signalName, arguments);
    }
    void setSignalTopicPublisher(
        std::function<void(const std::string&)> publisher) {
        signalTopicPublisher_ = [pub = std::move(publisher)](const std::string& topic) -> std::size_t {
            if (!pub) return 0;
            pub(topic);
            return 1;
        };
    }
    void setSignalTopicPublisherCounted(
        std::function<std::size_t(const std::string&)> publisher) {
        signalTopicPublisher_ = std::move(publisher);
    }

    // Deterministic Input Snapshot (RealScript.Input)
    void setInputAction(const std::string& action, bool pressed);
    void setInputAxis2D(const std::string& axisName, Vec2 value);
    void advanceInputTick();
    [[nodiscard]] bool isInputActionPressed(const std::string& action) const noexcept;
    [[nodiscard]] bool isInputActionJustPressed(const std::string& action) const noexcept;
    [[nodiscard]] bool isInputActionJustReleased(const std::string& action) const noexcept;
    [[nodiscard]] Vec2 getInputAxis2D(const std::string& axisName) const noexcept;
    [[nodiscard]] std::uint64_t computeInputDigest() const noexcept;
    [[nodiscard]] const std::map<std::string, bool>& currentInputActions() const noexcept {
        return currentInputActions_;
    }
    [[nodiscard]] const std::map<std::string, bool>& previousInputActions() const noexcept {
        return previousInputActions_;
    }
    [[nodiscard]] const std::map<std::string, Vec2>& inputAxes2D() const noexcept {
        return inputAxes2D_;
    }
    void restoreInputSnapshot(
        std::map<std::string, bool> currentActions,
        std::map<std::string, bool> previousActions,
        std::map<std::string, Vec2> axes2D);

    // Deterministic 2D Physics & Spatial Queries (RealScript.Physics)
    void setEntityCollider2D(SceneEntityId entity, SceneCollider2D collider);
    bool removeEntityCollider2D(SceneEntityId entity);
    [[nodiscard]] std::optional<SceneCollider2D> getEntityCollider2D(SceneEntityId entity) const noexcept;
    [[nodiscard]] const std::map<SceneEntityId, SceneCollider2D>& colliders2D() const noexcept {
        return colliders2D_;
    }
    [[nodiscard]] const std::set<std::pair<SceneEntityId, SceneEntityId>>& activeOverlaps2D() const noexcept {
        return activeOverlaps2D_;
    }
    [[nodiscard]] const std::map<std::pair<SceneEntityId, SceneEntityId>, bool>& activeOverlapWasTrigger2D() const noexcept {
        return activeOverlapWasTrigger2D_;
    }
    void restorePhysicsOverlapState(
        std::set<std::pair<SceneEntityId, SceneEntityId>> activeOverlaps,
        std::map<std::pair<SceneEntityId, SceneEntityId>, bool> activeOverlapWasTrigger);
    [[nodiscard]] std::optional<RaycastHit2D> raycast2D(
        Vec2 origin,
        Vec2 direction,
        Fixed maxDistance,
        std::uint32_t layerMask = 0xFFFFFFFFu) const;
    [[nodiscard]] std::vector<RaycastHit2D> raycastAll2D(
        Vec2 origin,
        Vec2 direction,
        Fixed maxDistance,
        std::uint32_t layerMask = 0xFFFFFFFFu) const;
    [[nodiscard]] std::vector<SceneEntityId> overlapCircle2D(
        Vec2 center,
        Fixed radius,
        std::uint32_t layerMask = 0xFFFFFFFFu) const;
    std::size_t stepPhysics2D();
    [[nodiscard]] std::optional<CollisionContact2D> getCollisionContact2D(
        SceneEntityId entityA,
        SceneEntityId entityB) const noexcept;
    [[nodiscard]] std::size_t lastBroadphaseCandidatePairs2D() const noexcept {
        return lastBroadphasePairs2D_;
    }

    // Deterministic 3D Physics & Spatial Queries (RealScript.Physics3D)
    void setEntityCollider3D(SceneEntityId entity, SceneCollider3D collider);
    bool removeEntityCollider3D(SceneEntityId entity);
    [[nodiscard]] std::optional<SceneCollider3D> getEntityCollider3D(SceneEntityId entity) const noexcept;
    [[nodiscard]] const std::map<SceneEntityId, SceneCollider3D>& colliders3D() const noexcept {
        return colliders3D_;
    }
    [[nodiscard]] const std::set<std::pair<SceneEntityId, SceneEntityId>>& activeOverlaps3D() const noexcept {
        return activeOverlaps3D_;
    }
    [[nodiscard]] const std::map<std::pair<SceneEntityId, SceneEntityId>, bool>& activeOverlapWasTrigger3D() const noexcept {
        return activeOverlapWasTrigger3D_;
    }
    void restorePhysicsOverlapState3D(
        std::set<std::pair<SceneEntityId, SceneEntityId>> activeOverlaps,
        std::map<std::pair<SceneEntityId, SceneEntityId>, bool> activeOverlapWasTrigger);
    [[nodiscard]] std::optional<RaycastHit3D> raycast3D(
        Vec3 origin,
        Vec3 direction,
        Fixed maxDistance,
        std::uint32_t layerMask = 0xFFFFFFFFu) const;
    [[nodiscard]] std::vector<RaycastHit3D> raycastAll3D(
        Vec3 origin,
        Vec3 direction,
        Fixed maxDistance,
        std::uint32_t layerMask = 0xFFFFFFFFu) const;
    [[nodiscard]] std::vector<SceneEntityId> overlapSphere3D(
        Vec3 center,
        Fixed radius,
        std::uint32_t layerMask = 0xFFFFFFFFu) const;
    std::size_t stepPhysics3D();
    [[nodiscard]] std::optional<CollisionContact3D> getCollisionContact3D(
        SceneEntityId entityA,
        SceneEntityId entityB) const noexcept;
    [[nodiscard]] std::size_t lastBroadphaseCandidatePairs3D() const noexcept {
        return lastBroadphasePairs3D_;
    }

    // Deferred Destruction, Entity Spawning, Generational Handles, Naming & Groups (Godot/Unity Parity)
    SceneEntityId spawnEntity(const std::string& name = {});
    SceneEntityId spawnGenerationalEntity(const std::string& name = {});
    [[nodiscard]] bool isEntityAlive(SceneEntityId entity) const noexcept;
    [[nodiscard]] bool isStaleGenerationalEntity(SceneEntityId entity) const noexcept;
    [[nodiscard]] std::uint32_t getEntitySlotGeneration(std::uint32_t slot) const noexcept;
    void registerImportedEntityHandle(SceneEntityId entity);
    void resetGenerationalSlots();
    void queueDestroyEntity(SceneEntityId entity);
    [[nodiscard]] std::size_t pendingDestroyCount() const noexcept {
        return pendingDestroyEntities_.size();
    }
    std::size_t flushDestroyedEntities();
    void setEntityName(SceneEntityId entity, std::string name);
    [[nodiscard]] std::string getEntityName(SceneEntityId entity) const;
    [[nodiscard]] SceneEntityId findEntityByName(const std::string& name) const noexcept;
    void addEntityToGroup(SceneEntityId entity, const std::string& group);
    bool removeEntityFromGroup(SceneEntityId entity, const std::string& group);
    [[nodiscard]] bool isEntityInGroup(SceneEntityId entity, const std::string& group) const noexcept;
    [[nodiscard]] std::vector<SceneEntityId> getEntitiesInGroup(const std::string& group) const;
    [[nodiscard]] std::vector<std::string> getEntityGroups(SceneEntityId entity) const;

    [[nodiscard]] std::optional<ComponentLocation> findObjectLocation(
        runtime::ObjectRef reference) const;

    [[nodiscard]] runtime::ExecutionResult invoke(
        SceneEntityId entity,
        const std::string& callback,
        const std::vector<runtime::Value>& arguments = {});
    [[nodiscard]] runtime::ExecutionResult invokeComponent(
        SceneEntityId entity,
        const std::string& scriptType,
        const std::string& callback,
        const std::vector<runtime::Value>& arguments = {});
    bool dispatch(const ScriptEvent& event);
    void enqueue(ScriptEvent event);
    std::size_t flushEvents();

    bool addTrigger(ScriptTrigger trigger);
    bool removeTrigger(const std::string& id);
    std::size_t evaluateTriggers();

    void setExecutionOptions(runtime::ExecutionOptions options) {
        executionOptions_ = std::move(options);
    }
    [[nodiscard]] const std::vector<SceneScriptError>& errors() const noexcept {
        return errors_;
    }
    void clearErrors() { errors_.clear(); }
    [[nodiscard]] ScriptRuntime& runtime() noexcept { return runtime_; }
    [[nodiscard]] const ScriptRuntime& runtime() const noexcept { return runtime_; }
    [[nodiscard]] ScriptObject* object(SceneEntityId entity);
    [[nodiscard]] ScriptObject* getComponent(
        SceneEntityId entity,
        const std::string& typeOrInterfaceName);
    [[nodiscard]] const ScriptObject* getComponent(
        SceneEntityId entity,
        const std::string& typeOrInterfaceName) const;
    [[nodiscard]] std::vector<ScriptObject*> getComponents(SceneEntityId entity);
    [[nodiscard]] std::vector<const ScriptObject*> getComponents(SceneEntityId entity) const;
    [[nodiscard]] std::vector<SceneEntityId> entities() const;
    [[nodiscard]] bool hasComponent(
        SceneEntityId entity,
        const std::string& typeOrInterfaceName) const;
    [[nodiscard]] std::size_t componentCount(SceneEntityId entity) const noexcept;

    bool setParent(SceneEntityId child, SceneEntityId parent);
    [[nodiscard]] SceneEntityId getParent(SceneEntityId entity) const noexcept;
    [[nodiscard]] std::vector<SceneEntityId> getChildren(SceneEntityId entity) const;
    [[nodiscard]] bool isActiveInHierarchy(SceneEntityId entity) const noexcept;

    void setLocalTransform(SceneEntityId entity, const Transform3D& transform);
    [[nodiscard]] Transform3D getLocalTransform(SceneEntityId entity) const noexcept;
    [[nodiscard]] Transform3D getWorldTransform(SceneEntityId entity) const noexcept;
    void setLocalPosition2D(SceneEntityId entity, Vec2 position);
    [[nodiscard]] Vec2 getWorldPosition2D(SceneEntityId entity) const noexcept;

    [[nodiscard]] ScriptObject* getComponentInChildren(
        SceneEntityId entity,
        const std::string& typeOrInterfaceName);
    [[nodiscard]] ScriptObject* getComponentInParent(
        SceneEntityId entity,
        const std::string& typeOrInterfaceName);

    SceneEntityId instantiatePrefab(
        const PrefabNodeTemplate& prefab,
        SceneEntityId& nextEntityId,
        SceneEntityId parent = 0,
        const std::map<std::string, runtime::Value>& rootMemberOverrides = {});

    void setFiberScheduler(runtime::FiberScheduler* scheduler) noexcept {
        fiberScheduler_ = scheduler;
    }
    [[nodiscard]] runtime::FiberScheduler* fiberScheduler() const noexcept {
        return fiberScheduler_;
    }
    std::uint32_t spawnEntityFiber(
        SceneEntityId entity,
        const runtime::CompiledFiberModule& module,
        const std::string& functionName,
        std::uint8_t ownerGroupId = 0,
        std::uint32_t seed = 0x53434257u);
    std::uint64_t stepFibers(std::uint32_t currentTick);

private:
    struct MethodCacheEntry {
        semantic::SymbolId typeId = 0;
        std::string name;
        std::size_t arity = 0;
        std::optional<ScriptMethod> method;
    };

    struct Instance {
        ScriptObject object;
        bool enabled = true;
        bool started = false;
        bool executeInEditMode = false;
        int executionOrder = 0;
        std::optional<ScriptMethod> onCreate0;
        std::optional<ScriptMethod> onCreate1;
        std::optional<ScriptMethod> onStart;
        std::optional<ScriptMethod> onEnable;
        std::optional<ScriptMethod> onDisable;
        std::optional<ScriptMethod> onUpdate;
        std::optional<ScriptMethod> onFixedUpdate;
        std::optional<ScriptMethod> onLateUpdate;
        std::optional<ScriptMethod> onDestroy;
        std::optional<ScriptMethod> onValidate;
        std::optional<ScriptMethod> onCollisionEnter;
        std::optional<ScriptMethod> onCollisionStay;
        std::optional<ScriptMethod> onCollisionExit;
        std::optional<ScriptMethod> onTriggerEnter;
        std::optional<ScriptMethod> onTriggerStay;
        std::optional<ScriptMethod> onTriggerExit;
        std::optional<ScriptMethod> onCollisionEnter3D;
        std::optional<ScriptMethod> onCollisionStay3D;
        std::optional<ScriptMethod> onCollisionExit3D;
        std::optional<ScriptMethod> onTriggerEnter3D;
        std::optional<ScriptMethod> onTriggerStay3D;
        std::optional<ScriptMethod> onTriggerExit3D;
    };

    struct TickRef {
        int order = 0;
        SceneEntityId entity = 0;
        std::size_t compIndex = 0;
    };

    void bindLifecycleMethods(Instance& instance) const;
    [[nodiscard]] bool shouldTickInCurrentMode(const Instance& instance) const noexcept;
    [[nodiscard]] bool matchesComponentType(
        const ScriptType& componentType,
        const std::string& query) const;
    void rebuildTickOrderIfNeeded() const;
    void invalidateTransformSubtree(SceneEntityId entity);
    bool rebuildSignalDelegateChain(
        SceneEntityId sourceEntity,
        const std::string& sourceComponent,
        const std::string& signalName);
    void removeSignalsForEntityOrComponent(
        SceneEntityId entity,
        const std::string& removedComponentType);
    bool attachComponentWithoutLifecycle(
        SceneEntityId entity,
        const std::string& scriptType,
        SceneScriptOptions options,
        std::size_t& outCompIndex);

    [[nodiscard]] const ScriptMethod* findCachedMethod(
        const ScriptType& type,
        const std::string& name,
        std::size_t arity);

    bool invokeLifecycle(
        SceneEntityId entity,
        Instance& instance,
        const std::optional<ScriptMethod>& method,
        const std::vector<runtime::Value>& arguments,
        const char* callback);
    void recordError(
        SceneEntityId entity,
        std::string callback,
        const runtime::RuntimeError& error);

    ScriptRuntime& runtime_;
    runtime::ExecutionOptions executionOptions_;
    SceneExecutionMode executionMode_ = SceneExecutionMode::Play;
    std::map<SceneEntityId, std::vector<Instance>> instances_;
    std::map<SceneEntityId, SceneEntityId> parentByEntity_;
    std::map<SceneEntityId, std::vector<SceneEntityId>> childrenByEntity_;
    std::map<SceneEntityId, bool> entityActiveSelf_;
    std::map<SceneEntityId, Transform3D> localTransforms_;
    mutable std::unordered_map<SceneEntityId, Transform3D> worldTransformCache_;
    mutable bool tickOrderDirty_ = true;
    mutable std::vector<TickRef> cachedTickOrder_;
    std::vector<SignalConnection> signalConnections_;
    std::function<std::size_t(const std::string&)> signalTopicPublisher_;
    std::map<std::string, bool> currentInputActions_;
    std::map<std::string, bool> previousInputActions_;
    std::map<std::string, Vec2> inputAxes2D_;
    std::map<SceneEntityId, SceneCollider2D> colliders2D_;
    std::set<std::pair<SceneEntityId, SceneEntityId>> activeOverlaps2D_;
    std::map<std::pair<SceneEntityId, SceneEntityId>, bool> activeOverlapWasTrigger2D_;
    std::map<std::pair<SceneEntityId, SceneEntityId>, CollisionContact2D> activeContacts2D_;
    std::size_t lastBroadphasePairs2D_ = 0;
    std::map<SceneEntityId, SceneCollider3D> colliders3D_;
    std::set<std::pair<SceneEntityId, SceneEntityId>> activeOverlaps3D_;
    std::map<std::pair<SceneEntityId, SceneEntityId>, bool> activeOverlapWasTrigger3D_;
    std::map<std::pair<SceneEntityId, SceneEntityId>, CollisionContact3D> activeContacts3D_;
    std::size_t lastBroadphasePairs3D_ = 0;
    std::vector<SceneEntityId> pendingDestroyEntities_;
    std::map<SceneEntityId, std::string> entityNames_;
    std::map<std::string, std::set<SceneEntityId>> groupEntities_;
    std::unordered_map<std::uint32_t, std::uint32_t> slotGenerations_;
    std::vector<std::uint32_t> freeGenerationalSlots_;
    std::uint32_t nextGenerationalSlot_ = 1;
    SceneEntityId nextAutoEntityId_ = 1;
    runtime::FiberScheduler* fiberScheduler_ = nullptr;
    std::map<std::size_t, std::vector<MethodCacheEntry>> methodCache_;
    std::vector<ScriptEvent> eventQueue_;
    std::map<std::string, ScriptTrigger> triggers_;
    std::vector<SceneScriptError> errors_;
};

/**
 * Default Fiber Host Adapter bridging `FiberScheduler` (`HostCommand` and `HostCondJump`)
 * directly to `SceneScriptRuntime` entities and component fields/methods.
 */
class SceneFiberHostAdapter final : public runtime::IFiberHostCallbacks {
public:
    explicit SceneFiberHostAdapter(SceneScriptRuntime& scene) noexcept
        : scene_(scene) {}

    void registerCommandCallback(
        std::uint8_t commandId,
        std::string callbackName,
        std::string componentType = {});
    void registerConditionMember(
        std::uint16_t key16,
        std::string memberName,
        std::string componentType = {});

    void onHostCommand(
        runtime::FiberContext& fiber,
        std::uint8_t commandId,
        std::uint16_t arg16,
        std::uint32_t arg32) noexcept override;

    bool onEvaluateCondition(
        const runtime::FiberContext& fiber,
        std::uint8_t conditionKind,
        std::uint16_t key16,
        std::uint16_t threshold16) const noexcept override;

private:
    struct CommandEntry {
        std::string callbackName;
        std::string componentType;
    };
    struct ConditionEntry {
        std::string memberName;
        std::string componentType;
    };
    SceneScriptRuntime& scene_;
    std::unordered_map<std::uint8_t, CommandEntry> commands_;
    std::unordered_map<std::uint16_t, ConditionEntry> conditions_;
};

bool installSceneComponentBindings(
    GameApi& api,
    SceneScriptRuntime& scene,
    std::string moduleName = "RealScript.Scene");

} // namespace realscript::game
