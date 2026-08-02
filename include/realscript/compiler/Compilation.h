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
    std::vector<LanguageAttributeRecord> nativeAttributes;
    std::vector<LanguageInterfaceImplementation> nativeInterfaces;
    std::vector<LanguageSequenceRecord> nativeSequences;
    BuildSnapshot snapshot;
};

class Compilation {
public:
    Compilation() = default;
    explicit Compilation(std::vector<SourceFile> sources)
        : sourceInputs_(std::move(sources)) {
        refreshLanguageExpansions();
    }

    void setLanguageExpansionOptions(LanguageExpansionOptions options) {
        expansionOptions_ = options;
        refreshLanguageExpansions();
    }

    [[nodiscard]] const LanguageExpansionOptions& languageExpansionOptions() const noexcept {
        return expansionOptions_;
    }

    void addSource(SourceFile source) {
        sourceInputs_.push_back(std::move(source));
        refreshLanguageExpansions();
    }

    [[nodiscard]] const std::vector<LanguageExpansionResult>& languageExpansions() const noexcept {
        return languageExpansions_;
    }

    [[nodiscard]] BuildResult build(
        const BuildSnapshot* previous = nullptr) const;

private:
    void refreshLanguageExpansions() {
        std::vector<LanguageExpansionSource> inputs;
        inputs.reserve(sourceInputs_.size());
        for (const auto& source : sourceInputs_) {
            inputs.push_back(LanguageExpansionSource{source.path, source.content});
        }

        languageExpansions_ = expandLanguageSources(inputs, expansionOptions_);
        sources_.clear();
        sources_.reserve(sourceInputs_.size());
        for (std::size_t index = 0; index < sourceInputs_.size(); ++index) {
            const auto& input = sourceInputs_[index];
            const auto useExpansion = index < languageExpansions_.size() &&
                languageExpansions_[index].changed &&
                !languageExpansions_[index].content.empty();
            sources_.push_back(SourceFile{
                input.path,
                useExpansion ? languageExpansions_[index].content : input.content});
        }
    }

    LanguageExpansionOptions expansionOptions_;
    std::vector<SourceFile> sourceInputs_;
    std::vector<SourceFile> sources_;
    std::vector<LanguageExpansionResult> languageExpansions_;
};

[[nodiscard]] std::uint64_t stableFingerprint(
    const std::string& value) noexcept;

} // namespace realscript::compiler
