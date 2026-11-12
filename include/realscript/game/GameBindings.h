#pragma once

#include "realscript/game/BindingMarshal.h"

#if !defined(RS_EXPORT_CLASS)
#define RS_EXPORT_CLASS(...)
#endif
#if !defined(RS_EXPORT_ECS_COMPONENT)
#define RS_EXPORT_ECS_COMPONENT(...)
#endif
#if !defined(RS_EXPORT_STRUCT)
#define RS_EXPORT_STRUCT(...)
#endif
#if !defined(RS_EXPORT_FIELD)
#define RS_EXPORT_FIELD
#endif
#if !defined(RS_EXPORT_METHOD)
#define RS_EXPORT_METHOD
#endif
#if !defined(RS_EXPORT_METHOD_EX)
#define RS_EXPORT_METHOD_EX(...)
#endif
#if !defined(RS_EXPORT_PROPERTY)
#define RS_EXPORT_PROPERTY(...)
#endif
#if !defined(RS_EXPORT_FUNCTION)
#define RS_EXPORT_FUNCTION(...)
#endif
#if !defined(RS_EXPORT_ENUM)
#define RS_EXPORT_ENUM
#endif
#if !defined(RS_EXPORT_CONSTRUCTOR)
#define RS_EXPORT_CONSTRUCTOR(...)
#endif

namespace realscript::game {

namespace detail {

inline std::string parameterSignatureKey(
    const std::vector<ParameterDeclaration>& parameters) {
    std::string out = "(";
    for (std::size_t i = 0; i < parameters.size(); ++i) {
        if (i != 0) out += ",";
        if (!parameters[i].type.moduleName.empty()) {
            out += parameters[i].type.moduleName + "::";
        }
        out += parameters[i].type.sourceName;
    }
    out += ")";
    return out;
}

inline std::string allocateOverloadNativeName(
    const std::unordered_set<std::string>& bindingNames,
    const std::string& moduleName,
    const std::string& baseNativeName,
    const std::vector<ParameterDeclaration>& parameters) {
    if (bindingNames.find(moduleName + "::" + baseNativeName) == bindingNames.end()) {
        return baseNativeName;
    }
    std::string suffix = "__ov" + std::to_string(parameters.size());
    for (const auto& p : parameters) {
        suffix += "_";
        for (const char ch : p.type.sourceName) {
            if ((ch >= 'a' && ch <= 'z') ||
                (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9')) {
                suffix.push_back(ch);
            } else if (ch == '[') {
                suffix += "Arr";
            }
        }
    }
    std::string candidate = baseNativeName + suffix;
    std::size_t disambig = 2;
    while (bindingNames.find(moduleName + "::" + candidate) != bindingNames.end()) {
        candidate = baseNativeName + suffix + "_" + std::to_string(disambig++);
    }
    return candidate;
}

} // namespace detail

class GameApi;

template <typename S>
class NativeStructBuilder {
public:
    template <auto FieldPtr>
    NativeStructBuilder& field(std::string name);

private:
    friend class GameApi;
    NativeStructBuilder(std::shared_ptr<detail::GameApiState> state, std::size_t index)
        : state_(std::move(state)), index_(index) {}

    std::shared_ptr<detail::GameApiState> state_;
    std::size_t index_ = 0;
};

template <typename T>
class NativeTypeBuilder {
public:
    template <typename Base>
    NativeTypeBuilder& base() {
        static_assert(std::is_base_of_v<Base, T> && !std::is_same_v<std::remove_cv_t<Base>, std::remove_cv_t<T>>,
            "Base must be a strict base class of T");
        auto& type = state_->nativeTypes[index_];
        type.baseCppType = std::type_index(typeid(Base));
        type.upcastToBase = [](const std::shared_ptr<void>& ptr) -> std::shared_ptr<void> {
            return std::static_pointer_cast<Base>(std::static_pointer_cast<T>(ptr));
        };
        const auto baseIt = state_->nativeTypeByCppType.find(*type.baseCppType);
        if (baseIt != state_->nativeTypeByCppType.end()) {
            const auto& baseDecl = state_->nativeTypes[baseIt->second];
            if (!baseDecl.moduleName.empty() && baseDecl.moduleName != type.moduleName) {
                state_->modules[type.moduleName].imports.insert(baseDecl.moduleName);
            }
        }
        return *this;
    }

    template <auto Method>
    NativeTypeBuilder& method(
        std::string name,
        runtime::BindingDeterminism determinism =
            runtime::BindingDeterminism::Deterministic,
        std::string requiredCapability = {});

    template <auto Method>
    NativeTypeBuilder& staticMethod(
        std::string name,
        runtime::BindingDeterminism determinism =
            runtime::BindingDeterminism::Deterministic,
        std::string requiredCapability = {}) {
        return method<Method>(std::move(name), determinism, std::move(requiredCapability));
    }

    template <typename... Args>
    NativeTypeBuilder& constructor(
        std::string factoryName = "Create",
        runtime::BindingDeterminism determinism =
            runtime::BindingDeterminism::Deterministic,
        std::string requiredCapability = {});

    template <auto Getter>
    NativeTypeBuilder& property(
        std::string name,
        runtime::BindingDeterminism determinism =
            runtime::BindingDeterminism::Deterministic);

    template <auto Getter, auto Setter>
    NativeTypeBuilder& property(
        std::string name,
        runtime::BindingDeterminism determinism =
            runtime::BindingDeterminism::Deterministic);

    template <auto FieldPtr>
    NativeTypeBuilder& field(
        std::string name,
        runtime::BindingDeterminism determinism =
            runtime::BindingDeterminism::Deterministic);

private:
    friend class GameApi;
    NativeTypeBuilder(std::shared_ptr<detail::GameApiState> state, std::size_t index)
        : state_(std::move(state)), index_(index) {}

    std::shared_ptr<detail::GameApiState> state_;
    std::size_t index_ = 0;
};

class GameApi {
public:
    GameApi();
    GameApi(
        std::shared_ptr<runtime::ManagedHeap> heap,
        std::shared_ptr<runtime::NativeHandleRegistry> nativeHandles);

    template <typename S>
    NativeStructBuilder<S> structType(std::string moduleName, std::string name) {
        static_assert(std::is_class_v<S> && std::is_default_constructible_v<S>,
            "registered native struct must be a default-constructible C++ struct/class");
        const auto key = std::type_index(typeid(S));
        const auto existing = state_->nativeStructByCppType.find(key);
        if (existing != state_->nativeStructByCppType.end()) {
            const auto& registered = state_->nativeStructs[existing->second];
            if (registered.moduleName != moduleName || registered.name != name) {
                state_->errors.push_back(
                    "C++ native struct was registered more than once with different script names");
            }
            return NativeStructBuilder<S>(state_, existing->second);
        }

        detail::NativeStructDeclaration declaration;
        declaration.cppType = key;
        declaration.moduleName = std::move(moduleName);
        declaration.name = std::move(name);
        declaration.typeId = semantic::stableTypeId(
            declaration.moduleName + "::" + declaration.name);
        const auto index = state_->nativeStructs.size();
        state_->nativeStructByCppType.emplace(key, index);
        state_->nativeStructs.push_back(std::move(declaration));
        auto& module = state_->modules[state_->nativeStructs.back().moduleName];
        module.name = state_->nativeStructs.back().moduleName;
        module.nativeStructs.push_back(index);
        return NativeStructBuilder<S>(state_, index);
    }

    template <typename T>
    NativeTypeBuilder<T> type(std::string moduleName, std::string name) {
        const auto key = std::type_index(typeid(T));
        const auto existing = state_->nativeTypeByCppType.find(key);
        if (existing != state_->nativeTypeByCppType.end()) {
            const auto& registered = state_->nativeTypes[existing->second];
            if (registered.moduleName != moduleName || registered.name != name) {
                state_->errors.push_back(
                    "C++ native type was registered more than once with different script names");
            }
            return NativeTypeBuilder<T>(state_, existing->second);
        }

        detail::NativeTypeDeclaration declaration;
        declaration.cppType = key;
        declaration.moduleName = std::move(moduleName);
        declaration.name = std::move(name);
        declaration.typeId = semantic::stableTypeId(
            declaration.moduleName + "::" + declaration.name);
        if (state_->nativeHandles) {
            state_->nativeHandles->registerCppType(key, declaration.typeId);
        }
        const auto index = state_->nativeTypes.size();
        state_->nativeTypeByCppType.emplace(key, index);
        state_->nativeTypes.push_back(std::move(declaration));
        auto& module = state_->modules[state_->nativeTypes.back().moduleName];
        module.name = state_->nativeTypes.back().moduleName;
        module.nativeTypes.push_back(index);
        return NativeTypeBuilder<T>(state_, index);
    }

    template <typename Callable>
    bool function(
        std::string moduleName,
        std::string name,
        Callable callable,
        runtime::BindingDeterminism determinism =
            runtime::BindingDeterminism::Deterministic,
        std::string requiredCapability = {}) {
        using CallableType = std::decay_t<Callable>;
        using Traits = detail::FunctionTraits<CallableType>;
        auto returnType = detail::resolveScriptType<typename Traits::Return>(state_);
        auto parameters = detail::makeParameters<typename Traits::Arguments>(
            state_, std::make_index_sequence<Traits::Arity>{});
        if (!returnType || !parameters) {
            state_->errors.push_back(
                "unsupported C++ type in native function '" + moduleName + "::" + name + "'");
            return false;
        }

        const auto publicName = moduleName + "::" + name +
            detail::parameterSignatureKey(*parameters);
        if (!state_->declarationNames.insert(publicName).second) {
            state_->errors.push_back("duplicate native declaration '" + moduleName + "::" + name + "'");
            return false;
        }

        auto& module = state_->modules[moduleName];
        module.name = moduleName;
        detail::collectImports(module, *returnType);
        for (const auto& parameter : *parameters) {
            detail::collectImports(module, parameter.type);
        }
        auto sharedCallable = std::make_shared<CallableType>(std::move(callable));
        auto state = state_;
        const auto nativeName = detail::allocateOverloadNativeName(
            state_->bindingNames, moduleName, "__rs_native_" + name, *parameters);
        const auto bindingName = moduleName + "::" + nativeName;
        if (!state_->bindingNames.insert(bindingName).second) {
            state_->errors.push_back("duplicate native binding '" + bindingName + "'");
            return false;
        }

        std::string call = nativeName + "(";
        for (std::size_t index = 0; index < parameters->size(); ++index) {
            if (index != 0) call += ", ";
            call += (*parameters)[index].name;
        }
        call += ")";
        const auto wrapperBody = std::is_void_v<typename Traits::Return>
            ? call + "; return;"
            : "return " + call + ";";
        module.functions.push_back(detail::FunctionDeclaration{
            name, *returnType, *parameters, wrapperBody, determinism, requiredCapability});
        module.functions.push_back(detail::FunctionDeclaration{
            nativeName,
            std::is_void_v<typename Traits::Return>
                ? detail::ScriptTypeRef{"int", {}, detail::ScriptTypeCategory::Int}
                : *returnType,
            *parameters,
            {},
            determinism,
            requiredCapability});
        return state_->bindings->bind(
            bindingName,
            [state, sharedCallable](const auto&, const auto& arguments, auto& error) {
                const auto invoked = detail::invokeFree<CallableType, Traits>(
                    state,
                    *sharedCallable,
                    arguments,
                    error,
                    std::make_index_sequence<Traits::Arity>{});
                if (!invoked) return std::optional<runtime::Value>{};
                if constexpr (std::is_void_v<typename Traits::Return>) {
                    return std::optional<runtime::Value>{std::int64_t{0}};
                } else {
                    return invoked;
                }
            },
            determinism,
            std::move(requiredCapability));
    }

    [[nodiscard]] std::vector<compiler::SourceFile> generatedSources() const;
    [[nodiscard]] std::vector<compiler::SourceFile> exportDeclarationStubs() const;
    [[nodiscard]] std::string exportProjectConfigJson(
        const std::string& projectName,
        const std::vector<std::string>& sourcePaths = {}) const;
    [[nodiscard]] std::set<std::string> nativeModules() const;
    [[nodiscard]] const std::unordered_set<std::string>& nativeBindingNames() const noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] const std::vector<std::string>& errors() const noexcept;
    [[nodiscard]] std::shared_ptr<runtime::BindingRegistry> bindings() const noexcept;
    [[nodiscard]] std::shared_ptr<runtime::ManagedHeap> heap() const noexcept;
    [[nodiscard]] std::shared_ptr<runtime::NativeHandleRegistry> nativeHandles() const noexcept;

    struct HeaderBindingGenerationResult {
        std::string cppRegistrationSource;
        std::string scriptDeclarationStub;
        std::size_t classesExported = 0;
        std::size_t structsExported = 0;
        std::size_t methodsExported = 0;
        std::size_t constructorsExported = 0;
        std::size_t functionsExported = 0;
        std::size_t propertiesExported = 0;
        std::size_t enumsExported = 0;
    };

    [[nodiscard]] static HeaderBindingGenerationResult generateBindingsFromCppHeader(
        std::string_view headerContent,
        const std::string& defaultModuleName = "Engine.Native",
        const std::unordered_map<std::string, std::string>& predefinedMacros = {});

    template <typename T>
    [[nodiscard]] std::string canonicalTypeName() const {
        const auto found = state_->nativeTypeByCppType.find(std::type_index(typeid(T)));
        if (found == state_->nativeTypeByCppType.end()) return {};
        const auto& type = state_->nativeTypes[found->second];
        return type.moduleName + "::" + type.name;
    }

private:
    template <typename S>
    friend class NativeStructBuilder;
    template <typename T>
    friend class NativeTypeBuilder;
    std::shared_ptr<detail::GameApiState> state_;
};

template <typename T>
template <auto Method>
NativeTypeBuilder<T>& NativeTypeBuilder<T>::method(
    std::string name,
    runtime::BindingDeterminism determinism,
    std::string requiredCapability) {
    using Traits = detail::FunctionTraits<decltype(Method)>;
    if constexpr (Traits::IsMemberFunction) {
        static_assert(std::is_base_of_v<typename Traits::Class, T>,
            "bound method must belong to or be inherited by the native type being registered");
    }

    auto returnType = detail::resolveScriptType<typename Traits::Return>(state_);
    auto parameters = detail::makeParameters<typename Traits::Arguments>(
        state_, std::make_index_sequence<Traits::Arity>{});
    auto& type = state_->nativeTypes[index_];
    if (!returnType || !parameters) {
        state_->errors.push_back(
            "unsupported C++ type in native method '" +
            type.moduleName + "::" + type.name + "." + name + "'");
        return *this;
    }

    const auto publicName = type.moduleName + "::" + type.name + "." + name +
        detail::parameterSignatureKey(*parameters);
    if (!state_->declarationNames.insert(publicName).second) {
        state_->errors.push_back(
            "duplicate native declaration '" + type.moduleName + "::" + type.name + "." + name + "'");
        return *this;
    }

    auto& module = state_->modules[type.moduleName];
    detail::collectImports(module, *returnType);
    for (const auto& parameter : *parameters) {
        detail::collectImports(module, parameter.type);
    }
    auto state = state_;
    const auto nativeName = detail::allocateOverloadNativeName(
        state_->bindingNames, type.moduleName, "__rs_native_" + type.name + "_" + name, *parameters);
    const auto bindingName = type.moduleName + "::" + nativeName;
    if (!state_->bindingNames.insert(bindingName).second) {
        state_->errors.push_back("duplicate native binding '" + bindingName + "'");
        return *this;
    }

    if constexpr (Traits::IsMemberFunction) {
        std::vector<detail::ParameterDeclaration> nativeParameters;
        nativeParameters.push_back({
            detail::ScriptTypeRef{
                type.name,
                type.moduleName,
                detail::ScriptTypeCategory::NativeObject},
            "self"});
        nativeParameters.insert(
            nativeParameters.end(), parameters->begin(), parameters->end());
        std::string call = nativeName + "(this";
        for (const auto& parameter : *parameters) {
            call += ", " + parameter.name;
        }
        call += ")";
        const auto wrapperBody = std::is_void_v<typename Traits::Return>
            ? call + "; return;"
            : "return " + call + ";";
        type.methods.push_back(detail::FunctionDeclaration{
            name, *returnType, *parameters, wrapperBody, determinism, requiredCapability, false});
        module.functions.push_back(detail::FunctionDeclaration{
            nativeName,
            std::is_void_v<typename Traits::Return>
                ? detail::ScriptTypeRef{"int", {}, detail::ScriptTypeCategory::Int}
                : *returnType,
            std::move(nativeParameters),
            {},
            determinism,
            requiredCapability,
            false});
        state_->bindings->bind(
            bindingName,
            [state](const auto&, const auto& arguments, auto& error) {
                const auto invoked = detail::invokeMethod<T, Method, Traits>(
                    state,
                    arguments,
                    error,
                    std::make_index_sequence<Traits::Arity>{});
                if (!invoked) return std::optional<runtime::Value>{};
                if constexpr (std::is_void_v<typename Traits::Return>) {
                    return std::optional<runtime::Value>{std::int64_t{0}};
                } else {
                    return invoked;
                }
            },
            determinism,
            std::move(requiredCapability));
    } else {
        std::string call = nativeName + "(";
        for (std::size_t index = 0; index < parameters->size(); ++index) {
            if (index != 0) call += ", ";
            call += (*parameters)[index].name;
        }
        call += ")";
        const auto wrapperBody = std::is_void_v<typename Traits::Return>
            ? call + "; return;"
            : "return " + call + ";";
        type.methods.push_back(detail::FunctionDeclaration{
            name, *returnType, *parameters, wrapperBody, determinism, requiredCapability, true});
        module.functions.push_back(detail::FunctionDeclaration{
            nativeName,
            std::is_void_v<typename Traits::Return>
                ? detail::ScriptTypeRef{"int", {}, detail::ScriptTypeCategory::Int}
                : *returnType,
            *parameters,
            {},
            determinism,
            requiredCapability,
            false});
        state_->bindings->bind(
            bindingName,
            [state](const auto&, const auto& arguments, auto& error) {
                auto fn = Method;
                const auto invoked = detail::invokeFree<decltype(fn), Traits>(
                    state,
                    fn,
                    arguments,
                    error,
                    std::make_index_sequence<Traits::Arity>{});
                if (!invoked) return std::optional<runtime::Value>{};
                if constexpr (std::is_void_v<typename Traits::Return>) {
                    return std::optional<runtime::Value>{std::int64_t{0}};
                } else {
                    return invoked;
                }
            },
            determinism,
            std::move(requiredCapability));
    }
    return *this;
}

namespace detail {

template <typename T, typename... Args>
struct ConstructorBindingCallable {
    std::shared_ptr<GameApiState> state;

    static std::shared_ptr<T> create(Args... args) {
        return std::make_shared<T>(std::forward<Args>(args)...);
    }

    std::optional<runtime::Value> operator()(
        const bytecode::FunctionReference&,
        runtime::ValueSpan arguments,
        runtime::RuntimeError& error) const {
        using FactoryFn = std::shared_ptr<T>(*)(Args...);
        using Traits = FunctionTraits<FactoryFn>;
        FactoryFn fn = &ConstructorBindingCallable::create;
        return invokeFree<FactoryFn, Traits>(
            state,
            fn,
            arguments,
            error,
            std::make_index_sequence<Traits::Arity>{});
    }
};

} // namespace detail

template <typename T>
template <typename... Args>
NativeTypeBuilder<T>& NativeTypeBuilder<T>::constructor(
    std::string factoryName,
    runtime::BindingDeterminism determinism,
    std::string requiredCapability) {
    using FactoryFn = std::shared_ptr<T>(*)(Args...);
    using Traits = detail::FunctionTraits<FactoryFn>;

    auto returnType = detail::resolveScriptType<std::shared_ptr<T>>(state_);
    auto parameters = detail::makeParameters<typename Traits::Arguments>(
        state_, std::make_index_sequence<Traits::Arity>{});
    auto& type = state_->nativeTypes[index_];
    if (!returnType || !parameters) {
        state_->errors.push_back(
            "unsupported C++ type in native constructor '" +
            type.moduleName + "::" + type.name + "." + factoryName + "'");
        return *this;
    }

    const auto publicName = type.moduleName + "::" + type.name + "." + factoryName +
        detail::parameterSignatureKey(*parameters);
    if (!state_->declarationNames.insert(publicName).second) {
        state_->errors.push_back(
            "duplicate native constructor declaration '" + type.moduleName + "::" + type.name + "." + factoryName + "'");
        return *this;
    }

    auto& module = state_->modules[type.moduleName];
    detail::collectImports(module, *returnType);
    for (const auto& parameter : *parameters) {
        detail::collectImports(module, parameter.type);
    }
    const auto nativeName = detail::allocateOverloadNativeName(
        state_->bindingNames, type.moduleName, "__rs_native_" + type.name + "_ctor_" + factoryName, *parameters);
    const auto bindingName = type.moduleName + "::" + nativeName;
    if (!state_->bindingNames.insert(bindingName).second) {
        state_->errors.push_back("duplicate native constructor binding '" + bindingName + "'");
        return *this;
    }

    std::string call = nativeName + "(";
    for (std::size_t index = 0; index < parameters->size(); ++index) {
        if (index != 0) call += ", ";
        call += (*parameters)[index].name;
    }
    call += ")";
    type.methods.push_back(detail::FunctionDeclaration{
        factoryName, *returnType, *parameters, "return " + call + ";", determinism, requiredCapability, true});
    module.functions.push_back(detail::FunctionDeclaration{
        nativeName, *returnType, *parameters, {}, determinism, requiredCapability, false});
    state_->bindings->bind(
        bindingName,
        detail::ConstructorBindingCallable<T, Args...>{state_},
        determinism,
        std::move(requiredCapability));
    return *this;
}

template <typename T>
template <auto Getter>
NativeTypeBuilder<T>& NativeTypeBuilder<T>::property(
    std::string name,
    runtime::BindingDeterminism determinism) {
    using Traits = detail::FunctionTraits<decltype(Getter)>;
    static_assert(std::is_base_of_v<typename Traits::Class, T>,
        "bound property getter must belong to or be inherited by the native type being registered");
    static_assert(Traits::Arity == 0,
        "bound property getter must not take arguments");
    static_assert(!std::is_void_v<typename Traits::Return>,
        "bound property getter must return a value");

    auto typeRef = detail::resolveScriptType<typename Traits::Return>(state_);
    auto& type = state_->nativeTypes[index_];
    if (!typeRef) {
        state_->errors.push_back(
            "unsupported C++ type in native property '" +
            type.moduleName + "::" + type.name + "." + name + "'");
        return *this;
    }
    const auto publicName = type.moduleName + "::" + type.name + ".get_" + name;
    if (!state_->declarationNames.insert(publicName).second) {
        state_->errors.push_back("duplicate native declaration '" + publicName + "'");
        return *this;
    }

    const auto nativeName = "__rs_native_" + type.name + "_get_" + name;
    const auto bindingName = type.moduleName + "::" + nativeName;
    if (!state_->bindingNames.insert(bindingName).second) {
        state_->errors.push_back("duplicate native binding '" + bindingName + "'");
        return *this;
    }

    auto& module = state_->modules[type.moduleName];
    detail::collectImports(module, *typeRef);
    type.properties.push_back(detail::PropertyDeclaration{
        name,
        *typeRef,
        true,
        false,
        "return " + nativeName + "(this);",
        {}});
    module.functions.push_back(detail::FunctionDeclaration{
        nativeName,
        *typeRef,
        {{detail::ScriptTypeRef{
            type.name,
            type.moduleName,
            detail::ScriptTypeCategory::NativeObject}, "self"}},
        {},
        determinism,
        {}});
    auto state = state_;
    state_->bindings->bind(
        bindingName,
        [state](const auto&, const auto& arguments, auto& error) {
            return detail::invokeMethod<T, Getter, Traits>(
                state,
                arguments,
                error,
                std::make_index_sequence<Traits::Arity>{});
        },
        determinism);
    return *this;
}

template <typename T>
template <auto Getter, auto Setter>
NativeTypeBuilder<T>& NativeTypeBuilder<T>::property(
    std::string name,
    runtime::BindingDeterminism determinism) {
    using GetterTraits = detail::FunctionTraits<decltype(Getter)>;
    using SetterTraits = detail::FunctionTraits<decltype(Setter)>;
    static_assert(std::is_base_of_v<typename GetterTraits::Class, T>,
        "bound property getter must belong to or be inherited by the native type being registered");
    static_assert(std::is_base_of_v<typename SetterTraits::Class, T>,
        "bound property setter must belong to or be inherited by the native type being registered");
    static_assert(GetterTraits::Arity == 0,
        "bound property getter must not take arguments");
    static_assert(SetterTraits::Arity == 1,
        "bound property setter must take one argument");
    static_assert(std::is_void_v<typename SetterTraits::Return>,
        "bound property setter must return void");

    using SetterArgument = std::tuple_element_t<0, typename SetterTraits::Arguments>;
    auto getterType = detail::resolveScriptType<typename GetterTraits::Return>(state_);
    auto setterType = detail::resolveScriptType<SetterArgument>(state_);
    auto& type = state_->nativeTypes[index_];
    if (!getterType || !setterType ||
        getterType->sourceName != setterType->sourceName ||
        getterType->moduleName != setterType->moduleName ||
        getterType->category != setterType->category) {
        state_->errors.push_back(
            "native property getter and setter types do not match for '" +
            type.moduleName + "::" + type.name + "." + name + "'");
        return *this;
    }
    const auto getterPublicName = type.moduleName + "::" + type.name + ".get_" + name;
    const auto setterPublicName = type.moduleName + "::" + type.name + ".set_" + name;
    if (!state_->declarationNames.insert(getterPublicName).second ||
        !state_->declarationNames.insert(setterPublicName).second) {
        state_->errors.push_back("duplicate native property declaration for '" + name + "'");
        return *this;
    }

    const auto getterNativeName = "__rs_native_" + type.name + "_get_" + name;
    const auto setterNativeName = "__rs_native_" + type.name + "_set_" + name;
    const auto getterBinding = type.moduleName + "::" + getterNativeName;
    const auto setterBinding = type.moduleName + "::" + setterNativeName;
    if (!state_->bindingNames.insert(getterBinding).second ||
        !state_->bindingNames.insert(setterBinding).second) {
        state_->errors.push_back("duplicate native property binding for '" + name + "'");
        return *this;
    }

    auto& module = state_->modules[type.moduleName];
    detail::collectImports(module, *getterType);
    const auto getterBody = "return " + getterNativeName + "(this);";
    const auto setterBody = setterNativeName + "(this, value); return;";
    type.properties.push_back(detail::PropertyDeclaration{
        name, *getterType, true, true, getterBody, setterBody});

    const detail::ParameterDeclaration receiver{
        detail::ScriptTypeRef{
            type.name,
            type.moduleName,
            detail::ScriptTypeCategory::NativeObject},
        "self"};
    module.functions.push_back(detail::FunctionDeclaration{
        getterNativeName,
        *getterType,
        {receiver},
        {},
        determinism,
        {}});
    module.functions.push_back(detail::FunctionDeclaration{
        setterNativeName,
        detail::ScriptTypeRef{"int", {}, detail::ScriptTypeCategory::Int},
        {receiver, detail::ParameterDeclaration{*setterType, "value"}},
        {},
        determinism,
        {}});

    auto state = state_;
    state_->bindings->bind(
        getterBinding,
        [state](const auto&, const auto& arguments, auto& error) {
            return detail::invokeMethod<T, Getter, GetterTraits>(
                state,
                arguments,
                error,
                std::make_index_sequence<GetterTraits::Arity>{});
        },
        determinism);
    state_->bindings->bind(
        setterBinding,
        [state](const auto&, const auto& arguments, auto& error) {
            const auto invoked = detail::invokeMethod<T, Setter, SetterTraits>(
                state,
                arguments,
                error,
                std::make_index_sequence<SetterTraits::Arity>{});
            if (!invoked) return std::optional<runtime::Value>{};
            return std::optional<runtime::Value>{std::int64_t{0}};
        },
        determinism);
    return *this;
}

namespace detail {

template <typename>
struct MemberFieldTraits;

template <typename ClassType, typename FieldType>
struct MemberFieldTraits<FieldType ClassType::*> {
    using Class = ClassType;
    using ValueType = std::remove_cv_t<FieldType>;
    static constexpr bool IsReadOnly = std::is_const_v<FieldType>;
};

} // namespace detail

template <typename T>
template <auto FieldPtr>
NativeTypeBuilder<T>& NativeTypeBuilder<T>::field(
    std::string name,
    runtime::BindingDeterminism determinism) {
    using Traits = detail::MemberFieldTraits<decltype(FieldPtr)>;
    static_assert(std::is_base_of_v<typename Traits::Class, T>,
        "bound field must belong to or be inherited by the native type being registered");
    using ValueType = typename Traits::ValueType;

    auto fieldType = detail::resolveScriptType<ValueType>(state_);
    auto& type = state_->nativeTypes[index_];
    if (!fieldType) {
        state_->errors.push_back(
            "unsupported C++ type in native field '" +
            type.moduleName + "::" + type.name + "." + name + "'");
        return *this;
    }

    const auto getterPublicName = type.moduleName + "::" + type.name + ".get_" + name;
    if (!state_->declarationNames.insert(getterPublicName).second) {
        state_->errors.push_back("duplicate native field declaration for '" + name + "'");
        return *this;
    }
    const auto getterNativeName = "__rs_native_" + type.name + "_get_" + name;
    const auto getterBinding = type.moduleName + "::" + getterNativeName;
    if (!state_->bindingNames.insert(getterBinding).second) {
        state_->errors.push_back("duplicate native field binding for '" + name + "'");
        return *this;
    }

    auto& module = state_->modules[type.moduleName];
    detail::collectImports(module, *fieldType);
    const detail::ParameterDeclaration receiver{
        detail::ScriptTypeRef{
            type.name,
            type.moduleName,
            detail::ScriptTypeCategory::NativeObject},
        "self"};

    auto state = state_;
    state_->bindings->bind(
        getterBinding,
        [state](const auto&, const auto& arguments, auto& error) -> std::optional<runtime::Value> {
            if (arguments.size() != 1) {
                detail::setInvalidArguments(error, "native field getter received the wrong argument count");
                return std::nullopt;
            }
            const auto recv = detail::readNativeObject<T>(state, arguments.front(), error);
            if (!recv || !*recv) {
                if (error.code == runtime::ErrorCode::None) {
                    error.code = runtime::ErrorCode::NullReference;
                    error.message = "native field getter receiver is null";
                }
                return std::nullopt;
            }
            return detail::writeValue(state, (**recv).*FieldPtr, error);
        },
        determinism);

    if constexpr (Traits::IsReadOnly) {
        type.properties.push_back(detail::PropertyDeclaration{
            name,
            *fieldType,
            true,
            false,
            "return " + getterNativeName + "(this);",
            {}});
        module.functions.push_back(detail::FunctionDeclaration{
            getterNativeName,
            *fieldType,
            {receiver},
            {},
            determinism,
            {}});
    } else {
        const auto setterPublicName = type.moduleName + "::" + type.name + ".set_" + name;
        const auto setterNativeName = "__rs_native_" + type.name + "_set_" + name;
        const auto setterBinding = type.moduleName + "::" + setterNativeName;
        if (!state_->declarationNames.insert(setterPublicName).second ||
            !state_->bindingNames.insert(setterBinding).second) {
            state_->errors.push_back("duplicate native field setter binding for '" + name + "'");
            return *this;
        }
        type.properties.push_back(detail::PropertyDeclaration{
            name,
            *fieldType,
            true,
            true,
            "return " + getterNativeName + "(this);",
            setterNativeName + "(this, value); return;"});
        module.functions.push_back(detail::FunctionDeclaration{
            getterNativeName,
            *fieldType,
            {receiver},
            {},
            determinism,
            {}});
        module.functions.push_back(detail::FunctionDeclaration{
            setterNativeName,
            detail::ScriptTypeRef{"int", {}, detail::ScriptTypeCategory::Int},
            {receiver, detail::ParameterDeclaration{*fieldType, "value"}},
            {},
            determinism,
            {}});
        state_->bindings->bind(
            setterBinding,
            [state](const auto&, const auto& arguments, auto& error) -> std::optional<runtime::Value> {
                if (arguments.size() != 2) {
                    detail::setInvalidArguments(error, "native field setter received the wrong argument count");
                    return std::nullopt;
                }
                const auto recv = detail::readNativeObject<T>(state, arguments[0], error);
                if (!recv || !*recv) {
                    if (error.code == runtime::ErrorCode::None) {
                        error.code = runtime::ErrorCode::NullReference;
                        error.message = "native field setter receiver is null";
                    }
                    return std::nullopt;
                }
                auto converted = detail::readValue<ValueType>(state, arguments[1], error);
                if (!converted) return std::nullopt;
                ((**recv).*FieldPtr) = detail::passArgument<ValueType>(*converted);
                return std::optional<runtime::Value>{std::int64_t{0}};
            },
            determinism);
    }
    return *this;
}

template <typename S>
template <auto FieldPtr>
NativeStructBuilder<S>& NativeStructBuilder<S>::field(std::string name) {
    using Traits = detail::MemberFieldTraits<decltype(FieldPtr)>;
    static_assert(std::is_base_of_v<typename Traits::Class, S>,
        "bound struct field must belong to or be inherited by the native struct being registered");
    using ValueType = typename Traits::ValueType;

    auto fieldType = detail::resolveScriptType<ValueType>(state_);
    auto& st = state_->nativeStructs[index_];
    if (!fieldType) {
        state_->errors.push_back(
            "unsupported C++ type in native struct field '" +
            st.moduleName + "::" + st.name + "." + name + "'");
        return *this;
    }
    const auto publicName = st.moduleName + "::" + st.name + ".field." + name;
    if (!state_->declarationNames.insert(publicName).second) {
        state_->errors.push_back("duplicate native struct field declaration for '" + name + "'");
        return *this;
    }
    auto& module = state_->modules[st.moduleName];
    detail::collectImports(module, *fieldType);
    detail::NativeStructFieldDeclaration decl;
    decl.name = std::move(name);
    decl.type = *fieldType;
    decl.readField = [](const std::shared_ptr<detail::GameApiState>& stState,
                        const void* rawStruct,
                        runtime::RuntimeError& error) -> std::optional<runtime::Value> {
        const auto* typed = static_cast<const S*>(rawStruct);
        return detail::writeValue(stState, typed->*FieldPtr, error);
    };
    decl.writeField = [](const std::shared_ptr<detail::GameApiState>& stState,
                         void* rawStruct,
                         const runtime::Value& val,
                         runtime::RuntimeError& error) -> bool {
        auto converted = detail::readValue<ValueType>(stState, val, error);
        if (!converted) return false;
        auto* typed = static_cast<S*>(rawStruct);
        typed->*FieldPtr = detail::passArgument<ValueType>(*converted);
        return true;
    };
    st.fields.push_back(std::move(decl));
    return *this;
}

} // namespace realscript::game
