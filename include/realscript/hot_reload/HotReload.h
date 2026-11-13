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
    StaleInlinedDependency,
};

struct ReloadIssue {
    ReloadIssueKind kind = ReloadIssueKind::InvalidProgram;
    std::string subject;
    std::string message;
};

struct ReloadOptions {
    bool allowStructuralChanges = false;
    bool allowModuleSetChanges = false;
    bool allowAddedFunctions = true;
};

struct ReloadPlan {
    bool compatible = false;
    bool structuralMigrationRequired = false;
    std::shared_ptr<const runtime::ProgramImage> program;
    std::vector<semantic::SymbolId> changedFunctions;
    std::vector<semantic::SymbolId> inlineInvalidatedFunctions;
    std::vector<semantic::SymbolId> addedFunctions;
    std::vector<semantic::SymbolId> removedFunctions;
    std::vector<semantic::SymbolId> addedTypes;
    std::vector<semantic::SymbolId> removedTypes;
    std::vector<semantic::SymbolId> layoutChangedTypes;
    std::vector<runtime::TypeLayoutMigration> typeMigrations;
    runtime::LayoutMigrationStats migrationStats{};
    std::vector<ReloadIssue> issues;
};

[[nodiscard]] ReloadPlan prepare(
    const runtime::ProgramImage& current,
    std::vector<bytecode::Module> replacementModules,
    ReloadOptions options = {});

[[nodiscard]] ReloadPlan apply(
    runtime::EngineRuntime& runtime,
    std::vector<bytecode::Module> replacementModules,
    ReloadOptions options = {});

[[nodiscard]] const char* reloadIssueKindName(ReloadIssueKind kind) noexcept;

} // namespace realscript::hot_reload
