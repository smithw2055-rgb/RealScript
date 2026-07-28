#pragma once

#include "realscript/game/BindingMarshal.h"

namespace realscript::game {

class GameApi;

template <typename T>
class NativeTypeBuilder {
public:
    template <auto Method>
    NativeTypeBuilder& method(
        std::string name,
        runtime::BindingDeterminism determinism =
            runtime::BindingDeterminism::Deterministic);

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
            runtime::BindingDeterminism::Deterministic) {
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

        const auto bindingName = moduleName + "::" + name;
        if (!state_->bindingNames.insert(bindingName).second) {
            state_->errors.push_back("duplicate native binding '" + bindingName + "'");
            return false;
        }

        auto& module = state_->modules[moduleName];
        module.name = moduleName;
        detail::collectImports(module, *returnType);
        for (const auto& parameter : *parameters) {
            detail::collectImports(module, parameter.type);
        }
        module.functions.push_back(detail::FunctionDeclaration{
            name, *returnType, *parameters});
        auto sharedCallable = std::make_shared<CallableType>(std::move(callable));
        auto state = state_;
        return state_->bindings->bind(
            bindingName,
            [state, sharedCallable](const auto&, const auto& arguments, auto& error) {
                return detail::invokeFree<CallableType, Traits>(
                    state,
                    *sharedCallable,
                    arguments,
                    error,
                    std::make_index_sequence<Traits::Arity>{});
            },
            determinism);
    }

    [[nodiscard]] std::vector<compiler::SourceFile> generatedSources() const;
    [[nodiscard]] std::set<std::string> nativeModules() const;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] const std::vector<std::string>& errors() const noexcept;
    [[nodiscard]] std::shared_ptr<runtime::BindingRegistry> bindings() const noexcept;
    [[nodiscard]] std::shared_ptr<runtime::ManagedHeap> heap() const noexcept;
    [[nodiscard]] std::shared_ptr<runtime::NativeHandleRegistry> nativeHandles() const noexcept;

    template <typename T>
    [[nodiscard]] std::string canonicalTypeName() const {
        const auto found = state_->nativeTypeByCppType.find(std::type_index(typeid(T)));
        if (found == state_->nativeTypeByCppType.end()) return {};
        const auto& type = state_->nativeTypes[found->second];
        return type.moduleName + "::" + type.name;
    }

private:
    template <typename T>
    friend class NativeTypeBuilder;
    std::shared_ptr<detail::GameApiState> state_;
};

template <typename T>
template <auto Method>
NativeTypeBuilder<T>& NativeTypeBuilder<T>::method(
    std::string name,
    runtime::BindingDeterminism determinism) {
    using Traits = detail::FunctionTraits<decltype(Method)>;
    static_assert(std::is_same_v<typename Traits::Class, T>,
        "bound method must belong to the native type being registered");

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

    const auto bindingName = type.moduleName + "::" + type.name + "." + name;
    if (!state_->bindingNames.insert(bindingName).second) {
        state_->errors.push_back("duplicate native binding '" + bindingName + "'");
        return *this;
    }

    auto& module = state_->modules[type.moduleName];
    detail::collectImports(module, *returnType);
    for (const auto& parameter : *parameters) {
        detail::collectImports(module, parameter.type);
    }
    type.methods.push_back(detail::FunctionDeclaration{name, *returnType, *parameters});
    auto state = state_;
    state_->bindings->bind(
        bindingName,
        [state](const auto&, const auto& arguments, auto& error) {
            return detail::invokeMethod<Method, Traits>(
                state,
                arguments,
                error,
                std::make_index_sequence<Traits::Arity>{});
        },
        determinism);
    return *this;
}

template <typename T>
template <auto Getter>
NativeTypeBuilder<T>& NativeTypeBuilder<T>::property(
    std::string name,
    runtime::BindingDeterminism determinism) {
    using Traits = detail::FunctionTraits<decltype(Getter)>;
    static_assert(std::is_same_v<typename Traits::Class, T>,
        "bound property getter must belong to the native type being registered");
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
    const auto bindingName = type.moduleName + "::" + type.name + ".get_" + name;
    if (!state_->bindingNames.insert(bindingName).second) {
        state_->errors.push_back("duplicate native binding '" + bindingName + "'");
        return *this;
    }

    auto& module = state_->modules[type.moduleName];
    detail::collectImports(module, *typeRef);
    type.properties.push_back(detail::PropertyDeclaration{name, *typeRef, true, false});
    auto state = state_;
    state_->bindings->bind(
        bindingName,
        [state](const auto&, const auto& arguments, auto& error) {
            return detail::invokeMethod<Getter, Traits>(
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
    static_assert(std::is_same_v<typename GetterTraits::Class, T>,
        "bound property getter must belong to the native type being registered");
    static_assert(std::is_same_v<typename SetterTraits::Class, T>,
        "bound property setter must belong to the native type being registered");
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
    const auto getterBinding = type.moduleName + "::" + type.name + ".get_" + name;
    const auto setterBinding = type.moduleName + "::" + type.name + ".set_" + name;
    if (state_->bindingNames.find(getterBinding) != state_->bindingNames.end() ||
        state_->bindingNames.find(setterBinding) != state_->bindingNames.end()) {
        state_->errors.push_back("duplicate native property binding for '" + name + "'");
        return *this;
    }
    state_->bindingNames.insert(getterBinding);
    state_->bindingNames.insert(setterBinding);

    auto& module = state_->modules[type.moduleName];
    detail::collectImports(module, *getterType);
    type.properties.push_back(detail::PropertyDeclaration{name, *getterType, true, true});
    auto state = state_;
    state_->bindings->bind(
        getterBinding,
        [state](const auto&, const auto& arguments, auto& error) {
            return detail::invokeMethod<Getter, GetterTraits>(
                state,
                arguments,
                error,
                std::make_index_sequence<GetterTraits::Arity>{});
        },
        determinism);
    state_->bindings->bind(
        setterBinding,
        [state](const auto&, const auto& arguments, auto& error) {
            return detail::invokeMethod<Setter, SetterTraits>(
                state,
                arguments,
                error,
                std::make_index_sequence<SetterTraits::Arity>{});
        },
        determinism);
    return *this;
}

} // namespace realscript::game
