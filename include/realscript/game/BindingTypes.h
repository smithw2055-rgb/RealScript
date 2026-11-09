#pragma once

#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/game/DeterministicMath.h"
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

#if defined(__cpp_rtti) || defined(_CPPRTTI)
using CppTypeKey = std::type_index;
template <typename T>
[[nodiscard]] inline CppTypeKey cppTypeKey() noexcept {
    return std::type_index(typeid(T));
}
#else
using CppTypeKey = const void*;
template <typename T>
struct CppTypeTag {
    static inline constexpr char id = 0;
};
template <>
struct CppTypeTag<void> {
    static inline constexpr char id = 0;
};
template <typename T>
[[nodiscard]] inline CppTypeKey cppTypeKey() noexcept {
    return &CppTypeTag<std::remove_cv_t<std::remove_reference_t<T>>>::id;
}
#endif

enum class ScriptTypeCategory {
    Void,
    Bool,
    Int,
    Long,
    Double,
    String,
    Struct,
    NativeObject,
    Array,
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
    runtime::BindingDeterminism determinism =
        runtime::BindingDeterminism::Deterministic;
    std::string requiredCapability;
    bool isStatic = false;
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
    CppTypeKey cppType = cppTypeKey<void>();
    std::string moduleName;
    std::string name;
    semantic::SymbolId typeId = 0;
    std::vector<FunctionDeclaration> methods;
    std::vector<PropertyDeclaration> properties;
};

struct NativeStructFieldDeclaration {
    std::string name;
    ScriptTypeRef type;
    std::function<std::optional<runtime::Value>(
        const std::shared_ptr<struct GameApiState>&,
        const void*,
        runtime::RuntimeError&)> readField;
    std::function<bool(
        const std::shared_ptr<struct GameApiState>&,
        void*,
        const runtime::Value&,
        runtime::RuntimeError&)> writeField;
};

struct NativeStructDeclaration {
    CppTypeKey cppType = cppTypeKey<void>();
    std::string moduleName;
    std::string name;
    semantic::SymbolId typeId = 0;
    std::vector<NativeStructFieldDeclaration> fields;
};

struct ModuleDeclaration {
    std::string name;
    std::vector<FunctionDeclaration> functions;
    std::vector<std::size_t> nativeTypes;
    std::vector<std::size_t> nativeStructs;
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
    std::unordered_map<CppTypeKey, std::size_t> nativeTypeByCppType;
    std::vector<NativeStructDeclaration> nativeStructs;
    std::unordered_map<CppTypeKey, std::size_t> nativeStructByCppType;
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
struct IsStdVector : std::false_type {};

template <typename T, typename Alloc>
struct IsStdVector<std::vector<T, Alloc>> : std::true_type {
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
    static constexpr bool IsMemberFunction = false;
};

template <typename R, typename... Args>
struct FunctionTraits<R (*)(Args...)> : FunctionTraits<R(Args...)> {};

template <typename C, typename R, typename... Args>
struct FunctionTraits<R (C::*)(Args...)> : FunctionTraits<R(Args...)> {
    using Class = C;
    static constexpr bool IsConst = false;
    static constexpr bool IsMemberFunction = true;
};

template <typename C, typename R, typename... Args>
struct FunctionTraits<R (C::*)(Args...) const> : FunctionTraits<R(Args...)> {
    using Class = C;
    static constexpr bool IsConst = true;
    static constexpr bool IsMemberFunction = true;
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
    } else if constexpr (std::is_same_v<ValueType, std::uint8_t>) {
        return ScriptTypeRef{"byte", {}, ScriptTypeCategory::Int};
    } else if constexpr (std::is_same_v<ValueType, std::int8_t>) {
        return ScriptTypeRef{"sbyte", {}, ScriptTypeCategory::Int};
    } else if constexpr (std::is_same_v<ValueType, std::int16_t>) {
        return ScriptTypeRef{"short", {}, ScriptTypeCategory::Int};
    } else if constexpr (std::is_same_v<ValueType, std::uint16_t>) {
        return ScriptTypeRef{"ushort", {}, ScriptTypeCategory::Int};
    } else if constexpr (std::is_same_v<ValueType, char16_t>) {
        return ScriptTypeRef{"char", {}, ScriptTypeCategory::Int};
    } else if constexpr (std::is_same_v<ValueType, std::uint32_t>) {
        return ScriptTypeRef{"uint", {}, ScriptTypeCategory::Long};
    } else if constexpr (std::is_same_v<ValueType, std::uint64_t>) {
        return ScriptTypeRef{"ulong", {}, ScriptTypeCategory::Long};
    } else if constexpr (std::is_same_v<ValueType, float>) {
        return ScriptTypeRef{"float", {}, ScriptTypeCategory::Double};
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
    } else if constexpr (std::is_same_v<ValueType, Fixed>) {
        return ScriptTypeRef{"Fixed", "RealScript.Math", ScriptTypeCategory::Struct};
    } else if constexpr (std::is_same_v<ValueType, Fixed16>) {
        return ScriptTypeRef{"Fixed16", "RealScript.Math", ScriptTypeCategory::Struct};
    } else if constexpr (std::is_same_v<ValueType, Vec2>) {
        return ScriptTypeRef{"Vec2", "RealScript.Math", ScriptTypeCategory::Struct};
    } else if constexpr (std::is_same_v<ValueType, Vec3>) {
        return ScriptTypeRef{"Vec3", "RealScript.Math", ScriptTypeCategory::Struct};
    } else if constexpr (std::is_same_v<ValueType, Quat>) {
        return ScriptTypeRef{"Quat", "RealScript.Math", ScriptTypeCategory::Struct};
    } else if constexpr (IsSharedPtr<ValueType>::value) {
        using Element = typename IsSharedPtr<ValueType>::Element;
        const auto found = state->nativeTypeByCppType.find(cppTypeKey<Element>());
        if (found == state->nativeTypeByCppType.end()) return std::nullopt;
        const auto& type = state->nativeTypes[found->second];
        return ScriptTypeRef{type.name, type.moduleName, ScriptTypeCategory::NativeObject};
    } else if constexpr (std::is_pointer_v<ValueType>) {
        using Element = std::remove_pointer_t<ValueType>;
        const auto found = state->nativeTypeByCppType.find(cppTypeKey<Element>());
        if (found == state->nativeTypeByCppType.end()) return std::nullopt;
        const auto& type = state->nativeTypes[found->second];
        return ScriptTypeRef{type.name, type.moduleName, ScriptTypeCategory::NativeObject};
    } else if constexpr (IsStdVector<ValueType>::value) {
        using Element = typename IsStdVector<ValueType>::Element;
        auto elemRef = resolveScriptType<Element>(state);
        if (!elemRef || elemRef->category == ScriptTypeCategory::Void ||
            elemRef->category == ScriptTypeCategory::Array) {
            return std::nullopt;
        }
        return ScriptTypeRef{
            elemRef->sourceName + "[]",
            elemRef->moduleName,
            ScriptTypeCategory::Array};
    } else if constexpr (std::is_class_v<ValueType> && std::is_default_constructible_v<ValueType>) {
        const auto found = state->nativeStructByCppType.find(cppTypeKey<ValueType>());
        if (found == state->nativeStructByCppType.end()) return std::nullopt;
        const auto& st = state->nativeStructs[found->second];
        return ScriptTypeRef{st.name, st.moduleName, ScriptTypeCategory::Struct};
    } else {
        return std::nullopt;
    }
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
    CppTypeKey type) {
    const auto found = state->nativeTypeByCppType.find(type);
    return found == state->nativeTypeByCppType.end()
        ? nullptr
        : &state->nativeTypes[found->second];
}

} // namespace detail
} // namespace realscript::game
