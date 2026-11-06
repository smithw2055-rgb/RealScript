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

    runtime::RuntimeError error;
    auto prototype = runtime.createObject(*type, {}, error);

    for (const auto& field : type->descriptor.fields) {
        if (field.synthetic || field.name == "__native") continue;

        const bool hasExport =
            metadata.findMemberAttribute(schema.canonicalTypeName, field.name, "Export") != nullptr ||
            metadata.findMemberAttribute(schema.canonicalTypeName, field.name, "SerializeField") != nullptr;
        const bool isPublic =
            field.accessibility == semantic::Accessibility::Public;
        const bool hidden =
            metadata.findMemberAttribute(schema.canonicalTypeName, field.name, "HideInInspector") != nullptr;

        InspectorFieldSchema fieldSchema;
        fieldSchema.name = field.name;
        fieldSchema.type = field.type;
        fieldSchema.typeName = field.typeName;
        fieldSchema.exported = hasExport || isPublic;
        fieldSchema.hiddenInInspector = hidden;

        if (prototype) {
            if (auto val = runtime.getMember(*prototype, field.name, error)) {
                fieldSchema.defaultValue = std::move(*val);
            }
        }

        if (const auto* rangeAttr = metadata.findMemberAttribute(
                schema.canonicalTypeName, field.name, "Range")) {
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
                schema.canonicalTypeName, field.name, "Tooltip")) {
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
                schema.canonicalTypeName, field.name, "FormerlySerializedAs")) {
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

        schema.fields.push_back(std::move(fieldSchema));
    }

    return schema;
}

} // namespace realscript::game
