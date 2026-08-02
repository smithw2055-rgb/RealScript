#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace realscript::compiler {

enum class LanguageExpansionSeverity {
    Warning,
    Error,
};

struct LanguageExpansionDiagnostic {
    std::string code;
    std::string message;
    std::size_t offset = 0;
    LanguageExpansionSeverity severity = LanguageExpansionSeverity::Error;
};

struct LanguageAttributeArgument {
    std::string name;
    std::string value;
};

struct LanguageAttributeRecord {
    std::string target;
    std::string name;
    std::vector<LanguageAttributeArgument> arguments;
    std::string sourceName;
    std::size_t offset = 0;
};

struct LanguageInterfaceImplementation {
    std::string typeName;
    std::vector<std::string> interfaces;
};

struct LanguageGenericInstantiation {
    std::string genericName;
    std::vector<std::string> arguments;
    std::string generatedName;
};

struct LanguageSequenceRecord {
    std::string typeName;
    std::string name;
    std::vector<std::string> callbacks;
    std::string sourceName;
    std::size_t offset = 0;
};

struct LanguageExpansionOptions {
    bool structuredControlFlow = false;
    bool delegatesLambdasEvents = true;
    bool interfaces = false;
    bool sourceAttributes = false;
    bool generics = true;
    bool deterministicCoroutines = false;
    bool referenceParameters = false;
    bool valueTypeAliases = true;
};

struct LanguageExpansionSource {
    std::string path;
    std::string content;
};

struct LanguageExpansionResult {
    std::string content;
    std::vector<LanguageExpansionDiagnostic> diagnostics;
    std::vector<LanguageAttributeRecord> attributes;
    std::vector<LanguageInterfaceImplementation> interfaces;
    std::vector<LanguageGenericInstantiation> genericInstantiations;
    bool changed = false;

    [[nodiscard]] bool succeeded() const noexcept;
};

// Expand one source in isolation. Prefer expandLanguageSources() for a
// Compilation so declarations in sibling files share delegate, interface, and
// generic discovery while each result retains its own path and metadata.
[[nodiscard]] LanguageExpansionResult expandLanguageSource(
    const std::string& path,
    const std::string& content,
    LanguageExpansionOptions options = {});

[[nodiscard]] std::vector<LanguageExpansionResult> expandLanguageSources(
    const std::vector<LanguageExpansionSource>& sources,
    LanguageExpansionOptions options = {});

[[nodiscard]] const char* languageExpansionSeverityName(
    LanguageExpansionSeverity severity) noexcept;

} // namespace realscript::compiler
