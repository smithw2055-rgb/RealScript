#include <cstring>
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
    BindingDeterminism determinism,
    std::string requiredCapability) {
    if (symbolId == 0 || !function) return false;
    const bool inserted = bySymbol_.emplace(
        symbolId,
        Entry{std::move(function), determinism, std::move(requiredCapability)}).second;
    if (inserted) ++entryCount_;
    return inserted;
}

bool BindingRegistry::bind(
    const std::string& canonicalName,
    ExternalFunction function,
    BindingDeterminism determinism,
    std::string requiredCapability) {
    if (canonicalName.empty() || !function) return false;
    if (byName_.find(canonicalName) != byName_.end()) return false;
    Entry entry{std::move(function), determinism, std::move(requiredCapability)};
    const auto symId = semantic::stableTypeId(canonicalName);
    bySymbol_.try_emplace(symId, entry);
    const bool inserted = byName_.emplace(canonicalName, std::move(entry)).second;
    if (inserted) ++entryCount_;
    return inserted;
}

std::optional<ResolvedBinding> BindingRegistry::resolve(
    const bytecode::FunctionReference& reference) const {
    if (reference.symbolId != 0) {
        const auto bySymbol = bySymbol_.find(reference.symbolId);
        if (bySymbol != bySymbol_.end()) {
            return ResolvedBinding{
                &bySymbol->second.function,
                bySymbol->second.determinism,
                bySymbol->second.requiredCapability};
        }
        const auto cached = resolvedBySymbolCache_.find(reference.symbolId);
        if (cached != resolvedBySymbolCache_.end() && cached->second) {
            return ResolvedBinding{
                &cached->second->function,
                cached->second->determinism,
                cached->second->requiredCapability};
        }
    }
    const auto byName = byName_.find(reference.name);
    if (byName != byName_.end()) {
        if (reference.symbolId != 0) {
            resolvedBySymbolCache_.emplace(reference.symbolId, &byName->second);
        }
        return ResolvedBinding{
            &byName->second.function,
            byName->second.determinism,
            byName->second.requiredCapability};
    }
    return std::nullopt;
}

std::optional<Value> BindingRegistry::invoke(
    const bytecode::FunctionReference& reference,
    ValueSpan arguments,
    RuntimeError& error) const {
    const auto resolved = resolve(reference);
    if (resolved && resolved->function) {
        return (*resolved->function)(reference, arguments, error);
    }
    error.code = ErrorCode::ExternalFunctionUnresolved;
    error.message = "external function '" + reference.name + "' is not registered";
    return std::nullopt;
}

std::size_t BindingRegistry::size() const noexcept { return entryCount_; }
std::size_t BindingRegistry::cachedSymbolCount() const noexcept {
    return bySymbol_.size() + resolvedBySymbolCache_.size();
}

semantic::SymbolId fixedTypeId() noexcept {
    static const semantic::SymbolId id =
        semantic::stableTypeId("RealScript.Math::Fixed");
    return id;
}

semantic::SymbolId fixed16TypeId() noexcept {
    static const semantic::SymbolId id =
        semantic::stableTypeId("RealScript.Math::Fixed16");
    return id;
}

semantic::SymbolId vec2TypeId() noexcept {
    static const semantic::SymbolId id =
        semantic::stableTypeId("RealScript.Math::Vec2");
    return id;
}

semantic::SymbolId vec3TypeId() noexcept {
    static const semantic::SymbolId id =
        semantic::stableTypeId("RealScript.Math::Vec3");
    return id;
}

semantic::SymbolId quatTypeId() noexcept {
    static const semantic::SymbolId id =
        semantic::stableTypeId("RealScript.Math::Quat");
    return id;
}

std::optional<semantic::SymbolId> structValueTypeId(const Value& value) noexcept {
    if (std::holds_alternative<FixedValue>(value)) return fixedTypeId();
    if (std::holds_alternative<Fixed16Value>(value)) return fixed16TypeId();
    if (std::holds_alternative<Vec2Value>(value)) return vec2TypeId();
    if (std::holds_alternative<Vec3Value>(value)) return vec3TypeId();
    if (std::holds_alternative<QuatValue>(value)) return quatTypeId();
    if (const auto* s = std::get_if<StructValue>(&value)) return s->typeId;
    return std::nullopt;
}

std::optional<Value> makeDefaultInlineStructValue(semantic::SymbolId typeId) noexcept {
    if (typeId == fixedTypeId()) return Value{FixedValue{}};
    if (typeId == fixed16TypeId()) return Value{Fixed16Value{}};
    if (typeId == vec2TypeId()) return Value{Vec2Value{}};
    if (typeId == vec3TypeId()) return Value{Vec3Value{}};
    if (typeId == quatTypeId()) return Value{QuatValue{}};
    return std::nullopt;
}

std::optional<InlineStructSlot> encodeInlineStructSlot(
    const Value& value) noexcept {
    InlineStructSlot slot{};
    if (const auto* b = std::get_if<bool>(&value)) {
        slot.kind = InlineStructSlot::Kind::Bool;
        slot.setBits(*b ? 1u : 0u);
        return slot;
    }
    if (const auto* v = std::get_if<ByteValue>(&value)) {
        slot.kind = InlineStructSlot::Kind::Byte;
        slot.setBits(v->value);
        return slot;
    }
    if (const auto* v = std::get_if<SByteValue>(&value)) {
        slot.kind = InlineStructSlot::Kind::SByte;
        slot.setBits(static_cast<std::uint8_t>(v->value));
        return slot;
    }
    if (const auto* v = std::get_if<ShortValue>(&value)) {
        slot.kind = InlineStructSlot::Kind::Short;
        slot.setBits(static_cast<std::uint16_t>(v->value));
        return slot;
    }
    if (const auto* v = std::get_if<UShortValue>(&value)) {
        slot.kind = InlineStructSlot::Kind::UShort;
        slot.setBits(v->value);
        return slot;
    }
    if (const auto* v = std::get_if<std::int64_t>(&value)) {
        slot.kind = InlineStructSlot::Kind::Int;
        slot.setBits(static_cast<std::uint64_t>(*v));
        return slot;
    }
    if (const auto* v = std::get_if<UIntValue>(&value)) {
        slot.kind = InlineStructSlot::Kind::UInt;
        slot.setBits(v->value);
        return slot;
    }
    if (const auto* v = std::get_if<LongValue>(&value)) {
        slot.kind = InlineStructSlot::Kind::Long;
        slot.setBits(static_cast<std::uint64_t>(v->value));
        return slot;
    }
    if (const auto* v = std::get_if<ULongValue>(&value)) {
        slot.kind = InlineStructSlot::Kind::ULong;
        slot.setBits(v->value);
        return slot;
    }
    if (const auto* v = std::get_if<FloatValue>(&value)) {
        slot.kind = InlineStructSlot::Kind::Float;
        std::uint32_t fbits = 0;
        std::memcpy(&fbits, &v->value, sizeof(fbits));
        slot.setBits(fbits);
        return slot;
    }
    if (const auto* v = std::get_if<double>(&value)) {
        slot.kind = InlineStructSlot::Kind::Double;
        std::uint64_t dbits = 0;
        std::memcpy(&dbits, v, sizeof(dbits));
        slot.setBits(dbits);
        return slot;
    }
    if (const auto* v = std::get_if<CharValue>(&value)) {
        slot.kind = InlineStructSlot::Kind::Char;
        slot.setBits(v->value);
        return slot;
    }
    if (const auto* v = std::get_if<FixedValue>(&value)) {
        slot.kind = InlineStructSlot::Kind::Fixed;
        slot.setBits(static_cast<std::uint32_t>(v->raw));
        return slot;
    }
    if (const auto* v = std::get_if<Fixed16Value>(&value)) {
        slot.kind = InlineStructSlot::Kind::Fixed16;
        slot.setBits(static_cast<std::uint64_t>(v->raw));
        return slot;
    }
    if (const auto* v = std::get_if<EnumValue>(&value)) {
        if (v->value >= -8388608 && v->value <= 8388607) {
            slot.kind = InlineStructSlot::Kind::Enum;
            slot.setBits(v->typeId);
            slot.setAuxInt24(static_cast<std::int32_t>(v->value));
            return slot;
        }
        return std::nullopt;
    }
    if (const auto* v = std::get_if<Vec2Value>(&value)) {
        slot.kind = InlineStructSlot::Kind::Vec2;
        slot.lo = static_cast<std::uint32_t>(v->x);
        slot.hi = static_cast<std::uint32_t>(v->y);
        return slot;
    }
    if (const auto* v = std::get_if<Vec3Value>(&value)) {
        if (v->z >= -8388608 && v->z <= 8388607) {
            slot.kind = InlineStructSlot::Kind::Vec3;
            slot.lo = static_cast<std::uint32_t>(v->x);
            slot.hi = static_cast<std::uint32_t>(v->y);
            slot.setAuxInt24(v->z);
            return slot;
        }
        return std::nullopt;
    }
    if (const auto* v = std::get_if<QuatValue>(&value)) {
        const auto inInt16 = [](std::int32_t c) noexcept {
            return c >= std::numeric_limits<std::int16_t>::min() &&
                   c <= std::numeric_limits<std::int16_t>::max();
        };
        if (inInt16(v->x) && inInt16(v->y) && inInt16(v->z) && inInt16(v->w)) {
            slot.kind = InlineStructSlot::Kind::Quat;
            slot.lo = static_cast<std::uint32_t>(static_cast<std::uint16_t>(static_cast<std::int16_t>(v->x))) |
                (static_cast<std::uint32_t>(static_cast<std::uint16_t>(static_cast<std::int16_t>(v->y))) << 16u);
            slot.hi = static_cast<std::uint32_t>(static_cast<std::uint16_t>(static_cast<std::int16_t>(v->z))) |
                (static_cast<std::uint32_t>(static_cast<std::uint16_t>(static_cast<std::int16_t>(v->w))) << 16u);
            return slot;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

Value decodeInlineStructSlot(const InlineStructSlot& slot) noexcept {
    const auto bits = slot.bits();
    switch (slot.kind) {
    case InlineStructSlot::Kind::Bool:
        return bits != 0;
    case InlineStructSlot::Kind::Byte:
        return ByteValue{static_cast<std::uint8_t>(bits)};
    case InlineStructSlot::Kind::SByte:
        return SByteValue{static_cast<std::int8_t>(static_cast<std::uint8_t>(bits))};
    case InlineStructSlot::Kind::Short:
        return ShortValue{static_cast<std::int16_t>(static_cast<std::uint16_t>(bits))};
    case InlineStructSlot::Kind::UShort:
        return UShortValue{static_cast<std::uint16_t>(bits)};
    case InlineStructSlot::Kind::Int:
        return static_cast<std::int64_t>(bits);
    case InlineStructSlot::Kind::UInt:
        return UIntValue{static_cast<std::uint32_t>(bits)};
    case InlineStructSlot::Kind::Long:
        return LongValue{static_cast<std::int64_t>(bits)};
    case InlineStructSlot::Kind::ULong:
        return ULongValue{bits};
    case InlineStructSlot::Kind::Float: {
        std::uint32_t fbits = slot.lo;
        float val = 0.0f;
        std::memcpy(&val, &fbits, sizeof(val));
        return FloatValue{val};
    }
    case InlineStructSlot::Kind::Double: {
        double val = 0.0;
        std::memcpy(&val, &bits, sizeof(val));
        return val;
    }
    case InlineStructSlot::Kind::Char:
        return CharValue{static_cast<char16_t>(bits)};
    case InlineStructSlot::Kind::Fixed:
        return FixedValue{static_cast<std::int32_t>(slot.lo)};
    case InlineStructSlot::Kind::Fixed16:
        return Fixed16Value{static_cast<std::int64_t>(bits)};
    case InlineStructSlot::Kind::Enum:
        return EnumValue{
            bits,
            static_cast<std::int64_t>(slot.auxInt24()),
        };
    case InlineStructSlot::Kind::Vec2:
        return Vec2Value{
            static_cast<std::int32_t>(slot.lo),
            static_cast<std::int32_t>(slot.hi),
        };
    case InlineStructSlot::Kind::Vec3:
        return Vec3Value{
            static_cast<std::int32_t>(slot.lo),
            static_cast<std::int32_t>(slot.hi),
            slot.auxInt24(),
        };
    case InlineStructSlot::Kind::Quat:
        return QuatValue{
            static_cast<std::int32_t>(static_cast<std::int16_t>(slot.lo & 0xFFFFu)),
            static_cast<std::int32_t>(static_cast<std::int16_t>(slot.lo >> 16u)),
            static_cast<std::int32_t>(static_cast<std::int16_t>(slot.hi & 0xFFFFu)),
            static_cast<std::int32_t>(static_cast<std::int16_t>(slot.hi >> 16u)),
        };
    case InlineStructSlot::Kind::None:
        break;
    }
    return std::monostate{};
}

StructValue makeStructValueFromFields(
    semantic::SymbolId typeId,
    ValueSpan fields) {
    if (fields.size() <= StructValue::MaxInlineFields) {
        StructValue inlineVal;
        inlineVal.typeId = typeId;
        inlineVal.inlineCount = static_cast<std::uint8_t>(fields.size());
        inlineVal.isInline = true;
        bool allInlined = true;
        for (std::size_t i = 0; i < fields.size(); ++i) {
            if (auto slot = encodeInlineStructSlot(fields[i])) {
                inlineVal.inlineFields[i] = *slot;
            } else {
                allInlined = false;
                break;
            }
        }
        if (allInlined) {
            return inlineVal;
        }
    }
    auto storage = std::make_shared<StructStorage>();
    storage->fields.assign(fields.begin(), fields.end());
    return StructValue{typeId, std::move(storage)};
}

StructValue makeStructValueFromFields(
    semantic::SymbolId typeId,
    std::vector<Value> fields) {
    if (fields.size() <= StructValue::MaxInlineFields) {
        StructValue inlineVal;
        inlineVal.typeId = typeId;
        inlineVal.inlineCount = static_cast<std::uint8_t>(fields.size());
        inlineVal.isInline = true;
        bool allInlined = true;
        for (std::size_t i = 0; i < fields.size(); ++i) {
            if (auto slot = encodeInlineStructSlot(fields[i])) {
                inlineVal.inlineFields[i] = *slot;
            } else {
                allInlined = false;
                break;
            }
        }
        if (allInlined) {
            return inlineVal;
        }
    }
    auto storage = std::make_shared<StructStorage>();
    storage->fields = std::move(fields);
    return StructValue{typeId, std::move(storage)};
}

std::optional<Value> structValueFieldAt(
    const StructValue& structure,
    std::size_t index) noexcept {
    if (structure.isInline) {
        if (index >= structure.inlineCount) return std::nullopt;
        return decodeInlineStructSlot(structure.inlineFields[index]);
    }
    if (!structure.storage || index >= structure.storage->fields.size()) {
        return std::nullopt;
    }
    return structure.storage->fields[index];
}

std::optional<Value> loadInlineStructField(
    const Value& value,
    std::size_t fieldIndex) noexcept {
    if (const auto* f = std::get_if<FixedValue>(&value)) {
        if (fieldIndex == 0) return Value{static_cast<std::int64_t>(f->raw)};
        return std::nullopt;
    }
    if (const auto* f16 = std::get_if<Fixed16Value>(&value)) {
        if (fieldIndex == 0) return Value{static_cast<std::int64_t>(f16->raw)};
        return std::nullopt;
    }
    if (const auto* v2 = std::get_if<Vec2Value>(&value)) {
        if (fieldIndex == 0) return Value{FixedValue{v2->x}};
        if (fieldIndex == 1) return Value{FixedValue{v2->y}};
        return std::nullopt;
    }
    if (const auto* v3 = std::get_if<Vec3Value>(&value)) {
        if (fieldIndex == 0) return Value{FixedValue{v3->x}};
        if (fieldIndex == 1) return Value{FixedValue{v3->y}};
        if (fieldIndex == 2) return Value{FixedValue{v3->z}};
        return std::nullopt;
    }
    if (const auto* q = std::get_if<QuatValue>(&value)) {
        if (fieldIndex == 0) return Value{FixedValue{q->x}};
        if (fieldIndex == 1) return Value{FixedValue{q->y}};
        if (fieldIndex == 2) return Value{FixedValue{q->z}};
        if (fieldIndex == 3) return Value{FixedValue{q->w}};
        return std::nullopt;
    }
    if (const auto* s = std::get_if<StructValue>(&value); s && s->isInline) {
        if (fieldIndex < s->inlineCount) {
            return decodeInlineStructSlot(s->inlineFields[fieldIndex]);
        }
        return std::nullopt;
    }
    return std::nullopt;
}

namespace {
std::optional<std::int32_t> extractRawFixedField(const Value& fieldValue) noexcept {
    if (const auto* f = std::get_if<FixedValue>(&fieldValue)) {
        return f->raw;
    }
    if (const auto* i = std::get_if<std::int64_t>(&fieldValue)) {
        return static_cast<std::int32_t>(*i);
    }
    if (const auto* l = std::get_if<LongValue>(&fieldValue)) {
        return static_cast<std::int32_t>(l->value);
    }
    if (const auto* s = std::get_if<StructValue>(&fieldValue)) {
        if (s->typeId == fixedTypeId() && s->storage && s->storage->fields.size() == 1) {
            if (const auto* rawInt = std::get_if<std::int64_t>(&s->storage->fields[0])) {
                return static_cast<std::int32_t>(*rawInt);
            }
            if (const auto* rawLong = std::get_if<LongValue>(&s->storage->fields[0])) {
                return static_cast<std::int32_t>(rawLong->value);
            }
        }
    }
    return std::nullopt;
}
} // namespace

std::optional<Value> storeInlineStructField(
    const Value& structure,
    std::size_t fieldIndex,
    const Value& fieldValue) noexcept {
    if (std::holds_alternative<FixedValue>(structure)) {
        if (fieldIndex != 0) return std::nullopt;
        if (const auto* i = std::get_if<std::int64_t>(&fieldValue)) {
            return Value{FixedValue{static_cast<std::int32_t>(*i)}};
        }
        if (const auto* l = std::get_if<LongValue>(&fieldValue)) {
            return Value{FixedValue{static_cast<std::int32_t>(l->value)}};
        }
        return std::nullopt;
    }
    if (std::holds_alternative<Fixed16Value>(structure)) {
        if (fieldIndex != 0) return std::nullopt;
        if (const auto* l = std::get_if<LongValue>(&fieldValue)) {
            return Value{Fixed16Value{l->value}};
        }
        if (const auto* i = std::get_if<std::int64_t>(&fieldValue)) {
            return Value{Fixed16Value{*i}};
        }
        return std::nullopt;
    }
    if (const auto* v2 = std::get_if<Vec2Value>(&structure)) {
        const auto raw = extractRawFixedField(fieldValue);
        if (!raw) return std::nullopt;
        Vec2Value out = *v2;
        if (fieldIndex == 0) {
            out.x = *raw;
            return Value{out};
        }
        if (fieldIndex == 1) {
            out.y = *raw;
            return Value{out};
        }
        return std::nullopt;
    }
    if (const auto* v3 = std::get_if<Vec3Value>(&structure)) {
        const auto raw = extractRawFixedField(fieldValue);
        if (!raw) return std::nullopt;
        Vec3Value out = *v3;
        if (fieldIndex == 0) {
            out.x = *raw;
            return Value{out};
        }
        if (fieldIndex == 1) {
            out.y = *raw;
            return Value{out};
        }
        if (fieldIndex == 2) {
            out.z = *raw;
            return Value{out};
        }
        return std::nullopt;
    }
    if (const auto* q = std::get_if<QuatValue>(&structure)) {
        const auto raw = extractRawFixedField(fieldValue);
        if (!raw) return std::nullopt;
        QuatValue out = *q;
        if (fieldIndex == 0) {
            out.x = *raw;
            return Value{out};
        }
        if (fieldIndex == 1) {
            out.y = *raw;
            return Value{out};
        }
        if (fieldIndex == 2) {
            out.z = *raw;
            return Value{out};
        }
        if (fieldIndex == 3) {
            out.w = *raw;
            return Value{out};
        }
        return std::nullopt;
    }
    if (const auto* s = std::get_if<StructValue>(&structure); s && s->isInline) {
        if (fieldIndex < s->inlineCount) {
            if (auto slot = encodeInlineStructSlot(fieldValue)) {
                StructValue updated = *s;
                updated.inlineFields[fieldIndex] = *slot;
                return Value{updated};
            }
        }
        return std::nullopt;
    }
    return std::nullopt;
}

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

namespace {

std::string extractIntrinsicString(const Value& val, const ManagedHeap* heap) {
    if (std::holds_alternative<NullString>(val) ||
        std::holds_alternative<std::monostate>(val)) {
        return {};
    }
    if (const auto* s = std::get_if<std::string>(&val)) return *s;
    if (const auto* ref = std::get_if<ObjectRef>(&val)) {
        if (heap) {
            if (const auto sv = heap->stringView(*ref)) {
                return std::string(*sv);
            }
        }
    }
    return valueToString(val, heap);
}

void splitStringBy(std::string_view input, char delim, std::vector<std::string>& out) {
    out.clear();
    if (input.empty()) return;
    std::size_t start = 0;
    while (start <= input.size()) {
        const auto pos = input.find(delim, start);
        const auto piece = pos == std::string_view::npos
            ? input.substr(start)
            : input.substr(start, pos - start);
        if (!piece.empty()) {
            out.emplace_back(piece);
        }
        if (pos == std::string_view::npos) break;
        start = pos + 1;
    }
}

void parseEcsQuerySpec(
    std::string_view spec,
    std::vector<std::string>& cols,
    std::vector<std::string>& withs,
    std::vector<std::string>& withouts) {
    cols.clear();
    withs.clear();
    withouts.clear();
    const auto p1 = spec.find(';');
    if (p1 == std::string_view::npos) {
        splitStringBy(spec, ',', cols);
        return;
    }
    splitStringBy(spec.substr(0, p1), ',', cols);
    const auto rest = spec.substr(p1 + 1);
    const auto p2 = rest.find(';');
    if (p2 == std::string_view::npos) {
        splitStringBy(rest, ',', withs);
        return;
    }
    splitStringBy(rest.substr(0, p2), ',', withs);
    splitStringBy(rest.substr(p2 + 1), ',', withouts);
}

bool readSparseIds(
    ManagedHeap* heap,
    const std::string& typeName,
    std::vector<std::int64_t>& outIds,
    ObjectRef* outValsRef = nullptr) {
    outIds.clear();
    if (!heap) return false;
    const auto idsVal = heap->getStaticField(
        semantic::stableTypeId("$ecs_sparse_ids::" + typeName));
    if (!idsVal) return false;
    const auto* idsRef = std::get_if<ObjectRef>(&*idsVal);
    if (!idsRef || !idsRef->valid()) return false;
    const auto len = heap->arrayLength(*idsRef);
    if (!len) return false;
    outIds.reserve(*len);
    for (std::size_t i = 0; i < *len; ++i) {
        const auto v = heap->arrayGet(*idsRef, i);
        if (v && std::holds_alternative<std::int64_t>(*v)) {
            outIds.push_back(std::get<std::int64_t>(*v));
        }
    }
    if (outValsRef) {
        const auto valsVal = heap->getStaticField(
            semantic::stableTypeId("$ecs_sparse_vals::" + typeName));
        if (valsVal && std::holds_alternative<ObjectRef>(*valsVal)) {
            *outValsRef = std::get<ObjectRef>(*valsVal);
        }
    }
    return true;
}

bool hasAnySparseEcsTable(
    ManagedHeap* heap,
    const std::vector<std::string>& cols,
    const std::vector<std::string>& withs,
    const std::vector<std::string>& withouts) {
    if (!heap) return false;
    for (const auto& list : {cols, withs, withouts}) {
        for (const auto& t : list) {
            if (heap->hasStaticField(semantic::stableTypeId("$ecs_sparse_ids::" + t))) {
                return true;
            }
        }
    }
    return false;
}

std::vector<std::int64_t> computeMatchedEcsEntities(
    ManagedHeap* heap,
    const std::vector<std::string>& cols,
    const std::vector<std::string>& withs,
    const std::vector<std::string>& withouts) {
    std::vector<std::int64_t> matched;
    bool initialized = false;
    for (const auto& list : {cols, withs}) {
        for (const auto& reqType : list) {
            std::vector<std::int64_t> typeIds;
            if (!readSparseIds(heap, reqType, typeIds)) {
                return {};
            }
            if (!initialized) {
                matched = std::move(typeIds);
                initialized = true;
            } else {
                std::vector<std::int64_t> next;
                for (const auto id : matched) {
                    if (std::binary_search(typeIds.begin(), typeIds.end(), id)) {
                        next.push_back(id);
                    }
                }
                matched = std::move(next);
            }
            if (matched.empty()) return {};
        }
    }
    for (const auto& exType : withouts) {
        std::vector<std::int64_t> exIds;
        if (!readSparseIds(heap, exType, exIds) || exIds.empty()) continue;
        std::vector<std::int64_t> next;
        for (const auto id : matched) {
            if (!std::binary_search(exIds.begin(), exIds.end(), id)) {
                next.push_back(id);
            }
        }
        matched = std::move(next);
        if (matched.empty()) return {};
    }
    return matched;
}

bool makeIntrinsicStringResult(
    std::string text,
    ManagedHeap* heap,
    Value& result,
    std::string& errorMessage) {
    if (heap) {
        RuntimeError err;
        const auto ref = heap->allocateString(std::move(text), &err);
        if (!ref) {
            errorMessage = err.message.empty()
                ? "out of memory allocating string"
                : err.message;
            return false;
        }
        result = *ref;
        return true;
    }
    result = std::move(text);
    return true;
}

} // namespace

bool tryExecuteRuntimeIntrinsic(
    std::string_view name,
    ValueSpan arguments,
    ManagedHeap* heap,
    Value& result,
    std::string& errorMessage) {
    constexpr std::string_view prefix = "$intrinsic::";
    if (name.rfind(prefix, 0) != 0) return false;
    const std::string_view op = name.substr(prefix.size());

    constexpr std::string_view staticHasPrefix = "static_has::";
    if (op.rfind(staticHasPrefix, 0) == 0) {
        const std::string idStr(op.substr(staticHasPrefix.size()));
        const auto id = static_cast<semantic::SymbolId>(
            std::strtoull(idStr.c_str(), nullptr, 10));
        result = heap ? heap->hasStaticField(id) : false;
        return true;
    }

    constexpr std::string_view staticGetPrefix = "static_get::";
    if (op.rfind(staticGetPrefix, 0) == 0) {
        const std::string_view subOp = op.substr(staticGetPrefix.size());
        constexpr std::string_view ecsColPrefix = "ecs_col::";
        if (subOp.rfind(ecsColPrefix, 0) == 0) {
            const auto rest = subOp.substr(ecsColPrefix.size());
            const auto sep = rest.find("::");
            if (sep == std::string_view::npos) {
                errorMessage = "malformed ecs_col intrinsic";
                return false;
            }
            const std::size_t colIdx = static_cast<std::size_t>(
                std::strtoull(std::string(rest.substr(0, sep)).c_str(), nullptr, 10));
            const auto spec = rest.substr(sep + 2);
            std::vector<std::string> cols, withs, withouts;
            parseEcsQuerySpec(spec, cols, withs, withouts);
            if (colIdx >= cols.size()) {
                result = arguments.empty() ? Value{} : arguments[0];
                return true;
            }
            const auto& targetType = cols[colIdx];
            if (!hasAnySparseEcsTable(heap, cols, withs, withouts) &&
                withs.empty() && withouts.empty()) {
                const auto denseId = semantic::stableTypeId("$ecs_column::" + targetType);
                if (heap) {
                    if (auto existing = heap->getStaticField(denseId)) {
                        result = std::move(*existing);
                        return true;
                    }
                }
                result = arguments.empty() ? Value{} : arguments[0];
                if (heap) {
                    heap->setStaticField(denseId, result);
                }
                return true;
            }
            if (!heap || arguments.empty() || !std::holds_alternative<ObjectRef>(arguments[0])) {
                result = arguments.empty() ? Value{} : arguments[0];
                return true;
            }
            const auto tmplRef = std::get<ObjectRef>(arguments[0]);
            const auto arrTypeId = heap->arrayTypeId(tmplRef).value_or(0);
            const auto elemPrim = heap->arrayElementType(tmplRef).value_or(semantic::PrimitiveType::Struct);
            const auto elemTypeId = heap->arrayElementTypeId(tmplRef).value_or(0);
            const auto matched = computeMatchedEcsEntities(heap, cols, withs, withouts);
            if (matched.empty()) {
                result = tmplRef;
                return true;
            }
            std::vector<std::int64_t> sparseIds;
            ObjectRef sparseValsRef{};
            if (!readSparseIds(heap, targetType, sparseIds, &sparseValsRef) ||
                !sparseValsRef.valid()) {
                result = tmplRef;
                return true;
            }
            std::vector<Value> matchedVals;
            matchedVals.reserve(matched.size());
            for (const auto entId : matched) {
                const auto it = std::lower_bound(sparseIds.begin(), sparseIds.end(), entId);
                if (it != sparseIds.end() && *it == entId) {
                    const auto idx = static_cast<std::size_t>(std::distance(sparseIds.begin(), it));
                    if (auto v = heap->arrayGet(sparseValsRef, idx)) {
                        matchedVals.push_back(std::move(*v));
                    }
                }
            }
            if (matchedVals.empty()) {
                result = tmplRef;
                return true;
            }
            RuntimeError allocErr;
            const auto outArr = heap->allocateTypedArray(
                arrTypeId, elemPrim, elemTypeId, matchedVals.size(), matchedVals[0], &allocErr);
            if (!outArr) {
                errorMessage = allocErr.message;
                return false;
            }
            for (std::size_t i = 0; i < matchedVals.size(); ++i) {
                (void)heap->arraySet(*outArr, i, matchedVals[i], &allocErr);
            }
            result = *outArr;
            return true;
        }

        constexpr std::string_view ecsEntPrefix = "ecs_ent::";
        if (subOp.rfind(ecsEntPrefix, 0) == 0) {
            const auto spec = subOp.substr(ecsEntPrefix.size());
            std::vector<std::string> cols, withs, withouts;
            parseEcsQuerySpec(spec, cols, withs, withouts);
            if (!heap || arguments.empty() || !std::holds_alternative<ObjectRef>(arguments[0])) {
                result = arguments.empty() ? Value{} : arguments[0];
                return true;
            }
            const auto tmplRef = std::get<ObjectRef>(arguments[0]);
            const auto arrTypeId = heap->arrayTypeId(tmplRef).value_or(0);
            std::vector<std::int64_t> matched;
            if (!hasAnySparseEcsTable(heap, cols, withs, withouts) &&
                withs.empty() && withouts.empty() && !cols.empty()) {
                const auto denseId = semantic::stableTypeId("$ecs_column::" + cols[0]);
                if (auto existing = heap->getStaticField(denseId)) {
                    if (const auto* arrRef = std::get_if<ObjectRef>(&*existing)) {
                        const auto len = heap->arrayLength(*arrRef).value_or(0);
                        matched.resize(len);
                        for (std::size_t i = 0; i < len; ++i) {
                            matched[i] = static_cast<std::int64_t>(i);
                        }
                    }
                }
            } else {
                matched = computeMatchedEcsEntities(heap, cols, withs, withouts);
            }
            if (matched.empty()) {
                result = tmplRef;
                return true;
            }
            RuntimeError allocErr;
            const auto outArr = heap->allocateTypedArray(
                arrTypeId,
                semantic::PrimitiveType::Int,
                0,
                matched.size(),
                std::int64_t{0},
                &allocErr);
            if (!outArr) {
                errorMessage = allocErr.message;
                return false;
            }
            for (std::size_t i = 0; i < matched.size(); ++i) {
                (void)heap->arraySet(*outArr, i, matched[i], &allocErr);
            }
            result = *outArr;
            return true;
        }

        const std::string idStr(subOp);
        const auto id = static_cast<semantic::SymbolId>(
            std::strtoull(idStr.c_str(), nullptr, 10));
        if (heap) {
            if (auto existing = heap->getStaticField(id)) {
                result = std::move(*existing);
                return true;
            }
        }
        result = arguments.empty() ? Value{} : arguments[0];
        if (heap) {
            heap->setStaticField(id, result);
        }
        return true;
    }

    constexpr std::string_view ecsWritebackPrefix = "ecs_writeback::";
    if (op.rfind(ecsWritebackPrefix, 0) == 0) {
        const auto rest = op.substr(ecsWritebackPrefix.size());
        const auto sep = rest.find("::");
        if (sep == std::string_view::npos || arguments.size() < 2 || !heap) {
            result = std::monostate{};
            return true;
        }
        const std::size_t colIdx = static_cast<std::size_t>(
            std::strtoull(std::string(rest.substr(0, sep)).c_str(), nullptr, 10));
        const auto spec = rest.substr(sep + 2);
        std::vector<std::string> cols, withs, withouts;
        parseEcsQuerySpec(spec, cols, withs, withouts);
        if (colIdx >= cols.size()) {
            result = std::monostate{};
            return true;
        }
        const std::string& compKey = cols[colIdx];
        if (!heap->hasStaticField(semantic::stableTypeId("$ecs_sparse_ids::" + compKey))) {
            result = std::monostate{};
            return true;
        }
        const auto* idxPtr = std::get_if<std::int64_t>(&arguments[0]);
        if (!idxPtr || *idxPtr < 0) {
            result = std::monostate{};
            return true;
        }
        const auto matched = computeMatchedEcsEntities(heap, cols, withs, withouts);
        const auto idx = static_cast<std::size_t>(*idxPtr);
        if (idx < matched.size()) {
            const auto entId = matched[idx];
            std::vector<std::int64_t> sparseIds;
            ObjectRef sparseValsRef{};
            if (readSparseIds(heap, compKey, sparseIds, &sparseValsRef) &&
                sparseValsRef.valid()) {
                const auto it = std::lower_bound(sparseIds.begin(), sparseIds.end(), entId);
                if (it != sparseIds.end() && *it == entId) {
                    const auto srcIdx = static_cast<std::size_t>(
                        std::distance(sparseIds.begin(), it));
                    RuntimeError setErr;
                    (void)heap->arraySet(sparseValsRef, srcIdx, arguments[1], &setErr);
                }
            }
        }
        result = std::monostate{};
        return true;
    }

    constexpr std::string_view staticSetPrefix = "static_set::";
    if (op.rfind(staticSetPrefix, 0) == 0) {
        const std::string idStr(op.substr(staticSetPrefix.size()));
        const auto id = static_cast<semantic::SymbolId>(
            std::strtoull(idStr.c_str(), nullptr, 10));
        if (heap && !arguments.empty()) {
            heap->setStaticField(id, arguments[0]);
        }
        result = std::monostate{};
        return true;
    }

    if (op == "string_concat") {
        if (arguments.size() != 2) {
            errorMessage = "string_concat requires 2 arguments";
            return false;
        }
        return makeIntrinsicStringResult(
            extractIntrinsicString(arguments[0], heap) +
                extractIntrinsicString(arguments[1], heap),
            heap,
            result,
            errorMessage);
    }

    constexpr std::string_view toStringPrefix = "to_string::";
    if (op.rfind(toStringPrefix, 0) == 0) {
        if (arguments.empty()) {
            errorMessage = "to_string requires 1 argument";
            return false;
        }
        const auto& val = arguments[0];
        if (const auto* d = std::get_if<double>(&val)) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%g", *d);
            return makeIntrinsicStringResult(buf, heap, result, errorMessage);
        }
        if (const auto* f = std::get_if<FloatValue>(&val)) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(f->value));
            return makeIntrinsicStringResult(buf, heap, result, errorMessage);
        }
        return makeIntrinsicStringResult(
            valueToString(val, heap), heap, result, errorMessage);
    }

    if (!arguments.empty() && std::holds_alternative<NullString>(arguments[0])) {
        errorMessage = "cannot invoke string method on null string";
        return false;
    }

    if (op == "string_length") {
        result = static_cast<std::int64_t>(
            extractIntrinsicString(arguments[0], heap).size());
        return true;
    }
    if (op == "string_is_empty") {
        result = extractIntrinsicString(arguments[0], heap).empty();
        return true;
    }
    if (op == "string_contains") {
        const auto s = extractIntrinsicString(arguments[0], heap);
        const auto sub = extractIntrinsicString(arguments[1], heap);
        result = (s.find(sub) != std::string::npos);
        return true;
    }
    if (op == "string_starts_with") {
        const auto s = extractIntrinsicString(arguments[0], heap);
        const auto p = extractIntrinsicString(arguments[1], heap);
        result = (s.size() >= p.size() && s.compare(0, p.size(), p) == 0);
        return true;
    }
    if (op == "string_ends_with") {
        const auto s = extractIntrinsicString(arguments[0], heap);
        const auto sfx = extractIntrinsicString(arguments[1], heap);
        result = (s.size() >= sfx.size() &&
                  s.compare(s.size() - sfx.size(), sfx.size(), sfx) == 0);
        return true;
    }
    if (op == "string_index_of") {
        const auto s = extractIntrinsicString(arguments[0], heap);
        const auto sub = extractIntrinsicString(arguments[1], heap);
        const auto pos = s.find(sub);
        result = (pos == std::string::npos)
            ? std::int64_t{-1}
            : static_cast<std::int64_t>(pos);
        return true;
    }
    if (op == "string_substring_1") {
        const auto s = extractIntrinsicString(arguments[0], heap);
        const auto rawStart = std::holds_alternative<std::int64_t>(arguments[1])
            ? std::get<std::int64_t>(arguments[1])
            : 0;
        const auto clampedStart = std::clamp<std::int64_t>(
            rawStart, 0, static_cast<std::int64_t>(s.size()));
        return makeIntrinsicStringResult(
            s.substr(static_cast<std::size_t>(clampedStart)),
            heap,
            result,
            errorMessage);
    }
    if (op == "string_substring_2") {
        const auto s = extractIntrinsicString(arguments[0], heap);
        const auto rawStart = std::holds_alternative<std::int64_t>(arguments[1])
            ? std::get<std::int64_t>(arguments[1])
            : 0;
        const auto rawLen = std::holds_alternative<std::int64_t>(arguments[2])
            ? std::get<std::int64_t>(arguments[2])
            : 0;
        const auto clampedStart = std::clamp<std::int64_t>(
            rawStart, 0, static_cast<std::int64_t>(s.size()));
        const auto clampedLen = std::clamp<std::int64_t>(
            rawLen, 0, static_cast<std::int64_t>(s.size()) - clampedStart);
        return makeIntrinsicStringResult(
            s.substr(
                static_cast<std::size_t>(clampedStart),
                static_cast<std::size_t>(clampedLen)),
            heap,
            result,
            errorMessage);
    }
    if (op == "string_trim") {
        const auto s = extractIntrinsicString(arguments[0], heap);
        const auto isSpace = [](unsigned char c) {
            return c == ' ' || c == '\t' || c == '\r' ||
                   c == '\n' || c == '\v' || c == '\f';
        };
        std::size_t first = 0;
        while (first < s.size() && isSpace(static_cast<unsigned char>(s[first]))) {
            ++first;
        }
        std::size_t last = s.size();
        while (last > first && isSpace(static_cast<unsigned char>(s[last - 1]))) {
            --last;
        }
        return makeIntrinsicStringResult(
            s.substr(first, last - first), heap, result, errorMessage);
    }
    if (op == "string_replace") {
        auto s = extractIntrinsicString(arguments[0], heap);
        const auto from = extractIntrinsicString(arguments[1], heap);
        const auto to = extractIntrinsicString(arguments[2], heap);
        if (!from.empty()) {
            std::size_t pos = 0;
            while ((pos = s.find(from, pos)) != std::string::npos) {
                s.replace(pos, from.size(), to);
                pos += to.size();
            }
        }
        return makeIntrinsicStringResult(std::move(s), heap, result, errorMessage);
    }
    if (op == "string_to_upper") {
        auto s = extractIntrinsicString(arguments[0], heap);
        for (auto& ch : s) {
            if (ch >= 'a' && ch <= 'z') {
                ch = static_cast<char>(ch - 'a' + 'A');
            }
        }
        return makeIntrinsicStringResult(std::move(s), heap, result, errorMessage);
    }
    if (op == "string_to_lower") {
        auto s = extractIntrinsicString(arguments[0], heap);
        for (auto& ch : s) {
            if (ch >= 'A' && ch <= 'Z') {
                ch = static_cast<char>(ch - 'A' + 'a');
            }
        }
        return makeIntrinsicStringResult(std::move(s), heap, result, errorMessage);
    }

    errorMessage = "unknown runtime intrinsic '" + std::string(name) + "'";
    return false;
}

} // namespace realscript::runtime
