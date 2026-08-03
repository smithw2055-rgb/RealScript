#!/usr/bin/env python3
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def write(path: str, text: str) -> None:
    (ROOT / path).write_text(text, encoding="utf-8")


def replace_once(path: str, old: str, new: str) -> None:
    text = read(path)
    if new in text:
        return
    if old not in text:
        raise RuntimeError(f"anchor not found in {path}: {old[:120]!r}")
    write(path, text.replace(old, new, 1))


def replace_regex(path: str, pattern: str, replacement: str) -> None:
    text = read(path)
    if replacement in text:
        return
    updated, count = re.subn(pattern, replacement, text, count=1, flags=re.S)
    if count != 1:
        raise RuntimeError(f"regex anchor not found in {path}: {pattern[:120]!r}")
    write(path, updated)


# Make empty parenthesized lambdas safe in source spans.
replace_once(
    "src/syntax/SyntaxNodes.cpp",
    '''text::TextSpan LambdaExpressionSyntax::span() const noexcept {
    return combine(
        openParenToken ? openParenToken->span
                       : parameterTokens.front().span,
        body->span());
}''',
    '''text::TextSpan LambdaExpressionSyntax::span() const noexcept {
    const auto start = openParenToken
        ? openParenToken->span
        : parameterTokens.empty()
            ? arrowToken.span
            : parameterTokens.front().span;
    return combine(start, body->span());
}''')

# Semantic symbols and Bound nodes.
replace_once(
    "include/realscript/semantic/Semantic.h",
    "    bool propertySetter = false;\n    std::string sourceName;",
    "    bool propertySetter = false;\n    bool synthetic = false;\n    std::string sourceName;")
replace_once(
    "include/realscript/semantic/Semantic.h",
    "struct PropertySymbol {",
    '''struct EventHandlerSymbol {
    FunctionSymbol function;
    FieldSymbol enabledField;
};

struct EventSubscriptionSymbol {
    text::TextSpan span;
    FieldSymbol enabledField;
    bool enabled = true;
};

struct EventSymbol {
    SymbolId id = 0;
    std::string name;
    std::string delegateName;
    std::vector<VariableSymbol> parameters;
    std::vector<EventHandlerSymbol> handlers;
    std::vector<EventSubscriptionSymbol> subscriptions;
    std::string sourceName;
    text::TextSpan declarationSpan;
};

struct PropertySymbol {''')
replace_once(
    "include/realscript/semantic/Semantic.h",
    "    std::vector<FunctionSymbol> constructors;\n    std::vector<PropertySymbol> properties;",
    "    std::vector<FunctionSymbol> constructors;\n    std::vector<PropertySymbol> properties;\n    std::vector<EventSymbol> events;")
replace_once(
    "include/realscript/semantic/Semantic.h",
    "    std::optional<FunctionSymbol> sequenceNextCallback;\n};",
    "    std::optional<FunctionSymbol> sequenceNextCallback;\n    const syntax::LambdaExpressionSyntax* eventLambda = nullptr;\n};")
replace_once(
    "include/realscript/semantic/Semantic.h",
    "    ReferenceCallExpression,\n    NewObjectExpression,",
    "    ReferenceCallExpression,\n    EventInvocationExpression,\n    NewObjectExpression,")
replace_once(
    "include/realscript/semantic/Semantic.h",
    "    SwitchStatement,\n    VariableDeclarationStatement,",
    "    SwitchStatement,\n    EventSubscriptionStatement,\n    VariableDeclarationStatement,")
replace_once(
    "include/realscript/semantic/Semantic.h",
    "struct BoundNewObjectExpression final : BoundExpression {",
    '''struct BoundEventInvocationExpression final : BoundExpression {
    TypeSymbol ownerType;
    EventSymbol event;
    std::unique_ptr<BoundExpression> receiver;
    std::vector<std::unique_ptr<BoundExpression>> arguments;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::EventInvocationExpression;
    }
};

struct BoundNewObjectExpression final : BoundExpression {''')
replace_once(
    "include/realscript/semantic/Semantic.h",
    "struct BoundVariableDeclarationStatement final : BoundStatement {",
    '''struct BoundEventSubscriptionStatement final : BoundStatement {
    TypeSymbol ownerType;
    std::unique_ptr<BoundExpression> receiver;
    FieldSymbol enabledField;
    bool enabled = true;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::EventSubscriptionStatement;
    }
};

struct BoundVariableDeclarationStatement final : BoundStatement {''')
replace_once(
    "include/realscript/semantic/Semantic.h",
    "    [[nodiscard]] std::unique_ptr<BoundStatement> bindSwitchStatement(\n        const syntax::SwitchStatementSyntax& syntax);",
    "    [[nodiscard]] std::unique_ptr<BoundStatement> bindSwitchStatement(\n        const syntax::SwitchStatementSyntax& syntax);\n    [[nodiscard]] std::unique_ptr<BoundStatement> bindEventSubscriptionStatement(\n        const syntax::EventSubscriptionStatementSyntax& syntax);")

# Compilation delegate contracts and event discovery helpers.
replace_once(
    "src/compiler/Compilation.cpp",
    "using InterfaceMap = std::map<std::string, InterfaceContract>;",
    '''using InterfaceMap = std::map<std::string, InterfaceContract>;

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

using DelegateMap = std::map<std::string, DelegateContract>;''')
replace_once(
    "src/compiler/Compilation.cpp",
    "    InterfaceMap visibleInterfaces;\n    std::vector<semantic::FunctionSymbol> declarations;",
    "    InterfaceMap visibleInterfaces;\n    std::vector<DelegateDeclarationInput> delegateInputs;\n    DelegateMap delegates;\n    DelegateMap visibleDelegates;\n    std::vector<semantic::FunctionSymbol> declarations;")
replace_once(
    "src/compiler/Compilation.cpp",
    "void refreshVisibleInterfaces(\n    std::map<std::string, ModuleWork>& modules,\n    ModuleWork& module) {",
    '''std::string canonicalDelegateName(
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
    ModuleWork& module) {''')
replace_once(
    "src/compiler/Compilation.cpp",
    "std::string fieldTypeSignature(const semantic::FieldSymbol& field) {",
    '''void collectEventSubscriptions(
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

std::string fieldTypeSignature(const semantic::FieldSymbol& field) {''')
# Include events in the stable type signature.
replace_once(
    "src/compiler/Compilation.cpp",
    "    for (const auto& property : type.properties) {",
    '''    for (const auto& event : type.events) {
        out << "event:" << event.name << ':' << event.delegateName << '(';
        for (const auto& parameter : event.parameters) {
            out << semantic::primitiveTypeName(parameter.type) << '#'
                << parameter.typeName << ';';
        }
        out << ");";
    }
    for (const auto& property : type.properties) {''')

# Collect delegate declarations together with named-type shells.
replace_once(
    "src/compiler/Compilation.cpp",
    '''            for (const auto& node : unit->syntaxTree->interfaces) {
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
''',
    '''            for (const auto& node : unit->syntaxTree->interfaces) {
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
''')

# Resolve delegate contracts after named type shells.
replace_once(
    "src/compiler/Compilation.cpp",
    '''    for (auto& [name, module] : modules) {
        (void)name;
        refreshVisibleInterfaces(modules, module);
    }

    // Resolve all field layouts and enum values before declaring member signatures.
''',
    '''    for (auto& [name, module] : modules) {
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
''')

# Native event contracts, subscriptions, lambda functions and deterministic slots.
replace_once(
    "src/compiler/Compilation.cpp",
    '''                    *ownerPointer = std::move(owner);
''',
    '''                    if (!typeSyntax.events.empty()) {
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
''')

# Add delegate contracts to module public fingerprints.
replace_once(
    "src/compiler/Compilation.cpp",
    '''        for (const auto& [interfaceName, contract] : module.interfaces) {
            (void)interfaceName;
            std::ostringstream interfaceSignature;
''',
    '''        for (const auto& [delegateName, contract] : module.delegates) {
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
''')

# Binder: event lambda bodies and subscription statements.
replace_once(
    "src/semantic/SemanticBinding.cpp",
    "    } else if (input.sequence) {\n        result.body = bindSequenceSegment(input);",
    '''    } else if (input.eventLambda) {
        result.body = std::make_unique<BoundBlockStatement>();
        result.body->span = input.eventLambda->span();
        auto statement = std::make_unique<BoundExpressionStatement>();
        statement->span = input.eventLambda->body->span();
        statement->expression = bindExpression(
            *input.eventLambda->body);
        result.body->statements.push_back(std::move(statement));
    } else if (input.sequence) {
        result.body = bindSequenceSegment(input);''')
replace_once(
    "src/semantic/SemanticBinding.cpp",
    "    case syntax::SyntaxKind::VariableDeclarationStatement:\n        return bindVariableDeclaration(",
    "    case syntax::SyntaxKind::EventSubscriptionStatement:\n        return bindEventSubscriptionStatement(\n            static_cast<const syntax::EventSubscriptionStatementSyntax&>(syntaxTree));\n    case syntax::SyntaxKind::VariableDeclarationStatement:\n        return bindVariableDeclaration(")
replace_once(
    "src/semantic/SemanticBinding.cpp",
    "std::unique_ptr<BoundStatement> Binder::bindVariableDeclaration(",
    '''std::unique_ptr<BoundStatement>
Binder::bindEventSubscriptionStatement(
    const syntax::EventSubscriptionStatementSyntax& syntaxTree) {
    auto result = std::make_unique<BoundEventSubscriptionStatement>();
    result->span = syntaxTree.span();
    if (!currentOwnerType_ || currentStaticMethod_) {
        diagnostics_.report(
            "RS8308",
            "event subscriptions require an instance method",
            syntaxTree.span());
        return result;
    }
    const auto* thisVariable = lookupVariable("this");
    if (!thisVariable) return result;
    for (const auto& event : currentOwnerType_->events) {
        if (event.name != syntaxTree.eventNameToken.text) continue;
        for (const auto& subscription : event.subscriptions) {
            if (subscription.span.start == syntaxTree.span().start &&
                subscription.span.length == syntaxTree.span().length) {
                result->ownerType = *currentOwnerType_;
                result->receiver = makeVariableAccess(
                    *thisVariable, syntaxTree.span());
                result->enabledField = subscription.enabledField;
                result->enabled = subscription.enabled;
                return result;
            }
        }
    }
    diagnostics_.report(
        "RS8309",
        "event subscription was not resolved",
        syntaxTree.span());
    return result;
}

std::unique_ptr<BoundStatement> Binder::bindVariableDeclaration(''')

# Event invocation binding before ordinary overload selection.
replace_once(
    "src/semantic/SemanticExpressions.cpp",
    '''    std::vector<const FunctionSymbol*> candidates;
    const auto globals = visibleFunctions_.find(
        syntaxTree.identifierToken.text);
''',
    '''    if (currentOwnerType_ && !currentStaticMethod_) {
        for (const auto& event : currentOwnerType_->events) {
            if (event.name != syntaxTree.identifierToken.text) continue;
            auto result =
                std::make_unique<BoundEventInvocationExpression>();
            result->span = syntaxTree.span();
            result->type = PrimitiveType::Void;
            result->ownerType = *currentOwnerType_;
            result->event = event;
            const auto* thisVariable = lookupVariable("this");
            if (!thisVariable) return makeError(syntaxTree.span());
            result->receiver = variableExpression(
                *thisVariable, syntaxTree.span());
            if (arguments.size() != event.parameters.size()) {
                diagnostics_.report(
                    "RS8313",
                    "event argument count does not match delegate",
                    syntaxTree.span());
            }
            const auto count = std::min(
                arguments.size(), event.parameters.size());
            for (std::size_t index = 0; index < count; ++index) {
                if (index < syntaxTree.argumentModifiers.size() &&
                    syntaxTree.argumentModifiers[index]) {
                    diagnostics_.report(
                        "RS8314",
                        "event arguments cannot use ref, out, or in",
                        syntaxTree.arguments[index]->span());
                }
                result->arguments.push_back(convertExpression(
                    std::move(arguments[index]),
                    event.parameters[index].type,
                    syntaxTree.arguments[index]->span(),
                    "event argument",
                    event.parameters[index].typeName));
            }
            return result;
        }
    }

    std::vector<const FunctionSymbol*> candidates;
    const auto globals = visibleFunctions_.find(
        syntaxTree.identifierToken.text);
''')

# Flow analysis.
replace_once(
    "src/semantic/FlowAnalysis.cpp",
    "        case BoundNodeKind::ReferenceCallExpression: {",
    '''        case BoundNodeKind::EventInvocationExpression: {
            const auto& event = static_cast<const
                BoundEventInvocationExpression&>(expression);
            analyzeExpression(*event.receiver, assigned);
            for (const auto& argument : event.arguments) {
                analyzeExpression(*argument, assigned);
            }
            return;
        }
        case BoundNodeKind::ReferenceCallExpression: {''')
replace_once(
    "src/semantic/FlowAnalysis.cpp",
    "        case BoundNodeKind::VariableDeclarationStatement: {",
    '''        case BoundNodeKind::EventSubscriptionStatement: {
            const auto& subscription = static_cast<const
                BoundEventSubscriptionStatement&>(statement);
            if (subscription.receiver) {
                analyzeExpression(*subscription.receiver, state.assigned);
            }
            return state;
        }
        case BoundNodeKind::VariableDeclarationStatement: {''')

# MIR lowering uses existing bool fields and normal calls.
replace_once(
    "src/mir/MirLowerer.cpp",
    "    case semantic::BoundNodeKind::VariableDeclarationStatement: {",
    '''    case semantic::BoundNodeKind::EventSubscriptionStatement: {
        const auto& subscription = static_cast<const
            semantic::BoundEventSubscriptionStatement&>(statement);
        const auto receiver = lowerExpression(*subscription.receiver);
        const auto checked = emitValue(
            Opcode::CheckNotNull,
            semantic::PrimitiveType::Object,
            {receiver},
            statement.span);
        auto& check = block(*currentBlockId_).instructions.back();
        check.typeId = subscription.ownerType.id;
        check.resultTypeId = subscription.ownerType.id;
        check.symbolName = semantic::canonicalTypeName(
            subscription.ownerType);
        const auto enabled = emitValue(
            Opcode::ConstantBool,
            semantic::PrimitiveType::Bool,
            {},
            statement.span);
        block(*currentBlockId_).instructions.back().boolImmediate =
            subscription.enabled;
        Instruction store;
        store.resultType = semantic::PrimitiveType::Void;
        store.opcode = Opcode::StoreField;
        store.operands = {checked, enabled};
        store.typeId = subscription.ownerType.id;
        store.fieldIndex = subscription.enabledField.index;
        store.symbolName = semantic::canonicalTypeName(
            subscription.ownerType);
        store.sourceSpan = statement.span;
        block(*currentBlockId_).instructions.push_back(
            std::move(store));
        return;
    }
    case semantic::BoundNodeKind::VariableDeclarationStatement: {''')
replace_once(
    "src/mir/MirExpressions.cpp",
    "    case semantic::BoundNodeKind::ReferenceCallExpression: {",
    '''    case semantic::BoundNodeKind::EventInvocationExpression: {
        const auto& invocation = static_cast<const
            semantic::BoundEventInvocationExpression&>(expression);
        auto receiver = lowerExpression(*invocation.receiver);
        receiver = emitValue(
            Opcode::CheckNotNull,
            semantic::PrimitiveType::Object,
            {receiver},
            invocation.receiver->span);
        auto& receiverCheck =
            block(*currentBlockId_).instructions.back();
        receiverCheck.typeId = invocation.ownerType.id;
        receiverCheck.resultTypeId = invocation.ownerType.id;
        receiverCheck.symbolName = semantic::canonicalTypeName(
            invocation.ownerType);
        std::vector<ValueId> values;
        for (const auto& argument : invocation.arguments) {
            values.push_back(lowerExpression(*argument));
        }
        for (const auto& handler : invocation.event.handlers) {
            const auto enabled = emitValue(
                Opcode::LoadField,
                semantic::PrimitiveType::Bool,
                {receiver},
                expression.span);
            auto& load = block(*currentBlockId_).instructions.back();
            load.typeId = invocation.ownerType.id;
            load.fieldIndex = handler.enabledField.index;
            load.symbolName = semantic::canonicalTypeName(
                invocation.ownerType);
            const auto callBlock = createBlock();
            const auto nextBlock = createBlock();
            emitBranch(
                enabled,
                callBlock,
                nextBlock,
                {},
                {},
                expression.span);
            setCurrentBlock(callBlock);
            std::vector<ValueId> arguments{receiver};
            arguments.insert(
                arguments.end(), values.begin(), values.end());
            (void)emitCallInstruction(
                handler.function,
                semantic::PrimitiveType::Void,
                {},
                std::move(arguments),
                expression.span);
            emitJump(nextBlock, {}, expression.span);
            setCurrentBlock(nextBlock);
        }
        return -1;
    }
    case semantic::BoundNodeKind::ReferenceCallExpression: {''')

# Disable legacy event/lambda source expansion.
replace_once(
    "include/realscript/compiler/LanguageExpansion.h",
    "    bool delegatesLambdasEvents = true;",
    "    bool delegatesLambdasEvents = false;")

# Native event execution, diagnostics, and expansion bypass coverage.
test_path = ROOT / "tests/phase18_native_control_flow_tests.cpp"
tests = test_path.read_text(encoding="utf-8")
anchor = "void testNativeSequenceDiagnostics() {"
insert = '''void testNativeEventsExecution() {
    const auto result = execute(R"(
module Phase18;
delegate void ChangedHandler(int amount);
class Counter
{
    event ChangedHandler Changed;
    int total;

    Counter()
    {
        Changed += OnChanged;
        Changed += amount => total = total + amount;
    }

    void OnChanged(int amount)
    {
        total = total + amount;
    }

    int Run()
    {
        Changed(3);
        Changed -= OnChanged;
        Changed(2);
        return total;
    }
}

int main()
{
    Counter counter = new Counter();
    return counter.Run();
}
)");
    require(
        result.succeeded &&
            std::get<std::int64_t>(result.value) == 8,
        "native event execution produced the wrong result");
}

void testNativeEventDiagnostics() {
    realscript::compiler::Compilation compilation({{
        "bad-events.rs",
        R"(
module Phase18.BadEvents;
delegate void ChangedHandler(int amount);
class Counter
{
    event ChangedHandler Changed;
    void Wrong(string value) {}
    void Run() { Changed += Wrong; }
}
)"}});
    const auto build = compilation.build();
    require(build.diagnostics.hasErrors(),
        "invalid native event handler was accepted");
    bool found = false;
    for (const auto& diagnostic : build.diagnostics.items()) {
        found = found || diagnostic.code == "RS8305";
    }
    require(found,
        "invalid native event handler did not produce RS8305");
}

void testEventsBypassExpansion() {
    const auto expansion =
        realscript::compiler::expandLanguageSource(
            "events.rs",
            "module Native; delegate void D(int v); "
            "class C{event D E;int x;void H(int v){x=x+v;}"
            "void R(){E+=H;E(1);}}");
    require(!expansion.changed,
        "native delegates/events still used source expansion");
}

'''
if insert not in tests:
    if anchor not in tests:
        raise RuntimeError("event execution test insertion anchor missing")
    tests = tests.replace(anchor, insert + anchor, 1)
run_anchor = '    run("native sequence diagnostics", testNativeSequenceDiagnostics);'
run_insert = '''    run("native event execution", testNativeEventsExecution);
    run("native event diagnostics", testNativeEventDiagnostics);
    run("events bypass expansion", testEventsBypassExpansion);
'''
if run_insert not in tests:
    if run_anchor not in tests:
        raise RuntimeError("event execution test registration anchor missing")
    tests = tests.replace(run_anchor, run_insert + run_anchor, 1)
test_path.write_text(tests, encoding="utf-8")

# Roadmap status.
roadmap_path = ROOT / "docs/roadmap/PHASE_18_24_NATIVE_LANGUAGE_AND_RUNTIME.md"
roadmap = roadmap_path.read_text(encoding="utf-8")
roadmap = roadmap.replace(
    "### 18B — delegates, lambdas, and events — in progress\n\n- Native delegate/event declarations.\n- Lambda syntax trees and closure analysis.\n- Method-group conversion and deterministic event ordering.\n- First migration may retain bounded captures, but the AST and symbols must be native.",
    "### 18B — delegates, lambdas, and events — complete for the bounded profile\n\nImplemented and validated:\n\n- native delegate and class-local event declarations;\n- native event subscription/removal statements and expression lambdas;\n- module/import-aware delegate contracts;\n- exact method-group signature validation;\n- compiler-owned deterministic boolean subscription slots;\n- field/`this`-capturing expression lambdas compiled as synthetic instance methods;\n- event arguments evaluated once and handlers invoked in first-subscription order;\n- event removal through stable method-group slots;\n- `LanguageExpansionOptions::delegatesLambdasEvents` disabled by default.\n\nFirst-class delegate values, arbitrary local captures, heap closures, combination/removal values, and general event storage remain Phase 20 work.")
roadmap_path.write_text(roadmap, encoding="utf-8")

print("native Phase 18 event semantics applied")
