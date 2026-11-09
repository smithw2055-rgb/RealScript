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

std::optional<runtime::Value> evaluateFieldConstantValue(
    const semantic::FieldSymbol& field,
    semantic::SymbolId fieldTypeId,
    runtime::ManagedHeap* heap,
    runtime::RuntimeError& error) {
    if (!field.hasConstantValue) return std::nullopt;
    if (std::holds_alternative<std::int64_t>(field.constantValue)) {
        const auto raw = std::get<std::int64_t>(field.constantValue);
        switch (field.type) {
        case semantic::PrimitiveType::Byte:
            return runtime::Value{runtime::ByteValue{static_cast<std::uint8_t>(raw)}};
        case semantic::PrimitiveType::SByte:
            return runtime::Value{runtime::SByteValue{static_cast<std::int8_t>(raw)}};
        case semantic::PrimitiveType::Short:
            return runtime::Value{runtime::ShortValue{static_cast<std::int16_t>(raw)}};
        case semantic::PrimitiveType::UShort:
            return runtime::Value{runtime::UShortValue{static_cast<std::uint16_t>(raw)}};
        case semantic::PrimitiveType::Int:
            return runtime::Value{raw};
        case semantic::PrimitiveType::UInt:
            return runtime::Value{runtime::UIntValue{static_cast<std::uint32_t>(raw)}};
        case semantic::PrimitiveType::Long:
            return runtime::Value{runtime::LongValue{raw}};
        case semantic::PrimitiveType::ULong:
            return runtime::Value{runtime::ULongValue{static_cast<std::uint64_t>(raw)}};
        case semantic::PrimitiveType::Char:
            return runtime::Value{runtime::CharValue{static_cast<char16_t>(raw)}};
        case semantic::PrimitiveType::Enum:
            return runtime::Value{runtime::EnumValue{fieldTypeId, raw}};
        case semantic::PrimitiveType::Float:
            return runtime::Value{runtime::FloatValue{static_cast<float>(raw)}};
        case semantic::PrimitiveType::Double:
            return runtime::Value{static_cast<double>(raw)};
        default:
            break;
        }
    } else if (std::holds_alternative<double>(field.constantValue)) {
        const auto raw = std::get<double>(field.constantValue);
        if (field.type == semantic::PrimitiveType::Float) {
            return runtime::Value{runtime::FloatValue{static_cast<float>(raw)}};
        }
        return runtime::Value{raw};
    } else if (std::holds_alternative<bool>(field.constantValue)) {
        return runtime::Value{std::get<bool>(field.constantValue)};
    } else if (std::holds_alternative<std::string>(field.constantValue)) {
        const auto& strVal = std::get<std::string>(field.constantValue);
        if (heap) {
            if (const auto strRef = heap->allocateString(strVal, &error)) {
                return runtime::Value{*strRef};
            }
            return std::nullopt;
        }
        return runtime::Value{strVal};
    }
    return std::nullopt;
}

} // namespace

runtime::ExecutionResult ScriptRuntime::invoke(
    const ScriptObject& receiver,
    const ScriptMethod& method,
    const std::vector<runtime::Value>& arguments,
    runtime::ExecutionOptions options) const {
    if (!receiver.valid()) {
        return failedResult(
            runtime::ErrorCode::InvalidObjectReference,
            "cannot invoke a method on an invalid script object");
    }
    if (!method.valid() || !method.instance()) {
        return failedResult(
            runtime::ErrorCode::InvalidArguments,
            "script method is not a valid instance method");
    }
    if (!isTypeAssignable(receiver.type().descriptor.id, method.descriptor.ownerTypeId)) {
        return failedResult(
            runtime::ErrorCode::TypeMismatch,
            "script method belongs to a different receiver type");
    }
    if (method.visibleArity() != arguments.size()) {
        return failedResult(
            runtime::ErrorCode::InvalidArguments,
            "script method received the wrong argument count");
    }
    std::vector<runtime::Value> fullArguments;
    fullArguments.reserve(arguments.size() + 1);
    fullArguments.push_back(receiver.value());
    fullArguments.insert(fullArguments.end(), arguments.begin(), arguments.end());
    return invokeSymbol(method.descriptor.id, fullArguments, std::move(options));
}

runtime::ExecutionResult ScriptRuntime::invokeStatic(
    const ScriptMethod& method,
    const std::vector<runtime::Value>& arguments,
    runtime::ExecutionOptions options) const {
    if (!method.valid() || method.instance()) {
        return failedResult(
            runtime::ErrorCode::InvalidArguments,
            "script method is not a valid static or module function");
    }
    if (method.visibleArity() != arguments.size()) {
        return failedResult(
            runtime::ErrorCode::InvalidArguments,
            "script function received the wrong argument count");
    }
    if (method.descriptor.ownerTypeId != 0) {
        if (const auto* owner = findTypeById(method.descriptor.ownerTypeId);
            owner && owner->staticConstructor &&
            owner->staticConstructor->id != method.descriptor.id) {
            auto cctorExec = invokeSymbol(owner->staticConstructor->id, {}, options);
            if (!cctorExec.succeeded) return cctorExec;
        }
    }
    return invokeSymbol(method.descriptor.id, arguments, std::move(options));
}

bool ScriptRuntime::setMember(
    ScriptObject& object,
    const std::string& name,
    runtime::Value value,
    runtime::RuntimeError& error,
    runtime::ExecutionOptions options) const {
    if (!object.valid()) {
        error.code = runtime::ErrorCode::InvalidObjectReference;
        error.message = "cannot set a member on an invalid script object";
        return false;
    }
    for (const auto& field : object.type().descriptor.fields) {
        if (field.name != name) continue;
        if (field.constField || field.readonlyField) {
            error.code = runtime::ErrorCode::InvalidArguments;
            error.message = "script field '" + name + "' is read-only";
            return false;
        }
        const auto typeId = exactTypeId(field.type, field.typeName);
        if (!valueMatches(value, field.type, typeId)) {
            error.code = runtime::ErrorCode::TypeMismatch;
            error.message = "script field '" + name + "' received a value of the wrong type";
            return false;
        }
        if (field.staticField) {
            if (!heap_) {
                error.code = runtime::ErrorCode::InvalidObjectReference;
                error.message = "cannot set static field '" + name + "' without a managed heap";
                return false;
            }
            heap_->setStaticField(field.id, std::move(value));
            return true;
        }
        return heap_->fieldSet(object.reference(), field.index, std::move(value), &error);
    }
    std::unordered_set<semantic::SymbolId> visited;
    const semantic::TypeSymbol* current = &object.type().descriptor;
    while (current && visited.insert(current->id).second) {
        for (const auto& property : current->properties) {
            if (property.name != name || !property.setter) continue;
            const auto execution = invoke(
                object, ScriptMethod{*property.setter}, {std::move(value)}, std::move(options));
            if (!execution.succeeded) {
                error = execution.error;
                return false;
            }
            return true;
        }
        const semantic::SymbolId baseId = current->baseTypeId != 0
            ? current->baseTypeId
            : (!current->baseTypeName.empty()
                   ? semantic::stableTypeId(current->baseTypeName)
                   : 0);
        current = findTypeById(baseId);
    }
    error.code = runtime::ErrorCode::FunctionNotFound;
    error.message = "script member '" + name + "' was not found or is read-only";
    return false;
}

std::optional<runtime::Value> ScriptRuntime::getMember(
    const ScriptObject& object,
    const std::string& name,
    runtime::RuntimeError& error,
    runtime::ExecutionOptions options) const {
    if (!object.valid()) {
        error.code = runtime::ErrorCode::InvalidObjectReference;
        error.message = "cannot get a member from an invalid script object";
        return std::nullopt;
    }
    for (const auto& field : object.type().descriptor.fields) {
        if (field.name == name) {
            const auto typeId = exactTypeId(field.type, field.typeName);
            if (field.constField) {
                if (auto constVal = evaluateFieldConstantValue(
                        field, typeId, heap_.get(), error)) {
                    return constVal;
                }
                if (error.code != runtime::ErrorCode::None) return std::nullopt;
                std::unordered_set<semantic::SymbolId> visiting;
                return defaultValue(field.type, typeId, visiting);
            }
            if (field.staticField) {
                if (!heap_) {
                    error.code = runtime::ErrorCode::InvalidObjectReference;
                    error.message = "cannot read static field '" + name + "' without a managed heap";
                    return std::nullopt;
                }
                if (auto existing = heap_->getStaticField(field.id)) {
                    return existing;
                }
                if (auto constVal = evaluateFieldConstantValue(
                        field, typeId, heap_.get(), error)) {
                    heap_->setStaticField(field.id, *constVal);
                    return constVal;
                }
                if (error.code != runtime::ErrorCode::None) return std::nullopt;
                std::unordered_set<semantic::SymbolId> visiting;
                return defaultValue(field.type, typeId, visiting);
            }
            auto value = heap_->fieldGet(object.reference(), field.index);
            if (!value) {
                error.code = runtime::ErrorCode::InvalidObjectReference;
                error.message = "failed to read script field '" + name + "'";
            }
            return value;
        }
    }
    std::unordered_set<semantic::SymbolId> visited;
    const semantic::TypeSymbol* current = &object.type().descriptor;
    while (current && visited.insert(current->id).second) {
        for (const auto& property : current->properties) {
            if (property.name != name || !property.getter) continue;
            const auto execution = invoke(
                object, ScriptMethod{*property.getter}, {}, std::move(options));
            if (!execution.succeeded) {
                error = execution.error;
                return std::nullopt;
            }
            return execution.value;
        }
        const semantic::SymbolId baseId = current->baseTypeId != 0
            ? current->baseTypeId
            : (!current->baseTypeName.empty()
                   ? semantic::stableTypeId(current->baseTypeName)
                   : 0);
        current = findTypeById(baseId);
    }
    error.code = runtime::ErrorCode::FunctionNotFound;
    error.message = "script member '" + name + "' was not found or is write-only";
    return std::nullopt;
}

bool ScriptRuntime::setStaticMember(
    const ScriptType& type,
    const std::string& name,
    runtime::Value value,
    runtime::RuntimeError& error,
    runtime::ExecutionOptions options) const {
    for (const auto& field : type.descriptor.fields) {
        if (field.name != name) continue;
        if (field.constField || field.readonlyField) {
            error.code = runtime::ErrorCode::InvalidArguments;
            error.message = "script field '" + name + "' is read-only";
            return false;
        }
        if (!field.staticField) {
            error.code = runtime::ErrorCode::InvalidArguments;
            error.message = "script field '" + name + "' is an instance field";
            return false;
        }
        const auto typeId = exactTypeId(field.type, field.typeName);
        if (!valueMatches(value, field.type, typeId)) {
            error.code = runtime::ErrorCode::TypeMismatch;
            error.message = "script field '" + name + "' received a value of the wrong type";
            return false;
        }
        if (!heap_) {
            error.code = runtime::ErrorCode::InvalidObjectReference;
            error.message = "cannot set static field '" + name + "' without a managed heap";
            return false;
        }
        if (type.descriptor.staticConstructor) {
            auto cctorExec = invokeSymbol(type.descriptor.staticConstructor->id, {}, options);
            if (!cctorExec.succeeded) {
                error = std::move(cctorExec.error);
                return false;
            }
        }
        heap_->setStaticField(field.id, std::move(value));
        return true;
    }
    std::unordered_set<semantic::SymbolId> visited;
    const semantic::TypeSymbol* current = &type.descriptor;
    while (current && visited.insert(current->id).second) {
        for (const auto& property : current->properties) {
            if (property.name != name || !property.setter || !property.staticProperty) continue;
            const auto execution = invokeStatic(
                ScriptMethod{*property.setter}, {std::move(value)}, std::move(options));
            if (!execution.succeeded) {
                error = execution.error;
                return false;
            }
            return true;
        }
        const semantic::SymbolId baseId = current->baseTypeId != 0
            ? current->baseTypeId
            : (!current->baseTypeName.empty()
                   ? semantic::stableTypeId(current->baseTypeName)
                   : 0);
        current = findTypeById(baseId);
    }
    error.code = runtime::ErrorCode::FunctionNotFound;
    error.message = "static script member '" + name + "' was not found or is read-only";
    return false;
}

bool ScriptRuntime::setStaticMember(
    const std::string& canonicalTypeName,
    const std::string& name,
    runtime::Value value,
    runtime::RuntimeError& error,
    runtime::ExecutionOptions options) const {
    const auto type = findType(canonicalTypeName);
    if (!type) {
        error.code = runtime::ErrorCode::FunctionNotFound;
        error.message = "script type '" + canonicalTypeName + "' was not found";
        return false;
    }
    return setStaticMember(*type, name, std::move(value), error, std::move(options));
}

std::optional<runtime::Value> ScriptRuntime::getStaticMember(
    const ScriptType& type,
    const std::string& name,
    runtime::RuntimeError& error,
    runtime::ExecutionOptions options) const {
    for (const auto& field : type.descriptor.fields) {
        if (field.name != name) continue;
        const auto typeId = exactTypeId(field.type, field.typeName);
        if (field.constField) {
            if (auto constVal = evaluateFieldConstantValue(
                    field, typeId, heap_.get(), error)) {
                return constVal;
            }
            if (error.code != runtime::ErrorCode::None) return std::nullopt;
            std::unordered_set<semantic::SymbolId> visiting;
            return defaultValue(field.type, typeId, visiting);
        }
        if (!field.staticField) {
            error.code = runtime::ErrorCode::InvalidArguments;
            error.message = "script field '" + name + "' is an instance field";
            return std::nullopt;
        }
        if (!heap_) {
            error.code = runtime::ErrorCode::InvalidObjectReference;
            error.message = "cannot read static field '" + name + "' without a managed heap";
            return std::nullopt;
        }
        if (type.descriptor.staticConstructor) {
            auto cctorExec = invokeSymbol(type.descriptor.staticConstructor->id, {}, options);
            if (!cctorExec.succeeded) {
                error = std::move(cctorExec.error);
                return std::nullopt;
            }
        }
        if (auto existing = heap_->getStaticField(field.id)) {
            return existing;
        }
        if (auto constVal = evaluateFieldConstantValue(
                field, typeId, heap_.get(), error)) {
            heap_->setStaticField(field.id, *constVal);
            return constVal;
        }
        if (error.code != runtime::ErrorCode::None) return std::nullopt;
        std::unordered_set<semantic::SymbolId> visiting;
        return defaultValue(field.type, typeId, visiting);
    }
    std::unordered_set<semantic::SymbolId> visited;
    const semantic::TypeSymbol* current = &type.descriptor;
    while (current && visited.insert(current->id).second) {
        for (const auto& property : current->properties) {
            if (property.name != name || !property.getter || !property.staticProperty) continue;
            const auto execution = invokeStatic(
                ScriptMethod{*property.getter}, {}, std::move(options));
            if (!execution.succeeded) {
                error = execution.error;
                return std::nullopt;
            }
            return execution.value;
        }
        const semantic::SymbolId baseId = current->baseTypeId != 0
            ? current->baseTypeId
            : (!current->baseTypeName.empty()
                   ? semantic::stableTypeId(current->baseTypeName)
                   : 0);
        current = findTypeById(baseId);
    }
    error.code = runtime::ErrorCode::FunctionNotFound;
    error.message = "static script member '" + name + "' was not found or is write-only";
    return std::nullopt;
}

std::optional<runtime::Value> ScriptRuntime::getStaticMember(
    const std::string& canonicalTypeName,
    const std::string& name,
    runtime::RuntimeError& error,
    runtime::ExecutionOptions options) const {
    const auto type = findType(canonicalTypeName);
    if (!type) {
        error.code = runtime::ErrorCode::FunctionNotFound;
        error.message = "script type '" + canonicalTypeName + "' was not found";
        return std::nullopt;
    }
    return getStaticMember(*type, name, error, std::move(options));
}

} // namespace realscript::game
