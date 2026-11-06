#include "realscript/game/GameplayMetadata.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <unordered_set>

namespace realscript::game {
namespace {

constexpr std::uint64_t FnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t FnvPrime = 1099511628211ULL;

void hashByte(std::uint64_t& hash, std::uint8_t value) noexcept {
    hash ^= value;
    hash *= FnvPrime;
}

void hashU64(std::uint64_t& hash, std::uint64_t value) noexcept {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        hashByte(hash, static_cast<std::uint8_t>(value >> shift));
    }
}

void hashString(std::uint64_t& hash, const std::string& value) noexcept {
    hashU64(hash, value.size());
    for (const auto character : value) {
        hashByte(hash, static_cast<std::uint8_t>(character));
    }
}

bool validAttribute(const ScriptAttribute& attribute) {
    return !attribute.name.empty() &&
        std::all_of(
            attribute.arguments.begin(), attribute.arguments.end(),
            [](const auto& entry) { return !entry.first.empty(); });
}

bool attributeLess(
    const ScriptAttribute& left,
    const ScriptAttribute& right) {
    return left.name < right.name;
}

bool normalizeAttributes(std::vector<ScriptAttribute>& attributes) {
    if (!std::all_of(attributes.begin(), attributes.end(), validAttribute)) {
        return false;
    }
    std::sort(attributes.begin(), attributes.end(), attributeLess);
    return std::adjacent_find(
        attributes.begin(), attributes.end(),
        [](const auto& left, const auto& right) {
            return left.name == right.name;
        }) == attributes.end();
}

} // namespace

bool ScriptMetadataRegistry::addTypeAttribute(
    std::string canonicalTypeName,
    ScriptAttribute attribute) {
    if (canonicalTypeName.empty() || !validAttribute(attribute)) return false;
    auto& attributes = typeAttributes_[std::move(canonicalTypeName)];
    const auto found = std::lower_bound(
        attributes.begin(), attributes.end(), attribute, attributeLess);
    if (found != attributes.end() && found->name == attribute.name) return false;
    attributes.insert(found, std::move(attribute));
    return true;
}

bool ScriptMetadataRegistry::addMemberAttribute(
    std::string canonicalTypeName,
    std::string memberName,
    ScriptAttribute attribute) {
    if (canonicalTypeName.empty() || memberName.empty() ||
        !validAttribute(attribute)) {
        return false;
    }
    auto& attributes = memberAttributes_[{
        std::move(canonicalTypeName), std::move(memberName)}];
    const auto found = std::lower_bound(
        attributes.begin(), attributes.end(), attribute, attributeLess);
    if (found != attributes.end() && found->name == attribute.name) return false;
    attributes.insert(found, std::move(attribute));
    return true;
}

namespace {

GameplayValue parseAttributeArgumentValue(const std::string& raw) {
    std::size_t begin = 0;
    std::size_t end = raw.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(raw[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(raw[end - 1]))) {
        --end;
    }
    const std::string trimmed = raw.substr(begin, end - begin);
    if (trimmed == "true") return true;
    if (trimmed == "false") return false;
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
    if (!trimmed.empty()) {
        char* parseEnd = nullptr;
        const long long intVal = std::strtoll(trimmed.c_str(), &parseEnd, 10);
        if (parseEnd && *parseEnd == '\0') {
            return static_cast<std::int64_t>(intVal);
        }
    }
    return trimmed;
}

bool ingestAttributeRecords(
    ScriptMetadataRegistry& registry,
    const std::vector<compiler::LanguageAttributeRecord>& records,
    std::size_t& count) {
    static const char* const kMemberKinds[] = {
        "::field:",
        "::property:",
        "::method:",
        "::ctor:",
        "::enum:",
    };
    for (const auto& record : records) {
        ScriptAttribute attribute;
        attribute.name = record.name;
        for (const auto& arg : record.arguments) {
            attribute.arguments[arg.name] = parseAttributeArgumentValue(arg.value);
        }

        bool matchedMember = false;
        for (const char* marker : kMemberKinds) {
            const auto pos = record.target.find(marker);
            if (pos != std::string::npos) {
                matchedMember = true;
                std::string owner = record.target.substr(0, pos);
                std::string member = record.target.substr(pos + std::strlen(marker));
                const auto arityPos = member.find('#');
                if (arityPos != std::string::npos) {
                    member = member.substr(0, arityPos);
                }
                if (registry.addMemberAttribute(std::move(owner), std::move(member), std::move(attribute))) {
                    ++count;
                }
                break;
            }
        }
        if (!matchedMember) {
            if (registry.addTypeAttribute(record.target, std::move(attribute))) {
                ++count;
            }
        }
    }
    return true;
}

} // namespace

std::size_t ScriptMetadataRegistry::populateFromLanguageMetadata(
    const GameLanguageMetadata& metadata) {
    std::size_t added = 0;
    ingestAttributeRecords(*this, metadata.attributes, added);
    return added;
}

std::size_t ScriptMetadataRegistry::populateFromProgramImage(
    const runtime::ProgramImage& program) {
    std::size_t added = 0;
    for (const auto& module : program.modules()) {
        ingestAttributeRecords(*this, module.languageMetadata.attributes, added);
    }
    return added;
}

std::size_t ScriptMetadataRegistry::populateFromProgram(
    const GameProgram& program) {
    std::size_t added = populateFromLanguageMetadata(program.languageMetadata());
    if (program.program()) {
        added += populateFromProgramImage(*program.program());
    }
    return added;
}

const std::vector<ScriptAttribute>* ScriptMetadataRegistry::typeAttributes(
    const std::string& canonicalTypeName) const noexcept {
    const auto found = typeAttributes_.find(canonicalTypeName);
    return found == typeAttributes_.end() ? nullptr : &found->second;
}

const std::vector<ScriptAttribute>* ScriptMetadataRegistry::memberAttributes(
    const std::string& canonicalTypeName,
    const std::string& memberName) const noexcept {
    const auto found = memberAttributes_.find({canonicalTypeName, memberName});
    return found == memberAttributes_.end() ? nullptr : &found->second;
}

const ScriptAttribute* ScriptMetadataRegistry::findTypeAttribute(
    const std::string& canonicalTypeName,
    const std::string& attributeName) const noexcept {
    const auto* attributes = typeAttributes(canonicalTypeName);
    if (!attributes) return nullptr;
    const auto found = std::lower_bound(
        attributes->begin(), attributes->end(), ScriptAttribute{attributeName, {}},
        attributeLess);
    return found == attributes->end() || found->name != attributeName
        ? nullptr
        : &*found;
}

const ScriptAttribute* ScriptMetadataRegistry::findMemberAttribute(
    const std::string& canonicalTypeName,
    const std::string& memberName,
    const std::string& attributeName) const noexcept {
    const auto* attributes = memberAttributes(canonicalTypeName, memberName);
    if (!attributes) return nullptr;
    const auto found = std::lower_bound(
        attributes->begin(), attributes->end(), ScriptAttribute{attributeName, {}},
        attributeLess);
    return found == attributes->end() || found->name != attributeName
        ? nullptr
        : &*found;
}

ScriptMetadataRegistry::State ScriptMetadataRegistry::snapshot() const {
    State state;
    state.typeAttributes = typeAttributes_;
    state.members.reserve(memberAttributes_.size());
    for (const auto& entry : memberAttributes_) {
        state.members.push_back(ScriptMemberMetadata{
            entry.first.first, entry.first.second, entry.second});
    }
    return state;
}

bool ScriptMetadataRegistry::restore(const State& state) {
    std::map<std::string, std::vector<ScriptAttribute>> typeAttributes;
    for (auto entry : state.typeAttributes) {
        if (entry.first.empty() || !normalizeAttributes(entry.second)) return false;
        typeAttributes.emplace(std::move(entry));
    }

    std::map<std::pair<std::string, std::string>, std::vector<ScriptAttribute>>
        memberAttributes;
    for (auto member : state.members) {
        if (member.canonicalTypeName.empty() || member.memberName.empty() ||
            !normalizeAttributes(member.attributes) ||
            !memberAttributes.emplace(
                std::make_pair(
                    std::move(member.canonicalTypeName),
                    std::move(member.memberName)),
                std::move(member.attributes)).second) {
            return false;
        }
    }
    typeAttributes_ = std::move(typeAttributes);
    memberAttributes_ = std::move(memberAttributes);
    return true;
}

std::uint64_t ScriptMetadataRegistry::stableHash() const noexcept {
    std::uint64_t hash = FnvOffset;
    const auto hashAttributes = [&hash](const auto& attributes) {
        hashU64(hash, attributes.size());
        for (const auto& attribute : attributes) {
            hashString(hash, attribute.name);
            hashU64(hash, attribute.arguments.size());
            for (const auto& argument : attribute.arguments) {
                hashString(hash, argument.first);
                hashU64(hash, stableGameplayValueHash(argument.second));
            }
        }
    };

    hashU64(hash, typeAttributes_.size());
    for (const auto& entry : typeAttributes_) {
        hashString(hash, entry.first);
        hashAttributes(entry.second);
    }
    hashU64(hash, memberAttributes_.size());
    for (const auto& entry : memberAttributes_) {
        hashString(hash, entry.first.first);
        hashString(hash, entry.first.second);
        hashAttributes(entry.second);
    }
    return hash;
}

ScriptContractReport validateScriptContract(
    const ScriptRuntime& runtime,
    const std::string& canonicalTypeName,
    const ScriptContract& contract) {
    ScriptContractReport report;
    const auto type = runtime.findType(canonicalTypeName);
    if (!type) {
        report.violations.push_back(ScriptContractViolation{
            {}, 0, "script type '" + canonicalTypeName + "' was not found"});
        return report;
    }
    report.typeFound = true;
    for (const auto& callback : contract.callbacks) {
        if (!callback.required) continue;
        const auto method = runtime.findMethod(*type, callback.name, callback.arity);
        if (!method || !method->instance()) {
            report.violations.push_back(ScriptContractViolation{
                callback.name,
                callback.arity,
                "contract '" + contract.name + "' requires instance callback '" +
                    callback.name + "' with arity " +
                    std::to_string(callback.arity)});
        }
    }
    return report;
}

ScriptContract sceneBehaviorContract() {
    return ScriptContract{
        "SceneBehavior",
        {
            {"OnStart", 0, false},
            {"OnFixedUpdate", 1, false},
            {"OnDestroy", 0, false},
        }};
}

namespace {

const compiler::LanguageGenericInstantiation* findGenericInstantiation(
    const runtime::ProgramImage* program,
    const std::string& typeName) {
    if (!program || typeName.empty()) return nullptr;
    std::string simpleName = typeName;
    const auto sep = simpleName.rfind("::");
    if (sep != std::string::npos) {
        simpleName = simpleName.substr(sep + 2);
    }
    for (const auto& module : program->modules()) {
        for (const auto& inst : module.languageMetadata.genericInstantiations) {
            if (inst.generatedName == simpleName ||
                (module.name + "::" + inst.generatedName) == typeName) {
                return &inst;
            }
        }
    }
    return nullptr;
}

bool isBuiltinCollectionGenericName(const std::string& name) {
    return name == "List" || name == "Queue" || name == "Stack" ||
        name == "HashSet" || name == "Dictionary";
}

InspectorFieldSchema buildInspectorFieldSchema(
    const ScriptRuntime& runtime,
    const ScriptMetadataRegistry& metadata,
    const std::string& ownerCanonicalTypeName,
    const semantic::FieldSymbol& field,
    const ScriptObject* prototype,
    const runtime::StructValue* parentStructValue,
    std::size_t fieldIndexInParent,
    std::unordered_set<std::string>& visitedStructs) {
    const bool hasExport =
        metadata.findMemberAttribute(ownerCanonicalTypeName, field.name, "Export") != nullptr ||
        metadata.findMemberAttribute(ownerCanonicalTypeName, field.name, "SerializeField") != nullptr;
    const bool isPublic =
        field.accessibility == semantic::Accessibility::Public;
    const bool hidden =
        metadata.findMemberAttribute(ownerCanonicalTypeName, field.name, "HideInInspector") != nullptr;

    InspectorFieldSchema fieldSchema;
    fieldSchema.name = field.name;
    fieldSchema.type = field.type;
    fieldSchema.typeName = field.typeName;
    fieldSchema.exported = hasExport || isPublic;
    fieldSchema.hiddenInInspector = hidden;

    runtime::RuntimeError error;
    if (parentStructValue && parentStructValue->storage &&
        fieldIndexInParent < parentStructValue->storage->fields.size()) {
        fieldSchema.defaultValue = parentStructValue->storage->fields[fieldIndexInParent];
    } else if (prototype) {
        if (auto val = runtime.getMember(*prototype, field.name, error)) {
            fieldSchema.defaultValue = std::move(*val);
        }
    }

    if (const auto* rangeAttr = metadata.findMemberAttribute(
            ownerCanonicalTypeName, field.name, "Range")) {
        const auto readIntArg = [&](const char* named, const char* positional) -> std::optional<std::int64_t> {
            auto it = rangeAttr->arguments.find(named);
            if (it == rangeAttr->arguments.end()) {
                it = rangeAttr->arguments.find(positional);
            }
            if (it != rangeAttr->arguments.end()) {
                if (const auto* v = std::get_if<std::int64_t>(&it->second)) return *v;
            }
            return std::nullopt;
        };
        fieldSchema.rangeMin = readIntArg("min", "arg0");
        fieldSchema.rangeMax = readIntArg("max", "arg1");
    }

    if (const auto* tooltipAttr = metadata.findMemberAttribute(
            ownerCanonicalTypeName, field.name, "Tooltip")) {
        auto it = tooltipAttr->arguments.find("text");
        if (it == tooltipAttr->arguments.end()) {
            it = tooltipAttr->arguments.find("arg0");
        }
        if (it != tooltipAttr->arguments.end()) {
            if (const auto* v = std::get_if<std::string>(&it->second)) {
                fieldSchema.tooltip = *v;
            }
        }
    }

    if (const auto* renamedAttr = metadata.findMemberAttribute(
            ownerCanonicalTypeName, field.name, "FormerlySerializedAs")) {
        auto it = renamedAttr->arguments.find("oldName");
        if (it == renamedAttr->arguments.end()) {
            it = renamedAttr->arguments.find("arg0");
        }
        if (it != renamedAttr->arguments.end()) {
            if (const auto* v = std::get_if<std::string>(&it->second)) {
                fieldSchema.formerlySerializedAs = *v;
            }
        }
    }

    // Reference attributes: [EntityRef], [ComponentRef], [AssetRef]
    const auto readRefTypeArg = [](const ScriptAttribute& attr) -> std::string {
        for (const char* key : {"type", "Type", "arg0"}) {
            const auto it = attr.arguments.find(key);
            if (it != attr.arguments.end()) {
                if (const auto* v = std::get_if<std::string>(&it->second)) {
                    return *v;
                }
            }
        }
        return {};
    };
    if (const auto* entityAttr = metadata.findMemberAttribute(
            ownerCanonicalTypeName, field.name, "EntityRef")) {
        fieldSchema.referenceKind = InspectorReferenceKind::Entity;
        fieldSchema.referencedTypeName = readRefTypeArg(*entityAttr);
        if (fieldSchema.referencedTypeName.empty()) {
            fieldSchema.referencedTypeName = "Entity";
        }
    } else if (const auto* assetAttr = metadata.findMemberAttribute(
                   ownerCanonicalTypeName, field.name, "AssetRef")) {
        fieldSchema.referenceKind = InspectorReferenceKind::Asset;
        fieldSchema.referencedTypeName = readRefTypeArg(*assetAttr);
        if (fieldSchema.referencedTypeName.empty()) {
            fieldSchema.referencedTypeName = field.typeName.empty() ? "Asset" : field.typeName;
        }
    } else if (const auto* compAttr = metadata.findMemberAttribute(
                   ownerCanonicalTypeName, field.name, "ComponentRef")) {
        fieldSchema.referenceKind = InspectorReferenceKind::Component;
        fieldSchema.referencedTypeName = readRefTypeArg(*compAttr);
        if (fieldSchema.referencedTypeName.empty()) {
            fieldSchema.referencedTypeName = field.typeName;
        }
    }

    // Enum dropdown options
    if (field.type == semantic::PrimitiveType::Enum && !field.typeName.empty()) {
        if (const auto enumType = runtime.findType(field.typeName)) {
            for (const auto& member : enumType->descriptor.enumMembers) {
                fieldSchema.enumOptions.push_back(InspectorEnumOption{
                    member.name,
                    member.value,
                });
            }
        }
    }

    // Array / Collection inspection
    if (field.type == semantic::PrimitiveType::Array) {
        fieldSchema.isCollection = true;
        semantic::PrimitiveType elemPrim = semantic::PrimitiveType::Error;
        std::string elemExact;
        if (semantic::decodeArrayTypeName(field.typeName, elemPrim, elemExact)) {
            fieldSchema.elementTypeName = elemExact.empty()
                ? std::string(semantic::primitiveTypeName(elemPrim))
                : elemExact;
        }
    } else if (field.type == semantic::PrimitiveType::Object && !field.typeName.empty()) {
        if (const auto* inst = findGenericInstantiation(runtime.program().get(), field.typeName)) {
            if (isBuiltinCollectionGenericName(inst->genericName)) {
                fieldSchema.isCollection = true;
                if (inst->genericName == "Dictionary" && inst->arguments.size() >= 2) {
                    fieldSchema.keyTypeName = inst->arguments[0];
                    fieldSchema.elementTypeName = inst->arguments[1];
                } else if (!inst->arguments.empty()) {
                    fieldSchema.elementTypeName = inst->arguments[0];
                }
            }
        }
        if (!fieldSchema.isCollection &&
            fieldSchema.referenceKind == InspectorReferenceKind::None) {
            if (const auto refType = runtime.findType(field.typeName)) {
                if (!refType->descriptor.delegateType) {
                    fieldSchema.referenceKind = InspectorReferenceKind::Component;
                    fieldSchema.referencedTypeName = refType->canonicalName();
                }
            }
        }
    }

    // Nested struct fields
    if (field.type == semantic::PrimitiveType::Struct && !field.typeName.empty() &&
        visitedStructs.insert(field.typeName).second) {
        if (const auto structType = runtime.findType(field.typeName)) {
            const auto* structVal = std::get_if<runtime::StructValue>(&fieldSchema.defaultValue);
            for (std::size_t idx = 0; idx < structType->descriptor.fields.size(); ++idx) {
                const auto& subField = structType->descriptor.fields[idx];
                if (subField.synthetic) continue;
                fieldSchema.nestedFields.push_back(buildInspectorFieldSchema(
                    runtime,
                    metadata,
                    structType->canonicalName(),
                    subField,
                    nullptr,
                    structVal,
                    idx,
                    visitedStructs));
            }
        }
        visitedStructs.erase(field.typeName);
    }

    return fieldSchema;
}

} // namespace

std::optional<InspectorTypeSchema> inspectTypeSchema(
    const ScriptRuntime& runtime,
    const ScriptMetadataRegistry& metadata,
    const std::string& canonicalTypeName) {
    const auto type = runtime.findType(canonicalTypeName);
    if (!type) return std::nullopt;

    InspectorTypeSchema schema;
    schema.canonicalTypeName = type->canonicalName();
    schema.executeInEditMode =
        metadata.findTypeAttribute(schema.canonicalTypeName, "Tool") != nullptr ||
        metadata.findTypeAttribute(schema.canonicalTypeName, "ExecuteInEditMode") != nullptr;
    schema.hasOnValidate =
        runtime.findMethod(*type, "OnValidate", 0).has_value();

    runtime::RuntimeError error;
    auto prototype = runtime.createObject(*type, {}, error);

    std::unordered_set<std::string> visitedStructs;
    std::unordered_set<std::string> recordedSignals;

    for (const auto& field : type->descriptor.fields) {
        // Detect synthetic event delegate fields: $event_<SignalName>_delegate
        if (field.synthetic) {
            static constexpr const char* kEventPrefix = "$event_";
            static constexpr const char* kEventSuffix = "_delegate";
            const std::size_t prefixLen = std::strlen(kEventPrefix);
            const std::size_t suffixLen = std::strlen(kEventSuffix);
            if (field.name.size() > prefixLen + suffixLen &&
                field.name.rfind(kEventPrefix, 0) == 0 &&
                field.name.compare(
                    field.name.size() - suffixLen, suffixLen, kEventSuffix) == 0) {
                const std::string signalName = field.name.substr(
                    prefixLen, field.name.size() - prefixLen - suffixLen);
                if (recordedSignals.insert(signalName).second) {
                    InspectorSignalSchema sig;
                    sig.name = signalName;
                    sig.delegateTypeName = field.typeName;
                    if (const auto delType = runtime.findType(field.typeName)) {
                        for (const auto& method : delType->descriptor.methods) {
                            if (method.name == "Invoke") {
                                const std::size_t startIdx =
                                    (method.method && !method.staticMethod &&
                                     !method.parameters.empty())
                                    ? 1u
                                    : 0u;
                                for (std::size_t i = startIdx;
                                     i < method.parameters.size();
                                     ++i) {
                                    sig.parameters.push_back(InspectorSignalParameter{
                                        method.parameters[i].name,
                                        method.parameters[i].type,
                                        method.parameters[i].typeName,
                                    });
                                }
                                break;
                            }
                        }
                    }
                    schema.signals.push_back(std::move(sig));
                }
            }
            continue;
        }
        if (field.name == "__native") continue;

        schema.fields.push_back(buildInspectorFieldSchema(
            runtime,
            metadata,
            schema.canonicalTypeName,
            field,
            prototype ? &*prototype : nullptr,
            nullptr,
            0,
            visitedStructs));
    }

    return schema;
}

} // namespace realscript::game
