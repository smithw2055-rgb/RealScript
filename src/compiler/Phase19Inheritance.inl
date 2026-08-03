// Included in Compilation.cpp after ModuleWork and visibility helpers.

semantic::TypeSymbol* phase19OwnType(
    ModuleWork& module,
    const std::string& name) {
    for (auto& type : module.types) {
        if (type.name == name ||
            semantic::canonicalTypeName(type) == name) {
            return &type;
        }
    }
    return nullptr;
}

const semantic::TypeSymbol* phase19GlobalType(
    const std::map<std::string, ModuleWork>& modules,
    const std::string& canonicalName) {
    for (const auto& [moduleName, module] : modules) {
        (void)moduleName;
        for (const auto& type : module.types) {
            if (semantic::canonicalTypeName(type) == canonicalName) {
                return &type;
            }
        }
    }
    return nullptr;
}

void resolvePhase19Inheritance(
    std::map<std::string, ModuleWork>& modules,
    BuildResult& result) {
    for (auto& [moduleName, module] : modules) {
        for (const auto* unit : module.units) {
            for (const auto& declaration : unit->syntaxTree->classes) {
                auto* type = phase19OwnType(
                    module, declaration.identifierToken.text);
                if (!type || declaration.interfaces.empty()) continue;
                const auto& candidate = declaration.interfaces.front();
                const auto found = module.visibleTypes.find(candidate.name.text);
                if (found == module.visibleTypes.end()) continue;
                if (found->second.kind != semantic::TypeKind::Class) {
                    continue;
                }
                if (found->second.id == type->id) {
                    result.diagnostics.report(
                        "RS2500", "class cannot inherit from itself",
                        candidate.span(),
                        diagnostics::DiagnosticSeverity::Error,
                        unit->source->name());
                    module.invalid = true;
                    continue;
                }
                if (found->second.sealedType) {
                    result.diagnostics.report(
                        "RS2501",
                        "cannot inherit from sealed class '" +
                            semantic::canonicalTypeName(found->second) + "'",
                        candidate.span(),
                        diagnostics::DiagnosticSeverity::Error,
                        unit->source->name());
                    module.invalid = true;
                    continue;
                }
                type->baseTypeId = found->second.id;
                type->baseTypeName =
                    semantic::canonicalTypeName(found->second);
            }
        }
    }

    std::unordered_map<semantic::SymbolId, int> colors;
    std::function<void(ModuleWork&, semantic::TypeSymbol&)> visit =
        [&](ModuleWork& module, semantic::TypeSymbol& type) {
            if (type.kind != semantic::TypeKind::Class) return;
            if (colors[type.id] == 2) return;
            if (colors[type.id] == 1) {
                result.diagnostics.report(
                    "RS2502",
                    "class inheritance cycle contains '" +
                        semantic::canonicalTypeName(type) + "'",
                    type.declarationSpan,
                    diagnostics::DiagnosticSeverity::Error,
                    type.sourceName);
                module.invalid = true;
                return;
            }
            colors[type.id] = 1;
            if (!type.baseTypeName.empty()) {
                const auto* base = phase19GlobalType(
                    modules, type.baseTypeName);
                if (base) {
                    for (auto& [baseModuleName, baseModule] : modules) {
                        (void)baseModuleName;
                        auto* mutableBase = phase19OwnType(
                            baseModule, semantic::canonicalTypeName(*base));
                        if (mutableBase) {
                            visit(baseModule, *mutableBase);
                            break;
                        }
                    }
                }
            }
            colors[type.id] = 2;
        };
    for (auto& [moduleName, module] : modules) {
        (void)moduleName;
        for (auto& type : module.types) visit(module, type);
    }
}

void applyPhase19FieldLayouts(
    std::map<std::string, ModuleWork>& modules,
    BuildResult& result) {
    std::unordered_map<semantic::SymbolId, int> complete;
    std::function<void(ModuleWork&, semantic::TypeSymbol&)> apply =
        [&](ModuleWork& module, semantic::TypeSymbol& type) {
            if (type.kind != semantic::TypeKind::Class ||
                complete[type.id] == 2) return;
            if (complete[type.id] == 1) return;
            complete[type.id] = 1;
            if (!type.baseTypeName.empty()) {
                semantic::TypeSymbol* base = nullptr;
                ModuleWork* baseModule = nullptr;
                for (auto& [name, candidateModule] : modules) {
                    (void)name;
                    auto* candidate = phase19OwnType(
                        candidateModule, type.baseTypeName);
                    if (candidate) {
                        base = candidate;
                        baseModule = &candidateModule;
                        break;
                    }
                }
                if (base && baseModule) {
                    apply(*baseModule, *base);
                    auto ownFields = type.fields;
                    type.fields = base->fields;
                    std::unordered_set<std::string> names;
                    for (const auto& field : type.fields) {
                        names.insert(field.name);
                    }
                    for (auto field : ownFields) {
                        if (!names.insert(field.name).second) {
                            result.diagnostics.report(
                                "RS2503",
                                "field '" + field.name +
                                    "' conflicts with an inherited field",
                                field.declarationSpan,
                                diagnostics::DiagnosticSeverity::Error,
                                field.sourceName);
                            module.invalid = true;
                            continue;
                        }
                        field.index = type.fields.size();
                        type.fields.push_back(std::move(field));
                    }
                }
            }
            complete[type.id] = 2;
        };
    for (auto& [moduleName, module] : modules) {
        (void)moduleName;
        for (auto& type : module.types) apply(module, type);
    }
    for (auto& [moduleName, module] : modules) {
        (void)moduleName;
        refreshVisibleTypes(modules, module);
    }
}
