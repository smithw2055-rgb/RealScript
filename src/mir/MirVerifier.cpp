#include "realscript/mir/Mir.h"

#include <deque>
#include <unordered_map>
#include <unordered_set>

namespace realscript::mir {
namespace {

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
    case Opcode::CheckNotNull:
    case Opcode::ArrayLength:
    case Opcode::LoadField:
    case Opcode::LoadStructField:
    case Opcode::NegateInt:
    case Opcode::NegateLong:
    case Opcode::NegateDouble:
    case Opcode::LogicalNot:
        return 1;
    case Opcode::StoreField:
    case Opcode::StoreStructField:
    case Opcode::LoadElement:
        return 2;
    case Opcode::StoreElement:
        return 3;
    case Opcode::Call:
        return instruction.parameterTypes.size();
    default:
        return 2;
    }
}


bool validTypeIdentity(
    semantic::PrimitiveType type,
    semantic::SymbolId typeId) noexcept {
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
    return instruction.opcode != Opcode::Call ||
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
                    "unreachable MIR block bb" + std::to_string(basicBlock.id),
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
                if (valueType(arguments[i]) != parameters[i].type ||
                    valueTypeId(arguments[i]) != parameters[i].typeId) {
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
                               (valueType(instruction.operands[2]) != instruction.elementType ||
                                valueTypeId(instruction.operands[2]) != instruction.elementTypeId)) {
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
                        foundType->second->kind != expectedKind) {
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
                        if (!load && (valueType(instruction.operands[1]) != field.type ||
                                      valueTypeId(instruction.operands[1]) != fieldTypeId)) {
                            diagnostics.report("RS3047", "MIR field store type mismatch", instruction.sourceSpan);
                        }
                        if (instruction.opcode == Opcode::StoreStructField &&
                            (instruction.resultType != semantic::PrimitiveType::Struct ||
                             instruction.resultTypeId != instruction.typeId)) {
                            diagnostics.report("RS3065", "MIR struct field store must return the updated struct", instruction.sourceSpan);
                        }
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
                    const auto expected = instruction.opcode == Opcode::NegateInt
                        ? semantic::PrimitiveType::Int
                        : instruction.opcode == Opcode::NegateLong
                            ? semantic::PrimitiveType::Long
                            : semantic::PrimitiveType::Double;
                    if (instruction.resultType != expected ||
                        valueType(instruction.operands.front()) != expected) {
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
                    const auto expected = instruction.opcode <= Opcode::RemainderInt
                        ? semantic::PrimitiveType::Int
                        : instruction.opcode <= Opcode::RemainderLong
                            ? semantic::PrimitiveType::Long
                            : semantic::PrimitiveType::Double;
                    if (instruction.resultType != expected ||
                        valueType(instruction.operands[0]) != expected ||
                        valueType(instruction.operands[1]) != expected) {
                        diagnostics.report("RS3019", "invalid numeric arithmetic types", instruction.sourceSpan);
                    }
                } else if (((instruction.opcode >= Opcode::LessInt &&
                              instruction.opcode <= Opcode::GreaterOrEqualInt) ||
                             (instruction.opcode >= Opcode::LessLong &&
                              instruction.opcode <= Opcode::GreaterOrEqualLong) ||
                             (instruction.opcode >= Opcode::LessDouble &&
                              instruction.opcode <= Opcode::GreaterOrEqualDouble)) &&
                           operandCountIsValid) {
                    const auto expected = instruction.opcode <= Opcode::GreaterOrEqualInt
                        ? semantic::PrimitiveType::Int
                        : instruction.opcode <= Opcode::GreaterOrEqualLong
                            ? semantic::PrimitiveType::Long
                            : semantic::PrimitiveType::Double;
                    if (instruction.resultType != semantic::PrimitiveType::Bool ||
                        valueType(instruction.operands[0]) != expected ||
                        valueType(instruction.operands[1]) != expected) {
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
                    "MIR block has no terminator",
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
            }
        }
    }

    return diagnostics.items().size() == initialCount;
}

} // namespace realscript::mir
