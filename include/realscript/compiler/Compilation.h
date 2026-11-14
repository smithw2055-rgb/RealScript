#pragma once

#include "realscript/compiler/LanguageMetadata.h"
#include "realscript/diagnostics/Diagnostic.h"
#include "realscript/mir/Mir.h"

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace realscript::text { class SourceText; }
namespace realscript::syntax { struct CompilationUnitSyntax; }

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
    std::vector<LanguageGenericInstantiation> nativeGenericInstantiations;
    std::vector<LanguageSequenceRecord> nativeSequences;
    BuildSnapshot snapshot;
};

struct MacroAstUnitView {
    std::string sourcePath;
    std::string moduleName;
    std::vector<std::string> imports;
    const text::SourceText* sourceText = nullptr;
    const syntax::CompilationUnitSyntax* syntaxTree = nullptr;
};

struct MacroExpansionContext {
    const std::vector<MacroAstUnitView>& parsedUnits;
};

class ICompilerMacroPlugin {
public:
    virtual ~ICompilerMacroPlugin() = default;
    [[nodiscard]] virtual std::string name() const = 0;
    [[nodiscard]] virtual std::vector<std::string> macroNames() const { return {}; }
    [[nodiscard]] virtual std::uint64_t fingerprint() const { return 0; }
    virtual void expand(
        const MacroExpansionContext& context,
        std::vector<SourceFile>& outGeneratedSources,
        diagnostics::DiagnosticBag& diagnostics) const = 0;
};

struct ModulePack {
    std::string moduleName;
    std::string virtualSourcePath;
    std::function<const std::string&()> sourceProvider;
    std::function<bool(std::string_view)> implicitUsagePredicate;
    bool autoInjectImportOnImplicitUsage = false;
};

class ModulePackRegistry {
public:
    ModulePackRegistry() = default;

    void registerPack(ModulePack pack);
    [[nodiscard]] bool unregisterPack(std::string_view moduleName);
    [[nodiscard]] const ModulePack* findPack(std::string_view moduleName) const noexcept;
    [[nodiscard]] const std::vector<ModulePack>& packs() const noexcept { return packs_; }
    [[nodiscard]] std::uint64_t fingerprint() const noexcept;

    [[nodiscard]] static const ModulePackRegistry& builtinDefaults();

private:
    std::vector<ModulePack> packs_;
};

class CompilerSession {
public:
    CompilerSession();
    ~CompilerSession();
    CompilerSession(const CompilerSession&) = delete;
    CompilerSession& operator=(const CompilerSession&) = delete;

    void clearCaches();
    [[nodiscard]] std::size_t parsedUnitCacheSize() const noexcept;
    [[nodiscard]] std::size_t moduleFrontendCacheSize() const noexcept;

    struct Impl;
    [[nodiscard]] Impl& impl() noexcept { return *impl_; }

private:
    std::unique_ptr<Impl> impl_;
};

struct CompilationOptions {
    std::shared_ptr<CompilerSession> session;
    const ModulePackRegistry* packRegistry = nullptr;
    bool allowDefaultBuiltinPacks = true;
    std::unordered_set<std::string> disabledPacks;
    std::vector<ModulePack> extraPacks;
    std::vector<std::shared_ptr<const ICompilerMacroPlugin>> macroPlugins;
    std::uint64_t projectConfigFingerprint = 0;
    std::string projectConfigPath;
};

class Compilation {
public:
    Compilation() = default;
    explicit Compilation(std::vector<SourceFile> sources, CompilationOptions options = {})
        : sources_(std::move(sources)), options_(std::move(options)) {}
    Compilation(std::initializer_list<SourceFile> sources)
        : sources_(sources) {}

    void addSource(SourceFile source) {
        sources_.push_back(std::move(source));
    }

    void setOptions(CompilationOptions options) {
        options_ = std::move(options);
    }
    [[nodiscard]] const CompilationOptions& options() const noexcept { return options_; }

    [[nodiscard]] BuildResult build(
        const BuildSnapshot* previous = nullptr) const;

private:
    std::vector<SourceFile> sources_;
    CompilationOptions options_;
};

[[nodiscard]] std::uint64_t stableFingerprint(
    const std::string& value) noexcept;

} // namespace realscript::compiler
