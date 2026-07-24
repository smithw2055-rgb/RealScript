#include "realscript/compiler/Compilation.h"

#include "realscript/semantic/Semantic.h"
#include "realscript/syntax/Syntax.h"
#include "realscript/text/Text.h"

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace realscript::compiler {

std::uint64_t stableFingerprint(const std::string& value) noexcept {
    std::uint64_t hash = 14695981039346656037ull;
    for (const auto character : value) {
        hash ^= static_cast<unsigned char>(character);
        hash *= 1099511628211ull;
    }
    return hash;
}

namespace {

struct ParsedUnit {
    std::unique_ptr<text::SourceText> source;
    std::unique_ptr<syntax::CompilationUnitSyntax> syntaxTree;
    std::string moduleName;
    std::vector<std::string> imports;
    std::uint64_t sourceFingerprint = 0;
    bool invalid = false;
};

struct ModuleWork {
    std::string name;
    std::vector<ParsedUnit*> units;
    std::set<std::string> imports;
    std::vector<semantic::FunctionSymbol> declarations;
    std::uint64_t sourceFingerprint = 0;
    std::uint64_t publicFingerprint = 0;
    std::uint64_t dependencyFingerprint = 0;
    bool invalid = false;
};

std::uint64_t combineFingerprint(
    std::uint64_t seed,
    std::uint64_t value) noexcept {
    seed ^= value + 0x9e3779b97f4a7c15ull +
        (seed << 6) + (seed >> 2);
    return seed;
}

} // namespace

BuildResult Compilation::build(const BuildSnapshot* previous) const {
    BuildResult result;
    std::vector<std::unique_ptr<ParsedUnit>> units;
    std::map<std::string, ModuleWork> modules;

    for (const auto& sourceFile : sources_) {
        auto unit = std::make_unique<ParsedUnit>();
        unit->source = std::make_unique<text::SourceText>(
            sourceFile.content,
            sourceFile.path);

        diagnostics::DiagnosticBag parseDiagnostics;
        syntax::Parser parser(*unit->source, parseDiagnostics);
        unit->syntaxTree =
            std::make_unique<syntax::CompilationUnitSyntax>(
                parser.parseCompilationUnit());
        unit->moduleName = unit->syntaxTree->moduleDeclaration
            ? unit->syntaxTree->moduleDeclaration->fullName()
            : "$global";
        unit->sourceFingerprint = stableFingerprint(
            sourceFile.path + "\n" + sourceFile.content);

        for (const auto& import : unit->syntaxTree->imports) {
            unit->imports.push_back(import.fullName());
        }
        for (const auto& diagnostic : parseDiagnostics.items()) {
            result.diagnostics.report(
                diagnostic.code,
                diagnostic.message,
                diagnostic.span,
                diagnostic.severity,
                sourceFile.path);
        }
        unit->invalid = parseDiagnostics.hasErrors();

        auto& module = modules[unit->moduleName];
        module.name = unit->moduleName;
        module.units.push_back(unit.get());
        module.invalid = module.invalid || unit->invalid;
        module.imports.insert(unit->imports.begin(), unit->imports.end());
        units.push_back(std::move(unit));
    }

    for (auto& [moduleName, module] : modules) {
        std::sort(
            module.units.begin(),
            module.units.end(),
            [](const ParsedUnit* left, const ParsedUnit* right) {
                return left->source->name() < right->source->name();
            });

        std::uint64_t sourceFingerprint = 14695981039346656037ull;
        std::unordered_set<std::string> functionKeys;

        for (const auto* unit : module.units) {
            sourceFingerprint = combineFingerprint(
                sourceFingerprint,
                unit->sourceFingerprint);

            for (const auto& functionSyntax : unit->syntaxTree->functions) {
                auto symbol = semantic::declareFunctionSymbol(
                    moduleName,
                    functionSyntax,
                    result.diagnostics);
                const auto key = semantic::canonicalFunctionKey(symbol);
                if (!functionKeys.insert(key).second) {
                    result.diagnostics.report(
                        "RS4002",
                        "duplicate function overload '" + key + "'",
                        functionSyntax.identifierToken.span,
                        diagnostics::DiagnosticSeverity::Error,
                        unit->source->name());
                    module.invalid = true;
                }
                module.declarations.push_back(std::move(symbol));
            }
        }
        module.sourceFingerprint = sourceFingerprint;

        std::vector<std::string> signatures;
        signatures.reserve(module.declarations.size());
        for (const auto& function : module.declarations) {
            signatures.push_back(
                semantic::canonicalFunctionSignature(function));
        }
        std::sort(signatures.begin(), signatures.end());

        std::ostringstream publicSurface;
        for (const auto& signature : signatures) {
            publicSurface << signature << '\n';
        }
        module.publicFingerprint = stableFingerprint(publicSurface.str());
    }

    for (auto& [moduleName, module] : modules) {
        std::ostringstream dependencies;
        for (const auto& importedModule : module.imports) {
            const auto imported = modules.find(importedModule);
            if (imported == modules.end()) {
                result.diagnostics.report(
                    "RS4001",
                    "module '" + moduleName + "' imports missing module '" +
                        importedModule + "'",
                    {});
                module.invalid = true;
                continue;
            }
            dependencies << importedModule << ':'
                << imported->second.publicFingerprint << '\n';
        }
        module.dependencyFingerprint = stableFingerprint(dependencies.str());
    }

    for (auto& [moduleName, module] : modules) {
        ModuleBuildInfo buildInfo{
            moduleName,
            module.sourceFingerprint,
            module.publicFingerprint,
            module.dependencyFingerprint,
            false,
        };

        if (previous) {
            const auto cached = previous->modules.find(moduleName);
            if (cached != previous->modules.end() && !module.invalid &&
                cached->second.sourceFingerprint ==
                    module.sourceFingerprint &&
                cached->second.publicFingerprint ==
                    module.publicFingerprint &&
                cached->second.dependencyFingerprint ==
                    module.dependencyFingerprint) {
                buildInfo.reused = true;
                result.modules.push_back(cached->second.module);
                result.snapshot.modules[moduleName] = cached->second;
                result.buildInfo.push_back(buildInfo);
                continue;
            }
        }

        if (module.invalid) {
            result.buildInfo.push_back(buildInfo);
            continue;
        }

        semantic::FunctionOverloadMap visibleFunctions;
        for (const auto& function : module.declarations) {
            visibleFunctions[function.name].push_back(function);
        }
        for (const auto& importedModule : module.imports) {
            const auto imported = modules.find(importedModule);
            if (imported == modules.end()) {
                continue;
            }
            for (const auto& function : imported->second.declarations) {
                visibleFunctions[function.name].push_back(function);
            }
        }

        semantic::ModuleBindingInput bindingInput;
        bindingInput.moduleName = moduleName;
        bindingInput.declarations = module.declarations;
        bindingInput.visibleFunctions = std::move(visibleFunctions);
        for (const auto* unit : module.units) {
            bindingInput.units.push_back(unit->syntaxTree.get());
        }

        diagnostics::DiagnosticBag moduleDiagnostics;
        semantic::Binder binder(moduleDiagnostics);
        auto semanticModel = binder.bindModule(bindingInput);
        if (moduleDiagnostics.hasErrors()) {
            result.diagnostics.append(moduleDiagnostics);
            result.buildInfo.push_back(buildInfo);
            continue;
        }

        mir::Lowerer lowerer;
        auto mirModule = lowerer.lower(semanticModel);
        (void)mir::verifyModule(mirModule, moduleDiagnostics);
        result.diagnostics.append(moduleDiagnostics);
        if (!moduleDiagnostics.hasErrors()) {
            result.modules.push_back(mirModule);
            result.snapshot.modules[moduleName] = {
                module.sourceFingerprint,
                module.publicFingerprint,
                module.dependencyFingerprint,
                mirModule,
            };
        }
        result.buildInfo.push_back(buildInfo);
    }

    return result;
}

} // namespace realscript::compiler
