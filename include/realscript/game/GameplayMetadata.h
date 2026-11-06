#pragma once

#include "realscript/game/GameplayPrimitives.h"
#include "realscript/game/GameScripting.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace realscript::game {

struct ScriptAttribute {
    std::string name;
    std::map<std::string, GameplayValue> arguments;
};

struct ScriptMemberMetadata {
    std::string canonicalTypeName;
    std::string memberName;
    std::vector<ScriptAttribute> attributes;
};

class ScriptMetadataRegistry {
public:
    struct State {
        std::map<std::string, std::vector<ScriptAttribute>> typeAttributes;
        std::vector<ScriptMemberMetadata> members;
    };

    bool addTypeAttribute(
        std::string canonicalTypeName,
        ScriptAttribute attribute);
    bool addMemberAttribute(
        std::string canonicalTypeName,
        std::string memberName,
        ScriptAttribute attribute);
    std::size_t populateFromLanguageMetadata(
        const GameLanguageMetadata& metadata);
    std::size_t populateFromProgramImage(
        const runtime::ProgramImage& program);
    std::size_t populateFromProgram(
        const GameProgram& program);

    [[nodiscard]] const std::vector<ScriptAttribute>* typeAttributes(
        const std::string& canonicalTypeName) const noexcept;
    [[nodiscard]] const std::vector<ScriptAttribute>* memberAttributes(
        const std::string& canonicalTypeName,
        const std::string& memberName) const noexcept;
    [[nodiscard]] const ScriptAttribute* findTypeAttribute(
        const std::string& canonicalTypeName,
        const std::string& attributeName) const noexcept;
    [[nodiscard]] const ScriptAttribute* findMemberAttribute(
        const std::string& canonicalTypeName,
        const std::string& memberName,
        const std::string& attributeName) const noexcept;

    [[nodiscard]] State snapshot() const;
    bool restore(const State& state);
    [[nodiscard]] std::uint64_t stableHash() const noexcept;

private:
    std::map<std::string, std::vector<ScriptAttribute>> typeAttributes_;
    std::map<std::pair<std::string, std::string>, std::vector<ScriptAttribute>>
        memberAttributes_;
};

struct ScriptCallbackRequirement {
    std::string name;
    std::size_t arity = 0;
    bool required = true;
};

struct ScriptContract {
    std::string name;
    std::vector<ScriptCallbackRequirement> callbacks;
};

struct ScriptContractViolation {
    std::string callback;
    std::size_t arity = 0;
    std::string message;
};

struct ScriptContractReport {
    bool typeFound = false;
    std::vector<ScriptContractViolation> violations;

    [[nodiscard]] bool satisfied() const noexcept {
        return typeFound && violations.empty();
    }
};

[[nodiscard]] ScriptContractReport validateScriptContract(
    const ScriptRuntime& runtime,
    const std::string& canonicalTypeName,
    const ScriptContract& contract);

[[nodiscard]] ScriptContract sceneBehaviorContract();

struct InspectorFieldSchema {
    std::string name;
    semantic::PrimitiveType type = semantic::PrimitiveType::Error;
    std::string typeName;
    runtime::Value defaultValue;
    bool exported = false;
    bool hiddenInInspector = false;
    std::optional<std::int64_t> rangeMin;
    std::optional<std::int64_t> rangeMax;
    std::string tooltip;
    std::string formerlySerializedAs;
};

struct InspectorTypeSchema {
    std::string canonicalTypeName;
    bool executeInEditMode = false;
    std::vector<InspectorFieldSchema> fields;
};

[[nodiscard]] std::optional<InspectorTypeSchema> inspectTypeSchema(
    const ScriptRuntime& runtime,
    const ScriptMetadataRegistry& metadata,
    const std::string& canonicalTypeName);

} // namespace realscript::game
