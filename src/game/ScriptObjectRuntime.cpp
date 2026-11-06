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

std::string canonicalTypeNameForId(
    const runtime::ProgramImage& program,
    semantic::SymbolId typeId) {
    if (typeId == 0) return {};
    for (const auto& module : program.modules()) {
        for (const auto& type : module.types) {
            if (type.id == typeId) return semantic::canonicalTypeName(type);
        }
    }
    return {};
}

std::optional<ScriptMethod> reflectedBytecodeMethod(
    const runtime::ProgramImage& program,
    const ScriptType& type,
    const std::string& name,
    std::size_t visibleArity) {
    const auto encodedName = type.descriptor.name + "." + name;
    for (const auto& module : program.modules()) {
        for (const auto& function : module.functions) {
            if (function.name != encodedName ||
                function.parameterTypes.empty() ||
                function.parameterTypes.size() !=
                    function.parameterTypeIds.size() ||
                function.parameterTypeIds.front() != type.descriptor.id ||
                function.parameterTypes.size() - 1u != visibleArity) {
                continue;
            }

            semantic::FunctionSymbol descriptor;
            descriptor.id = function.symbolId;
            descriptor.moduleName = module.name;
            descriptor.name = name;
            descriptor.ownerTypeName = type.descriptor.name;
            descriptor.ownerTypeId = type.descriptor.id;
            descriptor.returnType = function.returnType;
            descriptor.returnTypeName = canonicalTypeNameForId(
                program, function.returnTypeId);
            descriptor.method = true;
            descriptor.staticMethod = false;
            descriptor.parameters.reserve(function.parameterTypes.size());
            for (std::size_t index = 0;
                 index < function.parameterTypes.size(); ++index) {
                semantic::VariableSymbol parameter;
                parameter.name = index == 0
                    ? "this"
                    : "arg" + std::to_string(index - 1u);
                parameter.type = function.parameterTypes[index];
                parameter.typeName = canonicalTypeNameForId(
                    program, function.parameterTypeIds[index]);
                parameter.index = index;
                parameter.parameter = true;
                descriptor.parameters.push_back(std::move(parameter));
            }
            return ScriptMethod{std::move(descriptor)};
        }
    }
    return std::nullopt;
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
          : std::make_shared<runtime::NativeHandleRegistry>()),
      interpreter_(program_, heap_) {
    interpreter_.setBindingRegistry(bindings_);
}

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
    std::unordered_set<semantic::SymbolId> visited;
    const semantic::TypeSymbol* current = &type.descriptor;
    while (current && visited.insert(current->id).second) {
        for (const auto& method : current->methods) {
            ScriptMethod candidate{method};
            if (method.name == name && candidate.visibleArity() == visibleArity) {
                return candidate;
            }
        }
        for (const auto& property : current->properties) {
            if (property.getter && property.getter->name == name) {
                ScriptMethod candidate{*property.getter};
                if (candidate.visibleArity() == visibleArity) return candidate;
            }
            if (property.setter && property.setter->name == name) {
                ScriptMethod candidate{*property.setter};
                if (candidate.visibleArity() == visibleArity) return candidate;
            }
        }
        if (program_) {
            if (auto reflected = reflectedBytecodeMethod(
                    *program_, ScriptType{*current}, name, visibleArity)) {
                return reflected;
            }
        }
        const semantic::SymbolId baseId = current->baseTypeId != 0
            ? current->baseTypeId
            : (!current->baseTypeName.empty()
                   ? semantic::stableTypeId(current->baseTypeName)
                   : 0);
        current = findTypeById(baseId);
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

semantic::SymbolId ScriptRuntime::nativeBaseTypeId(
    const ScriptType& type) const noexcept {
    if (type.descriptor.kind != semantic::TypeKind::Class ||
        type.descriptor.fields.empty() ||
        type.descriptor.fields.front().type != semantic::PrimitiveType::Handle) {
        return 0;
    }
    if (type.descriptor.fields.front().declaringTypeId != 0) {
        return type.descriptor.fields.front().declaringTypeId;
    }
    semantic::SymbolId candidateId = type.descriptor.id;
    std::unordered_set<semantic::SymbolId> visited;
    const semantic::TypeSymbol* current = &type.descriptor;
    while (current && visited.insert(current->id).second) {
        if (!current->fields.empty() &&
            current->fields.front().type == semantic::PrimitiveType::Handle) {
            candidateId = current->id;
        }
        const semantic::SymbolId baseId = current->baseTypeId != 0
            ? current->baseTypeId
            : (!current->baseTypeName.empty()
                   ? semantic::stableTypeId(current->baseTypeName)
                   : 0);
        current = findTypeById(baseId);
    }
    return candidateId;
}

std::optional<ScriptObject> ScriptRuntime::allocateUninitializedObject(
    const ScriptType& type,
    runtime::RuntimeError& error) const {
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
    return ScriptObject{type, *reference, std::move(root)};
}

bool ScriptRuntime::runObjectConstructor(
    ScriptObject& object,
    const std::vector<runtime::Value>& constructorArguments,
    runtime::RuntimeError& error,
    runtime::ExecutionOptions options) const {
    const auto& type = object.type();
    if (!constructorArguments.empty() || !type.descriptor.constructors.empty()) {
        const auto constructor = findConstructor(type, constructorArguments);
        if (!constructor) {
            error.code = runtime::ErrorCode::InvalidArguments;
            error.message = "no script constructor matches the supplied arguments";
            return false;
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
            return false;
        }
    }
    return true;
}

std::optional<ScriptObject> ScriptRuntime::createObject(
    const ScriptType& type,
    const std::vector<runtime::Value>& constructorArguments,
    runtime::RuntimeError& error,
    runtime::ExecutionOptions options) const {
    auto object = allocateUninitializedObject(type, error);
    if (!object) return std::nullopt;
    if (!runObjectConstructor(*object, constructorArguments, error, std::move(options))) {
        return std::nullopt;
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

bool ScriptRuntime::isNativeAlive(const ScriptObject& object) const noexcept {
    if (!object.valid() || !heap_ || !nativeHandles_ ||
        object.type().descriptor.fields.empty() ||
        object.type().descriptor.fields.front().type !=
            semantic::PrimitiveType::Handle) {
        return false;
    }
    const auto field = heap_->fieldGet(object.reference(), 0);
    if (!field) return false;
    const auto* handle = std::get_if<runtime::NativeHandle>(&*field);
    if (!handle || !handle->valid()) return false;
    return nativeHandles_->isAlive(*handle);
}

bool ScriptRuntime::releaseNative(ScriptObject& object) const noexcept {
    if (!object.valid() || !heap_ || !nativeHandles_ ||
        object.type().descriptor.fields.empty() ||
        object.type().descriptor.fields.front().type !=
            semantic::PrimitiveType::Handle) {
        return false;
    }
    const auto field = heap_->fieldGet(object.reference(), 0);
    if (!field) return false;
    const auto* handle = std::get_if<runtime::NativeHandle>(&*field);
    if (!handle || !handle->valid()) return false;
    return nativeHandles_->release(*handle);
}

bool ScriptRuntime::isTypeAssignable(
    semantic::SymbolId actualTypeId,
    semantic::SymbolId expectedTypeId) const noexcept {
    if (expectedTypeId == 0 || actualTypeId == expectedTypeId) return true;
    if (actualTypeId == 0 || !program_) return false;

    std::unordered_set<semantic::SymbolId> visited;
    const semantic::TypeSymbol* current = findTypeById(actualTypeId);
    while (current && visited.insert(current->id).second) {
        if (current->id == expectedTypeId) return true;
        for (const auto& iface : current->interfaceDispatchMaps) {
            if (iface.interfaceTypeId == expectedTypeId) return true;
        }
        const semantic::SymbolId baseId = current->baseTypeId != 0
            ? current->baseTypeId
            : (!current->baseTypeName.empty()
                   ? semantic::stableTypeId(current->baseTypeName)
                   : 0);
        if (baseId == expectedTypeId) return true;
        current = findTypeById(baseId);
    }

    const auto* actualDesc = findTypeById(actualTypeId);
    const auto* expectedDesc = findTypeById(expectedTypeId);
    if (actualDesc && expectedDesc && program_) {
        const auto actualCanonical = semantic::canonicalTypeName(*actualDesc);
        const auto expectedCanonical = semantic::canonicalTypeName(*expectedDesc);
        for (const auto& module : program_->modules()) {
            for (const auto& impl : module.languageMetadata.interfaces) {
                if (impl.typeName != actualCanonical &&
                    impl.typeName != actualDesc->name) {
                    continue;
                }
                for (const auto& ifaceName : impl.interfaces) {
                    if (ifaceName == expectedCanonical ||
                        ifaceName == expectedDesc->name) {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

void ScriptRuntime::replaceProgram(const GameProgram& program) {
    program_ = program.program();
    if (program.bindings()) {
        bindings_ = program.bindings();
    }
    interpreter_ = runtime::Interpreter(program_, heap_);
    interpreter_.setBindingRegistry(bindings_);
    clearNativeBackend();
}

void ScriptRuntime::setAotBackend(const aot::ProgramDescriptor* descriptor) {
    jitModule_.reset();
    if (!descriptor) {
        nativeProgram_.reset();
        backend_ = ScriptExecutionBackend::Interpreter;
        return;
    }
    nativeProgram_ = std::make_shared<aot::Program>(*descriptor);
    nativeProgram_->setBindings(bindings_);
    nativeProgram_->setHeap(heap_);
    backend_ = ScriptExecutionBackend::Aot;
}

void ScriptRuntime::setJitBackend(std::shared_ptr<jit::Module> jitModule) {
    if (!jitModule || !jitModule->valid() || !jitModule->descriptor()) {
        clearNativeBackend();
        return;
    }
    jitModule_ = std::move(jitModule);
    jitModule_->setBindings(bindings_);
    jitModule_->setHeap(heap_);
    nativeProgram_ = std::make_shared<aot::Program>(*jitModule_->descriptor());
    nativeProgram_->setBindings(bindings_);
    nativeProgram_->setHeap(heap_);
    backend_ = ScriptExecutionBackend::Jit;
}

void ScriptRuntime::clearNativeBackend() noexcept {
    jitModule_.reset();
    nativeProgram_.reset();
    backend_ = ScriptExecutionBackend::Interpreter;
}

} // namespace realscript::game
