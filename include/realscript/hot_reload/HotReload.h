#pragma once

#include "realscript/runtime/Runtime.h"

#include <memory>
#include <string>
#include <vector>

namespace realscript::hot_reload {

enum class ReloadIssueKind {
    InvalidProgram,
    ModuleSetChanged,
    TypeLayoutChanged,
    FunctionSetChanged,
    FunctionSignatureChanged,
    LanguageMetadataChanged,
};

struct ReloadIssue {
    ReloadIssueKind kind = ReloadIssueKind::InvalidProgram;
    std::string subject;
    std::string message;
};

struct ReloadPlan {
    bool compatible = false;
    std::shared_ptr<const runtime::ProgramImage> program;
    std::vector<semantic::SymbolId> changedFunctions;
    std::vector<ReloadIssue> issues;
};

[[nodiscard]] ReloadPlan prepare(
    const runtime::ProgramImage& current,
    std::vector<bytecode::Module> replacementModules);

[[nodiscard]] ReloadPlan apply(
    runtime::EngineRuntime& runtime,
    std::vector<bytecode::Module> replacementModules);

[[nodiscard]] const char* reloadIssueKindName(ReloadIssueKind kind) noexcept;

} // namespace realscript::hot_reload
