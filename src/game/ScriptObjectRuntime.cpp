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

} // namespace

ScriptRuntime::ScriptRuntime(const GameProgram& program)
    : ScriptRuntime(
        program.program(),
        program.bindings(),
        program.heap(),
        program.nativeHandles()) {}

ScriptRuntime::ScriptRuntime(
    std::shared_ptr<const runtime::ProgramImage> program,
    std::shared_ptr<const runtime::BindingRegistry> bindings,
    std::shared_ptr<runtime::ManagedHeap> heap,
    std::shared_ptr<runtime::NativeHandleRegistry> nativeHandles)
    : program_(std::move(program)),
      bindings_(std::move(bindings)),
      heap_(heap ? std::move(heap) : std::make_shared<runtime::ManagedHeap>()),
      nativeHandles_(nativeHandles
          ? std::move(nativeHandles)
          : std::make_shared<runtime::NativeHandleRegistry>()) {}

std::optional<ScriptType> ScriptRuntime::findType(
    const std::string& canonicalName) const {
    if (!program_) return std::nullopt;
    for (const auto& module : program_->modules()) {
        for (const auto& type : module.types) {
            if (semantic::canonicalTypeName(type) == canonicalName) {
                return ScriptType{type};
            }
        }
    }
    return std::nullopt;
}

std::optional<ScriptMethod> ScriptRuntime::findMethod(
    const ScriptType& type,
    const std::string& name,
    std::size_t visibleArity) const {
    for (const auto& method : type.descriptor.methods) {
        ScriptMethod candidate{method};
        if (method.name == name && candidate.visibleArity() == visibleArity) {
            return candidate;
        }
    }
    for (const auto& property : type.descriptor.properties) {
        if (property.getter && property.getter->name == name) {
            ScriptMethod candidate{*property.getter};
            if (candidate.visibleArity() == visibleArity) return candidate;
        }
        if (property.setter && property.setter->name == name) {
            ScriptMethod candidate{*property.setter};
            if (candidate.visibleArity() == visibleArity) return candidate;
        }
    }
    return std::nullopt;
}

std::optional<ScriptMethod> ScriptRuntime::findConstructor(
    const ScriptType& type,
    const std::vector<runtime::Value>& arguments) const {
    for (const auto& constructor : type.descriptor.constructors) {
        if (constructor.parameters.size() != arguments.size() + 1) continue;
        bool matches = true;
        for (std::size_t index = 0; index < arguments.size(); ++index) {
            const auto& parameter = constructor.parameters[index + 1];
            if (!valueMatches(
                    arguments[index],
                    parameter.type,
                    exactTypeId(parameter.type, parameter.typeName))) {
                matches = false;
                break;
            }
        }
        if (matches) return ScriptMethod{constructor};
    }
    return std::nullopt;
}

std::optional<ScriptObject> ScriptRuntime::createObject(
    const ScriptType& type,
    const std::vector<runtime::Value>& constructorArguments,
    runtime::RuntimeError& error,
    runtime::ExecutionOptions options) const {
    if (!program_ || type.descriptor.kind != semantic::TypeKind::Class) {
        error.code = runtime::ErrorCode::TypeMismatch;
        error.message = "script object creation requires a valid class type";
        return std::nullopt;
    }

    std::vector<runtime::Value> fields;
    std::vector<std::size_t> referenceFields;
    fields.reserve(type.descriptor.fields.size());
    for (std::size_t index = 0; index < type.descriptor.fields.size(); ++index) {
        const auto& field = type.descriptor.fields[index];
        const auto fieldTypeId = exactTypeId(field.type, field.typeName);
        std::unordered_set<semantic::SymbolId> visiting;
        fields.push_back(defaultValue(field.type, fieldTypeId, visiting));
        if (field.type == semantic::PrimitiveType::String ||
            field.type == semantic::PrimitiveType::Object ||
            field.type == semantic::PrimitiveType::Array) {
            referenceFields.push_back(index);
        } else if (field.type == semantic::PrimitiveType::Struct) {
            visiting.clear();
            if (containsManagedReferences(fieldTypeId, visiting)) {
                referenceFields.push_back(index);
            }
        }
    }

    auto reference = heap_->allocateObject(
        type.descriptor.id,
        std::move(fields),
        std::move(referenceFields),
        &error);
    if (!reference) return std::nullopt;

    auto root = heap_->retain(*reference);
    if (!root.valid()) {
        error.code = runtime::ErrorCode::InvalidObjectReference;
        error.message = "failed to retain the new script object";
        return std::nullopt;
    }
    ScriptObject object{type, *reference, std::move(root)};

    if (!constructorArguments.empty() || !type.descriptor.constructors.empty()) {
        const auto constructor = findConstructor(type, constructorArguments);
        if (!constructor) {
            error.code = runtime::ErrorCode::InvalidArguments;
            error.message = "no script constructor matches the supplied arguments";
            return std::nullopt;
        }
        std::vector<runtime::Value> arguments;
        arguments.reserve(constructorArguments.size() + 1);
        arguments.push_back(object.value());
        arguments.insert(
            arguments.end(), constructorArguments.begin(), constructorArguments.end());
        auto execution = invokeSymbol(
            constructor->descriptor.id, arguments, std::move(options));
        if (!execution.succeeded) {
            error = std::move(execution.error);
            return std::nullopt;
        }
    }
    return object;
}

std::optional<ScriptObject> ScriptRuntime::createObject(
    const std::string& canonicalTypeName,
    runtime::RuntimeError& error,
    runtime::ExecutionOptions options) const {
    const auto type = findType(canonicalTypeName);
    if (!type) {
        error.code = runtime::ErrorCode::FunctionNotFound;
        error.message = "script type '" + canonicalTypeName + "' was not found";
        return std::nullopt;
    }
    return createObject(*type, {}, error, std::move(options));
}

} // namespace realscript::game
