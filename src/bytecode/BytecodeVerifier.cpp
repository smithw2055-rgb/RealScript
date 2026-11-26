#include "realscript/bytecode/Bytecode.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace realscript::bytecode {
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
        semantic::isNumericType(type) ||
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
        semantic::isNumericType(type) ||
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
    return (type == semantic::PrimitiveType::Struct ||
            type == semantic::PrimitiveType::Enum ||
            type == semantic::PrimitiveType::Array)
        ? typeId != 0
        : type == semantic::PrimitiveType::Object || typeId == 0;
}

bool descriptorAssignable(
    semantic::SymbolId actual,
    semantic::SymbolId expected,
    const std::unordered_map<
        semantic::SymbolId,
        const semantic::TypeSymbol*>& descriptors) noexcept {
    if (actual == expected) return true;

    std::array<semantic::SymbolId, 32> visitedSmall{};
    std::size_t visitedSmallCount = 0;
    std::vector<semantic::SymbolId> visitedOverflow;
    const auto markVisited = [&](semantic::SymbolId id, bool isIface) -> bool {
        for (std::size_t i = 0; i < visitedSmallCount; ++i) {
            if (visitedSmall[i] == id) return false;
        }
        if (!visitedOverflow.empty()) {
            for (const auto existing : visitedOverflow) {
                if (existing == id) return false;
            }
        }
        (void)isIface;
        if (visitedSmallCount < visitedSmall.size()) {
            visitedSmall[visitedSmallCount++] = id;
        } else {
            visitedOverflow.push_back(id);
        }
        return true;
    };

    std::array<semantic::SymbolId, 32> ifaceStackSmall{};
    std::size_t ifaceStackCount = 0;
    std::vector<semantic::SymbolId> ifaceStackOverflow;
    const auto pushIface = [&](semantic::SymbolId id) {
        if (ifaceStackCount < ifaceStackSmall.size() && ifaceStackOverflow.empty()) {
            ifaceStackSmall[ifaceStackCount++] = id;
        } else {
            ifaceStackOverflow.push_back(id);
        }
    };

    std::array<semantic::SymbolId, 32> visitedIfaceSmall{};
    std::size_t visitedIfaceSmallCount = 0;
    std::vector<semantic::SymbolId> visitedIfaceOverflow;
    const auto markVisitedIface = [&](semantic::SymbolId id) -> bool {
        for (std::size_t i = 0; i < visitedIfaceSmallCount; ++i) {
            if (visitedIfaceSmall[i] == id) return false;
        }
        if (!visitedIfaceOverflow.empty()) {
            for (const auto existing : visitedIfaceOverflow) {
                if (existing == id) return false;
            }
        }
        if (visitedIfaceSmallCount < visitedIfaceSmall.size()) {
            visitedIfaceSmall[visitedIfaceSmallCount++] = id;
        } else {
            visitedIfaceOverflow.push_back(id);
        }
        return true;
    };

    auto current = actual;
    while (current != 0 && markVisited(current, false)) {
        const auto found = descriptors.find(current);
        if (found == descriptors.end()) break;
        for (const auto& baseIfaceName : found->second->baseInterfaceNames) {
            const auto baseIfaceId = semantic::stableTypeId(baseIfaceName);
            if (baseIfaceId == expected) return true;
            pushIface(baseIfaceId);
        }
        for (const auto& interfaceMap :
             found->second->interfaceDispatchMaps) {
            if (interfaceMap.interfaceTypeId == expected) return true;
        }
        if (found->second->baseTypeId == expected) return true;
        current = found->second->baseTypeId;
    }
    while (ifaceStackCount > 0 || !ifaceStackOverflow.empty()) {
        semantic::SymbolId curIface = 0;
        if (!ifaceStackOverflow.empty()) {
            curIface = ifaceStackOverflow.back();
            ifaceStackOverflow.pop_back();
        } else {
            curIface = ifaceStackSmall[--ifaceStackCount];
        }
        if (curIface == expected) return true;
        if (!markVisitedIface(curIface)) continue;
        const auto foundIface = descriptors.find(curIface);
        if (foundIface == descriptors.end()) continue;
        for (const auto& baseIfaceName : foundIface->second->baseInterfaceNames) {
            const auto baseIfaceId = semantic::stableTypeId(baseIfaceName);
            if (baseIfaceId == expected) return true;
            pushIface(baseIfaceId);
        }
    }
    return false;
}

bool compatibleType(
    semantic::PrimitiveType actualType,
    semantic::SymbolId actualTypeId,
    semantic::PrimitiveType expectedType,
    semantic::SymbolId expectedTypeId,
    const std::unordered_map<semantic::SymbolId,
        const semantic::TypeSymbol*>& descriptors) noexcept {
    if (actualType != expectedType) return false;
    if (actualType == semantic::PrimitiveType::Object) {
        if (expectedTypeId == 0) return true;
        if (actualTypeId == 0) return false;
        return descriptorAssignable(actualTypeId, expectedTypeId, descriptors);
    }
    return actualTypeId == expectedTypeId;
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
    if (type == semantic::PrimitiveType::Object && typeId == 0) {
        return true;
    }
    if (type != semantic::PrimitiveType::Object &&
        type != semantic::PrimitiveType::Struct &&
        type != semantic::PrimitiveType::Enum) {
        return true;
    }
    const auto found = descriptors.find(typeId);
    return found != descriptors.end() &&
        found->second->kind == expectedTypeKind(type);
}

bool isAnyObjectFieldType(
    semantic::PrimitiveType type,
    std::string_view typeName) noexcept {
    return type == semantic::PrimitiveType::Object &&
        (typeName.empty() || typeName == "object");
}

semantic::SymbolId fieldTypeId(const semantic::FieldSymbol& field) noexcept {
    if (isAnyObjectFieldType(field.type, field.typeName)) {
        return 0;
    }
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
    if (elementType != semantic::PrimitiveType::Object &&
        elementType != semantic::PrimitiveType::Struct &&
        elementType != semantic::PrimitiveType::Enum) {
        static const auto kPrimitiveArrayIds = []() {
            std::array<semantic::SymbolId, 32> ids{};
            for (std::size_t i = 0; i < ids.size(); ++i) {
                const auto name = semantic::arrayTypeName(
                    static_cast<semantic::PrimitiveType>(i), "");
                ids[i] = semantic::stableTypeId(name);
            }
            return ids;
        }();
        const auto idx = static_cast<std::size_t>(elementType);
        if (idx < kPrimitiveArrayIds.size()) {
            return kPrimitiveArrayIds[idx];
        }
        return semantic::stableTypeId(semantic::arrayTypeName(elementType, ""));
    }
    const auto found = descriptors.find(elementTypeId);
    if (found == descriptors.end() ||
        found->second->kind != expectedTypeKind(elementType)) {
        return 0;
    }
    const std::string exactElementName = semantic::canonicalTypeName(*found->second);
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
        instruction.opcode == Opcode::StoreElement ||
        instruction.opcode == Opcode::AsyncEnter ||
        instruction.opcode == Opcode::AwaitSuspend) {
        return false;
    }
    if (instruction.opcode == Opcode::Call &&
        instruction.index < module.functionReferences.size()) {
        return module.functionReferences[instruction.index].returnType !=
            semantic::PrimitiveType::Void;
    }
    if (instruction.opcode == Opcode::InvokeDelegate ||
        instruction.opcode == Opcode::AwaitResume) {
        return instruction.result != InvalidRegister;
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
    case Opcode::ConstantTypeId:
    case Opcode::AsyncEnter:
    case Opcode::AwaitResume:
        return 0;
    case Opcode::StoreLocal:
    case Opcode::ConvertNullToString:
    case Opcode::ConvertNullToObject:
    case Opcode::ConvertNullToArray:
    case Opcode::ConvertIntToLong:
    case Opcode::ConvertIntToDouble:
    case Opcode::ConvertLongToDouble:
    case Opcode::ConvertNumeric:
    case Opcode::NewArray:
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
    case Opcode::AwaitSuspend:
        return 1;
    case Opcode::AsyncResolve:
        return instruction.operands.size();
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
    case Opcode::NewDelegate:
    case Opcode::InvokeDelegate:
        return instruction.operands.size();
    default:
        return 2;
    }
}

} // namespace

bool verifyModule(
    const Module& module,
    diagnostics::DiagnosticBag& diagnostics) {
    const auto initialCount = diagnostics.items().size();

    if (module.version.major != 0 ||
        (module.version.minor != 6 &&
         module.version.minor != 7 &&
         module.version.minor != 8 &&
         module.version.minor != 9)) {
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

        if (type.kind != semantic::TypeKind::Class &&
            !type.virtualDispatchTable.empty()) {
            diagnostics.report(
                "RS5180",
                "non-class bytecode type has a virtual dispatch table",
                {});
        }
        if (type.interfaceType &&
            (!type.fields.empty() ||
             !type.virtualDispatchTable.empty() ||
             !type.interfaceDispatchMaps.empty())) {
            diagnostics.report(
                "RS5184",
                "bytecode interface descriptor contains runtime class state",
                {});
        }
        if (!type.abstractType) {
            for (const auto symbolId : type.virtualDispatchTable) {
                if (symbolId == 0) {
                    diagnostics.report(
                        "RS5181",
                        "concrete bytecode type has an abstract virtual slot",
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
                    "RS5185",
                    "bytecode type has an invalid or duplicate interface map",
                    {});
            }
            if (!type.abstractType) {
                for (const auto symbolId : interfaceMap.slots) {
                    if (symbolId == 0) {
                        diagnostics.report(
                            "RS5186",
                            "concrete bytecode type has an abstract interface slot",
                            {});
                    }
                }
            }
        }

        for (std::size_t fieldIndex = 0; fieldIndex < type.fields.size(); ++fieldIndex) {
            const auto& field = type.fields[fieldIndex];
            const bool isAnyObj = isAnyObjectFieldType(field.type, field.typeName);
            bool exactTypeValid = true;
            if (isAnyObj) {
                exactTypeValid = true;
            } else if (field.type == semantic::PrimitiveType::Object ||
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
                ((field.type == semantic::PrimitiveType::Struct ||
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

        if (reference.virtualDispatch &&
            reference.interfaceDispatch) {
            diagnostics.report(
                "RS5187",
                "bytecode reference cannot use virtual and interface dispatch together",
                {});
        }
        if (reference.virtualDispatch) {
            const auto receiverTypeId =
                typeIdAt(reference.parameterTypeIds, 0);
            const auto receiver = typeDescriptors.find(receiverTypeId);
            if (reference.virtualSlot ==
                    std::numeric_limits<std::uint32_t>::max() ||
                reference.parameterTypes.empty() ||
                reference.parameterTypes.front() !=
                    semantic::PrimitiveType::Object ||
                receiver == typeDescriptors.end() ||
                reference.virtualSlot >=
                    receiver->second->virtualDispatchTable.size()) {
                diagnostics.report(
                    "RS5182",
                    "bytecode virtual reference has an invalid slot contract",
                    {});
            }
        } else if (reference.virtualSlot !=
                std::numeric_limits<std::uint32_t>::max()) {
            diagnostics.report(
                "RS5183",
                "static bytecode reference carries a virtual slot",
                {});
        }
        if (reference.interfaceDispatch) {
            const auto receiverTypeId =
                typeIdAt(reference.parameterTypeIds, 0);
            const auto receiver = typeDescriptors.find(
                reference.interfaceTypeId);
            if (reference.interfaceTypeId == 0 ||
                reference.interfaceSlot ==
                    std::numeric_limits<std::uint32_t>::max() ||
                reference.parameterTypes.empty() ||
                reference.parameterTypes.front() !=
                    semantic::PrimitiveType::Object ||
                receiverTypeId != reference.interfaceTypeId ||
                receiver == typeDescriptors.end() ||
                !receiver->second->interfaceType ||
                reference.interfaceSlot >=
                    receiver->second->methods.size()) {
                diagnostics.report(
                    "RS5188",
                    "bytecode interface reference has an invalid slot contract",
                    {});
            }
        } else if (reference.interfaceTypeId != 0 ||
                   reference.interfaceSlot !=
                       std::numeric_limits<std::uint32_t>::max()) {
            diagnostics.report(
                "RS5189",
                "non-interface bytecode reference carries interface metadata",
                {});
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
        const std::size_t kInvalidDense = std::numeric_limits<std::size_t>::max();
        BlockId maxBlockId = 0;
        for (const auto& block : function.blocks) {
            if (block.id > maxBlockId) {
                maxBlockId = block.id;
            }
        }
        const bool useDenseBlockTable =
            !function.blocks.empty() && maxBlockId < 65536u;
        std::vector<std::size_t> denseBlockById;
        std::unordered_map<BlockId, std::size_t> denseBlockByIdFallback;
        std::vector<const BasicBlock*> uniqueBlocks;
        uniqueBlocks.reserve(function.blocks.size());
        if (useDenseBlockTable) {
            denseBlockById.assign(static_cast<std::size_t>(maxBlockId) + 1u, kInvalidDense);
            for (const auto& block : function.blocks) {
                if (denseBlockById[block.id] == kInvalidDense) {
                    denseBlockById[block.id] = uniqueBlocks.size();
                    uniqueBlocks.push_back(&block);
                }
            }
        } else {
            denseBlockByIdFallback.reserve(function.blocks.size());
            for (const auto& block : function.blocks) {
                if (denseBlockByIdFallback.emplace(block.id, uniqueBlocks.size()).second) {
                    uniqueBlocks.push_back(&block);
                }
            }
        }
        const auto findDenseBlock = [&](BlockId id) noexcept -> std::size_t {
            if (useDenseBlockTable) {
                return id <= maxBlockId ? denseBlockById[id] : kInvalidDense;
            }
            const auto it = denseBlockByIdFallback.find(id);
            return it == denseBlockByIdFallback.end() ? kInvalidDense : it->second;
        };
        const auto findBlockById = [&](BlockId id) noexcept -> const BasicBlock* {
            const auto dense = findDenseBlock(id);
            return dense == kInvalidDense ? nullptr : uniqueBlocks[dense];
        };

        std::vector<std::uint64_t> validSequenceKeys;
        validSequenceKeys.reserve(function.debugInfo.sequencePoints.size());
        for (const auto& point : function.debugInfo.sequencePoints) {
            const auto* foundBlock = findBlockById(point.blockId);
            const bool validPosition = foundBlock != nullptr &&
                (point.terminator
                    ? point.instructionIndex == foundBlock->instructions.size()
                    : point.instructionIndex < foundBlock->instructions.size());
            const auto key = (static_cast<std::uint64_t>(point.blockId) << 33) |
                (static_cast<std::uint64_t>(point.instructionIndex) << 1) |
                (point.terminator ? 1ull : 0ull);
            bool rangeValid = false;
            if (point.mappedPath.empty()) {
                rangeValid = validSourceRange(point.range, sourceFiles);
            } else {
                const auto srcIt = sourceFiles.find(point.range.fileId);
                if (srcIt != sourceFiles.end() &&
                    (point.range.start.line < point.range.end.line ||
                     (point.range.start.line == point.range.end.line &&
                      point.range.start.column <= point.range.end.column))) {
                    const auto phys = debug::makeSourceRange(*srcIt->second, point.range.span);
                    rangeValid = phys.start.column == point.range.start.column &&
                        phys.end.column == point.range.end.column;
                }
            }
            if (!validPosition || !rangeValid) {
                diagnostics.report(
                    "RS5162",
                    "invalid or duplicate bytecode sequence point",
                    {});
            } else {
                validSequenceKeys.push_back(key);
            }
        }
        if (validSequenceKeys.size() > 1) {
            std::sort(validSequenceKeys.begin(), validSequenceKeys.end());
            for (std::size_t seqIdx = 1; seqIdx < validSequenceKeys.size(); ++seqIdx) {
                if (validSequenceKeys[seqIdx] == validSequenceKeys[seqIdx - 1]) {
                    diagnostics.report(
                        "RS5162",
                        "invalid or duplicate bytecode sequence point",
                        {});
                }
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

        const std::size_t numUniqueBlocks = uniqueBlocks.size();
        for (std::size_t bIdx = 0; bIdx < function.blocks.size(); ++bIdx) {
            const auto& block = function.blocks[bIdx];
            const auto firstDense = findDenseBlock(block.id);
            if (firstDense == kInvalidDense || uniqueBlocks[firstDense] != &block) {
                diagnostics.report("RS5114", "duplicate bytecode block id", {});
            }
        }
        std::vector<Definition> definitions(
            function.registerTypes.size(), Definition{0, -2});
        for (const auto& block : function.blocks) {
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
                if (parameter.target >= definitions.size() ||
                    definitions[parameter.target].instructionIndex != -2) {
                    diagnostics.report(
                        "RS5116",
                        "bytecode register has multiple definitions",
                        {});
                } else {
                    definitions[parameter.target] = Definition{block.id, -1};
                }
            }
            for (std::size_t index = 0; index < block.instructions.size(); ++index) {
                const auto& instruction = block.instructions[index];
                const bool defines = instructionDefinesValue(instruction, module);
                if (defines) {
                    if (instruction.result >= definitions.size() ||
                        definitions[instruction.result].instructionIndex != -2) {
                        diagnostics.report(
                            "RS5116",
                            "bytecode register has multiple or invalid definitions",
                            {});
                    } else {
                        definitions[instruction.result] = Definition{
                            block.id,
                            static_cast<std::int64_t>(index),
                        };
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
            if (definitions[value].instructionIndex == -2) {
                diagnostics.report(
                    "RS5150",
                    "bytecode register r" + std::to_string(value) +
                        " has no definition",
                    {});
            }
        }

        const auto entryDense = findDenseBlock(0);
        if (entryDense == kInvalidDense) {
            diagnostics.report(
                "RS5118",
                "bytecode function entry block must be bb0",
                {});
        }

        std::vector<std::vector<std::size_t>> successors(numUniqueBlocks);
        std::vector<std::vector<std::size_t>> predecessors(numUniqueBlocks);
        const auto addEdge = [&](BlockId from, BlockId to) {
            const auto toDense = findDenseBlock(to);
            if (toDense != kInvalidDense) {
                const auto fromDense = findDenseBlock(from);
                if (fromDense != kInvalidDense) {
                    successors[fromDense].push_back(toDense);
                    predecessors[toDense].push_back(fromDense);
                }
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
        for (const auto& handler : function.exceptionHandlers) {
            if (findDenseBlock(handler.handlerBlock) == kInvalidDense ||
                handler.exceptionLocal >= function.localTypes.size() ||
                function.localTypes[handler.exceptionLocal] !=
                    semantic::PrimitiveType::Object ||
                typeIdAt(function.localTypeIds, handler.exceptionLocal) !=
                    handler.catchTypeId) {
                diagnostics.report(
                    "RS5175", "invalid bytecode exception handler target or local", {});
                continue;
            }
            if (handler.catchTypeId != 0) {
                const auto catchType = typeDescriptors.find(
                    handler.catchTypeId);
                if (catchType == typeDescriptors.end() ||
                    catchType->second->kind != semantic::TypeKind::Class) {
                    diagnostics.report(
                        "RS5175", "invalid bytecode exception handler catch type", {});
                }
            }
            for (const auto protectedBlock : handler.protectedBlocks) {
                if (findDenseBlock(protectedBlock) == kInvalidDense) {
                    diagnostics.report(
                        "RS5175", "invalid bytecode protected block", {});
                } else {
                    addEdge(protectedBlock, handler.handlerBlock);
                }
            }
        }

        std::vector<std::uint8_t> reachable(numUniqueBlocks, 0u);
        std::vector<std::size_t> rpoOrder;
        if (entryDense != kInvalidDense) {
            std::deque<std::size_t> queue;
            reachable[entryDense] = 1u;
            queue.push_back(entryDense);
            while (!queue.empty()) {
                const auto current = queue.front();
                queue.pop_front();
                for (const auto next : successors[current]) {
                    if (!reachable[next]) {
                        reachable[next] = 1u;
                        queue.push_back(next);
                    }
                }
            }
            // Compute reverse post-order over reachable nodes (excluding entryDense)
            // for rapid iterative bitset dominator convergence.
            std::vector<std::uint8_t> dfsVisited(numUniqueBlocks, 0u);
            struct DfsFrame {
                std::size_t node;
                std::size_t nextSucc;
            };
            std::vector<DfsFrame> dfsStack;
            dfsStack.reserve(numUniqueBlocks);
            dfsVisited[entryDense] = 1u;
            dfsStack.push_back(DfsFrame{entryDense, 0u});
            rpoOrder.reserve(numUniqueBlocks);
            while (!dfsStack.empty()) {
                auto& frame = dfsStack.back();
                const auto& succs = successors[frame.node];
                if (frame.nextSucc < succs.size()) {
                    const auto next = succs[frame.nextSucc++];
                    if (!dfsVisited[next]) {
                        dfsVisited[next] = 1u;
                        dfsStack.push_back(DfsFrame{next, 0u});
                    }
                } else {
                    if (frame.node != entryDense) {
                        rpoOrder.push_back(frame.node);
                    }
                    dfsStack.pop_back();
                }
            }
            std::reverse(rpoOrder.begin(), rpoOrder.end());
        }
        for (const auto& block : function.blocks) {
            const auto d = findDenseBlock(block.id);
            if (d == kInvalidDense || !reachable[d]) {
                diagnostics.report(
                    "RS5119",
                    "unreachable bytecode block bb" + std::to_string(block.id),
                    {});
            }
        }

        const std::size_t wordsPerSet = (numUniqueBlocks + 63u) >> 6u;
        const std::size_t tailBits = numUniqueBlocks & 63u;
        const std::uint64_t tailMask =
            tailBits == 0u ? ~std::uint64_t{0} : ((std::uint64_t{1} << tailBits) - 1u);
        std::vector<std::uint64_t> domBits(
            numUniqueBlocks * wordsPerSet, ~std::uint64_t{0});
        if (wordsPerSet > 0u) {
            for (std::size_t d = 0; d < numUniqueBlocks; ++d) {
                domBits[d * wordsPerSet + (wordsPerSet - 1u)] &= tailMask;
            }
            if (entryDense != kInvalidDense) {
                std::uint64_t* entryRow = domBits.data() + entryDense * wordsPerSet;
                std::fill(entryRow, entryRow + wordsPerSet, std::uint64_t{0});
                entryRow[entryDense >> 6u] = std::uint64_t{1} << (entryDense & 63u);
            }
            std::vector<std::uint64_t> nextRow(wordsPerSet, 0u);
            bool changed = true;
            while (changed) {
                changed = false;
                for (const std::size_t bDense : rpoOrder) {
                    const auto& incoming = predecessors[bDense];
                    if (incoming.empty()) {
                        std::fill(nextRow.begin(), nextRow.end(), std::uint64_t{0});
                    } else {
                        const std::uint64_t* firstRow =
                            domBits.data() + incoming.front() * wordsPerSet;
                        for (std::size_t w = 0; w < wordsPerSet; ++w) {
                            nextRow[w] = firstRow[w];
                        }
                        for (std::size_t index = 1; index < incoming.size(); ++index) {
                            const std::uint64_t* predRow =
                                domBits.data() + incoming[index] * wordsPerSet;
                            for (std::size_t w = 0; w < wordsPerSet; ++w) {
                                nextRow[w] &= predRow[w];
                            }
                        }
                    }
                    nextRow[bDense >> 6u] |= std::uint64_t{1} << (bDense & 63u);
                    std::uint64_t* curRow = domBits.data() + bDense * wordsPerSet;
                    bool diff = false;
                    for (std::size_t w = 0; w < wordsPerSet; ++w) {
                        if (curRow[w] != nextRow[w]) {
                            curRow[w] = nextRow[w];
                            diff = true;
                        }
                    }
                    if (diff) {
                        changed = true;
                    }
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
            const auto& definition = definitions[value];
            if (definition.instructionIndex == -2) {
                diagnostics.report(
                    "RS5121",
                    "bytecode uses undefined register r" + std::to_string(value),
                    {});
                return;
            }
            if (definition.block == useBlock) {
                if (definition.instructionIndex >= useIndex &&
                    definition.instructionIndex >= 0) {
                    diagnostics.report(
                        "RS5122",
                        "bytecode register is used before its definition",
                        {});
                }
            } else {
                const auto useDense = findDenseBlock(useBlock);
                const auto defDense = findDenseBlock(definition.block);
                if (useDense == kInvalidDense || defDense == kInvalidDense ||
                    (domBits[useDense * wordsPerSet + (defDense >> 6u)] &
                        (std::uint64_t{1} << (defDense & 63u))) == 0u) {
                    diagnostics.report(
                        "RS5123",
                        "bytecode register definition does not dominate its use",
                        {});
                }
            }
        };

        const auto verifyEdge = [&](
            BlockId from,
            BlockId target,
            const SmallRegisterVector& arguments,
            std::int64_t useIndex) {
            const auto* targetBlock = findBlockById(target);
            if (!targetBlock) {
                diagnostics.report(
                    "RS5124",
                    "bytecode branch targets missing block bb" +
                        std::to_string(target),
                    {});
                return;
            }
            if (arguments.size() != targetBlock->parameters.size()) {
                diagnostics.report(
                    "RS5125",
                    "bytecode branch argument count does not match block parameters",
                    {});
                return;
            }
            for (std::size_t index = 0; index < arguments.size(); ++index) {
                checkUse(arguments[index], from, useIndex);
                if (!compatibleType(
                        registerType(arguments[index]),
                        registerTypeId(arguments[index]),
                        targetBlock->parameters[index].type,
                        targetBlock->parameters[index].typeId,
                        typeDescriptors)) {
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
                if (!instruction.checkedArithmetic &&
                    !supportsArithmeticMode(instruction.opcode)) {
                    diagnostics.report(
                        "RS5168",
                        "unchecked mode is invalid for this bytecode opcode",
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
                        !compatibleType(
                            registerType(instruction.operands.front()),
                            registerTypeId(instruction.operands.front()),
                            function.localTypes[instruction.index],
                            typeIdAt(function.localTypeIds, instruction.index),
                            typeDescriptors)) {
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
                case Opcode::ConvertNumeric:
                    if (instruction.operands.size() != 1 ||
                        !semantic::isNumericType(registerType(
                            instruction.operands.front())) ||
                        !semantic::isNumericType(resultType) ||
                        registerType(instruction.operands.front()) == resultType) {
                        diagnostics.report(
                            "RS5167", "invalid numeric conversion", {});
                    }
                    break;
                case Opcode::ConstantTypeId:
                    if (!instruction.operands.empty() ||
                        resultType != semantic::PrimitiveType::ULong) {
                        diagnostics.report(
                            "RS5173", "invalid typeof token", {});
                    }
                    break;
                case Opcode::IsType:
                case Opcode::AsType: {
                    const auto operandType = instruction.operands.empty()
                        ? semantic::PrimitiveType::Error
                        : registerType(instruction.operands.front());
                    const bool exactTarget = semantic::isExactType(
                        instruction.elementType);
                    const bool knownTarget = !exactTarget ||
                        typeDescriptors.find(
                            instruction.elementTypeId) !=
                            typeDescriptors.end();
                    if (!semantic::isReferenceType(operandType) ||
                        !knownTarget ||
                        (instruction.opcode == Opcode::IsType &&
                         resultType != semantic::PrimitiveType::Bool) ||
                        (instruction.opcode == Opcode::AsType &&
                         (resultType != instruction.elementType ||
                          !semantic::isReferenceType(
                              instruction.elementType)))) {
                        diagnostics.report(
                            "RS5174", "invalid runtime type operation", {});
                    }
                    break;
                }
                case Opcode::NewObject:
                    if (instruction.typeIndex >= module.types.size() ||
                        module.types[instruction.typeIndex].kind != semantic::TypeKind::Class ||
                        module.types[instruction.typeIndex].interfaceType ||
                        module.types[instruction.typeIndex].delegateType ||
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
                        semantic::isNumericType(instruction.elementType) ||
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
                        semantic::isNumericType(instruction.elementType) ||
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
                    } else if (!commonValid ||
                               instruction.operands.size() != 3 ||
                               !compatibleType(
                                   registerType(instruction.operands[2]),
                                   registerTypeId(instruction.operands[2]),
                                   instruction.elementType,
                                   instruction.elementTypeId,
                                   typeDescriptors) ||
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
                        !descriptorAssignable(
                            registerTypeId(instruction.operands.front()),
                            module.types[instruction.typeIndex].id,
                            typeDescriptors) ||
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
                                   !compatibleType(
                                       registerType(instruction.operands[1]),
                                       registerTypeId(instruction.operands[1]),
                                       field.type,
                                       expectedFieldTypeId,
                                       typeDescriptors) ||
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
                                   !compatibleType(
                                       registerType(instruction.operands[1]),
                                       registerTypeId(instruction.operands[1]),
                                       field.type,
                                       expectedFieldTypeId,
                                       typeDescriptors) ||
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
                            if (!compatibleType(
                                    registerType(instruction.operands[argument]),
                                    registerTypeId(instruction.operands[argument]),
                                    reference.parameterTypes[argument],
                                    typeIdAt(reference.parameterTypeIds, argument),
                                    typeDescriptors)) {
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
                case Opcode::NewDelegate:
                    if (instruction.typeIndex >= module.types.size() ||
                        !module.types[instruction.typeIndex].delegateType ||
                        instruction.index >=
                            module.functionReferences.size() ||
                        instruction.operands.size() > 1 ||
                        resultType != semantic::PrimitiveType::Object ||
                        registerTypeId(instruction.result) !=
                            module.types[instruction.typeIndex].id) {
                        diagnostics.report(
                            "RS5180",
                            "invalid bytecode delegate creation",
                            {});
                        break;
                    }
                    if (!instruction.operands.empty()) {
                        const auto& reference =
                            module.functionReferences[instruction.index];
                        if (reference.parameterTypes.empty() ||
                            !compatibleType(
                                registerType(
                                    instruction.operands.front()),
                                registerTypeId(
                                    instruction.operands.front()),
                                reference.parameterTypes.front(),
                                typeIdAt(
                                    reference.parameterTypeIds, 0),
                                typeDescriptors)) {
                            diagnostics.report(
                                "RS5181",
                                "bytecode delegate receiver type mismatch",
                                {});
                        }
                    }
                    break;
                case Opcode::InvokeDelegate: {
                    if (instruction.typeIndex >= module.types.size() ||
                        !module.types[instruction.typeIndex].delegateType ||
                        instruction.operands.empty()) {
                        diagnostics.report(
                            "RS5182",
                            "invalid bytecode delegate invocation",
                            {});
                        break;
                    }
                    const auto& type =
                        module.types[instruction.typeIndex];
                    const semantic::FunctionSymbol* invoke = nullptr;
                    for (const auto& method : type.methods) {
                        if (method.name == "Invoke") {
                            invoke = &method;
                            break;
                        }
                    }
                    bool valid = invoke &&
                        invoke->parameters.size() ==
                            instruction.operands.size() &&
                        registerType(instruction.operands.front()) ==
                            semantic::PrimitiveType::Object &&
                        registerTypeId(instruction.operands.front()) ==
                            type.id;
                    if (valid) {
                        for (std::size_t argumentIndex = 1;
                             argumentIndex < instruction.operands.size();
                             ++argumentIndex) {
                            const auto& parameter =
                                invoke->parameters[argumentIndex];
                            const auto parameterType =
                                semantic::storageTypeOf(parameter);
                            if (!compatibleType(
                                    registerType(
                                        instruction.operands[argumentIndex]),
                                    registerTypeId(
                                        instruction.operands[argumentIndex]),
                                    parameterType,
                                    semantic::isExactType(parameterType)
                                        ? semantic::stableTypeId(
                                            semantic::storageTypeNameOf(
                                                parameter))
                                        : 0,
                                    typeDescriptors)) {
                                valid = false;
                                break;
                            }
                        }
                    }
                    const auto expectedReturnTypeId = invoke &&
                            semantic::isExactType(invoke->returnType)
                        ? semantic::stableTypeId(invoke->returnTypeName)
                        : 0;
                    if (!valid || !invoke ||
                        (invoke->returnType ==
                                semantic::PrimitiveType::Void
                            ? instruction.result != InvalidRegister
                            : instruction.result == InvalidRegister ||
                                resultType != invoke->returnType ||
                                registerTypeId(instruction.result) !=
                                    expectedReturnTypeId)) {
                        diagnostics.report(
                            "RS5182",
                            "invalid bytecode delegate invocation",
                            {});
                    }
                    break;
                }
                case Opcode::CombineDelegate:
                case Opcode::RemoveDelegate:
                    if (instruction.typeIndex >= module.types.size() ||
                        !module.types[instruction.typeIndex].delegateType ||
                        instruction.operands.size() != 2 ||
                        registerType(instruction.operands[0]) !=
                            semantic::PrimitiveType::Object ||
                        registerTypeId(instruction.operands[0]) !=
                            module.types[instruction.typeIndex].id ||
                        registerType(instruction.operands[1]) !=
                            semantic::PrimitiveType::Object ||
                        registerTypeId(instruction.operands[1]) !=
                            module.types[instruction.typeIndex].id ||
                        resultType != semantic::PrimitiveType::Object ||
                        registerTypeId(instruction.result) !=
                            module.types[instruction.typeIndex].id) {
                        diagnostics.report(
                            "RS5183",
                            "invalid bytecode delegate combination",
                            {});
                    }
                    break;
                case Opcode::NegateInt:
                case Opcode::NegateLong:
                case Opcode::NegateDouble: {
                    const auto operandType = instruction.operands.empty()
                        ? semantic::PrimitiveType::Error
                        : registerType(instruction.operands.front());
                    const bool carrierMatches = instruction.opcode == Opcode::NegateInt
                        ? isIntCarrier(operandType)
                        : instruction.opcode == Opcode::NegateLong
                            ? isLongCarrier(operandType)
                            : isFloatCarrier(operandType);
                    if (instruction.operands.size() != 1 ||
                        !carrierMatches || resultType != operandType) {
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
                        !isIntCarrier(resultType) ||
                        registerType(instruction.operands[0]) != resultType ||
                        registerType(instruction.operands[1]) != resultType) {
                        diagnostics.report("RS5143", "invalid integer arithmetic", {});
                    }
                    break;
                case Opcode::AddLong:
                case Opcode::SubtractLong:
                case Opcode::MultiplyLong:
                case Opcode::DivideLong:
                case Opcode::RemainderLong:
                    if (instruction.operands.size() != 2 ||
                        !isLongCarrier(resultType) ||
                        registerType(instruction.operands[0]) != resultType ||
                        registerType(instruction.operands[1]) != resultType) {
                        diagnostics.report("RS5172", "invalid long arithmetic", {});
                    }
                    break;
                case Opcode::AddDouble:
                case Opcode::SubtractDouble:
                case Opcode::MultiplyDouble:
                case Opcode::DivideDouble:
                    if (instruction.operands.size() != 2 ||
                        !isFloatCarrier(resultType) ||
                        registerType(instruction.operands[0]) != resultType ||
                        registerType(instruction.operands[1]) != resultType) {
                        diagnostics.report("RS5173", "invalid double arithmetic", {});
                    }
                    break;
                case Opcode::LessInt:
                case Opcode::LessOrEqualInt:
                case Opcode::GreaterInt:
                case Opcode::GreaterOrEqualInt:
                    if (instruction.operands.size() != 2 ||
                        !isIntCarrier(registerType(instruction.operands[0])) ||
                        registerType(instruction.operands[1]) !=
                            registerType(instruction.operands[0]) ||
                        resultType != semantic::PrimitiveType::Bool) {
                        diagnostics.report("RS5144", "invalid integer comparison", {});
                    }
                    break;
                case Opcode::LessLong:
                case Opcode::LessOrEqualLong:
                case Opcode::GreaterLong:
                case Opcode::GreaterOrEqualLong:
                    if (instruction.operands.size() != 2 ||
                        !isLongCarrier(registerType(instruction.operands[0])) ||
                        registerType(instruction.operands[1]) != registerType(instruction.operands[0]) ||
                        resultType != semantic::PrimitiveType::Bool) {
                        diagnostics.report("RS5174", "invalid long comparison", {});
                    }
                    break;
                case Opcode::LessDouble:
                case Opcode::LessOrEqualDouble:
                case Opcode::GreaterDouble:
                case Opcode::GreaterOrEqualDouble:
                    if (instruction.operands.size() != 2 ||
                        !isFloatCarrier(registerType(instruction.operands[0])) ||
                        registerType(instruction.operands[1]) != registerType(instruction.operands[0]) ||
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
                case Opcode::AsyncEnter:
                    if (instruction.typeIndex >= module.types.size() ||
                        (module.types[instruction.typeIndex].kind != semantic::TypeKind::Struct &&
                         module.types[instruction.typeIndex].kind != semantic::TypeKind::Class) ||
                        (function.returnType != semantic::PrimitiveType::Struct &&
                         function.returnType != semantic::PrimitiveType::Object) ||
                        function.returnTypeId != module.types[instruction.typeIndex].id) {
                        diagnostics.report("RS5190", "invalid bytecode async.enter", {});
                    }
                    break;
                case Opcode::AwaitSuspend:
                    if (instruction.operands.size() != 1 ||
                        instruction.index >= function.localTypes.size() ||
                        (function.localTypes[instruction.index] != semantic::PrimitiveType::Struct &&
                         function.localTypes[instruction.index] != semantic::PrimitiveType::Object) ||
                        registerType(instruction.operands.front()) != function.localTypes[instruction.index] ||
                        instruction.typeIndex >= module.types.size() ||
                        instruction.integerImmediate < 0 ||
                        findBlockById(static_cast<BlockId>(instruction.integerImmediate)) == nullptr) {
                        diagnostics.report("RS5191", "invalid bytecode await.suspend", {});
                    }
                    break;
                case Opcode::AwaitResume:
                    if (instruction.index >= function.localTypes.size() ||
                        (function.localTypes[instruction.index] != semantic::PrimitiveType::Struct &&
                         function.localTypes[instruction.index] != semantic::PrimitiveType::Object) ||
                        instruction.typeIndex >= module.types.size()) {
                        diagnostics.report("RS5192", "invalid bytecode await.resume", {});
                    }
                    break;
                case Opcode::AsyncResolve:
                    if (instruction.operands.size() > 1 ||
                        instruction.typeIndex >= module.types.size() ||
                        (module.types[instruction.typeIndex].kind != semantic::TypeKind::Struct &&
                         module.types[instruction.typeIndex].kind != semantic::TypeKind::Class) ||
                        (resultType != semantic::PrimitiveType::Struct &&
                         resultType != semantic::PrimitiveType::Object) ||
                        registerTypeId(instruction.result) != module.types[instruction.typeIndex].id) {
                        diagnostics.report("RS5193", "invalid bytecode async.resolve", {});
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
                    !compatibleType(
                        registerType(block.terminator.value),
                        registerTypeId(block.terminator.value),
                        function.returnType,
                        function.returnTypeId,
                        typeDescriptors)) {
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
            case TerminatorKind::Throw:
                checkUse(block.terminator.value, block.id, useIndex);
                if (registerType(block.terminator.value) !=
                    semantic::PrimitiveType::Object) {
                    diagnostics.report(
                        "RS5176", "bytecode throw requires an object value", {});
                }
                break;
            }
        }
    }

    return diagnostics.items().size() == initialCount;
}

} // namespace realscript::bytecode
