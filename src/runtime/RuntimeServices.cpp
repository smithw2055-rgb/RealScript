#include "realscript/runtime/Runtime.h"

#include "ExecutionMirror.h"
#include "realscript/diagnostics/Diagnostic.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace realscript::runtime {

namespace {

using TypeIndex = std::unordered_map<
    semantic::SymbolId, const semantic::TypeSymbol*>;
using FunctionIndex = std::unordered_map<
    semantic::SymbolId, const bytecode::Function*>;

bool linkFailure(RuntimeError& error, std::string message) {
    error.code = ErrorCode::InvalidProgram;
    error.message = std::move(message);
    return false;
}

semantic::SymbolId exactTypeId(
    semantic::PrimitiveType type,
    const std::string& typeName) noexcept {
    return (semantic::isExactType(type) ||
            type == semantic::PrimitiveType::Handle) &&
            !typeName.empty()
        ? semantic::stableTypeId(typeName)
        : 0;
}

bool sameVariableContract(
    const semantic::VariableSymbol& left,
    const semantic::VariableSymbol& right) noexcept {
    return left.type == right.type &&
        left.typeName == right.typeName &&
        left.storageType == right.storageType &&
        left.storageTypeName == right.storageTypeName &&
        left.modifier == right.modifier;
}

bool sameMethodContract(
    const semantic::FunctionSymbol& left,
    const semantic::FunctionSymbol& right) noexcept {
    if (left.id != right.id || left.name != right.name ||
        left.returnType != right.returnType ||
        left.returnTypeName != right.returnTypeName ||
        left.returnModifier != right.returnModifier ||
        left.storageReturnType != right.storageReturnType ||
        left.storageReturnTypeName != right.storageReturnTypeName ||
        left.staticMethod != right.staticMethod ||
        left.virtualMethod != right.virtualMethod ||
        left.overrideMethod != right.overrideMethod ||
        left.abstractMethod != right.abstractMethod ||
        left.sealedMethod != right.sealedMethod ||
        left.virtualSlot != right.virtualSlot ||
        left.interfaceMethod != right.interfaceMethod ||
        left.interfaceSlot != right.interfaceSlot ||
        left.parameters.size() != right.parameters.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.parameters.size(); ++index) {
        if (!sameVariableContract(
                left.parameters[index], right.parameters[index])) {
            return false;
        }
    }
    return true;
}

bool sameTypeContract(
    const semantic::TypeSymbol& left,
    const semantic::TypeSymbol& right) noexcept {
    if (left.id != right.id || left.kind != right.kind ||
        left.accessibility != right.accessibility ||
        left.delegateType != right.delegateType ||
        left.interfaceType != right.interfaceType ||
        left.abstractType != right.abstractType ||
        left.sealedType != right.sealedType ||
        left.baseTypeId != right.baseTypeId ||
        left.baseTypeName != right.baseTypeName ||
        left.moduleName != right.moduleName || left.name != right.name ||
        left.virtualDispatchTable != right.virtualDispatchTable ||
        left.interfaceDispatchMaps != right.interfaceDispatchMaps ||
        left.fields.size() != right.fields.size() ||
        left.methods.size() != right.methods.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.fields.size(); ++index) {
        const auto& first = left.fields[index];
        const auto& second = right.fields[index];
        if (first.id != second.id || first.name != second.name ||
            first.accessibility != second.accessibility ||
            first.declaringTypeId != second.declaringTypeId ||
            first.type != second.type ||
            first.typeName != second.typeName ||
            first.index != second.index) {
            return false;
        }
    }
    for (std::size_t index = 0; index < left.methods.size(); ++index) {
        if (!sameMethodContract(left.methods[index], right.methods[index])) {
            return false;
        }
    }
    return true;
}

bool bytecodeMatchesMethod(
    const bytecode::Function& function,
    const semantic::FunctionSymbol& method) noexcept {
    const auto returnType = semantic::storageReturnTypeOf(method);
    const auto returnTypeName = semantic::storageReturnTypeNameOf(method);
    if (function.symbolId != method.id ||
        function.returnType != returnType ||
        function.returnTypeId != exactTypeId(returnType, returnTypeName) ||
        function.parameterTypes.size() != method.parameters.size() ||
        function.parameterTypeIds.size() != method.parameters.size()) {
        return false;
    }
    for (std::size_t index = 0; index < method.parameters.size(); ++index) {
        const auto& parameter = method.parameters[index];
        const auto type = semantic::storageTypeOf(parameter);
        const auto& typeName = semantic::storageTypeNameOf(parameter);
        if (function.parameterTypes[index] != type ||
            function.parameterTypeIds[index] != exactTypeId(type, typeName)) {
            return false;
        }
    }
    return true;
}

std::size_t visibleParameterOffset(
    const semantic::FunctionSymbol& method) noexcept {
    return method.method && !method.staticMethod ? 1u : 0u;
}

bool implementsInterfaceMethod(
    const semantic::FunctionSymbol& implementation,
    const semantic::FunctionSymbol& contract) noexcept {
    if (implementation.staticMethod || contract.staticMethod ||
        implementation.returnType != contract.returnType ||
        implementation.returnTypeName != contract.returnTypeName ||
        implementation.returnModifier != contract.returnModifier) {
        return false;
    }
    const auto implementationOffset = visibleParameterOffset(implementation);
    const auto contractOffset = visibleParameterOffset(contract);
    if (implementation.parameters.size() - implementationOffset !=
        contract.parameters.size() - contractOffset) {
        return false;
    }
    for (std::size_t index = 0;
         index < implementation.parameters.size() - implementationOffset;
         ++index) {
        const auto& actual =
            implementation.parameters[index + implementationOffset];
        const auto& expected = contract.parameters[index + contractOffset];
        if (actual.type != expected.type ||
            actual.typeName != expected.typeName ||
            actual.modifier != expected.modifier) {
            return false;
        }
    }
    return true;
}

const semantic::FunctionSymbol* methodById(
    const semantic::TypeSymbol& type,
    semantic::SymbolId methodId) noexcept {
    const auto found = std::find_if(
        type.methods.begin(), type.methods.end(),
        [&](const auto& method) { return method.id == methodId; });
    return found == type.methods.end() ? nullptr : &*found;
}

const semantic::FunctionSymbol* virtualMethodAt(
    const semantic::TypeSymbol& type,
    std::size_t slot) noexcept {
    const auto found = std::find_if(
        type.methods.begin(), type.methods.end(),
        [&](const auto& method) {
            return method.virtualSlot == slot &&
                (method.virtualMethod || method.overrideMethod ||
                 method.abstractMethod);
        });
    return found == type.methods.end() ? nullptr : &*found;
}

const semantic::FunctionSymbol* interfaceMethodAt(
    const semantic::TypeSymbol& type,
    std::size_t slot) noexcept {
    const auto found = std::find_if(
        type.methods.begin(), type.methods.end(),
        [&](const auto& method) {
            return method.interfaceMethod && method.interfaceSlot == slot;
        });
    return found == type.methods.end() ? nullptr : &*found;
}

bool validateObjectModel(
    const std::vector<bytecode::Module>& modules,
    RuntimeError& error) {
    TypeIndex types;
    FunctionIndex functions;
    for (const auto& module : modules) {
        for (const auto& type : module.types) {
            const auto [found, inserted] = types.emplace(type.id, &type);
            if (!inserted && !sameTypeContract(*found->second, type)) {
                return linkFailure(
                    error,
                    "conflicting type descriptors share TypeId " +
                        std::to_string(type.id));
            }
        }
        for (const auto& function : module.functions) {
            functions.emplace(function.symbolId, &function);
        }
    }

    std::unordered_map<semantic::SymbolId, int> colors;
    std::function<bool(const semantic::TypeSymbol&)> visit =
        [&](const semantic::TypeSymbol& type) {
            if (colors[type.id] == 2) return true;
            if (colors[type.id] == 1) {
                return linkFailure(
                    error,
                    "class inheritance cycle contains '" +
                        semantic::canonicalTypeName(type) + "'");
            }
            colors[type.id] = 1;
            if (type.baseTypeId != 0) {
                const auto found = types.find(type.baseTypeId);
                if (found == types.end() ||
                    found->second->kind != semantic::TypeKind::Class ||
                    found->second->interfaceType ||
                    found->second->sealedType ||
                    semantic::canonicalTypeName(*found->second) !=
                        type.baseTypeName) {
                    return linkFailure(
                        error,
                        "type '" + semantic::canonicalTypeName(type) +
                            "' has an invalid base type contract");
                }
                if (!visit(*found->second)) return false;
                const auto& base = *found->second;
                if (base.fields.size() > type.fields.size()) {
                    return linkFailure(
                        error,
                        "derived type '" + semantic::canonicalTypeName(type) +
                            "' truncates its base field layout");
                }
                for (std::size_t index = 0;
                     index < base.fields.size(); ++index) {
                    const auto& first = base.fields[index];
                    const auto& second = type.fields[index];
                    if (first.id != second.id || first.name != second.name ||
                        first.type != second.type ||
                        first.typeName != second.typeName ||
                        first.index != second.index ||
                        first.declaringTypeId != second.declaringTypeId) {
                        return linkFailure(
                            error,
                            "derived type '" +
                                semantic::canonicalTypeName(type) +
                                "' has an incompatible base field prefix");
                    }
                }
            } else if (!type.baseTypeName.empty()) {
                return linkFailure(
                    error,
                    "type '" + semantic::canonicalTypeName(type) +
                        "' carries a base name without a TypeId");
            }
            colors[type.id] = 2;
            return true;
        };

    for (const auto& [typeId, type] : types) {
        (void)typeId;
        if (type->kind == semantic::TypeKind::Class &&
            !type->interfaceType && !visit(*type)) {
            return false;
        }
    }

    for (const auto& [typeId, type] : types) {
        (void)typeId;
        for (std::size_t slot = 0;
             slot < type->virtualDispatchTable.size(); ++slot) {
            const auto* method = virtualMethodAt(*type, slot);
            if (!method) {
                return linkFailure(
                    error,
                    "type '" + semantic::canonicalTypeName(*type) +
                        "' has no method contract for virtual slot " +
                        std::to_string(slot));
            }
            const auto targetId = type->virtualDispatchTable[slot];
            if (targetId == 0) {
                if (!type->abstractType || !method->abstractMethod) {
                    return linkFailure(
                        error,
                        "type '" + semantic::canonicalTypeName(*type) +
                            "' has an invalid abstract virtual slot");
                }
                continue;
            }
            if (targetId != method->id) {
                return linkFailure(
                    error,
                    "type '" + semantic::canonicalTypeName(*type) +
                        "' virtual slot target does not match its method contract");
            }
            const auto target = functions.find(targetId);
            if (target == functions.end() ||
                !bytecodeMatchesMethod(*target->second, *method)) {
                return linkFailure(
                    error,
                    "type '" + semantic::canonicalTypeName(*type) +
                        "' virtual slot references a missing or incompatible function");
            }
        }

        for (const auto& map : type->interfaceDispatchMaps) {
            const auto interfaceFound = types.find(map.interfaceTypeId);
            if (interfaceFound == types.end() ||
                !interfaceFound->second->interfaceType) {
                return linkFailure(
                    error,
                    "type '" + semantic::canonicalTypeName(*type) +
                        "' references an invalid interface descriptor");
            }
            const auto& interfaceType = *interfaceFound->second;
            std::size_t interfaceSlotCount = 0;
            for (const auto& method : interfaceType.methods) {
                if (!method.interfaceMethod ||
                    method.interfaceSlot ==
                        std::numeric_limits<std::uint32_t>::max()) {
                    continue;
                }
                interfaceSlotCount = std::max(
                    interfaceSlotCount,
                    static_cast<std::size_t>(method.interfaceSlot) + 1);
            }
            if (map.slots.size() != interfaceSlotCount) {
                return linkFailure(
                    error,
                    "type '" + semantic::canonicalTypeName(*type) +
                        "' has an interface dispatch map with the wrong size");
            }
            for (std::size_t slot = 0; slot < map.slots.size(); ++slot) {
                const auto* contract = interfaceMethodAt(interfaceType, slot);
                if (!contract) {
                    return linkFailure(
                        error,
                        "interface '" +
                            semantic::canonicalTypeName(interfaceType) +
                            "' has a missing slot contract");
                }
                const auto targetId = map.slots[slot];
                if (targetId == 0) {
                    if (!type->abstractType) {
                        return linkFailure(
                            error,
                            "concrete type '" +
                                semantic::canonicalTypeName(*type) +
                                "' has an unresolved interface slot");
                    }
                    continue;
                }
                const auto* implementation = methodById(*type, targetId);
                const auto target = functions.find(targetId);
                if (!implementation || target == functions.end() ||
                    implementation->accessibility !=
                        semantic::Accessibility::Public ||
                    !implementsInterfaceMethod(*implementation, *contract) ||
                    !bytecodeMatchesMethod(
                        *target->second, *implementation)) {
                    return linkFailure(
                        error,
                        "type '" + semantic::canonicalTypeName(*type) +
                            "' interface slot references a missing or incompatible function");
                }
            }
        }
    }
    return true;
}

} // namespace

namespace detail {

std::shared_ptr<const ExecutionMirror> buildExecutionMirror(
    const std::vector<bytecode::Module>& modules,
    const std::unordered_map<
        semantic::SymbolId, ProgramImage::FunctionLocation>& functions) {
    auto mirror = std::make_shared<ExecutionMirror>();
    std::size_t functionCount = 0;
    for (const auto& module : modules) functionCount += module.functions.size();
    mirror->functions.reserve(functionCount);

    for (const auto& module : modules) {
        for (const auto& function : module.functions) {
            const auto quickOpcode = [&](const bytecode::Instruction& instruction) {
                switch (instruction.opcode) {
                case bytecode::Opcode::LoadParameter:
                    return QuickOpcode::LoadParameter;
                case bytecode::Opcode::ConstantInt:
                    if (instruction.result < function.registerTypes.size() &&
                        function.registerTypes[instruction.result] ==
                            semantic::PrimitiveType::Int) {
                        return QuickOpcode::ConstantInt;
                    }
                    break;
                case bytecode::Opcode::LoadLocal:
                    return QuickOpcode::LoadLocal;
                case bytecode::Opcode::StoreLocal:
                    return QuickOpcode::StoreLocal;
                default:
                    break;
                }
                if (instruction.operands.size() < 2 ||
                    instruction.operands[0] >= function.registerTypes.size() ||
                    instruction.operands[1] >= function.registerTypes.size() ||
                    function.registerTypes[instruction.operands[0]] !=
                        semantic::PrimitiveType::Int ||
                    function.registerTypes[instruction.operands[1]] !=
                        semantic::PrimitiveType::Int) {
                    return QuickOpcode::Generic;
                }
                switch (instruction.opcode) {
                case bytecode::Opcode::AddInt: return QuickOpcode::AddInt;
                case bytecode::Opcode::SubtractInt: return QuickOpcode::SubtractInt;
                case bytecode::Opcode::MultiplyInt: return QuickOpcode::MultiplyInt;
                case bytecode::Opcode::DivideInt: return QuickOpcode::DivideInt;
                case bytecode::Opcode::RemainderInt: return QuickOpcode::RemainderInt;
                case bytecode::Opcode::LessInt: return QuickOpcode::LessInt;
                case bytecode::Opcode::LessOrEqualInt:
                    return QuickOpcode::LessOrEqualInt;
                case bytecode::Opcode::GreaterInt: return QuickOpcode::GreaterInt;
                case bytecode::Opcode::GreaterOrEqualInt:
                    return QuickOpcode::GreaterOrEqualInt;
                default: return QuickOpcode::Generic;
                }
            };
            ExecutionFunction decoded;
            decoded.bytecode = &function;
            decoded.blocks.resize(function.blocks.size());
            decoded.blockIndices.reserve(function.blocks.size());
            for (std::size_t index = 0; index < function.blocks.size(); ++index) {
                decoded.blocks[index].bytecode = &function.blocks[index];
                auto& instructions = decoded.blocks[index].instructions;
                instructions.reserve(function.blocks[index].instructions.size());
                for (const auto& instruction : function.blocks[index].instructions) {
                    instructions.push_back({&instruction, quickOpcode(instruction)});
                }
                const auto isBinaryInt = [](QuickOpcode opcode) {
                    return opcode >= QuickOpcode::AddInt &&
                        opcode <= QuickOpcode::GreaterOrEqualInt;
                };
                for (std::size_t instructionIndex = 0;
                     instructionIndex < instructions.size();) {
                    auto& first = instructions[instructionIndex];
                    if (instructionIndex + 1 < instructions.size() &&
                        first.opcode == QuickOpcode::LoadParameter &&
                        instructions[instructionIndex + 1].opcode ==
                            QuickOpcode::StoreLocal &&
                        instructions[instructionIndex + 1]
                                .bytecode->operands[0] ==
                            first.bytecode->result) {
                        first.superOpcode = SuperOpcode::LoadParameterStoreLocal;
                        first.span = 2;
                        instructionIndex += 2;
                        continue;
                    }
                    if (instructionIndex + 2 < instructions.size() &&
                        first.opcode == QuickOpcode::LoadLocal &&
                        (instructions[instructionIndex + 1].opcode ==
                                QuickOpcode::ConstantInt ||
                            instructions[instructionIndex + 1].opcode ==
                                QuickOpcode::LoadLocal) &&
                        isBinaryInt(instructions[instructionIndex + 2].opcode)) {
                        const auto& second = instructions[instructionIndex + 1];
                        const auto& binary = instructions[instructionIndex + 2];
                        if (binary.bytecode->operands[0] ==
                                first.bytecode->result &&
                            binary.bytecode->operands[1] ==
                                second.bytecode->result) {
                            const auto constant =
                                second.opcode == QuickOpcode::ConstantInt;
                            const auto storesResult =
                                instructionIndex + 3 < instructions.size() &&
                                instructions[instructionIndex + 3].opcode ==
                                    QuickOpcode::StoreLocal &&
                                instructions[instructionIndex + 3]
                                        .bytecode->operands[0] ==
                                    binary.bytecode->result;
                            first.superOpcode = storesResult
                                ? (constant
                                    ? SuperOpcode::BinaryLocalIntConstantStoreLocal
                                    : SuperOpcode::BinaryLocalIntLocalStoreLocal)
                                : (constant
                                    ? SuperOpcode::BinaryLocalIntConstant
                                    : SuperOpcode::BinaryLocalIntLocal);
                            first.span = storesResult ? 4 : 3;
                            instructionIndex += first.span;
                            continue;
                        }
                    }
                    ++instructionIndex;
                }
                decoded.blockIndices.emplace(function.blocks[index].id, index);
            }

            const auto entry = decoded.blockIndices.find(0);
            if (entry != decoded.blockIndices.end()) decoded.entry = entry->second;
            const auto bindEdge = [&](ExecutionEdge& edge,
                                      bytecode::BlockId target,
                                      const std::vector<bytecode::Register>& sources) {
                const auto found = decoded.blockIndices.find(target);
                if (found == decoded.blockIndices.end()) return;
                edge.target = found->second;
                edge.sources = &sources;
                edge.destinations = &function.blocks[found->second].parameters;
                decoded.edgeScratchCount =
                    std::max(decoded.edgeScratchCount, sources.size());
            };
            for (std::size_t index = 0; index < function.blocks.size(); ++index) {
                const auto& block = function.blocks[index];
                auto& blockMirror = decoded.blocks[index];
                if (block.terminator.kind == bytecode::TerminatorKind::Jump ||
                    block.terminator.kind == bytecode::TerminatorKind::Branch) {
                    bindEdge(
                        blockMirror.branch,
                        block.terminator.target,
                        block.terminator.arguments);
                }
                if (block.terminator.kind == bytecode::TerminatorKind::Branch) {
                    bindEdge(
                        blockMirror.falseBranch,
                        block.terminator.falseTarget,
                        block.terminator.falseArguments);
                }
            }
            mirror->functions.emplace(&function, std::move(decoded));
        }
    }

    for (const auto& module : modules) {
        for (const auto& function : module.functions) {
            const auto decoded = mirror->functions.find(&function);
            if (decoded == mirror->functions.end()) continue;
            for (auto& block : decoded->second.blocks) {
                for (auto& instruction : block.instructions) {
                    if (instruction.bytecode->opcode !=
                            bytecode::Opcode::Call ||
                        instruction.bytecode->index >=
                            module.functionReferences.size()) {
                        continue;
                    }
                    const auto& reference =
                        module.functionReferences[instruction.bytecode->index];
                    if (reference.virtualDispatch || reference.interfaceDispatch) {
                        continue;
                    }
                    const auto target = functions.find(reference.symbolId);
                    if (target == functions.end() || !target->second.function) {
                        continue;
                    }
                    const auto& targetFunction = *target->second.function;
                    if (reference.returnType != targetFunction.returnType ||
                        reference.returnTypeId != targetFunction.returnTypeId ||
                        reference.parameterTypes !=
                            targetFunction.parameterTypes ||
                        reference.parameterTypeIds !=
                            targetFunction.parameterTypeIds ||
                        instruction.bytecode->operands.size() !=
                            targetFunction.parameterTypes.size()) {
                        continue;
                    }
                    instruction.opcode = QuickOpcode::CallDirect;
                    instruction.directTarget = &target->second;
                    instruction.directExecution = findExecutionFunction(
                        *mirror, targetFunction);
                }
            }
        }
    }
    return mirror;
}

const ExecutionMirror& executionMirror(const ProgramImage& program) noexcept {
    return *program.executionMirror_;
}

} // namespace detail

bool BindingRegistry::bind(
    semantic::SymbolId symbolId,
    ExternalFunction function,
    BindingDeterminism determinism) {
    if (symbolId == 0 || !function) return false;
    return bySymbol_.emplace(
        symbolId,
        Entry{std::move(function), determinism}).second;
}

bool BindingRegistry::bind(
    const std::string& canonicalName,
    ExternalFunction function,
    BindingDeterminism determinism) {
    if (canonicalName.empty() || !function) return false;
    return byName_.emplace(
        canonicalName,
        Entry{std::move(function), determinism}).second;
}

std::optional<ResolvedBinding> BindingRegistry::resolve(
    const bytecode::FunctionReference& reference) const {
    const auto bySymbol = bySymbol_.find(reference.symbolId);
    if (bySymbol != bySymbol_.end()) {
        return ResolvedBinding{&bySymbol->second.function, bySymbol->second.determinism};
    }
    const auto byName = byName_.find(reference.name);
    if (byName != byName_.end()) {
        return ResolvedBinding{&byName->second.function, byName->second.determinism};
    }
    return std::nullopt;
}

std::optional<Value> BindingRegistry::invoke(
    const bytecode::FunctionReference& reference,
    const std::vector<Value>& arguments,
    RuntimeError& error) const {
    const auto resolved = resolve(reference);
    if (resolved && resolved->function) {
        return (*resolved->function)(reference, arguments, error);
    }
    error.code = ErrorCode::ExternalFunctionUnresolved;
    error.message = "external function '" + reference.name + "' is not registered";
    return std::nullopt;
}

std::size_t BindingRegistry::size() const noexcept { return bySymbol_.size() + byName_.size(); }

ProgramImage::ProgramImage(std::vector<bytecode::Module> modules)
    : modules_(std::move(modules)) {
    for (const auto& module : modules_) {
        for (const auto& type : module.types) {
            types_.emplace(type.id, &type);
        }
        for (const auto& function : module.functions) {
            const auto qualified = module.name + "::" + function.name;
            functions_.emplace(
                function.symbolId,
                FunctionLocation{&module, &function, qualified});
            names_.emplace(qualified, function.symbolId);
        }
    }
    executionMirror_ = detail::buildExecutionMirror(modules_, functions_);
}

ProgramImage::ProgramImage(const ProgramImage& other)
    : ProgramImage(other.modules_) {}

ProgramImage& ProgramImage::operator=(const ProgramImage& other) {
    if (this == &other) return *this;
    ProgramImage replacement(other);
    *this = std::move(replacement);
    return *this;
}

std::optional<ProgramImage> ProgramImage::link(
    std::vector<bytecode::Module> modules,
    RuntimeError& error) {
    std::unordered_map<semantic::SymbolId, std::string> symbols;
    std::unordered_map<std::string, semantic::SymbolId> names;
    for (const auto& module : modules) {
        diagnostics::DiagnosticBag diagnostics;
        if (!bytecode::verifyModule(module, diagnostics)) {
            error.code = ErrorCode::InvalidProgram;
            error.message =
                "bytecode verification failed while linking module '" +
                module.name + "'";
            for (const auto& diagnostic : diagnostics.items()) {
                error.message += "; " + diagnostic.code + ": " +
                    diagnostic.message;
            }
            return std::nullopt;
        }
        for (const auto& function : module.functions) {
            const auto qualified = module.name + "::" + function.name;
            if (!symbols.emplace(function.symbolId, qualified).second) {
                error.code = ErrorCode::DuplicateSymbol;
                error.message = "duplicate function SymbolId while linking '" + qualified + "'";
                return std::nullopt;
            }
            // Source-level overloads intentionally share a qualified display
            // name. Calls and constructor invocations carry the stable SymbolId;
            // the name index retains the first declaration for legacy lookup.
            names.emplace(qualified, function.symbolId);
        }
    }
    if (!validateObjectModel(modules, error)) {
        return std::nullopt;
    }
    return ProgramImage(std::move(modules));
}

const std::vector<bytecode::Module>& ProgramImage::modules() const noexcept { return modules_; }
std::size_t ProgramImage::functionCount() const noexcept { return functions_.size(); }
std::size_t ProgramImage::moduleCount() const noexcept { return modules_.size(); }
bool ProgramImage::contains(semantic::SymbolId symbolId) const noexcept {
    return functions_.find(symbolId) != functions_.end();
}
std::optional<semantic::SymbolId> ProgramImage::findFunction(const std::string& qualifiedName) const {
    const auto found = names_.find(qualifiedName);
    if (found == names_.end()) return std::nullopt;
    return found->second;
}

const ProgramImage::FunctionLocation* ProgramImage::resolveFunction(
    semantic::SymbolId symbolId) const noexcept {
    const auto found = functions_.find(symbolId);
    return found == functions_.end() ? nullptr : &found->second;
}

const semantic::TypeSymbol* ProgramImage::resolveType(
    semantic::SymbolId typeId) const noexcept {
    const auto found = types_.find(typeId);
    return found == types_.end() ? nullptr : found->second;
}

EngineRuntime::EngineRuntime(std::shared_ptr<const ProgramImage> program)
    : program_(std::move(program)),
      heap_(std::make_shared<ManagedHeap>()),
      nativeHandles_(std::make_shared<NativeHandleRegistry>()) {}

void EngineRuntime::setBindings(std::shared_ptr<const BindingRegistry> bindings) {
    bindings_ = std::move(bindings);
}

void EngineRuntime::setHeap(std::shared_ptr<ManagedHeap> heap) {
    heap_ = heap ? std::move(heap) : std::make_shared<ManagedHeap>();
}

std::shared_ptr<ManagedHeap> EngineRuntime::heap() const noexcept { return heap_; }

void EngineRuntime::setNativeHandles(
    std::shared_ptr<NativeHandleRegistry> handles) {
    nativeHandles_ = handles
        ? std::move(handles)
        : std::make_shared<NativeHandleRegistry>();
}

std::shared_ptr<NativeHandleRegistry> EngineRuntime::nativeHandles() const noexcept {
    return nativeHandles_;
}

ExecutionResult EngineRuntime::invoke(
    const std::string& qualifiedName,
    const std::vector<Value>& arguments,
    ExecutionOptions options) const {
    auto program = programSnapshot();
    if (!program) {
        ExecutionResult result;
        result.error.code = ErrorCode::InvalidProgram;
        result.error.message = "runtime has no linked program image";
        return result;
    }
    const auto entry = program->findFunction(qualifiedName);
    if (!entry) {
        ExecutionResult result;
        result.error.code = ErrorCode::FunctionNotFound;
        result.error.message =
            "entry function '" + qualifiedName + "' was not found";
        return result;
    }
    Interpreter interpreter(std::move(program), heap_);
    interpreter.setBindingRegistry(bindings_);
    return interpreter.invoke(*entry, arguments, std::move(options));
}

ExecutionResult EngineRuntime::invoke(
    semantic::SymbolId symbolId,
    const std::vector<Value>& arguments,
    ExecutionOptions options) const {
    auto program = programSnapshot();
    if (!program) {
        ExecutionResult result;
        result.error.code = ErrorCode::InvalidProgram;
        result.error.message = "runtime has no linked program image";
        return result;
    }
    Interpreter interpreter(std::move(program), heap_);
    interpreter.setBindingRegistry(bindings_);
    return interpreter.invoke(symbolId, arguments, std::move(options));
}

const ProgramImage& EngineRuntime::program() const noexcept {
    std::lock_guard<std::mutex> lock(programMutex_);
    return *program_;
}

std::shared_ptr<const ProgramImage> EngineRuntime::programSnapshot() const noexcept {
    std::lock_guard<std::mutex> lock(programMutex_);
    return program_;
}

void EngineRuntime::replaceProgram(std::shared_ptr<const ProgramImage> program) {
    if (!program) return;
    std::lock_guard<std::mutex> lock(programMutex_);
    if (program_) retiredPrograms_.push_back(program_);
    program_ = std::move(program);
}

const char* traceEventKindName(TraceEventKind kind) noexcept {
    switch (kind) {
    case TraceEventKind::FunctionEnter: return "function-enter";
    case TraceEventKind::FunctionExit: return "function-exit";
    case TraceEventKind::Instruction: return "instruction";
    case TraceEventKind::Branch: return "branch";
    case TraceEventKind::ExternalCall: return "external-call";
    case TraceEventKind::GcStep: return "gc-step";
    case TraceEventKind::RuntimeError: return "runtime-error";
    }
    return "unknown";
}

} // namespace realscript::runtime
