#include "realscript/compiler/Compilation.h"

#include "realscript/semantic/Semantic.h"
#include "realscript/syntax/Syntax.h"
#include "realscript/text/Text.h"

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <unordered_map>
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

struct InterfaceTypeRef {
    semantic::PrimitiveType type = semantic::PrimitiveType::Error;
    std::string typeName;
};

struct InterfaceMethodContract {
    std::string name;
    InterfaceTypeRef returnType;
    std::vector<InterfaceTypeRef> parameters;
    text::TextSpan declarationSpan;
};

struct InterfaceContract {
    std::string moduleName;
    std::string name;
    std::string sourceName;
    text::TextSpan declarationSpan;
    std::vector<InterfaceMethodContract> methods;
};

struct InterfaceDeclarationInput {
    const syntax::InterfaceDeclarationSyntax* syntax = nullptr;
    std::string sourceName;
};

using InterfaceMap = std::map<std::string, InterfaceContract>;

struct DelegateContract {
    std::string moduleName;
    std::string name;
    std::string sourceName;
    InterfaceTypeRef returnType;
    std::vector<InterfaceTypeRef> parameters;
    text::TextSpan declarationSpan;
};

struct DelegateDeclarationInput {
    const syntax::DelegateDeclarationSyntax* syntax = nullptr;
    std::string sourceName;
};

using DelegateMap = std::map<std::string, DelegateContract>;

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
    std::vector<semantic::TypeSymbol> types;
    semantic::TypeSymbolMap visibleTypes;
    std::vector<InterfaceDeclarationInput> interfaceInputs;
    InterfaceMap interfaces;
    InterfaceMap visibleInterfaces;
    std::vector<DelegateDeclarationInput> delegateInputs;
    DelegateMap delegates;
    DelegateMap visibleDelegates;
    std::vector<semantic::FunctionSymbol> declarations;
    std::vector<semantic::FunctionBindingInput> functionBindings;
    std::uint64_t sourceFingerprint = 0;
    std::uint64_t publicFingerprint = 0;
    std::uint64_t dependencyFingerprint = 0;
    bool invalid = false;
};

#include "NativeGenerics.inl"

std::uint64_t combineFingerprint(
    std::uint64_t seed,
    std::uint64_t value) noexcept {
    seed ^= value + 0x9e3779b97f4a7c15ull +
        (seed << 6) + (seed >> 2);
    return seed;
}

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
    for (const auto& type : module.types) addVisibleType(module.visibleTypes, type, true);
    for (const auto& importedName : module.imports) {
        const auto imported = modules.find(importedName);
        if (imported == modules.end()) continue;
        for (const auto& type : imported->second.types) {
            addVisibleType(module.visibleTypes, type, true);
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
    std::size_t arity = 0) {
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
    for (const auto& [name, contract] : module.delegates) {
        module.visibleDelegates[name] = contract;
        module.visibleDelegates[
            canonicalDelegateName(contract.moduleName, contract.name)] = contract;
    }
    for (const auto& importedName : module.imports) {
        const auto imported = modules.find(importedName);
        if (imported == modules.end()) continue;
        for (const auto& [name, contract] : imported->second.delegates) {
            module.visibleDelegates[name] = contract;
            module.visibleDelegates[
                canonicalDelegateName(contract.moduleName, contract.name)] = contract;
        }
    }
}

void refreshVisibleInterfaces(
    std::map<std::string, ModuleWork>& modules,
    ModuleWork& module) {
    module.visibleInterfaces.clear();
    for (const auto& [name, contract] : module.interfaces) {
        module.visibleInterfaces[name] = contract;
        module.visibleInterfaces[
            canonicalInterfaceName(contract.moduleName, contract.name)] = contract;
    }
    for (const auto& importedName : module.imports) {
        const auto imported = modules.find(importedName);
        if (imported == modules.end()) continue;
        for (const auto& [name, contract] : imported->second.interfaces) {
            module.visibleInterfaces[name] = contract;
            module.visibleInterfaces[
                canonicalInterfaceName(contract.moduleName, contract.name)] = contract;
        }
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
    for (const auto& parameter : method.parameters) {
        out << semantic::primitiveTypeName(parameter.type) << '#'
            << parameter.typeName << ';';
    }
    out << ")->" << semantic::primitiveTypeName(method.returnType.type)
        << '#' << method.returnType.typeName;
    return out.str();
}

semantic::VariableSymbol makeSequenceThisParameter(
    const semantic::TypeSymbol& owner,
    text::TextSpan span) {
    semantic::VariableSymbol parameter;
    parameter.name = "this";
    parameter.type = owner.kind == semantic::TypeKind::Struct
        ? semantic::PrimitiveType::Struct
        : semantic::PrimitiveType::Object;
    parameter.typeName = semantic::canonicalTypeName(owner);
    parameter.index = 0;
    parameter.parameter = true;
    parameter.declarationSpan = span;
    return parameter;
}

semantic::FunctionSymbol makeSequenceFunction(
    const std::string& moduleName,
    const semantic::TypeSymbol& owner,
    const syntax::SequenceDeclarationSyntax& sequence,
    std::string name,
    bool entry,
    const std::string& sourceName) {
    semantic::FunctionSymbol result;
    result.moduleName = moduleName;
    result.name = std::move(name);
    result.ownerTypeName = owner.name;
    result.ownerTypeId = owner.id;
    result.returnType = semantic::PrimitiveType::Void;
    result.method = true;
    result.sourceName = sourceName;
    result.declarationSpan = sequence.identifierToken.span;
    result.bodySpan = sequence.body.span();
    result.parameters.push_back(makeSequenceThisParameter(
        owner,
        sequence.identifierToken.span));
    if (entry && !sequence.parameters.empty()) {
        semantic::VariableSymbol target;
        target.name = sequence.parameters.front().identifierToken.text;
        target.type = semantic::PrimitiveType::Long;
        target.index = 1;
        target.parameter = true;
        target.declarationSpan =
            sequence.parameters.front().identifierToken.span;
        result.parameters.push_back(std::move(target));
    }
    result.id = semantic::stableFunctionId(result);
    for (auto& parameter : result.parameters) {
        parameter.id = semantic::stableTypeId(
            std::to_string(result.id) + "::local:" +
            std::to_string(parameter.index) + ":" + parameter.name);
    }
    return result;
}

std::vector<const syntax::YieldWaitStatementSyntax*> sequenceYields(
    const syntax::SequenceDeclarationSyntax& sequence,
    diagnostics::DiagnosticBag& diagnostics,
    const std::string& sourceName) {
    std::vector<const syntax::YieldWaitStatementSyntax*> result;
    for (const auto& statement : sequence.body.statements) {
        if (statement->kind() == syntax::SyntaxKind::YieldWaitStatement) {
            result.push_back(
                static_cast<const syntax::YieldWaitStatementSyntax*>(
                    statement.get()));
        }
    }
    if (sequence.parameters.size() != 1 ||
        sequence.parameters.front().type.name.text != "long" ||
        sequence.parameters.front().type.isArray()) {
        diagnostics.report(
            "RS2490",
            "sequence must declare exactly one long target parameter",
            sequence.identifierToken.span,
            diagnostics::DiagnosticSeverity::Error,
            sourceName);
    }
    return result;
}

void collectEventSubscriptions(
    const syntax::StatementSyntax& statement,
    std::vector<const syntax::EventSubscriptionStatementSyntax*>& output) {
    switch (statement.kind()) {
    case syntax::SyntaxKind::EventSubscriptionStatement:
        output.push_back(static_cast<const
            syntax::EventSubscriptionStatementSyntax*>(&statement));
        return;
    case syntax::SyntaxKind::BlockStatement:
        for (const auto& child : static_cast<const
             syntax::BlockStatementSyntax&>(statement).statements) {
            collectEventSubscriptions(*child, output);
        }
        return;
    case syntax::SyntaxKind::IfStatement: {
        const auto& value = static_cast<const
            syntax::IfStatementSyntax&>(statement);
        collectEventSubscriptions(*value.thenStatement, output);
        if (value.elseStatement) {
            collectEventSubscriptions(*value.elseStatement, output);
        }
        return;
    }
    case syntax::SyntaxKind::WhileStatement:
        collectEventSubscriptions(*static_cast<const
            syntax::WhileStatementSyntax&>(statement).body, output);
        return;
    case syntax::SyntaxKind::ForStatement:
        collectEventSubscriptions(*static_cast<const
            syntax::ForStatementSyntax&>(statement).body, output);
        return;
    case syntax::SyntaxKind::ForeachStatement:
        collectEventSubscriptions(*static_cast<const
            syntax::ForeachStatementSyntax&>(statement).body, output);
        return;
    case syntax::SyntaxKind::DoWhileStatement:
        collectEventSubscriptions(*static_cast<const
            syntax::DoWhileStatementSyntax&>(statement).body, output);
        return;
    case syntax::SyntaxKind::SwitchStatement:
        for (const auto& section : static_cast<const
             syntax::SwitchStatementSyntax&>(statement).sections) {
            for (const auto& child : section.statements) {
                collectEventSubscriptions(*child, output);
            }
        }
        return;
    default:
        return;
    }
}

bool eventMethodMatches(
    const semantic::FunctionSymbol& function,
    const DelegateContract& contract) {
    if (function.staticMethod ||
        function.returnType != contract.returnType.type ||
        (semantic::isExactType(function.returnType) &&
         function.returnTypeName != contract.returnType.typeName)) {
        return false;
    }
    const auto offset = function.method && !function.staticMethod
        ? std::size_t{1}
        : std::size_t{0};
    if (function.parameters.size() != contract.parameters.size() + offset) {
        return false;
    }
    for (std::size_t index = 0; index < contract.parameters.size(); ++index) {
        const auto& parameter = function.parameters[index + offset];
        if (parameter.modifier != semantic::ParameterModifier::None ||
            parameter.type != contract.parameters[index].type ||
            (semantic::isExactType(parameter.type) &&
             parameter.typeName != contract.parameters[index].typeName)) {
            return false;
        }
    }
    return true;
}

semantic::VariableSymbol makeEventThisParameter(
    const semantic::TypeSymbol& owner,
    text::TextSpan span) {
    semantic::VariableSymbol parameter;
    parameter.name = "this";
    parameter.type = semantic::PrimitiveType::Object;
    parameter.typeName = semantic::canonicalTypeName(owner);
    parameter.storageType = parameter.type;
    parameter.storageTypeName = parameter.typeName;
    parameter.index = 0;
    parameter.parameter = true;
    parameter.declarationSpan = span;
    return parameter;
}

semantic::FunctionSymbol makeEventLambdaFunction(
    const std::string& moduleName,
    const semantic::TypeSymbol& owner,
    const DelegateContract& contract,
    const syntax::LambdaExpressionSyntax& lambda,
    const std::string& eventName,
    std::size_t ordinal,
    const std::string& sourceName,
    diagnostics::DiagnosticBag& diagnostics) {
    semantic::FunctionSymbol result;
    result.moduleName = moduleName;
    result.name = "$event_" + eventName + "_lambda_" +
        std::to_string(ordinal);
    result.ownerTypeName = owner.name;
    result.ownerTypeId = owner.id;
    result.returnType = contract.returnType.type;
    result.returnTypeName = contract.returnType.typeName;
    result.method = true;
    result.synthetic = true;
    result.sourceName = sourceName;
    result.declarationSpan = lambda.arrowToken.span;
    result.bodySpan = lambda.span();
    result.parameters.push_back(makeEventThisParameter(
        owner, lambda.span()));
    if (lambda.parameterTokens.size() != contract.parameters.size()) {
        diagnostics.report(
            "RS8312",
            "event lambda parameter count does not match delegate",
            lambda.span(),
            diagnostics::DiagnosticSeverity::Error,
            sourceName);
    }
    for (std::size_t index = 0; index < contract.parameters.size(); ++index) {
        semantic::VariableSymbol parameter;
        parameter.name = index < lambda.parameterTokens.size()
            ? lambda.parameterTokens[index].text
            : "$arg" + std::to_string(index);
        parameter.type = contract.parameters[index].type;
        parameter.typeName = contract.parameters[index].typeName;
        parameter.storageType = parameter.type;
        parameter.storageTypeName = parameter.typeName;
        parameter.index = result.parameters.size();
        parameter.parameter = true;
        parameter.declarationSpan = index < lambda.parameterTokens.size()
            ? lambda.parameterTokens[index].span
            : lambda.arrowToken.span;
        result.parameters.push_back(std::move(parameter));
    }
    result.id = semantic::stableFunctionId(result);
    for (auto& parameter : result.parameters) {
        parameter.id = semantic::stableTypeId(
            std::to_string(result.id) + "::local:" +
            std::to_string(parameter.index) + ":" + parameter.name);
    }
    return result;
}

std::string fieldTypeSignature(const semantic::FieldSymbol& field) {
    if (semantic::isExactType(field.type) && !field.typeName.empty()) {
        return field.typeName;
    }
    return semantic::primitiveTypeName(field.type);
}

std::string typeSignature(const semantic::TypeSymbol& type) {
    std::ostringstream out;
    out << static_cast<int>(type.kind) << ':' << semantic::canonicalTypeName(type) << '{';
    for (const auto& field : type.fields) {
        out << field.name << ':' << fieldTypeSignature(field)
            << (field.synthetic ? ":synthetic" : "") << ';';
    }
    for (const auto& constructor : type.constructors) {
        out << "ctor:" << semantic::canonicalFunctionSignature(constructor) << ';';
    }
    for (const auto& method : type.methods) {
        out << "method:" << (method.staticMethod ? "static:" : "instance:")
            << semantic::canonicalFunctionSignature(method) << ';';
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
            for (const auto& function : module.functions) {
                if (function.moduleName == moduleName &&
                    function.name == instantiation.generatedName) {
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

} // namespace

BuildResult Compilation::build(const BuildSnapshot* previous) const {
    BuildResult result;
    std::vector<std::unique_ptr<ParsedUnit>> units;
    std::map<std::string, ModuleWork> modules;

    for (const auto& sourceFile : sources_) {
        auto unit = std::make_unique<ParsedUnit>();
        unit->source = std::make_unique<text::SourceText>(sourceFile.content, sourceFile.path);
        diagnostics::DiagnosticBag parseDiagnostics;
        syntax::Parser parser(*unit->source, parseDiagnostics);
        unit->syntaxTree = std::make_unique<syntax::CompilationUnitSyntax>(
            parser.parseCompilationUnit());
        unit->moduleName = unit->syntaxTree->moduleDeclaration
            ? unit->syntaxTree->moduleDeclaration->fullName()
            : "$global";
        unit->sourceFingerprint = stableFingerprint(sourceFile.path + "\n" + sourceFile.content);
        for (const auto& import : unit->syntaxTree->imports) {
            unit->imports.push_back(import.fullName());
        }
        for (const auto& diagnostic : parseDiagnostics.items()) {
            result.diagnostics.report(
                diagnostic.code, diagnostic.message, diagnostic.span,
                diagnostic.severity, sourceFile.path);
        }
        unit->invalid = parseDiagnostics.hasErrors();
        auto& module = modules[unit->moduleName];
        module.name = unit->moduleName;
        module.units.push_back(unit.get());
        module.invalid = module.invalid || unit->invalid;
        module.imports.insert(unit->imports.begin(), unit->imports.end());
        units.push_back(std::move(unit));
    }

    specializeNativeGenerics(units, modules, result);

    // Stable source order and named-type shells.
    for (auto& moduleEntry : modules) {
        const auto& moduleName = moduleEntry.first;
        auto& module = moduleEntry.second;
        std::sort(module.units.begin(), module.units.end(),
            [](const ParsedUnit* left, const ParsedUnit* right) {
                return left->source->name() < right->source->name();
            });
        std::uint64_t sourceFingerprint = 14695981039346656037ull;
        std::unordered_set<std::string> typeNames;
        const auto addShell = [&](auto const& syntaxNode, auto declare) {
            auto type = declare(moduleName, syntaxNode);
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
            sourceFingerprint = combineFingerprint(sourceFingerprint, unit->sourceFingerprint);
            for (const auto& node : unit->syntaxTree->classes) {
                if (!node.typeParameters.empty()) continue;
                const auto before = module.types.size();
                addShell(node, [](const std::string& name, const auto& syntaxNode) {
                    return semantic::declareTypeShell(name, syntaxNode);
                });
                if (module.types.size() != before) {
                    module.types.back().sourceName = unit->source->name();
                    module.types.back().declarationSpan = node.identifierToken.span;
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
                }
            }
            for (const auto& node : unit->syntaxTree->interfaces) {
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
                module.interfaceInputs.push_back(
                    InterfaceDeclarationInput{&node, unit->source->name()});
            }
            for (const auto& node : unit->syntaxTree->delegates) {
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
                module.delegateInputs.push_back(
                    DelegateDeclarationInput{&node, unit->source->name()});
            }
        }
        module.sourceFingerprint = sourceFingerprint;
    }

    for (auto& [name, module] : modules) {
        (void)name;
        refreshVisibleTypes(modules, module);
    }

    // Resolve native interface contracts after all named type shells exist.
    for (auto& [moduleName, module] : modules) {
        for (const auto& input : module.interfaceInputs) {
            if (!input.syntax) continue;
            InterfaceContract contract;
            contract.moduleName = moduleName;
            contract.name = input.syntax->identifierToken.text;
            contract.sourceName = input.sourceName;
            contract.declarationSpan = input.syntax->identifierToken.span;
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
                    method.parameters.push_back(resolveInterfaceType(
                        parameter.type,
                        module.visibleTypes,
                        result.diagnostics,
                        input.sourceName,
                        false));
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
            }
            module.interfaces[contract.name] = std::move(contract);
        }
    }
    for (auto& [name, module] : modules) {
        (void)name;
        refreshVisibleInterfaces(modules, module);
    }

    // Resolve native delegate contracts after all named type shells exist.
    for (auto& [moduleName, module] : modules) {
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
                if (parameter.modifierToken) {
                    result.diagnostics.report(
                        "RS8311",
                        "event delegates do not support ref, out, or in parameters",
                        parameter.span(),
                        diagnostics::DiagnosticSeverity::Error,
                        input.sourceName);
                    module.invalid = true;
                }
                contract.parameters.push_back(resolveInterfaceType(
                    parameter.type,
                    module.visibleTypes,
                    result.diagnostics,
                    input.sourceName,
                    false));
            }
            module.delegates[contract.name] = std::move(contract);
        }
    }
    for (auto& [name, module] : modules) {
        (void)name;
        refreshVisibleDelegates(modules, module);
    }

    // Resolve all field layouts and enum values before declaring member signatures.
    for (auto& [moduleName, module] : modules) {
        (void)moduleName;
        for (const auto* unit : module.units) {
            for (const auto& node : unit->syntaxTree->classes) {
                if (!node.typeParameters.empty()) continue;
                auto* type = findOwnType(module, node.identifierToken.text);
                if (!type || !semantic::populateTypeFields(
                        *type, node, module.visibleTypes, result.diagnostics)) {
                    module.invalid = true;
                } else {
                    type->sourceName = unit->source->name();
                    for (auto& field : type->fields) field.sourceName = unit->source->name();
                }
            }
            for (const auto& node : unit->syntaxTree->structs) {
                if (!node.typeParameters.empty()) continue;
                auto* type = findOwnType(module, node.identifierToken.text);
                if (!type || !semantic::populateTypeFields(
                        *type, node, module.visibleTypes, result.diagnostics)) {
                    module.invalid = true;
                } else {
                    type->sourceName = unit->source->name();
                    for (auto& field : type->fields) field.sourceName = unit->source->name();
                }
            }
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
    for (auto& [name, module] : modules) {
        (void)name;
        refreshVisibleTypes(modules, module);
    }

    // Declare free functions, methods, constructors and properties.
    for (auto& moduleEntry : modules) {
        const auto& moduleName = moduleEntry.first;
        auto& module = moduleEntry.second;
        std::unordered_set<std::string> functionKeys;
        auto addBinding = [&](semantic::FunctionBindingInput binding,
                              text::TextSpan span) {
            const auto key = semantic::canonicalFunctionKey(binding.symbol);
            if (!functionKeys.insert(key).second) {
                result.diagnostics.report(
                    "RS4002", "duplicate function overload '" + key + "'", span);
                module.invalid = true;
            }
            module.declarations.push_back(binding.symbol);
            module.functionBindings.push_back(std::move(binding));
        };

        for (const auto* unit : module.units) {
            for (const auto& functionSyntax : unit->syntaxTree->functions) {
                if (!functionSyntax.typeParameters.empty()) continue;
                semantic::FunctionBindingInput binding;
                binding.symbol = semantic::declareFunctionSymbol(
                    moduleName, functionSyntax, module.visibleTypes, result.diagnostics);
                binding.sourceName = unit->source->name();
                binding.symbol.sourceName = binding.sourceName;
                binding.body = &functionSyntax.body;
                appendParameters(binding, functionSyntax.parameters);
                addBinding(std::move(binding), functionSyntax.identifierToken.span);
            }

            const auto addMembers = [&](auto const& declarations) {
                for (const auto& typeSyntax : declarations) {
                    if (!typeSyntax.typeParameters.empty()) continue;
                    auto* ownerPointer = findOwnType(module, typeSyntax.identifierToken.text);
                    if (!ownerPointer) {
                        module.invalid = true;
                        continue;
                    }
                    auto owner = *ownerPointer;
                    std::unordered_set<std::string> fieldNames;
                    std::unordered_set<std::string> methodNames;
                    std::unordered_set<std::string> propertyNames;
                    for (const auto& field : owner.fields) {
                        fieldNames.insert(field.name);
                    }
                    for (const auto& methodSyntax : typeSyntax.methods) {
                        if (fieldNames.find(methodSyntax.identifierToken.text) !=
                            fieldNames.end()) {
                            result.diagnostics.report(
                                "RS2464",
                                "method '" + methodSyntax.identifierToken.text +
                                    "' conflicts with a field of the same name",
                                methodSyntax.identifierToken.span);
                            module.invalid = true;
                        }
                        methodNames.insert(methodSyntax.identifierToken.text);
                        semantic::FunctionBindingInput binding;
                        binding.symbol = semantic::declareFunctionSymbol(
                            moduleName, methodSyntax, module.visibleTypes,
                            result.diagnostics, &owner);
                        binding.sourceName = unit->source->name();
                        binding.symbol.sourceName = binding.sourceName;
                        binding.body = &methodSyntax.body;
                        if (!binding.symbol.staticMethod) {
                            binding.parameterNames.push_back("this");
                            binding.parameterSpans.push_back(typeSyntax.identifierToken.span);
                        }
                        appendParameters(binding, methodSyntax.parameters);
                        owner.methods.push_back(binding.symbol);
                        addBinding(std::move(binding), methodSyntax.identifierToken.span);
                    }
                    for (const auto& sequenceSyntax : typeSyntax.sequences) {
                        if (owner.kind == semantic::TypeKind::Struct) {
                            result.diagnostics.report(
                                "RS2491",
                                "sequence methods require a class owner",
                                sequenceSyntax.identifierToken.span,
                                diagnostics::DiagnosticSeverity::Error,
                                unit->source->name());
                            module.invalid = true;
                            continue;
                        }
                        if (fieldNames.find(sequenceSyntax.identifierToken.text) !=
                                fieldNames.end() ||
                            methodNames.find(sequenceSyntax.identifierToken.text) !=
                                methodNames.end()) {
                            result.diagnostics.report(
                                "RS2464",
                                "sequence '" + sequenceSyntax.identifierToken.text +
                                    "' conflicts with another member",
                                sequenceSyntax.identifierToken.span,
                                diagnostics::DiagnosticSeverity::Error,
                                unit->source->name());
                            module.invalid = true;
                            continue;
                        }
                        methodNames.insert(sequenceSyntax.identifierToken.text);
                        const auto yields = sequenceYields(
                            sequenceSyntax,
                            result.diagnostics,
                            unit->source->name());
                        if (sequenceSyntax.parameters.size() != 1 ||
                            sequenceSyntax.parameters.front().type.name.text !=
                                "long" ||
                            sequenceSyntax.parameters.front().type.isArray()) {
                            module.invalid = true;
                            continue;
                        }

                        semantic::FieldSymbol targetField;
                        targetField.name = "$sequence_target_" +
                            sequenceSyntax.identifierToken.text;
                        targetField.type = semantic::PrimitiveType::Long;
                        targetField.index = owner.fields.size();
                        targetField.synthetic = true;
                        targetField.sourceName = unit->source->name();
                        targetField.declarationSpan =
                            sequenceSyntax.identifierToken.span;
                        targetField.id = semantic::stableTypeId(
                            semantic::canonicalTypeName(owner) +
                            "::field:" + targetField.name);
                        owner.fields.push_back(targetField);
                        fieldNames.insert(targetField.name);

                        std::vector<semantic::FunctionSymbol> callbacks;
                        callbacks.reserve(yields.size());
                        for (std::size_t callback = 0;
                             callback < yields.size();
                             ++callback) {
                            callbacks.push_back(makeSequenceFunction(
                                moduleName,
                                owner,
                                sequenceSyntax,
                                "$sequence_" +
                                    sequenceSyntax.identifierToken.text + "_" +
                                    std::to_string(callback + 1),
                                false,
                                unit->source->name()));
                        }

                        auto entry = makeSequenceFunction(
                            moduleName,
                            owner,
                            sequenceSyntax,
                            sequenceSyntax.identifierToken.text,
                            true,
                            unit->source->name());
                        semantic::FunctionBindingInput entryBinding;
                        entryBinding.symbol = entry;
                        entryBinding.sourceName = unit->source->name();
                        entryBinding.parameterNames.push_back("this");
                        entryBinding.parameterSpans.push_back(
                            typeSyntax.identifierToken.span);
                        entryBinding.parameterNames.push_back(
                            sequenceSyntax.parameters.front()
                                .identifierToken.text);
                        entryBinding.parameterSpans.push_back(
                            sequenceSyntax.parameters.front()
                                .identifierToken.span);
                        entryBinding.sequence = &sequenceSyntax;
                        entryBinding.sequenceSegment = 0;
                        entryBinding.sequenceTargetField = targetField;
                        if (!callbacks.empty()) {
                            entryBinding.sequenceNextCallback = callbacks.front();
                        }
                        owner.methods.push_back(entry);
                        addBinding(
                            std::move(entryBinding),
                            sequenceSyntax.identifierToken.span);

                        for (std::size_t callback = 0;
                             callback < callbacks.size();
                             ++callback) {
                            semantic::FunctionBindingInput binding;
                            binding.symbol = callbacks[callback];
                            binding.sourceName = unit->source->name();
                            binding.parameterNames.push_back("this");
                            binding.parameterSpans.push_back(
                                typeSyntax.identifierToken.span);
                            binding.sequence = &sequenceSyntax;
                            binding.sequenceSegment = callback + 1;
                            binding.sequenceTargetField = targetField;
                            if (callback + 1 < callbacks.size()) {
                                binding.sequenceNextCallback =
                                    callbacks[callback + 1];
                            }
                            owner.methods.push_back(callbacks[callback]);
                            addBinding(
                                std::move(binding),
                                sequenceSyntax.identifierToken.span);
                        }

                        LanguageSequenceRecord record;
                        record.typeName = semantic::canonicalTypeName(owner);
                        record.name = sequenceSyntax.identifierToken.text;
                        record.sourceName = unit->source->name();
                        record.offset =
                            sequenceSyntax.identifierToken.span.start;
                        for (const auto& callback : callbacks) {
                            record.callbacks.push_back(callback.name);
                        }
                        result.nativeSequences.push_back(std::move(record));
                    }
                    for (const auto& constructorSyntax : typeSyntax.constructors) {
                        semantic::FunctionBindingInput binding;
                        binding.symbol = semantic::declareConstructorSymbol(
                            moduleName, constructorSyntax, owner,
                            module.visibleTypes, result.diagnostics);
                        binding.sourceName = unit->source->name();
                        binding.symbol.sourceName = binding.sourceName;
                        binding.body = &constructorSyntax.body;
                        binding.parameterNames.push_back("this");
                        binding.parameterSpans.push_back(typeSyntax.identifierToken.span);
                        appendParameters(binding, constructorSyntax.parameters);
                        owner.constructors.push_back(binding.symbol);
                        addBinding(std::move(binding), constructorSyntax.identifierToken.span);
                    }
                    for (const auto& propertySyntax : typeSyntax.properties) {
                        if (!propertyNames.insert(
                                propertySyntax.identifierToken.text).second ||
                            fieldNames.find(propertySyntax.identifierToken.text) !=
                                fieldNames.end() ||
                            methodNames.find(propertySyntax.identifierToken.text) !=
                                methodNames.end()) {
                            result.diagnostics.report(
                                "RS2464",
                                "property '" + propertySyntax.identifierToken.text +
                                    "' conflicts with another member",
                                propertySyntax.identifierToken.span);
                            module.invalid = true;
                        }
                        auto property = semantic::declarePropertySymbol(
                            moduleName, propertySyntax, owner,
                            module.visibleTypes, result.diagnostics);
                        property.sourceName = unit->source->name();
                        if (property.getter) property.getter->sourceName = unit->source->name();
                        if (property.setter) property.setter->sourceName = unit->source->name();
                        const bool getterAuto = propertySyntax.getter &&
                            propertySyntax.getter->semicolonToken.has_value();
                        const bool setterAuto = propertySyntax.setter &&
                            propertySyntax.setter->semicolonToken.has_value();
                        const bool getterExplicit = propertySyntax.getter &&
                            propertySyntax.getter->body != nullptr;
                        const bool setterExplicit = propertySyntax.setter &&
                            propertySyntax.setter->body != nullptr;
                        const bool autoProperty = getterAuto || setterAuto;
                        if (autoProperty && (getterExplicit || setterExplicit)) {
                            result.diagnostics.report(
                                "RS2463",
                                "property accessors cannot mix auto and explicit bodies",
                                propertySyntax.identifierToken.span);
                            module.invalid = true;
                        }
                        if (autoProperty && property.staticProperty) {
                            result.diagnostics.report(
                                "RS2462",
                                "auto properties require an instance owner",
                                propertySyntax.identifierToken.span);
                            module.invalid = true;
                        }
                        if (autoProperty) {
                            property.backingFieldIndex = owner.fields.size();
                            semantic::FieldSymbol backing;
                            backing.name = "$" + property.name;
                            backing.type = property.type;
                            backing.typeName = property.typeName;
                            backing.index = owner.fields.size();
                            backing.synthetic = true;
                            backing.sourceName = unit->source->name();
                            backing.declarationSpan = propertySyntax.identifierToken.span;
                            backing.id = semantic::stableTypeId(
                                semantic::canonicalTypeName(owner) +
                                "::field:" + backing.name);
                            owner.fields.push_back(std::move(backing));
                        }
                        if (property.getter) {
                            semantic::FunctionBindingInput binding;
                            binding.symbol = *property.getter;
                            binding.sourceName = unit->source->name();
                            binding.body = propertySyntax.getter->body.get();
                            if (!binding.symbol.staticMethod) {
                                binding.parameterNames.push_back("this");
                                binding.parameterSpans.push_back(typeSyntax.identifierToken.span);
                            }
                            binding.syntheticAutoGetter =
                                propertySyntax.getter->semicolonToken.has_value();
                            if (autoProperty) {
                                binding.syntheticField = owner.fields[property.backingFieldIndex];
                            }
                            addBinding(std::move(binding), propertySyntax.identifierToken.span);
                        }
                        if (property.setter) {
                            semantic::FunctionBindingInput binding;
                            binding.symbol = *property.setter;
                            binding.sourceName = unit->source->name();
                            binding.body = propertySyntax.setter->body.get();
                            if (!binding.symbol.staticMethod) {
                                binding.parameterNames.push_back("this");
                                binding.parameterSpans.push_back(typeSyntax.identifierToken.span);
                            }
                            binding.parameterNames.push_back("value");
                            binding.parameterSpans.push_back(propertySyntax.identifierToken.span);
                            binding.syntheticAutoSetter =
                                propertySyntax.setter->semicolonToken.has_value();
                            if (autoProperty) {
                                binding.syntheticField = owner.fields[property.backingFieldIndex];
                            }
                            addBinding(std::move(binding), propertySyntax.identifierToken.span);
                        }
                        owner.properties.push_back(std::move(property));
                    }
                    if (!typeSyntax.events.empty()) {
                        if (owner.kind == semantic::TypeKind::Struct) {
                            result.diagnostics.report(
                                "RS8315",
                                "events require a class owner",
                                typeSyntax.identifierToken.span,
                                diagnostics::DiagnosticSeverity::Error,
                                unit->source->name());
                            module.invalid = true;
                        } else {
                            std::unordered_map<std::string, std::size_t>
                                eventIndices;
                            for (const auto& eventSyntax : typeSyntax.events) {
                                if (fieldNames.find(
                                        eventSyntax.identifierToken.text) !=
                                        fieldNames.end() ||
                                    methodNames.find(
                                        eventSyntax.identifierToken.text) !=
                                        methodNames.end() ||
                                    propertyNames.find(
                                        eventSyntax.identifierToken.text) !=
                                        propertyNames.end() ||
                                    eventIndices.find(
                                        eventSyntax.identifierToken.text) !=
                                        eventIndices.end()) {
                                    result.diagnostics.report(
                                        "RS2464",
                                        "event '" +
                                            eventSyntax.identifierToken.text +
                                            "' conflicts with another member",
                                        eventSyntax.identifierToken.span,
                                        diagnostics::DiagnosticSeverity::Error,
                                        unit->source->name());
                                    module.invalid = true;
                                    continue;
                                }
                                const auto delegate =
                                    module.visibleDelegates.find(
                                        eventSyntax.delegateType.name.text);
                                if (delegate ==
                                    module.visibleDelegates.end()) {
                                    result.diagnostics.report(
                                        "RS8301",
                                        "unknown event delegate '" +
                                            eventSyntax.delegateType.name.text +
                                            "'",
                                        eventSyntax.delegateType.span(),
                                        diagnostics::DiagnosticSeverity::Error,
                                        unit->source->name());
                                    module.invalid = true;
                                    continue;
                                }
                                if (delegate->second.returnType.type !=
                                    semantic::PrimitiveType::Void) {
                                    result.diagnostics.report(
                                        "RS8302",
                                        "event delegate must return void",
                                        eventSyntax.delegateType.span(),
                                        diagnostics::DiagnosticSeverity::Error,
                                        unit->source->name());
                                    module.invalid = true;
                                }
                                semantic::EventSymbol event;
                                event.name = eventSyntax.identifierToken.text;
                                event.delegateName = canonicalDelegateName(
                                    delegate->second.moduleName,
                                    delegate->second.name);
                                event.sourceName = unit->source->name();
                                event.declarationSpan =
                                    eventSyntax.identifierToken.span;
                                event.id = semantic::stableTypeId(
                                    semantic::canonicalTypeName(owner) +
                                    "::event:" + event.name);
                                for (std::size_t parameter = 0;
                                     parameter <
                                         delegate->second.parameters.size();
                                     ++parameter) {
                                    semantic::VariableSymbol value;
                                    value.name = "$arg" +
                                        std::to_string(parameter);
                                    value.type = delegate->second
                                        .parameters[parameter].type;
                                    value.typeName = delegate->second
                                        .parameters[parameter].typeName;
                                    value.storageType = value.type;
                                    value.storageTypeName = value.typeName;
                                    value.index = parameter;
                                    value.parameter = true;
                                    event.parameters.push_back(
                                        std::move(value));
                                }
                                eventIndices.emplace(
                                    event.name,
                                    owner.events.size());
                                owner.events.push_back(std::move(event));
                            }

                            std::vector<const
                                syntax::EventSubscriptionStatementSyntax*>
                                subscriptions;
                            for (const auto& method : typeSyntax.methods) {
                                collectEventSubscriptions(
                                    method.body, subscriptions);
                            }
                            for (const auto& constructor :
                                 typeSyntax.constructors) {
                                collectEventSubscriptions(
                                    constructor.body, subscriptions);
                            }
                            for (const auto& property :
                                 typeSyntax.properties) {
                                if (property.getter &&
                                    property.getter->body) {
                                    collectEventSubscriptions(
                                        *property.getter->body,
                                        subscriptions);
                                }
                                if (property.setter &&
                                    property.setter->body) {
                                    collectEventSubscriptions(
                                        *property.setter->body,
                                        subscriptions);
                                }
                            }
                            for (const auto& sequence :
                                 typeSyntax.sequences) {
                                collectEventSubscriptions(
                                    sequence.body, subscriptions);
                            }
                            std::stable_sort(
                                subscriptions.begin(),
                                subscriptions.end(),
                                [](const auto* left, const auto* right) {
                                    return left->span().start <
                                        right->span().start;
                                });

                            std::unordered_map<std::string,
                                semantic::FieldSymbol> handlerSlots;
                            std::size_t lambdaOrdinal = 0;
                            for (const auto* subscription : subscriptions) {
                                const auto foundEvent = eventIndices.find(
                                    subscription->eventNameToken.text);
                                if (foundEvent == eventIndices.end()) {
                                    result.diagnostics.report(
                                        "RS8303",
                                        "unknown event '" +
                                            subscription->eventNameToken.text +
                                            "'",
                                        subscription->eventNameToken.span,
                                        diagnostics::DiagnosticSeverity::Error,
                                        unit->source->name());
                                    module.invalid = true;
                                    continue;
                                }
                                auto& event = owner.events[
                                    foundEvent->second];
                                const auto delegate =
                                    module.visibleDelegates.find(
                                        event.delegateName);
                                if (delegate ==
                                    module.visibleDelegates.end()) {
                                    module.invalid = true;
                                    continue;
                                }

                                semantic::FunctionSymbol handler;
                                std::string slotKey;
                                if (subscription->handler->kind() ==
                                    syntax::SyntaxKind::NameExpression) {
                                    const auto& name = static_cast<const
                                        syntax::NameExpressionSyntax&>(
                                            *subscription->handler);
                                    bool matched = false;
                                    for (const auto& candidate :
                                         owner.methods) {
                                        if (candidate.name ==
                                                name.identifierToken.text &&
                                            eventMethodMatches(
                                                candidate,
                                                delegate->second)) {
                                            if (matched) {
                                                result.diagnostics.report(
                                                    "RS8304",
                                                    "event method group is ambiguous",
                                                    name.span(),
                                                    diagnostics::DiagnosticSeverity::Error,
                                                    unit->source->name());
                                                module.invalid = true;
                                                break;
                                            }
                                            handler = candidate;
                                            matched = true;
                                        }
                                    }
                                    if (!matched) {
                                        result.diagnostics.report(
                                            "RS8305",
                                            "event handler method does not match delegate",
                                            name.span(),
                                            diagnostics::DiagnosticSeverity::Error,
                                            unit->source->name());
                                        module.invalid = true;
                                        continue;
                                    }
                                    slotKey = "method:" +
                                        std::to_string(handler.id);
                                } else if (subscription->handler->kind() ==
                                    syntax::SyntaxKind::LambdaExpression) {
                                    if (subscription->operatorToken.kind ==
                                        syntax::SyntaxKind::MinusEqualsToken) {
                                        result.diagnostics.report(
                                            "RS8306",
                                            "lambda event handlers cannot be removed in the bounded Phase 18 model",
                                            subscription->span(),
                                            diagnostics::DiagnosticSeverity::Error,
                                            unit->source->name());
                                        module.invalid = true;
                                        continue;
                                    }
                                    const auto& lambda = static_cast<const
                                        syntax::LambdaExpressionSyntax&>(
                                            *subscription->handler);
                                    handler = makeEventLambdaFunction(
                                        moduleName,
                                        owner,
                                        delegate->second,
                                        lambda,
                                        event.name,
                                        ++lambdaOrdinal,
                                        unit->source->name(),
                                        result.diagnostics);
                                    semantic::FunctionBindingInput binding;
                                    binding.symbol = handler;
                                    binding.sourceName =
                                        unit->source->name();
                                    binding.eventLambda = &lambda;
                                    binding.parameterNames.push_back("this");
                                    binding.parameterSpans.push_back(
                                        typeSyntax.identifierToken.span);
                                    for (std::size_t parameter = 0;
                                         parameter <
                                             delegate->second.parameters.size();
                                         ++parameter) {
                                        binding.parameterNames.push_back(
                                            parameter <
                                                    lambda.parameterTokens.size()
                                                ? lambda.parameterTokens[
                                                    parameter].text
                                                : "$arg" +
                                                    std::to_string(parameter));
                                        binding.parameterSpans.push_back(
                                            parameter <
                                                    lambda.parameterTokens.size()
                                                ? lambda.parameterTokens[
                                                    parameter].span
                                                : lambda.arrowToken.span);
                                    }
                                    owner.methods.push_back(handler);
                                    addBinding(
                                        std::move(binding),
                                        lambda.arrowToken.span);
                                    slotKey = "lambda:" +
                                        std::to_string(lambda.span().start);
                                } else {
                                    result.diagnostics.report(
                                        "RS8307",
                                        "event handler must be a method group or lambda",
                                        subscription->handler->span(),
                                        diagnostics::DiagnosticSeverity::Error,
                                        unit->source->name());
                                    module.invalid = true;
                                    continue;
                                }

                                auto slot = handlerSlots.find(
                                    event.name + ":" + slotKey);
                                if (slot == handlerSlots.end()) {
                                    semantic::FieldSymbol field;
                                    field.name = "$event_" + event.name +
                                        "_slot_" +
                                        std::to_string(event.handlers.size());
                                    field.type =
                                        semantic::PrimitiveType::Bool;
                                    field.index = owner.fields.size();
                                    field.synthetic = true;
                                    field.sourceName = unit->source->name();
                                    field.declarationSpan =
                                        subscription->eventNameToken.span;
                                    field.id = semantic::stableTypeId(
                                        semantic::canonicalTypeName(owner) +
                                        "::field:" + field.name);
                                    owner.fields.push_back(field);
                                    event.handlers.push_back(
                                        semantic::EventHandlerSymbol{
                                            handler, field});
                                    slot = handlerSlots.emplace(
                                        event.name + ":" + slotKey,
                                        field).first;
                                }
                                event.subscriptions.push_back(
                                    semantic::EventSubscriptionSymbol{
                                        subscription->span(),
                                        slot->second,
                                        subscription->operatorToken.kind ==
                                            syntax::SyntaxKind::PlusEqualsToken});
                            }
                        }
                    }
                    *ownerPointer = std::move(owner);
                }
            };
            addMembers(unit->syntaxTree->classes);
            addMembers(unit->syntaxTree->structs);
        }

        // Materialize compiler-owned reference boxes. They are real runtime
        // descriptors but synthetic source symbols, so they stay out of the
        // public language surface and tooling occurrences.
        for (const auto& declaration : module.declarations) {
            for (const auto& parameter : declaration.parameters) {
                if (parameter.modifier !=
                        semantic::ParameterModifier::Ref &&
                    parameter.modifier !=
                        semantic::ParameterModifier::Out) {
                    continue;
                }
                const auto canonicalName =
                    semantic::storageTypeNameOf(parameter);
                const auto prefix = moduleName.empty()
                    ? std::string{}
                    : moduleName + "::";
                const auto simpleName =
                    canonicalName.rfind(prefix, 0) == 0
                        ? canonicalName.substr(prefix.size())
                        : canonicalName;
                if (findOwnType(module, simpleName)) continue;

                semantic::TypeSymbol wrapper;
                wrapper.kind = semantic::TypeKind::Class;
                wrapper.synthetic = true;
                wrapper.moduleName = moduleName;
                wrapper.name = simpleName;
                wrapper.id = semantic::stableTypeId(wrapper);

                semantic::FieldSymbol valueField;
                valueField.name = "Value";
                valueField.type = parameter.type;
                valueField.typeName = parameter.typeName;
                valueField.index = 0;
                valueField.synthetic = true;
                valueField.id = semantic::stableTypeId(
                    semantic::canonicalTypeName(wrapper) +
                    "::field:Value");
                wrapper.fields.push_back(std::move(valueField));
                module.types.push_back(std::move(wrapper));
            }
        }
        refreshVisibleTypes(modules, module);

        // Collect native attributes from original syntax declarations.
        for (const auto* unit : module.units) {
            const auto collectTypeAttributes = [&](auto const& declarations) {
                for (const auto& declaration : declarations) {
                    const auto* owner =
                        findOwnType(module, declaration.identifierToken.text);
                    if (!owner) continue;
                    const auto ownerName = semantic::canonicalTypeName(*owner);
                    appendNativeAttributes(
                        result.nativeAttributes,
                        declaration.attributes,
                        ownerName,
                        *unit->source);
                    for (const auto& field : declaration.fields) {
                        appendNativeAttributes(
                            result.nativeAttributes,
                            field.attributes,
                            memberAttributeTarget(
                                ownerName,
                                "field",
                                field.identifierToken.text),
                            *unit->source);
                    }
                    for (const auto& method : declaration.methods) {
                        appendNativeAttributes(
                            result.nativeAttributes,
                            method.attributes,
                            memberAttributeTarget(
                                ownerName,
                                "method",
                                method.identifierToken.text,
                                method.parameters.size()),
                            *unit->source);
                    }
                    for (const auto& sequence : declaration.sequences) {
                        appendNativeAttributes(
                            result.nativeAttributes,
                            sequence.attributes,
                            memberAttributeTarget(
                                ownerName,
                                "method",
                                sequence.identifierToken.text,
                                sequence.parameters.size()),
                            *unit->source);
                    }
                    for (const auto& constructor : declaration.constructors) {
                        appendNativeAttributes(
                            result.nativeAttributes,
                            constructor.attributes,
                            memberAttributeTarget(
                                ownerName,
                                "ctor",
                                declaration.identifierToken.text,
                                constructor.parameters.size()),
                            *unit->source);
                    }
                    for (const auto& property : declaration.properties) {
                        appendNativeAttributes(
                            result.nativeAttributes,
                            property.attributes,
                            memberAttributeTarget(
                                ownerName,
                                "property",
                                property.identifierToken.text),
                            *unit->source);
                    }
                }
            };
            collectTypeAttributes(unit->syntaxTree->classes);
            collectTypeAttributes(unit->syntaxTree->structs);
            for (const auto& enumeration : unit->syntaxTree->enums) {
                const auto* owner =
                    findOwnType(module, enumeration.identifierToken.text);
                if (!owner) continue;
                const auto ownerName = semantic::canonicalTypeName(*owner);
                appendNativeAttributes(
                    result.nativeAttributes,
                    enumeration.attributes,
                    ownerName,
                    *unit->source);
                for (const auto& member : enumeration.members) {
                    appendNativeAttributes(
                        result.nativeAttributes,
                        member.attributes,
                        memberAttributeTarget(
                            ownerName,
                            "enum",
                            member.identifierToken.text),
                        *unit->source);
                }
            }
            for (const auto& interfaceSyntax : unit->syntaxTree->interfaces) {
                const auto interfaceName = canonicalInterfaceName(
                    moduleName,
                    interfaceSyntax.identifierToken.text);
                appendNativeAttributes(
                    result.nativeAttributes,
                    interfaceSyntax.attributes,
                    interfaceName,
                    *unit->source);
                for (const auto& method : interfaceSyntax.methods) {
                    appendNativeAttributes(
                        result.nativeAttributes,
                        method.attributes,
                        memberAttributeTarget(
                            interfaceName,
                            "method",
                            method.identifierToken.text,
                            method.parameters.size()),
                        *unit->source);
                }
            }
            for (const auto& function : unit->syntaxTree->functions) {
                appendNativeAttributes(
                    result.nativeAttributes,
                    function.attributes,
                    memberAttributeTarget(
                        moduleName,
                        "function",
                        function.identifierToken.text,
                        function.parameters.size()),
                    *unit->source);
            }
        }

        const auto validateInterfaces = [&](auto const& declarations) {
            for (const auto& typeSyntax : declarations) {
                auto* owner = findOwnType(module, typeSyntax.identifierToken.text);
                if (!owner) continue;
                LanguageInterfaceImplementation implementation;
                implementation.typeName = semantic::canonicalTypeName(*owner);
                std::unordered_set<std::string> implementedNames;
                for (const auto& interfaceSyntax : typeSyntax.interfaces) {
                    const auto found =
                        module.visibleInterfaces.find(interfaceSyntax.name.text);
                    if (found == module.visibleInterfaces.end()) {
                        result.diagnostics.report(
                            "RS2473",
                            "unknown interface '" +
                                interfaceSyntax.name.text + "'",
                            interfaceSyntax.span(),
                            diagnostics::DiagnosticSeverity::Error,
                            owner->sourceName);
                        module.invalid = true;
                        continue;
                    }
                    const auto canonicalName = canonicalInterfaceName(
                        found->second.moduleName,
                        found->second.name);
                    if (!implementedNames.insert(canonicalName).second) {
                        result.diagnostics.report(
                            "RS2474",
                            "interface '" + canonicalName +
                                "' is implemented more than once",
                            interfaceSyntax.span(),
                            diagnostics::DiagnosticSeverity::Error,
                            owner->sourceName);
                        module.invalid = true;
                        continue;
                    }
                    implementation.interfaces.push_back(canonicalName);
                    for (const auto& required : found->second.methods) {
                        bool matched = false;
                        for (const auto& candidate : owner->methods) {
                            if (candidate.staticMethod ||
                                candidate.name != required.name) {
                                continue;
                            }
                            const auto parameterOffset =
                                candidate.method && !candidate.staticMethod
                                    ? std::size_t{1}
                                    : std::size_t{0};
                            if (candidate.parameters.size() < parameterOffset ||
                                candidate.parameters.size() - parameterOffset !=
                                    required.parameters.size()) {
                                continue;
                            }
                            bool compatible = sameInterfaceType(
                                required.returnType,
                                candidate.returnType,
                                candidate.returnTypeName);
                            for (std::size_t parameter = 0;
                                 compatible &&
                                 parameter < required.parameters.size();
                                 ++parameter) {
                                const auto& actual = candidate.parameters[
                                    parameter + parameterOffset];
                                compatible = sameInterfaceType(
                                    required.parameters[parameter],
                                    actual.type,
                                    actual.typeName);
                            }
                            if (compatible) {
                                matched = true;
                                break;
                            }
                        }
                        if (!matched) {
                            result.diagnostics.report(
                                "RS2475",
                                "type '" + implementation.typeName +
                                    "' does not implement interface method '" +
                                    canonicalName + "." +
                                    interfaceMethodSignature(required) + "'",
                                typeSyntax.identifierToken.span,
                                diagnostics::DiagnosticSeverity::Error,
                                owner->sourceName);
                            module.invalid = true;
                        }
                    }
                }
                if (!implementation.interfaces.empty()) {
                    std::sort(
                        implementation.interfaces.begin(),
                        implementation.interfaces.end());
                    result.nativeInterfaces.push_back(
                        std::move(implementation));
                }
            }
        };
        for (const auto* unit : module.units) {
            validateInterfaces(unit->syntaxTree->classes);
            validateInterfaces(unit->syntaxTree->structs);
        }

        std::vector<std::string> signatures;
        signatures.reserve(
            module.declarations.size() + module.types.size() +
            module.interfaces.size());
        for (const auto& type : module.types) {
            if (!type.synthetic) {
                signatures.push_back(typeSignature(type));
            }
        }
        for (const auto& [delegateName, contract] : module.delegates) {
            (void)delegateName;
            std::ostringstream delegateSignature;
            delegateSignature << "delegate:"
                << canonicalDelegateName(contract.moduleName, contract.name)
                << '(';
            for (const auto& parameter : contract.parameters) {
                delegateSignature << semantic::primitiveTypeName(
                    parameter.type) << '#' << parameter.typeName << ';';
            }
            delegateSignature << ")->"
                << semantic::primitiveTypeName(contract.returnType.type)
                << '#' << contract.returnType.typeName;
            signatures.push_back(delegateSignature.str());
        }
        for (const auto& [interfaceName, contract] : module.interfaces) {
            (void)interfaceName;
            std::ostringstream interfaceSignature;
            interfaceSignature << "interface:"
                << canonicalInterfaceName(contract.moduleName, contract.name)
                << '{';
            for (const auto& method : contract.methods) {
                interfaceSignature << interfaceMethodSignature(method) << ';';
            }
            interfaceSignature << '}';
            signatures.push_back(interfaceSignature.str());
        }
        for (const auto& attribute : result.nativeAttributes) {
            if (attribute.target.rfind(moduleName + "::", 0) != 0) {
                continue;
            }
            std::ostringstream attributeSignature;
            attributeSignature << "attribute:"
                << attribute.target << ':' << attribute.name << '(';
            for (const auto& argument : attribute.arguments) {
                attributeSignature << argument.name << '='
                    << argument.value << ',';
            }
            attributeSignature << ')';
            signatures.push_back(attributeSignature.str());
        }
        for (const auto& sequence : result.nativeSequences) {
            if (sequence.typeName.rfind(moduleName + "::", 0) != 0) {
                continue;
            }
            std::ostringstream sequenceSignature;
            sequenceSignature << "sequence:"
                << sequence.typeName << ':' << sequence.name << '(';
            for (const auto& callback : sequence.callbacks) {
                sequenceSignature << callback << ',';
            }
            sequenceSignature << ')';
            signatures.push_back(sequenceSignature.str());
        }
        for (const auto& implementation : result.nativeInterfaces) {
            if (implementation.typeName.rfind(moduleName + "::", 0) != 0) {
                continue;
            }
            std::ostringstream implementationSignature;
            implementationSignature << "implements:"
                << implementation.typeName << ':';
            for (const auto& interfaceName : implementation.interfaces) {
                implementationSignature << interfaceName << ',';
            }
            signatures.push_back(implementationSignature.str());
        }
        for (const auto& function : module.declarations) {
            signatures.push_back(semantic::canonicalFunctionSignature(function));
        }
        std::sort(signatures.begin(), signatures.end());
        std::ostringstream publicSurface;
        for (const auto& signature : signatures) publicSurface << signature << '\n';
        module.publicFingerprint = stableFingerprint(publicSurface.str());
    }

    // Reject recursive value-type layouts before lowering. A struct must have a
    // finite inline representation; reference fields may be recursive, value
    // fields may not.
    {
        struct StructOwner {
            ModuleWork* module = nullptr;
            const semantic::TypeSymbol* type = nullptr;
        };
        std::unordered_map<std::string, StructOwner> structs;
        for (auto& [moduleName, module] : modules) {
            (void)moduleName;
            for (const auto& type : module.types) {
                if (type.kind == semantic::TypeKind::Struct) {
                    structs.emplace(
                        semantic::canonicalTypeName(type),
                        StructOwner{&module, &type});
                }
            }
        }
        std::unordered_map<std::string, int> colors;
        std::unordered_set<std::string> reported;
        std::vector<std::string> stack;
        std::function<void(const std::string&)> visit = [&](const std::string& name) {
            const auto found = structs.find(name);
            if (found == structs.end()) return;
            colors[name] = 1;
            stack.push_back(name);
            for (const auto& field : found->second.type->fields) {
                if (field.type != semantic::PrimitiveType::Struct ||
                    field.typeName.empty()) {
                    continue;
                }
                const auto color = colors[field.typeName];
                if (color == 0) {
                    visit(field.typeName);
                } else if (color == 1 && reported.insert(field.typeName).second) {
                    std::ostringstream cycle;
                    auto begin = std::find(stack.begin(), stack.end(), field.typeName);
                    for (auto current = begin; current != stack.end(); ++current) {
                        if (current != begin) cycle << " -> ";
                        cycle << *current;
                    }
                    if (begin != stack.end()) cycle << " -> " << field.typeName;
                    result.diagnostics.report(
                        "RS2487",
                        "recursive struct layout is not allowed: " + cycle.str(),
                        {});
                    found->second.module->invalid = true;
                    const auto target = structs.find(field.typeName);
                    if (target != structs.end()) target->second.module->invalid = true;
                }
            }
            stack.pop_back();
            colors[name] = 2;
        };
        for (const auto& [name, owner] : structs) {
            (void)owner;
            if (colors[name] == 0) visit(name);
        }
    }

    // Member tables are part of the visible type descriptors.
    for (auto& [name, module] : modules) {
        (void)name;
        refreshVisibleTypes(modules, module);
    }

    for (auto& [moduleName, module] : modules) {
        std::ostringstream dependencies;
        for (const auto& importedModule : module.imports) {
            const auto imported = modules.find(importedModule);
            if (imported == modules.end()) {
                result.diagnostics.report(
                    "RS4001", "module '" + moduleName + "' imports missing module '" +
                        importedModule + "'", {});
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
            if (imported == modules.end()) continue;
            for (const auto& function : imported->second.declarations) {
                visibleFunctions[function.name].push_back(function);
            }
        }

        semantic::ModuleBindingInput bindingInput;
        bindingInput.moduleName = moduleName;
        bindingInput.declarations = module.declarations;
        bindingInput.functionBindings = module.functionBindings;
        {
            std::unordered_map<semantic::SymbolId, semantic::TypeSymbol> descriptorsById;
            for (const auto& [visibleName, type] : module.visibleTypes) {
                (void)visibleName;
                descriptorsById.emplace(type.id, type);
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
        for (const auto* unit : module.units) bindingInput.units.push_back(unit->syntaxTree.get());

        diagnostics::DiagnosticBag moduleDiagnostics;
        semantic::Binder binder(moduleDiagnostics);
        auto semanticModel = binder.bindModule(bindingInput);
        result.symbols.insert(result.symbols.end(),
            semanticModel.occurrences.begin(), semanticModel.occurrences.end());
        if (moduleDiagnostics.hasErrors()) {
            result.diagnostics.append(moduleDiagnostics);
            result.buildInfo.push_back(buildInfo);
            continue;
        }

        mir::Lowerer lowerer;
        auto mirModule = lowerer.lower(semanticModel);
        mirModule.sourceFiles.reserve(module.units.size());
        for (std::size_t sourceIndex = 0; sourceIndex < module.units.size(); ++sourceIndex) {
            mirModule.sourceFiles.push_back(makeSourceFileInfo(
                *module.units[sourceIndex]->source,
                static_cast<debug::SourceFileId>(sourceIndex)));
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
