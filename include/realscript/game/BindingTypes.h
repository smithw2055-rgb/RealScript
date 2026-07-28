#pragma once

#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/runtime/Runtime.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace realscript::game {

namespace detail {

enum class ScriptTypeCategory {
    Void,
    Bool,
    Int,
    Long,
    Double,
    String,
    NativeObject,
};

struct ScriptTypeRef {
    std::string sourceName;
    std::string moduleName;
    ScriptTypeCategory category = ScriptTypeCategory::Void;
};

struct ParameterDeclaration {
    ScriptTypeRef type;
    std::string name;
};

struct FunctionDeclaration {
    std::string name;
    ScriptTypeRef returnType;
    std::vector<ParameterDeclaration> parameters;
    std::string body;
};

struct PropertyDeclaration {
    std::string name;
    ScriptTypeRef type;
    bool getter = false;
    bool setter = false;
    std::string getterBody;
    std::string setterBody;
};

struct NativeTypeDeclaration {
    std::type_index cppType = typeid(void);
    std::string moduleName;
    std::string name;
    semantic::SymbolId typeId = 0;
    std::vector<FunctionDeclaration> methods;
    std::vector<PropertyDeclaration> properties;
};

struct ModuleDeclaration {
    std::string name;
    std::vector<FunctionDeclaration> functions;
    std::vector<std::size_t> nativeTypes;
    std::set<std::string> imports;
};

struct GameApiState {
    std::shared_ptr<runtime::BindingRegistry> bindings =
        std::make_shared<runtime::BindingRegistry>();
    std::shared_ptr<runtime::ManagedHeap> heap =
        std::make_shared<runtime::ManagedHeap>();
    std::shared_ptr<runtime::NativeHandleRegistry> nativeHandles =
        std::make_shared<runtime::NativeHandleRegistry>();
    std::vector<NativeTypeDeclaration> nativeTypes;
    std::unordered_map<std::type_index, std::size_t> nativeTypeByCppType;
    std::map<std::string, ModuleDeclaration> modules;
    std::unordered_set<std::string> bindingNames;
    std::unordered_set<std::string> declarationNames;
    std::vector<std::string> errors;
};

template <typename T>
struct IsSharedPtr : std::false_type {};

template <typename T>
struct IsSharedPtr<std::shared_ptr<T>> : std::true_type {
    using Element = T;
};

template <typename T>
using RemoveCvRef = std::remove_cv_t<std::remove_reference_t<T>>;

template <typename T>
struct FunctionTraits;

template <typename R, typename... Args>
struct FunctionTraits<R(Args...)> {
    using Return = R;
    using Arguments = std::tuple<Args...>;
    static constexpr std::size_t Arity = sizeof...(Args);
};

template <typename R, typename... Args>
struct FunctionTraits<R (*)(Args...)> : FunctionTraits<R(Args...)> {};

template <typename C, typename R, typename... Args>
struct FunctionTraits<R (C::*)(Args...)> : FunctionTraits<R(Args...)> {
    using Class = C;
    static constexpr bool IsConst = false;
};

template <typename C, typename R, typename... Args>
struct FunctionTraits<R (C::*)(Args...) const> : FunctionTraits<R(Args...)> {
    using Class = C;
    static constexpr bool IsConst = true;
};

template <typename F>
struct FunctionTraits : FunctionTraits<decltype(&F::operator())> {};

template <typename T>
std::optional<ScriptTypeRef> resolveScriptType(
    const std::shared_ptr<GameApiState>& state) {
    using ValueType = RemoveCvRef<T>;
    if constexpr (std::is_void_v<ValueType>) {
        return ScriptTypeRef{"void", {}, ScriptTypeCategory::Void};
    } else if constexpr (std::is_same_v<ValueType, bool>) {
        return ScriptTypeRef{"bool", {}, ScriptTypeCategory::Bool};
    } else if constexpr (
        std::is_integral_v<ValueType> &&
        !std::is_same_v<ValueType, bool> &&
        sizeof(ValueType) <= sizeof(std::int32_t)) {
        return ScriptTypeRef{"int", {}, ScriptTypeCategory::Int};
    } else if constexpr (
        std::is_integral_v<ValueType> &&
        !std::is_same_v<ValueType, bool>) {
        return ScriptTypeRef{"long", {}, ScriptTypeCategory::Long};
    } else if constexpr (std::is_floating_point_v<ValueType>) {
        return ScriptTypeRef{"double", {}, ScriptTypeCategory::Double};
    } else if constexpr (
        std::is_same_v<ValueType, std::string> ||
        std::is_same_v<ValueType, std::string_view>) {
        return ScriptTypeRef{"string", {}, ScriptTypeCategory::String};
    } else if constexpr (IsSharedPtr<ValueType>::value) {
        using Element = typename IsSharedPtr<ValueType>::Element;
        const auto found = state->nativeTypeByCppType.find(std::type_index(typeid(Element)));
        if (found == state->nativeTypeByCppType.end()) return std::nullopt;
        const auto& type = state->nativeTypes[found->second];
        return ScriptTypeRef{type.name, type.moduleName, ScriptTypeCategory::NativeObject};
    } else if constexpr (std::is_pointer_v<ValueType>) {
        using Element = std::remove_pointer_t<ValueType>;
        const auto found = state->nativeTypeByCppType.find(std::type_index(typeid(Element)));
        if (found == state->nativeTypeByCppType.end()) return std::nullopt;
        const auto& type = state->nativeTypes[found->second];
        return ScriptTypeRef{type.name, type.moduleName, ScriptTypeCategory::NativeObject};
    }
    return std::nullopt;
}

inline void setInvalidArguments(
    runtime::RuntimeError& error,
    std::string message) {
    error.code = runtime::ErrorCode::InvalidArguments;
    error.message = std::move(message);
}

inline void setTypeMismatch(
    runtime::RuntimeError& error,
    std::string message) {
    error.code = runtime::ErrorCode::TypeMismatch;
    error.message = std::move(message);
}

inline const NativeTypeDeclaration* findNativeType(
    const std::shared_ptr<GameApiState>& state,
    std::type_index type) {
    const auto found = state->nativeTypeByCppType.find(type);
    return found == state->nativeTypeByCppType.end()
        ? nullptr
        : &state->nativeTypes[found->second];
}

} // namespace detail
} // namespace realscript::game
