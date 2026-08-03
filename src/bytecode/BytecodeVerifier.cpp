#include "realscript/bytecode/Bytecode.h"

#include <algorithm>
#include <deque>
#include <unordered_map>
#include <unordered_set>

namespace realscript::bytecode {
namespace {

struct Definition {
    BlockId block = 0;
    std::int64_t instructionIndex = -1;
};


bool validSourceFile(const debug::SourceFileInfo& source) noexcept {
    if (source.path.empty() || source.lineStarts.empty() ||
        source.lineStarts.front() != 0) return false;
    for (std::size_t index = 1; index < source.lineStarts.size(); ++index) {
        if (source.lineStarts[index] <= source.lineStarts[index - 1]) return false;
    }
    return true;
}

bool validSourceRange(
    const debug::SourceRange& range,
    const std::unordered_map<
        debug::SourceFileId,
        const debug::SourceFileInfo*>& sources) noexcept {
    const auto found = sources.find(range.fileId);
    if (found == sources.end()) return false;
    const auto& source = *found->second;
    if (range.start.line >= source.lineStarts.size() ||
        range.end.line >= source.lineStarts.size()) return false;
    if (range.start.line > range.end.line ||
        (range.start.line == range.end.line &&
         range.start.column > range.end.column)) return false;
    return debug::offsetAt(source, range.start) == range.span.start &&
        debug::offsetAt(source, range.end) == range.span.end();
}

bool validRegisterType(semantic::PrimitiveType type) noexcept {
    return type == semantic::PrimitiveType::Bool ||
        type == semantic::PrimitiveType::Int ||
        type == semantic::PrimitiveType::Long ||
        type == semantic::PrimitiveType::Double ||
        type == semantic::PrimitiveType::String ||
        type == semantic::PrimitiveType::Object ||
        type == semantic::PrimitiveType::Struct ||
        type == semantic::PrimitiveType::Enum ||
        type == semantic::PrimitiveType::Array ||
        type == semantic::PrimitiveType::Handle ||
        type == semantic::PrimitiveType::Null;
}

bool validStorageType(semantic::PrimitiveType type) noexcept {
    return type == semantic::PrimitiveType::Bool ||
        type == semantic::PrimitiveType::Int ||
        type == semantic::PrimitiveType::Long ||
        type == semantic::PrimitiveType::Double ||
        type == semantic::PrimitiveType::String ||
        type == semantic::PrimitiveType::Object ||
        type == semantic::PrimitiveType::Struct ||
        type == semantic::PrimitiveType::Enum ||
        type == semantic::PrimitiveType::Array ||
        type == semantic::PrimitiveType::Handle;
}

bool validReturnType(semantic::PrimitiveType type) noexcept {
    return type == semantic::PrimitiveType::Void ||
        validStorageType(type);
}

bool validTypeIdentity(
    semantic::PrimitiveType type,
    semantic::SymbolId typeId) noexcept {
    return (type == semantic::PrimitiveType::Object ||
            type == semantic::PrimitiveType::Struct ||
            type == semantic::PrimitiveType::Enum ||
            type == semantic::PrimitiveType::Array)
        ? typeId != 0
        : typeId == 0;
}

semantic::TypeKind expectedTypeKind(semantic::PrimitiveType type) noexcept {
    switch (type) {
    case semantic::PrimitiveType::Object: return semantic::TypeKind::Class;
    case semantic::PrimitiveType::Struct: return semantic::TypeKind::Struct;
    case semantic::PrimitiveType::Enum: return semantic::TypeKind::Enum;
    default: return semantic::TypeKind::Class;
    }
}

bool validDescriptorIdentity(
    semantic::PrimitiveType type,
    semantic::SymbolId typeId,
    const std::unordered_map<
        semantic::SymbolId,
        const semantic::TypeSymbol*>& descriptors) noexcept {
    if (!validTypeIdentity(type, typeId)) return false;
    if (type != semantic::PrimitiveType::Object &&
        type != semantic::PrimitiveType::Struct &&
        type != semantic::PrimitiveType::Enum) {
        return true;
    }
    const auto found = descriptors.find(typeId);
    return found != descriptors.end() &&
        found->second->kind == expectedTypeKind(type);
}

semantic::SymbolId fieldTypeId(const semantic::FieldSymbol& field) noexcept {
    return semantic::isExactType(field.type)
        ? semantic::stableTypeId(field.typeName)
        : semantic::SymbolId{0};
}

semantic::SymbolId arrayTypeId(
    semantic::PrimitiveType elementType,
    semantic::SymbolId elementTypeId,
    const std::unordered_map<
        semantic::SymbolId,
        const semantic::TypeSymbol*>& descriptors) {
    std::string exactElementName;
    if (elementType == semantic::PrimitiveType::Object ||
        elementType == semantic::PrimitiveType::Struct ||
        elementType == semantic::PrimitiveType::Enum) {
        const auto found = descriptors.find(elementTypeId);
        if (found == descriptors.end() ||
            found->second->kind != expectedTypeKind(elementType)) {
            return 0;
        }
        exactElementName = semantic::canonicalTypeName(*found->second);
    }
    const auto name = semantic::arrayTypeName(elementType, exactElementName);
    return semantic::stableTypeId(name);
}

semantic::SymbolId typeIdAt(
    const std::vector<semantic::SymbolId>& typeIds,
    std::size_t index) noexcept {
    return index < typeIds.size() ? typeIds[index] : 0;
}

bool instructionDefinesValue(
    const Instruction& instruction,
    const Module& module) noexcept {
    if (instruction.opcode == Opcode::StoreLocal ||
        instruction.opcode == Opcode::StoreField ||
        instruction.opcode == Opcode::StoreElement) {
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
    case Opcode::ConstantDouble:
    case Opcode::ConstantBool:
    case Opcode::ConstantString:
    case Opcode::ConstantNull:
    case Opcode::LoadLocal:
    case Opcode::NewObject:
    case Opcode::NewStruct:
        return 0;
    case Opcode::StoreLocal:
    case Opcode::ConvertNullToString:
    case Opcode::ConvertNullToObject:
    case Opcode::ConvertNullToArray:
    case Opcode::ConvertIntToLong:
    case Opcode::ConvertIntToDouble:
    case Opcode::ConvertLongToDouble:
    case Opcode::NewArray:
    case Opcode::CheckNotNull:
    case Opcode::ArrayLength:
    case Opcode::LoadField:
    case Opcode::LoadStructField:
    case Opcode::NegateInt:
    case Opcode::NegateLong:
    case Opcode::NegateDouble:
    case Opcode::LogicalNot:
        return 1;
    case Opcode::LoadElement:
    case Opcode::StoreField:
    case Opcode::StoreStructField:
        return 2;
    case Opcode::StoreElement:
        return 3;
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

    if (module.version.major != 0 || module.version.minor != 6) {
        diagnostics.report(
            "RS5100",
            "unsupported in-memory bytecode version",
            {});
    }
    if (module.name.empty()) {
        diagnostics.report("RS5101", "bytecode module name is empty", {});
    }

    std::unordered_map<debug::SourceFileId, const debug::SourceFileInfo*> sourceFiles;
    std::unordered_set<std::string> sourcePaths;
    for (const auto& source : module.sourceFiles) {
        if (!validSourceFile(source) ||
            !sourcePaths.insert(source.path).second ||
            !sourceFiles.emplace(source.id, &source).second) {
            diagnostics.report(
                "RS5160",
                "duplicate or invalid debug source file",
                {});
        }
    }

    std::unordered_map<semantic::SymbolId, const semantic::TypeSymbol*> typeDescriptors;
    for (const auto& type : module.types) {
        if (type.id == 0 || type.name.empty() ||
            type.id != semantic::stableTypeId(semantic::canonicalTypeName(type)) ||
            !typeDescriptors.emplace(type.id, &type).second) {
            diagnostics.report(
                "RS5150",
                "duplicate or invalid bytecode type descriptor",
                {});
        }
    }

    for (const auto& type : module.types) {
        std::unordered_set<std::string> fieldNames;
        std::unordered_set<std::string> enumMemberNames;
        if (type.kind == semantic::TypeKind::Enum) {
            if (!type.fields.empty()) {
                diagnostics.report(
                    "RS5151",
                    "enum bytecode descriptor cannot contain fields",
                    {});
            }
            for (const auto& member : type.enumMembers) {
                if (member.name.empty() || !enumMemberNames.insert(member.name).second) {
                    diagnostics.report(
                        "RS5151",
                        "invalid or duplicate bytecode enum member",
                        {});
                }
            }
        } else if (!type.enumMembers.empty()) {
            diagnostics.report(
                "RS5151",
                "non-enum bytecode descriptor cannot contain enum members",
                {});
        }

        for (std::size_t fieldIndex = 0; fieldIndex < type.fields.size(); ++fieldIndex) {
            const auto& field = type.fields[fieldIndex];
            bool exactTypeValid = true;
            if (field.type == semantic::PrimitiveType::Object ||
                field.type == semantic::PrimitiveType::Struct ||
                field.type == semantic::PrimitiveType::Enum) {
                const auto fieldTypeId = semantic::stableTypeId(field.typeName);
                exactTypeValid = validDescriptorIdentity(
                    field.type,
                    fieldTypeId,
                    typeDescriptors);
            } else if (field.type == semantic::PrimitiveType::Array) {
                semantic::PrimitiveType elementType = semantic::PrimitiveType::Error;
                std::string elementTypeName;
                exactTypeValid = semantic::decodeArrayTypeName(
                    field.typeName,
                    elementType,
                    elementTypeName);
                if (exactTypeValid && !elementTypeName.empty()) {
                    exactTypeValid = typeDescriptors.find(
                        semantic::stableTypeId(elementTypeName)) !=
                        typeDescriptors.end();
                }
            }
            if (field.index != fieldIndex || field.name.empty() ||
                !fieldNames.insert(field.name).second ||
                !validStorageType(field.type) ||
                ((field.type == semantic::PrimitiveType::Object ||
                  field.type == semantic::PrimitiveType::Struct ||
                  field.type == semantic::PrimitiveType::Enum ||
                  field.type == semantic::PrimitiveType::Array) &&
                    field.typeName.empty()) ||
                ((field.type != semantic::PrimitiveType::Object &&
                  field.type != semantic::PrimitiveType::Struct &&
                  field.type != semantic::PrimitiveType::Enum &&
                  field.type != semantic::PrimitiveType::Array) &&
                    !field.typeName.empty()) ||
                !exactTypeValid) {
                diagnostics.report(
                    "RS5151",
                    "invalid bytecode field descriptor",
                    {});
            }
        }
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
             found->second->returnTypeId != reference.returnTypeId ||
             found->second->parameterTypes != reference.parameterTypes ||
             found->second->parameterTypeIds != reference.parameterTypeIds)) {
            diagnostics.report(
                "RS5104",
                "conflicting bytecode references share a SymbolId",
                {});
        } else {
            references[reference.symbolId] = &reference;
        }
        if (!validReturnType(reference.returnType) ||
            !validDescriptorIdentity(reference.returnType, reference.returnTypeId, typeDescriptors)) {
            diagnostics.report(
                "RS5105",
                "bytecode function reference has an invalid return type identity",
                {});
        }
        if (reference.parameterTypeIds.size() != reference.parameterTypes.size()) {
            diagnostics.report(
                "RS5106",
                "bytecode function reference type ID count is invalid",
                {});
        }
        for (std::size_t parameter = 0;
             parameter < reference.parameterTypes.size();
             ++parameter) {
            const auto type = reference.parameterTypes[parameter];
            if (!validStorageType(type) ||
                !validDescriptorIdentity(
                    type,
                    typeIdAt(reference.parameterTypeIds, parameter),
                    typeDescriptors)) {
                diagnostics.report(
                    "RS5106",
                    "bytecode function reference has an invalid parameter type identity",
                    {});
            }
        }

        const auto internal = internalFunctions.find(reference.symbolId);
        if (internal != internalFunctions.end() &&
            (internal->second->returnType != reference.returnType ||
             internal->second->returnTypeId != reference.returnTypeId ||
             internal->second->parameterTypes != reference.parameterTypes ||
             internal->second->parameterTypeIds != reference.parameterTypeIds)) {
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
        const bool hasDebugInfo = !function.debugInfo.sourceName.empty() ||
            !function.debugInfo.sequencePoints.empty() ||
            !function.debugInfo.locals.empty() ||
            function.debugInfo.declaration.valid() ||
            function.debugInfo.body.valid();
        if (hasDebugInfo &&
            (sourceFiles.find(function.debugInfo.sourceFileId) == sourceFiles.end() ||
             (function.debugInfo.declaration.valid() &&
              !validSourceRange(function.debugInfo.declaration, sourceFiles)) ||
             (function.debugInfo.body.valid() &&
              !validSourceRange(function.debugInfo.body, sourceFiles)))) {
            diagnostics.report(
                "RS5161",
                "function debug info references an invalid source range",
                {});
        }
        std::unordered_set<std::uint64_t> sequenceKeys;
        for (const auto& point : function.debugInfo.sequencePoints) {
            const auto foundBlock = std::find_if(
                function.blocks.begin(), function.blocks.end(),
                [&](const BasicBlock& block) { return block.id == point.blockId; });
            const bool validPosition = foundBlock != function.blocks.end() &&
                (point.terminator
                    ? point.instructionIndex == foundBlock->instructions.size()
                    : point.instructionIndex < foundBlock->instructions.size());
            const auto key = (static_cast<std::uint64_t>(point.blockId) << 33) |
                (static_cast<std::uint64_t>(point.instructionIndex) << 1) |
                (point.terminator ? 1ull : 0ull);
            if (!validPosition ||
                !validSourceRange(point.range, sourceFiles) ||
                !sequenceKeys.insert(key).second) {
                diagnostics.report(
                    "RS5162",
                    "invalid or duplicate bytecode sequence point",
                    {});
            }
        }
        std::unordered_set<std::uint32_t> localSlots;
        for (const auto& local : function.debugInfo.locals) {
            const bool slotValid = local.slot < function.localTypes.size();
            const auto expectedType = slotValid
                ? function.localTypes[local.slot]
                : semantic::PrimitiveType::Error;
            const auto expectedTypeId = slotValid
                ? typeIdAt(function.localTypeIds, local.slot)
                : semantic::SymbolId{0};
            if (local.name.empty() || !slotValid ||
                local.type != expectedType || local.typeId != expectedTypeId ||
                (local.parameter && local.slot >= function.parameterTypes.size()) ||
                !validSourceRange(local.declaration, sourceFiles) ||
                !validSourceRange(local.scope, sourceFiles) ||
                (!local.parameter &&
                 (local.declaration.span.start < local.scope.span.start ||
                  local.declaration.span.end() > local.scope.span.end())) ||
                !localSlots.insert(local.slot).second) {
                diagnostics.report(
                    "RS5163",
                    "invalid local-variable debug metadata for '" + local.name +
                        "' (slot=" + std::to_string(local.slot) +
                        ", parameter=" + (local.parameter ? "true" : "false") +
                        ", declaration=" + std::to_string(local.declaration.span.start) +
                        ":" + std::to_string(local.declaration.span.length) +
                        ", scope=" + std::to_string(local.scope.span.start) +
                        ":" + std::to_string(local.scope.span.length) + ")",
                    {});
            }
        }
        if (!validReturnType(function.returnType) ||
            !validDescriptorIdentity(function.returnType, function.returnTypeId, typeDescriptors)) {
            diagnostics.report(
                "RS5109",
                "bytecode function has an invalid return type identity",
                {});
        }
        if (function.parameterTypeIds.size() != function.parameterTypes.size()) {
            diagnostics.report(
                "RS5110",
                "bytecode function parameter type ID count is invalid",
                {});
        }
        for (std::size_t parameter = 0;
             parameter < function.parameterTypes.size();
             ++parameter) {
            const auto type = function.parameterTypes[parameter];
            if (!validStorageType(type) ||
                !validDescriptorIdentity(
                    type,
                    typeIdAt(function.parameterTypeIds, parameter),
                    typeDescriptors)) {
                diagnostics.report(
                    "RS5110",
                    "bytecode function has an invalid parameter type identity",
                    {});
            }
        }
        if (function.localTypeIds.size() != function.localTypes.size()) {
            diagnostics.report(
                "RS5111",
                "bytecode function local type ID count is invalid",
                {});
        }
        for (std::size_t local = 0; local < function.localTypes.size(); ++local) {
            const auto type = function.localTypes[local];
            if (!validStorageType(type) ||
                !validDescriptorIdentity(
                     type,
                     typeIdAt(function.localTypeIds, local),
                     typeDescriptors)) {
                diagnostics.report(
                    "RS5111",
                    "bytecode function has an invalid local type identity",
                    {});
            }
        }
        if (function.registerTypeIds.size() != function.registerTypes.size()) {
            diagnostics.report(
                "RS5112",
                "bytecode function register type ID count is invalid",
                {});
        }
        for (std::size_t registerIndex = 0;
             registerIndex < function.registerTypes.size();
             ++registerIndex) {
            const auto type = function.registerTypes[registerIndex];
            if (!validRegisterType(type) ||
                !validDescriptorIdentity(
                     type,
                     typeIdAt(function.registerTypeIds, registerIndex),
                     typeDescriptors)) {
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
                    parameter.type != function.registerTypes[parameter.target] ||
                    parameter.typeId !=
                        typeIdAt(function.registerTypeIds, parameter.target) ||
                    !validDescriptorIdentity(parameter.type, parameter.typeId, typeDescriptors)) {
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
        const auto registerTypeId = [&](Register value) {
            return value < function.registerTypeIds.size()
                ? function.registerTypeIds[value]
                : semantic::SymbolId{0};
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
                        targetBlock->second->parameters[index].type ||
                    registerTypeId(arguments[index]) !=
                        targetBlock->second->parameters[index].typeId) {
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
                        resultType != function.parameterTypes[instruction.index] ||
                        registerTypeId(instruction.result) !=
                            typeIdAt(function.parameterTypeIds, instruction.index)) {
                        diagnostics.report(
                            "RS5128",
                            "invalid bytecode parameter load",
                            {});
                    }
                    break;
                case Opcode::ConstantInt:
                    if (resultType != semantic::PrimitiveType::Int &&
                        resultType != semantic::PrimitiveType::Long &&
                        resultType != semantic::PrimitiveType::Enum) {
                        diagnostics.report("RS5129", "integer constant has invalid result type", {});
                    }
                    break;
                case Opcode::ConstantDouble:
                    if (resultType != semantic::PrimitiveType::Double) {
                        diagnostics.report("RS5129", "floating constant must produce double", {});
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
                        resultType != function.localTypes[instruction.index] ||
                        registerTypeId(instruction.result) !=
                            typeIdAt(function.localTypeIds, instruction.index)) {
                        diagnostics.report("RS5133", "invalid bytecode local load", {});
                    }
                    break;
                case Opcode::StoreLocal:
                    if (instruction.index >= function.localTypes.size() ||
                        instruction.operands.size() != 1 ||
                        registerType(instruction.operands.front()) !=
                            function.localTypes[instruction.index] ||
                        registerTypeId(instruction.operands.front()) !=
                            typeIdAt(function.localTypeIds, instruction.index)) {
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
                case Opcode::ConvertNullToObject:
                    if (instruction.operands.size() != 1 ||
                        registerType(instruction.operands.front()) !=
                            semantic::PrimitiveType::Null ||
                        resultType != semantic::PrimitiveType::Object) {
                        diagnostics.report(
                            "RS5152",
                            "invalid null-to-object bytecode conversion",
                            {});
                    }
                    break;
                case Opcode::ConvertNullToArray:
                    if (instruction.operands.size() != 1 ||
                        registerType(instruction.operands.front()) !=
                            semantic::PrimitiveType::Null ||
                        resultType != semantic::PrimitiveType::Array ||
                        registerTypeId(instruction.result) == 0) {
                        diagnostics.report(
                            "RS5159",
                            "invalid null-to-array bytecode conversion",
                            {});
                    }
                    break;
                case Opcode::ConvertIntToLong:
                    if (instruction.operands.size() != 1 ||
                        registerType(instruction.operands.front()) != semantic::PrimitiveType::Int ||
                        resultType != semantic::PrimitiveType::Long) {
                        diagnostics.report("RS5164", "invalid int-to-long conversion", {});
                    }
                    break;
                case Opcode::ConvertIntToDouble:
                    if (instruction.operands.size() != 1 ||
                        registerType(instruction.operands.front()) != semantic::PrimitiveType::Int ||
                        resultType != semantic::PrimitiveType::Double) {
                        diagnostics.report("RS5165", "invalid int-to-double conversion", {});
                    }
                    break;
                case Opcode::ConvertLongToDouble:
                    if (instruction.operands.size() != 1 ||
                        registerType(instruction.operands.front()) != semantic::PrimitiveType::Long ||
                        resultType != semantic::PrimitiveType::Double) {
                        diagnostics.report("RS5166", "invalid long-to-double conversion", {});
                    }
                    break;
                case Opcode::NewObject:
                    if (instruction.typeIndex >= module.types.size() ||
                        module.types[instruction.typeIndex].kind != semantic::TypeKind::Class ||
                        resultType != semantic::PrimitiveType::Object ||
                        registerTypeId(instruction.result) !=
                            module.types[instruction.typeIndex].id) {
                        diagnostics.report(
                            "RS5153",
                            "invalid bytecode object allocation",
                            {});
                    }
                    break;
                case Opcode::NewStruct:
                    if (instruction.typeIndex >= module.types.size() ||
                        module.types[instruction.typeIndex].kind != semantic::TypeKind::Struct ||
                        resultType != semantic::PrimitiveType::Struct ||
                        registerTypeId(instruction.result) != module.types[instruction.typeIndex].id) {
                        diagnostics.report("RS5167", "invalid bytecode struct construction", {});
                    }
                    break;
                case Opcode::NewArray: {
                    const auto validElement =
                        instruction.elementType == semantic::PrimitiveType::Bool ||
                        instruction.elementType == semantic::PrimitiveType::Int ||
                        instruction.elementType == semantic::PrimitiveType::Long ||
                        instruction.elementType == semantic::PrimitiveType::Double ||
                        instruction.elementType == semantic::PrimitiveType::String ||
                        instruction.elementType == semantic::PrimitiveType::Object ||
                        instruction.elementType == semantic::PrimitiveType::Struct ||
                        instruction.elementType == semantic::PrimitiveType::Enum ||
                        instruction.elementType == semantic::PrimitiveType::Handle;
                    const auto expectedArrayTypeId = arrayTypeId(
                        instruction.elementType,
                        instruction.elementTypeId,
                        typeDescriptors);
                    if (instruction.operands.size() != 1 ||
                        registerType(instruction.operands.front()) !=
                            semantic::PrimitiveType::Int ||
                        resultType != semantic::PrimitiveType::Array ||
                        registerTypeId(instruction.result) != expectedArrayTypeId ||
                        expectedArrayTypeId == 0 ||
                        !validElement ||
                        !validDescriptorIdentity(
                            instruction.elementType,
                            instruction.elementTypeId,
                            typeDescriptors)) {
                        diagnostics.report(
                            "RS5160",
                            "invalid bytecode array allocation",
                            {});
                    }
                    break;
                }
                case Opcode::ArrayLength:
                    if (instruction.operands.size() != 1 ||
                        registerType(instruction.operands.front()) !=
                            semantic::PrimitiveType::Array ||
                        resultType != semantic::PrimitiveType::Int) {
                        diagnostics.report(
                            "RS5161",
                            "invalid bytecode array length",
                            {});
                    }
                    break;
                case Opcode::LoadElement:
                case Opcode::StoreElement: {
                    const auto validElement =
                        instruction.elementType == semantic::PrimitiveType::Bool ||
                        instruction.elementType == semantic::PrimitiveType::Int ||
                        instruction.elementType == semantic::PrimitiveType::Long ||
                        instruction.elementType == semantic::PrimitiveType::Double ||
                        instruction.elementType == semantic::PrimitiveType::String ||
                        instruction.elementType == semantic::PrimitiveType::Object ||
                        instruction.elementType == semantic::PrimitiveType::Struct ||
                        instruction.elementType == semantic::PrimitiveType::Enum ||
                        instruction.elementType == semantic::PrimitiveType::Handle;
                    const auto expectedArrayTypeId = arrayTypeId(
                        instruction.elementType,
                        instruction.elementTypeId,
                        typeDescriptors);
                    const bool commonValid = validElement &&
                        expectedArrayTypeId != 0 &&
                        validDescriptorIdentity(
                            instruction.elementType,
                            instruction.elementTypeId,
                            typeDescriptors) &&
                        instruction.operands.size() >= 2 &&
                        registerType(instruction.operands[0]) ==
                            semantic::PrimitiveType::Array &&
                        registerTypeId(instruction.operands[0]) ==
                            expectedArrayTypeId &&
                        registerType(instruction.operands[1]) ==
                            semantic::PrimitiveType::Int;
                    if (instruction.opcode == Opcode::LoadElement) {
                        if (!commonValid || instruction.operands.size() != 2 ||
                            resultType != instruction.elementType ||
                            registerTypeId(instruction.result) !=
                                instruction.elementTypeId) {
                            diagnostics.report(
                                "RS5162",
                                "invalid bytecode array element load",
                                {});
                        }
                    } else if (!commonValid || instruction.operands.size() != 3 ||
                               registerType(instruction.operands[2]) !=
                                   instruction.elementType ||
                               registerTypeId(instruction.operands[2]) !=
                                   instruction.elementTypeId ||
                               instruction.result != InvalidRegister) {
                        diagnostics.report(
                            "RS5163",
                            "invalid bytecode array element store",
                            {});
                    }
                    break;
                }
                case Opcode::CheckNotNull:
                    if (instruction.typeIndex >= module.types.size() ||
                        module.types[instruction.typeIndex].kind !=
                            semantic::TypeKind::Class ||
                        instruction.operands.size() != 1 ||
                        registerType(instruction.operands.front()) !=
                            semantic::PrimitiveType::Object ||
                        registerTypeId(instruction.operands.front()) !=
                            module.types[instruction.typeIndex].id ||
                        resultType != semantic::PrimitiveType::Object ||
                        registerTypeId(instruction.result) !=
                            module.types[instruction.typeIndex].id) {
                        diagnostics.report(
                            "RS5154",
                            "invalid bytecode object null check",
                            {});
                    }
                    break;
                case Opcode::LoadField:
                case Opcode::StoreField:
                    if (instruction.typeIndex >= module.types.size()) {
                        diagnostics.report(
                            "RS5155",
                            "bytecode field access references an invalid type",
                            {});
                        break;
                    }
                    {
                        const auto& type = module.types[instruction.typeIndex];
                        if (type.kind != semantic::TypeKind::Class ||
                            instruction.index >= type.fields.size() ||
                            instruction.operands.empty() ||
                            registerType(instruction.operands[0]) !=
                                semantic::PrimitiveType::Object ||
                            registerTypeId(instruction.operands[0]) != type.id) {
                            diagnostics.report(
                                "RS5156",
                                "invalid bytecode field access",
                                {});
                            break;
                        }
                        const auto& field = type.fields[instruction.index];
                        const auto expectedFieldTypeId = fieldTypeId(field);
                        if (instruction.opcode == Opcode::LoadField) {
                            if (instruction.operands.size() != 1 ||
                                resultType != field.type ||
                                registerTypeId(instruction.result) !=
                                    expectedFieldTypeId) {
                                diagnostics.report(
                                    "RS5157",
                                    "bytecode field load type mismatch",
                                    {});
                            }
                        } else if (instruction.operands.size() != 2 ||
                                   registerType(instruction.operands[1]) != field.type ||
                                   registerTypeId(instruction.operands[1]) !=
                                       expectedFieldTypeId ||
                                   instruction.result != InvalidRegister) {
                            diagnostics.report(
                                "RS5158",
                                "bytecode field store type mismatch",
                                {});
                        }
                    }
                    break;
                case Opcode::LoadStructField:
                case Opcode::StoreStructField:
                    if (instruction.typeIndex >= module.types.size() ||
                        module.types[instruction.typeIndex].kind != semantic::TypeKind::Struct) {
                        diagnostics.report("RS5168", "struct field access references invalid descriptor", {});
                        break;
                    }
                    {
                        const auto& type = module.types[instruction.typeIndex];
                        if (instruction.index >= type.fields.size() || instruction.operands.empty() ||
                            registerType(instruction.operands[0]) != semantic::PrimitiveType::Struct ||
                            registerTypeId(instruction.operands[0]) != type.id) {
                            diagnostics.report("RS5169", "invalid struct field access", {});
                            break;
                        }
                        const auto& field = type.fields[instruction.index];
                        const auto expectedFieldTypeId = fieldTypeId(field);
                        if (instruction.opcode == Opcode::LoadStructField) {
                            if (instruction.operands.size() != 1 || resultType != field.type ||
                                registerTypeId(instruction.result) != expectedFieldTypeId) {
                                diagnostics.report("RS5170", "struct field load type mismatch", {});
                            }
                        } else if (instruction.operands.size() != 2 ||
                                   registerType(instruction.operands[1]) != field.type ||
                                   registerTypeId(instruction.operands[1]) != expectedFieldTypeId ||
                                   resultType != semantic::PrimitiveType::Struct ||
                                   registerTypeId(instruction.result) != type.id) {
                            diagnostics.report("RS5171", "struct field store type mismatch", {});
                        }
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
                                    reference.parameterTypes[argument] ||
                                registerTypeId(instruction.operands[argument]) !=
                                    typeIdAt(reference.parameterTypeIds, argument)) {
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
                                   resultType != reference.returnType ||
                                   registerTypeId(instruction.result) !=
                                       reference.returnTypeId) {
                            diagnostics.report(
                                "RS5140",
                                "bytecode call result does not match reference",
                                {});
                        }
                    }
                    break;
                case Opcode::NegateInt:
                case Opcode::NegateLong:
                case Opcode::NegateDouble: {
                    const auto expected = instruction.opcode == Opcode::NegateInt
                        ? semantic::PrimitiveType::Int
                        : instruction.opcode == Opcode::NegateLong
                            ? semantic::PrimitiveType::Long
                            : semantic::PrimitiveType::Double;
                    if (instruction.operands.size() != 1 ||
                        registerType(instruction.operands.front()) != expected ||
                        resultType != expected) {
                        diagnostics.report("RS5141", "invalid numeric negation", {});
                    }
                    break;
                }
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
                case Opcode::AddLong:
                case Opcode::SubtractLong:
                case Opcode::MultiplyLong:
                case Opcode::DivideLong:
                case Opcode::RemainderLong:
                    if (instruction.operands.size() != 2 ||
                        registerType(instruction.operands[0]) != semantic::PrimitiveType::Long ||
                        registerType(instruction.operands[1]) != semantic::PrimitiveType::Long ||
                        resultType != semantic::PrimitiveType::Long) {
                        diagnostics.report("RS5172", "invalid long arithmetic", {});
                    }
                    break;
                case Opcode::AddDouble:
                case Opcode::SubtractDouble:
                case Opcode::MultiplyDouble:
                case Opcode::DivideDouble:
                    if (instruction.operands.size() != 2 ||
                        registerType(instruction.operands[0]) != semantic::PrimitiveType::Double ||
                        registerType(instruction.operands[1]) != semantic::PrimitiveType::Double ||
                        resultType != semantic::PrimitiveType::Double) {
                        diagnostics.report("RS5173", "invalid double arithmetic", {});
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
                case Opcode::LessLong:
                case Opcode::LessOrEqualLong:
                case Opcode::GreaterLong:
                case Opcode::GreaterOrEqualLong:
                    if (instruction.operands.size() != 2 ||
                        registerType(instruction.operands[0]) != semantic::PrimitiveType::Long ||
                        registerType(instruction.operands[1]) != semantic::PrimitiveType::Long ||
                        resultType != semantic::PrimitiveType::Bool) {
                        diagnostics.report("RS5174", "invalid long comparison", {});
                    }
                    break;
                case Opcode::LessDouble:
                case Opcode::LessOrEqualDouble:
                case Opcode::GreaterDouble:
                case Opcode::GreaterOrEqualDouble:
                    if (instruction.operands.size() != 2 ||
                        registerType(instruction.operands[0]) != semantic::PrimitiveType::Double ||
                        registerType(instruction.operands[1]) != semantic::PrimitiveType::Double ||
                        resultType != semantic::PrimitiveType::Bool) {
                        diagnostics.report("RS5175", "invalid double comparison", {});
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
                    registerType(block.terminator.value) != function.returnType ||
                    registerTypeId(block.terminator.value) !=
                        function.returnTypeId) {
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
