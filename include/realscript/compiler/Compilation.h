#pragma once

#include "realscript/compiler/LanguageExpansion.h"
#include "realscript/diagnostics/Diagnostic.h"
#include "realscript/mir/Mir.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace realscript::compiler {

struct SourceFile {
    std::string path;
    std::string content;
};

struct ModuleBuildInfo {
    std::string name;
    std::uint64_t sourceFingerprint = 0;
    std::uint64_t publicFingerprint = 0;
    std::uint64_t dependencyFingerprint = 0;
    bool reused = false;
};

struct CachedModule {
    std::uint64_t sourceFingerprint = 0;
    std::uint64_t publicFingerprint = 0;
    std::uint64_t dependencyFingerprint = 0;
    mir::Module module;
    std::vector<semantic::SymbolOccurrence> symbols;
};

struct BuildSnapshot {
    std::unordered_map<std::string, CachedModule> modules;
};

struct BuildResult {
    std::vector<mir::Module> modules;
    std::vector<ModuleBuildInfo> buildInfo;
    diagnostics::DiagnosticBag diagnostics;
    std::vector<semantic::SymbolOccurrence> symbols;
    BuildSnapshot snapshot;
};

class Compilation {
public:
    Compilation() = default;
    explicit Compilation(std::vector<SourceFile> sources) {
        for (auto& source : sources) addSource(std::move(source));
    }

    void setLanguageExpansionOptions(LanguageExpansionOptions options) {
        expansionOptions_ = options;
    }

    [[nodiscard]] const LanguageExpansionOptions& languageExpansionOptions() const noexcept {
        return expansionOptions_;
    }

    void addSource(SourceFile source) {
        auto expansion = expandLanguageSource(source.path, source.content, expansionOptions_);
        source.content = expansion.content;
        languageExpansions_.push_back(std::move(expansion));
        sources_.push_back(std::move(source));
    }

    [[nodiscard]] const std::vector<LanguageExpansionResult>& languageExpansions() const noexcept {
        return languageExpansions_;
    }

    [[nodiscard]] BuildResult build(
        const BuildSnapshot* previous = nullptr) const;

private:
    LanguageExpansionOptions expansionOptions_;
    std::vector<SourceFile> sources_;
    std::vector<LanguageExpansionResult> languageExpansions_;
};

[[nodiscard]] std::uint64_t stableFingerprint(
    const std::string& value) noexcept;

} // namespace realscript::compiler
