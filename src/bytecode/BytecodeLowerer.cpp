#include "realscript/bytecode/Bytecode.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_map>

namespace realscript::bytecode {
namespace {

bool definesValue(const mir::Instruction& instruction) noexcept {
    if (instruction.opcode == mir::Opcode::StoreLocal ||
        instruction.opcode == mir::Opcode::StoreField ||
        instruction.opcode == mir::Opcode::StoreElement) {
        return false;
    }
    return instruction.opcode != mir::Opcode::Call ||
        instruction.resultType != semantic::PrimitiveType::Void;
}

Opcode lowerOpcode(mir::Opcode opcode) {
    switch (opcode) {
    case mir::Opcode::Parameter: return Opcode::LoadParameter;
    case mir::Opcode::ConstantInt: return Opcode::ConstantInt;
    case mir::Opcode::ConstantDouble: return Opcode::ConstantDouble;
    case mir::Opcode::ConstantBool: return Opcode::ConstantBool;
    case mir::Opcode::ConstantString: return Opcode::ConstantString;
    case mir::Opcode::ConstantNull: return Opcode::ConstantNull;
    case mir::Opcode::LoadLocal: return Opcode::LoadLocal;
    case mir::Opcode::StoreLocal: return Opcode::StoreLocal;
    case mir::Opcode::ConvertNullToString: return Opcode::ConvertNullToString;
    case mir::Opcode::ConvertNullToObject: return Opcode::ConvertNullToObject;
    case mir::Opcode::ConvertNullToArray: return Opcode::ConvertNullToArray;
    case mir::Opcode::ConvertIntToLong: return Opcode::ConvertIntToLong;
    case mir::Opcode::ConvertIntToDouble: return Opcode::ConvertIntToDouble;
    case mir::Opcode::ConvertLongToDouble: return Opcode::ConvertLongToDouble;
    case mir::Opcode::NewObject: return Opcode::NewObject;
    case mir::Opcode::NewStruct: return Opcode::NewStruct;
    case mir::Opcode::NewArray: return Opcode::NewArray;
    case mir::Opcode::CheckNotNull: return Opcode::CheckNotNull;
    case mir::Opcode::ArrayLength: return Opcode::ArrayLength;
    case mir::Opcode::LoadElement: return Opcode::LoadElement;
    case mir::Opcode::StoreElement: return Opcode::StoreElement;
    case mir::Opcode::LoadField: return Opcode::LoadField;
    case mir::Opcode::StoreField: return Opcode::StoreField;
    case mir::Opcode::LoadStructField: return Opcode::LoadStructField;
    case mir::Opcode::StoreStructField: return Opcode::StoreStructField;
    case mir::Opcode::Call: return Opcode::Call;
    case mir::Opcode::NegateInt: return Opcode::NegateInt;
    case mir::Opcode::NegateLong: return Opcode::NegateLong;
    case mir::Opcode::NegateDouble: return Opcode::NegateDouble;
    case mir::Opcode::LogicalNot: return Opcode::LogicalNot;
    case mir::Opcode::AddInt: return Opcode::AddInt;
    case mir::Opcode::SubtractInt: return Opcode::SubtractInt;
    case mir::Opcode::MultiplyInt: return Opcode::MultiplyInt;
    case mir::Opcode::DivideInt: return Opcode::DivideInt;
    case mir::Opcode::RemainderInt: return Opcode::RemainderInt;
    case mir::Opcode::Equal: return Opcode::Equal;
    case mir::Opcode::NotEqual: return Opcode::NotEqual;
    case mir::Opcode::LessInt: return Opcode::LessInt;
    case mir::Opcode::LessOrEqualInt: return Opcode::LessOrEqualInt;
    case mir::Opcode::GreaterInt: return Opcode::GreaterInt;
    case mir::Opcode::GreaterOrEqualInt: return Opcode::GreaterOrEqualInt;
    case mir::Opcode::AddLong: return Opcode::AddLong;
    case mir::Opcode::SubtractLong: return Opcode::SubtractLong;
    case mir::Opcode::MultiplyLong: return Opcode::MultiplyLong;
    case mir::Opcode::DivideLong: return Opcode::DivideLong;
    case mir::Opcode::RemainderLong: return Opcode::RemainderLong;
    case mir::Opcode::LessLong: return Opcode::LessLong;
    case mir::Opcode::LessOrEqualLong: return Opcode::LessOrEqualLong;
    case mir::Opcode::GreaterLong: return Opcode::GreaterLong;
    case mir::Opcode::GreaterOrEqualLong: return Opcode::GreaterOrEqualLong;
    case mir::Opcode::AddDouble: return Opcode::AddDouble;
    case mir::Opcode::SubtractDouble: return Opcode::SubtractDouble;
    case mir::Opcode::MultiplyDouble: return Opcode::MultiplyDouble;
    case mir::Opcode::DivideDouble: return Opcode::DivideDouble;
    case mir::Opcode::LessDouble: return Opcode::LessDouble;
    case mir::Opcode::LessOrEqualDouble: return Opcode::LessOrEqualDouble;
    case mir::Opcode::GreaterDouble: return Opcode::GreaterDouble;
    case mir::Opcode::GreaterOrEqualDouble: return Opcode::GreaterOrEqualDouble;
    }
    throw std::logic_error("unsupported MIR opcode in bytecode lowerer");
}

TerminatorKind lowerTerminator(mir::TerminatorKind kind) {
    switch (kind) {
    case mir::TerminatorKind::None: return TerminatorKind::None;
    case mir::TerminatorKind::Jump: return TerminatorKind::Jump;
    case mir::TerminatorKind::Branch: return TerminatorKind::Branch;
    case mir::TerminatorKind::ReturnValue: return TerminatorKind::ReturnValue;
    case mir::TerminatorKind::ReturnVoid: return TerminatorKind::ReturnVoid;
    }
    throw std::logic_error("unsupported MIR terminator in bytecode lowerer");
}

std::string referenceKey(const mir::Instruction& instruction) {
    std::string result = std::to_string(instruction.symbolId) + ":" +
        instruction.symbolName + "(";
    for (std::size_t index = 0; index < instruction.parameterTypes.size(); ++index) {
        result += semantic::primitiveTypeName(instruction.parameterTypes[index]);
        result.push_back('#');
        result += std::to_string(
            index < instruction.parameterTypeIds.size()
                ? instruction.parameterTypeIds[index]
                : 0);
        result.push_back(',');
    }
    result += ")->";
    result += semantic::primitiveTypeName(instruction.resultType);
    result.push_back('#');
    result += std::to_string(instruction.resultTypeId);
    return result;
}

} // namespace

Module Lowerer::lower(const mir::Module& source) const {
    Module result;
    result.name = source.name;
    result.languageMetadata = source.languageMetadata;
    result.sourceFiles = source.sourceFiles;
    result.types = source.types;

    std::unordered_map<std::string, std::uint32_t> referenceIndices;
    std::unordered_map<semantic::SymbolId, std::uint32_t> typeIndices;
    for (std::size_t index = 0; index < result.types.size(); ++index) {
        typeIndices.emplace(
            result.types[index].id,
            static_cast<std::uint32_t>(index));
    }

    for (const auto& sourceFunction : source.functions) {
        Function function;
        function.symbolId = sourceFunction.symbolId;
        function.name = sourceFunction.name;
        function.returnType = sourceFunction.returnType;
        function.returnTypeId = sourceFunction.returnTypeId;
        function.parameterTypes = sourceFunction.parameterTypes;
        function.parameterTypeIds = sourceFunction.parameterTypeIds;
        function.localTypes = sourceFunction.localTypes;
        function.localTypeIds = sourceFunction.localTypeIds;
        function.debugInfo = sourceFunction.debugInfo;

        mir::ValueId maximumValue = -1;
        for (const auto& sourceBlock : sourceFunction.blocks) {
            for (const auto& parameter : sourceBlock.parameters) {
                maximumValue = std::max(maximumValue, parameter.value);
            }
            for (const auto& instruction : sourceBlock.instructions) {
                if (definesValue(instruction)) {
                    maximumValue = std::max(maximumValue, instruction.result);
                }
            }
        }
        function.registerTypes.assign(
            maximumValue < 0 ? 0 : static_cast<std::size_t>(maximumValue + 1),
            semantic::PrimitiveType::Error);
        function.registerTypeIds.assign(function.registerTypes.size(), 0);

        for (const auto& sourceBlock : sourceFunction.blocks) {
            BasicBlock block;
            block.id = sourceBlock.id;
            for (const auto& parameter : sourceBlock.parameters) {
                const auto target = static_cast<Register>(parameter.value);
                block.parameters.push_back({
                    target,
                    parameter.type,
                    parameter.typeId,
                });
                function.registerTypes.at(target) = parameter.type;
                function.registerTypeIds.at(target) = parameter.typeId;
            }

            for (const auto& sourceInstruction : sourceBlock.instructions) {
                Instruction instruction;
                instruction.opcode = lowerOpcode(sourceInstruction.opcode);
                instruction.result = definesValue(sourceInstruction)
                    ? static_cast<Register>(sourceInstruction.result)
                    : InvalidRegister;
                for (const auto operand : sourceInstruction.operands) {
                    instruction.operands.push_back(static_cast<Register>(operand));
                }
                instruction.index = static_cast<std::uint32_t>(
                    sourceInstruction.opcode == mir::Opcode::Parameter
                        ? sourceInstruction.integerImmediate
                        : sourceInstruction.localIndex);
                if (sourceInstruction.opcode == mir::Opcode::LoadField ||
                    sourceInstruction.opcode == mir::Opcode::StoreField ||
                    sourceInstruction.opcode == mir::Opcode::NewStruct ||
                    sourceInstruction.opcode == mir::Opcode::LoadStructField ||
                    sourceInstruction.opcode == mir::Opcode::StoreStructField) {
                    instruction.index = static_cast<std::uint32_t>(
                        sourceInstruction.fieldIndex);
                }
                if (sourceInstruction.opcode == mir::Opcode::NewObject ||
                    sourceInstruction.opcode == mir::Opcode::CheckNotNull ||
                    sourceInstruction.opcode == mir::Opcode::LoadField ||
                    sourceInstruction.opcode == mir::Opcode::StoreField ||
                    sourceInstruction.opcode == mir::Opcode::NewStruct ||
                    sourceInstruction.opcode == mir::Opcode::LoadStructField ||
                    sourceInstruction.opcode == mir::Opcode::StoreStructField) {
                    const auto foundType = typeIndices.find(sourceInstruction.typeId);
                    if (foundType == typeIndices.end()) {
                        throw std::logic_error(
                            "MIR object instruction references missing type descriptor");
                    }
                    instruction.typeIndex = foundType->second;
                }
                instruction.elementType = sourceInstruction.elementType;
                instruction.elementTypeId = sourceInstruction.elementTypeId;
                instruction.integerImmediate = sourceInstruction.integerImmediate;
                instruction.doubleImmediate = sourceInstruction.doubleImmediate;
                instruction.boolImmediate = sourceInstruction.boolImmediate;
                instruction.stringImmediate = sourceInstruction.stringImmediate;

                if (sourceInstruction.opcode == mir::Opcode::Call) {
                    const auto key = referenceKey(sourceInstruction);
                    const auto found = referenceIndices.find(key);
                    if (found != referenceIndices.end()) {
                        instruction.index = found->second;
                    } else {
                        const auto index = static_cast<std::uint32_t>(
                            result.functionReferences.size());
                        FunctionReference reference;
                        reference.symbolId = sourceInstruction.symbolId;
                        reference.name = sourceInstruction.symbolName;
                        reference.returnType = sourceInstruction.resultType;
                        reference.returnTypeId = sourceInstruction.resultTypeId;
                        reference.parameterTypes = sourceInstruction.parameterTypes;
                        reference.parameterTypeIds =
                            sourceInstruction.parameterTypeIds;
                        result.functionReferences.push_back(std::move(reference));
                        referenceIndices.emplace(key, index);
                        instruction.index = index;
                    }
                }

                if (instruction.result != InvalidRegister) {
                    function.registerTypes.at(instruction.result) =
                        sourceInstruction.resultType;
                    function.registerTypeIds.at(instruction.result) =
                        sourceInstruction.resultTypeId;
                }
                block.instructions.push_back(std::move(instruction));
            }

            block.terminator.kind = lowerTerminator(sourceBlock.terminator.kind);
            block.terminator.condition = sourceBlock.terminator.condition < 0
                ? InvalidRegister
                : static_cast<Register>(sourceBlock.terminator.condition);
            block.terminator.value = sourceBlock.terminator.value < 0
                ? InvalidRegister
                : static_cast<Register>(sourceBlock.terminator.value);
            block.terminator.target = sourceBlock.terminator.target;
            block.terminator.falseTarget = sourceBlock.terminator.falseTarget;
            for (const auto argument : sourceBlock.terminator.arguments) {
                block.terminator.arguments.push_back(
                    static_cast<Register>(argument));
            }
            for (const auto argument : sourceBlock.terminator.falseArguments) {
                block.terminator.falseArguments.push_back(
                    static_cast<Register>(argument));
            }
            function.blocks.push_back(std::move(block));
        }
        result.functions.push_back(std::move(function));
    }
    return result;
}

} // namespace realscript::bytecode
