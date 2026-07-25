#include "realscript/syntax/Syntax.h"

namespace realscript::syntax {
namespace {

bool isIdentifierLike(SyntaxKind kind) noexcept {
    return kind == SyntaxKind::IdentifierToken || isPrimitiveTypeKeyword(kind);
}

} // namespace

Parser::Parser(const text::SourceText& source, diagnostics::DiagnosticBag& diagnostics)
    : source_(source), diagnostics_(diagnostics) {
    Lexer lexer(source_, diagnostics_);
    tokens_ = lexer.lexAll();
}

const SyntaxToken& Parser::current() const noexcept {
    return peek(0);
}

const SyntaxToken& Parser::peek(std::size_t offset) const noexcept {
    const auto index = position_ + offset;
    return index < tokens_.size() ? tokens_[index] : tokens_.back();
}

SyntaxToken Parser::nextToken() {
    const auto token = current();
    if (position_ < tokens_.size()) {
        ++position_;
    }
    return token;
}

SyntaxToken Parser::match(SyntaxKind expected) {
    if (current().kind == expected) {
        return nextToken();
    }
    diagnostics_.report(
        "RS1100",
        std::string("expected ") + syntaxKindName(expected) + ", found " + syntaxKindName(current().kind),
        current().span);
    return {expected, {current().span.start, 0}, "", {}, true};
}

CompilationUnitSyntax Parser::parseCompilationUnit() {
    CompilationUnitSyntax result;
    if (current().kind == SyntaxKind::ModuleKeyword) {
        result.moduleDeclaration = parseModuleDeclaration();
    }
    while (current().kind == SyntaxKind::ImportKeyword) {
        result.imports.push_back(parseImportDeclaration());
    }
    while (current().kind != SyntaxKind::EndOfFileToken) {
        const auto before = position_;
        if (current().kind == SyntaxKind::ClassKeyword) {
            result.classes.push_back(parseClassDeclaration());
        } else {
            result.functions.push_back(parseFunctionDeclaration());
        }
        if (position_ == before) {
            diagnostics_.report("RS1101", "parser made no progress while reading a member", current().span);
            nextToken();
        }
    }
    result.endOfFileToken = match(SyntaxKind::EndOfFileToken);
    return result;
}

std::unique_ptr<ModuleDeclarationSyntax> Parser::parseModuleDeclaration() {
    auto result = std::make_unique<ModuleDeclarationSyntax>();
    result->moduleKeyword = match(SyntaxKind::ModuleKeyword);
    parseQualifiedName(result->nameParts, result->dotTokens);
    result->semicolonToken = match(SyntaxKind::SemicolonToken);
    return result;
}

ImportDeclarationSyntax Parser::parseImportDeclaration() {
    ImportDeclarationSyntax result;
    result.importKeyword = match(SyntaxKind::ImportKeyword);
    parseQualifiedName(result.nameParts, result.dotTokens);
    result.semicolonToken = match(SyntaxKind::SemicolonToken);
    return result;
}

void Parser::parseQualifiedName(
    std::vector<SyntaxToken>& nameParts,
    std::vector<SyntaxToken>& dotTokens) {
    nameParts.push_back(match(SyntaxKind::IdentifierToken));
    while (current().kind == SyntaxKind::DotToken) {
        dotTokens.push_back(nextToken());
        nameParts.push_back(match(SyntaxKind::IdentifierToken));
    }
}

TypeSyntax Parser::parseType() {
    TypeSyntax result;
    if (isIdentifierLike(current().kind)) {
        result.name = nextToken();
    } else {
        result.name = match(SyntaxKind::IdentifierToken);
    }
    return result;
}

ParameterSyntax Parser::parseParameter() {
    ParameterSyntax result;
    result.type = parseType();
    result.identifierToken = match(SyntaxKind::IdentifierToken);
    return result;
}

FieldDeclarationSyntax Parser::parseFieldDeclaration() {
    FieldDeclarationSyntax result;
    result.type = parseType();
    result.identifierToken = match(SyntaxKind::IdentifierToken);
    result.semicolonToken = match(SyntaxKind::SemicolonToken);
    return result;
}

ClassDeclarationSyntax Parser::parseClassDeclaration() {
    ClassDeclarationSyntax result;
    result.classKeyword = match(SyntaxKind::ClassKeyword);
    result.identifierToken = match(SyntaxKind::IdentifierToken);
    result.openBraceToken = match(SyntaxKind::OpenBraceToken);
    while (current().kind != SyntaxKind::CloseBraceToken &&
           current().kind != SyntaxKind::EndOfFileToken) {
        const auto before = position_;
        result.fields.push_back(parseFieldDeclaration());
        if (before == position_) {
            diagnostics_.report(
                "RS1103",
                "parser made no progress while reading a class field",
                current().span);
            nextToken();
        }
    }
    result.closeBraceToken = match(SyntaxKind::CloseBraceToken);
    return result;
}

FunctionDeclarationSyntax Parser::parseFunctionDeclaration() {
    FunctionDeclarationSyntax result;
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
    result.body = parseBlockStatement();
    return result;
}

BlockStatementSyntax Parser::parseBlockStatement() {
    BlockStatementSyntax result;
    result.openBraceToken = match(SyntaxKind::OpenBraceToken);

    while (current().kind != SyntaxKind::CloseBraceToken &&
           current().kind != SyntaxKind::EndOfFileToken) {
        const auto before = position_;
        result.statements.push_back(parseStatement());
        if (before == position_) {
            diagnostics_.report("RS1102", "parser made no progress while reading a statement", current().span);
            nextToken();
        }
    }

    result.closeBraceToken = match(SyntaxKind::CloseBraceToken);
    return result;
}

std::unique_ptr<StatementSyntax> Parser::parseStatement() {
    switch (current().kind) {
    case SyntaxKind::ReturnKeyword:
        return parseReturnStatement();
    case SyntaxKind::IfKeyword:
        return parseIfStatement();
    case SyntaxKind::WhileKeyword:
        return parseWhileStatement();
    case SyntaxKind::OpenBraceToken:
        return std::make_unique<BlockStatementSyntax>(parseBlockStatement());
    default:
        break;
    }
    if (isVariableDeclarationStart()) {
        return parseVariableDeclarationStatement();
    }
    return parseExpressionStatement();
}

std::unique_ptr<StatementSyntax> Parser::parseReturnStatement() {
    auto result = std::make_unique<ReturnStatementSyntax>();
    result->returnKeyword = match(SyntaxKind::ReturnKeyword);
    if (current().kind != SyntaxKind::SemicolonToken) {
        result->expression = parseExpression();
    }
    result->semicolonToken = match(SyntaxKind::SemicolonToken);
    return result;
}

std::unique_ptr<StatementSyntax> Parser::parseIfStatement() {
    auto result = std::make_unique<IfStatementSyntax>();
    result->ifKeyword = match(SyntaxKind::IfKeyword);
    result->openParenToken = match(SyntaxKind::OpenParenToken);
    result->condition = parseExpression();
    result->closeParenToken = match(SyntaxKind::CloseParenToken);
    result->thenStatement = parseStatement();
    if (current().kind == SyntaxKind::ElseKeyword) {
        result->elseKeyword = nextToken();
        result->elseStatement = parseStatement();
    }
    return result;
}

std::unique_ptr<StatementSyntax> Parser::parseWhileStatement() {
    auto result = std::make_unique<WhileStatementSyntax>();
    result->whileKeyword = match(SyntaxKind::WhileKeyword);
    result->openParenToken = match(SyntaxKind::OpenParenToken);
    result->condition = parseExpression();
    result->closeParenToken = match(SyntaxKind::CloseParenToken);
    result->body = parseStatement();
    return result;
}

std::unique_ptr<StatementSyntax> Parser::parseVariableDeclarationStatement() {
    auto result = std::make_unique<VariableDeclarationStatementSyntax>();
    result->type = parseType();
    result->identifierToken = match(SyntaxKind::IdentifierToken);
    if (current().kind == SyntaxKind::EqualsToken) {
        result->equalsToken = nextToken();
        result->initializer = parseExpression();
    }
    result->semicolonToken = match(SyntaxKind::SemicolonToken);
    return result;
}

std::unique_ptr<StatementSyntax> Parser::parseExpressionStatement() {
    auto result = std::make_unique<ExpressionStatementSyntax>();
    result->expression = parseExpression();
    result->semicolonToken = match(SyntaxKind::SemicolonToken);
    return result;
}

std::unique_ptr<ExpressionSyntax> Parser::parseExpression() {
    return parseAssignmentExpression();
}

std::unique_ptr<ExpressionSyntax> Parser::parseAssignmentExpression() {
    auto left = parseBinaryExpression();
    if (current().kind != SyntaxKind::EqualsToken) {
        return left;
    }

    if (left->kind() == SyntaxKind::NameExpression) {
        auto name = std::unique_ptr<NameExpressionSyntax>(
            static_cast<NameExpressionSyntax*>(left.release()));
        auto result = std::make_unique<AssignmentExpressionSyntax>();
        result->identifierToken = std::move(name->identifierToken);
        result->equalsToken = nextToken();
        result->expression = parseAssignmentExpression();
        return result;
    }

    if (left->kind() == SyntaxKind::MemberAccessExpression) {
        auto member = std::unique_ptr<MemberAccessExpressionSyntax>(
            static_cast<MemberAccessExpressionSyntax*>(left.release()));
        auto result = std::make_unique<MemberAssignmentExpressionSyntax>();
        result->receiver = std::move(member->receiver);
        result->dotToken = std::move(member->dotToken);
        result->nameToken = std::move(member->nameToken);
        result->equalsToken = nextToken();
        result->expression = parseAssignmentExpression();
        return result;
    }

    return left;
}

std::unique_ptr<ExpressionSyntax> Parser::parseBinaryExpression(int parentPrecedence) {
    std::unique_ptr<ExpressionSyntax> left;
    const auto unary = unaryPrecedence(current().kind);
    if (unary != 0 && unary >= parentPrecedence) {
        auto expression = std::make_unique<UnaryExpressionSyntax>();
        expression->operatorToken = nextToken();
        expression->operand = parseBinaryExpression(unary);
        left = std::move(expression);
    } else {
        left = parsePostfixExpression();
    }

    for (;;) {
        const auto precedence = binaryPrecedence(current().kind);
        if (precedence == 0 || precedence <= parentPrecedence) {
            break;
        }
        auto expression = std::make_unique<BinaryExpressionSyntax>();
        expression->left = std::move(left);
        expression->operatorToken = nextToken();
        expression->right = parseBinaryExpression(precedence);
        left = std::move(expression);
    }
    return left;
}

std::unique_ptr<ExpressionSyntax> Parser::parsePostfixExpression() {
    auto expression = parsePrimaryExpression();
    while (current().kind == SyntaxKind::DotToken) {
        auto member = std::make_unique<MemberAccessExpressionSyntax>();
        member->receiver = std::move(expression);
        member->dotToken = nextToken();
        member->nameToken = match(SyntaxKind::IdentifierToken);
        expression = std::move(member);
    }
    return expression;
}

std::unique_ptr<ExpressionSyntax> Parser::parseNewObjectExpression() {
    auto result = std::make_unique<NewObjectExpressionSyntax>();
    result->newKeyword = match(SyntaxKind::NewKeyword);
    result->type = parseType();
    result->openParenToken = match(SyntaxKind::OpenParenToken);
    result->closeParenToken = match(SyntaxKind::CloseParenToken);
    return result;
}

std::unique_ptr<ExpressionSyntax> Parser::parsePrimaryExpression() {
    switch (current().kind) {
    case SyntaxKind::NewKeyword:
        return parseNewObjectExpression();
    case SyntaxKind::OpenParenToken: {
        auto result = std::make_unique<ParenthesizedExpressionSyntax>();
        result->openParenToken = nextToken();
        result->expression = parseExpression();
        result->closeParenToken = match(SyntaxKind::CloseParenToken);
        return result;
    }
    case SyntaxKind::TrueKeyword:
    case SyntaxKind::FalseKeyword:
    case SyntaxKind::NullKeyword:
    case SyntaxKind::IntegerLiteralToken:
    case SyntaxKind::FloatLiteralToken:
    case SyntaxKind::StringLiteralToken: {
        auto result = std::make_unique<LiteralExpressionSyntax>();
        result->literalToken = nextToken();
        return result;
    }
    case SyntaxKind::IdentifierToken:
        if (peek(1).kind == SyntaxKind::OpenParenToken) {
            return parseCallExpression();
        } else {
            auto result = std::make_unique<NameExpressionSyntax>();
            result->identifierToken = nextToken();
            return result;
        }
    default: {
        auto result = std::make_unique<NameExpressionSyntax>();
        result->identifierToken = match(SyntaxKind::IdentifierToken);
        return result;
    }
    }
}

std::unique_ptr<ExpressionSyntax> Parser::parseCallExpression() {
    auto result = std::make_unique<CallExpressionSyntax>();
    result->identifierToken = match(SyntaxKind::IdentifierToken);
    result->openParenToken = match(SyntaxKind::OpenParenToken);
    if (current().kind != SyntaxKind::CloseParenToken &&
        current().kind != SyntaxKind::EndOfFileToken) {
        result->arguments.push_back(parseExpression());
        while (current().kind == SyntaxKind::CommaToken) {
            result->commaTokens.push_back(nextToken());
            result->arguments.push_back(parseExpression());
        }
    }
    result->closeParenToken = match(SyntaxKind::CloseParenToken);
    return result;
}

bool Parser::isVariableDeclarationStart() const noexcept {
    if (!isIdentifierLike(current().kind)) {
        return false;
    }
    return peek(1).kind == SyntaxKind::IdentifierToken &&
        (peek(2).kind == SyntaxKind::EqualsToken || peek(2).kind == SyntaxKind::SemicolonToken);
}

} // namespace realscript::syntax
