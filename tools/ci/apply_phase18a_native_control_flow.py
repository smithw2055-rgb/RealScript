#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def write(path: str, content: str) -> None:
    (ROOT / path).write_text(content, encoding="utf-8", newline="\n")


def replace_once(text: str, old: str, new: str, path: str) -> str:
    if old not in text:
        raise RuntimeError(f"anchor not found in {path}: {old[:80]!r}")
    return text.replace(old, new, 1)


# ---------------------------------------------------------------------------
# Syntax model
# ---------------------------------------------------------------------------
path = "include/realscript/syntax/Syntax.h"
text = read(path)
if "ForKeyword" not in text:
    text = replace_once(text,
        "    WhileKeyword,\n    ClassKeyword,",
        "    WhileKeyword,\n    ForKeyword,\n    ForeachKeyword,\n    InKeyword,\n    DoKeyword,\n    BreakKeyword,\n    ContinueKeyword,\n    SwitchKeyword,\n    CaseKeyword,\n    DefaultKeyword,\n    ClassKeyword,", path)
    text = replace_once(text,
        "    WhileStatement,\n    VariableDeclarationStatement,",
        "    WhileStatement,\n    ForStatement,\n    ForeachStatement,\n    DoWhileStatement,\n    BreakStatement,\n    ContinueStatement,\n    SwitchSection,\n    SwitchStatement,\n    VariableDeclarationStatement,", path)
    anchor = "struct VariableDeclarationStatementSyntax final : StatementSyntax {"
    additions = r'''struct ForStatementSyntax final : StatementSyntax {
    SyntaxToken forKeyword;
    SyntaxToken openParenToken;
    std::unique_ptr<StatementSyntax> initializer;
    std::optional<SyntaxToken> firstSemicolonToken;
    std::unique_ptr<ExpressionSyntax> condition;
    SyntaxToken secondSemicolonToken;
    std::unique_ptr<ExpressionSyntax> increment;
    SyntaxToken closeParenToken;
    std::unique_ptr<StatementSyntax> body;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::ForStatement; }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct ForeachStatementSyntax final : StatementSyntax {
    SyntaxToken foreachKeyword;
    SyntaxToken openParenToken;
    TypeSyntax type;
    SyntaxToken identifierToken;
    SyntaxToken inKeyword;
    std::unique_ptr<ExpressionSyntax> collection;
    SyntaxToken closeParenToken;
    std::unique_ptr<StatementSyntax> body;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::ForeachStatement; }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct DoWhileStatementSyntax final : StatementSyntax {
    SyntaxToken doKeyword;
    std::unique_ptr<StatementSyntax> body;
    SyntaxToken whileKeyword;
    SyntaxToken openParenToken;
    std::unique_ptr<ExpressionSyntax> condition;
    SyntaxToken closeParenToken;
    SyntaxToken semicolonToken;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::DoWhileStatement; }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct BreakStatementSyntax final : StatementSyntax {
    SyntaxToken breakKeyword;
    SyntaxToken semicolonToken;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::BreakStatement; }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct ContinueStatementSyntax final : StatementSyntax {
    SyntaxToken continueKeyword;
    SyntaxToken semicolonToken;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::ContinueStatement; }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct SwitchSectionSyntax final : SyntaxNode {
    std::optional<SyntaxToken> caseKeyword;
    std::optional<SyntaxToken> defaultKeyword;
    std::unique_ptr<ExpressionSyntax> label;
    SyntaxToken colonToken;
    std::vector<std::unique_ptr<StatementSyntax>> statements;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::SwitchSection; }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct SwitchStatementSyntax final : StatementSyntax {
    SyntaxToken switchKeyword;
    SyntaxToken openParenToken;
    std::unique_ptr<ExpressionSyntax> expression;
    SyntaxToken closeParenToken;
    SyntaxToken openBraceToken;
    std::vector<SwitchSectionSyntax> sections;
    SyntaxToken closeBraceToken;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::SwitchStatement; }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

'''
    text = replace_once(text, anchor, additions + anchor, path)
    text = replace_once(text,
        "    [[nodiscard]] std::unique_ptr<StatementSyntax> parseWhileStatement();\n",
        "    [[nodiscard]] std::unique_ptr<StatementSyntax> parseWhileStatement();\n"
        "    [[nodiscard]] std::unique_ptr<StatementSyntax> parseForStatement();\n"
        "    [[nodiscard]] std::unique_ptr<StatementSyntax> parseForeachStatement();\n"
        "    [[nodiscard]] std::unique_ptr<StatementSyntax> parseDoWhileStatement();\n"
        "    [[nodiscard]] std::unique_ptr<StatementSyntax> parseBreakStatement();\n"
        "    [[nodiscard]] std::unique_ptr<StatementSyntax> parseContinueStatement();\n"
        "    [[nodiscard]] std::unique_ptr<StatementSyntax> parseSwitchStatement();\n", path)
    write(path, text)

# Syntax facts
path = "src/syntax/SyntaxFacts.cpp"
text = read(path)
if "RS_KIND(ForKeyword)" not in text:
    text = replace_once(text,
        "        RS_KIND(WhileKeyword);\n        RS_KIND(ClassKeyword);",
        "        RS_KIND(WhileKeyword);\n"
        "        RS_KIND(ForKeyword);\n"
        "        RS_KIND(ForeachKeyword);\n"
        "        RS_KIND(InKeyword);\n"
        "        RS_KIND(DoKeyword);\n"
        "        RS_KIND(BreakKeyword);\n"
        "        RS_KIND(ContinueKeyword);\n"
        "        RS_KIND(SwitchKeyword);\n"
        "        RS_KIND(CaseKeyword);\n"
        "        RS_KIND(DefaultKeyword);\n"
        "        RS_KIND(ClassKeyword);", path)
    text = replace_once(text,
        "        RS_KIND(WhileStatement);\n        RS_KIND(VariableDeclarationStatement);",
        "        RS_KIND(WhileStatement);\n"
        "        RS_KIND(ForStatement);\n"
        "        RS_KIND(ForeachStatement);\n"
        "        RS_KIND(DoWhileStatement);\n"
        "        RS_KIND(BreakStatement);\n"
        "        RS_KIND(ContinueStatement);\n"
        "        RS_KIND(SwitchSection);\n"
        "        RS_KIND(SwitchStatement);\n"
        "        RS_KIND(VariableDeclarationStatement);", path)
    text = replace_once(text,
        '        {"while", SyntaxKind::WhileKeyword},\n        {"class", SyntaxKind::ClassKeyword},',
        '        {"while", SyntaxKind::WhileKeyword},\n'
        '        {"for", SyntaxKind::ForKeyword},\n'
        '        {"foreach", SyntaxKind::ForeachKeyword},\n'
        '        {"in", SyntaxKind::InKeyword},\n'
        '        {"do", SyntaxKind::DoKeyword},\n'
        '        {"break", SyntaxKind::BreakKeyword},\n'
        '        {"continue", SyntaxKind::ContinueKeyword},\n'
        '        {"switch", SyntaxKind::SwitchKeyword},\n'
        '        {"case", SyntaxKind::CaseKeyword},\n'
        '        {"default", SyntaxKind::DefaultKeyword},\n'
        '        {"class", SyntaxKind::ClassKeyword},', path)
    write(path, text)

# Syntax spans
path = "src/syntax/SyntaxNodes.cpp"
text = read(path)
if "ForStatementSyntax::span" not in text:
    anchor = "text::TextSpan VariableDeclarationStatementSyntax::span() const noexcept {"
    additions = r'''text::TextSpan ForStatementSyntax::span() const noexcept {
    return combine(forKeyword.span, body ? body->span() : closeParenToken.span);
}

text::TextSpan ForeachStatementSyntax::span() const noexcept {
    return combine(foreachKeyword.span, body ? body->span() : closeParenToken.span);
}

text::TextSpan DoWhileStatementSyntax::span() const noexcept {
    return combine(doKeyword.span, semicolonToken.span);
}

text::TextSpan BreakStatementSyntax::span() const noexcept {
    return combine(breakKeyword.span, semicolonToken.span);
}

text::TextSpan ContinueStatementSyntax::span() const noexcept {
    return combine(continueKeyword.span, semicolonToken.span);
}

text::TextSpan SwitchSectionSyntax::span() const noexcept {
    const auto start = caseKeyword ? caseKeyword->span : defaultKeyword->span;
    return statements.empty() ? combine(start, colonToken.span)
                              : combine(start, statements.back()->span());
}

text::TextSpan SwitchStatementSyntax::span() const noexcept {
    return combine(switchKeyword.span, closeBraceToken.span);
}

'''
    text = replace_once(text, anchor, additions + anchor, path)
    write(path, text)

# Parser
path = "src/syntax/Parser.cpp"
text = read(path)
if "parseForStatement" not in text:
    text = replace_once(text,
        "    case SyntaxKind::WhileKeyword:\n        return parseWhileStatement();\n",
        "    case SyntaxKind::WhileKeyword:\n        return parseWhileStatement();\n"
        "    case SyntaxKind::ForKeyword:\n        return parseForStatement();\n"
        "    case SyntaxKind::ForeachKeyword:\n        return parseForeachStatement();\n"
        "    case SyntaxKind::DoKeyword:\n        return parseDoWhileStatement();\n"
        "    case SyntaxKind::BreakKeyword:\n        return parseBreakStatement();\n"
        "    case SyntaxKind::ContinueKeyword:\n        return parseContinueStatement();\n"
        "    case SyntaxKind::SwitchKeyword:\n        return parseSwitchStatement();\n", path)
    anchor = "std::unique_ptr<StatementSyntax> Parser::parseVariableDeclarationStatement() {"
    additions = r'''std::unique_ptr<StatementSyntax> Parser::parseForStatement() {
    auto result = std::make_unique<ForStatementSyntax>();
    result->forKeyword = match(SyntaxKind::ForKeyword);
    result->openParenToken = match(SyntaxKind::OpenParenToken);
    if (current().kind == SyntaxKind::SemicolonToken) {
        result->firstSemicolonToken = nextToken();
    } else if (isVariableDeclarationStart()) {
        result->initializer = parseVariableDeclarationStatement();
    } else {
        result->initializer = parseExpressionStatement();
    }
    if (current().kind != SyntaxKind::SemicolonToken) {
        result->condition = parseExpression();
    }
    result->secondSemicolonToken = match(SyntaxKind::SemicolonToken);
    if (current().kind != SyntaxKind::CloseParenToken) {
        result->increment = parseExpression();
    }
    result->closeParenToken = match(SyntaxKind::CloseParenToken);
    result->body = parseStatement();
    return result;
}

std::unique_ptr<StatementSyntax> Parser::parseForeachStatement() {
    auto result = std::make_unique<ForeachStatementSyntax>();
    result->foreachKeyword = match(SyntaxKind::ForeachKeyword);
    result->openParenToken = match(SyntaxKind::OpenParenToken);
    result->type = parseType();
    result->identifierToken = match(SyntaxKind::IdentifierToken);
    result->inKeyword = match(SyntaxKind::InKeyword);
    result->collection = parseExpression();
    result->closeParenToken = match(SyntaxKind::CloseParenToken);
    result->body = parseStatement();
    return result;
}

std::unique_ptr<StatementSyntax> Parser::parseDoWhileStatement() {
    auto result = std::make_unique<DoWhileStatementSyntax>();
    result->doKeyword = match(SyntaxKind::DoKeyword);
    result->body = parseStatement();
    result->whileKeyword = match(SyntaxKind::WhileKeyword);
    result->openParenToken = match(SyntaxKind::OpenParenToken);
    result->condition = parseExpression();
    result->closeParenToken = match(SyntaxKind::CloseParenToken);
    result->semicolonToken = match(SyntaxKind::SemicolonToken);
    return result;
}

std::unique_ptr<StatementSyntax> Parser::parseBreakStatement() {
    auto result = std::make_unique<BreakStatementSyntax>();
    result->breakKeyword = match(SyntaxKind::BreakKeyword);
    result->semicolonToken = match(SyntaxKind::SemicolonToken);
    return result;
}

std::unique_ptr<StatementSyntax> Parser::parseContinueStatement() {
    auto result = std::make_unique<ContinueStatementSyntax>();
    result->continueKeyword = match(SyntaxKind::ContinueKeyword);
    result->semicolonToken = match(SyntaxKind::SemicolonToken);
    return result;
}

std::unique_ptr<StatementSyntax> Parser::parseSwitchStatement() {
    auto result = std::make_unique<SwitchStatementSyntax>();
    result->switchKeyword = match(SyntaxKind::SwitchKeyword);
    result->openParenToken = match(SyntaxKind::OpenParenToken);
    result->expression = parseExpression();
    result->closeParenToken = match(SyntaxKind::CloseParenToken);
    result->openBraceToken = match(SyntaxKind::OpenBraceToken);
    bool defaultSeen = false;
    while (current().kind != SyntaxKind::CloseBraceToken &&
           current().kind != SyntaxKind::EndOfFileToken) {
        SwitchSectionSyntax section;
        if (current().kind == SyntaxKind::CaseKeyword) {
            section.caseKeyword = nextToken();
            section.label = parseExpression();
        } else if (current().kind == SyntaxKind::DefaultKeyword) {
            section.defaultKeyword = nextToken();
            if (defaultSeen) {
                diagnostics_.report("RS1110", "switch statement has more than one default label", section.defaultKeyword->span);
            }
            defaultSeen = true;
        } else {
            diagnostics_.report("RS1109", "expected case or default label", current().span);
            nextToken();
            continue;
        }
        section.colonToken = match(SyntaxKind::ColonToken);
        while (current().kind != SyntaxKind::CaseKeyword &&
               current().kind != SyntaxKind::DefaultKeyword &&
               current().kind != SyntaxKind::CloseBraceToken &&
               current().kind != SyntaxKind::EndOfFileToken) {
            section.statements.push_back(parseStatement());
        }
        result->sections.push_back(std::move(section));
    }
    result->closeBraceToken = match(SyntaxKind::CloseBraceToken);
    return result;
}

'''
    text = replace_once(text, anchor, additions + anchor, path)
    write(path, text)

# ---------------------------------------------------------------------------
# Semantic model and binder
# ---------------------------------------------------------------------------
path = "include/realscript/semantic/Semantic.h"
text = read(path)
if "BoundForStatement" not in text:
    text = replace_once(text,
        "    WhileStatement,\n    VariableDeclarationStatement,",
        "    WhileStatement,\n    ForStatement,\n    ForeachStatement,\n    DoWhileStatement,\n    BreakStatement,\n    ContinueStatement,\n    SwitchStatement,\n    VariableDeclarationStatement,", path)
    anchor = "struct BoundVariableDeclarationStatement final : BoundStatement {"
    additions = r'''struct BoundForStatement final : BoundStatement {
    std::unique_ptr<BoundStatement> initializer;
    std::unique_ptr<BoundExpression> condition;
    std::unique_ptr<BoundExpression> increment;
    std::unique_ptr<BoundStatement> body;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::ForStatement;
    }
};

struct BoundForeachStatement final : BoundStatement {
    VariableSymbol collectionVariable;
    VariableSymbol indexVariable;
    VariableSymbol iterationVariable;
    std::unique_ptr<BoundExpression> collection;
    std::unique_ptr<BoundExpression> count;
    std::unique_ptr<BoundExpression> element;
    std::unique_ptr<BoundStatement> body;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::ForeachStatement;
    }
};

struct BoundDoWhileStatement final : BoundStatement {
    std::unique_ptr<BoundStatement> body;
    std::unique_ptr<BoundExpression> condition;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::DoWhileStatement;
    }
};

struct BoundBreakStatement final : BoundStatement {
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::BreakStatement;
    }
};

struct BoundContinueStatement final : BoundStatement {
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::ContinueStatement;
    }
};

struct BoundSwitchSection {
    std::unique_ptr<BoundExpression> label;
    std::vector<std::unique_ptr<BoundStatement>> statements;
    text::TextSpan span;
};

struct BoundSwitchStatement final : BoundStatement {
    std::unique_ptr<BoundExpression> expression;
    std::vector<BoundSwitchSection> sections;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::SwitchStatement;
    }
};

'''
    text = replace_once(text, anchor, additions + anchor, path)
    text = replace_once(text,
        "    [[nodiscard]] std::unique_ptr<BoundStatement> bindWhileStatement(\n        const syntax::WhileStatementSyntax& syntax);\n",
        "    [[nodiscard]] std::unique_ptr<BoundStatement> bindWhileStatement(\n        const syntax::WhileStatementSyntax& syntax);\n"
        "    [[nodiscard]] std::unique_ptr<BoundStatement> bindForStatement(\n        const syntax::ForStatementSyntax& syntax);\n"
        "    [[nodiscard]] std::unique_ptr<BoundStatement> bindForeachStatement(\n        const syntax::ForeachStatementSyntax& syntax);\n"
        "    [[nodiscard]] std::unique_ptr<BoundStatement> bindDoWhileStatement(\n        const syntax::DoWhileStatementSyntax& syntax);\n"
        "    [[nodiscard]] std::unique_ptr<BoundStatement> bindBreakStatement(\n        const syntax::BreakStatementSyntax& syntax);\n"
        "    [[nodiscard]] std::unique_ptr<BoundStatement> bindContinueStatement(\n        const syntax::ContinueStatementSyntax& syntax);\n"
        "    [[nodiscard]] std::unique_ptr<BoundStatement> bindSwitchStatement(\n        const syntax::SwitchStatementSyntax& syntax);\n", path)
    text = replace_once(text,
        "    std::vector<SymbolOccurrence> occurrences_;\n",
        "    std::vector<SymbolOccurrence> occurrences_;\n"
        "    std::size_t loopDepth_ = 0;\n"
        "    std::size_t breakableDepth_ = 0;\n", path)
    write(path, text)

# Binder implementation
path = "src/semantic/SemanticBinding.cpp"
text = read(path)
if "Binder::bindForStatement" not in text:
    text = replace_once(text,
        "    case syntax::SyntaxKind::WhileStatement:\n        return bindWhileStatement(\n            static_cast<const syntax::WhileStatementSyntax&>(syntaxTree));\n",
        "    case syntax::SyntaxKind::WhileStatement:\n        return bindWhileStatement(\n            static_cast<const syntax::WhileStatementSyntax&>(syntaxTree));\n"
        "    case syntax::SyntaxKind::ForStatement:\n        return bindForStatement(\n            static_cast<const syntax::ForStatementSyntax&>(syntaxTree));\n"
        "    case syntax::SyntaxKind::ForeachStatement:\n        return bindForeachStatement(\n            static_cast<const syntax::ForeachStatementSyntax&>(syntaxTree));\n"
        "    case syntax::SyntaxKind::DoWhileStatement:\n        return bindDoWhileStatement(\n            static_cast<const syntax::DoWhileStatementSyntax&>(syntaxTree));\n"
        "    case syntax::SyntaxKind::BreakStatement:\n        return bindBreakStatement(\n            static_cast<const syntax::BreakStatementSyntax&>(syntaxTree));\n"
        "    case syntax::SyntaxKind::ContinueStatement:\n        return bindContinueStatement(\n            static_cast<const syntax::ContinueStatementSyntax&>(syntaxTree));\n"
        "    case syntax::SyntaxKind::SwitchStatement:\n        return bindSwitchStatement(\n            static_cast<const syntax::SwitchStatementSyntax&>(syntaxTree));\n", path)
    anchor = "std::unique_ptr<BoundStatement> Binder::bindVariableDeclaration("
    additions = r'''std::unique_ptr<BoundStatement> Binder::bindForStatement(
    const syntax::ForStatementSyntax& syntaxTree) {
    pushScope(syntaxTree.span());
    auto result = std::make_unique<BoundForStatement>();
    result->span = syntaxTree.span();
    if (syntaxTree.initializer) result->initializer = bindStatement(*syntaxTree.initializer);
    if (syntaxTree.condition) {
        result->condition = convertExpression(
            bindExpression(*syntaxTree.condition), PrimitiveType::Bool,
            syntaxTree.condition->span(), "for condition");
    } else {
        auto literal = std::make_unique<BoundLiteralExpression>();
        literal->type = PrimitiveType::Bool;
        literal->value = true;
        literal->span = syntaxTree.forKeyword.span;
        result->condition = std::move(literal);
    }
    if (syntaxTree.increment) result->increment = bindExpression(*syntaxTree.increment);
    ++loopDepth_;
    ++breakableDepth_;
    result->body = bindEmbeddedStatement(*syntaxTree.body);
    --breakableDepth_;
    --loopDepth_;
    popScope();
    return result;
}

std::unique_ptr<BoundStatement> Binder::bindForeachStatement(
    const syntax::ForeachStatementSyntax& syntaxTree) {
    auto result = std::make_unique<BoundForeachStatement>();
    result->span = syntaxTree.span();
    result->collection = bindExpression(*syntaxTree.collection);

    PrimitiveType elementType = PrimitiveType::Error;
    std::string elementTypeName;
    const TypeSymbol* collectionType = nullptr;
    const FunctionSymbol* countMethod = nullptr;
    const FunctionSymbol* getMethod = nullptr;
    if (result->collection->type == PrimitiveType::Array) {
        if (!decodeArrayTypeName(result->collection->typeName, elementType, elementTypeName)) {
            diagnostics_.report("RS2210", "foreach array element type is invalid", syntaxTree.collection->span());
        }
    } else if (result->collection->type == PrimitiveType::Object) {
        const auto found = visibleTypes_.find(result->collection->typeName);
        if (found != visibleTypes_.end()) collectionType = &found->second;
        if (collectionType) {
            for (const auto& method : collectionType->methods) {
                const auto visibleArity = method.parameters.size() -
                    ((method.method && !method.staticMethod) ? 1u : 0u);
                if (!method.staticMethod && method.name == "Count" && visibleArity == 0 &&
                    method.returnType == PrimitiveType::Int) countMethod = &method;
                if (!method.staticMethod && method.name == "Get" && visibleArity == 1 &&
                    method.parameters.back().type == PrimitiveType::Int) getMethod = &method;
            }
        }
        if (!countMethod || !getMethod) {
            diagnostics_.report("RS2211", "foreach collection must provide Count() and Get(int)", syntaxTree.collection->span());
        } else {
            elementType = getMethod->returnType;
            elementTypeName = getMethod->returnTypeName;
        }
    } else {
        diagnostics_.report("RS2211", "foreach requires an array or indexed collection", syntaxTree.collection->span());
    }

    pushScope(syntaxTree.span());
    result->collectionVariable.name = "$foreach_collection_" + std::to_string(nextVariableIndex_);
    result->collectionVariable.type = result->collection->type;
    result->collectionVariable.typeName = result->collection->typeName;
    result->collectionVariable.index = nextVariableIndex_++;
    result->collectionVariable.id = stableTypeId(std::to_string(currentFunctionId_) +
        "::local:" + std::to_string(result->collectionVariable.index) + ":" +
        result->collectionVariable.name);
    (void)declareVariable(result->collectionVariable, {});

    result->indexVariable.name = "$foreach_index_" + std::to_string(nextVariableIndex_);
    result->indexVariable.type = PrimitiveType::Int;
    result->indexVariable.index = nextVariableIndex_++;
    result->indexVariable.id = stableTypeId(std::to_string(currentFunctionId_) +
        "::local:" + std::to_string(result->indexVariable.index) + ":" +
        result->indexVariable.name);
    (void)declareVariable(result->indexVariable, {});

    std::string declaredTypeName;
    result->iterationVariable.name = syntaxTree.identifierToken.text;
    result->iterationVariable.type = bindType(syntaxTree.type, false, &declaredTypeName);
    result->iterationVariable.typeName = declaredTypeName;
    result->iterationVariable.index = nextVariableIndex_++;
    result->iterationVariable.declarationSpan = syntaxTree.identifierToken.span;
    result->iterationVariable.id = stableTypeId(std::to_string(currentFunctionId_) +
        "::local:" + std::to_string(result->iterationVariable.index) + ":" +
        result->iterationVariable.name);
    (void)declareVariable(result->iterationVariable, syntaxTree.identifierToken.span);

    const auto variableExpression = [](const VariableSymbol& variable, text::TextSpan span) {
        auto value = std::make_unique<BoundVariableExpression>();
        value->type = variable.type;
        value->typeName = variable.typeName;
        value->variable = variable;
        value->span = span;
        return value;
    };

    if (result->collection->type == PrimitiveType::Array) {
        auto count = std::make_unique<BoundArrayLengthExpression>();
        count->type = PrimitiveType::Int;
        count->span = syntaxTree.collection->span();
        count->receiver = variableExpression(result->collectionVariable, syntaxTree.collection->span());
        result->count = std::move(count);

        auto element = std::make_unique<BoundElementAccessExpression>();
        element->type = elementType;
        element->typeName = elementTypeName;
        element->elementType = elementType;
        element->elementTypeName = elementTypeName;
        element->span = syntaxTree.identifierToken.span;
        element->receiver = variableExpression(result->collectionVariable, syntaxTree.collection->span());
        element->index = variableExpression(result->indexVariable, syntaxTree.identifierToken.span);
        result->element = std::move(element);
    } else if (countMethod && getMethod) {
        auto count = std::make_unique<BoundCallExpression>();
        count->type = countMethod->returnType;
        count->typeName = countMethod->returnTypeName;
        count->function = *countMethod;
        count->span = syntaxTree.collection->span();
        count->arguments.push_back(variableExpression(result->collectionVariable, syntaxTree.collection->span()));
        result->count = std::move(count);

        auto element = std::make_unique<BoundCallExpression>();
        element->type = getMethod->returnType;
        element->typeName = getMethod->returnTypeName;
        element->function = *getMethod;
        element->span = syntaxTree.identifierToken.span;
        element->arguments.push_back(variableExpression(result->collectionVariable, syntaxTree.collection->span()));
        element->arguments.push_back(variableExpression(result->indexVariable, syntaxTree.identifierToken.span));
        result->element = std::move(element);
    } else {
        result->count = makeError(syntaxTree.collection->span());
        result->element = makeError(syntaxTree.identifierToken.span);
    }

    result->element = convertExpression(
        std::move(result->element), result->iterationVariable.type,
        syntaxTree.identifierToken.span, "foreach element",
        result->iterationVariable.typeName);

    ++loopDepth_;
    ++breakableDepth_;
    result->body = bindEmbeddedStatement(*syntaxTree.body);
    --breakableDepth_;
    --loopDepth_;
    popScope();
    return result;
}

std::unique_ptr<BoundStatement> Binder::bindDoWhileStatement(
    const syntax::DoWhileStatementSyntax& syntaxTree) {
    auto result = std::make_unique<BoundDoWhileStatement>();
    result->span = syntaxTree.span();
    ++loopDepth_;
    ++breakableDepth_;
    result->body = bindEmbeddedStatement(*syntaxTree.body);
    --breakableDepth_;
    --loopDepth_;
    result->condition = convertExpression(
        bindExpression(*syntaxTree.condition), PrimitiveType::Bool,
        syntaxTree.condition->span(), "do/while condition");
    return result;
}

std::unique_ptr<BoundStatement> Binder::bindBreakStatement(
    const syntax::BreakStatementSyntax& syntaxTree) {
    auto result = std::make_unique<BoundBreakStatement>();
    result->span = syntaxTree.span();
    if (breakableDepth_ == 0) {
        diagnostics_.report("RS2212", "break is not inside a loop or switch", syntaxTree.span());
    }
    return result;
}

std::unique_ptr<BoundStatement> Binder::bindContinueStatement(
    const syntax::ContinueStatementSyntax& syntaxTree) {
    auto result = std::make_unique<BoundContinueStatement>();
    result->span = syntaxTree.span();
    if (loopDepth_ == 0) {
        diagnostics_.report("RS2213", "continue is not inside a loop", syntaxTree.span());
    }
    return result;
}

std::unique_ptr<BoundStatement> Binder::bindSwitchStatement(
    const syntax::SwitchStatementSyntax& syntaxTree) {
    auto result = std::make_unique<BoundSwitchStatement>();
    result->span = syntaxTree.span();
    result->expression = bindExpression(*syntaxTree.expression);
    ++breakableDepth_;
    for (const auto& sourceSection : syntaxTree.sections) {
        BoundSwitchSection section;
        section.span = sourceSection.span();
        if (sourceSection.label) {
            section.label = convertExpression(
                bindExpression(*sourceSection.label), result->expression->type,
                sourceSection.label->span(), "switch case",
                result->expression->typeName);
        }
        pushScope(sourceSection.span());
        for (const auto& sourceStatement : sourceSection.statements) {
            section.statements.push_back(bindStatement(*sourceStatement));
        }
        popScope();
        result->sections.push_back(std::move(section));
    }
    --breakableDepth_;
    return result;
}

'''
    text = replace_once(text, anchor, additions + anchor, path)
    # reset context counters for every function
    text = replace_once(text,
        "    currentFunctionId_ = input.symbol.id;\n",
        "    currentFunctionId_ = input.symbol.id;\n    loopDepth_ = 0;\n    breakableDepth_ = 0;\n", path)
    write(path, text)

# Flow analysis is rewritten as a compact control-aware implementation.
write("src/semantic/FlowAnalysis.cpp", r'''#include "FlowAnalysis.h"

#include <unordered_set>
#include <utility>

namespace realscript::semantic {
namespace {

using AssignedSet = std::unordered_set<std::size_t>;

enum class ExitKind { None, Return, Break, Continue };

AssignedSet intersectAssigned(const AssignedSet& left, const AssignedSet& right) {
    AssignedSet result;
    const auto& smaller = left.size() <= right.size() ? left : right;
    const auto& larger = left.size() <= right.size() ? right : left;
    for (const auto value : smaller) if (larger.find(value) != larger.end()) result.insert(value);
    return result;
}

bool isLiteralTrue(const BoundExpression& expression) {
    if (expression.kind() != BoundNodeKind::LiteralExpression ||
        expression.type != PrimitiveType::Bool) return false;
    const auto& literal = static_cast<const BoundLiteralExpression&>(expression);
    return std::holds_alternative<bool>(literal.value) && std::get<bool>(literal.value);
}

bool containsBreakForCurrentTarget(const BoundStatement& statement, std::size_t nestedBreakables = 0) {
    switch (statement.kind()) {
    case BoundNodeKind::BreakStatement:
        return nestedBreakables == 0;
    case BoundNodeKind::BlockStatement:
        for (const auto& child : static_cast<const BoundBlockStatement&>(statement).statements)
            if (containsBreakForCurrentTarget(*child, nestedBreakables)) return true;
        return false;
    case BoundNodeKind::IfStatement: {
        const auto& value = static_cast<const BoundIfStatement&>(statement);
        return containsBreakForCurrentTarget(*value.thenStatement, nestedBreakables) ||
            (value.elseStatement && containsBreakForCurrentTarget(*value.elseStatement, nestedBreakables));
    }
    case BoundNodeKind::WhileStatement:
    case BoundNodeKind::ForStatement:
    case BoundNodeKind::ForeachStatement:
    case BoundNodeKind::DoWhileStatement:
    case BoundNodeKind::SwitchStatement:
        return false;
    default:
        return false;
    }
}

class FlowAnalyzer {
public:
    explicit FlowAnalyzer(diagnostics::DiagnosticBag& diagnostics) : diagnostics_(diagnostics) {}

    bool canReachFunctionEnd(const BoundFunction& function) {
        State state;
        for (const auto& parameter : function.symbol.parameters) state.assigned.insert(parameter.index);
        return analyzeStatement(*function.body, std::move(state)).exit == ExitKind::None;
    }

private:
    struct State { AssignedSet assigned; ExitKind exit = ExitKind::None; };

    void analyzeExpression(const BoundExpression& expression, AssignedSet& assigned) {
        switch (expression.kind()) {
        case BoundNodeKind::VariableExpression: {
            const auto& variable = static_cast<const BoundVariableExpression&>(expression).variable;
            if (assigned.find(variable.index) == assigned.end())
                diagnostics_.report("RS2300", "variable '" + variable.name + "' is used before it is definitely assigned", expression.span);
            return;
        }
        case BoundNodeKind::AssignmentExpression: {
            const auto& value = static_cast<const BoundAssignmentExpression&>(expression);
            analyzeExpression(*value.expression, assigned);
            assigned.insert(value.variable.index);
            return;
        }
        case BoundNodeKind::ConversionExpression:
            analyzeExpression(*static_cast<const BoundConversionExpression&>(expression).expression, assigned); return;
        case BoundNodeKind::CallExpression:
            for (const auto& argument : static_cast<const BoundCallExpression&>(expression).arguments)
                analyzeExpression(*argument, assigned);
            return;
        case BoundNodeKind::UnaryExpression:
            analyzeExpression(*static_cast<const BoundUnaryExpression&>(expression).operand, assigned); return;
        case BoundNodeKind::BinaryExpression: {
            const auto& value = static_cast<const BoundBinaryExpression&>(expression);
            analyzeExpression(*value.left, assigned);
            if (value.operatorKind == BoundBinaryOperatorKind::LogicalAnd ||
                value.operatorKind == BoundBinaryOperatorKind::LogicalOr) {
                auto copy = assigned; analyzeExpression(*value.right, copy);
            } else analyzeExpression(*value.right, assigned);
            return;
        }
        case BoundNodeKind::MemberAccessExpression:
            analyzeExpression(*static_cast<const BoundMemberAccessExpression&>(expression).receiver, assigned); return;
        case BoundNodeKind::MemberAssignmentExpression: {
            const auto& value = static_cast<const BoundMemberAssignmentExpression&>(expression);
            analyzeExpression(*value.receiver, assigned); analyzeExpression(*value.expression, assigned); return;
        }
        case BoundNodeKind::ArrayLengthExpression:
            analyzeExpression(*static_cast<const BoundArrayLengthExpression&>(expression).receiver, assigned); return;
        case BoundNodeKind::ElementAccessExpression: {
            const auto& value = static_cast<const BoundElementAccessExpression&>(expression);
            analyzeExpression(*value.receiver, assigned); analyzeExpression(*value.index, assigned); return;
        }
        case BoundNodeKind::ElementAssignmentExpression: {
            const auto& value = static_cast<const BoundElementAssignmentExpression&>(expression);
            analyzeExpression(*value.receiver, assigned); analyzeExpression(*value.index, assigned);
            analyzeExpression(*value.expression, assigned); return;
        }
        default: return;
        }
    }

    State analyzeBlock(const std::vector<std::unique_ptr<BoundStatement>>& statements, State state) {
        for (const auto& child : statements) {
            if (state.exit != ExitKind::None) break;
            state = analyzeStatement(*child, std::move(state));
        }
        return state;
    }

    State analyzeStatement(const BoundStatement& statement, State state) {
        if (state.exit != ExitKind::None) return state;
        switch (statement.kind()) {
        case BoundNodeKind::BlockStatement:
            return analyzeBlock(static_cast<const BoundBlockStatement&>(statement).statements, std::move(state));
        case BoundNodeKind::ReturnStatement: {
            const auto& value = static_cast<const BoundReturnStatement&>(statement);
            if (value.expression) analyzeExpression(*value.expression, state.assigned);
            state.exit = ExitKind::Return; return state;
        }
        case BoundNodeKind::BreakStatement: state.exit = ExitKind::Break; return state;
        case BoundNodeKind::ContinueStatement: state.exit = ExitKind::Continue; return state;
        case BoundNodeKind::VariableDeclarationStatement: {
            const auto& value = static_cast<const BoundVariableDeclarationStatement&>(statement);
            if (value.initializer) { analyzeExpression(*value.initializer, state.assigned); state.assigned.insert(value.variable.index); }
            else state.assigned.erase(value.variable.index);
            return state;
        }
        case BoundNodeKind::ExpressionStatement:
            analyzeExpression(*static_cast<const BoundExpressionStatement&>(statement).expression, state.assigned); return state;
        case BoundNodeKind::IfStatement: {
            const auto& value = static_cast<const BoundIfStatement&>(statement);
            analyzeExpression(*value.condition, state.assigned);
            auto left = analyzeStatement(*value.thenStatement, {state.assigned, ExitKind::None});
            auto right = value.elseStatement ? analyzeStatement(*value.elseStatement, {state.assigned, ExitKind::None})
                                             : State{state.assigned, ExitKind::None};
            if (left.exit == right.exit && left.exit != ExitKind::None) return {{}, left.exit};
            if (left.exit == ExitKind::None && right.exit == ExitKind::None)
                return {intersectAssigned(left.assigned, right.assigned), ExitKind::None};
            return left.exit == ExitKind::None ? left : right;
        }
        case BoundNodeKind::WhileStatement: {
            const auto& value = static_cast<const BoundWhileStatement&>(statement);
            analyzeExpression(*value.condition, state.assigned);
            (void)analyzeStatement(*value.body, {state.assigned, ExitKind::None});
            if (isLiteralTrue(*value.condition) && !containsBreakForCurrentTarget(*value.body)) state.exit = ExitKind::Return;
            return state;
        }
        case BoundNodeKind::ForStatement: {
            const auto& value = static_cast<const BoundForStatement&>(statement);
            if (value.initializer) state = analyzeStatement(*value.initializer, std::move(state));
            if (state.exit != ExitKind::None) return state;
            analyzeExpression(*value.condition, state.assigned);
            auto body = analyzeStatement(*value.body, {state.assigned, ExitKind::None});
            if (value.increment && body.exit != ExitKind::Return) analyzeExpression(*value.increment, body.assigned);
            if (isLiteralTrue(*value.condition) && !containsBreakForCurrentTarget(*value.body)) state.exit = ExitKind::Return;
            return state;
        }
        case BoundNodeKind::ForeachStatement: {
            const auto& value = static_cast<const BoundForeachStatement&>(statement);
            analyzeExpression(*value.collection, state.assigned);
            state.assigned.insert(value.collectionVariable.index);
            state.assigned.insert(value.indexVariable.index);
            auto bodyAssigned = state.assigned;
            analyzeExpression(*value.count, bodyAssigned);
            analyzeExpression(*value.element, bodyAssigned);
            bodyAssigned.insert(value.iterationVariable.index);
            (void)analyzeStatement(*value.body, {std::move(bodyAssigned), ExitKind::None});
            return state;
        }
        case BoundNodeKind::DoWhileStatement: {
            const auto& value = static_cast<const BoundDoWhileStatement&>(statement);
            auto body = analyzeStatement(*value.body, {state.assigned, ExitKind::None});
            if (body.exit == ExitKind::Return) return body;
            analyzeExpression(*value.condition, body.assigned);
            if (isLiteralTrue(*value.condition) && !containsBreakForCurrentTarget(*value.body)) body.exit = ExitKind::Return;
            else body.exit = ExitKind::None;
            return body;
        }
        case BoundNodeKind::SwitchStatement: {
            const auto& value = static_cast<const BoundSwitchStatement&>(statement);
            analyzeExpression(*value.expression, state.assigned);
            bool hasDefault = false;
            bool hasReachable = false;
            AssignedSet merged;
            bool first = true;
            for (const auto& section : value.sections) {
                if (section.label) analyzeExpression(*section.label, state.assigned); else hasDefault = true;
                auto sectionState = analyzeBlock(section.statements, {state.assigned, ExitKind::None});
                if (sectionState.exit == ExitKind::Break) sectionState.exit = ExitKind::None;
                if (sectionState.exit == ExitKind::None) {
                    hasReachable = true;
                    merged = first ? sectionState.assigned : intersectAssigned(merged, sectionState.assigned);
                    first = false;
                }
            }
            if (!hasDefault) { hasReachable = true; merged = first ? state.assigned : intersectAssigned(merged, state.assigned); }
            return hasReachable ? State{std::move(merged), ExitKind::None} : State{{}, ExitKind::Return};
        }
        default: return state;
        }
    }

    diagnostics::DiagnosticBag& diagnostics_;
};

} // namespace

namespace detail {
bool canReachFunctionEnd(const BoundFunction& function, diagnostics::DiagnosticBag& diagnostics) {
    FlowAnalyzer analyzer(diagnostics);
    return analyzer.canReachFunctionEnd(function);
}
} // namespace detail
} // namespace realscript::semantic
''')

# ---------------------------------------------------------------------------
# MIR native control-flow lowering
# ---------------------------------------------------------------------------
path = "include/realscript/mir/Mir.h"
text = read(path)
if "breakTargets_" not in text:
    text = replace_once(text,
        "    ValueId nextValueId_ = 0;\n",
        "    ValueId nextValueId_ = 0;\n"
        "    std::vector<BlockId> breakTargets_;\n"
        "    std::vector<BlockId> continueTargets_;\n", path)
    write(path, text)

write("src/mir/MirLowerer.cpp", r'''#include "realscript/mir/Mir.h"

#include <stdexcept>

namespace realscript::mir {
namespace {

bool isLiteralTrue(const semantic::BoundExpression& expression) {
    if (expression.kind() != semantic::BoundNodeKind::LiteralExpression ||
        expression.type != semantic::PrimitiveType::Bool) return false;
    const auto& literal = static_cast<const semantic::BoundLiteralExpression&>(expression);
    return std::holds_alternative<bool>(literal.value) && std::get<bool>(literal.value);
}

} // namespace

Module Lowerer::lower(const semantic::SemanticModel& model) {
    Module result; result.name = model.moduleName; result.types = model.types;
    for (const auto& function : model.functions) result.functions.push_back(lowerFunction(function));
    return result;
}

Function Lowerer::lowerFunction(const semantic::BoundFunction& function) {
    Function result;
    result.symbolId = function.symbol.id;
    result.moduleName = function.symbol.moduleName;
    result.name = function.symbol.ownerTypeName.empty() ? function.symbol.name
        : function.symbol.ownerTypeName + "." + function.symbol.name;
    result.returnType = function.symbol.returnType;
    result.debugInfo.sourceName = function.symbol.sourceName;
    result.debugInfo.declaration.span = function.symbol.declarationSpan;
    result.debugInfo.body.span = function.body ? function.body->span : function.symbol.bodySpan;
    result.returnTypeId = semantic::isExactType(function.symbol.returnType)
        ? semantic::stableTypeId(function.symbol.returnTypeName) : 0;
    result.localTypes.assign(function.variableCount, semantic::PrimitiveType::Error);
    result.localTypeIds.assign(function.variableCount, 0);

    for (const auto& variable : function.variables) {
        debug::LocalVariableInfo local;
        local.name = variable.name; local.slot = static_cast<std::uint32_t>(variable.index);
        local.type = variable.type;
        local.typeId = semantic::isExactType(variable.type) ? semantic::stableTypeId(variable.typeName) : 0;
        local.parameter = variable.parameter; local.declaration.span = variable.declarationSpan;
        local.scope.span = variable.scopeSpan.empty() ? (function.body ? function.body->span : function.symbol.bodySpan)
                                                      : variable.scopeSpan;
        result.debugInfo.locals.push_back(std::move(local));
    }
    for (const auto& parameter : function.symbol.parameters) {
        result.parameterTypes.push_back(parameter.type);
        result.parameterTypeIds.push_back(semantic::isExactType(parameter.type)
            ? semantic::stableTypeId(parameter.typeName) : 0);
        result.localTypes.at(parameter.index) = parameter.type;
        result.localTypeIds.at(parameter.index) = semantic::isExactType(parameter.type)
            ? semantic::stableTypeId(parameter.typeName) : 0;
    }

    currentFunction_ = &result; nextValueId_ = 0; breakTargets_.clear(); continueTargets_.clear();
    collectLocalTypes(*function.body);
    const auto entry = createBlock(); setCurrentBlock(entry);
    for (std::size_t i = 0; i < function.symbol.parameters.size(); ++i) {
        const auto& parameter = function.symbol.parameters[i];
        const auto value = emitValue(Opcode::Parameter, parameter.type, {}, {});
        auto& instruction = block(*currentBlockId_).instructions.back();
        instruction.integerImmediate = static_cast<std::int64_t>(i);
        instruction.resultTypeId = semantic::isExactType(parameter.type)
            ? semantic::stableTypeId(parameter.typeName) : 0;
        emitStoreLocal(parameter.index, value, {});
    }
    lowerStatement(*function.body);
    if (hasCurrentBlock() && !currentBlockTerminated() && function.symbol.returnType == semantic::PrimitiveType::Void)
        emitReturn(std::nullopt, function.body->span);

    for (const auto& basicBlock : result.blocks) {
        for (std::size_t i = 0; i < basicBlock.instructions.size(); ++i) {
            const auto& instruction = basicBlock.instructions[i];
            if (instruction.sourceSpan.empty()) continue;
            debug::SequencePoint point; point.blockId = basicBlock.id;
            point.instructionIndex = static_cast<std::uint32_t>(i); point.range.span = instruction.sourceSpan;
            const auto duplicate = !result.debugInfo.sequencePoints.empty() &&
                result.debugInfo.sequencePoints.back().range.span.start == point.range.span.start &&
                result.debugInfo.sequencePoints.back().range.span.length == point.range.span.length;
            if (!duplicate) result.debugInfo.sequencePoints.push_back(std::move(point));
        }
        if (!basicBlock.terminator.sourceSpan.empty()) {
            debug::SequencePoint point; point.blockId = basicBlock.id;
            point.instructionIndex = static_cast<std::uint32_t>(basicBlock.instructions.size());
            point.terminator = true; point.range.span = basicBlock.terminator.sourceSpan;
            const auto duplicate = !result.debugInfo.sequencePoints.empty() &&
                result.debugInfo.sequencePoints.back().range.span.start == point.range.span.start &&
                result.debugInfo.sequencePoints.back().range.span.length == point.range.span.length;
            if (!duplicate) result.debugInfo.sequencePoints.push_back(std::move(point));
        }
    }
    currentFunction_ = nullptr; clearCurrentBlock(); return result;
}

void Lowerer::collectLocalTypes(const semantic::BoundStatement& statement) {
    switch (statement.kind()) {
    case semantic::BoundNodeKind::BlockStatement:
        for (const auto& child : static_cast<const semantic::BoundBlockStatement&>(statement).statements)
            collectLocalTypes(*child);
        return;
    case semantic::BoundNodeKind::VariableDeclarationStatement: {
        const auto& value = static_cast<const semantic::BoundVariableDeclarationStatement&>(statement);
        currentFunction_->localTypes.at(value.variable.index) = value.variable.type;
        currentFunction_->localTypeIds.at(value.variable.index) = semantic::isExactType(value.variable.type)
            ? semantic::stableTypeId(value.variable.typeName) : 0;
        return;
    }
    case semantic::BoundNodeKind::IfStatement: {
        const auto& value = static_cast<const semantic::BoundIfStatement&>(statement);
        collectLocalTypes(*value.thenStatement); if (value.elseStatement) collectLocalTypes(*value.elseStatement); return;
    }
    case semantic::BoundNodeKind::WhileStatement:
        collectLocalTypes(*static_cast<const semantic::BoundWhileStatement&>(statement).body); return;
    case semantic::BoundNodeKind::ForStatement: {
        const auto& value = static_cast<const semantic::BoundForStatement&>(statement);
        if (value.initializer) collectLocalTypes(*value.initializer); collectLocalTypes(*value.body); return;
    }
    case semantic::BoundNodeKind::ForeachStatement: {
        const auto& value = static_cast<const semantic::BoundForeachStatement&>(statement);
        for (const auto* variable : {&value.collectionVariable, &value.indexVariable, &value.iterationVariable}) {
            currentFunction_->localTypes.at(variable->index) = variable->type;
            currentFunction_->localTypeIds.at(variable->index) = semantic::isExactType(variable->type)
                ? semantic::stableTypeId(variable->typeName) : 0;
        }
        collectLocalTypes(*value.body); return;
    }
    case semantic::BoundNodeKind::DoWhileStatement:
        collectLocalTypes(*static_cast<const semantic::BoundDoWhileStatement&>(statement).body); return;
    case semantic::BoundNodeKind::SwitchStatement:
        for (const auto& section : static_cast<const semantic::BoundSwitchStatement&>(statement).sections)
            for (const auto& child : section.statements) collectLocalTypes(*child);
        return;
    default: return;
    }
}

void Lowerer::lowerStatement(const semantic::BoundStatement& statement) {
    if (!hasCurrentBlock() || currentBlockTerminated()) return;
    const auto emitLoadLocal = [&](const semantic::VariableSymbol& variable, text::TextSpan span) {
        const auto value = emitValue(Opcode::LoadLocal, variable.type, {}, span);
        auto& instruction = block(*currentBlockId_).instructions.back();
        instruction.localIndex = variable.index;
        instruction.resultTypeId = semantic::isExactType(variable.type)
            ? semantic::stableTypeId(variable.typeName) : 0;
        return value;
    };
    const auto emitInt = [&](std::int64_t immediate, text::TextSpan span) {
        const auto value = emitValue(Opcode::ConstantInt, semantic::PrimitiveType::Int, {}, span);
        block(*currentBlockId_).instructions.back().integerImmediate = immediate; return value;
    };

    switch (statement.kind()) {
    case semantic::BoundNodeKind::BlockStatement:
        for (const auto& child : static_cast<const semantic::BoundBlockStatement&>(statement).statements) {
            lowerStatement(*child); if (!hasCurrentBlock() || currentBlockTerminated()) break;
        }
        return;
    case semantic::BoundNodeKind::ReturnStatement: {
        const auto& value = static_cast<const semantic::BoundReturnStatement&>(statement);
        emitReturn(value.expression ? std::optional<ValueId>{lowerExpression(*value.expression)} : std::nullopt, statement.span); return;
    }
    case semantic::BoundNodeKind::BreakStatement:
        if (breakTargets_.empty()) throw std::logic_error("unbound break reached MIR lowering");
        emitJump(breakTargets_.back(), {}, statement.span); return;
    case semantic::BoundNodeKind::ContinueStatement:
        if (continueTargets_.empty()) throw std::logic_error("unbound continue reached MIR lowering");
        emitJump(continueTargets_.back(), {}, statement.span); return;
    case semantic::BoundNodeKind::VariableDeclarationStatement: {
        const auto& value = static_cast<const semantic::BoundVariableDeclarationStatement&>(statement);
        if (value.initializer) emitStoreLocal(value.variable.index, lowerExpression(*value.initializer), statement.span); return;
    }
    case semantic::BoundNodeKind::ExpressionStatement:
        (void)lowerExpression(*static_cast<const semantic::BoundExpressionStatement&>(statement).expression); return;
    case semantic::BoundNodeKind::IfStatement: {
        const auto& value = static_cast<const semantic::BoundIfStatement&>(statement);
        const auto condition = lowerExpression(*value.condition); const auto thenBlock = createBlock();
        if (!value.elseStatement) {
            const auto merge = createBlock(); emitBranch(condition, thenBlock, merge, {}, {}, statement.span);
            setCurrentBlock(thenBlock); lowerStatement(*value.thenStatement);
            if (hasCurrentBlock() && !currentBlockTerminated()) emitJump(merge, {}, value.thenStatement->span);
            setCurrentBlock(merge); return;
        }
        const auto elseBlock = createBlock(); emitBranch(condition, thenBlock, elseBlock, {}, {}, statement.span);
        setCurrentBlock(thenBlock); lowerStatement(*value.thenStatement);
        std::optional<BlockId> thenEnd = hasCurrentBlock() && !currentBlockTerminated() ? currentBlockId_ : std::nullopt;
        setCurrentBlock(elseBlock); lowerStatement(*value.elseStatement);
        std::optional<BlockId> elseEnd = hasCurrentBlock() && !currentBlockTerminated() ? currentBlockId_ : std::nullopt;
        if (!thenEnd && !elseEnd) { clearCurrentBlock(); return; }
        const auto merge = createBlock();
        if (thenEnd) { setCurrentBlock(*thenEnd); emitJump(merge, {}, value.thenStatement->span); }
        if (elseEnd) { setCurrentBlock(*elseEnd); emitJump(merge, {}, value.elseStatement->span); }
        setCurrentBlock(merge); return;
    }
    case semantic::BoundNodeKind::WhileStatement: {
        const auto& value = static_cast<const semantic::BoundWhileStatement&>(statement);
        const auto conditionBlock = createBlock(), bodyBlock = createBlock(), exitBlock = createBlock();
        emitJump(conditionBlock, {}, statement.span); setCurrentBlock(conditionBlock);
        const auto condition = lowerExpression(*value.condition);
        emitBranch(condition, bodyBlock, exitBlock, {}, {}, value.condition->span);
        breakTargets_.push_back(exitBlock); continueTargets_.push_back(conditionBlock);
        setCurrentBlock(bodyBlock); lowerStatement(*value.body);
        continueTargets_.pop_back(); breakTargets_.pop_back();
        if (hasCurrentBlock() && !currentBlockTerminated()) emitJump(conditionBlock, {}, value.body->span);
        setCurrentBlock(exitBlock); return;
    }
    case semantic::BoundNodeKind::ForStatement: {
        const auto& value = static_cast<const semantic::BoundForStatement&>(statement);
        if (value.initializer) lowerStatement(*value.initializer);
        if (!hasCurrentBlock() || currentBlockTerminated()) return;
        const auto conditionBlock = createBlock(), bodyBlock = createBlock(), incrementBlock = createBlock(), exitBlock = createBlock();
        emitJump(conditionBlock, {}, statement.span); setCurrentBlock(conditionBlock);
        emitBranch(lowerExpression(*value.condition), bodyBlock, exitBlock, {}, {}, value.condition->span);
        breakTargets_.push_back(exitBlock); continueTargets_.push_back(incrementBlock);
        setCurrentBlock(bodyBlock); lowerStatement(*value.body);
        if (hasCurrentBlock() && !currentBlockTerminated()) emitJump(incrementBlock, {}, value.body->span);
        setCurrentBlock(incrementBlock); if (value.increment) (void)lowerExpression(*value.increment);
        if (!currentBlockTerminated()) emitJump(conditionBlock, {}, value.increment ? value.increment->span : statement.span);
        continueTargets_.pop_back(); breakTargets_.pop_back(); setCurrentBlock(exitBlock); return;
    }
    case semantic::BoundNodeKind::ForeachStatement: {
        const auto& value = static_cast<const semantic::BoundForeachStatement&>(statement);
        emitStoreLocal(value.collectionVariable.index, lowerExpression(*value.collection), value.collection->span);
        emitStoreLocal(value.indexVariable.index, emitInt(0, statement.span), statement.span);
        const auto conditionBlock = createBlock(), bodyBlock = createBlock(), incrementBlock = createBlock(), exitBlock = createBlock();
        emitJump(conditionBlock, {}, statement.span); setCurrentBlock(conditionBlock);
        const auto index = emitLoadLocal(value.indexVariable, statement.span);
        const auto count = lowerExpression(*value.count);
        const auto condition = emitValue(Opcode::LessInt, semantic::PrimitiveType::Bool, {index, count}, statement.span);
        emitBranch(condition, bodyBlock, exitBlock, {}, {}, statement.span);
        breakTargets_.push_back(exitBlock); continueTargets_.push_back(incrementBlock);
        setCurrentBlock(bodyBlock);
        emitStoreLocal(value.iterationVariable.index, lowerExpression(*value.element), value.iterationVariable.declarationSpan);
        lowerStatement(*value.body);
        if (hasCurrentBlock() && !currentBlockTerminated()) emitJump(incrementBlock, {}, value.body->span);
        setCurrentBlock(incrementBlock);
        const auto currentIndex = emitLoadLocal(value.indexVariable, statement.span);
        const auto nextIndex = emitValue(Opcode::AddInt, semantic::PrimitiveType::Int,
            {currentIndex, emitInt(1, statement.span)}, statement.span);
        emitStoreLocal(value.indexVariable.index, nextIndex, statement.span);
        emitJump(conditionBlock, {}, statement.span);
        continueTargets_.pop_back(); breakTargets_.pop_back(); setCurrentBlock(exitBlock); return;
    }
    case semantic::BoundNodeKind::DoWhileStatement: {
        const auto& value = static_cast<const semantic::BoundDoWhileStatement&>(statement);
        const auto bodyBlock = createBlock(), conditionBlock = createBlock(), exitBlock = createBlock();
        emitJump(bodyBlock, {}, statement.span);
        breakTargets_.push_back(exitBlock); continueTargets_.push_back(conditionBlock);
        setCurrentBlock(bodyBlock); lowerStatement(*value.body);
        if (hasCurrentBlock() && !currentBlockTerminated()) emitJump(conditionBlock, {}, value.body->span);
        setCurrentBlock(conditionBlock);
        emitBranch(lowerExpression(*value.condition), bodyBlock, exitBlock, {}, {}, value.condition->span);
        continueTargets_.pop_back(); breakTargets_.pop_back(); setCurrentBlock(exitBlock); return;
    }
    case semantic::BoundNodeKind::SwitchStatement: {
        const auto& value = static_cast<const semantic::BoundSwitchStatement&>(statement);
        const auto switchValue = lowerExpression(*value.expression); const auto exitBlock = createBlock();
        std::vector<BlockId> sectionBlocks; sectionBlocks.reserve(value.sections.size());
        std::optional<std::size_t> defaultIndex;
        for (std::size_t i = 0; i < value.sections.size(); ++i) {
            sectionBlocks.push_back(createBlock()); if (!value.sections[i].label) defaultIndex = i;
        }
        for (std::size_t i = 0; i < value.sections.size(); ++i) {
            if (!value.sections[i].label) continue;
            const auto nextCheck = createBlock();
            const auto caseValue = lowerExpression(*value.sections[i].label);
            const auto equal = emitValue(Opcode::Equal, semantic::PrimitiveType::Bool,
                {switchValue, caseValue}, value.sections[i].span);
            emitBranch(equal, sectionBlocks[i], nextCheck, {}, {}, value.sections[i].span);
            setCurrentBlock(nextCheck);
        }
        emitJump(defaultIndex ? sectionBlocks[*defaultIndex] : exitBlock, {}, statement.span);
        breakTargets_.push_back(exitBlock);
        for (std::size_t i = 0; i < value.sections.size(); ++i) {
            setCurrentBlock(sectionBlocks[i]);
            for (const auto& child : value.sections[i].statements) {
                lowerStatement(*child); if (!hasCurrentBlock() || currentBlockTerminated()) break;
            }
            if (hasCurrentBlock() && !currentBlockTerminated()) emitJump(exitBlock, {}, value.sections[i].span);
        }
        breakTargets_.pop_back(); setCurrentBlock(exitBlock); return;
    }
    default: throw std::logic_error("unsupported bound statement in MIR lowerer");
    }
}

} // namespace realscript::mir
''')

# Native control flow is now the default; the remaining expansion phases stay enabled.
path = "include/realscript/compiler/LanguageExpansion.h"
text = read(path)
text = text.replace("    bool structuredControlFlow = true;", "    bool structuredControlFlow = false;")
write(path, text)

# Native Phase 18A tests
write("tests/phase18_native_control_flow_tests.cpp", r'''#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/runtime/Runtime.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
void require(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }

std::string diagnosticsText(const realscript::diagnostics::DiagnosticBag& diagnostics) {
    std::string result;
    for (const auto& diagnostic : diagnostics.items()) {
        if (!result.empty()) result.push_back('\n');
        result += diagnostic.code + ": " + diagnostic.message;
    }
    return result;
}

realscript::runtime::ExecutionResult execute(const char* source) {
    realscript::compiler::Compilation compilation({{"phase18.rs", source}});
    const auto build = compilation.build();
    require(!build.diagnostics.hasErrors(), "native source failed to compile:\n" + diagnosticsText(build.diagnostics));
    realscript::bytecode::Lowerer lowerer;
    std::vector<realscript::bytecode::Module> modules;
    for (const auto& module : build.modules) modules.push_back(lowerer.lower(module));
    realscript::runtime::Interpreter interpreter(std::move(modules));
    return interpreter.invoke("Phase18::main");
}

void testNativeControlFlowExecution() {
    const char* source = R"(
module Phase18;
int main()
{
    int total = 0;
    for (int i = 0; i < 5; i = i + 1)
    {
        if (i == 1) continue;
        switch (i)
        {
            case 3:
                break;
            default:
                total = total + i;
                break;
        }
        if (i == 4) break;
    }

    int j = 0;
    do
    {
        j = j + 1;
        if (j < 2) continue;
        total = total + 10;
    }
    while (j < 3);

    int[] values = new int[3];
    values[0] = 1;
    values[1] = 2;
    values[2] = 3;
    foreach (int value in values)
    {
        total = total + value;
    }
    return total;
}
)";
    const auto result = execute(source);
    require(result.succeeded, "native control-flow execution failed: " + result.error.message);
    require(std::get<std::int64_t>(result.value) == 32, "native control-flow result was incorrect");
}

void testNativeInfiniteLoopBreak() {
    const auto result = execute(R"(
module Phase18;
int main()
{
    while (true)
    {
        break;
    }
    return 7;
}
)");
    require(result.succeeded && std::get<std::int64_t>(result.value) == 7,
        "break from an infinite loop did not reach the exit block");
}

void testNativeDiagnostics() {
    realscript::compiler::Compilation compilation({{"invalid.rs", R"(
module Invalid;
int main()
{
    break;
    continue;
    return 0;
}
)"}});
    const auto build = compilation.build();
    require(build.diagnostics.hasErrors(), "invalid native loop control was accepted");
    bool breakFound = false, continueFound = false;
    for (const auto& diagnostic : build.diagnostics.items()) {
        breakFound = breakFound || diagnostic.code == "RS2212";
        continueFound = continueFound || diagnostic.code == "RS2213";
    }
    require(breakFound && continueFound, "native loop-control diagnostics were not preserved");
}

void testNoStructuredSourceRewrite() {
    const auto expansion = realscript::compiler::expandLanguageSource(
        "native.rs", "module Native; int main(){for(int i=0;i<1;i=i+1){}return 1;}");
    require(!expansion.changed, "native for statement still used source expansion");
}
}

int main() {
    int failures = 0;
    const auto run = [&](const char* name, auto test) {
        try { test(); std::cout << "[PASS] " << name << '\n'; }
        catch (const std::exception& error) { ++failures; std::cerr << "[FAIL] " << name << ": " << error.what() << '\n'; }
    };
    run("native structured control flow", testNativeControlFlowExecution);
    run("native infinite loop break", testNativeInfiniteLoopBreak);
    run("native control diagnostics", testNativeDiagnostics);
    run("structured control flow bypasses expansion", testNoStructuredSourceRewrite);
    return failures == 0 ? 0 : 1;
}
''')

path = "CMakeLists.txt"
text = read(path)
if "realscript_phase18_native_control_flow_tests" not in text:
    anchor = "    realscript_add_cpp_test(\n        realscript_phase11_17_language_expansion_tests\n        realscript.phase11-17.language-expansion\n        tests/phase11_17_language_expansion_tests.cpp)\n"
    addition = anchor + "    realscript_add_cpp_test(\n        realscript_phase18_native_control_flow_tests\n        realscript.phase18.native-control-flow\n        tests/phase18_native_control_flow_tests.cpp)\n"
    text = replace_once(text, anchor, addition, path)
    write(path, text)

print("Phase 18A native control-flow patch applied")
