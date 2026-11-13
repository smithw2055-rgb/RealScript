#include "CompilationInternal.h"

#include <algorithm>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace realscript::compiler::detail {

void addVisibleType(
    semantic::TypeSymbolMap& map,
    const semantic::TypeSymbol& type,
    bool includeSimpleName) {
    map[semantic::canonicalTypeName(type)] = type;
    if (includeSimpleName) map[type.name] = type;
}

void refreshVisibleTypes(
    std::map<std::string, ModuleWork>& modules,
    ModuleWork& module) {
    module.visibleTypes.clear();
    for (const auto& importedName : module.imports) {
        const auto imported = modules.find(importedName);
        if (imported == modules.end()) continue;
        for (const auto& type : imported->second.types) {
            module.visibleTypes[semantic::canonicalTypeName(type)] = type;
            module.visibleTypes.try_emplace(type.name, type);
        }
    }
    for (const auto& type : module.types) {
        addVisibleType(module.visibleTypes, type, true);
    }
    for (const auto& hygienicName : module.hygienicModules) {
        const auto hyg = modules.find(hygienicName);
        if (hyg == modules.end()) continue;
        for (const auto& type : hyg->second.types) {
            addVisibleType(module.visibleTypes, type, false);
        }
    }
}

std::string attributeValueText(
    const syntax::AttributeArgumentSyntax& argument,
    const text::SourceText& source) {
    const auto span = argument.valueSpan();
    return span.empty()
        ? std::string{}
        : std::string(source.view(span));
}

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
                const auto& candidate = declaration.interfaces.front();
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
        const auto& inherited = owner.methods[*inheritedIndex];
        if (inherited.synthetic && inherited.virtualSlot != invalidSlot) {
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

void appendNativeAttributes(
    std::vector<LanguageAttributeRecord>& output,
    const std::vector<syntax::AttributeListSyntax>& lists,
    const std::string& target,
    const text::SourceText& source) {
    for (const auto& list : lists) {
        for (const auto& attribute : list.attributes) {
            LanguageAttributeRecord record;
            record.target = target;
            record.name = attribute.nameToken.text;
            record.sourceName = source.name();
            record.offset = attribute.nameToken.span.start;
            std::size_t positional = 0;
            for (const auto& argument : attribute.arguments) {
                LanguageAttributeArgument value;
                value.name = argument.nameToken
                    ? argument.nameToken->text
                    : "arg" + std::to_string(positional++);
                value.value = attributeValueText(argument, source);
                record.arguments.push_back(std::move(value));
            }
            output.push_back(std::move(record));
        }
    }
}

std::string memberAttributeTarget(
    const std::string& owner,
    const std::string& kind,
    const std::string& name,
    std::size_t arity) {
    std::ostringstream out;
    out << owner << "::" << kind << ':' << name;
    if (kind == "method" || kind == "ctor" ||
        kind == "function") {
        out << '#' << arity;
    }
    return out.str();
}

std::string canonicalInterfaceName(
    const std::string& moduleName,
    const std::string& name) {
    return moduleName.empty() ? name : moduleName + "::" + name;
}


std::string canonicalDelegateName(
    const std::string& moduleName,
    const std::string& name) {
    return moduleName.empty() ? name : moduleName + "::" + name;
}

void refreshVisibleDelegates(
    std::map<std::string, ModuleWork>& modules,
    ModuleWork& module) {
    module.visibleDelegates.clear();
    for (const auto& importedName : module.imports) {
        const auto imported = modules.find(importedName);
        if (imported == modules.end()) continue;
        for (const auto& [name, contract] : imported->second.delegates) {
            module.visibleDelegates.try_emplace(name, contract);
            module.visibleDelegates[
                canonicalDelegateName(contract.moduleName, contract.name)] = contract;
        }
    }
    for (const auto& [name, contract] : module.delegates) {
        module.visibleDelegates[name] = contract;
        module.visibleDelegates[
            canonicalDelegateName(contract.moduleName, contract.name)] = contract;
    }
    for (const auto& hygienicName : module.hygienicModules) {
        const auto hyg = modules.find(hygienicName);
        if (hyg == modules.end()) continue;
        for (const auto& [name, contract] : hyg->second.delegates) {
            (void)name;
            module.visibleDelegates[
                canonicalDelegateName(contract.moduleName, contract.name)] = contract;
        }
    }
}

void refreshVisibleInterfaces(
    std::map<std::string, ModuleWork>& modules,
    ModuleWork& module) {
    module.visibleInterfaces.clear();
    for (const auto& importedName : module.imports) {
        const auto imported = modules.find(importedName);
        if (imported == modules.end()) continue;
        for (const auto& [name, contract] : imported->second.interfaces) {
            module.visibleInterfaces.try_emplace(name, contract);
            module.visibleInterfaces[
                canonicalInterfaceName(contract.moduleName, contract.name)] = contract;
        }
    }
    for (const auto& [name, contract] : module.interfaces) {
        module.visibleInterfaces[name] = contract;
        module.visibleInterfaces[
            canonicalInterfaceName(contract.moduleName, contract.name)] = contract;
    }
    for (const auto& hygienicName : module.hygienicModules) {
        const auto hyg = modules.find(hygienicName);
        if (hyg == modules.end()) continue;
        for (const auto& [name, contract] : hyg->second.interfaces) {
            (void)name;
            module.visibleInterfaces[
                canonicalInterfaceName(contract.moduleName, contract.name)] = contract;
        }
    }
}

void appendStampedDiagnostics(
    diagnostics::DiagnosticBag& target,
    const diagnostics::DiagnosticBag& sourceBag,
    const std::string& defaultSourceName) {
    for (const auto& item : sourceBag.items()) {
        target.report(
            item.code,
            item.message,
            item.span,
            item.severity,
            item.sourceName.empty() ? defaultSourceName : item.sourceName);
    }
}

InterfaceTypeRef resolveInterfaceType(
    const syntax::TypeSyntax& syntaxTree,
    const semantic::TypeSymbolMap& visibleTypes,
    diagnostics::DiagnosticBag& diagnostics,
    const std::string& sourceName,
    bool allowVoid) {
    InterfaceTypeRef result;
    auto baseType = semantic::resolvePrimitiveType(syntaxTree.name.text);
    std::string exactName;
    if (baseType == semantic::PrimitiveType::Error) {
        const auto found = visibleTypes.find(syntaxTree.name.text);
        if (found == visibleTypes.end()) {
            diagnostics.report(
                "RS2470",
                "unknown interface signature type '" + syntaxTree.name.text + "'",
                syntaxTree.span(),
                diagnostics::DiagnosticSeverity::Error,
                sourceName);
            return result;
        }
        baseType = found->second.kind == semantic::TypeKind::Class
            ? semantic::PrimitiveType::Object
            : found->second.kind == semantic::TypeKind::Struct
                ? semantic::PrimitiveType::Struct
                : semantic::PrimitiveType::Enum;
        exactName = semantic::canonicalTypeName(found->second);
    }
    if (syntaxTree.isArray()) {
        if (baseType == semantic::PrimitiveType::Void ||
            baseType == semantic::PrimitiveType::Array) {
            diagnostics.report(
                "RS2470",
                "invalid interface array type",
                syntaxTree.span(),
                diagnostics::DiagnosticSeverity::Error,
                sourceName);
            return result;
        }
        result.type = semantic::PrimitiveType::Array;
        result.typeName = semantic::arrayTypeName(baseType, exactName);
        return result;
    }
    if (baseType == semantic::PrimitiveType::Void && !allowVoid) {
        diagnostics.report(
            "RS2470",
            "void is not valid for an interface parameter",
            syntaxTree.span(),
            diagnostics::DiagnosticSeverity::Error,
            sourceName);
        return result;
    }
    result.type = baseType;
    if (semantic::isExactType(baseType)) result.typeName = exactName;
    return result;
}

bool sameInterfaceType(
    const InterfaceTypeRef& expected,
    semantic::PrimitiveType actual,
    const std::string& actualName) {
    return expected.type == actual &&
        (!semantic::isExactType(expected.type) ||
         expected.typeName == actualName);
}

std::string interfaceMethodSignature(
    const InterfaceMethodContract& method) {
    std::ostringstream out;
    out << method.name << '(';
    for (std::size_t idx = 0; idx < method.parameters.size(); ++idx) {
        const auto& parameter = method.parameters[idx];
        const auto mod = idx < method.parameterModifiers.size()
            ? method.parameterModifiers[idx]
            : semantic::ParameterModifier::None;
        if (mod != semantic::ParameterModifier::None) {
            out << static_cast<int>(mod) << '@';
        }
        out << semantic::primitiveTypeName(parameter.type) << '#'
            << parameter.typeName << ';';
    }
    out << ")->" << semantic::primitiveTypeName(method.returnType.type)
        << '#' << method.returnType.typeName;
    return out.str();
}

std::string fieldTypeSignature(const semantic::FieldSymbol& field) {
    if (semantic::isExactType(field.type) && !field.typeName.empty()) {
        return field.typeName;
    }
    return semantic::primitiveTypeName(field.type);
}

std::string typeSignature(const semantic::TypeSymbol& type) {
    std::ostringstream out;
    out << static_cast<int>(type.kind) << ':'
        << (type.delegateType ? "delegate:" : "")
        << (type.interfaceType ? "interface:" : "")
        << semantic::canonicalTypeName(type) << '{';
    for (const auto& field : type.fields) {
        out << field.name << ':' << fieldTypeSignature(field)
            << (field.synthetic ? ":synthetic" : "") << ';';
    }
    for (const auto& constructor : type.constructors) {
        out << "ctor:" << semantic::canonicalFunctionSignature(constructor) << ';';
    }
    if (type.staticConstructor) {
        out << "cctor:" << semantic::canonicalFunctionSignature(*type.staticConstructor) << ';';
    }
    for (const auto& method : type.methods) {
        out << "method:" << (method.staticMethod ? "static:" : "instance:")
            << semantic::canonicalFunctionSignature(method);
        if (method.interfaceMethod) {
            out << ":islot=" << method.interfaceSlot;
        }
        out << ';';
    }
    for (const auto& interfaceMap : type.interfaceDispatchMaps) {
        out << "imap:" << interfaceMap.interfaceTypeId << '[';
        for (const auto functionId : interfaceMap.slots) {
            out << functionId << ';';
        }
        out << "];";
    }
    for (const auto& event : type.events) {
        out << "event:" << event.name << ':' << event.delegateName << '(';
        for (const auto& parameter : event.parameters) {
            out << semantic::primitiveTypeName(parameter.type) << '#'
                << parameter.typeName << ';';
        }
        out << ");";
    }
    for (const auto& property : type.properties) {
        out << "property:" << property.name << ':';
        if (semantic::isExactType(property.type) && !property.typeName.empty()) {
            out << property.typeName;
        } else {
            out << semantic::primitiveTypeName(property.type);
        }
        out << ':' << (property.staticProperty ? "static:" : "instance:")
            << (property.getter.has_value() ? 'g' : '-')
            << (property.setter.has_value() ? 's' : '-') << ';';
    }
    for (const auto& member : type.enumMembers) {
        out << "enum:" << member.name << '=' << member.value << ';';
    }
    out << '}';
    return out.str();
}

semantic::TypeSymbol* findOwnType(ModuleWork& module, const std::string& name) {
    for (auto& type : module.types) {
        if (type.name == name) return &type;
    }
    return nullptr;
}



LanguageModuleMetadata languageMetadataForModule(
    const std::string& moduleName,
    const mir::Module& module,
    const BuildResult& result) {
    LanguageModuleMetadata metadata;
    const auto prefix = moduleName + "::";
    for (const auto& attribute : result.nativeAttributes) {
        if (attribute.target.rfind(prefix, 0) == 0) {
            metadata.attributes.push_back(attribute);
        }
    }
    for (const auto& implementation : result.nativeInterfaces) {
        if (implementation.typeName.rfind(prefix, 0) == 0) {
            metadata.interfaces.push_back(implementation);
        }
    }
    for (const auto& sequence : result.nativeSequences) {
        if (sequence.typeName.rfind(prefix, 0) == 0) {
            metadata.sequences.push_back(sequence);
        }
    }
    for (const auto& instantiation : result.nativeGenericInstantiations) {
        bool emitted = false;
        for (const auto& type : module.types) {
            if (type.moduleName == moduleName &&
                type.name == instantiation.generatedName) {
                emitted = true;
                break;
            }
        }
        if (!emitted) {
            const auto dotPos = instantiation.genericName.find('.');
            const std::string expectedMemberFnName = dotPos != std::string::npos
                ? (instantiation.genericName.substr(0, dotPos + 1) + instantiation.generatedName)
                : std::string{};
            for (const auto& function : module.functions) {
                if (function.moduleName == moduleName &&
                    (function.name == instantiation.generatedName ||
                     (!expectedMemberFnName.empty() && function.name == expectedMemberFnName))) {
                    emitted = true;
                    break;
                }
            }
        }
        if (emitted) metadata.genericInstantiations.push_back(instantiation);
    }
    std::stable_sort(
        metadata.attributes.begin(), metadata.attributes.end(),
        [](const auto& left, const auto& right) {
            if (left.target != right.target) return left.target < right.target;
            if (left.name != right.name) return left.name < right.name;
            if (left.sourceName != right.sourceName) {
                return left.sourceName < right.sourceName;
            }
            return left.offset < right.offset;
        });
    std::sort(
        metadata.interfaces.begin(), metadata.interfaces.end(),
        [](const auto& left, const auto& right) {
            return left.typeName < right.typeName;
        });
    std::sort(
        metadata.genericInstantiations.begin(),
        metadata.genericInstantiations.end(),
        [](const auto& left, const auto& right) {
            if (left.generatedName != right.generatedName) {
                return left.generatedName < right.generatedName;
            }
            if (left.genericName != right.genericName) {
                return left.genericName < right.genericName;
            }
            return left.arguments < right.arguments;
        });
    std::stable_sort(
        metadata.sequences.begin(), metadata.sequences.end(),
        [](const auto& left, const auto& right) {
            if (left.typeName != right.typeName) {
                return left.typeName < right.typeName;
            }
            return left.name < right.name;
        });
    for (const auto& function : module.functions) {
        if (function.moduleName == moduleName &&
            function.accessibility != semantic::Accessibility::Public) {
            metadata.nonPublicFunctions.push_back(compiler::LanguageFunctionAccessibility{
                function.symbolId,
                static_cast<std::uint8_t>(function.accessibility),
            });
        }
    }
    std::sort(
        metadata.nonPublicFunctions.begin(),
        metadata.nonPublicFunctions.end(),
        [](const auto& left, const auto& right) {
            return left.symbolId < right.symbolId;
        });
    return metadata;
}

debug::SourceFileInfo makeSourceFileInfo(
    const text::SourceText& source,
    debug::SourceFileId id) {
    debug::SourceFileInfo info;
    info.id = id;
    info.path = source.name();
    info.contentHash = stableFingerprint(source.content());
    info.lineStarts.push_back(0);
    for (std::size_t index = 0; index < source.content().size(); ++index) {
        if (source.content()[index] == '\n') {
            info.lineStarts.push_back(static_cast<std::uint32_t>(index + 1));
        }
    }
    return info;
}

void appendParameters(
    semantic::FunctionBindingInput& binding,
    const std::vector<syntax::ParameterSyntax>& parameters) {
    for (const auto& parameter : parameters) {
        binding.parameterNames.push_back(parameter.identifierToken.text);
        binding.parameterSpans.push_back(parameter.identifierToken.span);
    }
}

void discoverTypesContractsAndLayouts(
    std::map<std::string, ModuleWork>& modules,
    BuildResult& result) {
    // Stable source order and named-type shells.
    for (auto& moduleEntry : modules) {
        const auto& moduleName = moduleEntry.first;
        auto& module = moduleEntry.second;
        if (module.frontendCached) continue;
        std::unordered_set<std::string> typeNames;
        const auto addShell = [&](auto const& syntaxNode, auto declare) {
            auto type = declare(moduleName, syntaxNode);
            if (module.deterministic) type.deterministic = true;
            if (!typeNames.insert(type.name).second) {
                result.diagnostics.report(
                    "RS4004", "duplicate type '" + type.name + "'",
                    syntaxNode.identifierToken.span,
                    diagnostics::DiagnosticSeverity::Error);
                module.invalid = true;
                return;
            }
            module.types.push_back(std::move(type));
        };
        for (const auto* unit : module.units) {
            for (const auto& node : unit->syntaxTree->classes) {
                if (!node.typeParameters.empty()) continue;
                const auto before = module.types.size();
                addShell(node, [](const std::string& name, const auto& syntaxNode) {
                    return semantic::declareTypeShell(name, syntaxNode);
                });
                if (module.types.size() != before) {
                    module.types.back().sourceName = unit->source->name();
                    module.types.back().declarationSpan = node.identifierToken.span;
                    module.types.back().hygienicAccessibleModules.insert(
                        unit->unitHygienicModules.begin(), unit->unitHygienicModules.end());
                }
            }
            for (const auto& node : unit->syntaxTree->structs) {
                if (!node.typeParameters.empty()) continue;
                const auto before = module.types.size();
                addShell(node, [](const std::string& name, const auto& syntaxNode) {
                    return semantic::declareTypeShell(name, syntaxNode);
                });
                if (module.types.size() != before) {
                    module.types.back().sourceName = unit->source->name();
                    module.types.back().declarationSpan = node.identifierToken.span;
                    module.types.back().hygienicAccessibleModules.insert(
                        unit->unitHygienicModules.begin(), unit->unitHygienicModules.end());
                }
            }
            for (const auto& node : unit->syntaxTree->enums) {
                const auto before = module.types.size();
                addShell(node, [](const std::string& name, const auto& syntaxNode) {
                    return semantic::declareTypeShell(name, syntaxNode);
                });
                if (module.types.size() != before) {
                    module.types.back().sourceName = unit->source->name();
                    module.types.back().declarationSpan = node.identifierToken.span;
                    module.types.back().hygienicAccessibleModules.insert(
                        unit->unitHygienicModules.begin(), unit->unitHygienicModules.end());
                }
            }
            for (const auto& node : unit->syntaxTree->interfaces) {
                if (!node.typeParameters.empty()) continue;
                if (!typeNames.insert(node.identifierToken.text).second) {
                    result.diagnostics.report(
                        "RS4004",
                        "duplicate type or interface '" +
                            node.identifierToken.text + "'",
                        node.identifierToken.span,
                        diagnostics::DiagnosticSeverity::Error,
                        unit->source->name());
                    module.invalid = true;
                    continue;
                }
                semantic::TypeSymbol type;
                type.kind = semantic::TypeKind::Class;
                type.accessibility = semantic::accessibilityFromModifiers(
                    node.modifiers);
                type.interfaceType = true;
                type.abstractType = true;
                type.sealedType = true;
                type.moduleName = moduleName;
                type.name = node.identifierToken.text;
                type.id = semantic::stableTypeId(type);
                type.sourceName = unit->source->name();
                type.declarationSpan = node.identifierToken.span;
                module.types.push_back(std::move(type));
                module.interfaceInputs.push_back(
                    InterfaceDeclarationInput{
                        &node,
                        unit->source->name(),
                        unit->unitHygienicModules});
            }
            for (const auto& node : unit->syntaxTree->delegates) {
                if (!node.typeParameters.empty()) continue;
                if (!typeNames.insert(node.identifierToken.text).second) {
                    result.diagnostics.report(
                        "RS4004",
                        "duplicate type or delegate '" +
                            node.identifierToken.text + "'",
                        node.identifierToken.span,
                        diagnostics::DiagnosticSeverity::Error,
                        unit->source->name());
                    module.invalid = true;
                    continue;
                }
                semantic::TypeSymbol type;
                type.kind = semantic::TypeKind::Class;
                type.accessibility = semantic::Accessibility::Public;
                type.delegateType = true;
                type.sealedType = true;
                type.moduleName = moduleName;
                type.name = node.identifierToken.text;
                type.id = semantic::stableTypeId(type);
                type.sourceName = unit->source->name();
                type.declarationSpan = node.identifierToken.span;
                module.types.push_back(std::move(type));
                module.delegateInputs.push_back(
                    DelegateDeclarationInput{&node, unit->source->name()});
            }
        }
    }

    for (auto& [name, module] : modules) {
        (void)name;
        if (module.frontendCached) continue;
        refreshVisibleTypes(modules, module);
    }

    // Resolve native interface contracts after all named type shells exist.
    for (auto& [moduleName, module] : modules) {
        if (module.frontendCached) continue;
        for (const auto& input : module.interfaceInputs) {
            if (!input.syntax) continue;
            InterfaceContract contract;
            contract.moduleName = moduleName;
            contract.name = input.syntax->identifierToken.text;
            contract.sourceName = input.sourceName;
            contract.declarationSpan = input.syntax->identifierToken.span;
            auto* descriptor = findOwnType(module, contract.name);
            if (!descriptor || !descriptor->interfaceType) {
                module.invalid = true;
                continue;
            }
            descriptor->methods.clear();
            std::unordered_set<std::string> signatures;
            for (const auto& methodSyntax : input.syntax->methods) {
                InterfaceMethodContract method;
                method.name = methodSyntax.identifierToken.text;
                method.declarationSpan = methodSyntax.identifierToken.span;
                method.returnType = resolveInterfaceType(
                    methodSyntax.returnType,
                    module.visibleTypes,
                    result.diagnostics,
                    input.sourceName,
                    true);
                for (const auto& parameter : methodSyntax.parameters) {
                    semantic::ParameterModifier pMod = semantic::ParameterModifier::None;
                    if (parameter.modifierToken) {
                        if (parameter.modifierToken->kind == syntax::SyntaxKind::RefKeyword)
                            pMod = semantic::ParameterModifier::Ref;
                        else if (parameter.modifierToken->kind == syntax::SyntaxKind::OutKeyword)
                            pMod = semantic::ParameterModifier::Out;
                        else if (parameter.modifierToken->kind == syntax::SyntaxKind::InKeyword)
                            pMod = semantic::ParameterModifier::In;
                    }
                    method.parameterModifiers.push_back(pMod);
                    method.parameters.push_back(resolveInterfaceType(
                        parameter.type,
                        module.visibleTypes,
                        result.diagnostics,
                        input.sourceName,
                        false));
                }
                if (methodSyntax.hasBody()) {
                    method.hasDefaultImplementation = true;
                    method.methodSyntax = &methodSyntax;
                    method.sourceName = input.sourceName;
                    method.hygienicModules.insert(moduleName);
                    method.hygienicModules.insert(module.imports.begin(), module.imports.end());
                    method.hygienicModules.insert(
                        input.unitHygienicModules.begin(),
                        input.unitHygienicModules.end());

                }
                const auto signature = interfaceMethodSignature(method);
                if (!signatures.insert(signature).second) {
                    result.diagnostics.report(
                        "RS2472",
                        "duplicate interface method '" + signature + "'",
                        methodSyntax.identifierToken.span,
                        diagnostics::DiagnosticSeverity::Error,
                        input.sourceName);
                    module.invalid = true;
                }
                contract.methods.push_back(std::move(method));

                auto symbol = semantic::declareFunctionSymbol(
                    moduleName, methodSyntax, module.visibleTypes,
                    result.diagnostics, descriptor);
                symbol.accessibility = semantic::Accessibility::Public;
                symbol.abstractMethod = true;
                symbol.virtualMethod = false;
                symbol.overrideMethod = false;
                symbol.sealedMethod = false;
                symbol.virtualSlot =
                    std::numeric_limits<std::uint32_t>::max();
                symbol.interfaceMethod = true;
                symbol.sourceName = input.sourceName;
                descriptor->methods.push_back(std::move(symbol));
            }

            std::vector<std::pair<std::string, std::size_t>> slotOrder;
            slotOrder.reserve(contract.methods.size());
            for (std::size_t index = 0;
                 index < contract.methods.size(); ++index) {
                slotOrder.emplace_back(
                    interfaceMethodSignature(contract.methods[index]),
                    index);
            }
            std::sort(
                slotOrder.begin(), slotOrder.end(),
                [](const auto& left, const auto& right) {
                    return left.first < right.first;
                });
            for (std::size_t slot = 0;
                 slot < slotOrder.size(); ++slot) {
                descriptor->methods[slotOrder[slot].second].interfaceSlot =
                    static_cast<std::uint32_t>(slot);
            }
            module.interfaces[contract.name] = std::move(contract);
        }
    }
    for (auto& [name, module] : modules) {
        (void)name;
        if (module.frontendCached) continue;
        refreshVisibleInterfaces(modules, module);
    }
    resolveClassInheritance(modules, result);
    for (auto& [name, module] : modules) {
        (void)name;
        if (module.frontendCached) continue;
        refreshVisibleTypes(modules, module);
    }

    // Resolve native delegate contracts after all named type shells exist.
    for (auto& [moduleName, module] : modules) {
        if (module.frontendCached) continue;
        for (const auto& input : module.delegateInputs) {
            if (!input.syntax) continue;
            DelegateContract contract;
            contract.moduleName = moduleName;
            contract.name = input.syntax->identifierToken.text;
            contract.sourceName = input.sourceName;
            contract.declarationSpan = input.syntax->identifierToken.span;
            contract.returnType = resolveInterfaceType(
                input.syntax->returnType,
                module.visibleTypes,
                result.diagnostics,
                input.sourceName,
                true);
            for (const auto& parameter : input.syntax->parameters) {
                semantic::ParameterModifier modifier =
                    semantic::ParameterModifier::None;
                if (parameter.modifierToken) {
                    switch (parameter.modifierToken->kind) {
                    case syntax::SyntaxKind::RefKeyword:
                        modifier = semantic::ParameterModifier::Ref;
                        break;
                    case syntax::SyntaxKind::OutKeyword:
                        modifier = semantic::ParameterModifier::Out;
                        break;
                    case syntax::SyntaxKind::InKeyword:
                        modifier = semantic::ParameterModifier::In;
                        break;
                    default:
                        break;
                    }
                }
                contract.parameterModifiers.push_back(modifier);
                contract.parameters.push_back(resolveInterfaceType(
                    parameter.type,
                    module.visibleTypes,
                    result.diagnostics,
                    input.sourceName,
                    false));
            }
            auto* descriptor = findOwnType(module, contract.name);
            if (!descriptor || !descriptor->delegateType) {
                module.invalid = true;
                continue;
            }
            semantic::FunctionSymbol invoke;
            invoke.accessibility = semantic::Accessibility::Public;
            invoke.declaringTypeId = descriptor->id;
            invoke.declaringTypeName =
                semantic::canonicalTypeName(*descriptor);
            invoke.moduleName = moduleName;
            invoke.name = "Invoke";
            invoke.ownerTypeName = descriptor->name;
            invoke.ownerTypeId = descriptor->id;
            invoke.returnType = contract.returnType.type;
            invoke.returnTypeName = contract.returnType.typeName;
            invoke.method = true;
            invoke.synthetic = true;
            invoke.sourceName = input.sourceName;
            invoke.declarationSpan = input.syntax->identifierToken.span;
            semantic::VariableSymbol self;
            self.name = "this";
            self.type = semantic::PrimitiveType::Object;
            self.typeName = semantic::canonicalTypeName(*descriptor);
            self.parameter = true;
            self.index = 0;
            self.declarationSpan = input.syntax->identifierToken.span;
            invoke.parameters.push_back(std::move(self));
            for (std::size_t index = 0;
                 index < contract.parameters.size(); ++index) {
                semantic::VariableSymbol parameter;
                parameter.name = input.syntax->parameters[index]
                    .identifierToken.text;
                parameter.type = contract.parameters[index].type;
                parameter.typeName = contract.parameters[index].typeName;
                parameter.modifier = contract.parameterModifiers[index];
                if (parameter.modifier ==
                        semantic::ParameterModifier::Ref ||
                    parameter.modifier ==
                        semantic::ParameterModifier::Out) {
                    parameter.storageType =
                        semantic::PrimitiveType::Object;
                    parameter.storageTypeName =
                        semantic::referenceWrapperTypeName(
                            moduleName,
                            parameter.type,
                            parameter.typeName);
                } else {
                    parameter.storageType = parameter.type;
                    parameter.storageTypeName = parameter.typeName;
                }
                parameter.parameter = true;
                parameter.index = invoke.parameters.size();
                parameter.declarationSpan = input.syntax->parameters[index]
                    .identifierToken.span;
                invoke.parameters.push_back(std::move(parameter));
            }
            invoke.id = semantic::stableFunctionId(invoke);
            descriptor->methods.clear();
            descriptor->methods.push_back(std::move(invoke));
            module.delegates[contract.name] = std::move(contract);
        }
    }
    for (auto& [name, module] : modules) {
        (void)name;
        if (module.frontendCached) continue;
        refreshVisibleDelegates(modules, module);
    }

    // Resolve all enum values first, then field layouts and cross-type constants.
    for (auto& [moduleName, module] : modules) {
        (void)moduleName;
        if (module.frontendCached) continue;
        for (const auto* unit : module.units) {
            for (const auto& node : unit->syntaxTree->enums) {
                auto* type = findOwnType(module, node.identifierToken.text);
                if (!type || !semantic::populateEnumMembers(
                        *type, node, result.diagnostics)) {
                    module.invalid = true;
                } else {
                    type->sourceName = unit->source->name();
                    for (auto& member : type->enumMembers) member.sourceName = unit->source->name();
                }
            }
        }
    }
    for (auto& [moduleName, module] : modules) {
        (void)moduleName;
        if (module.frontendCached) continue;
        refreshVisibleTypes(modules, module);
    }
    for (auto& [moduleName, module] : modules) {
        (void)moduleName;
        if (module.frontendCached) continue;
        for (const auto* unit : module.units) {
            for (const auto& node : unit->syntaxTree->classes) {
                if (!node.typeParameters.empty()) continue;
                auto* type = findOwnType(module, node.identifierToken.text);
                diagnostics::DiagnosticBag localBag;
                if (!type || !semantic::populateTypeFields(
                        *type, node, module.visibleTypes, localBag, true)) {
                    appendStampedDiagnostics(result.diagnostics, localBag, unit->source->name());
                    module.invalid = true;
                } else {
                    appendStampedDiagnostics(result.diagnostics, localBag, unit->source->name());
                    type->sourceName = unit->source->name();
                    for (auto& field : type->fields) field.sourceName = unit->source->name();
                }
            }
            for (const auto& node : unit->syntaxTree->structs) {
                if (!node.typeParameters.empty()) continue;
                auto* type = findOwnType(module, node.identifierToken.text);
                diagnostics::DiagnosticBag localBag;
                if (!type || !semantic::populateTypeFields(
                        *type, node, module.visibleTypes, localBag, true)) {
                    appendStampedDiagnostics(result.diagnostics, localBag, unit->source->name());
                    module.invalid = true;
                } else {
                    appendStampedDiagnostics(result.diagnostics, localBag, unit->source->name());
                    type->sourceName = unit->source->name();
                    for (auto& field : type->fields) field.sourceName = unit->source->name();
                }
            }
        }
    }
    applyInheritedFieldLayouts(modules, result);
    {
        bool constProgressed = true;
        while (constProgressed) {
            constProgressed = false;
            for (auto& [moduleName, module] : modules) {
                (void)moduleName;
                if (module.frontendCached) continue;
                bool moduleProgressed = false;
                for (auto& type : module.types) {
                    if (type.kind != semantic::TypeKind::Class &&
                        type.kind != semantic::TypeKind::Struct) {
                        continue;
                    }
                    if (semantic::resolveConstantFieldsPass(type, module.visibleTypes)) {
                        moduleProgressed = true;
                        constProgressed = true;
                    }
                }
                if (moduleProgressed) {
                    refreshVisibleTypes(modules, module);
                }
            }
            if (constProgressed) {
                for (auto& [moduleName, module] : modules) {
                    (void)moduleName;
                    if (module.frontendCached) continue;
                    refreshVisibleTypes(modules, module);
                }
            }
        }
        for (auto& [moduleName, module] : modules) {
            (void)moduleName;
            if (module.frontendCached) continue;
            for (auto& type : module.types) {
                if (type.kind != semantic::TypeKind::Class &&
                    type.kind != semantic::TypeKind::Struct) {
                    continue;
                }
                if (!semantic::validateConstantFields(
                        type, module.visibleTypes, result.diagnostics)) {
                    module.invalid = true;
                }
            }
            refreshVisibleTypes(modules, module);
        }
    }
}

} // namespace realscript::compiler::detail
