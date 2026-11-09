#pragma once

#include "realscript/game/BindingTypes.h"

namespace realscript::game::detail {

template <typename T>
std::optional<std::shared_ptr<T>> readNativeObject(
    const std::shared_ptr<GameApiState>& state,
    const runtime::Value& value,
    runtime::RuntimeError& error) {
    const auto* type = findNativeType(state, cppTypeKey<T>());
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
    if (!actualType) {
        setTypeMismatch(error, "script native-object wrapper has the wrong type");
        return std::nullopt;
    }
    const auto handleValue = state->heap->fieldGet(*reference, 0);
    if (!handleValue || !std::holds_alternative<runtime::NativeHandle>(*handleValue)) {
        if (*actualType != type->typeId) {
            setTypeMismatch(error, "script native-object wrapper has the wrong type");
        } else {
            setTypeMismatch(error, "script native-object wrapper does not contain a native handle");
        }
        return std::nullopt;
    }
    const auto handle = std::get<runtime::NativeHandle>(*handleValue);
    if (*actualType != type->typeId && handle.typeId != type->typeId) {
        setTypeMismatch(error, "script native-object wrapper has the wrong type");
        return std::nullopt;
    }
    auto resource = state->nativeHandles->resolve(
        handle, type->typeId, &error);
    if (!resource) return std::nullopt;
    return std::static_pointer_cast<T>(std::move(resource));
}

inline runtime::Value makeFixedStructValue(Fixed value) {
    return runtime::FixedValue{value.raw};
}

inline runtime::Value makeFixed16StructValue(Fixed16 value) {
    return runtime::Fixed16Value{static_cast<std::int64_t>(value.raw)};
}

inline std::optional<Fixed> extractFixedStructValue(const runtime::Value& value) {
    if (const auto* fixed = std::get_if<runtime::FixedValue>(&value)) {
        return Fixed::fromRaw(fixed->raw);
    }
    const auto* structure = std::get_if<runtime::StructValue>(&value);
    if (!structure ||
        structure->typeId != runtime::fixedTypeId() ||
        !structure->storage || structure->storage->fields.size() != 1) {
        return std::nullopt;
    }
    const auto* raw = std::get_if<std::int64_t>(&structure->storage->fields[0]);
    if (!raw) return std::nullopt;
    return Fixed::fromRaw(static_cast<std::int32_t>(*raw));
}

inline std::optional<Fixed16> extractFixed16StructValue(const runtime::Value& value) {
    if (const auto* fixed = std::get_if<runtime::Fixed16Value>(&value)) {
        return Fixed16::fromRaw(static_cast<std::int32_t>(fixed->raw));
    }
    const auto* structure = std::get_if<runtime::StructValue>(&value);
    if (!structure ||
        structure->typeId != runtime::fixed16TypeId() ||
        !structure->storage || structure->storage->fields.size() != 1) {
        return std::nullopt;
    }
    if (const auto* raw = std::get_if<std::int64_t>(&structure->storage->fields[0])) {
        return Fixed16::fromRaw(static_cast<std::int32_t>(*raw));
    }
    if (const auto* rawLong = std::get_if<runtime::LongValue>(&structure->storage->fields[0])) {
        return Fixed16::fromRaw(static_cast<std::int32_t>(rawLong->value));
    }
    return std::nullopt;
}

template <typename T>
std::optional<runtime::Value> writeNativeObject(
    const std::shared_ptr<GameApiState>& state,
    std::shared_ptr<T> value,
    runtime::RuntimeError& error) {
    const auto* type = findNativeType(state, cppTypeKey<T>());
    if (!type) {
        setTypeMismatch(error, "native C++ type is not registered with the game script API");
        return std::nullopt;
    }
    if (!value) return runtime::Value{runtime::NullObject{}};
    const auto heapId = state->heap->heapId();
    if (const auto existingHandle = state->nativeHandles->findHandle(type->typeId, value.get())) {
        if (const auto cachedRef = state->nativeHandles->cachedWrapper(*existingHandle, heapId);
            cachedRef && state->heap->isAlive(*cachedRef)) {
            return runtime::Value{*cachedRef};
        }
        std::vector<runtime::Value> fields;
        fields.emplace_back(*existingHandle);
        auto object = state->heap->allocateObject(type->typeId, std::move(fields), {}, &error);
        if (!object) return std::nullopt;
        (void)state->nativeHandles->setCachedWrapper(*existingHandle, *object, heapId);
        return runtime::Value{*object};
    }
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
    (void)state->nativeHandles->setCachedWrapper(handle, *object, heapId);
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
        return std::nullopt;
    } else if constexpr (std::is_same_v<ValueType, std::uint8_t>) {
        if (const auto* result = std::get_if<runtime::ByteValue>(&value)) return result->value;
        setTypeMismatch(error, "expected byte argument");
        return std::nullopt;
    } else if constexpr (std::is_same_v<ValueType, std::int8_t>) {
        if (const auto* result = std::get_if<runtime::SByteValue>(&value)) return result->value;
        setTypeMismatch(error, "expected sbyte argument");
        return std::nullopt;
    } else if constexpr (std::is_same_v<ValueType, std::int16_t>) {
        if (const auto* result = std::get_if<runtime::ShortValue>(&value)) return result->value;
        setTypeMismatch(error, "expected short argument");
        return std::nullopt;
    } else if constexpr (std::is_same_v<ValueType, std::uint16_t>) {
        if (const auto* result = std::get_if<runtime::UShortValue>(&value)) return result->value;
        setTypeMismatch(error, "expected ushort argument");
        return std::nullopt;
    } else if constexpr (std::is_same_v<ValueType, char16_t>) {
        if (const auto* result = std::get_if<runtime::CharValue>(&value)) return result->value;
        setTypeMismatch(error, "expected char argument");
        return std::nullopt;
    } else if constexpr (std::is_same_v<ValueType, std::uint32_t>) {
        if (const auto* result = std::get_if<runtime::UIntValue>(&value)) return result->value;
        setTypeMismatch(error, "expected uint argument");
        return std::nullopt;
    } else if constexpr (std::is_same_v<ValueType, std::uint64_t>) {
        if (const auto* result = std::get_if<runtime::ULongValue>(&value)) return result->value;
        setTypeMismatch(error, "expected ulong argument");
        return std::nullopt;
    } else if constexpr (std::is_same_v<ValueType, float>) {
        if (const auto* result = std::get_if<runtime::FloatValue>(&value)) return result->value;
        setTypeMismatch(error, "expected float argument");
        return std::nullopt;
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
        return std::nullopt;
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
        return std::nullopt;
    } else if constexpr (std::is_floating_point_v<ValueType>) {
        if (const auto* result = std::get_if<double>(&value)) {
            return static_cast<ValueType>(*result);
        }
        setTypeMismatch(error, "expected double argument");
        return std::nullopt;
    } else if constexpr (
        std::is_same_v<ValueType, std::string> ||
        std::is_same_v<ValueType, std::string_view>) {
        if (const auto* result = std::get_if<std::string>(&value)) {
            return ValueType(*result);
        }
        if (std::holds_alternative<runtime::NullString>(value)) {
            return ValueType{};
        }
        if (const auto* reference = std::get_if<runtime::ObjectRef>(&value)) {
            const auto text = state->heap->stringView(*reference);
            if (text) return ValueType(*text);
        }
        setTypeMismatch(error, "expected string argument");
        return std::nullopt;
    } else if constexpr (std::is_same_v<ValueType, Fixed>) {
        if (const auto fixed = extractFixedStructValue(value)) return *fixed;
        setTypeMismatch(error, "expected Fixed argument");
        return std::nullopt;
    } else if constexpr (std::is_same_v<ValueType, Fixed16>) {
        if (const auto fixed = extractFixed16StructValue(value)) return *fixed;
        setTypeMismatch(error, "expected Fixed16 argument");
        return std::nullopt;
    } else if constexpr (std::is_same_v<ValueType, Vec2>) {
        if (const auto* vec2 = std::get_if<runtime::Vec2Value>(&value)) {
            return Vec2{Fixed::fromRaw(vec2->x), Fixed::fromRaw(vec2->y)};
        }
        const auto* structure = std::get_if<runtime::StructValue>(&value);
        if (structure &&
            structure->typeId == runtime::vec2TypeId() &&
            structure->storage && structure->storage->fields.size() == 2) {
            const auto x = extractFixedStructValue(structure->storage->fields[0]);
            const auto y = extractFixedStructValue(structure->storage->fields[1]);
            if (x && y) return Vec2{*x, *y};
        }
        setTypeMismatch(error, "expected Vec2 argument");
        return std::nullopt;
    } else if constexpr (std::is_same_v<ValueType, Vec3>) {
        if (const auto* vec3 = std::get_if<runtime::Vec3Value>(&value)) {
            return Vec3{Fixed::fromRaw(vec3->x), Fixed::fromRaw(vec3->y), Fixed::fromRaw(vec3->z)};
        }
        const auto* structure = std::get_if<runtime::StructValue>(&value);
        if (structure &&
            structure->typeId == runtime::vec3TypeId() &&
            structure->storage && structure->storage->fields.size() == 3) {
            const auto x = extractFixedStructValue(structure->storage->fields[0]);
            const auto y = extractFixedStructValue(structure->storage->fields[1]);
            const auto z = extractFixedStructValue(structure->storage->fields[2]);
            if (x && y && z) return Vec3{*x, *y, *z};
        }
        setTypeMismatch(error, "expected Vec3 argument");
        return std::nullopt;
    } else if constexpr (std::is_same_v<ValueType, Quat>) {
        if (const auto* quat = std::get_if<runtime::QuatValue>(&value)) {
            return Quat{
                Fixed::fromRaw(quat->x),
                Fixed::fromRaw(quat->y),
                Fixed::fromRaw(quat->z),
                Fixed::fromRaw(quat->w)};
        }
        const auto* structure = std::get_if<runtime::StructValue>(&value);
        if (structure &&
            structure->typeId == runtime::quatTypeId() &&
            structure->storage && structure->storage->fields.size() == 4) {
            const auto x = extractFixedStructValue(structure->storage->fields[0]);
            const auto y = extractFixedStructValue(structure->storage->fields[1]);
            const auto z = extractFixedStructValue(structure->storage->fields[2]);
            const auto w = extractFixedStructValue(structure->storage->fields[3]);
            if (x && y && z && w) return Quat{*x, *y, *z, *w};
        }
        setTypeMismatch(error, "expected Quat argument");
        return std::nullopt;
    } else if constexpr (IsSharedPtr<ValueType>::value) {
        using Element = typename IsSharedPtr<ValueType>::Element;
        return readNativeObject<Element>(state, value, error);
    } else if constexpr (std::is_pointer_v<ValueType>) {
        using Element = std::remove_pointer_t<ValueType>;
        const auto object = readNativeObject<Element>(state, value, error);
        if (!object) return std::nullopt;
        return object->get();
    } else if constexpr (IsStdVector<ValueType>::value) {
        using Element = typename IsStdVector<ValueType>::Element;
        if (std::holds_alternative<runtime::NullArray>(value) ||
            std::holds_alternative<runtime::NullObject>(value)) {
            return ValueType{};
        }
        const auto* reference = std::get_if<runtime::ObjectRef>(&value);
        if (!reference || !state->heap) {
            setTypeMismatch(error, "expected array argument");
            return std::nullopt;
        }
        const auto len = state->heap->arrayLength(*reference);
        if (!len) {
            setTypeMismatch(error, "expected array argument");
            return std::nullopt;
        }
        ValueType out;
        out.reserve(*len);
        for (std::size_t i = 0; i < *len; ++i) {
            const auto elemVal = state->heap->arrayGet(*reference, i);
            if (!elemVal) {
                setTypeMismatch(error, "failed to read array element");
                return std::nullopt;
            }
            auto convertedElem = readValue<Element>(state, *elemVal, error);
            if (!convertedElem) return std::nullopt;
            out.push_back(std::move(*convertedElem));
        }
        return out;
    } else if constexpr (std::is_class_v<ValueType> && std::is_default_constructible_v<ValueType>) {
        const auto found = state->nativeStructByCppType.find(cppTypeKey<ValueType>());
        if (found == state->nativeStructByCppType.end()) {
            setTypeMismatch(error, "native C++ struct type is not registered with the game script API");
            return std::nullopt;
        }
        const auto& st = state->nativeStructs[found->second];
        const auto* sVal = std::get_if<runtime::StructValue>(&value);
        if (!sVal || !sVal->valid() || sVal->typeId != st.typeId) {
            setTypeMismatch(error, "expected struct '" + st.name + "' argument");
            return std::nullopt;
        }
        ValueType out{};
        for (std::size_t i = 0; i < st.fields.size(); ++i) {
            const auto fVal = runtime::structValueFieldAt(*sVal, i);
            if (!fVal || !st.fields[i].writeField(state, &out, *fVal, error)) {
                return std::nullopt;
            }
        }
        return out;
    } else {
        static_assert(!sizeof(T), "unsupported C++ argument type for RealScript game binding");
        return std::nullopt;
    }
}

template <typename T>
std::optional<runtime::Value> writeValue(
    const std::shared_ptr<GameApiState>& state,
    T&& value,
    runtime::RuntimeError& error) {
    using ValueType = RemoveCvRef<T>;
    if constexpr (std::is_same_v<ValueType, bool>) {
        return runtime::Value{static_cast<bool>(value)};
    } else if constexpr (std::is_same_v<ValueType, std::uint8_t>) {
        return runtime::Value{runtime::ByteValue{value}};
    } else if constexpr (std::is_same_v<ValueType, std::int8_t>) {
        return runtime::Value{runtime::SByteValue{value}};
    } else if constexpr (std::is_same_v<ValueType, std::int16_t>) {
        return runtime::Value{runtime::ShortValue{value}};
    } else if constexpr (std::is_same_v<ValueType, std::uint16_t>) {
        return runtime::Value{runtime::UShortValue{value}};
    } else if constexpr (std::is_same_v<ValueType, char16_t>) {
        return runtime::Value{runtime::CharValue{value}};
    } else if constexpr (std::is_same_v<ValueType, std::uint32_t>) {
        return runtime::Value{runtime::UIntValue{value}};
    } else if constexpr (std::is_same_v<ValueType, std::uint64_t>) {
        return runtime::Value{runtime::ULongValue{value}};
    } else if constexpr (std::is_same_v<ValueType, float>) {
        return runtime::Value{runtime::FloatValue{value}};
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
    } else if constexpr (std::is_same_v<ValueType, Fixed>) {
        return makeFixedStructValue(value);
    } else if constexpr (std::is_same_v<ValueType, Fixed16>) {
        return makeFixed16StructValue(value);
    } else if constexpr (std::is_same_v<ValueType, Vec2>) {
        return runtime::Value{runtime::Vec2Value{value.x.raw, value.y.raw}};
    } else if constexpr (std::is_same_v<ValueType, Vec3>) {
        return runtime::Value{runtime::Vec3Value{value.x.raw, value.y.raw, value.z.raw}};
    } else if constexpr (std::is_same_v<ValueType, Quat>) {
        return runtime::Value{runtime::QuatValue{value.x.raw, value.y.raw, value.z.raw, value.w.raw}};
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
    } else if constexpr (IsStdVector<ValueType>::value) {
        using Element = typename IsStdVector<ValueType>::Element;
        auto elemRef = resolveScriptType<Element>(state);
        if (!elemRef || !state->heap) {
            setTypeMismatch(error, "unsupported C++ vector element type");
            return std::nullopt;
        }
        semantic::PrimitiveType elemPrim = semantic::PrimitiveType::Error;
        semantic::SymbolId elemTypeId = 0;
        semantic::SymbolId arrTypeId = 0;
        runtime::Value defaultElemVal{};
        if (elemRef->category == ScriptTypeCategory::NativeObject) {
            elemPrim = semantic::PrimitiveType::Object;
            const std::string canonicalElem = elemRef->moduleName.empty()
                ? elemRef->sourceName
                : (elemRef->moduleName + "::" + elemRef->sourceName);
            elemTypeId = semantic::stableTypeId(canonicalElem);
            arrTypeId = semantic::stableTypeId(
                semantic::arrayTypeName(semantic::PrimitiveType::Object, canonicalElem));
            defaultElemVal = runtime::NullObject{};
        } else if (elemRef->category == ScriptTypeCategory::Struct) {
            elemPrim = semantic::PrimitiveType::Struct;
            const std::string canonicalElem = elemRef->moduleName.empty()
                ? elemRef->sourceName
                : (elemRef->moduleName + "::" + elemRef->sourceName);
            elemTypeId = semantic::stableTypeId(canonicalElem);
            arrTypeId = semantic::stableTypeId(
                semantic::arrayTypeName(semantic::PrimitiveType::Struct, canonicalElem));
            auto defOpt = writeValue(state, RemoveCvRef<Element>{}, error);
            if (!defOpt) return std::nullopt;
            defaultElemVal = std::move(*defOpt);
        } else if (elemRef->category == ScriptTypeCategory::String) {
            elemPrim = semantic::PrimitiveType::String;
            elemTypeId = 0;
            arrTypeId = semantic::stableTypeId(
                semantic::arrayTypeName(semantic::PrimitiveType::String, {}));
            defaultElemVal = runtime::NullString{};
        } else {
            elemPrim = semantic::resolvePrimitiveType(elemRef->sourceName);
            elemTypeId = 0;
            arrTypeId = semantic::stableTypeId(
                semantic::arrayTypeName(elemPrim, {}));
            auto defOpt = writeValue(state, RemoveCvRef<Element>{}, error);
            if (!defOpt) return std::nullopt;
            defaultElemVal = std::move(*defOpt);
        }
        const auto arrRef = state->heap->allocateTypedArray(
            arrTypeId, elemPrim, elemTypeId, value.size(), defaultElemVal, &error);
        if (!arrRef) return std::nullopt;
        for (std::size_t i = 0; i < value.size(); ++i) {
            auto elemVal = writeValue(state, value[i], error);
            if (!elemVal || !state->heap->arraySet(*arrRef, i, std::move(*elemVal), &error)) {
                return std::nullopt;
            }
        }
        return runtime::Value{*arrRef};
    } else if constexpr (std::is_class_v<ValueType> && std::is_default_constructible_v<ValueType>) {
        const auto found = state->nativeStructByCppType.find(cppTypeKey<ValueType>());
        if (found == state->nativeStructByCppType.end()) {
            setTypeMismatch(error, "native C++ struct type is not registered with the game script API");
            return std::nullopt;
        }
        const auto& st = state->nativeStructs[found->second];
        if (st.fields.size() <= 16u) {
            std::array<runtime::Value, 16> stackFields{};
            for (std::size_t i = 0; i < st.fields.size(); ++i) {
                auto fVal = st.fields[i].readField(state, &value, error);
                if (!fVal) return std::nullopt;
                stackFields[i] = std::move(*fVal);
            }
            return runtime::Value{runtime::makeStructValueFromFields(
                st.typeId, runtime::ValueSpan(stackFields.data(), st.fields.size()))};
        }
        std::vector<runtime::Value> heapFields;
        heapFields.reserve(st.fields.size());
        for (std::size_t i = 0; i < st.fields.size(); ++i) {
            auto fVal = st.fields[i].readField(state, &value, error);
            if (!fVal) return std::nullopt;
            heapFields.push_back(std::move(*fVal));
        }
        return runtime::Value{runtime::makeStructValueFromFields(st.typeId, std::move(heapFields))};
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
    runtime::ValueSpan arguments,
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
    runtime::ValueSpan arguments,
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
