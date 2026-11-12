#include "realscript/reflection/Reflection.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <set>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace realscript::reflection {

namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void hashByte(std::uint64_t& hash, std::uint8_t byte) noexcept {
    hash ^= byte;
    hash *= kFnvPrime;
}

void hashU64(std::uint64_t& hash, std::uint64_t value) noexcept {
    for (unsigned shift = 0; shift < 64u; shift += 8u) {
        hashByte(hash, static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
}

void hashString(std::uint64_t& hash, std::string_view text) noexcept {
    hashU64(hash, static_cast<std::uint64_t>(text.size()));
    for (const char ch : text) {
        hashByte(hash, static_cast<std::uint8_t>(static_cast<unsigned char>(ch)));
    }
}

std::string trimWhitespace(std::string_view text) {
    std::size_t start = 0;
    std::size_t end = text.size();
    while (start < end && std::isspace(static_cast<unsigned char>(text[start]))) {
        ++start;
    }
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return std::string(text.substr(start, end - start));
}

semantic::SymbolId resolveExactTypeId(
    semantic::PrimitiveType type,
    const std::string& typeName) noexcept {
    if ((semantic::isExactType(type) || type == semantic::PrimitiveType::Handle) &&
        !typeName.empty()) {
        return semantic::stableTypeId(typeName);
    }
    return 0;
}

std::string canonicalNameForTypeId(
    const runtime::ProgramImage& program,
    semantic::SymbolId typeId) {
    if (typeId == 0) return {};
    if (const auto* resolved = program.resolveType(typeId)) {
        return semantic::canonicalTypeName(*resolved);
    }
    for (const auto& mod : program.modules()) {
        for (const auto& t : mod.types) {
            if (t.id == typeId) {
                return semantic::canonicalTypeName(t);
            }
        }
    }
    return {};
}

bool isNullValueForType(
    semantic::PrimitiveType expected,
    const runtime::Value& value) noexcept {
    return (expected == semantic::PrimitiveType::String &&
            std::holds_alternative<runtime::NullString>(value)) ||
        (expected == semantic::PrimitiveType::Object &&
         std::holds_alternative<runtime::NullObject>(value)) ||
        (expected == semantic::PrimitiveType::Array &&
         std::holds_alternative<runtime::NullArray>(value));
}

runtime::ExecutionResult makeFailedExecution(
    runtime::ErrorCode code,
    std::string message) {
    runtime::ExecutionResult result;
    result.succeeded = false;
    result.error.code = code;
    result.error.message = std::move(message);
    return result;
}

AttributeReflection makeAttributeFromRecord(
    const compiler::LanguageAttributeRecord& record) {
    AttributeReflection attr;
    attr.name = record.name;
    attr.target = record.target;
    attr.sourceName = record.sourceName;
    attr.offset = record.offset;
    attr.arguments = record.arguments;
    for (std::size_t i = 0; i < record.arguments.size(); ++i) {
        const auto& arg = record.arguments[i];
        if (!arg.name.empty()) {
            attr.argumentMap[arg.name] = arg.value;
        }
        const std::string posKey = "arg" + std::to_string(i);
        attr.argumentMap.try_emplace(posKey, arg.value);
    }
    return attr;
}

void sortAndDeduplicateAttributes(std::vector<AttributeReflection>& attrs) {
    std::sort(
        attrs.begin(),
        attrs.end(),
        [](const AttributeReflection& a, const AttributeReflection& b) {
            if (a.name != b.name) return a.name < b.name;
            if (a.target != b.target) return a.target < b.target;
            return a.offset < b.offset;
        });
    attrs.erase(
        std::unique(
            attrs.begin(),
            attrs.end(),
            [](const AttributeReflection& a, const AttributeReflection& b) {
                if (a.name != b.name || a.arguments.size() != b.arguments.size()) {
                    return false;
                }
                for (std::size_t i = 0; i < a.arguments.size(); ++i) {
                    if (a.arguments[i].name != b.arguments[i].name ||
                        a.arguments[i].value != b.arguments[i].value) {
                        return false;
                    }
                }
                return true;
            }),
        attrs.end());
}

MethodReflection buildMethodReflection(
    const semantic::FunctionSymbol& fn,
    const std::unordered_map<std::string, std::vector<AttributeReflection>>& attrsByTarget,
    const std::string& ownerCanonicalName) {
    MethodReflection m;
    m.symbolId = fn.id;
    m.declaringTypeId = fn.declaringTypeId != 0 ? fn.declaringTypeId : fn.ownerTypeId;
    m.ownerTypeId = fn.ownerTypeId;
    m.moduleName = fn.moduleName;
    m.ownerTypeName = fn.ownerTypeName;
    m.name = fn.name;
    if (!fn.ownerTypeName.empty()) {
        m.qualifiedName = (fn.moduleName.empty() ? std::string{} : (fn.moduleName + "::")) +
            fn.ownerTypeName + "." + fn.name;
    } else {
        m.qualifiedName = (fn.moduleName.empty() ? std::string{} : (fn.moduleName + "::")) + fn.name;
    }
    m.accessibility = fn.accessibility;
    m.returnType = semantic::storageReturnTypeOf(fn);
    m.returnTypeName = semantic::storageReturnTypeNameOf(fn);
    m.returnTypeId = resolveExactTypeId(m.returnType, m.returnTypeName);
    m.isStatic = fn.staticMethod || (!fn.method && !fn.constructor);
    m.isConstructor = fn.constructor;
    m.isVirtual = fn.virtualMethod;
    m.isOverride = fn.overrideMethod;
    m.isAbstract = fn.abstractMethod;
    m.isSealed = fn.sealedMethod;
    m.isInterfaceMethod = fn.interfaceMethod;
    m.isPropertyAccessor = fn.propertyGetter || fn.propertySetter;
    m.isSynthetic = fn.synthetic;
    m.isAsync = fn.asyncFunction;
    m.isDeterministic = fn.deterministic;

    constexpr auto kInvalidSlot = std::numeric_limits<std::uint32_t>::max();
    if (fn.virtualSlot != kInvalidSlot) {
        m.virtualSlot = fn.virtualSlot;
    }
    if (fn.interfaceSlot != kInvalidSlot) {
        m.interfaceSlot = fn.interfaceSlot;
    }

    const std::size_t paramOffset = ((fn.method || fn.constructor) && !fn.staticMethod &&
                                     !fn.parameters.empty())
        ? 1u
        : 0u;
    m.parameters.reserve(fn.parameters.size() - paramOffset);
    for (std::size_t i = paramOffset; i < fn.parameters.size(); ++i) {
        const auto& p = fn.parameters[i];
        ParameterReflection pr;
        pr.name = p.name;
        pr.type = semantic::storageTypeOf(p);
        pr.typeName = semantic::storageTypeNameOf(p);
        pr.typeId = resolveExactTypeId(pr.type, pr.typeName);
        pr.modifier = p.modifier;
        pr.index = i - paramOffset;
        pr.hasDefaultValue = p.hasDefaultValue;
        m.parameters.push_back(std::move(pr));
    }

    const std::size_t visArity = m.parameters.size();
    std::vector<std::string> candidateTargets;
    if (fn.constructor && !ownerCanonicalName.empty()) {
        const auto sep = ownerCanonicalName.rfind("::");
        const std::string shortType = sep == std::string::npos
            ? ownerCanonicalName
            : ownerCanonicalName.substr(sep + 2);
        candidateTargets.push_back(
            ownerCanonicalName + "::ctor:" + shortType + "#" + std::to_string(visArity));
    } else if (!ownerCanonicalName.empty()) {
        candidateTargets.push_back(
            ownerCanonicalName + "::method:" + fn.name + "#" + std::to_string(visArity));
        if (!fn.declaringTypeName.empty() && fn.declaringTypeName != ownerCanonicalName) {
            candidateTargets.push_back(
                fn.declaringTypeName + "::method:" + fn.name + "#" + std::to_string(visArity));
        }
    } else if (!fn.moduleName.empty()) {
        candidateTargets.push_back(
            fn.moduleName + "::function:" + fn.name + "#" + std::to_string(visArity));
    }

    for (const auto& t : candidateTargets) {
        const auto it = attrsByTarget.find(t);
        if (it != attrsByTarget.end()) {
            m.attributes.insert(m.attributes.end(), it->second.begin(), it->second.end());
        }
    }
    sortAndDeduplicateAttributes(m.attributes);
    return m;
}

std::optional<std::string> extractStringArgument(
    const runtime::Value& val,
    const runtime::ManagedHeap* preferredHeap) {
    if (std::holds_alternative<runtime::NullString>(val)) {
        return std::string{};
    }
    if (const auto* s = std::get_if<std::string>(&val)) {
        return *s;
    }
    if (const auto* ref = std::get_if<runtime::ObjectRef>(&val)) {
        if (ref->kind != runtime::ObjectKind::String) return std::nullopt;
        if (preferredHeap && preferredHeap->heapId() == ref->heapId) {
            if (const auto sv = preferredHeap->stringView(*ref)) {
                return std::string(*sv);
            }
        }
        return runtime::ManagedHeap::tryCopyStringGlobal(*ref);
    }
    return std::nullopt;
}

runtime::Value makeReturnStringValue(
    std::string text,
    runtime::ManagedHeap* targetHeap) {
    (void)targetHeap;
    return runtime::Value{std::move(text)};
}

} // namespace

// ============================================================================
// AttributeReflection
// ============================================================================

bool AttributeReflection::hasArgument(std::string_view key) const noexcept {
    const std::string effectiveKey = key.empty() ? "arg0" : std::string(key);
    return argumentMap.find(effectiveKey) != argumentMap.end();
}

std::optional<std::string> AttributeReflection::getRaw(std::string_view key) const {
    const std::string effectiveKey = key.empty() ? "arg0" : std::string(key);
    const auto it = argumentMap.find(effectiveKey);
    if (it == argumentMap.end()) return std::nullopt;
    return trimWhitespace(it->second);
}

std::optional<std::string> AttributeReflection::getString(std::string_view key) const {
    const auto raw = getRaw(key);
    if (!raw || raw->size() < 2 || raw->front() != '"' || raw->back() != '"') {
        return std::nullopt;
    }
    std::string unescaped;
    unescaped.reserve(raw->size() - 2);
    for (std::size_t i = 1; i + 1 < raw->size(); ++i) {
        const char ch = (*raw)[i];
        if (ch == '\\') {
            if (i + 2 >= raw->size()) return std::nullopt;
            ++i;
            switch ((*raw)[i]) {
            case 'n': unescaped.push_back('\n'); break;
            case 'r': unescaped.push_back('\r'); break;
            case 't': unescaped.push_back('\t'); break;
            case '\\': unescaped.push_back('\\'); break;
            case '"': unescaped.push_back('"'); break;
            case '0': unescaped.push_back('\0'); break;
            default: unescaped.push_back((*raw)[i]); break;
            }
        } else {
            unescaped.push_back(ch);
        }
    }
    return unescaped;
}

std::optional<std::int64_t> AttributeReflection::getInt(std::string_view key) const {
    const auto raw = getRaw(key);
    if (!raw || raw->empty()) return std::nullopt;
    if (raw->front() == '"' || *raw == "true" || *raw == "false") {
        return std::nullopt;
    }
    std::string cleaned = *raw;
    while (!cleaned.empty() &&
           (cleaned.back() == 'L' || cleaned.back() == 'l' ||
            cleaned.back() == 'U' || cleaned.back() == 'u')) {
        cleaned.pop_back();
    }
    if (cleaned.empty()) return std::nullopt;
    if (cleaned.find('.') != std::string::npos) return std::nullopt;

    errno = 0;
    char* endPtr = nullptr;
    const long long parsed = std::strtoll(cleaned.c_str(), &endPtr, 0);
    if (errno == ERANGE || !endPtr || *endPtr != '\0') {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(parsed);
}

std::optional<double> AttributeReflection::getDouble(std::string_view key) const {
    const auto raw = getRaw(key);
    if (!raw || raw->empty()) return std::nullopt;
    if (raw->front() == '"' || *raw == "true" || *raw == "false") {
        return std::nullopt;
    }
    std::string cleaned = *raw;
    if (cleaned.size() > 1 &&
        (cleaned.back() == 'f' || cleaned.back() == 'F' ||
         cleaned.back() == 'd' || cleaned.back() == 'D')) {
        cleaned.pop_back();
    }
    errno = 0;
    char* endPtr = nullptr;
    const double parsed = std::strtod(cleaned.c_str(), &endPtr);
    if (errno == ERANGE || !endPtr || *endPtr != '\0' || !std::isfinite(parsed)) {
        return std::nullopt;
    }
    return parsed;
}

std::optional<bool> AttributeReflection::getBool(std::string_view key) const {
    const auto raw = getRaw(key);
    if (!raw) return std::nullopt;
    if (*raw == "true") return true;
    if (*raw == "false") return false;
    return std::nullopt;
}

std::optional<std::string> AttributeReflection::getStringAt(std::size_t positionalIndex) const {
    return getString("arg" + std::to_string(positionalIndex));
}

std::optional<std::int64_t> AttributeReflection::getIntAt(std::size_t positionalIndex) const {
    return getInt("arg" + std::to_string(positionalIndex));
}

std::optional<double> AttributeReflection::getDoubleAt(std::size_t positionalIndex) const {
    return getDouble("arg" + std::to_string(positionalIndex));
}

std::optional<bool> AttributeReflection::getBoolAt(std::size_t positionalIndex) const {
    return getBool("arg" + std::to_string(positionalIndex));
}

// ============================================================================
// Member & Type Lookup Helpers
// ============================================================================

const AttributeReflection* FieldReflection::findAttribute(std::string_view attrName) const noexcept {
    for (const auto& attr : attributes) {
        if (attr.name == attrName) return &attr;
    }
    return nullptr;
}

bool FieldReflection::hasAttribute(std::string_view attrName) const noexcept {
    return findAttribute(attrName) != nullptr;
}

const AttributeReflection* PropertyReflection::findAttribute(std::string_view attrName) const noexcept {
    for (const auto& attr : attributes) {
        if (attr.name == attrName) return &attr;
    }
    return nullptr;
}

bool PropertyReflection::hasAttribute(std::string_view attrName) const noexcept {
    return findAttribute(attrName) != nullptr;
}

const AttributeReflection* MethodReflection::findAttribute(std::string_view attrName) const noexcept {
    for (const auto& attr : attributes) {
        if (attr.name == attrName) return &attr;
    }
    return nullptr;
}

bool MethodReflection::hasAttribute(std::string_view attrName) const noexcept {
    return findAttribute(attrName) != nullptr;
}

const FieldReflection* TypeReflection::findField(std::string_view fieldName) const noexcept {
    for (const auto& f : fields) {
        if (f.name == fieldName) return &f;
    }
    for (const auto& f : fields) {
        if (f.isBackingField && f.backedPropertyName == fieldName) return &f;
    }
    return nullptr;
}

const PropertyReflection* TypeReflection::findProperty(std::string_view propertyName) const noexcept {
    for (const auto& p : properties) {
        if (p.name == propertyName) return &p;
    }
    return nullptr;
}

const MethodReflection* TypeReflection::findMethod(
    std::string_view methodName,
    std::optional<std::size_t> arity) const noexcept {
    for (const auto& m : methods) {
        if (m.name == methodName && (!arity.has_value() || m.parameters.size() == *arity)) {
            return &m;
        }
    }
    return nullptr;
}

std::vector<const MethodReflection*> TypeReflection::findMethods(
    std::string_view methodName) const {
    std::vector<const MethodReflection*> out;
    for (const auto& m : methods) {
        if (m.name == methodName) out.push_back(&m);
    }
    return out;
}

const AttributeReflection* TypeReflection::findAttribute(std::string_view attrName) const noexcept {
    for (const auto& attr : attributes) {
        if (attr.name == attrName) return &attr;
    }
    return nullptr;
}

bool TypeReflection::hasAttribute(std::string_view attrName) const noexcept {
    return findAttribute(attrName) != nullptr;
}

// ============================================================================
// ReflectionRegistry Construction & Deterministic Indexing
// ============================================================================

ReflectionRegistry::ReflectionRegistry(
    std::shared_ptr<const runtime::ProgramImage> program,
    std::shared_ptr<runtime::ManagedHeap> defaultHeap)
    : program_(std::move(program)) {
    if (defaultHeap) {
        bindHeap(std::move(defaultHeap));
    }
    buildFromProgram();
}

ReflectionRegistry::ReflectionRegistry(const runtime::EngineRuntime& runtime)
    : program_(runtime.programSnapshot()) {
    if (runtime.heap()) {
        bindHeap(runtime.heap());
    }
    buildFromProgram();
}

std::shared_ptr<ReflectionRegistry> ReflectionRegistry::fromProgram(
    std::shared_ptr<const runtime::ProgramImage> program,
    std::shared_ptr<runtime::ManagedHeap> defaultHeap) {
    return std::make_shared<ReflectionRegistry>(std::move(program), std::move(defaultHeap));
}

void ReflectionRegistry::bindHeap(std::shared_ptr<runtime::ManagedHeap> heap) const {
    if (!heap) return;
    std::lock_guard<std::mutex> lock(heapMutex_);
    defaultHeap_ = heap;
    heapsById_[static_cast<std::uint32_t>(heap->heapId())] = std::move(heap);
}

std::shared_ptr<runtime::ManagedHeap> ReflectionRegistry::resolveBoundHeap(
    std::uint32_t heapId) const {
    std::lock_guard<std::mutex> lock(heapMutex_);
    if (heapId != 0) {
        const auto it = heapsById_.find(heapId);
        if (it != heapsById_.end()) {
            if (auto locked = it->second.lock()) return locked;
        }
    }
    if (auto def = defaultHeap_.lock()) {
        if (heapId == 0 || static_cast<std::uint32_t>(def->heapId()) == heapId) {
            return def;
        }
    }
    if (heapId == 0) {
        for (const auto& [id, wk] : heapsById_) {
            (void)id;
            if (auto locked = wk.lock()) return locked;
        }
    }
    return nullptr;
}

void ReflectionRegistry::buildFromProgram() {
    types_.clear();
    moduleFunctions_.clear();
    typeIndexById_.clear();
    typeIndexByCanonical_.clear();
    typeIndicesByShortName_.clear();
    attributesByTarget_.clear();
    metadataDigest_ = 0;

    if (!program_) return;

    // 1. Collect all language attributes and interface declarations across modules
    std::unordered_map<std::string, std::set<std::string>> extraInterfacesByType;
    for (const auto& mod : program_->modules()) {
        for (const auto& rec : mod.languageMetadata.attributes) {
            attributesByTarget_[rec.target].push_back(makeAttributeFromRecord(rec));
        }
        for (const auto& ifaceImpl : mod.languageMetadata.interfaces) {
            for (const auto& ifaceName : ifaceImpl.interfaces) {
                extraInterfacesByType[ifaceImpl.typeName].insert(ifaceName);
            }
        }
    }
    for (auto& [target, list] : attributesByTarget_) {
        (void)target;
        sortAndDeduplicateAttributes(list);
    }

    // 2. Collect unique TypeSymbols across all modules (preferring owning module descriptor)
    std::unordered_map<semantic::SymbolId, const semantic::TypeSymbol*> bestTypeById;
    for (const auto& mod : program_->modules()) {
        for (const auto& typeSym : mod.types) {
            if (typeSym.id == 0) continue;
            const auto [it, inserted] = bestTypeById.emplace(typeSym.id, &typeSym);
            if (!inserted && typeSym.moduleName == mod.name &&
                it->second->moduleName != mod.name) {
                it->second = &typeSym;
            }
        }
    }

    types_.reserve(bestTypeById.size());
    for (const auto& [tid, typePtr] : bestTypeById) {
        (void)tid;
        const auto& ts = *typePtr;
        TypeReflection tr;
        tr.typeId = ts.id;
        tr.moduleName = ts.moduleName;
        tr.name = ts.name;
        tr.canonicalName = semantic::canonicalTypeName(ts);
        tr.kind = ts.kind;
        tr.accessibility = ts.accessibility;
        tr.isInterface = ts.interfaceType;
        tr.isAbstract = ts.abstractType;
        tr.isSealed = ts.sealedType;
        tr.isStatic = ts.staticType;
        tr.isDelegate = ts.delegateType;
        tr.isSynthetic = ts.synthetic;
        tr.baseTypeName = ts.baseTypeName;
        tr.baseTypeId = ts.baseTypeId != 0
            ? ts.baseTypeId
            : (!ts.baseTypeName.empty() ? semantic::stableTypeId(ts.baseTypeName) : 0);
        tr.enumMembers = ts.enumMembers;
        tr.virtualDispatchTable = ts.virtualDispatchTable;
        tr.interfaceDispatchMaps = ts.interfaceDispatchMaps;
        if (ts.staticConstructor.has_value()) {
            tr.staticConstructorSymbolId = ts.staticConstructor->id;
        }

        // Implemented interfaces (from dispatch maps + metadata, including base chain)
        std::set<semantic::SymbolId> ifaceIds;
        std::set<std::string> ifaceNames;
        for (const auto& imap : ts.interfaceDispatchMaps) {
            if (imap.interfaceTypeId != 0) {
                ifaceIds.insert(imap.interfaceTypeId);
                const auto cname = canonicalNameForTypeId(*program_, imap.interfaceTypeId);
                if (!cname.empty()) ifaceNames.insert(cname);
            }
        }
        if (const auto extraIt = extraInterfacesByType.find(tr.canonicalName);
            extraIt != extraInterfacesByType.end()) {
            for (const auto& iname : extraIt->second) {
                ifaceNames.insert(iname);
                ifaceIds.insert(semantic::stableTypeId(iname));
            }
        }
        if (const auto extraShort = extraInterfacesByType.find(tr.name);
            extraShort != extraInterfacesByType.end()) {
            for (const auto& iname : extraShort->second) {
                const std::string resolved = iname.find("::") == std::string::npos && !tr.moduleName.empty()
                    ? (tr.moduleName + "::" + iname)
                    : iname;
                ifaceNames.insert(resolved);
                ifaceIds.insert(semantic::stableTypeId(resolved));
            }
        }
        tr.implementedInterfaceIds.assign(ifaceIds.begin(), ifaceIds.end());
        tr.implementedInterfaces.assign(ifaceNames.begin(), ifaceNames.end());

        // Type attributes
        if (const auto attrIt = attributesByTarget_.find(tr.canonicalName);
            attrIt != attributesByTarget_.end()) {
            tr.attributes = attrIt->second;
        }

        // Properties & backing field index mapping
        std::unordered_map<std::size_t, std::string> backingIndexToPropName;
        tr.properties.reserve(ts.properties.size());
        for (const auto& ps : ts.properties) {
            PropertyReflection pr;
            pr.id = ps.id;
            pr.declaringTypeId = ps.declaringTypeId != 0 ? ps.declaringTypeId : ts.id;
            pr.declaringTypeName = ps.declaringTypeName.empty() ? tr.canonicalName : ps.declaringTypeName;
            pr.name = ps.name;
            pr.accessibility = ps.accessibility;
            pr.type = ps.type;
            pr.typeName = ps.typeName;
            pr.propertyTypeId = resolveExactTypeId(ps.type, ps.typeName);
            pr.isStatic = ps.staticProperty;
            if (ps.backingFieldIndex < ts.fields.size()) {
                pr.hasBackingField = true;
                pr.backingFieldIndex = ps.backingFieldIndex;
                pr.backingFieldName = ts.fields[ps.backingFieldIndex].name;
                backingIndexToPropName[ps.backingFieldIndex] = ps.name;
            }
            if (ps.getter) pr.getterSymbolId = ps.getter->id;
            if (ps.setter) pr.setterSymbolId = ps.setter->id;

            std::vector<std::string> propTargets{
                tr.canonicalName + "::property:" + ps.name};
            if (!ps.declaringTypeName.empty() && ps.declaringTypeName != tr.canonicalName) {
                propTargets.push_back(ps.declaringTypeName + "::property:" + ps.name);
            }
            for (const auto& pt : propTargets) {
                if (const auto ait = attributesByTarget_.find(pt); ait != attributesByTarget_.end()) {
                    pr.attributes.insert(pr.attributes.end(), ait->second.begin(), ait->second.end());
                }
            }
            sortAndDeduplicateAttributes(pr.attributes);
            tr.properties.push_back(std::move(pr));
        }

        // Fields (preserving exact slot layout order 0..N-1)
        tr.fields.reserve(ts.fields.size());
        for (std::size_t fi = 0; fi < ts.fields.size(); ++fi) {
            const auto& fs = ts.fields[fi];
            FieldReflection fr;
            fr.id = fs.id;
            fr.declaringTypeId = fs.declaringTypeId != 0 ? fs.declaringTypeId : ts.id;
            fr.declaringTypeName = fs.declaringTypeName.empty() ? tr.canonicalName : fs.declaringTypeName;
            fr.name = fs.name;
            fr.accessibility = fs.accessibility;
            fr.type = fs.type;
            fr.typeName = fs.typeName;
            fr.fieldTypeId = resolveExactTypeId(fs.type, fs.typeName);
            fr.offsetIndex = fs.index;
            fr.index = fs.index;
            fr.isStatic = fs.staticField;
            fr.isConst = fs.constField;
            fr.isReadonly = fs.readonlyField;
            fr.isSynthetic = fs.synthetic;
            fr.hasConstantValue = fs.hasConstantValue;
            fr.constantValue = fs.constantValue;

            if (const auto bpIt = backingIndexToPropName.find(fi);
                bpIt != backingIndexToPropName.end()) {
                fr.isBackingField = true;
                fr.backedPropertyName = bpIt->second;
            } else if (fs.synthetic && fs.name.size() > 1 && fs.name.front() == '$' &&
                       fs.name.rfind("$event_", 0) != 0) {
                fr.isBackingField = true;
                fr.backedPropertyName = fs.name.substr(1);
            }

            std::vector<std::string> fieldTargets{
                tr.canonicalName + "::field:" + fs.name};
            if (!fr.declaringTypeName.empty() && fr.declaringTypeName != tr.canonicalName) {
                fieldTargets.push_back(fr.declaringTypeName + "::field:" + fs.name);
            }
            if (fr.isBackingField && !fr.backedPropertyName.empty()) {
                fieldTargets.push_back(tr.canonicalName + "::property:" + fr.backedPropertyName);
                if (!fr.declaringTypeName.empty() && fr.declaringTypeName != tr.canonicalName) {
                    fieldTargets.push_back(fr.declaringTypeName + "::property:" + fr.backedPropertyName);
                }
            }
            for (const auto& ft : fieldTargets) {
                if (const auto ait = attributesByTarget_.find(ft); ait != attributesByTarget_.end()) {
                    fr.attributes.insert(fr.attributes.end(), ait->second.begin(), ait->second.end());
                }
            }
            sortAndDeduplicateAttributes(fr.attributes);
            tr.fields.push_back(std::move(fr));
        }

        // Constructors
        tr.constructors.reserve(ts.constructors.size());
        for (const auto& ctor : ts.constructors) {
            tr.constructors.push_back(
                buildMethodReflection(ctor, attributesByTarget_, tr.canonicalName));
        }
        std::sort(
            tr.constructors.begin(),
            tr.constructors.end(),
            [](const MethodReflection& a, const MethodReflection& b) {
                if (a.parameters.size() != b.parameters.size()) {
                    return a.parameters.size() < b.parameters.size();
                }
                return a.symbolId < b.symbolId;
            });

        // Methods (including property accessors if not already in ts.methods)
        std::unordered_set<semantic::SymbolId> seenMethodIds;
        for (const auto& mSym : ts.methods) {
            if (mSym.id != 0 && !seenMethodIds.insert(mSym.id).second) continue;
            tr.methods.push_back(
                buildMethodReflection(mSym, attributesByTarget_, tr.canonicalName));
        }
        for (const auto& ps : ts.properties) {
            if (ps.getter && ps.getter->id != 0 && seenMethodIds.insert(ps.getter->id).second) {
                tr.methods.push_back(
                    buildMethodReflection(*ps.getter, attributesByTarget_, tr.canonicalName));
            }
            if (ps.setter && ps.setter->id != 0 && seenMethodIds.insert(ps.setter->id).second) {
                tr.methods.push_back(
                    buildMethodReflection(*ps.setter, attributesByTarget_, tr.canonicalName));
            }
        }
        std::sort(
            tr.methods.begin(),
            tr.methods.end(),
            [](const MethodReflection& a, const MethodReflection& b) {
                if (a.name != b.name) return a.name < b.name;
                if (a.parameters.size() != b.parameters.size()) {
                    return a.parameters.size() < b.parameters.size();
                }
                return a.symbolId < b.symbolId;
            });

        types_.push_back(std::move(tr));
    }

    // Sort all types deterministically by (canonicalName, typeId)
    std::sort(
        types_.begin(),
        types_.end(),
        [](const TypeReflection& a, const TypeReflection& b) {
            if (a.canonicalName != b.canonicalName) {
                return a.canonicalName < b.canonicalName;
            }
            return a.typeId < b.typeId;
        });

    for (std::size_t idx = 0; idx < types_.size(); ++idx) {
        const auto& tr = types_[idx];
        typeIndexById_.emplace(tr.typeId, idx);
        typeIndexByCanonical_.emplace(tr.canonicalName, idx);
        if (!tr.isSynthetic) {
            typeIndicesByShortName_[tr.name].push_back(idx);
        }
    }

    // 3. Collect module-level free functions deterministically from bytecode modules
    std::unordered_set<semantic::SymbolId> memberMethodIds;
    for (const auto& tr : types_) {
        for (const auto& m : tr.methods) memberMethodIds.insert(m.symbolId);
        for (const auto& c : tr.constructors) memberMethodIds.insert(c.symbolId);
        if (tr.staticConstructorSymbolId) memberMethodIds.insert(*tr.staticConstructorSymbolId);
    }

    for (const auto& mod : program_->modules()) {
        for (const auto& fn : mod.functions) {
            if (memberMethodIds.find(fn.symbolId) != memberMethodIds.end()) continue;
            if (fn.name.find('.') != std::string::npos) continue;

            MethodReflection mf;
            mf.symbolId = fn.symbolId;
            mf.moduleName = mod.name;
            mf.name = fn.name;
            mf.qualifiedName = mod.name + "::" + fn.name;
            mf.returnType = fn.returnType;
            mf.returnTypeId = fn.returnTypeId;
            mf.returnTypeName = canonicalNameForTypeId(*program_, fn.returnTypeId);
            mf.isStatic = true;
            mf.parameters.reserve(fn.parameterTypes.size());
            for (std::size_t pi = 0; pi < fn.parameterTypes.size(); ++pi) {
                ParameterReflection pr;
                pr.name = "arg" + std::to_string(pi);
                pr.type = fn.parameterTypes[pi];
                pr.typeId = pi < fn.parameterTypeIds.size() ? fn.parameterTypeIds[pi] : 0;
                pr.typeName = canonicalNameForTypeId(*program_, pr.typeId);
                pr.index = pi;
                mf.parameters.push_back(std::move(pr));
            }
            const std::string target = mod.name + "::function:" + fn.name + "#" +
                std::to_string(mf.parameters.size());
            if (const auto ait = attributesByTarget_.find(target);
                ait != attributesByTarget_.end()) {
                mf.attributes = ait->second;
            }
            moduleFunctions_.push_back(std::move(mf));
        }
    }
    std::sort(
        moduleFunctions_.begin(),
        moduleFunctions_.end(),
        [](const MethodReflection& a, const MethodReflection& b) {
            if (a.qualifiedName != b.qualifiedName) {
                return a.qualifiedName < b.qualifiedName;
            }
            if (a.parameters.size() != b.parameters.size()) {
                return a.parameters.size() < b.parameters.size();
            }
            return a.symbolId < b.symbolId;
        });

    computeMetadataDigest();
}

void ReflectionRegistry::computeMetadataDigest() noexcept {
    std::uint64_t h = kFnvOffset;
    hashU64(h, static_cast<std::uint64_t>(types_.size()));
    for (const auto& t : types_) {
        hashU64(h, t.typeId);
        hashString(h, t.canonicalName);
        hashByte(h, static_cast<std::uint8_t>(t.kind));
        hashU64(h, t.baseTypeId);
        hashU64(h, static_cast<std::uint64_t>(t.implementedInterfaces.size()));
        for (const auto& iface : t.implementedInterfaces) {
            hashString(h, iface);
        }
        hashU64(h, static_cast<std::uint64_t>(t.attributes.size()));
        for (const auto& a : t.attributes) {
            hashString(h, a.name);
            for (const auto& arg : a.arguments) {
                hashString(h, arg.name);
                hashString(h, arg.value);
            }
        }
        hashU64(h, static_cast<std::uint64_t>(t.fields.size()));
        for (const auto& f : t.fields) {
            hashU64(h, f.id);
            hashString(h, f.name);
            hashByte(h, static_cast<std::uint8_t>(f.type));
            hashU64(h, static_cast<std::uint64_t>(f.offsetIndex));
            hashU64(h, static_cast<std::uint64_t>(f.attributes.size()));
            for (const auto& a : f.attributes) {
                hashString(h, a.name);
            }
        }
        hashU64(h, static_cast<std::uint64_t>(t.methods.size()));
        for (const auto& m : t.methods) {
            hashU64(h, m.symbolId);
            hashString(h, m.name);
            hashU64(h, static_cast<std::uint64_t>(m.parameters.size()));
            hashU64(h, static_cast<std::uint64_t>(m.attributes.size()));
            for (const auto& a : m.attributes) {
                hashString(h, a.name);
            }
        }
    }
    metadataDigest_ = h;
}

// ============================================================================
// Queries
// ============================================================================

const TypeReflection* ReflectionRegistry::findType(semantic::SymbolId typeId) const noexcept {
    if (typeId == 0) return nullptr;
    const auto it = typeIndexById_.find(typeId);
    return it == typeIndexById_.end() ? nullptr : &types_[it->second];
}

const TypeReflection* ReflectionRegistry::findType(std::string_view nameOrCanonical) const noexcept {
    if (nameOrCanonical.empty()) return nullptr;
    const std::string key(nameOrCanonical);
    if (const auto it = typeIndexByCanonical_.find(key); it != typeIndexByCanonical_.end()) {
        return &types_[it->second];
    }
    if (nameOrCanonical.find("::") == std::string_view::npos) {
        if (const auto shortIt = typeIndicesByShortName_.find(key);
            shortIt != typeIndicesByShortName_.end() && shortIt->second.size() == 1u) {
            return &types_[shortIt->second.front()];
        }
    }
    return nullptr;
}

std::vector<const TypeReflection*> ReflectionRegistry::findTypesByShortName(
    std::string_view shortName) const {
    std::vector<const TypeReflection*> out;
    const auto it = typeIndicesByShortName_.find(std::string(shortName));
    if (it == typeIndicesByShortName_.end()) return out;
    out.reserve(it->second.size());
    for (const std::size_t idx : it->second) {
        out.push_back(&types_[idx]);
    }
    return out;
}

const MethodReflection* ReflectionRegistry::findModuleFunction(
    std::string_view qualifiedOrModuleName,
    std::string_view funcName,
    std::optional<std::size_t> arity) const noexcept {
    std::string targetQualified;
    if (funcName.empty()) {
        targetQualified = std::string(qualifiedOrModuleName);
    } else {
        targetQualified = std::string(qualifiedOrModuleName) + "::" + std::string(funcName);
    }
    for (const auto& fn : moduleFunctions_) {
        if ((fn.qualifiedName == targetQualified ||
             (funcName.empty() && fn.name == targetQualified)) &&
            (!arity.has_value() || fn.parameters.size() == *arity)) {
            return &fn;
        }
    }
    return nullptr;
}

bool ReflectionRegistry::isTypeAssignable(
    semantic::SymbolId actualTypeId,
    semantic::SymbolId expectedTypeId) const noexcept {
    if (expectedTypeId == 0 || actualTypeId == expectedTypeId) return true;
    if (actualTypeId == 0) return false;

    std::unordered_set<semantic::SymbolId> visited;
    const TypeReflection* current = findType(actualTypeId);
    while (current && visited.insert(current->typeId).second) {
        if (current->typeId == expectedTypeId) return true;
        for (const auto ifaceId : current->implementedInterfaceIds) {
            if (ifaceId == expectedTypeId) return true;
        }
        if (current->baseTypeId == expectedTypeId) return true;
        current = findType(current->baseTypeId);
    }
    return false;
}

std::vector<AnnotatedTypeMatch> ReflectionRegistry::findTypesWithAttribute(
    std::string_view attributeName) const {
    std::vector<AnnotatedTypeMatch> out;
    for (const auto& t : types_) {
        if (const auto* attr = t.findAttribute(attributeName)) {
            out.push_back(AnnotatedTypeMatch{&t, attr});
        }
    }
    return out;
}

std::vector<AnnotatedFieldMatch> ReflectionRegistry::findFieldsWithAttribute(
    std::string_view attributeName) const {
    std::vector<AnnotatedFieldMatch> out;
    for (const auto& t : types_) {
        for (const auto& f : t.fields) {
            // Avoid reporting inherited base fields twice on derived classes
            if (f.declaringTypeId != 0 && f.declaringTypeId != t.typeId) {
                continue;
            }
            if (const auto* attr = f.findAttribute(attributeName)) {
                out.push_back(AnnotatedFieldMatch{&t, &f, attr});
            }
        }
    }
    return out;
}

std::vector<AnnotatedMethodMatch> ReflectionRegistry::findMethodsWithAttribute(
    std::string_view attributeName) const {
    std::vector<AnnotatedMethodMatch> out;
    for (const auto& t : types_) {
        for (const auto& m : t.methods) {
            if (m.declaringTypeId != 0 && m.declaringTypeId != t.typeId) {
                continue;
            }
            if (const auto* attr = m.findAttribute(attributeName)) {
                out.push_back(AnnotatedMethodMatch{&t, &m, attr});
            }
        }
        for (const auto& c : t.constructors) {
            if (const auto* attr = c.findAttribute(attributeName)) {
                out.push_back(AnnotatedMethodMatch{&t, &c, attr});
            }
        }
    }
    for (const auto& mf : moduleFunctions_) {
        if (const auto* attr = mf.findAttribute(attributeName)) {
            out.push_back(AnnotatedMethodMatch{nullptr, &mf, attr});
        }
    }
    return out;
}

const AttributeReflection* ReflectionRegistry::findAttributeOnTarget(
    std::string_view targetSpec,
    std::string_view attributeName) const noexcept {
    if (targetSpec.empty() || attributeName.empty()) return nullptr;
    const std::string key(targetSpec);

    // 1. Direct compiler target key match (e.g. "Mod::Type::field:hp" or "Mod::Type::method:Foo#1")
    if (const auto directIt = attributesByTarget_.find(key);
        directIt != attributesByTarget_.end()) {
        for (const auto& attr : directIt->second) {
            if (attr.name == attributeName) return &attr;
        }
    }

    // 2. Type lookup by canonical or short name
    if (const auto* t = findType(targetSpec)) {
        return t->findAttribute(attributeName);
    }

    // 3. Friendly member syntax: "Mod::Type.Member" or "Type.Member" or "Mod::Type::Member"
    std::string ownerPart;
    std::string memberPart;
    const auto dotPos = targetSpec.rfind('.');
    if (dotPos != std::string_view::npos) {
        ownerPart = std::string(targetSpec.substr(0, dotPos));
        memberPart = std::string(targetSpec.substr(dotPos + 1));
    } else {
        const auto sepPos = targetSpec.rfind("::");
        if (sepPos != std::string_view::npos) {
            ownerPart = std::string(targetSpec.substr(0, sepPos));
            memberPart = std::string(targetSpec.substr(sepPos + 2));
        }
    }

    if (!ownerPart.empty() && !memberPart.empty()) {
        if (const auto* owner = findType(ownerPart)) {
            if (const auto* fld = owner->findField(memberPart)) {
                if (const auto* a = fld->findAttribute(attributeName)) return a;
            }
            if (const auto* prop = owner->findProperty(memberPart)) {
                if (const auto* a = prop->findAttribute(attributeName)) return a;
            }
            std::optional<std::size_t> arity;
            std::string cleanMethod = memberPart;
            if (const auto hashPos = memberPart.find('#'); hashPos != std::string::npos) {
                cleanMethod = memberPart.substr(0, hashPos);
                arity = static_cast<std::size_t>(std::strtoull(memberPart.c_str() + hashPos + 1, nullptr, 10));
            }
            if (const auto* m = owner->findMethod(cleanMethod, arity)) {
                if (const auto* a = m->findAttribute(attributeName)) return a;
            }
        }
        if (const auto* modFn = findModuleFunction(ownerPart, memberPart)) {
            if (const auto* a = modFn->findAttribute(attributeName)) return a;
        }
    }
    return nullptr;
}

bool ReflectionRegistry::hasAttributeOnTarget(
    std::string_view targetSpec,
    std::string_view attributeName) const noexcept {
    return findAttributeOnTarget(targetSpec, attributeName) != nullptr;
}

// ============================================================================
// Dynamic Object Instantiation, Field Read/Write, and Polymorphic Invocation
// ============================================================================

runtime::Value ReflectionRegistry::makeDefaultFieldValue(
    semantic::PrimitiveType type,
    semantic::SymbolId typeId,
    std::uint32_t depth) const {
    switch (type) {
    case semantic::PrimitiveType::Bool: return false;
    case semantic::PrimitiveType::Byte: return runtime::ByteValue{};
    case semantic::PrimitiveType::SByte: return runtime::SByteValue{};
    case semantic::PrimitiveType::Short: return runtime::ShortValue{};
    case semantic::PrimitiveType::UShort: return runtime::UShortValue{};
    case semantic::PrimitiveType::Int: return std::int64_t{0};
    case semantic::PrimitiveType::UInt: return runtime::UIntValue{};
    case semantic::PrimitiveType::Long: return runtime::LongValue{};
    case semantic::PrimitiveType::ULong: return runtime::ULongValue{};
    case semantic::PrimitiveType::Float: return runtime::FloatValue{};
    case semantic::PrimitiveType::Double: return 0.0;
    case semantic::PrimitiveType::Char: return runtime::CharValue{};
    case semantic::PrimitiveType::String: return runtime::NullString{};
    case semantic::PrimitiveType::Object: return runtime::NullObject{};
    case semantic::PrimitiveType::Array: return runtime::NullArray{};
    case semantic::PrimitiveType::Handle: {
        runtime::NativeHandle h;
        h.typeId = typeId;
        return h;
    }
    case semantic::PrimitiveType::Enum:
        return runtime::EnumValue{typeId, 0};
    case semantic::PrimitiveType::Struct: {
        if (const auto inlined = runtime::makeDefaultInlineStructValue(typeId)) {
            return *inlined;
        }
        if (typeId == 0 || depth > 32u) return runtime::StructValue{};
        const auto* st = findType(typeId);
        if (!st || st->kind != semantic::TypeKind::Struct) {
            return runtime::StructValue{};
        }
        std::vector<runtime::Value> fields;
        fields.reserve(st->fields.size());
        for (const auto& f : st->fields) {
            if (f.isStatic || f.isConst) continue;
            fields.push_back(makeDefaultFieldValue(f.type, f.fieldTypeId, depth + 1u));
        }
        return runtime::makeStructValueFromFields(typeId, std::move(fields));
    }
    case semantic::PrimitiveType::Null:
    case semantic::PrimitiveType::Void:
    case semantic::PrimitiveType::Error:
        break;
    }
    return runtime::Value{};
}

std::optional<runtime::Value> ReflectionRegistry::evaluateConstantFieldValue(
    const FieldReflection& field,
    runtime::ManagedHeap* heap,
    runtime::RuntimeError& error) const {
    if (!field.hasConstantValue) return std::nullopt;
    if (std::holds_alternative<std::int64_t>(field.constantValue)) {
        const auto raw = std::get<std::int64_t>(field.constantValue);
        switch (field.type) {
        case semantic::PrimitiveType::Byte:
            return runtime::Value{runtime::ByteValue{static_cast<std::uint8_t>(raw)}};
        case semantic::PrimitiveType::SByte:
            return runtime::Value{runtime::SByteValue{static_cast<std::int8_t>(raw)}};
        case semantic::PrimitiveType::Short:
            return runtime::Value{runtime::ShortValue{static_cast<std::int16_t>(raw)}};
        case semantic::PrimitiveType::UShort:
            return runtime::Value{runtime::UShortValue{static_cast<std::uint16_t>(raw)}};
        case semantic::PrimitiveType::Int:
            return runtime::Value{raw};
        case semantic::PrimitiveType::UInt:
            return runtime::Value{runtime::UIntValue{static_cast<std::uint32_t>(raw)}};
        case semantic::PrimitiveType::Long:
            return runtime::Value{runtime::LongValue{raw}};
        case semantic::PrimitiveType::ULong:
            return runtime::Value{runtime::ULongValue{static_cast<std::uint64_t>(raw)}};
        case semantic::PrimitiveType::Char:
            return runtime::Value{runtime::CharValue{static_cast<char16_t>(raw)}};
        case semantic::PrimitiveType::Enum:
            return runtime::Value{runtime::EnumValue{field.fieldTypeId, raw}};
        case semantic::PrimitiveType::Float:
            return runtime::Value{runtime::FloatValue{static_cast<float>(raw)}};
        case semantic::PrimitiveType::Double:
            return runtime::Value{static_cast<double>(raw)};
        default:
            break;
        }
    } else if (std::holds_alternative<double>(field.constantValue)) {
        const auto raw = std::get<double>(field.constantValue);
        if (field.type == semantic::PrimitiveType::Float) {
            return runtime::Value{runtime::FloatValue{static_cast<float>(raw)}};
        }
        return runtime::Value{raw};
    } else if (std::holds_alternative<bool>(field.constantValue)) {
        return runtime::Value{std::get<bool>(field.constantValue)};
    } else if (std::holds_alternative<std::string>(field.constantValue)) {
        const auto& strVal = std::get<std::string>(field.constantValue);
        if (heap) {
            if (const auto strRef = heap->allocateString(strVal, &error)) {
                return runtime::Value{*strRef};
            }
            return std::nullopt;
        }
        return runtime::Value{strVal};
    }
    return std::nullopt;
}

bool ReflectionRegistry::structContainsManagedReferences(
    semantic::SymbolId structTypeId,
    std::uint32_t depth) const noexcept {
    if (structTypeId == 0 || depth > 32u) return false;
    const auto* st = findType(structTypeId);
    if (!st || st->kind != semantic::TypeKind::Struct) return false;
    for (const auto& f : st->fields) {
        if (f.isStatic || f.isConst) continue;
        if (f.type == semantic::PrimitiveType::String ||
            f.type == semantic::PrimitiveType::Object ||
            f.type == semantic::PrimitiveType::Array) {
            return true;
        }
        if (f.type == semantic::PrimitiveType::Struct &&
            structContainsManagedReferences(f.fieldTypeId, depth + 1u)) {
            return true;
        }
    }
    return false;
}

bool ReflectionRegistry::validateAndCoerceValue(
    const runtime::Value& input,
    semantic::PrimitiveType expectedType,
    semantic::SymbolId expectedTypeId,
    runtime::ManagedHeap* targetHeap,
    runtime::Value& outCoerced,
    runtime::RuntimeError& error) const {
    if (isNullValueForType(expectedType, input)) {
        outCoerced = input;
        return true;
    }

    const auto actualPrim = runtime::valueType(input);
    if (actualPrim != expectedType) {
        error.code = runtime::ErrorCode::TypeMismatch;
        error.message = "value of type '" + std::string(semantic::primitiveTypeName(actualPrim)) +
            "' cannot be assigned to '" + std::string(semantic::primitiveTypeName(expectedType)) + "'";
        return false;
    }

    switch (expectedType) {
    case semantic::PrimitiveType::Int: {
        std::int64_t v = 0;
        if (!input.tryGetInt64(v) ||
            v < static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()) ||
            v > static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max())) {
            error.code = runtime::ErrorCode::TypeMismatch;
            error.message = "int value is out of 32-bit signed integer bounds";
            return false;
        }
        outCoerced = input;
        return true;
    }
    case semantic::PrimitiveType::String: {
        if (const auto* s = std::get_if<std::string>(&input)) {
            if (targetHeap) {
                const auto allocated = targetHeap->allocateString(*s, &error);
                if (!allocated) return false;
                outCoerced = *allocated;
            } else {
                outCoerced = input;
            }
            return true;
        }
        if (const auto* ref = std::get_if<runtime::ObjectRef>(&input)) {
            if (ref->kind != runtime::ObjectKind::String) {
                error.code = runtime::ErrorCode::TypeMismatch;
                error.message = "expected string reference";
                return false;
            }
            if (targetHeap) {
                if (ref->heapId == static_cast<std::uint32_t>(targetHeap->heapId()) &&
                    targetHeap->isAlive(*ref)) {
                    outCoerced = input;
                    return true;
                }
                const auto copied = runtime::ManagedHeap::tryCopyStringGlobal(*ref);
                if (!copied) {
                    error.code = runtime::ErrorCode::InvalidObjectReference;
                    error.message = "string reference is dead or invalid";
                    return false;
                }
                const auto allocated = targetHeap->allocateString(*copied, &error);
                if (!allocated) return false;
                outCoerced = *allocated;
                return true;
            }
            outCoerced = input;
            return true;
        }
        error.code = runtime::ErrorCode::TypeMismatch;
        error.message = "invalid string value representation";
        return false;
    }
    case semantic::PrimitiveType::Object: {
        const auto* ref = std::get_if<runtime::ObjectRef>(&input);
        if (!ref || ref->kind != runtime::ObjectKind::Record) {
            error.code = runtime::ErrorCode::TypeMismatch;
            error.message = "expected managed class object reference";
            return false;
        }
        if (!targetHeap) {
            error.code = runtime::ErrorCode::InvalidObjectReference;
            error.message = "cannot validate object reference without target ManagedHeap";
            return false;
        }
        if (ref->heapId != static_cast<std::uint32_t>(targetHeap->heapId()) ||
            !targetHeap->isAlive(*ref)) {
            error.code = runtime::ErrorCode::InvalidObjectReference;
            error.message = "object reference is dead or belongs to a foreign heap";
            return false;
        }
        const auto actualObjTid = targetHeap->objectTypeId(*ref);
        if (!actualObjTid || (expectedTypeId != 0 && !isTypeAssignable(*actualObjTid, expectedTypeId))) {
            error.code = runtime::ErrorCode::TypeMismatch;
            error.message = "runtime object type is not assignable to target field/parameter type";
            return false;
        }
        outCoerced = input;
        return true;
    }
    case semantic::PrimitiveType::Array: {
        const auto* ref = std::get_if<runtime::ObjectRef>(&input);
        if (!ref || ref->kind != runtime::ObjectKind::Array) {
            error.code = runtime::ErrorCode::TypeMismatch;
            error.message = "expected managed array reference";
            return false;
        }
        if (!targetHeap ||
            ref->heapId != static_cast<std::uint32_t>(targetHeap->heapId()) ||
            !targetHeap->isAlive(*ref)) {
            error.code = runtime::ErrorCode::InvalidObjectReference;
            error.message = "array reference is dead or belongs to a foreign heap";
            return false;
        }
        const auto actualArrTid = targetHeap->arrayTypeId(*ref);
        if (!actualArrTid || (expectedTypeId != 0 && *actualArrTid != 0 && *actualArrTid != expectedTypeId)) {
            error.code = runtime::ErrorCode::TypeMismatch;
            error.message = "runtime array type does not match target type";
            return false;
        }
        outCoerced = input;
        return true;
    }
    case semantic::PrimitiveType::Struct: {
        const auto actualStructTid = runtime::structValueTypeId(input);
        if (!actualStructTid || *actualStructTid == 0 ||
            (expectedTypeId != 0 && *actualStructTid != expectedTypeId)) {
            error.code = runtime::ErrorCode::TypeMismatch;
            error.message = "struct value typeId does not match expected struct type";
            return false;
        }
        if (const auto* sv = std::get_if<runtime::StructValue>(&input)) {
            if (!sv->valid()) {
                error.code = runtime::ErrorCode::TypeMismatch;
                error.message = "struct value is uninitialized";
                return false;
            }
            // Validate no foreign ObjectRef sits inside struct fields
            if (targetHeap) {
                for (std::size_t i = 0; i < sv->fieldCount(); ++i) {
                    const auto sub = runtime::structValueFieldAt(*sv, i);
                    if (!sub) continue;
                    if (const auto* subRef = std::get_if<runtime::ObjectRef>(&*sub)) {
                        if (subRef->heapId != static_cast<std::uint32_t>(targetHeap->heapId()) ||
                            !targetHeap->isAlive(*subRef)) {
                            error.code = runtime::ErrorCode::InvalidObjectReference;
                            error.message = "struct contains dead or cross-heap ObjectRef";
                            return false;
                        }
                    }
                }
            }
        }
        outCoerced = input;
        return true;
    }
    case semantic::PrimitiveType::Enum: {
        runtime::EnumValue ev{};
        if (!input.tryGetEnum(ev) || (expectedTypeId != 0 && ev.typeId != expectedTypeId)) {
            error.code = runtime::ErrorCode::TypeMismatch;
            error.message = "enum value typeId does not match expected enum type";
            return false;
        }
        outCoerced = input;
        return true;
    }
    case semantic::PrimitiveType::Handle: {
        runtime::NativeHandle h{};
        if (!input.tryGetNativeHandle(h) ||
            (expectedTypeId != 0 && h.typeId != 0 && h.typeId != expectedTypeId)) {
            error.code = runtime::ErrorCode::TypeMismatch;
            error.message = "native handle typeId does not match expected handle type";
            return false;
        }
        outCoerced = input;
        return true;
    }
    default:
        outCoerced = input;
        return true;
    }
}

std::optional<runtime::ObjectRef> ReflectionRegistry::instantiateUninitializedObject(
    runtime::ManagedHeap& heap,
    semantic::SymbolId typeId,
    runtime::RuntimeError& error) const {
    const auto* type = findType(typeId);
    if (!type) {
        error.code = runtime::ErrorCode::FunctionNotFound;
        error.message = "reflected typeId " + std::to_string(typeId) + " was not found";
        return std::nullopt;
    }
    if (type->kind != semantic::TypeKind::Class ||
        type->isInterface || type->isAbstract || type->isStatic || type->isDelegate) {
        error.code = runtime::ErrorCode::TypeMismatch;
        error.message = "cannot instantiate non-concrete class type '" + type->canonicalName + "'";
        return std::nullopt;
    }

    std::vector<runtime::Value> fieldValues;
    std::vector<std::size_t> referenceFields;
    fieldValues.reserve(type->fields.size());

    for (std::size_t idx = 0; idx < type->fields.size(); ++idx) {
        const auto& fld = type->fields[idx];
        runtime::Value initial = makeDefaultFieldValue(fld.type, fld.fieldTypeId);
        if (!fld.isStatic && !fld.isConst && fld.hasConstantValue) {
            if (auto constVal = evaluateConstantFieldValue(fld, &heap, error)) {
                initial = std::move(*constVal);
            } else if (error.code != runtime::ErrorCode::None) {
                return std::nullopt;
            }
        }
        fieldValues.push_back(std::move(initial));
        if (fld.type == semantic::PrimitiveType::String ||
            fld.type == semantic::PrimitiveType::Object ||
            fld.type == semantic::PrimitiveType::Array) {
            referenceFields.push_back(idx);
        } else if (fld.type == semantic::PrimitiveType::Struct &&
                   structContainsManagedReferences(fld.fieldTypeId)) {
            referenceFields.push_back(idx);
        }
    }

    return heap.allocateObject(
        type->typeId,
        std::move(fieldValues),
        std::move(referenceFields),
        &error);
}

std::optional<runtime::ObjectRef> ReflectionRegistry::instantiateUninitializedObject(
    runtime::ManagedHeap& heap,
    std::string_view typeName,
    runtime::RuntimeError& error) const {
    const auto* type = findType(typeName);
    if (!type) {
        error.code = runtime::ErrorCode::FunctionNotFound;
        error.message = "reflected type '" + std::string(typeName) + "' was not found";
        return std::nullopt;
    }
    return instantiateUninitializedObject(heap, type->typeId, error);
}

std::optional<runtime::ObjectRef> ReflectionRegistry::instantiateObject(
    runtime::EngineRuntime& runtime,
    semantic::SymbolId typeId,
    const std::vector<runtime::Value>& constructorArgs,
    runtime::RuntimeError& error,
    bool runConstructor,
    runtime::ExecutionOptions options) const {
    auto heap = runtime.heap();
    if (!heap) {
        error.code = runtime::ErrorCode::InvalidObjectReference;
        error.message = "EngineRuntime has no ManagedHeap";
        return std::nullopt;
    }
    bindHeap(heap);

    const auto* type = findType(typeId);
    if (!type) {
        error.code = runtime::ErrorCode::FunctionNotFound;
        error.message = "reflected typeId " + std::to_string(typeId) + " was not found";
        return std::nullopt;
    }

    auto allocated = instantiateUninitializedObject(*heap, typeId, error);
    if (!allocated) return std::nullopt;

    if (!runConstructor) {
        if (!constructorArgs.empty()) {
            error.code = runtime::ErrorCode::InvalidArguments;
            error.message = "constructor arguments supplied while runConstructor is false";
            return std::nullopt;
        }
        return allocated;
    }

    auto tempRoot = heap->retain(*allocated);
    if (!tempRoot.valid()) {
        error.code = runtime::ErrorCode::InvalidObjectReference;
        error.message = "failed to retain newly instantiated object during constructor";
        return std::nullopt;
    }

    if (type->staticConstructorSymbolId.has_value()) {
        auto cctorRes = runtime.invoke(*type->staticConstructorSymbolId, {}, options);
        if (!cctorRes.succeeded) {
            error = std::move(cctorRes.error);
            return std::nullopt;
        }
    }

    if (!constructorArgs.empty() || !type->constructors.empty()) {
        const MethodReflection* matchedCtor = nullptr;
        std::vector<runtime::Value> coercedArgs;
        for (const auto& ctor : type->constructors) {
            if (ctor.parameters.size() != constructorArgs.size()) continue;
            coercedArgs.clear();
            coercedArgs.reserve(constructorArgs.size());
            bool allMatch = true;
            runtime::RuntimeError matchErr;
            for (std::size_t i = 0; i < constructorArgs.size(); ++i) {
                runtime::Value coerced;
                if (!validateAndCoerceValue(
                        constructorArgs[i],
                        ctor.parameters[i].type,
                        ctor.parameters[i].typeId,
                        heap.get(),
                        coerced,
                        matchErr)) {
                    allMatch = false;
                    break;
                }
                coercedArgs.push_back(std::move(coerced));
            }
            if (allMatch) {
                matchedCtor = &ctor;
                break;
            }
        }
        if (!matchedCtor) {
            error.code = runtime::ErrorCode::InvalidArguments;
            error.message = "no matching constructor found on type '" + type->canonicalName + "'";
            return std::nullopt;
        }

        std::vector<runtime::Value> fullArgs;
        fullArgs.reserve(coercedArgs.size() + 1u);
        fullArgs.push_back(runtime::Value{*allocated});
        for (auto& arg : coercedArgs) {
            fullArgs.push_back(std::move(arg));
        }
        auto execRes = runtime.invoke(matchedCtor->symbolId, fullArgs, std::move(options));
        if (!execRes.succeeded) {
            error = std::move(execRes.error);
            return std::nullopt;
        }
    }

    return allocated;
}

std::optional<runtime::ObjectRef> ReflectionRegistry::instantiateObject(
    runtime::EngineRuntime& runtime,
    std::string_view typeName,
    const std::vector<runtime::Value>& constructorArgs,
    runtime::RuntimeError& error,
    bool runConstructor,
    runtime::ExecutionOptions options) const {
    const auto* type = findType(typeName);
    if (!type) {
        error.code = runtime::ErrorCode::FunctionNotFound;
        error.message = "reflected type '" + std::string(typeName) + "' was not found";
        return std::nullopt;
    }
    return instantiateObject(
        runtime, type->typeId, constructorArgs, error, runConstructor, std::move(options));
}

std::optional<runtime::Value> ReflectionRegistry::getField(
    const runtime::ManagedHeap& heap,
    runtime::ObjectRef object,
    std::string_view fieldName,
    runtime::RuntimeError& error) const {
    if (!object.valid() || object.kind != runtime::ObjectKind::Record ||
        object.heapId != static_cast<std::uint32_t>(heap.heapId()) ||
        !heap.isAlive(object)) {
        error.code = runtime::ErrorCode::InvalidObjectReference;
        error.message = "cannot read field from invalid or foreign ObjectRef";
        return std::nullopt;
    }
    const auto actualTid = heap.objectTypeId(object);
    const auto* type = actualTid ? findType(*actualTid) : nullptr;
    if (!type) {
        error.code = runtime::ErrorCode::TypeMismatch;
        error.message = "object runtime type is not registered in ReflectionRegistry";
        return std::nullopt;
    }
    const auto* field = type->findField(fieldName);
    if (!field) {
        error.code = runtime::ErrorCode::FunctionNotFound;
        error.message = "field '" + std::string(fieldName) + "' was not found on '" +
            type->canonicalName + "'";
        return std::nullopt;
    }
    if (field->isConst) {
        if (auto cv = evaluateConstantFieldValue(*field, nullptr, error)) {
            return cv;
        }
        return makeDefaultFieldValue(field->type, field->fieldTypeId);
    }
    if (field->isStatic) {
        if (auto sv = heap.getStaticField(field->id)) {
            return sv;
        }
        if (auto cv = evaluateConstantFieldValue(*field, nullptr, error)) {
            return cv;
        }
        return makeDefaultFieldValue(field->type, field->fieldTypeId);
    }
    auto val = heap.fieldGet(object, field->offsetIndex);
    if (!val) {
        error.code = runtime::ErrorCode::InvalidObjectReference;
        error.message = "failed to read field '" + field->name + "' at slot " +
            std::to_string(field->offsetIndex);
    }
    return val;
}

bool ReflectionRegistry::setField(
    runtime::ManagedHeap& heap,
    runtime::ObjectRef object,
    std::string_view fieldName,
    runtime::Value value,
    runtime::RuntimeError& error) const {
    if (!object.valid() || object.kind != runtime::ObjectKind::Record ||
        object.heapId != static_cast<std::uint32_t>(heap.heapId()) ||
        !heap.isAlive(object)) {
        error.code = runtime::ErrorCode::InvalidObjectReference;
        error.message = "cannot write field on invalid or foreign ObjectRef";
        return false;
    }
    const auto actualTid = heap.objectTypeId(object);
    const auto* type = actualTid ? findType(*actualTid) : nullptr;
    if (!type) {
        error.code = runtime::ErrorCode::TypeMismatch;
        error.message = "object runtime type is not registered in ReflectionRegistry";
        return false;
    }
    const auto* field = type->findField(fieldName);
    if (!field) {
        error.code = runtime::ErrorCode::FunctionNotFound;
        error.message = "field '" + std::string(fieldName) + "' was not found on '" +
            type->canonicalName + "'";
        return false;
    }
    if (field->isConst || field->isReadonly) {
        error.code = runtime::ErrorCode::InvalidArguments;
        error.message = "field '" + field->name + "' is constant or readonly";
        return false;
    }

    runtime::Value coerced;
    if (!validateAndCoerceValue(value, field->type, field->fieldTypeId, &heap, coerced, error)) {
        return false;
    }

    if (field->isStatic) {
        heap.setStaticField(field->id, std::move(coerced));
        return true;
    }
    return heap.fieldSet(object, field->offsetIndex, std::move(coerced), &error);
}

std::optional<runtime::Value> ReflectionRegistry::getField(
    const runtime::EngineRuntime& runtime,
    runtime::ObjectRef object,
    std::string_view fieldName,
    runtime::RuntimeError& error) const {
    if (!runtime.heap()) {
        error.code = runtime::ErrorCode::InvalidObjectReference;
        error.message = "EngineRuntime has no ManagedHeap";
        return std::nullopt;
    }
    bindHeap(runtime.heap());
    return getField(*runtime.heap(), object, fieldName, error);
}

bool ReflectionRegistry::setField(
    runtime::EngineRuntime& runtime,
    runtime::ObjectRef object,
    std::string_view fieldName,
    runtime::Value value,
    runtime::RuntimeError& error) const {
    if (!runtime.heap()) {
        error.code = runtime::ErrorCode::InvalidObjectReference;
        error.message = "EngineRuntime has no ManagedHeap";
        return false;
    }
    bindHeap(runtime.heap());
    return setField(*runtime.heap(), object, fieldName, std::move(value), error);
}

std::optional<runtime::Value> ReflectionRegistry::getStructField(
    const runtime::Value& structValue,
    std::string_view fieldName,
    runtime::RuntimeError& error) const {
    const auto tid = runtime::structValueTypeId(structValue);
    const auto* type = tid ? findType(*tid) : nullptr;
    if (!type || type->kind != semantic::TypeKind::Struct) {
        error.code = runtime::ErrorCode::TypeMismatch;
        error.message = "value is not a known reflected struct";
        return std::nullopt;
    }
    const auto* field = type->findField(fieldName);
    if (!field) {
        error.code = runtime::ErrorCode::FunctionNotFound;
        error.message = "struct field '" + std::string(fieldName) + "' was not found on '" +
            type->canonicalName + "'";
        return std::nullopt;
    }
    if (field->isConst) {
        if (auto cv = evaluateConstantFieldValue(*field, nullptr, error)) {
            return cv;
        }
        return makeDefaultFieldValue(field->type, field->fieldTypeId);
    }
    if (const auto* sv = std::get_if<runtime::StructValue>(&structValue)) {
        auto out = runtime::structValueFieldAt(*sv, field->offsetIndex);
        if (!out) {
            error.code = runtime::ErrorCode::IndexOutOfRange;
            error.message = "struct field slot out of range";
        }
        return out;
    }
    if (auto inlined = runtime::loadInlineStructField(structValue, field->offsetIndex)) {
        return inlined;
    }
    error.code = runtime::ErrorCode::TypeMismatch;
    error.message = "failed to extract inline struct field '" + field->name + "'";
    return std::nullopt;
}

bool ReflectionRegistry::setStructField(
    runtime::Value& structValue,
    std::string_view fieldName,
    runtime::Value value,
    runtime::RuntimeError& error,
    runtime::ManagedHeap* heap) const {
    const auto tid = runtime::structValueTypeId(structValue);
    const auto* type = tid ? findType(*tid) : nullptr;
    if (!type || type->kind != semantic::TypeKind::Struct) {
        error.code = runtime::ErrorCode::TypeMismatch;
        error.message = "value is not a known reflected struct";
        return false;
    }
    const auto* field = type->findField(fieldName);
    if (!field || field->isStatic || field->isConst || field->isReadonly) {
        error.code = field ? runtime::ErrorCode::InvalidArguments : runtime::ErrorCode::FunctionNotFound;
        error.message = field
            ? ("struct field '" + field->name + "' is not writable")
            : ("struct field '" + std::string(fieldName) + "' was not found");
        return false;
    }

    runtime::Value coerced;
    if (!validateAndCoerceValue(value, field->type, field->fieldTypeId, heap, coerced, error)) {
        return false;
    }

    if (const auto* sv = std::get_if<runtime::StructValue>(&structValue)) {
        const std::size_t count = sv->fieldCount();
        if (field->offsetIndex >= count) {
            error.code = runtime::ErrorCode::IndexOutOfRange;
            error.message = "struct field slot out of bounds";
            return false;
        }
        std::vector<runtime::Value> updated;
        updated.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            if (i == field->offsetIndex) {
                updated.push_back(coerced);
            } else {
                const auto cur = runtime::structValueFieldAt(*sv, i);
                updated.push_back(cur.value_or(runtime::Value{}));
            }
        }
        structValue = runtime::makeStructValueFromFields(*tid, std::move(updated));
        return true;
    }

    if (auto updatedInline = runtime::storeInlineStructField(structValue, field->offsetIndex, coerced)) {
        structValue = *updatedInline;
        return true;
    }
    error.code = runtime::ErrorCode::TypeMismatch;
    error.message = "failed to update inline struct field '" + field->name + "'";
    return false;
}

std::optional<runtime::Value> ReflectionRegistry::getField(
    const runtime::StructValue& structure,
    std::string_view fieldName,
    runtime::RuntimeError& error) const {
    return getStructField(runtime::Value{structure}, fieldName, error);
}

bool ReflectionRegistry::setField(
    runtime::StructValue& structure,
    std::string_view fieldName,
    runtime::Value value,
    runtime::RuntimeError& error,
    runtime::ManagedHeap* heap) const {
    runtime::Value wrapped{structure};
    if (!setStructField(wrapped, fieldName, std::move(value), error, heap)) {
        return false;
    }
    if (const auto* updated = std::get_if<runtime::StructValue>(&wrapped)) {
        structure = *updated;
        return true;
    }
    error.code = runtime::ErrorCode::TypeMismatch;
    error.message = "updated struct value did not produce StructValue";
    return false;
}

std::optional<runtime::Value> ReflectionRegistry::getStaticField(
    runtime::ManagedHeap& heap,
    std::string_view typeName,
    std::string_view fieldName,
    runtime::RuntimeError& error,
    const runtime::EngineRuntime* runtime) const {
    const auto* type = findType(typeName);
    if (!type) {
        error.code = runtime::ErrorCode::FunctionNotFound;
        error.message = "type '" + std::string(typeName) + "' was not found";
        return std::nullopt;
    }
    const auto* field = type->findField(fieldName);
    if (!field) {
        error.code = runtime::ErrorCode::FunctionNotFound;
        error.message = "static field '" + std::string(fieldName) + "' was not found on '" +
            type->canonicalName + "'";
        return std::nullopt;
    }
    if (field->isConst) {
        if (auto cv = evaluateConstantFieldValue(*field, &heap, error)) {
            return cv;
        }
        if (error.code != runtime::ErrorCode::None) return std::nullopt;
        return makeDefaultFieldValue(field->type, field->fieldTypeId);
    }
    if (!field->isStatic) {
        error.code = runtime::ErrorCode::InvalidArguments;
        error.message = "field '" + field->name + "' is an instance field, not static";
        return std::nullopt;
    }
    if (runtime && type->staticConstructorSymbolId.has_value()) {
        auto cctorRes = runtime->invoke(*type->staticConstructorSymbolId);
        if (!cctorRes.succeeded) {
            error = std::move(cctorRes.error);
            return std::nullopt;
        }
    }
    if (auto existing = heap.getStaticField(field->id)) {
        return existing;
    }
    if (auto cv = evaluateConstantFieldValue(*field, &heap, error)) {
        heap.setStaticField(field->id, *cv);
        return cv;
    }
    if (error.code != runtime::ErrorCode::None) return std::nullopt;
    return makeDefaultFieldValue(field->type, field->fieldTypeId);
}

bool ReflectionRegistry::setStaticField(
    runtime::ManagedHeap& heap,
    std::string_view typeName,
    std::string_view fieldName,
    runtime::Value value,
    runtime::RuntimeError& error,
    const runtime::EngineRuntime* runtime) const {
    const auto* type = findType(typeName);
    if (!type) {
        error.code = runtime::ErrorCode::FunctionNotFound;
        error.message = "type '" + std::string(typeName) + "' was not found";
        return false;
    }
    const auto* field = type->findField(fieldName);
    if (!field) {
        error.code = runtime::ErrorCode::FunctionNotFound;
        error.message = "static field '" + std::string(fieldName) + "' was not found on '" +
            type->canonicalName + "'";
        return false;
    }
    if (field->isConst || field->isReadonly) {
        error.code = runtime::ErrorCode::InvalidArguments;
        error.message = "static field '" + field->name + "' is constant or readonly";
        return false;
    }
    if (!field->isStatic) {
        error.code = runtime::ErrorCode::InvalidArguments;
        error.message = "field '" + field->name + "' is an instance field, not static";
        return false;
    }

    runtime::Value coerced;
    if (!validateAndCoerceValue(value, field->type, field->fieldTypeId, &heap, coerced, error)) {
        return false;
    }
    if (runtime && type->staticConstructorSymbolId.has_value()) {
        auto cctorRes = runtime->invoke(*type->staticConstructorSymbolId);
        if (!cctorRes.succeeded) {
            error = std::move(cctorRes.error);
            return false;
        }
    }
    heap.setStaticField(field->id, std::move(coerced));
    return true;
}

runtime::ExecutionResult ReflectionRegistry::invokeMethod(
    runtime::EngineRuntime& runtime,
    const MethodReflection& declaredMethod,
    runtime::ObjectRef receiver,
    const std::vector<runtime::Value>& arguments,
    runtime::ExecutionOptions options) const {
    auto heap = runtime.heap();
    if (!heap) {
        return makeFailedExecution(
            runtime::ErrorCode::InvalidObjectReference,
            "EngineRuntime has no ManagedHeap");
    }
    bindHeap(heap);

    if (declaredMethod.isStatic) {
        return invokeStaticMethod(
            runtime,
            declaredMethod.ownerTypeId != 0
                ? canonicalNameForTypeId(*program_, declaredMethod.ownerTypeId)
                : declaredMethod.moduleName,
            declaredMethod.name,
            arguments,
            std::move(options));
    }

    if (!receiver.valid() || receiver.kind != runtime::ObjectKind::Record ||
        receiver.heapId != static_cast<std::uint32_t>(heap->heapId()) ||
        !heap->isAlive(receiver)) {
        return makeFailedExecution(
            runtime::ErrorCode::InvalidObjectReference,
            "receiver ObjectRef is dead, invalid, or belongs to a foreign heap");
    }

    const auto actualTid = heap->objectTypeId(receiver);
    const auto* actualType = actualTid ? findType(*actualTid) : nullptr;
    if (!actualType) {
        return makeFailedExecution(
            runtime::ErrorCode::TypeMismatch,
            "receiver runtime type is not registered in ReflectionRegistry");
    }

    if (declaredMethod.ownerTypeId != 0 &&
        !isTypeAssignable(actualType->typeId, declaredMethod.ownerTypeId)) {
        return makeFailedExecution(
            runtime::ErrorCode::TypeMismatch,
            "receiver runtime type '" + actualType->canonicalName +
                "' is not assignable to declaring method owner");
    }

    if (arguments.size() != declaredMethod.parameters.size()) {
        return makeFailedExecution(
            runtime::ErrorCode::InvalidArguments,
            "method '" + declaredMethod.name + "' expected " +
                std::to_string(declaredMethod.parameters.size()) + " arguments, got " +
                std::to_string(arguments.size()));
    }

    semantic::SymbolId targetSymbolId = declaredMethod.symbolId;
    if (declaredMethod.isInterfaceMethod && declaredMethod.interfaceSlot.has_value()) {
        const auto slot = *declaredMethod.interfaceSlot;
        semantic::SymbolId resolved = 0;
        const TypeReflection* cursor = actualType;
        std::unordered_set<semantic::SymbolId> visited;
        while (cursor && visited.insert(cursor->typeId).second) {
            for (const auto& imap : cursor->interfaceDispatchMaps) {
                if (imap.interfaceTypeId == declaredMethod.ownerTypeId &&
                    slot < imap.slots.size()) {
                    resolved = imap.slots[slot];
                    break;
                }
            }
            if (resolved != 0) break;
            cursor = findType(cursor->baseTypeId);
        }
        if (resolved == 0) {
            return makeFailedExecution(
                runtime::ErrorCode::FunctionNotFound,
                "unresolved interface dispatch slot on runtime type '" + actualType->canonicalName + "'");
        }
        targetSymbolId = resolved;
    } else if ((declaredMethod.isVirtual || declaredMethod.isOverride || declaredMethod.isAbstract) &&
               declaredMethod.virtualSlot.has_value()) {
        const auto slot = *declaredMethod.virtualSlot;
        if (slot >= actualType->virtualDispatchTable.size() ||
            actualType->virtualDispatchTable[slot] == 0) {
            return makeFailedExecution(
                runtime::ErrorCode::FunctionNotFound,
                "unresolved virtual dispatch slot on runtime type '" + actualType->canonicalName + "'");
        }
        targetSymbolId = actualType->virtualDispatchTable[slot];
    }

    std::vector<runtime::Value> fullArgs;
    fullArgs.reserve(arguments.size() + 1u);
    fullArgs.push_back(runtime::Value{receiver});
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        runtime::Value coerced;
        runtime::RuntimeError argErr;
        if (!validateAndCoerceValue(
                arguments[i],
                declaredMethod.parameters[i].type,
                declaredMethod.parameters[i].typeId,
                heap.get(),
                coerced,
                argErr)) {
            return makeFailedExecution(argErr.code, std::move(argErr.message));
        }
        fullArgs.push_back(std::move(coerced));
    }

    return runtime.invoke(targetSymbolId, fullArgs, std::move(options));
}

runtime::ExecutionResult ReflectionRegistry::invokeMethod(
    runtime::EngineRuntime& runtime,
    runtime::ObjectRef receiver,
    std::string_view methodName,
    const std::vector<runtime::Value>& arguments,
    runtime::ExecutionOptions options) const {
    auto heap = runtime.heap();
    if (!heap) {
        return makeFailedExecution(
            runtime::ErrorCode::InvalidObjectReference,
            "EngineRuntime has no ManagedHeap");
    }
    bindHeap(heap);

    if (!receiver.valid() || receiver.kind != runtime::ObjectKind::Record ||
        receiver.heapId != static_cast<std::uint32_t>(heap->heapId()) ||
        !heap->isAlive(receiver)) {
        return makeFailedExecution(
            runtime::ErrorCode::InvalidObjectReference,
            "receiver ObjectRef is dead, invalid, or belongs to a foreign heap");
    }

    const auto actualTid = heap->objectTypeId(receiver);
    const TypeReflection* cursor = actualTid ? findType(*actualTid) : nullptr;
    if (!cursor) {
        return makeFailedExecution(
            runtime::ErrorCode::TypeMismatch,
            "receiver runtime type is not registered in ReflectionRegistry");
    }

    std::unordered_set<semantic::SymbolId> visited;
    while (cursor && visited.insert(cursor->typeId).second) {
        for (const auto& candidate : cursor->methods) {
            if (candidate.isStatic || candidate.name != methodName ||
                candidate.parameters.size() != arguments.size()) {
                continue;
            }
            bool match = true;
            for (std::size_t i = 0; i < arguments.size(); ++i) {
                runtime::Value dummy;
                runtime::RuntimeError err;
                if (!validateAndCoerceValue(
                        arguments[i],
                        candidate.parameters[i].type,
                        candidate.parameters[i].typeId,
                        heap.get(),
                        dummy,
                        err)) {
                    match = false;
                    break;
                }
            }
            if (match) {
                return invokeMethod(runtime, candidate, receiver, arguments, std::move(options));
            }
        }
        cursor = findType(cursor->baseTypeId);
    }

    return makeFailedExecution(
        runtime::ErrorCode::FunctionNotFound,
        "instance method '" + std::string(methodName) + "' with arity " +
            std::to_string(arguments.size()) + " was not found");
}

runtime::ExecutionResult ReflectionRegistry::invokeStaticMethod(
    runtime::EngineRuntime& runtime,
    std::string_view typeOrModuleName,
    std::string_view methodName,
    const std::vector<runtime::Value>& arguments,
    runtime::ExecutionOptions options) const {
    auto heap = runtime.heap();
    if (heap) bindHeap(heap);

    if (const auto* type = findType(typeOrModuleName)) {
        for (const auto& candidate : type->methods) {
            if (!candidate.isStatic || candidate.name != methodName ||
                candidate.parameters.size() != arguments.size()) {
                continue;
            }
            std::vector<runtime::Value> coercedArgs;
            coercedArgs.reserve(arguments.size());
            bool match = true;
            runtime::RuntimeError lastErr;
            for (std::size_t i = 0; i < arguments.size(); ++i) {
                runtime::Value coerced;
                if (!validateAndCoerceValue(
                        arguments[i],
                        candidate.parameters[i].type,
                        candidate.parameters[i].typeId,
                        heap.get(),
                        coerced,
                        lastErr)) {
                    match = false;
                    break;
                }
                coercedArgs.push_back(std::move(coerced));
            }
            if (!match) continue;

            if (type->staticConstructorSymbolId.has_value() &&
                *type->staticConstructorSymbolId != candidate.symbolId) {
                auto cctorRes = runtime.invoke(*type->staticConstructorSymbolId, {}, options);
                if (!cctorRes.succeeded) return cctorRes;
            }
            return runtime.invoke(candidate.symbolId, coercedArgs, std::move(options));
        }
    }

    for (const auto& fn : moduleFunctions_) {
        const bool moduleMatch = (fn.moduleName == typeOrModuleName && fn.name == methodName) ||
            (methodName.empty() && fn.qualifiedName == typeOrModuleName);
        if (!moduleMatch || fn.parameters.size() != arguments.size()) continue;

        std::vector<runtime::Value> coercedArgs;
        coercedArgs.reserve(arguments.size());
        bool match = true;
        runtime::RuntimeError lastErr;
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            runtime::Value coerced;
            if (!validateAndCoerceValue(
                    arguments[i],
                    fn.parameters[i].type,
                    fn.parameters[i].typeId,
                    heap.get(),
                    coerced,
                    lastErr)) {
                match = false;
                break;
            }
            coercedArgs.push_back(std::move(coerced));
        }
        if (match) {
            return runtime.invoke(fn.symbolId, coercedArgs, std::move(options));
        }
    }

    return makeFailedExecution(
        runtime::ErrorCode::FunctionNotFound,
        "static method or module function '" + std::string(typeOrModuleName) + "::" +
            std::string(methodName) + "' was not found or arguments did not match");
}

runtime::ExecutionResult ReflectionRegistry::invokeMethod(
    runtime::EngineRuntime& runtime,
    std::string_view typeOrModuleName,
    std::string_view methodName,
    const std::vector<runtime::Value>& arguments,
    runtime::ExecutionOptions options) const {
    return invokeStaticMethod(
        runtime, typeOrModuleName, methodName, arguments, std::move(options));
}

// ============================================================================
// Script-Side `RealScript.Reflect` Declaration Stub & Capability-Gated Bindings
// ============================================================================

compiler::SourceFile exportReflectDeclarationStub() {
    static const char* const kStub = R"RS(// Auto-generated RealScript Deterministic Reflection Module Stub (.d.rscript)
// External host bindings (bodiless `;` extern declarations gated by capability)
module RealScript.Reflect;

public string __rs_reflect_type_name_of<T>(T obj, ulong staticTypeId);
public bool __rs_reflect_has_type(string typeName);
public bool __rs_reflect_has_attribute(string target, string attrName);
public string __rs_reflect_get_attribute_string(string target, string attrName, string argKey);
public int __rs_reflect_get_attribute_int(string target, string attrName, string argKey, int fallback);
public double __rs_reflect_get_attribute_double(string target, string attrName, string argKey, double fallback);
public bool __rs_reflect_get_attribute_bool(string target, string attrName, string argKey, bool fallback);
public int __rs_reflect_get_field_count(string typeName);
public string __rs_reflect_get_field_name(string typeName, int index);
public int __rs_reflect_get_method_count(string typeName);
public string __rs_reflect_get_method_name(string typeName, int index);
public int __rs_reflect_get_int_field<T>(T obj, string fieldName);
public bool __rs_reflect_set_int_field<T>(T obj, string fieldName, int value);
public string __rs_reflect_get_string_field<T>(T obj, string fieldName);
public bool __rs_reflect_set_string_field<T>(T obj, string fieldName, string value);
public bool __rs_reflect_get_bool_field<T>(T obj, string fieldName);
public bool __rs_reflect_set_bool_field<T>(T obj, string fieldName, bool value);
public double __rs_reflect_get_double_field<T>(T obj, string fieldName);
public bool __rs_reflect_set_double_field<T>(T obj, string fieldName, double value);

public static class Reflect {
    public static string TypeNameOf<T>(T obj) {
        return __rs_reflect_type_name_of<T>(obj, typeof(T));
    }
    public static bool HasType(string typeName) {
        return __rs_reflect_has_type(typeName);
    }
    public static bool HasAttribute(string target, string attrName) {
        return __rs_reflect_has_attribute(target, attrName);
    }
    public static string GetAttributeString(string target, string attrName, string argKey) {
        return __rs_reflect_get_attribute_string(target, attrName, argKey);
    }
    public static int GetAttributeInt(string target, string attrName, string argKey, int fallback) {
        return __rs_reflect_get_attribute_int(target, attrName, argKey, fallback);
    }
    public static double GetAttributeDouble(string target, string attrName, string argKey, double fallback) {
        return __rs_reflect_get_attribute_double(target, attrName, argKey, fallback);
    }
    public static bool GetAttributeBool(string target, string attrName, string argKey, bool fallback) {
        return __rs_reflect_get_attribute_bool(target, attrName, argKey, fallback);
    }
    public static int GetFieldCount(string typeName) {
        return __rs_reflect_get_field_count(typeName);
    }
    public static string GetFieldName(string typeName, int index) {
        return __rs_reflect_get_field_name(typeName, index);
    }
    public static int GetMethodCount(string typeName) {
        return __rs_reflect_get_method_count(typeName);
    }
    public static string GetMethodName(string typeName, int index) {
        return __rs_reflect_get_method_name(typeName, index);
    }
    public static int GetIntField<T>(T obj, string fieldName) {
        return __rs_reflect_get_int_field<T>(obj, fieldName);
    }
    public static bool SetIntField<T>(T obj, string fieldName, int value) {
        return __rs_reflect_set_int_field<T>(obj, fieldName, value);
    }
    public static string GetStringField<T>(T obj, string fieldName) {
        return __rs_reflect_get_string_field<T>(obj, fieldName);
    }
    public static bool SetStringField<T>(T obj, string fieldName, string value) {
        return __rs_reflect_set_string_field<T>(obj, fieldName, value);
    }
    public static bool GetBoolField<T>(T obj, string fieldName) {
        return __rs_reflect_get_bool_field<T>(obj, fieldName);
    }
    public static bool SetBoolField<T>(T obj, string fieldName, bool value) {
        return __rs_reflect_set_bool_field<T>(obj, fieldName, value);
    }
    public static double GetDoubleField<T>(T obj, string fieldName) {
        return __rs_reflect_get_double_field<T>(obj, fieldName);
    }
    public static bool SetDoubleField<T>(T obj, string fieldName, double value) {
        return __rs_reflect_set_double_field<T>(obj, fieldName, value);
    }
}
)RS";
    return compiler::SourceFile{"RealScript.Reflect.d.rscript", kStub};
}

void registerReflectBindings(
    runtime::BindingRegistry& registry,
    std::shared_ptr<const ReflectionRegistry> reflection,
    std::shared_ptr<runtime::ManagedHeap> heap,
    std::string requiredCapability) {
    registerReflectBindings(
        registry,
        std::move(reflection),
        std::move(requiredCapability),
        std::move(heap));
}

void registerReflectBindings(
    runtime::BindingRegistry& registry,
    std::shared_ptr<const ReflectionRegistry> reflection,
    std::string requiredCapability,
    std::shared_ptr<runtime::ManagedHeap> heap) {
    if (!reflection) return;
    if (heap) {
        reflection->bindHeap(heap);
    }

    auto resolveHeapForObject = [reflection, heap](std::uint32_t objHeapId)
        -> std::shared_ptr<runtime::ManagedHeap> {
        if (heap && (objHeapId == 0 || static_cast<std::uint32_t>(heap->heapId()) == objHeapId)) {
            return heap;
        }
        return reflection->resolveBoundHeap(objHeapId);
    };

    const runtime::ExternalFunction hasTypeFn =
        [reflection, resolveHeapForObject](
            const bytecode::FunctionReference&,
            runtime::ValueSpan args,
            runtime::RuntimeError& err) -> std::optional<runtime::Value> {
            if (args.size() != 1u) {
                err.code = runtime::ErrorCode::InvalidArguments;
                err.message = "Reflect.HasType expects 1 argument";
                return std::nullopt;
            }
            const auto h = resolveHeapForObject(0);
            const auto typeName = extractStringArgument(args[0], h.get());
            if (!typeName) {
                err.code = runtime::ErrorCode::TypeMismatch;
                err.message = "Reflect.HasType expects a string typeName";
                return std::nullopt;
            }
            return runtime::Value{reflection->findType(*typeName) != nullptr};
        };

    const runtime::ExternalFunction hasAttrFn =
        [reflection, resolveHeapForObject](
            const bytecode::FunctionReference&,
            runtime::ValueSpan args,
            runtime::RuntimeError& err) -> std::optional<runtime::Value> {
            if (args.size() != 2u) {
                err.code = runtime::ErrorCode::InvalidArguments;
                err.message = "Reflect.HasAttribute expects 2 arguments";
                return std::nullopt;
            }
            const auto h = resolveHeapForObject(0);
            const auto target = extractStringArgument(args[0], h.get());
            const auto attrName = extractStringArgument(args[1], h.get());
            if (!target || !attrName) {
                err.code = runtime::ErrorCode::TypeMismatch;
                err.message = "Reflect.HasAttribute expects string arguments";
                return std::nullopt;
            }
            return runtime::Value{reflection->hasAttributeOnTarget(*target, *attrName)};
        };

    const runtime::ExternalFunction getAttrStringFn =
        [reflection, resolveHeapForObject](
            const bytecode::FunctionReference&,
            runtime::ValueSpan args,
            runtime::RuntimeError& err) -> std::optional<runtime::Value> {
            if (args.size() != 3u) {
                err.code = runtime::ErrorCode::InvalidArguments;
                err.message = "Reflect.GetAttributeString expects 3 arguments";
                return std::nullopt;
            }
            const auto h = resolveHeapForObject(0);
            const auto target = extractStringArgument(args[0], h.get());
            const auto attrName = extractStringArgument(args[1], h.get());
            const auto argKey = extractStringArgument(args[2], h.get());
            if (!target || !attrName || !argKey) {
                err.code = runtime::ErrorCode::TypeMismatch;
                err.message = "Reflect.GetAttributeString expects string arguments";
                return std::nullopt;
            }
            const auto* attr = reflection->findAttributeOnTarget(*target, *attrName);
            if (!attr) {
                return makeReturnStringValue("", h.get());
            }
            const std::string key = argKey->empty() ? "arg0" : *argKey;
            if (auto s = attr->getString(key)) {
                return makeReturnStringValue(std::move(*s), h.get());
            }
            return makeReturnStringValue("", h.get());
        };

    const runtime::ExternalFunction getAttrIntFn =
        [reflection, resolveHeapForObject](
            const bytecode::FunctionReference&,
            runtime::ValueSpan args,
            runtime::RuntimeError& err) -> std::optional<runtime::Value> {
            if (args.size() != 4u) {
                err.code = runtime::ErrorCode::InvalidArguments;
                err.message = "Reflect.GetAttributeInt expects 4 arguments";
                return std::nullopt;
            }
            const auto h = resolveHeapForObject(0);
            const auto target = extractStringArgument(args[0], h.get());
            const auto attrName = extractStringArgument(args[1], h.get());
            const auto argKey = extractStringArgument(args[2], h.get());
            std::int64_t fallback = 0;
            if (!target || !attrName || !argKey || !args[3].tryGetInt64(fallback)) {
                err.code = runtime::ErrorCode::TypeMismatch;
                err.message = "Reflect.GetAttributeInt received invalid arguments";
                return std::nullopt;
            }
            const auto* attr = reflection->findAttributeOnTarget(*target, *attrName);
            if (!attr) return runtime::Value{fallback};
            const std::string key = argKey->empty() ? "arg0" : *argKey;
            return runtime::Value{attr->getInt(key).value_or(fallback)};
        };

    const runtime::ExternalFunction getAttrDoubleFn =
        [reflection, resolveHeapForObject](
            const bytecode::FunctionReference&,
            runtime::ValueSpan args,
            runtime::RuntimeError& err) -> std::optional<runtime::Value> {
            if (args.size() != 4u) {
                err.code = runtime::ErrorCode::InvalidArguments;
                err.message = "Reflect.GetAttributeDouble expects 4 arguments";
                return std::nullopt;
            }
            const auto h = resolveHeapForObject(0);
            const auto target = extractStringArgument(args[0], h.get());
            const auto attrName = extractStringArgument(args[1], h.get());
            const auto argKey = extractStringArgument(args[2], h.get());
            double fallback = 0.0;
            if (!target || !attrName || !argKey || !args[3].tryGetDouble(fallback)) {
                err.code = runtime::ErrorCode::TypeMismatch;
                err.message = "Reflect.GetAttributeDouble received invalid arguments";
                return std::nullopt;
            }
            const auto* attr = reflection->findAttributeOnTarget(*target, *attrName);
            if (!attr) return runtime::Value{fallback};
            const std::string key = argKey->empty() ? "arg0" : *argKey;
            return runtime::Value{attr->getDouble(key).value_or(fallback)};
        };

    const runtime::ExternalFunction getAttrBoolFn =
        [reflection, resolveHeapForObject](
            const bytecode::FunctionReference&,
            runtime::ValueSpan args,
            runtime::RuntimeError& err) -> std::optional<runtime::Value> {
            if (args.size() != 4u) {
                err.code = runtime::ErrorCode::InvalidArguments;
                err.message = "Reflect.GetAttributeBool expects 4 arguments";
                return std::nullopt;
            }
            const auto h = resolveHeapForObject(0);
            const auto target = extractStringArgument(args[0], h.get());
            const auto attrName = extractStringArgument(args[1], h.get());
            const auto argKey = extractStringArgument(args[2], h.get());
            bool fallback = false;
            if (!target || !attrName || !argKey || !args[3].tryGetBool(fallback)) {
                err.code = runtime::ErrorCode::TypeMismatch;
                err.message = "Reflect.GetAttributeBool received invalid arguments";
                return std::nullopt;
            }
            const auto* attr = reflection->findAttributeOnTarget(*target, *attrName);
            if (!attr) return runtime::Value{fallback};
            const std::string key = argKey->empty() ? "arg0" : *argKey;
            return runtime::Value{attr->getBool(key).value_or(fallback)};
        };

    const runtime::ExternalFunction getFieldCountFn =
        [reflection, resolveHeapForObject](
            const bytecode::FunctionReference&,
            runtime::ValueSpan args,
            runtime::RuntimeError& err) -> std::optional<runtime::Value> {
            if (args.size() != 1u) {
                err.code = runtime::ErrorCode::InvalidArguments;
                err.message = "Reflect.GetFieldCount expects 1 argument";
                return std::nullopt;
            }
            const auto h = resolveHeapForObject(0);
            const auto typeName = extractStringArgument(args[0], h.get());
            if (!typeName) {
                err.code = runtime::ErrorCode::TypeMismatch;
                err.message = "Reflect.GetFieldCount expects a string typeName";
                return std::nullopt;
            }
            const auto* t = reflection->findType(*typeName);
            if (!t) return runtime::Value{std::int64_t{0}};
            return runtime::Value{static_cast<std::int64_t>(t->fields.size())};
        };

    const runtime::ExternalFunction getFieldNameFn =
        [reflection, resolveHeapForObject](
            const bytecode::FunctionReference&,
            runtime::ValueSpan args,
            runtime::RuntimeError& err) -> std::optional<runtime::Value> {
            if (args.size() != 2u) {
                err.code = runtime::ErrorCode::InvalidArguments;
                err.message = "Reflect.GetFieldName expects 2 arguments";
                return std::nullopt;
            }
            const auto h = resolveHeapForObject(0);
            const auto typeName = extractStringArgument(args[0], h.get());
            std::int64_t idx = -1;
            if (!typeName || !args[1].tryGetInt64(idx)) {
                err.code = runtime::ErrorCode::TypeMismatch;
                err.message = "Reflect.GetFieldName expects (string, int)";
                return std::nullopt;
            }
            const auto* t = reflection->findType(*typeName);
            if (!t || idx < 0 || static_cast<std::size_t>(idx) >= t->fields.size()) {
                err.code = runtime::ErrorCode::IndexOutOfRange;
                err.message = "Reflect.GetFieldName index out of range";
                return std::nullopt;
            }
            const auto& f = t->fields[static_cast<std::size_t>(idx)];
            return makeReturnStringValue(f.name, h.get());
        };

    const runtime::ExternalFunction getMethodCountFn =
        [reflection, resolveHeapForObject](
            const bytecode::FunctionReference&,
            runtime::ValueSpan args,
            runtime::RuntimeError& err) -> std::optional<runtime::Value> {
            if (args.size() != 1u) {
                err.code = runtime::ErrorCode::InvalidArguments;
                err.message = "Reflect.GetMethodCount expects 1 argument";
                return std::nullopt;
            }
            const auto h = resolveHeapForObject(0);
            const auto typeName = extractStringArgument(args[0], h.get());
            if (!typeName) {
                err.code = runtime::ErrorCode::TypeMismatch;
                err.message = "Reflect.GetMethodCount expects a string typeName";
                return std::nullopt;
            }
            const auto* t = reflection->findType(*typeName);
            if (!t) return runtime::Value{std::int64_t{0}};
            return runtime::Value{static_cast<std::int64_t>(t->methods.size())};
        };

    const runtime::ExternalFunction getMethodNameFn =
        [reflection, resolveHeapForObject](
            const bytecode::FunctionReference&,
            runtime::ValueSpan args,
            runtime::RuntimeError& err) -> std::optional<runtime::Value> {
            if (args.size() != 2u) {
                err.code = runtime::ErrorCode::InvalidArguments;
                err.message = "Reflect.GetMethodName expects 2 arguments";
                return std::nullopt;
            }
            const auto h = resolveHeapForObject(0);
            const auto typeName = extractStringArgument(args[0], h.get());
            std::int64_t idx = -1;
            if (!typeName || !args[1].tryGetInt64(idx)) {
                err.code = runtime::ErrorCode::TypeMismatch;
                err.message = "Reflect.GetMethodName expects (string, int)";
                return std::nullopt;
            }
            const auto* t = reflection->findType(*typeName);
            if (!t || idx < 0 || static_cast<std::size_t>(idx) >= t->methods.size()) {
                err.code = runtime::ErrorCode::IndexOutOfRange;
                err.message = "Reflect.GetMethodName index out of range";
                return std::nullopt;
            }
            return makeReturnStringValue(t->methods[static_cast<std::size_t>(idx)].name, h.get());
        };

    const runtime::ExternalFunction typeNameOfFn =
        [reflection, resolveHeapForObject](
            const bytecode::FunctionReference&,
            runtime::ValueSpan args,
            runtime::RuntimeError& err) -> std::optional<runtime::Value> {
            if (args.empty() || args.size() > 2u) {
                err.code = runtime::ErrorCode::InvalidArguments;
                err.message = "Reflect.TypeNameOf expects 1 or 2 arguments";
                return std::nullopt;
            }
            const auto& target = args[0];
            std::uint32_t objHeapId = 0;
            if (const auto* ref = std::get_if<runtime::ObjectRef>(&target)) {
                objHeapId = ref->heapId;
            }
            const auto h = resolveHeapForObject(objHeapId);

            if (std::holds_alternative<runtime::NullObject>(target) ||
                std::holds_alternative<runtime::NullString>(target) ||
                std::holds_alternative<runtime::NullArray>(target) ||
                std::holds_alternative<std::monostate>(target)) {
                return makeReturnStringValue("", h.get());
            }

            if (const auto* ref = std::get_if<runtime::ObjectRef>(&target)) {
                if (ref->kind == runtime::ObjectKind::String) {
                    return makeReturnStringValue("string", h.get());
                }
                if (ref->kind == runtime::ObjectKind::Record) {
                    if (h && h->isAlive(*ref)) {
                        if (const auto runtimeTid = h->objectTypeId(*ref)) {
                            if (const auto* tr = reflection->findType(*runtimeTid)) {
                                return makeReturnStringValue(tr->canonicalName, h.get());
                            }
                        }
                    }
                    if (args.size() == 2u) {
                        if (const auto* ul = std::get_if<runtime::ULongValue>(&args[1])) {
                            if (const auto* tr = reflection->findType(ul->value)) {
                                return makeReturnStringValue(tr->canonicalName, h.get());
                            }
                        }
                    }
                }
            }

            if (const auto stId = runtime::structValueTypeId(target)) {
                if (const auto* tr = reflection->findType(*stId)) {
                    return makeReturnStringValue(tr->canonicalName, h.get());
                }
            }
            if (runtime::EnumValue ev{}; target.tryGetEnum(ev)) {
                if (const auto* tr = reflection->findType(ev.typeId)) {
                    return makeReturnStringValue(tr->canonicalName, h.get());
                }
            }
            return makeReturnStringValue(
                std::string(semantic::primitiveTypeName(runtime::valueType(target))),
                h.get());
        };

    const runtime::ExternalFunction getIntFieldFn =
        [reflection, resolveHeapForObject](
            const bytecode::FunctionReference&,
            runtime::ValueSpan args,
            runtime::RuntimeError& err) -> std::optional<runtime::Value> {
            if (args.size() != 2u) {
                err.code = runtime::ErrorCode::InvalidArguments;
                err.message = "Reflect.GetIntField expects 2 arguments";
                return std::nullopt;
            }
            if (const auto stId = runtime::structValueTypeId(args[0])) {
                (void)stId;
                const auto h = resolveHeapForObject(0);
                const auto fname = extractStringArgument(args[1], h.get());
                if (!fname) {
                    err.code = runtime::ErrorCode::TypeMismatch;
                    err.message = "fieldName must be a string";
                    return std::nullopt;
                }
                const auto val = reflection->getStructField(args[0], *fname, err);
                if (!val) return std::nullopt;
                std::int64_t iv = 0;
                if (!val->tryGetInt64(iv)) {
                    err.code = runtime::ErrorCode::TypeMismatch;
                    err.message = "field '" + *fname + "' is not of type int";
                    return std::nullopt;
                }
                return runtime::Value{iv};
            }
            const auto* ref = std::get_if<runtime::ObjectRef>(&args[0]);
            if (!ref || ref->kind != runtime::ObjectKind::Record) {
                err.code = runtime::ErrorCode::NullReference;
                err.message = "Reflect.GetIntField requires a non-null object or struct";
                return std::nullopt;
            }
            const auto h = resolveHeapForObject(ref->heapId);
            if (!h) {
                err.code = runtime::ErrorCode::InvalidObjectReference;
                err.message = "ManagedHeap is not bound to ReflectionRegistry";
                return std::nullopt;
            }
            const auto fname = extractStringArgument(args[1], h.get());
            if (!fname) {
                err.code = runtime::ErrorCode::TypeMismatch;
                err.message = "fieldName must be a string";
                return std::nullopt;
            }
            const auto val = reflection->getField(*h, *ref, *fname, err);
            if (!val) return std::nullopt;
            std::int64_t iv = 0;
            if (!val->tryGetInt64(iv)) {
                err.code = runtime::ErrorCode::TypeMismatch;
                err.message = "field '" + *fname + "' is not of type int";
                return std::nullopt;
            }
            return runtime::Value{iv};
        };

    const runtime::ExternalFunction setIntFieldFn =
        [reflection, resolveHeapForObject](
            const bytecode::FunctionReference&,
            runtime::ValueSpan args,
            runtime::RuntimeError& err) -> std::optional<runtime::Value> {
            if (args.size() != 3u) {
                err.code = runtime::ErrorCode::InvalidArguments;
                err.message = "Reflect.SetIntField expects 3 arguments";
                return std::nullopt;
            }
            const auto* ref = std::get_if<runtime::ObjectRef>(&args[0]);
            if (!ref || ref->kind != runtime::ObjectKind::Record) {
                err.code = runtime::ErrorCode::NullReference;
                err.message = "Reflect.SetIntField requires a non-null class object";
                return std::nullopt;
            }
            const auto h = resolveHeapForObject(ref->heapId);
            if (!h) {
                err.code = runtime::ErrorCode::InvalidObjectReference;
                err.message = "ManagedHeap is not bound to ReflectionRegistry";
                return std::nullopt;
            }
            const auto fname = extractStringArgument(args[1], h.get());
            std::int64_t iv = 0;
            if (!fname || !args[2].tryGetInt64(iv)) {
                err.code = runtime::ErrorCode::TypeMismatch;
                err.message = "Reflect.SetIntField expects (obj, string, int)";
                return std::nullopt;
            }
            if (!reflection->setField(*h, *ref, *fname, runtime::Value{iv}, err)) {
                return std::nullopt;
            }
            return runtime::Value{true};
        };

    const runtime::ExternalFunction getStringFieldFn =
        [reflection, resolveHeapForObject](
            const bytecode::FunctionReference&,
            runtime::ValueSpan args,
            runtime::RuntimeError& err) -> std::optional<runtime::Value> {
            if (args.size() != 2u) {
                err.code = runtime::ErrorCode::InvalidArguments;
                err.message = "Reflect.GetStringField expects 2 arguments";
                return std::nullopt;
            }
            if (const auto stId = runtime::structValueTypeId(args[0])) {
                (void)stId;
                const auto h = resolveHeapForObject(0);
                const auto fname = extractStringArgument(args[1], h.get());
                if (!fname) {
                    err.code = runtime::ErrorCode::TypeMismatch;
                    err.message = "fieldName must be a string";
                    return std::nullopt;
                }
                const auto val = reflection->getStructField(args[0], *fname, err);
                if (!val) return std::nullopt;
                if (runtime::valueType(*val) != semantic::PrimitiveType::String) {
                    err.code = runtime::ErrorCode::TypeMismatch;
                    err.message = "field '" + *fname + "' is not of type string";
                    return std::nullopt;
                }
                const auto s = extractStringArgument(*val, h.get());
                return makeReturnStringValue(s.value_or(""), h.get());
            }
            const auto* ref = std::get_if<runtime::ObjectRef>(&args[0]);
            if (!ref || ref->kind != runtime::ObjectKind::Record) {
                err.code = runtime::ErrorCode::NullReference;
                err.message = "Reflect.GetStringField requires a non-null object or struct";
                return std::nullopt;
            }
            const auto h = resolveHeapForObject(ref->heapId);
            if (!h) {
                err.code = runtime::ErrorCode::InvalidObjectReference;
                err.message = "ManagedHeap is not bound to ReflectionRegistry";
                return std::nullopt;
            }
            const auto fname = extractStringArgument(args[1], h.get());
            if (!fname) {
                err.code = runtime::ErrorCode::TypeMismatch;
                err.message = "fieldName must be a string";
                return std::nullopt;
            }
            const auto val = reflection->getField(*h, *ref, *fname, err);
            if (!val) return std::nullopt;
            if (runtime::valueType(*val) != semantic::PrimitiveType::String) {
                err.code = runtime::ErrorCode::TypeMismatch;
                err.message = "field '" + *fname + "' is not of type string";
                return std::nullopt;
            }
            if (std::holds_alternative<runtime::NullString>(*val)) {
                return runtime::Value{runtime::NullString{}};
            }
            const auto s = extractStringArgument(*val, h.get());
            return makeReturnStringValue(s.value_or(""), h.get());
        };

    const runtime::ExternalFunction setStringFieldFn =
        [reflection, resolveHeapForObject](
            const bytecode::FunctionReference&,
            runtime::ValueSpan args,
            runtime::RuntimeError& err) -> std::optional<runtime::Value> {
            if (args.size() != 3u) {
                err.code = runtime::ErrorCode::InvalidArguments;
                err.message = "Reflect.SetStringField expects 3 arguments";
                return std::nullopt;
            }
            const auto* ref = std::get_if<runtime::ObjectRef>(&args[0]);
            if (!ref || ref->kind != runtime::ObjectKind::Record) {
                err.code = runtime::ErrorCode::NullReference;
                err.message = "Reflect.SetStringField requires a non-null class object";
                return std::nullopt;
            }
            const auto h = resolveHeapForObject(ref->heapId);
            if (!h) {
                err.code = runtime::ErrorCode::InvalidObjectReference;
                err.message = "ManagedHeap is not bound to ReflectionRegistry";
                return std::nullopt;
            }
            const auto fname = extractStringArgument(args[1], h.get());
            if (!fname) {
                err.code = runtime::ErrorCode::TypeMismatch;
                err.message = "fieldName must be a string";
                return std::nullopt;
            }
            if (!reflection->setField(*h, *ref, *fname, args[2], err)) {
                return std::nullopt;
            }
            return runtime::Value{true};
        };

    const runtime::ExternalFunction getBoolFieldFn =
        [reflection, resolveHeapForObject](
            const bytecode::FunctionReference&,
            runtime::ValueSpan args,
            runtime::RuntimeError& err) -> std::optional<runtime::Value> {
            if (args.size() != 2u) {
                err.code = runtime::ErrorCode::InvalidArguments;
                err.message = "Reflect.GetBoolField expects 2 arguments";
                return std::nullopt;
            }
            const auto* ref = std::get_if<runtime::ObjectRef>(&args[0]);
            if (!ref || ref->kind != runtime::ObjectKind::Record) {
                err.code = runtime::ErrorCode::NullReference;
                err.message = "Reflect.GetBoolField requires a non-null class object";
                return std::nullopt;
            }
            const auto h = resolveHeapForObject(ref->heapId);
            if (!h) {
                err.code = runtime::ErrorCode::InvalidObjectReference;
                err.message = "ManagedHeap is not bound to ReflectionRegistry";
                return std::nullopt;
            }
            const auto fname = extractStringArgument(args[1], h.get());
            if (!fname) {
                err.code = runtime::ErrorCode::TypeMismatch;
                err.message = "fieldName must be a string";
                return std::nullopt;
            }
            const auto val = reflection->getField(*h, *ref, *fname, err);
            if (!val) return std::nullopt;
            bool bv = false;
            if (!val->tryGetBool(bv)) {
                err.code = runtime::ErrorCode::TypeMismatch;
                err.message = "field '" + *fname + "' is not of type bool";
                return std::nullopt;
            }
            return runtime::Value{bv};
        };

    const runtime::ExternalFunction setBoolFieldFn =
        [reflection, resolveHeapForObject](
            const bytecode::FunctionReference&,
            runtime::ValueSpan args,
            runtime::RuntimeError& err) -> std::optional<runtime::Value> {
            if (args.size() != 3u) {
                err.code = runtime::ErrorCode::InvalidArguments;
                err.message = "Reflect.SetBoolField expects 3 arguments";
                return std::nullopt;
            }
            const auto* ref = std::get_if<runtime::ObjectRef>(&args[0]);
            if (!ref || ref->kind != runtime::ObjectKind::Record) {
                err.code = runtime::ErrorCode::NullReference;
                err.message = "Reflect.SetBoolField requires a non-null class object";
                return std::nullopt;
            }
            const auto h = resolveHeapForObject(ref->heapId);
            if (!h) {
                err.code = runtime::ErrorCode::InvalidObjectReference;
                err.message = "ManagedHeap is not bound to ReflectionRegistry";
                return std::nullopt;
            }
            const auto fname = extractStringArgument(args[1], h.get());
            bool bv = false;
            if (!fname || !args[2].tryGetBool(bv)) {
                err.code = runtime::ErrorCode::TypeMismatch;
                err.message = "Reflect.SetBoolField expects (obj, string, bool)";
                return std::nullopt;
            }
            if (!reflection->setField(*h, *ref, *fname, runtime::Value{bv}, err)) {
                return std::nullopt;
            }
            return runtime::Value{true};
        };

    const runtime::ExternalFunction getDoubleFieldFn =
        [reflection, resolveHeapForObject](
            const bytecode::FunctionReference&,
            runtime::ValueSpan args,
            runtime::RuntimeError& err) -> std::optional<runtime::Value> {
            if (args.size() != 2u) {
                err.code = runtime::ErrorCode::InvalidArguments;
                err.message = "Reflect.GetDoubleField expects 2 arguments";
                return std::nullopt;
            }
            const auto* ref = std::get_if<runtime::ObjectRef>(&args[0]);
            if (!ref || ref->kind != runtime::ObjectKind::Record) {
                err.code = runtime::ErrorCode::NullReference;
                err.message = "Reflect.GetDoubleField requires a non-null class object";
                return std::nullopt;
            }
            const auto h = resolveHeapForObject(ref->heapId);
            if (!h) {
                err.code = runtime::ErrorCode::InvalidObjectReference;
                err.message = "ManagedHeap is not bound to ReflectionRegistry";
                return std::nullopt;
            }
            const auto fname = extractStringArgument(args[1], h.get());
            if (!fname) {
                err.code = runtime::ErrorCode::TypeMismatch;
                err.message = "fieldName must be a string";
                return std::nullopt;
            }
            const auto val = reflection->getField(*h, *ref, *fname, err);
            if (!val) return std::nullopt;
            double dv = 0.0;
            if (!val->tryGetDouble(dv)) {
                err.code = runtime::ErrorCode::TypeMismatch;
                err.message = "field '" + *fname + "' is not of type double";
                return std::nullopt;
            }
            return runtime::Value{dv};
        };

    const runtime::ExternalFunction setDoubleFieldFn =
        [reflection, resolveHeapForObject](
            const bytecode::FunctionReference&,
            runtime::ValueSpan args,
            runtime::RuntimeError& err) -> std::optional<runtime::Value> {
            if (args.size() != 3u) {
                err.code = runtime::ErrorCode::InvalidArguments;
                err.message = "Reflect.SetDoubleField expects 3 arguments";
                return std::nullopt;
            }
            const auto* ref = std::get_if<runtime::ObjectRef>(&args[0]);
            if (!ref || ref->kind != runtime::ObjectKind::Record) {
                err.code = runtime::ErrorCode::NullReference;
                err.message = "Reflect.SetDoubleField requires a non-null class object";
                return std::nullopt;
            }
            const auto h = resolveHeapForObject(ref->heapId);
            if (!h) {
                err.code = runtime::ErrorCode::InvalidObjectReference;
                err.message = "ManagedHeap is not bound to ReflectionRegistry";
                return std::nullopt;
            }
            const auto fname = extractStringArgument(args[1], h.get());
            double dv = 0.0;
            if (!fname || !args[2].tryGetDouble(dv)) {
                err.code = runtime::ErrorCode::TypeMismatch;
                err.message = "Reflect.SetDoubleField expects (obj, string, double)";
                return std::nullopt;
            }
            if (!reflection->setField(*h, *ref, *fname, runtime::Value{dv}, err)) {
                return std::nullopt;
            }
            return runtime::Value{true};
        };

    const auto bindDet = [&](const std::string& name, const runtime::ExternalFunction& fn) {
        (void)registry.bind(
            name,
            fn,
            runtime::BindingDeterminism::Deterministic,
            requiredCapability);
    };

    // Bind fixed non-generic reflection intrinsics (both internal & direct Reflect.* names)
    bindDet("RealScript.Reflect::__rs_reflect_has_type", hasTypeFn);
    bindDet("RealScript.Reflect::Reflect.HasType", hasTypeFn);
    bindDet("RealScript.Reflect::__rs_reflect_has_attribute", hasAttrFn);
    bindDet("RealScript.Reflect::Reflect.HasAttribute", hasAttrFn);
    bindDet("RealScript.Reflect::__rs_reflect_get_attribute_string", getAttrStringFn);
    bindDet("RealScript.Reflect::Reflect.GetAttributeString", getAttrStringFn);
    bindDet("RealScript.Reflect::__rs_reflect_get_attribute_int", getAttrIntFn);
    bindDet("RealScript.Reflect::Reflect.GetAttributeInt", getAttrIntFn);
    bindDet("RealScript.Reflect::__rs_reflect_get_attribute_double", getAttrDoubleFn);
    bindDet("RealScript.Reflect::Reflect.GetAttributeDouble", getAttrDoubleFn);
    bindDet("RealScript.Reflect::__rs_reflect_get_attribute_bool", getAttrBoolFn);
    bindDet("RealScript.Reflect::Reflect.GetAttributeBool", getAttrBoolFn);
    bindDet("RealScript.Reflect::__rs_reflect_get_field_count", getFieldCountFn);
    bindDet("RealScript.Reflect::Reflect.GetFieldCount", getFieldCountFn);
    bindDet("RealScript.Reflect::__rs_reflect_get_field_name", getFieldNameFn);
    bindDet("RealScript.Reflect::Reflect.GetFieldName", getFieldNameFn);
    bindDet("RealScript.Reflect::__rs_reflect_get_method_count", getMethodCountFn);
    bindDet("RealScript.Reflect::Reflect.GetMethodCount", getMethodCountFn);
    bindDet("RealScript.Reflect::__rs_reflect_get_method_name", getMethodNameFn);
    bindDet("RealScript.Reflect::Reflect.GetMethodName", getMethodNameFn);

    // Scan linked ProgramImage functionReferences to bind all monomorphized generic specializations
    if (const auto prog = reflection->program()) {
        const auto matchAndBind = [&](const bytecode::FunctionReference& ref) {
            const std::string& n = ref.name;
            const auto registerRef = [&](const runtime::ExternalFunction& fn) {
                (void)registry.bind(
                    ref.symbolId,
                    fn,
                    runtime::BindingDeterminism::Deterministic,
                    requiredCapability);
                (void)registry.bind(
                    n,
                    fn,
                    runtime::BindingDeterminism::Deterministic,
                    requiredCapability);
            };
            if (n.rfind("RealScript.Reflect::__rs_reflect_type_name_of", 0) == 0 ||
                n.rfind("RealScript.Reflect::Reflect.TypeNameOf", 0) == 0) {
                registerRef(typeNameOfFn);
            } else if (n.rfind("RealScript.Reflect::__rs_reflect_get_int_field", 0) == 0 ||
                       n.rfind("RealScript.Reflect::Reflect.GetIntField", 0) == 0) {
                registerRef(getIntFieldFn);
            } else if (n.rfind("RealScript.Reflect::__rs_reflect_set_int_field", 0) == 0 ||
                       n.rfind("RealScript.Reflect::Reflect.SetIntField", 0) == 0) {
                registerRef(setIntFieldFn);
            } else if (n.rfind("RealScript.Reflect::__rs_reflect_get_string_field", 0) == 0 ||
                       n.rfind("RealScript.Reflect::Reflect.GetStringField", 0) == 0) {
                registerRef(getStringFieldFn);
            } else if (n.rfind("RealScript.Reflect::__rs_reflect_set_string_field", 0) == 0 ||
                       n.rfind("RealScript.Reflect::Reflect.SetStringField", 0) == 0) {
                registerRef(setStringFieldFn);
            } else if (n.rfind("RealScript.Reflect::__rs_reflect_get_bool_field", 0) == 0 ||
                       n.rfind("RealScript.Reflect::Reflect.GetBoolField", 0) == 0) {
                registerRef(getBoolFieldFn);
            } else if (n.rfind("RealScript.Reflect::__rs_reflect_set_bool_field", 0) == 0 ||
                       n.rfind("RealScript.Reflect::Reflect.SetBoolField", 0) == 0) {
                registerRef(setBoolFieldFn);
            } else if (n.rfind("RealScript.Reflect::__rs_reflect_get_double_field", 0) == 0 ||
                       n.rfind("RealScript.Reflect::Reflect.GetDoubleField", 0) == 0) {
                registerRef(getDoubleFieldFn);
            } else if (n.rfind("RealScript.Reflect::__rs_reflect_set_double_field", 0) == 0 ||
                       n.rfind("RealScript.Reflect::Reflect.SetDoubleField", 0) == 0) {
                registerRef(setDoubleFieldFn);
            } else if (n == "RealScript.Reflect::__rs_reflect_has_type" ||
                       n == "RealScript.Reflect::Reflect.HasType") {
                registerRef(hasTypeFn);
            } else if (n == "RealScript.Reflect::__rs_reflect_has_attribute" ||
                       n == "RealScript.Reflect::Reflect.HasAttribute") {
                registerRef(hasAttrFn);
            } else if (n == "RealScript.Reflect::__rs_reflect_get_attribute_string" ||
                       n == "RealScript.Reflect::Reflect.GetAttributeString") {
                registerRef(getAttrStringFn);
            } else if (n == "RealScript.Reflect::__rs_reflect_get_attribute_int" ||
                       n == "RealScript.Reflect::Reflect.GetAttributeInt") {
                registerRef(getAttrIntFn);
            } else if (n == "RealScript.Reflect::__rs_reflect_get_attribute_double" ||
                       n == "RealScript.Reflect::Reflect.GetAttributeDouble") {
                registerRef(getAttrDoubleFn);
            } else if (n == "RealScript.Reflect::__rs_reflect_get_attribute_bool" ||
                       n == "RealScript.Reflect::Reflect.GetAttributeBool") {
                registerRef(getAttrBoolFn);
            } else if (n == "RealScript.Reflect::__rs_reflect_get_field_count" ||
                       n == "RealScript.Reflect::Reflect.GetFieldCount") {
                registerRef(getFieldCountFn);
            } else if (n == "RealScript.Reflect::__rs_reflect_get_field_name" ||
                       n == "RealScript.Reflect::Reflect.GetFieldName") {
                registerRef(getFieldNameFn);
            } else if (n == "RealScript.Reflect::__rs_reflect_get_method_count" ||
                       n == "RealScript.Reflect::Reflect.GetMethodCount") {
                registerRef(getMethodCountFn);
            } else if (n == "RealScript.Reflect::__rs_reflect_get_method_name" ||
                       n == "RealScript.Reflect::Reflect.GetMethodName") {
                registerRef(getMethodNameFn);
            }
        };

        for (const auto& mod : prog->modules()) {
            for (const auto& fref : mod.functionReferences) {
                matchAndBind(fref);
            }
        }
    }
}

} // namespace realscript::reflection
