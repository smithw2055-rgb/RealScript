#pragma once

#include "realscript/compiler/Compilation.h"
#include "realscript/debug/DebugInfo.h"
#include "realscript/text/Text.h"
#include "realscript/tooling/Json.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace realscript::tooling {

enum class LintSeverity : std::uint8_t {
    Off = 0,
    Info = 1,
    Warning = 2,
    Error = 3,
};

[[nodiscard]] const char* lintSeverityName(LintSeverity severity) noexcept;
[[nodiscard]] std::optional<LintSeverity> parseLintSeverity(std::string_view text) noexcept;
[[nodiscard]] std::string canonicalLintRuleCode(std::string_view codeOrSlug);
[[nodiscard]] const char* lintRuleSlug(std::string_view ruleCode) noexcept;

struct LintConfig {
    bool enableGovernanceLint = false;
    std::size_t maxCyclomaticComplexity = 20;
    std::unordered_map<std::string, LintSeverity> ruleSeverities;

    [[nodiscard]] LintSeverity severityFor(std::string_view ruleCode) const noexcept;
    void setRuleSeverity(std::string_view codeOrSlug, LintSeverity severity);
};

[[nodiscard]] bool parseLintConfigFromProjectJson(
    const Json& rootObject,
    LintConfig& ioConfig,
    bool merge = false);

struct LintDiagnostic {
    std::string code;
    std::string ruleName;
    std::string message;
    LintSeverity severity = LintSeverity::Warning;
    std::string sourcePath;
    text::TextSpan span{};
    debug::SourceRange range{};
};

class GovernanceLinter {
public:
    explicit GovernanceLinter(LintConfig config = {});

    void setConfig(LintConfig config) noexcept { config_ = std::move(config); }
    [[nodiscard]] const LintConfig& config() const noexcept { return config_; }

    [[nodiscard]] std::vector<LintDiagnostic> lintSource(
        const std::string& path,
        const std::string& content,
        const compiler::BuildResult* buildContext = nullptr,
        const std::vector<compiler::SourceFile>* workspaceSources = nullptr) const;

    [[nodiscard]] std::vector<LintDiagnostic> lintWorkspace(
        const std::vector<compiler::SourceFile>& sources,
        const compiler::BuildResult* buildContext = nullptr) const;

private:
    LintConfig config_;
};

} // namespace realscript::tooling
