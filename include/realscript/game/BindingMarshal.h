#pragma once

#include "realscript/game/BindingTypes.h"

namespace realscript::game::detail {

template <typename T>
std::optional<std::shared_ptr<T>> readNativeObject(
    const std::shared_ptr<GameApiState>& state,
    const runtime::Value& value,
    runtime::RuntimeError& error) {
    const auto* type = findNativeType(state, std::type_index(typeid(T)));
    if (!type) {
        setTypeMismatch(error, "native C++ type is not registered with the game script API");
        return std::nullopt;
    }
    if (std::holds_alternative<runtime::NullObject>(value)) {
        return std::shared_ptr<T>{};
    }
    const auto* reference = std::get_if<runtime::ObjectRef>(&value);
    if (!reference) {
        setTypeMismatch(error, "expected script native-object wrapper");
        return std::nullopt;
    }
    const auto actualType = state->heap->objectTypeId(*reference);
    if (!actualType || *actualType != type->typeId) {
        setTypeMismatch(error, "script native-object wrapper has the wrong type");
        return std::nullopt;
    }
    const auto handleValue = state->heap->fieldGet(*reference, 0);
    if (!handleValue || !std::holds_alternative<runtime::NativeHandle>(*handleValue)) {
        setTypeMismatch(error, "script native-object wrapper does not contain a native handle");
        return std::nullopt;
    }
    auto resource = state->nativeHandles->resolve(
        std::get<runtime::NativeHandle>(*handleValue), type->typeId, &error);
    if (!resource) return std::nullopt;
    return std::static_pointer_cast<T>(std::move(resource));
}

template <typename T>
std::optional<runtime::Value> writeNativeObject(
    const std::shared_ptr<GameApiState>& state,
    std::shared_ptr<T> value,
    runtime::RuntimeError& error) {
    const auto* type = findNativeType(state, std::type_index(typeid(T)));
    if (!type) {
        setTypeMismatch(error, "native C++ type is not registered with the game script API");
        return std::nullopt;
    }
    if (!value) return runtime::Value{runtime::NullObject{}};
    auto handle = state->nativeHandles->create(
        type->typeId,
        std::static_pointer_cast<void>(std::move(value)),
        type->moduleName + "::" + type->name);
    std::vector<runtime::Value> fields;
    fields.emplace_back(handle);
    auto object = state->heap->allocateObject(type->typeId, std::move(fields), {}, &error);
    if (!object) {
        state->nativeHandles->release(handle);
        return std::nullopt;
    }
    return runtime::Value{*object};
}

template <typename T>
std::optional<RemoveCvRef<T>> readValue(
    const std::shared_ptr<GameApiState>& state,
    const runtime::Value& value,
    runtime::RuntimeError& error) {
    using ValueType = RemoveCvRef<T>;
    if constexpr (std::is_same_v<ValueType, bool>) {
        if (const auto* result = std::get_if<bool>(&value)) return *result;
        setTypeMismatch(error, "expected bool argument");
    } else if constexpr (
        std::is_integral_v<ValueType> &&
        !std::is_same_v<ValueType, bool> &&
        sizeof(ValueType) <= sizeof(std::int32_t)) {
        if (const auto* result = std::get_if<std::int64_t>(&value)) {
            if constexpr (std::is_signed_v<ValueType>) {
                if (*result < static_cast<std::int64_t>(std::numeric_limits<ValueType>::min()) ||
                    *result > static_cast<std::int64_t>(std::numeric_limits<ValueType>::max())) {
                    setTypeMismatch(error, "integer argument is outside the C++ target range");
                    return std::nullopt;
                }
            } else {
                if (*result < 0 ||
                    static_cast<std::uint64_t>(*result) >
                        static_cast<std::uint64_t>(std::numeric_limits<ValueType>::max())) {
                    setTypeMismatch(error, "integer argument is outside the C++ target range");
                    return std::nullopt;
                }
            }
            return static_cast<ValueType>(*result);
        }
        setTypeMismatch(error, "expected int argument");
    } else if constexpr (
        std::is_integral_v<ValueType> &&
        !std::is_same_v<ValueType, bool>) {
        if (const auto* result = std::get_if<runtime::LongValue>(&value)) {
            if constexpr (std::is_signed_v<ValueType>) {
                if (result->value < static_cast<std::int64_t>(std::numeric_limits<ValueType>::min()) ||
                    result->value > static_cast<std::int64_t>(std::numeric_limits<ValueType>::max())) {
                    setTypeMismatch(error, "long argument is outside the C++ target range");
                    return std::nullopt;
                }
            } else {
                if (result->value < 0 ||
                    static_cast<std::uint64_t>(result->value) >
                        static_cast<std::uint64_t>(std::numeric_limits<ValueType>::max())) {
                    setTypeMismatch(error, "long argument is outside the C++ target range");
                    return std::nullopt;
                }
            }
            return static_cast<ValueType>(result->value);
        }
        setTypeMismatch(error, "expected long argument");
    } else if constexpr (std::is_floating_point_v<ValueType>) {
        if (const auto* result = std::get_if<double>(&value)) {
            return static_cast<ValueType>(*result);
        }
        setTypeMismatch(error, "expected double argument");
    } else if constexpr (
        std::is_same_v<ValueType, std::string> ||
        std::is_same_v<ValueType, std::string_view>) {
        if (const auto* result = std::get_if<std::string>(&value)) {
            return ValueType(*result);
        }
        if (const auto* reference = std::get_if<runtime::ObjectRef>(&value)) {
            const auto text = state->heap->stringView(*reference);
            if (text) return ValueType(*text);
        }
        setTypeMismatch(error, "expected string argument");
    } else if constexpr (IsSharedPtr<ValueType>::value) {
        using Element = typename IsSharedPtr<ValueType>::Element;
        return readNativeObject<Element>(state, value, error);
    } else if constexpr (std::is_pointer_v<ValueType>) {
        using Element = std::remove_pointer_t<ValueType>;
        const auto object = readNativeObject<Element>(state, value, error);
        if (!object) return std::nullopt;
        return object->get();
    } else {
        static_assert(!sizeof(T), "unsupported C++ argument type for RealScript game binding");
    }
    return std::nullopt;
}

template <typename T>
std::optional<runtime::Value> writeValue(
    const std::shared_ptr<GameApiState>& state,
    T&& value,
    runtime::RuntimeError& error) {
    using ValueType = RemoveCvRef<T>;
    if constexpr (std::is_same_v<ValueType, bool>) {
        return runtime::Value{static_cast<bool>(value)};
    } else if constexpr (
        std::is_integral_v<ValueType> &&
        !std::is_same_v<ValueType, bool> &&
        sizeof(ValueType) <= sizeof(std::int32_t)) {
        return runtime::Value{static_cast<std::int64_t>(value)};
    } else if constexpr (
        std::is_integral_v<ValueType> &&
        !std::is_same_v<ValueType, bool>) {
        return runtime::Value{runtime::LongValue{static_cast<std::int64_t>(value)}};
    } else if constexpr (std::is_floating_point_v<ValueType>) {
        return runtime::Value{static_cast<double>(value)};
    } else if constexpr (
        std::is_same_v<ValueType, std::string> ||
        std::is_same_v<ValueType, std::string_view>) {
        return runtime::Value{std::string(value)};
    } else if constexpr (std::is_same_v<ValueType, const char*>) {
        return runtime::Value{std::string(value ? value : "")};
    } else if constexpr (IsSharedPtr<ValueType>::value) {
        using Element = typename IsSharedPtr<ValueType>::Element;
        return writeNativeObject<Element>(state, std::forward<T>(value), error);
    } else if constexpr (std::is_pointer_v<ValueType>) {
        using Element = std::remove_pointer_t<ValueType>;
        if (!value) return runtime::Value{runtime::NullObject{}};
        return writeNativeObject<Element>(
            state,
            std::shared_ptr<Element>(value, [](Element*) {}),
            error);
    } else {
        static_assert(!sizeof(T), "unsupported C++ return type for RealScript game binding");
    }
}

template <typename Argument, typename Storage>
decltype(auto) passArgument(Storage& value) {
    if constexpr (std::is_lvalue_reference_v<Argument>) {
        return static_cast<Argument>(value);
    } else if constexpr (std::is_rvalue_reference_v<Argument>) {
        return std::move(value);
    } else {
        return value;
    }
}

template <typename Callable, typename Traits, std::size_t... Indices>
std::optional<runtime::Value> invokeFree(
    const std::shared_ptr<GameApiState>& state,
    Callable& callable,
    const std::vector<runtime::Value>& arguments,
    runtime::RuntimeError& error,
    std::index_sequence<Indices...>) {
    if (arguments.size() != sizeof...(Indices)) {
        setInvalidArguments(error, "native function received the wrong argument count");
        return std::nullopt;
    }
    using ArgumentTuple = typename Traits::Arguments;
    auto converted = std::make_tuple(
        readValue<std::tuple_element_t<Indices, ArgumentTuple>>(
            state, arguments[Indices], error)...);
    if (!(... && std::get<Indices>(converted).has_value())) return std::nullopt;

    using Return = typename Traits::Return;
    if constexpr (std::is_void_v<Return>) {
        std::invoke(
            callable,
            passArgument<std::tuple_element_t<Indices, ArgumentTuple>>(
                *std::get<Indices>(converted))...);
        return runtime::Value{};
    } else {
        decltype(auto) result = std::invoke(
            callable,
            passArgument<std::tuple_element_t<Indices, ArgumentTuple>>(
                *std::get<Indices>(converted))...);
        return writeValue(state, std::forward<decltype(result)>(result), error);
    }
}

template <auto Method, typename Traits, std::size_t... Indices>
std::optional<runtime::Value> invokeMethod(
    const std::shared_ptr<GameApiState>& state,
    const std::vector<runtime::Value>& arguments,
    runtime::RuntimeError& error,
    std::index_sequence<Indices...>) {
    using Class = typename Traits::Class;
    if (arguments.size() != sizeof...(Indices) + 1) {
        setInvalidArguments(error, "native method received the wrong argument count");
        return std::nullopt;
    }
    const auto receiver = readNativeObject<Class>(state, arguments.front(), error);
    if (!receiver || !*receiver) {
        if (error.code == runtime::ErrorCode::None) {
            error.code = runtime::ErrorCode::NullReference;
            error.message = "native method receiver is null";
        }
        return std::nullopt;
    }

    using ArgumentTuple = typename Traits::Arguments;
    auto converted = std::make_tuple(
        readValue<std::tuple_element_t<Indices, ArgumentTuple>>(
            state, arguments[Indices + 1], error)...);
    if (!(... && std::get<Indices>(converted).has_value())) return std::nullopt;

    using Return = typename Traits::Return;
    if constexpr (std::is_void_v<Return>) {
        std::invoke(
            Method,
            **receiver,
            passArgument<std::tuple_element_t<Indices, ArgumentTuple>>(
                *std::get<Indices>(converted))...);
        return runtime::Value{};
    } else {
        decltype(auto) result = std::invoke(
            Method,
            **receiver,
            passArgument<std::tuple_element_t<Indices, ArgumentTuple>>(
                *std::get<Indices>(converted))...);
        return writeValue(state, std::forward<decltype(result)>(result), error);
    }
}

template <typename Tuple, std::size_t... Indices>
std::optional<std::vector<ParameterDeclaration>> makeParameters(
    const std::shared_ptr<GameApiState>& state,
    std::index_sequence<Indices...>) {
    std::vector<ParameterDeclaration> result;
    result.reserve(sizeof...(Indices));
    bool valid = true;
    [[maybe_unused]] const auto append = [&](auto indexConstant) {
        constexpr std::size_t Index = decltype(indexConstant)::value;
        auto type = resolveScriptType<std::tuple_element_t<Index, Tuple>>(state);
        if (!type) {
            valid = false;
            return;
        }
        result.push_back(ParameterDeclaration{std::move(*type), "arg" + std::to_string(Index)});
    };
    (append(std::integral_constant<std::size_t, Indices>{}), ...);
    if (!valid) return std::nullopt;
    return result;
}

inline void collectImports(
    ModuleDeclaration& module,
    const ScriptTypeRef& type) {
    if (!type.moduleName.empty() && type.moduleName != module.name) {
        module.imports.insert(type.moduleName);
    }
}

} // namespace realscript::game::detail
