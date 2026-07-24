#include "realscript/syntax/Syntax.h"

#include <charconv>
#include <cstdlib>
#include <sstream>
#include <unordered_map>

namespace realscript::syntax {
namespace {

text::TextSpan combine(text::TextSpan first, text::TextSpan last) noexcept {
    return text::TextSpan::fromBounds(first.start, last.end());
}

std::string joinQualifiedName(const std::vector<SyntaxToken>& parts) {
    std::ostringstream out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            out << '.';
        }
        out << parts[i].text;
    }
    return out.str();
}

bool isIdentifierLike(SyntaxKind kind) noexcept {
    return kind == SyntaxKind::IdentifierToken || isPrimitiveTypeKeyword(kind);
}

} // namespace

const char* syntaxKindName(SyntaxKind kind) noexcept {
    switch (kind) {
#define RS_KIND(value) case SyntaxKind::value: return #value
        RS_KIND(BadToken);
        RS_KIND(EndOfFileToken);
        RS_KIND(IdentifierToken);
        RS_KIND(IntegerLiteralToken);
        RS_KIND(FloatLiteralToken);
        RS_KIND(StringLiteralToken);
        RS_KIND(PlusToken);
        RS_KIND(MinusToken);
        RS_KIND(StarToken);
        RS_KIND(SlashToken);
        RS_KIND(PercentToken);
        RS_KIND(BangToken);
        RS_KIND(AmpersandAmpersandToken);
        RS_KIND(PipePipeToken);
        RS_KIND(EqualsToken);
        RS_KIND(EqualsEqualsToken);
        RS_KIND(BangEqualsToken);
        RS_KIND(LessToken);
        RS_KIND(LessOrEqualsToken);
        RS_KIND(GreaterToken);
        RS_KIND(GreaterOrEqualsToken);
        RS_KIND(OpenParenToken);
        RS_KIND(CloseParenToken);
        RS_KIND(OpenBraceToken);
        RS_KIND(CloseBraceToken);
        RS_KIND(CommaToken);
        RS_KIND(DotToken);
        RS_KIND(ColonToken);
        RS_KIND(SemicolonToken);
        RS_KIND(ModuleKeyword);
        RS_KIND(ImportKeyword);
        RS_KIND(ReturnKeyword);
        RS_KIND(TrueKeyword);
        RS_KIND(FalseKeyword);
        RS_KIND(NullKeyword);
        RS_KIND(BoolKeyword);
        RS_KIND(ByteKeyword);
        RS_KIND(SByteKeyword);
        RS_KIND(ShortKeyword);
        RS_KIND(UShortKeyword);
        RS_KIND(IntKeyword);
        RS_KIND(UIntKeyword);
        RS_KIND(LongKeyword);
        RS_KIND(ULongKeyword);
        RS_KIND(FloatKeyword);
        RS_KIND(DoubleKeyword);
        RS_KIND(StringKeyword);
        RS_KIND(VoidKeyword);
        RS_KIND(CompilationUnit);
        RS_KIND(ModuleDeclaration);
        RS_KIND(ImportDeclaration);
        RS_KIND(FunctionDeclaration);
        RS_KIND(Parameter);
        RS_KIND(TypeName);
        RS_KIND(BlockStatement);
        RS_KIND(ReturnStatement);
        RS_KIND(VariableDeclarationStatement);
        RS_KIND(ExpressionStatement);
        RS_KIND(LiteralExpression);
        RS_KIND(NameExpression);
        RS_KIND(UnaryExpression);
        RS_KIND(BinaryExpression);
        RS_KIND(ParenthesizedExpression);
        RS_KIND(CallExpression);
#undef RS_KIND
    }
    return "UnknownSyntaxKind";
}

SyntaxKind keywordKind(const std::string& text) noexcept {
    static const std::unordered_map<std::string, SyntaxKind> keywords = {
        {"module", SyntaxKind::ModuleKeyword},
        {"import", SyntaxKind::ImportKeyword},
        {"return", SyntaxKind::ReturnKeyword},
        {"true", SyntaxKind::TrueKeyword},
        {"false", SyntaxKind::FalseKeyword},
        {"null", SyntaxKind::NullKeyword},
        {"bool", SyntaxKind::BoolKeyword},
        {"byte", SyntaxKind::ByteKeyword},
        {"sbyte", SyntaxKind::SByteKeyword},
        {"short", SyntaxKind::ShortKeyword},
        {"ushort", SyntaxKind::UShortKeyword},
        {"int", SyntaxKind::IntKeyword},
        {"uint", SyntaxKind::UIntKeyword},
        {"long", SyntaxKind::LongKeyword},
        {"ulong", SyntaxKind::ULongKeyword},
        {"float", SyntaxKind::FloatKeyword},
        {"double", SyntaxKind::DoubleKeyword},
        {"string", SyntaxKind::StringKeyword},
        {"void", SyntaxKind::VoidKeyword},
    };
    const auto it = keywords.find(text);
    return it == keywords.end() ? SyntaxKind::IdentifierToken : it->second;
}

int unaryPrecedence(SyntaxKind kind) noexcept {
    switch (kind) {
    case SyntaxKind::PlusToken:
    case SyntaxKind::MinusToken:
    case SyntaxKind::BangToken:
        return 7;
    default:
        return 0;
    }
}

int binaryPrecedence(SyntaxKind kind) noexcept {
    switch (kind) {
    case SyntaxKind::StarToken:
    case SyntaxKind::SlashToken:
    case SyntaxKind::PercentToken:
        return 6;
    case SyntaxKind::PlusToken:
    case SyntaxKind::MinusToken:
        return 5;
    case SyntaxKind::LessToken:
    case SyntaxKind::LessOrEqualsToken:
    case SyntaxKind::GreaterToken:
    case SyntaxKind::GreaterOrEqualsToken:
        return 4;
    case SyntaxKind::EqualsEqualsToken:
    case SyntaxKind::BangEqualsToken:
        return 3;
    case SyntaxKind::AmpersandAmpersandToken:
        return 2;
    case SyntaxKind::PipePipeToken:
        return 1;
    default:
        return 0;
    }
}

bool isPrimitiveTypeKeyword(SyntaxKind kind) noexcept {
    switch (kind) {
    case SyntaxKind::BoolKeyword:
    case SyntaxKind::ByteKeyword:
    case SyntaxKind::SByteKeyword:
    case SyntaxKind::ShortKeyword:
    case SyntaxKind::UShortKeyword:
    case SyntaxKind::IntKeyword:
    case SyntaxKind::UIntKeyword:
    case SyntaxKind::LongKeyword:
    case SyntaxKind::ULongKeyword:
    case SyntaxKind::FloatKeyword:
    case SyntaxKind::DoubleKeyword:
    case SyntaxKind::StringKeyword:
    case SyntaxKind::VoidKeyword:
        return true;
    default:
        return false;
    }
}

Lexer::Lexer(const text::SourceText& source, diagnostics::DiagnosticBag& diagnostics)
    : source_(source), diagnostics_(diagnostics) {}

char Lexer::current() const noexcept {
    return source_.at(position_);
}

char Lexer::peek(std::size_t offset) const noexcept {
    return source_.at(position_ + offset);
}

void Lexer::advance(std::size_t count) noexcept {
    position_ += count;
}

void Lexer::skipTrivia() {
    for (;;) {
        if (current() == ' ' || current() == '\t' || current() == '\r' || current() == '\n') {
            advance();
            continue;
        }
        if (current() == '/' && peek(1) == '/') {
            advance(2);
            while (current() != '\0' && current() != '\r' && current() != '\n') {
                advance();
            }
            continue;
        }
        if (current() == '/' && peek(1) == '*') {
            const auto start = position_;
            advance(2);
            while (current() != '\0' && !(current() == '*' && peek(1) == '/')) {
                advance();
            }
            if (current() == '\0') {
                diagnostics_.report("RS1002", "unterminated block comment", {start, position_ - start});
                return;
            }
            advance(2);
            continue;
        }
        return;
    }
}

SyntaxToken Lexer::lexNumber() {
    const auto start = position_;
    bool isFloat = false;
    while (current() >= '0' && current() <= '9') {
        advance();
    }
    if (current() == '.' && peek(1) >= '0' && peek(1) <= '9') {
        isFloat = true;
        advance();
        while (current() >= '0' && current() <= '9') {
            advance();
        }
    }

    const auto span = text::TextSpan{start, position_ - start};
    const auto tokenText = std::string(source_.view(span));
    if (isFloat) {
        char* end = nullptr;
        const double value = std::strtod(tokenText.c_str(), &end);
        if (end == tokenText.c_str() || *end != '\0') {
            diagnostics_.report("RS1003", "invalid floating-point literal", span);
            return {SyntaxKind::FloatLiteralToken, span, tokenText, 0.0, false};
        }
        return {SyntaxKind::FloatLiteralToken, span, tokenText, value, false};
    }

    std::int64_t value = 0;
    const auto result = std::from_chars(tokenText.data(), tokenText.data() + tokenText.size(), value);
    if (result.ec != std::errc{}) {
        diagnostics_.report("RS1004", "integer literal is outside the supported range", span);
        value = 0;
    }
    return {SyntaxKind::IntegerLiteralToken, span, tokenText, value, false};
}

SyntaxToken Lexer::lexIdentifierOrKeyword() {
    const auto start = position_;
    while ((current() >= 'a' && current() <= 'z') ||
           (current() >= 'A' && current() <= 'Z') ||
           (current() >= '0' && current() <= '9') || current() == '_') {
        advance();
    }
    const auto span = text::TextSpan{start, position_ - start};
    auto tokenText = std::string(source_.view(span));
    const auto kind = keywordKind(tokenText);
    TokenValue value;
    if (kind == SyntaxKind::TrueKeyword) {
        value = true;
    } else if (kind == SyntaxKind::FalseKeyword) {
        value = false;
    }
    return {kind, span, std::move(tokenText), std::move(value), false};
}

SyntaxToken Lexer::lexString() {
    const auto start = position_;
    advance();
    std::string value;
    bool terminated = false;

    while (current() != '\0') {
        if (current() == '"') {
            advance();
            terminated = true;
            break;
        }
        if (current() == '\r' || current() == '\n') {
            break;
        }
        if (current() == '\\') {
            const char escaped = peek(1);
            switch (escaped) {
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            case '\\': value.push_back('\\'); break;
            case '"': value.push_back('"'); break;
            default:
                diagnostics_.report("RS1005", "unsupported escape sequence", {position_, 2});
                value.push_back(escaped);
                break;
            }
            advance(2);
            continue;
        }
        value.push_back(current());
        advance();
    }

    const auto span = text::TextSpan{start, position_ - start};
    if (!terminated) {
        diagnostics_.report("RS1001", "unterminated string literal", span);
    }
    return {SyntaxKind::StringLiteralToken, span, std::string(source_.view(span)), std::move(value), false};
}

SyntaxToken Lexer::nextToken() {
    skipTrivia();
    const auto start = position_;

    if (current() == '\0') {
        return {SyntaxKind::EndOfFileToken, {position_, 0}, "", {}, false};
    }
    if (current() >= '0' && current() <= '9') {
        return lexNumber();
    }
    if ((current() >= 'a' && current() <= 'z') ||
        (current() >= 'A' && current() <= 'Z') || current() == '_') {
        return lexIdentifierOrKeyword();
    }
    if (current() == '"') {
        return lexString();
    }

    auto single = [&](SyntaxKind kind) {
        advance();
        const auto span = text::TextSpan{start, 1};
        return SyntaxToken{kind, span, std::string(source_.view(span)), {}, false};
    };
    auto pair = [&](SyntaxKind kind) {
        advance(2);
        const auto span = text::TextSpan{start, 2};
        return SyntaxToken{kind, span, std::string(source_.view(span)), {}, false};
    };

    switch (current()) {
    case '+': return single(SyntaxKind::PlusToken);
    case '-': return single(SyntaxKind::MinusToken);
    case '*': return single(SyntaxKind::StarToken);
    case '/': return single(SyntaxKind::SlashToken);
    case '%': return single(SyntaxKind::PercentToken);
    case '(': return single(SyntaxKind::OpenParenToken);
    case ')': return single(SyntaxKind::CloseParenToken);
    case '{': return single(SyntaxKind::OpenBraceToken);
    case '}': return single(SyntaxKind::CloseBraceToken);
    case ',': return single(SyntaxKind::CommaToken);
    case '.': return single(SyntaxKind::DotToken);
    case ':': return single(SyntaxKind::ColonToken);
    case ';': return single(SyntaxKind::SemicolonToken);
    case '!': return peek(1) == '=' ? pair(SyntaxKind::BangEqualsToken) : single(SyntaxKind::BangToken);
    case '=': return peek(1) == '=' ? pair(SyntaxKind::EqualsEqualsToken) : single(SyntaxKind::EqualsToken);
    case '<': return peek(1) == '=' ? pair(SyntaxKind::LessOrEqualsToken) : single(SyntaxKind::LessToken);
    case '>': return peek(1) == '=' ? pair(SyntaxKind::GreaterOrEqualsToken) : single(SyntaxKind::GreaterToken);
    case '&':
        if (peek(1) == '&') return pair(SyntaxKind::AmpersandAmpersandToken);
        break;
    case '|':
        if (peek(1) == '|') return pair(SyntaxKind::PipePipeToken);
        break;
    default:
        break;
    }

    advance();
    const auto span = text::TextSpan{start, 1};
    diagnostics_.report(
        "RS1000",
        "invalid character '" + std::string(source_.view(span)) + "'",
        span);
    return {SyntaxKind::BadToken, span, std::string(source_.view(span)), {}, false};
}

std::vector<SyntaxToken> Lexer::lexAll() {
    std::vector<SyntaxToken> result;
    for (;;) {
        auto token = nextToken();
        if (token.kind != SyntaxKind::BadToken) {
            result.push_back(token);
        }
        if (token.kind == SyntaxKind::EndOfFileToken) {
            break;
        }
    }
    return result;
}

text::TextSpan UnaryExpressionSyntax::span() const noexcept {
    return combine(operatorToken.span, operand->span());
}

text::TextSpan BinaryExpressionSyntax::span() const noexcept {
    return combine(left->span(), right->span());
}

text::TextSpan ParenthesizedExpressionSyntax::span() const noexcept {
    return combine(openParenToken.span, closeParenToken.span);
}

text::TextSpan CallExpressionSyntax::span() const noexcept {
    return combine(identifierToken.span, closeParenToken.span);
}

text::TextSpan ReturnStatementSyntax::span() const noexcept {
    return combine(returnKeyword.span, semicolonToken.span);
}

text::TextSpan VariableDeclarationStatementSyntax::span() const noexcept {
    return combine(type.span(), semicolonToken.span);
}

text::TextSpan ExpressionStatementSyntax::span() const noexcept {
    return combine(expression->span(), semicolonToken.span);
}

text::TextSpan BlockStatementSyntax::span() const noexcept {
    return combine(openBraceToken.span, closeBraceToken.span);
}

text::TextSpan ParameterSyntax::span() const noexcept {
    return combine(type.span(), identifierToken.span);
}

text::TextSpan FunctionDeclarationSyntax::span() const noexcept {
    return combine(returnType.span(), body.span());
}

std::string ModuleDeclarationSyntax::fullName() const {
    return joinQualifiedName(nameParts);
}

text::TextSpan ModuleDeclarationSyntax::span() const noexcept {
    return combine(moduleKeyword.span, semicolonToken.span);
}

std::string ImportDeclarationSyntax::fullName() const {
    return joinQualifiedName(nameParts);
}

text::TextSpan ImportDeclarationSyntax::span() const noexcept {
    return combine(importKeyword.span, semicolonToken.span);
}

text::TextSpan CompilationUnitSyntax::span() const noexcept {
    if (moduleDeclaration) {
        return combine(moduleDeclaration->span(), endOfFileToken.span);
    }
    if (!imports.empty()) {
        return combine(imports.front().span(), endOfFileToken.span);
    }
    if (!functions.empty()) {
        return combine(functions.front().span(), endOfFileToken.span);
    }
    return endOfFileToken.span;
}

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
        result.functions.push_back(parseFunctionDeclaration());
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
    if (current().kind == SyntaxKind::ReturnKeyword) {
        return parseReturnStatement();
    }
    if (current().kind == SyntaxKind::OpenBraceToken) {
        return std::make_unique<BlockStatementSyntax>(parseBlockStatement());
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

std::unique_ptr<ExpressionSyntax> Parser::parseExpression(int parentPrecedence) {
    std::unique_ptr<ExpressionSyntax> left;
    const auto unary = unaryPrecedence(current().kind);
    if (unary != 0 && unary >= parentPrecedence) {
        auto expression = std::make_unique<UnaryExpressionSyntax>();
        expression->operatorToken = nextToken();
        expression->operand = parseExpression(unary);
        left = std::move(expression);
    } else {
        left = parsePrimaryExpression();
    }

    for (;;) {
        const auto precedence = binaryPrecedence(current().kind);
        if (precedence == 0 || precedence <= parentPrecedence) {
            break;
        }
        auto expression = std::make_unique<BinaryExpressionSyntax>();
        expression->left = std::move(left);
        expression->operatorToken = nextToken();
        expression->right = parseExpression(precedence);
        left = std::move(expression);
    }
    return left;
}

std::unique_ptr<ExpressionSyntax> Parser::parsePrimaryExpression() {
    switch (current().kind) {
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

void Parser::synchronizeMember() {
    while (current().kind != SyntaxKind::EndOfFileToken &&
           current().kind != SyntaxKind::CloseBraceToken &&
           current().kind != SyntaxKind::SemicolonToken) {
        nextToken();
    }
    if (current().kind == SyntaxKind::SemicolonToken) {
        nextToken();
    }
}

} // namespace realscript::syntax
