#include "RuntimeInternal.h"

#include "ExecutionMirror.h"
#include "realscript/diagnostics/Diagnostic.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

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

    {
        std::vector<std::pair<semantic::SymbolId, std::vector<BlittableFieldInput>>> structSpecs;
        for (const auto& [typeId, type] : types) {
            if (type->kind != semantic::TypeKind::Struct) continue;
            std::vector<BlittableFieldInput> fInputs;
            fInputs.reserve(type->fields.size());
            for (const auto& field : type->fields) {
                const semantic::SymbolId fTid = semantic::isExactType(field.type)
                    ? semantic::stableTypeId(field.typeName)
                    : 0;
                fInputs.push_back(BlittableFieldInput{field.type, fTid});
            }
            structSpecs.emplace_back(typeId, std::move(fInputs));
        }
        if (!structSpecs.empty()) {
            registerBlittableStructTypes(structSpecs);
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

namespace {

DeterminismOperationId cachedInstructionOperationId(
    bytecode::Opcode opcode) noexcept {
    constexpr auto Count =
        static_cast<std::size_t>(bytecode::Opcode::AsyncResolve) + 1U;
    static const auto ids = [] {
        std::array<DeterminismOperationId, Count> result{};
        for (std::size_t index = 0; index < result.size(); ++index) {
            result[index] = determinismOperationId(bytecode::opcodeName(
                static_cast<bytecode::Opcode>(index)));
        }
        return result;
    }();
    return ids[static_cast<std::size_t>(opcode)];
}

DeterminismOperationId cachedTerminatorOperationId(
    bytecode::TerminatorKind terminator) noexcept {
    constexpr auto Count =
        static_cast<std::size_t>(bytecode::TerminatorKind::Throw) + 1U;
    static const auto ids = [] {
        std::array<DeterminismOperationId, Count> result{};
        for (std::size_t index = 0; index < result.size(); ++index) {
            result[index] = determinismOperationId(bytecode::terminatorName(
                static_cast<bytecode::TerminatorKind>(index)));
        }
        return result;
    }();
    return ids[static_cast<std::size_t>(terminator)];
}

std::uint16_t toCompactReg(std::uint32_t value) noexcept {
    return value < InvalidCompactReg
        ? static_cast<std::uint16_t>(value)
        : InvalidCompactReg;
}

std::uint64_t nextExecutionMirrorId() noexcept {
    static std::atomic<std::uint64_t> next{1};
    auto id = next.fetch_add(1, std::memory_order_relaxed);
    if (id == 0) id = next.fetch_add(1, std::memory_order_relaxed);
    return id;
}

} // namespace

std::shared_ptr<const ExecutionMirror> buildExecutionMirror(
    const std::vector<bytecode::Module>& modules,
    const std::unordered_map<
        semantic::SymbolId, ProgramImage::FunctionLocation>& functions) {
    auto mirror = std::make_shared<ExecutionMirror>();
    mirror->instanceId = nextExecutionMirrorId();
    std::size_t functionCount = 0;
    for (const auto& module : modules) functionCount += module.functions.size();
    mirror->functions.reserve(functionCount);

    for (const auto& module : modules) {
        for (const auto& function : module.functions) {
            const auto quickOpcode = [&](const bytecode::Instruction& instruction) {
                const bool validRes =
                    instruction.result < function.registerTypes.size() &&
                    instruction.result < InvalidCompactReg;
                switch (instruction.opcode) {
                case bytecode::Opcode::LoadParameter:
                    if (validRes &&
                        instruction.index < function.parameterTypes.size() &&
                        instruction.index < InvalidCompactReg) {
                        return QuickOpcode::LoadParameter;
                    }
                    return QuickOpcode::Generic;
                case bytecode::Opcode::ConstantInt:
                    if (validRes &&
                        function.registerTypes[instruction.result] ==
                            semantic::PrimitiveType::Int) {
                        return QuickOpcode::ConstantInt;
                    }
                    if (validRes &&
                        function.registerTypes[instruction.result] ==
                            semantic::PrimitiveType::Long) {
                        return QuickOpcode::ConstantLong;
                    }
                    return QuickOpcode::Generic;
                case bytecode::Opcode::ConstantBool:
                    if (validRes &&
                        function.registerTypes[instruction.result] ==
                            semantic::PrimitiveType::Bool) {
                        return QuickOpcode::ConstantBool;
                    }
                    return QuickOpcode::Generic;
                case bytecode::Opcode::ConstantDouble:
                    if (validRes &&
                        function.registerTypes[instruction.result] ==
                            semantic::PrimitiveType::Double) {
                        return QuickOpcode::ConstantDouble;
                    }
                    return QuickOpcode::Generic;
                case bytecode::Opcode::ArrayLength:
                    if (validRes &&
                        instruction.operands.size() == 1 &&
                        instruction.operands[0] < function.registerTypes.size() &&
                        instruction.operands[0] < InvalidCompactReg &&
                        function.registerTypes[instruction.operands[0]] ==
                            semantic::PrimitiveType::Array &&
                        function.registerTypes[instruction.result] ==
                            semantic::PrimitiveType::Int) {
                        return QuickOpcode::ArrayLengthFast;
                    }
                    return QuickOpcode::Generic;
                case bytecode::Opcode::ConvertIntToLong:
                    if (validRes &&
                        instruction.operands.size() == 1 &&
                        instruction.operands[0] < function.registerTypes.size() &&
                        instruction.operands[0] < InvalidCompactReg &&
                        function.registerTypes[instruction.operands[0]] ==
                            semantic::PrimitiveType::Int &&
                        function.registerTypes[instruction.result] ==
                            semantic::PrimitiveType::Long) {
                        return QuickOpcode::ConvertIntToLongFast;
                    }
                    return QuickOpcode::Generic;
                case bytecode::Opcode::ConvertIntToDouble:
                    if (validRes &&
                        instruction.operands.size() == 1 &&
                        instruction.operands[0] < function.registerTypes.size() &&
                        instruction.operands[0] < InvalidCompactReg &&
                        function.registerTypes[instruction.operands[0]] ==
                            semantic::PrimitiveType::Int &&
                        function.registerTypes[instruction.result] ==
                            semantic::PrimitiveType::Double) {
                        return QuickOpcode::ConvertIntToDoubleFast;
                    }
                    return QuickOpcode::Generic;
                case bytecode::Opcode::ConvertLongToDouble:
                    if (validRes &&
                        instruction.operands.size() == 1 &&
                        instruction.operands[0] < function.registerTypes.size() &&
                        instruction.operands[0] < InvalidCompactReg &&
                        function.registerTypes[instruction.operands[0]] ==
                            semantic::PrimitiveType::Long &&
                        function.registerTypes[instruction.result] ==
                            semantic::PrimitiveType::Double) {
                        return QuickOpcode::ConvertLongToDoubleFast;
                    }
                    return QuickOpcode::Generic;
                case bytecode::Opcode::ConvertNumeric:
                    if (validRes &&
                        instruction.operands.size() == 1 &&
                        instruction.operands[0] < function.registerTypes.size() &&
                        instruction.operands[0] < InvalidCompactReg) {
                        const auto srcType = function.registerTypes[instruction.operands[0]];
                        const auto dstType = function.registerTypes[instruction.result];
                        if (srcType == semantic::PrimitiveType::Int &&
                            dstType == semantic::PrimitiveType::Float) {
                            return QuickOpcode::ConvertIntToFloatFast;
                        }
                        if (srcType == semantic::PrimitiveType::Double &&
                            dstType == semantic::PrimitiveType::Float) {
                            return QuickOpcode::ConvertDoubleToFloatFast;
                        }
                        if (srcType == semantic::PrimitiveType::Float &&
                            dstType == semantic::PrimitiveType::Int) {
                            return QuickOpcode::ConvertFloatToIntFast;
                        }
                        if (srcType == semantic::PrimitiveType::Float &&
                            dstType == semantic::PrimitiveType::Double) {
                            return QuickOpcode::ConvertFloatToDoubleFast;
                        }
                        if (srcType == semantic::PrimitiveType::Int &&
                            dstType == semantic::PrimitiveType::Long) {
                            return QuickOpcode::ConvertIntToLongFast;
                        }
                        if (srcType == semantic::PrimitiveType::Long &&
                            dstType == semantic::PrimitiveType::Int) {
                            return QuickOpcode::ConvertLongToIntFast;
                        }
                        if (srcType == semantic::PrimitiveType::Int &&
                            dstType == semantic::PrimitiveType::Double) {
                            return QuickOpcode::ConvertIntToDoubleFast;
                        }
                        if (srcType == semantic::PrimitiveType::Long &&
                            dstType == semantic::PrimitiveType::Double) {
                            return QuickOpcode::ConvertLongToDoubleFast;
                        }
                    }
                    return QuickOpcode::Generic;
                case bytecode::Opcode::NewStruct:
                    if (validRes &&
                        function.registerTypes[instruction.result] ==
                            semantic::PrimitiveType::Struct &&
                        instruction.result < function.registerTypeIds.size() &&
                        instruction.typeIndex < module.types.size() &&
                        module.types[instruction.typeIndex].kind == semantic::TypeKind::Struct) {
                        const auto tid = module.types[instruction.typeIndex].id;
                        if (function.registerTypeIds[instruction.result] == tid &&
                            (tid == fixedTypeId() ||
                             tid == fixed16TypeId() ||
                             tid == fixed64TypeId() ||
                             tid == vec2TypeId() ||
                             tid == vec3TypeId() ||
                             tid == quatTypeId() ||
                             tid == vec2fTypeId() ||
                             tid == vec3fTypeId() ||
                             tid == quatfTypeId())) {
                            return QuickOpcode::NewStructFast;
                        }
                    }
                    return QuickOpcode::Generic;
                case bytecode::Opcode::NegateInt:
                    if (validRes &&
                        !instruction.checkedArithmetic &&
                        instruction.operands.size() == 1 &&
                        instruction.operands[0] < function.registerTypes.size() &&
                        instruction.operands[0] < InvalidCompactReg &&
                        function.registerTypes[instruction.operands[0]] ==
                            semantic::PrimitiveType::Int &&
                        function.registerTypes[instruction.result] ==
                            semantic::PrimitiveType::Int) {
                        return QuickOpcode::NegateIntUnchecked;
                    }
                    return QuickOpcode::Generic;
                case bytecode::Opcode::NegateDouble:
                    if (validRes &&
                        instruction.operands.size() == 1 &&
                        instruction.operands[0] < function.registerTypes.size() &&
                        instruction.operands[0] < InvalidCompactReg) {
                        if (function.registerTypes[instruction.operands[0]] ==
                                semantic::PrimitiveType::Double &&
                            function.registerTypes[instruction.result] ==
                                semantic::PrimitiveType::Double) {
                            return QuickOpcode::NegateDouble;
                        }
                        if (function.registerTypes[instruction.operands[0]] ==
                                semantic::PrimitiveType::Float &&
                            function.registerTypes[instruction.result] ==
                                semantic::PrimitiveType::Float) {
                            return QuickOpcode::NegateFloat;
                        }
                    }
                    return QuickOpcode::Generic;
                case bytecode::Opcode::LogicalNot:
                    if (validRes &&
                        instruction.operands.size() == 1 &&
                        instruction.operands[0] < function.registerTypes.size() &&
                        instruction.operands[0] < InvalidCompactReg &&
                        function.registerTypes[instruction.operands[0]] ==
                            semantic::PrimitiveType::Bool &&
                        function.registerTypes[instruction.result] ==
                            semantic::PrimitiveType::Bool) {
                        return QuickOpcode::LogicalNot;
                    }
                    return QuickOpcode::Generic;
                case bytecode::Opcode::LoadLocal:
                    if (validRes &&
                        instruction.index < function.localTypes.size() &&
                        instruction.index < InvalidCompactReg &&
                        function.registerTypes[instruction.result] ==
                            function.localTypes[instruction.index]) {
                        return QuickOpcode::LoadLocal;
                    }
                    return QuickOpcode::Generic;
                case bytecode::Opcode::StoreLocal:
                    if (instruction.index < function.localTypes.size() &&
                        instruction.index < InvalidCompactReg &&
                        instruction.operands.size() == 1 &&
                        instruction.operands[0] < function.registerTypes.size() &&
                        instruction.operands[0] < InvalidCompactReg &&
                        function.registerTypes[instruction.operands[0]] ==
                            function.localTypes[instruction.index]) {
                        return QuickOpcode::StoreLocal;
                    }
                    return QuickOpcode::Generic;
                case bytecode::Opcode::CheckNotNull:
                    if (validRes &&
                        instruction.operands.size() == 1 &&
                        instruction.operands[0] < function.registerTypes.size() &&
                        instruction.operands[0] < InvalidCompactReg &&
                        function.registerTypes[instruction.operands[0]] ==
                            semantic::PrimitiveType::Object) {
                        return QuickOpcode::CheckNotNullFast;
                    }
                    return QuickOpcode::Generic;
                case bytecode::Opcode::LoadElement:
                    if (validRes &&
                        instruction.elementType == semantic::PrimitiveType::Int &&
                        instruction.operands.size() == 2 &&
                        instruction.operands[0] < function.registerTypes.size() &&
                        instruction.operands[0] < InvalidCompactReg &&
                        instruction.operands[1] < function.registerTypes.size() &&
                        instruction.operands[1] < InvalidCompactReg &&
                        function.registerTypes[instruction.operands[0]] ==
                            semantic::PrimitiveType::Array &&
                        function.registerTypes[instruction.operands[1]] ==
                            semantic::PrimitiveType::Int) {
                        return QuickOpcode::LoadElementIntFast;
                    }
                    return QuickOpcode::Generic;
                case bytecode::Opcode::StoreElement:
                    if (instruction.elementType == semantic::PrimitiveType::Int &&
                        instruction.operands.size() == 3 &&
                        instruction.operands[0] < function.registerTypes.size() &&
                        instruction.operands[0] < InvalidCompactReg &&
                        instruction.operands[1] < function.registerTypes.size() &&
                        instruction.operands[1] < InvalidCompactReg &&
                        instruction.operands[2] < function.registerTypes.size() &&
                        instruction.operands[2] < InvalidCompactReg &&
                        function.registerTypes[instruction.operands[0]] ==
                            semantic::PrimitiveType::Array &&
                        function.registerTypes[instruction.operands[1]] ==
                            semantic::PrimitiveType::Int &&
                        function.registerTypes[instruction.operands[2]] ==
                            semantic::PrimitiveType::Int) {
                        return QuickOpcode::StoreElementIntFast;
                    }
                    return QuickOpcode::Generic;
                case bytecode::Opcode::LoadField:
                    if (validRes &&
                        instruction.operands.size() == 1 &&
                        instruction.operands[0] < function.registerTypes.size() &&
                        instruction.operands[0] < InvalidCompactReg &&
                        function.registerTypes[instruction.operands[0]] ==
                            semantic::PrimitiveType::Object &&
                        instruction.typeIndex < module.types.size() &&
                        instruction.index < module.types[instruction.typeIndex].fields.size() &&
                        instruction.index < InvalidCompactReg) {
                        const auto& fld = module.types[instruction.typeIndex].fields[instruction.index];
                        if (fld.type != semantic::PrimitiveType::Handle &&
                            fld.type != semantic::PrimitiveType::Void &&
                            fld.type != semantic::PrimitiveType::Null &&
                            fld.type != semantic::PrimitiveType::Error &&
                            function.registerTypes[instruction.result] == fld.type) {
                            return QuickOpcode::LoadFieldFast;
                        }
                    }
                    return QuickOpcode::Generic;
                case bytecode::Opcode::StoreField:
                    if (instruction.operands.size() == 2 &&
                        instruction.operands[0] < function.registerTypes.size() &&
                        instruction.operands[0] < InvalidCompactReg &&
                        instruction.operands[1] < function.registerTypes.size() &&
                        instruction.operands[1] < InvalidCompactReg &&
                        function.registerTypes[instruction.operands[0]] ==
                            semantic::PrimitiveType::Object &&
                        instruction.typeIndex < module.types.size() &&
                        instruction.index < module.types[instruction.typeIndex].fields.size() &&
                        instruction.index < InvalidCompactReg) {
                        const auto& fld = module.types[instruction.typeIndex].fields[instruction.index];
                        if (fld.type != semantic::PrimitiveType::Handle &&
                            fld.type != semantic::PrimitiveType::Void &&
                            fld.type != semantic::PrimitiveType::Null &&
                            fld.type != semantic::PrimitiveType::Error &&
                            function.registerTypes[instruction.operands[1]] == fld.type) {
                            return QuickOpcode::StoreFieldFast;
                        }
                    }
                    return QuickOpcode::Generic;
                case bytecode::Opcode::LoadStructField:
                    if (validRes &&
                        instruction.operands.size() == 1 &&
                        instruction.operands[0] < function.registerTypes.size() &&
                        instruction.operands[0] < InvalidCompactReg &&
                        function.registerTypes[instruction.operands[0]] ==
                            semantic::PrimitiveType::Struct &&
                        instruction.typeIndex < module.types.size() &&
                        module.types[instruction.typeIndex].kind == semantic::TypeKind::Struct &&
                        instruction.index < module.types[instruction.typeIndex].fields.size() &&
                        instruction.index < InvalidCompactReg) {
                        const auto& fld = module.types[instruction.typeIndex].fields[instruction.index];
                        if (!semantic::isExactType(fld.type) &&
                            fld.type != semantic::PrimitiveType::Handle &&
                            function.registerTypes[instruction.result] == fld.type) {
                            return QuickOpcode::LoadStructFieldFast;
                        }
                        if (fld.type == semantic::PrimitiveType::Struct &&
                            function.registerTypes[instruction.result] ==
                                semantic::PrimitiveType::Struct &&
                            instruction.result < function.registerTypeIds.size() &&
                            function.registerTypeIds[instruction.result] ==
                                exactTypeId(fld.type, fld.typeName)) {
                            return QuickOpcode::LoadStructFieldFast;
                        }
                    }
                    return QuickOpcode::Generic;
                case bytecode::Opcode::StoreStructField:
                    if (validRes &&
                        instruction.operands.size() == 2 &&
                        instruction.operands[0] < function.registerTypes.size() &&
                        instruction.operands[0] < InvalidCompactReg &&
                        instruction.operands[1] < function.registerTypes.size() &&
                        instruction.operands[1] < InvalidCompactReg &&
                        function.registerTypes[instruction.operands[0]] ==
                            semantic::PrimitiveType::Struct &&
                        instruction.typeIndex < module.types.size() &&
                        module.types[instruction.typeIndex].kind == semantic::TypeKind::Struct &&
                        instruction.index < module.types[instruction.typeIndex].fields.size() &&
                        instruction.index < InvalidCompactReg) {
                        const auto& fld = module.types[instruction.typeIndex].fields[instruction.index];
                        if (!semantic::isExactType(fld.type) &&
                            fld.type != semantic::PrimitiveType::Handle &&
                            function.registerTypes[instruction.operands[1]] == fld.type) {
                            return QuickOpcode::StoreStructFieldFast;
                        }
                        if (fld.type == semantic::PrimitiveType::Struct &&
                            function.registerTypes[instruction.operands[1]] ==
                                semantic::PrimitiveType::Struct &&
                            instruction.operands[1] < function.registerTypeIds.size() &&
                            function.registerTypeIds[instruction.operands[1]] ==
                                exactTypeId(fld.type, fld.typeName)) {
                            return QuickOpcode::StoreStructFieldFast;
                        }
                    }
                    return QuickOpcode::Generic;
                default:
                    break;
                }
                if (validRes &&
                    instruction.operands.size() == 2 &&
                    instruction.operands[0] < function.registerTypes.size() &&
                    instruction.operands[0] < InvalidCompactReg &&
                    instruction.operands[1] < function.registerTypes.size() &&
                    instruction.operands[1] < InvalidCompactReg &&
                    function.registerTypes[instruction.operands[0]] ==
                        semantic::PrimitiveType::Double &&
                    function.registerTypes[instruction.operands[1]] ==
                        semantic::PrimitiveType::Double) {
                    switch (instruction.opcode) {
                    case bytecode::Opcode::AddDouble: return QuickOpcode::AddDouble;
                    case bytecode::Opcode::SubtractDouble: return QuickOpcode::SubtractDouble;
                    case bytecode::Opcode::MultiplyDouble: return QuickOpcode::MultiplyDouble;
                    case bytecode::Opcode::DivideDouble: return QuickOpcode::DivideDouble;
                    case bytecode::Opcode::LessDouble: return QuickOpcode::LessDouble;
                    case bytecode::Opcode::LessOrEqualDouble: return QuickOpcode::LessOrEqualDouble;
                    case bytecode::Opcode::GreaterDouble: return QuickOpcode::GreaterDouble;
                    case bytecode::Opcode::GreaterOrEqualDouble: return QuickOpcode::GreaterOrEqualDouble;
                    default: break;
                    }
                }
                if (validRes &&
                    instruction.operands.size() == 2 &&
                    instruction.operands[0] < function.registerTypes.size() &&
                    instruction.operands[0] < InvalidCompactReg &&
                    instruction.operands[1] < function.registerTypes.size() &&
                    instruction.operands[1] < InvalidCompactReg &&
                    function.registerTypes[instruction.operands[0]] ==
                        semantic::PrimitiveType::Float &&
                    function.registerTypes[instruction.operands[1]] ==
                        semantic::PrimitiveType::Float) {
                    const auto resType = function.registerTypes[instruction.result];
                    if (resType == semantic::PrimitiveType::Float) {
                        switch (instruction.opcode) {
                        case bytecode::Opcode::AddDouble: return QuickOpcode::AddFloat;
                        case bytecode::Opcode::SubtractDouble: return QuickOpcode::SubtractFloat;
                        case bytecode::Opcode::MultiplyDouble: return QuickOpcode::MultiplyFloat;
                        case bytecode::Opcode::DivideDouble: return QuickOpcode::DivideFloat;
                        default: break;
                        }
                    } else if (resType == semantic::PrimitiveType::Bool) {
                        switch (instruction.opcode) {
                        case bytecode::Opcode::LessDouble: return QuickOpcode::LessFloat;
                        case bytecode::Opcode::LessOrEqualDouble: return QuickOpcode::LessOrEqualFloat;
                        case bytecode::Opcode::GreaterDouble: return QuickOpcode::GreaterFloat;
                        case bytecode::Opcode::GreaterOrEqualDouble: return QuickOpcode::GreaterOrEqualFloat;
                        case bytecode::Opcode::Equal: return QuickOpcode::EqualFloat;
                        case bytecode::Opcode::NotEqual: return QuickOpcode::NotEqualFloat;
                        default: break;
                        }
                    }
                }
                if (!validRes ||
                    instruction.operands.size() < 2 ||
                    instruction.operands[0] >= function.registerTypes.size() ||
                    instruction.operands[0] >= InvalidCompactReg ||
                    instruction.operands[1] >= function.registerTypes.size() ||
                    instruction.operands[1] >= InvalidCompactReg ||
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
                case bytecode::Opcode::Equal:
                    return QuickOpcode::EqualInt;
                case bytecode::Opcode::NotEqual:
                    return QuickOpcode::NotEqualInt;
                default: return QuickOpcode::Generic;
                }
            };
            ExecutionFunction decoded;
            decoded.bytecode = &function;
            decoded.hasDebugSequencePoints = !function.debugInfo.sequencePoints.empty();
            decoded.blocks.resize(function.blocks.size());
            decoded.blockIndices.reserve(function.blocks.size());
            for (std::size_t index = 0; index < function.blocks.size(); ++index) {
                const auto& bb = function.blocks[index];
                auto& blockMirror = decoded.blocks[index];
                blockMirror.bytecode = &bb;
                blockMirror.terminatorKind = bb.terminator.kind;
                blockMirror.terminatorOpId = cachedTerminatorOperationId(bb.terminator.kind);
                blockMirror.terminatorOpName = bytecode::terminatorName(bb.terminator.kind);
                if (bb.terminator.kind == bytecode::TerminatorKind::Branch) {
                    blockMirror.terminatorReg = bb.terminator.condition;
                    blockMirror.conditionPrevalidatedBool =
                        bb.terminator.condition < function.registerTypes.size() &&
                        function.registerTypes[bb.terminator.condition] ==
                            semantic::PrimitiveType::Bool;
                } else if (bb.terminator.kind == bytecode::TerminatorKind::ReturnValue ||
                           bb.terminator.kind == bytecode::TerminatorKind::Throw) {
                    blockMirror.terminatorReg = bb.terminator.value;
                    if (bb.terminator.kind == bytecode::TerminatorKind::ReturnValue &&
                        bb.terminator.value < function.registerTypes.size() &&
                        function.registerTypes[bb.terminator.value] == function.returnType &&
                        ((!semantic::isExactType(function.returnType) &&
                          function.returnType != semantic::PrimitiveType::Object &&
                          function.returnType != semantic::PrimitiveType::Array &&
                          function.returnType != semantic::PrimitiveType::String &&
                          function.returnType != semantic::PrimitiveType::Handle) ||
                         (function.returnType == semantic::PrimitiveType::Struct &&
                          bb.terminator.value < function.registerTypeIds.size() &&
                          function.registerTypeIds[bb.terminator.value] == function.returnTypeId &&
                          (function.returnTypeId == fixedTypeId() ||
                           function.returnTypeId == fixed16TypeId() ||
                           function.returnTypeId == fixed64TypeId() ||
                           function.returnTypeId == vec2TypeId() ||
                           function.returnTypeId == vec3TypeId() ||
                           function.returnTypeId == quatTypeId() ||
                           function.returnTypeId == vec2fTypeId() ||
                           function.returnTypeId == vec3fTypeId() ||
                           function.returnTypeId == quatfTypeId())))) {
                        blockMirror.returnPrevalidatedScalar = true;
                    }
                }

                auto& instructions = blockMirror.instructions;
                instructions.reserve(bb.instructions.size());
                for (const auto& instruction : bb.instructions) {
                    ExecutionInstruction execInst;
                    execInst.bytecode = &instruction;
                    execInst.opcode = quickOpcode(instruction);
                    execInst.operationId = cachedInstructionOperationId(instruction.opcode);
                    execInst.opcodeName = bytecode::opcodeName(instruction.opcode);
                    execInst.result = toCompactReg(instruction.result);
                    execInst.checkedArithmetic1 = instruction.checkedArithmetic;
                    execInst.integerImmediate = instruction.activeIntegerImmediate();
                    if (instruction.opcode == bytecode::Opcode::LoadParameter ||
                        instruction.opcode == bytecode::Opcode::LoadLocal ||
                        instruction.opcode == bytecode::Opcode::StoreLocal ||
                        instruction.opcode == bytecode::Opcode::LoadField ||
                        instruction.opcode == bytecode::Opcode::StoreField ||
                        instruction.opcode == bytecode::Opcode::LoadStructField ||
                        instruction.opcode == bytecode::Opcode::StoreStructField) {
                        execInst.auxReg1 = toCompactReg(instruction.index);
                    }
                    if (!instruction.operands.empty()) {
                        execInst.op0 = toCompactReg(instruction.operands[0]);
                    }
                    if (instruction.operands.size() >= 2) {
                        execInst.op1 = toCompactReg(instruction.operands[1]);
                    }
                    if (instruction.operands.size() >= 3) {
                        execInst.op2 = toCompactReg(instruction.operands[2]);
                    }
                    instructions.push_back(execInst);
                }

                const auto isBinaryInt = [](QuickOpcode opcode) {
                    return opcode >= QuickOpcode::AddInt &&
                        opcode <= QuickOpcode::NotEqualInt;
                };
                for (std::size_t instructionIndex = 0;
                     instructionIndex < instructions.size();) {
                    auto& first = instructions[instructionIndex];
                    // Pattern 1: LoadParameter + StoreLocal (span 2)
                    if (instructionIndex + 1 < instructions.size() &&
                        first.opcode == QuickOpcode::LoadParameter &&
                        instructions[instructionIndex + 1].opcode ==
                            QuickOpcode::StoreLocal &&
                        instructions[instructionIndex + 1].op0 == first.result) {
                        first.superOpcode = SuperOpcode::LoadParameterStoreLocal;
                        first.span = 2;
                        first.storeSlot = instructions[instructionIndex + 1].auxReg1;
                        instructionIndex += 2;
                        continue;
                    }
                    // Pattern 2: LoadLocal + (ConstantInt | LoadLocal) + BinaryInt (+ StoreLocal) (span 3 or 4)
                    if (instructionIndex + 2 < instructions.size() &&
                        first.opcode == QuickOpcode::LoadLocal &&
                        (instructions[instructionIndex + 1].opcode ==
                                QuickOpcode::ConstantInt ||
                            instructions[instructionIndex + 1].opcode ==
                                QuickOpcode::LoadLocal) &&
                        isBinaryInt(instructions[instructionIndex + 2].opcode)) {
                        const auto& second = instructions[instructionIndex + 1];
                        const auto& binary = instructions[instructionIndex + 2];
                        if (first.result != second.result &&
                            binary.op0 == first.result &&
                            binary.op1 == second.result) {
                            const auto constant =
                                second.opcode == QuickOpcode::ConstantInt;
                            const auto storesResult =
                                instructionIndex + 3 < instructions.size() &&
                                instructions[instructionIndex + 3].opcode ==
                                    QuickOpcode::StoreLocal &&
                                instructions[instructionIndex + 3].op0 ==
                                    binary.result;
                            first.superOpcode = storesResult
                                ? (constant
                                    ? SuperOpcode::BinaryLocalIntConstantStoreLocal
                                    : SuperOpcode::BinaryLocalIntLocalStoreLocal)
                                : (constant
                                    ? SuperOpcode::BinaryLocalIntConstant
                                    : SuperOpcode::BinaryLocalIntLocal);
                            first.span = storesResult ? 4 : 3;
                            first.binaryOpcode1 = binary.opcode;
                            first.checkedArithmetic1 = binary.checkedArithmetic1;
                            first.op1 = second.auxReg1; // right local index (if LoadLocal)
                            first.integerImmediate = second.integerImmediate; // right const (if ConstantInt)
                            first.auxReg2 = second.result; // second temp reg
                            first.superResult = binary.result; // binary dest reg
                            if (storesResult) {
                                first.storeSlot = instructions[instructionIndex + 3].auxReg1;
                            }
                            instructionIndex += first.span;
                            continue;
                        }
                    }
                    // Pattern 3: LoadLocal + BinaryInt(r_loaded, r_reg) + BinaryInt(r_mid, r_reg2) + StoreLocal (span 4, e.g. ai_tick)
                    if (instructionIndex + 3 < instructions.size() &&
                        first.opcode == QuickOpcode::LoadLocal &&
                        isBinaryInt(instructions[instructionIndex + 1].opcode) &&
                        isBinaryInt(instructions[instructionIndex + 2].opcode) &&
                        instructions[instructionIndex + 3].opcode == QuickOpcode::StoreLocal) {
                        const auto& bin1 = instructions[instructionIndex + 1];
                        const auto& bin2 = instructions[instructionIndex + 2];
                        const auto& store = instructions[instructionIndex + 3];
                        if (bin1.op0 == first.result &&
                            bin1.op1 != first.result &&
                            bin2.op0 == bin1.result &&
                            bin2.op1 != bin1.result &&
                            bin2.op1 != first.result &&
                            store.op0 == bin2.result) {
                            first.superOpcode = SuperOpcode::BinaryLocalIntRegChainStoreLocal;
                            first.span = 4;
                            first.binaryOpcode1 = bin1.opcode;
                            first.checkedArithmetic1 = bin1.checkedArithmetic1;
                            first.binaryOpcode2 = bin2.opcode;
                            first.checkedArithmetic2 = bin2.checkedArithmetic1;
                            first.op1 = bin1.op1;      // first rhs register
                            first.op2 = bin2.op1;      // second rhs register
                            first.auxReg2 = bin1.result;  // r_mid
                            first.superResult = bin2.result; // r_final
                            first.storeSlot = store.auxReg1; // target local index
                            instructionIndex += 4;
                            continue;
                        }
                    }
                    // Pattern 4: LoadLocal + BinaryInt(r_loaded, r_reg) (+ StoreLocal) (span 2 or 3, O2 LICM loop headers & bodies!)
                    if (instructionIndex + 1 < instructions.size() &&
                        first.opcode == QuickOpcode::LoadLocal &&
                        isBinaryInt(instructions[instructionIndex + 1].opcode)) {
                        const auto& binary = instructions[instructionIndex + 1];
                        if (binary.op0 == first.result &&
                            binary.op1 != first.result) {
                            const auto storesResult =
                                instructionIndex + 2 < instructions.size() &&
                                instructions[instructionIndex + 2].opcode ==
                                    QuickOpcode::StoreLocal &&
                                instructions[instructionIndex + 2].op0 ==
                                    binary.result;
                            first.superOpcode = storesResult
                                ? SuperOpcode::BinaryLocalIntRegStoreLocal
                                : SuperOpcode::BinaryLocalIntReg;
                            first.span = storesResult ? 3 : 2;
                            first.binaryOpcode1 = binary.opcode;
                            first.checkedArithmetic1 = binary.checkedArithmetic1;
                            first.op1 = binary.op1;       // rhs register index
                            first.superResult = binary.result; // binary result register
                            if (storesResult) {
                                first.storeSlot = instructions[instructionIndex + 2].auxReg1;
                            }
                            instructionIndex += first.span;
                            continue;
                        }
                    }
                    ++instructionIndex;
                }
                decoded.blockIndices.emplace(bb.id, index);
            }

            const auto entry = decoded.blockIndices.find(0);
            if (entry != decoded.blockIndices.end()) decoded.entry = entry->second;
            const auto bindEdge = [&](ExecutionEdge& edge,
                                      bytecode::BlockId target,
                                      const bytecode::SmallRegisterVector& sources) {
                const auto found = decoded.blockIndices.find(target);
                if (found == decoded.blockIndices.end()) return;
                edge.target = found->second;
                edge.targetBlockId = target;
                edge.sources = &sources;
                edge.destinations = &function.blocks[found->second].parameters;
                edge.hasArguments = !sources.empty();
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
            if (function.blocks.size() == 1 && function.exceptionHandlers.empty()) {
                const auto& bb = function.blocks[0];
                std::vector<int> localSourceParam(function.localTypes.size(), -1);
                std::vector<int> regSourceParam(function.registerTypes.size(), -1);
                const bytecode::FunctionReference* candidateRef = nullptr;
                bytecode::Register callResultReg = bytecode::InvalidRegister;
                bool validForwarder = !bb.instructions.empty();
                for (std::size_t instIdx = 0;
                     instIdx < bb.instructions.size() && validForwarder;
                     ++instIdx) {
                    const auto& inst = bb.instructions[instIdx];
                    switch (inst.opcode) {
                    case bytecode::Opcode::LoadParameter:
                        if (inst.result < regSourceParam.size() &&
                            inst.index < function.parameterTypes.size()) {
                            regSourceParam[inst.result] = static_cast<int>(inst.index);
                        } else {
                            validForwarder = false;
                        }
                        break;
                    case bytecode::Opcode::StoreLocal:
                        if (inst.index < localSourceParam.size() &&
                            inst.operands.size() == 1 &&
                            inst.operands[0] < regSourceParam.size() &&
                            regSourceParam[inst.operands[0]] >= 0) {
                            localSourceParam[inst.index] =
                                regSourceParam[inst.operands[0]];
                        } else {
                            validForwarder = false;
                        }
                        break;
                    case bytecode::Opcode::LoadLocal:
                        if (inst.result < regSourceParam.size() &&
                            inst.index < localSourceParam.size() &&
                            localSourceParam[inst.index] >= 0) {
                            regSourceParam[inst.result] =
                                localSourceParam[inst.index];
                        } else {
                            validForwarder = false;
                        }
                        break;
                    case bytecode::Opcode::Call:
                        if (instIdx + 1 == bb.instructions.size() &&
                            inst.index < module.functionReferences.size()) {
                            const auto& ref = module.functionReferences[inst.index];
                            if (!ref.virtualDispatch && !ref.interfaceDispatch &&
                                ref.name.rfind("__rs_native_", 0) == 0 &&
                                functions.find(ref.symbolId) == functions.end() &&
                                inst.operands.size() == function.parameterTypes.size() &&
                                ref.parameterTypes == function.parameterTypes &&
                                ref.parameterTypeIds == function.parameterTypeIds) {
                                bool allParamsMatch = true;
                                for (std::size_t k = 0; k < inst.operands.size(); ++k) {
                                    if (inst.operands[k] >= regSourceParam.size() ||
                                        regSourceParam[inst.operands[k]] !=
                                            static_cast<int>(k)) {
                                        allParamsMatch = false;
                                        break;
                                    }
                                }
                                if (allParamsMatch) {
                                    candidateRef = &ref;
                                    callResultReg = inst.result;
                                    break;
                                }
                            }
                        }
                        validForwarder = false;
                        break;
                    default:
                        validForwarder = false;
                        break;
                    }
                }
                if (validForwarder && candidateRef &&
                    candidateRef->name.rfind("$intrinsic::", 0) != 0) {
                    const bool returnMatches =
                        (function.returnType == semantic::PrimitiveType::Void &&
                         bb.terminator.kind == bytecode::TerminatorKind::ReturnVoid) ||
                        (function.returnType != semantic::PrimitiveType::Void &&
                         bb.terminator.kind == bytecode::TerminatorKind::ReturnValue &&
                         bb.terminator.value == callResultReg &&
                         candidateRef->returnType == function.returnType &&
                         candidateRef->returnTypeId == function.returnTypeId);
                    if (returnMatches) {
                        decoded->second.directNativeReference = candidateRef;
                        decoded->second.nativeForwarderInstructionCost =
                            static_cast<std::uint16_t>(bb.instructions.size() + 1u);
                    }
                }
            }
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
                        bool operandsValid =
                            instruction.bytecode->operands.size() ==
                                reference.parameterTypes.size() &&
                            !instruction.bytecode->operands.empty();
                        if (instruction.bytecode->result != bytecode::InvalidRegister &&
                            (instruction.bytecode->result >= function.registerTypes.size() ||
                             instruction.bytecode->result >= InvalidCompactReg)) {
                            operandsValid = false;
                        }
                        for (const auto op : instruction.bytecode->operands) {
                            if (op >= function.registerTypes.size() || op >= InvalidCompactReg) {
                                operandsValid = false;
                                break;
                            }
                        }
                        if (operandsValid &&
                            !(reference.virtualDispatch && reference.interfaceDispatch) &&
                            ((reference.virtualDispatch &&
                              reference.virtualSlot != std::numeric_limits<std::uint32_t>::max() &&
                              reference.parameterTypes.front() == semantic::PrimitiveType::Object) ||
                             (reference.interfaceDispatch &&
                              reference.interfaceTypeId != 0 &&
                              reference.interfaceSlot != std::numeric_limits<std::uint32_t>::max() &&
                              reference.parameterTypes.front() == semantic::PrimitiveType::Object &&
                              (reference.parameterTypeIds.empty() ? 0 : reference.parameterTypeIds.front()) ==
                                  reference.interfaceTypeId))) {
                            instruction.opcode = QuickOpcode::CallPolled;
                        }
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
                    bool callDirectRegsValid =
                        instruction.bytecode->result == bytecode::InvalidRegister ||
                        (instruction.bytecode->result < function.registerTypes.size() &&
                         instruction.bytecode->result < InvalidCompactReg);
                    for (const auto op : instruction.bytecode->operands) {
                        if (op >= function.registerTypes.size() || op >= InvalidCompactReg) {
                            callDirectRegsValid = false;
                            break;
                        }
                    }
                    if (!callDirectRegsValid) continue;
                    instruction.opcode = QuickOpcode::CallDirect;
                    instruction.directTarget = &target->second;
                    instruction.directExecution = findExecutionFunction(
                        *mirror, targetFunction);
                }
            }
        }
    }

    const auto isBuiltinMathModule = [](const bytecode::Module& mod) noexcept {
        return mod.name == "RealScript.Math" &&
            !mod.sourceFiles.empty() &&
            mod.sourceFiles[0].path == "$builtin/RealScript.Math.rscript";
    };

    const auto computeStraightLineCallMetrics = [](
        const auto& self,
        const ExecutionFunction* fn,
        std::uint32_t& outInstCost,
        std::uint16_t& outCallCount,
        std::uint16_t& outMaxDepth,
        int depth) noexcept -> bool {
        if (!fn || !fn->bytecode || fn->blocks.size() != 1 ||
            !fn->bytecode->exceptionHandlers.empty() || depth > 6) {
            return false;
        }
        const auto& blk = fn->blocks[0];
        if (blk.terminatorKind != bytecode::TerminatorKind::ReturnValue &&
            blk.terminatorKind != bytecode::TerminatorKind::ReturnVoid) {
            return false;
        }
        std::uint32_t instCost =
            static_cast<std::uint32_t>(blk.bytecode->instructions.size() + 1u);
        std::uint32_t callCount = 1u;
        std::uint16_t maxSubDepth = 0u;
        for (const auto& inst : blk.instructions) {
            if (!inst.bytecode) return false;
            const auto op = inst.bytecode->opcode;
            if (op == bytecode::Opcode::NewObject ||
                op == bytecode::Opcode::NewDelegate ||
                op == bytecode::Opcode::NewArray ||
                op == bytecode::Opcode::ConstantString) {
                return false;
            }
            if (op == bytecode::Opcode::Call) {
                if (inst.opcode != QuickOpcode::CallDirect || !inst.directExecution) {
                    return false;
                }
                std::uint32_t subInst = 0;
                std::uint16_t subCalls = 0;
                std::uint16_t subDepth = 0;
                if (!self(self, inst.directExecution, subInst, subCalls, subDepth, depth + 1)) {
                    return false;
                }
                instCost += subInst;
                callCount += subCalls;
                if (subDepth > maxSubDepth) maxSubDepth = subDepth;
            }
        }
        if (instCost >= InvalidCompactReg || callCount > 250u || (1u + maxSubDepth) > 250u) {
            return false;
        }
        outInstCost = instCost;
        outCallCount = static_cast<std::uint16_t>(callCount);
        outMaxDepth = static_cast<std::uint16_t>(1u + maxSubDepth);
        return true;
    };

    // Precompute block-derived instruction/call metrics for multi-block builtin RealScript.Math functions
    // (Fixed.Sin, Fixed.Cos, Math.Sqrt64, Fixed.Sqrt, Quat.Length, Quat.Normalized, Quat.FromEuler, Quat.Lerp)
    const auto findMathExecFn = [&](std::string_view name, std::size_t paramCount) noexcept -> const ExecutionFunction* {
        for (const auto& mod : modules) {
            if (!isBuiltinMathModule(mod)) continue;
            for (const auto& fn : mod.functions) {
                if (fn.name == name && fn.parameterTypes.size() == paramCount) {
                    const auto it = mirror->functions.find(&fn);
                    if (it != mirror->functions.end()) return &it->second;
                }
            }
        }
        return nullptr;
    };
    const auto bLen = [](const ExecutionFunction* fn, std::size_t idx) noexcept -> std::uint32_t {
        if (!fn || idx >= fn->blocks.size() || !fn->blocks[idx].bytecode) return 0u;
        return static_cast<std::uint32_t>(fn->blocks[idx].bytecode->instructions.size() + 1u);
    };

    std::uint32_t sinCalleeBaseCost = 0;
    std::uint16_t sinCalleeCalls = 0;
    std::uint32_t sinWrapExtraCost = 0;
    std::uint32_t cosCalleeBaseCost = 0;
    std::uint16_t cosCalleeCalls = 0;
    std::uint32_t sqrt64NegCalleeCost = 0;
    std::uint32_t sqrt64PosBaseCalleeCost = 0;
    std::uint32_t sqrt64LoopIterCost = 0;
    std::uint32_t fixedSqrtNegCalleeCost = 0;
    std::uint32_t fixedSqrtPosBaseCalleeCost = 0;
    std::uint32_t quatLenPosBaseCalleeCost = 0;
    std::uint16_t quatLenPosCalls = 0;
    std::uint32_t quatNormPosBaseCalleeCost = 0;
    std::uint16_t quatNormPosCalls = 0;
    std::uint32_t quatFromEulerBaseCalleeCost = 0;
    std::uint16_t quatFromEulerBaseCalls = 0;
    std::uint32_t quatLerpPosBaseCalleeCost = 0;
    std::uint16_t quatLerpPosBaseCalls = 0;
    std::uint32_t quatLerpDotNegExtraCost = 0;
    std::uint16_t quatLerpDotNegExtraCalls = 0;

    const auto accumBlockMetrics = [&](const ExecutionFunction* fn,
                                       std::size_t bIdx,
                                       std::uint32_t& ioInst,
                                       std::uint16_t& ioCalls) noexcept -> bool {
        if (!fn || bIdx >= fn->blocks.size() || !fn->blocks[bIdx].bytecode) return false;
        ioInst += static_cast<std::uint32_t>(fn->blocks[bIdx].bytecode->instructions.size() + 1u);
        for (const auto& inst : fn->blocks[bIdx].instructions) {
            if (!inst.bytecode || inst.bytecode->opcode != bytecode::Opcode::Call) continue;
            if (inst.opcode != QuickOpcode::CallDirect || !inst.directExecution ||
                !inst.directTarget || !inst.directTarget->function) {
                return false;
            }
            const auto& cName = inst.directTarget->function->name;
            if (cName == "Fixed.Sin" && sinCalleeBaseCost > 0) {
                ioInst += sinCalleeBaseCost;
                ioCalls = static_cast<std::uint16_t>(ioCalls + sinCalleeCalls);
            } else if (cName == "Fixed.Cos" && cosCalleeBaseCost > 0) {
                ioInst += cosCalleeBaseCost;
                ioCalls = static_cast<std::uint16_t>(ioCalls + cosCalleeCalls);
            } else if (cName == "Math.Sqrt64" && sqrt64PosBaseCalleeCost > 0) {
                ioInst += sqrt64PosBaseCalleeCost;
                ioCalls = static_cast<std::uint16_t>(ioCalls + 1u);
            } else if (cName == "Quat.Length" && quatLenPosBaseCalleeCost > 0) {
                ioInst += quatLenPosBaseCalleeCost;
                ioCalls = static_cast<std::uint16_t>(ioCalls + quatLenPosCalls);
            } else if (cName == "Quat.Normalized" && quatNormPosBaseCalleeCost > 0) {
                ioInst += quatNormPosBaseCalleeCost;
                ioCalls = static_cast<std::uint16_t>(ioCalls + quatNormPosCalls);
            } else {
                std::uint32_t subInst = 0;
                std::uint16_t subCalls = 0;
                std::uint16_t subDepth = 0;
                if (!computeStraightLineCallMetrics(
                        computeStraightLineCallMetrics,
                        inst.directExecution,
                        subInst,
                        subCalls,
                        subDepth,
                        0)) {
                    return false;
                }
                ioInst += subInst;
                ioCalls = static_cast<std::uint16_t>(ioCalls + subCalls);
            }
        }
        return true;
    };

    if (const auto* sinFn = findMathExecFn("Fixed.Sin", 1);
        sinFn && sinFn->blocks.size() == 11 &&
        bLen(sinFn, 1) == bLen(sinFn, 3) &&
        bLen(sinFn, 5) == bLen(sinFn, 6) + 1u &&
        bLen(sinFn, 8) == bLen(sinFn, 9) + 1u) {
        std::uint32_t cost = 0;
        std::uint16_t calls = 1;
        if (accumBlockMetrics(sinFn, 0, cost, calls) &&
            accumBlockMetrics(sinFn, 2, cost, calls) &&
            accumBlockMetrics(sinFn, 4, cost, calls) &&
            accumBlockMetrics(sinFn, 6, cost, calls) &&
            accumBlockMetrics(sinFn, 7, cost, calls) &&
            accumBlockMetrics(sinFn, 9, cost, calls) &&
            accumBlockMetrics(sinFn, 10, cost, calls)) {
            sinCalleeBaseCost = cost;
            sinCalleeCalls = calls;
            sinWrapExtraCost = bLen(sinFn, 1);
        }
    }
    if (const auto* cosFn = findMathExecFn("Fixed.Cos", 1);
        cosFn && cosFn->blocks.size() == 1 && sinCalleeBaseCost > 0) {
        std::uint32_t cost = 0;
        std::uint16_t calls = 1;
        if (accumBlockMetrics(cosFn, 0, cost, calls)) {
            cosCalleeBaseCost = cost;
            cosCalleeCalls = calls;
        }
    }
    if (const auto* sq64Fn = findMathExecFn("Math.Sqrt64", 1);
        sq64Fn && sq64Fn->blocks.size() == 12 &&
        bLen(sq64Fn, 3) == bLen(sq64Fn, 4) &&
        bLen(sq64Fn, 9) == bLen(sq64Fn, 10) + 2u) {
        sqrt64NegCalleeCost = bLen(sq64Fn, 0) + bLen(sq64Fn, 1);
        sqrt64PosBaseCalleeCost =
            bLen(sq64Fn, 0) + bLen(sq64Fn, 2) + bLen(sq64Fn, 3) +
            bLen(sq64Fn, 5) + bLen(sq64Fn, 6) + bLen(sq64Fn, 8);
        sqrt64LoopIterCost =
            bLen(sq64Fn, 6) + bLen(sq64Fn, 7) + bLen(sq64Fn, 10) + bLen(sq64Fn, 11);
    }
    if (const auto* fSqFn = findMathExecFn("Fixed.Sqrt", 1);
        fSqFn && fSqFn->blocks.size() == 3 && sqrt64PosBaseCalleeCost > 0) {
        std::uint32_t negCost = 0;
        std::uint16_t negCalls = 1;
        std::uint32_t posCost = 0;
        std::uint16_t posCalls = 1;
        if (accumBlockMetrics(fSqFn, 0, negCost, negCalls) &&
            accumBlockMetrics(fSqFn, 1, negCost, negCalls) &&
            accumBlockMetrics(fSqFn, 0, posCost, posCalls) &&
            accumBlockMetrics(fSqFn, 2, posCost, posCalls)) {
            fixedSqrtNegCalleeCost = negCost;
            fixedSqrtPosBaseCalleeCost = posCost;
        }
    }
    if (const auto* qLenFn = findMathExecFn("Quat.Length", 1);
        qLenFn && qLenFn->blocks.size() == 1 && sqrt64PosBaseCalleeCost > 0) {
        std::uint32_t cost = 0;
        std::uint16_t calls = 1;
        if (accumBlockMetrics(qLenFn, 0, cost, calls)) {
            quatLenPosBaseCalleeCost = cost;
            quatLenPosCalls = calls;
        }
    }
    if (const auto* qNormFn = findMathExecFn("Quat.Normalized", 1);
        qNormFn && qNormFn->blocks.size() == 3 && quatLenPosBaseCalleeCost > 0) {
        std::uint32_t cost = 0;
        std::uint16_t calls = 1;
        if (accumBlockMetrics(qNormFn, 0, cost, calls) &&
            accumBlockMetrics(qNormFn, 2, cost, calls)) {
            quatNormPosBaseCalleeCost = cost;
            quatNormPosCalls = calls;
        }
    }
    if (const auto* qEulerFn = findMathExecFn("Quat.FromEuler", 3);
        qEulerFn && qEulerFn->blocks.size() == 1 && sinCalleeBaseCost > 0 && cosCalleeBaseCost > 0) {
        std::uint32_t cost = 0;
        std::uint16_t calls = 1;
        if (accumBlockMetrics(qEulerFn, 0, cost, calls)) {
            quatFromEulerBaseCalleeCost = cost;
            quatFromEulerBaseCalls = calls;
        }
    }
    if (const auto* qLerpFn = findMathExecFn("Quat.Lerp", 3);
        qLerpFn && qLerpFn->blocks.size() == 3 && quatNormPosBaseCalleeCost > 0) {
        std::uint32_t cost = 0;
        std::uint16_t calls = 1;
        std::uint32_t negExtra = 0;
        std::uint16_t negCalls = 0;
        if (accumBlockMetrics(qLerpFn, 0, cost, calls) &&
            accumBlockMetrics(qLerpFn, 2, cost, calls) &&
            accumBlockMetrics(qLerpFn, 1, negExtra, negCalls)) {
            quatLerpPosBaseCalleeCost = cost;
            quatLerpPosBaseCalls = calls;
            quatLerpDotNegExtraCost = negExtra;
            quatLerpDotNegExtraCalls = negCalls;
        }
    }

    // Pre-build zero-malloc ObjectAllocationTemplates for concrete classes whose default fields are non-traceable
    std::unordered_map<semantic::SymbolId, std::uint16_t> objectTemplateByTypeId;
    for (const auto& module : modules) {
        for (const auto& type : module.types) {
            if (type.id == 0 || type.kind != semantic::TypeKind::Class ||
                type.interfaceType || type.delegateType ||
                objectTemplateByTypeId.find(type.id) != objectTemplateByTypeId.end() ||
                mirror->objectTemplates.size() >= InvalidCompactReg) {
                continue;
            }
            ObjectAllocationTemplate tmpl;
            tmpl.typeId = type.id;
            tmpl.defaultFields.reserve(type.fields.size());
            std::vector<std::size_t> refIndices;
            bool allInlineSafe = true;
            for (std::size_t fIdx = 0; fIdx < type.fields.size(); ++fIdx) {
                const auto fType = type.fields[fIdx].type;
                const auto fTid = exactTypeId(fType, type.fields[fIdx].typeName);
                Value defVal{};
                switch (fType) {
                case semantic::PrimitiveType::Bool: defVal = false; break;
                case semantic::PrimitiveType::Byte: defVal = ByteValue{}; break;
                case semantic::PrimitiveType::SByte: defVal = SByteValue{}; break;
                case semantic::PrimitiveType::Short: defVal = ShortValue{}; break;
                case semantic::PrimitiveType::UShort: defVal = UShortValue{}; break;
                case semantic::PrimitiveType::Int: defVal = std::int64_t{0}; break;
                case semantic::PrimitiveType::UInt: defVal = UIntValue{}; break;
                case semantic::PrimitiveType::Long: defVal = LongValue{}; break;
                case semantic::PrimitiveType::ULong: defVal = ULongValue{}; break;
                case semantic::PrimitiveType::Float: defVal = FloatValue{}; break;
                case semantic::PrimitiveType::Double: defVal = 0.0; break;
                case semantic::PrimitiveType::Char: defVal = CharValue{}; break;
                case semantic::PrimitiveType::String:
                    defVal = NullString{};
                    refIndices.push_back(fIdx);
                    break;
                case semantic::PrimitiveType::Object:
                    defVal = NullObject{};
                    refIndices.push_back(fIdx);
                    break;
                case semantic::PrimitiveType::Array:
                    defVal = NullArray{};
                    refIndices.push_back(fIdx);
                    break;
                case semantic::PrimitiveType::Enum:
                    defVal = EnumValue{fTid, 0};
                    break;
                case semantic::PrimitiveType::Handle: {
                    NativeHandle h{};
                    h.typeId = fTid;
                    defVal = h;
                    break;
                }
                case semantic::PrimitiveType::Struct:
                    if (const auto inl = makeDefaultInlineStructValue(fTid); inl.has_value()) {
                        defVal = *inl;
                    } else {
                        allInlineSafe = false;
                    }
                    break;
                default:
                    allInlineSafe = false;
                    break;
                }
                if (!allInlineSafe || defVal.isTraceable()) {
                    allInlineSafe = false;
                    break;
                }
                tmpl.defaultFields.push_back(defVal);
            }
            if (!allInlineSafe) continue;
            tmpl.referenceFields = std::make_shared<const std::vector<std::size_t>>(std::move(refIndices));
            const auto tIdx = static_cast<std::uint16_t>(mirror->objectTemplates.size());
            mirror->objectTemplates.push_back(std::move(tmpl));
            objectTemplateByTypeId.emplace(type.id, tIdx);
        }
    }

    const auto internFieldMeta = [&](semantic::SymbolId ownerTid, semantic::SymbolId fieldTid) -> std::uint32_t {
        for (std::size_t m = 0; m < mirror->fieldMetas.size(); ++m) {
            if (mirror->fieldMetas[m].ownerTypeId == ownerTid &&
                mirror->fieldMetas[m].fieldTypeId == fieldTid) {
                return static_cast<std::uint32_t>(m);
            }
        }
        const auto mIdx = static_cast<std::uint32_t>(mirror->fieldMetas.size());
        mirror->fieldMetas.push_back(FieldAccessMeta{ownerTid, fieldTid});
        return mIdx;
    };

    const auto classifyFastField = [&](const semantic::FieldSymbol& fld,
                                       semantic::SymbolId ownerTid,
                                       FastFieldKind& outKind,
                                       std::uint8_t& outExactTag,
                                       std::uint32_t& outMetaIdx) -> bool {
        outKind = FastFieldKind::None;
        outExactTag = 0u;
        outMetaIdx = 0u;
        switch (fld.type) {
        case semantic::PrimitiveType::Bool:
            outKind = FastFieldKind::ExactValueTag;
            outExactTag = static_cast<std::uint8_t>(ValueTag::Bool);
            return true;
        case semantic::PrimitiveType::Byte:
            outKind = FastFieldKind::ExactValueTag;
            outExactTag = static_cast<std::uint8_t>(ValueTag::Byte);
            return true;
        case semantic::PrimitiveType::SByte:
            outKind = FastFieldKind::ExactValueTag;
            outExactTag = static_cast<std::uint8_t>(ValueTag::SByte);
            return true;
        case semantic::PrimitiveType::Short:
            outKind = FastFieldKind::ExactValueTag;
            outExactTag = static_cast<std::uint8_t>(ValueTag::Short);
            return true;
        case semantic::PrimitiveType::UShort:
            outKind = FastFieldKind::ExactValueTag;
            outExactTag = static_cast<std::uint8_t>(ValueTag::UShort);
            return true;
        case semantic::PrimitiveType::Int:
            outKind = FastFieldKind::ExactValueTag;
            outExactTag = static_cast<std::uint8_t>(ValueTag::Int64);
            return true;
        case semantic::PrimitiveType::UInt:
            outKind = FastFieldKind::ExactValueTag;
            outExactTag = static_cast<std::uint8_t>(ValueTag::UInt);
            return true;
        case semantic::PrimitiveType::Long:
            outKind = FastFieldKind::ExactValueTag;
            outExactTag = static_cast<std::uint8_t>(ValueTag::Long);
            return true;
        case semantic::PrimitiveType::ULong:
            outKind = FastFieldKind::ExactValueTag;
            outExactTag = static_cast<std::uint8_t>(ValueTag::ULong);
            return true;
        case semantic::PrimitiveType::Float:
            outKind = FastFieldKind::ExactValueTag;
            outExactTag = static_cast<std::uint8_t>(ValueTag::Float);
            return true;
        case semantic::PrimitiveType::Double:
            outKind = FastFieldKind::ExactValueTag;
            outExactTag = static_cast<std::uint8_t>(ValueTag::Double);
            return true;
        case semantic::PrimitiveType::Char:
            outKind = FastFieldKind::ExactValueTag;
            outExactTag = static_cast<std::uint8_t>(ValueTag::Char);
            return true;
        case semantic::PrimitiveType::String:
            outKind = FastFieldKind::String;
            return true;
        case semantic::PrimitiveType::Object: {
            const auto fTid = exactTypeId(fld.type, fld.typeName);
            outKind = FastFieldKind::Object;
            outMetaIdx = internFieldMeta(ownerTid, fTid);
            return true;
        }
        case semantic::PrimitiveType::Array: {
            const auto fTid = exactTypeId(fld.type, fld.typeName);
            outKind = FastFieldKind::Array;
            outMetaIdx = internFieldMeta(ownerTid, fTid);
            return true;
        }
        case semantic::PrimitiveType::Enum: {
            const auto fTid = exactTypeId(fld.type, fld.typeName);
            if (fTid == 0) return false;
            outKind = FastFieldKind::Enum;
            outMetaIdx = internFieldMeta(ownerTid, fTid);
            return true;
        }
        case semantic::PrimitiveType::Struct: {
            const auto fTid = exactTypeId(fld.type, fld.typeName);
            if (fTid == 0) return false;
            if (fTid == fixedTypeId()) {
                outKind = FastFieldKind::ExactValueTag;
                outExactTag = static_cast<std::uint8_t>(ValueTag::Fixed);
                return true;
            }
            if (fTid == fixed16TypeId()) {
                outKind = FastFieldKind::ExactValueTag;
                outExactTag = static_cast<std::uint8_t>(ValueTag::Fixed16);
                return true;
            }
            if (fTid == vec2TypeId()) {
                outKind = FastFieldKind::ExactValueTag;
                outExactTag = static_cast<std::uint8_t>(ValueTag::Vec2);
                return true;
            }
            if (fTid == vec3TypeId()) {
                outKind = FastFieldKind::ExactValueTag;
                outExactTag = static_cast<std::uint8_t>(ValueTag::Vec3);
                return true;
            }
            if (fTid == quatTypeId()) {
                outKind = FastFieldKind::Quat;
                return true;
            }
            if (fTid == fixed64TypeId()) {
                outKind = FastFieldKind::ExactValueTag;
                outExactTag = static_cast<std::uint8_t>(ValueTag::Fixed64);
                return true;
            }
            if (fTid == vec2fTypeId()) {
                outKind = FastFieldKind::ExactValueTag;
                outExactTag = static_cast<std::uint8_t>(ValueTag::Vec2f);
                return true;
            }
            if (fTid == vec3fTypeId()) {
                outKind = FastFieldKind::ExactValueTag;
                outExactTag = static_cast<std::uint8_t>(ValueTag::Vec3f);
                return true;
            }
            if (fTid == quatfTypeId()) {
                outKind = FastFieldKind::Quatf;
                return true;
            }
            outKind = FastFieldKind::UserStruct;
            outMetaIdx = internFieldMeta(ownerTid, fTid);
            return true;
        }
        default:
            return false;
        }
    };

    // Pass 3: Build unified flat frame slot layout & direct-threaded FlatInstruction stream
    for (const auto& module : modules) {
        for (const auto& function : module.functions) {
            auto found = mirror->functions.find(&function);
            if (found == mirror->functions.end()) continue;
            auto& execFunc = found->second;
            execFunc.hasExceptionHandlers = !function.exceptionHandlers.empty();
            execFunc.isAsyncFunction = !function.blocks.empty() &&
                !function.blocks.front().instructions.empty() &&
                function.blocks.front().instructions.front().opcode == bytecode::Opcode::AsyncEnter;

            const std::size_t paramCount = function.parameterTypes.size();
            const std::size_t localCount = function.localTypes.size();
            const std::size_t regCount = function.registerTypes.size();
            const std::size_t totalSlots =
                paramCount + localCount + regCount + execFunc.edgeScratchCount;
            if (totalSlots >= InvalidCompactReg ||
                execFunc.entry == InvalidBlockIndex) {
                execFunc.canUseFlatFastPath = false;
                continue;
            }
            execFunc.paramOffset = 0;
            execFunc.localOffset = static_cast<std::uint16_t>(paramCount);
            execFunc.registerOffset = static_cast<std::uint16_t>(paramCount + localCount);
            execFunc.scratchOffset = static_cast<std::uint16_t>(paramCount + localCount + regCount);
            execFunc.totalFrameSlots = static_cast<std::uint32_t>(totalSlots);

            const auto paramSlot = [&](std::uint16_t idx) noexcept -> std::uint16_t {
                return idx < paramCount
                    ? static_cast<std::uint16_t>(execFunc.paramOffset + idx)
                    : InvalidCompactReg;
            };
            const auto localSlot = [&](std::uint16_t idx) noexcept -> std::uint16_t {
                return idx < localCount
                    ? static_cast<std::uint16_t>(execFunc.localOffset + idx)
                    : InvalidCompactReg;
            };
            const auto regSlot = [&](std::uint16_t idx) noexcept -> std::uint16_t {
                return idx < regCount
                    ? static_cast<std::uint16_t>(execFunc.registerOffset + idx)
                    : InvalidCompactReg;
            };

            // Compute global cross-block register read counts to safely elide dead SSA temporaries
            std::vector<std::uint32_t> globalRegReads(regCount, 0);
            for (const auto& bb : function.blocks) {
                for (const auto& inst : bb.instructions) {
                    for (const auto op : inst.operands) {
                        if (op < regCount) ++globalRegReads[op];
                    }
                }
                if (bb.terminator.kind == bytecode::TerminatorKind::Branch &&
                    bb.terminator.condition < regCount) {
                    ++globalRegReads[bb.terminator.condition];
                } else if ((bb.terminator.kind == bytecode::TerminatorKind::ReturnValue ||
                            bb.terminator.kind == bytecode::TerminatorKind::Throw) &&
                           bb.terminator.value < regCount) {
                    ++globalRegReads[bb.terminator.value];
                }
                for (const auto arg : bb.terminator.arguments) {
                    if (arg < regCount) ++globalRegReads[arg];
                }
                for (const auto arg : bb.terminator.falseArguments) {
                    if (arg < regCount) ++globalRegReads[arg];
                }
            }

            struct FlatBuildItem {
                FlatInstruction hot{};
                FlatColdMeta cold{};
            };
            execFunc.blockStreamOffsets.assign(execFunc.blocks.size(), InvalidStreamOffset);
            std::size_t estimatedStreamLen = 0;
            for (const auto& blk : execFunc.blocks) {
                estimatedStreamLen += blk.instructions.size() + 1;
            }
            std::vector<FlatBuildItem> buildStream;
            buildStream.reserve(estimatedStreamLen);

            for (std::size_t bIdx = 0; bIdx < execFunc.blocks.size(); ++bIdx) {
                const auto& blk = execFunc.blocks[bIdx];
                const std::size_t blockStreamStart = buildStream.size();
                execFunc.blockStreamOffsets[bIdx] =
                    static_cast<std::uint32_t>(blockStreamStart);

                const std::size_t nInst = blk.instructions.size();
                for (std::size_t i = 0; i < nInst;) {
                    const auto& inst = blk.instructions[i];
                    FlatInstruction flat;
                    FlatColdMeta cold;
                    cold.bytecode = inst.bytecode;
                    cold.directTarget = inst.directTarget;
                    cold.directExecution = inst.directExecution;
                    cold.blockIndex = static_cast<std::uint32_t>(bIdx);
                    cold.instructionIndexInBlock = static_cast<std::uint32_t>(i);
                    flat.integerImmediate = inst.integerImmediate;
                    flat.binaryOpcode1 = inst.binaryOpcode1;
                    flat.binaryOpcode2 = inst.binaryOpcode2;
                    flat.span = inst.span;
                    flat.setChecked1(inst.checkedArithmetic1);
                    flat.setChecked2(inst.checkedArithmetic2);

                    // Check for loop-header branch fusion at the end of a block:
                    // SuperOpcode::BinaryLocalIntReg (with LessInt) or BinaryLocalIntConstant (with LessInt)
                    // at the very last instruction group of the block, followed by a no-arg Branch on superResult!
                    const bool isAtBlockEnd = (i + inst.span == nInst);
                    const bool canFuseTermBranch =
                        isAtBlockEnd &&
                        blk.terminatorKind == bytecode::TerminatorKind::Branch &&
                        blk.conditionPrevalidatedBool &&
                        !blk.branch.hasArguments &&
                        !blk.falseBranch.hasArguments &&
                        blk.branch.target != InvalidBlockIndex &&
                        blk.falseBranch.target != InvalidBlockIndex;

                    const bool isIntComparison =
                        inst.binaryOpcode1 >= QuickOpcode::LessInt &&
                        inst.binaryOpcode1 <= QuickOpcode::NotEqualInt;
                    if (canFuseTermBranch &&
                        inst.superOpcode == SuperOpcode::BinaryLocalIntReg &&
                        isIntComparison &&
                        blk.terminatorReg == inst.superResult &&
                        inst.result < regCount &&
                        inst.superResult < regCount &&
                        globalRegReads[inst.result] == 1 &&
                        globalRegReads[inst.superResult] == 1) {
                        flat.flatOpcode = FlatOpcode::SuperLessLocalRegBranch;
                        flat.span = static_cast<std::uint8_t>(inst.span + 1); // 2 insts + 1 branch terminator = 3
                        flat.src0Slot = localSlot(inst.auxReg1);
                        flat.src1Slot = regSlot(inst.op1);
                        flat.trueTargetOffset = InvalidStreamOffset;
                        flat.falseTargetOffset = InvalidStreamOffset;
                        buildStream.push_back({flat, cold});
                        i += inst.span;
                        // Skip emitting a separate terminator instruction since it is fused!
                        goto next_block;
                    }
                    if (canFuseTermBranch &&
                        inst.superOpcode == SuperOpcode::BinaryLocalIntConstant &&
                        isIntComparison &&
                        blk.terminatorReg == inst.superResult &&
                        inst.result < regCount &&
                        inst.auxReg2 < regCount &&
                        inst.superResult < regCount &&
                        globalRegReads[inst.result] == 1 &&
                        globalRegReads[inst.auxReg2] == 1 &&
                        globalRegReads[inst.superResult] == 1) {
                        flat.flatOpcode = FlatOpcode::SuperLessLocalConstBranch;
                        flat.span = static_cast<std::uint8_t>(inst.span + 1); // 3 insts + 1 branch terminator = 4
                        flat.src0Slot = localSlot(inst.auxReg1);
                        flat.trueTargetOffset = InvalidStreamOffset;
                        flat.falseTargetOffset = InvalidStreamOffset;
                        buildStream.push_back({flat, cold});
                        i += inst.span;
                        goto next_block;
                    }
                    // Loop header fusion for `i < arr.Length` (5-inst or 4-inst with hoisted arr reg):
                    if (blk.terminatorKind == bytecode::TerminatorKind::Branch &&
                        blk.conditionPrevalidatedBool &&
                        !blk.branch.hasArguments &&
                        !blk.falseBranch.hasArguments &&
                        blk.branch.target != InvalidBlockIndex &&
                        blk.falseBranch.target != InvalidBlockIndex &&
                        (inst.opcode == QuickOpcode::LoadLocal || inst.opcode == QuickOpcode::LoadParameter) &&
                        inst.superOpcode == SuperOpcode::None &&
                        inst.result < regCount &&
                        globalRegReads[inst.result] == 1) {
                        const std::uint16_t lhsSlot = (inst.opcode == QuickOpcode::LoadLocal)
                            ? localSlot(inst.auxReg1)
                            : paramSlot(inst.auxReg1);
                        if (i + 4 == nInst) {
                            const auto& arrLoad = blk.instructions[i + 1];
                            const auto& arrLen = blk.instructions[i + 2];
                            const auto& cmpInst = blk.instructions[i + 3];
                            const bool validArrLoad =
                                (arrLoad.opcode == QuickOpcode::LoadLocal ||
                                 arrLoad.opcode == QuickOpcode::LoadParameter) &&
                                arrLoad.superOpcode == SuperOpcode::None &&
                                arrLoad.result < regCount &&
                                globalRegReads[arrLoad.result] == 1;
                            const bool validCmp =
                                cmpInst.opcode >= QuickOpcode::LessInt &&
                                cmpInst.opcode <= QuickOpcode::NotEqualInt &&
                                cmpInst.superOpcode == SuperOpcode::None &&
                                cmpInst.op0 == inst.result &&
                                cmpInst.op1 == arrLen.result &&
                                cmpInst.result < regCount &&
                                globalRegReads[cmpInst.result] == 1 &&
                                blk.terminatorReg == cmpInst.result;
                            if (validArrLoad &&
                                arrLen.opcode == QuickOpcode::ArrayLengthFast &&
                                arrLen.op0 == arrLoad.result &&
                                arrLen.result < regCount &&
                                globalRegReads[arrLen.result] == 1 &&
                                validCmp) {
                                flat.flatOpcode = FlatOpcode::SuperLessLocalArrayLengthBranch;
                                flat.span = 5;
                                flat.binaryOpcode1 = cmpInst.opcode;
                                flat.src0Slot = lhsSlot;
                                flat.src1Slot = (arrLoad.opcode == QuickOpcode::LoadLocal)
                                    ? localSlot(arrLoad.auxReg1)
                                    : paramSlot(arrLoad.auxReg1);
                                flat.trueTargetOffset = InvalidStreamOffset;
                                flat.falseTargetOffset = InvalidStreamOffset;
                                buildStream.push_back({flat, cold});
                                i += 4;
                                goto next_block;
                            }
                        } else if (i + 3 == nInst) {
                            const auto& arrLen = blk.instructions[i + 1];
                            const auto& cmpInst = blk.instructions[i + 2];
                            const bool validCmp =
                                cmpInst.opcode >= QuickOpcode::LessInt &&
                                cmpInst.opcode <= QuickOpcode::NotEqualInt &&
                                cmpInst.superOpcode == SuperOpcode::None &&
                                cmpInst.op0 == inst.result &&
                                cmpInst.op1 == arrLen.result &&
                                cmpInst.result < regCount &&
                                globalRegReads[cmpInst.result] == 1 &&
                                blk.terminatorReg == cmpInst.result;
                            if (arrLen.opcode == QuickOpcode::ArrayLengthFast &&
                                arrLen.op0 < regCount &&
                                arrLen.result < regCount &&
                                globalRegReads[arrLen.result] == 1 &&
                                validCmp) {
                                flat.flatOpcode = FlatOpcode::SuperLessLocalArrayLengthBranch;
                                flat.span = 4;
                                flat.binaryOpcode1 = cmpInst.opcode;
                                flat.src0Slot = lhsSlot;
                                flat.src1Slot = regSlot(arrLen.op0);
                                flat.trueTargetOffset = InvalidStreamOffset;
                                flat.falseTargetOffset = InvalidStreamOffset;
                                buildStream.push_back({flat, cold});
                                i += 3;
                                goto next_block;
                            }
                        }
                    }

                    if (inst.superOpcode != SuperOpcode::None) {
                        switch (inst.superOpcode) {
                        case SuperOpcode::LoadParameterStoreLocal:
                            flat.flatOpcode = (inst.result < regCount && globalRegReads[inst.result] == 1)
                                ? FlatOpcode::SuperMoveParamStoreLocalElided
                                : FlatOpcode::SuperMoveParamStoreLocal;
                            flat.src0Slot = paramSlot(inst.auxReg1);
                            flat.dstSlot = regSlot(inst.result);
                            flat.storeSlot = localSlot(inst.storeSlot);
                            break;
                        case SuperOpcode::BinaryLocalIntConstant:
                            flat.flatOpcode = FlatOpcode::SuperBinaryLocalIntConst;
                            flat.src0Slot = localSlot(inst.auxReg1);
                            flat.dstSlot = regSlot(inst.result);
                            flat.aux0Slot = regSlot(inst.auxReg2);
                            flat.aux1Slot = regSlot(inst.superResult);
                            break;
                        case SuperOpcode::BinaryLocalIntConstantStoreLocal: {
                            const bool tempsDead =
                                inst.result < regCount &&
                                inst.auxReg2 < regCount &&
                                inst.superResult < regCount &&
                                globalRegReads[inst.result] == 1 &&
                                globalRegReads[inst.auxReg2] == 1 &&
                                globalRegReads[inst.superResult] == 1;
                            flat.flatOpcode = tempsDead
                                ? FlatOpcode::SuperBinaryLocalIntConstStoreElided
                                : FlatOpcode::SuperBinaryLocalIntConstStore;
                            flat.src0Slot = localSlot(inst.auxReg1);
                            flat.dstSlot = regSlot(inst.result);
                            flat.aux0Slot = regSlot(inst.auxReg2);
                            flat.aux1Slot = regSlot(inst.superResult);
                            flat.storeSlot = localSlot(inst.storeSlot);
                            break;
                        }
                        case SuperOpcode::BinaryLocalIntLocal:
                            flat.flatOpcode = FlatOpcode::SuperBinaryLocalIntLocal;
                            flat.src0Slot = localSlot(inst.auxReg1);
                            flat.src1Slot = localSlot(inst.op1);
                            flat.dstSlot = regSlot(inst.result);
                            flat.aux0Slot = regSlot(inst.auxReg2);
                            flat.aux1Slot = regSlot(inst.superResult);
                            break;
                        case SuperOpcode::BinaryLocalIntLocalStoreLocal: {
                            const bool tempsDead =
                                inst.result < regCount &&
                                inst.auxReg2 < regCount &&
                                inst.superResult < regCount &&
                                globalRegReads[inst.result] == 1 &&
                                globalRegReads[inst.auxReg2] == 1 &&
                                globalRegReads[inst.superResult] == 1;
                            flat.flatOpcode = tempsDead
                                ? FlatOpcode::SuperBinaryLocalIntLocalStoreElided
                                : FlatOpcode::SuperBinaryLocalIntLocalStore;
                            flat.src0Slot = localSlot(inst.auxReg1);
                            flat.src1Slot = localSlot(inst.op1);
                            flat.dstSlot = regSlot(inst.result);
                            flat.aux0Slot = regSlot(inst.auxReg2);
                            flat.aux1Slot = regSlot(inst.superResult);
                            flat.storeSlot = localSlot(inst.storeSlot);
                            break;
                        }
                        case SuperOpcode::BinaryLocalIntReg:
                            flat.flatOpcode = FlatOpcode::SuperBinaryLocalIntReg;
                            flat.src0Slot = localSlot(inst.auxReg1);
                            flat.src1Slot = regSlot(inst.op1);
                            flat.dstSlot = regSlot(inst.result);
                            flat.aux1Slot = regSlot(inst.superResult);
                            break;
                        case SuperOpcode::BinaryLocalIntRegStoreLocal: {
                            const bool tempsDead =
                                inst.result < regCount &&
                                inst.superResult < regCount &&
                                globalRegReads[inst.result] == 1 &&
                                globalRegReads[inst.superResult] == 1;
                            flat.flatOpcode = tempsDead
                                ? FlatOpcode::SuperBinaryLocalIntRegStoreElided
                                : FlatOpcode::SuperBinaryLocalIntRegStore;
                            flat.src0Slot = localSlot(inst.auxReg1);
                            flat.src1Slot = regSlot(inst.op1);
                            flat.dstSlot = regSlot(inst.result);
                            flat.aux1Slot = regSlot(inst.superResult);
                            flat.storeSlot = localSlot(inst.storeSlot);
                            break;
                        }
                        case SuperOpcode::BinaryLocalIntRegChainStoreLocal: {
                            const bool tempsDead =
                                inst.result < regCount &&
                                inst.auxReg2 < regCount &&
                                inst.superResult < regCount &&
                                globalRegReads[inst.result] == 1 &&
                                globalRegReads[inst.auxReg2] == 1 &&
                                globalRegReads[inst.superResult] == 1;
                            flat.flatOpcode = tempsDead
                                ? FlatOpcode::SuperBinaryLocalIntRegChainStoreElided
                                : FlatOpcode::SuperBinaryLocalIntRegChainStore;
                            flat.src0Slot = localSlot(inst.auxReg1);
                            flat.src1Slot = regSlot(inst.op1);
                            flat.src2Slot = regSlot(inst.op2);
                            flat.dstSlot = regSlot(inst.result);
                            flat.aux0Slot = regSlot(inst.auxReg2);
                            flat.aux1Slot = regSlot(inst.superResult);
                            flat.storeSlot = localSlot(inst.storeSlot);
                            break;
                        }
                        default:
                            break;
                        }
                        buildStream.push_back({flat, cold});
                        i += inst.span;
                        continue;
                    }

                    // Pass 3 fusion for 4-instruction double local arithmetic:
                    // LoadLocal(double) + (ConstantDouble | LoadLocal(double)) + (AddDouble..DivideDouble) + StoreLocal(double)
                    if (i + 3 < nInst &&
                        inst.opcode == QuickOpcode::LoadLocal &&
                        inst.result < regCount &&
                        function.registerTypes[inst.result] == semantic::PrimitiveType::Double) {
                        const auto& second = blk.instructions[i + 1];
                        const auto& binary = blk.instructions[i + 2];
                        const auto& store = blk.instructions[i + 3];
                        const bool isSecondConst = (second.opcode == QuickOpcode::ConstantDouble);
                        const bool isSecondLocal = (second.opcode == QuickOpcode::LoadLocal &&
                            second.result < regCount &&
                            function.registerTypes[second.result] == semantic::PrimitiveType::Double);
                        const bool isDoubleArith =
                            binary.opcode >= QuickOpcode::AddDouble &&
                            binary.opcode <= QuickOpcode::DivideDouble;
                        if ((isSecondConst || isSecondLocal) &&
                            isDoubleArith &&
                            store.opcode == QuickOpcode::StoreLocal &&
                            inst.result != second.result &&
                            binary.op0 == inst.result &&
                            binary.op1 == second.result &&
                            store.op0 == binary.result &&
                            second.result < regCount &&
                            binary.result < regCount) {
                            const bool tempsDead =
                                globalRegReads[inst.result] == 1 &&
                                globalRegReads[second.result] == 1 &&
                                globalRegReads[binary.result] == 1;
                            flat.span = 4;
                            flat.binaryOpcode1 = binary.opcode;
                            flat.src0Slot = localSlot(inst.auxReg1);
                            flat.dstSlot = regSlot(inst.result);
                            flat.aux0Slot = regSlot(second.result);
                            flat.aux1Slot = regSlot(binary.result);
                            flat.storeSlot = localSlot(store.auxReg1);
                            if (isSecondConst) {
                                const double dImm = second.bytecode->activeDoubleImmediate();
                                std::memcpy(&flat.integerImmediate, &dImm, sizeof(double));
                                flat.flatOpcode = tempsDead
                                    ? FlatOpcode::SuperBinaryLocalDoubleConstStoreElided
                                    : FlatOpcode::SuperBinaryLocalDoubleConstStore;
                            } else {
                                flat.src1Slot = localSlot(second.auxReg1);
                                flat.flatOpcode = tempsDead
                                    ? FlatOpcode::SuperBinaryLocalDoubleLocalStoreElided
                                    : FlatOpcode::SuperBinaryLocalDoubleLocalStore;
                            }
                            buildStream.push_back({flat, cold});
                            i += 4;
                            continue;
                        }
                    }

                    // Pass 3 fusion for float constant literal:
                    // ConstantDouble + ConvertDoubleToFloatFast (+ optional StoreLocal(float))
                    if (i + 1 < nInst &&
                        inst.opcode == QuickOpcode::ConstantDouble &&
                        inst.result < regCount &&
                        globalRegReads[inst.result] == 1 &&
                        blk.instructions[i + 1].opcode == QuickOpcode::ConvertDoubleToFloatFast &&
                        blk.instructions[i + 1].op0 == inst.result &&
                        blk.instructions[i + 1].result < regCount) {
                        const double dImm = inst.bytecode->activeDoubleImmediate();
                        if (dImm >= -static_cast<double>(std::numeric_limits<float>::max()) &&
                            dImm <= static_cast<double>(std::numeric_limits<float>::max())) {
                            const auto& conv = blk.instructions[i + 1];
                            const float fImm = static_cast<float>(dImm);
                            std::memcpy(&flat.integerImmediate, &fImm, sizeof(float));
                            if (i + 2 < nInst &&
                                blk.instructions[i + 2].opcode == QuickOpcode::StoreLocal &&
                                blk.instructions[i + 2].op0 == conv.result &&
                                globalRegReads[conv.result] == 1) {
                                flat.flatOpcode = FlatOpcode::SuperConstFloatStoreLocal;
                                flat.span = 3;
                                flat.dstSlot = regSlot(conv.result);
                                flat.storeSlot = localSlot(blk.instructions[i + 2].auxReg1);
                                buildStream.push_back({flat, cold});
                                i += 3;
                                continue;
                            }
                            flat.flatOpcode = FlatOpcode::SuperConstFloat;
                            flat.span = 2;
                            flat.dstSlot = regSlot(conv.result);
                            buildStream.push_back({flat, cold});
                            i += 2;
                            continue;
                        }
                    }

                    // Pass 3 fusion for 4-instruction float local arithmetic:
                    // LoadLocal(float) + LoadLocal(float) + (AddFloat..DivideFloat) + StoreLocal(float)
                    if (i + 3 < nInst &&
                        inst.opcode == QuickOpcode::LoadLocal &&
                        inst.result < regCount &&
                        function.registerTypes[inst.result] == semantic::PrimitiveType::Float) {
                        const auto& second = blk.instructions[i + 1];
                        const auto& binary = blk.instructions[i + 2];
                        const auto& store = blk.instructions[i + 3];
                        const bool isSecondLocal = (second.opcode == QuickOpcode::LoadLocal &&
                            second.result < regCount &&
                            function.registerTypes[second.result] == semantic::PrimitiveType::Float);
                        const bool isFloatArith =
                            binary.opcode >= QuickOpcode::AddFloat &&
                            binary.opcode <= QuickOpcode::DivideFloat;
                        if (isSecondLocal &&
                            isFloatArith &&
                            store.opcode == QuickOpcode::StoreLocal &&
                            inst.result != second.result &&
                            binary.op0 == inst.result &&
                            binary.op1 == second.result &&
                            store.op0 == binary.result &&
                            binary.result < regCount) {
                            const bool tempsDead =
                                globalRegReads[inst.result] == 1 &&
                                globalRegReads[second.result] == 1 &&
                                globalRegReads[binary.result] == 1;
                            flat.span = 4;
                            flat.binaryOpcode1 = binary.opcode;
                            flat.src0Slot = localSlot(inst.auxReg1);
                            flat.src1Slot = localSlot(second.auxReg1);
                            flat.dstSlot = regSlot(inst.result);
                            flat.aux0Slot = regSlot(second.result);
                            flat.aux1Slot = regSlot(binary.result);
                            flat.storeSlot = localSlot(store.auxReg1);
                            flat.flatOpcode = tempsDead
                                ? FlatOpcode::SuperBinaryLocalFloatLocalStoreElided
                                : FlatOpcode::SuperBinaryLocalFloatLocalStore;
                            buildStream.push_back({flat, cold});
                            i += 4;
                            continue;
                        }
                    }

                    // Pass 3 fusion for 2/3-instruction float local-reg arithmetic:
                    // LoadLocal(float) + (AddFloat..DivideFloat)(r_loaded, r_reg) [+ StoreLocal(float)]
                    if (i + 1 < nInst &&
                        inst.opcode == QuickOpcode::LoadLocal &&
                        inst.result < regCount &&
                        function.registerTypes[inst.result] == semantic::PrimitiveType::Float) {
                        const auto& binary = blk.instructions[i + 1];
                        const bool isFloatArith =
                            binary.opcode >= QuickOpcode::AddFloat &&
                            binary.opcode <= QuickOpcode::DivideFloat;
                        if (isFloatArith &&
                            binary.op0 == inst.result &&
                            binary.op1 != inst.result &&
                            binary.result < regCount) {
                            const bool storesResult =
                                i + 2 < nInst &&
                                blk.instructions[i + 2].opcode == QuickOpcode::StoreLocal &&
                                blk.instructions[i + 2].op0 == binary.result;
                            flat.binaryOpcode1 = binary.opcode;
                            flat.src0Slot = localSlot(inst.auxReg1);
                            flat.src1Slot = regSlot(binary.op1);
                            flat.dstSlot = regSlot(inst.result);
                            flat.aux1Slot = regSlot(binary.result);
                            if (storesResult) {
                                const bool tempsDead =
                                    globalRegReads[inst.result] == 1 &&
                                    globalRegReads[binary.result] == 1;
                                flat.span = 3;
                                flat.storeSlot = localSlot(blk.instructions[i + 2].auxReg1);
                                flat.flatOpcode = tempsDead
                                    ? FlatOpcode::SuperBinaryLocalFloatRegStoreElided
                                    : FlatOpcode::SuperBinaryLocalFloatRegStore;
                                buildStream.push_back({flat, cold});
                                i += 3;
                                continue;
                            } else {
                                const bool tempDead = (globalRegReads[inst.result] == 1);
                                flat.span = 2;
                                flat.flatOpcode = tempDead
                                    ? FlatOpcode::SuperBinaryLocalFloatRegElided
                                    : FlatOpcode::SuperBinaryLocalFloatReg;
                                buildStream.push_back({flat, cold});
                                i += 2;
                                continue;
                            }
                        }
                    }

                    // Pass 3 fusion for 2-instruction float reg-reg arithmetic + store:
                    // (AddFloat..DivideFloat)(r_a, r_b) + StoreLocal(float) with dead temp
                    if (i + 1 < nInst &&
                        inst.opcode >= QuickOpcode::AddFloat &&
                        inst.opcode <= QuickOpcode::DivideFloat &&
                        inst.result < regCount &&
                        globalRegReads[inst.result] == 1 &&
                        blk.instructions[i + 1].opcode == QuickOpcode::StoreLocal &&
                        blk.instructions[i + 1].op0 == inst.result) {
                        flat.flatOpcode = FlatOpcode::SuperBinaryRegFloatStoreElided;
                        flat.span = 2;
                        flat.binaryOpcode1 = inst.opcode;
                        flat.src0Slot = regSlot(inst.op0);
                        flat.src1Slot = regSlot(inst.op1);
                        flat.dstSlot = regSlot(inst.result);
                        flat.storeSlot = localSlot(blk.instructions[i + 1].auxReg1);
                        buildStream.push_back({flat, cold});
                        i += 2;
                        continue;
                    }

                    // Single instruction mapping
                    flat.span = 1;
                    switch (inst.opcode) {
                    case QuickOpcode::LoadParameter:
                        flat.flatOpcode = FlatOpcode::MoveSlot;
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = paramSlot(inst.auxReg1);
                        break;
                    case QuickOpcode::LoadLocal:
                        flat.flatOpcode = FlatOpcode::MoveSlot;
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = localSlot(inst.auxReg1);
                        break;
                    case QuickOpcode::StoreLocal:
                        flat.flatOpcode = FlatOpcode::MoveSlot;
                        flat.dstSlot = localSlot(inst.auxReg1);
                        flat.src0Slot = regSlot(inst.op0);
                        break;
                    case QuickOpcode::ConstantInt:
                        flat.flatOpcode = FlatOpcode::ConstantInt;
                        flat.dstSlot = regSlot(inst.result);
                        break;
                    case QuickOpcode::AddInt:
                        flat.flatOpcode = inst.checkedArithmetic1
                            ? FlatOpcode::AddIntChecked
                            : FlatOpcode::AddIntUnchecked;
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        flat.src1Slot = regSlot(inst.op1);
                        break;
                    case QuickOpcode::SubtractInt:
                        flat.flatOpcode = inst.checkedArithmetic1
                            ? FlatOpcode::SubIntChecked
                            : FlatOpcode::SubIntUnchecked;
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        flat.src1Slot = regSlot(inst.op1);
                        break;
                    case QuickOpcode::MultiplyInt:
                        flat.flatOpcode = inst.checkedArithmetic1
                            ? FlatOpcode::MulIntChecked
                            : FlatOpcode::MulIntUnchecked;
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        flat.src1Slot = regSlot(inst.op1);
                        break;
                    case QuickOpcode::DivideInt:
                        flat.flatOpcode = FlatOpcode::DivInt;
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        flat.src1Slot = regSlot(inst.op1);
                        break;
                    case QuickOpcode::RemainderInt:
                        flat.flatOpcode = FlatOpcode::RemInt;
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        flat.src1Slot = regSlot(inst.op1);
                        break;
                    case QuickOpcode::LessInt:
                        flat.flatOpcode = FlatOpcode::LessInt;
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        flat.src1Slot = regSlot(inst.op1);
                        break;
                    case QuickOpcode::LessOrEqualInt:
                        flat.flatOpcode = FlatOpcode::LessOrEqualInt;
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        flat.src1Slot = regSlot(inst.op1);
                        break;
                    case QuickOpcode::GreaterInt:
                        flat.flatOpcode = FlatOpcode::GreaterInt;
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        flat.src1Slot = regSlot(inst.op1);
                        break;
                    case QuickOpcode::GreaterOrEqualInt:
                        flat.flatOpcode = FlatOpcode::GreaterOrEqualInt;
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        flat.src1Slot = regSlot(inst.op1);
                        break;
                    case QuickOpcode::EqualInt:
                        flat.flatOpcode = FlatOpcode::EqualInt;
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        flat.src1Slot = regSlot(inst.op1);
                        break;
                    case QuickOpcode::NotEqualInt:
                        flat.flatOpcode = FlatOpcode::NotEqualInt;
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        flat.src1Slot = regSlot(inst.op1);
                        break;
                    case QuickOpcode::CheckNotNullFast:
                        flat.flatOpcode = FlatOpcode::CheckNotNullFast;
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        break;
                    case QuickOpcode::LoadElementIntFast:
                        flat.flatOpcode = FlatOpcode::LoadElementIntFast;
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        flat.src1Slot = regSlot(inst.op1);
                        break;
                    case QuickOpcode::StoreElementIntFast:
                        flat.flatOpcode = FlatOpcode::StoreElementIntFast;
                        flat.src0Slot = regSlot(inst.op0);
                        flat.src1Slot = regSlot(inst.op1);
                        flat.src2Slot = regSlot(inst.op2);
                        break;
                    case QuickOpcode::CallDirect: {
                        flat.dstSlot = inst.result == InvalidCompactReg
                            ? InvalidCompactReg
                            : regSlot(inst.result);
                        bool loweredMathIntrinsic = false;
                        if (flat.dstSlot != InvalidCompactReg &&
                            inst.directTarget &&
                            inst.directExecution &&
                            inst.directTarget->module &&
                            isBuiltinMathModule(*inst.directTarget->module) &&
                            inst.directTarget->function) {
                            std::uint32_t calleeInstCost = 0;
                            std::uint16_t calleeCallCount = 0;
                            std::uint16_t calleeMaxDepth = 0;
                            if (computeStraightLineCallMetrics(
                                    computeStraightLineCallMetrics,
                                    inst.directExecution,
                                    calleeInstCost,
                                    calleeCallCount,
                                    calleeMaxDepth,
                                    0) &&
                                (1u + calleeInstCost) < InvalidCompactReg) {
                                const auto& targetFn = *inst.directTarget->function;
                                const auto& fnName = targetFn.name;
                                const auto& pTypes = targetFn.parameterTypes;
                                const auto& pTypeIds = targetFn.parameterTypeIds;
                                const auto retType = targetFn.returnType;
                                const auto retTypeId = targetFn.returnTypeId;
                                const auto fTid = fixedTypeId();
                                const auto v3Tid = vec3TypeId();
                                const auto v2Tid = vec2TypeId();

                                flat.aux0Slot = static_cast<std::uint16_t>(1u + calleeInstCost);
                                flat.aux1Slot = static_cast<std::uint16_t>(
                                    (calleeCallCount & 0xFFu) | ((calleeMaxDepth & 0xFFu) << 8u));

                                if (pTypes.empty() &&
                                    retType == semantic::PrimitiveType::Struct &&
                                    retTypeId == fTid) {
                                    if (fnName == "Fixed.Zero") {
                                        flat.flatOpcode = FlatOpcode::MathFixedConst;
                                        flat.integerImmediate = 0;
                                        loweredMathIntrinsic = true;
                                    } else if (fnName == "Fixed.One") {
                                        flat.flatOpcode = FlatOpcode::MathFixedConst;
                                        flat.integerImmediate = 256;
                                        loweredMathIntrinsic = true;
                                    } else if (fnName == "Fixed.Half") {
                                        flat.flatOpcode = FlatOpcode::MathFixedConst;
                                        flat.integerImmediate = 128;
                                        loweredMathIntrinsic = true;
                                    } else if (fnName == "Fixed.Pi" || fnName == "Math.Pi") {
                                        flat.flatOpcode = FlatOpcode::MathFixedConst;
                                        flat.integerImmediate = 804;
                                        loweredMathIntrinsic = true;
                                    } else if (fnName == "Fixed.HalfPi" || fnName == "Math.HalfPi") {
                                        flat.flatOpcode = FlatOpcode::MathFixedConst;
                                        flat.integerImmediate = 402;
                                        loweredMathIntrinsic = true;
                                    } else if (fnName == "Fixed.TwoPi" || fnName == "Math.TwoPi") {
                                        flat.flatOpcode = FlatOpcode::MathFixedConst;
                                        flat.integerImmediate = 1608;
                                        loweredMathIntrinsic = true;
                                    }
                                } else if (pTypes.size() == 1 &&
                                           retType == semantic::PrimitiveType::Struct &&
                                           retTypeId == fTid) {
                                    if (pTypes[0] == semantic::PrimitiveType::Int) {
                                        if (fnName == "Fixed.Raw") {
                                            flat.flatOpcode = FlatOpcode::MathFixedFromRaw;
                                            flat.src0Slot = regSlot(inst.op0);
                                            loweredMathIntrinsic = true;
                                        } else if (fnName == "Fixed.Of") {
                                            flat.flatOpcode = FlatOpcode::MathFixedFromInt;
                                            flat.src0Slot = regSlot(inst.op0);
                                            loweredMathIntrinsic = true;
                                        }
                                    } else if (pTypes[0] == semantic::PrimitiveType::Struct &&
                                               pTypeIds.size() == 1 &&
                                               pTypeIds[0] == fTid &&
                                               fnName == "Fixed.op_UnaryNegation") {
                                        flat.flatOpcode = FlatOpcode::MathFixedNeg;
                                        flat.src0Slot = regSlot(inst.op0);
                                        loweredMathIntrinsic = true;
                                    }
                                } else if (pTypes.size() == 2 &&
                                           retType == semantic::PrimitiveType::Struct &&
                                           retTypeId == fTid) {
                                    if (pTypes[0] == semantic::PrimitiveType::Int &&
                                        pTypes[1] == semantic::PrimitiveType::Int &&
                                        fnName == "Fixed.Ratio") {
                                        flat.flatOpcode = FlatOpcode::MathFixedRatio;
                                        flat.src0Slot = regSlot(inst.op0);
                                        flat.src1Slot = regSlot(inst.op1);
                                        loweredMathIntrinsic = true;
                                    } else if (pTypes[0] == semantic::PrimitiveType::Struct &&
                                               pTypes[1] == semantic::PrimitiveType::Struct &&
                                               pTypeIds.size() == 2 &&
                                               pTypeIds[0] == fTid &&
                                               pTypeIds[1] == fTid) {
                                        if (fnName == "Fixed.op_Addition") {
                                            flat.flatOpcode = FlatOpcode::MathFixedAdd;
                                            flat.src0Slot = regSlot(inst.op0);
                                            flat.src1Slot = regSlot(inst.op1);
                                            loweredMathIntrinsic = true;
                                        } else if (fnName == "Fixed.op_Subtraction") {
                                            flat.flatOpcode = FlatOpcode::MathFixedSub;
                                            flat.src0Slot = regSlot(inst.op0);
                                            flat.src1Slot = regSlot(inst.op1);
                                            loweredMathIntrinsic = true;
                                        } else if (fnName == "Fixed.op_Multiply") {
                                            flat.flatOpcode = FlatOpcode::MathFixedMul;
                                            flat.src0Slot = regSlot(inst.op0);
                                            flat.src1Slot = regSlot(inst.op1);
                                            loweredMathIntrinsic = true;
                                        } else if (fnName == "Fixed.op_Division") {
                                            flat.flatOpcode = FlatOpcode::MathFixedDiv;
                                            flat.src0Slot = regSlot(inst.op0);
                                            flat.src1Slot = regSlot(inst.op1);
                                            loweredMathIntrinsic = true;
                                        }
                                    } else if (fnName == "Fixed.op_Multiply" && pTypeIds.size() == 2) {
                                        if (pTypes[0] == semantic::PrimitiveType::Struct &&
                                            pTypeIds[0] == fTid &&
                                            pTypes[1] == semantic::PrimitiveType::Int) {
                                            flat.flatOpcode = FlatOpcode::MathFixedMulInt;
                                            flat.src0Slot = regSlot(inst.op0);
                                            flat.src1Slot = regSlot(inst.op1);
                                            loweredMathIntrinsic = true;
                                        } else if (pTypes[0] == semantic::PrimitiveType::Int &&
                                                   pTypes[1] == semantic::PrimitiveType::Struct &&
                                                   pTypeIds[1] == fTid) {
                                            flat.flatOpcode = FlatOpcode::MathFixedMulInt;
                                            flat.src0Slot = regSlot(inst.op1);
                                            flat.src1Slot = regSlot(inst.op0);
                                            loweredMathIntrinsic = true;
                                        }
                                    } else if (fnName == "Fixed.op_Division" &&
                                               pTypeIds.size() == 2 &&
                                               pTypes[0] == semantic::PrimitiveType::Struct &&
                                               pTypeIds[0] == fTid &&
                                               pTypes[1] == semantic::PrimitiveType::Int) {
                                        flat.flatOpcode = FlatOpcode::MathFixedDivInt;
                                        flat.src0Slot = regSlot(inst.op0);
                                        flat.src1Slot = regSlot(inst.op1);
                                        loweredMathIntrinsic = true;
                                    } else if (fnName == "Vec3.Dot" &&
                                               pTypeIds.size() == 2 &&
                                               pTypes[0] == semantic::PrimitiveType::Struct &&
                                               pTypes[1] == semantic::PrimitiveType::Struct &&
                                               pTypeIds[0] == v3Tid &&
                                               pTypeIds[1] == v3Tid) {
                                        flat.flatOpcode = FlatOpcode::MathVec3Dot;
                                        flat.src0Slot = regSlot(inst.op0);
                                        flat.src1Slot = regSlot(inst.op1);
                                        loweredMathIntrinsic = true;
                                    } else if (fnName == "Vec2.Dot" &&
                                               pTypeIds.size() == 2 &&
                                               pTypes[0] == semantic::PrimitiveType::Struct &&
                                               pTypes[1] == semantic::PrimitiveType::Struct &&
                                               pTypeIds[0] == v2Tid &&
                                               pTypeIds[1] == v2Tid) {
                                        flat.flatOpcode = FlatOpcode::MathVec2Dot;
                                        flat.src0Slot = regSlot(inst.op0);
                                        flat.src1Slot = regSlot(inst.op1);
                                        loweredMathIntrinsic = true;
                                    }
                                } else if (retType == semantic::PrimitiveType::Struct &&
                                           retTypeId == v3Tid) {
                                    if (pTypes.empty()) {
                                        if (fnName == "Vec3.Zero") {
                                            flat.flatOpcode = FlatOpcode::MathVec3Const;
                                            flat.integerImmediate = 0;
                                            loweredMathIntrinsic = true;
                                        } else if (fnName == "Vec3.One") {
                                            flat.flatOpcode = FlatOpcode::MathVec3Const;
                                            flat.integerImmediate = 256;
                                            loweredMathIntrinsic = true;
                                        }
                                    } else if (pTypes.size() == 3 &&
                                               pTypes[0] == semantic::PrimitiveType::Int &&
                                               pTypes[1] == semantic::PrimitiveType::Int &&
                                               pTypes[2] == semantic::PrimitiveType::Int) {
                                        if (fnName == "Vec3.Raw") {
                                            flat.flatOpcode = FlatOpcode::MathVec3Raw;
                                            flat.src0Slot = regSlot(inst.op0);
                                            flat.src1Slot = regSlot(inst.op1);
                                            flat.src2Slot = regSlot(inst.op2);
                                            loweredMathIntrinsic = true;
                                        } else if (fnName == "Vec3.Of") {
                                            flat.flatOpcode = FlatOpcode::MathVec3Of;
                                            flat.src0Slot = regSlot(inst.op0);
                                            flat.src1Slot = regSlot(inst.op1);
                                            flat.src2Slot = regSlot(inst.op2);
                                            loweredMathIntrinsic = true;
                                        }
                                    } else if (pTypes.size() == 1 &&
                                               pTypes[0] == semantic::PrimitiveType::Struct &&
                                               pTypeIds.size() == 1 &&
                                               pTypeIds[0] == v3Tid &&
                                               fnName == "Vec3.op_UnaryNegation") {
                                        flat.flatOpcode = FlatOpcode::MathVec3Neg;
                                        flat.src0Slot = regSlot(inst.op0);
                                        loweredMathIntrinsic = true;
                                    } else if (pTypes.size() == 2 && pTypeIds.size() == 2) {
                                        if (pTypes[0] == semantic::PrimitiveType::Struct &&
                                            pTypes[1] == semantic::PrimitiveType::Struct &&
                                            pTypeIds[0] == v3Tid &&
                                            pTypeIds[1] == v3Tid) {
                                            if (fnName == "Vec3.op_Addition") {
                                                flat.flatOpcode = FlatOpcode::MathVec3Add;
                                                flat.src0Slot = regSlot(inst.op0);
                                                flat.src1Slot = regSlot(inst.op1);
                                                loweredMathIntrinsic = true;
                                            } else if (fnName == "Vec3.op_Subtraction") {
                                                flat.flatOpcode = FlatOpcode::MathVec3Sub;
                                                flat.src0Slot = regSlot(inst.op0);
                                                flat.src1Slot = regSlot(inst.op1);
                                                loweredMathIntrinsic = true;
                                            } else if (fnName == "Vec3.Cross") {
                                                flat.flatOpcode = FlatOpcode::MathVec3Cross;
                                                flat.src0Slot = regSlot(inst.op0);
                                                flat.src1Slot = regSlot(inst.op1);
                                                loweredMathIntrinsic = true;
                                            }
                                        } else if (fnName == "Vec3.op_Multiply") {
                                            if (pTypes[0] == semantic::PrimitiveType::Struct &&
                                                pTypeIds[0] == v3Tid &&
                                                pTypes[1] == semantic::PrimitiveType::Struct &&
                                                pTypeIds[1] == fTid) {
                                                flat.flatOpcode = FlatOpcode::MathVec3MulFixed;
                                                flat.src0Slot = regSlot(inst.op0);
                                                flat.src1Slot = regSlot(inst.op1);
                                                loweredMathIntrinsic = true;
                                            } else if (pTypes[0] == semantic::PrimitiveType::Struct &&
                                                       pTypeIds[0] == fTid &&
                                                       pTypes[1] == semantic::PrimitiveType::Struct &&
                                                       pTypeIds[1] == v3Tid) {
                                                flat.flatOpcode = FlatOpcode::MathVec3MulFixed;
                                                flat.src0Slot = regSlot(inst.op1);
                                                flat.src1Slot = regSlot(inst.op0);
                                                loweredMathIntrinsic = true;
                                            } else if (pTypes[0] == semantic::PrimitiveType::Struct &&
                                                       pTypeIds[0] == v3Tid &&
                                                       pTypes[1] == semantic::PrimitiveType::Int) {
                                                flat.flatOpcode = FlatOpcode::MathVec3MulInt;
                                                flat.src0Slot = regSlot(inst.op0);
                                                flat.src1Slot = regSlot(inst.op1);
                                                loweredMathIntrinsic = true;
                                            } else if (pTypes[0] == semantic::PrimitiveType::Int &&
                                                       pTypes[1] == semantic::PrimitiveType::Struct &&
                                                       pTypeIds[1] == v3Tid) {
                                                flat.flatOpcode = FlatOpcode::MathVec3MulInt;
                                                flat.src0Slot = regSlot(inst.op1);
                                                flat.src1Slot = regSlot(inst.op0);
                                                loweredMathIntrinsic = true;
                                            }
                                        } else if (fnName == "Vec3.op_Division") {
                                            if (pTypes[0] == semantic::PrimitiveType::Struct &&
                                                pTypeIds[0] == v3Tid &&
                                                pTypes[1] == semantic::PrimitiveType::Struct &&
                                                pTypeIds[1] == fTid) {
                                                flat.flatOpcode = FlatOpcode::MathVec3DivFixed;
                                                flat.src0Slot = regSlot(inst.op0);
                                                flat.src1Slot = regSlot(inst.op1);
                                                loweredMathIntrinsic = true;
                                            } else if (pTypes[0] == semantic::PrimitiveType::Struct &&
                                                       pTypeIds[0] == v3Tid &&
                                                       pTypes[1] == semantic::PrimitiveType::Int) {
                                                flat.flatOpcode = FlatOpcode::MathVec3DivInt;
                                                flat.src0Slot = regSlot(inst.op0);
                                                flat.src1Slot = regSlot(inst.op1);
                                                loweredMathIntrinsic = true;
                                            }
                                        }
                                    }
                                } else if (retType == semantic::PrimitiveType::Struct &&
                                           retTypeId == v2Tid &&
                                           pTypes.size() == 2 &&
                                           pTypeIds.size() == 2) {
                                    if (pTypes[0] == semantic::PrimitiveType::Struct &&
                                        pTypes[1] == semantic::PrimitiveType::Struct &&
                                        pTypeIds[0] == v2Tid &&
                                        pTypeIds[1] == v2Tid) {
                                        if (fnName == "Vec2.op_Addition") {
                                            flat.flatOpcode = FlatOpcode::MathVec2Add;
                                            flat.src0Slot = regSlot(inst.op0);
                                            flat.src1Slot = regSlot(inst.op1);
                                            loweredMathIntrinsic = true;
                                        } else if (fnName == "Vec2.op_Subtraction") {
                                            flat.flatOpcode = FlatOpcode::MathVec2Sub;
                                            flat.src0Slot = regSlot(inst.op0);
                                            flat.src1Slot = regSlot(inst.op1);
                                            loweredMathIntrinsic = true;
                                        }
                                    } else if (fnName == "Vec2.op_Multiply") {
                                        if (pTypes[0] == semantic::PrimitiveType::Struct &&
                                            pTypeIds[0] == v2Tid &&
                                            pTypes[1] == semantic::PrimitiveType::Struct &&
                                            pTypeIds[1] == fTid) {
                                            flat.flatOpcode = FlatOpcode::MathVec2MulFixed;
                                            flat.src0Slot = regSlot(inst.op0);
                                            flat.src1Slot = regSlot(inst.op1);
                                            loweredMathIntrinsic = true;
                                        } else if (pTypes[0] == semantic::PrimitiveType::Struct &&
                                                   pTypeIds[0] == fTid &&
                                                   pTypes[1] == semantic::PrimitiveType::Struct &&
                                                   pTypeIds[1] == v2Tid) {
                                            flat.flatOpcode = FlatOpcode::MathVec2MulFixed;
                                            flat.src0Slot = regSlot(inst.op1);
                                            flat.src1Slot = regSlot(inst.op0);
                                            loweredMathIntrinsic = true;
                                        }
                                    }
                                }
                                const auto qTid = quatTypeId();
                                if (!loweredMathIntrinsic) {
                                    if (pTypes.empty() &&
                                        retType == semantic::PrimitiveType::Struct &&
                                        retTypeId == qTid &&
                                        (fnName == "Quat.Identity" || fnName == "QuatIdentity")) {
                                        flat.flatOpcode = FlatOpcode::MathQuatIdentity;
                                        loweredMathIntrinsic = true;
                                    } else if (pTypes.size() == 2 && pTypeIds.size() == 2 &&
                                               pTypes[0] == semantic::PrimitiveType::Struct &&
                                               pTypes[1] == semantic::PrimitiveType::Struct &&
                                               pTypeIds[0] == qTid && pTypeIds[1] == qTid) {
                                        if (retType == semantic::PrimitiveType::Struct &&
                                            retTypeId == fTid &&
                                            fnName == "Quat.Dot") {
                                            flat.flatOpcode = FlatOpcode::MathQuatDot;
                                            flat.src0Slot = regSlot(inst.op0);
                                            flat.src1Slot = regSlot(inst.op1);
                                            loweredMathIntrinsic = true;
                                        } else if (retType == semantic::PrimitiveType::Struct &&
                                                   retTypeId == qTid &&
                                                   fnName == "Quat.op_Multiply") {
                                            flat.flatOpcode = FlatOpcode::MathQuatMul;
                                            flat.src0Slot = regSlot(inst.op0);
                                            flat.src1Slot = regSlot(inst.op1);
                                            loweredMathIntrinsic = true;
                                        }
                                    } else if (pTypes.size() == 2 && pTypeIds.size() == 2 &&
                                               retType == semantic::PrimitiveType::Struct &&
                                               retTypeId == v3Tid &&
                                               pTypes[1] == semantic::PrimitiveType::Struct &&
                                               pTypeIds[1] == v3Tid) {
                                        if (pTypes[0] == semantic::PrimitiveType::Struct &&
                                            pTypeIds[0] == qTid &&
                                            (fnName == "Quat.Rotate" || fnName == "Quat.op_Multiply" || fnName == "QuatRotateVec3")) {
                                            flat.flatOpcode = FlatOpcode::MathQuatRotateVec3;
                                            flat.src0Slot = regSlot(inst.op0);
                                            flat.src1Slot = regSlot(inst.op1);
                                            loweredMathIntrinsic = true;
                                        } else if (pTypes[0] == semantic::PrimitiveType::Object &&
                                                   fnName == "Quat.Rotate") {
                                            flat.flatOpcode = FlatOpcode::MathQuatRotate;
                                            flat.src0Slot = regSlot(inst.op0);
                                            flat.src1Slot = regSlot(inst.op1);
                                            loweredMathIntrinsic = true;
                                        }
                                    }
                                }
                            }
                            if (!loweredMathIntrinsic) {
                                const auto& targetFn = *inst.directTarget->function;
                                const auto& fnName = targetFn.name;
                                const auto& pTypes = targetFn.parameterTypes;
                                const auto& pTypeIds = targetFn.parameterTypeIds;
                                const auto retType = targetFn.returnType;
                                const auto retTypeId = targetFn.returnTypeId;
                                const auto fTid = fixedTypeId();
                                const auto qTid = quatTypeId();
                                const bool isWrapper1Block = (inst.directExecution->blocks.size() == 1);
                                const std::uint32_t wrapperCost = isWrapper1Block
                                    ? bLen(inst.directExecution, 0)
                                    : 0u;

                                if (pTypes.size() == 1 && pTypeIds.size() == 1 &&
                                    pTypes[0] == semantic::PrimitiveType::Struct &&
                                    pTypeIds[0] == fTid &&
                                    retType == semantic::PrimitiveType::Struct &&
                                    retTypeId == fTid) {
                                    if ((fnName == "Fixed.Sin" || (isWrapper1Block && (fnName == "Math.Sin" || fnName == "Sin"))) &&
                                        sinCalleeBaseCost > 0) {
                                        const bool isWrap = (fnName != "Fixed.Sin");
                                        flat.flatOpcode = FlatOpcode::MathFixedSin;
                                        flat.src0Slot = regSlot(inst.op0);
                                        flat.aux0Slot = static_cast<std::uint16_t>(
                                            1u + sinCalleeBaseCost + (isWrap ? wrapperCost : 0u));
                                        const std::uint16_t calls = static_cast<std::uint16_t>(sinCalleeCalls + (isWrap ? 1u : 0u));
                                        const std::uint16_t depth = static_cast<std::uint16_t>(2u + (isWrap ? 1u : 0u));
                                        flat.aux1Slot = static_cast<std::uint16_t>((calls & 0xFFu) | ((depth & 0xFFu) << 8u));
                                        flat.integerImmediate = static_cast<std::int64_t>(sinWrapExtraCost);
                                        loweredMathIntrinsic = true;
                                    } else if ((fnName == "Fixed.Cos" || (isWrapper1Block && (fnName == "Math.Cos" || fnName == "Cos"))) &&
                                               cosCalleeBaseCost > 0) {
                                        const bool isWrap = (fnName != "Fixed.Cos");
                                        flat.flatOpcode = FlatOpcode::MathFixedCos;
                                        flat.src0Slot = regSlot(inst.op0);
                                        flat.aux0Slot = static_cast<std::uint16_t>(
                                            1u + cosCalleeBaseCost + (isWrap ? wrapperCost : 0u));
                                        const std::uint16_t calls = static_cast<std::uint16_t>(cosCalleeCalls + (isWrap ? 1u : 0u));
                                        const std::uint16_t depth = static_cast<std::uint16_t>(3u + (isWrap ? 1u : 0u));
                                        flat.aux1Slot = static_cast<std::uint16_t>((calls & 0xFFu) | ((depth & 0xFFu) << 8u));
                                        flat.integerImmediate = static_cast<std::int64_t>(sinWrapExtraCost);
                                        loweredMathIntrinsic = true;
                                    } else if (fnName == "Fixed.Sqrt" && fixedSqrtPosBaseCalleeCost > 0) {
                                        flat.flatOpcode = FlatOpcode::MathFixedSqrt;
                                        flat.src0Slot = regSlot(inst.op0);
                                        flat.aux0Slot = static_cast<std::uint16_t>(1u + fixedSqrtPosBaseCalleeCost);
                                        flat.aux1Slot = static_cast<std::uint16_t>(3u | (2u << 8u));
                                        const std::uint32_t negTotal = 1u + fixedSqrtNegCalleeCost;
                                        flat.integerImmediate = static_cast<std::int64_t>(
                                            (sqrt64LoopIterCost & 0xFFu) | ((negTotal & 0xFFFFu) << 8u));
                                        loweredMathIntrinsic = true;
                                    }
                                } else if (pTypes.size() == 1 && pTypeIds.size() == 1 &&
                                           pTypes[0] == semantic::PrimitiveType::Struct &&
                                           pTypeIds[0] == qTid &&
                                           retType == semantic::PrimitiveType::Struct &&
                                           retTypeId == qTid &&
                                           (fnName == "Quat.Normalized" || (isWrapper1Block && fnName == "QuatNormalize")) &&
                                           quatNormPosBaseCalleeCost > 0) {
                                    const bool isWrap = (fnName != "Quat.Normalized");
                                    const std::uint32_t totalNormCost =
                                        1u + (isWrap ? wrapperCost : 0u) + quatNormPosBaseCalleeCost;
                                    if (totalNormCost < InvalidCompactReg) {
                                        flat.flatOpcode = FlatOpcode::MathQuatNormalize;
                                        flat.src0Slot = regSlot(inst.op0);
                                        flat.aux0Slot = static_cast<std::uint16_t>(totalNormCost);
                                        const std::uint16_t calls =
                                            static_cast<std::uint16_t>((isWrap ? 1u : 0u) + quatNormPosCalls);
                                        const std::uint16_t depth = static_cast<std::uint16_t>(3u + (isWrap ? 1u : 0u));
                                        flat.aux1Slot = static_cast<std::uint16_t>((calls & 0xFFu) | ((depth & 0xFFu) << 8u));
                                        flat.integerImmediate = static_cast<std::int64_t>(sqrt64LoopIterCost);
                                        loweredMathIntrinsic = true;
                                    }
                                } else if (pTypes.size() == 1 &&
                                           pTypes[0] == semantic::PrimitiveType::Long &&
                                           retType == semantic::PrimitiveType::Long &&
                                           fnName == "Math.Sqrt64" &&
                                           sqrt64PosBaseCalleeCost > 0) {
                                    flat.flatOpcode = FlatOpcode::MathSqrt64;
                                    flat.src0Slot = regSlot(inst.op0);
                                    flat.aux0Slot = static_cast<std::uint16_t>(1u + sqrt64PosBaseCalleeCost);
                                    flat.aux1Slot = static_cast<std::uint16_t>(1u | (1u << 8u));
                                    const std::uint32_t negTotal = 1u + sqrt64NegCalleeCost;
                                    flat.integerImmediate = static_cast<std::int64_t>(
                                        (sqrt64LoopIterCost & 0xFFu) | ((negTotal & 0xFFFFu) << 8u));
                                    loweredMathIntrinsic = true;
                                } else if (pTypes.size() == 3 && pTypeIds.size() == 3 &&
                                           retType == semantic::PrimitiveType::Struct &&
                                           retTypeId == qTid &&
                                           inst.bytecode &&
                                           inst.bytecode->operands.size() == 3 &&
                                           inst.bytecode->operands[2] < regCount) {
                                    const std::uint16_t arg2Slot = regSlot(
                                        static_cast<std::uint16_t>(inst.bytecode->operands[2]));
                                    if (pTypes[0] == semantic::PrimitiveType::Struct && pTypeIds[0] == fTid &&
                                        pTypes[1] == semantic::PrimitiveType::Struct && pTypeIds[1] == fTid &&
                                        pTypes[2] == semantic::PrimitiveType::Struct && pTypeIds[2] == fTid &&
                                        (fnName == "Quat.FromEuler" || (isWrapper1Block && fnName == "QuatFromEuler")) &&
                                        quatFromEulerBaseCalleeCost > 0) {
                                        const bool isWrap = (fnName != "Quat.FromEuler");
                                        flat.flatOpcode = FlatOpcode::MathQuatFromEuler;
                                        flat.src0Slot = regSlot(inst.op0);
                                        flat.src1Slot = regSlot(inst.op1);
                                        flat.src2Slot = arg2Slot;
                                        flat.aux0Slot = static_cast<std::uint16_t>(
                                            1u + quatFromEulerBaseCalleeCost + (isWrap ? wrapperCost : 0u));
                                        const std::uint16_t calls = static_cast<std::uint16_t>(
                                            quatFromEulerBaseCalls + (isWrap ? 1u : 0u));
                                        const std::uint16_t depth = static_cast<std::uint16_t>(4u + (isWrap ? 1u : 0u));
                                        flat.aux1Slot = static_cast<std::uint16_t>((calls & 0xFFu) | ((depth & 0xFFu) << 8u));
                                        flat.integerImmediate = static_cast<std::int64_t>(sinWrapExtraCost);
                                        loweredMathIntrinsic = true;
                                    } else if (pTypes[0] == semantic::PrimitiveType::Struct && pTypeIds[0] == qTid &&
                                               pTypes[1] == semantic::PrimitiveType::Struct && pTypeIds[1] == qTid &&
                                               pTypes[2] == semantic::PrimitiveType::Struct && pTypeIds[2] == fTid &&
                                               (fnName == "Quat.Lerp" || (isWrapper1Block && fnName == "QuatLerp")) &&
                                               quatLerpPosBaseCalleeCost > 0) {
                                        const bool isWrap = (fnName != "Quat.Lerp");
                                        flat.flatOpcode = FlatOpcode::MathQuatLerp;
                                        flat.src0Slot = regSlot(inst.op0);
                                        flat.src1Slot = regSlot(inst.op1);
                                        flat.src2Slot = arg2Slot;
                                        flat.aux0Slot = static_cast<std::uint16_t>(
                                            1u + quatLerpPosBaseCalleeCost + (isWrap ? wrapperCost : 0u));
                                        const std::uint16_t calls = static_cast<std::uint16_t>(
                                            quatLerpPosBaseCalls + (isWrap ? 1u : 0u));
                                        const std::uint16_t depth = static_cast<std::uint16_t>(4u + (isWrap ? 1u : 0u));
                                        flat.aux1Slot = static_cast<std::uint16_t>((calls & 0xFFu) | ((depth & 0xFFu) << 8u));
                                        flat.integerImmediate = static_cast<std::int64_t>(
                                            (sqrt64LoopIterCost & 0xFFu) |
                                            ((quatLerpDotNegExtraCost & 0xFFFFu) << 8u) |
                                            ((static_cast<std::uint64_t>(quatLerpDotNegExtraCalls) & 0xFFu) << 24u));
                                        loweredMathIntrinsic = true;
                                    }
                                }
                            }
                        }
                        if (!loweredMathIntrinsic) {
                            if (inst.op0 == InvalidCompactReg) {
                                flat.flatOpcode = FlatOpcode::CallDirect0;
                            } else if (inst.op1 == InvalidCompactReg) {
                                flat.flatOpcode = FlatOpcode::CallDirect1;
                                flat.src0Slot = regSlot(inst.op0);
                            } else if (inst.op2 == InvalidCompactReg) {
                                flat.flatOpcode = FlatOpcode::CallDirect2;
                                flat.src0Slot = regSlot(inst.op0);
                                flat.src1Slot = regSlot(inst.op1);
                            } else {
                                flat.flatOpcode = FlatOpcode::CallDirectN;
                            }
                        }
                        break;
                    }
                    case QuickOpcode::CallPolled: {
                        flat.flatOpcode = FlatOpcode::CallPolledFast;
                        flat.dstSlot = inst.result == InvalidCompactReg
                            ? InvalidCompactReg
                            : regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        flat.src1Slot = inst.op1 == InvalidCompactReg
                            ? InvalidCompactReg
                            : regSlot(inst.op1);
                        flat.src2Slot = inst.op2 == InvalidCompactReg
                            ? InvalidCompactReg
                            : regSlot(inst.op2);
                        const std::size_t opCount = inst.bytecode ? inst.bytecode->operands.size() : 0;
                        flat.inlineArgCount = (opCount >= 1 && opCount <= 3)
                            ? static_cast<std::uint8_t>(opCount)
                            : 0u;
                        const std::uint32_t siteId =
                            static_cast<std::uint32_t>(mirror->polyCallSites.size());
                        PolymorphicCallSiteDesc siteDesc{};
                        if (inst.bytecode && inst.bytecode->index < module.functionReferences.size()) {
                            const auto& ref = module.functionReferences[inst.bytecode->index];
                            if (ref.virtualDispatch && !ref.interfaceDispatch) {
                                siteDesc.isInterface = false;
                                siteDesc.slot = ref.virtualSlot;
                                siteDesc.valid = true;
                            } else if (ref.interfaceDispatch && !ref.virtualDispatch) {
                                siteDesc.isInterface = true;
                                siteDesc.interfaceTypeId = ref.interfaceTypeId;
                                siteDesc.slot = ref.interfaceSlot;
                                siteDesc.valid = true;
                            }
                        }
                        mirror->polyCallSites.push_back(siteDesc);
                        flat.auxImm32 = siteId;
                        break;
                    }
                    case QuickOpcode::ConstantDouble: {
                        flat.flatOpcode = FlatOpcode::ConstantDouble;
                        flat.dstSlot = regSlot(inst.result);
                        const double dVal = inst.bytecode->activeDoubleImmediate();
                        std::memcpy(&flat.integerImmediate, &dVal, sizeof(double));
                        break;
                    }
                    case QuickOpcode::AddDouble:
                        flat.flatOpcode = FlatOpcode::AddDouble;
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        flat.src1Slot = regSlot(inst.op1);
                        break;
                    case QuickOpcode::SubtractDouble:
                        flat.flatOpcode = FlatOpcode::SubDouble;
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        flat.src1Slot = regSlot(inst.op1);
                        break;
                    case QuickOpcode::MultiplyDouble:
                        flat.flatOpcode = FlatOpcode::MulDouble;
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        flat.src1Slot = regSlot(inst.op1);
                        break;
                    case QuickOpcode::DivideDouble:
                        flat.flatOpcode = FlatOpcode::DivDouble;
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        flat.src1Slot = regSlot(inst.op1);
                        break;
                    case QuickOpcode::LessDouble:
                        flat.flatOpcode = FlatOpcode::LessDouble;
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        flat.src1Slot = regSlot(inst.op1);
                        break;
                    case QuickOpcode::LessOrEqualDouble:
                        flat.flatOpcode = FlatOpcode::LessOrEqualDouble;
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        flat.src1Slot = regSlot(inst.op1);
                        break;
                    case QuickOpcode::GreaterDouble:
                        flat.flatOpcode = FlatOpcode::GreaterDouble;
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        flat.src1Slot = regSlot(inst.op1);
                        break;
                    case QuickOpcode::GreaterOrEqualDouble:
                        flat.flatOpcode = FlatOpcode::GreaterOrEqualDouble;
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        flat.src1Slot = regSlot(inst.op1);
                        break;
                    case QuickOpcode::LoadFieldFast: {
                        const auto& ownerType = module.types[inst.bytecode->typeIndex];
                        const auto& fld = ownerType.fields[inst.auxReg1];
                        FastFieldKind fk = FastFieldKind::None;
                        std::uint8_t exactTag = 0u;
                        std::uint32_t metaIdx = 0u;
                        if (ownerType.id != 0 && classifyFastField(fld, ownerType.id, fk, exactTag, metaIdx)) {
                            flat.flatOpcode = (fk == FastFieldKind::ExactValueTag)
                                ? FlatOpcode::LoadFieldExactFast
                                : FlatOpcode::LoadFieldFast;
                            flat.dstSlot = regSlot(inst.result);
                            flat.src0Slot = regSlot(inst.op0);
                            flat.aux0Slot = inst.auxReg1;
                            flat.integerImmediate = static_cast<std::int64_t>(ownerType.id);
                            flat.binaryOpcode1 = static_cast<QuickOpcode>(exactTag);
                            flat.binaryOpcode2 = static_cast<QuickOpcode>(static_cast<std::uint8_t>(fk));
                            flat.auxImm32 = metaIdx;
                        } else {
                            flat.flatOpcode = FlatOpcode::GenericBytecode;
                        }
                        break;
                    }
                    case QuickOpcode::StoreFieldFast: {
                        const auto& ownerType = module.types[inst.bytecode->typeIndex];
                        const auto& fld = ownerType.fields[inst.auxReg1];
                        FastFieldKind fk = FastFieldKind::None;
                        std::uint8_t exactTag = 0u;
                        std::uint32_t metaIdx = 0u;
                        if (ownerType.id != 0 && classifyFastField(fld, ownerType.id, fk, exactTag, metaIdx)) {
                            flat.flatOpcode = (fk == FastFieldKind::ExactValueTag)
                                ? FlatOpcode::StoreFieldExactFast
                                : FlatOpcode::StoreFieldFast;
                            flat.src0Slot = regSlot(inst.op0);
                            flat.src1Slot = regSlot(inst.op1);
                            flat.aux0Slot = inst.auxReg1;
                            flat.integerImmediate = static_cast<std::int64_t>(ownerType.id);
                            flat.binaryOpcode1 = static_cast<QuickOpcode>(exactTag);
                            flat.binaryOpcode2 = static_cast<QuickOpcode>(static_cast<std::uint8_t>(fk));
                            flat.auxImm32 = metaIdx;
                        } else {
                            flat.flatOpcode = FlatOpcode::GenericBytecode;
                        }
                        break;
                    }
                    case QuickOpcode::LoadStructFieldFast: {
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        flat.aux0Slot = inst.auxReg1;
                        const auto& structType = module.types[inst.bytecode->typeIndex];
                        const auto& fld = structType.fields[inst.auxReg1];
                        const auto fldTid = exactTypeId(fld.type, fld.typeName);
                        flat.integerImmediate = static_cast<std::int64_t>(fldTid);
                        if (structType.id == fixedTypeId() &&
                            inst.auxReg1 == 0 &&
                            fld.type == semantic::PrimitiveType::Int) {
                            flat.flatOpcode = FlatOpcode::LoadFixedRawFast;
                        } else if (structType.id == vec3TypeId() &&
                                   inst.auxReg1 < 3 &&
                                   fld.type == semantic::PrimitiveType::Struct &&
                                   fldTid == fixedTypeId()) {
                            flat.flatOpcode = FlatOpcode::LoadVec3FieldFast;
                        } else if (structType.id == quatTypeId() &&
                                   inst.auxReg1 < 4 &&
                                   fld.type == semantic::PrimitiveType::Struct &&
                                   fldTid == fixedTypeId()) {
                            flat.flatOpcode = FlatOpcode::LoadQuatFieldFast;
                        } else {
                            flat.flatOpcode = FlatOpcode::LoadStructFieldFast;
                            flat.auxImm32 = inst.bytecode->typeIndex;
                        }
                        break;
                    }
                    case QuickOpcode::StoreStructFieldFast: {
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        flat.src1Slot = regSlot(inst.op1);
                        flat.aux0Slot = inst.auxReg1;
                        const auto& structType = module.types[inst.bytecode->typeIndex];
                        const auto& fld = structType.fields[inst.auxReg1];
                        const auto fldTid = exactTypeId(fld.type, fld.typeName);
                        flat.integerImmediate = static_cast<std::int64_t>(fldTid);
                        if (structType.id == fixedTypeId() &&
                            inst.auxReg1 == 0 &&
                            fld.type == semantic::PrimitiveType::Int) {
                            flat.flatOpcode = FlatOpcode::StoreFixedRawFast;
                        } else if (structType.id == vec3TypeId() &&
                                   inst.auxReg1 < 3 &&
                                   fld.type == semantic::PrimitiveType::Struct &&
                                   fldTid == fixedTypeId()) {
                            flat.flatOpcode = FlatOpcode::StoreVec3FieldFast;
                        } else if (structType.id == quatTypeId() &&
                                   inst.auxReg1 < 4 &&
                                   fld.type == semantic::PrimitiveType::Struct &&
                                   fldTid == fixedTypeId()) {
                            flat.flatOpcode = FlatOpcode::StoreQuatFieldFast;
                        } else {
                            flat.flatOpcode = FlatOpcode::StoreStructFieldFast;
                            flat.auxImm32 = inst.bytecode->typeIndex;
                        }
                        break;
                    }
                    case QuickOpcode::ConstantBool:
                        flat.flatOpcode = FlatOpcode::ConstantBool;
                        flat.dstSlot = regSlot(inst.result);
                        flat.integerImmediate = inst.bytecode->boolImmediate ? 1 : 0;
                        break;
                    case QuickOpcode::ConstantLong:
                        flat.flatOpcode = FlatOpcode::ConstantLong;
                        flat.dstSlot = regSlot(inst.result);
                        break;
                    case QuickOpcode::ArrayLengthFast:
                        flat.flatOpcode = FlatOpcode::ArrayLengthFast;
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        break;
                    case QuickOpcode::ConvertIntToLongFast:
                        flat.flatOpcode = FlatOpcode::ConvertIntToLongFast;
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        break;
                    case QuickOpcode::ConvertIntToDoubleFast:
                        flat.flatOpcode = FlatOpcode::ConvertIntToDoubleFast;
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        break;
                    case QuickOpcode::ConvertLongToDoubleFast:
                        flat.flatOpcode = FlatOpcode::ConvertLongToDoubleFast;
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        break;
                    case QuickOpcode::NegateIntUnchecked:
                        flat.flatOpcode = FlatOpcode::NegateIntUnchecked;
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        break;
                    case QuickOpcode::NegateDouble:
                        flat.flatOpcode = FlatOpcode::NegateDouble;
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        break;
                    case QuickOpcode::LogicalNot:
                        flat.flatOpcode = FlatOpcode::LogicalNot;
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        break;
                    case QuickOpcode::AddFloat:
                        flat.flatOpcode = FlatOpcode::AddFloat;
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        flat.src1Slot = regSlot(inst.op1);
                        break;
                    case QuickOpcode::SubtractFloat:
                        flat.flatOpcode = FlatOpcode::SubFloat;
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        flat.src1Slot = regSlot(inst.op1);
                        break;
                    case QuickOpcode::MultiplyFloat:
                        flat.flatOpcode = FlatOpcode::MulFloat;
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        flat.src1Slot = regSlot(inst.op1);
                        break;
                    case QuickOpcode::DivideFloat:
                        flat.flatOpcode = FlatOpcode::DivFloat;
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        flat.src1Slot = regSlot(inst.op1);
                        break;
                    case QuickOpcode::LessFloat:
                        flat.flatOpcode = FlatOpcode::LessFloat;
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        flat.src1Slot = regSlot(inst.op1);
                        break;
                    case QuickOpcode::LessOrEqualFloat:
                        flat.flatOpcode = FlatOpcode::LessOrEqualFloat;
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        flat.src1Slot = regSlot(inst.op1);
                        break;
                    case QuickOpcode::GreaterFloat:
                        flat.flatOpcode = FlatOpcode::GreaterFloat;
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        flat.src1Slot = regSlot(inst.op1);
                        break;
                    case QuickOpcode::GreaterOrEqualFloat:
                        flat.flatOpcode = FlatOpcode::GreaterOrEqualFloat;
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        flat.src1Slot = regSlot(inst.op1);
                        break;
                    case QuickOpcode::EqualFloat:
                        flat.flatOpcode = FlatOpcode::EqualFloat;
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        flat.src1Slot = regSlot(inst.op1);
                        break;
                    case QuickOpcode::NotEqualFloat:
                        flat.flatOpcode = FlatOpcode::NotEqualFloat;
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        flat.src1Slot = regSlot(inst.op1);
                        break;
                    case QuickOpcode::NegateFloat:
                        flat.flatOpcode = FlatOpcode::NegateFloat;
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        break;
                    case QuickOpcode::ConvertIntToFloatFast:
                        flat.flatOpcode = FlatOpcode::ConvertIntToFloatFast;
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        break;
                    case QuickOpcode::ConvertDoubleToFloatFast:
                        flat.flatOpcode = FlatOpcode::ConvertDoubleToFloatFast;
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        break;
                    case QuickOpcode::ConvertFloatToIntFast:
                        flat.flatOpcode = FlatOpcode::ConvertFloatToIntFast;
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        break;
                    case QuickOpcode::ConvertFloatToDoubleFast:
                        flat.flatOpcode = FlatOpcode::ConvertFloatToDoubleFast;
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        break;
                    case QuickOpcode::ConvertLongToIntFast:
                        flat.flatOpcode = FlatOpcode::ConvertLongToIntFast;
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        break;
                    case QuickOpcode::NewStructFast: {
                        flat.flatOpcode = FlatOpcode::NewStructFast;
                        flat.dstSlot = regSlot(inst.result);
                        const auto structTid = module.types[inst.bytecode->typeIndex].id;
                        if (structTid == fixedTypeId()) flat.aux0Slot = 1;
                        else if (structTid == fixed16TypeId()) flat.aux0Slot = 2;
                        else if (structTid == vec2TypeId()) flat.aux0Slot = 3;
                        else if (structTid == vec3TypeId()) flat.aux0Slot = 4;
                        else if (structTid == quatTypeId()) flat.aux0Slot = 5;
                        else if (structTid == fixed64TypeId()) flat.aux0Slot = 6;
                        else if (structTid == vec2fTypeId()) flat.aux0Slot = 7;
                        else if (structTid == vec3fTypeId()) flat.aux0Slot = 8;
                        else if (structTid == quatfTypeId()) flat.aux0Slot = 9;
                        else flat.aux0Slot = 0;
                        break;
                    }
                    case QuickOpcode::Generic:
                    default:
                        if (inst.bytecode) {
                            const auto& bcInst = *inst.bytecode;
                            if (bcInst.opcode == bytecode::Opcode::NewObject &&
                                bcInst.operands.empty() &&
                                bcInst.result < regCount &&
                                bcInst.typeIndex < module.types.size()) {
                                const auto& objType = module.types[bcInst.typeIndex];
                                const auto itTmpl = objectTemplateByTypeId.find(objType.id);
                                if (itTmpl != objectTemplateByTypeId.end()) {
                                    flat.flatOpcode = FlatOpcode::NewObjectFast;
                                    flat.dstSlot = regSlot(static_cast<std::uint16_t>(bcInst.result));
                                    flat.aux0Slot = itTmpl->second;
                                    break;
                                }
                            }
                            if (bcInst.opcode == bytecode::Opcode::NewArray &&
                                bcInst.operands.size() == 1 &&
                                bcInst.operands[0] < regCount &&
                                bcInst.result < regCount &&
                                bcInst.result < function.registerTypeIds.size()) {
                                flat.flatOpcode = FlatOpcode::NewArrayFast;
                                flat.dstSlot = regSlot(static_cast<std::uint16_t>(bcInst.result));
                                flat.src0Slot = regSlot(static_cast<std::uint16_t>(bcInst.operands[0]));
                                flat.binaryOpcode1 = static_cast<QuickOpcode>(static_cast<std::uint8_t>(bcInst.elementType));
                                flat.integerImmediate = static_cast<std::int64_t>(function.registerTypeIds[bcInst.result]);
                                if (bcInst.elementTypeId == 0 || bcInst.elementType == semantic::PrimitiveType::Object) {
                                    flat.aux0Slot = 1;
                                } else if (bcInst.elementType == semantic::PrimitiveType::Struct) {
                                    if (bcInst.elementTypeId == fixedTypeId()) flat.aux0Slot = 2;
                                    else if (bcInst.elementTypeId == fixed16TypeId()) flat.aux0Slot = 3;
                                    else if (bcInst.elementTypeId == vec2TypeId()) flat.aux0Slot = 4;
                                    else if (bcInst.elementTypeId == vec3TypeId()) flat.aux0Slot = 5;
                                    else if (bcInst.elementTypeId == quatTypeId()) flat.aux0Slot = 6;
                                    else if (bcInst.elementTypeId == fixed64TypeId()) flat.aux0Slot = 7;
                                    else if (bcInst.elementTypeId == vec2fTypeId()) flat.aux0Slot = 8;
                                    else if (bcInst.elementTypeId == vec3fTypeId()) flat.aux0Slot = 9;
                                    else if (bcInst.elementTypeId == quatfTypeId()) flat.aux0Slot = 10;
                                    else flat.aux0Slot = 0;
                                } else {
                                    flat.aux0Slot = 0;
                                }
                                break;
                            }
                            if ((bcInst.opcode == bytecode::Opcode::IsType ||
                                 bcInst.opcode == bytecode::Opcode::AsType) &&
                                bcInst.operands.size() == 1 &&
                                bcInst.operands[0] < regCount &&
                                bcInst.result < regCount &&
                                bcInst.elementType == semantic::PrimitiveType::Object &&
                                bcInst.elementTypeId != 0 &&
                                function.registerTypes[bcInst.operands[0]] ==
                                    semantic::PrimitiveType::Object) {
                                flat.flatOpcode = (bcInst.opcode == bytecode::Opcode::IsType)
                                    ? FlatOpcode::IsTypeObjFast
                                    : FlatOpcode::AsTypeObjFast;
                                flat.dstSlot = regSlot(static_cast<std::uint16_t>(bcInst.result));
                                flat.src0Slot = regSlot(static_cast<std::uint16_t>(bcInst.operands[0]));
                                flat.integerImmediate = static_cast<std::int64_t>(bcInst.elementTypeId);
                                break;
                            }
                            if (bcInst.opcode == bytecode::Opcode::NewDelegate &&
                                bcInst.result < regCount &&
                                bcInst.typeIndex < module.types.size() &&
                                bcInst.index < module.functionReferences.size() &&
                                bcInst.operands.size() <= 1) {
                                const auto& delType = module.types[bcInst.typeIndex];
                                const auto& fnRef = module.functionReferences[bcInst.index];
                                const bool recvOk = bcInst.operands.empty()
                                    ? !(fnRef.virtualDispatch || fnRef.interfaceDispatch)
                                    : (bcInst.operands[0] < regCount && !fnRef.parameterTypes.empty());
                                std::uint64_t delFlags = 0;
                                if (fnRef.virtualDispatch) delFlags |= 1u;
                                if (fnRef.interfaceDispatch) delFlags |= 2u;
                                if (delType.delegateType && delType.id != 0 && recvOk && delFlags <= 2u) {
                                    const auto delWord = [](std::uint64_t v, bool high) noexcept -> std::int64_t {
                                        return static_cast<std::int64_t>(
                                            static_cast<std::uint32_t>(high ? (v >> 32u) : v));
                                    };
                                    NewDelegateSiteDesc site{};
                                    site.delegateTypeId = delType.id;
                                    site.hasReceiverOperand = !bcInst.operands.empty();
                                    if (site.hasReceiverOperand) {
                                        site.expectedReceiverType = fnRef.parameterTypes.front();
                                        site.expectedReceiverTypeId = fnRef.parameterTypeIds.empty()
                                            ? 0
                                            : fnRef.parameterTypeIds.front();
                                    }
                                    site.templateFields[0] = NullObject{};
                                    site.templateFields[1] = delWord(fnRef.symbolId, false);
                                    site.templateFields[2] = delWord(fnRef.symbolId, true);
                                    site.templateFields[3] = static_cast<std::int64_t>(delFlags);
                                    site.templateFields[4] = static_cast<std::int64_t>(
                                        fnRef.virtualDispatch
                                            ? fnRef.virtualSlot
                                            : (fnRef.interfaceDispatch ? fnRef.interfaceSlot : 0u));
                                    site.templateFields[5] = delWord(fnRef.interfaceTypeId, false);
                                    site.templateFields[6] = delWord(fnRef.interfaceTypeId, true);
                                    site.templateFields[7] = NullObject{};
                                    site.valid = true;
                                    const auto siteId =
                                        static_cast<std::uint32_t>(mirror->newDelegateSites.size());
                                    mirror->newDelegateSites.push_back(site);
                                    flat.flatOpcode = FlatOpcode::NewDelegateFast;
                                    flat.dstSlot = regSlot(static_cast<std::uint16_t>(bcInst.result));
                                    flat.src0Slot = site.hasReceiverOperand
                                        ? regSlot(static_cast<std::uint16_t>(bcInst.operands[0]))
                                        : InvalidCompactReg;
                                    flat.inlineArgCount = static_cast<std::uint8_t>(bcInst.operands.size());
                                    flat.auxImm32 = siteId;
                                    break;
                                }
                            }
                            if ((bcInst.opcode == bytecode::Opcode::CombineDelegate ||
                                 bcInst.opcode == bytecode::Opcode::RemoveDelegate) &&
                                bcInst.operands.size() == 2 &&
                                bcInst.operands[0] < regCount &&
                                bcInst.operands[1] < regCount &&
                                bcInst.result < regCount &&
                                bcInst.typeIndex < module.types.size()) {
                                const auto& delType = module.types[bcInst.typeIndex];
                                if (delType.delegateType && delType.id != 0) {
                                    flat.flatOpcode =
                                        (bcInst.opcode == bytecode::Opcode::CombineDelegate)
                                            ? FlatOpcode::CombineDelegateFast
                                            : FlatOpcode::RemoveDelegateFast;
                                    flat.dstSlot = regSlot(static_cast<std::uint16_t>(bcInst.result));
                                    flat.src0Slot = regSlot(static_cast<std::uint16_t>(bcInst.operands[0]));
                                    flat.src1Slot = regSlot(static_cast<std::uint16_t>(bcInst.operands[1]));
                                    flat.integerImmediate = static_cast<std::int64_t>(delType.id);
                                    break;
                                }
                            }
                            if (bcInst.opcode == bytecode::Opcode::InvokeDelegate &&
                                !bcInst.operands.empty() &&
                                bcInst.operands[0] < regCount &&
                                (bcInst.result == bytecode::InvalidRegister || bcInst.result < regCount) &&
                                bcInst.typeIndex < module.types.size()) {
                                const auto& delType = module.types[bcInst.typeIndex];
                                const semantic::FunctionSymbol* invokeSym = nullptr;
                                if (delType.delegateType && delType.id != 0) {
                                    for (const auto& m : delType.methods) {
                                        if (m.name == "Invoke") {
                                            invokeSym = &m;
                                            break;
                                        }
                                    }
                                }
                                bool allOpsInRange = true;
                                for (const auto opReg : bcInst.operands) {
                                    if (opReg >= regCount) {
                                        allOpsInRange = false;
                                        break;
                                    }
                                }
                                if (invokeSym &&
                                    !invokeSym->parameters.empty() &&
                                    bcInst.operands.size() == invokeSym->parameters.size() &&
                                    allOpsInRange) {
                                    const auto exactScalarOrInlineTag =
                                        [&](semantic::PrimitiveType pt, semantic::SymbolId tid) noexcept -> std::uint8_t {
                                        switch (pt) {
                                        case semantic::PrimitiveType::Bool: return static_cast<std::uint8_t>(ValueTag::Bool);
                                        case semantic::PrimitiveType::Byte: return static_cast<std::uint8_t>(ValueTag::Byte);
                                        case semantic::PrimitiveType::SByte: return static_cast<std::uint8_t>(ValueTag::SByte);
                                        case semantic::PrimitiveType::Short: return static_cast<std::uint8_t>(ValueTag::Short);
                                        case semantic::PrimitiveType::UShort: return static_cast<std::uint8_t>(ValueTag::UShort);
                                        case semantic::PrimitiveType::Int: return static_cast<std::uint8_t>(ValueTag::Int64);
                                        case semantic::PrimitiveType::UInt: return static_cast<std::uint8_t>(ValueTag::UInt);
                                        case semantic::PrimitiveType::Long: return static_cast<std::uint8_t>(ValueTag::Long);
                                        case semantic::PrimitiveType::ULong: return static_cast<std::uint8_t>(ValueTag::ULong);
                                        case semantic::PrimitiveType::Float: return static_cast<std::uint8_t>(ValueTag::Float);
                                        case semantic::PrimitiveType::Double: return static_cast<std::uint8_t>(ValueTag::Double);
                                        case semantic::PrimitiveType::Char: return static_cast<std::uint8_t>(ValueTag::Char);
                                        case semantic::PrimitiveType::Struct:
                                            if (tid == fixedTypeId()) return static_cast<std::uint8_t>(ValueTag::Fixed);
                                            if (tid == fixed16TypeId()) return static_cast<std::uint8_t>(ValueTag::Fixed16);
                                            if (tid == vec2TypeId()) return static_cast<std::uint8_t>(ValueTag::Vec2);
                                            if (tid == vec3TypeId()) return static_cast<std::uint8_t>(ValueTag::Vec3);
                                            if (tid == fixed64TypeId()) return static_cast<std::uint8_t>(ValueTag::Fixed64);
                                            if (tid == vec2fTypeId()) return static_cast<std::uint8_t>(ValueTag::Vec2f);
                                            if (tid == vec3fTypeId()) return static_cast<std::uint8_t>(ValueTag::Vec3f);
                                            return 0u;
                                        default:
                                            return 0u;
                                        }
                                    };
                                    InvokeDelegateSiteDesc site{};
                                    site.delegateTypeId = delType.id;
                                    const std::size_t uCount = invokeSym->parameters.size() - 1u;
                                    site.userParamTypes.reserve(uCount);
                                    site.userParamTypeIds.reserve(uCount);
                                    site.userParamExactTags.reserve(uCount);
                                    for (std::size_t pIdx = 1; pIdx < invokeSym->parameters.size(); ++pIdx) {
                                        const auto& param = invokeSym->parameters[pIdx];
                                        const auto pt = semantic::storageTypeOf(param);
                                        const auto ptId = exactTypeId(pt, semantic::storageTypeNameOf(param));
                                        site.userParamTypes.push_back(pt);
                                        site.userParamTypeIds.push_back(ptId);
                                        site.userParamExactTags.push_back(exactScalarOrInlineTag(pt, ptId));
                                    }
                                    site.valid = true;
                                    const auto siteId =
                                        static_cast<std::uint32_t>(mirror->invokeDelegateSites.size());
                                    mirror->invokeDelegateSites.push_back(std::move(site));
                                    flat.flatOpcode = FlatOpcode::InvokeDelegateFast;
                                    flat.dstSlot = (bcInst.result == bytecode::InvalidRegister)
                                        ? InvalidCompactReg
                                        : regSlot(static_cast<std::uint16_t>(bcInst.result));
                                    flat.src0Slot = regSlot(static_cast<std::uint16_t>(bcInst.operands[0]));
                                    flat.src1Slot = bcInst.operands.size() >= 2
                                        ? regSlot(static_cast<std::uint16_t>(bcInst.operands[1]))
                                        : InvalidCompactReg;
                                    flat.src2Slot = bcInst.operands.size() >= 3
                                        ? regSlot(static_cast<std::uint16_t>(bcInst.operands[2]))
                                        : InvalidCompactReg;
                                    flat.inlineArgCount = bcInst.operands.size() <= 3
                                        ? static_cast<std::uint8_t>(bcInst.operands.size())
                                        : 0u;
                                    flat.auxImm32 = siteId;
                                    break;
                                }
                            }
                            if (bcInst.opcode == bytecode::Opcode::AsyncEnter) {
                                flat.flatOpcode = FlatOpcode::AsyncEnter;
                                break;
                            }
                            if (bcInst.opcode == bytecode::Opcode::AwaitSuspend &&
                                bcInst.operands.size() == 1 &&
                                bcInst.operands[0] < regCount &&
                                bcInst.index < localCount) {
                                flat.flatOpcode = FlatOpcode::AwaitSuspend;
                                flat.src0Slot = regSlot(static_cast<std::uint16_t>(bcInst.operands[0]));
                                flat.storeSlot = localSlot(static_cast<std::uint16_t>(bcInst.index));
                                flat.integerImmediate = bcInst.activeIntegerImmediate();
                                break;
                            }
                            if (bcInst.opcode == bytecode::Opcode::AwaitResume &&
                                bcInst.index < localCount &&
                                (bcInst.result == bytecode::InvalidRegister || bcInst.result < regCount)) {
                                flat.flatOpcode = FlatOpcode::AwaitResume;
                                flat.src0Slot = localSlot(static_cast<std::uint16_t>(bcInst.index));
                                flat.dstSlot = (bcInst.result == bytecode::InvalidRegister)
                                    ? InvalidCompactReg
                                    : regSlot(static_cast<std::uint16_t>(bcInst.result));
                                flat.binaryOpcode1 = static_cast<QuickOpcode>(static_cast<std::uint8_t>(bcInst.elementType));
                                flat.integerImmediate = static_cast<std::int64_t>(bcInst.activeElementTypeId());
                                break;
                            }
                            if (bcInst.opcode == bytecode::Opcode::AsyncResolve &&
                                bcInst.result < regCount &&
                                bcInst.operands.size() <= 1 &&
                                (bcInst.operands.empty() || bcInst.operands[0] < regCount) &&
                                bcInst.typeIndex < module.types.size()) {
                                flat.flatOpcode = FlatOpcode::AsyncResolve;
                                flat.dstSlot = regSlot(static_cast<std::uint16_t>(bcInst.result));
                                flat.src0Slot = bcInst.operands.empty()
                                    ? InvalidCompactReg
                                    : regSlot(static_cast<std::uint16_t>(bcInst.operands[0]));
                                flat.inlineArgCount = static_cast<std::uint8_t>(bcInst.operands.size());
                                flat.integerImmediate = static_cast<std::int64_t>(module.types[bcInst.typeIndex].id);
                                break;
                            }
                            if (bcInst.opcode == bytecode::Opcode::LoadElement &&
                                bcInst.operands.size() == 2 &&
                                bcInst.operands[0] < regCount &&
                                bcInst.operands[1] < regCount &&
                                bcInst.result < regCount) {
                                flat.flatOpcode = FlatOpcode::LoadElementTypedFast;
                                flat.dstSlot = regSlot(static_cast<std::uint16_t>(bcInst.result));
                                flat.src0Slot = regSlot(static_cast<std::uint16_t>(bcInst.operands[0]));
                                flat.src1Slot = regSlot(static_cast<std::uint16_t>(bcInst.operands[1]));
                                flat.binaryOpcode1 = static_cast<QuickOpcode>(static_cast<std::uint8_t>(bcInst.elementType));
                                flat.integerImmediate = static_cast<std::int64_t>(bcInst.elementTypeId);
                                break;
                            }
                            if (bcInst.opcode == bytecode::Opcode::StoreElement &&
                                bcInst.operands.size() == 3 &&
                                bcInst.operands[0] < regCount &&
                                bcInst.operands[1] < regCount &&
                                bcInst.operands[2] < regCount) {
                                const auto mapPrimTag = [](semantic::PrimitiveType pt) noexcept -> std::uint8_t {
                                    switch (pt) {
                                    case semantic::PrimitiveType::Bool: return static_cast<std::uint8_t>(ValueTag::Bool);
                                    case semantic::PrimitiveType::Byte: return static_cast<std::uint8_t>(ValueTag::Byte);
                                    case semantic::PrimitiveType::SByte: return static_cast<std::uint8_t>(ValueTag::SByte);
                                    case semantic::PrimitiveType::Short: return static_cast<std::uint8_t>(ValueTag::Short);
                                    case semantic::PrimitiveType::UShort: return static_cast<std::uint8_t>(ValueTag::UShort);
                                    case semantic::PrimitiveType::Int: return static_cast<std::uint8_t>(ValueTag::Int64);
                                    case semantic::PrimitiveType::UInt: return static_cast<std::uint8_t>(ValueTag::UInt);
                                    case semantic::PrimitiveType::Long: return static_cast<std::uint8_t>(ValueTag::Long);
                                    case semantic::PrimitiveType::ULong: return static_cast<std::uint8_t>(ValueTag::ULong);
                                    case semantic::PrimitiveType::Float: return static_cast<std::uint8_t>(ValueTag::Float);
                                    case semantic::PrimitiveType::Double: return static_cast<std::uint8_t>(ValueTag::Double);
                                    case semantic::PrimitiveType::Char: return static_cast<std::uint8_t>(ValueTag::Char);
                                    default: return 0u;
                                    }
                                };
                                const std::uint8_t primTag = (bcInst.elementTypeId == 0)
                                    ? mapPrimTag(bcInst.elementType)
                                    : 0u;
                                if (primTag != 0u) {
                                    flat.flatOpcode = FlatOpcode::StoreElementPrimFast;
                                    flat.dstSlot = InvalidCompactReg;
                                    flat.src0Slot = regSlot(static_cast<std::uint16_t>(bcInst.operands[0]));
                                    flat.src1Slot = regSlot(static_cast<std::uint16_t>(bcInst.operands[1]));
                                    flat.src2Slot = regSlot(static_cast<std::uint16_t>(bcInst.operands[2]));
                                    flat.binaryOpcode1 = static_cast<QuickOpcode>(static_cast<std::uint8_t>(bcInst.elementType));
                                    flat.binaryOpcode2 = static_cast<QuickOpcode>(primTag);
                                    flat.integerImmediate = 0;
                                    break;
                                }
                                if (bcInst.elementType == semantic::PrimitiveType::Struct &&
                                    bcInst.elementTypeId != 0) {
                                    flat.flatOpcode = FlatOpcode::StoreElementStructFast;
                                    flat.dstSlot = InvalidCompactReg;
                                    flat.src0Slot = regSlot(static_cast<std::uint16_t>(bcInst.operands[0]));
                                    flat.src1Slot = regSlot(static_cast<std::uint16_t>(bcInst.operands[1]));
                                    flat.src2Slot = regSlot(static_cast<std::uint16_t>(bcInst.operands[2]));
                                    flat.integerImmediate = static_cast<std::int64_t>(bcInst.elementTypeId);
                                    if (bcInst.elementTypeId == fixedTypeId()) flat.aux0Slot = 1;
                                    else if (bcInst.elementTypeId == fixed16TypeId()) flat.aux0Slot = 2;
                                    else if (bcInst.elementTypeId == vec2TypeId()) flat.aux0Slot = 3;
                                    else if (bcInst.elementTypeId == vec3TypeId()) flat.aux0Slot = 4;
                                    else if (bcInst.elementTypeId == quatTypeId()) flat.aux0Slot = 5;
                                    else if (bcInst.elementTypeId == fixed64TypeId()) flat.aux0Slot = 6;
                                    else if (bcInst.elementTypeId == vec2fTypeId()) flat.aux0Slot = 7;
                                    else if (bcInst.elementTypeId == vec3fTypeId()) flat.aux0Slot = 8;
                                    else if (bcInst.elementTypeId == quatfTypeId()) flat.aux0Slot = 9;
                                    else flat.aux0Slot = 0;
                                    break;
                                }
                                if (bcInst.elementType == semantic::PrimitiveType::Object ||
                                    bcInst.elementType == semantic::PrimitiveType::String) {
                                    flat.flatOpcode = FlatOpcode::StoreElementObjFast;
                                    flat.dstSlot = InvalidCompactReg;
                                    flat.src0Slot = regSlot(static_cast<std::uint16_t>(bcInst.operands[0]));
                                    flat.src1Slot = regSlot(static_cast<std::uint16_t>(bcInst.operands[1]));
                                    flat.src2Slot = regSlot(static_cast<std::uint16_t>(bcInst.operands[2]));
                                    flat.binaryOpcode1 = static_cast<QuickOpcode>(static_cast<std::uint8_t>(bcInst.elementType));
                                    flat.integerImmediate = static_cast<std::int64_t>(bcInst.elementTypeId);
                                    break;
                                }
                            }
                        }
                        flat.flatOpcode = FlatOpcode::GenericBytecode;
                        break;
                    }
                    buildStream.push_back({flat, cold});
                    ++i;
                }

                // Pass 3b: Dead-temporary MoveSlot folding, CheckNotNull+CallPolled fusion & compound math fusion
                if (buildStream.size() > blockStreamStart + 1) {
                    const auto isDeadSingleUseRegSlot = [&](std::uint16_t slot) noexcept -> bool {
                        if (slot < execFunc.registerOffset || slot >= execFunc.scratchOffset) {
                            return false;
                        }
                        const std::size_t rIdx = static_cast<std::size_t>(slot - execFunc.registerOffset);
                        return rIdx < regCount && globalRegReads[rIdx] == 1;
                    };
                    const auto isLoadLocalOrParamMove = [](const FlatBuildItem& item) noexcept -> bool {
                        return item.hot.flatOpcode == FlatOpcode::MoveSlot &&
                            item.hot.span == 1 &&
                            item.cold.bytecode != nullptr &&
                            (item.cold.bytecode->opcode == bytecode::Opcode::LoadLocal ||
                             item.cold.bytecode->opcode == bytecode::Opcode::LoadParameter);
                    };
                    const auto isStoreLocalMove = [](const FlatBuildItem& item) noexcept -> bool {
                        return item.hot.flatOpcode == FlatOpcode::MoveSlot &&
                            item.hot.span == 1 &&
                            item.cold.bytecode != nullptr &&
                            item.cold.bytecode->opcode == bytecode::Opcode::StoreLocal;
                    };
                    const auto foldableArity = [](FlatOpcode op, bool& usesSpanOnlyCost) noexcept -> int {
                        usesSpanOnlyCost = false;
                        switch (op) {
                        case FlatOpcode::LoadFixedRawFast:
                        case FlatOpcode::LoadVec3FieldFast:
                        case FlatOpcode::LoadQuatFieldFast:
                        case FlatOpcode::SuperLoadVec3RawFast:
                        case FlatOpcode::SuperLoadQuatRawFast:
                        case FlatOpcode::NegateIntUnchecked:
                        case FlatOpcode::NegateDouble:
                        case FlatOpcode::NegateFloat:
                        case FlatOpcode::LogicalNot:
                        case FlatOpcode::ConvertIntToLongFast:
                        case FlatOpcode::ConvertIntToDoubleFast:
                        case FlatOpcode::ConvertLongToDoubleFast:
                        case FlatOpcode::ConvertIntToFloatFast:
                        case FlatOpcode::ConvertDoubleToFloatFast:
                        case FlatOpcode::ConvertFloatToIntFast:
                        case FlatOpcode::ConvertFloatToDoubleFast:
                        case FlatOpcode::ConvertLongToIntFast:
                        case FlatOpcode::ArrayLengthFast:
                        case FlatOpcode::NewObjectFast:
                            usesSpanOnlyCost = true;
                            return 0;
                        case FlatOpcode::CheckNotNullFast:
                        case FlatOpcode::IsTypeObjFast:
                        case FlatOpcode::AsTypeObjFast:
                        case FlatOpcode::LoadFieldFast:
                        case FlatOpcode::LoadFieldExactFast:
                        case FlatOpcode::NewArrayFast:
                            usesSpanOnlyCost = true;
                            return 1;
                        case FlatOpcode::AddIntChecked:
                        case FlatOpcode::AddIntUnchecked:
                        case FlatOpcode::SubIntChecked:
                        case FlatOpcode::SubIntUnchecked:
                        case FlatOpcode::MulIntChecked:
                        case FlatOpcode::MulIntUnchecked:
                        case FlatOpcode::DivInt:
                        case FlatOpcode::RemInt:
                        case FlatOpcode::LessInt:
                        case FlatOpcode::LessOrEqualInt:
                        case FlatOpcode::GreaterInt:
                        case FlatOpcode::GreaterOrEqualInt:
                        case FlatOpcode::EqualInt:
                        case FlatOpcode::NotEqualInt:
                        case FlatOpcode::AddDouble:
                        case FlatOpcode::SubDouble:
                        case FlatOpcode::MulDouble:
                        case FlatOpcode::DivDouble:
                        case FlatOpcode::LessDouble:
                        case FlatOpcode::LessOrEqualDouble:
                        case FlatOpcode::GreaterDouble:
                        case FlatOpcode::GreaterOrEqualDouble:
                        case FlatOpcode::AddFloat:
                        case FlatOpcode::SubFloat:
                        case FlatOpcode::MulFloat:
                        case FlatOpcode::DivFloat:
                        case FlatOpcode::LessFloat:
                        case FlatOpcode::LessOrEqualFloat:
                        case FlatOpcode::GreaterFloat:
                        case FlatOpcode::GreaterOrEqualFloat:
                        case FlatOpcode::EqualFloat:
                        case FlatOpcode::NotEqualFloat:
                        case FlatOpcode::LoadElementIntFast:
                        case FlatOpcode::LoadElementTypedFast:
                        case FlatOpcode::StoreFieldFast:
                        case FlatOpcode::StoreFieldExactFast:
                        case FlatOpcode::CombineDelegateFast:
                        case FlatOpcode::RemoveDelegateFast:
                            usesSpanOnlyCost = true;
                            return 2;
                        case FlatOpcode::StoreElementIntFast:
                        case FlatOpcode::StoreElementPrimFast:
                        case FlatOpcode::StoreElementStructFast:
                        case FlatOpcode::StoreElementObjFast:
                            usesSpanOnlyCost = true;
                            return 3;
                        case FlatOpcode::MathFixedConst:
                        case FlatOpcode::MathVec3Const:
                        case FlatOpcode::MathQuatIdentity:
                            return 0;
                        case FlatOpcode::MathFixedFromRaw:
                        case FlatOpcode::MathFixedFromInt:
                        case FlatOpcode::MathFixedNeg:
                        case FlatOpcode::MathFixedSin:
                        case FlatOpcode::MathFixedCos:
                        case FlatOpcode::MathFixedSqrt:
                        case FlatOpcode::MathSqrt64:
                        case FlatOpcode::MathVec3Neg:
                        case FlatOpcode::MathQuatNormalize:
                        case FlatOpcode::SuperQuatNormalizedBoxed:
                            return 1;
                        case FlatOpcode::MathFixedRatio:
                        case FlatOpcode::MathFixedAdd:
                        case FlatOpcode::MathFixedSub:
                        case FlatOpcode::MathFixedMul:
                        case FlatOpcode::MathFixedMulInt:
                        case FlatOpcode::MathFixedDiv:
                        case FlatOpcode::MathFixedDivInt:
                        case FlatOpcode::MathVec3Add:
                        case FlatOpcode::MathVec3Sub:
                        case FlatOpcode::MathVec3MulFixed:
                        case FlatOpcode::MathVec3MulInt:
                        case FlatOpcode::MathVec3DivFixed:
                        case FlatOpcode::MathVec3DivInt:
                        case FlatOpcode::MathVec3Dot:
                        case FlatOpcode::MathVec3Cross:
                        case FlatOpcode::MathVec2Add:
                        case FlatOpcode::MathVec2Sub:
                        case FlatOpcode::MathVec2MulFixed:
                        case FlatOpcode::MathVec2Dot:
                        case FlatOpcode::MathQuatDot:
                        case FlatOpcode::MathQuatMul:
                        case FlatOpcode::MathQuatRotate:
                        case FlatOpcode::MathQuatRotateVec3:
                        case FlatOpcode::SuperQuatRotateBoxed:
                        case FlatOpcode::SuperQuatMulNormalized:
                            return 2;
                        case FlatOpcode::MathVec3Raw:
                        case FlatOpcode::MathVec3Of:
                        case FlatOpcode::MathQuatFromEuler:
                        case FlatOpcode::MathQuatLerp:
                        case FlatOpcode::SuperFixedMulAdd:
                        case FlatOpcode::SuperVec3MulAdd:
                        case FlatOpcode::SuperVec3AddQuatRotate:
                        case FlatOpcode::SuperQuatMulChain:
                        case FlatOpcode::SuperQuatMulRotateVec3:
                            return 3;
                        default:
                            return -1;
                        }
                    };
                    const auto canAddMoveCost = [](const FlatInstruction& fi, bool usesSpanOnlyCost) noexcept -> bool {
                        if (fi.span >= 250u) return false;
                        if (usesSpanOnlyCost) return true;
                        if (static_cast<std::uint32_t>(fi.aux0Slot) + 1u >= InvalidCompactReg) return false;
                        if (fi.flatOpcode == FlatOpcode::MathFixedSqrt ||
                            fi.flatOpcode == FlatOpcode::MathSqrt64) {
                            const auto packed = static_cast<std::uint64_t>(fi.integerImmediate);
                            const auto negCost = (packed >> 8u) & 0xFFFFu;
                            if (negCost + 1u >= 0xFFFFu) return false;
                        }
                        return true;
                    };
                    const auto addMoveCost = [](FlatInstruction& fi, bool usesSpanOnlyCost) noexcept {
                        fi.span = static_cast<std::uint8_t>(fi.span + 1u);
                        if (!usesSpanOnlyCost) {
                            fi.aux0Slot = static_cast<std::uint16_t>(fi.aux0Slot + 1u);
                            if (fi.flatOpcode == FlatOpcode::MathFixedSqrt ||
                                fi.flatOpcode == FlatOpcode::MathSqrt64) {
                                const auto packed = static_cast<std::uint64_t>(fi.integerImmediate);
                                const auto iterCost = packed & 0xFFu;
                                const auto negCost = ((packed >> 8u) & 0xFFFFu) + 1u;
                                fi.integerImmediate = static_cast<std::int64_t>(
                                    iterCost | ((negCost & 0xFFFFu) << 8u));
                            }
                        }
                    };

                    const auto isDeadUnusedRegSlot = [&](std::uint16_t slot) noexcept -> bool {
                        if (slot < execFunc.registerOffset || slot >= execFunc.scratchOffset) {
                            return false;
                        }
                        const std::size_t rIdx = static_cast<std::size_t>(slot - execFunc.registerOffset);
                        return rIdx < regCount && globalRegReads[rIdx] == 0;
                    };
                    const auto runMoveSlotFolding = [&]() {
                        std::vector<FlatBuildItem> out;
                        const std::size_t curEnd = buildStream.size();
                        out.reserve(curEnd - blockStreamStart);
                        for (std::size_t idx = blockStreamStart; idx < curEnd; ++idx) {
                            FlatBuildItem cur = buildStream[idx];
                            // Fuse CheckNotNullFast(s0 -> deadReg, span>=1) + CallPolledFast(s0 == deadReg, 0..2 extra args)
                            if (!out.empty() &&
                                out.back().hot.flatOpcode == FlatOpcode::CheckNotNullFast &&
                                cur.hot.flatOpcode == FlatOpcode::CallPolledFast &&
                                cur.hot.span == 1 &&
                                static_cast<std::uint32_t>(out.back().hot.span) + 1u <= 250u &&
                                cur.hot.inlineArgCount >= 1u &&
                                cur.hot.inlineArgCount <= 3u &&
                                cur.hot.src0Slot == out.back().hot.dstSlot &&
                                isDeadSingleUseRegSlot(out.back().hot.dstSlot) &&
                                out.back().cold.instructionIndexInBlock + out.back().hot.span ==
                                    cur.cold.instructionIndexInBlock) {
                                FlatBuildItem fusedPolled = cur;
                                fusedPolled.hot.flatOpcode = FlatOpcode::SuperCheckNotNullCallPolledFast;
                                fusedPolled.hot.span = static_cast<std::uint8_t>(out.back().hot.span + 1u);
                                fusedPolled.hot.src0Slot = out.back().hot.src0Slot;
                                fusedPolled.cold.instructionIndexInBlock = out.back().cold.instructionIndexInBlock;
                                cur = fusedPolled;
                                out.pop_back();
                                while (!out.empty() &&
                                       isLoadLocalOrParamMove(out.back()) &&
                                       isDeadSingleUseRegSlot(out.back().hot.dstSlot) &&
                                       cur.hot.span < 250u &&
                                       out.back().cold.instructionIndexInBlock + out.back().hot.span ==
                                           cur.cold.instructionIndexInBlock) {
                                    const std::uint16_t deadReg = out.back().hot.dstSlot;
                                    const std::uint16_t replSlot = out.back().hot.src0Slot;
                                    bool matched = false;
                                    if (cur.hot.src0Slot == deadReg) {
                                        cur.hot.src0Slot = replSlot;
                                        matched = true;
                                    } else if (cur.hot.inlineArgCount >= 2u && cur.hot.src1Slot == deadReg) {
                                        cur.hot.src1Slot = replSlot;
                                        matched = true;
                                    } else if (cur.hot.inlineArgCount >= 3u && cur.hot.src2Slot == deadReg) {
                                        cur.hot.src2Slot = replSlot;
                                        matched = true;
                                    }
                                    if (!matched) break;
                                    cur.cold.instructionIndexInBlock = out.back().cold.instructionIndexInBlock;
                                    cur.hot.span = static_cast<std::uint8_t>(cur.hot.span + out.back().hot.span);
                                    out.pop_back();
                                }
                                out.push_back(cur);
                                continue;
                            }
                            bool spanOnly = false;
                            int arity = foldableArity(cur.hot.flatOpcode, spanOnly);
                            if (cur.hot.flatOpcode == FlatOpcode::NewDelegateFast &&
                                cur.hot.inlineArgCount <= 1u) {
                                spanOnly = true;
                                arity = static_cast<int>(cur.hot.inlineArgCount);
                            } else if ((cur.hot.flatOpcode == FlatOpcode::CallPolledFast ||
                                        cur.hot.flatOpcode == FlatOpcode::SuperCheckNotNullCallPolledFast ||
                                        cur.hot.flatOpcode == FlatOpcode::InvokeDelegateFast) &&
                                       cur.hot.inlineArgCount >= 1u &&
                                       cur.hot.inlineArgCount <= 3u) {
                                spanOnly = true;
                                arity = static_cast<int>(cur.hot.inlineArgCount);
                            }
                            if (arity >= 0) {
                                while (!out.empty()) {
                                    // Fold preceding CheckNotNullFast(s0 -> deadReg) directly into LoadFieldFast / StoreFieldFast!
                                    if ((cur.hot.flatOpcode == FlatOpcode::LoadFieldFast ||
                                         cur.hot.flatOpcode == FlatOpcode::LoadFieldExactFast ||
                                         cur.hot.flatOpcode == FlatOpcode::StoreFieldFast ||
                                         cur.hot.flatOpcode == FlatOpcode::StoreFieldExactFast) &&
                                        out.back().hot.flatOpcode == FlatOpcode::CheckNotNullFast &&
                                        cur.hot.src0Slot == out.back().hot.dstSlot &&
                                        isDeadSingleUseRegSlot(out.back().hot.dstSlot) &&
                                        static_cast<std::uint32_t>(cur.hot.span) + out.back().hot.span <= 250u &&
                                        out.back().cold.instructionIndexInBlock + out.back().hot.span ==
                                            cur.cold.instructionIndexInBlock) {
                                        cur.hot.src0Slot = out.back().hot.src0Slot;
                                        cur.cold.instructionIndexInBlock = out.back().cold.instructionIndexInBlock;
                                        cur.hot.span = static_cast<std::uint8_t>(cur.hot.span + out.back().hot.span);
                                        out.pop_back();
                                        continue;
                                    }
                                    if (isLoadLocalOrParamMove(out.back()) &&
                                        isDeadSingleUseRegSlot(out.back().hot.dstSlot) &&
                                        canAddMoveCost(cur.hot, spanOnly) &&
                                        out.back().cold.instructionIndexInBlock + out.back().hot.span ==
                                            cur.cold.instructionIndexInBlock) {
                                        const std::uint16_t deadReg = out.back().hot.dstSlot;
                                        const std::uint16_t replSlot = out.back().hot.src0Slot;
                                        bool matched = false;
                                        if (arity >= 1 && cur.hot.src0Slot == deadReg) {
                                            cur.hot.src0Slot = replSlot;
                                            matched = true;
                                        } else if (arity >= 2 && cur.hot.src1Slot == deadReg) {
                                            cur.hot.src1Slot = replSlot;
                                            matched = true;
                                        } else if (arity >= 3 && cur.hot.src2Slot == deadReg) {
                                            cur.hot.src2Slot = replSlot;
                                            matched = true;
                                        }
                                        if (matched) {
                                            cur.cold.instructionIndexInBlock = out.back().cold.instructionIndexInBlock;
                                            addMoveCost(cur.hot, spanOnly);
                                            out.pop_back();
                                            continue;
                                        }
                                    }
                                    break;
                                }
                                if (cur.hot.dstSlot != InvalidCompactReg &&
                                    cur.hot.flatOpcode != FlatOpcode::CallPolledFast &&
                                    cur.hot.flatOpcode != FlatOpcode::SuperCheckNotNullCallPolledFast &&
                                    cur.hot.flatOpcode != FlatOpcode::NewDelegateFast &&
                                    cur.hot.flatOpcode != FlatOpcode::CombineDelegateFast &&
                                    cur.hot.flatOpcode != FlatOpcode::RemoveDelegateFast &&
                                    cur.hot.flatOpcode != FlatOpcode::InvokeDelegateFast &&
                                    idx + 1 < curEnd &&
                                    isStoreLocalMove(buildStream[idx + 1]) &&
                                    buildStream[idx + 1].hot.src0Slot == cur.hot.dstSlot &&
                                    isDeadSingleUseRegSlot(cur.hot.dstSlot) &&
                                    canAddMoveCost(cur.hot, spanOnly) &&
                                    cur.cold.instructionIndexInBlock + cur.hot.span ==
                                        buildStream[idx + 1].cold.instructionIndexInBlock) {
                                    cur.hot.dstSlot = buildStream[idx + 1].hot.dstSlot;
                                    addMoveCost(cur.hot, spanOnly);
                                    ++idx;
                                }
                            }
                            out.push_back(cur);
                        }

                        // Intra-block redundant dead CheckNotNullFast absorption:
                        // When a slot S was already validated non-null & alive earlier in the block (without S being overwritten),
                        // any subsequent unused CheckNotNullFast(s0 == S, dst == unusedReg) can fold its span cost into the adjacent spanOnly op!
                        std::vector<std::uint8_t> provenNonNull(execFunc.totalFrameSlots, 0);
                        std::vector<FlatBuildItem> pruned;
                        pruned.reserve(out.size());
                        for (std::size_t idx = 0; idx < out.size(); ++idx) {
                            FlatBuildItem cur = out[idx];
                            if (cur.hot.flatOpcode == FlatOpcode::CheckNotNullFast &&
                                cur.hot.span == 1 &&
                                cur.hot.src0Slot < provenNonNull.size() &&
                                provenNonNull[cur.hot.src0Slot] != 0 &&
                                isDeadUnusedRegSlot(cur.hot.dstSlot) &&
                                idx + 1 < out.size()) {
                                bool nextSpanOnly = false;
                                const int nextArity = foldableArity(out[idx + 1].hot.flatOpcode, nextSpanOnly);
                                if (nextArity >= 0 && nextSpanOnly &&
                                    static_cast<std::uint32_t>(cur.hot.span) + out[idx + 1].hot.span <= 250u &&
                                    cur.cold.instructionIndexInBlock + cur.hot.span ==
                                        out[idx + 1].cold.instructionIndexInBlock) {
                                    out[idx + 1].cold.instructionIndexInBlock = cur.cold.instructionIndexInBlock;
                                    out[idx + 1].hot.span = static_cast<std::uint8_t>(
                                        out[idx + 1].hot.span + cur.hot.span);
                                    continue;
                                }
                            }
                            // Update provenNonNull state: first record validated source slot, then clear overwritten destination slots
                            if (cur.hot.flatOpcode == FlatOpcode::GenericBytecode) {
                                std::fill(provenNonNull.begin(), provenNonNull.end(), std::uint8_t{0});
                            } else {
                                const bool validatesSrc0 =
                                    cur.hot.flatOpcode == FlatOpcode::CheckNotNullFast ||
                                    cur.hot.flatOpcode == FlatOpcode::LoadFieldFast ||
                                    cur.hot.flatOpcode == FlatOpcode::LoadFieldExactFast ||
                                    cur.hot.flatOpcode == FlatOpcode::StoreFieldFast ||
                                    cur.hot.flatOpcode == FlatOpcode::StoreFieldExactFast ||
                                    cur.hot.flatOpcode == FlatOpcode::SuperCheckNotNullCallPolledFast ||
                                    cur.hot.flatOpcode == FlatOpcode::ArrayLengthFast ||
                                    cur.hot.flatOpcode == FlatOpcode::LoadElementIntFast ||
                                    cur.hot.flatOpcode == FlatOpcode::LoadElementTypedFast ||
                                    cur.hot.flatOpcode == FlatOpcode::StoreElementIntFast ||
                                    cur.hot.flatOpcode == FlatOpcode::StoreElementPrimFast ||
                                    cur.hot.flatOpcode == FlatOpcode::StoreElementStructFast ||
                                    cur.hot.flatOpcode == FlatOpcode::StoreElementObjFast ||
                                    cur.hot.flatOpcode == FlatOpcode::InvokeDelegateFast;
                                if (validatesSrc0 && cur.hot.src0Slot < provenNonNull.size()) {
                                    provenNonNull[cur.hot.src0Slot] = 1;
                                }
                                const std::uint8_t src0NonNull =
                                    (cur.hot.src0Slot < provenNonNull.size()) ? provenNonNull[cur.hot.src0Slot] : 0;
                                if (cur.hot.dstSlot < provenNonNull.size()) {
                                    provenNonNull[cur.hot.dstSlot] = 0;
                                }
                                if (cur.hot.storeSlot < provenNonNull.size()) {
                                    provenNonNull[cur.hot.storeSlot] = 0;
                                }
                                if (cur.hot.flatOpcode >= FlatOpcode::SuperBinaryLocalIntConst &&
                                    cur.hot.flatOpcode <= FlatOpcode::SuperLessLocalConstBranch) {
                                    if (cur.hot.aux0Slot < provenNonNull.size()) provenNonNull[cur.hot.aux0Slot] = 0;
                                    if (cur.hot.aux1Slot < provenNonNull.size()) provenNonNull[cur.hot.aux1Slot] = 0;
                                }
                                if (cur.hot.flatOpcode == FlatOpcode::CheckNotNullFast ||
                                    cur.hot.flatOpcode == FlatOpcode::NewArrayFast ||
                                    cur.hot.flatOpcode == FlatOpcode::NewObjectFast ||
                                    cur.hot.flatOpcode == FlatOpcode::NewDelegateFast) {
                                    if (cur.hot.dstSlot < provenNonNull.size()) {
                                        provenNonNull[cur.hot.dstSlot] = 1;
                                    }
                                } else if (cur.hot.flatOpcode == FlatOpcode::MoveSlot &&
                                           cur.hot.dstSlot < provenNonNull.size()) {
                                    provenNonNull[cur.hot.dstSlot] = src0NonNull;
                                }
                            }
                            pruned.push_back(cur);
                        }

                        buildStream.erase(
                            buildStream.begin() + static_cast<std::ptrdiff_t>(blockStreamStart),
                            buildStream.end());
                        buildStream.insert(buildStream.end(), pruned.begin(), pruned.end());
                    };

                    const auto runCompoundMathFusion = [&]() {
                        std::vector<FlatBuildItem> out;
                        const std::size_t curEnd = buildStream.size();
                        out.reserve(curEnd - blockStreamStart);
                        const auto combineCallMeta = [](std::uint16_t a1, std::uint16_t b1, std::uint16_t& outMeta) noexcept -> bool {
                            const std::uint32_t calls = (a1 & 0xFFu) + (b1 & 0xFFu);
                            const std::uint32_t depth = std::max<std::uint32_t>((a1 >> 8u) & 0xFFu, (b1 >> 8u) & 0xFFu);
                            if (calls > 250u || depth > 250u) return false;
                            outMeta = static_cast<std::uint16_t>((calls & 0xFFu) | ((depth & 0xFFu) << 8u));
                            return true;
                        };
                        for (std::size_t idx = blockStreamStart; idx < curEnd;) {
                            if (idx + 2 < curEnd) {
                                const auto& m0 = buildStream[idx];
                                const auto& f0 = buildStream[idx + 1];
                                const auto& f1 = buildStream[idx + 2];
                                if (isLoadLocalOrParamMove(m0) &&
                                    isDeadSingleUseRegSlot(m0.hot.dstSlot) &&
                                    isDeadSingleUseRegSlot(f0.hot.dstSlot) &&
                                    (f0.hot.storeSlot == InvalidCompactReg || f0.hot.storeSlot != m0.hot.src0Slot) &&
                                    m0.cold.instructionIndexInBlock + m0.hot.span == f0.cold.instructionIndexInBlock &&
                                    f0.cold.instructionIndexInBlock + f0.hot.span == f1.cold.instructionIndexInBlock &&
                                    static_cast<std::uint32_t>(m0.hot.span) + f0.hot.span + f1.hot.span <= 250u &&
                                    static_cast<std::uint32_t>(m0.hot.span) + f0.hot.aux0Slot + f1.hot.aux0Slot < InvalidCompactReg) {
                                    std::uint16_t mergedMeta = 0;
                                    const bool lhsAndRhsMatch =
                                        (f1.hot.src0Slot == m0.hot.dstSlot && f1.hot.src1Slot == f0.hot.dstSlot) ||
                                        (f1.hot.src1Slot == m0.hot.dstSlot && f1.hot.src0Slot == f0.hot.dstSlot);
                                    if (lhsAndRhsMatch && combineCallMeta(f0.hot.aux1Slot, f1.hot.aux1Slot, mergedMeta)) {
                                        FlatBuildItem fused = f1;
                                        fused.cold.instructionIndexInBlock = m0.cold.instructionIndexInBlock;
                                        fused.hot.span = static_cast<std::uint8_t>(m0.hot.span + f0.hot.span + f1.hot.span);
                                        fused.hot.aux0Slot = static_cast<std::uint16_t>(m0.hot.span + f0.hot.aux0Slot + f1.hot.aux0Slot);
                                        fused.hot.aux1Slot = mergedMeta;
                                        if (f0.hot.flatOpcode == FlatOpcode::MathVec3MulFixed &&
                                            f1.hot.flatOpcode == FlatOpcode::MathVec3Add) {
                                            fused.hot.flatOpcode = FlatOpcode::SuperVec3MulAdd;
                                            fused.hot.src0Slot = m0.hot.src0Slot;
                                            fused.hot.src1Slot = f0.hot.src0Slot;
                                            fused.hot.src2Slot = f0.hot.src1Slot;
                                            fused.hot.dstSlot = f1.hot.dstSlot;
                                            out.push_back(fused);
                                            idx += 3;
                                            continue;
                                        }
                                        if (f0.hot.flatOpcode == FlatOpcode::MathFixedMul &&
                                            f1.hot.flatOpcode == FlatOpcode::MathFixedAdd) {
                                            fused.hot.flatOpcode = FlatOpcode::SuperFixedMulAdd;
                                            fused.hot.src0Slot = m0.hot.src0Slot;
                                            fused.hot.src1Slot = f0.hot.src0Slot;
                                            fused.hot.src2Slot = f0.hot.src1Slot;
                                            fused.hot.dstSlot = f1.hot.dstSlot;
                                            out.push_back(fused);
                                            idx += 3;
                                            continue;
                                        }
                                        if ((f0.hot.flatOpcode == FlatOpcode::MathQuatRotateVec3 ||
                                             f0.hot.flatOpcode == FlatOpcode::SuperQuatRotateBoxed) &&
                                            f1.hot.flatOpcode == FlatOpcode::MathVec3Add) {
                                            fused.hot.flatOpcode = FlatOpcode::SuperVec3AddQuatRotate;
                                            fused.hot.src0Slot = m0.hot.src0Slot;
                                            fused.hot.src1Slot = f0.hot.src0Slot;
                                            fused.hot.src2Slot = f0.hot.src1Slot;
                                            fused.hot.storeSlot = (f0.hot.flatOpcode == FlatOpcode::SuperQuatRotateBoxed)
                                                ? f0.hot.storeSlot
                                                : InvalidCompactReg;
                                            fused.hot.dstSlot = f1.hot.dstSlot;
                                            out.push_back(fused);
                                            idx += 3;
                                            continue;
                                        }
                                    }
                                }
                            }

                            if (idx + 1 < curEnd) {
                                const auto& f0 = buildStream[idx];
                                const auto& f1 = buildStream[idx + 1];
                                if (isDeadSingleUseRegSlot(f0.hot.dstSlot) &&
                                    f0.cold.instructionIndexInBlock + f0.hot.span == f1.cold.instructionIndexInBlock &&
                                    static_cast<std::uint32_t>(f0.hot.span) + f1.hot.span <= 250u) {
                                    if ((f0.hot.flatOpcode == FlatOpcode::LoadVec3FieldFast ||
                                         f0.hot.flatOpcode == FlatOpcode::LoadQuatFieldFast) &&
                                        f1.hot.flatOpcode == FlatOpcode::LoadFixedRawFast &&
                                        f1.hot.src0Slot == f0.hot.dstSlot) {
                                        FlatBuildItem fused = f0;
                                        fused.hot.flatOpcode = (f0.hot.flatOpcode == FlatOpcode::LoadVec3FieldFast)
                                            ? FlatOpcode::SuperLoadVec3RawFast
                                            : FlatOpcode::SuperLoadQuatRawFast;
                                        fused.hot.span = static_cast<std::uint8_t>(f0.hot.span + f1.hot.span);
                                        fused.hot.dstSlot = f1.hot.dstSlot;
                                        out.push_back(fused);
                                        idx += 2;
                                        continue;
                                    }

                                    std::uint16_t mergedMeta = 0;
                                    if (static_cast<std::uint32_t>(f0.hot.aux0Slot) + f1.hot.aux0Slot < InvalidCompactReg &&
                                        combineCallMeta(f0.hot.aux1Slot, f1.hot.aux1Slot, mergedMeta)) {
                                        FlatBuildItem fused = f1;
                                        fused.cold.instructionIndexInBlock = f0.cold.instructionIndexInBlock;
                                        fused.hot.span = static_cast<std::uint8_t>(f0.hot.span + f1.hot.span);
                                        fused.hot.aux0Slot = static_cast<std::uint16_t>(f0.hot.aux0Slot + f1.hot.aux0Slot);
                                        fused.hot.aux1Slot = mergedMeta;

                                        if (f0.hot.flatOpcode == FlatOpcode::MathVec3MulFixed &&
                                            f1.hot.flatOpcode == FlatOpcode::MathVec3Add &&
                                            (f1.hot.src0Slot == f0.hot.dstSlot || f1.hot.src1Slot == f0.hot.dstSlot)) {
                                            fused.hot.flatOpcode = FlatOpcode::SuperVec3MulAdd;
                                            fused.hot.src0Slot = (f1.hot.src0Slot == f0.hot.dstSlot) ? f1.hot.src1Slot : f1.hot.src0Slot;
                                            fused.hot.src1Slot = f0.hot.src0Slot;
                                            fused.hot.src2Slot = f0.hot.src1Slot;
                                            fused.hot.dstSlot = f1.hot.dstSlot;
                                            out.push_back(fused);
                                            idx += 2;
                                            continue;
                                        }
                                        if (f0.hot.flatOpcode == FlatOpcode::MathFixedMul &&
                                            f1.hot.flatOpcode == FlatOpcode::MathFixedAdd &&
                                            (f1.hot.src0Slot == f0.hot.dstSlot || f1.hot.src1Slot == f0.hot.dstSlot)) {
                                            fused.hot.flatOpcode = FlatOpcode::SuperFixedMulAdd;
                                            fused.hot.src0Slot = (f1.hot.src0Slot == f0.hot.dstSlot) ? f1.hot.src1Slot : f1.hot.src0Slot;
                                            fused.hot.src1Slot = f0.hot.src0Slot;
                                            fused.hot.src2Slot = f0.hot.src1Slot;
                                            fused.hot.dstSlot = f1.hot.dstSlot;
                                            out.push_back(fused);
                                            idx += 2;
                                            continue;
                                        }
                                        if ((f0.hot.flatOpcode == FlatOpcode::MathQuatRotateVec3 ||
                                             f0.hot.flatOpcode == FlatOpcode::SuperQuatRotateBoxed) &&
                                            f1.hot.flatOpcode == FlatOpcode::MathVec3Add &&
                                            (f1.hot.src0Slot == f0.hot.dstSlot || f1.hot.src1Slot == f0.hot.dstSlot)) {
                                            const std::uint16_t aSlot =
                                                (f1.hot.src0Slot == f0.hot.dstSlot) ? f1.hot.src1Slot : f1.hot.src0Slot;
                                            const std::uint16_t rotStore =
                                                (f0.hot.flatOpcode == FlatOpcode::SuperQuatRotateBoxed)
                                                    ? f0.hot.storeSlot
                                                    : InvalidCompactReg;
                                            if (rotStore == InvalidCompactReg || rotStore != aSlot) {
                                                fused.hot.flatOpcode = FlatOpcode::SuperVec3AddQuatRotate;
                                                fused.hot.src0Slot = aSlot;
                                                fused.hot.src1Slot = f0.hot.src0Slot;
                                                fused.hot.src2Slot = f0.hot.src1Slot;
                                                fused.hot.storeSlot = rotStore;
                                                fused.hot.dstSlot = f1.hot.dstSlot;
                                                out.push_back(fused);
                                                idx += 2;
                                                continue;
                                            }
                                        }
                                        if (f0.hot.flatOpcode == FlatOpcode::MathQuatMul &&
                                            (f1.hot.flatOpcode == FlatOpcode::SuperQuatNormalizedBoxed ||
                                             f1.hot.flatOpcode == FlatOpcode::MathQuatNormalize) &&
                                            f1.hot.src0Slot == f0.hot.dstSlot) {
                                            fused.hot.flatOpcode = FlatOpcode::SuperQuatMulNormalized;
                                            fused.hot.src0Slot = f0.hot.src0Slot;
                                            fused.hot.src1Slot = f0.hot.src1Slot;
                                            fused.hot.storeSlot = (f1.hot.flatOpcode == FlatOpcode::SuperQuatNormalizedBoxed)
                                                ? f1.hot.storeSlot
                                                : InvalidCompactReg;
                                            fused.hot.integerImmediate = f1.hot.integerImmediate;
                                            fused.hot.dstSlot = f1.hot.dstSlot;
                                            out.push_back(fused);
                                            idx += 2;
                                            continue;
                                        }
                                        if (f0.hot.flatOpcode == FlatOpcode::MathQuatMul &&
                                            f1.hot.flatOpcode == FlatOpcode::MathQuatMul &&
                                            f1.hot.src0Slot == f0.hot.dstSlot) {
                                            fused.hot.flatOpcode = FlatOpcode::SuperQuatMulChain;
                                            fused.hot.src0Slot = f0.hot.src0Slot;
                                            fused.hot.src1Slot = f0.hot.src1Slot;
                                            fused.hot.src2Slot = f1.hot.src1Slot;
                                            fused.hot.dstSlot = f1.hot.dstSlot;
                                            out.push_back(fused);
                                            idx += 2;
                                            continue;
                                        }
                                        if (f0.hot.flatOpcode == FlatOpcode::MathQuatMul &&
                                            ((f1.hot.flatOpcode == FlatOpcode::MathQuatRotateVec3) ||
                                             (f1.hot.flatOpcode == FlatOpcode::SuperQuatRotateBoxed &&
                                              f1.hot.storeSlot == InvalidCompactReg)) &&
                                            f1.hot.src0Slot == f0.hot.dstSlot) {
                                            fused.hot.flatOpcode = FlatOpcode::SuperQuatMulRotateVec3;
                                            fused.hot.src0Slot = f0.hot.src0Slot;
                                            fused.hot.src1Slot = f0.hot.src1Slot;
                                            fused.hot.src2Slot = f1.hot.src1Slot;
                                            fused.hot.dstSlot = f1.hot.dstSlot;
                                            out.push_back(fused);
                                            idx += 2;
                                            continue;
                                        }
                                    }
                                }
                            }

                            out.push_back(buildStream[idx]);
                            ++idx;
                        }
                        buildStream.erase(
                            buildStream.begin() + static_cast<std::ptrdiff_t>(blockStreamStart),
                            buildStream.end());
                        buildStream.insert(buildStream.end(), out.begin(), out.end());
                    };

                    runMoveSlotFolding();
                    runCompoundMathFusion();
                    runMoveSlotFolding();
                }

                // Fuse block-ending SuperBinaryLocalInt*StoreElided + unconditional TermJumpNoArgs back-edges!
                if (blk.terminatorKind == bytecode::TerminatorKind::Jump &&
                    !blk.branch.hasArguments &&
                    blk.branch.target != InvalidBlockIndex &&
                    buildStream.size() > blockStreamStart) {
                    auto& tail = buildStream.back();
                    if ((tail.hot.flatOpcode == FlatOpcode::SuperBinaryLocalIntConstStoreElided ||
                         tail.hot.flatOpcode == FlatOpcode::SuperBinaryLocalIntLocalStoreElided ||
                         tail.hot.flatOpcode == FlatOpcode::SuperBinaryLocalIntRegStoreElided) &&
                        tail.hot.span < 250u &&
                        tail.cold.instructionIndexInBlock + tail.hot.span == nInst) {
                        const std::uint16_t targetLocal = tail.hot.storeSlot;
                        if (tail.hot.flatOpcode == FlatOpcode::SuperBinaryLocalIntConstStoreElided) {
                            tail.hot.flatOpcode = FlatOpcode::SuperBinaryLocalIntConstStoreJump;
                        } else if (tail.hot.flatOpcode == FlatOpcode::SuperBinaryLocalIntLocalStoreElided) {
                            tail.hot.flatOpcode = FlatOpcode::SuperBinaryLocalIntLocalStoreJump;
                        } else {
                            tail.hot.flatOpcode = FlatOpcode::SuperBinaryLocalIntRegStoreJump;
                        }
                        tail.hot.dstSlot = targetLocal;
                        tail.hot.span = static_cast<std::uint8_t>(tail.hot.span + 1u);
                        tail.hot.trueTargetOffset = InvalidStreamOffset;
                        tail.hot.falseTargetOffset = InvalidStreamOffset;
                        goto next_block;
                    }
                }

                // Append block terminator as a FlatInstruction
                {
                    FlatInstruction termInst;
                    FlatColdMeta termCold;
                    termCold.blockIndex = static_cast<std::uint32_t>(bIdx);
                    termCold.instructionIndexInBlock = static_cast<std::uint32_t>(nInst);
                    termInst.span = 1;
                    switch (blk.terminatorKind) {
                    case bytecode::TerminatorKind::Jump:
                        termInst.trueTargetOffset = InvalidStreamOffset;
                        termInst.falseTargetOffset = InvalidStreamOffset;
                        if (!blk.branch.hasArguments &&
                            blk.branch.target != InvalidBlockIndex) {
                            termInst.flatOpcode = FlatOpcode::TermJumpNoArgs;
                        } else {
                            termInst.flatOpcode = FlatOpcode::TermBranchGeneric;
                        }
                        break;
                    case bytecode::TerminatorKind::Branch:
                        termInst.trueTargetOffset = InvalidStreamOffset;
                        termInst.falseTargetOffset = InvalidStreamOffset;
                        if (blk.conditionPrevalidatedBool &&
                            !blk.branch.hasArguments &&
                            !blk.falseBranch.hasArguments &&
                            blk.branch.target != InvalidBlockIndex &&
                            blk.falseBranch.target != InvalidBlockIndex &&
                            blk.terminatorReg < regCount) {
                            termInst.flatOpcode = FlatOpcode::TermBranchBoolNoArgs;
                            termInst.src0Slot = regSlot(static_cast<std::uint16_t>(blk.terminatorReg));
                        } else {
                            termInst.flatOpcode = FlatOpcode::TermBranchGeneric;
                        }
                        break;
                    case bytecode::TerminatorKind::ReturnVoid:
                        termInst.flatOpcode = FlatOpcode::TermReturnVoid;
                        break;
                    case bytecode::TerminatorKind::ReturnValue:
                        if (blk.returnPrevalidatedScalar &&
                            blk.terminatorReg < regCount) {
                            termInst.flatOpcode = FlatOpcode::TermReturnScalarFast;
                            termInst.src0Slot = regSlot(static_cast<std::uint16_t>(blk.terminatorReg));
                        } else {
                            termInst.flatOpcode = FlatOpcode::TermReturnGeneric;
                        }
                        break;
                    case bytecode::TerminatorKind::Throw:
                    default:
                        termInst.flatOpcode = FlatOpcode::TermThrow;
                        break;
                    }
                    buildStream.push_back({termInst, termCold});
                }
            next_block:;
            }

            // Patch block stream offsets into branch/jump instructions
            execFunc.entryStreamOffset = execFunc.blockStreamOffsets[execFunc.entry];
            const auto isBranchOrJumpFlatOp = [](FlatOpcode op) noexcept -> bool {
                return op == FlatOpcode::TermJumpNoArgs ||
                    op == FlatOpcode::TermBranchBoolNoArgs ||
                    op == FlatOpcode::TermBranchGeneric ||
                    op == FlatOpcode::SuperLessLocalRegBranch ||
                    op == FlatOpcode::SuperLessLocalConstBranch ||
                    op == FlatOpcode::SuperLessLocalArrayLengthBranch ||
                    op == FlatOpcode::SuperBinaryLocalIntConstStoreJump ||
                    op == FlatOpcode::SuperBinaryLocalIntLocalStoreJump ||
                    op == FlatOpcode::SuperBinaryLocalIntRegStoreJump;
            };
            for (auto& item : buildStream) {
                if (!isBranchOrJumpFlatOp(item.hot.flatOpcode)) continue;
                const auto& blk = execFunc.blocks[item.cold.blockIndex];
                if (blk.branch.target != InvalidBlockIndex &&
                    blk.branch.target < execFunc.blockStreamOffsets.size()) {
                    item.hot.trueTargetOffset = execFunc.blockStreamOffsets[blk.branch.target];
                }
                if (blk.falseBranch.target != InvalidBlockIndex &&
                    blk.falseBranch.target < execFunc.blockStreamOffsets.size()) {
                    item.hot.falseTargetOffset = execFunc.blockStreamOffsets[blk.falseBranch.target];
                }
            }

            // Pass 3c: Static Natural-Loop Redundant Non-Null & Bounds-Check Elimination (BCE)
            if (!execFunc.hasExceptionHandlers && execFunc.blocks.size() >= 3 && !buildStream.empty()) {
                const std::size_t nBlocks = execFunc.blocks.size();
                std::vector<std::vector<std::size_t>> preds(nBlocks);
                for (std::size_t b = 0; b < nBlocks; ++b) {
                    const auto& eb = execFunc.blocks[b];
                    if (eb.branch.target != InvalidBlockIndex && eb.branch.target < nBlocks) {
                        preds[eb.branch.target].push_back(b);
                    }
                    if (eb.falseBranch.target != InvalidBlockIndex && eb.falseBranch.target < nBlocks) {
                        preds[eb.falseBranch.target].push_back(b);
                    }
                }
                std::vector<std::size_t> blkEnd(nBlocks, buildStream.size());
                for (std::size_t b = 0; b + 1 < nBlocks; ++b) {
                    blkEnd[b] = execFunc.blockStreamOffsets[b + 1];
                }
                const auto instWritesAux = [](FlatOpcode op, bool& wAux0, bool& wAux1) noexcept {
                    wAux0 = false;
                    wAux1 = false;
                    switch (op) {
                    case FlatOpcode::SuperBinaryLocalIntConst:
                    case FlatOpcode::SuperBinaryLocalIntConstStore:
                    case FlatOpcode::SuperBinaryLocalIntLocal:
                    case FlatOpcode::SuperBinaryLocalIntLocalStore:
                    case FlatOpcode::SuperBinaryLocalIntRegChainStore:
                    case FlatOpcode::SuperLessLocalConstBranch:
                    case FlatOpcode::SuperBinaryLocalDoubleConstStore:
                    case FlatOpcode::SuperBinaryLocalDoubleLocalStore:
                    case FlatOpcode::SuperBinaryLocalFloatLocalStore:
                        wAux0 = true;
                        wAux1 = true;
                        break;
                    case FlatOpcode::SuperBinaryLocalIntReg:
                    case FlatOpcode::SuperBinaryLocalIntRegStore:
                    case FlatOpcode::SuperLessLocalRegBranch:
                    case FlatOpcode::SuperBinaryLocalFloatReg:
                    case FlatOpcode::SuperBinaryLocalFloatRegElided:
                    case FlatOpcode::SuperBinaryLocalFloatRegStore:
                        wAux1 = true;
                        break;
                    default:
                        break;
                    }
                };
                const auto instWritesSlot = [&](const FlatInstruction& fi, std::uint16_t slot) noexcept -> bool {
                    if (fi.dstSlot == slot) return true;
                    if (!isBranchOrJumpFlatOp(fi.flatOpcode) && fi.storeSlot == slot) return true;
                    bool wAux0 = false, wAux1 = false;
                    instWritesAux(fi.flatOpcode, wAux0, wAux1);
                    if (wAux0 && fi.aux0Slot == slot) return true;
                    if (wAux1 && fi.aux1Slot == slot) return true;
                    return false;
                };

                for (std::size_t latchBlk = 2; latchBlk < nBlocks; ++latchBlk) {
                    const auto& latchEb = execFunc.blocks[latchBlk];
                    if (latchEb.terminatorKind != bytecode::TerminatorKind::Jump ||
                        latchEb.branch.hasArguments ||
                        latchEb.branch.target == InvalidBlockIndex ||
                        latchEb.branch.target == 0 ||
                        latchEb.branch.target >= latchBlk) {
                        continue;
                    }
                    const std::size_t headerBlk = latchEb.branch.target;
                    const std::size_t preBlk = headerBlk - 1;
                    if (execFunc.blocks[preBlk].terminatorKind != bytecode::TerminatorKind::Jump ||
                        execFunc.blocks[preBlk].branch.hasArguments ||
                        execFunc.blocks[preBlk].branch.target != headerBlk) {
                        continue;
                    }

                    // Verify single-entry natural loop [headerBlk .. latchBlk] with acyclic body before latchBlk
                    bool validLoopCfg = true;
                    for (const auto p : preds[headerBlk]) {
                        if (p != preBlk && (p < headerBlk || p > latchBlk)) {
                            validLoopCfg = false;
                            break;
                        }
                    }
                    for (std::size_t b = headerBlk + 1; validLoopCfg && b <= latchBlk; ++b) {
                        for (const auto p : preds[b]) {
                            if (p < headerBlk || p >= b) {
                                // Only forward edges inside (headerBlk .. latchBlk] so headerBlk strictly dominates all body blocks
                                validLoopCfg = false;
                                break;
                            }
                        }
                    }
                    if (!validLoopCfg) continue;

                    const std::size_t hStart = execFunc.blockStreamOffsets[headerBlk];
                    const std::size_t hEnd = blkEnd[headerBlk];
                    if (hEnd != hStart + 1) continue;
                    const auto& headerInst = buildStream[hStart].hot;

                    const auto& headerEb = execFunc.blocks[headerBlk];
                    const bool trueEntersLoop =
                        headerEb.branch.target > headerBlk && headerEb.branch.target <= latchBlk;
                    const bool falseExitsLoop =
                        headerEb.falseBranch.target > latchBlk || headerEb.falseBranch.target < headerBlk;
                    if (!trueEntersLoop || !falseExitsLoop) continue;

                    // Track integer constants & fixed-size NewArray allocations at end of preBlk
                    std::unordered_map<std::uint16_t, std::int64_t> preSlotConst;
                    std::unordered_map<std::uint16_t, std::int64_t> preArrayLen;
                    for (std::size_t k = execFunc.blockStreamOffsets[preBlk]; k < blkEnd[preBlk]; ++k) {
                        const auto& pfi = buildStream[k].hot;
                        if (pfi.flatOpcode == FlatOpcode::ConstantInt && pfi.dstSlot != InvalidCompactReg) {
                            preSlotConst[pfi.dstSlot] = pfi.integerImmediate;
                            preArrayLen.erase(pfi.dstSlot);
                        } else if (pfi.flatOpcode == FlatOpcode::MoveSlot && pfi.dstSlot != InvalidCompactReg) {
                            const auto itC = preSlotConst.find(pfi.src0Slot);
                            if (itC != preSlotConst.end()) preSlotConst[pfi.dstSlot] = itC->second;
                            else preSlotConst.erase(pfi.dstSlot);
                            const auto itA = preArrayLen.find(pfi.src0Slot);
                            if (itA != preArrayLen.end()) preArrayLen[pfi.dstSlot] = itA->second;
                            else preArrayLen.erase(pfi.dstSlot);
                        } else if (pfi.flatOpcode == FlatOpcode::NewArrayFast && pfi.dstSlot != InvalidCompactReg) {
                            preSlotConst.erase(pfi.dstSlot);
                            const auto itLen = preSlotConst.find(pfi.src0Slot);
                            if (itLen != preSlotConst.end() && itLen->second > 0) {
                                preArrayLen[pfi.dstSlot] = itLen->second;
                            } else {
                                preArrayLen.erase(pfi.dstSlot);
                            }
                        } else {
                            if (pfi.dstSlot != InvalidCompactReg) {
                                preSlotConst.erase(pfi.dstSlot);
                                preArrayLen.erase(pfi.dstSlot);
                            }
                            if (!isBranchOrJumpFlatOp(pfi.flatOpcode) && pfi.storeSlot != InvalidCompactReg) {
                                preSlotConst.erase(pfi.storeSlot);
                                preArrayLen.erase(pfi.storeSlot);
                            }
                            bool wAux0 = false, wAux1 = false;
                            instWritesAux(pfi.flatOpcode, wAux0, wAux1);
                            if (wAux0 && pfi.aux0Slot != InvalidCompactReg) {
                                preSlotConst.erase(pfi.aux0Slot);
                                preArrayLen.erase(pfi.aux0Slot);
                            }
                            if (wAux1 && pfi.aux1Slot != InvalidCompactReg) {
                                preSlotConst.erase(pfi.aux1Slot);
                                preArrayLen.erase(pfi.aux1Slot);
                            }
                        }
                    }

                    std::uint16_t iSlot = InvalidCompactReg;
                    std::vector<std::uint16_t> provenArrSlots;
                    if (headerInst.flatOpcode == FlatOpcode::SuperLessLocalArrayLengthBranch &&
                        headerInst.binaryOpcode1 == QuickOpcode::LessInt) {
                        iSlot = headerInst.src0Slot;
                        provenArrSlots.push_back(headerInst.src1Slot);
                    } else if (headerInst.binaryOpcode1 == QuickOpcode::LessInt &&
                               (headerInst.flatOpcode == FlatOpcode::SuperLessLocalConstBranch ||
                                headerInst.flatOpcode == FlatOpcode::SuperLessLocalRegBranch)) {
                        std::int64_t boundN = -1;
                        if (headerInst.flatOpcode == FlatOpcode::SuperLessLocalConstBranch) {
                            boundN = headerInst.integerImmediate;
                        } else {
                            const auto itB = preSlotConst.find(headerInst.src1Slot);
                            if (itB != preSlotConst.end()) {
                                bool rhsModified = false;
                                for (std::size_t k = hStart; k < blkEnd[latchBlk]; ++k) {
                                    if (instWritesSlot(buildStream[k].hot, headerInst.src1Slot)) {
                                        rhsModified = true;
                                        break;
                                    }
                                }
                                if (!rhsModified) boundN = itB->second;
                            }
                        }
                        if (boundN > 0) {
                            iSlot = headerInst.src0Slot;
                            for (const auto& [aSlot, aLen] : preArrayLen) {
                                if (aLen >= boundN) provenArrSlots.push_back(aSlot);
                            }
                        }
                    }

                    if (iSlot == InvalidCompactReg || provenArrSlots.empty()) continue;
                    const auto itInit = preSlotConst.find(iSlot);
                    if (itInit == preSlotConst.end() || itInit->second < 0) continue;

                    // Verify latchBlk is a single checked positive increment `iSlot = iSlot + posStep; jump header`
                    const std::size_t lStart = execFunc.blockStreamOffsets[latchBlk];
                    const std::size_t lEnd = blkEnd[latchBlk];
                    if (lEnd != lStart + 1) continue;
                    const auto& latchInst = buildStream[lStart].hot;
                    bool validPositiveCheckedStep = false;
                    if (latchInst.checked1() &&
                        latchInst.binaryOpcode1 == QuickOpcode::AddInt &&
                        latchInst.dstSlot == iSlot &&
                        latchInst.src0Slot == iSlot) {
                        if (latchInst.flatOpcode == FlatOpcode::SuperBinaryLocalIntConstStoreJump &&
                            latchInst.integerImmediate > 0) {
                            validPositiveCheckedStep = true;
                        } else if (latchInst.flatOpcode == FlatOpcode::SuperBinaryLocalIntRegStoreJump) {
                            const auto itStep = preSlotConst.find(latchInst.src1Slot);
                            if (itStep != preSlotConst.end() && itStep->second > 0) {
                                bool stepModified = false;
                                for (std::size_t k = hStart; k < lEnd; ++k) {
                                    if (instWritesSlot(buildStream[k].hot, latchInst.src1Slot)) {
                                        stepModified = true;
                                        break;
                                    }
                                }
                                if (!stepModified) validPositiveCheckedStep = true;
                            }
                        }
                    }
                    if (!validPositiveCheckedStep) continue;

                    // Verify iSlot is NOT written in [hStart .. lStart) and no GenericBytecode or TermBranchGeneric exists in loop
                    bool bodyClean = true;
                    for (std::size_t k = hStart; k < lStart; ++k) {
                        if (buildStream[k].hot.flatOpcode == FlatOpcode::GenericBytecode ||
                            buildStream[k].hot.flatOpcode == FlatOpcode::TermBranchGeneric ||
                            instWritesSlot(buildStream[k].hot, iSlot)) {
                            bodyClean = false;
                            break;
                        }
                    }
                    if (!bodyClean) continue;

                    for (const std::uint16_t arrSlot : provenArrSlots) {
                        bool arrClean = true;
                        for (std::size_t k = hStart; k < lEnd; ++k) {
                            if (instWritesSlot(buildStream[k].hot, arrSlot)) {
                                arrClean = false;
                                break;
                            }
                        }
                        if (!arrClean) continue;

                        // Also propagate simple intra-block MoveSlot copies of arrSlot & iSlot (for `arr[i] = arr[i] + 1`)
                        std::vector<std::uint16_t> slotOrigin(execFunc.totalFrameSlots);
                        for (std::uint16_t sIdx = 0; sIdx < execFunc.totalFrameSlots; ++sIdx) slotOrigin[sIdx] = sIdx;
                        std::uint32_t curBlk = 0xffffffffu;
                        for (std::size_t k = hEnd; k < lStart; ++k) {
                            if (buildStream[k].cold.blockIndex != curBlk) {
                                curBlk = buildStream[k].cold.blockIndex;
                                for (std::uint16_t sIdx = 0; sIdx < execFunc.totalFrameSlots; ++sIdx) slotOrigin[sIdx] = sIdx;
                            }
                            auto& bfi = buildStream[k].hot;
                            const std::uint16_t effArr = (bfi.src0Slot < slotOrigin.size()) ? slotOrigin[bfi.src0Slot] : bfi.src0Slot;
                            const std::uint16_t effIdx = (bfi.src1Slot < slotOrigin.size()) ? slotOrigin[bfi.src1Slot] : bfi.src1Slot;
                            if ((bfi.flatOpcode == FlatOpcode::LoadElementIntFast ||
                                 bfi.flatOpcode == FlatOpcode::LoadElementTypedFast ||
                                 bfi.flatOpcode == FlatOpcode::StoreElementIntFast ||
                                 bfi.flatOpcode == FlatOpcode::StoreElementPrimFast ||
                                 bfi.flatOpcode == FlatOpcode::StoreElementStructFast ||
                                 bfi.flatOpcode == FlatOpcode::StoreElementObjFast) &&
                                effArr == arrSlot) {
                                bfi.arithFlags = static_cast<std::uint8_t>(
                                    bfi.arithFlags | FlatInstruction::FlagArrayNoNullCheck);
                                if (effIdx == iSlot) {
                                    bfi.arithFlags = static_cast<std::uint8_t>(
                                        bfi.arithFlags | FlatInstruction::FlagArrayNoBoundsCheck);
                                }
                            }
                            if (bfi.flatOpcode == FlatOpcode::MoveSlot &&
                                bfi.dstSlot < slotOrigin.size() &&
                                bfi.src0Slot < slotOrigin.size()) {
                                slotOrigin[bfi.dstSlot] = slotOrigin[bfi.src0Slot];
                            } else {
                                if (bfi.dstSlot < slotOrigin.size()) slotOrigin[bfi.dstSlot] = bfi.dstSlot;
                                if (!isBranchOrJumpFlatOp(bfi.flatOpcode) && bfi.storeSlot < slotOrigin.size()) {
                                    slotOrigin[bfi.storeSlot] = bfi.storeSlot;
                                }
                                bool wAux0 = false, wAux1 = false;
                                instWritesAux(bfi.flatOpcode, wAux0, wAux1);
                                if (wAux0 && bfi.aux0Slot < slotOrigin.size()) slotOrigin[bfi.aux0Slot] = bfi.aux0Slot;
                                if (wAux1 && bfi.aux1Slot < slotOrigin.size()) slotOrigin[bfi.aux1Slot] = bfi.aux1Slot;
                            }
                        }
                    }
                }
            }

            // Pass 3d (P0-B): Forward CFG Slot-Type & Definite-Initialization Proof Pass
            // Eliminates redundant dynamic ValueTag checks on verified scalar registers/locals.
            if (!execFunc.hasExceptionHandlers && !buildStream.empty() && execFunc.totalFrameSlots > 0 &&
                execFunc.totalFrameSlots <= 1024) {
                enum class ProvenTag : std::uint8_t {
                    Uninit = 0,
                    ExactInt32 = 1,
                    ExactDouble = 2,
                    ExactFloat = 3,
                    ExactBool = 4,
                    Mixed = 5,
                };
                const std::size_t nBlocks = execFunc.blocks.size();
                const std::size_t nSlots = execFunc.totalFrameSlots;
                const auto mergeTag = [](ProvenTag a, ProvenTag b) noexcept -> ProvenTag {
                    if (a == b) return a;
                    return ProvenTag::Mixed;
                };

                std::vector<std::size_t> blkEnd(nBlocks, buildStream.size());
                for (std::size_t b = 0; b + 1 < nBlocks; ++b) {
                    blkEnd[b] = execFunc.blockStreamOffsets[b + 1];
                }

                // Initialize entry block with verified entry parameter types; non-entry blocks start clean for intersection
                std::vector<std::vector<ProvenTag>> blkIn(nBlocks, std::vector<ProvenTag>(nSlots, ProvenTag::Uninit));
                std::vector<std::vector<ProvenTag>> blkOut(nBlocks, std::vector<ProvenTag>(nSlots, ProvenTag::Uninit));
                std::vector<bool> blkVisited(nBlocks, false);
                if (execFunc.entry < nBlocks) {
                    blkVisited[execFunc.entry] = true;
                    auto& entryState = blkIn[execFunc.entry];
                    for (std::size_t p = 0; p < paramCount && (execFunc.paramOffset + p) < nSlots; ++p) {
                        switch (function.parameterTypes[p]) {
                        case semantic::PrimitiveType::Int:
                            entryState[execFunc.paramOffset + p] = ProvenTag::ExactInt32;
                            break;
                        case semantic::PrimitiveType::Double:
                            entryState[execFunc.paramOffset + p] = ProvenTag::ExactDouble;
                            break;
                        case semantic::PrimitiveType::Float:
                            entryState[execFunc.paramOffset + p] = ProvenTag::ExactFloat;
                            break;
                        case semantic::PrimitiveType::Bool:
                            entryState[execFunc.paramOffset + p] = ProvenTag::ExactBool;
                            break;
                        default:
                            entryState[execFunc.paramOffset + p] = ProvenTag::Mixed;
                            break;
                        }
                    }
                }

                const auto stepInstState = [&](const FlatInstruction& fi, std::vector<ProvenTag>& st, bool rewrite) {
                    const auto isInt = [&](std::uint16_t s) noexcept {
                        return s < nSlots && st[s] == ProvenTag::ExactInt32;
                    };
                    const auto isDbl = [&](std::uint16_t s) noexcept {
                        return s < nSlots && st[s] == ProvenTag::ExactDouble;
                    };
                    const auto isFlt = [&](std::uint16_t s) noexcept {
                        return s < nSlots && st[s] == ProvenTag::ExactFloat;
                    };
                    const auto setSlot = [&](std::uint16_t s, ProvenTag t) noexcept {
                        if (s < nSlots) st[s] = t;
                    };

                    FlatInstruction out = fi;
                    switch (fi.flatOpcode) {
                    case FlatOpcode::MoveSlot:
                        if (fi.dstSlot < nSlots) {
                            st[fi.dstSlot] = (fi.src0Slot < nSlots) ? st[fi.src0Slot] : ProvenTag::Mixed;
                        }
                        break;
                    case FlatOpcode::SuperMoveParamStoreLocal:
                        if (fi.src0Slot < nSlots) {
                            const ProvenTag pt = st[fi.src0Slot];
                            setSlot(fi.dstSlot, pt);
                            setSlot(fi.storeSlot, pt);
                        }
                        break;
                    case FlatOpcode::SuperMoveParamStoreLocalElided:
                        if (fi.src0Slot < nSlots) {
                            setSlot(fi.storeSlot, st[fi.src0Slot]);
                        }
                        break;
                    case FlatOpcode::ConstantInt:
                        setSlot(fi.dstSlot,
                            (fi.integerImmediate >= std::numeric_limits<std::int32_t>::min() &&
                             fi.integerImmediate <= std::numeric_limits<std::int32_t>::max())
                                ? ProvenTag::ExactInt32
                                : ProvenTag::Mixed);
                        break;
                    case FlatOpcode::ConstantDouble:
                        setSlot(fi.dstSlot, ProvenTag::ExactDouble);
                        break;
                    case FlatOpcode::ConstantBool:
                        setSlot(fi.dstSlot, ProvenTag::ExactBool);
                        break;
                    case FlatOpcode::SuperConstFloat:
                        setSlot(fi.dstSlot, ProvenTag::ExactFloat);
                        break;
                    case FlatOpcode::SuperConstFloatStoreLocal:
                        setSlot(fi.dstSlot, ProvenTag::ExactFloat);
                        setSlot(fi.storeSlot, ProvenTag::ExactFloat);
                        break;
                    case FlatOpcode::AddIntChecked:
                        if (rewrite && isInt(fi.src0Slot) && isInt(fi.src1Slot)) {
                            out.flatOpcode = FlatOpcode::AddIntTrustedChecked;
                        }
                        setSlot(fi.dstSlot, ProvenTag::ExactInt32);
                        break;
                    case FlatOpcode::AddIntUnchecked:
                        if (rewrite && isInt(fi.src0Slot) && isInt(fi.src1Slot)) {
                            out.flatOpcode = FlatOpcode::AddIntTrustedUnchecked;
                        }
                        setSlot(fi.dstSlot, ProvenTag::ExactInt32);
                        break;
                    case FlatOpcode::SubIntChecked:
                        if (rewrite && isInt(fi.src0Slot) && isInt(fi.src1Slot)) {
                            out.flatOpcode = FlatOpcode::SubIntTrustedChecked;
                        }
                        setSlot(fi.dstSlot, ProvenTag::ExactInt32);
                        break;
                    case FlatOpcode::SubIntUnchecked:
                        if (rewrite && isInt(fi.src0Slot) && isInt(fi.src1Slot)) {
                            out.flatOpcode = FlatOpcode::SubIntTrustedUnchecked;
                        }
                        setSlot(fi.dstSlot, ProvenTag::ExactInt32);
                        break;
                    case FlatOpcode::MulIntChecked:
                        if (rewrite && isInt(fi.src0Slot) && isInt(fi.src1Slot)) {
                            out.flatOpcode = FlatOpcode::MulIntTrustedChecked;
                        }
                        setSlot(fi.dstSlot, ProvenTag::ExactInt32);
                        break;
                    case FlatOpcode::MulIntUnchecked:
                        if (rewrite && isInt(fi.src0Slot) && isInt(fi.src1Slot)) {
                            out.flatOpcode = FlatOpcode::MulIntTrustedUnchecked;
                        }
                        setSlot(fi.dstSlot, ProvenTag::ExactInt32);
                        break;
                    case FlatOpcode::DivInt:
                    case FlatOpcode::RemInt:
                    case FlatOpcode::NegateIntUnchecked:
                    case FlatOpcode::ArrayLengthFast:
                    case FlatOpcode::ConvertFloatToIntFast:
                    case FlatOpcode::ConvertLongToIntFast:
                        setSlot(fi.dstSlot, ProvenTag::ExactInt32);
                        break;
                    case FlatOpcode::LessInt:
                        if (rewrite && isInt(fi.src0Slot) && isInt(fi.src1Slot)) {
                            out.flatOpcode = FlatOpcode::LessIntTrusted;
                        }
                        setSlot(fi.dstSlot, ProvenTag::ExactBool);
                        break;
                    case FlatOpcode::LessOrEqualInt:
                    case FlatOpcode::GreaterInt:
                    case FlatOpcode::GreaterOrEqualInt:
                    case FlatOpcode::EqualInt:
                    case FlatOpcode::NotEqualInt:
                    case FlatOpcode::LogicalNot:
                        setSlot(fi.dstSlot, ProvenTag::ExactBool);
                        break;
                    case FlatOpcode::AddDouble:
                        if (rewrite && isDbl(fi.src0Slot) && isDbl(fi.src1Slot)) {
                            out.flatOpcode = FlatOpcode::AddDoubleTrusted;
                        }
                        setSlot(fi.dstSlot, ProvenTag::ExactDouble);
                        break;
                    case FlatOpcode::SubDouble:
                        if (rewrite && isDbl(fi.src0Slot) && isDbl(fi.src1Slot)) {
                            out.flatOpcode = FlatOpcode::SubDoubleTrusted;
                        }
                        setSlot(fi.dstSlot, ProvenTag::ExactDouble);
                        break;
                    case FlatOpcode::MulDouble:
                        if (rewrite && isDbl(fi.src0Slot) && isDbl(fi.src1Slot)) {
                            out.flatOpcode = FlatOpcode::MulDoubleTrusted;
                        }
                        setSlot(fi.dstSlot, ProvenTag::ExactDouble);
                        break;
                    case FlatOpcode::DivDouble:
                        if (rewrite && isDbl(fi.src0Slot) && isDbl(fi.src1Slot)) {
                            out.flatOpcode = FlatOpcode::DivDoubleTrusted;
                        }
                        setSlot(fi.dstSlot, ProvenTag::ExactDouble);
                        break;
                    case FlatOpcode::ConvertIntToDoubleFast:
                    case FlatOpcode::ConvertLongToDoubleFast:
                    case FlatOpcode::ConvertFloatToDoubleFast:
                    case FlatOpcode::NegateDouble:
                    case FlatOpcode::MathSqrt64:
                        setSlot(fi.dstSlot, ProvenTag::ExactDouble);
                        break;
                    case FlatOpcode::AddFloat:
                        if (rewrite && isFlt(fi.src0Slot) && isFlt(fi.src1Slot)) {
                            out.flatOpcode = FlatOpcode::AddFloatTrusted;
                        }
                        setSlot(fi.dstSlot, ProvenTag::ExactFloat);
                        break;
                    case FlatOpcode::SubFloat:
                        if (rewrite && isFlt(fi.src0Slot) && isFlt(fi.src1Slot)) {
                            out.flatOpcode = FlatOpcode::SubFloatTrusted;
                        }
                        setSlot(fi.dstSlot, ProvenTag::ExactFloat);
                        break;
                    case FlatOpcode::MulFloat:
                        if (rewrite && isFlt(fi.src0Slot) && isFlt(fi.src1Slot)) {
                            out.flatOpcode = FlatOpcode::MulFloatTrusted;
                        }
                        setSlot(fi.dstSlot, ProvenTag::ExactFloat);
                        break;
                    case FlatOpcode::DivFloat:
                    case FlatOpcode::NegateFloat:
                    case FlatOpcode::ConvertIntToFloatFast:
                    case FlatOpcode::ConvertDoubleToFloatFast:
                        setSlot(fi.dstSlot, ProvenTag::ExactFloat);
                        break;
                    case FlatOpcode::LoadElementIntFast:
                        if (rewrite && isInt(fi.src1Slot)) {
                            out.arithFlags = static_cast<std::uint8_t>(
                                out.arithFlags | FlatInstruction::FlagTrustedSrc0);
                        }
                        setSlot(fi.dstSlot, ProvenTag::ExactInt32);
                        break;
                    case FlatOpcode::StoreElementIntFast:
                        if (rewrite) {
                            if (isInt(fi.src1Slot)) {
                                out.arithFlags = static_cast<std::uint8_t>(
                                    out.arithFlags | FlatInstruction::FlagTrustedSrc0);
                            }
                            if (isInt(fi.src2Slot)) {
                                out.arithFlags = static_cast<std::uint8_t>(
                                    out.arithFlags | FlatInstruction::FlagTrustedSrc1);
                            }
                        }
                        break;
                    case FlatOpcode::SuperLessLocalConstBranch:
                        if (rewrite && isInt(fi.src0Slot)) {
                            out.arithFlags = static_cast<std::uint8_t>(
                                out.arithFlags | FlatInstruction::FlagTrustedSrc0);
                        }
                        setSlot(fi.dstSlot, ProvenTag::ExactInt32);
                        setSlot(fi.aux0Slot, ProvenTag::ExactInt32);
                        setSlot(fi.aux1Slot, ProvenTag::ExactBool);
                        break;
                    case FlatOpcode::SuperLessLocalRegBranch:
                        if (rewrite && isInt(fi.src0Slot) && isInt(fi.src1Slot)) {
                            out.arithFlags = static_cast<std::uint8_t>(
                                out.arithFlags | FlatInstruction::FlagTrustedSrc0 | FlatInstruction::FlagTrustedSrc1);
                        }
                        setSlot(fi.dstSlot, ProvenTag::ExactInt32);
                        setSlot(fi.aux1Slot, ProvenTag::ExactBool);
                        break;
                    case FlatOpcode::SuperBinaryLocalIntConstStoreElided:
                    case FlatOpcode::SuperBinaryLocalIntConstStoreJump: {
                        const bool producesBool =
                            fi.binaryOpcode1 >= QuickOpcode::LessInt &&
                            fi.binaryOpcode1 <= QuickOpcode::NotEqualInt;
                        if (rewrite && isInt(fi.src0Slot)) {
                            out.arithFlags = static_cast<std::uint8_t>(
                                out.arithFlags | FlatInstruction::FlagTrustedSrc0);
                        }
                        const ProvenTag resTag = producesBool ? ProvenTag::ExactBool : ProvenTag::ExactInt32;
                        if (fi.flatOpcode == FlatOpcode::SuperBinaryLocalIntConstStoreJump) {
                            setSlot(fi.dstSlot, resTag);
                        } else {
                            setSlot(fi.storeSlot, resTag);
                        }
                        break;
                    }
                    case FlatOpcode::SuperBinaryLocalIntLocalStoreElided:
                    case FlatOpcode::SuperBinaryLocalIntLocalStoreJump:
                    case FlatOpcode::SuperBinaryLocalIntRegStoreElided:
                    case FlatOpcode::SuperBinaryLocalIntRegStoreJump: {
                        const bool producesBool =
                            fi.binaryOpcode1 >= QuickOpcode::LessInt &&
                            fi.binaryOpcode1 <= QuickOpcode::NotEqualInt;
                        if (rewrite && isInt(fi.src0Slot) && isInt(fi.src1Slot)) {
                            out.arithFlags = static_cast<std::uint8_t>(
                                out.arithFlags | FlatInstruction::FlagTrustedSrc0 | FlatInstruction::FlagTrustedSrc1);
                        }
                        const ProvenTag resTag = producesBool ? ProvenTag::ExactBool : ProvenTag::ExactInt32;
                        if (fi.flatOpcode == FlatOpcode::SuperBinaryLocalIntLocalStoreJump ||
                            fi.flatOpcode == FlatOpcode::SuperBinaryLocalIntRegStoreJump) {
                            setSlot(fi.dstSlot, resTag);
                        } else {
                            setSlot(fi.storeSlot, resTag);
                        }
                        break;
                    }
                    case FlatOpcode::GenericBytecode:
                        std::fill(st.begin(), st.end(), ProvenTag::Mixed);
                        break;
                    default: {
                        // Conservative transfer for any other instruction writing slots
                        setSlot(fi.dstSlot, ProvenTag::Mixed);
                        if (!isBranchOrJumpFlatOp(fi.flatOpcode)) {
                            setSlot(fi.storeSlot, ProvenTag::Mixed);
                        }
                        setSlot(fi.aux0Slot, ProvenTag::Mixed);
                        setSlot(fi.aux1Slot, ProvenTag::Mixed);
                        break;
                    }
                    }
                    return out;
                };

                const auto propagateEdge = [&](const std::vector<ProvenTag>& outSt,
                                               const ExecutionEdge& edge) {
                    if (edge.target == InvalidBlockIndex || edge.target >= nBlocks) return false;
                    std::vector<ProvenTag> edgeSt = outSt;
                    if (edge.hasArguments && edge.sources && edge.destinations) {
                        const std::size_t cnt = std::min(edge.sources->size(), edge.destinations->size());
                        std::vector<ProvenTag> transferred(cnt, ProvenTag::Mixed);
                        for (std::size_t i = 0; i < cnt; ++i) {
                            const std::size_t sSlot = execFunc.registerOffset + (*edge.sources)[i];
                            transferred[i] = sSlot < nSlots ? outSt[sSlot] : ProvenTag::Mixed;
                        }
                        for (std::size_t i = 0; i < cnt; ++i) {
                            const std::size_t dSlot = execFunc.registerOffset + (*edge.destinations)[i].target;
                            if (dSlot < nSlots) edgeSt[dSlot] = transferred[i];
                        }
                    }
                    bool changed = false;
                    if (!blkVisited[edge.target]) {
                        blkVisited[edge.target] = true;
                        blkIn[edge.target] = std::move(edgeSt);
                        changed = true;
                    } else {
                        auto& dstIn = blkIn[edge.target];
                        for (std::size_t s = 0; s < nSlots; ++s) {
                            const ProvenTag m = mergeTag(dstIn[s], edgeSt[s]);
                            if (m != dstIn[s]) {
                                dstIn[s] = m;
                                changed = true;
                            }
                        }
                    }
                    return changed;
                };

                bool changed = true;
                std::size_t guardIter = 0;
                while (changed && guardIter++ < 32) {
                    changed = false;
                    for (std::size_t b = 0; b < nBlocks; ++b) {
                        if (!blkVisited[b]) continue;
                        std::vector<ProvenTag> cur = blkIn[b];
                        for (std::size_t k = execFunc.blockStreamOffsets[b]; k < blkEnd[b]; ++k) {
                            (void)stepInstState(buildStream[k].hot, cur, false);
                        }
                        blkOut[b] = cur;
                        const auto& eb = execFunc.blocks[b];
                        if (propagateEdge(cur, eb.branch)) changed = true;
                        if (propagateEdge(cur, eb.falseBranch)) changed = true;
                    }
                }

                for (std::size_t b = 0; b < nBlocks; ++b) {
                    if (!blkVisited[b]) continue;
                    std::vector<ProvenTag> cur = blkIn[b];
                    for (std::size_t k = execFunc.blockStreamOffsets[b]; k < blkEnd[b]; ++k) {
                        buildStream[k].hot = stepInstState(buildStream[k].hot, cur, true);
                    }
                }
            }

            execFunc.flatStream.resize(buildStream.size());
            execFunc.flatCold.resize(buildStream.size());
            for (std::size_t idx = 0; idx < buildStream.size(); ++idx) {
                execFunc.flatStream[idx] = buildStream[idx].hot;
                execFunc.flatCold[idx] = buildStream[idx].cold;
            }
            execFunc.canUseFlatFastPath = !execFunc.flatStream.empty();

            // Detect trivial single-block methods (ReturnVoid / ReturnConst / ReturnParam) for CallPolledFast IC inlining
            if (execFunc.canUseFlatFastPath &&
                execFunc.blocks.size() == 1 &&
                !execFunc.hasExceptionHandlers &&
                execFunc.directNativeReference == nullptr &&
                execFunc.entryStreamOffset == 0 &&
                execFunc.flatStream.size() <= 8) {
                std::uint32_t totalSpan = 0;
                bool okTrivial = true;
                bool hasConst = false;
                std::uint16_t constReg = InvalidCompactReg;
                Value constVal{};
                std::vector<std::int16_t> slotParamOrigin(execFunc.totalFrameSlots, -1);
                for (std::size_t p = 0; p < paramCount; ++p) {
                    slotParamOrigin[p] = static_cast<std::int16_t>(p);
                }
                for (std::size_t idx = 0; idx + 1 < execFunc.flatStream.size(); ++idx) {
                    const auto& fi = execFunc.flatStream[idx];
                    totalSpan += fi.span;
                    if (fi.flatOpcode == FlatOpcode::SuperMoveParamStoreLocalElided ||
                        fi.flatOpcode == FlatOpcode::SuperMoveParamStoreLocal) {
                        if (fi.src0Slot < slotParamOrigin.size()) {
                            const auto orig = slotParamOrigin[fi.src0Slot];
                            if (fi.storeSlot < slotParamOrigin.size()) slotParamOrigin[fi.storeSlot] = orig;
                            if (fi.flatOpcode == FlatOpcode::SuperMoveParamStoreLocal &&
                                fi.dstSlot < slotParamOrigin.size()) {
                                slotParamOrigin[fi.dstSlot] = orig;
                            }
                        }
                    } else if (fi.flatOpcode == FlatOpcode::MoveSlot) {
                        if (fi.src0Slot < slotParamOrigin.size() && fi.dstSlot < slotParamOrigin.size()) {
                            slotParamOrigin[fi.dstSlot] = slotParamOrigin[fi.src0Slot];
                        } else {
                            okTrivial = false;
                            break;
                        }
                    } else if (!hasConst && fi.flatOpcode == FlatOpcode::ConstantInt) {
                        hasConst = true;
                        constReg = fi.dstSlot;
                        constVal = fi.integerImmediate;
                    } else if (!hasConst && fi.flatOpcode == FlatOpcode::ConstantBool) {
                        hasConst = true;
                        constReg = fi.dstSlot;
                        constVal = (fi.integerImmediate != 0);
                    } else if (!hasConst && fi.flatOpcode == FlatOpcode::ConstantLong) {
                        hasConst = true;
                        constReg = fi.dstSlot;
                        constVal = LongValue{fi.integerImmediate};
                    } else if (!hasConst && fi.flatOpcode == FlatOpcode::ConstantDouble) {
                        hasConst = true;
                        constReg = fi.dstSlot;
                        double d = 0.0;
                        std::memcpy(&d, &fi.integerImmediate, sizeof(double));
                        constVal = d;
                    } else if (!hasConst && fi.flatOpcode == FlatOpcode::SuperConstFloat &&
                               fi.storeSlot == InvalidCompactReg) {
                        hasConst = true;
                        constReg = fi.dstSlot;
                        float fl = 0.0f;
                        std::memcpy(&fl, &fi.integerImmediate, sizeof(float));
                        constVal = FloatValue{fl};
                    } else {
                        okTrivial = false;
                        break;
                    }
                }
                if (okTrivial && !execFunc.flatStream.empty()) {
                    const auto& retInst = execFunc.flatStream.back();
                    totalSpan += retInst.span;
                    if (totalSpan > 0 && totalSpan <= 64u) {
                        if (retInst.flatOpcode == FlatOpcode::TermReturnVoid) {
                            execFunc.trivialKind = TrivialMethodKind::ReturnVoid;
                            execFunc.trivialInstCost = static_cast<std::uint16_t>(totalSpan);
                        } else if (retInst.flatOpcode == FlatOpcode::TermReturnScalarFast) {
                            if (hasConst && retInst.src0Slot == constReg) {
                                execFunc.trivialKind = TrivialMethodKind::ReturnConst;
                                execFunc.trivialInstCost = static_cast<std::uint16_t>(totalSpan);
                                execFunc.trivialConstValue = constVal;
                            } else if (retInst.src0Slot < slotParamOrigin.size() &&
                                       slotParamOrigin[retInst.src0Slot] >= 0) {
                                execFunc.trivialKind = TrivialMethodKind::ReturnParam;
                                execFunc.trivialInstCost = static_cast<std::uint16_t>(totalSpan);
                                execFunc.trivialParamIndex =
                                    static_cast<std::uint16_t>(slotParamOrigin[retInst.src0Slot]);
                            }
                        }
                    }
                }
            }
            // Extended trivial field getter / setter / binary-field micro-methods for virtual & interface dispatch
            if (execFunc.trivialKind == TrivialMethodKind::None &&
                execFunc.canUseFlatFastPath &&
                execFunc.blocks.size() == 1 &&
                !execFunc.hasExceptionHandlers &&
                execFunc.directNativeReference == nullptr &&
                execFunc.entryStreamOffset == 0 &&
                paramCount >= 1 &&
                function.parameterTypes[0] == semantic::PrimitiveType::Object &&
                execFunc.flatStream.size() <= 10) {
                std::uint32_t totalSpan = 0;
                std::vector<std::int16_t> slotParamOrigin(execFunc.totalFrameSlots, -1);
                for (std::size_t p = 0; p < paramCount; ++p) {
                    slotParamOrigin[p] = static_cast<std::int16_t>(p);
                }
                struct LoadedFieldInfo {
                    bool valid = false;
                    std::uint16_t fieldIdx = 0;
                    std::uint8_t exactTag = 0;
                    semantic::SymbolId ownerTid = 0;
                };
                std::vector<LoadedFieldInfo> slotFieldOrigin(execFunc.totalFrameSlots);
                std::vector<bool> slotIsConstInt(execFunc.totalFrameSlots, false);
                std::vector<std::int64_t> slotConstInt(execFunc.totalFrameSlots, 0);

                bool okFieldMicro = true;
                bool sawStoreField = false;
                std::uint16_t storedFieldIdx = 0;
                std::uint8_t storedFieldTag = 0;
                semantic::SymbolId storedOwnerTid = 0;
                std::int16_t storedSrcParam = -1;

                TrivialMethodKind computedExprKind = TrivialMethodKind::None;
                std::uint16_t exprSlot = InvalidCompactReg;
                std::uint16_t exprF0 = 0, exprF1 = 0;
                semantic::SymbolId exprOwnerTid = 0;
                std::int64_t exprConst = 0;
                bool exprChecked = true;

                for (std::size_t idx = 0; idx + 1 < execFunc.flatStream.size(); ++idx) {
                    const auto& fi = execFunc.flatStream[idx];
                    totalSpan += fi.span;
                    if (fi.flatOpcode == FlatOpcode::SuperMoveParamStoreLocalElided ||
                        fi.flatOpcode == FlatOpcode::SuperMoveParamStoreLocal) {
                        if (fi.src0Slot < slotParamOrigin.size()) {
                            const auto orig = slotParamOrigin[fi.src0Slot];
                            if (fi.storeSlot < slotParamOrigin.size()) {
                                if (computedExprKind != TrivialMethodKind::None && fi.storeSlot == exprSlot) {
                                    okFieldMicro = false;
                                    break;
                                }
                                slotParamOrigin[fi.storeSlot] = orig;
                                slotFieldOrigin[fi.storeSlot] = {};
                                slotIsConstInt[fi.storeSlot] = false;
                                slotConstInt[fi.storeSlot] = 0;
                            }
                            if (fi.flatOpcode == FlatOpcode::SuperMoveParamStoreLocal &&
                                fi.dstSlot < slotParamOrigin.size()) {
                                if (computedExprKind != TrivialMethodKind::None && fi.dstSlot == exprSlot) {
                                    okFieldMicro = false;
                                    break;
                                }
                                slotParamOrigin[fi.dstSlot] = orig;
                                slotFieldOrigin[fi.dstSlot] = {};
                                slotIsConstInt[fi.dstSlot] = false;
                                slotConstInt[fi.dstSlot] = 0;
                            }
                        } else {
                            okFieldMicro = false;
                            break;
                        }
                    } else if (fi.flatOpcode == FlatOpcode::MoveSlot) {
                        if (fi.src0Slot < execFunc.totalFrameSlots && fi.dstSlot < execFunc.totalFrameSlots) {
                            slotParamOrigin[fi.dstSlot] = slotParamOrigin[fi.src0Slot];
                            slotFieldOrigin[fi.dstSlot] = slotFieldOrigin[fi.src0Slot];
                            slotIsConstInt[fi.dstSlot] = slotIsConstInt[fi.src0Slot];
                            slotConstInt[fi.dstSlot] = slotConstInt[fi.src0Slot];
                            if (computedExprKind != TrivialMethodKind::None) {
                                if (fi.src0Slot == exprSlot) {
                                    exprSlot = fi.dstSlot;
                                } else if (fi.dstSlot == exprSlot) {
                                    okFieldMicro = false;
                                    break;
                                }
                            }
                        } else {
                            okFieldMicro = false;
                            break;
                        }
                    } else if (fi.flatOpcode == FlatOpcode::CheckNotNullFast) {
                        if (fi.src0Slot < execFunc.totalFrameSlots &&
                            fi.dstSlot < execFunc.totalFrameSlots &&
                            slotParamOrigin[fi.src0Slot] == 0) {
                            if (computedExprKind != TrivialMethodKind::None && fi.dstSlot == exprSlot) {
                                okFieldMicro = false;
                                break;
                            }
                            slotParamOrigin[fi.dstSlot] = 0;
                            slotFieldOrigin[fi.dstSlot] = {};
                            slotIsConstInt[fi.dstSlot] = false;
                            slotConstInt[fi.dstSlot] = 0;
                        } else {
                            okFieldMicro = false;
                            break;
                        }
                    } else if (fi.flatOpcode == FlatOpcode::LoadFieldExactFast) {
                        if (fi.src0Slot < execFunc.totalFrameSlots &&
                            fi.dstSlot < execFunc.totalFrameSlots &&
                            slotParamOrigin[fi.src0Slot] == 0) {
                            if (computedExprKind != TrivialMethodKind::None && fi.dstSlot == exprSlot) {
                                okFieldMicro = false;
                                break;
                            }
                            slotParamOrigin[fi.dstSlot] = -1;
                            slotIsConstInt[fi.dstSlot] = false;
                            slotConstInt[fi.dstSlot] = 0;
                            slotFieldOrigin[fi.dstSlot] = {
                                true,
                                fi.aux0Slot,
                                static_cast<std::uint8_t>(fi.binaryOpcode1),
                                static_cast<semantic::SymbolId>(fi.integerImmediate),
                            };
                        } else {
                            okFieldMicro = false;
                            break;
                        }
                    } else if (fi.flatOpcode == FlatOpcode::StoreFieldExactFast && !sawStoreField &&
                               computedExprKind == TrivialMethodKind::None) {
                        const auto tag = static_cast<ValueTag>(static_cast<std::uint8_t>(fi.binaryOpcode1));
                        const bool nonRefScalarTag =
                            (tag >= ValueTag::Bool && tag <= ValueTag::QuatInline) ||
                            (tag >= ValueTag::Fixed64 && tag <= ValueTag::QuatfInline);
                        if (nonRefScalarTag &&
                            fi.src0Slot < execFunc.totalFrameSlots &&
                            fi.src1Slot < execFunc.totalFrameSlots &&
                            slotParamOrigin[fi.src0Slot] == 0 &&
                            slotParamOrigin[fi.src1Slot] >= 1) {
                            sawStoreField = true;
                            storedFieldIdx = fi.aux0Slot;
                            storedFieldTag = static_cast<std::uint8_t>(fi.binaryOpcode1);
                            storedOwnerTid = static_cast<semantic::SymbolId>(fi.integerImmediate);
                            storedSrcParam = slotParamOrigin[fi.src1Slot];
                        } else {
                            okFieldMicro = false;
                            break;
                        }
                    } else if (fi.flatOpcode == FlatOpcode::ConstantInt) {
                        if (fi.dstSlot < execFunc.totalFrameSlots) {
                            if (computedExprKind != TrivialMethodKind::None && fi.dstSlot == exprSlot) {
                                okFieldMicro = false;
                                break;
                            }
                            slotParamOrigin[fi.dstSlot] = -1;
                            slotFieldOrigin[fi.dstSlot] = {};
                            slotIsConstInt[fi.dstSlot] = true;
                            slotConstInt[fi.dstSlot] = fi.integerImmediate;
                        } else {
                            okFieldMicro = false;
                            break;
                        }
                    } else if ((fi.flatOpcode == FlatOpcode::AddIntChecked ||
                                fi.flatOpcode == FlatOpcode::AddIntUnchecked ||
                                fi.flatOpcode == FlatOpcode::SubIntChecked ||
                                fi.flatOpcode == FlatOpcode::SubIntUnchecked ||
                                fi.flatOpcode == FlatOpcode::MulIntChecked ||
                                fi.flatOpcode == FlatOpcode::MulIntUnchecked ||
                                fi.flatOpcode == FlatOpcode::AddIntTrustedChecked ||
                                fi.flatOpcode == FlatOpcode::AddIntTrustedUnchecked ||
                                fi.flatOpcode == FlatOpcode::SubIntTrustedChecked ||
                                fi.flatOpcode == FlatOpcode::SubIntTrustedUnchecked ||
                                fi.flatOpcode == FlatOpcode::MulIntTrustedChecked ||
                                fi.flatOpcode == FlatOpcode::MulIntTrustedUnchecked) &&
                               !sawStoreField &&
                               computedExprKind == TrivialMethodKind::None &&
                               fi.src0Slot < execFunc.totalFrameSlots &&
                               fi.src1Slot < execFunc.totalFrameSlots &&
                               fi.dstSlot < execFunc.totalFrameSlots) {
                        const auto& lf = slotFieldOrigin[fi.src0Slot];
                        const auto& rf = slotFieldOrigin[fi.src1Slot];
                        const bool checkedOp =
                            fi.flatOpcode == FlatOpcode::AddIntChecked ||
                            fi.flatOpcode == FlatOpcode::SubIntChecked ||
                            fi.flatOpcode == FlatOpcode::MulIntChecked ||
                            fi.flatOpcode == FlatOpcode::AddIntTrustedChecked ||
                            fi.flatOpcode == FlatOpcode::SubIntTrustedChecked ||
                            fi.flatOpcode == FlatOpcode::MulIntTrustedChecked;
                        const bool isAdd =
                            fi.flatOpcode == FlatOpcode::AddIntChecked ||
                            fi.flatOpcode == FlatOpcode::AddIntUnchecked ||
                            fi.flatOpcode == FlatOpcode::AddIntTrustedChecked ||
                            fi.flatOpcode == FlatOpcode::AddIntTrustedUnchecked;
                        const bool isSub =
                            fi.flatOpcode == FlatOpcode::SubIntChecked ||
                            fi.flatOpcode == FlatOpcode::SubIntUnchecked ||
                            fi.flatOpcode == FlatOpcode::SubIntTrustedChecked ||
                            fi.flatOpcode == FlatOpcode::SubIntTrustedUnchecked;
                        const bool isMul =
                            fi.flatOpcode == FlatOpcode::MulIntChecked ||
                            fi.flatOpcode == FlatOpcode::MulIntUnchecked ||
                            fi.flatOpcode == FlatOpcode::MulIntTrustedChecked ||
                            fi.flatOpcode == FlatOpcode::MulIntTrustedUnchecked;
                        const auto intTag = static_cast<std::uint8_t>(ValueTag::Int64);
                        if (lf.valid && rf.valid &&
                            lf.exactTag == intTag && rf.exactTag == intTag &&
                            lf.ownerTid == rf.ownerTid && (isAdd || isSub)) {
                            computedExprKind = isAdd
                                ? TrivialMethodKind::ReturnThisFieldAddField
                                : TrivialMethodKind::ReturnThisFieldSubField;
                            exprSlot = fi.dstSlot;
                            exprF0 = lf.fieldIdx;
                            exprF1 = rf.fieldIdx;
                            exprOwnerTid = lf.ownerTid;
                            exprChecked = checkedOp;
                        } else if (lf.valid && lf.exactTag == intTag && slotIsConstInt[fi.src1Slot]) {
                            computedExprKind = isAdd
                                ? TrivialMethodKind::ReturnThisFieldAddConst
                                : (isSub
                                    ? TrivialMethodKind::ReturnThisFieldSubConst
                                    : TrivialMethodKind::ReturnThisFieldMulConst);
                            exprSlot = fi.dstSlot;
                            exprF0 = lf.fieldIdx;
                            exprOwnerTid = lf.ownerTid;
                            exprConst = slotConstInt[fi.src1Slot];
                            exprChecked = checkedOp;
                        } else if ((isAdd || isMul) && rf.valid && rf.exactTag == intTag &&
                                   slotIsConstInt[fi.src0Slot]) {
                            computedExprKind = isAdd
                                ? TrivialMethodKind::ReturnThisFieldAddConst
                                : TrivialMethodKind::ReturnThisFieldMulConst;
                            exprSlot = fi.dstSlot;
                            exprF0 = rf.fieldIdx;
                            exprOwnerTid = rf.ownerTid;
                            exprConst = slotConstInt[fi.src0Slot];
                            exprChecked = checkedOp;
                        } else {
                            okFieldMicro = false;
                            break;
                        }
                    } else {
                        okFieldMicro = false;
                        break;
                    }
                }
                if (okFieldMicro && !execFunc.flatStream.empty()) {
                    const auto& retInst = execFunc.flatStream.back();
                    totalSpan += retInst.span;
                    if (totalSpan > 0 && totalSpan <= 64u) {
                        if (sawStoreField && retInst.flatOpcode == FlatOpcode::TermReturnVoid) {
                            execFunc.trivialKind = TrivialMethodKind::StoreThisFieldExact;
                            execFunc.trivialInstCost = static_cast<std::uint16_t>(totalSpan);
                            execFunc.trivialFieldIndex0 = storedFieldIdx;
                            execFunc.trivialFieldTag0 = storedFieldTag;
                            execFunc.trivialOwnerTypeId = storedOwnerTid;
                            execFunc.trivialParamIndex = static_cast<std::uint16_t>(storedSrcParam);
                        } else if (!sawStoreField &&
                                   retInst.flatOpcode == FlatOpcode::TermReturnScalarFast &&
                                   retInst.src0Slot < execFunc.totalFrameSlots) {
                            if (computedExprKind != TrivialMethodKind::None &&
                                retInst.src0Slot == exprSlot) {
                                execFunc.trivialKind = computedExprKind;
                                execFunc.trivialInstCost = static_cast<std::uint16_t>(totalSpan);
                                execFunc.trivialFieldIndex0 = exprF0;
                                execFunc.trivialFieldIndex1 = exprF1;
                                execFunc.trivialOwnerTypeId = exprOwnerTid;
                                execFunc.trivialConstValue = exprConst;
                                execFunc.trivialCheckedArith = exprChecked;
                            } else if (computedExprKind == TrivialMethodKind::None &&
                                       slotFieldOrigin[retInst.src0Slot].valid) {
                                const auto& fld = slotFieldOrigin[retInst.src0Slot];
                                execFunc.trivialKind = TrivialMethodKind::ReturnThisFieldExact;
                                execFunc.trivialInstCost = static_cast<std::uint16_t>(totalSpan);
                                execFunc.trivialFieldIndex0 = fld.fieldIdx;
                                execFunc.trivialFieldTag0 = fld.exactTag;
                                execFunc.trivialOwnerTypeId = fld.ownerTid;
                            }
                        }
                    }
                }
            }

            // Classify small-frame multi-block leaf inline candidates for Pass 4 unfolding
            if (execFunc.canUseFlatFastPath &&
                !execFunc.hasExceptionHandlers &&
                execFunc.directNativeReference == nullptr &&
                execFunc.entryStreamOffset == 0 &&
                paramCount <= 4 &&
                execFunc.totalFrameSlots <= 48 &&
                execFunc.flatStream.size() <= 40) {
                const auto isPureSlotInlinableOp = [](FlatOpcode op) noexcept -> bool {
                    switch (op) {
                    case FlatOpcode::MoveSlot:
                    case FlatOpcode::ConstantInt:
                    case FlatOpcode::AddIntChecked:
                    case FlatOpcode::AddIntUnchecked:
                    case FlatOpcode::SubIntChecked:
                    case FlatOpcode::SubIntUnchecked:
                    case FlatOpcode::MulIntChecked:
                    case FlatOpcode::MulIntUnchecked:
                    case FlatOpcode::DivInt:
                    case FlatOpcode::RemInt:
                    case FlatOpcode::LessInt:
                    case FlatOpcode::LessOrEqualInt:
                    case FlatOpcode::GreaterInt:
                    case FlatOpcode::GreaterOrEqualInt:
                    case FlatOpcode::EqualInt:
                    case FlatOpcode::NotEqualInt:
                    case FlatOpcode::SuperMoveParamStoreLocal:
                    case FlatOpcode::SuperMoveParamStoreLocalElided:
                    case FlatOpcode::SuperBinaryLocalIntConst:
                    case FlatOpcode::SuperBinaryLocalIntConstStore:
                    case FlatOpcode::SuperBinaryLocalIntConstStoreElided:
                    case FlatOpcode::SuperBinaryLocalIntLocal:
                    case FlatOpcode::SuperBinaryLocalIntLocalStore:
                    case FlatOpcode::SuperBinaryLocalIntLocalStoreElided:
                    case FlatOpcode::SuperBinaryLocalIntReg:
                    case FlatOpcode::SuperBinaryLocalIntRegStore:
                    case FlatOpcode::SuperBinaryLocalIntRegStoreElided:
                    case FlatOpcode::SuperBinaryLocalIntRegChainStore:
                    case FlatOpcode::SuperBinaryLocalIntRegChainStoreElided:
                    case FlatOpcode::SuperBinaryLocalIntConstStoreJump:
                    case FlatOpcode::SuperBinaryLocalIntLocalStoreJump:
                    case FlatOpcode::SuperBinaryLocalIntRegStoreJump:
                    case FlatOpcode::SuperLessLocalRegBranch:
                    case FlatOpcode::SuperLessLocalConstBranch:
                    case FlatOpcode::ConstantDouble:
                    case FlatOpcode::AddDouble:
                    case FlatOpcode::SubDouble:
                    case FlatOpcode::MulDouble:
                    case FlatOpcode::DivDouble:
                    case FlatOpcode::LessDouble:
                    case FlatOpcode::LessOrEqualDouble:
                    case FlatOpcode::GreaterDouble:
                    case FlatOpcode::GreaterOrEqualDouble:
                    case FlatOpcode::ConstantBool:
                    case FlatOpcode::ConstantLong:
                    case FlatOpcode::ConvertIntToLongFast:
                    case FlatOpcode::ConvertIntToDoubleFast:
                    case FlatOpcode::ConvertLongToDoubleFast:
                    case FlatOpcode::SuperBinaryLocalDoubleConstStore:
                    case FlatOpcode::SuperBinaryLocalDoubleConstStoreElided:
                    case FlatOpcode::SuperBinaryLocalDoubleLocalStore:
                    case FlatOpcode::SuperBinaryLocalDoubleLocalStoreElided:
                    case FlatOpcode::NegateIntUnchecked:
                    case FlatOpcode::NegateDouble:
                    case FlatOpcode::LogicalNot:
                    case FlatOpcode::AddFloat:
                    case FlatOpcode::SubFloat:
                    case FlatOpcode::MulFloat:
                    case FlatOpcode::DivFloat:
                    case FlatOpcode::LessFloat:
                    case FlatOpcode::LessOrEqualFloat:
                    case FlatOpcode::GreaterFloat:
                    case FlatOpcode::GreaterOrEqualFloat:
                    case FlatOpcode::EqualFloat:
                    case FlatOpcode::NotEqualFloat:
                    case FlatOpcode::NegateFloat:
                    case FlatOpcode::ConvertIntToFloatFast:
                    case FlatOpcode::ConvertDoubleToFloatFast:
                    case FlatOpcode::ConvertFloatToIntFast:
                    case FlatOpcode::ConvertFloatToDoubleFast:
                    case FlatOpcode::ConvertLongToIntFast:
                    case FlatOpcode::SuperConstFloat:
                    case FlatOpcode::SuperConstFloatStoreLocal:
                    case FlatOpcode::SuperBinaryLocalFloatLocalStore:
                    case FlatOpcode::SuperBinaryLocalFloatLocalStoreElided:
                    case FlatOpcode::SuperBinaryLocalFloatReg:
                    case FlatOpcode::SuperBinaryLocalFloatRegElided:
                    case FlatOpcode::SuperBinaryLocalFloatRegStore:
                    case FlatOpcode::SuperBinaryLocalFloatRegStoreElided:
                    case FlatOpcode::SuperBinaryRegFloatStoreElided:
                    case FlatOpcode::NewStructFast:
                    case FlatOpcode::LoadFixedRawFast:
                    case FlatOpcode::StoreFixedRawFast:
                    case FlatOpcode::LoadVec3FieldFast:
                    case FlatOpcode::StoreVec3FieldFast:
                    case FlatOpcode::LoadQuatFieldFast:
                    case FlatOpcode::StoreQuatFieldFast:
                    case FlatOpcode::MathFixedConst:
                    case FlatOpcode::MathFixedFromRaw:
                    case FlatOpcode::MathFixedFromInt:
                    case FlatOpcode::MathFixedRatio:
                    case FlatOpcode::MathFixedNeg:
                    case FlatOpcode::MathFixedAdd:
                    case FlatOpcode::MathFixedSub:
                    case FlatOpcode::MathFixedMul:
                    case FlatOpcode::MathFixedMulInt:
                    case FlatOpcode::MathFixedDiv:
                    case FlatOpcode::MathFixedDivInt:
                    case FlatOpcode::MathFixedSin:
                    case FlatOpcode::MathFixedCos:
                    case FlatOpcode::MathFixedSqrt:
                    case FlatOpcode::MathSqrt64:
                    case FlatOpcode::MathVec3Const:
                    case FlatOpcode::MathVec3Raw:
                    case FlatOpcode::MathVec3Of:
                    case FlatOpcode::MathVec3Neg:
                    case FlatOpcode::MathVec3Add:
                    case FlatOpcode::MathVec3Sub:
                    case FlatOpcode::MathVec3MulFixed:
                    case FlatOpcode::MathVec3MulInt:
                    case FlatOpcode::MathVec3DivFixed:
                    case FlatOpcode::MathVec3DivInt:
                    case FlatOpcode::MathVec3Dot:
                    case FlatOpcode::MathVec3Cross:
                    case FlatOpcode::MathVec2Add:
                    case FlatOpcode::MathVec2Sub:
                    case FlatOpcode::MathVec2MulFixed:
                    case FlatOpcode::MathVec2Dot:
                    case FlatOpcode::MathQuatIdentity:
                    case FlatOpcode::MathQuatDot:
                    case FlatOpcode::MathQuatMul:
                    case FlatOpcode::MathQuatRotateVec3:
                    case FlatOpcode::SuperQuatRotateBoxed:
                    case FlatOpcode::MathQuatNormalize:
                    case FlatOpcode::SuperQuatNormalizedBoxed:
                    case FlatOpcode::MathQuatFromEuler:
                    case FlatOpcode::MathQuatLerp:
                    case FlatOpcode::SuperLoadVec3RawFast:
                    case FlatOpcode::SuperLoadQuatRawFast:
                    case FlatOpcode::SuperFixedMulAdd:
                    case FlatOpcode::SuperVec3MulAdd:
                    case FlatOpcode::SuperVec3AddQuatRotate:
                    case FlatOpcode::SuperQuatMulNormalized:
                    case FlatOpcode::SuperQuatMulChain:
                    case FlatOpcode::SuperQuatMulRotateVec3:
                    case FlatOpcode::CheckNotNullFast:
                    case FlatOpcode::IsTypeObjFast:
                    case FlatOpcode::AsTypeObjFast:
                    case FlatOpcode::LoadFieldFast:
                    case FlatOpcode::LoadFieldExactFast:
                    case FlatOpcode::StoreFieldFast:
                    case FlatOpcode::StoreFieldExactFast:
                    case FlatOpcode::ArrayLengthFast:
                    case FlatOpcode::LoadElementIntFast:
                    case FlatOpcode::LoadElementTypedFast:
                    case FlatOpcode::StoreElementIntFast:
                    case FlatOpcode::StoreElementPrimFast:
                    case FlatOpcode::StoreElementStructFast:
                    case FlatOpcode::SuperLessLocalArrayLengthBranch:
                    case FlatOpcode::AddIntTrustedChecked:
                    case FlatOpcode::AddIntTrustedUnchecked:
                    case FlatOpcode::SubIntTrustedChecked:
                    case FlatOpcode::SubIntTrustedUnchecked:
                    case FlatOpcode::MulIntTrustedChecked:
                    case FlatOpcode::MulIntTrustedUnchecked:
                    case FlatOpcode::LessIntTrusted:
                    case FlatOpcode::AddDoubleTrusted:
                    case FlatOpcode::SubDoubleTrusted:
                    case FlatOpcode::MulDoubleTrusted:
                    case FlatOpcode::DivDoubleTrusted:
                    case FlatOpcode::AddFloatTrusted:
                    case FlatOpcode::SubFloatTrusted:
                    case FlatOpcode::MulFloatTrusted:
                    case FlatOpcode::TermJumpNoArgs:
                    case FlatOpcode::TermBranchBoolNoArgs:
                    case FlatOpcode::TermReturnVoid:
                    case FlatOpcode::TermReturnScalarFast:
                        return true;
                    default:
                        return false;
                    }
                };
                bool allPure = true;
                for (const auto& fi : execFunc.flatStream) {
                    if (!isPureSlotInlinableOp(fi.flatOpcode)) {
                        allPure = false;
                        break;
                    }
                }
                execFunc.isSmallLeafInlineCandidate = allPure;
                if (allPure && paramCount > 0 && execFunc.flatStream.size() > paramCount && localCount >= paramCount) {
                    bool fastPrologue = true;
                    for (std::size_t p = 0; p < paramCount; ++p) {
                        const auto& pInst = execFunc.flatStream[p];
                        if (pInst.flatOpcode != FlatOpcode::SuperMoveParamStoreLocalElided ||
                            pInst.span != 2 ||
                            pInst.src0Slot != static_cast<std::uint16_t>(p) ||
                            pInst.storeSlot != static_cast<std::uint16_t>(paramCount + p)) {
                            fastPrologue = false;
                            break;
                        }
                    }
                    if (fastPrologue) {
                        // Ensure no instruction after the prologue reads raw parameter slots [0..paramCount-1]
                        // or branches back to [0..paramCount-1]
                        for (std::size_t idx = paramCount; idx < execFunc.flatStream.size(); ++idx) {
                            const auto& fi = execFunc.flatStream[idx];
                            if (isBranchOrJumpFlatOp(fi.flatOpcode)) {
                                if (fi.trueTargetOffset < paramCount ||
                                    (fi.falseTargetOffset != InvalidStreamOffset && fi.falseTargetOffset < paramCount)) {
                                    fastPrologue = false;
                                    break;
                                }
                                if (fi.src0Slot < paramCount || fi.src1Slot < paramCount) {
                                    fastPrologue = false;
                                    break;
                                }
                            } else {
                                if (fi.src0Slot < paramCount || fi.src1Slot < paramCount) {
                                    fastPrologue = false;
                                    break;
                                }
                                if (fi.src2Slot < paramCount) {
                                    fastPrologue = false;
                                    break;
                                }
                            }
                        }
                    }
                    execFunc.hasFastParamToLocalPrologue = fastPrologue;
                }
            }
        }
    }

    // Pass 3b: Pre-link per-type virtual & interface dispatch tables + flattened O(1) assignable IDs
    {
        std::unordered_map<semantic::SymbolId, const semantic::TypeSymbol*> allTypesById;
        for (const auto& module : modules) {
            for (const auto& type : module.types) {
                if (type.id != 0) allTypesById.emplace(type.id, &type);
            }
        }
        const auto resolvePrelinkedTarget = [&](semantic::SymbolId symId) noexcept -> PrelinkedDispatchTarget {
            PrelinkedDispatchTarget out{};
            out.symbolId = symId;
            if (symId == 0) return out;
            const auto fIt = functions.find(symId);
            if (fIt != functions.end() && fIt->second.function) {
                out.target = &fIt->second;
                out.execution = findExecutionFunction(*mirror, *fIt->second.function);
            }
            return out;
        };

        mirror->typeDispatch.reserve(allTypesById.size());
        for (const auto& [tid, typePtr] : allTypesById) {
            const auto& type = *typePtr;
            TypeDispatchMetadata meta;
            meta.typeId = tid;

            // Flatten self + all base classes + all interfaces (deduplicated) for O(1) L1 scan
            auto cur = tid;
            for (std::size_t depth = 0; cur != 0 && depth < 256; ++depth) {
                if (std::find(meta.assignableTargetIds.begin(), meta.assignableTargetIds.end(), cur) ==
                    meta.assignableTargetIds.end()) {
                    meta.assignableTargetIds.push_back(cur);
                }
                const auto cIt = allTypesById.find(cur);
                if (cIt == allTypesById.end()) break;
                for (const auto& imap : cIt->second->interfaceDispatchMaps) {
                    if (imap.interfaceTypeId != 0 &&
                        std::find(meta.assignableTargetIds.begin(),
                                  meta.assignableTargetIds.end(),
                                  imap.interfaceTypeId) == meta.assignableTargetIds.end()) {
                        meta.assignableTargetIds.push_back(imap.interfaceTypeId);
                    }
                }
                cur = cIt->second->baseTypeId;
            }

            if (type.kind == semantic::TypeKind::Class) {
                meta.virtualTargets.reserve(type.virtualDispatchTable.size());
                for (const auto vSym : type.virtualDispatchTable) {
                    meta.virtualTargets.push_back(resolvePrelinkedTarget(vSym));
                }

                // Collect own + inherited interface maps (first match in inheritance order wins)
                cur = tid;
                for (std::size_t depth = 0; cur != 0 && depth < 256; ++depth) {
                    const auto cIt = allTypesById.find(cur);
                    if (cIt == allTypesById.end()) break;
                    for (const auto& imap : cIt->second->interfaceDispatchMaps) {
                        if (imap.interfaceTypeId == 0) continue;
                        const bool exists = std::any_of(
                            meta.interfaceMaps.begin(),
                            meta.interfaceMaps.end(),
                            [&](const PrelinkedInterfaceMap& m) {
                                return m.interfaceTypeId == imap.interfaceTypeId;
                            });
                        if (!exists) {
                            PrelinkedInterfaceMap pmap;
                            pmap.interfaceTypeId = imap.interfaceTypeId;
                            pmap.slots.reserve(imap.slots.size());
                            for (const auto sSym : imap.slots) {
                                pmap.slots.push_back(resolvePrelinkedTarget(sSym));
                            }
                            meta.interfaceMaps.push_back(std::move(pmap));
                        }
                    }
                    cur = cIt->second->baseTypeId;
                }
                std::sort(
                    meta.interfaceMaps.begin(),
                    meta.interfaceMaps.end(),
                    [](const PrelinkedInterfaceMap& a, const PrelinkedInterfaceMap& b) {
                        return a.interfaceTypeId < b.interfaceTypeId;
                    });
            }
            mirror->typeDispatch.emplace(tid, std::move(meta));
        }
    }

    // Pass 4: Unfold small-frame leaf script functions at CallDirect sites & thread empty Jump->Jump chains
    for (const auto& module : modules) {
        for (const auto& function : module.functions) {
            auto found = mirror->functions.find(&function);
            if (found == mirror->functions.end()) continue;
            auto& caller = found->second;
            if (!caller.canUseFlatFastPath || caller.flatStream.empty()) continue;

            const auto isBranchLikeOp = [](FlatOpcode op) noexcept -> bool {
                return op == FlatOpcode::TermJumpNoArgs ||
                    op == FlatOpcode::TermBranchBoolNoArgs ||
                    op == FlatOpcode::TermBranchGeneric ||
                    op == FlatOpcode::SuperLessLocalRegBranch ||
                    op == FlatOpcode::SuperLessLocalConstBranch ||
                    op == FlatOpcode::SuperLessLocalArrayLengthBranch ||
                    op == FlatOpcode::SuperBinaryLocalIntConstStoreJump ||
                    op == FlatOpcode::SuperBinaryLocalIntLocalStoreJump ||
                    op == FlatOpcode::SuperBinaryLocalIntRegStoreJump ||
                    op == FlatOpcode::InlineReturnVoidJump ||
                    op == FlatOpcode::InlineReturnScalarJump;
            };
            const auto hasAuxNonSlotMeta = [](FlatOpcode op) noexcept -> bool {
                return op == FlatOpcode::NewStructFast ||
                    op == FlatOpcode::NewArrayFast ||
                    op == FlatOpcode::NewObjectFast ||
                    op == FlatOpcode::CheckNotNullFast ||
                    op == FlatOpcode::IsTypeObjFast ||
                    op == FlatOpcode::AsTypeObjFast ||
                    op == FlatOpcode::NewDelegateFast ||
                    op == FlatOpcode::CombineDelegateFast ||
                    op == FlatOpcode::RemoveDelegateFast ||
                    op == FlatOpcode::InvokeDelegateFast ||
                    op == FlatOpcode::LoadFieldFast ||
                    op == FlatOpcode::LoadFieldExactFast ||
                    op == FlatOpcode::StoreFieldFast ||
                    op == FlatOpcode::StoreFieldExactFast ||
                    op == FlatOpcode::StoreElementStructFast ||
                    op == FlatOpcode::LoadVec3FieldFast ||
                    op == FlatOpcode::StoreVec3FieldFast ||
                    op == FlatOpcode::LoadQuatFieldFast ||
                    op == FlatOpcode::StoreQuatFieldFast ||
                    op == FlatOpcode::SuperLoadVec3RawFast ||
                    op == FlatOpcode::SuperLoadQuatRawFast ||
                    (op >= FlatOpcode::MathFixedConst && op <= FlatOpcode::SuperQuatMulRotateVec3);
            };

            // Scan CallDirect sites eligible for unfolding
            std::uint32_t maxCalleeSlots = 0;
            std::size_t extraInstCount = 0;
            bool anyInlinableCall = false;
            for (std::size_t i = 0; i < caller.flatStream.size(); ++i) {
                const auto& fi = caller.flatStream[i];
                const auto& fc = caller.flatCold[i];
                if ((fi.flatOpcode == FlatOpcode::CallDirect0 ||
                     fi.flatOpcode == FlatOpcode::CallDirect1 ||
                     fi.flatOpcode == FlatOpcode::CallDirect2 ||
                     fi.flatOpcode == FlatOpcode::CallDirectN) &&
                    fc.directExecution != nullptr &&
                    fc.directTarget != nullptr &&
                    fc.directExecution != &caller &&
                    fc.directExecution->isSmallLeafInlineCandidate) {
                    if (fi.flatOpcode == FlatOpcode::CallDirectN) {
                        if (!fc.bytecode || fc.bytecode->operands.size() > 4) continue;
                    }
                    if (caller.totalFrameSlots + fc.directExecution->totalFrameSlots >= InvalidCompactReg) {
                        continue;
                    }
                    if (caller.flatStream.size() + extraInstCount + fc.directExecution->flatStream.size() > 512) {
                        continue;
                    }
                    anyInlinableCall = true;
                    maxCalleeSlots = std::max(maxCalleeSlots, fc.directExecution->totalFrameSlots);
                    extraInstCount += fc.directExecution->flatStream.size();
                }
            }

            if (anyInlinableCall) {
                const std::uint16_t inlineBias = static_cast<std::uint16_t>(caller.totalFrameSlots);
                caller.totalFrameSlots += maxCalleeSlots;
                const auto biasSlot = [inlineBias](std::uint16_t s) noexcept -> std::uint16_t {
                    return s == InvalidCompactReg
                        ? InvalidCompactReg
                        : static_cast<std::uint16_t>(s + inlineBias);
                };

                // Map old instruction index -> new stream start offset so all caller branches remap cleanly!
                const std::size_t oldLen = caller.flatStream.size();
                std::vector<std::uint32_t> oldToNewOffset(oldLen + 1, 0);
                std::vector<FlatInstruction> newStream;
                std::vector<FlatColdMeta> newCold;
                newStream.reserve(oldLen + extraInstCount);
                newCold.reserve(oldLen + extraInstCount);

                for (std::size_t i = 0; i < oldLen; ++i) {
                    oldToNewOffset[i] = static_cast<std::uint32_t>(newStream.size());
                    const auto fi = caller.flatStream[i];
                    const auto fc = caller.flatCold[i];

                    bool canUnfoldHere =
                        (fi.flatOpcode == FlatOpcode::CallDirect0 ||
                         fi.flatOpcode == FlatOpcode::CallDirect1 ||
                         fi.flatOpcode == FlatOpcode::CallDirect2 ||
                         (fi.flatOpcode == FlatOpcode::CallDirectN &&
                          fc.bytecode && fc.bytecode->operands.size() <= 4)) &&
                        fc.directExecution != nullptr &&
                        fc.directTarget != nullptr &&
                        fc.directExecution != &caller &&
                        fc.directExecution->isSmallLeafInlineCandidate &&
                        (newStream.size() + (oldLen - i) + fc.directExecution->flatStream.size() <= 576);

                    if (!canUnfoldHere) {
                        newStream.push_back(fi);
                        newCold.push_back(fc);
                        continue;
                    }

                    const auto& callee = *fc.directExecution;
                    const std::size_t calleeParamCount = callee.bytecode->parameterTypes.size();
                    const std::size_t calleeLocalCount = callee.bytecode->localTypes.size();

                    // 1. Emit InlineCallEnter
                    FlatInstruction enterInst;
                    enterInst.flatOpcode = FlatOpcode::InlineCallEnter;
                    enterInst.span = 1;
                    enterInst.dstSlot = inlineBias;
                    enterInst.inlineArgCount = static_cast<std::uint8_t>(calleeParamCount);
                    enterInst.aux0Slot = static_cast<std::uint16_t>(calleeLocalCount);
                    if (callee.hasFastParamToLocalPrologue) {
                        enterInst.arithFlags = static_cast<std::uint8_t>(
                            enterInst.arithFlags | FlatInstruction::FlagFastParamPrologue);
                    }
                    if (fi.flatOpcode == FlatOpcode::CallDirect1) {
                        enterInst.src0Slot = fi.src0Slot;
                    } else if (fi.flatOpcode == FlatOpcode::CallDirect2) {
                        enterInst.src0Slot = fi.src0Slot;
                        enterInst.src1Slot = fi.src1Slot;
                    } else if (fi.flatOpcode == FlatOpcode::CallDirectN && fc.bytecode) {
                        if (fc.bytecode->operands.size() >= 1) {
                            enterInst.src0Slot = static_cast<std::uint16_t>(
                                caller.registerOffset + fc.bytecode->operands[0]);
                        }
                        if (fc.bytecode->operands.size() >= 2) {
                            enterInst.src1Slot = static_cast<std::uint16_t>(
                                caller.registerOffset + fc.bytecode->operands[1]);
                        }
                        if (fc.bytecode->operands.size() >= 3) {
                            enterInst.src2Slot = static_cast<std::uint16_t>(
                                caller.registerOffset + fc.bytecode->operands[2]);
                        }
                        if (fc.bytecode->operands.size() >= 4) {
                            enterInst.aux1Slot = static_cast<std::uint16_t>(
                                caller.registerOffset + fc.bytecode->operands[3]);
                        }
                    }
                    FlatColdMeta enterCold;
                    enterCold.bytecode = fc.bytecode;
                    enterCold.blockIndex = fc.blockIndex;
                    enterCold.instructionIndexInBlock = fc.instructionIndexInBlock;
                    newStream.push_back(enterInst);
                    newCold.push_back(enterCold);

                    // Check whether callee has any reference/traceable slot types that must be cleared on InlineReturn*
                    const auto mayHoldTraceableValue = [](semantic::PrimitiveType pt) noexcept -> bool {
                        return pt == semantic::PrimitiveType::Object ||
                            pt == semantic::PrimitiveType::Array ||
                            pt == semantic::PrimitiveType::String ||
                            pt == semantic::PrimitiveType::Struct ||
                            pt == semantic::PrimitiveType::Enum ||
                            pt == semantic::PrimitiveType::Error;
                    };
                    bool calleeHasTraceableSlots = false;
                    for (const auto pt : callee.bytecode->parameterTypes) {
                        if (mayHoldTraceableValue(pt)) { calleeHasTraceableSlots = true; break; }
                    }
                    if (!calleeHasTraceableSlots) {
                        for (const auto pt : callee.bytecode->localTypes) {
                            if (mayHoldTraceableValue(pt)) { calleeHasTraceableSlots = true; break; }
                        }
                    }
                    if (!calleeHasTraceableSlots) {
                        for (const auto pt : callee.bytecode->registerTypes) {
                            if (mayHoldTraceableValue(pt)) { calleeHasTraceableSlots = true; break; }
                        }
                    }
                    const std::int64_t inlineClearMeta = calleeHasTraceableSlots
                        ? static_cast<std::int64_t>(
                              static_cast<std::uint64_t>(inlineBias) |
                              (static_cast<std::uint64_t>(callee.totalFrameSlots) << 16u))
                        : 0;

                    // 2. Clone & slot-bias callee flatStream
                    const std::uint32_t bodyBase = static_cast<std::uint32_t>(newStream.size());
                    const std::size_t cLen = callee.flatStream.size();
                    const std::uint32_t afterInlineOffset = bodyBase + static_cast<std::uint32_t>(cLen);

                    for (std::size_t k = 0; k < cLen; ++k) {
                        FlatInstruction cInst = callee.flatStream[k];
                        FlatColdMeta cCold = callee.flatCold[k];
                        cCold.directExecution = &callee;
                        cCold.inlinedCallee = fc.directTarget;
                        cCold.callerBlockIndex = fc.blockIndex;
                        cCold.callerInstIndex = fc.instructionIndexInBlock;
                        cCold.inlineFrameBias = inlineBias;

                        const bool isLast = (k + 1 == cLen);
                        if (cInst.flatOpcode == FlatOpcode::TermReturnVoid) {
                            FlatInstruction ret;
                            ret.span = 1;
                            ret.integerImmediate = inlineClearMeta;
                            if (isLast) {
                                ret.flatOpcode = FlatOpcode::InlineReturnVoid;
                            } else {
                                ret.flatOpcode = FlatOpcode::InlineReturnVoidJump;
                                ret.trueTargetOffset = afterInlineOffset;
                                ret.falseTargetOffset = InvalidStreamOffset;
                            }
                            newStream.push_back(ret);
                            newCold.push_back(cCold);
                            continue;
                        }
                        if (cInst.flatOpcode == FlatOpcode::TermReturnScalarFast) {
                            FlatInstruction ret;
                            ret.span = 1;
                            ret.integerImmediate = inlineClearMeta;
                            ret.dstSlot = fi.dstSlot;
                            ret.src0Slot = biasSlot(cInst.src0Slot);
                            if (isLast) {
                                ret.flatOpcode = FlatOpcode::InlineReturnScalar;
                            } else {
                                ret.flatOpcode = FlatOpcode::InlineReturnScalarJump;
                                ret.trueTargetOffset = afterInlineOffset;
                                ret.falseTargetOffset = InvalidStreamOffset;
                            }
                            newStream.push_back(ret);
                            newCold.push_back(cCold);
                            continue;
                        }

                        cInst.dstSlot = biasSlot(cInst.dstSlot);
                        cInst.src0Slot = biasSlot(cInst.src0Slot);
                        cInst.src1Slot = biasSlot(cInst.src1Slot);

                        if (isBranchLikeOp(cInst.flatOpcode)) {
                            if (cInst.trueTargetOffset != InvalidStreamOffset) {
                                cInst.trueTargetOffset += bodyBase;
                            }
                            if (cInst.falseTargetOffset != InvalidStreamOffset) {
                                cInst.falseTargetOffset += bodyBase;
                            }
                        } else if (hasAuxNonSlotMeta(cInst.flatOpcode)) {
                            cInst.src2Slot = biasSlot(cInst.src2Slot);
                            cInst.storeSlot = biasSlot(cInst.storeSlot);
                            if (cInst.flatOpcode >= FlatOpcode::MathFixedConst &&
                                cInst.flatOpcode <= FlatOpcode::SuperQuatMulRotateVec3) {
                                const std::uint32_t mCalls = cInst.aux1Slot & 0xFFu;
                                const std::uint32_t mDepth = (cInst.aux1Slot >> 8u) & 0xFFu;
                                if (mDepth > 0u && mDepth < 254u) {
                                    cInst.aux1Slot = static_cast<std::uint16_t>(
                                        mCalls | ((mDepth + 1u) << 8u));
                                }
                            }
                        } else {
                            cInst.src2Slot = biasSlot(cInst.src2Slot);
                            cInst.aux0Slot = biasSlot(cInst.aux0Slot);
                            cInst.aux1Slot = biasSlot(cInst.aux1Slot);
                            cInst.storeSlot = biasSlot(cInst.storeSlot);
                        }

                        newStream.push_back(cInst);
                        newCold.push_back(cCold);
                    }
                }
                oldToNewOffset[oldLen] = static_cast<std::uint32_t>(newStream.size());

                // Remap caller blockStreamOffsets, entryStreamOffset, and caller-origin branch targets
                for (auto& blkOff : caller.blockStreamOffsets) {
                    if (blkOff <= oldLen) blkOff = oldToNewOffset[blkOff];
                }
                if (caller.entryStreamOffset <= oldLen) {
                    caller.entryStreamOffset = oldToNewOffset[caller.entryStreamOffset];
                }
                for (std::size_t idx = 0; idx < newStream.size(); ++idx) {
                    if (newCold[idx].inlinedCallee != nullptr) continue;
                    auto& inst = newStream[idx];
                    if (!isBranchLikeOp(inst.flatOpcode)) continue;
                    if (inst.trueTargetOffset <= oldLen) {
                        inst.trueTargetOffset = oldToNewOffset[inst.trueTargetOffset];
                    }
                    if (inst.falseTargetOffset != InvalidStreamOffset &&
                        inst.falseTargetOffset <= oldLen) {
                        inst.falseTargetOffset = oldToNewOffset[inst.falseTargetOffset];
                    }
                }

                caller.flatStream = std::move(newStream);
                caller.flatCold = std::move(newCold);
            }

            // Thread 1-hop unconditional Jump -> Jump chains (e.g. nested if/else-if merge blocks)
            for (std::size_t idx = 0; idx < caller.flatStream.size(); ++idx) {
                auto& inst = caller.flatStream[idx];
                if (inst.flatOpcode == FlatOpcode::TermJumpNoArgs &&
                    inst.span == 1 &&
                    inst.trueTargetOffset < caller.flatStream.size() &&
                    inst.trueTargetOffset != idx) {
                    const std::uint32_t midIdx = inst.trueTargetOffset;
                    const auto& mid = caller.flatStream[midIdx];
                    if (mid.flatOpcode == FlatOpcode::TermJumpNoArgs &&
                        mid.span == 1 &&
                        mid.trueTargetOffset < caller.flatStream.size() &&
                        mid.trueTargetOffset != midIdx &&
                        caller.flatCold[idx].inlinedCallee == caller.flatCold[midIdx].inlinedCallee) {
                        inst.auxImm32 = midIdx;
                        inst.trueTargetOffset = mid.trueTargetOffset;
                        inst.span = 2;
                    } else if (!caller.hasExceptionHandlers &&
                               (mid.flatOpcode == FlatOpcode::SuperBinaryLocalIntConstStoreJump ||
                                mid.flatOpcode == FlatOpcode::SuperBinaryLocalIntLocalStoreJump ||
                                mid.flatOpcode == FlatOpcode::SuperBinaryLocalIntRegStoreJump) &&
                               (mid.arithFlags & FlatInstruction::FlagPreJump1) == 0u &&
                               mid.span < 250u &&
                               caller.flatCold[idx].inlinedCallee == caller.flatCold[midIdx].inlinedCallee) {
                        FlatInstruction fusedJump = mid;
                        fusedJump.span = static_cast<std::uint8_t>(mid.span + 1u);
                        fusedJump.arithFlags = static_cast<std::uint8_t>(
                            mid.arithFlags | FlatInstruction::FlagPreJump1);
                        fusedJump.auxImm32 = midIdx;
                        inst = fusedJump;
                    }
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

} // namespace realscript::runtime
