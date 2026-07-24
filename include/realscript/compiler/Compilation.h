#pragma once

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
};

struct BuildSnapshot {
    std::unordered_map<std::string, CachedModule> modules;
};

struct BuildResult {
    std::vector<mir::Module> modules;
    std::vector<ModuleBuildInfo> buildInfo;
    diagnostics::DiagnosticBag diagnostics;
    BuildSnapshot snapshot;
};

class Compilation {
public:
    Compilation() = default;
    explicit Compilation(std::vector<SourceFile> sources)
        : sources_(std::move(sources)) {}

    void addSource(SourceFile source) {
        sources_.push_back(std::move(source));
    }

    [[nodiscard]] BuildResult build(
        const BuildSnapshot* previous = nullptr) const;

private:
    std::vector<SourceFile> sources_;
};

[[nodiscard]] std::uint64_t stableFingerprint(
    const std::string& value) noexcept;

} // namespace realscript::compiler
