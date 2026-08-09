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
        return actual && *actual == typeId;
    }
    case semantic::PrimitiveType::Array: {
        if (typeId == 0) return true;
        const auto* reference = std::get_if<runtime::ObjectRef>(&value);
        const auto actual = reference ? heap_->arrayTypeId(*reference) : std::nullopt;
        return actual && *actual == typeId;
    }
    case semantic::PrimitiveType::Struct: {
        const auto* structure = std::get_if<runtime::StructValue>(&value);
        return structure && structure->typeId == typeId && structure->storage;
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
    return interpreter_.invoke(symbolId, arguments, std::move(options));
}

} // namespace realscript::game
