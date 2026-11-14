#include <cstring>
#include "realscript/game/GameScripting.h"
#include "realscript/game/GameplayMetadata.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace realscript::game {
namespace {

semantic::SymbolId exactFieldTypeId(
    semantic::PrimitiveType type,
    const std::string& typeName) {
    return semantic::isExactType(type) && !typeName.empty()
        ? semantic::stableTypeId(typeName)
        : 0;
}

bool hasEditModeAttribute(
    const runtime::ProgramImage* program,
    const std::string& canonicalTypeName) {
    if (!program) return false;
    for (const auto& module : program->modules()) {
        for (const auto& attr : module.languageMetadata.attributes) {
            if (attr.target == canonicalTypeName &&
                (attr.name == "Tool" || attr.name == "ExecuteInEditMode")) {
                return true;
            }
        }
    }
    return false;
}

int findExecutionOrderAttribute(
    const runtime::ProgramImage* program,
    const std::string& canonicalTypeName) {
    if (!program) return 0;
    for (const auto& module : program->modules()) {
        for (const auto& attr : module.languageMetadata.attributes) {
            if (attr.target == canonicalTypeName &&
                (attr.name == "DefaultExecutionOrder" || attr.name == "ExecutionOrder") &&
                !attr.arguments.empty()) {
                try {
                    return std::stoi(attr.arguments.front().value);
                } catch (...) {
                    return 0;
                }
            }
        }
    }
    return 0;
}

std::vector<std::string> findRequiredComponents(
    const runtime::ProgramImage* program,
    const std::string& canonicalTypeName) {
    std::vector<std::string> required;
    if (!program) return required;
    for (const auto& module : program->modules()) {
        for (const auto& attr : module.languageMetadata.attributes) {
            if (attr.target == canonicalTypeName &&
                attr.name == "RequireComponent" &&
                !attr.arguments.empty()) {
                std::string val = attr.arguments.front().value;
                if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
                    val = val.substr(1, val.size() - 2);
                }
                if (!val.empty()) required.push_back(std::move(val));
            }
        }
    }
    return required;
}

bool isSceneAttrNamed(std::string_view attrName, std::string_view expectedBase) noexcept {
    if (attrName == expectedBase) return true;
    const std::string expectedSuffix = std::string(expectedBase) + "Attribute";
    if (attrName == expectedSuffix) return true;
    const auto sep = attrName.rfind("::");
    if (sep != std::string_view::npos) {
        const auto tail = attrName.substr(sep + 2);
        return tail == expectedBase || tail == expectedSuffix;
    }
    const auto dot = attrName.rfind('.');
    if (dot != std::string_view::npos) {
        const auto tail = attrName.substr(dot + 1);
        return tail == expectedBase || tail == expectedSuffix;
    }
    return false;
}

std::optional<std::int32_t> parseSceneFieldTagRaw(const std::string& raw) {
    std::size_t b = 0;
    std::size_t e = raw.size();
    while (b < e && std::isspace(static_cast<unsigned char>(raw[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(raw[e - 1]))) --e;
    std::string trimmed = raw.substr(b, e - b);
    if (trimmed.size() >= 2 && trimmed.front() == '"' && trimmed.back() == '"') {
        trimmed = trimmed.substr(1, trimmed.size() - 2);
    }
    if (trimmed.empty()) return std::nullopt;
    char* endPtr = nullptr;
    const long long parsed = std::strtoll(trimmed.c_str(), &endPtr, 10);
    if (!endPtr || *endPtr != '\0') return std::nullopt;
    if (parsed < std::numeric_limits<std::int32_t>::min() ||
        parsed > std::numeric_limits<std::int32_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(parsed);
}

std::optional<std::int32_t> parseSceneFieldTagValue(const GameplayValue& val) {
    if (const auto* iv64 = std::get_if<std::int64_t>(&val)) {
        if (*iv64 < std::numeric_limits<std::int32_t>::min() ||
            *iv64 > std::numeric_limits<std::int32_t>::max()) {
            return std::nullopt;
        }
        return static_cast<std::int32_t>(*iv64);
    }
    if (const auto* iv32 = std::get_if<std::int32_t>(&val)) {
        return *iv32;
    }
    if (const auto* sv = std::get_if<std::string>(&val)) {
        return parseSceneFieldTagRaw(*sv);
    }
    return std::nullopt;
}

std::optional<std::int32_t> findFieldTagAttribute(
    const runtime::ProgramImage* program,
    const ScriptMetadataRegistry* metadata,
    const std::string& canonicalTypeName,
    const std::string& fieldName,
    const semantic::FieldSymbol* field = nullptr) {
    if (field && field->fieldTag.has_value()) {
        return field->fieldTag;
    }
    if (metadata) {
        if (const auto* attributes = metadata->memberAttributes(canonicalTypeName, fieldName)) {
            for (const auto& attr : *attributes) {
                if (!isSceneAttrNamed(attr.name, "FieldTag")) continue;
                static const char* const kTagKeys[] = {"tag", "id", "arg0", "value"};
                for (const char* key : kTagKeys) {
                    const auto found = attr.arguments.find(key);
                    if (found != attr.arguments.end()) {
                        if (const auto parsed = parseSceneFieldTagValue(found->second)) {
                            return parsed;
                        }
                    }
                }
                if (attr.arguments.size() == 1) {
                    if (const auto parsed = parseSceneFieldTagValue(attr.arguments.begin()->second)) {
                        return parsed;
                    }
                }
            }
        }
    }
    if (program) {
        const std::string fieldTarget = canonicalTypeName + "::field:" + fieldName;
        const bool isAutoPropBacking = fieldName.size() > 1 && fieldName.front() == '$';
        const std::string propTarget = isAutoPropBacking
            ? (canonicalTypeName + "::property:" + fieldName.substr(1))
            : std::string{};
        for (const auto& module : program->modules()) {
            for (const auto& attr : module.languageMetadata.attributes) {
                if (!isSceneAttrNamed(attr.name, "FieldTag")) continue;
                if (attr.target != fieldTarget &&
                    (!isAutoPropBacking || attr.target != propTarget)) {
                    continue;
                }
                static const char* const kTagKeys[] = {"tag", "id", "arg0", "value"};
                for (const char* key : kTagKeys) {
                    for (const auto& arg : attr.arguments) {
                        if (arg.name == key) {
                            if (const auto parsed = parseSceneFieldTagRaw(arg.value)) {
                                return parsed;
                            }
                        }
                    }
                }
                if (attr.arguments.size() == 1) {
                    if (const auto parsed = parseSceneFieldTagRaw(attr.arguments.front().value)) {
                        return parsed;
                    }
                }
            }
        }
    }
    return std::nullopt;
}

std::vector<std::string> findRenamedFieldSources(
    const ScriptMetadataRegistry& metadata,
    const std::string& canonicalTypeName,
    const std::string& newFieldName,
    const semantic::FieldSymbol* field = nullptr,
    const runtime::ProgramImage* program = nullptr) {
    std::vector<std::string> aliases;
    const auto appendAlias = [&](const std::string& alias) {
        if (!alias.empty() &&
            std::find(aliases.begin(), aliases.end(), alias) == aliases.end()) {
            aliases.push_back(alias);
        }
    };
    if (field) {
        for (const auto& alias : field->formerlySerializedAs) {
            appendAlias(alias);
        }
    }
    const auto* attributes = metadata.memberAttributes(canonicalTypeName, newFieldName);
    if (attributes) {
        static const char* const kKeys[] = {"oldName", "name", "arg0"};
        for (const auto& attr : *attributes) {
            if (!isSceneAttrNamed(attr.name, "FormerlySerializedAs")) continue;
            bool extracted = false;
            for (const char* key : kKeys) {
                const auto found = attr.arguments.find(key);
                if (found != attr.arguments.end()) {
                    if (const auto* str = std::get_if<std::string>(&found->second)) {
                        appendAlias(*str);
                        extracted = true;
                        break;
                    }
                }
            }
            if (!extracted) {
                for (const auto& entry : attr.arguments) {
                    if (const auto* str = std::get_if<std::string>(&entry.second)) {
                        appendAlias(*str);
                        break;
                    }
                }
            }
        }
    }
    if (program) {
        static const char* const kAttrKeys[] = {"oldName", "name", "arg0"};
        const std::string fieldTarget = canonicalTypeName + "::field:" + newFieldName;
        const bool isAutoPropBacking = newFieldName.size() > 1 && newFieldName.front() == '$';
        const std::string propTarget = isAutoPropBacking
            ? (canonicalTypeName + "::property:" + newFieldName.substr(1))
            : std::string{};
        const auto unquoteArg = [](std::string val) -> std::string {
            std::size_t b = 0;
            std::size_t e = val.size();
            while (b < e && std::isspace(static_cast<unsigned char>(val[b]))) ++b;
            while (e > b && std::isspace(static_cast<unsigned char>(val[e - 1]))) --e;
            val = val.substr(b, e - b);
            if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
                val = val.substr(1, val.size() - 2);
            }
            return val;
        };
        for (const auto& module : program->modules()) {
            for (const auto& attr : module.languageMetadata.attributes) {
                if (!isSceneAttrNamed(attr.name, "FormerlySerializedAs")) continue;
                if (attr.target != fieldTarget &&
                    (!isAutoPropBacking || attr.target != propTarget)) {
                    continue;
                }
                bool extracted = false;
                for (const char* key : kAttrKeys) {
                    for (const auto& arg : attr.arguments) {
                        if (arg.name == key) {
                            auto v = unquoteArg(arg.value);
                            if (!v.empty()) {
                                appendAlias(v);
                                extracted = true;
                                break;
                            }
                        }
                    }
                    if (extracted) break;
                }
                if (!extracted && !attr.arguments.empty()) {
                    auto v = unquoteArg(attr.arguments.front().value);
                    if (!v.empty()) appendAlias(v);
                }
            }
        }
    }
    return aliases;
}

std::string findRenamedFieldSource(
    const ScriptMetadataRegistry& metadata,
    const std::string& canonicalTypeName,
    const std::string& newFieldName) {
    const auto aliases = findRenamedFieldSources(metadata, canonicalTypeName, newFieldName);
    return aliases.empty() ? std::string{} : aliases.front();
}

bool isNonZeroSequenceValue(const std::optional<runtime::Value>& val) noexcept {
    if (!val) return false;
    if (const auto* iv = std::get_if<std::int64_t>(&*val)) {
        return *iv != 0;
    }
    if (const auto* lv = std::get_if<runtime::LongValue>(&*val)) {
        return lv->value != 0;
    }
    if (const auto* dv = std::get_if<double>(&*val)) {
        return *dv != 0.0;
    }
    if (const auto* bv = std::get_if<bool>(&*val)) {
        return *bv;
    }
    return true;
}

std::uint64_t hashAsyncBridgeArgs(std::string_view a, std::string_view b = {}, std::uint64_t extra = 0) noexcept {
    std::uint64_t h = 14695981039346656037ULL;
    for (const char ch : a) {
        h ^= static_cast<unsigned char>(ch);
        h *= 1099511628211ULL;
    }
    h ^= 0xFFu;
    h *= 1099511628211ULL;
    for (const char ch : b) {
        h ^= static_cast<unsigned char>(ch);
        h *= 1099511628211ULL;
    }
    h ^= extra;
    h *= 1099511628211ULL;
    return h;
}

void patchMigratedObjectRefsInHeapObject(
    runtime::ManagedHeap* heap,
    runtime::ObjectRef target,
    const std::unordered_map<std::uint64_t, runtime::ObjectRef>& refMap,
    std::unordered_set<std::uint64_t>& visited);

bool patchMigratedObjectRefsInValue(
    runtime::ManagedHeap* heap,
    runtime::Value& val,
    const std::unordered_map<std::uint64_t, runtime::ObjectRef>& refMap,
    std::unordered_set<std::uint64_t>& visited) {
    if (auto* objRef = std::get_if<runtime::ObjectRef>(&val)) {
        if (!objRef->valid()) return false;
        const std::uint64_t refKey =
            (static_cast<std::uint64_t>(objRef->generation) << 32u) | objRef->slot;
        const auto mapped = refMap.find(refKey);
        if (mapped != refMap.end()) {
            *objRef = mapped->second;
            patchMigratedObjectRefsInHeapObject(heap, *objRef, refMap, visited);
            return true;
        }
        patchMigratedObjectRefsInHeapObject(heap, *objRef, refMap, visited);
        return false;
    }
    if (auto* structVal = std::get_if<runtime::StructValue>(&val)) {
        if (!structVal->storage) return false;
        auto cloned = std::make_shared<runtime::StructStorage>(*structVal->storage);
        bool changed = false;
        for (auto& subField : cloned->fields) {
            if (patchMigratedObjectRefsInValue(heap, subField, refMap, visited)) {
                changed = true;
            }
        }
        if (changed) {
            structVal->storage = std::move(cloned);
            runtime::pinValueInPools(val);
        }
        return changed;
    }
    return false;
}

void patchMigratedObjectRefsInHeapObject(
    runtime::ManagedHeap* heap,
    runtime::ObjectRef target,
    const std::unordered_map<std::uint64_t, runtime::ObjectRef>& refMap,
    std::unordered_set<std::uint64_t>& visited) {
    if (!heap || !target.valid()) return;
    const std::uint64_t key =
        (static_cast<std::uint64_t>(target.generation) << 32u) | target.slot;
    if (!visited.insert(key).second) return;

    if (target.kind == runtime::ObjectKind::Array) {
        const auto aLen = heap->arrayLength(target);
        if (!aLen) return;
        for (std::size_t i = 0; i < *aLen; ++i) {
            auto elem = heap->arrayGet(target, i);
            if (!elem) continue;
            if (patchMigratedObjectRefsInValue(heap, *elem, refMap, visited)) {
                (void)heap->arraySet(target, i, *elem);
            }
        }
        return;
    }

    const auto fCount = heap->fieldCount(target);
    if (!fCount) return;
    for (std::size_t i = 0; i < *fCount; ++i) {
        auto val = heap->fieldGet(target, i);
        if (!val) continue;
        if (patchMigratedObjectRefsInValue(heap, *val, refMap, visited)) {
            (void)heap->fieldSet(target, i, *val);
        }
    }
}

void collectPrefabTraceableValues(
    const PrefabNodeTemplate& node,
    std::vector<runtime::Value>& outValues) {
    if (node.components.empty() && node.children.empty()) return;
    for (const auto& comp : node.components) {
        if (comp.options.owner.isTraceable()) {
            outValues.push_back(comp.options.owner);
        }
        for (const auto& [_, val] : comp.options.initialMembers) {
            if (val.isTraceable()) {
                outValues.push_back(val);
            }
        }
    }
    for (const auto& child : node.children) {
        collectPrefabTraceableValues(child, outValues);
    }
}

void releaseRootTokenList(
    runtime::ManagedHeap* heap,
    std::vector<runtime::ManagedHeap::RootToken>& tokens) noexcept {
    if (heap) {
        for (const auto tok : tokens) {
            if (tok != 0) {
                (void)heap->removePersistentRoot(tok);
            }
        }
    }
    tokens.clear();
}

constexpr std::uint64_t packWsHandle(std::uint32_t slot, std::uint32_t generation) noexcept {
    return (static_cast<std::uint64_t>(generation) << 32u) | static_cast<std::uint64_t>(slot);
}

constexpr void unpackWsHandle(
    SceneScriptRuntime::WebSocketHandle raw,
    std::uint32_t& outSlot,
    std::uint32_t& outGen) noexcept {
    const auto u = static_cast<std::uint64_t>(raw);
    outSlot = static_cast<std::uint32_t>(u & 0xFFFFFFFFULL);
    outGen = static_cast<std::uint32_t>(u >> 32u);
}

std::string normalizeHttpKey(std::string_view method, std::string_view url) {
    std::string m(method);
    for (char& ch : m) {
        if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
    }
    return m + " " + std::string(url);
}


constexpr std::uint64_t kCoroFpFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kCoroFpFnvPrime = 1099511628211ULL;

void hashCoroMix64(std::uint64_t& h, std::uint64_t v) noexcept {
    h ^= v;
    h *= kCoroFpFnvPrime;
}

void hashCoroString(std::uint64_t& h, std::string_view s) noexcept {
    hashCoroMix64(h, static_cast<std::uint64_t>(s.size()));
    for (const char ch : s) {
        h ^= static_cast<unsigned char>(ch);
        h *= kCoroFpFnvPrime;
    }
}

std::string formatProgramSlotType(
    const runtime::ProgramImage& program,
    semantic::PrimitiveType prim,
    semantic::SymbolId typeId,
    std::string_view fallbackTypeName = {}) {
    if (prim == semantic::PrimitiveType::Void) {
        return "void";
    }
    const std::string baseName(semantic::primitiveTypeName(prim));
    if (!semantic::isExactType(prim)) {
        return baseName;
    }
    if (!fallbackTypeName.empty()) {
        return baseName + ":" + std::string(fallbackTypeName);
    }
    if (typeId == 0) {
        return baseName;
    }
    if (const auto* resolved = program.resolveType(typeId)) {
        return baseName + ":" + semantic::canonicalTypeName(*resolved);
    }
    if (typeId == runtime::fixedTypeId()) return "struct:RealScript.Math::Fixed";
    if (typeId == runtime::fixed16TypeId()) return "struct:RealScript.Math::Fixed16";
    if (typeId == runtime::vec2TypeId()) return "struct:RealScript.Math::Vec2";
    if (typeId == runtime::vec3TypeId()) return "struct:RealScript.Math::Vec3";
    if (typeId == runtime::quatTypeId()) return "struct:RealScript.Math::Quat";
    if (typeId == runtime::fixed64TypeId()) return "struct:RealScript.Math::Fixed64";
    if (typeId == runtime::vec2fTypeId()) return "struct:RealScript.Math::Vec2f";
    if (typeId == runtime::vec3fTypeId()) return "struct:RealScript.Math::Vec3f";
    if (typeId == runtime::quatfTypeId()) return "struct:RealScript.Math::Quatf";
    if (typeId == runtime::awaitableVoidTypeId()) return "struct:RealScript.Async::Awaitable";
    if (typeId == runtime::whenAnyResultTypeId()) return "struct:RealScript.Async::WhenAnyResult";
    return baseName + ":#" + std::to_string(typeId);
}

bool methodReturnsAwaitableStruct(const ScriptMethod& method) {
    if (semantic::storageReturnTypeOf(method.descriptor) !=
        semantic::PrimitiveType::Struct) {
        return false;
    }
    const std::string& retTypeName =
        semantic::storageReturnTypeNameOf(method.descriptor);
    return retTypeName == "RealScript.Async::Awaitable" ||
        retTypeName.find("::Awaitable__") != std::string::npos;
}

std::string findExplicitTypeGuidAttribute(
    const runtime::ProgramImage* program,
    const std::string& canonicalTypeName) {
    if (!program) return {};
    for (const auto& module : program->modules()) {
        for (const auto& attr : module.languageMetadata.attributes) {
            if (attr.target == canonicalTypeName &&
                (attr.name == "Guid" || attr.name == "AssetGuid") &&
                !attr.arguments.empty()) {
                std::string val = attr.arguments.front().value;
                if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
                    val = val.substr(1, val.size() - 2);
                }
                if (!val.empty()) return val;
            }
        }
    }
    return {};
}

std::vector<std::string> collectSequenceOwnedFieldNames(
    const semantic::TypeSymbol& owner,
    const std::string& seqName) {
    std::vector<std::string> names;
    const std::string targetField = "$sequence_target_" + seqName;
    bool inSequenceRange = false;
    for (const auto& field : owner.fields) {
        if (!field.synthetic) {
            if (inSequenceRange) break;
            continue;
        }
        if (field.name == targetField) {
            inSequenceRange = true;
            names.push_back(field.name);
            continue;
        }
        if (inSequenceRange) {
            if (field.name.rfind("$sequence_target_", 0) == 0) {
                break;
            }
            if (field.name.rfind("$sequence_", 0) == 0) {
                names.push_back(field.name);
            } else {
                break;
            }
        }
    }
    return names;
}

const semantic::TypeSymbol* resolveProgramTypeByNameOrId(
    const runtime::ProgramImage& program,
    semantic::SymbolId typeId,
    std::string_view typeName = {}) {
    if (typeId != 0) {
        if (const auto* byId = program.resolveType(typeId)) {
            return byId;
        }
    }
    if (!typeName.empty()) {
        const auto byStableId = semantic::stableTypeId(std::string(typeName));
        if (const auto* byId = program.resolveType(byStableId)) {
            return byId;
        }
        for (const auto& mod : program.modules()) {
            for (const auto& t : mod.types) {
                if (semantic::canonicalTypeName(t) == typeName || t.name == typeName) {
                    return &t;
                }
            }
        }
    }
    return nullptr;
}

const semantic::TypeSymbol* unwrapToRecordOrStructType(
    const runtime::ProgramImage& program,
    semantic::PrimitiveType prim,
    semantic::SymbolId typeId,
    std::string_view typeName = {}) {
    if (prim == semantic::PrimitiveType::Object || prim == semantic::PrimitiveType::Struct) {
        return resolveProgramTypeByNameOrId(program, typeId, typeName);
    }
    if (prim == semantic::PrimitiveType::Array) {
        std::string arrName(typeName);
        if (arrName.empty() && typeId != 0) {
            if (const auto* arrDesc = program.resolveType(typeId)) {
                arrName = semantic::canonicalTypeName(*arrDesc);
            }
        }
        std::size_t guard = 0;
        while (!arrName.empty() && guard++ < 16u) {
            semantic::PrimitiveType elemPrim = semantic::PrimitiveType::Error;
            std::string elemTypeName;
            if (!semantic::decodeArrayTypeName(arrName, elemPrim, elemTypeName)) {
                break;
            }
            if (elemPrim == semantic::PrimitiveType::Object ||
                elemPrim == semantic::PrimitiveType::Struct) {
                return resolveProgramTypeByNameOrId(
                    program,
                    elemTypeName.empty() ? 0 : semantic::stableTypeId(elemTypeName),
                    elemTypeName);
            }
            if (elemPrim == semantic::PrimitiveType::Array) {
                arrName = std::move(elemTypeName);
                continue;
            }
            break;
        }
    }
    return nullptr;
}

std::uint64_t computeRecursiveRecordOrStructLayoutHash(
    const runtime::ProgramImage& program,
    const semantic::TypeSymbol& typeDesc,
    std::unordered_set<std::string>& activeStack,
    std::unordered_map<std::string, std::uint64_t>& memo) {
    const std::string canon = semantic::canonicalTypeName(typeDesc);
    if (activeStack.empty()) {
        if (const auto mIt = memo.find(canon); mIt != memo.end()) {
            return mIt->second;
        }
    }
    if (!activeStack.insert(canon).second) {
        std::uint64_t cycleHash = kCoroFpFnvOffset;
        hashCoroMix64(cycleHash, 0xC1C1EULL);
        hashCoroString(cycleHash, canon);
        return cycleHash == 0 ? 1u : cycleHash;
    }

    std::uint64_t h = kCoroFpFnvOffset;
    hashCoroMix64(h, static_cast<std::uint64_t>(typeDesc.kind));
    hashCoroString(h, canon);
    hashCoroString(h, typeDesc.baseTypeName);
    if (!typeDesc.baseTypeName.empty()) {
        if (const auto* baseDesc = resolveProgramTypeByNameOrId(
                program,
                semantic::stableTypeId(typeDesc.baseTypeName),
                typeDesc.baseTypeName)) {
            hashCoroMix64(
                h,
                computeRecursiveRecordOrStructLayoutHash(
                    program, *baseDesc, activeStack, memo));
        }
    }

    std::size_t nonStaticCount = 0;
    for (const auto& fld : typeDesc.fields) {
        if (fld.staticField || fld.constField) continue;
        // Exclude sequence-internal synthetic fields so independent sequences on a class
        // do not cross-contaminate non-sequence field layout checks.
        if (fld.synthetic && fld.name.rfind("$sequence_", 0) == 0) continue;
        ++nonStaticCount;
        const semantic::SymbolId ftid =
            semantic::isExactType(fld.type) && !fld.typeName.empty()
                ? semantic::stableTypeId(fld.typeName)
                : 0;
        hashCoroMix64(h, fld.index);
        hashCoroMix64(h, fld.synthetic ? 1u : 0u);
        hashCoroString(h, fld.name);
        hashCoroString(h, formatProgramSlotType(program, fld.type, ftid, fld.typeName));

        if (const auto* childDesc = unwrapToRecordOrStructType(
                program, fld.type, ftid, fld.typeName)) {
            if (childDesc->kind == semantic::TypeKind::Class ||
                childDesc->kind == semantic::TypeKind::Struct) {
                hashCoroMix64(
                    h,
                    computeRecursiveRecordOrStructLayoutHash(
                        program, *childDesc, activeStack, memo));
            }
        }
    }
    hashCoroMix64(h, nonStaticCount);

    activeStack.erase(canon);
    if (h == 0) h = 1;
    if (activeStack.empty()) {
        memo.emplace(canon, h);
    }
    return h;
}

void collectReferencedTypesRecursive(
    const runtime::ProgramImage& program,
    const semantic::TypeSymbol* typeDesc,
    std::unordered_map<std::string, std::uint64_t>& layoutMemo,
    std::map<std::string, std::uint64_t>& outStructLayouts,
    std::map<std::string, std::uint64_t>& outClassLayouts,
    std::unordered_set<std::string>& visitedTypes) {
    if (!typeDesc) return;
    if (typeDesc->kind != semantic::TypeKind::Class &&
        typeDesc->kind != semantic::TypeKind::Struct) {
        return;
    }
    const std::string canon = semantic::canonicalTypeName(*typeDesc);
    if (!visitedTypes.insert(canon).second) return;

    std::unordered_set<std::string> stack;
    const std::uint64_t layoutHash =
        computeRecursiveRecordOrStructLayoutHash(program, *typeDesc, stack, layoutMemo);
    if (typeDesc->kind == semantic::TypeKind::Struct) {
        outStructLayouts[canon] = layoutHash;
    } else {
        outClassLayouts[canon] = layoutHash;
    }

    if (!typeDesc->baseTypeName.empty()) {
        if (const auto* baseDesc = resolveProgramTypeByNameOrId(
                program,
                semantic::stableTypeId(typeDesc->baseTypeName),
                typeDesc->baseTypeName)) {
            collectReferencedTypesRecursive(
                program, baseDesc, layoutMemo, outStructLayouts, outClassLayouts, visitedTypes);
        }
    }
    for (const auto& fld : typeDesc->fields) {
        if (fld.staticField || fld.constField) continue;
        if (fld.synthetic && fld.name.rfind("$sequence_", 0) == 0) continue;
        const semantic::SymbolId ftid =
            semantic::isExactType(fld.type) && !fld.typeName.empty()
                ? semantic::stableTypeId(fld.typeName)
                : 0;
        if (const auto* childDesc = unwrapToRecordOrStructType(
                program, fld.type, ftid, fld.typeName)) {
            collectReferencedTypesRecursive(
                program, childDesc, layoutMemo, outStructLayouts, outClassLayouts, visitedTypes);
        }
    }
}

struct HotReloadCoroBridge {
    static constexpr const char* kBridgeModuleName = "__rs_coro_hr_bridge";
    static constexpr const char* kTrampolineName = "__trampoline";
    static constexpr const char* kTrampolineQualName = "__rs_coro_hr_bridge::__trampoline";
    static constexpr const char* kHostResumeName = "__rs_coro_hr_bridge::__host_resume";

    runtime::CoroutineResumeCallback innerCallback = nullptr;
    void* innerRunnerContext = nullptr;
    std::shared_ptr<const void> innerRunnerOwner{};
    semantic::SymbolId trampolineSymbolId = 0;
    runtime::ExecutionOptions executionOptions{};
    std::shared_ptr<debug::DebugController> fallbackDebugger{};
    std::shared_ptr<const runtime::ProgramImage> targetProgram{};
    std::unique_ptr<runtime::Interpreter> interpreter{};

    struct ActiveResumeCall {
        runtime::CoroutineResumeCallback innerCallback = nullptr;
        void* innerRunnerContext = nullptr;
        runtime::CoroutineFrameHandle frameHandle{};
        runtime::CoroutineFrame* framePtr = nullptr;
        runtime::AsyncRuntimeContext* asyncCtx = nullptr;
    };

    static inline thread_local ActiveResumeCall* tlsActiveCall_ = nullptr;

    static void onResume(
        void* runnerContext,
        runtime::CoroutineFrameHandle frameHandle,
        runtime::CoroutineFrame& frame,
        runtime::AsyncRuntimeContext& asyncContext) {
        auto* self = static_cast<HotReloadCoroBridge*>(runnerContext);
        if (!self || !self->innerCallback) return;

        // Keep `self` alive on the C++ stack and clear `frame.runnerOwner` so if the
        // resumed coroutine suspends at a subsequent `await` inside the new ProgramImage,
        // `suspendInterpreterCoroutine` binds `frame.runnerOwner` directly to the new env.
        const std::shared_ptr<const void> selfOwnerKeepAlive = frame.runnerOwner;
        frame.runnerOwner.reset();

        if (!self->interpreter) {
            self->innerCallback(self->innerRunnerContext, frameHandle, frame, asyncContext);
            return;
        }

        // If already executing inside an active interpreter state that resolves the new
        // function symbol, `resumeInterpreterCoroutineCallback` uses `activeInterpreterState` directly.
        ActiveResumeCall callData{
            self->innerCallback,
            self->innerRunnerContext,
            frameHandle,
            &frame,
            &asyncContext,
        };
        ActiveResumeCall* const prevTls = tlsActiveCall_;
        tlsActiveCall_ = &callData;
        void* const prevActiveInterp = asyncContext.activeInterpreterState();
        asyncContext.setActiveInterpreterState(nullptr);
        runtime::ExecutionOptions opts = self->executionOptions;
        if (!opts.debugger && self->fallbackDebugger) {
            opts.debugger = self->fallbackDebugger;
        }
        struct ScopeRestore {
            runtime::AsyncRuntimeContext& ctx;
            void* prevInterp;
            ActiveResumeCall* prevCall;
            debug::RuntimeDebugHook* dbg;
            const runtime::ProgramImage* restoreProg;
            ~ScopeRestore() noexcept {
                ctx.setActiveInterpreterState(prevInterp);
                tlsActiveCall_ = prevCall;
                if (dbg && restoreProg) {
                    dbg->bindProgram(*restoreProg);
                }
            }
        } restore{
            asyncContext,
            prevActiveInterp,
            prevTls,
            opts.debugger.get(),
            self->targetProgram.get(),
        };

        (void)self->interpreter->invoke(
            self->trampolineSymbolId,
            {},
            std::move(opts));
    }
};

std::shared_ptr<HotReloadCoroBridge> createHotReloadCoroBridge(
    const GameProgram& newProgram,
    const std::shared_ptr<const runtime::BindingRegistry>& fallbackBindings,
    const std::shared_ptr<debug::DebugController>& fallbackDebugger,
    const std::shared_ptr<runtime::ManagedHeap>& heap,
    const std::shared_ptr<runtime::AsyncRuntimeContext>& asyncCtx,
    const runtime::ExecutionOptions& execOpts,
    runtime::CoroutineResumeCallback origCallback,
    void* origRunnerContext,
    std::shared_ptr<const void> origRunnerOwner) {
    if (!newProgram.valid() || !newProgram.program() || !origCallback) {
        return nullptr;
    }

    // Unwrap chained HotReloadCoroBridges across multiple consecutive hot reloads
    if (origCallback == &HotReloadCoroBridge::onResume && origRunnerContext != nullptr) {
        auto* prevBridge = static_cast<HotReloadCoroBridge*>(origRunnerContext);
        origCallback = prevBridge->innerCallback;
        origRunnerContext = prevBridge->innerRunnerContext;
        origRunnerOwner = prevBridge->innerRunnerOwner;
    }

    std::vector<bytecode::Module> modules = newProgram.program()->modules();
    const semantic::SymbolId hostResumeSym =
        semantic::stableTypeId(HotReloadCoroBridge::kHostResumeName);
    const semantic::SymbolId trampSym =
        semantic::stableTypeId(HotReloadCoroBridge::kTrampolineQualName);

    bytecode::Module bridgeMod;
    bridgeMod.name = HotReloadCoroBridge::kBridgeModuleName;

    bytecode::FunctionReference hostRef;
    hostRef.symbolId = hostResumeSym;
    hostRef.name = HotReloadCoroBridge::kHostResumeName;
    hostRef.returnType = semantic::PrimitiveType::Void;
    hostRef.returnTypeId = 0;
    bridgeMod.functionReferences.push_back(std::move(hostRef));

    bytecode::Function trampFn;
    trampFn.symbolId = trampSym;
    trampFn.name = HotReloadCoroBridge::kTrampolineName;
    trampFn.returnType = semantic::PrimitiveType::Void;
    trampFn.returnTypeId = 0;

    bytecode::BasicBlock entryBb;
    entryBb.id = 0;
    bytecode::Instruction callInst;
    callInst.opcode = bytecode::Opcode::Call;
    callInst.elementType = semantic::PrimitiveType::Void;
    callInst.result = bytecode::InvalidRegister;
    callInst.index = 0;
    entryBb.instructions.push_back(callInst);
    entryBb.terminator.kind = bytecode::TerminatorKind::ReturnVoid;
    trampFn.blocks.push_back(std::move(entryBb));
    bridgeMod.functions.push_back(std::move(trampFn));

    modules.push_back(std::move(bridgeMod));

    runtime::RuntimeError linkErr;
    auto linked = runtime::ProgramImage::link(std::move(modules), linkErr);
    if (!linked) {
        return nullptr;
    }

    const runtime::BindingRegistry* baseBindings = newProgram.bindings()
        ? newProgram.bindings().get()
        : fallbackBindings.get();
    auto bridgeBindings = baseBindings
        ? std::make_shared<runtime::BindingRegistry>(*baseBindings)
        : std::make_shared<runtime::BindingRegistry>();
    (void)bridgeBindings->bind(
        hostResumeSym,
        [](const bytecode::FunctionReference&,
           runtime::ValueSpan,
           runtime::RuntimeError&) -> std::optional<runtime::Value> {
            auto* active = HotReloadCoroBridge::tlsActiveCall_;
            if (active && active->innerCallback && active->framePtr && active->asyncCtx) {
                active->innerCallback(
                    active->innerRunnerContext,
                    active->frameHandle,
                    *active->framePtr,
                    *active->asyncCtx);
            }
            return runtime::Value{};
        },
        runtime::BindingDeterminism::Deterministic);

    auto bridge = std::make_shared<HotReloadCoroBridge>();
    bridge->innerCallback = origCallback;
    bridge->innerRunnerContext = origRunnerContext;
    bridge->innerRunnerOwner = std::move(origRunnerOwner);
    bridge->trampolineSymbolId = trampSym;
    bridge->executionOptions = execOpts;
    if (!bridge->executionOptions.debugger && fallbackDebugger) {
        bridge->executionOptions.debugger = fallbackDebugger;
    }
    bridge->fallbackDebugger = fallbackDebugger;
    bridge->targetProgram = newProgram.program();
    bridge->interpreter = std::make_unique<runtime::Interpreter>(
        std::make_shared<runtime::ProgramImage>(std::move(*linked)),
        heap);
    bridge->interpreter->setBindingRegistry(std::move(bridgeBindings));
    bridge->interpreter->setAsyncRuntime(asyncCtx);
    return bridge;
}

} // namespace

std::vector<CoroutineStateMachineFingerprint> computeProgramCoroutineFingerprints(
    const runtime::ProgramImage& program) {
    std::vector<CoroutineStateMachineFingerprint> results;
    std::unordered_map<semantic::SymbolId, std::uint32_t> linearIndexBySymbol;
    std::unordered_map<std::string, std::uint64_t> layoutMemo;

    std::uint32_t linearFuncIndex = 0;
    for (const auto& module : program.modules()) {
        for (const auto& func : module.functions) {
            const std::uint32_t curFuncIdx = linearFuncIndex++;
            linearIndexBySymbol[func.symbolId] = curFuncIdx;

            bool isAsync = false;
            for (const auto& blk : func.blocks) {
                for (const auto& inst : blk.instructions) {
                    if (inst.opcode == bytecode::Opcode::AsyncEnter ||
                        inst.opcode == bytecode::Opcode::AwaitSuspend ||
                        inst.opcode == bytecode::Opcode::AsyncResolve) {
                        isAsync = true;
                        break;
                    }
                }
                if (isAsync) break;
            }
            if (!isAsync) continue;

            CoroutineStateMachineFingerprint fp;
            fp.isSequence = false;
            const auto* loc = program.resolveFunction(func.symbolId);
            fp.canonicalName = loc
                ? loc->qualifiedName
                : (module.name.empty() ? func.name : (module.name + "::" + func.name));
            fp.functionIndex = curFuncIdx;
            fp.symbolId = func.symbolId;
            fp.returnTypeId = func.returnTypeId;
            fp.returnTypeName = formatProgramSlotType(program, func.returnType, func.returnTypeId);

            std::map<std::string, std::uint64_t> refStructs;
            std::map<std::string, std::uint64_t> refClasses;
            std::unordered_set<std::string> visitedRefTypes;
            const auto noteSlotType = [&](semantic::PrimitiveType prim,
                                          semantic::SymbolId tid,
                                          std::string_view fallbackName = {}) {
                if (const auto* td = unwrapToRecordOrStructType(program, prim, tid, fallbackName)) {
                    collectReferencedTypesRecursive(
                        program, td, layoutMemo, refStructs, refClasses, visitedRefTypes);
                }
            };

            noteSlotType(func.returnType, func.returnTypeId);

            // Extract parameter names from method metadata and/or debugInfo.locals
            fp.parameterNames.assign(func.parameterTypes.size(), std::string{});
            for (const auto& t : module.types) {
                for (const auto& m : t.methods) {
                    if (m.id == func.symbolId) {
                        const std::size_t hasThisOffset = (!m.staticMethod && m.method) ? 1u : 0u;
                        if (hasThisOffset == 1u && !fp.parameterNames.empty()) {
                            fp.parameterNames[0] = "this";
                        }
                        for (std::size_t pi = 0;
                             pi < m.parameters.size() && (pi + hasThisOffset) < fp.parameterNames.size();
                             ++pi) {
                            fp.parameterNames[pi + hasThisOffset] = m.parameters[pi].name;
                        }
                    }
                }
            }
            for (const auto& dl : func.debugInfo.locals) {
                if (dl.parameter && dl.slot < fp.parameterNames.size() &&
                    fp.parameterNames[dl.slot].empty()) {
                    fp.parameterNames[dl.slot] = dl.name;
                }
            }

            fp.parameterSlotTypes.reserve(func.parameterTypes.size());
            for (std::size_t i = 0; i < func.parameterTypes.size(); ++i) {
                const semantic::SymbolId tid =
                    i < func.parameterTypeIds.size() ? func.parameterTypeIds[i] : 0;
                fp.parameterSlotTypes.push_back(
                    formatProgramSlotType(program, func.parameterTypes[i], tid));
                noteSlotType(func.parameterTypes[i], tid);
            }

            fp.localSlotTypes.reserve(func.localTypes.size());
            for (std::size_t i = 0; i < func.localTypes.size(); ++i) {
                const semantic::SymbolId tid =
                    i < func.localTypeIds.size() ? func.localTypeIds[i] : 0;
                fp.localSlotTypes.push_back(
                    formatProgramSlotType(program, func.localTypes[i], tid));
                noteSlotType(func.localTypes[i], tid);
            }

            fp.debugLocals.reserve(func.debugInfo.locals.size());
            for (const auto& dl : func.debugInfo.locals) {
                CoroutineLocalDebugDesc ddesc;
                ddesc.name = dl.name;
                ddesc.slot = dl.slot;
                ddesc.parameter = dl.parameter;
                ddesc.slotTypeName = formatProgramSlotType(program, dl.type, dl.typeId);
                noteSlotType(dl.type, dl.typeId);
                fp.debugLocals.push_back(std::move(ddesc));
            }

            for (std::size_t r = 0; r < func.registerTypes.size(); ++r) {
                const semantic::SymbolId rtid =
                    r < func.registerTypeIds.size() ? func.registerTypeIds[r] : 0;
                noteSlotType(func.registerTypes[r], rtid);
            }
            for (const auto& blk : func.blocks) {
                for (const auto& bp : blk.parameters) {
                    noteSlotType(bp.type, bp.typeId);
                }
                for (const auto& inst : blk.instructions) {
                    const bool opcodeUsesTypeIndex =
                        inst.opcode == bytecode::Opcode::NewObject ||
                        inst.opcode == bytecode::Opcode::NewStruct ||
                        inst.opcode == bytecode::Opcode::LoadField ||
                        inst.opcode == bytecode::Opcode::StoreField ||
                        inst.opcode == bytecode::Opcode::LoadStructField ||
                        inst.opcode == bytecode::Opcode::StoreStructField ||
                        inst.opcode == bytecode::Opcode::IsType ||
                        inst.opcode == bytecode::Opcode::AsType ||
                        inst.opcode == bytecode::Opcode::AwaitSuspend;
                    if (opcodeUsesTypeIndex && inst.typeIndex < module.types.size()) {
                        const auto& modType = module.types[inst.typeIndex];
                        if (modType.kind == semantic::TypeKind::Class ||
                            modType.kind == semantic::TypeKind::Struct) {
                            collectReferencedTypesRecursive(
                                program, &modType, layoutMemo, refStructs, refClasses, visitedRefTypes);
                        }
                    }
                    if (inst.usesElementTypeId() && inst.activeElementTypeId() != 0) {
                        noteSlotType(inst.elementType, inst.activeElementTypeId());
                        if (const auto* et = program.resolveType(inst.activeElementTypeId())) {
                            collectReferencedTypesRecursive(
                                program, et, layoutMemo, refStructs, refClasses, visitedRefTypes);
                        }
                    } else if (inst.opcode == bytecode::Opcode::ConstantTypeId) {
                        const auto tid = static_cast<semantic::SymbolId>(inst.activeIntegerImmediate());
                        if (const auto* ct = program.resolveType(tid)) {
                            collectReferencedTypesRecursive(
                                program, ct, layoutMemo, refStructs, refClasses, visitedRefTypes);
                        }
                    }
                }
            }
            for (const auto& eh : func.exceptionHandlers) {
                if (eh.catchTypeId != 0) {
                    noteSlotType(semantic::PrimitiveType::Object, eh.catchTypeId);
                }
            }
            fp.referencedStructLayouts.assign(refStructs.begin(), refStructs.end());
            fp.referencedClassLayouts.assign(refClasses.begin(), refClasses.end());

            fp.registerSlotCount = func.registerTypes.size();

            // Backward liveness dataflow for SSA registers live-in at each basic block
            const std::size_t nBlocks = func.blocks.size();
            const std::size_t nRegs = func.registerTypes.size();
            std::unordered_map<bytecode::BlockId, std::size_t> blockIdxById;
            blockIdxById.reserve(nBlocks);
            for (std::size_t b = 0; b < nBlocks; ++b) {
                blockIdxById[func.blocks[b].id] = b;
            }

            std::vector<std::vector<bool>> useRegs(nBlocks, std::vector<bool>(nRegs, false));
            std::vector<std::vector<bool>> defRegs(nBlocks, std::vector<bool>(nRegs, false));
            std::vector<std::vector<std::size_t>> succBlocks(nBlocks);

            for (std::size_t b = 0; b < nBlocks; ++b) {
                const auto& blk = func.blocks[b];
                auto& use = useRegs[b];
                auto& def = defRegs[b];
                for (const auto& bp : blk.parameters) {
                    if (bp.target < nRegs) {
                        def[bp.target] = true;
                    }
                }
                for (const auto& inst : blk.instructions) {
                    for (const auto op : inst.operands) {
                        if (op < nRegs && !def[op]) {
                            use[op] = true;
                        }
                    }
                    if (inst.result != bytecode::InvalidRegister && inst.result < nRegs) {
                        def[inst.result] = true;
                    }
                }
                const auto& term = blk.terminator;
                const auto markTermUse = [&](bytecode::Register r) {
                    if (r != bytecode::InvalidRegister && r < nRegs && !def[r]) {
                        use[r] = true;
                    }
                };
                if (term.kind == bytecode::TerminatorKind::Branch) {
                    markTermUse(term.condition);
                    for (const auto r : term.arguments) markTermUse(r);
                    for (const auto r : term.falseArguments) markTermUse(r);
                    if (const auto it = blockIdxById.find(term.target); it != blockIdxById.end()) {
                        succBlocks[b].push_back(it->second);
                    }
                    if (const auto it = blockIdxById.find(term.falseTarget); it != blockIdxById.end()) {
                        succBlocks[b].push_back(it->second);
                    }
                } else if (term.kind == bytecode::TerminatorKind::Jump) {
                    for (const auto r : term.arguments) markTermUse(r);
                    if (const auto it = blockIdxById.find(term.target); it != blockIdxById.end()) {
                        succBlocks[b].push_back(it->second);
                    }
                } else if (term.kind == bytecode::TerminatorKind::ReturnValue ||
                           term.kind == bytecode::TerminatorKind::Throw) {
                    markTermUse(term.value);
                }

                for (const auto& eh : func.exceptionHandlers) {
                    if (std::find(
                            eh.protectedBlocks.begin(),
                            eh.protectedBlocks.end(),
                            blk.id) != eh.protectedBlocks.end()) {
                        if (const auto hIt = blockIdxById.find(eh.handlerBlock);
                            hIt != blockIdxById.end()) {
                            succBlocks[b].push_back(hIt->second);
                        }
                    }
                }
            }

            std::vector<std::vector<bool>> liveIn = useRegs;
            bool changed = true;
            while (changed) {
                changed = false;
                for (std::size_t rev = nBlocks; rev > 0; --rev) {
                    const std::size_t b = rev - 1;
                    for (const std::size_t s : succBlocks[b]) {
                        for (std::size_t r = 0; r < nRegs; ++r) {
                            if (liveIn[s][r] && !defRegs[b][r] && !liveIn[b][r]) {
                                liveIn[b][r] = true;
                                changed = true;
                            }
                        }
                    }
                }
            }

            std::uint32_t ordinal = 0;
            for (const auto& blk : func.blocks) {
                for (const auto& inst : blk.instructions) {
                    if (inst.opcode != bytecode::Opcode::AwaitSuspend) continue;
                    CoroutineSuspensionPointDesc sp;
                    sp.ordinal = ordinal++;
                    sp.awaitLocalIndex = inst.index;
                    sp.resumeBlockId = static_cast<std::uint32_t>(inst.activeIntegerImmediate());
                    if (inst.typeIndex < module.types.size()) {
                        sp.awaitedTypeName =
                            semantic::canonicalTypeName(module.types[inst.typeIndex]);
                    }
                    if (const auto rbIt = blockIdxById.find(sp.resumeBlockId);
                        rbIt != blockIdxById.end()) {
                        const auto& rBlk = func.blocks[rbIt->second];
                        for (const auto& bp : rBlk.parameters) {
                            sp.resumeBlockParamTypes.push_back(
                                formatProgramSlotType(program, bp.type, bp.typeId));
                        }
                        for (const auto& rInst : rBlk.instructions) {
                            if (rInst.opcode == bytecode::Opcode::AwaitResume &&
                                rInst.index == sp.awaitLocalIndex) {
                                if (rInst.result == bytecode::InvalidRegister) {
                                    sp.resumeResultTypeName = "void";
                                } else if (rInst.result < func.registerTypes.size()) {
                                    const semantic::SymbolId rtid =
                                        rInst.result < func.registerTypeIds.size()
                                            ? func.registerTypeIds[rInst.result]
                                            : 0;
                                    sp.resumeResultTypeName = formatProgramSlotType(
                                        program, func.registerTypes[rInst.result], rtid);
                                }
                                break;
                            }
                        }
                        for (std::size_t r = 0; r < nRegs; ++r) {
                            if (liveIn[rbIt->second][r]) {
                                const semantic::SymbolId rtid =
                                    r < func.registerTypeIds.size() ? func.registerTypeIds[r] : 0;
                                sp.liveInRegisters.emplace_back(
                                    static_cast<std::uint32_t>(r),
                                    formatProgramSlotType(program, func.registerTypes[r], rtid));
                            }
                        }
                    }
                    for (const auto& eh : func.exceptionHandlers) {
                        if (std::find(
                                eh.protectedBlocks.begin(),
                                eh.protectedBlocks.end(),
                                sp.resumeBlockId) != eh.protectedBlocks.end()) {
                            sp.protectingHandlers.push_back(
                                std::to_string(eh.handlerBlock) + ":" +
                                std::to_string(eh.exceptionLocal) + ":" +
                                formatProgramSlotType(
                                    program, semantic::PrimitiveType::Object, eh.catchTypeId));
                        }
                    }
                    fp.suspensionPoints.push_back(std::move(sp));
                }
            }
            fp.suspensionPointCount = fp.suspensionPoints.size();

            std::uint64_t h = kCoroFpFnvOffset;
            hashCoroMix64(h, 0u); // async coroutine tag
            hashCoroString(h, fp.canonicalName);
            hashCoroString(h, fp.returnTypeName);
            hashCoroMix64(h, fp.parameterNames.size());
            for (const auto& pn : fp.parameterNames) hashCoroString(h, pn);
            hashCoroMix64(h, fp.parameterSlotTypes.size());
            for (const auto& pt : fp.parameterSlotTypes) hashCoroString(h, pt);
            hashCoroMix64(h, fp.localSlotTypes.size());
            for (const auto& lt : fp.localSlotTypes) hashCoroString(h, lt);
            hashCoroMix64(h, fp.debugLocals.size());
            for (const auto& dl : fp.debugLocals) {
                hashCoroString(h, dl.name);
                hashCoroMix64(h, dl.slot);
                hashCoroMix64(h, dl.parameter ? 1u : 0u);
                hashCoroString(h, dl.slotTypeName);
            }
            hashCoroMix64(h, fp.suspensionPointCount);
            for (const auto& sp : fp.suspensionPoints) {
                hashCoroMix64(h, sp.ordinal);
                hashCoroMix64(h, sp.awaitLocalIndex);
                hashCoroMix64(h, sp.resumeBlockId);
                hashCoroString(h, sp.awaitedTypeName);
                hashCoroString(h, sp.resumeResultTypeName);
                hashCoroMix64(h, sp.resumeBlockParamTypes.size());
                for (const auto& bpt : sp.resumeBlockParamTypes) hashCoroString(h, bpt);
                hashCoroMix64(h, sp.liveInRegisters.size());
                for (const auto& [rIdx, rType] : sp.liveInRegisters) {
                    hashCoroMix64(h, rIdx);
                    hashCoroString(h, rType);
                }
                hashCoroMix64(h, sp.protectingHandlers.size());
                for (const auto& ph : sp.protectingHandlers) hashCoroString(h, ph);
            }
            hashCoroMix64(h, fp.referencedStructLayouts.size());
            for (const auto& [sName, sHash] : fp.referencedStructLayouts) {
                hashCoroString(h, sName);
                hashCoroMix64(h, sHash);
            }
            std::uint64_t coreH = h;
            if (coreH == 0) coreH = 1;
            fp.coreStateMachineHash = coreH;

            hashCoroMix64(h, fp.referencedClassLayouts.size());
            for (const auto& [cName, cHash] : fp.referencedClassLayouts) {
                hashCoroString(h, cName);
                hashCoroMix64(h, cHash);
            }
            if (h == 0) h = 1;
            fp.structuralHash = h;
            results.push_back(std::move(fp));
        }
    }

    // Also compute deterministic structural fingerprints for `sequence` state machines
    for (const auto& module : program.modules()) {
        for (const auto& seq : module.languageMetadata.sequences) {
            const semantic::TypeSymbol* ownerType = nullptr;
            for (const auto& t : module.types) {
                if (semantic::canonicalTypeName(t) == seq.typeName || t.name == seq.typeName) {
                    ownerType = &t;
                    break;
                }
            }
            if (!ownerType) continue;

            const std::string ownerCanonical = semantic::canonicalTypeName(*ownerType);
            CoroutineStateMachineFingerprint fp;
            fp.isSequence = true;
            fp.canonicalName = ownerCanonical + "." + seq.name;
            fp.returnTypeName = seq.resultTypeName;

            std::map<std::string, std::uint64_t> seqRefStructs;
            std::map<std::string, std::uint64_t> seqRefClasses;
            std::unordered_set<std::string> seqVisitedTypes;
            collectReferencedTypesRecursive(
                program, ownerType, layoutMemo, seqRefStructs, seqRefClasses, seqVisitedTypes);

            for (const auto& m : ownerType->methods) {
                if (m.name == seq.name && m.method && !m.staticMethod) {
                    fp.symbolId = m.id;
                    if (const auto fIt = linearIndexBySymbol.find(m.id);
                        fIt != linearIndexBySymbol.end()) {
                        fp.functionIndex = fIt->second;
                    }
                    for (const auto& p : m.parameters) {
                        const semantic::SymbolId ptid =
                            semantic::isExactType(p.type) && !p.typeName.empty()
                                ? semantic::stableTypeId(p.typeName)
                                : 0;
                        fp.parameterNames.push_back(p.name);
                        fp.parameterSlotTypes.push_back(
                            formatProgramSlotType(program, p.type, ptid, p.typeName));
                        if (const auto* td = unwrapToRecordOrStructType(program, p.type, ptid, p.typeName)) {
                            collectReferencedTypesRecursive(
                                program, td, layoutMemo, seqRefStructs, seqRefClasses, seqVisitedTypes);
                        }
                    }
                    break;
                }
            }

            std::size_t stateFieldIndex = static_cast<std::size_t>(-1);
            const std::string stateFieldName = "$sequence_state_" + seq.name;
            const auto seqFieldNames = collectSequenceOwnedFieldNames(*ownerType, seq.name);
            const std::unordered_set<std::string> seqFieldSet(
                seqFieldNames.begin(), seqFieldNames.end());

            for (const auto& fld : ownerType->fields) {
                if (!fld.synthetic || seqFieldSet.find(fld.name) == seqFieldSet.end()) continue;
                if (fld.name == stateFieldName) {
                    stateFieldIndex = fld.index;
                }
                if (fld.name.rfind("$sequence_local_", 0) == 0 ||
                    fld.name.rfind("$sequence_foreach_", 0) == 0 ||
                    fld.name.rfind("$sequence_parameter_", 0) == 0 ||
                    fld.name.rfind("$sequence_result_", 0) == 0) {
                    const semantic::SymbolId ftid =
                        semantic::isExactType(fld.type) && !fld.typeName.empty()
                            ? semantic::stableTypeId(fld.typeName)
                            : 0;
                    const std::string slotTypeStr =
                        formatProgramSlotType(program, fld.type, ftid, fld.typeName);
                    fp.localSlotTypes.push_back(fld.name + ":" + slotTypeStr);
                    fp.debugLocals.push_back(CoroutineLocalDebugDesc{
                        fld.name,
                        static_cast<std::uint32_t>(fp.debugLocals.size()),
                        fld.name.rfind("$sequence_parameter_", 0) == 0,
                        slotTypeStr,
                    });
                    if (const auto* td = unwrapToRecordOrStructType(program, fld.type, ftid, fld.typeName)) {
                        collectReferencedTypesRecursive(
                            program, td, layoutMemo, seqRefStructs, seqRefClasses, seqVisitedTypes);
                    }
                }
            }
            fp.referencedStructLayouts.assign(seqRefStructs.begin(), seqRefStructs.end());
            fp.referencedClassLayouts.assign(seqRefClasses.begin(), seqRefClasses.end());

            // Extract yield suspension state IDs and schedule signatures in encounter order
            struct SequenceYieldPoint {
                std::uint32_t stateId = 0;
                std::string scheduleSignature;
            };
            std::vector<SequenceYieldPoint> yieldPoints;
            if (stateFieldIndex != static_cast<std::size_t>(-1)) {
                std::vector<std::string> orderedSeqFuncNames;
                orderedSeqFuncNames.reserve(1u + seq.callbacks.size());
                orderedSeqFuncNames.push_back(ownerType->name + "." + seq.name);
                for (const auto& cbName : seq.callbacks) {
                    orderedSeqFuncNames.push_back(ownerType->name + "." + cbName);
                }
                for (const auto& seqFnName : orderedSeqFuncNames) {
                    for (const auto& fn : module.functions) {
                        if (fn.name != seqFnName) continue;
                        for (const auto& blk : fn.blocks) {
                            std::unordered_map<bytecode::Register, std::int64_t> constInts;
                            std::unordered_map<bytecode::Register, std::string> constStrings;
                            for (std::size_t ii = 0; ii < blk.instructions.size(); ++ii) {
                                const auto& inst = blk.instructions[ii];
                                if (inst.opcode == bytecode::Opcode::ConstantInt &&
                                    inst.result != bytecode::InvalidRegister) {
                                    constInts[inst.result] = inst.activeIntegerImmediate();
                                } else if (inst.opcode == bytecode::Opcode::ConstantString &&
                                           inst.result != bytecode::InvalidRegister) {
                                    constStrings[inst.result] = inst.stringImmediate;
                                } else if (inst.opcode == bytecode::Opcode::StoreField &&
                                           inst.index == stateFieldIndex &&
                                           inst.operands.size() == 2) {
                                    const auto cIt = constInts.find(inst.operands[1]);
                                    if (cIt != constInts.end() && cIt->second > 0) {
                                        const auto sid = static_cast<std::uint32_t>(cIt->second);
                                        std::string schedSig = fn.name;
                                        for (std::size_t jj = ii + 1u; jj < blk.instructions.size(); ++jj) {
                                            const auto& nextInst = blk.instructions[jj];
                                            if (nextInst.opcode == bytecode::Opcode::StoreField &&
                                                nextInst.index == stateFieldIndex) {
                                                break;
                                            }
                                            if (nextInst.opcode == bytecode::Opcode::ConstantString &&
                                                nextInst.result != bytecode::InvalidRegister) {
                                                constStrings[nextInst.result] = nextInst.stringImmediate;
                                            }
                                            if (nextInst.opcode == bytecode::Opcode::Call &&
                                                nextInst.index < module.functionReferences.size()) {
                                                schedSig += "|" + module.functionReferences[nextInst.index].name;
                                                for (const auto opReg : nextInst.operands) {
                                                    if (const auto sIt = constStrings.find(opReg);
                                                        sIt != constStrings.end()) {
                                                        schedSig += ":" + sIt->second;
                                                    }
                                                }
                                                break;
                                            }
                                        }
                                        yieldPoints.push_back(SequenceYieldPoint{sid, std::move(schedSig)});
                                    }
                                }
                            }
                        }
                    }
                }
            }

            if (!yieldPoints.empty()) {
                std::uint32_t ord = 0;
                for (auto& yp : yieldPoints) {
                    CoroutineSuspensionPointDesc sp;
                    sp.ordinal = ord++;
                    sp.resumeBlockId = yp.stateId;
                    sp.awaitedTypeName = std::move(yp.scheduleSignature);
                    fp.suspensionPoints.push_back(std::move(sp));
                }
            } else {
                for (std::size_t i = 0; i < seq.callbacks.size(); ++i) {
                    CoroutineSuspensionPointDesc sp;
                    sp.ordinal = static_cast<std::uint32_t>(i);
                    sp.resumeBlockId = static_cast<std::uint32_t>(i + 1u);
                    sp.awaitedTypeName = seq.callbacks[i];
                    fp.suspensionPoints.push_back(std::move(sp));
                }
            }
            fp.suspensionPointCount = fp.suspensionPoints.size();

            std::uint64_t h = kCoroFpFnvOffset;
            hashCoroMix64(h, 1u); // sequence tag
            hashCoroString(h, fp.canonicalName);
            hashCoroString(h, fp.returnTypeName);
            hashCoroMix64(h, fp.parameterNames.size());
            for (const auto& pn : fp.parameterNames) hashCoroString(h, pn);
            hashCoroMix64(h, fp.parameterSlotTypes.size());
            for (const auto& pt : fp.parameterSlotTypes) hashCoroString(h, pt);
            hashCoroMix64(h, fp.localSlotTypes.size());
            for (const auto& lt : fp.localSlotTypes) hashCoroString(h, lt);
            hashCoroMix64(h, seq.callbacks.size());
            for (const auto& cb : seq.callbacks) hashCoroString(h, cb);
            hashCoroMix64(h, fp.suspensionPointCount);
            for (const auto& sp : fp.suspensionPoints) {
                hashCoroMix64(h, sp.ordinal);
                hashCoroMix64(h, sp.resumeBlockId);
                hashCoroString(h, sp.awaitedTypeName);
            }
            hashCoroMix64(h, fp.referencedStructLayouts.size());
            for (const auto& [sName, sHash] : fp.referencedStructLayouts) {
                hashCoroString(h, sName);
                hashCoroMix64(h, sHash);
            }
            std::uint64_t coreH = h;
            if (coreH == 0) coreH = 1;
            fp.coreStateMachineHash = coreH;

            hashCoroMix64(h, fp.referencedClassLayouts.size());
            for (const auto& [cName, cHash] : fp.referencedClassLayouts) {
                hashCoroString(h, cName);
                hashCoroMix64(h, cHash);
            }
            if (h == 0) h = 1;
            fp.structuralHash = h;
            results.push_back(std::move(fp));
        }
    }

    return results;
}

std::optional<CoroutineStateMachineFingerprint> findCoroutineStateMachineFingerprint(
    const runtime::ProgramImage& program,
    const std::string& canonicalName) {
    for (auto& fp : computeProgramCoroutineFingerprints(program)) {
        if (fp.canonicalName == canonicalName) {
            return std::move(fp);
        }
    }
    return std::nullopt;
}

SceneScriptRuntime::~SceneScriptRuntime() noexcept {
    releaseAsyncTemplateRoots();
    releasePendingNetworkAsyncOps();
    if (const auto ctx = runtime_.asyncRuntime()) {
        (void)ctx->awaitables().cancelContinuationsForContext(this);
        for (const auto& [_, op] : pendingPrefabOps_) {
            if (op.ioHandle.valid() && !op.ioHandle.isCompletedVoidSentinel()) {
                (void)ctx->awaitables().trySetCanceled(op.ioHandle);
                (void)ctx->awaitables().discard(op.ioHandle);
            }
            if (op.outerHandle.valid() && !op.outerHandle.isCompletedVoidSentinel()) {
                (void)ctx->awaitables().trySetCanceled(op.outerHandle);
            }
        }
        for (const auto& [_, op] : pendingAssetOps_) {
            if (op.ioHandle.valid() && !op.ioHandle.isCompletedVoidSentinel()) {
                (void)ctx->awaitables().trySetCanceled(op.ioHandle);
                (void)ctx->awaitables().discard(op.ioHandle);
            }
            if (op.outerHandle.valid() && !op.outerHandle.isCompletedVoidSentinel()) {
                (void)ctx->awaitables().trySetCanceled(op.outerHandle);
            }
        }
        for (const auto& pkt : deferredAsyncIoPackets_) {
            (void)ctx->completionQueue().cancelRequest(pkt.requestId);
            if (pkt.targetHandle.valid() && !pkt.targetHandle.isCompletedVoidSentinel()) {
                (void)ctx->awaitables().trySetCanceled(pkt.targetHandle);
            }
        }
        pendingPrefabOps_.clear();
        pendingAssetOps_.clear();
        deferredAsyncIoPackets_.clear();
        pendingLifecycleCoroutines_.clear();
        (void)ctx->completionQueue().pruneStaleRequests(ctx->awaitables());
        for (const auto& [slot, gen] : slotGenerations_) {
            if (gen != 0) {
                (void)ctx->cancellations().cancelEntity(
                    runtime::GenerationalEntityHandle{slot, gen},
                    runtime::EntityDestroyDomain::SceneGenerational);
            }
        }
        for (const auto& [slot, ep] : nonGenDestroyEpoch_) {
            if (ep != 0) {
                (void)ctx->cancellations().cancelEntity(
                    runtime::GenerationalEntityHandle{slot, 0x80000000u | ep},
                    runtime::EntityDestroyDomain::SceneNonGenerational);
            }
        }
    }
}

void SceneScriptRuntime::releaseAsyncTemplateRoots() noexcept {
    auto* heap = runtime_.heap().get();
    for (auto& [_, tokens] : prefabTemplateRootTokens_) {
        releaseRootTokenList(heap, tokens);
    }
    prefabTemplateRootTokens_.clear();
    for (auto& [_, tokens] : asyncAssetDefRootTokens_) {
        releaseRootTokenList(heap, tokens);
    }
    asyncAssetDefRootTokens_.clear();
}

void SceneScriptRuntime::releasePendingNetworkAsyncOps() noexcept {
    std::vector<runtime::AsyncRequestId> receiverReqs;
    {
        std::lock_guard<std::mutex> lock(asyncIoMutex_);
        for (auto& sess : wsSessions_) {
            while (!sess.pendingReceivers.empty()) {
                receiverReqs.push_back(sess.pendingReceivers.front().requestId);
                sess.pendingReceivers.pop_front();
            }
            sess.inboundQueue.clear();
            sess.allocated = false;
            sess.connected = false;
            sess.closed = true;
        }
        wsSessions_.clear();
        freeWsSessionSlots_.clear();
    }

    if (const auto ctx = runtime_.asyncRuntime()) {
        for (const auto reqId : receiverReqs) {
            (void)ctx->completionQueue().cancelRequest(reqId);
        }
        (void)ctx->awaitables().cancelContinuationsForContext(this);

        auto cancelAndRecycleInternal = [&](runtime::AwaitableHandle ioH,
                                            runtime::AwaitableHandle outerH,
                                            runtime::CancellationSourceHandle ctsH) {
            if (ioH.valid() && !ioH.isCompletedVoidSentinel()) {
                (void)ctx->awaitables().trySetCanceled(ioH);
                (void)ctx->awaitables().discard(ioH);
            }
            if (outerH.valid() && !outerH.isCompletedVoidSentinel()) {
                (void)ctx->awaitables().trySetCanceled(outerH);
            }
            if (ctsH.valid()) {
                (void)ctx->cancellations().cancel(ctsH);
                (void)ctx->cancellations().releaseSource(ctsH);
            }
        };

        for (const auto& [_, op] : pendingHttpOps_) {
            cancelAndRecycleInternal(op.ioHandle, op.outerHandle, op.ownedLinkedCts);
        }
        for (const auto& [_, op] : pendingWsConnectOps_) {
            cancelAndRecycleInternal(op.ioHandle, op.outerHandle, op.ownedLinkedCts);
        }
        for (const auto& [_, op] : pendingNetTokenOps_) {
            cancelAndRecycleInternal(op.ioHandle, op.outerHandle, op.ownedLinkedCts);
        }
    }

    pendingHttpOps_.clear();
    pendingWsConnectOps_.clear();
    pendingNetTokenOps_.clear();
}

void SceneScriptRuntime::bindLifecycleMethods(Instance& instance) const {
    const auto& type = instance.object.type();
    instance.onCreate0 = runtime_.findMethod(type, "OnCreate", 0);
    instance.onCreate1 = runtime_.findMethod(type, "OnCreate", 1);
    instance.onStart = runtime_.findMethod(type, "OnStart", 0);
    instance.onEnable = runtime_.findMethod(type, "OnEnable", 0);
    instance.onDisable = runtime_.findMethod(type, "OnDisable", 0);
    instance.onUpdate = runtime_.findMethod(type, "OnUpdate", 1);
    instance.onFixedUpdate = runtime_.findMethod(type, "OnFixedUpdate", 1);
    instance.onLateUpdate = runtime_.findMethod(type, "OnLateUpdate", 1);
    instance.onDestroy = runtime_.findMethod(type, "OnDestroy", 0);
    instance.onValidate = runtime_.findMethod(type, "OnValidate", 0);
    instance.onCollisionEnter = runtime_.findMethod(type, "OnCollisionEnter", 1);
    instance.onCollisionStay = runtime_.findMethod(type, "OnCollisionStay", 1);
    instance.onCollisionExit = runtime_.findMethod(type, "OnCollisionExit", 1);
    instance.onTriggerEnter = runtime_.findMethod(type, "OnTriggerEnter", 1);
    instance.onTriggerStay = runtime_.findMethod(type, "OnTriggerStay", 1);
    instance.onTriggerExit = runtime_.findMethod(type, "OnTriggerExit", 1);
    instance.onCollisionEnter3D = runtime_.findMethod(type, "OnCollisionEnter3D", 1);
    instance.onCollisionStay3D = runtime_.findMethod(type, "OnCollisionStay3D", 1);
    instance.onCollisionExit3D = runtime_.findMethod(type, "OnCollisionExit3D", 1);
    instance.onTriggerEnter3D = runtime_.findMethod(type, "OnTriggerEnter3D", 1);
    instance.onTriggerStay3D = runtime_.findMethod(type, "OnTriggerStay3D", 1);
    instance.onTriggerExit3D = runtime_.findMethod(type, "OnTriggerExit3D", 1);
    instance.sequenceCancelPlan = resolveSequenceCancelPlan(type);
    instance.executeInEditMode = hasEditModeAttribute(
        runtime_.program().get(), type.canonicalName());
    instance.executionOrder = findExecutionOrderAttribute(
        runtime_.program().get(), type.canonicalName());
}

std::shared_ptr<const SceneScriptRuntime::SequenceCancelPlan>
SceneScriptRuntime::resolveSequenceCancelPlan(const ScriptType& type) const {
    const auto found = sequenceCancelPlanCache_.find(type.descriptor.id);
    if (found != sequenceCancelPlanCache_.end()) {
        return found->second;
    }

    static constexpr const char* kPrefix = "$sequence_state_";
    const std::size_t prefixLen = std::strlen(kPrefix);
    constexpr std::size_t kNpos = static_cast<std::size_t>(-1);

    std::size_t sharedTimerIdx = kNpos;
    std::vector<SequenceCancelSlot> slots;
    std::unordered_set<semantic::SymbolId> addedMethodIds;

    for (const auto& field : type.descriptor.fields) {
        if (!field.synthetic) continue;
        if (field.name.size() > prefixLen && field.name.rfind(kPrefix, 0) == 0) {
            const std::string seqName = field.name.substr(prefixLen);
            if (const auto cancelMethod = runtime_.findMethod(type, "Cancel" + seqName, 0)) {
                if (addedMethodIds.insert(cancelMethod->descriptor.id).second) {
                    slots.push_back(SequenceCancelSlot{field.index, *cancelMethod});
                }
            }
        } else if (field.name == "$sequence_timer") {
            sharedTimerIdx = field.index;
        }
    }

    if (sharedTimerIdx != kNpos) {
        for (const auto& method : type.descriptor.methods) {
            if (method.synthetic &&
                method.name.rfind("Cancel", 0) == 0 &&
                method.parameters.size() == 1 &&
                addedMethodIds.insert(method.id).second) {
                slots.push_back(SequenceCancelSlot{kNpos, ScriptMethod{method}});
            }
        }
    }

    std::shared_ptr<const SequenceCancelPlan> plan;
    if (!slots.empty()) {
        auto built = std::make_shared<SequenceCancelPlan>();
        built->sharedTimerFieldIndex = sharedTimerIdx;
        built->slots = std::move(slots);
        plan = std::move(built);
    }
    sequenceCancelPlanCache_.emplace(type.descriptor.id, plan);
    return plan;
}

void SceneScriptRuntime::cancelInstanceSequencesFast(const Instance& instance) const {
    ++legacySequenceCancelChecks_;
    if (!instance.sequenceCancelPlan || !instance.object.valid()) {
        return;
    }
    auto* heap = runtime_.heap().get();
    if (!heap) return;

    constexpr std::size_t kNpos = static_cast<std::size_t>(-1);
    const auto& plan = *instance.sequenceCancelPlan;
    const runtime::ObjectRef objRef = instance.object.reference();
    const bool sharedTimerActive =
        plan.sharedTimerFieldIndex != kNpos &&
        isNonZeroSequenceValue(heap->fieldGet(objRef, plan.sharedTimerFieldIndex));

    for (const auto& slot : plan.slots) {
        const bool active = (slot.stateFieldIndex != kNpos)
            ? (isNonZeroSequenceValue(heap->fieldGet(objRef, slot.stateFieldIndex)) ||
               sharedTimerActive)
            : sharedTimerActive;
        if (active) {
            ++legacySequenceCancelInvocations_;
            (void)runtime_.invoke(instance.object, slot.cancelMethod, {}, executionOptions_);
        }
    }
}

bool SceneScriptRuntime::shouldTickInCurrentMode(
    const Instance& instance) const noexcept {
    if (executionMode_ == SceneExecutionMode::Play) return true;
    return instance.executeInEditMode;
}

bool SceneScriptRuntime::matchesComponentType(
    const ScriptType& componentType,
    const std::string& query) const {
    if (query.empty()) return false;
    if (componentType.canonicalName() == query ||
        componentType.descriptor.name == query) {
        return true;
    }
    if (const auto targetType = runtime_.findType(query)) {
        return runtime_.isTypeAssignable(
            componentType.descriptor.id, targetType->descriptor.id);
    }
    if (runtime_.program()) {
        for (const auto& module : runtime_.program()->modules()) {
            for (const auto& candidate : module.types) {
                if (candidate.name == query &&
                    runtime_.isTypeAssignable(
                        componentType.descriptor.id, candidate.id)) {
                    return true;
                }
            }
        }
    }
    return false;
}

void SceneScriptRuntime::rebuildTickOrderIfNeeded() const {
    if (!tickOrderDirty_ || tickDispatchDepth_ > 0) return;

    const auto* prog = runtime_.program().get();
    if (cachedProgramForFuncIndex_ != prog) {
        functionIndexBySymbol_.clear();
        if (prog) {
            std::uint32_t linearIdx = 0;
            for (const auto& mod : prog->modules()) {
                for (const auto& fn : mod.functions) {
                    functionIndexBySymbol_.emplace(fn.symbolId, linearIdx++);
                }
            }
        }
        cachedProgramForFuncIndex_ = prog;
    }

    cachedTickOrder_.clear();
    std::size_t totalComps = 0;
    for (const auto& [entity, components] : instances_) {
        (void)entity;
        totalComps += components.size();
    }
    cachedTickOrder_.reserve(totalComps);

    for (const auto& [entity, components] : instances_) {
        for (std::size_t idx = 0; idx < components.size(); ++idx) {
            cachedTickOrder_.push_back(
                TickRef{components[idx].executionOrder, entity, idx, components[idx].instanceUid});
        }
    }
    std::sort(
        cachedTickOrder_.begin(),
        cachedTickOrder_.end(),
        [](const TickRef& a, const TickRef& b) {
            if (a.order != b.order) return a.order < b.order;
            if (a.entity != b.entity) return a.entity < b.entity;
            return a.compIndex < b.compIndex;
        });

    flatFixedUpdateSlots_.clear();
    flatUpdateSlots_.clear();
    flatLateUpdateSlots_.clear();
    activeCachedComponents_ = 0;

    std::unordered_map<SceneEntityId, bool> hierarchyActiveMemo;
    hierarchyActiveMemo.reserve(instances_.size());

    auto& mutableInstances = const_cast<std::unordered_map<SceneEntityId, std::vector<Instance>>&>(instances_);
    for (std::size_t cursor = 0; cursor < cachedTickOrder_.size(); ++cursor) {
        const TickRef& ref = cachedTickOrder_[cursor];
        auto memoIt = hierarchyActiveMemo.find(ref.entity);
        if (memoIt == hierarchyActiveMemo.end()) {
            memoIt = hierarchyActiveMemo.emplace(ref.entity, isActiveInHierarchy(ref.entity)).first;
        }
        if (!memoIt->second) continue;

        const auto instIt = mutableInstances.find(ref.entity);
        if (instIt == mutableInstances.end() || ref.compIndex >= instIt->second.size()) continue;
        Instance& instance = instIt->second[ref.compIndex];
        if (!instance.enabled || !shouldTickInCurrentMode(instance)) continue;
        if (!instance.started && executionMode_ == SceneExecutionMode::Play) continue;
        if (!instance.object.valid()) continue;

        ++activeCachedComponents_;
        const runtime::ObjectRef recvRef = instance.object.reference();

        const auto appendPhaseSlot = [&](
            std::vector<FlatLifecycleSlot>& outVec,
            const std::optional<ScriptMethod>& optMethod) {
            if (!optMethod || !optMethod->valid()) return;
            const semantic::SymbolId symId = optMethod->descriptor.id;
            std::uint32_t fnIdx = 0;
            if (const auto fIt = functionIndexBySymbol_.find(symId);
                fIt != functionIndexBySymbol_.end()) {
                fnIdx = fIt->second;
            }
            outVec.push_back(FlatLifecycleSlot{
                ref.entity,
                ref.compIndex,
                ref.instanceUid,
                cursor,
                fnIdx,
                symId,
                recvRef,
                &instance,
                &*optMethod,
                methodReturnsAwaitableStruct(*optMethod),
            });
        };

        appendPhaseSlot(flatFixedUpdateSlots_, instance.onFixedUpdate);
        appendPhaseSlot(flatUpdateSlots_, instance.onUpdate);
        appendPhaseSlot(flatLateUpdateSlots_, instance.onLateUpdate);
    }

    tickOrderDirty_ = false;
    ++hotPathStats_.cacheRebuilds;
}

SceneLifecycleHotPathStats SceneScriptRuntime::lifecycleHotPathStats() const noexcept {
    SceneLifecycleHotPathStats out = hotPathStats_;
    out.structuralGeneration = structuralGeneration_;
    out.activeCachedComponents = activeCachedComponents_;
    out.cachedFixedUpdateSlots = flatFixedUpdateSlots_.size();
    out.cachedUpdateSlots = flatUpdateSlots_.size();
    out.cachedLateUpdateSlots = flatLateUpdateSlots_.size();
    out.currentDispatchDepth = tickDispatchDepth_;
    return out;
}

void SceneScriptRuntime::resetLifecycleHotPathStats() noexcept {
    hotPathStats_ = SceneLifecycleHotPathStats{};
}

void SceneScriptRuntime::invalidateTransformSubtree(SceneEntityId entity) {
    if (entity == BroadcastTarget) return;
    worldTransformCache_.erase(entity);
    const auto chIt = childrenByEntity_.find(entity);
    if (chIt == childrenByEntity_.end()) return;
    for (const auto child : chIt->second) {
        invalidateTransformSubtree(child);
    }
}

SceneScriptRuntime::Instance* SceneScriptRuntime::findComponentByUid(
    SceneEntityId entity,
    std::uint64_t instanceUid,
    std::size_t hintIndex) noexcept {
    if (instanceUid == 0) return nullptr;
    const auto it = instances_.find(entity);
    if (it == instances_.end()) return nullptr;
    auto& vec = it->second;
    if (hintIndex < vec.size() && vec[hintIndex].instanceUid == instanceUid) {
        return &vec[hintIndex];
    }
    for (auto& inst : vec) {
        if (inst.instanceUid == instanceUid) {
            return &inst;
        }
    }
    return nullptr;
}

bool SceneScriptRuntime::attachComponentWithoutLifecycle(
    SceneEntityId entity,
    const std::string& scriptType,
    SceneScriptOptions options,
    std::size_t& outCompIndex) {
    if (entity == BroadcastTarget || isStaleGenerationalEntity(entity)) return false;
    const auto existingEntity = instances_.find(entity);
    if (existingEntity != instances_.end()) {
        for (const auto& comp : existingEntity->second) {
            if (comp.object.type().canonicalName() == scriptType ||
                comp.object.type().descriptor.name == scriptType) {
                return false;
            }
        }
    }

    runtime::RuntimeError error;
    auto object = runtime_.createObject(scriptType, error, executionOptions_);
    if (!object) {
        recordError(entity, "OnCreate", error);
        return false;
    }
    if (entityActiveSelf_.find(entity) == entityActiveSelf_.end()) {
        entityActiveSelf_[entity] = true;
    }
    for (auto& [name, value] : options.initialMembers) {
        if (!runtime_.setMember(*object, name, std::move(value), error, executionOptions_)) {
            recordError(entity, "SetMember:" + name, error);
            return false;
        }
    }

    Instance instance;
    instance.instanceUid = nextInstanceUid_++;
    instance.object = std::move(*object);
    instance.enabled = options.enabled;
    bindLifecycleMethods(instance);

    auto& list = instances_[entity];
    outCompIndex = list.size();
    list.push_back(std::move(instance));
    hasUnstartedPlayComponents_ = true;
    markLifecycleCacheDirty();
    return true;
}

bool SceneScriptRuntime::attach(
    SceneEntityId entity,
    const std::string& scriptType,
    SceneScriptOptions options) {
    if (entity == BroadcastTarget || instances_.find(entity) != instances_.end()) {
        return false;
    }
    return attachComponent(entity, scriptType, std::move(options));
}

bool SceneScriptRuntime::attachComponent(
    SceneEntityId entity,
    const std::string& scriptType,
    SceneScriptOptions options) {
    if (entity == BroadcastTarget || isStaleGenerationalEntity(entity)) {
        return false;
    }
    const auto existingEntity = instances_.find(entity);
    if (existingEntity != instances_.end()) {
        for (const auto& comp : existingEntity->second) {
            if (comp.object.type().canonicalName() == scriptType ||
                comp.object.type().descriptor.name == scriptType) {
                return false;
            }
        }
    }

    runtime::RuntimeError error;
    auto object = runtime_.createObject(scriptType, error, executionOptions_);
    if (!object) {
        recordError(entity, "OnCreate", error);
        return false;
    }
    for (const auto& reqType : findRequiredComponents(
             runtime_.program().get(), object->type().canonicalName())) {
        if (!hasComponent(entity, reqType)) {
            error.code = runtime::ErrorCode::InvalidArguments;
            error.message = "missing required component '" + reqType +
                "' for '" + object->type().canonicalName() + "'";
            recordError(entity, "RequireComponent", error);
            return false;
        }
    }
    if (entityActiveSelf_.find(entity) == entityActiveSelf_.end()) {
        entityActiveSelf_[entity] = true;
    }
    for (auto& [name, value] : options.initialMembers) {
        if (!runtime_.setMember(*object, name, std::move(value), error, executionOptions_)) {
            recordError(entity, "SetMember:" + name, error);
            return false;
        }
    }

    Instance instance;
    instance.instanceUid = nextInstanceUid_++;
    instance.object = std::move(*object);
    instance.enabled = options.enabled;
    bindLifecycleMethods(instance);

    const std::uint64_t createdUid = instance.instanceUid;
    const std::uint64_t expectedResetEpoch = sceneResetEpoch_;
    const std::optional<ScriptMethod> onCreate1Copy = instance.onCreate1;
    const std::optional<ScriptMethod> onCreate0Copy = instance.onCreate0;
    const std::optional<ScriptMethod> onEnableCopy = instance.onEnable;

    auto& list = instances_[entity];
    const std::size_t hintIndex = list.size();
    list.push_back(std::move(instance));
    hasUnstartedPlayComponents_ = true;
    markLifecycleCacheDirty();

    const auto rollbackByUid = [&]() {
        if (sceneResetEpoch_ != expectedResetEpoch) return;
        const auto entIt = instances_.find(entity);
        if (entIt == instances_.end()) return;
        auto& vec = entIt->second;
        for (auto vIt = vec.begin(); vIt != vec.end(); ++vIt) {
            if (vIt->instanceUid == createdUid) {
                vec.erase(vIt);
                break;
            }
        }
        if (vec.empty()) {
            instances_.erase(entIt);
        }
        markLifecycleCacheDirty();
    };

    const bool hasOwner = !std::holds_alternative<std::monostate>(options.owner);
    Instance* createdPtr = findComponentByUid(entity, createdUid, hintIndex);
    if (!createdPtr) return false;
    if (hasOwner && onCreate1Copy) {
        if (!invokeLifecycle(
                entity, *createdPtr, onCreate1Copy, {std::move(options.owner)}, "OnCreate")) {
            rollbackByUid();
            return false;
        }
    } else if (!invokeLifecycle(
            entity, *createdPtr, onCreate0Copy, {}, "OnCreate")) {
        rollbackByUid();
        return false;
    }
    if (sceneResetEpoch_ != expectedResetEpoch) {
        return true;
    }
    Instance* postCreatePtr = findComponentByUid(entity, createdUid, hintIndex);
    if (!postCreatePtr) {
        return true;
    }
    if (postCreatePtr->enabled && isActiveInHierarchy(entity)) {
        if (!invokeLifecycle(entity, *postCreatePtr, onEnableCopy, {}, "OnEnable")) {
            rollbackByUid();
            return false;
        }
    }
    return true;
}

bool SceneScriptRuntime::detach(SceneEntityId entity) {
    if (entity == BroadcastTarget) return false;
    struct DetachDepthScope {
        SceneScriptRuntime& self;
        explicit DetachDepthScope(SceneScriptRuntime& s) noexcept : self(s) {
            ++self.detachDepth_;
        }
        ~DetachDepthScope() {
            if (--self.detachDepth_ == 0 &&
                self.pendingCanceledUnwind_ &&
                !self.inFrameQueueDrain_) {
                self.pendingCanceledUnwind_ = false;
                if (const auto ctx = self.runtime_.asyncRuntime()) {
                    (void)ctx->awaitables().drainReadyContinuations(
                        runtime::ResumeQueue::PreTickIo, 0);
                    (void)ctx->completionQueue().pruneStaleRequests(ctx->awaitables());
                    (void)ctx->awaitables().drainReadyContinuations(
                        runtime::ResumeQueue::FixedTick, 0);
                    (void)ctx->awaitables().drainReadyContinuations(
                        runtime::ResumeQueue::Update, 0);
                    (void)ctx->awaitables().drainReadyContinuations(
                        runtime::ResumeQueue::EndOfFrame, 0);
                }
            }
        }
    } depthScope(*this);
    const auto childrenIt = childrenByEntity_.find(entity);
    if (childrenIt != childrenByEntity_.end()) {
        const auto childrenCopy = childrenIt->second;
        for (const auto child : childrenCopy) {
            (void)detach(child);
        }
        childrenByEntity_.erase(entity);
    }
    const bool wasActiveInHierarchy = isActiveInHierarchy(entity);
    const auto parentIt = parentByEntity_.find(entity);
    const bool hadHierarchyOrTransform =
        parentIt != parentByEntity_.end() ||
        entityActiveSelf_.find(entity) != entityActiveSelf_.end() ||
        localTransforms_.find(entity) != localTransforms_.end() ||
        entityNames_.find(entity) != entityNames_.end() ||
        colliders2D_.find(entity) != colliders2D_.end() ||
        colliders3D_.find(entity) != colliders3D_.end();
    if (parentIt != parentByEntity_.end()) {
        const auto parentId = parentIt->second;
        parentByEntity_.erase(parentIt);
        const auto pChildrenIt = childrenByEntity_.find(parentId);
        if (pChildrenIt != childrenByEntity_.end()) {
            auto& vec = pChildrenIt->second;
            vec.erase(std::remove(vec.begin(), vec.end(), entity), vec.end());
        }
    }
    entityActiveSelf_.erase(entity);
    localTransforms_.erase(entity);
    worldTransformCache_.erase(entity);
    entityNames_.erase(entity);
    for (auto gIt = groupEntities_.begin(); gIt != groupEntities_.end();) {
        auto& vec = gIt->second;
        vec.erase(std::remove(vec.begin(), vec.end(), entity), vec.end());
        if (vec.empty()) {
            gIt = groupEntities_.erase(gIt);
        } else {
            ++gIt;
        }
    }
    (void)removeEntityCollider2D(entity);
    (void)removeEntityCollider3D(entity);

    closeEntityOwnedWebSockets(entity);
    if (cancelEntityDestroyToken(entity)) {
        pendingCanceledUnwind_ = true;
    }

    const std::uint32_t gen = entityIdGeneration(entity);
    const std::uint32_t slot = entityIdSlot(entity);
    if (gen != 0) {
        const auto genIt = slotGenerations_.find(slot);
        if (genIt != slotGenerations_.end() && genIt->second == gen) {
            ++genIt->second;
            if (genIt->second == 0) genIt->second = 1;
            freeGenerationalSlots_.push_back(slot);
        }
    }

    if (fiberScheduler_) {
        if (gen != 0) {
            (void)fiberScheduler_->killEntityFibers(
                runtime::GenerationalEntityHandle{slot, gen});
        } else {
            (void)fiberScheduler_->killEntityFibersBySlot(slot);
        }
    }

    removeSignalsForEntityOrComponent(entity, {});

    const auto found = instances_.find(entity);
    if (found == instances_.end()) {
        if (hadHierarchyOrTransform) {
            markLifecycleCacheDirty();
        }
        return hadHierarchyOrTransform;
    }
    markLifecycleCacheDirty();
    for (auto& instance : found->second) {
        cancelInstanceSequencesFast(instance);
        if (shouldTickInCurrentMode(instance) || instance.started) {
            if (instance.enabled && wasActiveInHierarchy) {
                (void)invokeLifecycle(entity, instance, instance.onDisable, {}, "OnDisable");
            }
            (void)invokeLifecycle(entity, instance, instance.onDestroy, {}, "OnDestroy");
        }
    }
    instances_.erase(entity);
    markLifecycleCacheDirty();
    return true;
}

bool SceneScriptRuntime::detachComponent(
    SceneEntityId entity,
    const std::string& scriptType) {
    const auto found = instances_.find(entity);
    if (found == instances_.end()) return false;
    auto& list = found->second;
    for (auto it = list.begin(); it != list.end(); ++it) {
        if (matchesComponentType(it->object.type(), scriptType)) {
            const std::string canonicalRemoved = it->object.type().canonicalName();
            removeSignalsForEntityOrComponent(entity, canonicalRemoved);
            markLifecycleCacheDirty();
            cancelInstanceSequencesFast(*it);
            if (shouldTickInCurrentMode(*it) || it->started) {
                if (it->enabled && isActiveInHierarchy(entity)) {
                    (void)invokeLifecycle(entity, *it, it->onDisable, {}, "OnDisable");
                }
                (void)invokeLifecycle(entity, *it, it->onDestroy, {}, "OnDestroy");
            }
            const auto refound = instances_.find(entity);
            if (refound != instances_.end()) {
                auto& curList = refound->second;
                for (auto curIt = curList.begin(); curIt != curList.end(); ++curIt) {
                    if (curIt->object.type().canonicalName() == canonicalRemoved) {
                        curList.erase(curIt);
                        break;
                    }
                }
                if (curList.empty()) instances_.erase(refound);
            }
            markLifecycleCacheDirty();
            return true;
        }
    }
    return false;
}

bool SceneScriptRuntime::isEntityActiveSelf(SceneEntityId entity) const noexcept {
    if (entity == BroadcastTarget) return false;
    const auto it = entityActiveSelf_.find(entity);
    if (it != entityActiveSelf_.end()) return it->second;
    return true;
}

bool SceneScriptRuntime::setEntityActive(SceneEntityId entity, bool active) {
    if (entity == BroadcastTarget || isStaleGenerationalEntity(entity)) return false;
    const bool currentSelf = isEntityActiveSelf(entity);
    if (entityActiveSelf_.find(entity) != entityActiveSelf_.end() &&
        currentSelf == active) {
        return false;
    }
    const bool parentActive =
        getParent(entity) == BroadcastTarget || isActiveInHierarchy(getParent(entity));
    const bool wasActiveInHierarchy = currentSelf && parentActive;

    entityActiveSelf_[entity] = active;
    markLifecycleCacheDirty();

    const bool nowActiveInHierarchy = active && parentActive;
    if (nowActiveInHierarchy) {
        hasUnstartedPlayComponents_ = true;
    }
    if (wasActiveInHierarchy == nowActiveInHierarchy) {
        return true;
    }

    const auto notifyNodeComponents = [&](SceneEntityId nodeId, bool becomingActive) -> bool {
        if (!becomingActive && fiberScheduler_) {
            const std::uint32_t nodeGen = entityIdGeneration(nodeId);
            const std::uint32_t nodeSlot = entityIdSlot(nodeId);
            if (nodeGen != 0) {
                (void)fiberScheduler_->killEntityFibers(
                    runtime::GenerationalEntityHandle{nodeSlot, nodeGen});
            } else {
                (void)fiberScheduler_->killEntityFibersBySlot(nodeSlot);
            }
        }
        const auto instIt = instances_.find(nodeId);
        if (instIt == instances_.end()) return true;
        bool ok = true;
        for (auto& comp : instIt->second) {
            if (!comp.enabled) continue;
            if (!becomingActive) {
                cancelInstanceSequencesFast(comp);
            }
            const auto& cb = becomingActive ? comp.onEnable : comp.onDisable;
            if (!invokeLifecycle(
                    nodeId,
                    comp,
                    cb,
                    {},
                    becomingActive ? "OnEnable" : "OnDisable")) {
                ok = false;
            }
        }
        return ok;
    };

    (void)notifyNodeComponents(entity, nowActiveInHierarchy);

    const auto notifySubtree = [&](const auto& self, SceneEntityId parentNode) -> void {
        const auto chIt = childrenByEntity_.find(parentNode);
        if (chIt == childrenByEntity_.end()) return;
        const auto childrenCopy = chIt->second;
        for (const auto childId : childrenCopy) {
            if (!isEntityActiveSelf(childId)) continue;
            (void)notifyNodeComponents(childId, nowActiveInHierarchy);
            self(self, childId);
        }
    };
    notifySubtree(notifySubtree, entity);
    markLifecycleCacheDirty();
    return true;
}

bool SceneScriptRuntime::setEnabled(SceneEntityId entity, bool enabled) {
    if (entity == BroadcastTarget) return false;
    const auto found = instances_.find(entity);
    if (found == instances_.end() || found->second.empty()) {
        // Pure transform/hierarchy node without components: delegate to setEntityActive
        return setEntityActive(entity, enabled);
    }
    if (found->second.front().enabled == enabled) {
        return false;
    }
    const bool parentActive =
        getParent(entity) == BroadcastTarget || isActiveInHierarchy(getParent(entity));
    auto& instance = found->second.front();
    if (!enabled) {
        cancelInstanceSequencesFast(instance);
        // Only terminate entity-wide fibers if this is the sole component on the entity
        if (fiberScheduler_ && found->second.size() == 1) {
            const std::uint32_t entGen = entityIdGeneration(entity);
            const std::uint32_t entSlot = entityIdSlot(entity);
            if (entGen != 0) {
                (void)fiberScheduler_->killEntityFibers(
                    runtime::GenerationalEntityHandle{entSlot, entGen});
            } else {
                (void)fiberScheduler_->killEntityFibersBySlot(entSlot);
            }
        }
    }
    const auto& callback = enabled ? instance.onEnable : instance.onDisable;
    if (parentActive) {
        if (!invokeLifecycle(
                entity,
                instance,
                callback,
                {},
                enabled ? "OnEnable" : "OnDisable")) {
            return false;
        }
    }
    instance.enabled = enabled;
    if (enabled && !instance.started) {
        hasUnstartedPlayComponents_ = true;
    }
    markLifecycleCacheDirty();
    return true;
}

bool SceneScriptRuntime::setComponentEnabled(
    SceneEntityId entity,
    const std::string& scriptType,
    bool enabled) {
    const auto found = instances_.find(entity);
    if (found == instances_.end()) return false;
    for (auto& instance : found->second) {
        if (!matchesComponentType(instance.object.type(), scriptType)) continue;
        if (instance.enabled == enabled) return false;
        if (!enabled) {
            cancelInstanceSequencesFast(instance);
        }
        const auto& callback = enabled ? instance.onEnable : instance.onDisable;
        if (isActiveInHierarchy(entity)) {
            if (!invokeLifecycle(
                    entity,
                    instance,
                    callback,
                    {},
                    enabled ? "OnEnable" : "OnDisable")) {
                return false;
            }
        }
        instance.enabled = enabled;
        if (enabled && !instance.started) {
            hasUnstartedPlayComponents_ = true;
        }
        markLifecycleCacheDirty();
        return true;
    }
    return false;
}

bool SceneScriptRuntime::isComponentEnabled(
    SceneEntityId entity,
    const std::string& scriptType) const noexcept {
    const auto found = instances_.find(entity);
    if (found == instances_.end()) return false;
    for (const auto& instance : found->second) {
        if (matchesComponentType(instance.object.type(), scriptType)) {
            return instance.enabled;
        }
    }
    return false;
}

bool SceneScriptRuntime::isComponentStarted(
    SceneEntityId entity,
    const std::string& scriptType) const noexcept {
    const auto found = instances_.find(entity);
    if (found == instances_.end()) return false;
    for (const auto& instance : found->second) {
        if (matchesComponentType(instance.object.type(), scriptType)) {
            return instance.started;
        }
    }
    return false;
}

bool SceneScriptRuntime::restoreComponentWithoutLifecycle(
    SceneEntityId entity,
    const std::string& scriptType,
    bool enabled,
    bool started) {
    SceneScriptOptions opts;
    opts.enabled = enabled;
    std::size_t compIndex = 0;
    if (!attachComponentWithoutLifecycle(entity, scriptType, std::move(opts), compIndex)) {
        return false;
    }
    instances_[entity][compIndex].started = started;
    markLifecycleCacheDirty();
    return true;
}

void SceneScriptRuntime::restoreEntityActiveWithoutLifecycle(
    SceneEntityId entity,
    bool active) {
    if (entity == BroadcastTarget || isStaleGenerationalEntity(entity)) return;
    entityActiveSelf_[entity] = active;
    markLifecycleCacheDirty();
}

void SceneScriptRuntime::clearForSnapshotImport() {
    ++sceneResetEpoch_;
    if (fiberScheduler_) {
        for (const auto entity : entities()) {
            const std::uint32_t gen = entityIdGeneration(entity);
            const std::uint32_t slot = entityIdSlot(entity);
            if (gen != 0) {
                (void)fiberScheduler_->killEntityFibers(
                    runtime::GenerationalEntityHandle{slot, gen});
            } else {
                (void)fiberScheduler_->killEntityFibersBySlot(slot);
            }
        }
    }
    for (const auto ent : entities()) {
        (void)cancelEntityDestroyToken(ent);
    }
    for (auto& [entity, comps] : instances_) {
        (void)entity;
        for (auto& comp : comps) {
            cancelInstanceSequencesFast(comp);
        }
    }
    instances_.clear();
    parentByEntity_.clear();
    childrenByEntity_.clear();
    entityActiveSelf_.clear();
    localTransforms_.clear();
    worldTransformCache_.clear();
    markLifecycleCacheDirty();
    cachedTickOrder_.clear();
    flatFixedUpdateSlots_.clear();
    flatUpdateSlots_.clear();
    flatLateUpdateSlots_.clear();
    activeCachedComponents_ = 0;
    signalConnections_.clear();
    currentInputActions_.clear();
    previousInputActions_.clear();
    inputAxes2D_.clear();
    colliders2D_.clear();
    activeOverlaps2D_.clear();
    activeOverlapWasTrigger2D_.clear();
    activeContacts2D_.clear();
    lastBroadphasePairs2D_ = 0;
    colliders3D_.clear();
    activeOverlaps3D_.clear();
    activeOverlapWasTrigger3D_.clear();
    activeContacts3D_.clear();
    lastBroadphasePairs3D_ = 0;
    pendingDestroyEntities_.clear();
    entityNames_.clear();
    groupEntities_.clear();
    asyncRequests_.clear();
    pendingAsyncCompletions_.clear();
    nextAsyncRequestId_ = 1;
    nonGenDestroyEpoch_.clear();
    sceneStarted_ = false;
    hasUnstartedPlayComponents_ = false;
    pendingCanceledUnwind_ = false;
    releasePendingNetworkAsyncOps();
    lastHttpStatusCode_ = 0;
    lastHttpResponseBody_.clear();
    lastHttpTimedOut_ = false;
    pendingPrefabOps_.clear();
    pendingAssetOps_.clear();
    deferredAsyncIoPackets_.clear();
    pendingLifecycleCoroutines_.clear();
    if (const auto ctx = runtime_.asyncRuntime()) {
        ctx->reset();
    }
    eventQueue_.clear();
    resetGenerationalSlots();
}

void SceneScriptRuntime::startPendingComponents() {
    const std::uint64_t expectedResetEpoch = sceneResetEpoch_;
    for (std::size_t pass = 0; pass < 16u; ++pass) {
        if (sceneResetEpoch_ != expectedResetEpoch) return;
        hasUnstartedPlayComponents_ = false;
        std::vector<TickRef> snapshot;
        if (tickDispatchDepth_ > 0 && tickOrderDirty_) {
            std::size_t totalComps = 0;
            for (const auto& [entity, components] : instances_) {
                (void)entity;
                totalComps += components.size();
            }
            snapshot.reserve(totalComps);
            for (const auto& [entity, components] : instances_) {
                for (std::size_t idx = 0; idx < components.size(); ++idx) {
                    snapshot.push_back(
                        TickRef{components[idx].executionOrder, entity, idx, components[idx].instanceUid});
                }
            }
            std::sort(
                snapshot.begin(),
                snapshot.end(),
                [](const TickRef& a, const TickRef& b) {
                    if (a.order != b.order) return a.order < b.order;
                    if (a.entity != b.entity) return a.entity < b.entity;
                    return a.compIndex < b.compIndex;
                });
        } else {
            rebuildTickOrderIfNeeded();
            snapshot = cachedTickOrder_;
        }
        bool startedAny = false;
        for (const auto& ref : snapshot) {
            if (sceneResetEpoch_ != expectedResetEpoch) return;
            if (!isActiveInHierarchy(ref.entity)) continue;
            auto* instance = findComponentByUid(ref.entity, ref.instanceUid, ref.compIndex);
            if (!instance) continue;
            if (!instance->enabled || instance->started ||
                !shouldTickInCurrentMode(*instance)) {
                continue;
            }
            const std::optional<ScriptMethod> onStartCopy = instance->onStart;
            if (invokeLifecycle(ref.entity, *instance, onStartCopy, {}, "OnStart")) {
                if (sceneResetEpoch_ != expectedResetEpoch) return;
                if (auto* postStart = findComponentByUid(ref.entity, ref.instanceUid, ref.compIndex)) {
                    postStart->started = true;
                    startedAny = true;
                }
            } else if (sceneResetEpoch_ != expectedResetEpoch) {
                return;
            }
        }
        if (startedAny) {
            markLifecycleCacheDirty();
        }
        if (!hasUnstartedPlayComponents_) {
            break;
        }
    }
}

void SceneScriptRuntime::start() {
    if (executionMode_ == SceneExecutionMode::Play) {
        sceneStarted_ = true;
    }
    startPendingComponents();
}

void SceneScriptRuntime::dispatchTickRefsFallback(
    const TickRef* refs,
    std::size_t count,
    LifecyclePhase phase,
    double deltaSeconds,
    const char* callbackName) {
    if (!refs || count == 0) return;
    const std::uint64_t expectedResetEpoch = sceneResetEpoch_;
    std::vector<runtime::Value> argVec{runtime::Value{deltaSeconds}};
    for (std::size_t i = 0; i < count; ++i) {
        if (sceneResetEpoch_ != expectedResetEpoch) return;
        const TickRef& ref = refs[i];
        if (!isActiveInHierarchy(ref.entity)) continue;
        auto* instance = findComponentByUid(ref.entity, ref.instanceUid, ref.compIndex);
        if (!instance) continue;
        if (!instance->enabled || !shouldTickInCurrentMode(*instance)) continue;
        if (!instance->started && executionMode_ == SceneExecutionMode::Play) {
            continue;
        }
        std::optional<ScriptMethod> methodCopy;
        switch (phase) {
        case LifecyclePhase::FixedUpdate:
            methodCopy = instance->onFixedUpdate;
            break;
        case LifecyclePhase::Update:
            methodCopy = instance->onUpdate;
            break;
        case LifecyclePhase::LateUpdate:
            methodCopy = instance->onLateUpdate;
            break;
        }
        if (!methodCopy) continue;
        ++hotPathStats_.fallbackDispatchedCallbacks;
        (void)invokeLifecycle(ref.entity, *instance, methodCopy, argVec, callbackName);
        if (sceneResetEpoch_ != expectedResetEpoch) return;
    }
}

void SceneScriptRuntime::dispatchLifecyclePhase(
    LifecyclePhase phase,
    double deltaSeconds,
    const char* callbackName) {
    struct TickDispatchScope {
        SceneScriptRuntime& self;
        explicit TickDispatchScope(SceneScriptRuntime& s) noexcept : self(s) {
            ++self.tickDispatchDepth_;
        }
        ~TickDispatchScope() noexcept {
            --self.tickDispatchDepth_;
        }
    };

    // Reentrant dispatch guard (`tickDispatchDepth_ > 0`): never mutate cached flat arrays
    // while an outer frame loop is active; take an isolated on-demand snapshot instead.
    if (tickDispatchDepth_ > 0) {
        ++hotPathStats_.reentrantDispatches;
        ++hotPathStats_.onDemandSnapshots;
        TickDispatchScope depthGuard(*this);
        std::vector<TickRef> reentrantSnapshot;
        if (!tickOrderDirty_) {
            reentrantSnapshot = cachedTickOrder_;
        } else {
            for (const auto& [entity, components] : instances_) {
                for (std::size_t idx = 0; idx < components.size(); ++idx) {
                    reentrantSnapshot.push_back(
                        TickRef{components[idx].executionOrder, entity, idx, components[idx].instanceUid});
                }
            }
            std::sort(
                reentrantSnapshot.begin(),
                reentrantSnapshot.end(),
                [](const TickRef& a, const TickRef& b) {
                    if (a.order != b.order) return a.order < b.order;
                    if (a.entity != b.entity) return a.entity < b.entity;
                    return a.compIndex < b.compIndex;
                });
        }
        dispatchTickRefsFallback(
            reentrantSnapshot.data(),
            reentrantSnapshot.size(),
            phase,
            deltaSeconds,
            callbackName);
        return;
    }

    if (tickOrderDirty_) {
        rebuildTickOrderIfNeeded();
    } else {
        ++hotPathStats_.cacheHits;
    }

    const std::vector<FlatLifecycleSlot>& phaseSlots =
        (phase == LifecyclePhase::FixedUpdate)
            ? flatFixedUpdateSlots_
            : (phase == LifecyclePhase::Update ? flatUpdateSlots_ : flatLateUpdateSlots_);
    if (phaseSlots.empty()) {
        return;
    }

    TickDispatchScope depthGuard(*this);
    const std::uint64_t expectedGen = structuralGeneration_;
    const std::uint64_t expectedResetEpoch = sceneResetEpoch_;
    if (tickScratchArgs_.size() != 2u) {
        tickScratchArgs_.resize(2u);
    }
    tickScratchArgs_[1].storeDouble(deltaSeconds);

    const std::size_t slotCount = phaseSlots.size();
    for (std::size_t i = 0; i < slotCount; ++i) {
        const FlatLifecycleSlot& slot = phaseSlots[i];
        const SceneEntityId slotEntity = slot.entity;
        const std::size_t slotTickCursor = slot.tickOrderIndex;
        const semantic::SymbolId slotMethodSymbolId = slot.methodSymbolId;
        const bool slotReturnsAwaitable = slot.returnsAwaitable;
        std::optional<ScriptMethod> awaitableMethodCopy;
        if (slotReturnsAwaitable && slot.methodPtr) {
            awaitableMethodCopy = *slot.methodPtr;
        }

        tickScratchArgs_[0] = runtime::Value{slot.receiverRef};
        ++hotPathStats_.fastDispatchedCallbacks;
        const auto result = runtime_.invokeSymbol(
            slotMethodSymbolId, tickScratchArgs_, executionOptions_);
        if (!result.succeeded) {
            recordError(slotEntity, callbackName ? callbackName : "", result.error);
        } else if (slotReturnsAwaitable && awaitableMethodCopy) {
            (void)consumeFireAndForgetInvocationResult(
                slotEntity,
                *awaitableMethodCopy,
                result,
                callbackName ? std::string_view(callbackName) : std::string_view{});
        }

        // Whole-scene replacement (clear() / deserialization load) inside callback:
        // immediately abort all remaining cursors of the superseded scene!
        if (sceneResetEpoch_ != expectedResetEpoch) {
            break;
        }

        // Structural mutation occurred inside the callback: switch to an on-demand tail snapshot
        // for the remaining components of the initial tick order (resolved by instanceUid!),
        // leaving rebuild deferred to next frame.
        if (structuralGeneration_ != expectedGen) {
            ++hotPathStats_.midTickMutations;
            if (slotTickCursor + 1u < cachedTickOrder_.size()) {
                ++hotPathStats_.onDemandSnapshots;
                const std::vector<TickRef> tailSnapshot(
                    cachedTickOrder_.begin() + static_cast<std::ptrdiff_t>(slotTickCursor + 1u),
                    cachedTickOrder_.end());
                dispatchTickRefsFallback(
                    tailSnapshot.data(),
                    tailSnapshot.size(),
                    phase,
                    deltaSeconds,
                    callbackName);
            }
            break;
        }
    }
}

void SceneScriptRuntime::update(double deltaTime) {
    const std::uint64_t expectedResetEpoch = sceneResetEpoch_;
    (void)advanceAsyncTimeouts(deltaTime);
    (void)stepAsyncPreTickIo();
    (void)flushCompletedAsyncRequests();
    if (sceneResetEpoch_ != expectedResetEpoch) return;
    if (sceneStarted_ && executionMode_ == SceneExecutionMode::Play &&
        hasUnstartedPlayComponents_) {
        startPendingComponents();
        if (sceneResetEpoch_ != expectedResetEpoch) return;
    }
    dispatchLifecyclePhase(LifecyclePhase::Update, deltaTime, "OnUpdate");
    if (sceneResetEpoch_ != expectedResetEpoch) return;
    (void)stepAsyncResumeQueue(runtime::ResumeQueue::Update);
    if (sceneResetEpoch_ != expectedResetEpoch) return;
    (void)flushDestroyedEntities();
}

void SceneScriptRuntime::fixedUpdate(double fixedDeltaTime) {
    const std::uint64_t expectedResetEpoch = sceneResetEpoch_;
    if (sceneStarted_ && executionMode_ == SceneExecutionMode::Play &&
        hasUnstartedPlayComponents_) {
        startPendingComponents();
        if (sceneResetEpoch_ != expectedResetEpoch) return;
    }
    dispatchLifecyclePhase(LifecyclePhase::FixedUpdate, fixedDeltaTime, "OnFixedUpdate");
    if (sceneResetEpoch_ != expectedResetEpoch) return;
    (void)stepAsyncResumeQueue(runtime::ResumeQueue::FixedTick);
    if (sceneResetEpoch_ != expectedResetEpoch) return;
    (void)flushDestroyedEntities();
}

void SceneScriptRuntime::lateUpdate(double deltaTime) {
    const std::uint64_t expectedResetEpoch = sceneResetEpoch_;
    if (sceneStarted_ && executionMode_ == SceneExecutionMode::Play &&
        hasUnstartedPlayComponents_) {
        startPendingComponents();
        if (sceneResetEpoch_ != expectedResetEpoch) return;
    }
    dispatchLifecyclePhase(LifecyclePhase::LateUpdate, deltaTime, "OnLateUpdate");
    if (sceneResetEpoch_ != expectedResetEpoch) return;
    (void)stepAsyncResumeQueue(runtime::ResumeQueue::EndOfFrame);
    (void)flushDestroyedEntities();
}

SceneHotReloadReport SceneScriptRuntime::hotReload(
    const GameProgram& newProgram,
    const ScriptMetadataRegistry* metadata) {
    SceneHotReloadReport report;
    if (!newProgram.valid()) {
        report.errors.push_back("hot reload target GameProgram is invalid");
        return report;
    }

    ScriptMetadataRegistry mergedMetadata;
    if (metadata) {
        mergedMetadata = *metadata;
    }
    mergedMetadata.populateFromProgram(newProgram);

    const auto oldProgramPtr = runtime_.program();
    const auto newProgramPtr = newProgram.program();

    std::vector<CoroutineStateMachineFingerprint> oldCoroFps;
    std::vector<CoroutineStateMachineFingerprint> newCoroFps;
    if (oldProgramPtr) {
        oldCoroFps = computeProgramCoroutineFingerprints(*oldProgramPtr);
    }
    if (newProgramPtr) {
        newCoroFps = computeProgramCoroutineFingerprints(*newProgramPtr);
    }

    std::unordered_map<semantic::SymbolId, const CoroutineStateMachineFingerprint*> oldAsyncBySymbol;
    std::unordered_map<std::string, const CoroutineStateMachineFingerprint*> oldSeqByName;
    for (const auto& fp : oldCoroFps) {
        if (fp.isSequence) {
            oldSeqByName.emplace(fp.canonicalName, &fp);
        } else {
            oldAsyncBySymbol.emplace(fp.symbolId, &fp);
        }
    }

    std::unordered_map<std::string, const CoroutineStateMachineFingerprint*> newAsyncByName;
    std::unordered_map<std::string, const CoroutineStateMachineFingerprint*> newSeqByName;
    for (const auto& fp : newCoroFps) {
        if (fp.isSequence) {
            newSeqByName.emplace(fp.canonicalName, &fp);
        } else {
            newAsyncByName.emplace(fp.canonicalName, &fp);
        }
    }

    // Map explicit `[Guid("...")]` / `[AssetGuid("...")]` attributes on types for GUID-stable rename fallback
    std::unordered_map<std::string, std::string> newTypeNameByExplicitGuid;
    if (newProgramPtr) {
        for (const auto& mod : newProgramPtr->modules()) {
            for (const auto& t : mod.types) {
                const std::string canon = semantic::canonicalTypeName(t);
                const std::string g = findExplicitTypeGuidAttribute(newProgramPtr.get(), canon);
                if (!g.empty()) {
                    newTypeNameByExplicitGuid[g] = canon;
                }
            }
        }
    }

    const auto resolveReloadTargetType = [&](const std::string& oldCanonicalTypeName)
        -> std::optional<ScriptType> {
        if (const auto direct = runtime_.findType(oldCanonicalTypeName)) {
            return direct;
        }
        if (oldProgramPtr) {
            const std::string explicitGuid =
                findExplicitTypeGuidAttribute(oldProgramPtr.get(), oldCanonicalTypeName);
            if (!explicitGuid.empty()) {
                const auto gIt = newTypeNameByExplicitGuid.find(explicitGuid);
                if (gIt != newTypeNameByExplicitGuid.end()) {
                    return runtime_.findType(gIt->second);
                }
            }
        }
        return std::nullopt;
    };

    // Inspect active `sequence` state machines on live instances BEFORE replacing program so
    // incompatible/removed in-flight sequences cancel their scheduled timers cleanly on old program.
    struct SavedComponentState {
        std::string canonicalTypeName;
        runtime::ObjectRef oldRef;
        bool enabled = true;
        bool started = false;
        std::unordered_map<std::string, runtime::Value> fields;
        std::unordered_map<std::int32_t, std::string> fieldNameByTag;
        std::unordered_set<std::string> abortedSequenceFields;
    };
    const auto recordSavedFieldTags = [&](
        SavedComponentState& saved,
        const std::vector<semantic::FieldSymbol>& oldFields) {
        if (!oldProgramPtr) return;
        std::unordered_map<std::int32_t, std::size_t> tagCounts;
        for (const auto& f : oldFields) {
            if (f.staticField || f.constField) continue;
            if (saved.abortedSequenceFields.find(f.name) !=
                saved.abortedSequenceFields.end()) {
                continue;
            }
            const auto tag = findFieldTagAttribute(
                oldProgramPtr.get(), nullptr, saved.canonicalTypeName, f.name, &f);
            if (!tag) continue;
            if (++tagCounts[*tag] == 1) {
                saved.fieldNameByTag[*tag] = f.name;
            } else {
                saved.fieldNameByTag.erase(*tag);
            }
        }
    };
    std::map<SceneEntityId, std::vector<SavedComponentState>> savedEntities;
    std::map<std::string, SavedComponentState> savedAssets;

    for (auto& [entity, components] : instances_) {
        auto& savedList = savedEntities[entity];
        savedList.reserve(components.size());
        for (auto& instance : components) {
            SavedComponentState saved;
            saved.canonicalTypeName = instance.object.type().canonicalName();
            saved.oldRef = instance.object.reference();
            saved.enabled = instance.enabled;
            saved.started = instance.started;

            const auto& oldTypeDesc = instance.object.type().descriptor;
            std::unordered_map<std::string, runtime::Value> rawFields;
            for (const auto& field : oldTypeDesc.fields) {
                if (auto val = runtime_.heap()->fieldGet(
                        instance.object.reference(), field.index)) {
                    rawFields.emplace(field.name, std::move(*val));
                }
            }

            // Evaluate each sequence declared on `oldTypeDesc`
            static constexpr const char* kSeqStatePrefix = "$sequence_state_";
            const std::size_t statePrefixLen = std::strlen(kSeqStatePrefix);
            for (const auto& field : oldTypeDesc.fields) {
                if (!field.synthetic ||
                    field.name.size() <= statePrefixLen ||
                    field.name.rfind(kSeqStatePrefix, 0) != 0) {
                    continue;
                }
                const std::string seqName = field.name.substr(statePrefixLen);
                const std::string timerFieldName = "$sequence_timer_" + seqName;

                std::optional<runtime::Value> stateVal;
                if (const auto sIt = rawFields.find(field.name); sIt != rawFields.end()) {
                    stateVal = sIt->second;
                }
                std::optional<runtime::Value> timerVal;
                if (const auto tIt = rawFields.find(timerFieldName); tIt != rawFields.end()) {
                    timerVal = tIt->second;
                }
                const bool inFlight =
                    isNonZeroSequenceValue(stateVal) || isNonZeroSequenceValue(timerVal);
                if (!inFlight) continue;

                // Resolve target owner canonical name in `newProgram`
                std::string targetOwnerCanonical = saved.canonicalTypeName;
                if (newProgramPtr) {
                    bool existsDirect = false;
                    for (const auto& mod : newProgramPtr->modules()) {
                        for (const auto& t : mod.types) {
                            if (semantic::canonicalTypeName(t) == saved.canonicalTypeName) {
                                existsDirect = true;
                                break;
                            }
                        }
                        if (existsDirect) break;
                    }
                    if (!existsDirect && oldProgramPtr) {
                        const std::string explicitGuid =
                            findExplicitTypeGuidAttribute(oldProgramPtr.get(), saved.canonicalTypeName);
                        if (!explicitGuid.empty()) {
                            if (const auto gIt = newTypeNameByExplicitGuid.find(explicitGuid);
                                gIt != newTypeNameByExplicitGuid.end()) {
                                targetOwnerCanonical = gIt->second;
                            }
                        }
                    }
                }

                const std::string oldSeqKey = saved.canonicalTypeName + "." + seqName;
                const std::string newSeqKey = targetOwnerCanonical + "." + seqName;
                const auto oSeqIt = oldSeqByName.find(oldSeqKey);
                const auto nSeqIt = newSeqByName.find(newSeqKey);

                bool canPreserveSeq = false;
                if (oSeqIt != oldSeqByName.end() && nSeqIt != newSeqByName.end()) {
                    if (targetOwnerCanonical == saved.canonicalTypeName) {
                        canPreserveSeq = oSeqIt->second->matchesCoreStateMachine(*nSeqIt->second);
                    } else {
                        CoroutineStateMachineFingerprint normalizedOld = *oSeqIt->second;
                        normalizedOld.canonicalName = newSeqKey;
                        canPreserveSeq =
                            (normalizedOld.returnTypeName == nSeqIt->second->returnTypeName &&
                             normalizedOld.parameterNames == nSeqIt->second->parameterNames &&
                             normalizedOld.parameterSlotTypes == nSeqIt->second->parameterSlotTypes &&
                             normalizedOld.localSlotTypes == nSeqIt->second->localSlotTypes &&
                             normalizedOld.referencedStructLayouts ==
                                 nSeqIt->second->referencedStructLayouts &&
                             normalizedOld.suspensionPointCount ==
                                 nSeqIt->second->suspensionPointCount);
                    }
                    // Abort sequence if any sequence-owned field holds a non-component heap object
                    // whose class layout changed in newProgram!
                    if (canPreserveSeq && oldProgramPtr && newProgramPtr && runtime_.heap()) {
                        std::unordered_map<std::string, std::uint64_t> oldMemo;
                        std::unordered_map<std::string, std::uint64_t> newMemo;
                        std::unordered_set<std::uint64_t> allSceneCompRefs;
                        for (const auto& [entKey, compVec] : instances_) {
                            (void)entKey;
                            for (const auto& cInst : compVec) {
                                const auto r = cInst.object.reference();
                                if (r.valid()) {
                                    allSceneCompRefs.insert(
                                        (static_cast<std::uint64_t>(r.generation) << 32u) | r.slot);
                                }
                            }
                        }
                        for (const auto& [aPath, aObj] : runtime_.assets_) {
                            (void)aPath;
                            const auto r = aObj.reference();
                            if (r.valid()) {
                                allSceneCompRefs.insert(
                                    (static_cast<std::uint64_t>(r.generation) << 32u) | r.slot);
                            }
                        }
                        std::unordered_set<std::uint64_t> visitedObjs;
                        const auto hasStaleHeapLayout = [&](const auto& self,
                                                            const runtime::Value& val) -> bool {
                            if (const auto* sTid = std::get_if<runtime::StructValue>(&val)) {
                                if (sTid->valid()) {
                                    const auto* oTd = oldProgramPtr->resolveType(sTid->typeId);
                                    const auto* nTd = oTd
                                        ? resolveProgramTypeByNameOrId(
                                              *newProgramPtr,
                                              0,
                                              semantic::canonicalTypeName(*oTd))
                                        : nullptr;
                                    if (!oTd || !nTd) return true;
                                    std::unordered_set<std::string> st1, st2;
                                    if (computeRecursiveRecordOrStructLayoutHash(
                                            *oldProgramPtr, *oTd, st1, oldMemo) !=
                                        computeRecursiveRecordOrStructLayoutHash(
                                            *newProgramPtr, *nTd, st2, newMemo)) {
                                        return true;
                                    }
                                    const std::size_t fc = sTid->fieldCount();
                                    for (std::size_t fi = 0; fi < fc; ++fi) {
                                        if (const auto fv = runtime::structValueFieldAt(*sTid, fi)) {
                                            if (self(self, *fv)) return true;
                                        }
                                    }
                                }
                                return false;
                            }
                            const auto* ref = std::get_if<runtime::ObjectRef>(&val);
                            if (!ref || !ref->valid() || ref->kind == runtime::ObjectKind::String) {
                                return false;
                            }
                            const std::uint64_t key =
                                (static_cast<std::uint64_t>(ref->generation) << 32u) | ref->slot;
                            if (!visitedObjs.insert(key).second) return false;
                            if (ref->kind == runtime::ObjectKind::Array) {
                                const auto len = runtime_.heap()->arrayLength(*ref).value_or(0);
                                for (std::size_t ai = 0; ai < len; ++ai) {
                                    if (const auto ev = runtime_.heap()->arrayGet(*ref, ai)) {
                                        if (self(self, *ev)) return true;
                                    }
                                }
                                return false;
                            }
                            if (allSceneCompRefs.find(key) == allSceneCompRefs.end()) {
                                const auto oTid = runtime_.heap()->objectTypeId(*ref);
                                const auto* oTd = oTid ? oldProgramPtr->resolveType(*oTid) : nullptr;
                                const auto* nTd = oTd
                                    ? resolveProgramTypeByNameOrId(
                                          *newProgramPtr, 0, semantic::canonicalTypeName(*oTd))
                                    : nullptr;
                                if (!oTd || !nTd) return true;
                                std::unordered_set<std::string> st1, st2;
                                if (computeRecursiveRecordOrStructLayoutHash(
                                        *oldProgramPtr, *oTd, st1, oldMemo) !=
                                    computeRecursiveRecordOrStructLayoutHash(
                                        *newProgramPtr, *nTd, st2, newMemo)) {
                                    return true;
                                }
                                const auto fc = runtime_.heap()->fieldCount(*ref).value_or(0);
                                for (std::size_t fi = 0; fi < fc; ++fi) {
                                    if (const auto fv = runtime_.heap()->fieldGet(*ref, fi)) {
                                        if (self(self, *fv)) return true;
                                    }
                                }
                            }
                            return false;
                        };
                        for (const auto& f : oldTypeDesc.fields) {
                            if (auto fv = runtime_.heap()->fieldGet(
                                    instance.object.reference(), f.index)) {
                                if (hasStaleHeapLayout(hasStaleHeapLayout, *fv)) {
                                    canPreserveSeq = false;
                                    break;
                                }
                            }
                        }
                    }
                }

                if (canPreserveSeq) {
                    ++report.preservedCoroutines;
                    report.remappedCoroutineFunctionIndices[oSeqIt->second->functionIndex] =
                        nSeqIt->second->functionIndex;
                } else {
                    if (const auto cancelMethod =
                            runtime_.findMethod(instance.object.type(), "Cancel" + seqName, 0)) {
                        (void)runtime_.invoke(instance.object, *cancelMethod, {}, executionOptions_);
                    }
                    for (const auto& ownedSeqField :
                         collectSequenceOwnedFieldNames(oldTypeDesc, seqName)) {
                        saved.abortedSequenceFields.insert(ownedSeqField);
                    }
                    ++report.abortedCoroutines;
                }
            }

            // Re-snapshot fields after any selective sequence cancellations
            for (const auto& field : oldTypeDesc.fields) {
                if (saved.abortedSequenceFields.find(field.name) !=
                    saved.abortedSequenceFields.end()) {
                    continue;
                }
                auto val = runtime_.heap()->fieldGet(
                    instance.object.reference(), field.index);
                if (val) {
                    saved.fields.emplace(field.name, std::move(*val));
                }
            }
            recordSavedFieldTags(saved, oldTypeDesc.fields);
            savedList.push_back(std::move(saved));
        }
    }

    for (const auto& [assetPath, assetObj] : runtime_.assets_) {
        SavedComponentState saved;
        saved.canonicalTypeName = assetObj.type().canonicalName();
        saved.oldRef = assetObj.reference();
        for (const auto& field : assetObj.type().descriptor.fields) {
            auto val = runtime_.heap()->fieldGet(
                assetObj.reference(), field.index);
            if (val) {
                saved.fields.emplace(field.name, std::move(*val));
            }
        }
        recordSavedFieldTags(saved, assetObj.type().descriptor.fields);
        savedAssets.emplace(assetPath, std::move(saved));
    }

    // Snapshot `[FieldTag(N)]` static fields on `oldProgramPtr` before replacing the program.
    struct OldStaticTaggedField {
        semantic::SymbolId staticId = 0;
        std::string oldCanonicalType;
        std::string oldFieldName;
        runtime::Value value;
    };
    std::map<std::pair<std::string, std::int32_t>, OldStaticTaggedField> oldStaticTaggedFields;
    std::map<std::pair<std::string, std::int32_t>, std::size_t> oldStaticTagCounts;
    std::unordered_map<std::string, std::string> oldTypeByNewCanonical;
    if (oldProgramPtr && newProgramPtr) {
        if (auto* heap = runtime_.heap().get()) {
            for (const auto& module : oldProgramPtr->modules()) {
                for (const auto& typeDesc : module.types) {
                    const std::string oldCanon = semantic::canonicalTypeName(typeDesc);
                    const std::string explicitGuid =
                        findExplicitTypeGuidAttribute(oldProgramPtr.get(), oldCanon);
                    if (!explicitGuid.empty()) {
                        const auto gIt = newTypeNameByExplicitGuid.find(explicitGuid);
                        if (gIt != newTypeNameByExplicitGuid.end()) {
                            oldTypeByNewCanonical.emplace(gIt->second, oldCanon);
                        }
                    }
                    for (const auto& oldField : typeDesc.fields) {
                        if (!oldField.staticField || oldField.constField) continue;
                        const auto tag = findFieldTagAttribute(
                            oldProgramPtr.get(), nullptr, oldCanon, oldField.name, &oldField);
                        if (!tag) continue;
                        const auto key = std::make_pair(oldCanon, *tag);
                        if (++oldStaticTagCounts[key] == 1) {
                            if (const auto val = heap->getStaticField(oldField.id)) {
                                oldStaticTaggedFields.emplace(
                                    key, OldStaticTaggedField{oldField.id, oldCanon, oldField.name, *val});
                            }
                        } else {
                            oldStaticTaggedFields.erase(key);
                        }
                    }
                }
            }
        }
    }

    // Collect currently allocated `async Awaitable` CoroutineFrames in `CoroutineFramePool`
    struct ActiveCoroCandidate {
        runtime::CoroutineFrameHandle handle{};
        runtime::CoroutineFrame* frame = nullptr;
        const CoroutineStateMachineFingerprint* oldFp = nullptr;
        const CoroutineStateMachineFingerprint* newFp = nullptr;
    };
    std::vector<ActiveCoroCandidate> activeCoroFrames;
    const auto asyncCtx = runtime_.asyncRuntime();
    if (asyncCtx) {
        const auto stats = asyncCtx->coroutines().statistics();
        if (stats.activeCount > 0) {
            activeCoroFrames.reserve(stats.activeCount);
            asyncCtx->coroutines().forEachActiveFrame(
                [&](runtime::CoroutineFrameHandle handle, runtime::CoroutineFrame& f) {
                    ActiveCoroCandidate cand;
                    cand.handle = handle;
                    cand.frame = &f;
                    if (const auto oIt = oldAsyncBySymbol.find(f.functionSymbolId);
                        oIt != oldAsyncBySymbol.end()) {
                        cand.oldFp = oIt->second;
                        if (const auto nIt = newAsyncByName.find(cand.oldFp->canonicalName);
                            nIt != newAsyncByName.end()) {
                            cand.newFp = nIt->second;
                        }
                    }
                    activeCoroFrames.push_back(cand);
                });
        }
    }

    runtime_.replaceProgram(newProgram);
    methodCache_.clear();
    sequenceCancelPlanCache_.clear();
    cachedProgramForFuncIndex_ = nullptr;
    functionIndexBySymbol_.clear();
    markLifecycleCacheDirty();

    std::unordered_map<std::uint64_t, runtime::ObjectRef> migratedRefMap;
    std::unordered_set<std::uint64_t> failedComponentOldRefs;
    std::unordered_map<std::string, std::optional<ScriptObject>> prototypeCache;

    const auto migrateSavedFieldsToNewObject = [&](
        const SavedComponentState& saved,
        const ScriptType& newType,
        const ScriptObject& newObj,
        runtime::RuntimeError& allocError) {
        const std::string newCanonicalName = newType.canonicalName();
        const auto& newFields = newType.descriptor.fields;
        std::vector<std::optional<std::string>> matchedOldNames(newFields.size());
        std::unordered_set<std::string> claimedSavedNames;

        const auto matchesSavedField = [&](
            const std::string& oldName,
            const semantic::FieldSymbol& newField) -> bool {
            if (claimedSavedNames.find(oldName) != claimedSavedNames.end()) {
                return false;
            }
            const auto it = saved.fields.find(oldName);
            if (it == saved.fields.end()) return false;
            const auto newFieldTypeId = exactFieldTypeId(
                newField.type, newField.typeName);
            return runtime_.valueMatches(it->second, newField.type, newFieldTypeId);
        };

        // Priority 1: [FieldTag(N)] matching (old tag == new tag).
        for (std::size_t idx = 0; idx < newFields.size(); ++idx) {
            const auto& newField = newFields[idx];
            if (newField.staticField || newField.constField) continue;
            auto newTag = findFieldTagAttribute(
                newProgramPtr.get(), &mergedMetadata, newCanonicalName, newField.name, &newField);
            if (!newTag && newCanonicalName != saved.canonicalTypeName) {
                newTag = findFieldTagAttribute(
                    newProgramPtr.get(), &mergedMetadata, saved.canonicalTypeName, newField.name, &newField);
            }
            if (!newTag) continue;
            const auto tagIt = saved.fieldNameByTag.find(*newTag);
            if (tagIt != saved.fieldNameByTag.end() &&
                matchesSavedField(tagIt->second, newField)) {
                claimedSavedNames.insert(tagIt->second);
                matchedOldNames[idx] = tagIt->second;
            }
        }

        // Priority 2: Exact field name.
        for (std::size_t idx = 0; idx < newFields.size(); ++idx) {
            if (matchedOldNames[idx]) continue;
            const auto& newField = newFields[idx];
            if (matchesSavedField(newField.name, newField)) {
                claimedSavedNames.insert(newField.name);
                matchedOldNames[idx] = newField.name;
            }
        }

        // Priority 3: Ordered multi-alias [FormerlySerializedAs].
        for (std::size_t idx = 0; idx < newFields.size(); ++idx) {
            if (matchedOldNames[idx]) continue;
            const auto& newField = newFields[idx];
            std::vector<std::string> aliases = findRenamedFieldSources(
                mergedMetadata, newCanonicalName, newField.name, &newField, newProgramPtr.get());
            if (newCanonicalName != saved.canonicalTypeName) {
                for (auto& extra : findRenamedFieldSources(
                         mergedMetadata, saved.canonicalTypeName, newField.name, &newField, newProgramPtr.get())) {
                    if (std::find(aliases.begin(), aliases.end(), extra) == aliases.end()) {
                        aliases.push_back(std::move(extra));
                    }
                }
            }
            for (const auto& alias : aliases) {
                if (matchesSavedField(alias, newField)) {
                    claimedSavedNames.insert(alias);
                    matchedOldNames[idx] = alias;
                    break;
                }
            }
        }

        for (std::size_t idx = 0; idx < newFields.size(); ++idx) {
            const auto& newField = newFields[idx];
            const bool isSequenceSynthetic =
                newField.synthetic && newField.name.rfind("$sequence_", 0) == 0;
            const bool isInstanceField =
                !newField.synthetic && !newField.staticField && !newField.constField;
            if (matchedOldNames[idx]) {
                const auto& oldFieldName = *matchedOldNames[idx];
                (void)runtime_.heap()->fieldSet(
                    newObj.reference(),
                    newField.index,
                    saved.fields.at(oldFieldName),
                    &allocError);
                if (isInstanceField) {
                    ++report.fieldsMigrated;
                    if (oldFieldName != newField.name) {
                        ++report.fieldsRenamed;
                    }
                }
                (void)isSequenceSynthetic;
                continue;
            }
            if (isInstanceField) {
                auto protoIt = prototypeCache.find(newCanonicalName);
                if (protoIt == prototypeCache.end()) {
                    runtime::RuntimeError protoErr;
                    protoIt = prototypeCache
                                  .emplace(
                                      newCanonicalName,
                                      runtime_.createPrototypeObject(newType, protoErr))
                                  .first;
                }
                if (protoIt->second) {
                    if (auto protoVal = runtime_.heap()->fieldGet(
                            protoIt->second->reference(), newField.index)) {
                        (void)runtime_.heap()->fieldSet(
                            newObj.reference(), newField.index, *protoVal, &allocError);
                    }
                }
                ++report.fieldsDefaulted;
            }
        }
    };

    for (auto assetIt = runtime_.assets_.begin(); assetIt != runtime_.assets_.end();) {
        const auto savedIt = savedAssets.find(assetIt->first);
        if (savedIt == savedAssets.end()) {
            ++assetIt;
            continue;
        }
        const auto& saved = savedIt->second;
        const auto newType = resolveReloadTargetType(saved.canonicalTypeName);
        if (!newType) {
            if (saved.oldRef.valid()) {
                failedComponentOldRefs.insert(
                    (static_cast<std::uint64_t>(saved.oldRef.generation) << 32u) |
                    saved.oldRef.slot);
            }
            assetIt = runtime_.assets_.erase(assetIt);
            continue;
        }
        runtime::RuntimeError allocError;
        auto newObj = runtime_.allocateUninitializedObject(*newType, allocError);
        if (!newObj) {
            if (saved.oldRef.valid()) {
                failedComponentOldRefs.insert(
                    (static_cast<std::uint64_t>(saved.oldRef.generation) << 32u) |
                    saved.oldRef.slot);
            }
            assetIt = runtime_.assets_.erase(assetIt);
            continue;
        }
        if (saved.oldRef.valid()) {
            const std::uint64_t oldKey =
                (static_cast<std::uint64_t>(saved.oldRef.generation) << 32u) |
                saved.oldRef.slot;
            migratedRefMap[oldKey] = newObj->reference();
        }
        migrateSavedFieldsToNewObject(saved, *newType, *newObj, allocError);
        assetIt->second = std::move(*newObj);
        ++report.instancesMigrated;
        ++assetIt;
    }

    for (const auto& [entity, savedList] : savedEntities) {
        auto instIt = instances_.find(entity);
        if (instIt == instances_.end()) continue;
        auto& components = instIt->second;
        std::vector<Instance> migratedComponents;
        migratedComponents.reserve(components.size());
        for (std::size_t idx = 0; idx < components.size(); ++idx) {
            auto& instance = components[idx];
            const auto& saved = savedList[idx];
            const auto newType = resolveReloadTargetType(saved.canonicalTypeName);
            if (!newType) {
                if (saved.oldRef.valid()) {
                    failedComponentOldRefs.insert(
                        (static_cast<std::uint64_t>(saved.oldRef.generation) << 32u) |
                        saved.oldRef.slot);
                }
                removeSignalsForEntityOrComponent(entity, saved.canonicalTypeName);
                report.errors.push_back(
                    "script type '" + saved.canonicalTypeName +
                    "' was removed in hot-reloaded program");
                continue;
            }
            runtime::RuntimeError allocError;
            auto newObj = runtime_.allocateUninitializedObject(*newType, allocError);
            if (!newObj) {
                if (saved.oldRef.valid()) {
                    failedComponentOldRefs.insert(
                        (static_cast<std::uint64_t>(saved.oldRef.generation) << 32u) |
                        saved.oldRef.slot);
                }
                removeSignalsForEntityOrComponent(entity, saved.canonicalTypeName);
                report.errors.push_back(
                    "failed to allocate migrated instance of '" +
                    saved.canonicalTypeName + "': " + allocError.message);
                continue;
            }

            if (saved.oldRef.valid()) {
                const std::uint64_t oldKey =
                    (static_cast<std::uint64_t>(saved.oldRef.generation) << 32u) |
                    saved.oldRef.slot;
                migratedRefMap[oldKey] = newObj->reference();
            }

            migrateSavedFieldsToNewObject(saved, *newType, *newObj, allocError);

            instance.object = std::move(*newObj);
            instance.enabled = saved.enabled;
            instance.started = saved.started;
            if (!instance.started) {
                hasUnstartedPlayComponents_ = true;
            }
            bindLifecycleMethods(instance);
            const std::string newTypeCanon = instance.object.type().canonicalName();
            if (mergedMetadata.findTypeAttribute(newTypeCanon, "Tool") ||
                mergedMetadata.findTypeAttribute(newTypeCanon, "ExecuteInEditMode")) {
                instance.executeInEditMode = true;
            }
            migratedComponents.push_back(std::move(instance));
            ++report.instancesMigrated;
        }
        if (migratedComponents.empty()) {
            instances_.erase(instIt);
        } else {
            components = std::move(migratedComponents);
        }
    }

    // Reconcile in-flight `async Awaitable` CoroutineFrames, `AwaitableCorePool` payloads,
    // and scene async request results (`eff_1`):
    // - Preserve frames whose state-machine structural fingerprint matches in `newProgram`,
    //   patching migrated `ObjectRef`s, remapping `functionSymbolId` & `functionIndex`,
    //   refreshing GC roots, and rebinding resumption to `newProgram`.
    // - Safely abort frames whose function was removed or whose state-machine layout changed
    //   (deferring slot recycle via `invalidated = true` if currently active on the C++ stack).
    if (asyncCtx || !asyncRequests_.empty() || !pendingAsyncCompletions_.empty()) {
        std::unordered_map<std::string, std::uint64_t> oldCoroLayoutMemo;
        std::unordered_map<std::string, std::uint64_t> newCoroLayoutMemo;
        auto* heapForCheck = runtime_.heap().get();

        const auto valueHasIncompatibleUnmigratedLayout = [&](
            const auto& self,
            const runtime::Value& val,
            std::unordered_set<std::uint64_t>& visitedHeapObjs) -> bool {
            if (!oldProgramPtr || !newProgramPtr) return true;
            if (const auto* stVal = std::get_if<runtime::StructValue>(&val)) {
                if (!stVal->valid()) return false;
                if (stVal->typeId == runtime::awaitableVoidTypeId() ||
                    stVal->typeId == runtime::whenAnyResultTypeId() ||
                    stVal->typeId == runtime::fixedTypeId() ||
                    stVal->typeId == runtime::fixed16TypeId() ||
                    stVal->typeId == runtime::vec2TypeId() ||
                    stVal->typeId == runtime::vec3TypeId() ||
                    stVal->typeId == runtime::quatTypeId() ||
                    stVal->typeId == runtime::fixed64TypeId() ||
                    stVal->typeId == runtime::vec2fTypeId() ||
                    stVal->typeId == runtime::vec3fTypeId() ||
                    stVal->typeId == runtime::quatfTypeId()) {
                    return false;
                }
                const auto* oTd = oldProgramPtr->resolveType(stVal->typeId);
                if (!oTd) return false;
                const std::string canon = semantic::canonicalTypeName(*oTd);
                if (canon.find("::Awaitable__") != std::string::npos) return false;
                const auto* nTd = resolveProgramTypeByNameOrId(*newProgramPtr, 0, canon);
                if (!nTd || nTd->kind != semantic::TypeKind::Struct) return true;
                std::unordered_set<std::string> s1, s2;
                if (computeRecursiveRecordOrStructLayoutHash(
                        *oldProgramPtr, *oTd, s1, oldCoroLayoutMemo) !=
                    computeRecursiveRecordOrStructLayoutHash(
                        *newProgramPtr, *nTd, s2, newCoroLayoutMemo)) {
                    return true;
                }
                const std::size_t fc = stVal->fieldCount();
                for (std::size_t fi = 0; fi < fc; ++fi) {
                    if (const auto fv = runtime::structValueFieldAt(*stVal, fi)) {
                        if (self(self, *fv, visitedHeapObjs)) return true;
                    }
                }
                return false;
            }
            const auto* objRef = std::get_if<runtime::ObjectRef>(&val);
            if (!objRef || !objRef->valid() || !heapForCheck ||
                objRef->kind == runtime::ObjectKind::String) {
                return false;
            }
            const std::uint64_t rawKey =
                (static_cast<std::uint64_t>(objRef->generation) << 32u) | objRef->slot;
            if (failedComponentOldRefs.find(rawKey) != failedComponentOldRefs.end()) {
                return true;
            }
            if (!visitedHeapObjs.insert(rawKey).second) {
                return false;
            }
            // If this ObjectRef was migrated by SceneScriptRuntime (component or asset),
            // traverse its newly-migrated target fields instead of rejecting it!
            const auto migIt = migratedRefMap.find(rawKey);
            if (migIt != migratedRefMap.end()) {
                const runtime::ObjectRef newRef = migIt->second;
                const auto fc = heapForCheck->fieldCount(newRef).value_or(0);
                for (std::size_t fi = 0; fi < fc; ++fi) {
                    if (const auto fv = heapForCheck->fieldGet(newRef, fi)) {
                        if (self(self, *fv, visitedHeapObjs)) return true;
                    }
                }
                return false;
            }
            if (objRef->kind == runtime::ObjectKind::Array) {
                const auto len = heapForCheck->arrayLength(*objRef).value_or(0);
                for (std::size_t ai = 0; ai < len; ++ai) {
                    if (const auto ev = heapForCheck->arrayGet(*objRef, ai)) {
                        if (self(self, *ev, visitedHeapObjs)) return true;
                    }
                }
                return false;
            }
            // Non-component heap Record object: if its class layout changed (including nested
            // class/struct fields), the old heap slot still has old field offsets -> MUST ABORT!
            const auto oTid = heapForCheck->objectTypeId(*objRef);
            const auto* oTd = oTid ? oldProgramPtr->resolveType(*oTid) : nullptr;
            if (!oTd) return false;
            const auto* nTd = resolveProgramTypeByNameOrId(
                *newProgramPtr, 0, semantic::canonicalTypeName(*oTd));
            if (!nTd || nTd->kind != semantic::TypeKind::Class) {
                return true;
            }
            std::unordered_set<std::string> s1, s2;
            if (computeRecursiveRecordOrStructLayoutHash(
                    *oldProgramPtr, *oTd, s1, oldCoroLayoutMemo) !=
                computeRecursiveRecordOrStructLayoutHash(
                    *newProgramPtr, *nTd, s2, newCoroLayoutMemo)) {
                return true;
            }
            const auto fc = heapForCheck->fieldCount(*objRef).value_or(0);
            for (std::size_t fi = 0; fi < fc; ++fi) {
                if (const auto fv = heapForCheck->fieldGet(*objRef, fi)) {
                    if (self(self, *fv, visitedHeapObjs)) return true;
                }
            }
            return false;
        };

        std::unordered_set<std::uint64_t> sharedAsyncPatchVisited;
        const auto patchOrValidateAsyncValue = [&](runtime::Value& val) -> bool {
            std::unordered_set<std::uint64_t> visitedCheck;
            if (valueHasIncompatibleUnmigratedLayout(
                    valueHasIncompatibleUnmigratedLayout, val, visitedCheck)) {
                return false;
            }
            if (auto* stVal = std::get_if<runtime::StructValue>(&val)) {
                if (stVal->valid() && oldProgramPtr && newProgramPtr) {
                    if (const auto* oTd = oldProgramPtr->resolveType(stVal->typeId)) {
                        if (const auto* nTd = resolveProgramTypeByNameOrId(
                                *newProgramPtr, 0, semantic::canonicalTypeName(*oTd))) {
                            stVal->typeId = nTd->id;
                        }
                    }
                }
            }
            if (!migratedRefMap.empty()) {
                (void)patchMigratedObjectRefsInValue(
                    heapForCheck, val, migratedRefMap, sharedAsyncPatchVisited);
            }
            return true;
        };
        const auto patchOrValidateAsyncObjectRef = [&](runtime::ObjectRef& ref) -> bool {
            runtime::Value tmp{ref};
            if (!patchOrValidateAsyncValue(tmp)) {
                return false;
            }
            if (const auto* outRef = std::get_if<runtime::ObjectRef>(&tmp)) {
                ref = *outRef;
            }
            return true;
        };

        if (asyncCtx && !activeCoroFrames.empty()) {
            std::vector<ActiveCoroCandidate> toPreserve;
            std::vector<ActiveCoroCandidate> toAbort;
            toPreserve.reserve(activeCoroFrames.size());
            toAbort.reserve(activeCoroFrames.size());

            for (const auto& cand : activeCoroFrames) {
                bool compatible = false;
                if (cand.frame != nullptr &&
                    cand.frame->suspended &&
                    !cand.frame->completed &&
                    !cand.frame->invalidated &&
                    !cand.frame->usesScalarBuffer &&
                    cand.oldFp != nullptr &&
                    cand.newFp != nullptr &&
                    cand.oldFp->matchesCoreStateMachine(*cand.newFp)) {
                    bool resumeBlockFound = false;
                    for (const auto& sp : cand.newFp->suspensionPoints) {
                        if (sp.resumeBlockId == cand.frame->resumeBlockId) {
                            resumeBlockFound = true;
                            break;
                        }
                    }
                    bool heapObjectsSafe = true;
                    std::unordered_set<std::uint64_t> visitedHeapObjs;
                    for (const auto& argVal : cand.frame->savedArguments) {
                        if (valueHasIncompatibleUnmigratedLayout(
                                valueHasIncompatibleUnmigratedLayout, argVal, visitedHeapObjs)) {
                            heapObjectsSafe = false;
                            break;
                        }
                    }
                    for (std::size_t li = 0;
                         heapObjectsSafe && li < cand.frame->savedLocals.size();
                         ++li) {
                        if (valueHasIncompatibleUnmigratedLayout(
                                valueHasIncompatibleUnmigratedLayout,
                                cand.frame->savedLocals[li],
                                visitedHeapObjs)) {
                            heapObjectsSafe = false;
                        }
                    }
                    for (std::size_t ri = 0;
                         heapObjectsSafe && ri < cand.frame->savedRegisters.size();
                         ++ri) {
                        if (valueHasIncompatibleUnmigratedLayout(
                                valueHasIncompatibleUnmigratedLayout,
                                cand.frame->savedRegisters[ri],
                                visitedHeapObjs)) {
                            heapObjectsSafe = false;
                        }
                    }
                    if (heapObjectsSafe && heapForCheck &&
                        !cand.oldFp->matchesLayout(*cand.newFp)) {
                        // Check static fields on heap in case a non-component heap object with
                        // mutated layout is held in a static field referenced after resume.
                        for (const auto sfId : heapForCheck->staticFieldIds()) {
                            if (const auto sfVal = heapForCheck->getStaticField(sfId)) {
                                if (valueHasIncompatibleUnmigratedLayout(
                                        valueHasIncompatibleUnmigratedLayout, *sfVal, visitedHeapObjs)) {
                                    heapObjectsSafe = false;
                                    break;
                                }
                            }
                        }
                    }
                    compatible = resumeBlockFound && heapObjectsSafe;
                }

                if (compatible) {
                    toPreserve.push_back(cand);
                } else {
                    toAbort.push_back(cand);
                }
            }

            if (toPreserve.empty()) {
                const std::size_t abortedNow = asyncCtx->coroutines().abortAllSuspendedFrames();
                report.abortedCoroutines += abortedNow;
            } else {
                // 1. Abort only incompatible frames
                for (const auto& bad : toAbort) {
                    auto* curFrame = asyncCtx->coroutines().resolve(bad.handle);
                    if (!curFrame || curFrame->invalidated) continue;
                    const runtime::AwaitableHandle completion = curFrame->completionHandle;
                    const runtime::AwaitableHandle awaited = curFrame->awaitedHandle;
                    if (!curFrame->suspended && !curFrame->completed) {
                        curFrame->invalidated = true;
                        curFrame->awaitedHandle = runtime::AwaitableHandle::invalid();
                    } else {
                        asyncCtx->coroutines().recycleFrame(bad.handle);
                    }
                    ++report.abortedCoroutines;

                    if (awaited.valid() && !awaited.isCompletedVoidSentinel()) {
                        (void)asyncCtx->awaitables().discard(awaited);
                    }
                    if (completion.valid() && !completion.isCompletedVoidSentinel()) {
                        (void)asyncCtx->awaitables().trySetCanceled(completion);
                    }
                }

                // 2. Remap & rebind preserved frames
                auto* heap = runtime_.heap().get();
                std::unordered_set<std::uint64_t> coroPatchVisited;
                for (const auto& good : toPreserve) {
                    auto* curFrame = asyncCtx->coroutines().resolve(good.handle);
                    if (!curFrame) continue;

                    curFrame->functionSymbolId = good.newFp->symbolId;
                    curFrame->returnStructTypeId = good.newFp->returnTypeId;
                    report.remappedCoroutineFunctionIndices[good.oldFp->functionIndex] =
                        good.newFp->functionIndex;

                    if (!migratedRefMap.empty()) {
                        for (auto& argVal : curFrame->savedArguments) {
                            (void)patchMigratedObjectRefsInValue(
                                heap, argVal, migratedRefMap, coroPatchVisited);
                        }
                        for (auto& locVal : curFrame->savedLocals) {
                            (void)patchMigratedObjectRefsInValue(
                                heap, locVal, migratedRefMap, coroPatchVisited);
                        }
                        for (auto& regVal : curFrame->savedRegisters) {
                            (void)patchMigratedObjectRefsInValue(
                                heap, regVal, migratedRefMap, coroPatchVisited);
                        }
                    }
                    curFrame->savedRegisters.resize(good.newFp->registerSlotCount, runtime::Value{});

                    auto bridge = createHotReloadCoroBridge(
                        newProgram,
                        runtime_.bindings_,
                        runtime_.debugger_,
                        runtime_.heap(),
                        asyncCtx,
                        executionOptions_,
                        curFrame->resumeCallback,
                        curFrame->runnerContext,
                        curFrame->runnerOwner);
                    if (bridge) {
                        curFrame->resumeCallback = &HotReloadCoroBridge::onResume;
                        curFrame->runnerContext = bridge.get();
                        curFrame->runnerOwner = std::move(bridge);
                    }

                    ++report.preservedCoroutines;
                }

                // 3. Refresh CoroutineFramePool GC roots so migrated V2 ObjectRefs are pinned
                //    and superseded V1 ObjectRefs are unpinned cleanly.
                asyncCtx->coroutines().setHeap(nullptr);
                asyncCtx->coroutines().setHeap(heap);
                asyncCtx->coroutines().notePoolRootsPotentiallyModified();
            }
        }

        if (asyncCtx) {
            asyncCtx->awaitables().reconcileValuesOnHotReload(
                patchOrValidateAsyncValue,
                patchOrValidateAsyncObjectRef);
        }
        for (auto& [reqId, entry] : asyncRequests_) {
            (void)reqId;
            if (!patchOrValidateAsyncValue(entry.result)) {
                entry.result = runtime::Value{};
            }
        }
        for (auto& item : pendingAsyncCompletions_) {
            if (!patchOrValidateAsyncValue(item.result)) {
                item.result = runtime::Value{};
            }
        }
    }

    // Migrate [FieldTag(N)] and [FormerlySerializedAs] static fields on reloaded types
    if (auto* heap = runtime_.heap().get()) {
        if (const auto prog = runtime_.program()) {
            std::unordered_set<semantic::SymbolId> claimedOldStaticIds;
            for (const auto& module : prog->modules()) {
                for (const auto& typeDesc : module.types) {
                    const std::string canonicalType = semantic::canonicalTypeName(typeDesc);
                    std::string oldCanonicalType = canonicalType;
                    if (const auto oldIt = oldTypeByNewCanonical.find(canonicalType);
                        oldIt != oldTypeByNewCanonical.end()) {
                        oldCanonicalType = oldIt->second;
                    }

                    std::unordered_set<semantic::SymbolId> declaredStaticIds;
                    for (const auto& f : typeDesc.fields) {
                        if (f.staticField && !f.constField) {
                            declaredStaticIds.insert(f.id);
                        }
                    }

                    // Pass 1: [FieldTag(N)] static fields (batch before writing so swapped tags don't collide).
                    struct PendingStaticTagWrite {
                        semantic::SymbolId targetId = 0;
                        semantic::SymbolId oldId = 0;
                        runtime::Value value;
                        bool renamed = false;
                    };
                    std::vector<PendingStaticTagWrite> pendingTagWrites;
                    for (const auto& newField : typeDesc.fields) {
                        if (!newField.staticField || newField.constField) continue;
                        auto newTag = findFieldTagAttribute(
                            prog.get(), &mergedMetadata, canonicalType, newField.name, &newField);
                        if (!newTag && oldCanonicalType != canonicalType) {
                            newTag = findFieldTagAttribute(
                                prog.get(), &mergedMetadata, oldCanonicalType, newField.name, &newField);
                        }
                        if (!newTag) continue;

                        auto oldTagIt = oldStaticTaggedFields.find({oldCanonicalType, *newTag});
                        if (oldTagIt == oldStaticTaggedFields.end() && oldCanonicalType != canonicalType) {
                            oldTagIt = oldStaticTaggedFields.find({canonicalType, *newTag});
                        }
                        if (oldTagIt == oldStaticTaggedFields.end()) continue;
                        if (claimedOldStaticIds.find(oldTagIt->second.staticId) !=
                            claimedOldStaticIds.end()) {
                            continue;
                        }
                        const auto newFieldTypeId = exactFieldTypeId(
                            newField.type, newField.typeName);
                        if (runtime_.valueMatches(
                                oldTagIt->second.value, newField.type, newFieldTypeId)) {
                            claimedOldStaticIds.insert(oldTagIt->second.staticId);
                            pendingTagWrites.push_back(PendingStaticTagWrite{
                                newField.id,
                                oldTagIt->second.staticId,
                                oldTagIt->second.value,
                                oldTagIt->second.oldFieldName != newField.name,
                            });
                        }
                    }
                    for (const auto& writeOp : pendingTagWrites) {
                        heap->setStaticField(writeOp.targetId, writeOp.value);
                        if (writeOp.oldId != writeOp.targetId &&
                            declaredStaticIds.find(writeOp.oldId) == declaredStaticIds.end()) {
                            (void)heap->removeStaticField(writeOp.oldId);
                        }
                        ++report.fieldsMigrated;
                        if (writeOp.renamed) {
                            ++report.fieldsRenamed;
                        }
                    }

                    // Pass 2: Ordered multi-alias [FormerlySerializedAs] static fields.
                    for (const auto& newField : typeDesc.fields) {
                        if (!newField.staticField || newField.constField) continue;
                        if (heap->hasStaticField(newField.id)) continue;
                        std::vector<std::string> aliases = findRenamedFieldSources(
                            mergedMetadata, canonicalType, newField.name, &newField, prog.get());
                        if (oldCanonicalType != canonicalType) {
                            for (auto& extra : findRenamedFieldSources(
                                     mergedMetadata, oldCanonicalType, newField.name, &newField, prog.get())) {
                                if (std::find(aliases.begin(), aliases.end(), extra) == aliases.end()) {
                                    aliases.push_back(std::move(extra));
                                }
                            }
                        }
                        const auto newFieldTypeId = exactFieldTypeId(
                            newField.type, newField.typeName);
                        for (const auto& oldFieldName : aliases) {
                            const auto oldStaticId = semantic::stableTypeId(
                                oldCanonicalType + "::field:" + oldFieldName);
                            if (claimedOldStaticIds.find(oldStaticId) != claimedOldStaticIds.end()) {
                                continue;
                            }
                            const auto oldVal = heap->getStaticField(oldStaticId);
                            if (oldVal &&
                                runtime_.valueMatches(*oldVal, newField.type, newFieldTypeId)) {
                                claimedOldStaticIds.insert(oldStaticId);
                                heap->setStaticField(newField.id, *oldVal);
                                if (declaredStaticIds.find(oldStaticId) == declaredStaticIds.end()) {
                                    (void)heap->removeStaticField(oldStaticId);
                                }
                                ++report.fieldsMigrated;
                                ++report.fieldsRenamed;
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    // Patch cross-component ObjectRef fields (including inside Arrays, Structs, Assets, Static Fields, and Native ECS Tables)
    if (!migratedRefMap.empty()) {
        std::unordered_set<std::uint64_t> visited;
        auto* heap = runtime_.heap().get();
        for (const auto& [entity, savedList] : savedEntities) {
            (void)savedList;
            auto instIt = instances_.find(entity);
            if (instIt == instances_.end()) continue;
            for (auto& instance : instIt->second) {
                patchMigratedObjectRefsInHeapObject(
                    heap,
                    instance.object.reference(),
                    migratedRefMap,
                    visited);
            }
        }
        for (auto& [assetPath, assetObj] : runtime_.assets_) {
            (void)assetPath;
            patchMigratedObjectRefsInHeapObject(
                heap,
                assetObj.reference(),
                migratedRefMap,
                visited);
        }
        if (heap) {
            for (const auto staticId : heap->staticFieldIds()) {
                auto staticVal = heap->getStaticField(staticId);
                if (!staticVal) continue;
                if (patchMigratedObjectRefsInValue(heap, *staticVal, migratedRefMap, visited)) {
                    heap->setStaticField(staticId, std::move(*staticVal));
                }
            }
            for (const auto& tableName : heap->nativeEcsTableNames()) {
                const auto* vals = heap->nativeEcsComponentValues(tableName);
                if (!vals) continue;
                const std::size_t count = vals->size();
                for (std::size_t denseIdx = 0; denseIdx < count; ++denseIdx) {
                    runtime::Value compVal = (*vals)[denseIdx];
                    if (patchMigratedObjectRefsInValue(heap, compVal, migratedRefMap, visited)) {
                        (void)heap->setNativeEcsComponentByDenseIndex(
                            tableName, denseIdx, std::move(compVal));
                    }
                }
            }
        }
    }

    // Rebind wired signal connections to updated method SymbolIds and migrated ObjectRefs
    if (!signalConnections_.empty()) {
        std::set<std::tuple<SceneEntityId, std::string, std::string>> uniqueSources;
        for (const auto& conn : signalConnections_) {
            uniqueSources.emplace(conn.sourceEntity, conn.sourceComponent, conn.signalName);
        }
        for (const auto& [srcEnt, srcComp, sigName] : uniqueSources) {
            if (rebuildSignalDelegateChain(srcEnt, srcComp, sigName)) {
                ++report.signalsRebound;
            }
        }
    }

    report.succeeded = report.errors.empty();
    return report;
}

bool SceneScriptRuntime::validate(SceneEntityId entity) {
    const auto found = instances_.find(entity);
    if (found == instances_.end() || found->second.empty()) return false;
    bool allOk = true;
    for (auto& instance : found->second) {
        if (!invokeLifecycle(entity, instance, instance.onValidate, {}, "OnValidate")) {
            allOk = false;
        }
    }
    return allOk;
}

bool SceneScriptRuntime::validateComponent(
    SceneEntityId entity,
    const std::string& scriptType) {
    const auto found = instances_.find(entity);
    if (found == instances_.end()) return false;
    for (auto& instance : found->second) {
        if (!matchesComponentType(instance.object.type(), scriptType)) continue;
        return invokeLifecycle(entity, instance, instance.onValidate, {}, "OnValidate");
    }
    return false;
}

bool SceneScriptRuntime::setInspectorMember(
    SceneEntityId entity,
    const std::string& scriptType,
    const std::string& memberName,
    runtime::Value value) {
    const auto found = instances_.find(entity);
    if (found == instances_.end()) return false;
    runtime::RuntimeError lastError;
    for (auto& instance : found->second) {
        if (!scriptType.empty() &&
            !matchesComponentType(instance.object.type(), scriptType)) {
            continue;
        }
        runtime::RuntimeError error;
        if (!runtime_.setMember(
                instance.object,
                memberName,
                value,
                error,
                executionOptions_)) {
            if (scriptType.empty()) {
                lastError = std::move(error);
                continue;
            }
            recordError(entity, "SetInspectorMember:" + memberName, error);
            return false;
        }
        if (instance.onValidate) {
            if (!invokeLifecycle(
                    entity, instance, instance.onValidate, {}, "OnValidate")) {
                return false;
            }
        }
        return true;
    }
    if (scriptType.empty() && !found->second.empty() &&
        lastError.code != runtime::ErrorCode::None) {
        recordError(entity, "SetInspectorMember:" + memberName, lastError);
    }
    return false;
}

bool SceneScriptRuntime::rebuildSignalDelegateChain(
    SceneEntityId sourceEntity,
    const std::string& sourceComponent,
    const std::string& signalName) {
    ScriptObject* srcObj = sourceComponent.empty()
        ? object(sourceEntity)
        : getComponent(sourceEntity, sourceComponent);
    if (!srcObj || signalName.empty()) return false;

    const std::string backingFieldName = "$event_" + signalName + "_delegate";
    const semantic::FieldSymbol* eventField = nullptr;
    for (const auto& field : srcObj->type().descriptor.fields) {
        if (field.name == backingFieldName &&
            field.type == semantic::PrimitiveType::Object) {
            eventField = &field;
            break;
        }
    }
    if (!eventField) return false;

    const auto delegateType = runtime_.findType(eventField->typeName);
    if (!delegateType || !delegateType->descriptor.delegateType) return false;

    const semantic::FunctionSymbol* invokeSig = nullptr;
    for (const auto& method : delegateType->descriptor.methods) {
        if (method.name == "Invoke") {
            invokeSig = &method;
            break;
        }
    }
    if (!invokeSig) return false;
    const std::size_t arity = invokeSig->parameters.size() > 0
        ? invokeSig->parameters.size() - 1u
        : 0u;

    runtime::RuntimeError error;
    std::optional<runtime::ObjectRef> chainHead;
    runtime::PersistentRoot chainHeadRoot;

    for (const auto& conn : signalConnections_) {
        if (conn.sourceEntity != sourceEntity || conn.signalName != signalName) {
            continue;
        }
        if (!sourceComponent.empty() && !conn.sourceComponent.empty() &&
            !matchesComponentType(srcObj->type(), conn.sourceComponent)) {
            continue;
        }
        ScriptObject* dstObj = conn.targetComponent.empty()
            ? object(conn.targetEntity)
            : getComponent(conn.targetEntity, conn.targetComponent);
        if (!dstObj) continue;

        const auto dstMethod = runtime_.findMethod(dstObj->type(), conn.targetMethod, arity);
        if (!dstMethod || !dstMethod->instance()) continue;

        const auto symbolId = dstMethod->descriptor.id;
        const auto lowWord = static_cast<std::int64_t>(symbolId & 0xffffffffULL);
        const auto highWord = static_cast<std::int64_t>((symbolId >> 32u) & 0xffffffffULL);
        std::vector<runtime::Value> leafFields{
            dstObj->value(),
            lowWord,
            highWord,
            std::int64_t{0},
            std::int64_t{0},
            std::int64_t{0},
            std::int64_t{0},
            runtime::NullObject{},
        };
        const auto leafRef = runtime_.heap()->allocateObject(
            delegateType->descriptor.id, std::move(leafFields), {0u, 7u}, &error);
        if (!leafRef) return false;
        auto leafRoot = runtime_.heap()->retain(*leafRef);

        if (!chainHead) {
            chainHead = *leafRef;
            chainHeadRoot = std::move(leafRoot);
        } else {
            std::vector<runtime::Value> combinedFields{
                *chainHead,
                std::int64_t{0},
                std::int64_t{0},
                std::int64_t{4},
                std::int64_t{0},
                std::int64_t{0},
                std::int64_t{0},
                *leafRef,
            };
            const auto combinedRef = runtime_.heap()->allocateObject(
                delegateType->descriptor.id, std::move(combinedFields), {0u, 7u}, &error);
            if (!combinedRef) return false;
            chainHead = *combinedRef;
            chainHeadRoot = runtime_.heap()->retain(*combinedRef);
        }
    }

    if (!chainHead) {
        return runtime_.heap()->fieldSet(
            srcObj->reference(), eventField->index, runtime::NullObject{}, &error);
    }
    return runtime_.heap()->fieldSet(
        srcObj->reference(), eventField->index, *chainHead, &error);
}

void SceneScriptRuntime::removeSignalsForEntityOrComponent(
    SceneEntityId entity,
    const std::string& removedComponentType) {
    if (signalConnections_.empty()) return;

    const auto matchesRemoved = [&](SceneEntityId ent, const std::string& compName) {
        if (ent != entity) return false;
        if (removedComponentType.empty()) return true;
        if (compName.empty()) return false;
        if (compName == removedComponentType) return true;
        if (const auto* compObj = getComponent(entity, compName)) {
            return compObj->type().canonicalName() == removedComponentType;
        }
        return false;
    };

    std::set<std::tuple<SceneEntityId, std::string, std::string>> affectedSources;
    for (auto it = signalConnections_.begin(); it != signalConnections_.end();) {
        const bool srcRemoved = matchesRemoved(it->sourceEntity, it->sourceComponent);
        const bool dstRemoved = matchesRemoved(it->targetEntity, it->targetComponent);
        if (srcRemoved || dstRemoved) {
            if (!srcRemoved) {
                affectedSources.emplace(
                    it->sourceEntity, it->sourceComponent, it->signalName);
            }
            it = signalConnections_.erase(it);
        } else {
            ++it;
        }
    }

    for (const auto& [srcEnt, srcComp, sigName] : affectedSources) {
        (void)rebuildSignalDelegateChain(srcEnt, srcComp, sigName);
    }
}

bool SceneScriptRuntime::connectSignal(
    SceneEntityId sourceEntity,
    const std::string& sourceComponent,
    const std::string& signalName,
    SceneEntityId targetEntity,
    const std::string& targetComponent,
    const std::string& targetMethod) {
    ScriptObject* srcObj = sourceComponent.empty()
        ? object(sourceEntity)
        : getComponent(sourceEntity, sourceComponent);
    ScriptObject* dstObj = targetComponent.empty()
        ? object(targetEntity)
        : getComponent(targetEntity, targetComponent);
    if (!srcObj || !dstObj || signalName.empty() || targetMethod.empty()) {
        return false;
    }

    const std::string backingFieldName = "$event_" + signalName + "_delegate";
    const semantic::FieldSymbol* eventField = nullptr;
    for (const auto& field : srcObj->type().descriptor.fields) {
        if (field.name == backingFieldName &&
            field.type == semantic::PrimitiveType::Object) {
            eventField = &field;
            break;
        }
    }
    if (!eventField) return false;

    const auto delegateType = runtime_.findType(eventField->typeName);
    if (!delegateType || !delegateType->descriptor.delegateType) return false;

    const semantic::FunctionSymbol* invokeSig = nullptr;
    for (const auto& method : delegateType->descriptor.methods) {
        if (method.name == "Invoke") {
            invokeSig = &method;
            break;
        }
    }
    if (!invokeSig) return false;
    const std::size_t arity = invokeSig->parameters.size() > 0
        ? invokeSig->parameters.size() - 1u
        : 0u;

    const auto dstMethod = runtime_.findMethod(dstObj->type(), targetMethod, arity);
    if (!dstMethod || !dstMethod->instance()) return false;

    const std::string resolvedSrcComp = srcObj->type().canonicalName();
    const std::string resolvedDstComp = dstObj->type().canonicalName();

    for (const auto& existing : signalConnections_) {
        if (existing.sourceEntity == sourceEntity &&
            existing.sourceComponent == resolvedSrcComp &&
            existing.signalName == signalName &&
            existing.targetEntity == targetEntity &&
            existing.targetComponent == resolvedDstComp &&
            existing.targetMethod == targetMethod) {
            return true;
        }
    }

    signalConnections_.push_back(SignalConnection{
        sourceEntity,
        resolvedSrcComp,
        signalName,
        targetEntity,
        resolvedDstComp,
        targetMethod,
    });
    return rebuildSignalDelegateChain(sourceEntity, resolvedSrcComp, signalName);
}

bool SceneScriptRuntime::disconnectSignal(
    SceneEntityId sourceEntity,
    const std::string& sourceComponent,
    const std::string& signalName,
    SceneEntityId targetEntity,
    const std::string& targetComponent,
    const std::string& targetMethod) {
    ScriptObject* srcObj = sourceComponent.empty()
        ? object(sourceEntity)
        : getComponent(sourceEntity, sourceComponent);
    ScriptObject* dstObj = targetComponent.empty()
        ? object(targetEntity)
        : getComponent(targetEntity, targetComponent);
    const std::string resolvedSrcComp = srcObj ? srcObj->type().canonicalName() : sourceComponent;
    const std::string resolvedDstComp = dstObj ? dstObj->type().canonicalName() : targetComponent;

    bool removedAny = false;
    for (auto it = signalConnections_.begin(); it != signalConnections_.end();) {
        const bool srcMatch = it->sourceEntity == sourceEntity &&
            (sourceComponent.empty() || it->sourceComponent == resolvedSrcComp ||
             it->sourceComponent == sourceComponent);
        const bool sigMatch = it->signalName == signalName;
        const bool dstMatch = it->targetEntity == targetEntity &&
            (targetComponent.empty() || it->targetComponent == resolvedDstComp ||
             it->targetComponent == targetComponent) &&
            (targetMethod.empty() || it->targetMethod == targetMethod);
        if (srcMatch && sigMatch && dstMatch) {
            it = signalConnections_.erase(it);
            removedAny = true;
        } else {
            ++it;
        }
    }
    if (!removedAny) return false;
    return rebuildSignalDelegateChain(sourceEntity, resolvedSrcComp, signalName);
}

std::optional<SceneScriptRuntime::ComponentLocation>
SceneScriptRuntime::findObjectLocation(runtime::ObjectRef reference) const {
    if (!reference.valid()) return std::nullopt;
    for (const auto& [entity, components] : instances_) {
        for (const auto& instance : components) {
            if (instance.object.reference() == reference) {
                return ComponentLocation{
                    entity,
                    instance.object.type().canonicalName(),
                };
            }
        }
    }
    return std::nullopt;
}

bool SceneScriptRuntime::setParent(SceneEntityId child, SceneEntityId parent) {
    if (child == BroadcastTarget || child == parent ||
        isStaleGenerationalEntity(child) ||
        (parent != BroadcastTarget && isStaleGenerationalEntity(parent))) {
        return false;
    }
    // Prevent cycle in hierarchy
    SceneEntityId cursor = parent;
    while (cursor != BroadcastTarget) {
        if (cursor == child) return false;
        const auto pIt = parentByEntity_.find(cursor);
        cursor = (pIt == parentByEntity_.end()) ? BroadcastTarget : pIt->second;
    }

    if (entityActiveSelf_.find(child) == entityActiveSelf_.end()) {
        entityActiveSelf_[child] = true;
    }
    if (parent != BroadcastTarget &&
        entityActiveSelf_.find(parent) == entityActiveSelf_.end()) {
        entityActiveSelf_[parent] = true;
    }

    const bool wasActiveInHierarchy = isActiveInHierarchy(child);

    const auto oldParentIt = parentByEntity_.find(child);
    if (oldParentIt != parentByEntity_.end()) {
        const auto oldParent = oldParentIt->second;
        if (oldParent == parent) return true;
        const auto chIt = childrenByEntity_.find(oldParent);
        if (chIt != childrenByEntity_.end()) {
            auto& vec = chIt->second;
            vec.erase(std::remove(vec.begin(), vec.end(), child), vec.end());
        }
        parentByEntity_.erase(oldParentIt);
    }

    if (parent != BroadcastTarget) {
        parentByEntity_[child] = parent;
        auto& children = childrenByEntity_[parent];
        if (std::find(children.begin(), children.end(), child) == children.end()) {
            children.push_back(child);
        }
    }
    invalidateTransformSubtree(child);

    const bool nowActiveInHierarchy = isActiveInHierarchy(child);
    if (wasActiveInHierarchy != nowActiveInHierarchy) {
        if (nowActiveInHierarchy) {
            hasUnstartedPlayComponents_ = true;
        }
        markLifecycleCacheDirty();
        const auto notifyNodeComponents = [&](SceneEntityId nodeId, bool becomingActive) {
            if (!becomingActive && fiberScheduler_) {
                const std::uint32_t nodeGen = entityIdGeneration(nodeId);
                const std::uint32_t nodeSlot = entityIdSlot(nodeId);
                if (nodeGen != 0) {
                    (void)fiberScheduler_->killEntityFibers(
                        runtime::GenerationalEntityHandle{nodeSlot, nodeGen});
                } else {
                    (void)fiberScheduler_->killEntityFibersBySlot(nodeSlot);
                }
            }
            const auto instIt = instances_.find(nodeId);
            if (instIt == instances_.end()) return;
            for (auto& comp : instIt->second) {
                if (!comp.enabled) continue;
                if (!becomingActive) {
                    cancelInstanceSequencesFast(comp);
                }
                const auto& cb = becomingActive ? comp.onEnable : comp.onDisable;
                (void)invokeLifecycle(
                    nodeId,
                    comp,
                    cb,
                    {},
                    becomingActive ? "OnEnable" : "OnDisable");
            }
        };
        const auto notifySubtree = [&](const auto& self, SceneEntityId nodeId) -> void {
            if (!isEntityActiveSelf(nodeId)) return;
            notifyNodeComponents(nodeId, nowActiveInHierarchy);
            const auto chIt = childrenByEntity_.find(nodeId);
            if (chIt == childrenByEntity_.end()) return;
            const auto childrenCopy = chIt->second;
            for (const auto subChild : childrenCopy) {
                self(self, subChild);
            }
        };
        notifySubtree(notifySubtree, child);
        markLifecycleCacheDirty();
    }
    return true;
}

SceneEntityId SceneScriptRuntime::getParent(SceneEntityId entity) const noexcept {
    const auto it = parentByEntity_.find(entity);
    return it == parentByEntity_.end() ? BroadcastTarget : it->second;
}

std::vector<SceneEntityId> SceneScriptRuntime::getChildren(SceneEntityId entity) const {
    const auto it = childrenByEntity_.find(entity);
    return it == childrenByEntity_.end() ? std::vector<SceneEntityId>{} : it->second;
}

bool SceneScriptRuntime::isActiveInHierarchy(SceneEntityId entity) const noexcept {
    if (entity == BroadcastTarget) return false;
    SceneEntityId cursor = entity;
    while (cursor != BroadcastTarget) {
        const auto activeIt = entityActiveSelf_.find(cursor);
        if (activeIt != entityActiveSelf_.end() && !activeIt->second) {
            return false;
        }
        const auto pIt = parentByEntity_.find(cursor);
        cursor = (pIt == parentByEntity_.end()) ? BroadcastTarget : pIt->second;
    }
    return true;
}

void SceneScriptRuntime::setLocalTransform(
    SceneEntityId entity,
    const Transform3D& transform) {
    if (entity == BroadcastTarget || isStaleGenerationalEntity(entity)) return;
    if (entityActiveSelf_.find(entity) == entityActiveSelf_.end()) {
        entityActiveSelf_[entity] = true;
    }
    localTransforms_[entity] = transform;
    invalidateTransformSubtree(entity);
}

Transform3D SceneScriptRuntime::getLocalTransform(
    SceneEntityId entity) const noexcept {
    const auto it = localTransforms_.find(entity);
    return it == localTransforms_.end() ? Transform3D::identity() : it->second;
}

Transform3D SceneScriptRuntime::getWorldTransform(
    SceneEntityId entity) const noexcept {
    if (entity == BroadcastTarget) return Transform3D::identity();
    const auto cachedIt = worldTransformCache_.find(entity);
    if (cachedIt != worldTransformCache_.end()) {
        return cachedIt->second;
    }
    const SceneEntityId parentId = getParent(entity);
    const Transform3D world = (parentId == BroadcastTarget)
        ? getLocalTransform(entity)
        : Transform3D::combine(getWorldTransform(parentId), getLocalTransform(entity));
    worldTransformCache_[entity] = world;
    return world;
}

void SceneScriptRuntime::setLocalPosition2D(SceneEntityId entity, Vec2 position) {
    if (entity == BroadcastTarget || isStaleGenerationalEntity(entity)) return;
    if (entityActiveSelf_.find(entity) == entityActiveSelf_.end()) {
        entityActiveSelf_[entity] = true;
    }
    Transform3D t = getLocalTransform(entity);
    t.position = Vec3{position.x, position.y, t.position.z};
    localTransforms_[entity] = t;
    invalidateTransformSubtree(entity);
}

Vec2 SceneScriptRuntime::getWorldPosition2D(SceneEntityId entity) const noexcept {
    const Transform3D w = getWorldTransform(entity);
    return Vec2{w.position.x, w.position.y};
}

ScriptObject* SceneScriptRuntime::getComponentInChildren(
    SceneEntityId entity,
    const std::string& typeOrInterfaceName) {
    if (auto* selfComp = getComponent(entity, typeOrInterfaceName)) {
        return selfComp;
    }
    const auto chIt = childrenByEntity_.find(entity);
    if (chIt == childrenByEntity_.end()) return nullptr;
    for (const auto child : chIt->second) {
        if (auto* found = getComponentInChildren(child, typeOrInterfaceName)) {
            return found;
        }
    }
    return nullptr;
}

ScriptObject* SceneScriptRuntime::getComponentInParent(
    SceneEntityId entity,
    const std::string& typeOrInterfaceName) {
    SceneEntityId cursor = entity;
    while (cursor != BroadcastTarget) {
        if (auto* comp = getComponent(cursor, typeOrInterfaceName)) {
            return comp;
        }
        const auto pIt = parentByEntity_.find(cursor);
        cursor = (pIt == parentByEntity_.end()) ? BroadcastTarget : pIt->second;
    }
    return nullptr;
}

SceneEntityId SceneScriptRuntime::instantiatePrefab(
    const PrefabNodeTemplate& prefab,
    SceneEntityId& nextEntityId,
    SceneEntityId parent,
    const std::map<std::string, runtime::Value>& rootMemberOverrides) {
    if (nextEntityId == 0) nextEntityId = 1;

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
    struct PendingActivation {
        int order = 0;
        SceneEntityId entity = 0;
        std::size_t compIndex = 0;
        runtime::Value owner;
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    std::vector<PendingActivation> activations;

    // Phase 1: Allocate all entities, transforms, hierarchy links, and component instances
    const SceneEntityId initialNextEntityId = nextEntityId;
    std::vector<SceneEntityId> allocatedEntities;
    const auto allocateTree = [&](
        const auto& self,
        const PrefabNodeTemplate& node,
        SceneEntityId parentId,
        bool isRoot) -> SceneEntityId {
        while (nextEntityId == BroadcastTarget ||
               entityIdGeneration(nextEntityId) != 0 ||
               slotGenerations_.find(static_cast<std::uint32_t>(nextEntityId)) !=
                   slotGenerations_.end() ||
               entityActiveSelf_.find(nextEntityId) != entityActiveSelf_.end() ||
               instances_.find(nextEntityId) != instances_.end() ||
               localTransforms_.find(nextEntityId) != localTransforms_.end() ||
               parentByEntity_.find(nextEntityId) != parentByEntity_.end() ||
               childrenByEntity_.find(nextEntityId) != childrenByEntity_.end() ||
               entityNames_.find(nextEntityId) != entityNames_.end() ||
               colliders2D_.find(nextEntityId) != colliders2D_.end() ||
               colliders3D_.find(nextEntityId) != colliders3D_.end()) {
            ++nextEntityId;
        }
        const SceneEntityId entityId = nextEntityId++;
        allocatedEntities.push_back(entityId);
        entityActiveSelf_[entityId] = true;
        localTransforms_[entityId] = node.localTransform;
        if (!node.name.empty()) {
            entityNames_[entityId] = node.name;
        }
        if (parentId != BroadcastTarget) {
            (void)setParent(entityId, parentId);
        }
        for (std::size_t i = 0; i < node.components.size(); ++i) {
            SceneScriptOptions opts = node.components[i].options;
            runtime::Value ownerCopy = opts.owner;
            std::size_t compIdx = 0;
            if (!attachComponentWithoutLifecycle(
                    entityId, node.components[i].scriptType, std::move(opts), compIdx)) {
                return BroadcastTarget;
            }
            const int order = instances_[entityId][compIdx].executionOrder;
            activations.push_back(PendingActivation{
                order, entityId, compIdx, std::move(ownerCopy)});
        }
        // Validate [RequireComponent] after all components on this entity have been allocated
        const auto instIt = instances_.find(entityId);
        if (instIt != instances_.end()) {
            for (const auto& comp : instIt->second) {
                for (const auto& reqType : findRequiredComponents(
                         runtime_.program().get(), comp.object.type().canonicalName())) {
                    if (!hasComponent(entityId, reqType)) {
                        runtime::RuntimeError error;
                        error.code = runtime::ErrorCode::InvalidArguments;
                        error.message = "missing required component '" + reqType +
                            "' for '" + comp.object.type().canonicalName() + "'";
                        recordError(entityId, "RequireComponent", error);
                        return BroadcastTarget;
                    }
                }
            }
        }
        if (isRoot && !rootMemberOverrides.empty()) {
            for (const auto& [k, v] : rootMemberOverrides) {
                bool applied = false;
                if (instIt != instances_.end()) {
                    for (auto& comp : instIt->second) {
                        runtime::RuntimeError err;
                        if (runtime_.setMember(comp.object, k, v, err, executionOptions_)) {
                            applied = true;
                            break;
                        }
                    }
                }
                if (!applied) {
                    runtime::RuntimeError err;
                    err.code = runtime::ErrorCode::InvalidArguments;
                    err.message = "override member '" + k + "' not found on root prefab entity";
                    recordError(entityId, "SetInspectorMember:" + k, err);
                    return BroadcastTarget;
                }
            }
        }
        for (const auto& childPrefab : node.children) {
            if (self(self, childPrefab, entityId, false) == BroadcastTarget) {
                return BroadcastTarget;
            }
        }
        return entityId;
    };

    const SceneEntityId rootEntityId = allocateTree(allocateTree, prefab, parent, true);
    if (rootEntityId == BroadcastTarget) {
        for (auto it = allocatedEntities.rbegin(); it != allocatedEntities.rend(); ++it) {
            const SceneEntityId eid = *it;
            const auto pIt = parentByEntity_.find(eid);
            if (pIt != parentByEntity_.end()) {
                const auto pId = pIt->second;
                parentByEntity_.erase(pIt);
                const auto chIt = childrenByEntity_.find(pId);
                if (chIt != childrenByEntity_.end()) {
                    auto& vec = chIt->second;
                    vec.erase(std::remove(vec.begin(), vec.end(), eid), vec.end());
                }
            }
            childrenByEntity_.erase(eid);
            entityActiveSelf_.erase(eid);
            localTransforms_.erase(eid);
            worldTransformCache_.erase(eid);
            entityNames_.erase(eid);
            instances_.erase(eid);
        }
        nextEntityId = initialNextEntityId;
        markLifecycleCacheDirty();
        return BroadcastTarget;
    }

    // Phase 2: Sort all newly instantiated components by (ExecutionOrder, EntityId, CompIndex)
    // and invoke OnCreate followed by OnEnable
    std::sort(
        activations.begin(),
        activations.end(),
        [](const PendingActivation& a, const PendingActivation& b) {
            if (a.order != b.order) return a.order < b.order;
            if (a.entity != b.entity) return a.entity < b.entity;
            return a.compIndex < b.compIndex;
        });

    for (auto& item : activations) {
        auto& stored = instances_[item.entity][item.compIndex];
        const bool hasOwner = !std::holds_alternative<std::monostate>(item.owner);
        if (hasOwner && stored.onCreate1) {
            if (!invokeLifecycle(
                    item.entity, stored, stored.onCreate1, {std::move(item.owner)}, "OnCreate")) {
                (void)detach(rootEntityId);
                return BroadcastTarget;
            }
        } else if (!invokeLifecycle(
                item.entity, stored, stored.onCreate0, {}, "OnCreate")) {
            (void)detach(rootEntityId);
            return BroadcastTarget;
        }
        if (stored.onValidate && !rootMemberOverrides.empty() && item.entity == rootEntityId) {
            if (!invokeLifecycle(
                    item.entity, stored, stored.onValidate, {}, "OnValidate")) {
                (void)detach(rootEntityId);
                return BroadcastTarget;
            }
        }
    }

    for (auto& item : activations) {
        auto& stored = instances_[item.entity][item.compIndex];
        if (stored.enabled && isActiveInHierarchy(item.entity)) {
            if (!invokeLifecycle(
                    item.entity, stored, stored.onEnable, {}, "OnEnable")) {
                (void)detach(rootEntityId);
                return BroadcastTarget;
            }
        }
    }

    for (const SceneEntityId eid : allocatedEntities) {
        if (entityIdGeneration(eid) == 0) {
            const auto slot = static_cast<std::uint32_t>(eid);
            if (nextGenerationalSlot_ <= slot) {
                nextGenerationalSlot_ = slot + 1u;
            }
            if (nextAutoEntityId_ <= eid) {
                nextAutoEntityId_ = eid + 1u;
            }
        }
    }

    markLifecycleCacheDirty();
    invalidateTransformSubtree(rootEntityId);
    return rootEntityId;
}

std::uint32_t SceneScriptRuntime::spawnEntityFiber(
    SceneEntityId entity,
    const runtime::CompiledFiberModule& module,
    const std::string& functionName,
    std::uint8_t ownerGroupId,
    std::uint32_t seed) {
    if (!fiberScheduler_ || entity == BroadcastTarget || !isActiveInHierarchy(entity) ||
        isStaleGenerationalEntity(entity)) {
        return 0;
    }
    const std::uint32_t slot = entityIdSlot(entity);
    const std::uint32_t gen = entityIdGeneration(entity);
    return fiberScheduler_->spawnFunction(
        module,
        functionName,
        runtime::GenerationalEntityHandle{slot, gen != 0 ? gen : 1u},
        ownerGroupId,
        0u,
        seed);
}

std::uint64_t SceneScriptRuntime::stepFibers(std::uint32_t currentTick) {
    (void)flushCompletedAsyncRequests();
    if (!fiberScheduler_) return 0;
    return fiberScheduler_->stepTick(currentTick);
}

SceneScriptRuntime::AsyncRequestId SceneScriptRuntime::createAsyncRequest(
    std::string debugTag) {
    const AsyncRequestId id = nextAsyncRequestId_++;
    AsyncRequestEntry entry;
    entry.id = id;
    entry.debugTag = std::move(debugTag);
    entry.completed = false;
    asyncRequests_.emplace(id, std::move(entry));
    return id;
}

bool SceneScriptRuntime::completeAsyncRequest(
    AsyncRequestId requestId,
    runtime::Value result) {
    const auto it = asyncRequests_.find(requestId);
    if (it == asyncRequests_.end() || it->second.completed) return false;
    for (const auto& pending : pendingAsyncCompletions_) {
        if (pending.id == requestId) return false;
    }
    pendingAsyncCompletions_.push_back(PendingAsyncCompletion{
        requestId,
        std::move(result),
    });
    return true;
}

bool SceneScriptRuntime::isAsyncRequestCompleted(
    AsyncRequestId requestId) const noexcept {
    const auto it = asyncRequests_.find(requestId);
    return it != asyncRequests_.end() && it->second.completed;
}

std::optional<runtime::Value> SceneScriptRuntime::getAsyncRequestResult(
    AsyncRequestId requestId) const {
    const auto it = asyncRequests_.find(requestId);
    if (it == asyncRequests_.end() || !it->second.completed) return std::nullopt;
    return it->second.result;
}

bool SceneScriptRuntime::waitAsyncRequest(
    AsyncRequestId requestId,
    SceneEntityId targetEntity,
    std::string callbackMethod,
    std::string componentType) {
    if (targetEntity == BroadcastTarget || callbackMethod.empty()) return false;
    const auto it = asyncRequests_.find(requestId);
    if (it == asyncRequests_.end()) return false;
    if (it->second.completed) {
        if (componentType.empty()) {
            (void)invoke(targetEntity, callbackMethod, {});
        } else {
            (void)invokeComponent(targetEntity, componentType, callbackMethod, {});
        }
        return true;
    }
    it->second.waiters.push_back(AsyncRequestWaiter{
        targetEntity,
        std::move(callbackMethod),
        std::move(componentType),
    });
    return true;
}

std::size_t SceneScriptRuntime::flushCompletedAsyncRequests() {
    if (pendingAsyncCompletions_.empty()) return 0;
    auto batch = std::move(pendingAsyncCompletions_);
    pendingAsyncCompletions_.clear();
    std::size_t completedCount = 0;
    for (auto& item : batch) {
        const auto it = asyncRequests_.find(item.id);
        if (it == asyncRequests_.end() || it->second.completed) continue;
        it->second.completed = true;
        it->second.result = std::move(item.result);
        ++completedCount;
        auto waiters = std::move(it->second.waiters);
        it->second.waiters.clear();
        for (const auto& waiter : waiters) {
            if (!isEntityAlive(waiter.targetEntity) ||
                !isActiveInHierarchy(waiter.targetEntity)) {
                continue;
            }
            if (waiter.componentType.empty()) {
                (void)invoke(waiter.targetEntity, waiter.callbackMethod, {});
            } else {
                (void)invokeComponent(
                    waiter.targetEntity,
                    waiter.componentType,
                    waiter.callbackMethod,
                    {});
            }
        }
    }
    return completedCount;
}

std::size_t SceneScriptRuntime::emitSignal(
    SceneEntityId sourceEntity,
    const std::string& sourceComponent,
    const std::string& signalName,
    const std::vector<runtime::Value>& arguments) {
    if (sourceEntity == BroadcastTarget || signalName.empty()) return 0;
    ScriptObject* srcObj = sourceComponent.empty()
        ? object(sourceEntity)
        : getComponent(sourceEntity, sourceComponent);
    const std::string resolvedSrcComp = srcObj ? srcObj->type().canonicalName() : sourceComponent;

    std::size_t invokedCount = 0;
    const auto connectionsSnapshot = signalConnections_;
    for (const auto& conn : connectionsSnapshot) {
        if (conn.sourceEntity != sourceEntity || conn.signalName != signalName) {
            continue;
        }
        if (!sourceComponent.empty() && conn.sourceComponent != resolvedSrcComp &&
            conn.sourceComponent != sourceComponent) {
            continue;
        }
        const auto foundTarget = instances_.find(conn.targetEntity);
        if (foundTarget == instances_.end()) continue;
        for (auto& targetInst : foundTarget->second) {
            if (!matchesComponentType(targetInst.object.type(), conn.targetComponent)) {
                continue;
            }
            const auto* method = findCachedMethod(
                targetInst.object.type(), conn.targetMethod, arguments.size());
            if (!method) {
                runtime::RuntimeError missingErr;
                missingErr.code = runtime::ErrorCode::FunctionNotFound;
                missingErr.message = "script callback '" + conn.targetMethod + "' was not found";
                recordError(conn.targetEntity, conn.targetMethod, missingErr);
                break;
            }
            const auto res = runtime_.invoke(
                targetInst.object, *method, arguments, executionOptions_);
            if (consumeFireAndForgetInvocationResult(
                    conn.targetEntity, *method, res, conn.targetMethod)) {
                ++invokedCount;
            }
            break;
        }
    }

    if (signalTopicPublisher_) {
        invokedCount += signalTopicPublisher_("$signal::" + std::to_string(sourceEntity) + "::" + signalName);
        invokedCount += signalTopicPublisher_(signalName);
    }
    return invokedCount;
}

void SceneScriptRuntime::restoreInputSnapshot(
    std::map<std::string, bool> currentActions,
    std::map<std::string, bool> previousActions,
    std::map<std::string, Vec2> axes2D) {
    currentInputActions_ = std::move(currentActions);
    previousInputActions_ = std::move(previousActions);
    inputAxes2D_ = std::move(axes2D);
}

void SceneScriptRuntime::setInputAction(const std::string& action, bool pressed) {
    if (action.empty()) return;
    currentInputActions_[action] = pressed;
}

void SceneScriptRuntime::setInputAxis2D(const std::string& axisName, Vec2 value) {
    if (axisName.empty()) return;
    inputAxes2D_[axisName] = value;
}

void SceneScriptRuntime::advanceInputTick() {
    previousInputActions_ = currentInputActions_;
}

bool SceneScriptRuntime::isInputActionPressed(const std::string& action) const noexcept {
    const auto it = currentInputActions_.find(action);
    return it != currentInputActions_.end() && it->second;
}

bool SceneScriptRuntime::isInputActionJustPressed(const std::string& action) const noexcept {
    const bool curr = isInputActionPressed(action);
    const auto prevIt = previousInputActions_.find(action);
    const bool prev = prevIt != previousInputActions_.end() && prevIt->second;
    return curr && !prev;
}

bool SceneScriptRuntime::isInputActionJustReleased(const std::string& action) const noexcept {
    const bool curr = isInputActionPressed(action);
    const auto prevIt = previousInputActions_.find(action);
    const bool prev = prevIt != previousInputActions_.end() && prevIt->second;
    return !curr && prev;
}

Vec2 SceneScriptRuntime::getInputAxis2D(const std::string& axisName) const noexcept {
    const auto it = inputAxes2D_.find(axisName);
    return it != inputAxes2D_.end() ? it->second : Vec2{};
}

std::uint64_t SceneScriptRuntime::computeInputDigest() const noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    const auto mixByte = [&](std::uint8_t b) {
        hash ^= b;
        hash *= 1099511628211ULL;
    };
    const auto mixU32 = [&](std::uint32_t v) {
        mixByte(static_cast<std::uint8_t>(v & 0xFFu));
        mixByte(static_cast<std::uint8_t>((v >> 8u) & 0xFFu));
        mixByte(static_cast<std::uint8_t>((v >> 16u) & 0xFFu));
        mixByte(static_cast<std::uint8_t>((v >> 24u) & 0xFFu));
    };
    const auto mixStr = [&](const std::string& s) {
        for (const unsigned char ch : s) mixByte(ch);
        mixByte(0xFFu);
    };
    for (const auto& [name, state] : currentInputActions_) {
        mixStr(name);
        mixByte(state ? 1u : 0u);
        const auto prevIt = previousInputActions_.find(name);
        const bool prev = prevIt != previousInputActions_.end() && prevIt->second;
        mixByte(prev ? 1u : 0u);
    }
    for (const auto& [axis, vec] : inputAxes2D_) {
        mixStr(axis);
        mixU32(static_cast<std::uint32_t>(vec.x.raw));
        mixU32(static_cast<std::uint32_t>(vec.y.raw));
    }
    return hash;
}

void SceneScriptRuntime::setEntityCollider2D(SceneEntityId entity, SceneCollider2D collider) {
    if (entity == BroadcastTarget || isStaleGenerationalEntity(entity)) return;
    if (entityActiveSelf_.find(entity) == entityActiveSelf_.end()) {
        entityActiveSelf_[entity] = true;
    }
    colliders2D_[entity] = collider;
}

bool SceneScriptRuntime::removeEntityCollider2D(SceneEntityId entity) {
    for (auto it = activeOverlaps2D_.begin(); it != activeOverlaps2D_.end();) {
        if (it->first == entity || it->second == entity) {
            activeOverlapWasTrigger2D_.erase(*it);
            activeContacts2D_.erase(*it);
            it = activeOverlaps2D_.erase(it);
        } else {
            ++it;
        }
    }
    return colliders2D_.erase(entity) != 0;
}

std::optional<SceneCollider2D> SceneScriptRuntime::getEntityCollider2D(
    SceneEntityId entity) const noexcept {
    const auto it = colliders2D_.find(entity);
    if (it == colliders2D_.end()) return std::nullopt;
    return it->second;
}

void SceneScriptRuntime::restorePhysicsOverlapState(
    std::set<std::pair<SceneEntityId, SceneEntityId>> activeOverlaps,
    std::map<std::pair<SceneEntityId, SceneEntityId>, bool> activeOverlapWasTrigger) {
    activeOverlaps2D_ = std::move(activeOverlaps);
    activeOverlapWasTrigger2D_ = std::move(activeOverlapWasTrigger);
    activeContacts2D_.clear();
}

namespace {

constexpr std::int64_t fixedAbs64(std::int64_t v) noexcept {
    return v < 0 ? -v : v;
}

constexpr std::int64_t fixedClamp64(std::int64_t v, std::int64_t lo, std::int64_t hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

constexpr bool collidersMatchLayers(
    const SceneCollider2D& colA,
    const SceneCollider2D& colB) noexcept {
    const std::uint32_t layerA = colA.collisionLayer & colA.layerMask;
    const std::uint32_t maskA = colA.collisionMask & colA.layerMask;
    const std::uint32_t layerB = colB.collisionLayer & colB.layerMask;
    const std::uint32_t maskB = colB.collisionMask & colB.layerMask;
    return ((layerA & maskB) != 0u) && ((layerB & maskA) != 0u);
}

bool testOverlap2D(
    Vec2 posA, const SceneCollider2D& colA,
    Vec2 posB, const SceneCollider2D& colB) noexcept {
    if (!collidersMatchLayers(colA, colB)) return false;
    if (colA.isCircle && colB.isCircle) {
        const std::int64_t dx = static_cast<std::int64_t>(posB.x.raw) - static_cast<std::int64_t>(posA.x.raw);
        const std::int64_t dy = static_cast<std::int64_t>(posB.y.raw) - static_cast<std::int64_t>(posA.y.raw);
        const std::int64_t sumR = static_cast<std::int64_t>(colA.radius.raw) + static_cast<std::int64_t>(colB.radius.raw);
        if (sumR < 0) return false;
        return (dx * dx + dy * dy) <= (sumR * sumR);
    }
    if (!colA.isCircle && !colB.isCircle) {
        const std::int64_t dx = fixedAbs64(static_cast<std::int64_t>(posB.x.raw) - static_cast<std::int64_t>(posA.x.raw));
        const std::int64_t dy = fixedAbs64(static_cast<std::int64_t>(posB.y.raw) - static_cast<std::int64_t>(posA.y.raw));
        const std::int64_t sumX = static_cast<std::int64_t>(colA.halfExtents.x.raw) + static_cast<std::int64_t>(colB.halfExtents.x.raw);
        const std::int64_t sumY = static_cast<std::int64_t>(colA.halfExtents.y.raw) + static_cast<std::int64_t>(colB.halfExtents.y.raw);
        return dx <= sumX && dy <= sumY;
    }
    const Vec2 circlePos = colA.isCircle ? posA : posB;
    const Fixed radius = colA.isCircle ? colA.radius : colB.radius;
    const Vec2 boxPos = colA.isCircle ? posB : posA;
    const Vec2 halfExt = colA.isCircle ? colB.halfExtents : colA.halfExtents;
    const std::int64_t cx = circlePos.x.raw;
    const std::int64_t cy = circlePos.y.raw;
    const std::int64_t bx = boxPos.x.raw;
    const std::int64_t by = boxPos.y.raw;
    const std::int64_t hx = halfExt.x.raw;
    const std::int64_t hy = halfExt.y.raw;
    const std::int64_t clampedX = fixedClamp64(cx, bx - hx, bx + hx);
    const std::int64_t clampedY = fixedClamp64(cy, by - hy, by + hy);
    const std::int64_t dx = cx - clampedX;
    const std::int64_t dy = cy - clampedY;
    const std::int64_t r = radius.raw;
    if (r < 0) return false;
    return (dx * dx + dy * dy) <= (r * r);
}

} // namespace

std::vector<RaycastHit2D> SceneScriptRuntime::raycastAll2D(
    Vec2 origin,
    Vec2 direction,
    Fixed maxDistance,
    std::uint32_t layerMask) const {
    std::vector<RaycastHit2D> hits;
    const Fixed dirLen = direction.length();
    if (dirLen == Fixed::fromRaw(0) || maxDistance <= Fixed::fromRaw(0)) {
        return hits;
    }
    const Vec2 dir = direction / dirLen;

    for (const auto& [entity, col] : colliders2D_) {
        const std::uint32_t effectiveLayer = col.collisionLayer & col.layerMask;
        if ((effectiveLayer & layerMask) == 0 || !isActiveInHierarchy(entity)) {
            continue;
        }
        const Vec2 center = getWorldPosition2D(entity);
        if (col.isCircle) {
            const std::int64_t toCenterX = static_cast<std::int64_t>(center.x.raw) - static_cast<std::int64_t>(origin.x.raw);
            const std::int64_t toCenterY = static_cast<std::int64_t>(center.y.raw) - static_cast<std::int64_t>(origin.y.raw);
            const std::int64_t projRaw = (toCenterX * static_cast<std::int64_t>(dir.x.raw) +
                                          toCenterY * static_cast<std::int64_t>(dir.y.raw)) >> 8;
            const std::int64_t rRaw = static_cast<std::int64_t>(col.radius.raw);
            const std::int64_t maxDistRaw = static_cast<std::int64_t>(maxDistance.raw);
            if (rRaw < 0 || projRaw < -rRaw || projRaw > maxDistRaw + rRaw) continue;
            const std::int64_t closestX = static_cast<std::int64_t>(origin.x.raw) +
                ((static_cast<std::int64_t>(dir.x.raw) * projRaw) >> 8);
            const std::int64_t closestY = static_cast<std::int64_t>(origin.y.raw) +
                ((static_cast<std::int64_t>(dir.y.raw) * projRaw) >> 8);
            const std::int64_t diffX = static_cast<std::int64_t>(center.x.raw) - closestX;
            const std::int64_t diffY = static_cast<std::int64_t>(center.y.raw) - closestY;
            const std::int64_t distSqRaw2 = diffX * diffX + diffY * diffY;
            const std::int64_t rSqRaw2 = rRaw * rRaw;
            if (distSqRaw2 > rSqRaw2) continue;
            const std::int64_t offsetRaw = deterministicSqrt64(rSqRaw2 - distSqRaw2);
            if (projRaw + offsetRaw < 0) continue;
            std::int64_t hitDistRaw = projRaw - offsetRaw;
            if (hitDistRaw < 0) hitDistRaw = 0;
            if (hitDistRaw > maxDistRaw) continue;
            const Fixed hitDist = Fixed::fromRaw(Fixed::clampRaw64(hitDistRaw));
            const Vec2 hitPt{
                Fixed::fromRaw(Fixed::clampRaw64(
                    static_cast<std::int64_t>(origin.x.raw) + ((static_cast<std::int64_t>(dir.x.raw) * hitDistRaw) >> 8))),
                Fixed::fromRaw(Fixed::clampRaw64(
                    static_cast<std::int64_t>(origin.y.raw) + ((static_cast<std::int64_t>(dir.y.raw) * hitDistRaw) >> 8))),
            };
            const Vec2 diffNorm = hitPt - center;
            const Fixed diffLen = diffNorm.length();
            const Vec2 hitNormal = diffLen > Fixed::fromRaw(0)
                ? (diffNorm / diffLen)
                : Vec2{Fixed::fromInt(-1), Fixed::fromRaw(0)};
            hits.push_back(RaycastHit2D{entity, hitPt, hitNormal, hitDist});
        } else {
            std::int64_t tMinRaw = 0;
            std::int64_t tMaxRaw = static_cast<std::int64_t>(maxDistance.raw);
            int hitAxis = (fixedAbs64(dir.y.raw) > fixedAbs64(dir.x.raw)) ? 1 : 0;
            int hitSign = (hitAxis == 0)
                ? (dir.x.raw >= 0 ? -1 : 1)
                : (dir.y.raw >= 0 ? -1 : 1);
            bool hit = true;
            for (int axis = 0; axis < 2; ++axis) {
                const std::int64_t o = axis == 0 ? origin.x.raw : origin.y.raw;
                const std::int64_t d = axis == 0 ? dir.x.raw : dir.y.raw;
                const std::int64_t c = axis == 0 ? center.x.raw : center.y.raw;
                const std::int64_t h = axis == 0 ? col.halfExtents.x.raw : col.halfExtents.y.raw;
                const std::int64_t bMin = c - h;
                const std::int64_t bMax = c + h;
                if (d == 0) {
                    if (o < bMin || o > bMax) {
                        hit = false;
                        break;
                    }
                } else {
                    std::int64_t t1 = ((bMin - o) << 8) / d;
                    std::int64_t t2 = ((bMax - o) << 8) / d;
                    const bool swapped = t1 > t2;
                    if (swapped) std::swap(t1, t2);
                    if (t1 > tMinRaw) {
                        tMinRaw = t1;
                        hitAxis = axis;
                        hitSign = swapped ? 1 : -1;
                    }
                    if (t2 < tMaxRaw) tMaxRaw = t2;
                    if (tMinRaw > tMaxRaw) {
                        hit = false;
                        break;
                    }
                }
            }
            if (!hit || tMinRaw > static_cast<std::int64_t>(maxDistance.raw)) continue;
            const Fixed tMin = Fixed::fromRaw(Fixed::clampRaw64(tMinRaw));
            const Vec2 hitPt{
                Fixed::fromRaw(Fixed::clampRaw64(
                    static_cast<std::int64_t>(origin.x.raw) + ((static_cast<std::int64_t>(dir.x.raw) * tMinRaw) >> 8))),
                Fixed::fromRaw(Fixed::clampRaw64(
                    static_cast<std::int64_t>(origin.y.raw) + ((static_cast<std::int64_t>(dir.y.raw) * tMinRaw) >> 8))),
            };
            const Vec2 hitNormal = (hitAxis == 0)
                ? Vec2{Fixed::fromInt(hitSign), Fixed::fromRaw(0)}
                : Vec2{Fixed::fromRaw(0), Fixed::fromInt(hitSign)};
            hits.push_back(RaycastHit2D{entity, hitPt, hitNormal, tMin});
        }
    }
    std::sort(hits.begin(), hits.end(), [](const RaycastHit2D& a, const RaycastHit2D& b) {
        if (a.distance != b.distance) return a.distance < b.distance;
        return a.entity < b.entity;
    });
    return hits;
}

std::optional<RaycastHit2D> SceneScriptRuntime::raycast2D(
    Vec2 origin,
    Vec2 direction,
    Fixed maxDistance,
    std::uint32_t layerMask) const {
    const auto all = raycastAll2D(origin, direction, maxDistance, layerMask);
    if (all.empty()) return std::nullopt;
    return all.front();
}

std::vector<SceneEntityId> SceneScriptRuntime::overlapCircle2D(
    Vec2 center,
    Fixed radius,
    std::uint32_t layerMask) const {
    std::vector<SceneEntityId> result;
    SceneCollider2D queryCircle;
    queryCircle.isCircle = true;
    queryCircle.radius = radius;
    queryCircle.layerMask = layerMask;
    for (const auto& [entity, col] : colliders2D_) {
        if (!isActiveInHierarchy(entity)) continue;
        if (testOverlap2D(center, queryCircle, getWorldPosition2D(entity), col)) {
            result.push_back(entity);
        }
    }
    return result;
}

namespace {

CollisionContact2D computeContact2D(
    SceneEntityId entA, Vec2 posA, const SceneCollider2D& colA,
    SceneEntityId entB, Vec2 posB, const SceneCollider2D& colB) noexcept {
    CollisionContact2D contact;
    contact.entityA = entA;
    contact.entityB = entB;
    contact.isTrigger = colA.isTrigger || colB.isTrigger;
    const Vec2 delta = posB - posA;
    if (colA.isCircle && colB.isCircle) {
        const Fixed dist = delta.length();
        const Fixed sumR = colA.radius + colB.radius;
        contact.penetration = sumR > dist ? (sumR - dist) : Fixed::fromRaw(0);
        contact.normal = dist > Fixed::fromRaw(0)
            ? (delta / dist)
            : Vec2{Fixed::fromInt(1), Fixed::fromRaw(0)};
        contact.contactPoint = posA + contact.normal * colA.radius;
        return contact;
    }
    if (colA.isCircle != colB.isCircle) {
        const bool aIsCircle = colA.isCircle;
        const Vec2 circlePos = aIsCircle ? posA : posB;
        const Fixed radius = aIsCircle ? colA.radius : colB.radius;
        const Vec2 boxPos = aIsCircle ? posB : posA;
        const Vec2 halfExt = aIsCircle ? colB.halfExtents : colA.halfExtents;
        const std::int64_t cx = circlePos.x.raw;
        const std::int64_t cy = circlePos.y.raw;
        const std::int64_t bx = boxPos.x.raw;
        const std::int64_t by = boxPos.y.raw;
        const std::int64_t hx = halfExt.x.raw;
        const std::int64_t hy = halfExt.y.raw;
        const std::int64_t clampedX = fixedClamp64(cx, bx - hx, bx + hx);
        const std::int64_t clampedY = fixedClamp64(cy, by - hy, by + hy);
        const Vec2 closestOnBox{
            Fixed::fromRaw(Fixed::clampRaw64(clampedX)),
            Fixed::fromRaw(Fixed::clampRaw64(clampedY)),
        };
        const Vec2 circleToBox = closestOnBox - circlePos;
        const Fixed dist = circleToBox.length();
        if (dist > Fixed::fromRaw(0)) {
            const Vec2 dirCircleToBox = circleToBox / dist;
            contact.normal = aIsCircle
                ? dirCircleToBox
                : Vec2{-dirCircleToBox.x, -dirCircleToBox.y};
            contact.penetration = radius > dist ? (radius - dist) : Fixed::fromRaw(0);
            contact.contactPoint = closestOnBox;
            return contact;
        }
        // Circle center is inside the AABB: push out along nearest box face
        const std::int64_t distLeft = cx - (bx - hx);
        const std::int64_t distRight = (bx + hx) - cx;
        const std::int64_t distDown = cy - (by - hy);
        const std::int64_t distUp = (by + hy) - cy;
        const std::int64_t minX = distLeft <= distRight ? distLeft : distRight;
        const std::int64_t minY = distDown <= distUp ? distDown : distUp;
        Vec2 dirCircleToBox{};
        std::int64_t penRaw = 0;
        if (minX <= minY) {
            dirCircleToBox = Vec2{
                distLeft <= distRight ? Fixed::fromInt(1) : Fixed::fromInt(-1),
                Fixed::fromRaw(0)};
            penRaw = static_cast<std::int64_t>(radius.raw) + minX;
        } else {
            dirCircleToBox = Vec2{
                Fixed::fromRaw(0),
                distDown <= distUp ? Fixed::fromInt(1) : Fixed::fromInt(-1)};
            penRaw = static_cast<std::int64_t>(radius.raw) + minY;
        }
        contact.normal = aIsCircle
            ? dirCircleToBox
            : Vec2{-dirCircleToBox.x, -dirCircleToBox.y};
        contact.penetration = Fixed::fromRaw(Fixed::clampRaw64(penRaw > 0 ? penRaw : 0));
        contact.contactPoint = circlePos;
        return contact;
    }
    const std::int64_t rxA = colA.isCircle ? colA.radius.raw : colA.halfExtents.x.raw;
    const std::int64_t ryA = colA.isCircle ? colA.radius.raw : colA.halfExtents.y.raw;
    const std::int64_t rxB = colB.isCircle ? colB.radius.raw : colB.halfExtents.x.raw;
    const std::int64_t ryB = colB.isCircle ? colB.radius.raw : colB.halfExtents.y.raw;
    const std::int64_t overlapX = (rxA + rxB) - fixedAbs64(static_cast<std::int64_t>(delta.x.raw));
    const std::int64_t overlapY = (ryA + ryB) - fixedAbs64(static_cast<std::int64_t>(delta.y.raw));
    if (overlapX <= overlapY) {
        contact.penetration = Fixed::fromRaw(Fixed::clampRaw64(overlapX > 0 ? overlapX : 0));
        contact.normal = Vec2{
            delta.x.raw >= 0 ? Fixed::fromInt(1) : Fixed::fromInt(-1),
            Fixed::fromRaw(0)};
    } else {
        contact.penetration = Fixed::fromRaw(Fixed::clampRaw64(overlapY > 0 ? overlapY : 0));
        contact.normal = Vec2{
            Fixed::fromRaw(0),
            delta.y.raw >= 0 ? Fixed::fromInt(1) : Fixed::fromInt(-1)};
    }
    contact.contactPoint = Vec2{
        Fixed::fromRaw(Fixed::clampRaw64((static_cast<std::int64_t>(posA.x.raw) + static_cast<std::int64_t>(posB.x.raw)) / 2)),
        Fixed::fromRaw(Fixed::clampRaw64((static_cast<std::int64_t>(posA.y.raw) + static_cast<std::int64_t>(posB.y.raw)) / 2)),
    };
    return contact;
}

} // namespace

std::optional<CollisionContact2D> SceneScriptRuntime::getCollisionContact2D(
    SceneEntityId entityA,
    SceneEntityId entityB) const noexcept {
    if (entityA == entityB) return std::nullopt;
    const auto key = entityA < entityB
        ? std::make_pair(entityA, entityB)
        : std::make_pair(entityB, entityA);
    const auto it = activeContacts2D_.find(key);
    if (it == activeContacts2D_.end()) return std::nullopt;
    if (entityA < entityB) return it->second;
    CollisionContact2D flipped = it->second;
    std::swap(flipped.entityA, flipped.entityB);
    flipped.normal = Vec2{-flipped.normal.x, -flipped.normal.y};
    return flipped;
}

std::size_t SceneScriptRuntime::stepPhysics2D() {
    std::set<std::pair<SceneEntityId, SceneEntityId>> currentOverlaps;
    std::map<std::pair<SceneEntityId, SceneEntityId>, bool> currentIsTrigger;
    std::map<std::pair<SceneEntityId, SceneEntityId>, CollisionContact2D> currentContacts;

    struct ActiveColliderEntry {
        SceneEntityId entity = 0;
        Vec2 pos{};
        const SceneCollider2D* col = nullptr;
        std::int64_t minX = 0;
        std::int64_t maxX = 0;
        std::int64_t minY = 0;
        std::int64_t maxY = 0;
    };
    std::vector<ActiveColliderEntry> activeEntries;
    activeEntries.reserve(colliders2D_.size());
    for (const auto& [ent, col] : colliders2D_) {
        if (!isActiveInHierarchy(ent)) continue;
        const Vec2 pos = getWorldPosition2D(ent);
        const std::int64_t extX = col.isCircle ? col.radius.raw : col.halfExtents.x.raw;
        const std::int64_t extY = col.isCircle ? col.radius.raw : col.halfExtents.y.raw;
        activeEntries.push_back(ActiveColliderEntry{
            ent,
            pos,
            &col,
            static_cast<std::int64_t>(pos.x.raw) - extX,
            static_cast<std::int64_t>(pos.x.raw) + extX,
            static_cast<std::int64_t>(pos.y.raw) - extY,
            static_cast<std::int64_t>(pos.y.raw) + extY,
        });
    }

    // Spatial Hash Grid broadphase (cell size = 16 world units = 4096 raw Q24.8 units)
    constexpr std::int64_t kCellShift = 12; // 4096 raw units
    std::unordered_map<std::uint64_t, std::vector<std::size_t>> gridCells;
    std::set<std::pair<std::size_t, std::size_t>> candidatePairs;
    bool useSweepFallback = false;

    for (std::size_t idx = 0; idx < activeEntries.size(); ++idx) {
        const auto& e = activeEntries[idx];
        const std::int64_t cMinX = e.minX >> kCellShift;
        const std::int64_t cMaxX = e.maxX >> kCellShift;
        const std::int64_t cMinY = e.minY >> kCellShift;
        const std::int64_t cMaxY = e.maxY >> kCellShift;
        if ((cMaxX - cMinX + 1) * (cMaxY - cMinY + 1) > 64) {
            useSweepFallback = true;
            break;
        }
        for (std::int64_t cy = cMinY; cy <= cMaxY; ++cy) {
            for (std::int64_t cx = cMinX; cx <= cMaxX; ++cx) {
                const std::uint64_t cellKey =
                    (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cx)) << 32u) |
                    static_cast<std::uint64_t>(static_cast<std::uint32_t>(cy));
                auto& bucket = gridCells[cellKey];
                for (const std::size_t otherIdx : bucket) {
                    candidatePairs.emplace(
                        std::min(otherIdx, idx), std::max(otherIdx, idx));
                }
                bucket.push_back(idx);
            }
        }
    }

    if (useSweepFallback) {
        candidatePairs.clear();
        for (std::size_t i = 0; i < activeEntries.size(); ++i) {
            for (std::size_t j = i + 1; j < activeEntries.size(); ++j) {
                candidatePairs.emplace(i, j);
            }
        }
    }
    lastBroadphasePairs2D_ = candidatePairs.size();

    for (const auto& [idxA, idxB] : candidatePairs) {
        const auto& a = activeEntries[idxA];
        const auto& b = activeEntries[idxB];
        if (testOverlap2D(a.pos, *a.col, b.pos, *b.col)) {
            const auto pair = a.entity < b.entity
                ? std::make_pair(a.entity, b.entity)
                : std::make_pair(b.entity, a.entity);
            currentOverlaps.insert(pair);
            currentIsTrigger[pair] = a.col->isTrigger || b.col->isTrigger;
            currentContacts[pair] = (a.entity < b.entity)
                ? computeContact2D(a.entity, a.pos, *a.col, b.entity, b.pos, *b.col)
                : computeContact2D(b.entity, b.pos, *b.col, a.entity, a.pos, *a.col);
        }
    }
    activeContacts2D_ = currentContacts;

    std::size_t callbacksFired = 0;
    const auto dispatchPhysicsCallback = [&](
        SceneEntityId selfEntity,
        SceneEntityId otherEntity,
        bool isTrigger,
        int phase /* 0=Enter, 1=Stay, 2=Exit */) {
        const auto instIt = instances_.find(selfEntity);
        if (instIt == instances_.end()) return;
        for (auto& instance : instIt->second) {
            if (!instance.enabled || !shouldTickInCurrentMode(instance)) continue;
            const std::optional<ScriptMethod>* slot = nullptr;
            const char* cbName = "";
            if (isTrigger) {
                if (phase == 0) { slot = &instance.onTriggerEnter; cbName = "OnTriggerEnter"; }
                else if (phase == 1) { slot = &instance.onTriggerStay; cbName = "OnTriggerStay"; }
                else { slot = &instance.onTriggerExit; cbName = "OnTriggerExit"; }
            } else {
                if (phase == 0) { slot = &instance.onCollisionEnter; cbName = "OnCollisionEnter"; }
                else if (phase == 1) { slot = &instance.onCollisionStay; cbName = "OnCollisionStay"; }
                else { slot = &instance.onCollisionExit; cbName = "OnCollisionExit"; }
            }
            if (!slot || !*slot) continue;
            const auto& param = (*slot)->descriptor.parameters.back();
            runtime::Value argVal = param.type == semantic::PrimitiveType::Long
                ? runtime::Value{runtime::LongValue{static_cast<std::int64_t>(otherEntity)}}
                : runtime::Value{static_cast<std::int64_t>(otherEntity)};
            if (invokeLifecycle(selfEntity, instance, *slot, {std::move(argVal)}, cbName)) {
                ++callbacksFired;
            }
        }
    };

    // Enter & Stay
    const auto previousOverlapsSnapshot = activeOverlaps2D_;
    const auto previousTriggerSnapshot = activeOverlapWasTrigger2D_;
    activeOverlaps2D_ = currentOverlaps;
    activeOverlapWasTrigger2D_ = currentIsTrigger;

    for (const auto& pair : currentOverlaps) {
        if (colliders2D_.find(pair.first) == colliders2D_.end() ||
            colliders2D_.find(pair.second) == colliders2D_.end()) {
            continue;
        }
        const bool isTrig = currentIsTrigger[pair];
        const int phase = previousOverlapsSnapshot.find(pair) == previousOverlapsSnapshot.end() ? 0 : 1;
        dispatchPhysicsCallback(pair.first, pair.second, isTrig, phase);
        dispatchPhysicsCallback(pair.second, pair.first, isTrig, phase);
    }
    // Exit
    for (const auto& pair : previousOverlapsSnapshot) {
        if (currentOverlaps.find(pair) == currentOverlaps.end()) {
            const auto trigIt = previousTriggerSnapshot.find(pair);
            const bool isTrig = trigIt != previousTriggerSnapshot.end() && trigIt->second;
            dispatchPhysicsCallback(pair.first, pair.second, isTrig, 2);
            dispatchPhysicsCallback(pair.second, pair.first, isTrig, 2);
        }
    }

    (void)flushDestroyedEntities();
    return callbacksFired;
}

void SceneScriptRuntime::setEntityCollider3D(SceneEntityId entity, SceneCollider3D collider) {
    if (entity == BroadcastTarget || isStaleGenerationalEntity(entity)) return;
    if (entityActiveSelf_.find(entity) == entityActiveSelf_.end()) {
        entityActiveSelf_[entity] = true;
    }
    colliders3D_[entity] = collider;
}

bool SceneScriptRuntime::removeEntityCollider3D(SceneEntityId entity) {
    for (auto it = activeOverlaps3D_.begin(); it != activeOverlaps3D_.end();) {
        if (it->first == entity || it->second == entity) {
            activeOverlapWasTrigger3D_.erase(*it);
            activeContacts3D_.erase(*it);
            it = activeOverlaps3D_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = activeContacts3D_.begin(); it != activeContacts3D_.end();) {
        if (it->first.first == entity || it->first.second == entity) {
            it = activeContacts3D_.erase(it);
        } else {
            ++it;
        }
    }
    return colliders3D_.erase(entity) != 0;
}

std::optional<SceneCollider3D> SceneScriptRuntime::getEntityCollider3D(
    SceneEntityId entity) const noexcept {
    const auto it = colliders3D_.find(entity);
    if (it == colliders3D_.end()) return std::nullopt;
    return it->second;
}

void SceneScriptRuntime::restorePhysicsOverlapState3D(
    std::set<std::pair<SceneEntityId, SceneEntityId>> activeOverlaps,
    std::map<std::pair<SceneEntityId, SceneEntityId>, bool> activeOverlapWasTrigger) {
    activeOverlaps3D_ = std::move(activeOverlaps);
    activeOverlapWasTrigger3D_ = std::move(activeOverlapWasTrigger);
    activeContacts3D_.clear();
}

namespace {

constexpr bool collidersMatchLayers3D(
    const SceneCollider3D& colA,
    const SceneCollider3D& colB) noexcept {
    const std::uint32_t layerA = colA.collisionLayer & colA.layerMask;
    const std::uint32_t maskA = colA.collisionMask & colA.layerMask;
    const std::uint32_t layerB = colB.collisionLayer & colB.layerMask;
    const std::uint32_t maskB = colB.collisionMask & colB.layerMask;
    return ((layerA & maskB) != 0u) && ((layerB & maskA) != 0u);
}

bool testOverlap3D(
    Vec3 posA, const SceneCollider3D& colA,
    Vec3 posB, const SceneCollider3D& colB) noexcept {
    if (!collidersMatchLayers3D(colA, colB)) return false;
    if (colA.isSphere && colB.isSphere) {
        const std::int64_t dx = static_cast<std::int64_t>(posB.x.raw) - static_cast<std::int64_t>(posA.x.raw);
        const std::int64_t dy = static_cast<std::int64_t>(posB.y.raw) - static_cast<std::int64_t>(posA.y.raw);
        const std::int64_t dz = static_cast<std::int64_t>(posB.z.raw) - static_cast<std::int64_t>(posA.z.raw);
        const std::int64_t sumR = static_cast<std::int64_t>(colA.radius.raw) + static_cast<std::int64_t>(colB.radius.raw);
        if (sumR < 0) return false;
        return (dx * dx + dy * dy + dz * dz) <= (sumR * sumR);
    }
    if (!colA.isSphere && !colB.isSphere) {
        const std::int64_t dx = fixedAbs64(static_cast<std::int64_t>(posB.x.raw) - static_cast<std::int64_t>(posA.x.raw));
        const std::int64_t dy = fixedAbs64(static_cast<std::int64_t>(posB.y.raw) - static_cast<std::int64_t>(posA.y.raw));
        const std::int64_t dz = fixedAbs64(static_cast<std::int64_t>(posB.z.raw) - static_cast<std::int64_t>(posA.z.raw));
        const std::int64_t sumX = static_cast<std::int64_t>(colA.halfExtents.x.raw) + static_cast<std::int64_t>(colB.halfExtents.x.raw);
        const std::int64_t sumY = static_cast<std::int64_t>(colA.halfExtents.y.raw) + static_cast<std::int64_t>(colB.halfExtents.y.raw);
        const std::int64_t sumZ = static_cast<std::int64_t>(colA.halfExtents.z.raw) + static_cast<std::int64_t>(colB.halfExtents.z.raw);
        return dx <= sumX && dy <= sumY && dz <= sumZ;
    }
    const Vec3 spherePos = colA.isSphere ? posA : posB;
    const Fixed radius = colA.isSphere ? colA.radius : colB.radius;
    const Vec3 boxPos = colA.isSphere ? posB : posA;
    const Vec3 halfExt = colA.isSphere ? colB.halfExtents : colA.halfExtents;
    const std::int64_t clampedX = fixedClamp64(spherePos.x.raw, static_cast<std::int64_t>(boxPos.x.raw) - halfExt.x.raw, static_cast<std::int64_t>(boxPos.x.raw) + halfExt.x.raw);
    const std::int64_t clampedY = fixedClamp64(spherePos.y.raw, static_cast<std::int64_t>(boxPos.y.raw) - halfExt.y.raw, static_cast<std::int64_t>(boxPos.y.raw) + halfExt.y.raw);
    const std::int64_t clampedZ = fixedClamp64(spherePos.z.raw, static_cast<std::int64_t>(boxPos.z.raw) - halfExt.z.raw, static_cast<std::int64_t>(boxPos.z.raw) + halfExt.z.raw);
    const std::int64_t dx = static_cast<std::int64_t>(spherePos.x.raw) - clampedX;
    const std::int64_t dy = static_cast<std::int64_t>(spherePos.y.raw) - clampedY;
    const std::int64_t dz = static_cast<std::int64_t>(spherePos.z.raw) - clampedZ;
    const std::int64_t r = radius.raw;
    if (r < 0) return false;
    return (dx * dx + dy * dy + dz * dz) <= (r * r);
}

CollisionContact3D computeContact3D(
    SceneEntityId entA, Vec3 posA, const SceneCollider3D& colA,
    SceneEntityId entB, Vec3 posB, const SceneCollider3D& colB) noexcept {
    CollisionContact3D contact;
    contact.entityA = entA;
    contact.entityB = entB;
    contact.isTrigger = colA.isTrigger || colB.isTrigger;
    const Vec3 delta = posB - posA;
    if (colA.isSphere && colB.isSphere) {
        const Fixed dist = delta.length();
        const Fixed sumR = colA.radius + colB.radius;
        contact.penetration = sumR > dist ? (sumR - dist) : Fixed::fromRaw(0);
        contact.normal = dist > Fixed::fromRaw(0)
            ? (delta / dist)
            : Vec3{Fixed::fromInt(1), Fixed::fromRaw(0), Fixed::fromRaw(0)};
        contact.contactPoint = posA + contact.normal * colA.radius;
        return contact;
    }
    if (colA.isSphere != colB.isSphere) {
        const bool aIsSphere = colA.isSphere;
        const Vec3 spherePos = aIsSphere ? posA : posB;
        const Fixed radius = aIsSphere ? colA.radius : colB.radius;
        const Vec3 boxPos = aIsSphere ? posB : posA;
        const Vec3 halfExt = aIsSphere ? colB.halfExtents : colA.halfExtents;
        const std::int64_t sx = spherePos.x.raw;
        const std::int64_t sy = spherePos.y.raw;
        const std::int64_t sz = spherePos.z.raw;
        const std::int64_t bx = boxPos.x.raw;
        const std::int64_t by = boxPos.y.raw;
        const std::int64_t bz = boxPos.z.raw;
        const std::int64_t hx = halfExt.x.raw;
        const std::int64_t hy = halfExt.y.raw;
        const std::int64_t hz = halfExt.z.raw;
        const std::int64_t clampedX = fixedClamp64(sx, bx - hx, bx + hx);
        const std::int64_t clampedY = fixedClamp64(sy, by - hy, by + hy);
        const std::int64_t clampedZ = fixedClamp64(sz, bz - hz, bz + hz);
        const Vec3 closestOnBox{
            Fixed::fromRaw(Fixed::clampRaw64(clampedX)),
            Fixed::fromRaw(Fixed::clampRaw64(clampedY)),
            Fixed::fromRaw(Fixed::clampRaw64(clampedZ)),
        };
        const Vec3 sphereToBox = closestOnBox - spherePos;
        const Fixed dist = sphereToBox.length();
        if (dist > Fixed::fromRaw(0)) {
            const Vec3 dirSphereToBox = sphereToBox / dist;
            contact.normal = aIsSphere
                ? dirSphereToBox
                : Vec3{-dirSphereToBox.x, -dirSphereToBox.y, -dirSphereToBox.z};
            contact.penetration = radius > dist ? (radius - dist) : Fixed::fromRaw(0);
            contact.contactPoint = closestOnBox;
            return contact;
        }
        // Sphere center is inside the 3D AABB: push out along nearest box face
        const std::int64_t distLeft = sx - (bx - hx);
        const std::int64_t distRight = (bx + hx) - sx;
        const std::int64_t distDown = sy - (by - hy);
        const std::int64_t distUp = (by + hy) - sy;
        const std::int64_t distBack = sz - (bz - hz);
        const std::int64_t distFront = (bz + hz) - sz;
        const std::int64_t minX = distLeft <= distRight ? distLeft : distRight;
        const std::int64_t minY = distDown <= distUp ? distDown : distUp;
        const std::int64_t minZ = distBack <= distFront ? distBack : distFront;
        Vec3 dirSphereToBox{};
        std::int64_t penRaw = 0;
        if (minX <= minY && minX <= minZ) {
            dirSphereToBox = Vec3{
                distLeft <= distRight ? Fixed::fromInt(1) : Fixed::fromInt(-1),
                Fixed::fromRaw(0),
                Fixed::fromRaw(0)};
            penRaw = static_cast<std::int64_t>(radius.raw) + minX;
        } else if (minY <= minX && minY <= minZ) {
            dirSphereToBox = Vec3{
                Fixed::fromRaw(0),
                distDown <= distUp ? Fixed::fromInt(1) : Fixed::fromInt(-1),
                Fixed::fromRaw(0)};
            penRaw = static_cast<std::int64_t>(radius.raw) + minY;
        } else {
            dirSphereToBox = Vec3{
                Fixed::fromRaw(0),
                Fixed::fromRaw(0),
                distBack <= distFront ? Fixed::fromInt(1) : Fixed::fromInt(-1)};
            penRaw = static_cast<std::int64_t>(radius.raw) + minZ;
        }
        contact.normal = aIsSphere
            ? dirSphereToBox
            : Vec3{-dirSphereToBox.x, -dirSphereToBox.y, -dirSphereToBox.z};
        contact.penetration = Fixed::fromRaw(Fixed::clampRaw64(penRaw > 0 ? penRaw : 0));
        contact.contactPoint = spherePos;
        return contact;
    }
    const std::int64_t rxA = colA.isSphere ? colA.radius.raw : colA.halfExtents.x.raw;
    const std::int64_t ryA = colA.isSphere ? colA.radius.raw : colA.halfExtents.y.raw;
    const std::int64_t rzA = colA.isSphere ? colA.radius.raw : colA.halfExtents.z.raw;
    const std::int64_t rxB = colB.isSphere ? colB.radius.raw : colB.halfExtents.x.raw;
    const std::int64_t ryB = colB.isSphere ? colB.radius.raw : colB.halfExtents.y.raw;
    const std::int64_t rzB = colB.isSphere ? colB.radius.raw : colB.halfExtents.z.raw;
    const std::int64_t overlapX = (rxA + rxB) - fixedAbs64(static_cast<std::int64_t>(delta.x.raw));
    const std::int64_t overlapY = (ryA + ryB) - fixedAbs64(static_cast<std::int64_t>(delta.y.raw));
    const std::int64_t overlapZ = (rzA + rzB) - fixedAbs64(static_cast<std::int64_t>(delta.z.raw));
    if (overlapX <= overlapY && overlapX <= overlapZ) {
        contact.penetration = Fixed::fromRaw(Fixed::clampRaw64(overlapX > 0 ? overlapX : 0));
        contact.normal = Vec3{delta.x.raw >= 0 ? Fixed::fromInt(1) : Fixed::fromInt(-1), Fixed::fromRaw(0), Fixed::fromRaw(0)};
    } else if (overlapY <= overlapX && overlapY <= overlapZ) {
        contact.penetration = Fixed::fromRaw(Fixed::clampRaw64(overlapY > 0 ? overlapY : 0));
        contact.normal = Vec3{Fixed::fromRaw(0), delta.y.raw >= 0 ? Fixed::fromInt(1) : Fixed::fromInt(-1), Fixed::fromRaw(0)};
    } else {
        contact.penetration = Fixed::fromRaw(Fixed::clampRaw64(overlapZ > 0 ? overlapZ : 0));
        contact.normal = Vec3{Fixed::fromRaw(0), Fixed::fromRaw(0), delta.z.raw >= 0 ? Fixed::fromInt(1) : Fixed::fromInt(-1)};
    }
    contact.contactPoint = Vec3{
        Fixed::fromRaw(Fixed::clampRaw64((static_cast<std::int64_t>(posA.x.raw) + static_cast<std::int64_t>(posB.x.raw)) / 2)),
        Fixed::fromRaw(Fixed::clampRaw64((static_cast<std::int64_t>(posA.y.raw) + static_cast<std::int64_t>(posB.y.raw)) / 2)),
        Fixed::fromRaw(Fixed::clampRaw64((static_cast<std::int64_t>(posA.z.raw) + static_cast<std::int64_t>(posB.z.raw)) / 2)),
    };
    return contact;
}

} // namespace

std::vector<RaycastHit3D> SceneScriptRuntime::raycastAll3D(
    Vec3 origin,
    Vec3 direction,
    Fixed maxDistance,
    std::uint32_t layerMask) const {
    std::vector<RaycastHit3D> hits;
    const Fixed dirLen = direction.length();
    if (dirLen == Fixed::fromRaw(0) || maxDistance <= Fixed::fromRaw(0)) {
        return hits;
    }
    const Vec3 dir = direction / dirLen;

    for (const auto& [entity, col] : colliders3D_) {
        const std::uint32_t effectiveLayer = col.collisionLayer & col.layerMask;
        if ((effectiveLayer & layerMask) == 0 || !isActiveInHierarchy(entity)) {
            continue;
        }
        const Vec3 center = getWorldTransform(entity).position;
        if (col.isSphere) {
            const std::int64_t toCenterX = static_cast<std::int64_t>(center.x.raw) - static_cast<std::int64_t>(origin.x.raw);
            const std::int64_t toCenterY = static_cast<std::int64_t>(center.y.raw) - static_cast<std::int64_t>(origin.y.raw);
            const std::int64_t toCenterZ = static_cast<std::int64_t>(center.z.raw) - static_cast<std::int64_t>(origin.z.raw);
            const std::int64_t projRaw = (toCenterX * static_cast<std::int64_t>(dir.x.raw) +
                                          toCenterY * static_cast<std::int64_t>(dir.y.raw) +
                                          toCenterZ * static_cast<std::int64_t>(dir.z.raw)) >> 8;
            const std::int64_t rRaw = static_cast<std::int64_t>(col.radius.raw);
            const std::int64_t maxDistRaw = static_cast<std::int64_t>(maxDistance.raw);
            if (rRaw < 0 || projRaw < -rRaw || projRaw > maxDistRaw + rRaw) continue;
            const std::int64_t closestX = static_cast<std::int64_t>(origin.x.raw) + ((static_cast<std::int64_t>(dir.x.raw) * projRaw) >> 8);
            const std::int64_t closestY = static_cast<std::int64_t>(origin.y.raw) + ((static_cast<std::int64_t>(dir.y.raw) * projRaw) >> 8);
            const std::int64_t closestZ = static_cast<std::int64_t>(origin.z.raw) + ((static_cast<std::int64_t>(dir.z.raw) * projRaw) >> 8);
            const std::int64_t diffX = static_cast<std::int64_t>(center.x.raw) - closestX;
            const std::int64_t diffY = static_cast<std::int64_t>(center.y.raw) - closestY;
            const std::int64_t diffZ = static_cast<std::int64_t>(center.z.raw) - closestZ;
            const std::int64_t distSqRaw2 = diffX * diffX + diffY * diffY + diffZ * diffZ;
            const std::int64_t rSqRaw2 = rRaw * rRaw;
            if (distSqRaw2 > rSqRaw2) continue;
            const std::int64_t offsetRaw = deterministicSqrt64(rSqRaw2 - distSqRaw2);
            if (projRaw + offsetRaw < 0) continue;
            std::int64_t hitDistRaw = projRaw - offsetRaw;
            if (hitDistRaw < 0) hitDistRaw = 0;
            if (hitDistRaw > maxDistRaw) continue;
            const Fixed hitDist = Fixed::fromRaw(Fixed::clampRaw64(hitDistRaw));
            const Vec3 hitPt{
                Fixed::fromRaw(Fixed::clampRaw64(static_cast<std::int64_t>(origin.x.raw) + ((static_cast<std::int64_t>(dir.x.raw) * hitDistRaw) >> 8))),
                Fixed::fromRaw(Fixed::clampRaw64(static_cast<std::int64_t>(origin.y.raw) + ((static_cast<std::int64_t>(dir.y.raw) * hitDistRaw) >> 8))),
                Fixed::fromRaw(Fixed::clampRaw64(static_cast<std::int64_t>(origin.z.raw) + ((static_cast<std::int64_t>(dir.z.raw) * hitDistRaw) >> 8))),
            };
            const Vec3 diffNorm = hitPt - center;
            const Fixed diffLen = diffNorm.length();
            const Vec3 hitNormal = diffLen > Fixed::fromRaw(0)
                ? (diffNorm / diffLen)
                : Vec3{Fixed::fromInt(-1), Fixed::fromRaw(0), Fixed::fromRaw(0)};
            hits.push_back(RaycastHit3D{entity, hitPt, hitNormal, hitDist});
        } else {
            std::int64_t tMinRaw = 0;
            std::int64_t tMaxRaw = static_cast<std::int64_t>(maxDistance.raw);
            int entryAxis = -1;
            bool hit = true;
            for (int axis = 0; axis < 3; ++axis) {
                const std::int64_t o = axis == 0 ? origin.x.raw : (axis == 1 ? origin.y.raw : origin.z.raw);
                const std::int64_t d = axis == 0 ? dir.x.raw : (axis == 1 ? dir.y.raw : dir.z.raw);
                const std::int64_t c = axis == 0 ? center.x.raw : (axis == 1 ? center.y.raw : center.z.raw);
                const std::int64_t h = axis == 0 ? col.halfExtents.x.raw : (axis == 1 ? col.halfExtents.y.raw : col.halfExtents.z.raw);
                const std::int64_t bMin = c - h;
                const std::int64_t bMax = c + h;
                if (d == 0) {
                    if (o < bMin || o > bMax) {
                        hit = false;
                        break;
                    }
                } else {
                    std::int64_t t1 = ((bMin - o) << 8) / d;
                    std::int64_t t2 = ((bMax - o) << 8) / d;
                    if (t1 > t2) std::swap(t1, t2);
                    if (t1 > tMinRaw) {
                        tMinRaw = t1;
                        entryAxis = axis;
                    }
                    if (t2 < tMaxRaw) tMaxRaw = t2;
                    if (tMinRaw > tMaxRaw) {
                        hit = false;
                        break;
                    }
                }
            }
            if (!hit || tMinRaw > static_cast<std::int64_t>(maxDistance.raw)) continue;
            if (entryAxis == -1) {
                const std::int64_t ax = fixedAbs64(dir.x.raw);
                const std::int64_t ay = fixedAbs64(dir.y.raw);
                const std::int64_t az = fixedAbs64(dir.z.raw);
                entryAxis = (ax >= ay && ax >= az) ? 0 : (ay >= az ? 1 : 2);
            }
            const Fixed tMin = Fixed::fromRaw(Fixed::clampRaw64(tMinRaw));
            const Vec3 hitPt{
                Fixed::fromRaw(Fixed::clampRaw64(static_cast<std::int64_t>(origin.x.raw) + ((static_cast<std::int64_t>(dir.x.raw) * tMinRaw) >> 8))),
                Fixed::fromRaw(Fixed::clampRaw64(static_cast<std::int64_t>(origin.y.raw) + ((static_cast<std::int64_t>(dir.y.raw) * tMinRaw) >> 8))),
                Fixed::fromRaw(Fixed::clampRaw64(static_cast<std::int64_t>(origin.z.raw) + ((static_cast<std::int64_t>(dir.z.raw) * tMinRaw) >> 8))),
            };
            Vec3 hitNormal{};
            if (entryAxis == 0) {
                hitNormal = Vec3{dir.x.raw >= 0 ? Fixed::fromInt(-1) : Fixed::fromInt(1), Fixed::fromRaw(0), Fixed::fromRaw(0)};
            } else if (entryAxis == 1) {
                hitNormal = Vec3{Fixed::fromRaw(0), dir.y.raw >= 0 ? Fixed::fromInt(-1) : Fixed::fromInt(1), Fixed::fromRaw(0)};
            } else {
                hitNormal = Vec3{Fixed::fromRaw(0), Fixed::fromRaw(0), dir.z.raw >= 0 ? Fixed::fromInt(-1) : Fixed::fromInt(1)};
            }
            hits.push_back(RaycastHit3D{entity, hitPt, hitNormal, tMin});
        }
    }
    std::sort(hits.begin(), hits.end(), [](const RaycastHit3D& a, const RaycastHit3D& b) {
        if (a.distance != b.distance) return a.distance < b.distance;
        return a.entity < b.entity;
    });
    return hits;
}

std::optional<RaycastHit3D> SceneScriptRuntime::raycast3D(
    Vec3 origin,
    Vec3 direction,
    Fixed maxDistance,
    std::uint32_t layerMask) const {
    const auto all = raycastAll3D(origin, direction, maxDistance, layerMask);
    if (all.empty()) return std::nullopt;
    return all.front();
}

std::vector<SceneEntityId> SceneScriptRuntime::overlapSphere3D(
    Vec3 center,
    Fixed radius,
    std::uint32_t layerMask) const {
    std::vector<SceneEntityId> result;
    SceneCollider3D querySphere;
    querySphere.isSphere = true;
    querySphere.radius = radius;
    querySphere.layerMask = layerMask;
    for (const auto& [entity, col] : colliders3D_) {
        if (!isActiveInHierarchy(entity)) continue;
        if (testOverlap3D(center, querySphere, getWorldTransform(entity).position, col)) {
            result.push_back(entity);
        }
    }
    return result;
}

std::optional<CollisionContact3D> SceneScriptRuntime::getCollisionContact3D(
    SceneEntityId entityA,
    SceneEntityId entityB) const noexcept {
    if (entityA == entityB) return std::nullopt;
    const auto key = entityA < entityB
        ? std::make_pair(entityA, entityB)
        : std::make_pair(entityB, entityA);
    const auto it = activeContacts3D_.find(key);
    if (it == activeContacts3D_.end()) return std::nullopt;
    if (entityA < entityB) return it->second;
    CollisionContact3D flipped = it->second;
    std::swap(flipped.entityA, flipped.entityB);
    flipped.normal = Vec3{-flipped.normal.x, -flipped.normal.y, -flipped.normal.z};
    return flipped;
}

std::size_t SceneScriptRuntime::stepPhysics3D() {
    std::set<std::pair<SceneEntityId, SceneEntityId>> currentOverlaps;
    std::map<std::pair<SceneEntityId, SceneEntityId>, bool> currentIsTrigger;
    std::map<std::pair<SceneEntityId, SceneEntityId>, CollisionContact3D> currentContacts;

    struct ActiveColliderEntry3D {
        SceneEntityId entity = 0;
        Vec3 pos{};
        const SceneCollider3D* col = nullptr;
        std::int64_t minX = 0, maxX = 0;
        std::int64_t minY = 0, maxY = 0;
        std::int64_t minZ = 0, maxZ = 0;
    };
    std::vector<ActiveColliderEntry3D> activeEntries;
    activeEntries.reserve(colliders3D_.size());
    for (const auto& [ent, col] : colliders3D_) {
        if (!isActiveInHierarchy(ent)) continue;
        const Vec3 pos = getWorldTransform(ent).position;
        const std::int64_t extX = col.isSphere ? col.radius.raw : col.halfExtents.x.raw;
        const std::int64_t extY = col.isSphere ? col.radius.raw : col.halfExtents.y.raw;
        const std::int64_t extZ = col.isSphere ? col.radius.raw : col.halfExtents.z.raw;
        activeEntries.push_back(ActiveColliderEntry3D{
            ent, pos, &col,
            static_cast<std::int64_t>(pos.x.raw) - extX, static_cast<std::int64_t>(pos.x.raw) + extX,
            static_cast<std::int64_t>(pos.y.raw) - extY, static_cast<std::int64_t>(pos.y.raw) + extY,
            static_cast<std::int64_t>(pos.z.raw) - extZ, static_cast<std::int64_t>(pos.z.raw) + extZ,
        });
    }

    constexpr std::int64_t kCellShift = 12; // 16 world units
    std::unordered_map<std::uint64_t, std::vector<std::size_t>> gridCells;
    std::set<std::pair<std::size_t, std::size_t>> candidatePairs;
    bool useSweepFallback = false;

    for (std::size_t idx = 0; idx < activeEntries.size(); ++idx) {
        const auto& e = activeEntries[idx];
        const std::int64_t cMinX = e.minX >> kCellShift, cMaxX = e.maxX >> kCellShift;
        const std::int64_t cMinY = e.minY >> kCellShift, cMaxY = e.maxY >> kCellShift;
        const std::int64_t cMinZ = e.minZ >> kCellShift, cMaxZ = e.maxZ >> kCellShift;
        if ((cMaxX - cMinX + 1) * (cMaxY - cMinY + 1) * (cMaxZ - cMinZ + 1) > 64) {
            useSweepFallback = true;
            break;
        }
        for (std::int64_t cz = cMinZ; cz <= cMaxZ; ++cz) {
            for (std::int64_t cy = cMinY; cy <= cMaxY; ++cy) {
                for (std::int64_t cx = cMinX; cx <= cMaxX; ++cx) {
                    const std::uint64_t cellKey =
                        (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cx) & 0x1FFFFFu) << 42u) |
                        (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cy) & 0x1FFFFFu) << 21u) |
                        static_cast<std::uint64_t>(static_cast<std::uint32_t>(cz) & 0x1FFFFFu);
                    auto& bucket = gridCells[cellKey];
                    for (const std::size_t otherIdx : bucket) {
                        candidatePairs.emplace(std::min(otherIdx, idx), std::max(otherIdx, idx));
                    }
                    bucket.push_back(idx);
                }
            }
        }
    }

    if (useSweepFallback) {
        candidatePairs.clear();
        for (std::size_t i = 0; i < activeEntries.size(); ++i) {
            for (std::size_t j = i + 1; j < activeEntries.size(); ++j) {
                candidatePairs.emplace(i, j);
            }
        }
    }
    lastBroadphasePairs3D_ = candidatePairs.size();

    for (const auto& [idxA, idxB] : candidatePairs) {
        const auto& a = activeEntries[idxA];
        const auto& b = activeEntries[idxB];
        if (testOverlap3D(a.pos, *a.col, b.pos, *b.col)) {
            const auto pair = a.entity < b.entity
                ? std::make_pair(a.entity, b.entity)
                : std::make_pair(b.entity, a.entity);
            currentOverlaps.insert(pair);
            currentIsTrigger[pair] = a.col->isTrigger || b.col->isTrigger;
            currentContacts[pair] = (a.entity < b.entity)
                ? computeContact3D(a.entity, a.pos, *a.col, b.entity, b.pos, *b.col)
                : computeContact3D(b.entity, b.pos, *b.col, a.entity, a.pos, *a.col);
        }
    }
    activeContacts3D_ = currentContacts;

    std::size_t callbacksFired = 0;
    const auto dispatchPhysicsCallback3D = [&](
        SceneEntityId selfEntity,
        SceneEntityId otherEntity,
        bool isTrigger,
        int phase /* 0=Enter, 1=Stay, 2=Exit */) {
        const auto instIt = instances_.find(selfEntity);
        if (instIt == instances_.end()) return;
        for (auto& instance : instIt->second) {
            if (!instance.enabled || !shouldTickInCurrentMode(instance)) continue;
            const std::optional<ScriptMethod>* slot = nullptr;
            const char* cbName = "";
            if (isTrigger) {
                if (phase == 0) { slot = &instance.onTriggerEnter3D; cbName = "OnTriggerEnter3D"; }
                else if (phase == 1) { slot = &instance.onTriggerStay3D; cbName = "OnTriggerStay3D"; }
                else { slot = &instance.onTriggerExit3D; cbName = "OnTriggerExit3D"; }
            } else {
                if (phase == 0) { slot = &instance.onCollisionEnter3D; cbName = "OnCollisionEnter3D"; }
                else if (phase == 1) { slot = &instance.onCollisionStay3D; cbName = "OnCollisionStay3D"; }
                else { slot = &instance.onCollisionExit3D; cbName = "OnCollisionExit3D"; }
            }
            if (!slot || !*slot) continue;
            const auto& param = (*slot)->descriptor.parameters.back();
            runtime::Value argVal = param.type == semantic::PrimitiveType::Long
                ? runtime::Value{runtime::LongValue{static_cast<std::int64_t>(otherEntity)}}
                : runtime::Value{static_cast<std::int64_t>(otherEntity)};
            if (invokeLifecycle(selfEntity, instance, *slot, {std::move(argVal)}, cbName)) {
                ++callbacksFired;
            }
        }
    };

    const auto previousOverlapsSnapshot = activeOverlaps3D_;
    const auto previousTriggerSnapshot = activeOverlapWasTrigger3D_;
    activeOverlaps3D_ = currentOverlaps;
    activeOverlapWasTrigger3D_ = currentIsTrigger;

    for (const auto& pair : currentOverlaps) {
        if (colliders3D_.find(pair.first) == colliders3D_.end() ||
            colliders3D_.find(pair.second) == colliders3D_.end()) {
            continue;
        }
        const bool isTrig = currentIsTrigger[pair];
        const int phase = previousOverlapsSnapshot.find(pair) == previousOverlapsSnapshot.end() ? 0 : 1;
        dispatchPhysicsCallback3D(pair.first, pair.second, isTrig, phase);
        dispatchPhysicsCallback3D(pair.second, pair.first, isTrig, phase);
    }
    for (const auto& pair : previousOverlapsSnapshot) {
        if (currentOverlaps.find(pair) == currentOverlaps.end()) {
            const auto trigIt = previousTriggerSnapshot.find(pair);
            const bool isTrig = trigIt != previousTriggerSnapshot.end() && trigIt->second;
            dispatchPhysicsCallback3D(pair.first, pair.second, isTrig, 2);
            dispatchPhysicsCallback3D(pair.second, pair.first, isTrig, 2);
        }
    }

    (void)flushDestroyedEntities();
    return callbacksFired;
}

SceneEntityId SceneScriptRuntime::spawnEntity(const std::string& name) {
    while (nextAutoEntityId_ == BroadcastTarget ||
           slotGenerations_.find(static_cast<std::uint32_t>(nextAutoEntityId_)) != slotGenerations_.end() ||
           entityActiveSelf_.find(nextAutoEntityId_) != entityActiveSelf_.end() ||
           instances_.find(nextAutoEntityId_) != instances_.end() ||
           localTransforms_.find(nextAutoEntityId_) != localTransforms_.end() ||
           parentByEntity_.find(nextAutoEntityId_) != parentByEntity_.end() ||
           colliders2D_.find(nextAutoEntityId_) != colliders2D_.end() ||
           colliders3D_.find(nextAutoEntityId_) != colliders3D_.end()) {
        ++nextAutoEntityId_;
    }
    const SceneEntityId id = nextAutoEntityId_++;
    if (nextGenerationalSlot_ <= static_cast<std::uint32_t>(id)) {
        nextGenerationalSlot_ = static_cast<std::uint32_t>(id) + 1u;
    }
    entityActiveSelf_[id] = true;
    localTransforms_[id] = Transform3D::identity();
    if (!name.empty()) {
        entityNames_[id] = name;
    }
    return id;
}

SceneEntityId SceneScriptRuntime::spawnGenerationalEntity(const std::string& name) {
    std::uint32_t slot = 0;
    if (!freeGenerationalSlots_.empty()) {
        slot = freeGenerationalSlots_.back();
        freeGenerationalSlots_.pop_back();
    } else {
        while (nextGenerationalSlot_ == 0 ||
               entityActiveSelf_.find(nextGenerationalSlot_) != entityActiveSelf_.end() ||
               instances_.find(nextGenerationalSlot_) != instances_.end() ||
               localTransforms_.find(nextGenerationalSlot_) != localTransforms_.end() ||
               colliders2D_.find(nextGenerationalSlot_) != colliders2D_.end() ||
               colliders3D_.find(nextGenerationalSlot_) != colliders3D_.end()) {
            ++nextGenerationalSlot_;
        }
        slot = nextGenerationalSlot_++;
        if (nextAutoEntityId_ <= slot) {
            nextAutoEntityId_ = static_cast<SceneEntityId>(slot) + 1u;
        }
    }
    std::uint32_t& gen = slotGenerations_[slot];
    if (gen == 0) gen = 1;
    const SceneEntityId id = makeGenerationalEntityId(slot, gen);
    entityActiveSelf_[id] = true;
    localTransforms_[id] = Transform3D::identity();
    if (!name.empty()) {
        entityNames_[id] = name;
    }
    return id;
}

bool SceneScriptRuntime::isStaleGenerationalEntity(SceneEntityId entity) const noexcept {
    const std::uint32_t gen = entityIdGeneration(entity);
    if (gen == 0) return false;
    const auto genIt = slotGenerations_.find(entityIdSlot(entity));
    return genIt == slotGenerations_.end() || genIt->second != gen;
}

bool SceneScriptRuntime::isEntityAlive(SceneEntityId entity) const noexcept {
    if (entity == BroadcastTarget || isStaleGenerationalEntity(entity)) return false;
    return entityActiveSelf_.find(entity) != entityActiveSelf_.end() ||
           instances_.find(entity) != instances_.end() ||
           localTransforms_.find(entity) != localTransforms_.end() ||
           parentByEntity_.find(entity) != parentByEntity_.end() ||
           colliders2D_.find(entity) != colliders2D_.end() ||
           colliders3D_.find(entity) != colliders3D_.end() ||
           entityNames_.find(entity) != entityNames_.end();
}

std::uint32_t SceneScriptRuntime::getEntitySlotGeneration(std::uint32_t slot) const noexcept {
    const auto it = slotGenerations_.find(slot);
    return it != slotGenerations_.end() ? it->second : 0u;
}

void SceneScriptRuntime::registerImportedEntityHandle(SceneEntityId entity) {
    if (entity == BroadcastTarget) return;
    const std::uint32_t slot = entityIdSlot(entity);
    const std::uint32_t gen = entityIdGeneration(entity);
    if (gen != 0) {
        auto& storedGen = slotGenerations_[slot];
        if (gen > storedGen) storedGen = gen;
        freeGenerationalSlots_.erase(
            std::remove(freeGenerationalSlots_.begin(), freeGenerationalSlots_.end(), slot),
            freeGenerationalSlots_.end());
    }
    if (slot >= nextGenerationalSlot_) {
        nextGenerationalSlot_ = slot + 1u;
    }
    if (gen == 0 && entity >= nextAutoEntityId_) {
        nextAutoEntityId_ = entity + 1u;
    }
}

void SceneScriptRuntime::resetGenerationalSlots() {
    slotGenerations_.clear();
    freeGenerationalSlots_.clear();
    nextGenerationalSlot_ = 1;
    nextAutoEntityId_ = 1;
}

void SceneScriptRuntime::queueDestroyEntity(SceneEntityId entity) {
    if (entity == BroadcastTarget) return;
    if (std::find(pendingDestroyEntities_.begin(), pendingDestroyEntities_.end(), entity) ==
        pendingDestroyEntities_.end()) {
        pendingDestroyEntities_.push_back(entity);
    }
}

std::size_t SceneScriptRuntime::flushDestroyedEntities() {
    if (pendingDestroyEntities_.empty()) return 0;
    auto queue = std::move(pendingDestroyEntities_);
    pendingDestroyEntities_.clear();
    std::size_t destroyed = 0;
    for (const auto entity : queue) {
        if (detach(entity)) {
            ++destroyed;
        }
    }
    return destroyed;
}

void SceneScriptRuntime::setEntityName(SceneEntityId entity, std::string name) {
    if (entity == BroadcastTarget || isStaleGenerationalEntity(entity)) return;
    if (entityActiveSelf_.find(entity) == entityActiveSelf_.end()) {
        entityActiveSelf_[entity] = true;
    }
    if (name.empty()) {
        entityNames_.erase(entity);
    } else {
        entityNames_[entity] = std::move(name);
    }
}

std::string SceneScriptRuntime::getEntityName(SceneEntityId entity) const {
    const auto it = entityNames_.find(entity);
    return it != entityNames_.end() ? it->second : std::string{};
}

SceneEntityId SceneScriptRuntime::findEntityByName(const std::string& name) const noexcept {
    if (name.empty()) return 0;
    SceneEntityId bestActive = 0;
    SceneEntityId bestAny = 0;
    for (const auto& [entity, entName] : entityNames_) {
        if (entName != name) continue;
        if (isActiveInHierarchy(entity)) {
            if (bestActive == 0 || entity < bestActive) bestActive = entity;
        }
        if (bestAny == 0 || entity < bestAny) bestAny = entity;
    }
    return bestActive != 0 ? bestActive : bestAny;
}

void SceneScriptRuntime::addEntityToGroup(SceneEntityId entity, const std::string& group) {
    if (entity == BroadcastTarget || group.empty() || isStaleGenerationalEntity(entity)) return;
    if (entityActiveSelf_.find(entity) == entityActiveSelf_.end()) {
        entityActiveSelf_[entity] = true;
    }
    auto& vec = groupEntities_[group];
    const auto it = std::lower_bound(vec.begin(), vec.end(), entity);
    if (it == vec.end() || *it != entity) {
        vec.insert(it, entity);
    }
}

bool SceneScriptRuntime::removeEntityFromGroup(SceneEntityId entity, const std::string& group) {
    const auto it = groupEntities_.find(group);
    if (it == groupEntities_.end()) return false;
    auto& vec = it->second;
    const auto pos = std::lower_bound(vec.begin(), vec.end(), entity);
    if (pos == vec.end() || *pos != entity) return false;
    vec.erase(pos);
    if (vec.empty()) {
        groupEntities_.erase(it);
    }
    return true;
}

bool SceneScriptRuntime::isEntityInGroup(SceneEntityId entity, const std::string& group) const noexcept {
    const auto it = groupEntities_.find(group);
    return it != groupEntities_.end() &&
        std::binary_search(it->second.begin(), it->second.end(), entity);
}

std::vector<SceneEntityId> SceneScriptRuntime::getEntitiesInGroup(const std::string& group) const {
    const auto it = groupEntities_.find(group);
    if (it == groupEntities_.end()) return {};
    return it->second;
}

std::vector<std::string> SceneScriptRuntime::getEntityGroups(SceneEntityId entity) const {
    std::vector<std::string> result;
    for (const auto& [group, members] : groupEntities_) {
        if (std::binary_search(members.begin(), members.end(), entity)) {
            result.push_back(group);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

runtime::GenerationalEntityHandle SceneScriptRuntime::resolveActiveDestroyTokenHandle(
    SceneEntityId entity) const noexcept {
    if (entity == BroadcastTarget || isStaleGenerationalEntity(entity)) {
        return {};
    }
    const std::uint32_t gen = entityIdGeneration(entity);
    const std::uint32_t slot = entityIdSlot(entity);
    if (gen != 0) {
        return runtime::GenerationalEntityHandle{slot, gen};
    }
    const auto epIt = nonGenDestroyEpoch_.find(slot);
    if (epIt == nonGenDestroyEpoch_.end()) {
        return {};
    }
    return runtime::GenerationalEntityHandle{slot, 0x80000000u | epIt->second};
}

runtime::GenerationalEntityHandle SceneScriptRuntime::getOrAssignDestroyTokenHandle(
    SceneEntityId entity) {
    const std::uint32_t gen = entityIdGeneration(entity);
    const std::uint32_t slot = entityIdSlot(entity);
    if (gen != 0) {
        return runtime::GenerationalEntityHandle{slot, gen};
    }
    auto& epoch = nonGenDestroyEpoch_[slot];
    if (epoch == 0) {
        epoch = 1u;
    }
    return runtime::GenerationalEntityHandle{slot, 0x80000000u | epoch};
}

runtime::CancellationToken SceneScriptRuntime::entityDestroyToken(SceneEntityId entity) {
    const auto ctx = runtime_.asyncRuntime();
    if (!ctx || entity == BroadcastTarget || !isEntityAlive(entity)) {
        return runtime::CancellationToken::preCanceled();
    }
    const auto handle = getOrAssignDestroyTokenHandle(entity);
    const auto domain = entityIdGeneration(entity) != 0
        ? runtime::EntityDestroyDomain::SceneGenerational
        : runtime::EntityDestroyDomain::SceneNonGenerational;
    return ctx->cancellations().getOrCreateEntityDestroyToken(handle, domain);
}

bool SceneScriptRuntime::hasEntityDestroyToken(SceneEntityId entity) const noexcept {
    const auto ctx = runtime_.asyncRuntime();
    if (!ctx) return false;
    const auto handle = resolveActiveDestroyTokenHandle(entity);
    const auto domain = entityIdGeneration(entity) != 0
        ? runtime::EntityDestroyDomain::SceneGenerational
        : runtime::EntityDestroyDomain::SceneNonGenerational;
    return handle.valid() && ctx->cancellations().hasEntityDestroyToken(handle, domain);
}

bool SceneScriptRuntime::cancelEntityDestroyToken(SceneEntityId entity) {
    const auto ctx = runtime_.asyncRuntime();
    if (!ctx || entity == BroadcastTarget) return false;
    const auto handle = resolveActiveDestroyTokenHandle(entity);
    const auto domain = entityIdGeneration(entity) != 0
        ? runtime::EntityDestroyDomain::SceneGenerational
        : runtime::EntityDestroyDomain::SceneNonGenerational;
    bool canceled = false;
    if (handle.valid()) {
        canceled = ctx->cancellations().cancelEntity(handle, domain);
    }
    if (entityIdGeneration(entity) == 0) {
        const auto epIt = nonGenDestroyEpoch_.find(entityIdSlot(entity));
        if (epIt != nonGenDestroyEpoch_.end()) {
            epIt->second = (epIt->second + 1u) & 0x7FFFFFFFu;
            if (epIt->second == 0u) epIt->second = 1u;
        }
    }
    return canceled;
}

void SceneScriptRuntime::advanceDeferredAsyncIoPackets() {
    if (deferredAsyncIoPackets_.empty()) return;
    const auto ctx = runtime_.asyncRuntime();
    if (!ctx) {
        deferredAsyncIoPackets_.clear();
        return;
    }
    auto& cq = ctx->completionQueue();
    auto& cancellations = ctx->cancellations();

    std::size_t writeIdx = 0;
    for (std::size_t i = 0; i < deferredAsyncIoPackets_.size(); ++i) {
        auto& pkt = deferredAsyncIoPackets_[i];
        if (pkt.remainingFrames > 0) {
            --pkt.remainingFrames;
        }
        if (pkt.remainingFrames > 0) {
            if (writeIdx != i) {
                deferredAsyncIoPackets_[writeIdx] = std::move(pkt);
            }
            ++writeIdx;
            continue;
        }
        if (cancellations.isCancellationRequested(pkt.token)) {
            cq.submitCanceled(pkt.requestId);
            continue;
        }
        switch (pkt.kind) {
        case DeferredAsyncIoPacket::Kind::ScalarBool:
            cq.submitScalar(pkt.requestId, runtime::Value(pkt.boolValue));
            break;
        case DeferredAsyncIoPacket::Kind::String:
            cq.submitString(pkt.requestId, std::move(pkt.textPayload));
            break;
        case DeferredAsyncIoPacket::Kind::SaveCommit: {
            {
                std::lock_guard<std::mutex> lock(asyncIoMutex_);
                saveGameSlots_[pkt.key] = std::move(pkt.textPayload);
            }
            cq.submitScalar(pkt.requestId, runtime::Value(true));
            break;
        }
        case DeferredAsyncIoPacket::Kind::SaveRead: {
            std::optional<std::string> stored;
            {
                std::lock_guard<std::mutex> lock(asyncIoMutex_);
                const auto found = saveGameSlots_.find(pkt.key);
                if (found != saveGameSlots_.end()) {
                    stored = found->second;
                }
            }
            if (stored) {
                cq.submitString(pkt.requestId, std::move(*stored));
            } else {
                cq.submitFailure(
                    pkt.requestId,
                    runtime::ErrorCode::InvalidArguments,
                    "save slot '" + pkt.key + "' does not exist");
            }
            break;
        }
        case DeferredAsyncIoPacket::Kind::WsMockSend: {
            std::uint32_t slotIdx = 0;
            std::uint32_t gen = 0;
            unpackWsHandle(pkt.wsHandle, slotIdx, gen);
            bool sentOk = false;
            bool doEcho = false;
            std::string echoPayload;
            {
                std::lock_guard<std::mutex> lock(asyncIoMutex_);
                if (slotIdx < wsSessions_.size()) {
                    auto& sess = wsSessions_[slotIdx];
                    if (sess.allocated && sess.generation == gen &&
                        sess.connected && !sess.closed) {
                        sess.sentMessages.push_back(pkt.textPayload);
                        sentOk = true;
                        doEcho = sess.autoEcho;
                        if (doEcho) {
                            echoPayload = "echo:" + pkt.textPayload;
                        }
                    }
                }
            }
            if (sentOk) {
                cq.submitScalar(pkt.requestId, runtime::Value(true));
                if (doEcho) {
                    (void)pushWebSocketInboundMessage(pkt.wsHandle, std::move(echoPayload));
                }
            } else {
                cq.submitFailure(
                    pkt.requestId,
                    runtime::ErrorCode::ScriptException,
                    "WebSocket is not connected");
            }
            break;
        }
        case DeferredAsyncIoPacket::Kind::Failure:
            cq.submitFailure(pkt.requestId, pkt.errorCode, std::move(pkt.textPayload));
            break;
        }
    }
    deferredAsyncIoPackets_.resize(writeIdx);
}

bool SceneScriptRuntime::stepAsyncPreTickIo() {
    const auto ctx = runtime_.asyncRuntime();
    if (!ctx) return true;
    const bool isReplay =
        determinismSession_ &&
        determinismSession_->mode() == runtime::DeterminismMode::Replay;
    if (!isReplay) {
        advanceDeferredAsyncIoPackets();
    }

    const bool prevDrain = inFrameQueueDrain_;
    inFrameQueueDrain_ = true;
    runtime::RuntimeError ioErr;
    const bool ok = ctx->stepPreTickIo(determinismSession_, ioErr);
    inFrameQueueDrain_ = prevDrain;
    if (!ok) {
        recordError(BroadcastTarget, "AsyncPreTickIo", ioErr);
        return false;
    }
    return true;
}

std::size_t SceneScriptRuntime::stepAsyncResumeQueue(
    runtime::ResumeQueue queue,
    std::size_t maxContinuations) {
    const auto ctx = runtime_.asyncRuntime();
    if (!ctx) return 0;
    const bool prevDrain = inFrameQueueDrain_;
    inFrameQueueDrain_ = true;
    const std::size_t ran = ctx->stepResumeQueue(queue, maxContinuations);
    inFrameQueueDrain_ = prevDrain;
    return ran;
}

void SceneScriptRuntime::registerPrefabTemplate(
    std::string prefabKey,
    PrefabNodeTemplate prefab) {
    if (prefabKey.empty()) return;
    if (auto* heap = runtime_.heap().get()) {
        auto& tokens = prefabTemplateRootTokens_[prefabKey];
        releaseRootTokenList(heap, tokens);
        std::vector<runtime::Value> traceables;
        collectPrefabTraceableValues(prefab, traceables);
        for (const auto& val : traceables) {
            const auto tok = heap->addPersistentRoot(val);
            if (tok != 0) tokens.push_back(tok);
        }
        if (tokens.empty()) {
            prefabTemplateRootTokens_.erase(prefabKey);
        }
    }
    std::lock_guard<std::mutex> lock(asyncIoMutex_);
    prefabTemplates_.insert_or_assign(std::move(prefabKey), std::move(prefab));
}

bool SceneScriptRuntime::unregisterPrefabTemplate(const std::string& prefabKey) {
    if (const auto tokIt = prefabTemplateRootTokens_.find(prefabKey);
        tokIt != prefabTemplateRootTokens_.end()) {
        releaseRootTokenList(runtime_.heap().get(), tokIt->second);
        prefabTemplateRootTokens_.erase(tokIt);
    }
    std::lock_guard<std::mutex> lock(asyncIoMutex_);
    return prefabTemplates_.erase(prefabKey) != 0;
}

bool SceneScriptRuntime::hasPrefabTemplate(const std::string& prefabKey) const {
    std::lock_guard<std::mutex> lock(asyncIoMutex_);
    return prefabTemplates_.find(prefabKey) != prefabTemplates_.end();
}

void SceneScriptRuntime::setPrefabStreamWorker(AsyncPrefabStreamWorker worker) {
    std::lock_guard<std::mutex> lock(asyncIoMutex_);
    prefabStreamWorker_ = std::move(worker);
}

void SceneScriptRuntime::onAsyncPrefabIoCompleted(
    void* context,
    runtime::AwaitableHandle ioHandle,
    runtime::AwaitStatus,
    std::uint64_t opId) {
    auto* self = static_cast<SceneScriptRuntime*>(context);
    if (!self) return;
    const auto ctx = self->runtime_.asyncRuntime();
    if (!ctx) return;

    auto& pool = ctx->awaitables();
    runtime::Value ioValue{};
    runtime::RuntimeError ioError{};
    const auto terminal = pool.getResult(ioHandle, ioValue, ioError);

    const auto opIt = self->pendingPrefabOps_.find(opId);
    if (opIt == self->pendingPrefabOps_.end()) {
        return;
    }
    PendingPrefabMaterializeOp op = std::move(opIt->second);
    self->pendingPrefabOps_.erase(opIt);

    if (!pool.isValid(op.outerHandle) || pool.isCompleted(op.outerHandle)) {
        return;
    }

    if (terminal == runtime::AwaitStatus::Canceled ||
        ctx->cancellations().isCancellationRequested(op.token) ||
        (op.parentEntity != BroadcastTarget && !self->isEntityAlive(op.parentEntity))) {
        (void)pool.trySetCanceled(op.outerHandle);
        return;
    }

    if (terminal == runtime::AwaitStatus::Faulted) {
        (void)pool.trySetFaulted(
            op.outerHandle,
            ioError.code == runtime::ErrorCode::None
                ? runtime::ErrorCode::ScriptException
                : ioError.code,
            ioError.message.empty()
                ? "prefab async stream failed"
                : std::move(ioError.message));
        return;
    }

    std::optional<PrefabNodeTemplate> tpl;
    {
        std::lock_guard<std::mutex> lock(self->asyncIoMutex_);
        const auto found = self->prefabTemplates_.find(op.prefabKey);
        if (found != self->prefabTemplates_.end()) {
            tpl = found->second;
        }
    }
    if (!tpl) {
        (void)pool.trySetFaulted(
            op.outerHandle,
            runtime::ErrorCode::InvalidArguments,
            "unknown prefab template '" + op.prefabKey + "'");
        return;
    }

    const SceneEntityId spawnedRoot = self->instantiatePrefab(
        *tpl, self->nextAutoEntityId_, op.parentEntity);
    if (spawnedRoot == BroadcastTarget) {
        (void)pool.trySetFaulted(
            op.outerHandle,
            runtime::ErrorCode::ScriptException,
            "failed to instantiate streamed prefab '" + op.prefabKey + "'");
        return;
    }

    (void)pool.trySetResult(
        op.outerHandle,
        runtime::Value{runtime::LongValue{static_cast<std::int64_t>(spawnedRoot)}});
}

runtime::AwaitableHandle SceneScriptRuntime::loadPrefabSceneAsync(
    std::string prefabKey,
    SceneEntityId parentEntity,
    runtime::CancellationToken token) {
    const auto ctx = runtime_.asyncRuntime();
    if (!ctx) return runtime::AwaitableHandle::invalid();
    auto& pool = ctx->awaitables();

    if (ctx->cancellations().isCancellationRequested(token) ||
        (parentEntity != BroadcastTarget && !isEntityAlive(parentEntity))) {
        return pool.fromCanceled();
    }
    if (prefabKey.empty()) {
        return pool.fromFaulted(
            runtime::ErrorCode::InvalidArguments,
            "prefab key must not be empty");
    }

    const auto ioHandle = pool.allocatePending(runtime::ResumeQueue::PreTickIo, token);
    const auto outerHandle = pool.allocatePending(runtime::ResumeQueue::Update, token);
    const std::uint64_t opId = nextMaterializeOpId_++;
    pendingPrefabOps_.emplace(opId, PendingPrefabMaterializeOp{
        prefabKey,
        parentEntity,
        token,
        ioHandle,
        outerHandle,
    });

    runtime::AsyncContinuation cont{};
    cont.callback = &SceneScriptRuntime::onAsyncPrefabIoCompleted;
    cont.context = this;
    cont.userTag = opId;
    cont.resumeQueue = runtime::ResumeQueue::PreTickIo;
    (void)pool.onCompleted(ioHandle, cont);

    const auto reqId = ctx->completionQueue().registerRequest(
        ioHandle,
        "Scene.LoadPrefabSceneAsync",
        hashAsyncBridgeArgs(prefabKey, {}, parentEntity),
        runtime::BindingDeterminism::Recordable,
        token);

    const bool isReplay =
        determinismSession_ &&
        determinismSession_->mode() == runtime::DeterminismMode::Replay;
    if (!isReplay) {
        AsyncPrefabStreamWorker worker;
        bool exists = false;
        {
            std::lock_guard<std::mutex> lock(asyncIoMutex_);
            worker = prefabStreamWorker_;
            exists = prefabTemplates_.find(prefabKey) != prefabTemplates_.end();
        }
        if (worker) {
            worker(prefabKey, reqId, token, ctx->completionQueue());
        } else if (asyncIoLatencyFrames_ > 0) {
            DeferredAsyncIoPacket pkt;
            pkt.remainingFrames = asyncIoLatencyFrames_;
            pkt.requestId = reqId;
            pkt.token = token;
            if (exists) {
                pkt.kind = DeferredAsyncIoPacket::Kind::ScalarBool;
                pkt.boolValue = true;
            } else {
                pkt.kind = DeferredAsyncIoPacket::Kind::Failure;
                pkt.errorCode = runtime::ErrorCode::InvalidArguments;
                pkt.textPayload = "unknown prefab template '" + prefabKey + "'";
            }
            deferredAsyncIoPackets_.push_back(std::move(pkt));
        } else if (exists) {
            ctx->completionQueue().submitScalar(reqId, runtime::Value(true));
        } else {
            ctx->completionQueue().submitFailure(
                reqId,
                runtime::ErrorCode::InvalidArguments,
                "unknown prefab template '" + prefabKey + "'");
        }
    }

    return outerHandle;
}

void SceneScriptRuntime::registerAsyncAssetDefinition(
    std::string assetPath,
    std::map<std::string, runtime::Value> initialFields) {
    if (assetPath.empty()) return;
    if (auto* heap = runtime_.heap().get()) {
        auto& tokens = asyncAssetDefRootTokens_[assetPath];
        releaseRootTokenList(heap, tokens);
        for (const auto& [_, val] : initialFields) {
            if (val.isTraceable()) {
                const auto tok = heap->addPersistentRoot(val);
                if (tok != 0) tokens.push_back(tok);
            }
        }
        if (tokens.empty()) {
            asyncAssetDefRootTokens_.erase(assetPath);
        }
    }
    std::lock_guard<std::mutex> lock(asyncIoMutex_);
    asyncAssetDefinitions_.insert_or_assign(std::move(assetPath), std::move(initialFields));
}

bool SceneScriptRuntime::unregisterAsyncAssetDefinition(const std::string& assetPath) {
    if (const auto tokIt = asyncAssetDefRootTokens_.find(assetPath);
        tokIt != asyncAssetDefRootTokens_.end()) {
        releaseRootTokenList(runtime_.heap().get(), tokIt->second);
        asyncAssetDefRootTokens_.erase(tokIt);
    }
    std::lock_guard<std::mutex> lock(asyncIoMutex_);
    return asyncAssetDefinitions_.erase(assetPath) != 0;
}

void SceneScriptRuntime::setAssetLoadWorker(AsyncAssetLoadWorker worker) {
    std::lock_guard<std::mutex> lock(asyncIoMutex_);
    assetLoadWorker_ = std::move(worker);
}

void SceneScriptRuntime::onAsyncAssetIoCompleted(
    void* context,
    runtime::AwaitableHandle ioHandle,
    runtime::AwaitStatus,
    std::uint64_t opId) {
    auto* self = static_cast<SceneScriptRuntime*>(context);
    if (!self) return;
    const auto ctx = self->runtime_.asyncRuntime();
    if (!ctx) return;

    auto& pool = ctx->awaitables();
    runtime::Value ioValue{};
    runtime::RuntimeError ioError{};
    const auto terminal = pool.getResult(ioHandle, ioValue, ioError);

    const auto opIt = self->pendingAssetOps_.find(opId);
    if (opIt == self->pendingAssetOps_.end()) {
        return;
    }
    PendingAssetMaterializeOp op = std::move(opIt->second);
    self->pendingAssetOps_.erase(opIt);

    if (!pool.isValid(op.outerHandle) || pool.isCompleted(op.outerHandle)) {
        return;
    }

    if (terminal == runtime::AwaitStatus::Canceled ||
        ctx->cancellations().isCancellationRequested(op.token)) {
        (void)pool.trySetCanceled(op.outerHandle);
        return;
    }

    if (terminal == runtime::AwaitStatus::Faulted) {
        (void)pool.trySetFaulted(
            op.outerHandle,
            ioError.code == runtime::ErrorCode::None
                ? runtime::ErrorCode::ScriptException
                : ioError.code,
            ioError.message.empty()
                ? "asset async load failed"
                : std::move(ioError.message));
        return;
    }

    std::map<std::string, runtime::Value> fields;
    {
        std::lock_guard<std::mutex> lock(self->asyncIoMutex_);
        const auto found = self->asyncAssetDefinitions_.find(op.assetPath);
        if (found != self->asyncAssetDefinitions_.end()) {
            fields = found->second;
        }
    }

    runtime::RuntimeError createErr;
    const auto created = self->runtime_.createAsset(
        op.assetPath, op.canonicalTypeName, createErr, fields);
    if (!created) {
        (void)pool.trySetFaulted(
            op.outerHandle,
            createErr.code == runtime::ErrorCode::None
                ? runtime::ErrorCode::ScriptException
                : createErr.code,
            createErr.message.empty()
                ? "failed to materialize asset '" + op.assetPath + "'"
                : std::move(createErr.message));
        return;
    }

    (void)pool.trySetResult(op.outerHandle, runtime::Value(true));
}

runtime::AwaitableHandle SceneScriptRuntime::loadAssetAsync(
    std::string assetPath,
    std::string canonicalTypeName,
    runtime::CancellationToken token) {
    const auto ctx = runtime_.asyncRuntime();
    if (!ctx) return runtime::AwaitableHandle::invalid();
    auto& pool = ctx->awaitables();

    if (ctx->cancellations().isCancellationRequested(token)) {
        return pool.fromCanceled();
    }
    if (assetPath.empty() || canonicalTypeName.empty()) {
        return pool.fromFaulted(
            runtime::ErrorCode::InvalidArguments,
            "asset path and type name must not be empty");
    }

    const auto ioHandle = pool.allocatePending(runtime::ResumeQueue::PreTickIo, token);
    const auto outerHandle = pool.allocatePending(runtime::ResumeQueue::Update, token);
    const std::uint64_t opId = nextMaterializeOpId_++;
    pendingAssetOps_.emplace(opId, PendingAssetMaterializeOp{
        assetPath,
        canonicalTypeName,
        token,
        ioHandle,
        outerHandle,
    });

    runtime::AsyncContinuation cont{};
    cont.callback = &SceneScriptRuntime::onAsyncAssetIoCompleted;
    cont.context = this;
    cont.userTag = opId;
    cont.resumeQueue = runtime::ResumeQueue::PreTickIo;
    (void)pool.onCompleted(ioHandle, cont);

    const auto reqId = ctx->completionQueue().registerRequest(
        ioHandle,
        "Scene.LoadAssetAsync",
        hashAsyncBridgeArgs(assetPath, canonicalTypeName, 0),
        runtime::BindingDeterminism::Recordable,
        token);

    const bool isReplay =
        determinismSession_ &&
        determinismSession_->mode() == runtime::DeterminismMode::Replay;
    if (!isReplay) {
        AsyncAssetLoadWorker worker;
        {
            std::lock_guard<std::mutex> lock(asyncIoMutex_);
            worker = assetLoadWorker_;
        }
        if (worker) {
            worker(assetPath, canonicalTypeName, reqId, token, ctx->completionQueue());
        } else if (asyncIoLatencyFrames_ > 0) {
            DeferredAsyncIoPacket pkt;
            pkt.remainingFrames = asyncIoLatencyFrames_;
            pkt.requestId = reqId;
            pkt.token = token;
            pkt.kind = DeferredAsyncIoPacket::Kind::ScalarBool;
            pkt.boolValue = true;
            deferredAsyncIoPackets_.push_back(std::move(pkt));
        } else {
            ctx->completionQueue().submitScalar(reqId, runtime::Value(true));
        }
    }

    return outerHandle;
}

void SceneScriptRuntime::setSaveGameSlot(std::string slotKey, std::string payload) {
    if (slotKey.empty()) return;
    std::lock_guard<std::mutex> lock(asyncIoMutex_);
    saveGameSlots_.insert_or_assign(std::move(slotKey), std::move(payload));
}

std::optional<std::string> SceneScriptRuntime::getSaveGameSlot(
    const std::string& slotKey) const {
    std::lock_guard<std::mutex> lock(asyncIoMutex_);
    const auto found = saveGameSlots_.find(slotKey);
    if (found == saveGameSlots_.end()) return std::nullopt;
    return found->second;
}

bool SceneScriptRuntime::eraseSaveGameSlot(const std::string& slotKey) {
    std::lock_guard<std::mutex> lock(asyncIoMutex_);
    return saveGameSlots_.erase(slotKey) != 0;
}

void SceneScriptRuntime::setSaveWriteWorker(AsyncSaveWriteWorker worker) {
    std::lock_guard<std::mutex> lock(asyncIoMutex_);
    saveWriteWorker_ = std::move(worker);
}

void SceneScriptRuntime::setSaveReadWorker(AsyncSaveReadWorker worker) {
    std::lock_guard<std::mutex> lock(asyncIoMutex_);
    saveReadWorker_ = std::move(worker);
}

runtime::AwaitableHandle SceneScriptRuntime::saveGameAsync(
    std::string slotKey,
    std::string payload,
    runtime::CancellationToken token) {
    const auto ctx = runtime_.asyncRuntime();
    if (!ctx) return runtime::AwaitableHandle::invalid();
    auto& pool = ctx->awaitables();

    if (ctx->cancellations().isCancellationRequested(token)) {
        return pool.fromCanceled();
    }
    if (slotKey.empty()) {
        return pool.fromFaulted(
            runtime::ErrorCode::InvalidArguments,
            "save slot key must not be empty");
    }

    const auto target = pool.allocatePending(runtime::ResumeQueue::Update, token);
    const auto reqId = ctx->completionQueue().registerRequest(
        target,
        "Scene.SaveGameAsync",
        hashAsyncBridgeArgs(slotKey, payload, 0),
        runtime::BindingDeterminism::Recordable,
        token);

    const bool isReplay =
        determinismSession_ &&
        determinismSession_->mode() == runtime::DeterminismMode::Replay;
    if (!isReplay) {
        AsyncSaveWriteWorker worker;
        {
            std::lock_guard<std::mutex> lock(asyncIoMutex_);
            worker = saveWriteWorker_;
        }
        if (worker) {
            worker(slotKey, payload, reqId, token, ctx->completionQueue());
        } else if (asyncIoLatencyFrames_ > 0) {
            DeferredAsyncIoPacket pkt;
            pkt.remainingFrames = asyncIoLatencyFrames_;
            pkt.requestId = reqId;
            pkt.targetHandle = target;
            pkt.token = token;
            pkt.kind = DeferredAsyncIoPacket::Kind::SaveCommit;
            pkt.key = std::move(slotKey);
            pkt.textPayload = std::move(payload);
            deferredAsyncIoPackets_.push_back(std::move(pkt));
        } else {
            {
                std::lock_guard<std::mutex> lock(asyncIoMutex_);
                saveGameSlots_.insert_or_assign(std::move(slotKey), std::move(payload));
            }
            ctx->completionQueue().submitScalar(reqId, runtime::Value(true));
        }
    }

    return target;
}

runtime::AwaitableHandle SceneScriptRuntime::loadGameAsync(
    std::string slotKey,
    runtime::CancellationToken token) {
    const auto ctx = runtime_.asyncRuntime();
    if (!ctx) return runtime::AwaitableHandle::invalid();
    auto& pool = ctx->awaitables();

    if (ctx->cancellations().isCancellationRequested(token)) {
        return pool.fromCanceled();
    }
    if (slotKey.empty()) {
        return pool.fromFaulted(
            runtime::ErrorCode::InvalidArguments,
            "save slot key must not be empty");
    }

    const auto target = pool.allocatePending(runtime::ResumeQueue::Update, token);
    const auto reqId = ctx->completionQueue().registerRequest(
        target,
        "Scene.LoadGameAsync",
        hashAsyncBridgeArgs(slotKey, {}, 0),
        runtime::BindingDeterminism::Recordable,
        token);

    const bool isReplay =
        determinismSession_ &&
        determinismSession_->mode() == runtime::DeterminismMode::Replay;
    if (!isReplay) {
        AsyncSaveReadWorker worker;
        std::optional<std::string> stored;
        {
            std::lock_guard<std::mutex> lock(asyncIoMutex_);
            worker = saveReadWorker_;
            if (!worker) {
                const auto found = saveGameSlots_.find(slotKey);
                if (found != saveGameSlots_.end()) {
                    stored = found->second;
                }
            }
        }
        if (worker) {
            worker(slotKey, reqId, token, ctx->completionQueue());
        } else if (asyncIoLatencyFrames_ > 0) {
            DeferredAsyncIoPacket pkt;
            pkt.remainingFrames = asyncIoLatencyFrames_;
            pkt.requestId = reqId;
            pkt.targetHandle = target;
            pkt.token = token;
            pkt.kind = DeferredAsyncIoPacket::Kind::SaveRead;
            pkt.key = std::move(slotKey);
            deferredAsyncIoPackets_.push_back(std::move(pkt));
        } else if (stored) {
            ctx->completionQueue().submitString(reqId, std::move(*stored));
        } else {
            ctx->completionQueue().submitFailure(
                reqId,
                runtime::ErrorCode::InvalidArguments,
                "save slot '" + slotKey + "' does not exist");
        }
    }

    return target;
}

// ============================================================================
// Stage 4: Timeout-Linked HTTP & WebSocket Awaitable Bridges
// ============================================================================

std::size_t SceneScriptRuntime::advanceAsyncTimeouts(double deltaSeconds) {
    const auto ctx = runtime_.asyncRuntime();
    if (!ctx) return 0;
    return ctx->advanceTimeouts(deltaSeconds);
}

runtime::CancellationToken SceneScriptRuntime::buildScopedNetToken(
    SceneEntityId optionalOwnerEntity,
    runtime::CancellationToken callerToken,
    double timeoutSeconds,
    runtime::CancellationSourceHandle& outOwnedCts) {
    outOwnedCts = {};
    const auto ctx = runtime_.asyncRuntime();
    if (!ctx) return runtime::CancellationToken::preCanceled();

    std::array<runtime::CancellationToken, 2> parents{};
    std::size_t parentCount = 0;
    if (callerToken.canBeCanceled()) {
        parents[parentCount++] = callerToken;
    }
    if (optionalOwnerEntity != BroadcastTarget) {
        const auto entTok = entityDestroyToken(optionalOwnerEntity);
        if (entTok.canBeCanceled()) {
            parents[parentCount++] = entTok;
        }
    }

    if (std::isnan(timeoutSeconds) ||
        (std::isfinite(timeoutSeconds) && timeoutSeconds > 0.0)) {
        if (parentCount > 0) {
            outOwnedCts = ctx->createLinkedTimeoutSource(
                parents.data(), parentCount, timeoutSeconds, false);
        } else {
            outOwnedCts = ctx->createTimeoutSource(timeoutSeconds, false);
        }
        return ctx->cancellations().token(outOwnedCts);
    }

    if (parentCount == 0) {
        return runtime::CancellationToken::none();
    }
    if (parentCount == 1) {
        return parents[0];
    }
    outOwnedCts = ctx->cancellations().createLinkedSource(parents.data(), parentCount);
    return ctx->cancellations().token(outOwnedCts);
}

std::string SceneScriptRuntime::encodeHttpWirePacket(
    int statusCode,
    std::string_view responseBody) {
    std::string out = std::to_string(statusCode);
    out.push_back('\n');
    out.append(responseBody.data(), responseBody.size());
    return out;
}

bool SceneScriptRuntime::decodeHttpWirePacket(
    std::string_view packet,
    int& outStatusCode,
    std::string& outBody) {
    const auto nl = packet.find('\n');
    if (nl == std::string_view::npos || nl == 0) return false;
    int code = 0;
    for (std::size_t i = 0; i < nl; ++i) {
        const char ch = packet[i];
        if (ch < '0' || ch > '9') return false;
        code = code * 10 + (ch - '0');
    }
    outStatusCode = code;
    outBody.assign(packet.substr(nl + 1));
    return true;
}

void SceneScriptRuntime::registerHttpEndpoint(
    std::string method,
    std::string url,
    int statusCode,
    std::string responseBody,
    std::uint32_t latencyFrames) {
    if (url.empty()) return;
    const std::string key = normalizeHttpKey(method, url);
    std::lock_guard<std::mutex> lock(asyncIoMutex_);
    httpEndpoints_.insert_or_assign(
        key,
        HttpMockEndpoint{statusCode, std::move(responseBody), latencyFrames});
}

bool SceneScriptRuntime::unregisterHttpEndpoint(
    const std::string& method,
    const std::string& url) {
    const std::string key = normalizeHttpKey(method, url);
    std::lock_guard<std::mutex> lock(asyncIoMutex_);
    return httpEndpoints_.erase(key) != 0;
}

void SceneScriptRuntime::setHttpWorker(AsyncHttpWorker worker) {
    std::lock_guard<std::mutex> lock(asyncIoMutex_);
    httpWorker_ = std::move(worker);
}

void SceneScriptRuntime::onAsyncHttpIoCompleted(
    void* context,
    runtime::AwaitableHandle ioHandle,
    runtime::AwaitStatus,
    std::uint64_t opId) {
    auto* self = static_cast<SceneScriptRuntime*>(context);
    if (!self) return;
    const auto ctx = self->runtime_.asyncRuntime();
    if (!ctx) return;

    auto& pool = ctx->awaitables();
    runtime::Value ioValue{};
    runtime::RuntimeError ioError{};
    const auto terminal = pool.getResult(ioHandle, ioValue, ioError);

    const auto opIt = self->pendingHttpOps_.find(opId);
    if (opIt == self->pendingHttpOps_.end()) return;
    PendingHttpMaterializeOp op = std::move(opIt->second);
    self->pendingHttpOps_.erase(opIt);

    const bool timedOut = ctx->cancellations().isTimedOut(op.effectiveToken);
    self->lastHttpTimedOut_ = timedOut;
    if (op.ownedLinkedCts.valid()) {
        (void)ctx->cancellations().releaseSource(op.ownedLinkedCts);
    }

    if (!pool.isValid(op.outerHandle)) {
        return;
    }

    if (terminal == runtime::AwaitStatus::Canceled ||
        ctx->cancellations().isCancellationRequested(op.effectiveToken)) {
        self->lastHttpStatusCode_ = timedOut ? 408 : 0;
        self->lastHttpResponseBody_.clear();
        (void)pool.trySetCanceled(op.outerHandle);
        return;
    }

    if (terminal == runtime::AwaitStatus::Faulted) {
        (void)pool.trySetFaulted(
            op.outerHandle,
            ioError.code == runtime::ErrorCode::None
                ? runtime::ErrorCode::ScriptException
                : ioError.code,
            ioError.message.empty() ? "HTTP async request failed" : std::move(ioError.message));
        return;
    }

    std::string rawWire;
    if (const auto* sv = std::get_if<std::string>(&ioValue)) {
        rawWire = *sv;
    } else if (const auto* ref = std::get_if<runtime::ObjectRef>(&ioValue);
               ref && self->runtime_.heap()) {
        if (const auto view = self->runtime_.heap()->stringView(*ref)) {
            rawWire.assign(view->data(), view->size());
        }
    }

    int statusCode = 0;
    std::string body;
    if (!decodeHttpWirePacket(rawWire, statusCode, body)) {
        (void)pool.trySetFaulted(
            op.outerHandle,
            runtime::ErrorCode::InvalidArguments,
            "malformed HTTP wire packet");
        return;
    }

    self->lastHttpStatusCode_ = statusCode;
    self->lastHttpResponseBody_ = body;

    if (op.returnStatusOnly) {
        (void)pool.trySetResult(op.outerHandle, runtime::Value(static_cast<std::int64_t>(statusCode)));
        return;
    }

    if (statusCode < 200 || statusCode >= 400) {
        (void)pool.trySetFaulted(
            op.outerHandle,
            runtime::ErrorCode::ScriptException,
            "HTTP " + std::to_string(statusCode) + ": " + body);
        return;
    }

    if (auto* heap = self->runtime_.heap().get()) {
        runtime::RuntimeError allocErr;
        const auto strRef = heap->allocateString(std::move(body), &allocErr);
        if (!strRef) {
            (void)pool.trySetFaulted(
                op.outerHandle,
                runtime::ErrorCode::OutOfMemory,
                allocErr.message);
            return;
        }
        (void)pool.trySetResult(op.outerHandle, runtime::Value(*strRef));
    } else {
        (void)pool.trySetResult(op.outerHandle, runtime::Value(std::move(body)));
    }
}

runtime::AwaitableHandle SceneScriptRuntime::httpRequestAsync(
    HttpRequestSpec request,
    bool returnStatusOnly,
    runtime::CancellationToken callerToken) {
    const auto ctx = runtime_.asyncRuntime();
    if (!ctx) return runtime::AwaitableHandle::invalid();
    auto& pool = ctx->awaitables();

    if (request.url.empty()) {
        return pool.fromFaulted(
            runtime::ErrorCode::InvalidArguments,
            "HTTP request URL must not be empty");
    }

    auto schedulePreCanceledHttp = [&](runtime::CancellationToken tok,
                                       runtime::CancellationSourceHandle ownedCts) {
        const auto ioHandle = pool.allocatePending(runtime::ResumeQueue::Update);
        const auto outerHandle = pool.allocatePending(runtime::ResumeQueue::Update);
        const std::uint64_t opId = nextMaterializeOpId_++;
        pendingHttpOps_.emplace(opId, PendingHttpMaterializeOp{
            returnStatusOnly,
            tok,
            ownedCts,
            ioHandle,
            outerHandle,
        });
        runtime::AsyncContinuation cont{};
        cont.callback = &SceneScriptRuntime::onAsyncHttpIoCompleted;
        cont.context = this;
        cont.userTag = opId;
        cont.resumeQueue = runtime::ResumeQueue::Update;
        cont.runInlineOnComplete = false;
        (void)pool.onCompleted(ioHandle, cont);
        (void)pool.trySetCanceled(ioHandle);
        return outerHandle;
    };

    if (ctx->cancellations().isCancellationRequested(callerToken)) {
        return schedulePreCanceledHttp(callerToken, {});
    }

    runtime::CancellationSourceHandle ownedCts{};
    const auto effectiveToken = buildScopedNetToken(
        BroadcastTarget, callerToken, request.timeoutSeconds, ownedCts);
    if (ctx->cancellations().isCancellationRequested(effectiveToken)) {
        return schedulePreCanceledHttp(effectiveToken, ownedCts);
    }

    const auto ioHandle = pool.allocatePending(runtime::ResumeQueue::PreTickIo, effectiveToken);
    const auto outerHandle = pool.allocatePending(runtime::ResumeQueue::Update, effectiveToken);
    const std::uint64_t opId = nextMaterializeOpId_++;
    pendingHttpOps_.emplace(opId, PendingHttpMaterializeOp{
        returnStatusOnly,
        effectiveToken,
        ownedCts,
        ioHandle,
        outerHandle,
    });

    runtime::AsyncContinuation cont{};
    cont.callback = &SceneScriptRuntime::onAsyncHttpIoCompleted;
    cont.context = this;
    cont.userTag = opId;
    cont.resumeQueue = runtime::ResumeQueue::PreTickIo;
    cont.runInlineOnComplete = true;
    (void)pool.onCompleted(ioHandle, cont);

    const std::string key = normalizeHttpKey(request.method, request.url);
    const auto reqId = ctx->completionQueue().registerRequest(
        ioHandle,
        "Scene.HttpRequestAsync",
        hashAsyncBridgeArgs(key, request.body, returnStatusOnly ? 1u : 0u),
        runtime::BindingDeterminism::Recordable,
        effectiveToken);

    const bool isReplay =
        determinismSession_ &&
        determinismSession_->mode() == runtime::DeterminismMode::Replay;
    if (!isReplay) {
        AsyncHttpWorker worker;
        std::optional<HttpMockEndpoint> mockRoute;
        {
            std::lock_guard<std::mutex> lock(asyncIoMutex_);
            const auto found = httpEndpoints_.find(key);
            if (found != httpEndpoints_.end()) {
                mockRoute = found->second;
            } else {
                worker = httpWorker_;
            }
        }
        if (mockRoute) {
            const std::uint32_t waitFrames =
                mockRoute->latencyFrames > 0 ? mockRoute->latencyFrames : asyncIoLatencyFrames_;
            std::string wire = encodeHttpWirePacket(mockRoute->statusCode, mockRoute->responseBody);
            if (waitFrames > 0) {
                DeferredAsyncIoPacket pkt;
                pkt.remainingFrames = waitFrames;
                pkt.requestId = reqId;
                pkt.targetHandle = ioHandle;
                pkt.token = effectiveToken;
                pkt.kind = DeferredAsyncIoPacket::Kind::String;
                pkt.textPayload = std::move(wire);
                deferredAsyncIoPackets_.push_back(std::move(pkt));
            } else {
                ctx->completionQueue().submitString(reqId, std::move(wire));
            }
        } else if (worker) {
            worker(request, reqId, effectiveToken, ctx->completionQueue());
        } else {
            ctx->completionQueue().submitFailure(
                reqId,
                runtime::ErrorCode::InvalidArguments,
                "unregistered HTTP endpoint '" + key + "'");
        }
    }

    return outerHandle;
}

void SceneScriptRuntime::registerWebSocketEndpoint(
    std::string url,
    bool autoEcho,
    std::uint32_t connectLatencyFrames) {
    if (url.empty()) return;
    std::lock_guard<std::mutex> lock(asyncIoMutex_);
    wsEndpoints_.insert_or_assign(
        std::move(url),
        WebSocketEndpointConfig{autoEcho, connectLatencyFrames});
}

bool SceneScriptRuntime::unregisterWebSocketEndpoint(const std::string& url) {
    std::lock_guard<std::mutex> lock(asyncIoMutex_);
    return wsEndpoints_.erase(url) != 0;
}

void SceneScriptRuntime::setWebSocketConnectWorker(AsyncWebSocketConnectWorker worker) {
    std::lock_guard<std::mutex> lock(asyncIoMutex_);
    wsConnectWorker_ = std::move(worker);
}

void SceneScriptRuntime::setWebSocketSendWorker(AsyncWebSocketSendWorker worker) {
    std::lock_guard<std::mutex> lock(asyncIoMutex_);
    wsSendWorker_ = std::move(worker);
}

bool SceneScriptRuntime::pushWebSocketInboundMessage(
    WebSocketHandle wsHandle,
    std::string message) {
    if (wsHandle <= 0) return false;
    const auto ctx = runtime_.asyncRuntime();
    if (!ctx) return false;

    std::uint32_t slotIdx = 0;
    std::uint32_t gen = 0;
    unpackWsHandle(wsHandle, slotIdx, gen);

    runtime::AsyncRequestId matchedReqId = 0;
    {
        std::lock_guard<std::mutex> lock(asyncIoMutex_);
        if (slotIdx >= wsSessions_.size()) return false;
        auto& sess = wsSessions_[slotIdx];
        if (!sess.allocated || sess.generation != gen || sess.closed) {
            return false;
        }
        while (!sess.pendingReceivers.empty()) {
            const auto waiter = sess.pendingReceivers.front();
            sess.pendingReceivers.pop_front();
            if (ctx->cancellations().isCancellationRequested(waiter.token)) {
                continue;
            }
            matchedReqId = waiter.requestId;
            break;
        }
        if (matchedReqId == 0) {
            sess.inboundQueue.push_back(std::move(message));
            return true;
        }
    }

    ctx->completionQueue().submitString(matchedReqId, std::move(message));
    return true;
}

bool SceneScriptRuntime::closeWebSocketFromHost(
    WebSocketHandle wsHandle,
    std::string reason) {
    if (wsHandle <= 0) return false;
    const auto ctx = runtime_.asyncRuntime();
    std::uint32_t slotIdx = 0;
    std::uint32_t gen = 0;
    unpackWsHandle(wsHandle, slotIdx, gen);

    std::vector<runtime::AsyncRequestId> toFailOrCancel;
    std::string errText = reason.empty() ? "WebSocket closed" : std::move(reason);
    {
        std::lock_guard<std::mutex> lock(asyncIoMutex_);
        if (slotIdx >= wsSessions_.size()) return false;
        auto& sess = wsSessions_[slotIdx];
        if (!sess.allocated || sess.generation != gen || sess.closed) {
            return false;
        }
        sess.connected = false;
        sess.closed = true;
        sess.closeReason = errText;
        while (!sess.pendingReceivers.empty()) {
            toFailOrCancel.push_back(sess.pendingReceivers.front().requestId);
            sess.pendingReceivers.pop_front();
        }
        sess.inboundQueue.clear();
        sess.allocated = false;
        sess.generation = (sess.generation + 1u == 0u) ? 1u : (sess.generation + 1u);
        freeWsSessionSlots_.push_back(slotIdx);
    }

    if (ctx) {
        for (const auto reqId : toFailOrCancel) {
            ctx->completionQueue().submitCanceled(reqId);
        }
    }
    return true;
}

void SceneScriptRuntime::closeEntityOwnedWebSockets(SceneEntityId entity) {
    if (entity == BroadcastTarget) return;
    std::vector<WebSocketHandle> ownedHandles;
    {
        std::lock_guard<std::mutex> lock(asyncIoMutex_);
        for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(wsSessions_.size()); ++i) {
            const auto& sess = wsSessions_[i];
            if (sess.allocated && !sess.closed && sess.ownerEntity == entity) {
                ownedHandles.push_back(static_cast<WebSocketHandle>(packWsHandle(i, sess.generation)));
            }
        }
    }
    for (const auto h : ownedHandles) {
        (void)closeWebSocketFromHost(h, "owner entity destroyed");
    }
}

bool SceneScriptRuntime::isWebSocketConnected(WebSocketHandle wsHandle) const noexcept {
    if (wsHandle <= 0) return false;
    std::uint32_t slotIdx = 0;
    std::uint32_t gen = 0;
    unpackWsHandle(wsHandle, slotIdx, gen);
    std::lock_guard<std::mutex> lock(asyncIoMutex_);
    if (slotIdx >= wsSessions_.size()) return false;
    const auto& sess = wsSessions_[slotIdx];
    return sess.allocated && sess.generation == gen && sess.connected && !sess.closed;
}

std::vector<std::string> SceneScriptRuntime::getWebSocketSentMessages(
    WebSocketHandle wsHandle) const {
    if (wsHandle <= 0) return {};
    std::uint32_t slotIdx = 0;
    std::uint32_t gen = 0;
    unpackWsHandle(wsHandle, slotIdx, gen);
    std::lock_guard<std::mutex> lock(asyncIoMutex_);
    if (slotIdx >= wsSessions_.size()) return {};
    const auto& sess = wsSessions_[slotIdx];
    if (!sess.allocated || sess.generation != gen) return {};
    return sess.sentMessages;
}

void SceneScriptRuntime::onAsyncWsConnectIoCompleted(
    void* context,
    runtime::AwaitableHandle ioHandle,
    runtime::AwaitStatus,
    std::uint64_t opId) {
    auto* self = static_cast<SceneScriptRuntime*>(context);
    if (!self) return;
    const auto ctx = self->runtime_.asyncRuntime();
    if (!ctx) return;

    auto& pool = ctx->awaitables();
    runtime::Value ioValue{};
    runtime::RuntimeError ioError{};
    const auto terminal = pool.getResult(ioHandle, ioValue, ioError);

    const auto opIt = self->pendingWsConnectOps_.find(opId);
    if (opIt == self->pendingWsConnectOps_.end()) return;
    PendingWsConnectOp op = std::move(opIt->second);
    self->pendingWsConnectOps_.erase(opIt);

    const bool canceled =
        terminal == runtime::AwaitStatus::Canceled ||
        ctx->cancellations().isCancellationRequested(op.effectiveToken);
    if (op.ownedLinkedCts.valid()) {
        (void)ctx->cancellations().releaseSource(op.ownedLinkedCts);
    }

    if (canceled) {
        (void)self->closeWebSocketFromHost(op.wsHandle, "connect canceled");
        if (pool.isValid(op.outerHandle)) {
            (void)pool.trySetCanceled(op.outerHandle);
        }
        return;
    }

    if (terminal == runtime::AwaitStatus::Faulted) {
        (void)self->closeWebSocketFromHost(op.wsHandle, ioError.message);
        if (pool.isValid(op.outerHandle)) {
            (void)pool.trySetFaulted(
                op.outerHandle,
                ioError.code == runtime::ErrorCode::None
                    ? runtime::ErrorCode::ScriptException
                    : ioError.code,
                ioError.message.empty() ? "WebSocket connect failed" : std::move(ioError.message));
        }
        return;
    }

    std::uint32_t slotIdx = 0;
    std::uint32_t gen = 0;
    unpackWsHandle(op.wsHandle, slotIdx, gen);
    bool alive = false;
    {
        std::lock_guard<std::mutex> lock(self->asyncIoMutex_);
        if (slotIdx < self->wsSessions_.size()) {
            auto& sess = self->wsSessions_[slotIdx];
            if (sess.allocated && sess.generation == gen && !sess.closed) {
                if (sess.ownerEntity != BroadcastTarget && !self->isEntityAlive(sess.ownerEntity)) {
                    alive = false;
                } else {
                    sess.connected = true;
                    alive = true;
                }
            }
        }
    }
    if (!alive) {
        (void)self->closeWebSocketFromHost(op.wsHandle, "owner dead");
        if (pool.isValid(op.outerHandle)) {
            (void)pool.trySetCanceled(op.outerHandle);
        }
        return;
    }

    if (pool.isValid(op.outerHandle)) {
        (void)pool.trySetResult(
            op.outerHandle,
            runtime::Value{runtime::LongValue{op.wsHandle}});
    }
}

void SceneScriptRuntime::onAsyncNetPassthroughCompleted(
    void* context,
    runtime::AwaitableHandle ioHandle,
    runtime::AwaitStatus,
    std::uint64_t opId) {
    auto* self = static_cast<SceneScriptRuntime*>(context);
    if (!self) return;
    const auto ctx = self->runtime_.asyncRuntime();
    if (!ctx) return;

    auto& pool = ctx->awaitables();
    runtime::Value ioValue{};
    runtime::RuntimeError ioError{};
    const auto terminal = pool.getResult(ioHandle, ioValue, ioError);

    const auto opIt = self->pendingNetTokenOps_.find(opId);
    if (opIt == self->pendingNetTokenOps_.end()) return;
    PendingNetTokenCleanupOp op = std::move(opIt->second);
    self->pendingNetTokenOps_.erase(opIt);

    if (op.ownedLinkedCts.valid()) {
        (void)ctx->cancellations().releaseSource(op.ownedLinkedCts);
    }

    if (op.isReceiveOp && terminal != runtime::AwaitStatus::Succeeded && op.wsHandle > 0) {
        if (op.requestId != 0) {
            (void)ctx->completionQueue().cancelRequest(op.requestId);
        }
        std::uint32_t slotIdx = 0;
        std::uint32_t gen = 0;
        unpackWsHandle(op.wsHandle, slotIdx, gen);
        std::lock_guard<std::mutex> lock(self->asyncIoMutex_);
        if (slotIdx < self->wsSessions_.size()) {
            auto& sess = self->wsSessions_[slotIdx];
            if (sess.allocated && sess.generation == gen) {
                sess.pendingReceivers.erase(
                    std::remove_if(
                        sess.pendingReceivers.begin(),
                        sess.pendingReceivers.end(),
                        [&](const PendingWsReceiveWaiter& w) {
                            return w.requestId == op.requestId ||
                                ctx->cancellations().isCancellationRequested(w.token);
                        }),
                    sess.pendingReceivers.end());
            }
        }
    }

    if (op.recordSentOnSuccess && terminal == runtime::AwaitStatus::Succeeded && op.wsHandle > 0) {
        const auto* boolVal = std::get_if<bool>(&ioValue);
        if (!boolVal || *boolVal) {
            std::uint32_t slotIdx = 0;
            std::uint32_t gen = 0;
            unpackWsHandle(op.wsHandle, slotIdx, gen);
            bool openOk = false;
            {
                std::lock_guard<std::mutex> lock(self->asyncIoMutex_);
                if (slotIdx < self->wsSessions_.size()) {
                    auto& sess = self->wsSessions_[slotIdx];
                    if (sess.allocated && sess.generation == gen &&
                        sess.connected && !sess.closed) {
                        sess.sentMessages.push_back(op.sentPayload);
                        openOk = true;
                    }
                }
            }
            if (!openOk) {
                if (pool.isValid(op.outerHandle)) {
                    (void)pool.trySetFaulted(
                        op.outerHandle,
                        runtime::ErrorCode::ScriptException,
                        "WebSocket closed before send completed");
                }
                return;
            }
        }
    }

    if (!pool.isValid(op.outerHandle)) {
        return;
    }
    if (terminal == runtime::AwaitStatus::Canceled) {
        (void)pool.trySetCanceled(op.outerHandle);
    } else if (terminal == runtime::AwaitStatus::Faulted) {
        (void)pool.trySetFaulted(
            op.outerHandle,
            ioError.code == runtime::ErrorCode::None
                ? runtime::ErrorCode::ScriptException
                : ioError.code,
            std::move(ioError.message));
    } else {
        (void)pool.trySetResult(op.outerHandle, std::move(ioValue));
    }
}

runtime::AwaitableHandle SceneScriptRuntime::wsConnectAsync(
    std::string url,
    SceneEntityId ownerEntity,
    double timeoutSeconds,
    runtime::CancellationToken callerToken) {
    const auto ctx = runtime_.asyncRuntime();
    if (!ctx) return runtime::AwaitableHandle::invalid();
    auto& pool = ctx->awaitables();

    if (url.empty()) {
        return pool.fromFaulted(
            runtime::ErrorCode::InvalidArguments,
            "WebSocket URL must not be empty");
    }
    if (ownerEntity != BroadcastTarget && !isEntityAlive(ownerEntity)) {
        return pool.fromCanceled();
    }

    runtime::CancellationSourceHandle ownedCts{};
    const auto effectiveToken = buildScopedNetToken(
        ownerEntity, callerToken, timeoutSeconds, ownedCts);
    if (ctx->cancellations().isCancellationRequested(effectiveToken)) {
        if (ownedCts.valid()) (void)ctx->cancellations().releaseSource(ownedCts);
        return pool.fromCanceled();
    }

    WebSocketHandle wsHandle = 0;
    AsyncWebSocketConnectWorker worker;
    std::optional<WebSocketEndpointConfig> endpointCfg;
    {
        std::lock_guard<std::mutex> lock(asyncIoMutex_);
        worker = wsConnectWorker_;
        const auto cfgIt = wsEndpoints_.find(url);
        if (cfgIt != wsEndpoints_.end()) {
            endpointCfg = cfgIt->second;
        }
        std::uint32_t slotIdx = 0;
        if (!freeWsSessionSlots_.empty()) {
            slotIdx = freeWsSessionSlots_.back();
            freeWsSessionSlots_.pop_back();
        } else {
            slotIdx = static_cast<std::uint32_t>(wsSessions_.size());
            wsSessions_.emplace_back();
        }
        auto& sess = wsSessions_[slotIdx];
        if (sess.generation == 0u || sess.generation >= 0x7FFFFFFFu) {
            sess.generation = 1u;
        }
        sess.allocated = true;
        sess.connected = false;
        sess.closed = false;
        sess.autoEcho = endpointCfg ? endpointCfg->autoEcho : false;
        sess.url = url;
        sess.closeReason.clear();
        sess.ownerEntity = ownerEntity;
        sess.nextReceiveSeq = 1;
        sess.nextSendSeq = 1;
        sess.inboundQueue.clear();
        sess.sentMessages.clear();
        sess.pendingReceivers.clear();
        wsHandle = static_cast<WebSocketHandle>(packWsHandle(slotIdx, sess.generation));
    }

    const auto ioHandle = pool.allocatePending(runtime::ResumeQueue::PreTickIo, effectiveToken);
    const auto outerHandle = pool.allocatePending(runtime::ResumeQueue::Update, effectiveToken);
    const std::uint64_t opId = nextMaterializeOpId_++;
    pendingWsConnectOps_.emplace(opId, PendingWsConnectOp{
        wsHandle,
        effectiveToken,
        ownedCts,
        ioHandle,
        outerHandle,
    });

    runtime::AsyncContinuation cont{};
    cont.callback = &SceneScriptRuntime::onAsyncWsConnectIoCompleted;
    cont.context = this;
    cont.userTag = opId;
    cont.resumeQueue = runtime::ResumeQueue::PreTickIo;
    cont.runInlineOnComplete = true;
    (void)pool.onCompleted(ioHandle, cont);

    const auto reqId = ctx->completionQueue().registerRequest(
        ioHandle,
        "Scene.WsConnectAsync",
        hashAsyncBridgeArgs(url, {}, ownerEntity),
        runtime::BindingDeterminism::Recordable,
        effectiveToken);

    const bool isReplay =
        determinismSession_ &&
        determinismSession_->mode() == runtime::DeterminismMode::Replay;
    if (!isReplay) {
        if (worker) {
            worker(wsHandle, url, reqId, effectiveToken, ctx->completionQueue());
        } else if (endpointCfg) {
            const std::uint32_t waitFrames =
                endpointCfg->connectLatencyFrames > 0
                    ? endpointCfg->connectLatencyFrames
                    : asyncIoLatencyFrames_;
            if (waitFrames > 0) {
                DeferredAsyncIoPacket pkt;
                pkt.remainingFrames = waitFrames;
                pkt.requestId = reqId;
                pkt.targetHandle = ioHandle;
                pkt.token = effectiveToken;
                pkt.kind = DeferredAsyncIoPacket::Kind::ScalarBool;
                pkt.boolValue = true;
                deferredAsyncIoPackets_.push_back(std::move(pkt));
            } else {
                ctx->completionQueue().submitScalar(reqId, runtime::Value(true));
            }
        } else {
            ctx->completionQueue().submitFailure(
                reqId,
                runtime::ErrorCode::InvalidArguments,
                "unregistered WebSocket endpoint '" + url + "'");
        }
    }

    return outerHandle;
}

runtime::AwaitableHandle SceneScriptRuntime::wsSendAsync(
    WebSocketHandle wsHandle,
    std::string payload,
    double timeoutSeconds,
    runtime::CancellationToken callerToken) {
    const auto ctx = runtime_.asyncRuntime();
    if (!ctx) return runtime::AwaitableHandle::invalid();
    auto& pool = ctx->awaitables();

    if (wsHandle <= 0) {
        return pool.fromFaulted(
            runtime::ErrorCode::InvalidArguments,
            "invalid WebSocket handle");
    }

    std::uint32_t slotIdx = 0;
    std::uint32_t gen = 0;
    unpackWsHandle(wsHandle, slotIdx, gen);

    SceneEntityId ownerEntity = 0;
    bool autoEcho = false;
    std::uint64_t sendSeq = 0;
    AsyncWebSocketSendWorker worker;
    {
        std::lock_guard<std::mutex> lock(asyncIoMutex_);
        if (slotIdx >= wsSessions_.size()) {
            return pool.fromFaulted(runtime::ErrorCode::InvalidArguments, "invalid WebSocket slot");
        }
        auto& sess = wsSessions_[slotIdx];
        if (!sess.allocated || sess.generation != gen || !sess.connected || sess.closed) {
            return pool.fromFaulted(runtime::ErrorCode::ScriptException, "WebSocket is not connected");
        }
        ownerEntity = sess.ownerEntity;
        autoEcho = sess.autoEcho;
        sendSeq = sess.nextSendSeq++;
        worker = wsSendWorker_;
    }

    runtime::CancellationSourceHandle ownedCts{};
    const auto effectiveToken = buildScopedNetToken(
        ownerEntity, callerToken, timeoutSeconds, ownedCts);
    if (ctx->cancellations().isCancellationRequested(effectiveToken)) {
        if (ownedCts.valid()) (void)ctx->cancellations().releaseSource(ownedCts);
        return pool.fromCanceled();
    }

    const bool isReplay =
        determinismSession_ &&
        determinismSession_->mode() == runtime::DeterminismMode::Replay;
    const bool deferRecordToCompletion = isReplay || static_cast<bool>(worker);

    const auto ioHandle = pool.allocatePending(runtime::ResumeQueue::PreTickIo, effectiveToken);
    const auto outerHandle = pool.allocatePending(runtime::ResumeQueue::Update, effectiveToken);
    const std::uint64_t opId = nextMaterializeOpId_++;
    pendingNetTokenOps_.emplace(opId, PendingNetTokenCleanupOp{
        ownedCts,
        ioHandle,
        outerHandle,
        wsHandle,
        0,
        false,
        deferRecordToCompletion,
        deferRecordToCompletion ? payload : std::string{},
    });

    runtime::AsyncContinuation cont{};
    cont.callback = &SceneScriptRuntime::onAsyncNetPassthroughCompleted;
    cont.context = this;
    cont.userTag = opId;
    cont.resumeQueue = runtime::ResumeQueue::PreTickIo;
    cont.runInlineOnComplete = true;
    (void)pool.onCompleted(ioHandle, cont);

    const auto reqId = ctx->completionQueue().registerRequest(
        ioHandle,
        "Scene.WsSendAsync",
        hashAsyncBridgeArgs(payload, {}, sendSeq),
        runtime::BindingDeterminism::Recordable,
        effectiveToken);

    if (!isReplay) {
        if (worker) {
            worker(wsHandle, payload, reqId, effectiveToken, ctx->completionQueue());
        } else if (asyncIoLatencyFrames_ > 0) {
            DeferredAsyncIoPacket pkt;
            pkt.remainingFrames = asyncIoLatencyFrames_;
            pkt.requestId = reqId;
            pkt.targetHandle = ioHandle;
            pkt.token = effectiveToken;
            pkt.kind = DeferredAsyncIoPacket::Kind::WsMockSend;
            pkt.wsHandle = wsHandle;
            pkt.textPayload = std::move(payload);
            deferredAsyncIoPackets_.push_back(std::move(pkt));
        } else {
            bool sentOk = false;
            {
                std::lock_guard<std::mutex> lock(asyncIoMutex_);
                if (slotIdx < wsSessions_.size()) {
                    auto& sess = wsSessions_[slotIdx];
                    if (sess.allocated && sess.generation == gen &&
                        sess.connected && !sess.closed) {
                        sess.sentMessages.push_back(payload);
                        sentOk = true;
                    }
                }
            }
            if (sentOk) {
                ctx->completionQueue().submitScalar(reqId, runtime::Value(true));
                if (autoEcho) {
                    (void)pushWebSocketInboundMessage(wsHandle, "echo:" + payload);
                }
            } else {
                ctx->completionQueue().submitFailure(
                    reqId,
                    runtime::ErrorCode::ScriptException,
                    "WebSocket is not connected");
            }
        }
    }

    return outerHandle;
}

runtime::AwaitableHandle SceneScriptRuntime::wsReceiveAsync(
    WebSocketHandle wsHandle,
    double timeoutSeconds,
    runtime::CancellationToken callerToken) {
    const auto ctx = runtime_.asyncRuntime();
    if (!ctx) return runtime::AwaitableHandle::invalid();
    auto& pool = ctx->awaitables();

    if (wsHandle <= 0) {
        return pool.fromFaulted(
            runtime::ErrorCode::InvalidArguments,
            "invalid WebSocket handle");
    }

    std::uint32_t slotIdx = 0;
    std::uint32_t gen = 0;
    unpackWsHandle(wsHandle, slotIdx, gen);

    SceneEntityId ownerEntity = 0;
    std::uint64_t recvSeq = 0;
    {
        std::lock_guard<std::mutex> lock(asyncIoMutex_);
        if (slotIdx >= wsSessions_.size()) {
            return pool.fromFaulted(runtime::ErrorCode::InvalidArguments, "invalid WebSocket slot");
        }
        auto& sess = wsSessions_[slotIdx];
        if (!sess.allocated || sess.generation != gen || !sess.connected || sess.closed) {
            return pool.fromFaulted(runtime::ErrorCode::ScriptException, "WebSocket is not connected");
        }
        ownerEntity = sess.ownerEntity;
        recvSeq = sess.nextReceiveSeq++;
    }

    runtime::CancellationSourceHandle ownedCts{};
    const auto effectiveToken = buildScopedNetToken(
        ownerEntity, callerToken, timeoutSeconds, ownedCts);
    if (ctx->cancellations().isCancellationRequested(effectiveToken)) {
        if (ownedCts.valid()) (void)ctx->cancellations().releaseSource(ownedCts);
        return pool.fromCanceled();
    }

    const auto ioHandle = pool.allocatePending(runtime::ResumeQueue::PreTickIo, effectiveToken);
    const auto outerHandle = pool.allocatePending(runtime::ResumeQueue::Update, effectiveToken);
    const auto reqId = ctx->completionQueue().registerRequest(
        ioHandle,
        "Scene.WsReceiveAsync",
        hashAsyncBridgeArgs("ws_recv", {}, recvSeq ^ static_cast<std::uint64_t>(wsHandle)),
        runtime::BindingDeterminism::Recordable,
        effectiveToken);

    const std::uint64_t opId = nextMaterializeOpId_++;
    pendingNetTokenOps_.emplace(opId, PendingNetTokenCleanupOp{
        ownedCts,
        ioHandle,
        outerHandle,
        wsHandle,
        reqId,
        true,
        false,
        {},
    });

    runtime::AsyncContinuation cont{};
    cont.callback = &SceneScriptRuntime::onAsyncNetPassthroughCompleted;
    cont.context = this;
    cont.userTag = opId;
    cont.resumeQueue = runtime::ResumeQueue::PreTickIo;
    cont.runInlineOnComplete = true;
    (void)pool.onCompleted(ioHandle, cont);

    const bool isReplay =
        determinismSession_ &&
        determinismSession_->mode() == runtime::DeterminismMode::Replay;
    if (!isReplay) {
        std::optional<std::string> readyMsg;
        {
            std::lock_guard<std::mutex> lock(asyncIoMutex_);
            if (slotIdx < wsSessions_.size()) {
                auto& sess = wsSessions_[slotIdx];
                if (sess.allocated && sess.generation == gen && !sess.closed) {
                    if (!sess.inboundQueue.empty()) {
                        readyMsg = std::move(sess.inboundQueue.front());
                        sess.inboundQueue.pop_front();
                    } else {
                        sess.pendingReceivers.erase(
                            std::remove_if(
                                sess.pendingReceivers.begin(),
                                sess.pendingReceivers.end(),
                                [&](const PendingWsReceiveWaiter& w) {
                                    return ctx->cancellations().isCancellationRequested(w.token);
                                }),
                            sess.pendingReceivers.end());
                        sess.pendingReceivers.push_back(PendingWsReceiveWaiter{
                            reqId,
                            effectiveToken,
                        });
                    }
                }
            }
        }
        if (readyMsg) {
            ctx->completionQueue().submitString(reqId, std::move(*readyMsg));
        }
    }

    return outerHandle;
}

runtime::AwaitableHandle SceneScriptRuntime::wsCloseAsync(WebSocketHandle wsHandle) {
    const auto ctx = runtime_.asyncRuntime();
    if (!ctx) return runtime::AwaitableHandle::invalid();
    const bool closed = closeWebSocketFromHost(wsHandle, "client close");
    return ctx->awaitables().fromResult(runtime::Value(closed));
}

} // namespace realscript::game
