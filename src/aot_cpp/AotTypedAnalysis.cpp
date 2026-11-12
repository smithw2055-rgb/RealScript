#include "AotCppInternal.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace realscript::aot::detail {

std::string hexId(semantic::SymbolId id) {
    std::ostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill('0') << id;
    return stream.str();
}

std::string primitiveExpression(semantic::PrimitiveType type) {
    return "semantic::PrimitiveType::" + std::string([&]() {
        switch (type) {
        case semantic::PrimitiveType::Error: return "Error";
        case semantic::PrimitiveType::Void: return "Void";
        case semantic::PrimitiveType::Bool: return "Bool";
        case semantic::PrimitiveType::Byte: return "Byte";
        case semantic::PrimitiveType::SByte: return "SByte";
        case semantic::PrimitiveType::Short: return "Short";
        case semantic::PrimitiveType::UShort: return "UShort";
        case semantic::PrimitiveType::Int: return "Int";
        case semantic::PrimitiveType::UInt: return "UInt";
        case semantic::PrimitiveType::Long: return "Long";
        case semantic::PrimitiveType::ULong: return "ULong";
        case semantic::PrimitiveType::Float: return "Float";
        case semantic::PrimitiveType::Double: return "Double";
        case semantic::PrimitiveType::Char: return "Char";
        case semantic::PrimitiveType::String: return "String";
        case semantic::PrimitiveType::Object: return "Object";
        case semantic::PrimitiveType::Struct: return "Struct";
        case semantic::PrimitiveType::Enum: return "Enum";
        case semantic::PrimitiveType::Array: return "Array";
        case semantic::PrimitiveType::Handle: return "Handle";
        case semantic::PrimitiveType::Null: return "Null";
        }
        return "Error";
    }());
}

std::string typeKindExpression(semantic::TypeKind kind) {
    switch (kind) {
    case semantic::TypeKind::Class: return "semantic::TypeKind::Class";
    case semantic::TypeKind::Struct: return "semantic::TypeKind::Struct";
    case semantic::TypeKind::Enum: return "semantic::TypeKind::Enum";
    }
    return "semantic::TypeKind::Class";
}

semantic::SymbolId exactTypeId(
    semantic::PrimitiveType type,
    const std::string& name) {
    return semantic::isExactType(type) && !name.empty()
        ? semantic::stableTypeId(name)
        : 0;
}

std::string conversionExpression(mir::Opcode opcode) {
    switch (opcode) {
    case mir::Opcode::ConvertNullToString:
        return "semantic::ConversionKind::NullToString";
    case mir::Opcode::ConvertNullToObject:
        return "semantic::ConversionKind::NullToObject";
    case mir::Opcode::ConvertNullToArray:
        return "semantic::ConversionKind::NullToArray";
    case mir::Opcode::ConvertIntToLong:
        return "semantic::ConversionKind::IntToLong";
    case mir::Opcode::ConvertIntToDouble:
        return "semantic::ConversionKind::IntToDouble";
    case mir::Opcode::ConvertLongToDouble:
        return "semantic::ConversionKind::LongToDouble";
    case mir::Opcode::ConvertNumeric:
        return "semantic::ConversionKind::Numeric";
    default:
        return "semantic::ConversionKind::None";
    }
}

std::string unaryExpression(mir::Opcode opcode) {
    switch (opcode) {
    case mir::Opcode::NegateInt: return "UnaryOperation::NegateInt";
    case mir::Opcode::NegateLong: return "UnaryOperation::NegateLong";
    case mir::Opcode::NegateDouble: return "UnaryOperation::NegateDouble";
    case mir::Opcode::LogicalNot: return "UnaryOperation::LogicalNot";
    default: return "UnaryOperation::LogicalNot";
    }
}

std::string binaryExpression(mir::Opcode opcode) {
    switch (opcode) {
    case mir::Opcode::Equal: return "BinaryOperation::Equal";
    case mir::Opcode::NotEqual: return "BinaryOperation::NotEqual";
    case mir::Opcode::AddInt: return "BinaryOperation::AddInt";
    case mir::Opcode::SubtractInt: return "BinaryOperation::SubtractInt";
    case mir::Opcode::MultiplyInt: return "BinaryOperation::MultiplyInt";
    case mir::Opcode::DivideInt: return "BinaryOperation::DivideInt";
    case mir::Opcode::RemainderInt: return "BinaryOperation::RemainderInt";
    case mir::Opcode::LessInt: return "BinaryOperation::LessInt";
    case mir::Opcode::LessOrEqualInt: return "BinaryOperation::LessOrEqualInt";
    case mir::Opcode::GreaterInt: return "BinaryOperation::GreaterInt";
    case mir::Opcode::GreaterOrEqualInt: return "BinaryOperation::GreaterOrEqualInt";
    case mir::Opcode::AddLong: return "BinaryOperation::AddLong";
    case mir::Opcode::SubtractLong: return "BinaryOperation::SubtractLong";
    case mir::Opcode::MultiplyLong: return "BinaryOperation::MultiplyLong";
    case mir::Opcode::DivideLong: return "BinaryOperation::DivideLong";
    case mir::Opcode::RemainderLong: return "BinaryOperation::RemainderLong";
    case mir::Opcode::LessLong: return "BinaryOperation::LessLong";
    case mir::Opcode::LessOrEqualLong: return "BinaryOperation::LessOrEqualLong";
    case mir::Opcode::GreaterLong: return "BinaryOperation::GreaterLong";
    case mir::Opcode::GreaterOrEqualLong: return "BinaryOperation::GreaterOrEqualLong";
    case mir::Opcode::AddDouble: return "BinaryOperation::AddDouble";
    case mir::Opcode::SubtractDouble: return "BinaryOperation::SubtractDouble";
    case mir::Opcode::MultiplyDouble: return "BinaryOperation::MultiplyDouble";
    case mir::Opcode::DivideDouble: return "BinaryOperation::DivideDouble";
    case mir::Opcode::LessDouble: return "BinaryOperation::LessDouble";
    case mir::Opcode::LessOrEqualDouble: return "BinaryOperation::LessOrEqualDouble";
    case mir::Opcode::GreaterDouble: return "BinaryOperation::GreaterDouble";
    case mir::Opcode::GreaterOrEqualDouble:
        return "BinaryOperation::GreaterOrEqualDouble";
    default: return "BinaryOperation::Equal";
    }
}

bool isConversion(mir::Opcode opcode) {
    return (opcode >= mir::Opcode::ConvertNullToString &&
            opcode <= mir::Opcode::ConvertLongToDouble) ||
        opcode == mir::Opcode::ConvertNumeric;
}

bool isUnary(mir::Opcode opcode) {
    return opcode >= mir::Opcode::NegateInt &&
        opcode <= mir::Opcode::LogicalNot;
}

bool isBinary(mir::Opcode opcode) {
    return (opcode >= mir::Opcode::AddInt &&
        opcode <= mir::Opcode::GreaterOrEqualDouble) ||
        opcode == mir::Opcode::Equal || opcode == mir::Opcode::NotEqual;
}

std::size_t registerCount(const mir::Function& function) {
    mir::ValueId maximum = -1;
    const auto observe = [&](mir::ValueId value) {
        if (value > maximum) maximum = value;
    };
    for (const auto& block : function.blocks) {
        for (const auto& parameter : block.parameters) observe(parameter.value);
        for (const auto& instruction : block.instructions) {
            observe(instruction.result);
            for (const auto operand : instruction.operands) observe(operand);
        }
        observe(block.terminator.condition);
        observe(block.terminator.value);
        for (const auto value : block.terminator.arguments) observe(value);
        for (const auto value : block.terminator.falseArguments) observe(value);
    }
    return maximum < 0 ? 0 : static_cast<std::size_t>(maximum) + 1;
}

const mir::BasicBlock* findBlock(
    const mir::Function& function,
    mir::BlockId id) {
    for (const auto& block : function.blocks) {
        if (block.id == id) return &block;
    }
    return nullptr;
}

const debug::SequencePoint* findPoint(
    const mir::Function& function,
    mir::BlockId blockId,
    std::uint32_t instructionIndex,
    bool terminator) {
    for (const auto& point : function.debugInfo.sequencePoints) {
        if (point.blockId == blockId &&
            point.instructionIndex == instructionIndex &&
            point.terminator == terminator) {
            return &point;
        }
    }
    return nullptr;
}

const debug::SourceFileInfo* findSource(
    const mir::Module& module,
    debug::SourceFileId id) {
    for (const auto& source : module.sourceFiles) {
        if (source.id == id) return &source;
    }
    return nullptr;
}

const debug::SourceFileInfo* findFunctionSource(
    const mir::Module& module,
    const mir::Function& function) {
    if (const auto* byId = findSource(module, function.debugInfo.sourceFileId)) {
        return byId;
    }
    if (!function.debugInfo.sourceName.empty()) {
        for (const auto& source : module.sourceFiles) {
            if (source.path == function.debugInfo.sourceName) return &source;
        }
    }
    return module.sourceFiles.empty() ? nullptr : &module.sourceFiles.front();
}

std::optional<debug::SequencePoint> resolveSequencePoint(
    const mir::Module& module,
    const mir::Function& function,
    mir::BlockId blockId,
    std::uint32_t instructionIndex,
    bool terminator,
    text::TextSpan fallbackSpan,
    bool* outExplicitSequencePoint) {
    if (const auto* point = findPoint(function, blockId, instructionIndex, terminator)) {
        if (outExplicitSequencePoint) *outExplicitSequencePoint = true;
        return *point;
    }
    if (outExplicitSequencePoint) *outExplicitSequencePoint = false;
    if (fallbackSpan.empty()) return std::nullopt;
    const auto* source = findFunctionSource(module, function);
    if (!source || source->path.empty()) return std::nullopt;
    debug::SequencePoint synthesized;
    synthesized.blockId = blockId;
    synthesized.instructionIndex = instructionIndex;
    synthesized.terminator = terminator;
    synthesized.range = debug::makeSourceRange(*source, fallbackSpan);
    return synthesized;
}

bool emitLineDirective(
    Emitter& output,
    const mir::Module& module,
    const std::optional<debug::SequencePoint>& point,
    bool enabled,
    std::vector<SourceMapRecord>* sourceMap,
    semantic::SymbolId symbolId,
    bool recordInMap) {
    if (!point) return false;
    const auto* source = findSource(module, point->range.fileId);
    if (!source || source->path.empty()) return false;
    const auto line = static_cast<std::uint32_t>(point->range.start.line + 1);
    if (enabled) {
        output.line("#line " + std::to_string(line) + " \"" +
            escapeCppString(source->path) + "\"");
    }
    if (sourceMap && recordInMap) {
        SourceMapRecord entry;
        entry.symbolId = symbolId;
        entry.generatedLine = output.nextLine();
        entry.sourcePath = source->path;
        entry.sourceLine = line;
        entry.sourceColumn = static_cast<std::uint32_t>(point->range.start.column + 1);
        sourceMap->push_back(std::move(entry));
    }
    return enabled;
}

void emitGeneratedLineReset(
    Emitter& output,
    bool enabled) {
    if (!enabled) return;
    output.line("#line " + std::to_string(output.nextLine() + 1u) +
        " \"realscript_aot_generated.cpp\"");
}

std::string valueExpression(mir::ValueId id) {
    return "registers[" + std::to_string(id) + "]";
}

InlineMathKind classifyInlineMath(
    semantic::PrimitiveType type,
    semantic::SymbolId typeId) noexcept {
    if (type != semantic::PrimitiveType::Struct || typeId == 0) {
        return InlineMathKind::None;
    }
    if (typeId == runtime::fixedTypeId()) return InlineMathKind::Fixed;
    if (typeId == runtime::fixed16TypeId()) return InlineMathKind::Fixed16;
    if (typeId == runtime::fixed64TypeId()) return InlineMathKind::Fixed64;
    if (typeId == runtime::vec2TypeId()) return InlineMathKind::Vec2;
    if (typeId == runtime::vec3TypeId()) return InlineMathKind::Vec3;
    if (typeId == runtime::quatTypeId()) return InlineMathKind::Quat;
    if (typeId == runtime::vec2fTypeId()) return InlineMathKind::Vec2f;
    if (typeId == runtime::vec3fTypeId()) return InlineMathKind::Vec3f;
    if (typeId == runtime::quatfTypeId()) return InlineMathKind::Quatf;
    return InlineMathKind::None;
}

const char* inlineMathFieldMember(
    InlineMathKind kind,
    std::size_t fieldIndex) noexcept {
    switch (kind) {
    case InlineMathKind::Fixed:
    case InlineMathKind::Fixed16:
    case InlineMathKind::Fixed64:
        return fieldIndex == 0 ? "raw" : nullptr;
    case InlineMathKind::Vec2:
    case InlineMathKind::Vec2f:
        return fieldIndex == 0 ? "x" : (fieldIndex == 1 ? "y" : nullptr);
    case InlineMathKind::Vec3:
    case InlineMathKind::Vec3f:
        return fieldIndex == 0 ? "x"
            : (fieldIndex == 1 ? "y" : (fieldIndex == 2 ? "z" : nullptr));
    case InlineMathKind::Quat:
    case InlineMathKind::Quatf:
        return fieldIndex == 0 ? "x"
            : (fieldIndex == 1 ? "y"
                : (fieldIndex == 2 ? "z" : (fieldIndex == 3 ? "w" : nullptr)));
    case InlineMathKind::None:
        return nullptr;
    }
    return nullptr;
}

bool isTypedAotSlot(
    TypedSlotType slot,
    const std::unordered_set<semantic::SymbolId>* customStructs) noexcept {
    switch (slot.primitive) {
    case semantic::PrimitiveType::Bool:
    case semantic::PrimitiveType::Int:
    case semantic::PrimitiveType::Long:
    case semantic::PrimitiveType::Float:
    case semantic::PrimitiveType::Double:
    case semantic::PrimitiveType::Null:
        return slot.typeId == 0;
    case semantic::PrimitiveType::Enum:
    case semantic::PrimitiveType::Object:
        return slot.typeId != 0;
    case semantic::PrimitiveType::Array:
        return true;
    case semantic::PrimitiveType::Struct:
        if (classifyInlineMath(slot.primitive, slot.typeId) !=
            InlineMathKind::None) {
            return true;
        }
        return customStructs != nullptr &&
            slot.typeId != 0 &&
            customStructs->find(slot.typeId) != customStructs->end();
    default:
        return false;
    }
}

std::vector<TypedSlotType> valueSlotTypes(const mir::Function& function) {
    std::vector<TypedSlotType> result(registerCount(function));
    for (const auto& block : function.blocks) {
        for (const auto& parameter : block.parameters) {
            if (parameter.value >= 0) {
                result[static_cast<std::size_t>(parameter.value)] = {
                    parameter.type,
                    parameter.typeId,
                };
            }
        }
        for (const auto& instruction : block.instructions) {
            if (instruction.result >= 0) {
                const auto typeId = instruction.resultTypeId != 0
                    ? instruction.resultTypeId
                    : (semantic::isExactType(instruction.resultType)
                        ? instruction.typeId
                        : 0);
                result[static_cast<std::size_t>(instruction.result)] = {
                    instruction.resultType,
                    typeId,
                };
            }
        }
    }
    return result;
}

bool supportsTypedPrimitiveAot(
    const mir::Function& function,
    const std::unordered_set<semantic::SymbolId>& customStructs) {
    if (!function.exceptionHandlers.empty()) {
        return false;
    }
    const bool isAsyncFunction = !function.blocks.empty() &&
        !function.blocks.front().instructions.empty() &&
        function.blocks.front().instructions.front().opcode == mir::Opcode::AsyncEnter;
    if (isAsyncFunction) {
        if (function.returnType != semantic::PrimitiveType::Struct ||
            customStructs.find(function.returnTypeId) == customStructs.end()) {
            return false;
        }
    }
    if (function.returnType != semantic::PrimitiveType::Void &&
        !isTypedAotSlot({function.returnType, function.returnTypeId}, &customStructs)) {
        return false;
    }
    bool usesReferenceSlots =
        function.returnType == semantic::PrimitiveType::Object ||
        function.returnType == semantic::PrimitiveType::Array;
    for (std::size_t index = 0; index < function.parameterTypes.size(); ++index) {
        const auto typeId = index < function.parameterTypeIds.size()
            ? function.parameterTypeIds[index]
            : 0;
        if (!isTypedAotSlot({function.parameterTypes[index], typeId}, &customStructs)) {
            return false;
        }
        if (function.parameterTypes[index] == semantic::PrimitiveType::Object ||
            function.parameterTypes[index] == semantic::PrimitiveType::Array) {
            usesReferenceSlots = true;
        }
    }
    for (std::size_t index = 0; index < function.localTypes.size(); ++index) {
        const auto typeId = index < function.localTypeIds.size()
            ? function.localTypeIds[index]
            : 0;
        if (!isTypedAotSlot({function.localTypes[index], typeId}, &customStructs)) {
            return false;
        }
        if (function.localTypes[index] == semantic::PrimitiveType::Object ||
            function.localTypes[index] == semantic::PrimitiveType::Array) {
            usesReferenceSlots = true;
        }
    }
    const auto types = valueSlotTypes(function);
    for (const auto& slot : types) {
        if (slot.primitive != semantic::PrimitiveType::Error &&
            slot.primitive != semantic::PrimitiveType::Void &&
            !isTypedAotSlot(slot, &customStructs)) {
            return false;
        }
        if (slot.primitive == semantic::PrimitiveType::Object ||
            slot.primitive == semantic::PrimitiveType::Array) {
            usesReferenceSlots = true;
        }
    }
    const auto operandSlot = [&](const mir::Instruction& instruction,
                                 std::size_t index) -> TypedSlotType {
        if (index >= instruction.operands.size() ||
            instruction.operands[index] < 0) {
            return {};
        }
        return types[static_cast<std::size_t>(instruction.operands[index])];
    };
    for (const auto& block : function.blocks) {
        for (const auto& instruction : block.instructions) {
            switch (instruction.opcode) {
            case mir::Opcode::Parameter:
            case mir::Opcode::ConstantInt:
            case mir::Opcode::ConstantDouble:
            case mir::Opcode::ConstantBool:
            case mir::Opcode::ConstantNull:
            case mir::Opcode::ConvertNullToObject:
            case mir::Opcode::ConvertNullToArray:
            case mir::Opcode::LoadLocal:
            case mir::Opcode::StoreLocal:
            case mir::Opcode::ConvertIntToLong:
            case mir::Opcode::ConvertIntToDouble:
            case mir::Opcode::ConvertLongToDouble:
            case mir::Opcode::ConvertNumeric:
            case mir::Opcode::LessInt:
            case mir::Opcode::LessOrEqualInt:
            case mir::Opcode::GreaterInt:
            case mir::Opcode::GreaterOrEqualInt:
            case mir::Opcode::AddInt:
            case mir::Opcode::SubtractInt:
            case mir::Opcode::MultiplyInt:
            case mir::Opcode::DivideInt:
            case mir::Opcode::RemainderInt:
            case mir::Opcode::NegateInt:
            case mir::Opcode::LessLong:
            case mir::Opcode::LessOrEqualLong:
            case mir::Opcode::GreaterLong:
            case mir::Opcode::GreaterOrEqualLong:
            case mir::Opcode::AddLong:
            case mir::Opcode::SubtractLong:
            case mir::Opcode::MultiplyLong:
            case mir::Opcode::DivideLong:
            case mir::Opcode::RemainderLong:
            case mir::Opcode::NegateLong:
            case mir::Opcode::LessDouble:
            case mir::Opcode::LessOrEqualDouble:
            case mir::Opcode::GreaterDouble:
            case mir::Opcode::GreaterOrEqualDouble:
            case mir::Opcode::AddDouble:
            case mir::Opcode::SubtractDouble:
            case mir::Opcode::MultiplyDouble:
            case mir::Opcode::DivideDouble:
            case mir::Opcode::NegateDouble:
            case mir::Opcode::CheckNotNull:
            case mir::Opcode::ArrayLength:
                break;
            case mir::Opcode::NewObject: {
                const auto objTid = instruction.resultTypeId != 0
                    ? instruction.resultTypeId
                    : instruction.typeId;
                if (instruction.resultType != semantic::PrimitiveType::Object ||
                    objTid == 0 ||
                    !isTypedAotSlot({semantic::PrimitiveType::Object, objTid}, &customStructs)) {
                    return false;
                }
                break;
            }
            case mir::Opcode::IsType:
            case mir::Opcode::AsType: {
                if (instruction.operands.size() != 1 ||
                    instruction.elementType != semantic::PrimitiveType::Object ||
                    instruction.elementTypeId == 0 ||
                    operandSlot(instruction, 0).primitive != semantic::PrimitiveType::Object) {
                    return false;
                }
                if (instruction.opcode == mir::Opcode::IsType &&
                    instruction.resultType != semantic::PrimitiveType::Bool) {
                    return false;
                }
                if (instruction.opcode == mir::Opcode::AsType &&
                    instruction.resultType != semantic::PrimitiveType::Object) {
                    return false;
                }
                break;
            }
            case mir::Opcode::LoadField:
                if (!isTypedAotSlot({instruction.resultType, instruction.resultTypeId}, &customStructs)) {
                    return false;
                }
                break;
            case mir::Opcode::StoreField:
                if (instruction.operands.size() < 2 ||
                    !isTypedAotSlot(operandSlot(instruction, 1), &customStructs)) {
                    return false;
                }
                break;
            case mir::Opcode::NewArray:
                if (instruction.operands.size() != 1 ||
                    operandSlot(instruction, 0).primitive != semantic::PrimitiveType::Int ||
                    !isTypedAotSlot({instruction.elementType, instruction.elementTypeId}, &customStructs)) {
                    return false;
                }
                break;
            case mir::Opcode::LoadElement:
                if (operandSlot(instruction, 1).primitive != semantic::PrimitiveType::Int ||
                    !isTypedAotSlot({instruction.elementType, instruction.elementTypeId}, &customStructs)) {
                    return false;
                }
                break;
            case mir::Opcode::StoreElement:
                if (operandSlot(instruction, 1).primitive != semantic::PrimitiveType::Int ||
                    !isTypedAotSlot({instruction.elementType, instruction.elementTypeId}, &customStructs) ||
                    !isTypedAotSlot(operandSlot(instruction, 2), &customStructs)) {
                    return false;
                }
                break;
            case mir::Opcode::LogicalNot:
                if (operandSlot(instruction, 0).primitive !=
                    semantic::PrimitiveType::Bool) return false;
                break;
            case mir::Opcode::Equal:
            case mir::Opcode::NotEqual:
                if (operandSlot(instruction, 0) !=
                    operandSlot(instruction, 1)) return false;
                break;
            case mir::Opcode::NewStruct: {
                const auto typeId = instruction.resultTypeId != 0
                    ? instruction.resultTypeId
                    : instruction.typeId;
                if (classifyInlineMath(semantic::PrimitiveType::Struct, typeId) ==
                        InlineMathKind::None &&
                    customStructs.find(typeId) == customStructs.end()) {
                    return false;
                }
                break;
            }
            case mir::Opcode::LoadStructField:
            case mir::Opcode::StoreStructField: {
                const auto kind = classifyInlineMath(
                    semantic::PrimitiveType::Struct, instruction.typeId);
                if (kind != InlineMathKind::None) {
                    if (inlineMathFieldMember(kind, instruction.fieldIndex) == nullptr) {
                        return false;
                    }
                } else if (customStructs.find(instruction.typeId) == customStructs.end() ||
                           instruction.fieldIndex >= 16) {
                    return false;
                }
                break;
            }
            case mir::Opcode::AsyncEnter:
                if (!isAsyncFunction) return false;
                break;
            case mir::Opcode::AwaitSuspend: {
                if (!isAsyncFunction ||
                    instruction.operands.size() != 1 ||
                    instruction.localIndex >= function.localTypes.size()) {
                    return false;
                }
                const auto opSlot = operandSlot(instruction, 0);
                const auto locTid = instruction.localIndex < function.localTypeIds.size()
                    ? function.localTypeIds[instruction.localIndex]
                    : 0;
                if (opSlot.primitive != semantic::PrimitiveType::Struct ||
                    customStructs.find(opSlot.typeId) == customStructs.end() ||
                    function.localTypes[instruction.localIndex] != semantic::PrimitiveType::Struct ||
                    locTid != opSlot.typeId) {
                    return false;
                }
                break;
            }
            case mir::Opcode::AwaitResume: {
                if (!isAsyncFunction ||
                    instruction.localIndex >= function.localTypes.size() ||
                    function.localTypes[instruction.localIndex] != semantic::PrimitiveType::Struct) {
                    return false;
                }
                const auto locTid = instruction.localIndex < function.localTypeIds.size()
                    ? function.localTypeIds[instruction.localIndex]
                    : 0;
                if (customStructs.find(locTid) == customStructs.end()) {
                    return false;
                }
                if (instruction.result >= 0 &&
                    instruction.elementType != semantic::PrimitiveType::Void &&
                    !isTypedAotSlot({instruction.elementType, instruction.elementTypeId}, &customStructs)) {
                    return false;
                }
                break;
            }
            case mir::Opcode::AsyncResolve: {
                if (!isAsyncFunction ||
                    instruction.result < 0 ||
                    instruction.resultType != semantic::PrimitiveType::Struct ||
                    customStructs.find(instruction.typeId) == customStructs.end() ||
                    instruction.operands.size() > 1) {
                    return false;
                }
                if (instruction.operands.size() == 1 &&
                    !isTypedAotSlot(operandSlot(instruction, 0), &customStructs)) {
                    return false;
                }
                break;
            }
            case mir::Opcode::Call: {
                if (instruction.virtualDispatch && instruction.interfaceDispatch) {
                    return false;
                }
                const std::string_view symName(instruction.symbolName);
                const bool isEcsIntrinsic =
                    symName.rfind("$intrinsic::static_get::ecs_col::", 0) == 0 ||
                    symName.rfind("$intrinsic::static_get::ecs_ent::", 0) == 0 ||
                    symName.rfind("$intrinsic::ecs_writeback::", 0) == 0;
                const bool isAsyncIntrinsic =
                    symName.rfind("$intrinsic::Async.__", 0) == 0;
                bool hasPolyOrLoopInFn = function.blocks.size() > 1 || isAsyncFunction;
                if (!hasPolyOrLoopInFn && usesReferenceSlots && !isEcsIntrinsic && !isAsyncIntrinsic) {
                    for (const auto& scanBlk : function.blocks) {
                        for (const auto& scanInst : scanBlk.instructions) {
                            if (scanInst.opcode == mir::Opcode::IsType ||
                                scanInst.opcode == mir::Opcode::AsType ||
                                scanInst.opcode == mir::Opcode::NewDelegate ||
                                scanInst.opcode == mir::Opcode::InvokeDelegate ||
                                scanInst.opcode == mir::Opcode::CombineDelegate ||
                                scanInst.opcode == mir::Opcode::RemoveDelegate ||
                                (scanInst.opcode == mir::Opcode::Call &&
                                 (scanInst.virtualDispatch || scanInst.interfaceDispatch))) {
                                hasPolyOrLoopInFn = true;
                                break;
                            }
                        }
                        if (hasPolyOrLoopInFn) break;
                    }
                    if (!hasPolyOrLoopInFn) {
                        return false;
                    }
                }
                if (!symName.empty() &&
                    symName.rfind("$intrinsic::", 0) == 0 &&
                    !isEcsIntrinsic &&
                    !isAsyncIntrinsic) {
                    return false;
                }
                if (instruction.resultType != semantic::PrimitiveType::Void &&
                    !isTypedAotSlot({instruction.resultType, instruction.resultTypeId}, &customStructs)) {
                    return false;
                }
                if (instruction.operands.size() != instruction.parameterTypes.size()) {
                    return false;
                }
                for (std::size_t index = 0; index < instruction.operands.size(); ++index) {
                    const auto paramTypeId = index < instruction.parameterTypeIds.size()
                        ? instruction.parameterTypeIds[index]
                        : 0;
                    const TypedSlotType expectedSlot{
                        instruction.parameterTypes[index],
                        paramTypeId,
                    };
                    if (!isTypedAotSlot(expectedSlot, &customStructs)) {
                        return false;
                    }
                    const auto actualOpSlot = operandSlot(instruction, index);
                    if (expectedSlot.primitive == semantic::PrimitiveType::Object) {
                        if (actualOpSlot.primitive != semantic::PrimitiveType::Object &&
                            actualOpSlot.primitive != semantic::PrimitiveType::Null) {
                            return false;
                        }
                    } else if (actualOpSlot != expectedSlot) {
                        return false;
                    }
                }
                break;
            }
            case mir::Opcode::NewDelegate: {
                if (instruction.resultType != semantic::PrimitiveType::Object ||
                    instruction.typeId == 0 ||
                    instruction.operands.size() > 1 ||
                    (instruction.virtualDispatch && instruction.interfaceDispatch)) {
                    return false;
                }
                if (instruction.operands.size() == 1) {
                    const auto recvSlot = operandSlot(instruction, 0);
                    if (recvSlot.primitive != semantic::PrimitiveType::Object &&
                        recvSlot.primitive != semantic::PrimitiveType::Null) {
                        return false;
                    }
                } else if (instruction.virtualDispatch || instruction.interfaceDispatch) {
                    return false;
                }
                break;
            }
            case mir::Opcode::CombineDelegate:
            case mir::Opcode::RemoveDelegate: {
                if (instruction.resultType != semantic::PrimitiveType::Object ||
                    instruction.typeId == 0 ||
                    instruction.operands.size() != 2) {
                    return false;
                }
                const auto lSlot = operandSlot(instruction, 0);
                const auto rSlot = operandSlot(instruction, 1);
                if ((lSlot.primitive != semantic::PrimitiveType::Object &&
                     lSlot.primitive != semantic::PrimitiveType::Null) ||
                    (rSlot.primitive != semantic::PrimitiveType::Object &&
                     rSlot.primitive != semantic::PrimitiveType::Null)) {
                    return false;
                }
                break;
            }
            case mir::Opcode::InvokeDelegate: {
                if (instruction.typeId == 0 || instruction.operands.empty()) {
                    return false;
                }
                const auto delSlot = operandSlot(instruction, 0);
                if (delSlot.primitive != semantic::PrimitiveType::Object &&
                    delSlot.primitive != semantic::PrimitiveType::Null) {
                    return false;
                }
                if (instruction.resultType != semantic::PrimitiveType::Void &&
                    !isTypedAotSlot({instruction.resultType, instruction.resultTypeId}, &customStructs)) {
                    return false;
                }
                for (std::size_t idx = 1; idx < instruction.operands.size(); ++idx) {
                    if (!isTypedAotSlot(operandSlot(instruction, idx), &customStructs)) {
                        return false;
                    }
                }
                break;
            }
            default:
                return false;
            }
        }
        switch (block.terminator.kind) {
        case mir::TerminatorKind::ReturnVoid:
        case mir::TerminatorKind::ReturnValue:
        case mir::TerminatorKind::Jump:
        case mir::TerminatorKind::Branch:
            break;
        case mir::TerminatorKind::Throw:
        case mir::TerminatorKind::None:
            return false;
        }
    }
    return true;
}

std::string typedCppType(TypedSlotType slot) {
    switch (slot.primitive) {
    case semantic::PrimitiveType::Bool:
        return "bool";
    case semantic::PrimitiveType::Int:
    case semantic::PrimitiveType::Long:
        return "std::int64_t";
    case semantic::PrimitiveType::Float:
        return "float";
    case semantic::PrimitiveType::Double:
        return "double";
    case semantic::PrimitiveType::Enum:
        return "runtime::EnumValue";
    case semantic::PrimitiveType::Object:
    case semantic::PrimitiveType::Array:
    case semantic::PrimitiveType::Null:
        return "runtime::Value";
    case semantic::PrimitiveType::Struct:
        switch (classifyInlineMath(slot.primitive, slot.typeId)) {
        case InlineMathKind::Fixed:   return "runtime::FixedValue";
        case InlineMathKind::Fixed16: return "runtime::Fixed16Value";
        case InlineMathKind::Fixed64: return "runtime::Fixed64Value";
        case InlineMathKind::Vec2:    return "runtime::Vec2Value";
        case InlineMathKind::Vec3:    return "runtime::Vec3Value";
        case InlineMathKind::Quat:    return "runtime::QuatValue";
        case InlineMathKind::Vec2f:   return "runtime::Vec2fValue";
        case InlineMathKind::Vec3f:   return "runtime::Vec3fValue";
        case InlineMathKind::Quatf:   return "runtime::QuatfValue";
        case InlineMathKind::None:    return "AotStruct_" + hexId(slot.typeId);
        }
        break;
    default:
        break;
    }
    return "std::int64_t";
}

std::string typedDefaultValue(TypedSlotType slot) {
    switch (slot.primitive) {
    case semantic::PrimitiveType::Bool:
        return "false";
    case semantic::PrimitiveType::Int:
    case semantic::PrimitiveType::Long:
        return "std::int64_t{0}";
    case semantic::PrimitiveType::Float:
        return "0.0f";
    case semantic::PrimitiveType::Double:
        return "0.0";
    case semantic::PrimitiveType::Enum:
        return "runtime::EnumValue{0x" + hexId(slot.typeId) + "ULL, 0}";
    case semantic::PrimitiveType::Object:
        return "runtime::Value{runtime::NullObject{}}";
    case semantic::PrimitiveType::Array:
        return "runtime::Value{runtime::NullArray{}}";
    case semantic::PrimitiveType::Null:
        return "runtime::Value{std::monostate{}}";
    case semantic::PrimitiveType::Struct:
        switch (classifyInlineMath(slot.primitive, slot.typeId)) {
        case InlineMathKind::Fixed:   return "runtime::FixedValue{}";
        case InlineMathKind::Fixed16: return "runtime::Fixed16Value{}";
        case InlineMathKind::Fixed64: return "runtime::Fixed64Value{}";
        case InlineMathKind::Vec2:    return "runtime::Vec2Value{}";
        case InlineMathKind::Vec3:    return "runtime::Vec3Value{}";
        case InlineMathKind::Quat:    return "runtime::QuatValue{}";
        case InlineMathKind::Vec2f:   return "runtime::Vec2fValue{}";
        case InlineMathKind::Vec3f:   return "runtime::Vec3fValue{}";
        case InlineMathKind::Quatf:   return "runtime::QuatfValue{}";
        case InlineMathKind::None:    return "AotStruct_" + hexId(slot.typeId) + "{}";
        }
        break;
    default:
        break;
    }
    return "std::int64_t{0}";
}

std::string typedValueExpression(mir::ValueId id) {
    return "typedRegister_" + std::to_string(id);
}

std::string typedLocalExpression(std::size_t index) {
    return "typedLocal_" + std::to_string(index);
}

std::string int64Literal(std::int64_t value) {
    if (value == std::numeric_limits<std::int64_t>::min()) {
        return "std::numeric_limits<std::int64_t>::min()";
    }
    return std::to_string(value) + "LL";
}

std::string typedScalarFunctionSignature(
    const FunctionView& view,
    bool declarationOnly) {
    const auto& function = *view.function;
    std::ostringstream out;
    out << "template <bool FastAccounting, bool DeterminismAccounting, bool TrustedRelease"
        << (declarationOnly ? " = false" : "") << ">\n";
    out << "static bool " << view.cppName << "_scalar(\n";
    out << "    ExecutionContext& context";
    for (std::size_t index = 0; index < function.parameterTypes.size(); ++index) {
        const auto typeId = index < function.parameterTypeIds.size()
            ? function.parameterTypeIds[index]
            : 0;
        out << ",\n    "
            << typedCppType({function.parameterTypes[index], typeId})
            << " typedArg_" << index;
    }
    if (function.returnType != semantic::PrimitiveType::Void) {
        out << ",\n    "
            << typedCppType({function.returnType, function.returnTypeId})
            << "& typedOutResult";
    }
    out << ")" << (declarationOnly ? ";" : " {");
    return out.str();
}

void emitTypedTransfer(
    Emitter& output,
    const mir::Function& function,
    mir::BlockId target,
    const std::vector<mir::ValueId>& arguments,
    const std::string& prefix,
    const std::string& indent,
    const std::vector<std::size_t>* regGuardSlot) {
    const auto* block = findBlock(function, target);
    if (!block) return;
    output.line(indent + "if constexpr (FastAccounting && !TrustedRelease) {");
    output.line(indent + "    rawAccounting.branch();");
    output.line(indent + "} else if constexpr (DeterminismAccounting && !TrustedRelease) {");
    output.line(indent + "    determinismAccounting.branch(" +
        std::to_string(target) + ");");
    output.line(indent + "} else if constexpr (!TrustedRelease) {");
    output.line(indent + "    if (!context.branch(" +
        std::to_string(target) + ")) return false;");
    output.line(indent + "}");
    for (std::size_t index = 0;
         index < arguments.size() && index < block->parameters.size();
         ++index) {
        output.line(indent + "const auto " + prefix + "_" +
            std::to_string(index) + " = " +
            typedValueExpression(arguments[index]) + ";");
    }
    for (std::size_t index = 0;
         index < arguments.size() && index < block->parameters.size();
         ++index) {
        const auto paramVal = block->parameters[index].value;
        output.line(indent +
            typedValueExpression(paramVal) + " = " +
            prefix + "_" + std::to_string(index) + ";");
        if (regGuardSlot != nullptr && paramVal >= 0) {
            const auto rIdx = static_cast<std::size_t>(paramVal);
            if (rIdx < regGuardSlot->size() &&
                (*regGuardSlot)[rIdx] != std::numeric_limits<std::size_t>::max()) {
                output.line(indent + "if constexpr (!FastAccounting && !DeterminismAccounting && !TrustedRelease) {");
                output.line(indent + "    aot::retainTypedGcSlot(context.rawHeap(), typedLocalGcGuards[" +
                    std::to_string((*regGuardSlot)[rIdx]) + "], " +
                    typedValueExpression(paramVal) + ");");
                output.line(indent + "}");
            }
        }
    }
    output.line(indent + "goto typedBlock_" + std::to_string(target) + ";");
}

void emitTypedRawConsume(
    Emitter& output,
    std::uint64_t count,
    const std::string& indent) {
    if (count == 0) return;
    output.line(indent + "if constexpr (FastAccounting && !TrustedRelease) {");
    output.line(indent + "    if (!rawAccounting.consume(" +
        std::to_string(count) + "ULL)) return false;");
    output.line(indent + "}");
}

void emitTypedConsume(
    Emitter& output,
    mir::Opcode opcode,
    const std::string& indent) {
    const auto operationId = std::to_string(
        runtime::determinismOperationId(mir::opcodeName(opcode))) + "ULL";
    output.line(indent + "if constexpr (DeterminismAccounting && !TrustedRelease) {");
    output.line(indent + "    if (!determinismAccounting.consume(" +
        operationId + ")) return false;");
    output.line(indent + "} else if constexpr (!FastAccounting && !TrustedRelease) {");
    output.line(indent + "    if (!context.consume(" +
        operationId + ", \"" +
        escapeCppString(mir::opcodeName(opcode)) +
        "\")) return false;");
    output.line(indent + "}");
}

void emitTypedConsume(
    Emitter& output,
    mir::TerminatorKind terminator,
    const std::string& indent) {
    const auto operationId = std::to_string(
        runtime::determinismOperationId(
            mir::terminatorName(terminator))) + "ULL";
    output.line(indent + "if constexpr (DeterminismAccounting && !TrustedRelease) {");
    output.line(indent + "    if (!determinismAccounting.consume(" +
        operationId + ")) return false;");
    output.line(indent + "} else if constexpr (!FastAccounting && !TrustedRelease) {");
    output.line(indent + "    if (!context.consume(" +
        operationId + ", \"" +
        escapeCppString(mir::terminatorName(terminator)) +
        "\")) return false;");
    output.line(indent + "}");
}

bool typedCheckedArithmeticProvenSafe(
    const TypedRangeAnalysis& analysis,
    const mir::Instruction& instruction) noexcept {
    return instruction.result >= 0 &&
        analysis.safeCheckedArithmetic.find(instruction.result) !=
            analysis.safeCheckedArithmetic.end();
}

bool typedInstructionCanFail(
    const TypedRangeAnalysis& analysis,
    const mir::Instruction& instruction) noexcept {
    switch (instruction.opcode) {
    case mir::Opcode::Parameter:
    case mir::Opcode::Call:
    case mir::Opcode::DivideInt:
    case mir::Opcode::RemainderInt:
    case mir::Opcode::DivideLong:
    case mir::Opcode::RemainderLong:
    case mir::Opcode::CheckNotNull:
    case mir::Opcode::LoadField:
    case mir::Opcode::StoreField:
    case mir::Opcode::NewObject:
    case mir::Opcode::NewArray:
    case mir::Opcode::IsType:
    case mir::Opcode::AsType:
    case mir::Opcode::ArrayLength:
    case mir::Opcode::LoadElement:
    case mir::Opcode::StoreElement:
    case mir::Opcode::NewStruct:
    case mir::Opcode::LoadStructField:
    case mir::Opcode::StoreStructField:
    case mir::Opcode::NewDelegate:
    case mir::Opcode::CombineDelegate:
    case mir::Opcode::RemoveDelegate:
    case mir::Opcode::InvokeDelegate:
    case mir::Opcode::AwaitSuspend:
    case mir::Opcode::AwaitResume:
    case mir::Opcode::AsyncResolve:
        return true;
    case mir::Opcode::AddInt:
    case mir::Opcode::SubtractInt:
    case mir::Opcode::MultiplyInt:
    case mir::Opcode::NegateInt:
        return instruction.checkedArithmetic &&
            !typedCheckedArithmeticProvenSafe(analysis, instruction);
    case mir::Opcode::AddLong:
    case mir::Opcode::SubtractLong:
    case mir::Opcode::MultiplyLong:
    case mir::Opcode::NegateLong:
    case mir::Opcode::ConvertNumeric:
        return instruction.checkedArithmetic;
    default:
        return false;
    }
}

std::vector<std::uint64_t> typedRawConsumeBatches(
    const mir::BasicBlock& block,
    const TypedRangeAnalysis& analysis) {
    std::vector<std::uint64_t> result(block.instructions.size() + 1, 0);
    std::size_t batchStart = 0;
    for (std::size_t index = 0; index < block.instructions.size(); ++index) {
        if (!typedInstructionCanFail(analysis, block.instructions[index])) continue;
        result[batchStart] = index - batchStart + 1;
        batchStart = index + 1;
    }
    result[batchStart] = block.instructions.size() - batchStart + 1;
    return result;
}

} // namespace realscript::aot::detail
