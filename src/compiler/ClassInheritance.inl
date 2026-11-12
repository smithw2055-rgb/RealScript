// Included in Compilation.cpp after ModuleWork and visibility helpers.

semantic::TypeSymbol* findOwnClassOrType(
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

const semantic::TypeSymbol* findGlobalType(
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

void resolveClassInheritance(
    std::map<std::string, ModuleWork>& modules,
    BuildResult& result) {
    for (auto& [moduleName, module] : modules) {
        if (module.frontendCached) continue;
        for (const auto* unit : module.units) {
            for (const auto& declaration : unit->syntaxTree->classes) {
                auto* type = findOwnClassOrType(
                    module, declaration.identifierToken.text);
                if (!type || declaration.interfaces.empty()) continue;
                const std::size_t maxCheck = type->partialType
                    ? declaration.interfaces.size()
                    : 1u;
                for (std::size_t idx = 0; idx < maxCheck; ++idx) {
                    const auto& candidate = declaration.interfaces[idx];
                    const auto found = module.visibleTypes.find(candidate.name.text);
                    if (found == module.visibleTypes.end()) continue;
                    if (found->second.kind != semantic::TypeKind::Class ||
                        found->second.interfaceType) {
                        continue;
                    }
                    if (found->second.id == type->id) {
                        result.diagnostics.report(
                            "RS2500", "class cannot inherit from itself",
                            candidate.span(),
                            diagnostics::DiagnosticSeverity::Error,
                            unit->source->name());
                        module.invalid = true;
                        break;
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
                        break;
                    }
                    type->baseTypeId = found->second.id;
                    type->baseTypeName =
                        semantic::canonicalTypeName(found->second);
                    break;
                }
            }
        }
    }

    std::unordered_map<semantic::SymbolId, int> colors;
    std::function<void(ModuleWork&, semantic::TypeSymbol&)> visit =
        [&](ModuleWork& module, semantic::TypeSymbol& type) {
            if (module.frontendCached) return;
            if (type.kind != semantic::TypeKind::Class ||
                type.interfaceType) return;
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
                const auto* base = findGlobalType(
                    modules, type.baseTypeName);
                if (base) {
                    for (auto& [baseModuleName, baseModule] : modules) {
                        (void)baseModuleName;
                        auto* mutableBase = findOwnClassOrType(
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
        if (module.frontendCached) continue;
        for (auto& type : module.types) visit(module, type);
    }
}

void applyInheritedFieldLayouts(
    std::map<std::string, ModuleWork>& modules,
    BuildResult& result) {
    std::unordered_map<semantic::SymbolId, int> complete;
    std::function<void(ModuleWork&, semantic::TypeSymbol&)> apply =
        [&](ModuleWork& module, semantic::TypeSymbol& type) {
            if (module.frontendCached) return;
            if (type.kind != semantic::TypeKind::Class ||
                type.interfaceType || complete[type.id] == 2) return;
            if (complete[type.id] == 1) return;
            complete[type.id] = 1;
            if (!type.baseTypeName.empty()) {
                semantic::TypeSymbol* base = nullptr;
                ModuleWork* baseModule = nullptr;
                for (auto& [name, candidateModule] : modules) {
                    (void)name;
                    auto* candidate = findOwnClassOrType(
                        candidateModule, type.baseTypeName);
                    if (candidate) {
                        base = candidate;
                        baseModule = &candidateModule;
                        break;
                    }
                }
                if (base && baseModule) {
                    apply(*baseModule, *base);
                    if (base->observable) {
                        type.observable = true;
                    }
                    auto ownFields = type.fields;
                    type.fields = base->fields;
                    type.nextObservableBitIndex = base->nextObservableBitIndex;
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
                        if (type.observable && !field.staticField && !field.constField && !field.synthetic) {
                            field.observable = true;
                        }
                        if (field.observable) {
                            field.observableBitIndex = (type.nextObservableBitIndex < 63u)
                                ? type.nextObservableBitIndex++
                                : 63u;
                        }
                        type.fields.push_back(std::move(field));
                    }
                }
            }
            complete[type.id] = 2;
        };
    for (auto& [moduleName, module] : modules) {
        (void)moduleName;
        if (module.frontendCached) continue;
        for (auto& type : module.types) apply(module, type);
    }
    for (auto& [moduleName, module] : modules) {
        (void)moduleName;
        if (module.frontendCached) continue;
        refreshVisibleTypes(modules, module);
    }
}

std::size_t visibleMethodParameterOffset(
    const semantic::FunctionSymbol& function) noexcept {
    return function.method && !function.staticMethod ? 1u : 0u;
}

bool sameInheritedMethodSignature(
    const semantic::FunctionSymbol& left,
    const semantic::FunctionSymbol& right) {
    if (left.name != right.name ||
        left.staticMethod != right.staticMethod ||
        left.returnType != right.returnType ||
        left.returnTypeName != right.returnTypeName) {
        return false;
    }
    const auto leftOffset = visibleMethodParameterOffset(left);
    const auto rightOffset = visibleMethodParameterOffset(right);
    if (left.parameters.size() - leftOffset !=
        right.parameters.size() - rightOffset) {
        return false;
    }
    for (std::size_t index = 0;
         index < left.parameters.size() - leftOffset;
         ++index) {
        const auto& a = left.parameters[index + leftOffset];
        const auto& b = right.parameters[index + rightOffset];
        if (a.type != b.type || a.typeName != b.typeName ||
            a.modifier != b.modifier) {
            return false;
        }
    }
    return true;
}

std::optional<std::size_t> prepareVirtualMethod(
    semantic::TypeSymbol& owner,
    semantic::FunctionSymbol& method,
    const syntax::FunctionDeclarationSyntax& syntaxTree,
    ModuleWork& module,
    BuildResult& result,
    const std::string& sourceName) {
    constexpr auto invalidSlot =
        std::numeric_limits<std::uint32_t>::max();
    const auto report = [&](const char* code, const std::string& message) {
        result.diagnostics.report(
            code, message, syntaxTree.identifierToken.span,
            diagnostics::DiagnosticSeverity::Error, sourceName);
        module.invalid = true;
    };

    const bool polymorphic = method.virtualMethod ||
        method.overrideMethod || method.abstractMethod ||
        method.sealedMethod;
    if (owner.kind != semantic::TypeKind::Class && polymorphic) {
        report("RS2510", "virtual method modifiers require a class owner");
    }
    if (method.staticMethod && polymorphic) {
        report("RS2511", "static methods cannot be virtual, abstract, override, or sealed");
    }
    if (method.virtualMethod && method.overrideMethod) {
        report("RS2512", "a method cannot be both virtual and override");
    }
    if (method.sealedMethod && !method.overrideMethod) {
        report("RS2513", "sealed is valid only on an override method");
    }
    if (method.abstractMethod && method.sealedMethod) {
        report("RS2514", "an abstract method cannot be sealed");
    }
    if (method.abstractMethod && !owner.abstractType) {
        report("RS2515", "abstract methods require an abstract class");
    }
    if (method.abstractMethod && !syntaxTree.semicolonToken) {
        report("RS2516", "abstract methods must end with ';'");
    }
    if (!method.abstractMethod && syntaxTree.semicolonToken) {
        report("RS2517", "only abstract methods may omit a body");
    }

    std::optional<std::size_t> inheritedIndex;
    for (std::size_t index = 0; index < owner.methods.size(); ++index) {
        const auto& candidate = owner.methods[index];
        if (candidate.declaringTypeId == owner.id ||
            !sameInheritedMethodSignature(candidate, method)) {
            continue;
        }
        inheritedIndex = index;
        break;
    }

    if (method.overrideMethod) {
        if (!inheritedIndex) {
            report("RS2518", "override method has no matching inherited method");
            return std::nullopt;
        }
        const auto& inherited = owner.methods[*inheritedIndex];
        if (!(inherited.virtualMethod || inherited.overrideMethod ||
              inherited.abstractMethod) || inherited.virtualSlot == invalidSlot) {
            report("RS2519", "override target is not virtual or abstract");
            return inheritedIndex;
        }
        if (inherited.sealedMethod) {
            report("RS2520", "cannot override a sealed method");
            return inheritedIndex;
        }
        if (method.accessibility != inherited.accessibility) {
            report(
                "RS2525",
                "override method must preserve inherited accessibility");
            return inheritedIndex;
        }
        method.virtualMethod = true;
        method.virtualSlot = inherited.virtualSlot;
        if (owner.virtualDispatchTable.size() <= method.virtualSlot) {
            owner.virtualDispatchTable.resize(
                static_cast<std::size_t>(method.virtualSlot) + 1, 0);
        }
        owner.virtualDispatchTable[method.virtualSlot] =
            method.abstractMethod ? 0 : method.id;
        return inheritedIndex;
    }

    if (inheritedIndex) {
        report("RS2521", "method hides an inherited member; use override");
    }
    if (method.virtualMethod || method.abstractMethod) {
        if (method.virtualSlot == invalidSlot) {
            method.virtualSlot = static_cast<std::uint32_t>(
                owner.virtualDispatchTable.size());
        }
        if (owner.virtualDispatchTable.size() <= method.virtualSlot) {
            owner.virtualDispatchTable.resize(
                static_cast<std::size_t>(method.virtualSlot) + 1, 0);
        }
        owner.virtualDispatchTable[method.virtualSlot] =
            method.abstractMethod ? 0 : method.id;
    }
    return std::nullopt;
}

void validateConcreteClassType(
    const semantic::TypeSymbol& owner,
    ModuleWork& module,
    BuildResult& result) {
    if (owner.kind != semantic::TypeKind::Class ||
        owner.interfaceType) return;
    if (owner.abstractType && owner.sealedType) {
        result.diagnostics.report(
            "RS2522", "a class cannot be both abstract and sealed",
            owner.declarationSpan,
            diagnostics::DiagnosticSeverity::Error,
            owner.sourceName);
        module.invalid = true;
    }
    if (owner.abstractType) return;
    for (std::size_t slot = 0;
         slot < owner.virtualDispatchTable.size(); ++slot) {
        if (owner.virtualDispatchTable[slot] == 0) {
            result.diagnostics.report(
                "RS2523",
                "non-abstract class '" +
                    semantic::canonicalTypeName(owner) +
                    "' does not implement virtual slot " +
                    std::to_string(slot),
                owner.declarationSpan,
                diagnostics::DiagnosticSeverity::Error,
                owner.sourceName);
            module.invalid = true;
        }
    }
}
