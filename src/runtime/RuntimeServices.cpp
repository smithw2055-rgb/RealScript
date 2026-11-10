#include <atomic>
#include <deque>
#include <mutex>
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

namespace {

DeterminismOperationId cachedInstructionOperationId(
    bytecode::Opcode opcode) noexcept {
    constexpr auto Count =
        static_cast<std::size_t>(bytecode::Opcode::ConstantTypeId) + 1U;
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
                             tid == vec2TypeId() ||
                             tid == vec3TypeId() ||
                             tid == quatTypeId())) {
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
                        if (!semantic::isExactType(fld.type) &&
                            fld.type != semantic::PrimitiveType::Handle) {
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
                        if (!semantic::isExactType(fld.type) &&
                            fld.type != semantic::PrimitiveType::Handle &&
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
                           function.returnTypeId == vec2TypeId() ||
                           function.returnTypeId == vec3TypeId() ||
                           function.returnTypeId == quatTypeId())))) {
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
                if (validForwarder && candidateRef) {
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
            mod.sourceFiles.size() == 1 &&
            mod.sourceFiles[0].path == "$builtin/RealScript.Math.rs";
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
            if (op == bytecode::Opcode::NewDelegate ||
                op == bytecode::Opcode::NewObject ||
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

    // Pass 3: Build unified flat frame slot layout & direct-threaded FlatInstruction stream
    for (const auto& module : modules) {
        for (const auto& function : module.functions) {
            auto found = mirror->functions.find(&function);
            if (found == mirror->functions.end()) continue;
            auto& execFunc = found->second;
            execFunc.hasExceptionHandlers = !function.exceptionHandlers.empty();

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

            execFunc.blockStreamOffsets.assign(execFunc.blocks.size(), InvalidStreamOffset);
            std::size_t estimatedStreamLen = 0;
            for (const auto& blk : execFunc.blocks) {
                estimatedStreamLen += blk.instructions.size() + 1;
            }
            execFunc.flatStream.reserve(estimatedStreamLen);

            for (std::size_t bIdx = 0; bIdx < execFunc.blocks.size(); ++bIdx) {
                const auto& blk = execFunc.blocks[bIdx];
                execFunc.blockStreamOffsets[bIdx] =
                    static_cast<std::uint32_t>(execFunc.flatStream.size());

                const std::size_t nInst = blk.instructions.size();
                for (std::size_t i = 0; i < nInst;) {
                    const auto& inst = blk.instructions[i];
                    FlatInstruction flat;
                    flat.bytecode = inst.bytecode;
                    flat.directTarget = inst.directTarget;
                    flat.directExecution = inst.directExecution;
                    flat.integerImmediate = inst.integerImmediate;
                    flat.blockIndex = static_cast<std::uint32_t>(bIdx);
                    flat.instructionIndexInBlock = static_cast<std::uint32_t>(i);
                    flat.binaryOpcode1 = inst.binaryOpcode1;
                    flat.binaryOpcode2 = inst.binaryOpcode2;
                    flat.span = inst.span;
                    flat.checkedArithmetic1 = inst.checkedArithmetic1;
                    flat.checkedArithmetic2 = inst.checkedArithmetic2;

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
                        flat.dstSlot = regSlot(inst.result);
                        flat.aux0Slot = regSlot(inst.superResult);
                        execFunc.flatStream.push_back(flat);
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
                        flat.dstSlot = regSlot(inst.result);
                        flat.aux0Slot = regSlot(inst.auxReg2);
                        flat.aux1Slot = regSlot(inst.superResult);
                        execFunc.flatStream.push_back(flat);
                        i += inst.span;
                        goto next_block;
                    }

                    if (inst.superOpcode != SuperOpcode::None) {
                        switch (inst.superOpcode) {
                        case SuperOpcode::LoadParameterStoreLocal:
                            flat.flatOpcode = FlatOpcode::SuperMoveParamStoreLocal;
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
                        execFunc.flatStream.push_back(flat);
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
                            execFunc.flatStream.push_back(flat);
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
                                execFunc.flatStream.push_back(flat);
                                i += 3;
                                continue;
                            }
                            flat.flatOpcode = FlatOpcode::SuperConstFloat;
                            flat.span = 2;
                            flat.dstSlot = regSlot(conv.result);
                            execFunc.flatStream.push_back(flat);
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
                            execFunc.flatStream.push_back(flat);
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
                                execFunc.flatStream.push_back(flat);
                                i += 3;
                                continue;
                            } else {
                                const bool tempDead = (globalRegReads[inst.result] == 1);
                                flat.span = 2;
                                flat.flatOpcode = tempDead
                                    ? FlatOpcode::SuperBinaryLocalFloatRegElided
                                    : FlatOpcode::SuperBinaryLocalFloatReg;
                                execFunc.flatStream.push_back(flat);
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
                        execFunc.flatStream.push_back(flat);
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
                    case QuickOpcode::CallPolled:
                        flat.flatOpcode = FlatOpcode::CallPolledFast;
                        flat.dstSlot = inst.result == InvalidCompactReg
                            ? InvalidCompactReg
                            : regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        flat.src1Slot = inst.op1 == InvalidCompactReg
                            ? InvalidCompactReg
                            : regSlot(inst.op1);
                        break;
                    case QuickOpcode::ConstantDouble:
                        flat.flatOpcode = FlatOpcode::ConstantDouble;
                        flat.dstSlot = regSlot(inst.result);
                        break;
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
                    case QuickOpcode::LoadFieldFast:
                        flat.flatOpcode = FlatOpcode::LoadFieldFast;
                        flat.dstSlot = regSlot(inst.result);
                        flat.src0Slot = regSlot(inst.op0);
                        flat.aux0Slot = inst.auxReg1;
                        flat.aux1Slot = inst.bytecode->typeIndex;
                        break;
                    case QuickOpcode::StoreFieldFast:
                        flat.flatOpcode = FlatOpcode::StoreFieldFast;
                        flat.src0Slot = regSlot(inst.op0);
                        flat.src1Slot = regSlot(inst.op1);
                        flat.aux0Slot = inst.auxReg1;
                        flat.aux1Slot = inst.bytecode->typeIndex;
                        break;
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
                        } else {
                            flat.flatOpcode = FlatOpcode::LoadStructFieldFast;
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
                        } else {
                            flat.flatOpcode = FlatOpcode::StoreStructFieldFast;
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
                        else flat.aux0Slot = 0;
                        break;
                    }
                    case QuickOpcode::Generic:
                    default:
                        if (inst.bytecode &&
                            inst.bytecode->opcode == bytecode::Opcode::NewArray &&
                            inst.bytecode->operands.size() == 1 &&
                            inst.bytecode->operands[0] < regCount &&
                            inst.bytecode->result < regCount &&
                            inst.bytecode->result < function.registerTypeIds.size()) {
                            flat.flatOpcode = FlatOpcode::NewArrayFast;
                            flat.dstSlot = regSlot(static_cast<std::uint16_t>(inst.bytecode->result));
                            flat.src0Slot = regSlot(static_cast<std::uint16_t>(inst.bytecode->operands[0]));
                        } else {
                            flat.flatOpcode = FlatOpcode::GenericBytecode;
                        }
                        break;
                    }
                    execFunc.flatStream.push_back(flat);
                    ++i;
                }

                // Append block terminator as a FlatInstruction
                {
                    FlatInstruction termInst;
                    termInst.blockIndex = static_cast<std::uint32_t>(bIdx);
                    termInst.instructionIndexInBlock = static_cast<std::uint32_t>(nInst);
                    termInst.span = 1;
                    switch (blk.terminatorKind) {
                    case bytecode::TerminatorKind::Jump:
                        if (!blk.branch.hasArguments &&
                            blk.branch.target != InvalidBlockIndex) {
                            termInst.flatOpcode = FlatOpcode::TermJumpNoArgs;
                        } else {
                            termInst.flatOpcode = FlatOpcode::TermBranchGeneric;
                        }
                        break;
                    case bytecode::TerminatorKind::Branch:
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
                    execFunc.flatStream.push_back(termInst);
                }
            next_block:;
            }

            // Patch block stream offsets into branch/jump instructions
            execFunc.entryStreamOffset = execFunc.blockStreamOffsets[execFunc.entry];
            for (auto& flat : execFunc.flatStream) {
                const auto& blk = execFunc.blocks[flat.blockIndex];
                if (blk.branch.target != InvalidBlockIndex &&
                    blk.branch.target < execFunc.blockStreamOffsets.size()) {
                    flat.trueTargetOffset = execFunc.blockStreamOffsets[blk.branch.target];
                }
                if (blk.falseBranch.target != InvalidBlockIndex &&
                    blk.falseBranch.target < execFunc.blockStreamOffsets.size()) {
                    flat.falseTargetOffset = execFunc.blockStreamOffsets[blk.falseBranch.target];
                }
            }
            execFunc.canUseFlatFastPath = !execFunc.flatStream.empty();
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
    if (std::holds_alternative<NullString>(value)) {
        slot.kind = InlineStructSlot::Kind::NullString;
        return slot;
    }
    if (std::holds_alternative<NullObject>(value)) {
        slot.kind = InlineStructSlot::Kind::NullObject;
        return slot;
    }
    if (std::holds_alternative<NullArray>(value)) {
        slot.kind = InlineStructSlot::Kind::NullArray;
        return slot;
    }
    if (const auto* sv = std::get_if<StringValue>(&value)) {
        slot.kind = InlineStructSlot::Kind::String;
        slot.lo = sv->id;
        return slot;
    }

    return std::nullopt;
}

StringValue runtimeStringPoolById(std::uint32_t id) noexcept;

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
    case InlineStructSlot::Kind::NullString:
        return NullString{};
    case InlineStructSlot::Kind::NullObject:
        return NullObject{};
    case InlineStructSlot::Kind::NullArray:
        return NullArray{};
    case InlineStructSlot::Kind::String:
        return runtimeStringPoolById(slot.lo);

    case InlineStructSlot::Kind::None:
        break;
    }
    return std::monostate{};
}

// ============================================================================
// Refactored Sharded Lock-Free-Read & Bounded-Memory Value Pools
// Eliminates:
//   1. Process-global mutex contention on reads (100% lock-free acquire page loads)
//      and on writes (1024-way epoch-validated TLS cache + 64 cache-line-aligned shards).
//   2. Unbounded append-only memory growth via two-tier reclamation:
//      - Per-shard Second-Chance Clock eviction for unpinned ephemeral values.
//      - Exact ManagedHeap pin/mark/sweep reclamation for heap-retained values.
// ============================================================================

constexpr std::uint32_t kPoolShardBits = 6u;
constexpr std::uint32_t kPoolShardCount = 1u << kPoolShardBits; // 64 shards
constexpr std::uint32_t kPoolShardMask = kPoolShardCount - 1u;
constexpr std::uint32_t kPoolLocalBits = 20u;
constexpr std::uint32_t kPoolLocalMask = (1u << kPoolLocalBits) - 1u;
constexpr std::uint32_t kInvalidLocalSlot = 0xFFFFFFFFu;

constexpr std::uint8_t kSlotFlagOccupied  = 0x01u;
constexpr std::uint8_t kSlotFlagClock     = 0x02u;
constexpr std::uint8_t kSlotFlagPinned    = 0x04u;
constexpr std::uint8_t kSlotFlagGcMarked  = 0x08u;
constexpr std::uint8_t kSlotFlagStackRoot = 0x10u;

std::atomic<bool> g_poolGcMarkInProgress{false};
std::atomic<bool> g_anyPoolHasPinnedSlots{false};

inline void noteShardPinned(std::atomic<bool>& shardFlag) noexcept {
    if (!shardFlag.load(std::memory_order_relaxed)) {
        shardFlag.store(true, std::memory_order_release);
    }
    if (!g_anyPoolHasPinnedSlots.load(std::memory_order_relaxed)) {
        g_anyPoolHasPinnedSlots.store(true, std::memory_order_release);
    }
}

inline void noteShardMarked(std::atomic<bool>& shardFlag) noexcept {
    if (!shardFlag.load(std::memory_order_relaxed)) {
        shardFlag.store(true, std::memory_order_release);
    }
}

[[nodiscard]] inline std::uint8_t activePinFlags() noexcept {
    return kSlotFlagPinned | kSlotFlagClock |
        (g_poolGcMarkInProgress.load(std::memory_order_acquire) ? kSlotFlagGcMarked : 0u);
}

[[nodiscard]] constexpr std::uint32_t encodeShardedPoolId(
    std::uint32_t shardIdx,
    std::uint32_t localSlot) noexcept {
    return ((shardIdx & kPoolShardMask) << kPoolLocalBits) |
           ((localSlot + 1u) & kPoolLocalMask);
}

[[nodiscard]] constexpr bool decodeShardedPoolId(
    std::uint32_t id,
    std::uint32_t& outShardIdx,
    std::uint32_t& outLocalSlot) noexcept {
    const std::uint32_t rawLocal = id & kPoolLocalMask;
    if (id == 0u || rawLocal == 0u) return false;
    outShardIdx = (id >> kPoolLocalBits) & kPoolShardMask;
    outLocalSlot = rawLocal - 1u;
    return true;
}

[[nodiscard]] inline std::uint64_t fnv1a64(std::string_view text) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char ch : text) {
        hash ^= static_cast<std::uint64_t>(ch);
        hash *= 1099511628211ULL;
    }
    return hash;
}

inline void refreshClockBitIfUnset(std::atomic<std::uint8_t>& flagByte) noexcept {
    const std::uint8_t cur = flagByte.load(std::memory_order_relaxed);
    if ((cur & kSlotFlagClock) == 0u) {
        flagByte.fetch_or(kSlotFlagClock, std::memory_order_relaxed);
    }
}

// ----------------------------------------------------------------------------
// 1. ShardedRuntimeStringPool
// ----------------------------------------------------------------------------
struct RuntimeStringPool {
    static constexpr std::uint32_t kPageShift = 8u;
    static constexpr std::uint32_t kPageSize = 1u << kPageShift; // 256 strings/page
    static constexpr std::uint32_t kPageMask = kPageSize - 1u;
    static constexpr std::uint32_t kSoftMaxPagesPerShard = 8u;   // 2,048 ephemeral strings/shard (131,072 total)
    static constexpr std::uint32_t kHardMaxPagesPerShard = 64u;  // 16,384 max if all pinned
    static constexpr std::uint32_t kSoftMaxSlotsPerShard = kSoftMaxPagesPerShard * kPageSize;
    static constexpr std::uint32_t kHardMaxSlotsPerShard = kHardMaxPagesPerShard * kPageSize;
    static constexpr std::uint32_t kRecentGuardWindow = 64u;

    struct StringPage {
        std::string strings[kPageSize][2];
        std::atomic<std::uint8_t> activeBank[kPageSize]{};
        std::atomic<std::uint8_t> flags[kPageSize]{};
    };

    struct alignas(64) Shard {
        std::mutex mutex;
        std::atomic<StringPage*> pages[kHardMaxPagesPerShard]{};
        std::vector<std::unique_ptr<StringPage>> ownedPages;
        std::unordered_map<std::string_view, std::uint32_t> indexByView;
        std::vector<std::uint32_t> freeList;
        std::uint32_t highWaterMark = 0u;
        std::uint32_t clockHand = 0u;
        std::uint32_t lastAllocatedSlot = kInvalidLocalSlot;
        std::atomic<std::uint32_t> epoch{1u};
        std::atomic<bool> mayHavePinnedOrMarked{false};
        std::atomic<std::uint64_t> clockEvictions{0u};
        std::atomic<std::uint64_t> gcReclaimed{0u};
    };

    std::string emptyString_{};
    Shard shards_[kPoolShardCount]{};

    RuntimeStringPool() = default;

    [[nodiscard]] const std::string& empty() const noexcept {
        return emptyString_;
    }

    [[nodiscard]] std::uint32_t tryClockEvictLocked(Shard& shard, bool enforceGuardWindow) {
        const std::uint32_t limit = shard.highWaterMark;
        if (limit == 0u) return kInvalidLocalSlot;
        const std::uint32_t maxScans = limit * 2u;
        for (std::uint32_t scan = 0u; scan < maxScans; ++scan) {
            const std::uint32_t candidate = shard.clockHand;
            shard.clockHand = (shard.clockHand + 1u) % limit;
            if (enforceGuardWindow && shard.lastAllocatedSlot != kInvalidLocalSlot) {
                const std::uint32_t dist = (shard.lastAllocatedSlot >= candidate)
                    ? (shard.lastAllocatedSlot - candidate)
                    : (limit - (candidate - shard.lastAllocatedSlot));
                if (dist < kRecentGuardWindow) continue;
            }

            StringPage* page = shard.pages[candidate >> kPageShift].load(std::memory_order_relaxed);
            if (!page) continue;
            const std::uint32_t offset = candidate & kPageMask;
            auto& fl = page->flags[offset];
            const std::uint8_t f = fl.load(std::memory_order_relaxed);
            if ((f & (kSlotFlagPinned | kSlotFlagGcMarked | kSlotFlagStackRoot)) != 0u) {
                continue;
            }
            if ((f & kSlotFlagClock) != 0u) {
                fl.fetch_and(static_cast<std::uint8_t>(~kSlotFlagClock), std::memory_order_relaxed);
                continue;
            }
            const std::uint8_t curBank = page->activeBank[offset].load(std::memory_order_relaxed) & 1u;
            const std::string& victimStr = page->strings[offset][curBank];
            if (!victimStr.empty()) {
                shard.indexByView.erase(std::string_view(victimStr));
            }
            shard.epoch.fetch_add(1u, std::memory_order_release);
            shard.clockEvictions.fetch_add(1u, std::memory_order_relaxed);
            return candidate;
        }
        return kInvalidLocalSlot;
    }

    [[nodiscard]] std::uint32_t acquireSlotLocked(Shard& shard) {
        if (!shard.freeList.empty()) {
            const std::uint32_t slot = shard.freeList.back();
            shard.freeList.pop_back();
            return slot;
        }
        if (shard.highWaterMark < kSoftMaxSlotsPerShard) {
            return allocateFreshSlotLocked(shard);
        }
        const std::uint32_t evicted = tryClockEvictLocked(shard, true);
        if (evicted != kInvalidLocalSlot) {
            return evicted;
        }
        if (shard.highWaterMark < kHardMaxSlotsPerShard) {
            return allocateFreshSlotLocked(shard);
        }
        return tryClockEvictLocked(shard, false);
    }

    [[nodiscard]] std::uint32_t allocateFreshSlotLocked(Shard& shard) {
        const std::uint32_t slot = shard.highWaterMark++;
        const std::uint32_t pageIdx = slot >> kPageShift;
        if (!shard.pages[pageIdx].load(std::memory_order_relaxed)) {
            auto newPage = std::make_unique<StringPage>();
            StringPage* rawPage = newPage.get();
            shard.ownedPages.push_back(std::move(newPage));
            shard.pages[pageIdx].store(rawPage, std::memory_order_release);
        }
        return slot;
    }

    StringValue intern(std::string_view text, bool pinEntry = false) {
        if (text.empty()) {
            StringValue sv;
            sv.id = 0u;
            sv.length = 0u;
            sv.ptr = &emptyString_;
            return sv;
        }

        const std::uint64_t h = fnv1a64(text);
        const std::uint32_t shardIdx = static_cast<std::uint32_t>((h >> 58u) & kPoolShardMask);
        Shard& shard = shards_[shardIdx];

        struct TlsStringSlot {
            std::uint64_t hash = 0;
            std::uint32_t id = 0;
            std::uint32_t length = 0;
            const std::string* ptr = nullptr;
            std::uint32_t epoch = 0;
        };
        thread_local std::array<TlsStringSlot, 512> tlsCache{};
        auto& tls = tlsCache[h & 511u];
        const std::uint32_t curEpoch = shard.epoch.load(std::memory_order_acquire);
        if (tls.id != 0u && tls.epoch == curEpoch && tls.hash == h &&
            tls.length == text.size() && tls.ptr && *tls.ptr == text) {
            std::uint32_t sIdx = 0u, lSlot = 0u;
            if (decodeShardedPoolId(tls.id, sIdx, lSlot)) {
                if (StringPage* p = shard.pages[lSlot >> kPageShift].load(std::memory_order_relaxed)) {
                    auto& fl = p->flags[lSlot & kPageMask];
                    refreshClockBitIfUnset(fl);
                    if (pinEntry) {
                        fl.fetch_or(activePinFlags(), std::memory_order_relaxed);
                        noteShardPinned(shard.mayHavePinnedOrMarked);
                    }
                }
            }
            StringValue sv;
            sv.id = tls.id;
            sv.length = tls.length;
            sv.ptr = tls.ptr;
            return sv;
        }

        std::lock_guard<std::mutex> lock(shard.mutex);
        const auto found = shard.indexByView.find(text);
        if (found != shard.indexByView.end()) {
            const std::uint32_t localSlot = found->second;
            StringPage* page = shard.pages[localSlot >> kPageShift].load(std::memory_order_relaxed);
            const std::uint32_t offset = localSlot & kPageMask;
            const std::uint8_t bank = page->activeBank[offset].load(std::memory_order_relaxed) & 1u;
            const std::string& stored = page->strings[offset][bank];
            const std::uint8_t addFlags = kSlotFlagOccupied | kSlotFlagClock | (pinEntry ? activePinFlags() : 0u);
            page->flags[offset].fetch_or(addFlags, std::memory_order_relaxed);
            if (pinEntry) {
                noteShardPinned(shard.mayHavePinnedOrMarked);
            }

            const std::uint32_t id = encodeShardedPoolId(shardIdx, localSlot);
            tls.hash = h;
            tls.id = id;
            tls.length = static_cast<std::uint32_t>(stored.size());
            tls.ptr = &stored;
            tls.epoch = shard.epoch.load(std::memory_order_relaxed);

            StringValue sv;
            sv.id = id;
            sv.length = tls.length;
            sv.ptr = &stored;
            return sv;
        }

        const std::uint32_t localSlot = acquireSlotLocked(shard);
        if (localSlot == kInvalidLocalSlot) {
            StringValue sv;
            sv.id = 0u;
            sv.length = 0u;
            sv.ptr = &emptyString_;
            return sv;
        }
        shard.lastAllocatedSlot = localSlot;
        StringPage* page = shard.pages[localSlot >> kPageShift].load(std::memory_order_relaxed);
        const std::uint32_t offset = localSlot & kPageMask;
        const std::uint8_t nextBank = (page->activeBank[offset].load(std::memory_order_relaxed) ^ 1u) & 1u;
        std::string& stored = page->strings[offset][nextBank];
        stored.assign(text.data(), text.size()); // Reuses string capacity on inactive bank!
        page->activeBank[offset].store(nextBank, std::memory_order_release);
        const std::uint8_t initFlags = kSlotFlagOccupied | kSlotFlagClock | (pinEntry ? activePinFlags() : 0u);
        page->flags[offset].store(initFlags, std::memory_order_release);
        if (pinEntry) {
            noteShardPinned(shard.mayHavePinnedOrMarked);
        }
        shard.indexByView.emplace(std::string_view(stored), localSlot);

        const std::uint32_t id = encodeShardedPoolId(shardIdx, localSlot);
        tls.hash = h;
        tls.id = id;
        tls.length = static_cast<std::uint32_t>(stored.size());
        tls.ptr = &stored;
        tls.epoch = shard.epoch.load(std::memory_order_relaxed);

        StringValue sv;
        sv.id = id;
        sv.length = tls.length;
        sv.ptr = &stored;
        return sv;
    }

    // 100% Lock-Free Read Path
    [[nodiscard]] StringValue getById(std::uint32_t id) noexcept {
        std::uint32_t shardIdx = 0u;
        std::uint32_t localSlot = 0u;
        if (!decodeShardedPoolId(id, shardIdx, localSlot)) {
            StringValue sv;
            sv.id = 0u;
            sv.length = 0u;
            sv.ptr = &emptyString_;
            return sv;
        }
        const std::uint32_t pageIdx = localSlot >> kPageShift;
        if (pageIdx < kHardMaxPagesPerShard) {
            Shard& shard = shards_[shardIdx];
            if (StringPage* page = shard.pages[pageIdx].load(std::memory_order_acquire)) {
                const std::uint32_t offset = localSlot & kPageMask;
                refreshClockBitIfUnset(page->flags[offset]);
                const std::uint8_t bank = page->activeBank[offset].load(std::memory_order_acquire) & 1u;
                const std::string& stored = page->strings[offset][bank];
                StringValue sv;
                sv.id = id;
                sv.length = static_cast<std::uint32_t>(stored.size());
                sv.ptr = &stored;
                return sv;
            }
        }
        StringValue sv;
        sv.id = 0u;
        sv.length = 0u;
        sv.ptr = &emptyString_;
        return sv;
    }

    void pinById(std::uint32_t id) noexcept {
        std::uint32_t shardIdx = 0u, localSlot = 0u;
        if (!decodeShardedPoolId(id, shardIdx, localSlot)) return;
        const std::uint32_t pageIdx = localSlot >> kPageShift;
        if (pageIdx < kHardMaxPagesPerShard) {
            Shard& shard = shards_[shardIdx];
            if (StringPage* page = shard.pages[pageIdx].load(std::memory_order_acquire)) {
                page->flags[localSlot & kPageMask].fetch_or(
                    activePinFlags(), std::memory_order_relaxed);
                noteShardPinned(shard.mayHavePinnedOrMarked);
            }
        }
    }

    void markById(std::uint32_t id) noexcept {
        std::uint32_t shardIdx = 0u, localSlot = 0u;
        if (!decodeShardedPoolId(id, shardIdx, localSlot)) return;
        const std::uint32_t pageIdx = localSlot >> kPageShift;
        if (pageIdx < kHardMaxPagesPerShard) {
            Shard& shard = shards_[shardIdx];
            if (StringPage* page = shard.pages[pageIdx].load(std::memory_order_acquire)) {
                page->flags[localSlot & kPageMask].fetch_or(
                    kSlotFlagGcMarked | kSlotFlagPinned, std::memory_order_relaxed);
                noteShardPinned(shard.mayHavePinnedOrMarked);
            }
        }
    }

    void markStackRootById(std::uint32_t id) noexcept {
        std::uint32_t shardIdx = 0u, localSlot = 0u;
        if (!decodeShardedPoolId(id, shardIdx, localSlot)) return;
        const std::uint32_t pageIdx = localSlot >> kPageShift;
        if (pageIdx < kHardMaxPagesPerShard) {
            Shard& shard = shards_[shardIdx];
            if (StringPage* page = shard.pages[pageIdx].load(std::memory_order_acquire)) {
                page->flags[localSlot & kPageMask].fetch_or(
                    kSlotFlagStackRoot | kSlotFlagClock, std::memory_order_relaxed);
                noteShardMarked(shard.mayHavePinnedOrMarked);
            }
        }
    }
};

RuntimeStringPool& runtimeStringPool() {
    static RuntimeStringPool pool;
    return pool;
}

StringValue runtimeStringPoolById(std::uint32_t id) noexcept {
    return runtimeStringPool().getById(id);
}

// ----------------------------------------------------------------------------
// 2. ShardedInlineStructPool
// ----------------------------------------------------------------------------
struct InlineStructEntryEq {
    bool operator()(const InlineStructArenaEntry& a, const InlineStructArenaEntry& b) const noexcept {
        return a.nestedTypeId == b.nestedTypeId &&
            a.nestedFieldMask == b.nestedFieldMask &&
            a.slots == b.slots;
    }
};

struct InlineStructEntryHash {
    std::size_t operator()(const InlineStructArenaEntry& e) const noexcept {
        std::uint64_t h = e.nestedTypeId ^ (static_cast<std::uint64_t>(e.nestedFieldMask) * 0x9e3779b97f4a7c15ULL);
        for (const auto& s : e.slots) {
            const std::uint64_t w = s.bits() ^
                (static_cast<std::uint64_t>(s.aux0) << 40u) ^
                (static_cast<std::uint64_t>(s.aux1) << 48u) ^
                (static_cast<std::uint64_t>(s.aux2) << 56u) ^
                static_cast<std::uint64_t>(s.kind);
            h ^= w + 0x9e3779b97f4a7c15ULL + (h << 6u) + (h >> 2u);
        }
        return static_cast<std::size_t>(h);
    }
};

const InlineStructArenaEntry g_zeroInlineEntry{};

struct InlineStructPool {
    static constexpr std::uint32_t kPageShift = 9u;
    static constexpr std::uint32_t kPageSize = 1u << kPageShift; // 512 entries/page
    static constexpr std::uint32_t kPageMask = kPageSize - 1u;
    static constexpr std::uint32_t kSoftMaxPagesPerShard = 4u;   // 2,048 slots/shard (131,072 total)
    static constexpr std::uint32_t kHardMaxPagesPerShard = 64u;
    static constexpr std::uint32_t kSoftMaxSlotsPerShard = kSoftMaxPagesPerShard * kPageSize;
    static constexpr std::uint32_t kHardMaxSlotsPerShard = kHardMaxPagesPerShard * kPageSize;
    static constexpr std::uint32_t kRecentGuardWindow = 64u;

    struct InlinePage {
        InlineStructArenaEntry entries[kPageSize][2]{};
        std::atomic<std::uint8_t> activeBank[kPageSize]{};
        std::atomic<std::uint8_t> flags[kPageSize]{};
    };

    struct alignas(64) Shard {
        std::mutex mutex;
        std::atomic<InlinePage*> pages[kHardMaxPagesPerShard]{};
        std::vector<std::unique_ptr<InlinePage>> ownedPages;
        std::unordered_map<
            InlineStructArenaEntry,
            std::uint32_t,
            InlineStructEntryHash,
            InlineStructEntryEq> dedupMap;
        std::vector<std::uint32_t> freeList;
        std::uint32_t highWaterMark = 0u;
        std::uint32_t clockHand = 0u;
        std::uint32_t lastAllocatedSlot = kInvalidLocalSlot;
        std::atomic<std::uint32_t> epoch{1u};
        std::atomic<bool> mayHavePinnedOrMarked{false};
        std::atomic<std::uint64_t> clockEvictions{0u};
        std::atomic<std::uint64_t> gcReclaimed{0u};
    };

    Shard shards_[kPoolShardCount]{};

    static void pinChildSlots(const InlineStructArenaEntry& entry) noexcept {
        for (const auto& s : entry.slots) {
            if (s.kind == InlineStructSlot::Kind::String && s.lo != 0u) {
                runtimeStringPool().pinById(s.lo);
            }
        }
    }

    [[nodiscard]] std::uint32_t allocateFreshSlotLocked(Shard& shard) {
        const std::uint32_t slot = shard.highWaterMark++;
        const std::uint32_t pageIdx = slot >> kPageShift;
        if (!shard.pages[pageIdx].load(std::memory_order_relaxed)) {
            auto newPage = std::make_unique<InlinePage>();
            InlinePage* rawPage = newPage.get();
            shard.ownedPages.push_back(std::move(newPage));
            shard.pages[pageIdx].store(rawPage, std::memory_order_release);
        }
        return slot;
    }

    [[nodiscard]] std::uint32_t tryClockEvictLocked(Shard& shard, bool enforceGuardWindow) {
        const std::uint32_t limit = shard.highWaterMark;
        if (limit == 0u) return kInvalidLocalSlot;
        const std::uint32_t maxScans = limit * 2u;
        for (std::uint32_t scan = 0u; scan < maxScans; ++scan) {
            const std::uint32_t candidate = shard.clockHand;
            shard.clockHand = (shard.clockHand + 1u) % limit;
            if (enforceGuardWindow && shard.lastAllocatedSlot != kInvalidLocalSlot) {
                const std::uint32_t dist = (shard.lastAllocatedSlot >= candidate)
                    ? (shard.lastAllocatedSlot - candidate)
                    : (limit - (candidate - shard.lastAllocatedSlot));
                if (dist < kRecentGuardWindow) continue;
            }

            InlinePage* page = shard.pages[candidate >> kPageShift].load(std::memory_order_relaxed);
            if (!page) continue;
            const std::uint32_t offset = candidate & kPageMask;
            auto& fl = page->flags[offset];
            const std::uint8_t f = fl.load(std::memory_order_relaxed);
            if ((f & (kSlotFlagPinned | kSlotFlagGcMarked | kSlotFlagStackRoot)) != 0u) {
                continue;
            }
            if ((f & kSlotFlagClock) != 0u) {
                fl.fetch_and(static_cast<std::uint8_t>(~kSlotFlagClock), std::memory_order_relaxed);
                continue;
            }
            const std::uint8_t curBank = page->activeBank[offset].load(std::memory_order_relaxed) & 1u;
            const InlineStructArenaEntry& victim = page->entries[offset][curBank];
            shard.dedupMap.erase(victim);
            shard.epoch.fetch_add(1u, std::memory_order_release);
            shard.clockEvictions.fetch_add(1u, std::memory_order_relaxed);
            return candidate;
        }
        return kInvalidLocalSlot;
    }

    [[nodiscard]] std::uint32_t acquireSlotLocked(Shard& shard) {
        if (!shard.freeList.empty()) {
            const std::uint32_t slot = shard.freeList.back();
            shard.freeList.pop_back();
            return slot;
        }
        if (shard.highWaterMark < kSoftMaxSlotsPerShard) {
            return allocateFreshSlotLocked(shard);
        }
        const std::uint32_t evicted = tryClockEvictLocked(shard, true);
        if (evicted != kInvalidLocalSlot) {
            return evicted;
        }
        if (shard.highWaterMark < kHardMaxSlotsPerShard) {
            return allocateFreshSlotLocked(shard);
        }
        return tryClockEvictLocked(shard, false);
    }

    std::uint32_t intern(const InlineStructArenaEntry& entry, bool pinEntry = false) noexcept {
        const std::size_t h = InlineStructEntryHash{}(entry);
        const std::uint32_t shardIdx = static_cast<std::uint32_t>((h >> 10u) & kPoolShardMask);
        Shard& shard = shards_[shardIdx];

        struct TlsCacheSlot {
            InlineStructArenaEntry key{};
            std::uint32_t idx = 0;
            std::uint32_t epoch = 0;
        };
        thread_local std::array<TlsCacheSlot, 1024> tlsCache{};
        auto& slot = tlsCache[h & 1023u];
        const std::uint32_t curEpoch = shard.epoch.load(std::memory_order_acquire);
        if (slot.idx != 0u && slot.epoch == curEpoch && InlineStructEntryEq{}(slot.key, entry)) {
            std::uint32_t sIdx = 0u, lSlot = 0u;
            if (decodeShardedPoolId(slot.idx, sIdx, lSlot)) {
                if (InlinePage* page = shard.pages[lSlot >> kPageShift].load(std::memory_order_relaxed)) {
                    auto& fl = page->flags[lSlot & kPageMask];
                    refreshClockBitIfUnset(fl);
                    if (pinEntry) {
                        fl.fetch_or(activePinFlags(), std::memory_order_relaxed);
                        noteShardPinned(shard.mayHavePinnedOrMarked);
                        pinChildSlots(entry);
                    }
                }
            }
            return slot.idx;
        }

        std::lock_guard<std::mutex> lock(shard.mutex);
        const auto found = shard.dedupMap.find(entry);
        if (found != shard.dedupMap.end()) {
            const std::uint32_t localSlot = found->second;
            if (InlinePage* page = shard.pages[localSlot >> kPageShift].load(std::memory_order_relaxed)) {
                const std::uint8_t addFlags = kSlotFlagOccupied | kSlotFlagClock | (pinEntry ? activePinFlags() : 0u);
                page->flags[localSlot & kPageMask].fetch_or(addFlags, std::memory_order_relaxed);
            }
            if (pinEntry) {
                noteShardPinned(shard.mayHavePinnedOrMarked);
                pinChildSlots(entry);
            }
            const std::uint32_t id = encodeShardedPoolId(shardIdx, localSlot);
            slot.key = entry;
            slot.idx = id;
            slot.epoch = shard.epoch.load(std::memory_order_relaxed);
            return id;
        }

        const std::uint32_t localSlot = acquireSlotLocked(shard);
        if (localSlot == kInvalidLocalSlot) {
            return 0u;
        }
        shard.lastAllocatedSlot = localSlot;
        InlinePage* page = shard.pages[localSlot >> kPageShift].load(std::memory_order_relaxed);
        const std::uint32_t offset = localSlot & kPageMask;
        const std::uint8_t nextBank = (page->activeBank[offset].load(std::memory_order_relaxed) ^ 1u) & 1u;
        page->entries[offset][nextBank] = entry;
        page->activeBank[offset].store(nextBank, std::memory_order_release);
        const std::uint8_t initFlags = kSlotFlagOccupied | kSlotFlagClock | (pinEntry ? activePinFlags() : 0u);
        page->flags[offset].store(initFlags, std::memory_order_release);
        if (pinEntry) {
            noteShardPinned(shard.mayHavePinnedOrMarked);
            pinChildSlots(entry);
        }
        shard.dedupMap.emplace(entry, localSlot);

        const std::uint32_t id = encodeShardedPoolId(shardIdx, localSlot);
        slot.key = entry;
        slot.idx = id;
        slot.epoch = shard.epoch.load(std::memory_order_relaxed);
        return id;
    }

    // 100% Lock-Free Read Path
    [[nodiscard]] const InlineStructArenaEntry& get(std::uint32_t idx) noexcept {
        std::uint32_t shardIdx = 0u;
        std::uint32_t localSlot = 0u;
        if (!decodeShardedPoolId(idx, shardIdx, localSlot)) {
            return g_zeroInlineEntry;
        }
        const std::uint32_t pageIdx = localSlot >> kPageShift;
        if (pageIdx < kHardMaxPagesPerShard) {
            Shard& shard = shards_[shardIdx];
            if (InlinePage* page = shard.pages[pageIdx].load(std::memory_order_acquire)) {
                const std::uint32_t offset = localSlot & kPageMask;
                refreshClockBitIfUnset(page->flags[offset]);
                const std::uint8_t bank = page->activeBank[offset].load(std::memory_order_acquire) & 1u;
                return page->entries[offset][bank];
            }
        }
        return g_zeroInlineEntry;
    }

    void pinById(std::uint32_t idx) noexcept {
        std::uint32_t shardIdx = 0u, localSlot = 0u;
        if (!decodeShardedPoolId(idx, shardIdx, localSlot)) return;
        const std::uint32_t pageIdx = localSlot >> kPageShift;
        if (pageIdx < kHardMaxPagesPerShard) {
            Shard& shard = shards_[shardIdx];
            if (InlinePage* page = shard.pages[pageIdx].load(std::memory_order_acquire)) {
                const std::uint32_t offset = localSlot & kPageMask;
                page->flags[offset].fetch_or(
                    activePinFlags(), std::memory_order_relaxed);
                noteShardPinned(shard.mayHavePinnedOrMarked);
                const std::uint8_t bank = page->activeBank[offset].load(std::memory_order_acquire) & 1u;
                pinChildSlots(page->entries[offset][bank]);
            }
        }
    }

    void markById(std::uint32_t idx) noexcept {
        std::uint32_t shardIdx = 0u, localSlot = 0u;
        if (!decodeShardedPoolId(idx, shardIdx, localSlot)) return;
        const std::uint32_t pageIdx = localSlot >> kPageShift;
        if (pageIdx < kHardMaxPagesPerShard) {
            Shard& shard = shards_[shardIdx];
            if (InlinePage* page = shard.pages[pageIdx].load(std::memory_order_acquire)) {
                const std::uint32_t offset = localSlot & kPageMask;
                page->flags[offset].fetch_or(
                    kSlotFlagGcMarked | kSlotFlagPinned, std::memory_order_relaxed);
                noteShardPinned(shard.mayHavePinnedOrMarked);
                const std::uint8_t bank = page->activeBank[offset].load(std::memory_order_acquire) & 1u;
                const auto& entry = page->entries[offset][bank];
                for (const auto& s : entry.slots) {
                    if (s.kind == InlineStructSlot::Kind::String && s.lo != 0u) {
                        runtimeStringPool().markById(s.lo);
                    }
                }
            }
        }
    }

    void markStackRootById(std::uint32_t idx) noexcept {
        std::uint32_t shardIdx = 0u, localSlot = 0u;
        if (!decodeShardedPoolId(idx, shardIdx, localSlot)) return;
        const std::uint32_t pageIdx = localSlot >> kPageShift;
        if (pageIdx < kHardMaxPagesPerShard) {
            Shard& shard = shards_[shardIdx];
            if (InlinePage* page = shard.pages[pageIdx].load(std::memory_order_acquire)) {
                const std::uint32_t offset = localSlot & kPageMask;
                page->flags[offset].fetch_or(
                    kSlotFlagStackRoot | kSlotFlagClock, std::memory_order_relaxed);
                noteShardMarked(shard.mayHavePinnedOrMarked);
                const std::uint8_t bank = page->activeBank[offset].load(std::memory_order_acquire) & 1u;
                const auto& entry = page->entries[offset][bank];
                for (const auto& s : entry.slots) {
                    if (s.kind == InlineStructSlot::Kind::String && s.lo != 0u) {
                        runtimeStringPool().markStackRootById(s.lo);
                    }
                }
            }
        }
    }
};

InlineStructPool& inlineStructPool() {
    static InlineStructPool pool;
    return pool;
}

std::uint32_t allocateInlineArenaEntry(const InlineStructArenaEntry& entry) noexcept {
    return inlineStructPool().intern(entry);
}

// ----------------------------------------------------------------------------
// 3. ShardedHeapStructStoragePool
// ----------------------------------------------------------------------------
struct HeapStructStoragePool {
    static constexpr std::uint32_t kPageShift = 7u;
    static constexpr std::uint32_t kPageSize = 1u << kPageShift; // 128 StructStorage slots/page
    static constexpr std::uint32_t kPageMask = kPageSize - 1u;
    static constexpr std::uint32_t kSoftMaxPagesPerShard = 4u;   // 512 slots/shard (32,768 total)
    static constexpr std::uint32_t kHardMaxPagesPerShard = 64u;
    static constexpr std::uint32_t kSoftMaxSlotsPerShard = kSoftMaxPagesPerShard * kPageSize;
    static constexpr std::uint32_t kHardMaxSlotsPerShard = kHardMaxPagesPerShard * kPageSize;
    static constexpr std::uint32_t kRecentGuardWindow = 32u;

    struct HeapStructPage {
        StructStorage storages[kPageSize][2]{};
        std::atomic<std::uint8_t> activeBank[kPageSize]{};
        std::uint64_t hashes[kPageSize]{};
        std::atomic<std::uint8_t> flags[kPageSize]{};
    };

    struct alignas(64) Shard {
        std::mutex mutex;
        std::atomic<HeapStructPage*> pages[kHardMaxPagesPerShard]{};
        std::vector<std::unique_ptr<HeapStructPage>> ownedPages;
        std::unordered_multimap<std::uint64_t, std::uint32_t> dedupByHash;
        std::vector<std::uint32_t> freeList;
        std::uint32_t highWaterMark = 0u;
        std::uint32_t clockHand = 0u;
        std::uint32_t lastAllocatedSlot = kInvalidLocalSlot;
        std::atomic<std::uint32_t> epoch{1u};
        std::atomic<bool> mayHavePinnedOrMarked{false};
        std::atomic<std::uint64_t> clockEvictions{0u};
        std::atomic<std::uint64_t> gcReclaimed{0u};
    };

    Shard shards_[kPoolShardCount]{};

    [[nodiscard]] static std::uint64_t computeFieldsHash(const std::vector<Value>& fields) noexcept {
        std::uint64_t h = 14695981039346656037ULL ^ fields.size();
        for (const auto& v : fields) {
            std::uint64_t vw = static_cast<std::uint64_t>(v.index()) * 0x9e3779b97f4a7c15ULL;
            if (const auto* bv = std::get_if<bool>(&v)) {
                vw ^= *bv ? 1ULL : 0ULL;
            } else if (const auto* byv = std::get_if<ByteValue>(&v)) {
                vw ^= byv->value;
            } else if (const auto* sbv = std::get_if<SByteValue>(&v)) {
                vw ^= static_cast<std::uint8_t>(sbv->value);
            } else if (const auto* shv = std::get_if<ShortValue>(&v)) {
                vw ^= static_cast<std::uint16_t>(shv->value);
            } else if (const auto* ushv = std::get_if<UShortValue>(&v)) {
                vw ^= ushv->value;
            } else if (const auto* uiv = std::get_if<UIntValue>(&v)) {
                vw ^= uiv->value;
            } else if (const auto* ulv = std::get_if<ULongValue>(&v)) {
                vw ^= ulv->value;
            } else if (const auto* iv = std::get_if<std::int64_t>(&v)) {
                vw ^= static_cast<std::uint64_t>(*iv);
            } else if (const auto* lv = std::get_if<LongValue>(&v)) {
                vw ^= static_cast<std::uint64_t>(lv->value);
            } else if (const auto* flv = std::get_if<FloatValue>(&v)) {
                float norm = (flv->value == 0.0f) ? 0.0f : flv->value;
                std::uint32_t fbits = 0;
                std::memcpy(&fbits, &norm, sizeof(fbits));
                vw ^= fbits;
            } else if (const auto* dv = std::get_if<double>(&v)) {
                double norm = (*dv == 0.0) ? 0.0 : *dv;
                std::uint64_t dbits = 0;
                std::memcpy(&dbits, &norm, sizeof(dbits));
                vw ^= dbits;
            } else if (const auto* chv = std::get_if<CharValue>(&v)) {
                vw ^= chv->value;
            } else if (const auto* env = std::get_if<EnumValue>(&v)) {
                vw ^= env->typeId ^ (static_cast<std::uint64_t>(env->value) * 0x9e3779b97f4a7c15ULL);
            } else if (const auto* fx = std::get_if<FixedValue>(&v)) {
                vw ^= static_cast<std::uint32_t>(fx->raw);
            } else if (const auto* fx16 = std::get_if<Fixed16Value>(&v)) {
                vw ^= static_cast<std::uint64_t>(fx16->raw);
            } else if (const auto* v2 = std::get_if<Vec2Value>(&v)) {
                vw ^= static_cast<std::uint32_t>(v2->x) |
                    (static_cast<std::uint64_t>(static_cast<std::uint32_t>(v2->y)) << 32u);
            } else if (const auto* v3 = std::get_if<Vec3Value>(&v)) {
                vw ^= static_cast<std::uint32_t>(v3->x) ^
                    (static_cast<std::uint64_t>(static_cast<std::uint32_t>(v3->y)) << 21u) ^
                    (static_cast<std::uint64_t>(static_cast<std::uint32_t>(v3->z)) << 42u);
            } else if (const auto* qt = std::get_if<QuatValue>(&v)) {
                vw ^= static_cast<std::uint32_t>(qt->x) ^
                    (static_cast<std::uint64_t>(static_cast<std::uint32_t>(qt->y)) << 16u) ^
                    (static_cast<std::uint64_t>(static_cast<std::uint32_t>(qt->z)) << 32u) ^
                    (static_cast<std::uint64_t>(static_cast<std::uint32_t>(qt->w)) << 48u);
            } else if (const auto* sv = std::get_if<StringValue>(&v)) {
                vw ^= sv->id;
            } else if (const auto* st = std::get_if<StructValue>(&v)) {
                vw ^= st->typeId ^ (static_cast<std::uint64_t>(st->storage.raw_) << 16u);
            } else if (const auto* obj = std::get_if<ObjectRef>(&v)) {
                vw ^= (static_cast<std::uint64_t>(obj->slot) << 32u) ^ obj->generation;
            } else if (const auto* nh = std::get_if<NativeHandle>(&v)) {
                vw ^= nh->typeId ^ (static_cast<std::uint64_t>(nh->slot) << 16u) ^ nh->generation;
            }
            h ^= vw + 0x9e3779b97f4a7c15ULL + (h << 6u) + (h >> 2u);
        }
        return h;
    }

    [[nodiscard]] std::uint32_t allocateFreshSlotLocked(Shard& shard) {
        const std::uint32_t slot = shard.highWaterMark++;
        const std::uint32_t pageIdx = slot >> kPageShift;
        if (!shard.pages[pageIdx].load(std::memory_order_relaxed)) {
            auto newPage = std::make_unique<HeapStructPage>();
            HeapStructPage* rawPage = newPage.get();
            shard.ownedPages.push_back(std::move(newPage));
            shard.pages[pageIdx].store(rawPage, std::memory_order_release);
        }
        return slot;
    }

    void removeDedupEntryLocked(Shard& shard, std::uint64_t hash, std::uint32_t slot) {
        auto range = shard.dedupByHash.equal_range(hash);
        for (auto it = range.first; it != range.second; ++it) {
            if (it->second == slot) {
                shard.dedupByHash.erase(it);
                break;
            }
        }
    }

    [[nodiscard]] std::uint32_t tryClockEvictLocked(Shard& shard, bool enforceGuardWindow) {
        const std::uint32_t limit = shard.highWaterMark;
        if (limit == 0u) return kInvalidLocalSlot;
        const std::uint32_t maxScans = limit * 2u;
        for (std::uint32_t scan = 0u; scan < maxScans; ++scan) {
            const std::uint32_t candidate = shard.clockHand;
            shard.clockHand = (shard.clockHand + 1u) % limit;
            if (enforceGuardWindow && shard.lastAllocatedSlot != kInvalidLocalSlot) {
                const std::uint32_t dist = (shard.lastAllocatedSlot >= candidate)
                    ? (shard.lastAllocatedSlot - candidate)
                    : (limit - (candidate - shard.lastAllocatedSlot));
                if (dist < kRecentGuardWindow) continue;
            }

            HeapStructPage* page = shard.pages[candidate >> kPageShift].load(std::memory_order_relaxed);
            if (!page) continue;
            const std::uint32_t offset = candidate & kPageMask;
            auto& fl = page->flags[offset];
            const std::uint8_t f = fl.load(std::memory_order_relaxed);
            if ((f & (kSlotFlagPinned | kSlotFlagGcMarked | kSlotFlagStackRoot)) != 0u) {
                continue;
            }
            if ((f & kSlotFlagClock) != 0u) {
                fl.fetch_and(static_cast<std::uint8_t>(~kSlotFlagClock), std::memory_order_relaxed);
                continue;
            }
            removeDedupEntryLocked(shard, page->hashes[offset], candidate);
            shard.epoch.fetch_add(1u, std::memory_order_release);
            shard.clockEvictions.fetch_add(1u, std::memory_order_relaxed);
            return candidate;
        }
        return kInvalidLocalSlot;
    }

    [[nodiscard]] std::uint32_t acquireSlotLocked(Shard& shard) {
        if (!shard.freeList.empty()) {
            const std::uint32_t slot = shard.freeList.back();
            shard.freeList.pop_back();
            return slot;
        }
        if (shard.highWaterMark < kSoftMaxSlotsPerShard) {
            return allocateFreshSlotLocked(shard);
        }
        const std::uint32_t evicted = tryClockEvictLocked(shard, true);
        if (evicted != kInvalidLocalSlot) {
            return evicted;
        }
        if (shard.highWaterMark < kHardMaxSlotsPerShard) {
            return allocateFreshSlotLocked(shard);
        }
        return tryClockEvictLocked(shard, false);
    }

    std::uint32_t registerStorage(std::shared_ptr<const StructStorage> store, bool pinEntry = false) {
        if (!store) return 0u;
        const std::uint64_t h = computeFieldsHash(store->fields);
        const std::uint32_t shardIdx = static_cast<std::uint32_t>((h >> 58u) & kPoolShardMask);
        Shard& shard = shards_[shardIdx];

        std::lock_guard<std::mutex> lock(shard.mutex);
        auto range = shard.dedupByHash.equal_range(h);
        for (auto it = range.first; it != range.second; ++it) {
            const std::uint32_t localSlot = it->second;
            HeapStructPage* page = shard.pages[localSlot >> kPageShift].load(std::memory_order_relaxed);
            if (page) {
                const std::uint32_t offset = localSlot & kPageMask;
                const std::uint8_t bank = page->activeBank[offset].load(std::memory_order_relaxed) & 1u;
                if (page->storages[offset][bank].fields == store->fields) {
                    const std::uint8_t addFlags = kSlotFlagOccupied | kSlotFlagClock | (pinEntry ? activePinFlags() : 0u);
                    page->flags[offset].fetch_or(addFlags, std::memory_order_relaxed);
                    if (pinEntry) {
                        noteShardPinned(shard.mayHavePinnedOrMarked);
                        for (const auto& f : store->fields) {
                            pinValueInPools(f);
                        }
                    }
                    return encodeShardedPoolId(shardIdx, localSlot);
                }
            }
        }

        const std::uint32_t localSlot = acquireSlotLocked(shard);
        if (localSlot == kInvalidLocalSlot) {
            return 0u;
        }
        shard.lastAllocatedSlot = localSlot;
        HeapStructPage* page = shard.pages[localSlot >> kPageShift].load(std::memory_order_relaxed);
        const std::uint32_t offset = localSlot & kPageMask;
        const std::uint8_t nextBank = (page->activeBank[offset].load(std::memory_order_relaxed) ^ 1u) & 1u;
        page->storages[offset][nextBank].fields.assign(store->fields.begin(), store->fields.end());
        page->activeBank[offset].store(nextBank, std::memory_order_release);
        page->hashes[offset] = h;
        const std::uint8_t initFlags = kSlotFlagOccupied | kSlotFlagClock | (pinEntry ? activePinFlags() : 0u);
        page->flags[offset].store(initFlags, std::memory_order_release);
        if (pinEntry) {
            noteShardPinned(shard.mayHavePinnedOrMarked);
            for (const auto& f : store->fields) {
                pinValueInPools(f);
            }
        }
        shard.dedupByHash.emplace(h, localSlot);
        return encodeShardedPoolId(shardIdx, localSlot);
    }

    // 100% Lock-Free Read Path (No std::mutex on StructStorageHandle::get()!)
    [[nodiscard]] const StructStorage* get(std::uint32_t id) noexcept {
        std::uint32_t shardIdx = 0u;
        std::uint32_t localSlot = 0u;
        if (!decodeShardedPoolId(id, shardIdx, localSlot)) {
            return nullptr;
        }
        const std::uint32_t pageIdx = localSlot >> kPageShift;
        if (pageIdx < kHardMaxPagesPerShard) {
            Shard& shard = shards_[shardIdx];
            if (HeapStructPage* page = shard.pages[pageIdx].load(std::memory_order_acquire)) {
                const std::uint32_t offset = localSlot & kPageMask;
                refreshClockBitIfUnset(page->flags[offset]);
                const std::uint8_t bank = page->activeBank[offset].load(std::memory_order_acquire) & 1u;
                return &page->storages[offset][bank];
            }
        }
        return nullptr;
    }

    void pinById(std::uint32_t id) noexcept {
        std::uint32_t shardIdx = 0u, localSlot = 0u;
        if (!decodeShardedPoolId(id, shardIdx, localSlot)) return;
        const std::uint32_t pageIdx = localSlot >> kPageShift;
        if (pageIdx < kHardMaxPagesPerShard) {
            Shard& shard = shards_[shardIdx];
            if (HeapStructPage* page = shard.pages[pageIdx].load(std::memory_order_acquire)) {
                const std::uint32_t offset = localSlot & kPageMask;
                const std::uint8_t prev = page->flags[offset].fetch_or(
                    activePinFlags(), std::memory_order_relaxed);
                noteShardPinned(shard.mayHavePinnedOrMarked);
                if ((prev & kSlotFlagPinned) == 0u) {
                    const std::uint8_t bank = page->activeBank[offset].load(std::memory_order_acquire) & 1u;
                    for (const auto& f : page->storages[offset][bank].fields) {
                        pinValueInPools(f);
                    }
                }
            }
        }
    }

    void markById(std::uint32_t id) noexcept {
        std::uint32_t shardIdx = 0u, localSlot = 0u;
        if (!decodeShardedPoolId(id, shardIdx, localSlot)) return;
        const std::uint32_t pageIdx = localSlot >> kPageShift;
        if (pageIdx < kHardMaxPagesPerShard) {
            Shard& shard = shards_[shardIdx];
            if (HeapStructPage* page = shard.pages[pageIdx].load(std::memory_order_acquire)) {
                const std::uint32_t offset = localSlot & kPageMask;
                const std::uint8_t prev = page->flags[offset].fetch_or(
                    kSlotFlagGcMarked | kSlotFlagPinned, std::memory_order_relaxed);
                noteShardPinned(shard.mayHavePinnedOrMarked);
                if ((prev & kSlotFlagGcMarked) == 0u) {
                    const std::uint8_t bank = page->activeBank[offset].load(std::memory_order_acquire) & 1u;
                    for (const auto& f : page->storages[offset][bank].fields) {
                        markValueInPools(f);
                    }
                }
            }
        }
    }

    void markStackRootById(std::uint32_t id) noexcept {
        std::uint32_t shardIdx = 0u, localSlot = 0u;
        if (!decodeShardedPoolId(id, shardIdx, localSlot)) return;
        const std::uint32_t pageIdx = localSlot >> kPageShift;
        if (pageIdx < kHardMaxPagesPerShard) {
            Shard& shard = shards_[shardIdx];
            if (HeapStructPage* page = shard.pages[pageIdx].load(std::memory_order_acquire)) {
                const std::uint32_t offset = localSlot & kPageMask;
                const std::uint8_t prev = page->flags[offset].fetch_or(
                    kSlotFlagStackRoot | kSlotFlagClock, std::memory_order_relaxed);
                noteShardMarked(shard.mayHavePinnedOrMarked);
                if ((prev & kSlotFlagStackRoot) == 0u) {
                    const std::uint8_t bank = page->activeBank[offset].load(std::memory_order_acquire) & 1u;
                    for (const auto& f : page->storages[offset][bank].fields) {
                        markStackRootValueInPools(f);
                    }
                }
            }
        }
    }
};

HeapStructStoragePool& heapStructPool() {
    static HeapStructStoragePool pool;
    return pool;
}

StructStorageHandle::StructStorageHandle(std::shared_ptr<const StructStorage> store)
    : raw_(heapStructPool().registerStorage(std::move(store))) {}

StructStorageHandle::StructStorageHandle(std::shared_ptr<StructStorage> store)
    : raw_(heapStructPool().registerStorage(std::move(store))) {}

const StructStorage* StructStorageHandle::get() const noexcept {
    if (!isStoragePtr()) return nullptr;
    return heapStructPool().get(raw_);
}

const InlineStructArenaEntry& StructValue::inlineEntry() const noexcept {
    return inlineStructPool().get(storage.inlineArenaIndex());
}

std::uint16_t StructValue::nestedFieldMask() const noexcept {
    return isInline ? inlineEntry().nestedFieldMask : 0u;
}

semantic::SymbolId StructValue::nestedTypeId() const noexcept {
    return isInline ? inlineEntry().nestedTypeId : 0u;
}

bool operator==(const StructValue& left, const StructValue& right) {
    if (left.typeId != right.typeId) return false;
    const auto count = left.fieldCount();
    if (count != right.fieldCount()) return false;
    if (left.isInline && right.isInline) {
        if (left.storage.raw_ == right.storage.raw_ &&
            left.inlineSlotCount == right.inlineSlotCount &&
            left.packedOffsetsAndLengths == right.packedOffsetsAndLengths) {
            return true;
        }
        const auto& le = left.inlineEntry();
        const auto& re = right.inlineEntry();
        if (le.nestedTypeId == re.nestedTypeId &&
            left.inlineSlotCount == right.inlineSlotCount &&
            left.packedOffsetsAndLengths == right.packedOffsetsAndLengths &&
            le.nestedFieldMask == re.nestedFieldMask) {
            const std::size_t slots = left.inlineSlotCount != 0 ? left.inlineSlotCount : count;
            for (std::size_t i = 0; i < slots; ++i) {
                if (le.slots[i] != re.slots[i]) return false;
            }
            return true;
        }
    }
    if (!left.isInline && !right.isInline) {
        if (left.storage == right.storage) return true;
        if (!left.storage || !right.storage) return false;
        return left.storage->fields == right.storage->fields;
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (!(structValueFieldAt(left, i) == structValueFieldAt(right, i))) {
            return false;
        }
    }
    return true;
}

StringValue::StringValue(const std::string& value) {
    *this = runtimeStringPool().intern(value);
}

StringValue::StringValue(std::string&& value) {
    *this = runtimeStringPool().intern(value);
}

StringValue::StringValue(std::string_view value) {
    *this = runtimeStringPool().intern(value);
}

const std::string& StringValue::str() const noexcept {
    return ptr ? *ptr : runtimeStringPool().empty();
}

namespace {

bool tryPackInlineStructFields(
    semantic::SymbolId typeId,
    ValueSpan fields,
    StructValue& outVal) noexcept {
    if (fields.size() > StructValue::MaxInlineFields) return false;
    outVal = StructValue{};
    outVal.typeId = typeId;
    outVal.inlineCount = static_cast<std::uint8_t>(fields.size());
    outVal.isInline = true;
    InlineStructArenaEntry entry{};
    std::size_t slotCursor = 0;
    bool anyNested = false;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (auto slot = encodeInlineStructSlot(fields[i])) {
            if (slotCursor + 1 > StructValue::MaxInlineSlots) return false;
            outVal.packedOffsetsAndLengths |=
                static_cast<std::uint8_t>((slotCursor & 0x3u) << (i * 2));
            entry.nestedFieldMask |= static_cast<std::uint16_t>(1u << (i * 2));
            entry.slots[slotCursor++] = *slot;
            continue;
        }
        if (const auto* nested = std::get_if<StructValue>(&fields[i]);
            nested && nested->isInline && nested->inlineSlotCount > 0 &&
            nested->inlineSlotCount <= 3 && (nested->nestedFieldMask() >> 8u) == 0 &&
            nested->nestedTypeId() == 0 && !anyNested) {
            if (slotCursor + nested->inlineSlotCount <= StructValue::MaxInlineSlots) {
                anyNested = true;
                const auto& nestedEntry = nested->inlineEntry();
                entry.nestedTypeId = nested->typeId;
                outVal.packedOffsetsAndLengths |=
                    static_cast<std::uint8_t>((slotCursor & 0x3u) << (i * 2));
                entry.nestedFieldMask |=
                    static_cast<std::uint16_t>((nested->inlineSlotCount & 0x3u) << (i * 2));
                entry.nestedFieldMask |=
                    static_cast<std::uint16_t>(((i & 0x3u) + 1u) << 8u);
                for (std::size_t k = 0; k < nested->inlineSlotCount; ++k) {
                    entry.slots[slotCursor++] = nestedEntry.slots[k];
                }
                continue;
            }
        }
        return false;
    }
    if (!anyNested) {
        entry.nestedTypeId = 0;
        outVal.packedOffsetsAndLengths = 0;
        entry.nestedFieldMask = 0;
    }
    outVal.inlineSlotCount = static_cast<std::uint8_t>(slotCursor);
    outVal.storage.setInlineArenaIndex(allocateInlineArenaEntry(entry));
    return true;
}

} // namespace

StructValue makeStructValueFromFields(
    semantic::SymbolId typeId,
    ValueSpan fields) {
    StructValue inlineVal;
    if (tryPackInlineStructFields(typeId, fields, inlineVal)) {
        return inlineVal;
    }
    auto storage = std::make_shared<StructStorage>();
    storage->fields.assign(fields.begin(), fields.end());
    return StructValue{typeId, std::move(storage)};
}

StructValue makeStructValueFromFields(
    semantic::SymbolId typeId,
    std::vector<Value> fields) {
    StructValue inlineVal;
    if (tryPackInlineStructFields(typeId, ValueSpan(fields), inlineVal)) {
        return inlineVal;
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
        const auto& entry = structure.inlineEntry();
        const std::uint8_t highByte = static_cast<std::uint8_t>(entry.nestedFieldMask >> 8u);
        if (highByte != 0) {
            const std::size_t nestedIdx = static_cast<std::size_t>(highByte - 1u) & 0x3u;
            const std::uint8_t offset = structure.fieldOffset(index);
            const std::uint8_t len = structure.fieldLength(index);
            if (index == nestedIdx) {
                const semantic::SymbolId nestedType = entry.nestedTypeId;
                if (nestedType == 0 || offset + len > structure.inlineSlotCount) {
                    return std::nullopt;
                }
                StructValue nested;
                nested.typeId = nestedType;
                nested.isInline = true;
                nested.inlineCount = len;
                nested.inlineSlotCount = len;
                InlineStructArenaEntry nestedEntry{};
                for (std::uint8_t k = 0; k < len; ++k) {
                    nestedEntry.slots[k] = entry.slots[offset + k];
                }
                nested.storage.setInlineArenaIndex(allocateInlineArenaEntry(nestedEntry));
                return Value{nested};
            }
            if (offset >= structure.inlineSlotCount) return std::nullopt;
            return decodeInlineStructSlot(entry.slots[offset]);
        }
        return decodeInlineStructSlot(entry.slots[index]);
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
        return structValueFieldAt(*s, fieldIndex);
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
            std::array<Value, StructValue::MaxInlineFields> unpacked{};
            for (std::size_t i = 0; i < s->inlineCount; ++i) {
                if (i == fieldIndex) {
                    unpacked[i] = fieldValue;
                } else {
                    const auto existing = structValueFieldAt(*s, i);
                    if (!existing) return std::nullopt;
                    unpacked[i] = *existing;
                }
            }
            return Value{makeStructValueFromFields(
                s->typeId,
                ValueSpan(unpacked.data(), s->inlineCount))};
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
    if (const auto* nativeIds = heap->nativeEcsEntityIds(typeName)) {
        outIds = *nativeIds;
        if (outValsRef) *outValsRef = ObjectRef{};
        return true;
    }
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
            if (heap->hasNativeEcsTable(t)) {
                return true;
            }
            if (heap->hasStaticField(semantic::stableTypeId("$ecs_sparse_ids::" + t))) {
                heap->notifySparseEcsState();
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

bool isStringLikeValue(const Value& val) noexcept {
    if (std::holds_alternative<std::string>(val) ||
        std::holds_alternative<NullString>(val)) {
        return true;
    }
    if (const auto* ref = std::get_if<ObjectRef>(&val)) {
        return ref->kind == ObjectKind::String;
    }
    return false;
}

bool intrinsicValuesEqual(const Value& a, const Value& b, const ManagedHeap* heap) {
    if (isStringLikeValue(a) || isStringLikeValue(b)) {
        const bool aNull = std::holds_alternative<NullString>(a);
        const bool bNull = std::holds_alternative<NullString>(b);
        if (aNull || bNull) return aNull && bNull;
        return extractIntrinsicString(a, heap) == extractIntrinsicString(b, heap);
    }
    return a == b;
}

bool intrinsicValueLess(const Value& a, const Value& b, const ManagedHeap* heap) {
    if (isStringLikeValue(a) || isStringLikeValue(b)) {
        const bool aNull = std::holds_alternative<NullString>(a);
        const bool bNull = std::holds_alternative<NullString>(b);
        if (aNull != bNull) return aNull;
        if (aNull && bNull) return false;
        return extractIntrinsicString(a, heap) < extractIntrinsicString(b, heap);
    }
    if (a.index() == b.index()) {
        if (const auto* ia = std::get_if<std::int64_t>(&a)) return *ia < std::get<std::int64_t>(b);
        if (const auto* la = std::get_if<LongValue>(&a)) return la->value < std::get<LongValue>(b).value;
        if (const auto* uia = std::get_if<UIntValue>(&a)) return uia->value < std::get<UIntValue>(b).value;
        if (const auto* ula = std::get_if<ULongValue>(&a)) return ula->value < std::get<ULongValue>(b).value;
        if (const auto* bya = std::get_if<ByteValue>(&a)) return bya->value < std::get<ByteValue>(b).value;
        if (const auto* sba = std::get_if<SByteValue>(&a)) return sba->value < std::get<SByteValue>(b).value;
        if (const auto* sha = std::get_if<ShortValue>(&a)) return sha->value < std::get<ShortValue>(b).value;
        if (const auto* usa = std::get_if<UShortValue>(&a)) return usa->value < std::get<UShortValue>(b).value;
        if (const auto* cha = std::get_if<CharValue>(&a)) return cha->value < std::get<CharValue>(b).value;
        if (const auto* ba = std::get_if<bool>(&a)) return (!*ba && std::get<bool>(b));
        if (const auto* ena = std::get_if<EnumValue>(&a)) {
            const auto& enb = std::get<EnumValue>(b);
            return ena->typeId != enb.typeId ? (ena->typeId < enb.typeId) : (ena->value < enb.value);
        }
        if (const auto* fxa = std::get_if<FixedValue>(&a)) return fxa->raw < std::get<FixedValue>(b).raw;
        if (const auto* f16a = std::get_if<Fixed16Value>(&a)) return f16a->raw < std::get<Fixed16Value>(b).raw;
        if (const auto* da = std::get_if<double>(&a)) return *da < std::get<double>(b);
        if (const auto* fla = std::get_if<FloatValue>(&a)) return fla->value < std::get<FloatValue>(b).value;
    }
    return valueToString(a, heap) < valueToString(b, heap);
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
            struct CachedEcsColSpec {
                bool valid = false;
                std::size_t colIdx = 0;
                std::string specStr;
                std::string targetType;
                std::vector<std::string> cols;
                std::vector<std::string> withs;
                std::vector<std::string> withouts;
                semantic::SymbolId denseId = 0;
                semantic::SymbolId emptyFallbackId = 0;
                semantic::SymbolId arrTypeId = 0;
                semantic::SymbolId elemTypeId = 0;
                semantic::PrimitiveType elemPrim = semantic::PrimitiveType::Struct;
                semantic::SymbolId wbMapId = 0;
                semantic::SymbolId wbTargetId = 0;
            };
            thread_local std::unordered_map<std::string, CachedEcsColSpec> colCache;
            const std::string restKey(rest);
            auto cacheIt = colCache.find(restKey);
            if (cacheIt == colCache.end()) {
                CachedEcsColSpec entry;
                const auto sep = rest.find("::");
                if (sep != std::string_view::npos) {
                    entry.colIdx = static_cast<std::size_t>(
                        std::strtoull(std::string(rest.substr(0, sep)).c_str(), nullptr, 10));
                    const auto specView = rest.substr(sep + 2);
                    entry.specStr = std::string(specView);
                    parseEcsQuerySpec(specView, entry.cols, entry.withs, entry.withouts);
                    if (entry.colIdx < entry.cols.size()) {
                        entry.valid = true;
                        entry.targetType = entry.cols[entry.colIdx];
                        entry.denseId = semantic::stableTypeId("$ecs_column::" + entry.targetType);
                        entry.emptyFallbackId = semantic::stableTypeId("$ecs_empty_col::" + entry.targetType);
                        entry.elemPrim = semantic::PrimitiveType::Struct;
                        entry.elemTypeId = semantic::stableTypeId(entry.targetType);
                        entry.arrTypeId = semantic::stableTypeId(
                            semantic::arrayTypeName(entry.elemPrim, entry.targetType));
                        entry.wbMapId = semantic::stableTypeId("$ecs_wb_map::" + restKey);
                        entry.wbTargetId = semantic::stableTypeId("$ecs_wb_target::" + restKey);
                    }
                }
                cacheIt = colCache.emplace(restKey, std::move(entry)).first;
            }
            const auto& cached = cacheIt->second;
            if (!cached.valid) {
                errorMessage = "malformed ecs_col intrinsic";
                return false;
            }
            const auto getOrCreateEmptyColArray = [&]() -> Value {
                if (!arguments.empty() && std::holds_alternative<ObjectRef>(arguments[0])) {
                    return arguments[0];
                }
                if (!heap) return NullArray{};
                if (auto existingEmpty = heap->getStaticField(cached.emptyFallbackId)) {
                    return *existingEmpty;
                }
                RuntimeError allocErr;
                Value defaultElem = makeDefaultInlineStructValue(cached.elemTypeId)
                    .value_or(Value{StructValue{cached.elemTypeId, std::make_shared<StructStorage>()}});
                if (const auto emptyRef = heap->allocateTypedArray(
                        cached.arrTypeId, cached.elemPrim, cached.elemTypeId, 0, defaultElem, &allocErr)) {
                    heap->setStaticField(cached.emptyFallbackId, *emptyRef);
                    return *emptyRef;
                }
                return NullArray{};
            };
            const auto& targetType = cached.targetType;
            const auto& cols = cached.cols;
            const auto& withs = cached.withs;
            const auto& withouts = cached.withouts;
            const auto colIdx = cached.colIdx;
            const std::string_view spec = cached.specStr;
            if (!hasAnySparseEcsTable(heap, cols, withs, withouts) &&
                withs.empty() && withouts.empty()) {
                if (heap) {
                    if (auto existing = heap->getStaticField(cached.denseId)) {
                        result = std::move(*existing);
                        return true;
                    }
                }
                result = getOrCreateEmptyColArray();
                if (heap && std::holds_alternative<ObjectRef>(result)) {
                    heap->setStaticField(cached.denseId, result);
                }
                return true;
            }
            if (!heap) {
                result = getOrCreateEmptyColArray();
                return true;
            }
            semantic::SymbolId arrTypeId = cached.arrTypeId;
            semantic::PrimitiveType elemPrim = cached.elemPrim;
            semantic::SymbolId elemTypeId = cached.elemTypeId;
            if (!arguments.empty() && std::holds_alternative<ObjectRef>(arguments[0])) {
                const auto tmplRef = std::get<ObjectRef>(arguments[0]);
                arrTypeId = heap->arrayTypeId(tmplRef).value_or(arrTypeId);
                elemPrim = heap->arrayElementType(tmplRef).value_or(elemPrim);
                elemTypeId = heap->arrayElementTypeId(tmplRef).value_or(elemTypeId);
            }
            const auto matched = computeMatchedEcsEntities(heap, cols, withs, withouts);
            if (matched.empty()) {
                result = getOrCreateEmptyColArray();
                return true;
            }
            const auto* nativeIds = heap->nativeEcsEntityIds(targetType);
            const auto* nativeVals = heap->nativeEcsComponentValues(targetType);
            std::vector<std::int64_t> sparseIds;
            ObjectRef sparseValsRef{};
            if (!nativeIds || !nativeVals) {
                if (!readSparseIds(heap, targetType, sparseIds, &sparseValsRef) ||
                    !sparseValsRef.valid()) {
                    result = getOrCreateEmptyColArray();
                    return true;
                }
            }
            std::vector<Value> matchedVals;
            matchedVals.reserve(matched.size());
            std::vector<std::int64_t> matchedSrcIndices;
            matchedSrcIndices.reserve(matched.size());
            for (const auto entId : matched) {
                if (nativeIds && nativeVals) {
                    const auto it = std::lower_bound(nativeIds->begin(), nativeIds->end(), entId);
                    if (it != nativeIds->end() && *it == entId) {
                        const auto idx = static_cast<std::size_t>(std::distance(nativeIds->begin(), it));
                        if (idx < nativeVals->size()) {
                            matchedVals.push_back((*nativeVals)[idx]);
                            matchedSrcIndices.push_back(static_cast<std::int64_t>(idx));
                        }
                    }
                } else {
                    const auto it = std::lower_bound(sparseIds.begin(), sparseIds.end(), entId);
                    if (it != sparseIds.end() && *it == entId) {
                        const auto idx = static_cast<std::size_t>(std::distance(sparseIds.begin(), it));
                        if (auto v = heap->arrayGet(sparseValsRef, idx)) {
                            matchedVals.push_back(std::move(*v));
                            matchedSrcIndices.push_back(static_cast<std::int64_t>(idx));
                        }
                    }
                }
            }
            if (matchedVals.empty()) {
                result = getOrCreateEmptyColArray();
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
            const auto intArrTypeId = semantic::stableTypeId(
                semantic::arrayTypeName(semantic::PrimitiveType::Int, {}));
            if (const auto mapArr = heap->allocateTypedArray(
                    intArrTypeId,
                    semantic::PrimitiveType::Int,
                    0,
                    matchedSrcIndices.size(),
                    std::int64_t{0},
                    &allocErr)) {
                for (std::size_t i = 0; i < matchedSrcIndices.size(); ++i) {
                    (void)heap->arraySet(*mapArr, i, matchedSrcIndices[i], &allocErr);
                }
                const std::string wbMapKey =
                    "$ecs_wb_map::" + std::to_string(colIdx) + "::" + std::string(spec);
                const std::string wbTargetKey =
                    "$ecs_wb_target::" + std::to_string(colIdx) + "::" + std::string(spec);
                heap->setStaticField(semantic::stableTypeId(wbMapKey), *mapArr);
                heap->setStaticField(semantic::stableTypeId(wbTargetKey), sparseValsRef);
                heap->notifySparseEcsState();
            }
            result = *outArr;
            return true;
        }

        constexpr std::string_view ecsEntPrefix = "ecs_ent::";
        if (subOp.rfind(ecsEntPrefix, 0) == 0) {
            const auto spec = subOp.substr(ecsEntPrefix.size());
            std::vector<std::string> cols, withs, withouts;
            parseEcsQuerySpec(spec, cols, withs, withouts);
            const auto arrTypeId = (!arguments.empty() && std::holds_alternative<ObjectRef>(arguments[0]) && heap)
                ? heap->arrayTypeId(std::get<ObjectRef>(arguments[0])).value_or(0)
                : semantic::stableTypeId(semantic::arrayTypeName(semantic::PrimitiveType::Int, {}));
            const auto getOrCreateEmptyEntArray = [&]() -> Value {
                if (!arguments.empty() && std::holds_alternative<ObjectRef>(arguments[0])) {
                    return arguments[0];
                }
                if (!heap) return NullArray{};
                const auto emptyEntId = semantic::stableTypeId("$ecs_empty_ent_arr");
                if (auto existingEmpty = heap->getStaticField(emptyEntId)) {
                    return *existingEmpty;
                }
                RuntimeError allocErr;
                if (const auto emptyRef = heap->allocateTypedArray(
                        arrTypeId, semantic::PrimitiveType::Int, 0, 0, std::int64_t{0}, &allocErr)) {
                    heap->setStaticField(emptyEntId, *emptyRef);
                    return *emptyRef;
                }
                return NullArray{};
            };
            if (!heap) {
                result = getOrCreateEmptyEntArray();
                return true;
            }
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
                result = getOrCreateEmptyEntArray();
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
        if (arguments.size() < 2 || !heap) {
            result = std::monostate{};
            return true;
        }
        const auto* idxPtr = std::get_if<std::int64_t>(&arguments[0]);
        if (!idxPtr || *idxPtr < 0) {
            result = std::monostate{};
            return true;
        }
        struct CachedWritebackSpec {
            bool valid = false;
            std::size_t colIdx = 0;
            std::string compKey;
            std::vector<std::string> cols;
            std::vector<std::string> withs;
            std::vector<std::string> withouts;
            semantic::SymbolId wbMapId = 0;
            semantic::SymbolId wbTargetId = 0;
            semantic::SymbolId sparseIdsId = 0;
        };
        thread_local std::unordered_map<std::string, CachedWritebackSpec> wbCache;
        const std::string restKey(rest);
        auto cacheIt = wbCache.find(restKey);
        if (cacheIt == wbCache.end()) {
            CachedWritebackSpec entry;
            const auto sep = rest.find("::");
            if (sep != std::string_view::npos) {
                entry.colIdx = static_cast<std::size_t>(
                    std::strtoull(std::string(rest.substr(0, sep)).c_str(), nullptr, 10));
                const auto spec = rest.substr(sep + 2);
                parseEcsQuerySpec(spec, entry.cols, entry.withs, entry.withouts);
                if (entry.colIdx < entry.cols.size()) {
                    entry.valid = true;
                    entry.compKey = entry.cols[entry.colIdx];
                    entry.wbMapId = semantic::stableTypeId("$ecs_wb_map::" + restKey);
                    entry.wbTargetId = semantic::stableTypeId("$ecs_wb_target::" + restKey);
                    entry.sparseIdsId = semantic::stableTypeId("$ecs_sparse_ids::" + entry.compKey);
                }
            }
            cacheIt = wbCache.emplace(restKey, std::move(entry)).first;
        }
        const auto& cached = cacheIt->second;
        if (!cached.valid) {
            result = std::monostate{};
            return true;
        }
        // Fast O(1) exit for dense columns and zero-copy host C++ raw columns (StoreElement already mutated in-place)
        if (!heap->hasSparseEcsState() &&
            !heap->hasStaticField(cached.wbMapId) &&
            !heap->hasStaticField(cached.sparseIdsId)) {
            result = std::monostate{};
            return true;
        }
        const auto idx = static_cast<std::size_t>(*idxPtr);
        const std::string& compKey = cached.compKey;
        const auto& cols = cached.cols;
        const auto& withs = cached.withs;
        const auto& withouts = cached.withouts;
        if (const auto mapVal = heap->getStaticField(cached.wbMapId);
            mapVal && std::holds_alternative<ObjectRef>(*mapVal)) {
            const auto mapRef = std::get<ObjectRef>(*mapVal);
            if (const auto srcIdxVal = heap->arrayGet(mapRef, idx);
                srcIdxVal && std::holds_alternative<std::int64_t>(*srcIdxVal)) {
                const auto srcIdx = static_cast<std::size_t>(std::get<std::int64_t>(*srcIdxVal));
                if (heap->hasNativeEcsTable(compKey)) {
                    (void)heap->setNativeEcsComponentByDenseIndex(compKey, srcIdx, arguments[1]);
                    result = std::monostate{};
                    return true;
                }
                const auto targetVal = heap->getStaticField(cached.wbTargetId);
                if (targetVal && std::holds_alternative<ObjectRef>(*targetVal)) {
                    const auto targetRef = std::get<ObjectRef>(*targetVal);
                    RuntimeError setErr;
                    (void)heap->arraySet(targetRef, srcIdx, arguments[1], &setErr);
                    result = std::monostate{};
                    return true;
                }
            }
        }
        if (heap->hasNativeEcsTable(compKey)) {
            const auto matched = computeMatchedEcsEntities(heap, cols, withs, withouts);
            if (idx < matched.size()) {
                (void)heap->setNativeEcsComponent(compKey, matched[idx], arguments[1]);
            }
            result = std::monostate{};
            return true;
        }
        if (!heap->hasStaticField(cached.sparseIdsId)) {
            result = std::monostate{};
            return true;
        }
        heap->notifySparseEcsState();
        const auto matched = computeMatchedEcsEntities(heap, cols, withs, withouts);
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

    if (op == "stable_hash" || op.rfind("stable_hash::", 0) == 0) {
        if (arguments.empty()) {
            result = std::int64_t{0};
            return true;
        }
        std::uint64_t h = 1469598103934665603ULL;
        const auto mixU64 = [&](std::uint64_t v) {
            for (unsigned shift = 0; shift < 64; shift += 8) {
                h ^= static_cast<std::uint8_t>((v >> shift) & 0xffu);
                h *= 1099511628211ULL;
            }
        };
        const auto mixStr = [&](std::string_view s) {
            mixU64(s.size());
            for (unsigned char ch : s) {
                h ^= ch;
                h *= 1099511628211ULL;
            }
        };
        const auto hashVal = [&](const auto& self, const Value& v) -> void {
            mixU64(v.index());
            if (const auto* b = std::get_if<bool>(&v)) mixU64(*b ? 1u : 0u);
            else if (const auto* i = std::get_if<std::int64_t>(&v)) mixU64(static_cast<std::uint64_t>(*i));
            else if (const auto* l = std::get_if<LongValue>(&v)) mixU64(static_cast<std::uint64_t>(l->value));
            else if (const auto* ui = std::get_if<UIntValue>(&v)) mixU64(ui->value);
            else if (const auto* ul = std::get_if<ULongValue>(&v)) mixU64(ul->value);
            else if (const auto* by = std::get_if<ByteValue>(&v)) mixU64(by->value);
            else if (const auto* sb = std::get_if<SByteValue>(&v)) mixU64(static_cast<std::uint8_t>(sb->value));
            else if (const auto* sh = std::get_if<ShortValue>(&v)) mixU64(static_cast<std::uint16_t>(sh->value));
            else if (const auto* us = std::get_if<UShortValue>(&v)) mixU64(us->value);
            else if (const auto* ch = std::get_if<CharValue>(&v)) mixU64(ch->value);
            else if (const auto* en = std::get_if<EnumValue>(&v)) { mixU64(en->typeId); mixU64(static_cast<std::uint64_t>(en->value)); }
            else if (const auto* fx = std::get_if<FixedValue>(&v)) mixU64(static_cast<std::uint32_t>(fx->raw));
            else if (const auto* f16 = std::get_if<Fixed16Value>(&v)) mixU64(static_cast<std::uint64_t>(f16->raw));
            else if (const auto* v2 = std::get_if<Vec2Value>(&v)) { mixU64(static_cast<std::uint32_t>(v2->x)); mixU64(static_cast<std::uint32_t>(v2->y)); }
            else if (const auto* v3 = std::get_if<Vec3Value>(&v)) { mixU64(static_cast<std::uint32_t>(v3->x)); mixU64(static_cast<std::uint32_t>(v3->y)); mixU64(static_cast<std::uint32_t>(v3->z)); }
            else if (const auto* q = std::get_if<QuatValue>(&v)) { mixU64(static_cast<std::uint32_t>(q->x)); mixU64(static_cast<std::uint32_t>(q->y)); mixU64(static_cast<std::uint32_t>(q->z)); mixU64(static_cast<std::uint32_t>(q->w)); }
            else if (const auto* d = std::get_if<double>(&v)) {
                std::uint64_t bits = 0;
                const double norm = (*d == 0.0) ? 0.0 : *d;
                std::memcpy(&bits, &norm, sizeof(bits));
                mixU64(bits);
            } else if (const auto* fl = std::get_if<FloatValue>(&v)) {
                std::uint32_t bits = 0;
                const float norm = (fl->value == 0.0f) ? 0.0f : fl->value;
                std::memcpy(&bits, &norm, sizeof(bits));
                mixU64(bits);
            } else if (const auto* s = std::get_if<std::string>(&v)) {
                mixStr(*s);
            } else if (const auto* o = std::get_if<ObjectRef>(&v)) {
                if (o->kind == ObjectKind::String && heap) {
                    if (const auto sv = heap->stringView(*o)) {
                        mixStr(*sv);
                        return;
                    }
                }
                mixU64((static_cast<std::uint64_t>(o->generation) << 32u) | o->slot);
            } else if (const auto* st = std::get_if<StructValue>(&v)) {
                mixU64(st->typeId);
                for (std::size_t idx = 0; idx < st->fieldCount(); ++idx) {
                    if (const auto fld = structValueFieldAt(*st, idx)) {
                        self(self, *fld);
                    }
                }
            }
        };
        hashVal(hashVal, arguments[0]);
        result = static_cast<std::int64_t>(static_cast<std::int32_t>(h & 0x7fffffffULL));
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

    if (op.rfind("array_", 0) == 0) {
        if (arguments.empty() || std::holds_alternative<NullArray>(arguments[0])) {
            errorMessage = "cannot invoke array method on null array";
            return false;
        }
        const auto* arrRef = std::get_if<ObjectRef>(&arguments[0]);
        const auto lenOpt = (arrRef && heap) ? heap->arrayLength(*arrRef) : std::nullopt;
        if (!lenOpt) {
            errorMessage = "invalid array reference";
            return false;
        }
        const std::size_t len = *lenOpt;

        if (op.rfind("array_index_of::", 0) == 0) {
            if (arguments.size() < 2) {
                errorMessage = "array_index_of requires 2 arguments";
                return false;
            }
            std::int64_t found = -1;
            for (std::size_t i = 0; i < len; ++i) {
                const auto elem = heap->arrayGet(*arrRef, i);
                if (elem && intrinsicValuesEqual(*elem, arguments[1], heap)) {
                    found = static_cast<std::int64_t>(i);
                    break;
                }
            }
            result = found;
            return true;
        }
        if (op.rfind("array_last_index_of::", 0) == 0) {
            if (arguments.size() < 2) {
                errorMessage = "array_last_index_of requires 2 arguments";
                return false;
            }
            std::int64_t found = -1;
            for (std::size_t i = len; i > 0; --i) {
                const auto elem = heap->arrayGet(*arrRef, i - 1);
                if (elem && intrinsicValuesEqual(*elem, arguments[1], heap)) {
                    found = static_cast<std::int64_t>(i - 1);
                    break;
                }
            }
            result = found;
            return true;
        }
        if (op.rfind("array_contains::", 0) == 0) {
            if (arguments.size() < 2) {
                errorMessage = "array_contains requires 2 arguments";
                return false;
            }
            bool found = false;
            for (std::size_t i = 0; i < len; ++i) {
                const auto elem = heap->arrayGet(*arrRef, i);
                if (elem && intrinsicValuesEqual(*elem, arguments[1], heap)) {
                    found = true;
                    break;
                }
            }
            result = found;
            return true;
        }
        if (op.rfind("array_reverse::", 0) == 0) {
            for (std::size_t i = 0; i < len / 2; ++i) {
                const std::size_t j = len - 1 - i;
                const auto leftVal = heap->arrayGet(*arrRef, i);
                const auto rightVal = heap->arrayGet(*arrRef, j);
                if (leftVal && rightVal) {
                    RuntimeError setErr;
                    (void)heap->arraySet(*arrRef, i, *rightVal, &setErr);
                    (void)heap->arraySet(*arrRef, j, *leftVal, &setErr);
                }
            }
            result = std::monostate{};
            return true;
        }
        if (op.rfind("array_sort::", 0) == 0 || op.rfind("array_sort_range::", 0) == 0) {
            std::size_t sortLen = len;
            if (op.rfind("array_sort_range::", 0) == 0 && arguments.size() >= 2) {
                const auto rawCount = std::holds_alternative<std::int64_t>(arguments[1])
                    ? std::get<std::int64_t>(arguments[1])
                    : 0;
                const auto clampedCount = std::clamp<std::int64_t>(
                    rawCount, 0, static_cast<std::int64_t>(len));
                sortLen = static_cast<std::size_t>(clampedCount);
            }
            if (sortLen > 1) {
                std::vector<Value> elems;
                elems.reserve(sortLen);
                for (std::size_t i = 0; i < sortLen; ++i) {
                    elems.push_back(heap->arrayGet(*arrRef, i).value_or(Value{}));
                }
                std::stable_sort(
                    elems.begin(),
                    elems.end(),
                    [heap](const Value& a, const Value& b) {
                        return intrinsicValueLess(a, b, heap);
                    });
                RuntimeError setErr;
                for (std::size_t i = 0; i < sortLen; ++i) {
                    (void)heap->arraySet(*arrRef, i, elems[i], &setErr);
                }
            }
            result = std::monostate{};
            return true;
        }
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
    if (op == "string_last_index_of") {
        const auto s = extractIntrinsicString(arguments[0], heap);
        const auto sub = extractIntrinsicString(arguments[1], heap);
        const auto pos = s.rfind(sub);
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
    if (op == "string_trim_start") {
        const auto s = extractIntrinsicString(arguments[0], heap);
        const auto isSpace = [](unsigned char c) {
            return c == ' ' || c == '\t' || c == '\r' ||
                   c == '\n' || c == '\v' || c == '\f';
        };
        std::size_t first = 0;
        while (first < s.size() && isSpace(static_cast<unsigned char>(s[first]))) {
            ++first;
        }
        return makeIntrinsicStringResult(
            s.substr(first), heap, result, errorMessage);
    }
    if (op == "string_trim_end") {
        const auto s = extractIntrinsicString(arguments[0], heap);
        const auto isSpace = [](unsigned char c) {
            return c == ' ' || c == '\t' || c == '\r' ||
                   c == '\n' || c == '\v' || c == '\f';
        };
        std::size_t last = s.size();
        while (last > 0 && isSpace(static_cast<unsigned char>(s[last - 1]))) {
            --last;
        }
        return makeIntrinsicStringResult(
            s.substr(0, last), heap, result, errorMessage);
    }
    if (op == "string_split") {
        if (!heap) {
            errorMessage = "string_split requires a managed heap";
            return false;
        }
        const auto s = extractIntrinsicString(arguments[0], heap);
        const auto sep = extractIntrinsicString(arguments[1], heap);
        std::vector<std::string> parts;
        if (sep.empty()) {
            parts.push_back(s);
        } else {
            std::size_t start = 0;
            while (true) {
                const auto pos = s.find(sep, start);
                if (pos == std::string::npos) {
                    parts.push_back(s.substr(start));
                    break;
                }
                parts.push_back(s.substr(start, pos - start));
                start = pos + sep.size();
            }
        }
        const auto arrTypeId = semantic::stableTypeId(
            semantic::arrayTypeName(semantic::PrimitiveType::String, ""));
        RuntimeError allocErr;
        const auto defaultStrRef = heap->allocateString("", &allocErr);
        if (!defaultStrRef) {
            errorMessage = allocErr.message.empty()
                ? "out of memory allocating string array"
                : allocErr.message;
            return false;
        }
        const auto arrRef = heap->allocateTypedArray(
            arrTypeId,
            semantic::PrimitiveType::String,
            0,
            parts.size(),
            *defaultStrRef,
            &allocErr);
        if (!arrRef) {
            errorMessage = allocErr.message.empty()
                ? "out of memory allocating string array"
                : allocErr.message;
            return false;
        }
        for (std::size_t i = 0; i < parts.size(); ++i) {
            const auto elemStrRef = heap->allocateString(std::move(parts[i]), &allocErr);
            if (!elemStrRef || !heap->arraySet(*arrRef, i, *elemStrRef, &allocErr)) {
                errorMessage = allocErr.message.empty()
                    ? "out of memory populating string array"
                    : allocErr.message;
                return false;
            }
        }
        result = *arrRef;
        return true;
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


bool hasAnyPinnedPoolValues() noexcept {
    return g_anyPoolHasPinnedSlots.load(std::memory_order_acquire);
}

void pinValueInPools(const Value& value) noexcept {
    if (const auto* sv = std::get_if<StringValue>(&value)) {
        if (sv->id != 0u) runtimeStringPool().pinById(sv->id);
        return;
    }
    if (const auto* st = std::get_if<StructValue>(&value)) {
        if (st->isInline) {
            const std::uint32_t idx = st->storage.inlineArenaIndex();
            if (idx != 0u) inlineStructPool().pinById(idx);
        } else if (st->storage.isStoragePtr()) {
            heapStructPool().pinById(st->storage.raw_);
        }
    }
}

void beginValuePoolGcMark() noexcept {
    g_poolGcMarkInProgress.store(true, std::memory_order_release);
    g_anyPoolHasPinnedSlots.store(false, std::memory_order_relaxed);
    constexpr std::uint8_t clearMask =
        static_cast<std::uint8_t>(~(kSlotFlagGcMarked | kSlotFlagStackRoot));
    auto& strPool = runtimeStringPool();
    for (std::uint32_t s = 0u; s < kPoolShardCount; ++s) {
        auto& shard = strPool.shards_[s];
        if (!shard.mayHavePinnedOrMarked.load(std::memory_order_acquire)) continue;
        if (!shard.pages[0].load(std::memory_order_relaxed)) continue;
        std::lock_guard<std::mutex> lock(shard.mutex);
        for (std::uint32_t slot = 0u; slot < shard.highWaterMark; ++slot) {
            if (auto* page = shard.pages[slot >> RuntimeStringPool::kPageShift].load(std::memory_order_relaxed)) {
                page->flags[slot & RuntimeStringPool::kPageMask].fetch_and(
                    clearMask, std::memory_order_relaxed);
            }
        }
    }
    auto& inlPool = inlineStructPool();
    for (std::uint32_t s = 0u; s < kPoolShardCount; ++s) {
        auto& shard = inlPool.shards_[s];
        if (!shard.mayHavePinnedOrMarked.load(std::memory_order_acquire)) continue;
        if (!shard.pages[0].load(std::memory_order_relaxed)) continue;
        std::lock_guard<std::mutex> lock(shard.mutex);
        for (std::uint32_t slot = 0u; slot < shard.highWaterMark; ++slot) {
            if (auto* page = shard.pages[slot >> InlineStructPool::kPageShift].load(std::memory_order_relaxed)) {
                page->flags[slot & InlineStructPool::kPageMask].fetch_and(
                    clearMask, std::memory_order_relaxed);
            }
        }
    }
    auto& hpPool = heapStructPool();
    for (std::uint32_t s = 0u; s < kPoolShardCount; ++s) {
        auto& shard = hpPool.shards_[s];
        if (!shard.mayHavePinnedOrMarked.load(std::memory_order_acquire)) continue;
        if (!shard.pages[0].load(std::memory_order_relaxed)) continue;
        std::lock_guard<std::mutex> lock(shard.mutex);
        for (std::uint32_t slot = 0u; slot < shard.highWaterMark; ++slot) {
            if (auto* page = shard.pages[slot >> HeapStructStoragePool::kPageShift].load(std::memory_order_relaxed)) {
                page->flags[slot & HeapStructStoragePool::kPageMask].fetch_and(
                    clearMask, std::memory_order_relaxed);
            }
        }
    }
}

void markValueInPools(const Value& value) noexcept {
    if (const auto* sv = std::get_if<StringValue>(&value)) {
        if (sv->id != 0u) runtimeStringPool().markById(sv->id);
        return;
    }
    if (const auto* st = std::get_if<StructValue>(&value)) {
        if (st->isInline) {
            const std::uint32_t idx = st->storage.inlineArenaIndex();
            if (idx != 0u) inlineStructPool().markById(idx);
        } else if (st->storage.isStoragePtr()) {
            heapStructPool().markById(st->storage.raw_);
        }
    }
}

void markStackRootValueInPools(const Value& value) noexcept {
    if (const auto* sv = std::get_if<StringValue>(&value)) {
        if (sv->id != 0u) runtimeStringPool().markStackRootById(sv->id);
        return;
    }
    if (const auto* st = std::get_if<StructValue>(&value)) {
        if (st->isInline) {
            const std::uint32_t idx = st->storage.inlineArenaIndex();
            if (idx != 0u) inlineStructPool().markStackRootById(idx);
        } else if (st->storage.isStoragePtr()) {
            heapStructPool().markStackRootById(st->storage.raw_);
        }
    }
}

std::size_t finishValuePoolGcSweep() noexcept {
    std::size_t reclaimed = 0u;
    bool anyPoolPinned = false;
    constexpr std::uint8_t clearMarkBits =
        static_cast<std::uint8_t>(~(kSlotFlagGcMarked | kSlotFlagStackRoot));
    constexpr std::uint8_t clearPinAndMarkBits =
        static_cast<std::uint8_t>(~(kSlotFlagGcMarked | kSlotFlagStackRoot | kSlotFlagPinned));

    auto& strPool = runtimeStringPool();
    for (std::uint32_t s = 0u; s < kPoolShardCount; ++s) {
        auto& shard = strPool.shards_[s];
        if (!shard.mayHavePinnedOrMarked.load(std::memory_order_acquire)) continue;
        if (!shard.pages[0].load(std::memory_order_relaxed)) continue;
        std::lock_guard<std::mutex> lock(shard.mutex);
        shard.mayHavePinnedOrMarked.store(false, std::memory_order_relaxed);
        bool anyReclaimed = false;
        bool shardStillPinned = false;
        for (std::uint32_t slot = 0u; slot < shard.highWaterMark; ++slot) {
            auto* page = shard.pages[slot >> RuntimeStringPool::kPageShift].load(std::memory_order_relaxed);
            if (!page) continue;
            const std::uint32_t offset = slot & RuntimeStringPool::kPageMask;
            const std::uint8_t f = page->flags[offset].load(std::memory_order_relaxed);
            if ((f & kSlotFlagOccupied) == 0u) continue;
            if ((f & kSlotFlagGcMarked) != 0u) {
                page->flags[offset].fetch_and(clearMarkBits, std::memory_order_relaxed);
                shardStillPinned = true;
                continue;
            }
            if ((f & kSlotFlagStackRoot) != 0u) {
                page->flags[offset].fetch_and(clearPinAndMarkBits, std::memory_order_relaxed);
                page->flags[offset].fetch_or(kSlotFlagClock, std::memory_order_relaxed);
                continue;
            }
            if ((f & kSlotFlagPinned) == 0u) {
                continue;
            }
            const std::uint8_t bank = page->activeBank[offset].load(std::memory_order_relaxed) & 1u;
            if (!page->strings[offset][bank].empty()) {
                shard.indexByView.erase(std::string_view(page->strings[offset][bank]));
            }
            page->flags[offset].store(0u, std::memory_order_release);
            shard.freeList.push_back(slot);
            shard.gcReclaimed.fetch_add(1u, std::memory_order_relaxed);
            anyReclaimed = true;
            ++reclaimed;
        }
        if (shardStillPinned) {
            shard.mayHavePinnedOrMarked.store(true, std::memory_order_release);
            anyPoolPinned = true;
        }
        if (anyReclaimed) {
            shard.epoch.fetch_add(1u, std::memory_order_release);
        }
    }

    auto& inlPool = inlineStructPool();
    for (std::uint32_t s = 0u; s < kPoolShardCount; ++s) {
        auto& shard = inlPool.shards_[s];
        if (!shard.mayHavePinnedOrMarked.load(std::memory_order_acquire)) continue;
        if (!shard.pages[0].load(std::memory_order_relaxed)) continue;
        std::lock_guard<std::mutex> lock(shard.mutex);
        shard.mayHavePinnedOrMarked.store(false, std::memory_order_relaxed);
        bool anyReclaimed = false;
        bool shardStillPinned = false;
        for (std::uint32_t slot = 0u; slot < shard.highWaterMark; ++slot) {
            auto* page = shard.pages[slot >> InlineStructPool::kPageShift].load(std::memory_order_relaxed);
            if (!page) continue;
            const std::uint32_t offset = slot & InlineStructPool::kPageMask;
            const std::uint8_t f = page->flags[offset].load(std::memory_order_relaxed);
            if ((f & kSlotFlagOccupied) == 0u) continue;
            if ((f & kSlotFlagGcMarked) != 0u) {
                page->flags[offset].fetch_and(clearMarkBits, std::memory_order_relaxed);
                shardStillPinned = true;
                continue;
            }
            if ((f & kSlotFlagStackRoot) != 0u) {
                page->flags[offset].fetch_and(clearPinAndMarkBits, std::memory_order_relaxed);
                page->flags[offset].fetch_or(kSlotFlagClock, std::memory_order_relaxed);
                continue;
            }
            if ((f & kSlotFlagPinned) == 0u) {
                continue;
            }
            const std::uint8_t bank = page->activeBank[offset].load(std::memory_order_relaxed) & 1u;
            shard.dedupMap.erase(page->entries[offset][bank]);
            page->flags[offset].store(0u, std::memory_order_release);
            shard.freeList.push_back(slot);
            shard.gcReclaimed.fetch_add(1u, std::memory_order_relaxed);
            anyReclaimed = true;
            ++reclaimed;
        }
        if (shardStillPinned) {
            shard.mayHavePinnedOrMarked.store(true, std::memory_order_release);
            anyPoolPinned = true;
        }
        if (anyReclaimed) {
            shard.epoch.fetch_add(1u, std::memory_order_release);
        }
    }

    auto& hpPool = heapStructPool();
    for (std::uint32_t s = 0u; s < kPoolShardCount; ++s) {
        auto& shard = hpPool.shards_[s];
        if (!shard.mayHavePinnedOrMarked.load(std::memory_order_acquire)) continue;
        if (!shard.pages[0].load(std::memory_order_relaxed)) continue;
        std::lock_guard<std::mutex> lock(shard.mutex);
        shard.mayHavePinnedOrMarked.store(false, std::memory_order_relaxed);
        bool anyReclaimed = false;
        bool shardStillPinned = false;
        for (std::uint32_t slot = 0u; slot < shard.highWaterMark; ++slot) {
            auto* page = shard.pages[slot >> HeapStructStoragePool::kPageShift].load(std::memory_order_relaxed);
            if (!page) continue;
            const std::uint32_t offset = slot & HeapStructStoragePool::kPageMask;
            const std::uint8_t f = page->flags[offset].load(std::memory_order_relaxed);
            if ((f & kSlotFlagOccupied) == 0u) continue;
            if ((f & kSlotFlagGcMarked) != 0u) {
                page->flags[offset].fetch_and(clearMarkBits, std::memory_order_relaxed);
                shardStillPinned = true;
                continue;
            }
            if ((f & kSlotFlagStackRoot) != 0u) {
                page->flags[offset].fetch_and(clearPinAndMarkBits, std::memory_order_relaxed);
                page->flags[offset].fetch_or(kSlotFlagClock, std::memory_order_relaxed);
                continue;
            }
            if ((f & kSlotFlagPinned) == 0u) {
                continue;
            }
            hpPool.removeDedupEntryLocked(shard, page->hashes[offset], slot);
            page->hashes[offset] = 0u;
            page->flags[offset].store(0u, std::memory_order_release);
            shard.freeList.push_back(slot);
            shard.gcReclaimed.fetch_add(1u, std::memory_order_relaxed);
            anyReclaimed = true;
            ++reclaimed;
        }
        if (shardStillPinned) {
            shard.mayHavePinnedOrMarked.store(true, std::memory_order_release);
            anyPoolPinned = true;
        }
        if (anyReclaimed) {
            shard.epoch.fetch_add(1u, std::memory_order_release);
        }
    }

    if (anyPoolPinned) {
        g_anyPoolHasPinnedSlots.store(true, std::memory_order_release);
    }
    g_poolGcMarkInProgress.store(false, std::memory_order_release);
    return reclaimed;
}

RuntimeValuePoolStatistics getRuntimeValuePoolStatistics() noexcept {
    RuntimeValuePoolStatistics stats{};

    auto& strPool = runtimeStringPool();
    for (std::uint32_t s = 0u; s < kPoolShardCount; ++s) {
        auto& shard = strPool.shards_[s];
        std::lock_guard<std::mutex> lock(shard.mutex);
        stats.stringActiveSlots += shard.indexByView.size();
        stats.stringFreeListSlots += shard.freeList.size();
        stats.stringAllocatedPages += shard.ownedPages.size();
        stats.stringClockEvictions += shard.clockEvictions.load(std::memory_order_relaxed);
        stats.stringGcReclaimed += shard.gcReclaimed.load(std::memory_order_relaxed);
        for (std::uint32_t slot = 0u; slot < shard.highWaterMark; ++slot) {
            if (auto* page = shard.pages[slot >> RuntimeStringPool::kPageShift].load(std::memory_order_relaxed)) {
                const std::uint8_t f = page->flags[slot & RuntimeStringPool::kPageMask].load(std::memory_order_relaxed);
                if ((f & kSlotFlagOccupied) != 0u && (f & kSlotFlagPinned) != 0u) {
                    ++stats.stringPinnedSlots;
                }
            }
        }
    }

    auto& inlPool = inlineStructPool();
    for (std::uint32_t s = 0u; s < kPoolShardCount; ++s) {
        auto& shard = inlPool.shards_[s];
        std::lock_guard<std::mutex> lock(shard.mutex);
        stats.inlineStructActiveSlots += shard.dedupMap.size();
        stats.inlineStructFreeListSlots += shard.freeList.size();
        stats.inlineStructAllocatedPages += shard.ownedPages.size();
        stats.inlineStructClockEvictions += shard.clockEvictions.load(std::memory_order_relaxed);
        stats.inlineStructGcReclaimed += shard.gcReclaimed.load(std::memory_order_relaxed);
        for (std::uint32_t slot = 0u; slot < shard.highWaterMark; ++slot) {
            if (auto* page = shard.pages[slot >> InlineStructPool::kPageShift].load(std::memory_order_relaxed)) {
                const std::uint8_t f = page->flags[slot & InlineStructPool::kPageMask].load(std::memory_order_relaxed);
                if ((f & kSlotFlagOccupied) != 0u && (f & kSlotFlagPinned) != 0u) {
                    ++stats.inlineStructPinnedSlots;
                }
            }
        }
    }

    auto& hpPool = heapStructPool();
    for (std::uint32_t s = 0u; s < kPoolShardCount; ++s) {
        auto& shard = hpPool.shards_[s];
        std::lock_guard<std::mutex> lock(shard.mutex);
        stats.heapStructActiveSlots += shard.dedupByHash.size();
        stats.heapStructFreeListSlots += shard.freeList.size();
        stats.heapStructAllocatedPages += shard.ownedPages.size();
        stats.heapStructClockEvictions += shard.clockEvictions.load(std::memory_order_relaxed);
        stats.heapStructGcReclaimed += shard.gcReclaimed.load(std::memory_order_relaxed);
        for (std::uint32_t slot = 0u; slot < shard.highWaterMark; ++slot) {
            if (auto* page = shard.pages[slot >> HeapStructStoragePool::kPageShift].load(std::memory_order_relaxed)) {
                const std::uint8_t f = page->flags[slot & HeapStructStoragePool::kPageMask].load(std::memory_order_relaxed);
                if ((f & kSlotFlagOccupied) != 0u && (f & kSlotFlagPinned) != 0u) {
                    ++stats.heapStructPinnedSlots;
                }
            }
        }
    }

    return stats;
}

} // namespace realscript::runtime
