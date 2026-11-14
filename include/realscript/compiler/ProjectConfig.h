#pragma once

#include "realscript/compiler/Compilation.h"
#include "realscript/diagnostics/Diagnostic.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace realscript::compiler {

/// Cache-option bit used by CLI tools when --no-project-config disables auto-discovery.
constexpr std::uint32_t kBuildCacheFlagNoProjectConfig = 1u << 8;

struct ProjectConfig {
    std::filesystem::path configPath;
    std::unordered_set<std::string> disabledBuiltinPacks;
    std::vector<std::filesystem::path> extraPackFiles;
    std::vector<std::filesystem::path> nativeMacroPluginPaths;
    std::uint64_t configFingerprint = 0;
};

[[nodiscard]] std::optional<std::filesystem::path> discoverProjectConfigFile(
    const std::filesystem::path& startPathOrDir = {});

[[nodiscard]] std::optional<std::filesystem::path> discoverProjectConfigFile(
    const std::vector<std::filesystem::path>& hintPaths);

[[nodiscard]] std::optional<ProjectConfig> loadProjectConfigFile(
    const std::filesystem::path& configPath,
    diagnostics::DiagnosticBag* diagnostics = nullptr);

[[nodiscard]] std::shared_ptr<const ICompilerMacroPlugin> loadNativeCompilerMacroPlugin(
    const std::filesystem::path& libraryPath,
    diagnostics::DiagnosticBag* diagnostics = nullptr);

[[nodiscard]] bool applyProjectConfigToOptions(
    const ProjectConfig& config,
    CompilationOptions& options,
    diagnostics::DiagnosticBag* diagnostics = nullptr);

[[nodiscard]] bool resolveAndApplyProjectConfigForInputs(
    const std::vector<SourceFile>& sources,
    const std::optional<std::filesystem::path>& explicitConfigPath,
    bool noProjectConfig,
    CompilationOptions& options,
    diagnostics::DiagnosticBag* diagnostics = nullptr);

class ScopedProjectCompilationOptions {
public:
    explicit ScopedProjectCompilationOptions(const CompilationOptions& options) noexcept;
    ~ScopedProjectCompilationOptions();

    ScopedProjectCompilationOptions(const ScopedProjectCompilationOptions&) = delete;
    ScopedProjectCompilationOptions& operator=(const ScopedProjectCompilationOptions&) = delete;

    [[nodiscard]] static const CompilationOptions* current() noexcept;

private:
    const CompilationOptions* previous_ = nullptr;
    CompilationOptions active_;
};

} // namespace realscript::compiler
