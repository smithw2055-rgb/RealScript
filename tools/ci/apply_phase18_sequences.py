#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def write(path: str, text: str) -> None:
    (ROOT / path).write_text(text, encoding="utf-8", newline="\n")


def replace_once(text: str, old: str, new: str, path: str) -> str:
    if old not in text:
        raise RuntimeError(f"anchor missing in {path}: {old[:120]!r}")
    return text.replace(old, new, 1)


# ---------------------------------------------------------------------------
# Public sequence metadata and expansion default
# ---------------------------------------------------------------------------
path = "include/realscript/compiler/LanguageExpansion.h"
text = read(path)
if "struct LanguageSequenceRecord" not in text:
    text = replace_once(
        text,
        "struct LanguageExpansionOptions {\n",
        '''struct LanguageSequenceRecord {
    std::string typeName;
    std::string name;
    std::vector<std::string> callbacks;
    std::string sourceName;
    std::size_t offset = 0;
};

struct LanguageExpansionOptions {
''',
        path)
text = text.replace(
    "    bool deterministicCoroutines = true;",
    "    bool deterministicCoroutines = false;")
write(path, text)


# ---------------------------------------------------------------------------
# Syntax model
# ---------------------------------------------------------------------------
path = "include/realscript/syntax/Syntax.h"
text = read(path)
if "SequenceKeyword" not in text:
    text = replace_once(
        text,
        "    DefaultKeyword,\n    ClassKeyword,",
        "    DefaultKeyword,\n"
        "    SequenceKeyword,\n"
        "    YieldKeyword,\n"
        "    ClassKeyword,",
        path)
    text = replace_once(
        text,
        "    FunctionDeclaration,\n    Parameter,",
        "    FunctionDeclaration,\n"
        "    SequenceDeclaration,\n"
        "    Parameter,",
        path)
    text = replace_once(
        text,
        "    SwitchStatement,\n    VariableDeclarationStatement,",
        "    SwitchStatement,\n"
        "    YieldWaitStatement,\n"
        "    VariableDeclarationStatement,",
        path)

    yield_node = '''struct YieldWaitStatementSyntax final : StatementSyntax {
    SyntaxToken yieldKeyword;
    SyntaxToken waitTicksToken;
    SyntaxToken openParenToken;
    std::unique_ptr<ExpressionSyntax> delay;
    SyntaxToken closeParenToken;
    SyntaxToken semicolonToken;

    [[nodiscard]] SyntaxKind kind() const noexcept override {
        return SyntaxKind::YieldWaitStatement;
    }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

'''
    text = replace_once(
        text,
        "struct VariableDeclarationStatementSyntax final : StatementSyntax {",
        yield_node +
        "struct VariableDeclarationStatementSyntax final : StatementSyntax {",
        path)

    text = replace_once(
        text,
        "struct FunctionDeclarationSyntax;\n",
        "struct FunctionDeclarationSyntax;\n"
        "struct SequenceDeclarationSyntax;\n",
        path)
    text = replace_once(
        text,
        "    std::vector<FunctionDeclarationSyntax> methods;\n"
        "    std::vector<ConstructorDeclarationSyntax> constructors;",
        "    std::vector<FunctionDeclarationSyntax> methods;\n"
        "    std::vector<SequenceDeclarationSyntax> sequences;\n"
        "    std::vector<ConstructorDeclarationSyntax> constructors;",
        path)
    # Second occurrence for struct.
    text = replace_once(
        text,
        "    std::vector<FunctionDeclarationSyntax> methods;\n"
        "    std::vector<ConstructorDeclarationSyntax> constructors;",
        "    std::vector<FunctionDeclarationSyntax> methods;\n"
        "    std::vector<SequenceDeclarationSyntax> sequences;\n"
        "    std::vector<ConstructorDeclarationSyntax> constructors;",
        path)

    sequence_node = '''struct SequenceDeclarationSyntax final : SyntaxNode {
    std::vector<AttributeListSyntax> attributes;
    SyntaxToken sequenceKeyword;
    SyntaxToken identifierToken;
    SyntaxToken openParenToken;
    std::vector<ParameterSyntax> parameters;
    std::vector<SyntaxToken> commaTokens;
    SyntaxToken closeParenToken;
    BlockStatementSyntax body;

    [[nodiscard]] SyntaxKind kind() const noexcept override {
        return SyntaxKind::SequenceDeclaration;
    }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

'''
    text = replace_once(
        text,
        "struct ModuleDeclarationSyntax final : SyntaxNode {",
        sequence_node +
        "struct ModuleDeclarationSyntax final : SyntaxNode {",
        path)

    text = replace_once(
        text,
        "    [[nodiscard]] ConstructorDeclarationSyntax parseConstructorDeclaration(\n",
        "    [[nodiscard]] SequenceDeclarationSyntax parseSequenceDeclaration(\n"
        "        std::vector<AttributeListSyntax> attributes);\n"
        "    [[nodiscard]] ConstructorDeclarationSyntax parseConstructorDeclaration(\n",
        path)
    text = replace_once(
        text,
        "    [[nodiscard]] std::unique_ptr<StatementSyntax> parseSwitchStatement();\n",
        "    [[nodiscard]] std::unique_ptr<StatementSyntax> parseSwitchStatement();\n"
        "    [[nodiscard]] std::unique_ptr<StatementSyntax> parseYieldWaitStatement();\n",
        path)
    write(path, text)


# ---------------------------------------------------------------------------
# Syntax facts and spans
# ---------------------------------------------------------------------------
path = "src/syntax/SyntaxFacts.cpp"
text = read(path)
if "RS_KIND(SequenceKeyword)" not in text:
    text = replace_once(
        text,
        "        RS_KIND(DefaultKeyword);\n        RS_KIND(ClassKeyword);",
        "        RS_KIND(DefaultKeyword);\n"
        "        RS_KIND(SequenceKeyword);\n"
        "        RS_KIND(YieldKeyword);\n"
        "        RS_KIND(ClassKeyword);",
        path)
    text = replace_once(
        text,
        "        RS_KIND(FunctionDeclaration);\n        RS_KIND(Parameter);",
        "        RS_KIND(FunctionDeclaration);\n"
        "        RS_KIND(SequenceDeclaration);\n"
        "        RS_KIND(Parameter);",
        path)
    text = replace_once(
        text,
        "        RS_KIND(SwitchStatement);\n"
        "        RS_KIND(VariableDeclarationStatement);",
        "        RS_KIND(SwitchStatement);\n"
        "        RS_KIND(YieldWaitStatement);\n"
        "        RS_KIND(VariableDeclarationStatement);",
        path)
    text = replace_once(
        text,
        '        {"default", SyntaxKind::DefaultKeyword},\n'
        '        {"class", SyntaxKind::ClassKeyword},',
        '        {"default", SyntaxKind::DefaultKeyword},\n'
        '        {"sequence", SyntaxKind::SequenceKeyword},\n'
        '        {"yield", SyntaxKind::YieldKeyword},\n'
        '        {"class", SyntaxKind::ClassKeyword},',
        path)
    write(path, text)

path = "src/syntax/SyntaxNodes.cpp"
text = read(path)
if "YieldWaitStatementSyntax::span" not in text:
    text = replace_once(
        text,
        "text::TextSpan VariableDeclarationStatementSyntax::span() const noexcept {",
        '''text::TextSpan YieldWaitStatementSyntax::span() const noexcept {
    return combine(yieldKeyword.span, semicolonToken.span);
}

text::TextSpan VariableDeclarationStatementSyntax::span() const noexcept {''',
        path)
    text = replace_once(
        text,
        "text::TextSpan ModuleDeclarationSyntax::span() const noexcept {",
        '''text::TextSpan SequenceDeclarationSyntax::span() const noexcept {
    return combine(
        declarationStart(attributes, sequenceKeyword.span),
        body.span());
}

text::TextSpan ModuleDeclarationSyntax::span() const noexcept {''',
        path)
    write(path, text)


# ---------------------------------------------------------------------------
# Parser
# ---------------------------------------------------------------------------
path = "src/syntax/Parser.cpp"
text = read(path)
if "Parser::parseSequenceDeclaration" not in text:
    sequence_parser = '''SequenceDeclarationSyntax Parser::parseSequenceDeclaration(
    std::vector<AttributeListSyntax> attributes) {
    SequenceDeclarationSyntax result;
    result.attributes = std::move(attributes);
    result.sequenceKeyword = match(SyntaxKind::SequenceKeyword);
    result.identifierToken = match(SyntaxKind::IdentifierToken);
    result.openParenToken = match(SyntaxKind::OpenParenToken);
    if (current().kind != SyntaxKind::CloseParenToken &&
        current().kind != SyntaxKind::EndOfFileToken) {
        result.parameters.push_back(parseParameter());
        while (current().kind == SyntaxKind::CommaToken) {
            result.commaTokens.push_back(nextToken());
            result.parameters.push_back(parseParameter());
        }
    }
    result.closeParenToken = match(SyntaxKind::CloseParenToken);
    result.body = parseBlockStatement();
    return result;
}

'''
    text = replace_once(
        text,
        "ConstructorDeclarationSyntax Parser::parseConstructorDeclaration(\n",
        sequence_parser +
        "ConstructorDeclarationSyntax Parser::parseConstructorDeclaration(\n",
        path)

    class_anchor = '''        if (!staticKeyword && current().kind == SyntaxKind::IdentifierToken &&
            current().text == typeName && peek(1).kind == SyntaxKind::OpenParenToken) {
'''
    class_replace = '''        if (!staticKeyword &&
            current().kind == SyntaxKind::SequenceKeyword) {
            result.sequences.push_back(parseSequenceDeclaration(
                std::move(memberAttributes)));
        } else if (!staticKeyword && current().kind == SyntaxKind::IdentifierToken &&
            current().text == typeName && peek(1).kind == SyntaxKind::OpenParenToken) {
'''
    text = replace_once(text, class_anchor, class_replace, path)
    text = replace_once(text, class_anchor, class_replace, path)

    text = replace_once(
        text,
        "    case SyntaxKind::SwitchKeyword:\n"
        "        return parseSwitchStatement();\n",
        "    case SyntaxKind::SwitchKeyword:\n"
        "        return parseSwitchStatement();\n"
        "    case SyntaxKind::YieldKeyword:\n"
        "        return parseYieldWaitStatement();\n",
        path)

    yield_parser = '''std::unique_ptr<StatementSyntax> Parser::parseYieldWaitStatement() {
    auto result = std::make_unique<YieldWaitStatementSyntax>();
    result->yieldKeyword = match(SyntaxKind::YieldKeyword);
    result->waitTicksToken = match(SyntaxKind::IdentifierToken);
    if (result->waitTicksToken.text != "wait_ticks") {
        diagnostics_.report(
            "RS1113",
            "yield currently supports only wait_ticks(expression)",
            result->waitTicksToken.span);
    }
    result->openParenToken = match(SyntaxKind::OpenParenToken);
    result->delay = parseExpression();
    result->closeParenToken = match(SyntaxKind::CloseParenToken);
    result->semicolonToken = match(SyntaxKind::SemicolonToken);
    return result;
}

'''
    text = replace_once(
        text,
        "std::unique_ptr<StatementSyntax> Parser::parseVariableDeclarationStatement() {",
        yield_parser +
        "std::unique_ptr<StatementSyntax> Parser::parseVariableDeclarationStatement() {",
        path)
    write(path, text)


# ---------------------------------------------------------------------------
# Semantic binding model
# ---------------------------------------------------------------------------
path = "include/realscript/semantic/Semantic.h"
text = read(path)
if "sequenceSegment" not in text:
    text = replace_once(
        text,
        "    FieldSymbol syntheticField;\n",
        "    FieldSymbol syntheticField;\n"
        "    const syntax::SequenceDeclarationSyntax* sequence = nullptr;\n"
        "    std::size_t sequenceSegment = 0;\n"
        "    FieldSymbol sequenceTargetField;\n"
        "    std::optional<FunctionSymbol> sequenceNextCallback;\n",
        path)
    text = replace_once(
        text,
        "    [[nodiscard]] BoundFunction bindFunction(\n"
        "        const FunctionBindingInput& input);\n",
        "    [[nodiscard]] BoundFunction bindFunction(\n"
        "        const FunctionBindingInput& input);\n"
        "    [[nodiscard]] std::unique_ptr<BoundBlockStatement> bindSequenceSegment(\n"
        "        const FunctionBindingInput& input);\n"
        "    [[nodiscard]] std::unique_ptr<BoundExpression> makeSequenceFieldAccess(\n"
        "        const FieldSymbol& field, text::TextSpan span);\n"
        "    [[nodiscard]] std::unique_ptr<BoundExpression> makeVariableAccess(\n"
        "        const VariableSymbol& variable, text::TextSpan span);\n"
        "    [[nodiscard]] const FunctionSymbol* findScheduleFunction() const noexcept;\n",
        path)
    write(path, text)


# ---------------------------------------------------------------------------
# Binder native sequence segments
# ---------------------------------------------------------------------------
path = "src/semantic/SemanticBinding.cpp"
text = read(path)
if "Binder::bindSequenceSegment" not in text:
    text = replace_once(
        text,
        '''    } else if (input.body) {
        result.body = bindBlockStatement(*input.body, false);
    } else {
''',
        '''    } else if (input.sequence) {
        result.body = bindSequenceSegment(input);
    } else if (input.body) {
        result.body = bindBlockStatement(*input.body, false);
    } else {
''',
        path)

    sequence_binding = r'''std::unique_ptr<BoundExpression> Binder::makeVariableAccess(
    const VariableSymbol& variable,
    text::TextSpan span) {
    auto result = std::make_unique<BoundVariableExpression>();
    result->type = variable.type;
    result->typeName = variable.typeName;
    result->variable = variable;
    result->span = span;
    return result;
}

std::unique_ptr<BoundExpression> Binder::makeSequenceFieldAccess(
    const FieldSymbol& field,
    text::TextSpan span) {
    if (!currentOwnerType_) return makeError(span);
    const auto* receiver = lookupVariable("this");
    if (!receiver) return makeError(span);
    if (currentOwnerType_->kind == TypeKind::Struct) {
        auto result = std::make_unique<BoundStructFieldAccessExpression>();
        result->type = field.type;
        result->typeName = field.typeName;
        result->span = span;
        result->receiver = makeVariableAccess(*receiver, span);
        result->ownerType = *currentOwnerType_;
        result->field = field;
        return result;
    }
    auto result = std::make_unique<BoundMemberAccessExpression>();
    result->type = field.type;
    result->typeName = field.typeName;
    result->span = span;
    result->receiver = makeVariableAccess(*receiver, span);
    result->ownerType = *currentOwnerType_;
    result->field = field;
    return result;
}

const FunctionSymbol* Binder::findScheduleFunction() const noexcept {
    const auto found = visibleFunctions_.find("Schedule");
    if (found == visibleFunctions_.end()) return nullptr;
    for (const auto& candidate : found->second) {
        if (candidate.method || candidate.parameters.size() != 3 ||
            candidate.parameters[0].type != PrimitiveType::Long ||
            candidate.parameters[1].type != PrimitiveType::String ||
            candidate.parameters[2].type != PrimitiveType::Int) {
            continue;
        }
        return &candidate;
    }
    return nullptr;
}

std::unique_ptr<BoundBlockStatement> Binder::bindSequenceSegment(
    const FunctionBindingInput& input) {
    auto result = std::make_unique<BoundBlockStatement>();
    if (!input.sequence) return result;
    result->span = input.sequence->body.span();

    std::vector<std::size_t> yields;
    for (std::size_t index = 0;
         index < input.sequence->body.statements.size();
         ++index) {
        if (input.sequence->body.statements[index]->kind() ==
            syntax::SyntaxKind::YieldWaitStatement) {
            yields.push_back(index);
        }
    }
    const auto segmentCount = yields.size() + 1;
    if (input.sequenceSegment >= segmentCount) {
        diagnostics_.report(
            "RS2492",
            "invalid compiler sequence segment",
            input.sequence->identifierToken.span);
        return result;
    }

    const auto begin = input.sequenceSegment == 0
        ? std::size_t{0}
        : yields[input.sequenceSegment - 1] + 1;
    const auto end = input.sequenceSegment < yields.size()
        ? yields[input.sequenceSegment]
        : input.sequence->body.statements.size();

    if (input.sequenceSegment == 0) {
        const auto* receiver = lookupVariable("this");
        const auto* target = input.sequence->parameters.empty()
            ? nullptr
            : lookupVariable(
                input.sequence->parameters.front().identifierToken.text);
        if (!receiver || !target || !currentOwnerType_) {
            diagnostics_.report(
                "RS2492",
                "sequence entry parameters are incomplete",
                input.sequence->identifierToken.span);
        } else if (currentOwnerType_->kind == TypeKind::Struct) {
            diagnostics_.report(
                "RS2491",
                "sequence methods require a class owner",
                input.sequence->identifierToken.span);
        } else {
            auto assignment =
                std::make_unique<BoundMemberAssignmentExpression>();
            assignment->type = input.sequenceTargetField.type;
            assignment->typeName = input.sequenceTargetField.typeName;
            assignment->span = input.sequence->identifierToken.span;
            assignment->receiver = makeVariableAccess(
                *receiver,
                input.sequence->identifierToken.span);
            assignment->ownerType = *currentOwnerType_;
            assignment->field = input.sequenceTargetField;
            assignment->expression = makeVariableAccess(
                *target,
                input.sequence->parameters.front().identifierToken.span);
            auto statement = std::make_unique<BoundExpressionStatement>();
            statement->span = assignment->span;
            statement->expression = std::move(assignment);
            result->statements.push_back(std::move(statement));
        }
    }

    for (std::size_t index = begin; index < end; ++index) {
        result->statements.push_back(
            bindStatement(*input.sequence->body.statements[index]));
    }

    if (input.sequenceSegment < yields.size()) {
        const auto& yieldSyntax =
            static_cast<const syntax::YieldWaitStatementSyntax&>(
                *input.sequence->body.statements[yields[input.sequenceSegment]]);
        const auto* schedule = findScheduleFunction();
        if (!schedule || !input.sequenceNextCallback) {
            diagnostics_.report(
                "RS2493",
                "sequence requires imported RealScript.Game.Schedule",
                yieldSyntax.span());
        } else {
            auto call = std::make_unique<BoundCallExpression>();
            call->type = schedule->returnType;
            call->typeName = schedule->returnTypeName;
            call->span = yieldSyntax.span();
            call->function = *schedule;
            call->arguments.push_back(makeSequenceFieldAccess(
                input.sequenceTargetField,
                yieldSyntax.span()));

            auto callback = std::make_unique<BoundLiteralExpression>();
            callback->type = PrimitiveType::String;
            callback->span = yieldSyntax.waitTicksToken.span;
            callback->value = input.sequenceNextCallback->name;
            call->arguments.push_back(std::move(callback));

            call->arguments.push_back(convertExpression(
                bindExpression(*yieldSyntax.delay),
                PrimitiveType::Int,
                yieldSyntax.delay->span(),
                "sequence wait_ticks delay"));

            auto statement = std::make_unique<BoundExpressionStatement>();
            statement->span = yieldSyntax.span();
            statement->expression = std::move(call);
            result->statements.push_back(std::move(statement));
        }
    }
    return result;
}

'''
    text = replace_once(
        text,
        "std::unique_ptr<BoundBlockStatement> Binder::bindBlockStatement(\n",
        sequence_binding +
        "std::unique_ptr<BoundBlockStatement> Binder::bindBlockStatement(\n",
        path)

    text = replace_once(
        text,
        "    case syntax::SyntaxKind::SwitchStatement:\n"
        "        return bindSwitchStatement(\n"
        "            static_cast<const syntax::SwitchStatementSyntax&>(syntaxTree));\n",
        "    case syntax::SyntaxKind::SwitchStatement:\n"
        "        return bindSwitchStatement(\n"
        "            static_cast<const syntax::SwitchStatementSyntax&>(syntaxTree));\n"
        "    case syntax::SyntaxKind::YieldWaitStatement:\n"
        "        diagnostics_.report(\n"
        "            \"RS2494\",\n"
        "            \"yield wait_ticks is valid only at sequence top level\",\n"
        "            syntaxTree.span());\n"
        "        return std::make_unique<BoundExpressionStatement>();\n",
        path)
    write(path, text)


# ---------------------------------------------------------------------------
# Compiler native sequence symbols and bindings
# ---------------------------------------------------------------------------
path = "include/realscript/compiler/Compilation.h"
text = read(path)
if "nativeSequences" not in text:
    text = replace_once(
        text,
        "    std::vector<LanguageInterfaceImplementation> nativeInterfaces;\n",
        "    std::vector<LanguageInterfaceImplementation> nativeInterfaces;\n"
        "    std::vector<LanguageSequenceRecord> nativeSequences;\n",
        path)
    write(path, text)

path = "src/compiler/Compilation.cpp"
text = read(path)
if "makeSequenceFunction" not in text:
    helpers = r'''semantic::VariableSymbol makeSequenceThisParameter(
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

'''
    text = replace_once(
        text,
        "std::string fieldTypeSignature(const semantic::FieldSymbol& field) {",
        helpers +
        "std::string fieldTypeSignature(const semantic::FieldSymbol& field) {",
        path)

    # Insert sequences in member declaration after normal methods.
    method_loop_end = '''                        owner.methods.push_back(binding.symbol);
                        addBinding(std::move(binding), methodSyntax.identifierToken.span);
                    }
                    for (const auto& constructorSyntax : typeSyntax.constructors) {
'''
    sequence_declarations = r'''                        owner.methods.push_back(binding.symbol);
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
'''
    text = replace_once(
        text,
        method_loop_end,
        sequence_declarations,
        path)

    # Collect sequence attributes.
    sequence_attrs_anchor = '''                    for (const auto& constructor : declaration.constructors) {
'''
    sequence_attrs = '''                    for (const auto& sequence : declaration.sequences) {
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
'''
    text = replace_once(
        text,
        sequence_attrs_anchor,
        sequence_attrs,
        path)

    # Fingerprint native sequence metadata and sort.
    text = replace_once(
        text,
        "        for (const auto& implementation : result.nativeInterfaces) {\n",
        '''        for (const auto& sequence : result.nativeSequences) {
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
''',
        path)
    text = replace_once(
        text,
        "    std::stable_sort(\n"
        "        result.nativeInterfaces.begin(),\n",
        '''    std::stable_sort(
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
''',
        path)
    write(path, text)


# ---------------------------------------------------------------------------
# Game SDK metadata
# ---------------------------------------------------------------------------
path = "include/realscript/game/GameScripting.h"
text = read(path)
if "LanguageSequenceRecord" not in text:
    text = replace_once(
        text,
        "    std::vector<compiler::LanguageGenericInstantiation> genericInstantiations;\n",
        "    std::vector<compiler::LanguageGenericInstantiation> genericInstantiations;\n"
        "    std::vector<compiler::LanguageSequenceRecord> sequences;\n",
        path)
    text = replace_once(
        text,
        "        return attributes.empty() && interfaces.empty() &&\n"
        "            genericInstantiations.empty();\n",
        "        return attributes.empty() && interfaces.empty() &&\n"
        "            genericInstantiations.empty() && sequences.empty();\n",
        path)
    write(path, text)

path = "src/game/GameApi.cpp"
text = read(path)
if "build.nativeSequences" not in text:
    text = replace_once(
        text,
        "    result.languageMetadata.interfaces.insert(\n",
        '''    result.languageMetadata.sequences.insert(
        result.languageMetadata.sequences.end(),
        build.nativeSequences.begin(),
        build.nativeSequences.end());
    std::stable_sort(
        result.languageMetadata.sequences.begin(),
        result.languageMetadata.sequences.end(),
        [](const auto& left, const auto& right) {
            if (left.typeName != right.typeName) {
                return left.typeName < right.typeName;
            }
            return left.name < right.name;
        });
    result.languageMetadata.interfaces.insert(
''',
        path)
    write(path, text)


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
path = "tests/phase11_17_language_expansion_tests.cpp"
text = read(path)
if "sequence source still used expansion" not in text:
    anchor = '''    require(compiled.program.languageMetadata().attributes.size() == 1,
        "GameProgram did not retain source attributes");
'''
    replacement = anchor + '''    require(compiled.languageMetadata.sequences.size() == 1 &&
            compiled.languageMetadata.sequences.front().typeName ==
                "SequenceDemo::Behavior" &&
            compiled.languageMetadata.sequences.front().name == "Attack" &&
            compiled.languageMetadata.sequences.front().callbacks.size() == 2,
        "GameCompileResult did not retain native sequence metadata");
    require(compiled.program.languageMetadata().sequences.size() == 1,
        "GameProgram did not retain native sequence metadata");
    const auto expansion = realscript::compiler::expandLanguageSource(
        "native-sequence.rs", source);
    require(!expansion.changed,
        "sequence source still used expansion");
'''
    text = replace_once(text, anchor, replacement, path)
    write(path, text)

path = "tests/phase18_native_control_flow_tests.cpp"
text = read(path)
if "testNativeSequenceDiagnostics" not in text:
    insertion = r'''
void testNativeSequenceDiagnostics() {
    realscript::compiler::Compilation compilation({{"bad-sequence.rs", R"(
module Phase18.SequenceBad;
class Behavior
{
    sequence Attack(int target)
    {
        yield wait_ticks(1);
    }
}
)"}});
    const auto build = compilation.build();
    require(build.diagnostics.hasErrors(),
        "invalid native sequence signature was accepted");
    bool found = false;
    for (const auto& diagnostic : build.diagnostics.items()) {
        found = found || diagnostic.code == "RS2490";
    }
    require(found,
        "invalid native sequence did not produce RS2490");
}

void testYieldOutsideSequenceDiagnostics() {
    realscript::compiler::Compilation compilation({{"bad-yield.rs", R"(
module Phase18.YieldBad;
int main()
{
    yield wait_ticks(1);
    return 0;
}
)"}});
    const auto build = compilation.build();
    require(build.diagnostics.hasErrors(),
        "yield outside sequence was accepted");
    bool found = false;
    for (const auto& diagnostic : build.diagnostics.items()) {
        found = found || diagnostic.code == "RS2494";
    }
    require(found,
        "yield outside sequence did not produce RS2494");
}
'''
    text = replace_once(
        text,
        "\n} // namespace\n\nint main() {",
        insertion + "\n} // namespace\n\nint main() {",
        path)
    text = replace_once(
        text,
        '    run("attributes bypass expansion", testAttributesBypassExpansion);\n',
        '    run("attributes bypass expansion", testAttributesBypassExpansion);\n'
        '    run("native sequence diagnostics", testNativeSequenceDiagnostics);\n'
        '    run("yield outside sequence diagnostics", testYieldOutsideSequenceDiagnostics);\n',
        path)
    write(path, text)

print("Native Phase 18 deterministic sequences applied")
