#include "realscript/mir/Mir.h"

#include <deque>
#include <unordered_map>
#include <unordered_set>

namespace realscript::mir {
namespace {

bool isIntCarrier(semantic::PrimitiveType type) noexcept {
    return type == semantic::PrimitiveType::Int ||
        type == semantic::PrimitiveType::UInt;
}

bool isLongCarrier(semantic::PrimitiveType type) noexcept {
    return type == semantic::PrimitiveType::Long ||
        type == semantic::PrimitiveType::ULong;
}

bool isFloatCarrier(semantic::PrimitiveType type) noexcept {
    return type == semantic::PrimitiveType::Float ||
        type == semantic::PrimitiveType::Double;
}

bool supportsArithmeticMode(Opcode opcode) noexcept {
    return opcode == Opcode::ConvertNumeric ||
        opcode == Opcode::NegateInt || opcode == Opcode::NegateLong ||
        opcode == Opcode::NegateDouble ||
        (opcode >= Opcode::AddInt && opcode <= Opcode::RemainderInt) ||
        (opcode >= Opcode::AddLong && opcode <= Opcode::RemainderLong) ||
        (opcode >= Opcode::AddDouble && opcode <= Opcode::DivideDouble);
}

struct Definition {
    BlockId block = 0;
    std::int64_t instructionIndex = -1;
    semantic::PrimitiveType type = semantic::PrimitiveType::Error;
    semantic::SymbolId typeId = 0;
};

bool descriptorAssignable(
    semantic::SymbolId actual,
    semantic::SymbolId expected,
    const std::unordered_map<semantic::SymbolId,
        const semantic::TypeSymbol*>& types) noexcept {
    if (actual == expected) return true;
    std::unordered_set<semantic::SymbolId> visited;
    auto current = actual;
    while (current != 0 && visited.insert(current).second) {
        const auto found = types.find(current);
        if (found == types.end()) return false;
        for (const auto& interfaceMap :
             found->second->interfaceDispatchMaps) {
            if (interfaceMap.interfaceTypeId == expected) return true;
        }
        if (found->second->baseTypeId == expected) return true;
        current = found->second->baseTypeId;
    }
    return false;
}

bool compatibleType(
    semantic::PrimitiveType actualType,
    semantic::SymbolId actualTypeId,
    semantic::PrimitiveType expectedType,
    semantic::SymbolId expectedTypeId,
    const std::unordered_map<semantic::SymbolId,
        const semantic::TypeSymbol*>& types) noexcept {
    if (actualType != expectedType) return false;
    if (actualType == semantic::PrimitiveType::Object) {
        if (expectedTypeId == 0) return true;
        if (actualTypeId == 0) return false;
        return descriptorAssignable(actualTypeId, expectedTypeId, types);
    }
    return actualTypeId == expectedTypeId;
}

std::size_t expectedOperandCount(const Instruction& instruction) noexcept {
    switch (instruction.opcode) {
    case Opcode::Parameter:
    case Opcode::ConstantInt:
    case Opcode::ConstantDouble:
    case Opcode::ConstantBool:
    case Opcode::ConstantString:
    case Opcode::ConstantNull:
    case Opcode::LoadLocal:
    case Opcode::NewObject:
    case Opcode::NewStruct:
    case Opcode::ConstantTypeId:
        return 0;
    case Opcode::NewArray:
        return 1;
    case Opcode::StoreLocal:
    case Opcode::ConvertNullToString:
    case Opcode::ConvertNullToObject:
    case Opcode::ConvertNullToArray:
    case Opcode::ConvertIntToLong:
    case Opcode::ConvertIntToDouble:
    case Opcode::ConvertLongToDouble:
    case Opcode::ConvertNumeric:
    case Opcode::CheckNotNull:
    case Opcode::ArrayLength:
    case Opcode::LoadField:
    case Opcode::LoadStructField:
    case Opcode::NegateInt:
    case Opcode::NegateLong:
    case Opcode::NegateDouble:
    case Opcode::LogicalNot:
    case Opcode::IsType:
    case Opcode::AsType:
        return 1;
    case Opcode::StoreField:
    case Opcode::StoreStructField:
    case Opcode::LoadElement:
        return 2;
    case Opcode::StoreElement:
        return 3;
    case Opcode::Call:
        return instruction.parameterTypes.size();
    case Opcode::NewDelegate:
        return instruction.operands.size();
    case Opcode::InvokeDelegate:
        return instruction.operands.size();
    default:
        return 2;
    }
}


bool validTypeIdentity(
    semantic::PrimitiveType type,
    semantic::SymbolId typeId) noexcept {
    if (type == semantic::PrimitiveType::Object && typeId == 0) {
        return true;
    }
    return semantic::isExactType(type) ? typeId != 0 : typeId == 0;
}


semantic::SymbolId typeIdAt(
    const std::vector<semantic::SymbolId>& typeIds,
    std::size_t index) noexcept {
    return index < typeIds.size() ? typeIds[index] : 0;
}

bool definesValue(const Instruction& instruction) noexcept {
    if (instruction.opcode == Opcode::StoreLocal ||
        instruction.opcode == Opcode::StoreField ||
        instruction.opcode == Opcode::StoreElement) {
        return false;
    }
    return (instruction.opcode != Opcode::Call &&
            instruction.opcode != Opcode::InvokeDelegate) ||
        instruction.resultType != semantic::PrimitiveType::Void;
}

} // namespace

bool verifyModule(
    const Module& module,
    diagnostics::DiagnosticBag& diagnostics) {
    const auto initialCount = diagnostics.items().size();

    std::unordered_map<semantic::SymbolId, const semantic::TypeSymbol*> types;
    for (const auto& type : module.types) {
        if (type.id == 0 || !types.emplace(type.id, &type).second) {
            diagnostics.report("RS3040", "duplicate or invalid MIR type descriptor", {});
        }
        if (type.kind != semantic::TypeKind::Class &&
            !type.virtualDispatchTable.empty()) {
            diagnostics.report(
                "RS3066",
                "non-class MIR type has a virtual dispatch table",
                {});
        }
        if (type.interfaceType &&
            (!type.fields.empty() ||
             !type.virtualDispatchTable.empty() ||
             !type.interfaceDispatchMaps.empty())) {
            diagnostics.report(
                "RS3070",
                "MIR interface descriptor contains runtime class state",
                {});
        }
        if (!type.abstractType) {
            for (const auto symbolId : type.virtualDispatchTable) {
                if (symbolId == 0) {
                    diagnostics.report(
                        "RS3067",
                        "concrete MIR type has an abstract virtual slot",
                        {});
                }
            }
        }
        std::unordered_set<semantic::SymbolId> interfaceIds;
        for (const auto& interfaceMap :
             type.interfaceDispatchMaps) {
            if (interfaceMap.interfaceTypeId == 0 ||
                !interfaceIds.insert(
                    interfaceMap.interfaceTypeId).second) {
                diagnostics.report(
                    "RS3071",
                    "MIR type has an invalid or duplicate interface map",
                    {});
            }
            if (!type.abstractType) {
                for (const auto symbolId : interfaceMap.slots) {
                    if (symbolId == 0) {
                        diagnostics.report(
                            "RS3072",
                            "concrete MIR type has an abstract interface slot",
                            {});
                    }
                }
            }
        }
        for (const auto& field : type.fields) {
            if (field.type == semantic::PrimitiveType::Void ||
                field.type == semantic::PrimitiveType::Null ||
                field.type == semantic::PrimitiveType::Error) {
                diagnostics.report("RS3041", "invalid MIR field type", {});
            }
        }
    }

    for (const auto& function : module.functions) {
        if (!validTypeIdentity(function.returnType, function.returnTypeId)) {
            diagnostics.report(
                "RS3048",
                "MIR function has an invalid return type identity",
                {});
        }
        if (function.parameterTypeIds.size() != function.parameterTypes.size()) {
            diagnostics.report(
                "RS3049",
                "MIR function parameter type ID count is invalid",
                {});
        }
        for (std::size_t parameter = 0;
             parameter < function.parameterTypes.size();
             ++parameter) {
            if (!validTypeIdentity(
                    function.parameterTypes[parameter],
                    typeIdAt(function.parameterTypeIds, parameter))) {
                diagnostics.report(
                    "RS3050",
                    "MIR function has an invalid parameter type identity",
                    {});
            }
        }
        if (function.localTypeIds.size() != function.localTypes.size()) {
            diagnostics.report(
                "RS3059",
                "MIR function local type ID count is invalid",
                {});
        }
        for (std::size_t local = 0; local < function.localTypes.size(); ++local) {
            if (!validTypeIdentity(
                    function.localTypes[local],
                    typeIdAt(function.localTypeIds, local))) {
                diagnostics.report(
                    "RS3060",
                    "MIR function has an invalid local type identity",
                    {});
            }
        }
        if (function.blocks.empty()) {
            diagnostics.report(
                "RS3000",
                "MIR function '" + function.name + "' has no entry block",
                {});
            continue;
        }

        std::unordered_map<BlockId, const BasicBlock*> blocks;
        std::unordered_map<ValueId, Definition> definitions;

        for (const auto& basicBlock : function.blocks) {
            if (!blocks.emplace(basicBlock.id, &basicBlock).second) {
                diagnostics.report(
                    "RS3001",
                    "duplicate MIR block id",
                    basicBlock.terminator.sourceSpan);
            }

            for (const auto& parameter : basicBlock.parameters) {
                if (!validTypeIdentity(parameter.type, parameter.typeId)) {
                    diagnostics.report(
                        "RS3061",
                        "MIR block parameter has an invalid type identity",
                        {});
                }
                if (parameter.value < 0 ||
                    !definitions.emplace(
                        parameter.value,
                        Definition{
                            basicBlock.id,
                            -1,
                            parameter.type,
                            parameter.typeId,
                        }).second) {
                    diagnostics.report(
                        "RS3003",
                        "duplicate or invalid MIR value id",
                        {});
                }
            }

            for (std::size_t i = 0; i < basicBlock.instructions.size(); ++i) {
                const auto& instruction = basicBlock.instructions[i];
                if (definesValue(instruction)) {
                    if (!validTypeIdentity(
                            instruction.resultType,
                            instruction.resultTypeId)) {
                        diagnostics.report(
                            "RS3062",
                            "MIR instruction has an invalid result type identity",
                            instruction.sourceSpan);
                    }
                    if (instruction.result < 0 ||
                        !definitions.emplace(
                            instruction.result,
                            Definition{
                                basicBlock.id,
                                static_cast<std::int64_t>(i),
                                instruction.resultType,
                                instruction.resultTypeId,
                            }).second) {
                        diagnostics.report(
                            "RS3003",
                            "duplicate or invalid MIR value id",
                            instruction.sourceSpan);
                    }
                } else if (instruction.result >= 0) {
                    diagnostics.report(
                        "RS3004",
                        "non-value MIR instruction defines a result",
                        instruction.sourceSpan);
                }
            }
        }
        if (blocks.find(0) == blocks.end()) {
            diagnostics.report(
                "RS3002",
                "MIR function entry block must be bb0",
                {});
        }

        const auto valueType = [&](ValueId value) {
            const auto found = definitions.find(value);
            return found == definitions.end()
                ? semantic::PrimitiveType::Error
                : found->second.type;
        };
        const auto valueTypeId = [&](ValueId value) {
            const auto found = definitions.find(value);
            return found == definitions.end() ? 0 : found->second.typeId;
        };

        std::unordered_map<BlockId, std::vector<BlockId>> successors;
        std::unordered_map<BlockId, std::vector<BlockId>> predecessors;
        const auto addEdge = [&](BlockId from, BlockId to) {
            if (blocks.find(to) != blocks.end()) {
                successors[from].push_back(to);
                predecessors[to].push_back(from);
            }
        };

        for (const auto& basicBlock : function.blocks) {
            if (basicBlock.terminator.kind == TerminatorKind::Jump) {
                addEdge(basicBlock.id, basicBlock.terminator.target);
            } else if (basicBlock.terminator.kind == TerminatorKind::Branch) {
                addEdge(basicBlock.id, basicBlock.terminator.target);
                addEdge(basicBlock.id, basicBlock.terminator.falseTarget);
            }
        }
        for (const auto& handler : function.exceptionHandlers) {
            if (blocks.find(handler.handlerBlock) == blocks.end() ||
                handler.exceptionLocal >= function.localTypes.size() ||
                function.localTypes[handler.exceptionLocal] !=
                    semantic::PrimitiveType::Object ||
                typeIdAt(function.localTypeIds, handler.exceptionLocal) !=
                    handler.catchTypeId) {
                diagnostics.report(
                    "RS3064", "invalid MIR exception handler target or local", {});
                continue;
            }
            if (handler.catchTypeId != 0) {
                const auto catchType = types.find(handler.catchTypeId);
                if (catchType == types.end() ||
                    catchType->second->kind != semantic::TypeKind::Class) {
                    diagnostics.report(
                        "RS3064", "invalid MIR exception handler catch type", {});
                }
            }
            for (const auto protectedBlock : handler.protectedBlocks) {
                if (blocks.find(protectedBlock) == blocks.end()) {
                    diagnostics.report(
                        "RS3064", "invalid MIR protected block", {});
                } else {
                    addEdge(protectedBlock, handler.handlerBlock);
                }
            }
        }

        std::unordered_set<BlockId> reachable;
        std::deque<BlockId> worklist;
        if (blocks.find(0) != blocks.end()) {
            reachable.insert(0);
            worklist.push_back(0);
        }
        while (!worklist.empty()) {
            const auto current = worklist.front();
            worklist.pop_front();
            for (const auto next : successors[current]) {
                if (reachable.insert(next).second) {
                    worklist.push_back(next);
                }
            }
        }
        for (const auto& basicBlock : function.blocks) {
            if (reachable.find(basicBlock.id) == reachable.end()) {
                diagnostics.report(
                    "RS3005",
                    "unreachable MIR block bb" + std::to_string(basicBlock.id) +
                        " in function '" + function.name + "'",
                    {});
            }
        }

        std::unordered_map<BlockId, std::unordered_set<BlockId>> dominators;
        for (const auto& basicBlock : function.blocks) {
            if (basicBlock.id == 0) {
                dominators[basicBlock.id] = {0};
            } else {
                for (const auto& candidate : function.blocks) {
                    dominators[basicBlock.id].insert(candidate.id);
                }
            }
        }

        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& basicBlock : function.blocks) {
                if (basicBlock.id == 0 ||
                    reachable.find(basicBlock.id) == reachable.end()) {
                    continue;
                }

                std::unordered_set<BlockId> next;
                const auto& blockPredecessors = predecessors[basicBlock.id];
                if (!blockPredecessors.empty()) {
                    next = dominators[blockPredecessors.front()];
                    for (std::size_t i = 1;
                         i < blockPredecessors.size();
                         ++i) {
                        std::unordered_set<BlockId> intersection;
                        for (const auto value : next) {
                            if (dominators[blockPredecessors[i]].find(value) !=
                                dominators[blockPredecessors[i]].end()) {
                                intersection.insert(value);
                            }
                        }
                        next = std::move(intersection);
                    }
                }
                next.insert(basicBlock.id);
                if (next != dominators[basicBlock.id]) {
                    dominators[basicBlock.id] = std::move(next);
                    changed = true;
                }
            }
        }

        const auto checkUse = [&](ValueId value,
                                  BlockId useBlock,
                                  std::int64_t useIndex,
                                  text::TextSpan span) {
            const auto found = definitions.find(value);
            if (found == definitions.end()) {
                diagnostics.report(
                    "RS3006",
                    "MIR uses undefined value %" + std::to_string(value),
                    span);
                return;
            }
            const auto& definition = found->second;
            if (definition.block == useBlock) {
                if (definition.instructionIndex >= useIndex &&
                    definition.instructionIndex >= 0) {
                    diagnostics.report(
                        "RS3007",
                        "MIR value is used before its definition",
                        span);
                }
            } else if (dominators[useBlock].find(definition.block) ==
                       dominators[useBlock].end()) {
                diagnostics.report(
                    "RS3008",
                    "MIR value definition does not dominate its use",
                    span);
            }
        };

        const auto verifyEdge = [&](BlockId from,
                                    BlockId target,
                                    const std::vector<ValueId>& arguments,
                                    text::TextSpan span) {
            const auto targetBlock = blocks.find(target);
            if (targetBlock == blocks.end()) {
                diagnostics.report(
                    "RS3009",
                    "MIR branch targets missing block bb" +
                        std::to_string(target),
                    span);
                return;
            }

            const auto& parameters = targetBlock->second->parameters;
            if (arguments.size() != parameters.size()) {
                diagnostics.report(
                    "RS3010",
                    "MIR branch argument count does not match target block",
                    span);
                return;
            }

            for (std::size_t i = 0; i < arguments.size(); ++i) {
                checkUse(
                    arguments[i],
                    from,
                    static_cast<std::int64_t>(
                        blocks.at(from)->instructions.size()),
                    span);
                if (!compatibleType(
                        valueType(arguments[i]),
                        valueTypeId(arguments[i]),
                        parameters[i].type,
                        parameters[i].typeId,
                        types)) {
                    diagnostics.report(
                        "RS3011",
                        "MIR branch argument type does not match block parameter",
                        span);
                }
            }
        };

        for (const auto& basicBlock : function.blocks) {
            for (std::size_t i = 0; i < basicBlock.instructions.size(); ++i) {
                const auto& instruction = basicBlock.instructions[i];
                const auto operandCountIsValid =
                    instruction.operands.size() ==
                    expectedOperandCount(instruction);
                if (!operandCountIsValid) {
                    diagnostics.report(
                        "RS3012",
                        "MIR instruction has an invalid operand count",
                        instruction.sourceSpan);
                }
                if (!instruction.checkedArithmetic &&
                    !supportsArithmeticMode(instruction.opcode)) {
                    diagnostics.report(
                        "RS3065",
                        "unchecked mode is invalid for this MIR opcode",
                        instruction.sourceSpan);
                }
                for (const auto operand : instruction.operands) {
                    checkUse(
                        operand,
                        basicBlock.id,
                        static_cast<std::int64_t>(i),
                        instruction.sourceSpan);
                }

                if (instruction.opcode == Opcode::Parameter) {
                    const auto parameterIndex =
                        static_cast<std::size_t>(instruction.integerImmediate);
                    if (instruction.integerImmediate < 0 ||
                        parameterIndex >= function.parameterTypes.size() ||
                        instruction.resultType !=
                            function.parameterTypes[parameterIndex] ||
                        instruction.resultTypeId !=
                            typeIdAt(function.parameterTypeIds, parameterIndex)) {
                        diagnostics.report(
                            "RS3013",
                            "invalid MIR parameter instruction",
                            instruction.sourceSpan);
                    }
                } else if (instruction.opcode == Opcode::ConstantInt &&
                           instruction.resultType != semantic::PrimitiveType::Int &&
                           instruction.resultType != semantic::PrimitiveType::Long &&
                           instruction.resultType != semantic::PrimitiveType::Enum) {
                    diagnostics.report(
                        "RS3026",
                        "integer constant must produce int, long, or enum",
                        instruction.sourceSpan);
                } else if (instruction.opcode == Opcode::ConstantDouble &&
                           instruction.resultType != semantic::PrimitiveType::Double) {
                    diagnostics.report(
                        "RS3063",
                        "const.f64 must produce double",
                        instruction.sourceSpan);
                } else if (instruction.opcode == Opcode::ConstantBool &&
                           instruction.resultType !=
                               semantic::PrimitiveType::Bool) {
                    diagnostics.report(
                        "RS3027",
                        "const.bool must produce bool",
                        instruction.sourceSpan);
                } else if (instruction.opcode == Opcode::ConstantString &&
                           instruction.resultType !=
                               semantic::PrimitiveType::String) {
                    diagnostics.report(
                        "RS3028",
                        "const.string must produce string",
                        instruction.sourceSpan);
                } else if (instruction.opcode == Opcode::ConstantNull &&
                           instruction.resultType !=
                               semantic::PrimitiveType::Null) {
                    diagnostics.report(
                        "RS3029",
                        "const.null must produce null",
                        instruction.sourceSpan);
                } else if (instruction.opcode == Opcode::LoadLocal ||
                           instruction.opcode == Opcode::StoreLocal) {
                    if (instruction.localIndex >= function.localTypes.size()) {
                        diagnostics.report(
                            "RS3014",
                            "MIR local index is outside the function local table",
                            instruction.sourceSpan);
                    } else if (instruction.opcode == Opcode::LoadLocal &&
                               (instruction.resultType !=
                                    function.localTypes[instruction.localIndex] ||
                                instruction.resultTypeId !=
                                    typeIdAt(function.localTypeIds,
                                        instruction.localIndex))) {
                        diagnostics.report(
                            "RS3015",
                            "MIR local load type does not match local table",
                            instruction.sourceSpan);
                    } else if (instruction.opcode == Opcode::StoreLocal &&
                               operandCountIsValid &&
                               !compatibleType(
                                    valueType(instruction.operands.front()),
                                    valueTypeId(instruction.operands.front()),
                                    function.localTypes[instruction.localIndex],
                                    typeIdAt(function.localTypeIds,
                                        instruction.localIndex),
                                    types)) {
                        diagnostics.report(
                            "RS3016",
                            "MIR local store type does not match local table",
                            instruction.sourceSpan);
                    }
                } else if (instruction.opcode == Opcode::ConvertNullToString &&
                           operandCountIsValid) {
                    if (instruction.resultType !=
                            semantic::PrimitiveType::String ||
                        valueType(instruction.operands.front()) !=
                            semantic::PrimitiveType::Null) {
                        diagnostics.report(
                            "RS3030",
                            "invalid null-to-string conversion",
                            instruction.sourceSpan);
                    }
                } else if (instruction.opcode == Opcode::ConvertNullToObject &&
                           operandCountIsValid) {
                    if (instruction.resultType != semantic::PrimitiveType::Object ||
                        valueType(instruction.operands.front()) !=
                            semantic::PrimitiveType::Null) {
                        diagnostics.report(
                            "RS3042",
                            "invalid null-to-object conversion",
                            instruction.sourceSpan);
                    }
                } else if (instruction.opcode == Opcode::ConvertNullToArray &&
                           operandCountIsValid) {
                    if (instruction.resultType != semantic::PrimitiveType::Array ||
                        instruction.resultTypeId == 0 ||
                        valueType(instruction.operands.front()) !=
                            semantic::PrimitiveType::Null) {
                        diagnostics.report(
                            "RS3053",
                            "invalid null-to-array conversion",
                            instruction.sourceSpan);
                    }
                } else if ((instruction.opcode == Opcode::ConvertIntToLong ||
                            instruction.opcode == Opcode::ConvertIntToDouble ||
                            instruction.opcode == Opcode::ConvertLongToDouble) &&
                           operandCountIsValid) {
                    const auto sourceType = valueType(instruction.operands.front());
                    const bool valid =
                        (instruction.opcode == Opcode::ConvertIntToLong &&
                         sourceType == semantic::PrimitiveType::Int &&
                         instruction.resultType == semantic::PrimitiveType::Long) ||
                        (instruction.opcode == Opcode::ConvertIntToDouble &&
                         sourceType == semantic::PrimitiveType::Int &&
                         instruction.resultType == semantic::PrimitiveType::Double) ||
                        (instruction.opcode == Opcode::ConvertLongToDouble &&
                         sourceType == semantic::PrimitiveType::Long &&
                         instruction.resultType == semantic::PrimitiveType::Double);
                    if (!valid) diagnostics.report(
                        "RS3064", "invalid numeric conversion", instruction.sourceSpan);
                } else if (instruction.opcode == Opcode::ConvertNumeric &&
                           operandCountIsValid) {
                    const auto sourceType = valueType(
                        instruction.operands.front());
                    if (!semantic::isNumericType(sourceType) ||
                        !semantic::isNumericType(instruction.resultType) ||
                        sourceType == instruction.resultType) {
                        diagnostics.report(
                            "RS3064", "invalid numeric conversion",
                            instruction.sourceSpan);
                    }
                } else if (instruction.opcode == Opcode::NewArray &&
                           operandCountIsValid) {
                    if (instruction.resultType != semantic::PrimitiveType::Array ||
                        instruction.resultTypeId == 0 ||
                        valueType(instruction.operands.front()) !=
                            semantic::PrimitiveType::Int ||
                        instruction.elementType == semantic::PrimitiveType::Error ||
                        instruction.elementType == semantic::PrimitiveType::Void ||
                        (semantic::isExactType(instruction.elementType) &&
                         instruction.elementTypeId == 0) ||
                        ((instruction.elementType == semantic::PrimitiveType::Object ||
                          instruction.elementType == semantic::PrimitiveType::Struct ||
                          instruction.elementType == semantic::PrimitiveType::Enum) &&
                         types.find(instruction.elementTypeId) == types.end())) {
                        diagnostics.report(
                            "RS3054",
                            "invalid MIR array allocation",
                            instruction.sourceSpan);
                    }
                } else if (instruction.opcode == Opcode::ArrayLength &&
                           operandCountIsValid) {
                    if (instruction.resultType != semantic::PrimitiveType::Int ||
                        valueType(instruction.operands.front()) !=
                            semantic::PrimitiveType::Array) {
                        diagnostics.report(
                            "RS3055",
                            "invalid MIR array length",
                            instruction.sourceSpan);
                    }
                } else if ((instruction.opcode == Opcode::LoadElement ||
                            instruction.opcode == Opcode::StoreElement) &&
                           operandCountIsValid) {
                    if (valueType(instruction.operands[0]) !=
                            semantic::PrimitiveType::Array ||
                        valueType(instruction.operands[1]) !=
                            semantic::PrimitiveType::Int ||
                        instruction.elementType == semantic::PrimitiveType::Error ||
                        instruction.elementType == semantic::PrimitiveType::Void ||
                        (semantic::isExactType(instruction.elementType) &&
                         instruction.elementTypeId == 0) ||
                        ((instruction.elementType == semantic::PrimitiveType::Object ||
                          instruction.elementType == semantic::PrimitiveType::Struct ||
                          instruction.elementType == semantic::PrimitiveType::Enum) &&
                         types.find(instruction.elementTypeId) == types.end())) {
                        diagnostics.report(
                            "RS3056",
                            "invalid MIR array element access",
                            instruction.sourceSpan);
                    } else if (instruction.opcode == Opcode::LoadElement &&
                               (instruction.resultType != instruction.elementType ||
                                instruction.resultTypeId != instruction.elementTypeId)) {
                        diagnostics.report(
                            "RS3057",
                            "MIR array element load type mismatch",
                            instruction.sourceSpan);
                    } else if (instruction.opcode == Opcode::StoreElement &&
                               !compatibleType(
                                   valueType(instruction.operands[2]),
                                   valueTypeId(instruction.operands[2]),
                                   instruction.elementType,
                                   instruction.elementTypeId,
                                   types)) {
                        diagnostics.report(
                            "RS3058",
                            "MIR array element store type mismatch",
                            instruction.sourceSpan);
                    }
                } else if (instruction.opcode == Opcode::NewObject ||
                           instruction.opcode == Opcode::NewStruct) {
                    const auto foundType = types.find(instruction.typeId);
                    const auto expected = instruction.opcode == Opcode::NewStruct
                        ? semantic::PrimitiveType::Struct
                        : semantic::PrimitiveType::Object;
                    const auto expectedKind = instruction.opcode == Opcode::NewStruct
                        ? semantic::TypeKind::Struct
                        : semantic::TypeKind::Class;
                    if (instruction.resultType != expected ||
                        instruction.resultTypeId != instruction.typeId ||
                        foundType == types.end() ||
                        foundType->second->kind != expectedKind ||
                        (instruction.opcode == Opcode::NewObject &&
                         (foundType->second->interfaceType ||
                          foundType->second->delegateType))) {
                        diagnostics.report(
                            "RS3043",
                            "invalid MIR named-type allocation",
                            instruction.sourceSpan);
                    }
                } else if (instruction.opcode == Opcode::CheckNotNull &&
                           operandCountIsValid) {
                    if (instruction.resultType != semantic::PrimitiveType::Object ||
                        valueType(instruction.operands.front()) !=
                            semantic::PrimitiveType::Object ||
                        types.find(instruction.typeId) == types.end()) {
                        diagnostics.report(
                            "RS3044",
                            "invalid object null check",
                            instruction.sourceSpan);
                    }
                } else if (instruction.opcode == Opcode::ConstantTypeId) {
                    if (instruction.resultType !=
                            semantic::PrimitiveType::ULong ||
                        !instruction.operands.empty()) {
                        diagnostics.report(
                            "RS3062", "invalid MIR typeof token",
                            instruction.sourceSpan);
                    }
                } else if ((instruction.opcode == Opcode::IsType ||
                            instruction.opcode == Opcode::AsType) &&
                           operandCountIsValid) {
                    const auto operandType = valueType(
                        instruction.operands.front());
                    const auto targetExact = semantic::isExactType(
                        instruction.elementType);
                    if (!semantic::isReferenceType(operandType) ||
                        (instruction.opcode == Opcode::IsType &&
                         instruction.resultType !=
                             semantic::PrimitiveType::Bool) ||
                        (instruction.opcode == Opcode::AsType &&
                         (instruction.resultType != instruction.elementType ||
                          !semantic::isReferenceType(
                              instruction.elementType))) ||
                        (targetExact &&
                         types.find(instruction.elementTypeId) == types.end())) {
                        diagnostics.report(
                            "RS3063", "invalid MIR runtime type operation",
                            instruction.sourceSpan);
                    }
                } else if ((instruction.opcode == Opcode::LoadField ||
                            instruction.opcode == Opcode::StoreField ||
                            instruction.opcode == Opcode::LoadStructField ||
                            instruction.opcode == Opcode::StoreStructField) &&
                           operandCountIsValid) {
                    const auto foundType = types.find(instruction.typeId);
                    const bool structure = instruction.opcode == Opcode::LoadStructField ||
                        instruction.opcode == Opcode::StoreStructField;
                    const auto receiverType = structure
                        ? semantic::PrimitiveType::Struct
                        : semantic::PrimitiveType::Object;
                    if (foundType == types.end() ||
                        instruction.fieldIndex >= foundType->second->fields.size() ||
                        valueType(instruction.operands[0]) != receiverType ||
                        valueTypeId(instruction.operands[0]) != instruction.typeId) {
                        diagnostics.report("RS3045", "invalid MIR field access", instruction.sourceSpan);
                    } else {
                        const auto& field = foundType->second->fields[instruction.fieldIndex];
                        const auto fieldTypeId = semantic::isExactType(field.type) &&
                                !field.typeName.empty()
                            ? semantic::stableTypeId(field.typeName)
                            : 0;
                        const bool load = instruction.opcode == Opcode::LoadField ||
                            instruction.opcode == Opcode::LoadStructField;
                        if (load && (instruction.resultType != field.type ||
                                     instruction.resultTypeId != fieldTypeId)) {
                            diagnostics.report("RS3046", "MIR field load type mismatch", instruction.sourceSpan);
                        }
                        if (!load && !compatibleType(
                                valueType(instruction.operands[1]),
                                valueTypeId(instruction.operands[1]),
                                field.type,
                                fieldTypeId,
                                types)) {
                            diagnostics.report(
                                "RS3047",
                                "MIR field store type mismatch for '" +
                                    foundType->second->name + "." + field.name +
                                    "': expected " +
                                    semantic::primitiveTypeName(field.type) +
                                    ", got " + semantic::primitiveTypeName(
                                        valueType(instruction.operands[1])) +
                                    " at source offset " + std::to_string(
                                        instruction.sourceSpan.start) +
                                    " in function '" + function.name + "'",
                                instruction.sourceSpan);
                        }
                        if (instruction.opcode == Opcode::StoreStructField &&
                            (instruction.resultType != semantic::PrimitiveType::Struct ||
                             instruction.resultTypeId != instruction.typeId)) {
                            diagnostics.report("RS3065", "MIR struct field store must return the updated struct", instruction.sourceSpan);
                        }
                    }
                } else if (instruction.opcode == Opcode::NewDelegate) {
                    const auto foundType = types.find(instruction.typeId);
                    if (instruction.operands.size() > 1 ||
                        instruction.resultType !=
                            semantic::PrimitiveType::Object ||
                        instruction.resultTypeId != instruction.typeId ||
                        foundType == types.end() ||
                        !foundType->second->delegateType ||
                        instruction.symbolId == 0 ||
                        instruction.parameterTypeIds.size() !=
                            instruction.parameterTypes.size() ||
                        instruction.elementType ==
                            semantic::PrimitiveType::Error ||
                        !validTypeIdentity(
                            instruction.elementType,
                            instruction.elementTypeId)) {
                        diagnostics.report(
                            "RS3080",
                            "invalid MIR delegate creation",
                            instruction.sourceSpan);
                    } else if (!instruction.operands.empty() &&
                        (instruction.parameterTypes.empty() ||
                         !compatibleType(
                             valueType(instruction.operands.front()),
                             valueTypeId(instruction.operands.front()),
                             instruction.parameterTypes.front(),
                             typeIdAt(
                                 instruction.parameterTypeIds, 0),
                             types))) {
                        diagnostics.report(
                            "RS3081",
                            "MIR delegate receiver type mismatch",
                            instruction.sourceSpan);
                    }
                } else if (instruction.opcode ==
                           Opcode::InvokeDelegate) {
                    const auto foundType = types.find(instruction.typeId);
                    const semantic::FunctionSymbol* invoke = nullptr;
                    if (foundType != types.end()) {
                        for (const auto& method :
                             foundType->second->methods) {
                            if (method.name == "Invoke") {
                                invoke = &method;
                                break;
                            }
                        }
                    }
                    bool valid = foundType != types.end() &&
                        foundType->second->delegateType && invoke &&
                        !instruction.operands.empty() &&
                        invoke->parameters.size() ==
                            instruction.operands.size() &&
                        valueType(instruction.operands.front()) ==
                            semantic::PrimitiveType::Object &&
                        valueTypeId(instruction.operands.front()) ==
                            instruction.typeId &&
                        instruction.resultType == invoke->returnType &&
                        instruction.resultTypeId ==
                            (semantic::isExactType(invoke->returnType)
                                ? semantic::stableTypeId(
                                    invoke->returnTypeName)
                                : 0);
                    if (valid) {
                        for (std::size_t index = 1;
                             index < instruction.operands.size(); ++index) {
                            const auto& parameter =
                                invoke->parameters[index];
                            const auto parameterType =
                                semantic::storageTypeOf(parameter);
                            const auto parameterTypeId =
                                semantic::isExactType(parameterType)
                                    ? semantic::stableTypeId(
                                        semantic::storageTypeNameOf(
                                            parameter))
                                    : 0;
                            if (!compatibleType(
                                    valueType(instruction.operands[index]),
                                    valueTypeId(instruction.operands[index]),
                                    parameterType,
                                    parameterTypeId,
                                    types)) {
                                valid = false;
                                break;
                            }
                        }
                    }
                    if (!valid ||
                        (instruction.resultType ==
                                semantic::PrimitiveType::Void
                            ? instruction.result >= 0
                            : instruction.result < 0)) {
                        diagnostics.report(
                            "RS3082",
                            "invalid MIR delegate invocation",
                            instruction.sourceSpan);
                    }
                } else if (instruction.opcode ==
                               Opcode::CombineDelegate ||
                           instruction.opcode ==
                               Opcode::RemoveDelegate) {
                    const auto foundType = types.find(
                        instruction.typeId);
                    if (instruction.operands.size() != 2 ||
                        foundType == types.end() ||
                        !foundType->second->delegateType ||
                        instruction.resultType !=
                            semantic::PrimitiveType::Object ||
                        instruction.resultTypeId != instruction.typeId ||
                        valueType(instruction.operands[0]) !=
                            semantic::PrimitiveType::Object ||
                        valueTypeId(instruction.operands[0]) !=
                            instruction.typeId ||
                        valueType(instruction.operands[1]) !=
                            semantic::PrimitiveType::Object ||
                        valueTypeId(instruction.operands[1]) !=
                            instruction.typeId) {
                        diagnostics.report(
                            "RS3083",
                            "invalid MIR delegate combination",
                            instruction.sourceSpan);
                    }
                } else if (instruction.opcode == Opcode::Call) {
                    if (instruction.symbolId == 0) {
                        diagnostics.report(
                            "RS3031",
                            "MIR call has invalid SymbolId",
                            instruction.sourceSpan);
                    }
                    if (instruction.parameterTypes.size() !=
                        instruction.operands.size()) {
                        diagnostics.report(
                            "RS3032",
                            "MIR call signature argument count mismatch",
                            instruction.sourceSpan);
                    }
                    if (instruction.parameterTypeIds.size() !=
                            instruction.parameterTypes.size()) {
                        diagnostics.report(
                            "RS3051",
                            "MIR call type ID count mismatch",
                            instruction.sourceSpan);
                    }
                    if (!validTypeIdentity(
                            instruction.resultType,
                            instruction.resultTypeId)) {
                        diagnostics.report(
                            "RS3052",
                            "MIR call result type identity is invalid",
                            instruction.sourceSpan);
                    }
                    for (std::size_t argumentIndex = 0;
                         argumentIndex < instruction.parameterTypes.size() &&
                         argumentIndex < instruction.operands.size();
                         ++argumentIndex) {
                        if (!compatibleType(
                                valueType(instruction.operands[argumentIndex]),
                                valueTypeId(instruction.operands[argumentIndex]),
                                instruction.parameterTypes[argumentIndex],
                                typeIdAt(instruction.parameterTypeIds,
                                    argumentIndex),
                                types)) {
                            diagnostics.report(
                                "RS3033",
                                "MIR call argument type mismatch",
                                instruction.sourceSpan);
                        }
                        if (!validTypeIdentity(
                                instruction.parameterTypes[argumentIndex],
                                typeIdAt(
                                    instruction.parameterTypeIds,
                                    argumentIndex))) {
                            diagnostics.report(
                                "RS3053",
                                "MIR call argument type identity is invalid",
                                instruction.sourceSpan);
                        }
                    }
                    if (instruction.virtualDispatch &&
                        instruction.interfaceDispatch) {
                        diagnostics.report(
                            "RS3073",
                            "MIR call cannot use virtual and interface dispatch together",
                            instruction.sourceSpan);
                    }
                    if (instruction.virtualDispatch) {
                        const auto receiverTypeId =
                            typeIdAt(instruction.parameterTypeIds, 0);
                        const auto receiverType = types.find(receiverTypeId);
                        if (instruction.virtualSlot ==
                                std::numeric_limits<std::uint32_t>::max() ||
                            instruction.parameterTypes.empty() ||
                            instruction.parameterTypes.front() !=
                                semantic::PrimitiveType::Object ||
                            receiverType == types.end() ||
                            instruction.virtualSlot >=
                                receiverType->second->virtualDispatchTable.size()) {
                            diagnostics.report(
                                "RS3068",
                                "MIR virtual call has an invalid slot contract",
                                instruction.sourceSpan);
                        }
                    } else if (instruction.virtualSlot !=
                            std::numeric_limits<std::uint32_t>::max()) {
                        diagnostics.report(
                            "RS3069",
                            "static MIR call carries a virtual slot",
                            instruction.sourceSpan);
                    }
                    if (instruction.interfaceDispatch) {
                        const auto receiverTypeId =
                            typeIdAt(instruction.parameterTypeIds, 0);
                        const auto receiverType = types.find(
                            instruction.interfaceTypeId);
                        if (instruction.interfaceTypeId == 0 ||
                            instruction.interfaceSlot ==
                                std::numeric_limits<std::uint32_t>::max() ||
                            instruction.parameterTypes.empty() ||
                            instruction.parameterTypes.front() !=
                                semantic::PrimitiveType::Object ||
                            receiverTypeId != instruction.interfaceTypeId ||
                            receiverType == types.end() ||
                            !receiverType->second->interfaceType ||
                            instruction.interfaceSlot >=
                                receiverType->second->methods.size()) {
                            diagnostics.report(
                                "RS3074",
                                "MIR interface call has an invalid slot contract",
                                instruction.sourceSpan);
                        }
                    } else if (instruction.interfaceTypeId != 0 ||
                               instruction.interfaceSlot !=
                                   std::numeric_limits<std::uint32_t>::max()) {
                        diagnostics.report(
                            "RS3075",
                            "non-interface MIR call carries interface metadata",
                            instruction.sourceSpan);
                    }
                    if (instruction.resultType == semantic::PrimitiveType::Void &&
                        instruction.result >= 0) {
                        diagnostics.report(
                            "RS3034",
                            "void MIR call defines a result",
                            instruction.sourceSpan);
                    }
                    if (instruction.resultType != semantic::PrimitiveType::Void &&
                        instruction.result < 0) {
                        diagnostics.report(
                            "RS3035",
                            "value MIR call has no result",
                            instruction.sourceSpan);
                    }
                } else if ((instruction.opcode == Opcode::NegateInt ||
                            instruction.opcode == Opcode::NegateLong ||
                            instruction.opcode == Opcode::NegateDouble) &&
                           operandCountIsValid) {
                    const auto operandType = valueType(
                        instruction.operands.front());
                    const bool carrierMatches = instruction.opcode == Opcode::NegateInt
                        ? isIntCarrier(operandType)
                        : instruction.opcode == Opcode::NegateLong
                            ? isLongCarrier(operandType)
                            : isFloatCarrier(operandType);
                    if (!carrierMatches ||
                        instruction.resultType != operandType) {
                        diagnostics.report("RS3017", "invalid numeric negation types", instruction.sourceSpan);
                    }
                } else if (instruction.opcode == Opcode::LogicalNot &&
                           operandCountIsValid) {
                    if (instruction.resultType != semantic::PrimitiveType::Bool ||
                        valueType(instruction.operands.front()) != semantic::PrimitiveType::Bool) {
                        diagnostics.report("RS3018", "invalid logical negation types", instruction.sourceSpan);
                    }
                } else if (((instruction.opcode >= Opcode::AddInt &&
                              instruction.opcode <= Opcode::RemainderInt) ||
                             (instruction.opcode >= Opcode::AddLong &&
                              instruction.opcode <= Opcode::RemainderLong) ||
                             (instruction.opcode >= Opcode::AddDouble &&
                              instruction.opcode <= Opcode::DivideDouble)) &&
                           operandCountIsValid) {
                    const auto operandType = valueType(instruction.operands[0]);
                    const bool carrierMatches = instruction.opcode <= Opcode::RemainderInt
                        ? isIntCarrier(operandType)
                        : instruction.opcode <= Opcode::RemainderLong
                            ? isLongCarrier(operandType)
                            : isFloatCarrier(operandType);
                    if (!carrierMatches ||
                        instruction.resultType != operandType ||
                        valueType(instruction.operands[1]) != operandType) {
                        diagnostics.report("RS3019", "invalid numeric arithmetic types", instruction.sourceSpan);
                    }
                } else if (((instruction.opcode >= Opcode::LessInt &&
                              instruction.opcode <= Opcode::GreaterOrEqualInt) ||
                             (instruction.opcode >= Opcode::LessLong &&
                              instruction.opcode <= Opcode::GreaterOrEqualLong) ||
                             (instruction.opcode >= Opcode::LessDouble &&
                              instruction.opcode <= Opcode::GreaterOrEqualDouble)) &&
                           operandCountIsValid) {
                    const auto operandType = valueType(instruction.operands[0]);
                    const bool carrierMatches = instruction.opcode <= Opcode::GreaterOrEqualInt
                        ? isIntCarrier(operandType)
                        : instruction.opcode <= Opcode::GreaterOrEqualLong
                            ? isLongCarrier(operandType)
                            : isFloatCarrier(operandType);
                    if (instruction.resultType != semantic::PrimitiveType::Bool ||
                        !carrierMatches ||
                        valueType(instruction.operands[1]) != operandType) {
                        diagnostics.report("RS3020", "invalid numeric comparison types", instruction.sourceSpan);
                    }
                } else if ((instruction.opcode == Opcode::Equal ||
                            instruction.opcode == Opcode::NotEqual) &&
                           operandCountIsValid) {
                    if (instruction.resultType != semantic::PrimitiveType::Bool ||
                        valueType(instruction.operands[0]) != valueType(instruction.operands[1]) ||
                        valueTypeId(instruction.operands[0]) != valueTypeId(instruction.operands[1])) {
                        diagnostics.report(
                            "RS3021",
                            "invalid equality comparison types",
                            instruction.sourceSpan);
                    }
                }
            }

            const auto& terminator = basicBlock.terminator;
            const auto terminatorUseIndex =
                static_cast<std::int64_t>(basicBlock.instructions.size());
            switch (terminator.kind) {
            case TerminatorKind::None:
                diagnostics.report(
                    "RS3022",
                    "MIR block has no terminator in function '" +
                        function.name + "'",
                    terminator.sourceSpan);
                break;
            case TerminatorKind::Jump:
                verifyEdge(
                    basicBlock.id,
                    terminator.target,
                    terminator.arguments,
                    terminator.sourceSpan);
                break;
            case TerminatorKind::Branch:
                checkUse(
                    terminator.condition,
                    basicBlock.id,
                    terminatorUseIndex,
                    terminator.sourceSpan);
                if (valueType(terminator.condition) !=
                    semantic::PrimitiveType::Bool) {
                    diagnostics.report(
                        "RS3023",
                        "MIR branch condition must be bool",
                        terminator.sourceSpan);
                }
                verifyEdge(
                    basicBlock.id,
                    terminator.target,
                    terminator.arguments,
                    terminator.sourceSpan);
                verifyEdge(
                    basicBlock.id,
                    terminator.falseTarget,
                    terminator.falseArguments,
                    terminator.sourceSpan);
                break;
            case TerminatorKind::ReturnValue:
                checkUse(
                    terminator.value,
                    basicBlock.id,
                    terminatorUseIndex,
                    terminator.sourceSpan);
                if (function.returnType == semantic::PrimitiveType::Void ||
                    !compatibleType(
                        valueType(terminator.value),
                        valueTypeId(terminator.value),
                        function.returnType,
                        function.returnTypeId,
                        types)) {
                    diagnostics.report(
                        "RS3024",
                        "MIR return value type does not match function",
                        terminator.sourceSpan);
                }
                break;
            case TerminatorKind::ReturnVoid:
                if (function.returnType != semantic::PrimitiveType::Void) {
                    diagnostics.report(
                        "RS3025",
                        "non-void MIR function uses ret.void",
                        terminator.sourceSpan);
                }
                break;
            case TerminatorKind::Throw:
                checkUse(
                    terminator.value,
                    basicBlock.id,
                    terminatorUseIndex,
                    terminator.sourceSpan);
                if (valueType(terminator.value) !=
                    semantic::PrimitiveType::Object) {
                    diagnostics.report(
                        "RS3065", "MIR throw requires an object value", {});
                }
                break;
            }
        }
    }

    return diagnostics.items().size() == initialCount;
}

} // namespace realscript::mir
