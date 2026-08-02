#pragma once

#include "realscript/game/GameBindings.h"

namespace realscript::game {

struct GameLanguageMetadata {
    std::vector<compiler::LanguageAttributeRecord> attributes;
    std::vector<compiler::LanguageInterfaceImplementation> interfaces;
    std::vector<compiler::LanguageGenericInstantiation> genericInstantiations;

    [[nodiscard]] bool empty() const noexcept {
        return attributes.empty() && interfaces.empty() &&
            genericInstantiations.empty();
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
        auto handle = nativeHandles_->create(
            type->descriptor.id,
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
};

using SceneEntityId = std::uint64_t;

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

    explicit SceneScriptRuntime(ScriptRuntime& runtime)
        : runtime_(runtime) {}

    bool attach(
        SceneEntityId entity,
        const std::string& scriptType,
        SceneScriptOptions options = {});
    bool detach(SceneEntityId entity);
    bool setEnabled(SceneEntityId entity, bool enabled);
    void start();
    void update(double deltaTime);
    void fixedUpdate(double fixedDeltaTime);
    void lateUpdate(double deltaTime);

    [[nodiscard]] runtime::ExecutionResult invoke(
        SceneEntityId entity,
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
    [[nodiscard]] ScriptObject* object(SceneEntityId entity);

private:
    struct Instance {
        ScriptObject object;
        bool enabled = true;
        bool started = false;
        std::optional<ScriptMethod> onCreate0;
        std::optional<ScriptMethod> onCreate1;
        std::optional<ScriptMethod> onStart;
        std::optional<ScriptMethod> onEnable;
        std::optional<ScriptMethod> onDisable;
        std::optional<ScriptMethod> onUpdate;
        std::optional<ScriptMethod> onFixedUpdate;
        std::optional<ScriptMethod> onLateUpdate;
        std::optional<ScriptMethod> onDestroy;
    };

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
    std::map<SceneEntityId, Instance> instances_;
    std::vector<ScriptEvent> eventQueue_;
    std::map<std::string, ScriptTrigger> triggers_;
    std::vector<SceneScriptError> errors_;
};

} // namespace realscript::game
