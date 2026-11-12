#include "CompilationInternal.h"

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace realscript::compiler {

using namespace detail;

std::uint64_t stableFingerprint(const std::string& value) noexcept {
    std::uint64_t hash = 14695981039346656037ull;
    for (const auto character : value) {
        hash ^= static_cast<unsigned char>(character);
        hash *= 1099511628211ull;
    }
    return hash;
}


BuildResult Compilation::build(const BuildSnapshot* previous) const {
    BuildResult result;
    std::vector<std::unique_ptr<ParsedUnit>> units;
    std::map<std::string, ModuleWork> modules;

    static std::mutex sParsedUnitCacheMutex;
    static std::unordered_map<std::uint64_t, ParsedUnit> sParsedUnitCache;
    static std::mutex sModuleFrontendCacheMutex;
    static std::unordered_map<std::string, CachedModuleFrontend> sModuleFrontendCache;

    const auto addSourceUnit = [&](const SourceFile& sourceFile) {
        const auto fp = stableFingerprint(sourceFile.path + "\n" + sourceFile.content);
        auto unit = std::make_unique<ParsedUnit>();
        bool cacheHit = false;
        {
            std::lock_guard<std::mutex> lock(sParsedUnitCacheMutex);
            const auto found = sParsedUnitCache.find(fp);
            if (found != sParsedUnitCache.end() &&
                found->second.source &&
                found->second.source->name() == sourceFile.path) {
                *unit = found->second;
                cacheHit = true;
            }
        }
        if (!cacheHit) {
            unit->source = std::make_shared<text::SourceText>(sourceFile.content, sourceFile.path);
            diagnostics::DiagnosticBag parseDiagnostics;
            syntax::Parser parser(*unit->source, parseDiagnostics);
            unit->syntaxTree = std::make_shared<syntax::CompilationUnitSyntax>(
                parser.parseCompilationUnit());
            unit->moduleName = unit->syntaxTree->moduleDeclaration
                ? unit->syntaxTree->moduleDeclaration->fullName()
                : "$global";
            unit->sourceFingerprint = fp;
            for (const auto& import : unit->syntaxTree->imports) {
                unit->imports.push_back(import.fullName());
            }
            unit->parseDiagnostics = parseDiagnostics.items();
            unit->invalid = parseDiagnostics.hasErrors();
            if (unit->syntaxTree->moduleDeclaration) {
                for (const auto& list : unit->syntaxTree->moduleDeclaration->attributes) {
                    for (const auto& attr : list.attributes) {
                        if (attr.nameToken.text == "Deterministic") {
                            unit->deterministic = true;
                        }
                    }
                }
            }
            unit->hasGenericSyntax = (sourceFile.content.find('<') != std::string::npos);
            {
                std::lock_guard<std::mutex> lock(sParsedUnitCacheMutex);
                if (sParsedUnitCache.size() > 8192) sParsedUnitCache.clear();
                sParsedUnitCache[fp] = *unit;
            }
        }
        for (const auto& diagnostic : unit->parseDiagnostics) {
            result.diagnostics.report(
                diagnostic.code, diagnostic.message, diagnostic.span,
                diagnostic.severity, sourceFile.path);
        }
        auto& module = modules[unit->moduleName];
        module.name = unit->moduleName;
        if (unit->deterministic) module.deterministic = true;
        module.units.push_back(unit.get());
        module.invalid = module.invalid || unit->invalid;
        module.imports.insert(unit->imports.begin(), unit->imports.end());
        units.push_back(std::move(unit));
    };

    for (const auto& sourceFile : sources_) {
        addSourceUnit(sourceFile);
    }

    bool importsBuiltinMath = false;
    bool importsBuiltinFiber = false;
    for (const auto& [name, module] : modules) {
        (void)name;
        if (module.imports.find("RealScript.Math") != module.imports.end()) {
            importsBuiltinMath = true;
        }
        if (module.imports.find("RealScript.Fiber") != module.imports.end()) {
            importsBuiltinFiber = true;
        }
    }
    if (importsBuiltinMath && modules.find("RealScript.Math") == modules.end()) {
        addSourceUnit(SourceFile{
            "$builtin/RealScript.Math.rscript",
            builtinDeterministicMathModuleSource()});
    }
    if (importsBuiltinFiber && modules.find("RealScript.Fiber") == modules.end()) {
        addSourceUnit(SourceFile{
            "$builtin/RealScript.Fiber.rscript",
            builtinFiberModuleSource()});
    }

    bool needsBuiltinAsync = false;
    std::vector<std::string> modulesUsingAsync;
    for (const auto& [name, module] : modules) {
        if (name == "RealScript.Async") continue;
        bool moduleUsesAsync =
            module.imports.find("RealScript.Async") != module.imports.end();
        if (!moduleUsesAsync) {
            for (const auto* u : module.units) {
                if (u && u->source && sourceMentionsAsyncPrimitives(u->source->content())) {
                    moduleUsesAsync = true;
                    break;
                }
            }
        }
        if (moduleUsesAsync) {
            needsBuiltinAsync = true;
            modulesUsingAsync.push_back(name);
        }
    }
    if (needsBuiltinAsync && modules.find("RealScript.Async") == modules.end()) {
        addSourceUnit(SourceFile{
            "$builtin/RealScript.Async.rscript",
            builtinAsyncModuleSource()});
    }
    for (const auto& modName : modulesUsingAsync) {
        modules[modName].imports.insert("RealScript.Async");
    }

    bool needsBuiltinResource = false;
    std::vector<std::string> modulesUsingResource;
    for (const auto& [name, module] : modules) {
        if (name == "RealScript.Resource" ||
            name == "RealScript.Math" ||
            name == "RealScript.Fiber" ||
            name == "RealScript.Async") {
            continue;
        }
        bool moduleUsesResource =
            module.imports.find("RealScript.Resource") != module.imports.end();
        if (!moduleUsesResource) {
            for (const auto* u : module.units) {
                if (u && u->source && sourceMentionsResourcePrimitives(u->source->content())) {
                    moduleUsesResource = true;
                    break;
                }
            }
        }
        if (moduleUsesResource) {
            needsBuiltinResource = true;
            modulesUsingResource.push_back(name);
        }
    }
    if (needsBuiltinResource && modules.find("RealScript.Resource") == modules.end()) {
        addSourceUnit(SourceFile{
            "$builtin/RealScript.Resource.rscript",
            builtinResourceModuleSource()});
    }
    for (const auto& modName : modulesUsingResource) {
        modules[modName].imports.insert("RealScript.Resource");
    }

    specializeNativeGenerics(units, modules, result);

    {
        bool changed = true;
        while (changed) {
            changed = false;
            for (auto& [modName, mod] : modules) {
                auto propagateFrom = [&](const std::string& depName) {
                    if (depName == modName) return;
                    const auto depIt = modules.find(depName);
                    if (depIt == modules.end()) return;
                    for (const auto& sub : depIt->second.imports) {
                        if (sub != modName &&
                            mod.imports.find(sub) == mod.imports.end() &&
                            mod.hygienicModules.insert(sub).second) {
                            changed = true;
                        }
                    }
                    for (const auto& sub : depIt->second.hygienicModules) {
                        if (sub != modName &&
                            mod.imports.find(sub) == mod.imports.end() &&
                            mod.hygienicModules.insert(sub).second) {
                            changed = true;
                        }
                    }
                };
                const auto currentImports = mod.imports;
                const auto currentHygienic = mod.hygienicModules;
                for (const auto& dep : currentImports) propagateFrom(dep);
                for (const auto& dep : currentHygienic) propagateFrom(dep);
            }
        }
    }

    // Stable source order, source fingerprints, and incremental module frontend cache check.
    for (auto& [moduleName, module] : modules) {
        (void)moduleName;
        std::stable_sort(module.units.begin(), module.units.end(),
            [](const ParsedUnit* left, const ParsedUnit* right) {
                if (left->source->name() != right->source->name()) {
                    return left->source->name() < right->source->name();
                }
                return left->sourceFingerprint < right->sourceFingerprint;
            });
        std::uint64_t sourceFingerprint = 14695981039346656037ull;
        for (const auto* unit : module.units) {
            sourceFingerprint = combineFingerprint(sourceFingerprint, unit->sourceFingerprint);
        }
        module.sourceFingerprint = sourceFingerprint;
    }

    if (result.nativeGenericInstantiations.empty()) {
        std::unordered_map<std::string, int> fpState;
        std::function<std::uint64_t(const std::string&)> computeTransitiveFp =
            [&](const std::string& modName) -> std::uint64_t {
            const auto it = modules.find(modName);
            if (it == modules.end() || it->second.invalid) return 0;
            const int st = fpState[modName];
            if (st == 1) return 0;
            if (st == 2) return it->second.transitiveSourceFingerprint;
            fpState[modName] = 1;
            std::uint64_t tfp = combineFingerprint(
                it->second.sourceFingerprint,
                stableFingerprint(modName + (it->second.deterministic ? ":det" : ":std")));
            for (const auto& imp : it->second.imports) {
                const auto impFp = computeTransitiveFp(imp);
                if (impFp == 0) {
                    fpState[modName] = 2;
                    it->second.transitiveSourceFingerprint = 0;
                    return 0;
                }
                tfp = combineFingerprint(tfp, impFp);
            }
            fpState[modName] = 2;
            it->second.transitiveSourceFingerprint = tfp;
            return tfp;
        };
        for (auto& [moduleName, module] : modules) {
            (void)module;
            (void)computeTransitiveFp(moduleName);
        }
        if (previous) {
            std::lock_guard<std::mutex> lock(sModuleFrontendCacheMutex);
            for (auto& [moduleName, module] : modules) {
                if (moduleName == "$global" || module.invalid || module.transitiveSourceFingerprint == 0) continue;
                const auto prevIt = previous->modules.find(moduleName);
                if (prevIt == previous->modules.end() ||
                    prevIt->second.sourceFingerprint != module.sourceFingerprint) {
                    continue;
                }
                const std::string cacheKey =
                    moduleName + "#" + std::to_string(module.transitiveSourceFingerprint);
                const auto fcIt = sModuleFrontendCache.find(cacheKey);
                if (fcIt == sModuleFrontendCache.end() ||
                    fcIt->second.transitiveSourceFingerprint != module.transitiveSourceFingerprint ||
                    fcIt->second.sourceFingerprint != module.sourceFingerprint ||
                    fcIt->second.publicFingerprint != prevIt->second.publicFingerprint ||
                    fcIt->second.dependencyFingerprint != prevIt->second.dependencyFingerprint) {
                    continue;
                }
                module.frontendCached = true;
                module.publicFingerprint = fcIt->second.publicFingerprint;
                module.dependencyFingerprint = fcIt->second.dependencyFingerprint;
                result.nativeAttributes.insert(
                    result.nativeAttributes.end(),
                    fcIt->second.nativeAttributes.begin(),
                    fcIt->second.nativeAttributes.end());
                result.nativeSequences.insert(
                    result.nativeSequences.end(),
                    fcIt->second.nativeSequences.begin(),
                    fcIt->second.nativeSequences.end());
                result.nativeInterfaces.insert(
                    result.nativeInterfaces.end(),
                    fcIt->second.nativeInterfaces.begin(),
                    fcIt->second.nativeInterfaces.end());
            }
            // Mark frontendCached modules that are transitively imported by any non-frontendCached module
            std::vector<std::string> dirtyQueue;
            for (const auto& [moduleName, module] : modules) {
                if (!module.frontendCached) {
                    for (const auto& imp : module.imports) dirtyQueue.push_back(imp);
                }
            }
            while (!dirtyQueue.empty()) {
                auto curr = std::move(dirtyQueue.back());
                dirtyQueue.pop_back();
                auto it = modules.find(curr);
                if (it == modules.end() || it->second.neededByDirtyModule) continue;
                it->second.neededByDirtyModule = true;
                for (const auto& imp : it->second.imports) dirtyQueue.push_back(imp);
            }
            for (auto& [moduleName, module] : modules) {
                if (module.frontendCached && module.neededByDirtyModule) {
                    const std::string cacheKey =
                        moduleName + "#" + std::to_string(module.transitiveSourceFingerprint);
                    const auto& fc = sModuleFrontendCache.at(cacheKey);
                    module.types = fc.types;
                    module.interfaces = fc.interfaces;
                    module.delegates = fc.delegates;
                    module.declarations = fc.declarations;
                }
            }
        }
    }

    discoverTypesContractsAndLayouts(modules, result);
    synthesizeDeclarationsSequencesAndMembers(modules, result);

    std::unordered_map<semantic::SymbolId, semantic::ObsoleteInfo> globalObsoleteSymbols;
    closeGlobalInterfacesValidateAttributesAndLayouts(
        modules, result, globalObsoleteSymbols);

    if (result.nativeGenericInstantiations.empty()) {
        std::lock_guard<std::mutex> lock(sModuleFrontendCacheMutex);
        if (sModuleFrontendCache.size() > 4096) sModuleFrontendCache.clear();
        for (const auto& [moduleName, module] : modules) {
            if (moduleName == "$global" || module.frontendCached || module.invalid || module.transitiveSourceFingerprint == 0) {
                continue;
            }
            CachedModuleFrontend fc;
            fc.sourceFingerprint = module.sourceFingerprint;
            fc.transitiveSourceFingerprint = module.transitiveSourceFingerprint;
            fc.publicFingerprint = module.publicFingerprint;
            fc.dependencyFingerprint = module.dependencyFingerprint;
            fc.types = module.types;
            fc.interfaces = module.interfaces;
            fc.delegates = module.delegates;
            fc.declarations = module.declarations;
            for (const auto* u : module.units) {
                if (u && u->syntaxTree) {
                    fc.keptSyntaxTrees.push_back(u->syntaxTree);
                }
            }
            const std::string prefix = moduleName + "::";
            for (const auto& attr : result.nativeAttributes) {
                if (attr.target == moduleName || attr.target.rfind(prefix, 0) == 0) {
                    fc.nativeAttributes.push_back(attr);
                }
            }
            for (const auto& seq : result.nativeSequences) {
                if (seq.typeName.rfind(prefix, 0) == 0) {
                    fc.nativeSequences.push_back(seq);
                }
            }
            for (const auto& impl : result.nativeInterfaces) {
                if (impl.typeName.rfind(prefix, 0) == 0) {
                    fc.nativeInterfaces.push_back(impl);
                }
            }
            const std::string cacheKey =
                moduleName + "#" + std::to_string(module.transitiveSourceFingerprint);
            sModuleFrontendCache[cacheKey] = std::move(fc);
        }
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
                cached->second.sourceFingerprint == module.sourceFingerprint &&
                cached->second.publicFingerprint == module.publicFingerprint &&
                cached->second.dependencyFingerprint == module.dependencyFingerprint) {
                buildInfo.reused = true;
                result.modules.push_back(cached->second.module);
                result.symbols.insert(result.symbols.end(),
                    cached->second.symbols.begin(), cached->second.symbols.end());
                result.snapshot.modules[moduleName] = cached->second;
                result.buildInfo.push_back(buildInfo);
                continue;
            }
        }
        // Even if a syntax error occurred (such as an incomplete trailing '.' during editing),
        // still run the semantic binder to collect symbol occurrences for IDE/LSP services,
        // and only skip MIR lowering if errors remain.
        const bool hadSyntaxError = module.invalid;

        auto freeFunctionParamKey = [](const semantic::FunctionSymbol& fn) {
            std::string key = fn.name + "(";
            for (std::size_t i = 0; i < fn.parameters.size(); ++i) {
                if (i != 0) key += ",";
                key += std::to_string(static_cast<int>(fn.parameters[i].modifier)) + ":" +
                    std::to_string(static_cast<int>(fn.parameters[i].type)) + ":" +
                    fn.parameters[i].typeName;
            }
            key += ")";
            return key;
        };
        std::unordered_set<std::string> ownFreeFnSignatures;
        semantic::FunctionOverloadMap visibleFunctions;
        for (const auto& function : module.declarations) {
            visibleFunctions[function.name].push_back(function);
            if (!function.method) {
                ownFreeFnSignatures.insert(freeFunctionParamKey(function));
            }
        }
        for (const auto& importedModule : module.imports) {
            const auto imported = modules.find(importedModule);
            if (imported == modules.end()) continue;
            for (const auto& function : imported->second.declarations) {
                if (!function.method &&
                    ownFreeFnSignatures.find(freeFunctionParamKey(function)) !=
                        ownFreeFnSignatures.end()) {
                    continue;
                }
                visibleFunctions[function.name].push_back(function);
            }
        }
        for (const auto& hygModName : module.hygienicModules) {
            if (hygModName == moduleName || module.imports.count(hygModName) != 0) continue;
            const auto hygIt = modules.find(hygModName);
            if (hygIt == modules.end()) continue;
            for (const auto& function : hygIt->second.declarations) {
                if (!function.method &&
                    ownFreeFnSignatures.find(freeFunctionParamKey(function)) ==
                        ownFreeFnSignatures.end()) {
                    visibleFunctions[function.name].push_back(function);
                }
            }
        }

        semantic::ModuleBindingInput bindingInput;
        // Hygiene is scoped strictly per instantiated template unit/symbol,
        // never opening another module's `internal` symbols to consumer user code.
        bindingInput.moduleName = moduleName;
        bindingInput.declarations = module.declarations;
        bindingInput.functionBindings = module.functionBindings;
        {
            std::unordered_map<semantic::SymbolId, semantic::TypeSymbol> descriptorsById;
            std::vector<semantic::TypeSymbol> descriptorQueue;
            for (const auto& [visibleName, type] : module.visibleTypes) {
                (void)visibleName;
                if (descriptorsById.emplace(type.id, type).second) {
                    descriptorQueue.push_back(type);
                }
            }
            const auto enqueueGlobalTypeByName = [&](const std::string& canonical) {
                if (canonical.empty()) return;
                if (const auto* target = findGlobalType(modules, canonical)) {
                    if (descriptorsById.emplace(target->id, *target).second) {
                        descriptorQueue.push_back(*target);
                    }
                }
            };
            for (std::size_t qIdx = 0; qIdx < descriptorQueue.size(); ++qIdx) {
                const auto curType = descriptorQueue[qIdx];
                if (!curType.baseTypeName.empty()) {
                    enqueueGlobalTypeByName(curType.baseTypeName);
                }
                for (const auto& fld : curType.fields) {
                    if (fld.type == semantic::PrimitiveType::Object ||
                        fld.type == semantic::PrimitiveType::Struct ||
                        fld.type == semantic::PrimitiveType::Enum) {
                        enqueueGlobalTypeByName(fld.typeName);
                    } else if (fld.type == semantic::PrimitiveType::Array) {
                        semantic::PrimitiveType elemPrim = semantic::PrimitiveType::Error;
                        std::string elemTypeName;
                        if (semantic::decodeArrayTypeName(fld.typeName, elemPrim, elemTypeName) &&
                            !elemTypeName.empty()) {
                            enqueueGlobalTypeByName(elemTypeName);
                        }
                    }
                }
            }
            bindingInput.types.reserve(descriptorsById.size());
            for (auto& [typeId, type] : descriptorsById) {
                (void)typeId;
                bindingInput.types.push_back(std::move(type));
            }
            std::sort(bindingInput.types.begin(), bindingInput.types.end(),
                [](const semantic::TypeSymbol& left, const semantic::TypeSymbol& right) {
                    return semantic::canonicalTypeName(left) < semantic::canonicalTypeName(right);
                });
        }
        bindingInput.visibleFunctions = std::move(visibleFunctions);
        bindingInput.visibleTypes = module.visibleTypes;
        bindingInput.obsoleteSymbols = globalObsoleteSymbols;
        for (const auto* unit : module.units) bindingInput.units.push_back(unit->syntaxTree.get());

        diagnostics::DiagnosticBag moduleDiagnostics;
        semantic::Binder binder(moduleDiagnostics);
        auto semanticModel = binder.bindModule(bindingInput);
        result.symbols.insert(result.symbols.end(),
            semanticModel.occurrences.begin(), semanticModel.occurrences.end());
        if (hadSyntaxError || moduleDiagnostics.hasErrors()) {
            result.diagnostics.append(moduleDiagnostics);
            for (const auto& t : bindingInput.types) {
                if (t.moduleName == moduleName) {
                    mir::Module partialMod;
                    partialMod.name = moduleName;
                    for (const auto& [visName, visType] : module.visibleTypes) {
                        (void)visName;
                        if (visType.moduleName == moduleName) {
                            const bool dup = std::any_of(
                                partialMod.types.begin(),
                                partialMod.types.end(),
                                [&](const semantic::TypeSymbol& ex) { return ex.id == visType.id; });
                            if (!dup) partialMod.types.push_back(visType);
                        }
                    }
                    {
                        std::unordered_set<std::string> seenPaths;
                        for (const auto* u : module.units) {
                            if (u && u->source && seenPaths.insert(u->source->name()).second) {
                                partialMod.sourceFiles.push_back(makeSourceFileInfo(
                                    *u->source,
                                    static_cast<debug::SourceFileId>(partialMod.sourceFiles.size())));
                            }
                        }
                    }
                    result.modules.push_back(std::move(partialMod));
                    break;
                }
            }
            result.buildInfo.push_back(buildInfo);
            continue;
        }

        mir::Lowerer lowerer;
        auto mirModule = lowerer.lower(semanticModel);
        mirModule.sourceFiles.reserve(module.units.size());
        {
            std::unordered_set<std::string> seenPaths;
            for (const auto* u : module.units) {
                if (u && u->source && seenPaths.insert(u->source->name()).second) {
                    mirModule.sourceFiles.push_back(makeSourceFileInfo(
                        *u->source,
                        static_cast<debug::SourceFileId>(mirModule.sourceFiles.size())));
                }
            }
        }
        for (auto& function : mirModule.functions) {
            debug::finalizeFunctionDebugInfo(
                function.debugInfo, mirModule.sourceFiles);
        }
        mirModule.languageMetadata = languageMetadataForModule(
            moduleName, mirModule, result);
        (void)mir::verifyModule(mirModule, moduleDiagnostics);
        result.diagnostics.append(moduleDiagnostics);
        if (!moduleDiagnostics.hasErrors()) {
            result.modules.push_back(mirModule);
            result.snapshot.modules[moduleName] = {
                module.sourceFingerprint,
                module.publicFingerprint,
                module.dependencyFingerprint,
                mirModule,
                semanticModel.occurrences,
            };
        }
        result.buildInfo.push_back(buildInfo);
    }

    std::stable_sort(
        result.nativeAttributes.begin(),
        result.nativeAttributes.end(),
        [](const auto& left, const auto& right) {
            if (left.target != right.target) {
                return left.target < right.target;
            }
            if (left.name != right.name) {
                return left.name < right.name;
            }
            if (left.sourceName != right.sourceName) {
                return left.sourceName < right.sourceName;
            }
            return left.offset < right.offset;
        });
    std::stable_sort(
        result.nativeSequences.begin(),
        result.nativeSequences.end(),
        [](const auto& left, const auto& right) {
            if (left.typeName != right.typeName) {
                return left.typeName < right.typeName;
            }
            return left.name < right.name;
        });
    std::stable_sort(
        result.nativeInterfaces.begin(),
        result.nativeInterfaces.end(),
        [](const auto& left, const auto& right) {
            return left.typeName < right.typeName;
        });
    return result;
}

} // namespace realscript::compiler
