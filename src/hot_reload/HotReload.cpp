#include "realscript/hot_reload/HotReload.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <map>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace realscript::hot_reload {
namespace {

struct FunctionView {
    const bytecode::Module* module = nullptr;
    const bytecode::Function* function = nullptr;
};
using FunctionMap = std::unordered_map<semantic::SymbolId, FunctionView>;
using TypeMap = std::unordered_map<semantic::SymbolId, const semantic::TypeSymbol*>;
using FormerNamesMap = std::unordered_map<std::string, std::vector<std::string>>;

std::string moduleSet(const std::vector<bytecode::Module>& modules) {
    std::vector<std::string> names;
    for (const auto& module : modules) names.push_back(module.name);
    std::sort(names.begin(), names.end());
    std::ostringstream output;
    for (const auto& name : names) output << name << '\n';
    return output.str();
}

FunctionMap functions(const std::vector<bytecode::Module>& modules) {
    FunctionMap result;
    for (const auto& module : modules) {
        for (const auto& function : module.functions) {
            result.emplace(function.symbolId, FunctionView{&module, &function});
        }
    }
    return result;
}

TypeMap types(const std::vector<bytecode::Module>& modules) {
    TypeMap result;
    for (const auto& module : modules) {
        for (const auto& type : module.types) result.emplace(type.id, &type);
    }
    return result;
}

bool sameField(const semantic::FieldSymbol& left, const semantic::FieldSymbol& right) {
    return left.name == right.name &&
        left.type == right.type &&
        left.typeName == right.typeName &&
        left.index == right.index &&
        left.staticField == right.staticField &&
        left.constField == right.constField;
}

bool sameMethodSignature(
    const semantic::FunctionSymbol& left,
    const semantic::FunctionSymbol& right) {
    if (left.id != right.id || left.name != right.name ||
        left.returnType != right.returnType ||
        left.returnTypeName != right.returnTypeName ||
        left.parameters.size() != right.parameters.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.parameters.size(); ++index) {
        const auto& first = left.parameters[index];
        const auto& second = right.parameters[index];
        if (first.type != second.type ||
            first.typeName != second.typeName ||
            first.storageType != second.storageType ||
            first.storageTypeName != second.storageTypeName ||
            first.modifier != second.modifier) {
            return false;
        }
    }
    return true;
}

bool sameType(const semantic::TypeSymbol& left, const semantic::TypeSymbol& right) {
    if (left.id != right.id || left.kind != right.kind ||
        left.baseTypeId != right.baseTypeId ||
        left.baseTypeName != right.baseTypeName ||
        left.moduleName != right.moduleName || left.name != right.name ||
        left.abstractType != right.abstractType ||
        left.sealedType != right.sealedType ||
        left.delegateType != right.delegateType ||
        left.interfaceType != right.interfaceType ||
        left.staticType != right.staticType ||
        left.virtualDispatchTable != right.virtualDispatchTable ||
        left.interfaceDispatchMaps != right.interfaceDispatchMaps ||
        left.fields.size() != right.fields.size() ||
        left.methods.size() != right.methods.size() ||
        left.enumMembers.size() != right.enumMembers.size()) return false;
    for (std::size_t index = 0; index < left.fields.size(); ++index) {
        if (!sameField(left.fields[index], right.fields[index])) return false;
    }
    for (std::size_t index = 0; index < left.enumMembers.size(); ++index) {
        if (left.enumMembers[index].name != right.enumMembers[index].name ||
            left.enumMembers[index].value != right.enumMembers[index].value) return false;
    }
    for (std::size_t index = 0; index < left.methods.size(); ++index) {
        if (!sameMethodSignature(
                left.methods[index], right.methods[index])) {
            return false;
        }
    }
    return true;
}

bool sameSignature(const bytecode::Function& left, const bytecode::Function& right) {
    return left.symbolId == right.symbolId && left.name == right.name &&
        left.returnType == right.returnType && left.returnTypeId == right.returnTypeId &&
        left.parameterTypes == right.parameterTypes &&
        left.parameterTypeIds == right.parameterTypeIds;
}

bool sameLanguageMetadata(
    const compiler::LanguageModuleMetadata& left,
    const compiler::LanguageModuleMetadata& right) {
    if (left.attributes.size() != right.attributes.size() ||
        left.interfaces.size() != right.interfaces.size() ||
        left.genericInstantiations.size() !=
            right.genericInstantiations.size() ||
        left.sequences.size() != right.sequences.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.attributes.size(); ++index) {
        const auto& first = left.attributes[index];
        const auto& second = right.attributes[index];
        if (first.target != second.target || first.name != second.name ||
            first.arguments.size() != second.arguments.size()) {
            return false;
        }
        for (std::size_t argument = 0;
             argument < first.arguments.size(); ++argument) {
            if (first.arguments[argument].name !=
                    second.arguments[argument].name ||
                first.arguments[argument].value !=
                    second.arguments[argument].value) {
                return false;
            }
        }
    }
    for (std::size_t index = 0; index < left.interfaces.size(); ++index) {
        if (left.interfaces[index].typeName !=
                right.interfaces[index].typeName ||
            left.interfaces[index].interfaces !=
                right.interfaces[index].interfaces) {
            return false;
        }
    }
    for (std::size_t index = 0;
         index < left.genericInstantiations.size(); ++index) {
        const auto& first = left.genericInstantiations[index];
        const auto& second = right.genericInstantiations[index];
        if (first.genericName != second.genericName ||
            first.arguments != second.arguments ||
            first.generatedName != second.generatedName) {
            return false;
        }
    }
    for (std::size_t index = 0; index < left.sequences.size(); ++index) {
        const auto& first = left.sequences[index];
        const auto& second = right.sequences[index];
        if (first.typeName != second.typeName ||
            first.name != second.name ||
            first.resultTypeName != second.resultTypeName ||
            first.callbacks != second.callbacks) {
            return false;
        }
    }
    return true;
}

void append(std::uint64_t& hash, std::uint64_t value) noexcept {
    hash ^= value;
    hash *= 1099511628211ull;
}

std::uint64_t functionBodyFingerprint(const bytecode::Module& module, const bytecode::Function& function) {
    std::uint64_t hash = 1469598103934665603ull;
    for (const auto& block : function.blocks) {
        append(hash, block.id);
        for (const auto& parameter : block.parameters) {
            append(hash, parameter.target);
            append(hash, static_cast<std::uint64_t>(parameter.type));
            append(hash, parameter.typeId);
        }
        for (const auto& instruction : block.instructions) {
            append(hash, static_cast<std::uint64_t>(instruction.opcode));
            append(hash, instruction.result);
            append(hash, instruction.index);
            if ((instruction.opcode == bytecode::Opcode::Call ||
                 instruction.opcode == bytecode::Opcode::NewDelegate) &&
                instruction.index < module.functionReferences.size()) {
                const auto& reference =
                    module.functionReferences[instruction.index];
                append(hash, reference.symbolId);
                append(hash, reference.virtualDispatch ? 1 : 0);
                append(hash, reference.virtualSlot);
                append(hash, reference.interfaceDispatch ? 1 : 0);
                append(hash, reference.interfaceTypeId);
                append(hash, reference.interfaceSlot);
            }
            append(hash, instruction.typeIndex);
            append(hash, static_cast<std::uint64_t>(instruction.elementType));
            append(hash, instruction.activeElementTypeId());
            append(hash, static_cast<std::uint64_t>(instruction.activeIntegerImmediate()));
            std::uint64_t bits = 0;
            const double activeDbl = instruction.activeDoubleImmediate();
            static_assert(sizeof(bits) == sizeof(activeDbl), "double size");
            std::memcpy(&bits, &activeDbl, sizeof(bits));
            append(hash, bits);
            append(hash, instruction.boolImmediate ? 1 : 0);
            append(hash, instruction.checkedArithmetic ? 1 : 0);
            for (const auto value : instruction.activeStringImmediate()) append(hash, static_cast<unsigned char>(value));
            for (const auto operand : instruction.operands) append(hash, operand);
        }
        append(hash, static_cast<std::uint64_t>(block.terminator.kind));
        append(hash, block.terminator.condition);
        append(hash, block.terminator.value);
        append(hash, block.terminator.target);
        append(hash, block.terminator.falseTarget);
        for (const auto argument : block.terminator.arguments) append(hash, argument);
        for (const auto argument : block.terminator.falseArguments) append(hash, argument);
    }
    for (const auto& handler : function.exceptionHandlers) {
        append(hash, handler.catchTypeId);
        append(hash, handler.handlerBlock);
        append(hash, handler.exceptionLocal);
        append(hash, handler.protectedBlocks.size());
        for (const auto block : handler.protectedBlocks) append(hash, block);
    }
    return hash;
}

bool isFormerlySerializedAsName(std::string_view attrName) noexcept {
    if (attrName == "FormerlySerializedAs" ||
        attrName == "FormerlySerializedAsAttribute") {
        return true;
    }
    const auto sep = attrName.rfind("::");
    if (sep != std::string_view::npos) {
        const auto tail = attrName.substr(sep + 2);
        return tail == "FormerlySerializedAs" ||
            tail == "FormerlySerializedAsAttribute";
    }
    return false;
}

std::string unquoteAttributeString(const std::string& raw) {
    std::size_t begin = 0;
    std::size_t end = raw.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(raw[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(raw[end - 1]))) {
        --end;
    }
    const std::string trimmed = raw.substr(begin, end - begin);
    if (trimmed.size() >= 2 && trimmed.front() == '"' && trimmed.back() == '"') {
        std::string unescaped;
        unescaped.reserve(trimmed.size() - 2);
        for (std::size_t i = 1; i + 1 < trimmed.size(); ++i) {
            if (trimmed[i] == '\\' && i + 2 < trimmed.size()) {
                ++i;
                switch (trimmed[i]) {
                case 'n': unescaped.push_back('\n'); break;
                case 'r': unescaped.push_back('\r'); break;
                case 't': unescaped.push_back('\t'); break;
                default: unescaped.push_back(trimmed[i]); break;
                }
            } else {
                unescaped.push_back(trimmed[i]);
            }
        }
        return unescaped;
    }
    return trimmed;
}

std::string extractFormerlySerializedAsValue(
    const compiler::LanguageAttributeRecord& record) {
    static const char* const kPreferredKeys[] = {"oldName", "name", "arg0"};
    for (const char* key : kPreferredKeys) {
        for (const auto& arg : record.arguments) {
            if (arg.name == key) {
                auto parsed = unquoteAttributeString(arg.value);
                if (!parsed.empty()) return parsed;
            }
        }
    }
    for (const auto& arg : record.arguments) {
        auto parsed = unquoteAttributeString(arg.value);
        if (!parsed.empty()) return parsed;
    }
    return {};
}

FormerNamesMap collectFormerlySerializedAsMap(
    const std::vector<bytecode::Module>& modules) {
    FormerNamesMap result;
    for (const auto& module : modules) {
        for (const auto& attr : module.languageMetadata.attributes) {
            if (!isFormerlySerializedAsName(attr.name)) continue;
            auto oldName = extractFormerlySerializedAsValue(attr);
            if (oldName.empty()) continue;
            auto& list = result[attr.target];
            if (std::find(list.begin(), list.end(), oldName) == list.end()) {
                list.push_back(std::move(oldName));
            }
        }
    }
    return result;
}

std::vector<std::string> lookupFormerFieldNames(
    const FormerNamesMap& map,
    const std::string& ownerCanonicalName,
    const semantic::FieldSymbol& field) {
    std::vector<std::string> candidates;
    const auto appendUnique = [&](const std::string& value) {
        if (!value.empty() &&
            std::find(candidates.begin(), candidates.end(), value) == candidates.end()) {
            candidates.push_back(value);
        }
    };
    const auto appendFromTarget = [&](const std::string& target, bool asAutoPropBacking) {
        const auto it = map.find(target);
        if (it == map.end()) return;
        for (const auto& name : it->second) {
            if (asAutoPropBacking) {
                appendUnique("$" + name);
                appendUnique(name);
            } else {
                appendUnique(name);
            }
        }
    };

    if (!field.declaringTypeName.empty()) {
        appendFromTarget(field.declaringTypeName + "::field:" + field.name, false);
    }
    if (field.declaringTypeName != ownerCanonicalName) {
        appendFromTarget(ownerCanonicalName + "::field:" + field.name, false);
    }
    if (field.synthetic && field.name.size() > 1 && field.name.front() == '$') {
        const std::string propName = field.name.substr(1);
        if (!field.declaringTypeName.empty()) {
            appendFromTarget(field.declaringTypeName + "::property:" + propName, true);
        }
        if (field.declaringTypeName != ownerCanonicalName) {
            appendFromTarget(ownerCanonicalName + "::property:" + propName, true);
        }
    }
    return candidates;
}

semantic::SymbolId exactFieldTypeId(const semantic::FieldSymbol& field) {
    return semantic::isExactType(field.type) && !field.typeName.empty()
        ? semantic::stableTypeId(field.typeName)
        : 0;
}

semantic::SymbolId resolveFieldSymbolId(
    const std::string& ownerCanonicalName,
    const semantic::FieldSymbol& field) {
    if (field.id != 0) return field.id;
    const std::string& owner = field.declaringTypeName.empty()
        ? ownerCanonicalName
        : field.declaringTypeName;
    return semantic::stableTypeId(owner + "::field:" + field.name);
}

bool fieldMayHoldManagedReferences(
    const semantic::FieldSymbol& field,
    semantic::SymbolId fieldTypeId) noexcept {
    return field.type == semantic::PrimitiveType::String ||
        field.type == semantic::PrimitiveType::Object ||
        field.type == semantic::PrimitiveType::Array ||
        (field.type == semantic::PrimitiveType::Struct &&
         !runtime::makeDefaultInlineStructValue(fieldTypeId).has_value());
}

runtime::Value makeFieldDefaultValue(
    const semantic::FieldSymbol& field,
    const TypeMap& allTypes,
    std::size_t depth = 0) {
    const auto fieldTid = exactFieldTypeId(field);
    if (field.hasConstantValue) {
        if (const auto* bv = std::get_if<bool>(&field.constantValue)) {
            if (field.type == semantic::PrimitiveType::Bool) {
                return runtime::Value{*bv};
            }
        } else if (const auto* iv = std::get_if<std::int64_t>(&field.constantValue)) {
            if (field.type == semantic::PrimitiveType::Enum) {
                return runtime::Value{runtime::EnumValue{fieldTid, *iv}};
            }
            if (field.type == semantic::PrimitiveType::ULong) {
                return runtime::Value{runtime::ULongValue{static_cast<std::uint64_t>(*iv)}};
            }
            if (field.type == semantic::PrimitiveType::Long) {
                return runtime::Value{runtime::LongValue{*iv}};
            }
            if (field.type == semantic::PrimitiveType::Int) {
                return runtime::Value{*iv};
            }
            if (semantic::isNumericType(field.type)) {
                runtime::Value converted{};
                if (runtime::tryConvertNumeric(
                        runtime::Value{runtime::LongValue{*iv}},
                        field.type,
                        converted,
                        /*checkedArithmetic=*/true)) {
                    return converted;
                }
            }
        } else if (const auto* dv = std::get_if<double>(&field.constantValue)) {
            if (field.type == semantic::PrimitiveType::Double) {
                return runtime::Value{*dv};
            }
            if (field.type == semantic::PrimitiveType::Float) {
                return runtime::Value{runtime::FloatValue{static_cast<float>(*dv)}};
            }
            if (semantic::isNumericType(field.type)) {
                runtime::Value converted{};
                if (runtime::tryConvertNumeric(
                        runtime::Value{*dv},
                        field.type,
                        converted,
                        /*checkedArithmetic=*/true)) {
                    return converted;
                }
            }
        } else if (const auto* sv = std::get_if<std::string>(&field.constantValue)) {
            if (field.type == semantic::PrimitiveType::String) {
                return runtime::Value{runtime::StringValue{*sv}};
            }
            if (field.type == semantic::PrimitiveType::Char && !sv->empty()) {
                return runtime::Value{runtime::CharValue{
                    static_cast<char16_t>(static_cast<unsigned char>((*sv)[0]))}};
            }
        }
    }

    switch (field.type) {
    case semantic::PrimitiveType::Bool:
        return runtime::Value{false};
    case semantic::PrimitiveType::Byte:
        return runtime::Value{runtime::ByteValue{0}};
    case semantic::PrimitiveType::SByte:
        return runtime::Value{runtime::SByteValue{0}};
    case semantic::PrimitiveType::Short:
        return runtime::Value{runtime::ShortValue{0}};
    case semantic::PrimitiveType::UShort:
        return runtime::Value{runtime::UShortValue{0}};
    case semantic::PrimitiveType::Int:
        return runtime::Value{std::int64_t{0}};
    case semantic::PrimitiveType::UInt:
        return runtime::Value{runtime::UIntValue{0}};
    case semantic::PrimitiveType::Long:
        return runtime::Value{runtime::LongValue{0}};
    case semantic::PrimitiveType::ULong:
        return runtime::Value{runtime::ULongValue{0}};
    case semantic::PrimitiveType::Float:
        return runtime::Value{runtime::FloatValue{0.0f}};
    case semantic::PrimitiveType::Double:
        return runtime::Value{0.0};
    case semantic::PrimitiveType::Char:
        return runtime::Value{runtime::CharValue{0}};
    case semantic::PrimitiveType::String:
        return runtime::Value{runtime::NullString{}};
    case semantic::PrimitiveType::Object:
        return runtime::Value{runtime::NullObject{}};
    case semantic::PrimitiveType::Array:
        return runtime::Value{runtime::NullArray{}};
    case semantic::PrimitiveType::Enum:
        return runtime::Value{runtime::EnumValue{fieldTid, 0}};
    case semantic::PrimitiveType::Handle: {
        runtime::NativeHandle handle{};
        handle.typeId = fieldTid;
        return runtime::Value{handle};
    }
    case semantic::PrimitiveType::Struct: {
        if (const auto inlined = runtime::makeDefaultInlineStructValue(fieldTid)) {
            return *inlined;
        }
        if (depth > 64 || fieldTid == 0) {
            return runtime::Value{runtime::StructValue{}};
        }
        const auto it = allTypes.find(fieldTid);
        if (it == allTypes.end() || it->second->kind != semantic::TypeKind::Struct) {
            return runtime::Value{runtime::StructValue{}};
        }
        const auto& structType = *it->second;
        if (structType.fields.size() <= runtime::StructValue::MaxInlineFields) {
            std::array<runtime::Value, runtime::StructValue::MaxInlineFields> stackFields{};
            for (std::size_t i = 0; i < structType.fields.size(); ++i) {
                stackFields[i] = makeFieldDefaultValue(structType.fields[i], allTypes, depth + 1);
            }
            return runtime::Value{runtime::makeStructValueFromFields(
                fieldTid, runtime::ValueSpan(stackFields.data(), structType.fields.size()))};
        }
        std::vector<runtime::Value> heapFields;
        heapFields.reserve(structType.fields.size());
        for (const auto& sf : structType.fields) {
            heapFields.push_back(makeFieldDefaultValue(sf, allTypes, depth + 1));
        }
        return runtime::Value{runtime::makeStructValueFromFields(fieldTid, std::move(heapFields))};
    }
    case semantic::PrimitiveType::Null:
    case semantic::PrimitiveType::Void:
    case semantic::PrimitiveType::Error:
        break;
    }
    return runtime::Value{};
}

bool isTypeAssignableInMap(
    semantic::SymbolId concreteTid,
    semantic::SymbolId targetTid,
    const TypeMap& typeMap) noexcept {
    if (concreteTid == 0) return false;
    if (targetTid == 0 || concreteTid == targetTid) return true;
    auto cur = concreteTid;
    for (std::size_t depth = 0; cur != 0 && depth < 256; ++depth) {
        if (cur == targetTid) return true;
        const auto it = typeMap.find(cur);
        if (it == typeMap.end()) break;
        for (const auto& imap : it->second->interfaceDispatchMaps) {
            if (imap.interfaceTypeId == targetTid) return true;
        }
        cur = it->second->baseTypeId;
    }
    return false;
}

std::unordered_map<semantic::SymbolId, std::vector<semantic::SymbolId>>
buildAssignableConcreteTypesMap(const TypeMap& typeMap) {
    std::vector<semantic::SymbolId> concreteClassIds;
    concreteClassIds.reserve(typeMap.size());
    for (const auto& [tid, type] : typeMap) {
        if (tid != 0 &&
            type->kind == semantic::TypeKind::Class &&
            !type->interfaceType &&
            !type->staticType) {
            concreteClassIds.push_back(tid);
        }
    }
    std::sort(concreteClassIds.begin(), concreteClassIds.end());

    std::unordered_map<semantic::SymbolId, std::vector<semantic::SymbolId>> result;
    for (const auto& [targetTid, targetType] : typeMap) {
        (void)targetType;
        if (targetTid == 0) continue;
        std::vector<semantic::SymbolId> assignable;
        for (const auto candTid : concreteClassIds) {
            if (isTypeAssignableInMap(candTid, targetTid, typeMap)) {
                assignable.push_back(candTid);
            }
        }
        result.emplace(targetTid, std::move(assignable));
    }
    return result;
}

struct MethodDispatchInfo {
    std::int64_t flags = 0;
    std::uint32_t slot = 0;
};

std::unordered_map<semantic::SymbolId, MethodDispatchInfo>
collectMethodDispatchInfo(
    const std::vector<bytecode::Module>& modules,
    const TypeMap& typeMap) {
    std::unordered_map<semantic::SymbolId, MethodDispatchInfo> info;
    // 1. Authoritative virtual slots from each class's virtualDispatchTable
    for (const auto& [tid, type] : typeMap) {
        (void)tid;
        if (type->kind == semantic::TypeKind::Class && !type->interfaceType) {
            for (std::uint32_t slot = 0;
                 slot < static_cast<std::uint32_t>(type->virtualDispatchTable.size());
                 ++slot) {
                const auto vSym = type->virtualDispatchTable[slot];
                if (vSym != 0) {
                    info[vSym] = MethodDispatchInfo{1, slot};
                }
            }
        }
    }
    // 2. Interface method slots & remaining non-virtual methods
    for (const auto& [tid, type] : typeMap) {
        (void)tid;
        if (type->interfaceType) {
            for (std::uint32_t idx = 0;
                 idx < static_cast<std::uint32_t>(type->methods.size());
                 ++idx) {
                const auto& method = type->methods[idx];
                if (method.id == 0) continue;
                const std::uint32_t iSlot =
                    method.interfaceSlot != std::numeric_limits<std::uint32_t>::max()
                        ? method.interfaceSlot
                        : idx;
                info[method.id] = MethodDispatchInfo{2, iSlot};
            }
            continue;
        }
        for (const auto& method : type->methods) {
            if (method.id == 0 || info.find(method.id) != info.end()) continue;
            if (method.virtualMethod || method.overrideMethod || method.abstractMethod) {
                const std::uint32_t vSlot =
                    method.virtualSlot != std::numeric_limits<std::uint32_t>::max()
                        ? method.virtualSlot
                        : 0u;
                info.emplace(method.id, MethodDispatchInfo{1, vSlot});
            } else {
                info.emplace(method.id, MethodDispatchInfo{0, 0u});
            }
        }
    }
    for (const auto& module : modules) {
        for (const auto& fn : module.functions) {
            if (fn.symbolId != 0 && info.find(fn.symbolId) == info.end()) {
                info.emplace(fn.symbolId, MethodDispatchInfo{0, 0u});
            }
        }
    }
    return info;
}

std::vector<runtime::DelegateMethodSlotUpdate> computeDelegateSlotUpdates(
    const std::vector<bytecode::Module>& oldModules,
    const std::vector<bytecode::Module>& newModules,
    const TypeMap& oldTypes,
    const TypeMap& newTypes) {
    const auto oldDispatch = collectMethodDispatchInfo(oldModules, oldTypes);
    const auto newDispatch = collectMethodDispatchInfo(newModules, newTypes);
    std::vector<runtime::DelegateMethodSlotUpdate> updates;
    for (const auto& [symId, oldInfo] : oldDispatch) {
        const auto found = newDispatch.find(symId);
        if (found == newDispatch.end()) {
            updates.push_back(runtime::DelegateMethodSlotUpdate{
                symId, 0, 0u, /*removed=*/true});
        } else if (found->second.flags != oldInfo.flags ||
                   found->second.slot != oldInfo.slot) {
            updates.push_back(runtime::DelegateMethodSlotUpdate{
                symId,
                found->second.flags,
                found->second.slot,
                /*removed=*/false});
        }
    }
    std::sort(
        updates.begin(),
        updates.end(),
        [](const runtime::DelegateMethodSlotUpdate& a,
           const runtime::DelegateMethodSlotUpdate& b) {
            return a.methodSymbolId < b.methodSymbolId;
        });
    return updates;
}

bool buildStructuralTypeMigrations(
    const std::vector<bytecode::Module>& oldModules,
    const std::vector<bytecode::Module>& newModules,
    const TypeMap& oldTypes,
    const TypeMap& newTypes,
    ReloadPlan& plan) {
    for (const auto& [id, oldType] : oldTypes) {
        const auto found = newTypes.find(id);
        if (found == newTypes.end()) continue;
        const auto* newType = found->second;
        if (oldType->kind != newType->kind ||
            oldType->delegateType != newType->delegateType ||
            oldType->interfaceType != newType->interfaceType ||
            oldType->staticType != newType->staticType) {
            plan.issues.push_back({
                ReloadIssueKind::TypeLayoutChanged,
                semantic::canonicalTypeName(*oldType),
                "cannot change type category during structural hot reload",
            });
            return false;
        }
    }

    const auto formerNames = collectFormerlySerializedAsMap(newModules);
    const auto oldAssignable = buildAssignableConcreteTypesMap(oldTypes);
    const auto newAssignable = buildAssignableConcreteTypesMap(newTypes);
    const auto delegateUpdates = computeDelegateSlotUpdates(
        oldModules, newModules, oldTypes, newTypes);

    std::vector<semantic::SymbolId> allTypeIds;
    allTypeIds.reserve(oldTypes.size() + newTypes.size());
    for (const auto& [id, t] : oldTypes) {
        (void)t;
        allTypeIds.push_back(id);
    }
    for (const auto& [id, t] : newTypes) {
        (void)t;
        if (oldTypes.find(id) == oldTypes.end()) {
            allTypeIds.push_back(id);
        }
    }
    std::sort(allTypeIds.begin(), allTypeIds.end());

    bool attachedDelegateUpdates = false;

    for (const auto id : allTypeIds) {
        const auto oldIt = oldTypes.find(id);
        const auto newIt = newTypes.find(id);

        if (oldIt != oldTypes.end() && newIt == newTypes.end()) {
            const auto& oldType = *oldIt->second;
            const std::string canonicalOld = semantic::canonicalTypeName(oldType);
            runtime::TypeLayoutMigration mig;
            mig.typeId = id;
            mig.typeKind = oldType.kind;
            mig.canonicalName = canonicalOld;
            mig.removedType = true;
            mig.delegateType = oldType.delegateType;
            mig.interfaceType = oldType.interfaceType;
            mig.oldFieldCount = oldType.fields.size();
            for (const auto& f : oldType.fields) {
                if (f.staticField && !f.constField) {
                    const auto sid = resolveFieldSymbolId(canonicalOld, f);
                    mig.removedStaticFieldIds.push_back(sid);
                    const auto ownSid = semantic::stableTypeId(canonicalOld + "::field:" + f.name);
                    if (ownSid != sid) mig.removedStaticFieldIds.push_back(ownSid);
                }
            }
            mig.removedStaticFieldIds.push_back(
                semantic::stableTypeId(canonicalOld + "::cctor_guard"));
            plan.typeMigrations.push_back(std::move(mig));
            continue;
        }

        if (oldIt == oldTypes.end() && newIt != newTypes.end()) {
            const auto& newType = *newIt->second;
            const std::string canonicalNew = semantic::canonicalTypeName(newType);
            runtime::TypeLayoutMigration mig;
            mig.typeId = id;
            mig.typeKind = newType.kind;
            mig.canonicalName = canonicalNew;
            mig.addedType = true;
            mig.delegateType = newType.delegateType;
            mig.interfaceType = newType.interfaceType;
            mig.newFieldCount = newType.fields.size();
            mig.virtualDispatchTable = newType.virtualDispatchTable;
            mig.interfaceDispatchMaps = newType.interfaceDispatchMaps;
            for (std::size_t idx = 0; idx < newType.fields.size(); ++idx) {
                const auto& nf = newType.fields[idx];
                const auto nfTid = exactFieldTypeId(nf);
                if (fieldMayHoldManagedReferences(nf, nfTid)) {
                    mig.newReferenceFields.push_back(idx);
                }
                if (nf.staticField && !nf.constField) {
                    runtime::StaticFieldMigrationOp sop;
                    sop.kind = runtime::FieldMigrationKind::Default;
                    sop.hasConstantInitializer = nf.hasConstantValue;
                    sop.targetFieldId = resolveFieldSymbolId(canonicalNew, nf);
                    sop.targetType = nf.type;
                    sop.targetTypeId = nfTid;
                    sop.fieldName = nf.name;
                    sop.defaultValue = makeFieldDefaultValue(nf, newTypes);
                    mig.staticFieldOps.push_back(std::move(sop));
                }
            }
            plan.typeMigrations.push_back(std::move(mig));
            continue;
        }

        const auto& oldType = *oldIt->second;
        const auto& newType = *newIt->second;
        const std::string canonicalOld = semantic::canonicalTypeName(oldType);
        const std::string canonicalNew = semantic::canonicalTypeName(newType);

        bool assignableSetChangedForField = false;
        if (!newType.delegateType && !newType.interfaceType) {
            for (const auto& nf : newType.fields) {
                if (nf.type == semantic::PrimitiveType::Object && !nf.typeName.empty()) {
                    const auto fTid = exactFieldTypeId(nf);
                    const auto oA = oldAssignable.find(fTid);
                    const auto nA = newAssignable.find(fTid);
                    const auto emptyVec = std::vector<semantic::SymbolId>{};
                    const auto& oVec = (oA != oldAssignable.end()) ? oA->second : emptyVec;
                    const auto& nVec = (nA != newAssignable.end()) ? nA->second : emptyVec;
                    if (oVec != nVec) {
                        assignableSetChangedForField = true;
                        break;
                    }
                }
            }
        }

        const bool typeChanged = !sameType(oldType, newType);
        const bool needsDelegateEntry = newType.delegateType && !delegateUpdates.empty();
        if (!typeChanged && !assignableSetChangedForField && !needsDelegateEntry) {
            continue;
        }

        runtime::TypeLayoutMigration mig;
        mig.typeId = id;
        mig.typeKind = newType.kind;
        mig.canonicalName = canonicalNew;
        mig.delegateType = newType.delegateType;
        mig.interfaceType = newType.interfaceType;
        mig.oldFieldCount = oldType.fields.size();
        mig.newFieldCount = newType.fields.size();
        mig.virtualDispatchTable = newType.virtualDispatchTable;
        mig.interfaceDispatchMaps = newType.interfaceDispatchMaps;
        mig.dispatchTableChanged =
            (oldType.virtualDispatchTable != newType.virtualDispatchTable) ||
            (oldType.interfaceDispatchMaps != newType.interfaceDispatchMaps);

        if (newType.delegateType) {
            if (!delegateUpdates.empty() && !attachedDelegateUpdates) {
                mig.delegateSlotUpdates = delegateUpdates;
                attachedDelegateUpdates = true;
            }
            plan.typeMigrations.push_back(std::move(mig));
            continue;
        }

        if (newType.interfaceType || newType.kind == semantic::TypeKind::Enum) {
            plan.typeMigrations.push_back(std::move(mig));
            continue;
        }

        std::unordered_map<std::string, const semantic::FieldSymbol*> oldInstanceByName;
        std::unordered_map<std::string, const semantic::FieldSymbol*> oldStaticByName;
        for (const auto& of : oldType.fields) {
            if (of.staticField) {
                if (!of.constField) {
                    oldStaticByName.emplace(of.name, &of);
                }
            } else {
                oldInstanceByName.emplace(of.name, &of);
            }
        }

        std::unordered_set<std::string> claimedOldInstanceNames;
        std::unordered_set<std::string> claimedOldStaticNames;
        for (const auto& nf : newType.fields) {
            if (nf.staticField) {
                if (!nf.constField && oldStaticByName.find(nf.name) != oldStaticByName.end()) {
                    claimedOldStaticNames.insert(nf.name);
                }
            } else if (oldInstanceByName.find(nf.name) != oldInstanceByName.end()) {
                claimedOldInstanceNames.insert(nf.name);
            }
        }

        std::unordered_set<semantic::SymbolId> retainedStaticIds;
        mig.fieldOps.reserve(newType.fields.size());

        for (std::size_t idx = 0; idx < newType.fields.size(); ++idx) {
            const auto& nf = newType.fields[idx];
            const auto nfTid = exactFieldTypeId(nf);
            const runtime::Value defVal = makeFieldDefaultValue(nf, newTypes);

            if (fieldMayHoldManagedReferences(nf, nfTid)) {
                mig.newReferenceFields.push_back(idx);
            }

            if (nf.staticField) {
                runtime::FieldMigrationOp slotPlaceholder;
                slotPlaceholder.kind = runtime::FieldMigrationKind::Default;
                slotPlaceholder.targetType = nf.type;
                slotPlaceholder.targetTypeId = nfTid;
                slotPlaceholder.fieldName = nf.name;
                slotPlaceholder.defaultValue = defVal;
                mig.fieldOps.push_back(std::move(slotPlaceholder));

                if (!nf.constField) {
                    runtime::StaticFieldMigrationOp sop;
                    sop.targetFieldId = resolveFieldSymbolId(canonicalNew, nf);
                    sop.targetType = nf.type;
                    sop.targetTypeId = nfTid;
                    sop.fieldName = nf.name;
                    sop.defaultValue = defVal;
                    sop.hasConstantInitializer = nf.hasConstantValue;
                    if (nf.type == semantic::PrimitiveType::Object && nfTid != 0) {
                        const auto aIt = newAssignable.find(nfTid);
                        if (aIt != newAssignable.end()) sop.assignableTypeIds = aIt->second;
                    }

                    const semantic::FieldSymbol* matchedOldStatic = nullptr;
                    const auto exactIt = oldStaticByName.find(nf.name);
                    if (exactIt != oldStaticByName.end()) {
                        matchedOldStatic = exactIt->second;
                    } else {
                        for (const auto& former : lookupFormerFieldNames(formerNames, canonicalNew, nf)) {
                            const auto fIt = oldStaticByName.find(former);
                            if (fIt != oldStaticByName.end() &&
                                claimedOldStaticNames.insert(former).second) {
                                matchedOldStatic = fIt->second;
                                break;
                            }
                        }
                    }

                    if (matchedOldStatic) {
                        sop.sourceFieldId = resolveFieldSymbolId(canonicalOld, *matchedOldStatic);
                        sop.renamed = (matchedOldStatic->name != nf.name);
                        if (sop.renamed) sop.formerName = matchedOldStatic->name;
                        if (matchedOldStatic->type == nf.type &&
                            (matchedOldStatic->typeName == nf.typeName ||
                             nf.type == semantic::PrimitiveType::Object)) {
                            sop.kind = runtime::FieldMigrationKind::Copy;
                        } else if (semantic::isNumericType(matchedOldStatic->type) &&
                                   semantic::isNumericType(nf.type)) {
                            sop.kind = runtime::FieldMigrationKind::ConvertNumeric;
                            sop.checkedArithmetic = true;
                        } else {
                            sop.kind = runtime::FieldMigrationKind::Default;
                        }
                    } else {
                        sop.kind = runtime::FieldMigrationKind::Default;
                    }
                    retainedStaticIds.insert(sop.targetFieldId);
                    mig.staticFieldOps.push_back(std::move(sop));
                }
                continue;
            }

            runtime::FieldMigrationOp fop;
            fop.targetType = nf.type;
            fop.targetTypeId = nfTid;
            fop.fieldName = nf.name;
            fop.defaultValue = defVal;
            if (nf.type == semantic::PrimitiveType::Object && nfTid != 0) {
                const auto aIt = newAssignable.find(nfTid);
                if (aIt != newAssignable.end()) fop.assignableTypeIds = aIt->second;
            }

            const semantic::FieldSymbol* matchedOldInst = nullptr;
            const auto exactIt = oldInstanceByName.find(nf.name);
            if (exactIt != oldInstanceByName.end()) {
                matchedOldInst = exactIt->second;
            } else {
                for (const auto& former : lookupFormerFieldNames(formerNames, canonicalNew, nf)) {
                    const auto fIt = oldInstanceByName.find(former);
                    if (fIt != oldInstanceByName.end() &&
                        claimedOldInstanceNames.insert(former).second) {
                        matchedOldInst = fIt->second;
                        break;
                    }
                }
            }

            if (matchedOldInst) {
                fop.sourceIndex = matchedOldInst->index;
                fop.renamed = (matchedOldInst->name != nf.name);
                if (fop.renamed) fop.formerName = matchedOldInst->name;
                if (matchedOldInst->type == nf.type &&
                    (matchedOldInst->typeName == nf.typeName ||
                     nf.type == semantic::PrimitiveType::Object)) {
                    fop.kind = runtime::FieldMigrationKind::Copy;
                } else if (semantic::isNumericType(matchedOldInst->type) &&
                           semantic::isNumericType(nf.type)) {
                    fop.kind = runtime::FieldMigrationKind::ConvertNumeric;
                    fop.checkedArithmetic = true;
                } else {
                    fop.kind = runtime::FieldMigrationKind::Default;
                }
            } else {
                fop.kind = runtime::FieldMigrationKind::Default;
            }
            mig.fieldOps.push_back(std::move(fop));
        }

        for (const auto& of : oldType.fields) {
            if (of.staticField && !of.constField) {
                const auto oldSid = resolveFieldSymbolId(canonicalOld, of);
                if (retainedStaticIds.find(oldSid) == retainedStaticIds.end()) {
                    mig.removedStaticFieldIds.push_back(oldSid);
                }
                const auto ownOldSid = semantic::stableTypeId(canonicalOld + "::field:" + of.name);
                if (ownOldSid != oldSid &&
                    retainedStaticIds.find(ownOldSid) == retainedStaticIds.end()) {
                    mig.removedStaticFieldIds.push_back(ownOldSid);
                }
            }
        }

        bool layoutDiffers = (oldType.fields.size() != newType.fields.size()) ||
            assignableSetChangedForField;
        if (!layoutDiffers) {
            for (std::size_t idx = 0; idx < newType.fields.size(); ++idx) {
                const auto& of = oldType.fields[idx];
                const auto& nf = newType.fields[idx];
                if (of.staticField != nf.staticField ||
                    of.constField != nf.constField) {
                    layoutDiffers = true;
                    break;
                }
                if (!nf.staticField) {
                    const auto& op = mig.fieldOps[idx];
                    if (op.kind != runtime::FieldMigrationKind::Copy ||
                        op.sourceIndex != idx ||
                        op.renamed ||
                        of.type != nf.type ||
                        of.typeName != nf.typeName) {
                        layoutDiffers = true;
                        break;
                    }
                }
            }
        }
        mig.instanceLayoutChanged = layoutDiffers;
        plan.typeMigrations.push_back(std::move(mig));
    }

    if (!delegateUpdates.empty() && !attachedDelegateUpdates) {
        if (!plan.typeMigrations.empty()) {
            plan.typeMigrations.front().delegateSlotUpdates = delegateUpdates;
        } else {
            runtime::TypeLayoutMigration carrier;
            carrier.delegateType = true;
            carrier.delegateSlotUpdates = delegateUpdates;
            plan.typeMigrations.push_back(std::move(carrier));
        }
    }
    return true;
}

} // namespace

ReloadPlan prepare(
    const runtime::ProgramImage& current,
    std::vector<bytecode::Module> replacementModules,
    ReloadOptions options) {
    ReloadPlan plan;
    runtime::RuntimeError error;
    auto linked = runtime::ProgramImage::link(std::move(replacementModules), error);
    if (!linked) {
        plan.issues.push_back({ReloadIssueKind::InvalidProgram, {}, error.message});
        return plan;
    }

    const auto& oldModules = current.modules();
    const auto& newModules = linked->modules();
    if (moduleSet(oldModules) != moduleSet(newModules)) {
        if (!options.allowModuleSetChanges && !options.allowStructuralChanges) {
            plan.issues.push_back({
                ReloadIssueKind::ModuleSetChanged,
                {},
                "hot reload cannot add or remove modules",
            });
        } else {
            plan.structuralMigrationRequired = true;
        }
    }
    for (const auto& oldModule : oldModules) {
        const auto replacement = std::find_if(
            newModules.begin(), newModules.end(),
            [&](const bytecode::Module& module) {
                return module.name == oldModule.name;
            });
        if (replacement != newModules.end() &&
            !sameLanguageMetadata(
                oldModule.languageMetadata,
                replacement->languageMetadata)) {
            if (!options.allowStructuralChanges) {
                plan.issues.push_back({
                    ReloadIssueKind::LanguageMetadataChanged,
                    oldModule.name,
                    "language metadata changed during body-only hot reload",
                });
            } else {
                plan.structuralMigrationRequired = true;
            }
        }
    }

    const auto oldTypes = types(oldModules);
    const auto newTypes = types(newModules);
    if (oldTypes.size() != newTypes.size() && !options.allowStructuralChanges) {
        plan.issues.push_back({
            ReloadIssueKind::TypeLayoutChanged,
            {},
            "hot reload cannot add or remove types",
        });
    }
    for (const auto& [id, oldType] : oldTypes) {
        const auto found = newTypes.find(id);
        if (found == newTypes.end()) {
            plan.removedTypes.push_back(id);
            plan.structuralMigrationRequired = true;
            if (!options.allowStructuralChanges) {
                plan.issues.push_back({
                    ReloadIssueKind::TypeLayoutChanged,
                    semantic::canonicalTypeName(*oldType),
                    "type layout changed during body-only hot reload",
                });
            }
        } else if (!sameType(*oldType, *found->second)) {
            plan.layoutChangedTypes.push_back(id);
            plan.structuralMigrationRequired = true;
            if (!options.allowStructuralChanges) {
                plan.issues.push_back({
                    ReloadIssueKind::TypeLayoutChanged,
                    semantic::canonicalTypeName(*oldType),
                    "type layout changed during body-only hot reload",
                });
            }
        }
    }
    for (const auto& [id, newType] : newTypes) {
        (void)newType;
        if (oldTypes.find(id) == oldTypes.end()) {
            plan.addedTypes.push_back(id);
            plan.structuralMigrationRequired = true;
        }
    }
    std::sort(plan.removedTypes.begin(), plan.removedTypes.end());
    std::sort(plan.layoutChangedTypes.begin(), plan.layoutChangedTypes.end());
    std::sort(plan.addedTypes.begin(), plan.addedTypes.end());

    const auto oldFunctions = functions(oldModules);
    const auto newFunctions = functions(newModules);
    if (oldFunctions.size() != newFunctions.size() &&
        !options.allowStructuralChanges &&
        !options.allowAddedFunctions) {
        plan.issues.push_back({
            ReloadIssueKind::FunctionSetChanged,
            {},
            "hot reload cannot add or remove functions",
        });
    }
    for (const auto& [id, oldFunction] : oldFunctions) {
        const auto found = newFunctions.find(id);
        if (found == newFunctions.end()) {
            plan.removedFunctions.push_back(id);
            plan.structuralMigrationRequired = true;
            if (!options.allowStructuralChanges) {
                plan.issues.push_back({
                    ReloadIssueKind::FunctionSetChanged,
                    oldFunction.function->name,
                    "function was removed during hot reload",
                });
            }
            continue;
        }
        if (!sameSignature(*oldFunction.function, *found->second.function)) {
            plan.structuralMigrationRequired = true;
            if (!options.allowStructuralChanges) {
                plan.issues.push_back({
                    ReloadIssueKind::FunctionSignatureChanged,
                    oldFunction.function->name,
                    "function signature changed",
                });
            } else {
                plan.changedFunctions.push_back(id);
            }
            continue;
        }
        if (functionBodyFingerprint(*oldFunction.module, *oldFunction.function) !=
            functionBodyFingerprint(*found->second.module, *found->second.function)) {
            plan.changedFunctions.push_back(id);
        }
    }
    for (const auto& [id, newFunction] : newFunctions) {
        if (oldFunctions.find(id) == oldFunctions.end()) {
            plan.addedFunctions.push_back(id);
            plan.structuralMigrationRequired = true;
            if (!options.allowStructuralChanges && !options.allowAddedFunctions) {
                plan.issues.push_back({
                    ReloadIssueKind::FunctionSetChanged,
                    newFunction.function->name,
                    "function was added during hot reload",
                });
            }
        }
    }
    // Cross-module -O2 inlining consistency verification:
    // 1) Build map of pre-inlining MIR stamps in newModules and old inline edges in oldModules.
    std::unordered_map<semantic::SymbolId, std::uint64_t> newPreInlineStamps;
    std::unordered_set<std::string> newStampedModules;
    for (const auto& nm : newModules) {
        if (!nm.languageMetadata.functionInlineStamps.empty()) {
            newStampedModules.insert(nm.name);
            for (const auto& st : nm.languageMetadata.functionInlineStamps) {
                newPreInlineStamps[st.symbolId] = st.bodyFingerprint;
            }
        }
    }
    std::unordered_map<std::uint64_t, std::uint64_t> oldEdgeFingerprints;
    const auto makeInlineEdgeKey = [](semantic::SymbolId callerSym, semantic::SymbolId calleeSym) noexcept -> std::uint64_t {
        return callerSym ^ (calleeSym + 0x9e3779b97f4a7c15ULL + (callerSym << 6u) + (callerSym >> 2u));
    };
    for (const auto& om : oldModules) {
        for (const auto& dep : om.languageMetadata.inlinedDependencies) {
            oldEdgeFingerprints[makeInlineEdgeKey(dep.callerSymbolId, dep.calleeSymbolId)] =
                dep.calleeBodyFingerprint;
        }
    }
    const std::unordered_set<semantic::SymbolId> changedOrRemovedSet = [&]() {
        std::unordered_set<semantic::SymbolId> s(plan.changedFunctions.begin(), plan.changedFunctions.end());
        s.insert(plan.removedFunctions.begin(), plan.removedFunctions.end());
        return s;
    }();

    for (const auto& nm : newModules) {
        for (const auto& dep : nm.languageMetadata.inlinedDependencies) {
            const std::string callerDisplay = nm.name + "::" + dep.callerFunction;
            const auto calleeNewIt = newFunctions.find(dep.calleeSymbolId);
            if (calleeNewIt == newFunctions.end()) {
                plan.issues.push_back({
                    ReloadIssueKind::StaleInlinedDependency,
                    callerDisplay,
                    "caller '" + callerDisplay + "' embeds an inlined body of removed function '" +
                        dep.calleeModule + "::" + dep.calleeFunction + "'; recompile module '" + nm.name + "'",
                });
                continue;
            }
            bool staleInlineBody = false;
            const auto stampIt = newPreInlineStamps.find(dep.calleeSymbolId);
            if (stampIt != newPreInlineStamps.end()) {
                if (stampIt->second != dep.calleeBodyFingerprint) {
                    staleInlineBody = true;
                }
            } else if (newStampedModules.count(dep.calleeModule) > 0) {
                staleInlineBody = true;
            } else if (changedOrRemovedSet.count(dep.calleeSymbolId) > 0) {
                const auto oldEdgeIt = oldEdgeFingerprints.find(
                    makeInlineEdgeKey(dep.callerSymbolId, dep.calleeSymbolId));
                if (oldEdgeIt != oldEdgeFingerprints.end() &&
                    oldEdgeIt->second == dep.calleeBodyFingerprint) {
                    staleInlineBody = true;
                }
            }

            if (staleInlineBody) {
                plan.issues.push_back({
                    ReloadIssueKind::StaleInlinedDependency,
                    callerDisplay,
                    "caller '" + callerDisplay + "' embeds a stale -O2 inlined body of '" +
                        dep.calleeModule + "::" + dep.calleeFunction + "'; recompile module '" + nm.name + "'",
                });
                continue;
            }

            // If the inlined callee changed and the caller WAS properly recompiled, record the caller
            // in inlineInvalidatedFunctions and changedFunctions so JIT / coroutine / IC caches flush!
            const auto oldEdgeIt = oldEdgeFingerprints.find(
                makeInlineEdgeKey(dep.callerSymbolId, dep.calleeSymbolId));
            if (changedOrRemovedSet.count(dep.calleeSymbolId) > 0 ||
                (oldEdgeIt != oldEdgeFingerprints.end() &&
                 oldEdgeIt->second != dep.calleeBodyFingerprint)) {
                plan.inlineInvalidatedFunctions.push_back(dep.callerSymbolId);
                plan.changedFunctions.push_back(dep.callerSymbolId);
            }
        }
    }

    // Also catch the case where oldMod had an inline dependency on a changed callee, and in the
    // properly recompiled newMod the callee is no longer inlined (e.g. grew >16 instructions):
    for (const auto& om : oldModules) {
        for (const auto& oldDep : om.languageMetadata.inlinedDependencies) {
            if (changedOrRemovedSet.count(oldDep.calleeSymbolId) > 0 &&
                newFunctions.find(oldDep.callerSymbolId) != newFunctions.end()) {
                plan.inlineInvalidatedFunctions.push_back(oldDep.callerSymbolId);
                plan.changedFunctions.push_back(oldDep.callerSymbolId);
            }
        }
    }

    auto sortAndDedup = [](std::vector<semantic::SymbolId>& vec) {
        std::sort(vec.begin(), vec.end());
        vec.erase(std::unique(vec.begin(), vec.end()), vec.end());
    };
    sortAndDedup(plan.changedFunctions);
    sortAndDedup(plan.inlineInvalidatedFunctions);
    sortAndDedup(plan.addedFunctions);
    sortAndDedup(plan.removedFunctions);

    if (options.allowStructuralChanges && plan.issues.empty()) {
        (void)buildStructuralTypeMigrations(
            oldModules, newModules, oldTypes, newTypes, plan);
    }

    plan.compatible = plan.issues.empty();
    if (plan.compatible) {
        plan.program = std::make_shared<const runtime::ProgramImage>(std::move(*linked));
    }
    return plan;
}

ReloadPlan apply(
    runtime::EngineRuntime& runtime,
    std::vector<bytecode::Module> replacementModules,
    ReloadOptions options) {
    auto current = runtime.programSnapshot();
    if (!current) {
        ReloadPlan plan;
        plan.issues.push_back({
            ReloadIssueKind::InvalidProgram,
            {},
            "runtime has no current program image",
        });
        return plan;
    }
    auto plan = prepare(*current, std::move(replacementModules), options);
    if (!plan.compatible) return plan;
    if (options.allowStructuralChanges && !plan.typeMigrations.empty()) {
        runtime::RuntimeError migrationError;
        if (!runtime.replaceProgram(
                plan.program,
                plan.typeMigrations,
                &plan.migrationStats,
                &migrationError)) {
            plan.compatible = false;
            plan.program.reset();
            plan.issues.push_back({
                ReloadIssueKind::TypeLayoutChanged,
                {},
                migrationError.message.empty()
                    ? "managed heap type layout migration failed"
                    : migrationError.message,
            });
        }
        return plan;
    }
    runtime.replaceProgram(plan.program);
    return plan;
}

const char* reloadIssueKindName(ReloadIssueKind kind) noexcept {
    switch (kind) {
    case ReloadIssueKind::InvalidProgram: return "invalid-program";
    case ReloadIssueKind::ModuleSetChanged: return "module-set-changed";
    case ReloadIssueKind::TypeLayoutChanged: return "type-layout-changed";
    case ReloadIssueKind::FunctionSetChanged: return "function-set-changed";
    case ReloadIssueKind::FunctionSignatureChanged: return "function-signature-changed";
    case ReloadIssueKind::LanguageMetadataChanged: return "language-metadata-changed";
    case ReloadIssueKind::StaleInlinedDependency:
        return "stale-inlined-dependency";
    }
    return "unknown";
}

} // namespace realscript::hot_reload
