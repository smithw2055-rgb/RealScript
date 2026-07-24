#include "realscript/mir/Mir.h"

#include <algorithm>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <deque>

namespace realscript::mir {
namespace {
bool isValueOpcode(Opcode opcode) noexcept {
    return opcode != Opcode::StoreLocal;
}

std::size_t expectedOperandCount(Opcode opcode) noexcept {
    switch (opcode) {
    case Opcode::Parameter:
    case Opcode::ConstantInt:
    case Opcode::ConstantBool:
    case Opcode::ConstantString:
    case Opcode::ConstantNull:
    case Opcode::LoadLocal:
        return 0;
    case Opcode::StoreLocal:
    case Opcode::NegateInt:
    case Opcode::LogicalNot:
        return 1;
    default:
        return 2;
    }
}

struct Definition {
    BlockId block = 0;
    std::int64_t instructionIndex = -1;
    semantic::PrimitiveType type = semantic::PrimitiveType::Error;
};

} // namespace

bool verifyModule(const Module& module, diagnostics::DiagnosticBag& diagnostics) {
    const auto initialCount = diagnostics.items().size();

    for (const auto& function : module.functions) {
        if (function.blocks.empty()) {
            diagnostics.report("RS3000", "MIR function '" + function.name + "' has no entry block", {});
            continue;
        }

        std::unordered_map<BlockId, const BasicBlock*> blocks;
        for (const auto& basicBlock : function.blocks) {
            if (!blocks.emplace(basicBlock.id, &basicBlock).second) {
                diagnostics.report("RS3001", "duplicate MIR block id", basicBlock.terminator.sourceSpan);
            }
        }
        if (blocks.find(0) == blocks.end()) {
            diagnostics.report("RS3002", "MIR function entry block must be bb0", {});
        }

        std::unordered_map<ValueId, Definition> definitions;
        for (const auto& basicBlock : function.blocks) {
            for (const auto& parameter : basicBlock.parameters) {
                if (parameter.value < 0 ||
                    !definitions.emplace(parameter.value, Definition{basicBlock.id, -1, parameter.type}).second) {
                    diagnostics.report("RS3003", "duplicate or invalid MIR value id", {});
                }
            }
            for (std::size_t index = 0; index < basicBlock.instructions.size(); ++index) {
                const auto& instruction = basicBlock.instructions[index];
                if (isValueOpcode(instruction.opcode)) {
                    if (instruction.result < 0 ||
                        !definitions.emplace(
                            instruction.result,
                            Definition{basicBlock.id, static_cast<std::int64_t>(index), instruction.resultType})
                             .second) {
                        diagnostics.report("RS3003", "duplicate or invalid MIR value id", instruction.sourceSpan);
                    }
                } else if (instruction.result >= 0) {
                    diagnostics.report("RS3004", "non-value MIR instruction defines a result", instruction.sourceSpan);
                }
            }
        }

        auto valueType = [&](ValueId value) {
            const auto found = definitions.find(value);
            return found == definitions.end() ? semantic::PrimitiveType::Error : found->second.type;
        };

        std::unordered_map<BlockId, std::vector<BlockId>> predecessors;
        std::unordered_map<BlockId, std::vector<BlockId>> successors;
        auto addEdge = [&](BlockId from, BlockId to) {
            if (blocks.find(to) == blocks.end()) {
                return;
            }
            successors[from].push_back(to);
            predecessors[to].push_back(from);
        };

        for (const auto& basicBlock : function.blocks) {
            const auto& terminator = basicBlock.terminator;
            if (terminator.kind == TerminatorKind::Jump) {
                addEdge(basicBlock.id, terminator.target);
            } else if (terminator.kind == TerminatorKind::Branch) {
                addEdge(basicBlock.id, terminator.target);
                addEdge(basicBlock.id, terminator.falseTarget);
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
                diagnostics.report("RS3005", "unreachable MIR block bb" + std::to_string(basicBlock.id), {});
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
                if (basicBlock.id == 0 || reachable.find(basicBlock.id) == reachable.end()) {
                    continue;
                }
                std::unordered_set<BlockId> next;
                const auto& preds = predecessors[basicBlock.id];
                if (!preds.empty()) {
                    next = dominators[preds.front()];
                    for (std::size_t i = 1; i < preds.size(); ++i) {
                        std::unordered_set<BlockId> intersection;
                        for (const auto value : next) {
                            if (dominators[preds[i]].find(value) != dominators[preds[i]].end()) {
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

        auto checkUse = [&](
            ValueId value,
            BlockId useBlock,
            std::int64_t useInstructionIndex,
            text::TextSpan span) {
            const auto found = definitions.find(value);
            if (found == definitions.end()) {
                diagnostics.report("RS3006", "MIR uses undefined value %" + std::to_string(value), span);
                return;
            }
            const auto& definition = found->second;
            if (definition.block == useBlock) {
                if (definition.instructionIndex >= useInstructionIndex && definition.instructionIndex >= 0) {
                    diagnostics.report("RS3007", "MIR value is used before its definition", span);
                }
            } else if (dominators[useBlock].find(definition.block) == dominators[useBlock].end()) {
                diagnostics.report("RS3008", "MIR value definition does not dominate its use", span);
            }
        };

        auto verifyEdge = [&](
            BlockId from,
            BlockId target,
            const std::vector<ValueId>& arguments,
            text::TextSpan span) {
            const auto targetIt = blocks.find(target);
            if (targetIt == blocks.end()) {
                diagnostics.report("RS3009", "MIR branch targets missing block bb" + std::to_string(target), span);
                return;
            }
            const auto& parameters = targetIt->second->parameters;
            if (arguments.size() != parameters.size()) {
                diagnostics.report("RS3010", "MIR branch argument count does not match target block", span);
                return;
            }
            for (std::size_t i = 0; i < arguments.size(); ++i) {
                checkUse(arguments[i], from, static_cast<std::int64_t>(blocks[from]->instructions.size()), span);
                if (valueType(arguments[i]) != parameters[i].type) {
                    diagnostics.report("RS3011", "MIR branch argument type does not match block parameter", span);
                }
            }
        };

        for (const auto& basicBlock : function.blocks) {
            for (std::size_t index = 0; index < basicBlock.instructions.size(); ++index) {
                const auto& instruction = basicBlock.instructions[index];
                const auto operandCountIsValid =
                    instruction.operands.size() == expectedOperandCount(instruction.opcode);
                if (!operandCountIsValid) {
                    diagnostics.report("RS3012", "MIR instruction has an invalid operand count", instruction.sourceSpan);
                }
                for (const auto operand : instruction.operands) {
                    checkUse(operand, basicBlock.id, static_cast<std::int64_t>(index), instruction.sourceSpan);
                }

                if (instruction.opcode == Opcode::Parameter) {
                    const auto parameterIndex = static_cast<std::size_t>(instruction.integerImmediate);
                    if (instruction.integerImmediate < 0 || parameterIndex >= function.parameterTypes.size() ||
                        instruction.resultType != function.parameterTypes[parameterIndex]) {
                        diagnostics.report("RS3013", "invalid MIR parameter instruction", instruction.sourceSpan);
                    }
                } else if (instruction.opcode == Opcode::ConstantInt &&
                           instruction.resultType != semantic::PrimitiveType::Int) {
                    diagnostics.report("RS3026", "const.i32 must produce int", instruction.sourceSpan);
                } else if (instruction.opcode == Opcode::ConstantBool &&
                           instruction.resultType != semantic::PrimitiveType::Bool) {
                    diagnostics.report("RS3027", "const.bool must produce bool", instruction.sourceSpan);
                } else if (instruction.opcode == Opcode::ConstantString &&
                           instruction.resultType != semantic::PrimitiveType::String) {
                    diagnostics.report("RS3028", "const.string must produce string", instruction.sourceSpan);
                } else if (instruction.opcode == Opcode::ConstantNull &&
                           instruction.resultType != semantic::PrimitiveType::Null) {
                    diagnostics.report("RS3029", "const.null must produce null", instruction.sourceSpan);
                } else if (instruction.opcode == Opcode::LoadLocal ||
                           instruction.opcode == Opcode::StoreLocal) {
                    if (instruction.localIndex >= function.localTypes.size()) {
                        diagnostics.report("RS3014", "MIR local index is outside the function local table", instruction.sourceSpan);
                    } else if (instruction.opcode == Opcode::LoadLocal &&
                               instruction.resultType != function.localTypes[instruction.localIndex]) {
                        diagnostics.report("RS3015", "MIR local load type does not match local table", instruction.sourceSpan);
                    } else if (instruction.opcode == Opcode::StoreLocal && operandCountIsValid &&
                               valueType(instruction.operands.front()) != function.localTypes[instruction.localIndex]) {
                        diagnostics.report("RS3016", "MIR local store type does not match local table", instruction.sourceSpan);
                    }
                } else if (instruction.opcode == Opcode::NegateInt && operandCountIsValid) {
                    if (instruction.resultType != semantic::PrimitiveType::Int ||
                        valueType(instruction.operands.front()) != semantic::PrimitiveType::Int) {
                        diagnostics.report("RS3017", "invalid integer negation types", instruction.sourceSpan);
                    }
                } else if (instruction.opcode == Opcode::LogicalNot && operandCountIsValid) {
                    if (instruction.resultType != semantic::PrimitiveType::Bool ||
                        valueType(instruction.operands.front()) != semantic::PrimitiveType::Bool) {
                        diagnostics.report("RS3018", "invalid logical negation types", instruction.sourceSpan);
                    }
                } else if ((instruction.opcode == Opcode::AddInt ||
                            instruction.opcode == Opcode::SubtractInt ||
                            instruction.opcode == Opcode::MultiplyInt ||
                            instruction.opcode == Opcode::DivideInt ||
                            instruction.opcode == Opcode::RemainderInt) && operandCountIsValid) {
                    if (instruction.resultType != semantic::PrimitiveType::Int ||
                        valueType(instruction.operands[0]) != semantic::PrimitiveType::Int ||
                        valueType(instruction.operands[1]) != semantic::PrimitiveType::Int) {
                        diagnostics.report("RS3019", "invalid integer arithmetic types", instruction.sourceSpan);
                    }
                } else if ((instruction.opcode == Opcode::LessInt ||
                            instruction.opcode == Opcode::LessOrEqualInt ||
                            instruction.opcode == Opcode::GreaterInt ||
                            instruction.opcode == Opcode::GreaterOrEqualInt) && operandCountIsValid) {
                    if (instruction.resultType != semantic::PrimitiveType::Bool ||
                        valueType(instruction.operands[0]) != semantic::PrimitiveType::Int ||
                        valueType(instruction.operands[1]) != semantic::PrimitiveType::Int) {
                        diagnostics.report("RS3020", "invalid integer comparison types", instruction.sourceSpan);
                    }
                } else if ((instruction.opcode == Opcode::Equal || instruction.opcode == Opcode::NotEqual) &&
                           operandCountIsValid) {
                    if (instruction.resultType != semantic::PrimitiveType::Bool ||
                        valueType(instruction.operands[0]) != valueType(instruction.operands[1])) {
                        diagnostics.report("RS3021", "invalid equality comparison types", instruction.sourceSpan);
                    }
                }
            }

            const auto& terminator = basicBlock.terminator;
            const auto terminatorUseIndex = static_cast<std::int64_t>(basicBlock.instructions.size());
            switch (terminator.kind) {
            case TerminatorKind::None:
                diagnostics.report("RS3022", "MIR block has no terminator", terminator.sourceSpan);
                break;
            case TerminatorKind::Jump:
                verifyEdge(basicBlock.id, terminator.target, terminator.arguments, terminator.sourceSpan);
                break;
            case TerminatorKind::Branch:
                checkUse(terminator.condition, basicBlock.id, terminatorUseIndex, terminator.sourceSpan);
                if (valueType(terminator.condition) != semantic::PrimitiveType::Bool) {
                    diagnostics.report("RS3023", "MIR branch condition must be bool", terminator.sourceSpan);
                }
                verifyEdge(basicBlock.id, terminator.target, terminator.arguments, terminator.sourceSpan);
                verifyEdge(
                    basicBlock.id,
                    terminator.falseTarget,
                    terminator.falseArguments,
                    terminator.sourceSpan);
                break;
            case TerminatorKind::ReturnValue:
                checkUse(terminator.value, basicBlock.id, terminatorUseIndex, terminator.sourceSpan);
                if (function.returnType == semantic::PrimitiveType::Void ||
                    valueType(terminator.value) != function.returnType) {
                    diagnostics.report("RS3024", "MIR return value type does not match function", terminator.sourceSpan);
                }
                break;
            case TerminatorKind::ReturnVoid:
                if (function.returnType != semantic::PrimitiveType::Void) {
                    diagnostics.report("RS3025", "non-void MIR function uses ret.void", terminator.sourceSpan);
                }
                break;
            }
        }
    }

    return diagnostics.items().size() == initialCount;
}


} // namespace realscript::mir
