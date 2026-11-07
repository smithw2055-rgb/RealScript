#include "realscript/game/GameScripting.h"

#include <utility>

namespace realscript::game {
namespace {

semantic::SymbolId exactTypeId(
    semantic::PrimitiveType type,
    const std::string& typeName) {
    return semantic::isExactType(type) && !typeName.empty()
        ? semantic::stableTypeId(typeName)
        : 0;
}

runtime::ExecutionResult failedResult(
    runtime::ErrorCode code,
    std::string message) {
    runtime::ExecutionResult result;
    result.error.code = code;
    result.error.message = std::move(message);
    return result;
}

bool isNullFor(semantic::PrimitiveType type, const runtime::Value& value) {
    return (type == semantic::PrimitiveType::String &&
            std::holds_alternative<runtime::NullString>(value)) ||
        (type == semantic::PrimitiveType::Object &&
            std::holds_alternative<runtime::NullObject>(value)) ||
        (type == semantic::PrimitiveType::Array &&
            std::holds_alternative<runtime::NullArray>(value));
}

} // namespace

const semantic::TypeSymbol* ScriptRuntime::findTypeById(
    semantic::SymbolId typeId) const {
    if (!program_ || typeId == 0) return nullptr;
    for (const auto& module : program_->modules()) {
        for (const auto& type : module.types) {
            if (type.id == typeId) return &type;
        }
    }
    return nullptr;
}

runtime::Value ScriptRuntime::defaultValue(
    semantic::PrimitiveType type,
    semantic::SymbolId typeId,
    std::unordered_set<semantic::SymbolId>& visiting) const {
    switch (type) {
    case semantic::PrimitiveType::Bool: return false;
    case semantic::PrimitiveType::Byte: return runtime::ByteValue{};
    case semantic::PrimitiveType::SByte: return runtime::SByteValue{};
    case semantic::PrimitiveType::Short: return runtime::ShortValue{};
    case semantic::PrimitiveType::UShort: return runtime::UShortValue{};
    case semantic::PrimitiveType::Int: return std::int64_t{0};
    case semantic::PrimitiveType::UInt: return runtime::UIntValue{};
    case semantic::PrimitiveType::Long: return runtime::LongValue{};
    case semantic::PrimitiveType::ULong: return runtime::ULongValue{};
    case semantic::PrimitiveType::Float: return runtime::FloatValue{};
    case semantic::PrimitiveType::Double: return 0.0;
    case semantic::PrimitiveType::Char: return runtime::CharValue{};
    case semantic::PrimitiveType::String: return runtime::NullString{};
    case semantic::PrimitiveType::Object: return runtime::NullObject{};
    case semantic::PrimitiveType::Array: return runtime::NullArray{};
    case semantic::PrimitiveType::Handle: {
        runtime::NativeHandle handle;
        handle.typeId = typeId;
        return handle;
    }
    case semantic::PrimitiveType::Enum:
        return runtime::EnumValue{typeId, 0};
    case semantic::PrimitiveType::Struct: {
        if (auto inlineVal = runtime::makeDefaultInlineStructValue(typeId)) {
            return *inlineVal;
        }
        if (typeId == 0 || !visiting.insert(typeId).second) {
            return runtime::StructValue{};
        }
        const auto* descriptor = findTypeById(typeId);
        if (!descriptor || descriptor->kind != semantic::TypeKind::Struct) {
            visiting.erase(typeId);
            return runtime::StructValue{};
        }
        auto storage = std::make_shared<runtime::StructStorage>();
        storage->fields.reserve(descriptor->fields.size());
        for (const auto& field : descriptor->fields) {
            storage->fields.push_back(defaultValue(
                field.type,
                exactTypeId(field.type, field.typeName),
                visiting));
        }
        visiting.erase(typeId);
        return runtime::StructValue{typeId, std::move(storage)};
    }
    case semantic::PrimitiveType::Null:
    case semantic::PrimitiveType::Void:
    case semantic::PrimitiveType::Error:
        break;
    }
    return runtime::Value{};
}

bool ScriptRuntime::containsManagedReferences(
    semantic::SymbolId typeId,
    std::unordered_set<semantic::SymbolId>& visiting) const {
    if (typeId == 0 || !visiting.insert(typeId).second) return false;
    const auto* descriptor = findTypeById(typeId);
    if (!descriptor || descriptor->kind != semantic::TypeKind::Struct) {
        visiting.erase(typeId);
        return false;
    }
    for (const auto& field : descriptor->fields) {
        if (field.type == semantic::PrimitiveType::String ||
            field.type == semantic::PrimitiveType::Object ||
            field.type == semantic::PrimitiveType::Array) {
            visiting.erase(typeId);
            return true;
        }
        if (field.type == semantic::PrimitiveType::Struct &&
            containsManagedReferences(
                exactTypeId(field.type, field.typeName), visiting)) {
            visiting.erase(typeId);
            return true;
        }
    }
    visiting.erase(typeId);
    return false;
}

bool ScriptRuntime::valueMatches(
    const runtime::Value& value,
    semantic::PrimitiveType type,
    semantic::SymbolId typeId) const {
    if (isNullFor(type, value)) return true;
    if (runtime::valueType(value) != type) return false;
    switch (type) {
    case semantic::PrimitiveType::Object: {
        if (typeId == 0) return true;
        const auto* reference = std::get_if<runtime::ObjectRef>(&value);
        const auto actual = reference ? heap_->objectTypeId(*reference) : std::nullopt;
        return actual && isTypeAssignable(*actual, typeId);
    }
    case semantic::PrimitiveType::Array: {
        if (typeId == 0) return true;
        const auto* reference = std::get_if<runtime::ObjectRef>(&value);
        const auto actual = reference ? heap_->arrayTypeId(*reference) : std::nullopt;
        return actual && (*actual == 0 || *actual == typeId);
    }
    case semantic::PrimitiveType::Struct: {
        const auto actualTypeId = runtime::structValueTypeId(value);
        if (!actualTypeId || *actualTypeId == 0 || *actualTypeId != typeId) return false;
        if (const auto* structure = std::get_if<runtime::StructValue>(&value)) {
            return structure->storage != nullptr;
        }
        return true;
    }
    case semantic::PrimitiveType::Enum: {
        const auto* enumeration = std::get_if<runtime::EnumValue>(&value);
        return enumeration && enumeration->typeId == typeId;
    }
    case semantic::PrimitiveType::Handle: {
        const auto* handle = std::get_if<runtime::NativeHandle>(&value);
        return handle && (typeId == 0 || handle->typeId == 0 || handle->typeId == typeId);
    }
    default:
        return true;
    }
}

runtime::ExecutionResult ScriptRuntime::invokeSymbol(
    semantic::SymbolId symbolId,
    const std::vector<runtime::Value>& arguments,
    runtime::ExecutionOptions options) const {
    if (!program_) {
        return failedResult(
            runtime::ErrorCode::InvalidProgram,
            "script runtime has no linked program image");
    }
    if (nativeProgram_ && backend_ != ScriptExecutionBackend::Interpreter) {
        const auto& desc = nativeProgram_->descriptor();
        for (std::uint32_t idx = 0; idx < desc.functionCount; ++idx) {
            if (desc.functions && desc.functions[idx].symbolId == symbolId) {
                return nativeProgram_->invoke(symbolId, arguments, std::move(options));
            }
        }
    }
    return interpreter_.invoke(symbolId, arguments, std::move(options));
}

runtime::Value ScriptRuntime::makeFixed(Fixed value) {
    return runtime::FixedValue{value.raw};
}

runtime::Value ScriptRuntime::makeVec2(Vec2 value) {
    return runtime::Vec2Value{value.x.raw, value.y.raw};
}

runtime::Value ScriptRuntime::makeVec3(Vec3 value) {
    return runtime::Vec3Value{value.x.raw, value.y.raw, value.z.raw};
}

runtime::Value ScriptRuntime::makeQuat(Quat value) {
    return runtime::QuatValue{value.x.raw, value.y.raw, value.z.raw, value.w.raw};
}

std::optional<Fixed> ScriptRuntime::readFixed(
    const runtime::Value& value) noexcept {
    return detail::extractFixedStructValue(value);
}

std::optional<Vec2> ScriptRuntime::readVec2(
    const runtime::Value& value) noexcept {
    if (const auto* vec2 = std::get_if<runtime::Vec2Value>(&value)) {
        return Vec2{Fixed::fromRaw(vec2->x), Fixed::fromRaw(vec2->y)};
    }
    const auto* structure = std::get_if<runtime::StructValue>(&value);
    if (!structure ||
        structure->typeId != runtime::vec2TypeId() ||
        !structure->storage || structure->storage->fields.size() != 2) {
        return std::nullopt;
    }
    const auto x = detail::extractFixedStructValue(structure->storage->fields[0]);
    const auto y = detail::extractFixedStructValue(structure->storage->fields[1]);
    if (!x || !y) return std::nullopt;
    return Vec2{*x, *y};
}

std::optional<Vec3> ScriptRuntime::readVec3(
    const runtime::Value& value) noexcept {
    if (const auto* vec3 = std::get_if<runtime::Vec3Value>(&value)) {
        return Vec3{Fixed::fromRaw(vec3->x), Fixed::fromRaw(vec3->y), Fixed::fromRaw(vec3->z)};
    }
    const auto* structure = std::get_if<runtime::StructValue>(&value);
    if (!structure ||
        structure->typeId != runtime::vec3TypeId() ||
        !structure->storage || structure->storage->fields.size() != 3) {
        return std::nullopt;
    }
    const auto x = detail::extractFixedStructValue(structure->storage->fields[0]);
    const auto y = detail::extractFixedStructValue(structure->storage->fields[1]);
    const auto z = detail::extractFixedStructValue(structure->storage->fields[2]);
    if (!x || !y || !z) return std::nullopt;
    return Vec3{*x, *y, *z};
}

std::optional<Quat> ScriptRuntime::readQuat(
    const runtime::Value& value) noexcept {
    if (const auto* quat = std::get_if<runtime::QuatValue>(&value)) {
        return Quat{
            Fixed::fromRaw(quat->x),
            Fixed::fromRaw(quat->y),
            Fixed::fromRaw(quat->z),
            Fixed::fromRaw(quat->w)};
    }
    const auto* structure = std::get_if<runtime::StructValue>(&value);
    if (!structure ||
        structure->typeId != runtime::quatTypeId() ||
        !structure->storage || structure->storage->fields.size() != 4) {
        return std::nullopt;
    }
    const auto x = detail::extractFixedStructValue(structure->storage->fields[0]);
    const auto y = detail::extractFixedStructValue(structure->storage->fields[1]);
    const auto z = detail::extractFixedStructValue(structure->storage->fields[2]);
    const auto w = detail::extractFixedStructValue(structure->storage->fields[3]);
    if (!x || !y || !z || !w) return std::nullopt;
    return Quat{*x, *y, *z, *w};
}

std::optional<Vec2> ScriptRuntime::getMemberVec2(
    const ScriptObject& object,
    const std::string& name,
    runtime::RuntimeError& error,
    runtime::ExecutionOptions options) const {
    const auto value = getMember(object, name, error, std::move(options));
    if (!value) return std::nullopt;
    const auto vec = readVec2(*value);
    if (!vec) {
        error.code = runtime::ErrorCode::TypeMismatch;
        error.message = "member '" + name + "' is not a RealScript.Math::Vec2";
        return std::nullopt;
    }
    return vec;
}

bool ScriptRuntime::setMemberVec2(
    ScriptObject& object,
    const std::string& name,
    Vec2 value,
    runtime::RuntimeError& error,
    runtime::ExecutionOptions options) const {
    if (object.valid() && heap_) {
        const auto& fields = object.type().descriptor.fields;
        for (std::size_t index = 0; index < fields.size(); ++index) {
            if (fields[index].name != name) continue;
            const auto current = heap_->fieldGet(object.reference(), index);
            if (current) {
                const auto* vecStruct = std::get_if<runtime::StructValue>(&*current);
                if (vecStruct &&
                    vecStruct->typeId == semantic::stableTypeId("RealScript.Math::Vec2") &&
                    vecStruct->storage &&
                    vecStruct->storage.use_count() == 2 &&
                    vecStruct->storage->fields.size() == 2) {
                    auto* mutableVec = const_cast<runtime::StructStorage*>(
                        vecStruct->storage.get());
                    auto* fx = std::get_if<runtime::StructValue>(&mutableVec->fields[0]);
                    auto* fy = std::get_if<runtime::StructValue>(&mutableVec->fields[1]);
                    if (fx && fx->storage && fx->storage.use_count() == 1 &&
                        fx->storage->fields.size() == 1 &&
                        fy && fy->storage && fy->storage.use_count() == 1 &&
                        fy->storage->fields.size() == 1) {
                        const_cast<runtime::StructStorage*>(fx->storage.get())->fields[0] =
                            static_cast<std::int64_t>(value.x.raw);
                        const_cast<runtime::StructStorage*>(fy->storage.get())->fields[0] =
                            static_cast<std::int64_t>(value.y.raw);
                        return true;
                    }
                }
            }
            break;
        }
    }
    return setMember(object, name, makeVec2(value), error, std::move(options));
}

bool ScriptRuntime::readArrayInt(
    runtime::ObjectRef arrayRef,
    std::vector<std::int64_t>& out,
    runtime::RuntimeError& error) const {
    if (!heap_) return false;
    return heap_->readPackedIntArray(arrayRef, out, &error);
}

bool ScriptRuntime::writeArrayInt(
    runtime::ObjectRef arrayRef,
    const std::vector<std::int64_t>& values,
    runtime::RuntimeError& error) const {
    if (!heap_) return false;
    return heap_->writePackedIntArray(arrayRef, values.data(), values.size(), &error);
}

bool ScriptRuntime::readArrayVec2(
    runtime::ObjectRef arrayRef,
    std::vector<Vec2>& out,
    runtime::RuntimeError& error) const {
    if (!heap_) return false;
    std::vector<runtime::Vec2Value> raw;
    if (!heap_->readPackedVec2Array(arrayRef, raw, &error)) return false;
    out.resize(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        out[i] = Vec2{Fixed::fromRaw(raw[i].x), Fixed::fromRaw(raw[i].y)};
    }
    return true;
}

bool ScriptRuntime::writeArrayVec2(
    runtime::ObjectRef arrayRef,
    const std::vector<Vec2>& values,
    runtime::RuntimeError& error) const {
    if (!heap_) return false;
    std::vector<runtime::Vec2Value> raw(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        raw[i] = runtime::Vec2Value{values[i].x.raw, values[i].y.raw};
    }
    return heap_->writePackedVec2Array(arrayRef, raw.data(), raw.size(), &error);
}

} // namespace realscript::game
