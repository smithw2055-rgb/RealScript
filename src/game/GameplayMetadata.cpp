#include "realscript/game/GameplayMetadata.h"

#include <algorithm>

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

} // namespace realscript::game
