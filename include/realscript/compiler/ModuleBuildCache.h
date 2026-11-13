#pragma once

#include "realscript/aot_cpp/AotCpp.h"
#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/diagnostics/Diagnostic.h"
#include "realscript/mir/Mir.h"
#include "realscript/optimization/Optimizer.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace realscript::compiler {

inline constexpr std::uint64_t kBuildCacheFlagNone = 0ull;
inline constexpr std::uint64_t kBuildCacheFlagDeterministic = 1ull << 0;
inline constexpr std::uint64_t kBuildCacheFlagPreserveDebugInfo = 1ull << 1;
inline constexpr std::uint64_t kBuildCacheFlagTrustedRelease = 1ull << 2;
inline constexpr std::uint64_t kBuildCacheFlagNoLineDirectives = 1ull << 3;

struct ModuleCacheKey {
    std::string moduleName;
    std::uint64_t implementationFingerprint = 0;
    std::vector<std::pair<std::string, std::uint64_t>> dependencyPublicFingerprints;
    optimization::Level optLevel = optimization::Level::None;
    std::uint64_t flags = 0;

    [[nodiscard]] std::uint64_t digest() const noexcept;
};

struct ExtractedModuleInfo {
    std::string name;
    std::vector<std::string> imports;
    std::vector<SourceFile> sources;
    std::uint64_t implementationFingerprint = 0;
    std::uint64_t publicFingerprint = 0;
    std::string declarationStub;
    bool deterministic = false;
    bool hasPublicGenerics = false;
};

struct ModuleGraphExtraction {
    std::vector<std::string> topologicalOrder;
    std::unordered_map<std::string, ExtractedModuleInfo> modules;
    diagnostics::DiagnosticBag diagnostics;

    [[nodiscard]] bool succeeded() const noexcept {
        return !diagnostics.hasErrors();
    }
};

struct ModuleCacheEntryStatus {
    std::string moduleName;
    std::vector<std::string> imports;
    std::uint64_t implementationFingerprint = 0;
    std::uint64_t publicFingerprint = 0;
    std::uint64_t dependencyPublicFingerprint = 0;
    std::uint64_t cacheKey = 0;
    bool cacheHit = false;
    bool healedCorruptedCache = false;
    bool usedDeclarationStubs = false;
    std::filesystem::path bytecodeCachePath;
    std::filesystem::path stubCachePath;
};

struct BuildCacheStatistics {
    std::size_t totalModules = 0;
    std::size_t cacheHits = 0;
    std::size_t cacheMisses = 0;
    std::size_t corruptedEntriesHealed = 0;
    std::size_t declarationStubsGenerated = 0;
    bool aotOutputSkipped = false;
};

struct ModuleBuildCacheOptions {
    std::filesystem::path cacheDirectory;
    optimization::Options optimizationOptions{};
    std::uint64_t flags = kBuildCacheFlagPreserveDebugInfo;
    bool emitDeclarationStubs = true;
    bool retainMirModules = false;

    ModuleBuildCacheOptions() noexcept {
        optimizationOptions.level = optimization::Level::None;
    }
};

struct CachedBuildResult {
    std::vector<bytecode::Module> bytecodeModules;
    std::vector<mir::Module> mirModules;
    std::vector<std::string> topologicalOrder;
    std::vector<ModuleCacheEntryStatus> moduleStatuses;
    BuildCacheStatistics statistics;
    optimization::Statistics optimizationStatistics;
    diagnostics::DiagnosticBag diagnostics;

    [[nodiscard]] bool succeeded() const noexcept {
        return !diagnostics.hasErrors();
    }
};

struct CachedAotBuildResult {
    aot::GeneratedProgram program;
    std::vector<std::string> topologicalOrder;
    std::vector<ModuleCacheEntryStatus> moduleStatuses;
    BuildCacheStatistics statistics;
    optimization::Statistics optimizationStatistics;
    diagnostics::DiagnosticBag diagnostics;
    bool aotCacheHit = false;

    [[nodiscard]] bool succeeded() const noexcept {
        return !diagnostics.hasErrors();
    }
};

class ModuleBuildCache {
public:
    ModuleBuildCache() = default;
    explicit ModuleBuildCache(std::filesystem::path cacheDirectory);

    void setCacheDirectory(std::filesystem::path cacheDirectory);
    [[nodiscard]] const std::filesystem::path& cacheDirectory() const noexcept {
        return cacheDirectory_;
    }

    [[nodiscard]] static ModuleGraphExtraction extractModuleGraph(
        const std::vector<SourceFile>& sources,
        optimization::Options optimizationOptions = {},
        std::uint64_t flags = kBuildCacheFlagPreserveDebugInfo);

    [[nodiscard]] static std::string generateDeclarationStub(
        const ExtractedModuleInfo& moduleInfo);

    [[nodiscard]] std::filesystem::path materializeDeclarationStub(
        const ExtractedModuleInfo& moduleInfo,
        const std::filesystem::path& overrideCacheDirectory = {}) const;

    [[nodiscard]] CachedBuildResult build(
        const std::vector<SourceFile>& sources,
        ModuleBuildCacheOptions options = {}) const;

    using AotGeneratorCallback = aot::GeneratedProgram (*)(
        const std::vector<mir::Module>& modules,
        diagnostics::DiagnosticBag& diagnostics,
        const aot::GenerationOptions& options);

    [[nodiscard]] CachedAotBuildResult buildAot(
        const std::vector<SourceFile>& sources,
        ModuleBuildCacheOptions cacheOptions,
        aot::GenerationOptions aotOptions = {}) const;

    [[nodiscard]] CachedAotBuildResult buildAotWithGenerator(
        const std::vector<SourceFile>& sources,
        ModuleBuildCacheOptions cacheOptions,
        aot::GenerationOptions aotOptions,
        AotGeneratorCallback generatorCallback) const;

    [[nodiscard]] static std::string formatStatistics(
        const BuildCacheStatistics& statistics);

private:
    std::filesystem::path cacheDirectory_;
};

} // namespace realscript::compiler
