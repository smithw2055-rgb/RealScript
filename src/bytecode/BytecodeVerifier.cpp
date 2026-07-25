#include "realscript/bytecode/Bytecode.h"

#include <deque>
#include <unordered_map>
#include <unordered_set>

namespace realscript::bytecode {
namespace {

struct Definition {
    BlockId block = 0;
    std::int64_t instructionIndex = -1;
};

bool validRegisterType(semantic::PrimitiveType type) noexcept {
    return type == semantic::PrimitiveType::Bool ||
        type == semantic::PrimitiveType::Int ||
        type == semantic::PrimitiveType::String ||
        type == semantic::PrimitiveType::Null ||
        type == semantic::PrimitiveType::Object;
}

bool validStorageType(semantic::PrimitiveType type) noexcept {
    return type == semantic::PrimitiveType::Bool ||
        type == semantic::PrimitiveType::Int ||
        type == semantic::PrimitiveType::String ||
        type == semantic::PrimitiveType::Object;
}

bool validReturnType(semantic::PrimitiveType type) noexcept {
    return type == semantic::PrimitiveType::Void ||
        validStorageType(type);
}

bool instructionDefinesValue(
    const Instruction& instruction,
    const Module& module) noexcept {
    if (instruction.opcode == Opcode::StoreLocal) {
        return false;
    }
    if (instruction.opcode == Opcode::Call &&
        instruction.index < module.functionReferences.size()) {
        return module.functionReferences[instruction.index].returnType !=
            semantic::PrimitiveType::Void;
    }
    return true;
}

std::size_t expectedOperandCount(
    const Instruction& instruction,
    const Module& module) noexcept {
    switch (instruction.opcode) {
    case Opcode::LoadParameter:
    case Opcode::ConstantInt:
    case Opcode::ConstantBool:
    case Opcode::ConstantString:
    case Opcode::ConstantNull:
    case Opcode::LoadLocal:
        return 0;
    case Opcode::StoreLocal:
    case Opcode::ConvertNullToString:
    case Opcode::NegateInt:
    case Opcode::LogicalNot:
        return 1;
    case Opcode::Call:
        return instruction.index < module.functionReferences.size()
            ? module.functionReferences[instruction.index].parameterTypes.size()
            : instruction.operands.size();
    default:
        return 2;
    }
}

} // namespace

bool verifyModule(
    const Module& module,
    diagnostics::DiagnosticBag& diagnostics) {
    const auto initialCount = diagnostics.items().size();

    if (module.version.major != 0 || module.version.minor != 1) {
        diagnostics.report(
            "RS5100",
            "unsupported in-memory bytecode version",
            {});
    }
    if (module.name.empty()) {
        diagnostics.report("RS5101", "bytecode module name is empty", {});
    }

    std::unordered_map<semantic::SymbolId, const Function*> internalFunctions;
    for (const auto& function : module.functions) {
        if (function.symbolId == 0 ||
            !internalFunctions.emplace(function.symbolId, &function).second) {
            diagnostics.report(
                "RS5102",
                "duplicate or invalid bytecode function SymbolId",
                {});
        }
    }

    std::unordered_map<semantic::SymbolId, const FunctionReference*> references;
    for (const auto& reference : module.functionReferences) {
        if (reference.symbolId == 0 || reference.name.empty()) {
            diagnostics.report(
                "RS5103",
                "invalid bytecode function reference",
                {});
        }
        const auto found = references.find(reference.symbolId);
        if (found != references.end() &&
            (found->second->name != reference.name ||
             found->second->returnType != reference.returnType ||
             found->second->parameterTypes != reference.parameterTypes)) {
            diagnostics.report(
                "RS5104",
                "conflicting bytecode references share a SymbolId",
                {});
        } else {
            references[reference.symbolId] = &reference;
        }
        if (!validReturnType(reference.returnType)) {
            diagnostics.report(
                "RS5105",
                "bytecode function reference has an invalid return type",
                {});
        }
        for (const auto type : reference.parameterTypes) {
            if (!validStorageType(type)) {
                diagnostics.report(
                    "RS5106",
                    "bytecode function reference has an invalid parameter type",
                    {});
            }
        }

        const auto internal = internalFunctions.find(reference.symbolId);
        if (internal != internalFunctions.end() &&
            (internal->second->returnType != reference.returnType ||
             internal->second->parameterTypes != reference.parameterTypes)) {
            diagnostics.report(
                "RS5107",
                "internal bytecode call reference signature does not match function",
                {});
        }
    }

    for (const auto& function : module.functions) {
        if (function.name.empty()) {
            diagnostics.report("RS5108", "bytecode function name is empty", {});
        }
        if (!validReturnType(function.returnType)) {
            diagnostics.report(
                "RS5109",
                "bytecode function has an invalid return type",
                {});
        }
        for (const auto type : function.parameterTypes) {
            if (!validStorageType(type)) {
                diagnostics.report(
                    "RS5110",
                    "bytecode function has an invalid parameter type",
                    {});
            }
        }
        for (const auto type : function.localTypes) {
            if (!validStorageType(type)) {
                diagnostics.report(
                    "RS5111",
                    "bytecode function has an invalid local type",
                    {});
            }
        }
        for (const auto type : function.registerTypes) {
            if (!validRegisterType(type)) {
                diagnostics.report(
                    "RS5112",
                    "bytecode function has an invalid or unassigned register type",
                    {});
            }
        }
        if (function.blocks.empty()) {
            diagnostics.report(
                "RS5113",
                "bytecode function has no entry block",
                {});
            continue;
        }

        std::unordered_map<BlockId, const BasicBlock*> blocks;
        std::unordered_map<Register, Definition> definitions;
        for (const auto& block : function.blocks) {
            if (!blocks.emplace(block.id, &block).second) {
                diagnostics.report("RS5114", "duplicate bytecode block id", {});
            }
            for (const auto& parameter : block.parameters) {
                if (parameter.target >= function.registerTypes.size() ||
                    parameter.type != function.registerTypes[parameter.target]) {
                    diagnostics.report(
                        "RS5115",
                        "bytecode block parameter type does not match register table",
                        {});
                }
                if (!definitions.emplace(
                        parameter.target,
                        Definition{block.id, -1}).second) {
                    diagnostics.report(
                        "RS5116",
                        "bytecode register has multiple definitions",
                        {});
                }
            }
            for (std::size_t index = 0; index < block.instructions.size(); ++index) {
                const auto& instruction = block.instructions[index];
                const bool defines = instructionDefinesValue(instruction, module);
                if (defines) {
                    if (instruction.result >= function.registerTypes.size() ||
                        !definitions.emplace(
                            instruction.result,
                            Definition{
                                block.id,
                                static_cast<std::int64_t>(index),
                            }).second) {
                        diagnostics.report(
                            "RS5116",
                            "bytecode register has multiple or invalid definitions",
                            {});
                    }
                } else if (instruction.result != InvalidRegister) {
                    diagnostics.report(
                        "RS5117",
                        "non-value bytecode instruction defines a register",
                        {});
                }
            }
        }

        for (Register value = 0; value < function.registerTypes.size(); ++value) {
            if (definitions.find(value) == definitions.end()) {
                diagnostics.report(
                    "RS5150",
                    "bytecode register r" + std::to_string(value) +
                        " has no definition",
                    {});
            }
        }

        if (blocks.find(0) == blocks.end()) {
            diagnostics.report(
                "RS5118",
                "bytecode function entry block must be bb0",
                {});
        }

        std::unordered_map<BlockId, std::vector<BlockId>> successors;
        std::unordered_map<BlockId, std::vector<BlockId>> predecessors;
        const auto addEdge = [&](BlockId from, BlockId to) {
            if (blocks.find(to) != blocks.end()) {
                successors[from].push_back(to);
                predecessors[to].push_back(from);
            }
        };
        for (const auto& block : function.blocks) {
            if (block.terminator.kind == TerminatorKind::Jump) {
                addEdge(block.id, block.terminator.target);
            } else if (block.terminator.kind == TerminatorKind::Branch) {
                addEdge(block.id, block.terminator.target);
                addEdge(block.id, block.terminator.falseTarget);
            }
        }

        std::unordered_set<BlockId> reachable;
        std::deque<BlockId> queue;
        if (blocks.find(0) != blocks.end()) {
            reachable.insert(0);
            queue.push_back(0);
        }
        while (!queue.empty()) {
            const auto current = queue.front();
            queue.pop_front();
            for (const auto next : successors[current]) {
                if (reachable.insert(next).second) {
                    queue.push_back(next);
                }
            }
        }
        for (const auto& block : function.blocks) {
            if (reachable.find(block.id) == reachable.end()) {
                diagnostics.report(
                    "RS5119",
                    "unreachable bytecode block bb" + std::to_string(block.id),
                    {});
            }
        }

        std::unordered_map<BlockId, std::unordered_set<BlockId>> dominators;
        for (const auto& block : function.blocks) {
            if (block.id == 0) {
                dominators[block.id] = {0};
            } else {
                for (const auto& candidate : function.blocks) {
                    dominators[block.id].insert(candidate.id);
                }
            }
        }
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& block : function.blocks) {
                if (block.id == 0 || reachable.find(block.id) == reachable.end()) {
                    continue;
                }
                std::unordered_set<BlockId> next;
                const auto& incoming = predecessors[block.id];
                if (!incoming.empty()) {
                    next = dominators[incoming.front()];
                    for (std::size_t index = 1; index < incoming.size(); ++index) {
                        std::unordered_set<BlockId> intersection;
                        for (const auto candidate : next) {
                            if (dominators[incoming[index]].find(candidate) !=
                                dominators[incoming[index]].end()) {
                                intersection.insert(candidate);
                            }
                        }
                        next = std::move(intersection);
                    }
                }
                next.insert(block.id);
                if (next != dominators[block.id]) {
                    dominators[block.id] = std::move(next);
                    changed = true;
                }
            }
        }

        const auto registerType = [&](Register value) {
            return value < function.registerTypes.size()
                ? function.registerTypes[value]
                : semantic::PrimitiveType::Error;
        };
        const auto checkUse = [&](
            Register value,
            BlockId useBlock,
            std::int64_t useIndex) {
            if (value >= function.registerTypes.size()) {
                diagnostics.report(
                    "RS5120",
                    "bytecode uses register outside the register table",
                    {});
                return;
            }
            const auto definition = definitions.find(value);
            if (definition == definitions.end()) {
                diagnostics.report(
                    "RS5121",
                    "bytecode uses undefined register r" + std::to_string(value),
                    {});
                return;
            }
            if (definition->second.block == useBlock) {
                if (definition->second.instructionIndex >= useIndex &&
                    definition->second.instructionIndex >= 0) {
                    diagnostics.report(
                        "RS5122",
                        "bytecode register is used before its definition",
                        {});
                }
            } else if (dominators[useBlock].find(definition->second.block) ==
                       dominators[useBlock].end()) {
                diagnostics.report(
                    "RS5123",
                    "bytecode register definition does not dominate its use",
                    {});
            }
        };

        const auto verifyEdge = [&](
            BlockId from,
            BlockId target,
            const std::vector<Register>& arguments,
            std::int64_t useIndex) {
            const auto targetBlock = blocks.find(target);
            if (targetBlock == blocks.end()) {
                diagnostics.report(
                    "RS5124",
                    "bytecode branch targets missing block bb" +
                        std::to_string(target),
                    {});
                return;
            }
            if (arguments.size() != targetBlock->second->parameters.size()) {
                diagnostics.report(
                    "RS5125",
                    "bytecode branch argument count does not match block parameters",
                    {});
                return;
            }
            for (std::size_t index = 0; index < arguments.size(); ++index) {
                checkUse(arguments[index], from, useIndex);
                if (registerType(arguments[index]) !=
                    targetBlock->second->parameters[index].type) {
                    diagnostics.report(
                        "RS5126",
                        "bytecode branch argument type does not match block parameter",
                        {});
                }
            }
        };

        for (const auto& block : function.blocks) {
            for (std::size_t index = 0; index < block.instructions.size(); ++index) {
                const auto& instruction = block.instructions[index];
                if (instruction.operands.size() !=
                    expectedOperandCount(instruction, module)) {
                    diagnostics.report(
                        "RS5127",
                        "bytecode instruction has an invalid operand count",
                        {});
                }
                for (const auto operand : instruction.operands) {
                    checkUse(
                        operand,
                        block.id,
                        static_cast<std::int64_t>(index));
                }

                const auto resultType = registerType(instruction.result);
                switch (instruction.opcode) {
                case Opcode::LoadParameter:
                    if (instruction.index >= function.parameterTypes.size() ||
                        resultType != function.parameterTypes[instruction.index]) {
                        diagnostics.report(
                            "RS5128",
                            "invalid bytecode parameter load",
                            {});
                    }
                    break;
                case Opcode::ConstantInt:
                    if (resultType != semantic::PrimitiveType::Int) {
                        diagnostics.report("RS5129", "const.i32 must produce int", {});
                    }
                    break;
                case Opcode::ConstantBool:
                    if (resultType != semantic::PrimitiveType::Bool) {
                        diagnostics.report("RS5130", "const.bool must produce bool", {});
                    }
                    break;
                case Opcode::ConstantString:
                    if (resultType != semantic::PrimitiveType::String) {
                        diagnostics.report("RS5131", "const.string must produce string", {});
                    }
                    break;
                case Opcode::ConstantNull:
                    if (resultType != semantic::PrimitiveType::Null) {
                        diagnostics.report("RS5132", "const.null must produce null", {});
                    }
                    break;
                case Opcode::LoadLocal:
                    if (instruction.index >= function.localTypes.size() ||
                        resultType != function.localTypes[instruction.index]) {
                        diagnostics.report("RS5133", "invalid bytecode local load", {});
                    }
                    break;
                case Opcode::StoreLocal:
                    if (instruction.index >= function.localTypes.size() ||
                        instruction.operands.size() != 1 ||
                        registerType(instruction.operands.front()) !=
                            function.localTypes[instruction.index]) {
                        diagnostics.report("RS5134", "invalid bytecode local store", {});
                    }
                    break;
                case Opcode::ConvertNullToString:
                    if (instruction.operands.size() != 1 ||
                        registerType(instruction.operands.front()) !=
                            semantic::PrimitiveType::Null ||
                        resultType != semantic::PrimitiveType::String) {
                        diagnostics.report(
                            "RS5135",
                            "invalid null-to-string bytecode conversion",
                            {});
                    }
                    break;
                case Opcode::Call:
                    if (instruction.index >= module.functionReferences.size()) {
                        diagnostics.report(
                            "RS5136",
                            "bytecode call references an invalid function index",
                            {});
                        break;
                    }
                    {
                        const auto& reference =
                            module.functionReferences[instruction.index];
                        if (instruction.operands.size() !=
                            reference.parameterTypes.size()) {
                            diagnostics.report(
                                "RS5137",
                                "bytecode call argument count does not match reference",
                                {});
                        }
                        const auto count = std::min(
                            instruction.operands.size(),
                            reference.parameterTypes.size());
                        for (std::size_t argument = 0; argument < count; ++argument) {
                            if (registerType(instruction.operands[argument]) !=
                                reference.parameterTypes[argument]) {
                                diagnostics.report(
                                    "RS5138",
                                    "bytecode call argument type does not match reference",
                                    {});
                            }
                        }
                        if (reference.returnType == semantic::PrimitiveType::Void) {
                            if (instruction.result != InvalidRegister) {
                                diagnostics.report(
                                    "RS5139",
                                    "void bytecode call defines a result register",
                                    {});
                            }
                        } else if (instruction.result == InvalidRegister ||
                                   resultType != reference.returnType) {
                            diagnostics.report(
                                "RS5140",
                                "bytecode call result does not match reference",
                                {});
                        }
                    }
                    break;
                case Opcode::NegateInt:
                    if (instruction.operands.size() != 1 ||
                        registerType(instruction.operands.front()) !=
                            semantic::PrimitiveType::Int ||
                        resultType != semantic::PrimitiveType::Int) {
                        diagnostics.report("RS5141", "invalid integer negation", {});
                    }
                    break;
                case Opcode::LogicalNot:
                    if (instruction.operands.size() != 1 ||
                        registerType(instruction.operands.front()) !=
                            semantic::PrimitiveType::Bool ||
                        resultType != semantic::PrimitiveType::Bool) {
                        diagnostics.report("RS5142", "invalid logical negation", {});
                    }
                    break;
                case Opcode::AddInt:
                case Opcode::SubtractInt:
                case Opcode::MultiplyInt:
                case Opcode::DivideInt:
                case Opcode::RemainderInt:
                    if (instruction.operands.size() != 2 ||
                        registerType(instruction.operands[0]) !=
                            semantic::PrimitiveType::Int ||
                        registerType(instruction.operands[1]) !=
                            semantic::PrimitiveType::Int ||
                        resultType != semantic::PrimitiveType::Int) {
                        diagnostics.report("RS5143", "invalid integer arithmetic", {});
                    }
                    break;
                case Opcode::LessInt:
                case Opcode::LessOrEqualInt:
                case Opcode::GreaterInt:
                case Opcode::GreaterOrEqualInt:
                    if (instruction.operands.size() != 2 ||
                        registerType(instruction.operands[0]) !=
                            semantic::PrimitiveType::Int ||
                        registerType(instruction.operands[1]) !=
                            semantic::PrimitiveType::Int ||
                        resultType != semantic::PrimitiveType::Bool) {
                        diagnostics.report("RS5144", "invalid integer comparison", {});
                    }
                    break;
                case Opcode::Equal:
                case Opcode::NotEqual:
                    if (instruction.operands.size() != 2 ||
                        registerType(instruction.operands[0]) !=
                            registerType(instruction.operands[1]) ||
                        resultType != semantic::PrimitiveType::Bool) {
                        diagnostics.report("RS5145", "invalid equality comparison", {});
                    }
                    break;
                }
            }

            const auto useIndex =
                static_cast<std::int64_t>(block.instructions.size());
            switch (block.terminator.kind) {
            case TerminatorKind::None:
                diagnostics.report("RS5146", "bytecode block has no terminator", {});
                break;
            case TerminatorKind::Jump:
                verifyEdge(
                    block.id,
                    block.terminator.target,
                    block.terminator.arguments,
                    useIndex);
                break;
            case TerminatorKind::Branch:
                checkUse(block.terminator.condition, block.id, useIndex);
                if (registerType(block.terminator.condition) !=
                    semantic::PrimitiveType::Bool) {
                    diagnostics.report(
                        "RS5147",
                        "bytecode branch condition must be bool",
                        {});
                }
                verifyEdge(
                    block.id,
                    block.terminator.target,
                    block.terminator.arguments,
                    useIndex);
                verifyEdge(
                    block.id,
                    block.terminator.falseTarget,
                    block.terminator.falseArguments,
                    useIndex);
                break;
            case TerminatorKind::ReturnValue:
                checkUse(block.terminator.value, block.id, useIndex);
                if (function.returnType == semantic::PrimitiveType::Void ||
                    registerType(block.terminator.value) != function.returnType) {
                    diagnostics.report(
                        "RS5148",
                        "bytecode return value does not match function",
                        {});
                }
                break;
            case TerminatorKind::ReturnVoid:
                if (function.returnType != semantic::PrimitiveType::Void) {
                    diagnostics.report(
                        "RS5149",
                        "non-void bytecode function uses ret.void",
                        {});
                }
                break;
            }
        }
    }

    return diagnostics.items().size() == initialCount;
}

} // namespace realscript::bytecode
