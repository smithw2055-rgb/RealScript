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
    if (method.descriptor.ownerTypeId != receiver.type().descriptor.id) {
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
        const auto typeId = exactTypeId(field.type, field.typeName);
        if (!valueMatches(value, field.type, typeId)) {
            error.code = runtime::ErrorCode::TypeMismatch;
            error.message = "script field '" + name + "' received a value of the wrong type";
            return false;
        }
        return heap_->fieldSet(object.reference(), field.index, std::move(value), &error);
    }
    for (const auto& property : object.type().descriptor.properties) {
        if (property.name != name || !property.setter) continue;
        const auto execution = invoke(
            object, ScriptMethod{*property.setter}, {std::move(value)}, std::move(options));
        if (!execution.succeeded) {
            error = execution.error;
            return false;
        }
        return true;
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
            auto value = heap_->fieldGet(object.reference(), field.index);
            if (!value) {
                error.code = runtime::ErrorCode::InvalidObjectReference;
                error.message = "failed to read script field '" + name + "'";
            }
            return value;
        }
    }
    for (const auto& property : object.type().descriptor.properties) {
        if (property.name != name || !property.getter) continue;
        const auto execution = invoke(
            object, ScriptMethod{*property.getter}, {}, std::move(options));
        if (!execution.succeeded) {
            error = execution.error;
            return std::nullopt;
        }
        return execution.value;
    }
    error.code = runtime::ErrorCode::FunctionNotFound;
    error.message = "script member '" + name + "' was not found or is write-only";
    return std::nullopt;
}

} // namespace realscript::game
