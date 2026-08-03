#!/usr/bin/env python3
from pathlib import Path
import re

root = Path(__file__).resolve().parents[2]

def update(path, old, new, count=1):
    target = root / path
    text = target.read_text(encoding="utf-8")
    if new in text:
        return
    if text.count(old) < count:
        raise RuntimeError(f"anchor missing in {path}: {old[:100]!r}")
    target.write_text(text.replace(old, new, count), encoding="utf-8")

def regex(path, pattern, replacement):
    target = root / path
    text = target.read_text(encoding="utf-8")
    if replacement in text:
        return
    text, count = re.subn(pattern, replacement, text, count=1, flags=re.S)
    if count != 1:
        raise RuntimeError(f"regex anchor missing in {path}: {pattern[:100]!r}")
    target.write_text(text, encoding="utf-8")

# Syntax model.
update("include/realscript/syntax/Syntax.h",
       "    PlusToken,\n    MinusToken,",
       "    PlusToken,\n    PlusEqualsToken,\n    MinusToken,\n    MinusEqualsToken,\n    ArrowToken,")
update("include/realscript/syntax/Syntax.h",
       "    InterfaceKeyword,\n    StaticKeyword,",
       "    InterfaceKeyword,\n    DelegateKeyword,\n    EventKeyword,\n    StaticKeyword,")
update("include/realscript/syntax/Syntax.h",
       "    InterfaceMethodDeclaration,\n    EnumMemberDeclaration,",
       "    InterfaceMethodDeclaration,\n    DelegateDeclaration,\n    EventDeclaration,\n    EnumMemberDeclaration,")
update("include/realscript/syntax/Syntax.h",
       "    YieldWaitStatement,\n    VariableDeclarationStatement,",
       "    YieldWaitStatement,\n    EventSubscriptionStatement,\n    VariableDeclarationStatement,")
update("include/realscript/syntax/Syntax.h",
       "    NameExpression,\n    UnaryExpression,",
       "    NameExpression,\n    LambdaExpression,\n    UnaryExpression,")
update("include/realscript/syntax/Syntax.h",
       "struct UnaryExpressionSyntax final : ExpressionSyntax {",
       '''struct LambdaExpressionSyntax final : ExpressionSyntax {
    std::optional<SyntaxToken> openParenToken;
    std::vector<SyntaxToken> parameterTokens;
    std::vector<SyntaxToken> commaTokens;
    std::optional<SyntaxToken> closeParenToken;
    SyntaxToken arrowToken;
    std::unique_ptr<ExpressionSyntax> body;

    [[nodiscard]] SyntaxKind kind() const noexcept override {
        return SyntaxKind::LambdaExpression;
    }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct UnaryExpressionSyntax final : ExpressionSyntax {''')
update("include/realscript/syntax/Syntax.h",
       "struct VariableDeclarationStatementSyntax final : StatementSyntax {",
       '''struct EventSubscriptionStatementSyntax final : StatementSyntax {
    SyntaxToken eventNameToken;
    SyntaxToken operatorToken;
    std::unique_ptr<ExpressionSyntax> handler;
    SyntaxToken semicolonToken;

    [[nodiscard]] SyntaxKind kind() const noexcept override {
        return SyntaxKind::EventSubscriptionStatement;
    }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct VariableDeclarationStatementSyntax final : StatementSyntax {''')
update("include/realscript/syntax/Syntax.h",
       "struct FunctionDeclarationSyntax;\nstruct SequenceDeclarationSyntax;",
       "struct FunctionDeclarationSyntax;\nstruct SequenceDeclarationSyntax;\n\nstruct EventDeclarationSyntax final : SyntaxNode {\n    std::vector<AttributeListSyntax> attributes;\n    SyntaxToken eventKeyword;\n    TypeSyntax delegateType;\n    SyntaxToken identifierToken;\n    SyntaxToken semicolonToken;\n\n    [[nodiscard]] SyntaxKind kind() const noexcept override {\n        return SyntaxKind::EventDeclaration;\n    }\n    [[nodiscard]] text::TextSpan span() const noexcept override;\n};")
update("include/realscript/syntax/Syntax.h",
       "    std::vector<FieldDeclarationSyntax> fields;\n    std::vector<FunctionDeclarationSyntax> methods;",
       "    std::vector<FieldDeclarationSyntax> fields;\n    std::vector<EventDeclarationSyntax> events;\n    std::vector<FunctionDeclarationSyntax> methods;",
       count=2)
update("include/realscript/syntax/Syntax.h",
       "struct SequenceDeclarationSyntax final : SyntaxNode {",
       '''struct DelegateDeclarationSyntax final : SyntaxNode {
    std::vector<AttributeListSyntax> attributes;
    SyntaxToken delegateKeyword;
    TypeSyntax returnType;
    SyntaxToken identifierToken;
    SyntaxToken openParenToken;
    std::vector<ParameterSyntax> parameters;
    std::vector<SyntaxToken> commaTokens;
    SyntaxToken closeParenToken;
    SyntaxToken semicolonToken;

    [[nodiscard]] SyntaxKind kind() const noexcept override {
        return SyntaxKind::DelegateDeclaration;
    }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct SequenceDeclarationSyntax final : SyntaxNode {''')
update("include/realscript/syntax/Syntax.h",
       "    std::vector<InterfaceDeclarationSyntax> interfaces;\n    std::vector<FunctionDeclarationSyntax> functions;",
       "    std::vector<InterfaceDeclarationSyntax> interfaces;\n    std::vector<DelegateDeclarationSyntax> delegates;\n    std::vector<FunctionDeclarationSyntax> functions;")
update("include/realscript/syntax/Syntax.h",
       "    [[nodiscard]] InterfaceMethodDeclarationSyntax parseInterfaceMethodDeclaration(\n        std::vector<AttributeListSyntax> attributes);",
       "    [[nodiscard]] InterfaceMethodDeclarationSyntax parseInterfaceMethodDeclaration(\n        std::vector<AttributeListSyntax> attributes);\n    [[nodiscard]] DelegateDeclarationSyntax parseDelegateDeclaration(\n        std::vector<AttributeListSyntax> attributes);\n    [[nodiscard]] EventDeclarationSyntax parseEventDeclaration(\n        std::vector<AttributeListSyntax> attributes);")
update("include/realscript/syntax/Syntax.h",
       "    [[nodiscard]] std::unique_ptr<StatementSyntax> parseYieldWaitStatement();\n    [[nodiscard]] std::unique_ptr<StatementSyntax> parseVariableDeclarationStatement();",
       "    [[nodiscard]] std::unique_ptr<StatementSyntax> parseYieldWaitStatement();\n    [[nodiscard]] std::unique_ptr<StatementSyntax> parseEventSubscriptionStatement();\n    [[nodiscard]] std::unique_ptr<ExpressionSyntax> parseEventHandler();\n    [[nodiscard]] std::unique_ptr<StatementSyntax> parseVariableDeclarationStatement();")

# Facts and lexer.
update("src/syntax/SyntaxFacts.cpp",
       "        RS_KIND(PlusToken);\n        RS_KIND(MinusToken);",
       "        RS_KIND(PlusToken);\n        RS_KIND(PlusEqualsToken);\n        RS_KIND(MinusToken);\n        RS_KIND(MinusEqualsToken);\n        RS_KIND(ArrowToken);")
update("src/syntax/SyntaxFacts.cpp",
       "        RS_KIND(InterfaceKeyword);\n        RS_KIND(StaticKeyword);",
       "        RS_KIND(InterfaceKeyword);\n        RS_KIND(DelegateKeyword);\n        RS_KIND(EventKeyword);\n        RS_KIND(StaticKeyword);")
update("src/syntax/SyntaxFacts.cpp",
       "        RS_KIND(InterfaceMethodDeclaration);\n        RS_KIND(EnumMemberDeclaration);",
       "        RS_KIND(InterfaceMethodDeclaration);\n        RS_KIND(DelegateDeclaration);\n        RS_KIND(EventDeclaration);\n        RS_KIND(EnumMemberDeclaration);")
update("src/syntax/SyntaxFacts.cpp",
       "        RS_KIND(YieldWaitStatement);\n        RS_KIND(VariableDeclarationStatement);",
       "        RS_KIND(YieldWaitStatement);\n        RS_KIND(EventSubscriptionStatement);\n        RS_KIND(VariableDeclarationStatement);")
update("src/syntax/SyntaxFacts.cpp",
       "        RS_KIND(NameExpression);\n        RS_KIND(UnaryExpression);",
       "        RS_KIND(NameExpression);\n        RS_KIND(LambdaExpression);\n        RS_KIND(UnaryExpression);")
update("src/syntax/SyntaxFacts.cpp",
       "        {\"interface\", SyntaxKind::InterfaceKeyword},\n        {\"static\", SyntaxKind::StaticKeyword},",
       "        {\"interface\", SyntaxKind::InterfaceKeyword},\n        {\"delegate\", SyntaxKind::DelegateKeyword},\n        {\"event\", SyntaxKind::EventKeyword},\n        {\"static\", SyntaxKind::StaticKeyword},")
update("src/syntax/Lexer.cpp",
       "    case '+': return single(SyntaxKind::PlusToken);\n    case '-': return single(SyntaxKind::MinusToken);",
       "    case '+': return peek(1) == '='\n        ? pair(SyntaxKind::PlusEqualsToken)\n        : single(SyntaxKind::PlusToken);\n    case '-': return peek(1) == '='\n        ? pair(SyntaxKind::MinusEqualsToken)\n        : single(SyntaxKind::MinusToken);")
update("src/syntax/Lexer.cpp",
       "    case '=': return peek(1) == '=' ? pair(SyntaxKind::EqualsEqualsToken) : single(SyntaxKind::EqualsToken);",
       "    case '=':\n        if (peek(1) == '=') return pair(SyntaxKind::EqualsEqualsToken);\n        if (peek(1) == '>') return pair(SyntaxKind::ArrowToken);\n        return single(SyntaxKind::EqualsToken);")

# Source spans.
update("src/syntax/SyntaxNodes.cpp",
       "text::TextSpan UnaryExpressionSyntax::span() const noexcept {",
       "text::TextSpan LambdaExpressionSyntax::span() const noexcept {\n    return combine(\n        openParenToken ? openParenToken->span\n                       : parameterTokens.front().span,\n        body->span());\n}\n\ntext::TextSpan UnaryExpressionSyntax::span() const noexcept {")
update("src/syntax/SyntaxNodes.cpp",
       "text::TextSpan VariableDeclarationStatementSyntax::span() const noexcept {",
       "text::TextSpan EventSubscriptionStatementSyntax::span() const noexcept {\n    return combine(eventNameToken.span, semicolonToken.span);\n}\n\ntext::TextSpan VariableDeclarationStatementSyntax::span() const noexcept {")
update("src/syntax/SyntaxNodes.cpp",
       "text::TextSpan ConstructorDeclarationSyntax::span() const noexcept {",
       "text::TextSpan EventDeclarationSyntax::span() const noexcept {\n    return combine(\n        declarationStart(attributes, eventKeyword.span),\n        semicolonToken.span);\n}\n\ntext::TextSpan ConstructorDeclarationSyntax::span() const noexcept {")
update("src/syntax/SyntaxNodes.cpp",
       "text::TextSpan SequenceDeclarationSyntax::span() const noexcept {",
       "text::TextSpan DelegateDeclarationSyntax::span() const noexcept {\n    return combine(\n        declarationStart(attributes, delegateKeyword.span),\n        semicolonToken.span);\n}\n\ntext::TextSpan SequenceDeclarationSyntax::span() const noexcept {")
update("src/syntax/SyntaxNodes.cpp",
       "    if (!functions.empty()) {\n        return combine(functions.front().span(), endOfFileToken.span);\n    }",
       "    if (!delegates.empty()) {\n        return combine(delegates.front().span(), endOfFileToken.span);\n    }\n    if (!functions.empty()) {\n        return combine(functions.front().span(), endOfFileToken.span);\n    }")

# Parser declarations and statements.
update("src/syntax/Parser.cpp",
       "        } else if (current().kind == SyntaxKind::InterfaceKeyword) {\n            result.interfaces.push_back(\n                parseInterfaceDeclaration(std::move(attributes)));\n        } else {",
       "        } else if (current().kind == SyntaxKind::InterfaceKeyword) {\n            result.interfaces.push_back(\n                parseInterfaceDeclaration(std::move(attributes)));\n        } else if (current().kind == SyntaxKind::DelegateKeyword) {\n            result.delegates.push_back(\n                parseDelegateDeclaration(std::move(attributes)));\n        } else {")
update("src/syntax/Parser.cpp",
       "ConstructorDeclarationSyntax Parser::parseConstructorDeclaration(",
       '''DelegateDeclarationSyntax Parser::parseDelegateDeclaration(
    std::vector<AttributeListSyntax> attributes) {
    DelegateDeclarationSyntax result;
    result.attributes = std::move(attributes);
    result.delegateKeyword = match(SyntaxKind::DelegateKeyword);
    result.returnType = parseType();
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
    result.semicolonToken = match(SyntaxKind::SemicolonToken);
    return result;
}

EventDeclarationSyntax Parser::parseEventDeclaration(
    std::vector<AttributeListSyntax> attributes) {
    EventDeclarationSyntax result;
    result.attributes = std::move(attributes);
    result.eventKeyword = match(SyntaxKind::EventKeyword);
    result.delegateType = parseType();
    result.identifierToken = match(SyntaxKind::IdentifierToken);
    result.semicolonToken = match(SyntaxKind::SemicolonToken);
    return result;
}

ConstructorDeclarationSyntax Parser::parseConstructorDeclaration(''')
# Class and struct event branches.
update("src/syntax/Parser.cpp",
       "        std::optional<SyntaxToken> staticKeyword;\n        if (current().kind == SyntaxKind::StaticKeyword) {\n            staticKeyword = nextToken();\n        }\n        if (!staticKeyword &&\n            current().kind == SyntaxKind::SequenceKeyword) {",
       "        if (current().kind == SyntaxKind::EventKeyword) {\n            result.events.push_back(parseEventDeclaration(\n                std::move(memberAttributes)));\n            continue;\n        }\n        std::optional<SyntaxToken> staticKeyword;\n        if (current().kind == SyntaxKind::StaticKeyword) {\n            staticKeyword = nextToken();\n        }\n        if (!staticKeyword &&\n            current().kind == SyntaxKind::SequenceKeyword) {")
update("src/syntax/Parser.cpp",
       "        std::optional<SyntaxToken> staticKeyword;\n        if (current().kind == SyntaxKind::StaticKeyword) staticKeyword = nextToken();\n        if (!staticKeyword &&\n            current().kind == SyntaxKind::SequenceKeyword) {",
       "        if (current().kind == SyntaxKind::EventKeyword) {\n            result.events.push_back(parseEventDeclaration(\n                std::move(memberAttributes)));\n            continue;\n        }\n        std::optional<SyntaxToken> staticKeyword;\n        if (current().kind == SyntaxKind::StaticKeyword) staticKeyword = nextToken();\n        if (!staticKeyword &&\n            current().kind == SyntaxKind::SequenceKeyword) {")
update("src/syntax/Parser.cpp",
       "std::unique_ptr<StatementSyntax> Parser::parseStatement() {\n    switch (current().kind) {",
       "std::unique_ptr<StatementSyntax> Parser::parseStatement() {\n    if (current().kind == SyntaxKind::IdentifierToken &&\n        (peek(1).kind == SyntaxKind::PlusEqualsToken ||\n         peek(1).kind == SyntaxKind::MinusEqualsToken)) {\n        return parseEventSubscriptionStatement();\n    }\n    switch (current().kind) {")
update("src/syntax/Parser.cpp",
       "std::unique_ptr<StatementSyntax> Parser::parseVariableDeclarationStatement() {",
       '''std::unique_ptr<ExpressionSyntax> Parser::parseEventHandler() {
    auto lambda = std::make_unique<LambdaExpressionSyntax>();
    if (current().kind == SyntaxKind::IdentifierToken &&
        peek(1).kind == SyntaxKind::ArrowToken) {
        lambda->parameterTokens.push_back(nextToken());
        lambda->arrowToken = nextToken();
        lambda->body = parseExpression();
        return lambda;
    }
    if (current().kind == SyntaxKind::OpenParenToken) {
        lambda->openParenToken = nextToken();
        if (current().kind != SyntaxKind::CloseParenToken) {
            lambda->parameterTokens.push_back(
                match(SyntaxKind::IdentifierToken));
            while (current().kind == SyntaxKind::CommaToken) {
                lambda->commaTokens.push_back(nextToken());
                lambda->parameterTokens.push_back(
                    match(SyntaxKind::IdentifierToken));
            }
        }
        lambda->closeParenToken = match(SyntaxKind::CloseParenToken);
        lambda->arrowToken = match(SyntaxKind::ArrowToken);
        lambda->body = parseExpression();
        return lambda;
    }
    return parseExpression();
}

std::unique_ptr<StatementSyntax>
Parser::parseEventSubscriptionStatement() {
    auto result =
        std::make_unique<EventSubscriptionStatementSyntax>();
    result->eventNameToken = match(SyntaxKind::IdentifierToken);
    if (current().kind == SyntaxKind::PlusEqualsToken ||
        current().kind == SyntaxKind::MinusEqualsToken) {
        result->operatorToken = nextToken();
    } else {
        result->operatorToken = match(SyntaxKind::PlusEqualsToken);
    }
    result->handler = parseEventHandler();
    result->semicolonToken = match(SyntaxKind::SemicolonToken);
    return result;
}

std::unique_ptr<StatementSyntax> Parser::parseVariableDeclarationStatement() {''')

# Parser-only regression test while compatibility execution remains enabled.
test_path = root / "tests/phase18_native_control_flow_tests.cpp"
tests = test_path.read_text(encoding="utf-8")
anchor = "void testNativeSequenceDiagnostics() {"
insert = '''void testNativeEventSyntax() {
    realscript::text::SourceText source(R"(
module Phase18.Events;
delegate void ChangedHandler(int amount);
class Counter
{
    event ChangedHandler Changed;
    int total;
    void Run()
    {
        Changed += OnChanged;
        Changed += amount => total = total + amount;
    }
    void OnChanged(int amount) { total = total + amount; }
}
)", "event-syntax.rs");
    realscript::diagnostics::DiagnosticBag diagnostics;
    realscript::syntax::Parser parser(source, diagnostics);
    auto unit = parser.parseCompilationUnit();
    require(!diagnostics.hasErrors(),
        "native event syntax failed to parse:\n" +
            diagnosticsText(diagnostics));
    require(unit.delegates.size() == 1 &&
            unit.classes.size() == 1 &&
            unit.classes.front().events.size() == 1,
        "native delegate or event declaration was not retained");
    const auto& statements =
        unit.classes.front().methods.front().body.statements;
    require(statements.size() == 2 &&
            statements[0]->kind() ==
                realscript::syntax::SyntaxKind::EventSubscriptionStatement &&
            statements[1]->kind() ==
                realscript::syntax::SyntaxKind::EventSubscriptionStatement,
        "native event subscription statements were not retained");
    const auto& subscription = static_cast<const
        realscript::syntax::EventSubscriptionStatementSyntax&>(
            *statements[1]);
    require(subscription.handler->kind() ==
            realscript::syntax::SyntaxKind::LambdaExpression,
        "native event lambda was not retained");
}

'''
if insert not in tests:
    if anchor not in tests:
        raise RuntimeError("event syntax test insertion anchor missing")
    tests = tests.replace(anchor, insert + anchor, 1)
run_anchor = '    run("native sequence diagnostics", testNativeSequenceDiagnostics);'
run_insert = '    run("native event syntax", testNativeEventSyntax);\n'
if run_insert not in tests:
    if run_anchor not in tests:
        raise RuntimeError("event syntax test registration anchor missing")
    tests = tests.replace(run_anchor, run_insert + run_anchor, 1)
test_path.write_text(tests, encoding="utf-8")

print("native delegate/event syntax applied")
