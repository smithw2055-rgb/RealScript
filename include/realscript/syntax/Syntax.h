#pragma once

#include "realscript/diagnostics/Diagnostic.h"
#include "realscript/text/Text.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace realscript::syntax {

enum class SyntaxKind {
    BadToken,
    EndOfFileToken,
    IdentifierToken,
    IntegerLiteralToken,
    FloatLiteralToken,
    StringLiteralToken,

    PlusToken,
    MinusToken,
    StarToken,
    SlashToken,
    PercentToken,
    BangToken,
    AmpersandAmpersandToken,
    PipePipeToken,
    EqualsToken,
    EqualsEqualsToken,
    BangEqualsToken,
    LessToken,
    LessOrEqualsToken,
    GreaterToken,
    GreaterOrEqualsToken,
    OpenParenToken,
    CloseParenToken,
    OpenBraceToken,
    CloseBraceToken,
    CommaToken,
    DotToken,
    ColonToken,
    SemicolonToken,

    ModuleKeyword,
    ImportKeyword,
    ReturnKeyword,
    TrueKeyword,
    FalseKeyword,
    NullKeyword,
    BoolKeyword,
    ByteKeyword,
    SByteKeyword,
    ShortKeyword,
    UShortKeyword,
    IntKeyword,
    UIntKeyword,
    LongKeyword,
    ULongKeyword,
    FloatKeyword,
    DoubleKeyword,
    StringKeyword,
    VoidKeyword,

    CompilationUnit,
    ModuleDeclaration,
    ImportDeclaration,
    FunctionDeclaration,
    Parameter,
    TypeName,
    BlockStatement,
    ReturnStatement,
    VariableDeclarationStatement,
    ExpressionStatement,
    LiteralExpression,
    NameExpression,
    UnaryExpression,
    BinaryExpression,
    ParenthesizedExpression,
    CallExpression,
};

using TokenValue = std::variant<std::monostate, std::int64_t, double, bool, std::string>;

struct SyntaxToken {
    SyntaxKind kind = SyntaxKind::BadToken;
    text::TextSpan span;
    std::string text;
    TokenValue value;
    bool missing = false;
};

[[nodiscard]] const char* syntaxKindName(SyntaxKind kind) noexcept;
[[nodiscard]] SyntaxKind keywordKind(const std::string& text) noexcept;
[[nodiscard]] int unaryPrecedence(SyntaxKind kind) noexcept;
[[nodiscard]] int binaryPrecedence(SyntaxKind kind) noexcept;
[[nodiscard]] bool isPrimitiveTypeKeyword(SyntaxKind kind) noexcept;

class Lexer {
public:
    Lexer(const text::SourceText& source, diagnostics::DiagnosticBag& diagnostics);

    [[nodiscard]] SyntaxToken nextToken();
    [[nodiscard]] std::vector<SyntaxToken> lexAll();

private:
    void skipTrivia();
    [[nodiscard]] SyntaxToken lexNumber();
    [[nodiscard]] SyntaxToken lexIdentifierOrKeyword();
    [[nodiscard]] SyntaxToken lexString();
    [[nodiscard]] char current() const noexcept;
    [[nodiscard]] char peek(std::size_t offset) const noexcept;
    void advance(std::size_t count = 1) noexcept;

    const text::SourceText& source_;
    diagnostics::DiagnosticBag& diagnostics_;
    std::size_t position_ = 0;
};

struct SyntaxNode {
    virtual ~SyntaxNode() = default;
    [[nodiscard]] virtual SyntaxKind kind() const noexcept = 0;
    [[nodiscard]] virtual text::TextSpan span() const noexcept = 0;
};

struct ExpressionSyntax : SyntaxNode {};
struct StatementSyntax : SyntaxNode {};

struct TypeSyntax final : SyntaxNode {
    SyntaxToken name;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::TypeName; }
    [[nodiscard]] text::TextSpan span() const noexcept override { return name.span; }
};

struct LiteralExpressionSyntax final : ExpressionSyntax {
    SyntaxToken literalToken;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::LiteralExpression; }
    [[nodiscard]] text::TextSpan span() const noexcept override { return literalToken.span; }
};

struct NameExpressionSyntax final : ExpressionSyntax {
    SyntaxToken identifierToken;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::NameExpression; }
    [[nodiscard]] text::TextSpan span() const noexcept override { return identifierToken.span; }
};

struct UnaryExpressionSyntax final : ExpressionSyntax {
    SyntaxToken operatorToken;
    std::unique_ptr<ExpressionSyntax> operand;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::UnaryExpression; }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct BinaryExpressionSyntax final : ExpressionSyntax {
    std::unique_ptr<ExpressionSyntax> left;
    SyntaxToken operatorToken;
    std::unique_ptr<ExpressionSyntax> right;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::BinaryExpression; }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct ParenthesizedExpressionSyntax final : ExpressionSyntax {
    SyntaxToken openParenToken;
    std::unique_ptr<ExpressionSyntax> expression;
    SyntaxToken closeParenToken;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::ParenthesizedExpression; }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct CallExpressionSyntax final : ExpressionSyntax {
    SyntaxToken identifierToken;
    SyntaxToken openParenToken;
    std::vector<std::unique_ptr<ExpressionSyntax>> arguments;
    std::vector<SyntaxToken> commaTokens;
    SyntaxToken closeParenToken;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::CallExpression; }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct ReturnStatementSyntax final : StatementSyntax {
    SyntaxToken returnKeyword;
    std::unique_ptr<ExpressionSyntax> expression;
    SyntaxToken semicolonToken;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::ReturnStatement; }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct VariableDeclarationStatementSyntax final : StatementSyntax {
    TypeSyntax type;
    SyntaxToken identifierToken;
    std::optional<SyntaxToken> equalsToken;
    std::unique_ptr<ExpressionSyntax> initializer;
    SyntaxToken semicolonToken;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::VariableDeclarationStatement; }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct ExpressionStatementSyntax final : StatementSyntax {
    std::unique_ptr<ExpressionSyntax> expression;
    SyntaxToken semicolonToken;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::ExpressionStatement; }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct BlockStatementSyntax final : StatementSyntax {
    SyntaxToken openBraceToken;
    std::vector<std::unique_ptr<StatementSyntax>> statements;
    SyntaxToken closeBraceToken;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::BlockStatement; }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct ParameterSyntax final : SyntaxNode {
    TypeSyntax type;
    SyntaxToken identifierToken;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::Parameter; }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct FunctionDeclarationSyntax final : SyntaxNode {
    TypeSyntax returnType;
    SyntaxToken identifierToken;
    SyntaxToken openParenToken;
    std::vector<ParameterSyntax> parameters;
    std::vector<SyntaxToken> commaTokens;
    SyntaxToken closeParenToken;
    BlockStatementSyntax body;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::FunctionDeclaration; }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct ModuleDeclarationSyntax final : SyntaxNode {
    SyntaxToken moduleKeyword;
    std::vector<SyntaxToken> nameParts;
    std::vector<SyntaxToken> dotTokens;
    SyntaxToken semicolonToken;

    [[nodiscard]] std::string fullName() const;
    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::ModuleDeclaration; }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct ImportDeclarationSyntax final : SyntaxNode {
    SyntaxToken importKeyword;
    std::vector<SyntaxToken> nameParts;
    std::vector<SyntaxToken> dotTokens;
    SyntaxToken semicolonToken;

    [[nodiscard]] std::string fullName() const;
    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::ImportDeclaration; }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct CompilationUnitSyntax final : SyntaxNode {
    std::unique_ptr<ModuleDeclarationSyntax> moduleDeclaration;
    std::vector<ImportDeclarationSyntax> imports;
    std::vector<FunctionDeclarationSyntax> functions;
    SyntaxToken endOfFileToken;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::CompilationUnit; }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

class Parser {
public:
    Parser(const text::SourceText& source, diagnostics::DiagnosticBag& diagnostics);

    [[nodiscard]] CompilationUnitSyntax parseCompilationUnit();
    [[nodiscard]] const std::vector<SyntaxToken>& tokens() const noexcept { return tokens_; }

private:
    [[nodiscard]] const SyntaxToken& current() const noexcept;
    [[nodiscard]] const SyntaxToken& peek(std::size_t offset) const noexcept;
    SyntaxToken nextToken();
    SyntaxToken match(SyntaxKind expected);

    [[nodiscard]] std::unique_ptr<ModuleDeclarationSyntax> parseModuleDeclaration();
    [[nodiscard]] ImportDeclarationSyntax parseImportDeclaration();
    void parseQualifiedName(
        std::vector<SyntaxToken>& nameParts,
        std::vector<SyntaxToken>& dotTokens);
    [[nodiscard]] FunctionDeclarationSyntax parseFunctionDeclaration();
    [[nodiscard]] TypeSyntax parseType();
    [[nodiscard]] ParameterSyntax parseParameter();
    [[nodiscard]] BlockStatementSyntax parseBlockStatement();
    [[nodiscard]] std::unique_ptr<StatementSyntax> parseStatement();
    [[nodiscard]] std::unique_ptr<StatementSyntax> parseReturnStatement();
    [[nodiscard]] std::unique_ptr<StatementSyntax> parseVariableDeclarationStatement();
    [[nodiscard]] std::unique_ptr<StatementSyntax> parseExpressionStatement();
    [[nodiscard]] std::unique_ptr<ExpressionSyntax> parseExpression(int parentPrecedence = 0);
    [[nodiscard]] std::unique_ptr<ExpressionSyntax> parsePrimaryExpression();
    [[nodiscard]] std::unique_ptr<ExpressionSyntax> parseCallExpression();
    [[nodiscard]] bool isVariableDeclarationStart() const noexcept;
    void synchronizeMember();

    const text::SourceText& source_;
    diagnostics::DiagnosticBag& diagnostics_;
    std::vector<SyntaxToken> tokens_;
    std::size_t position_ = 0;
};

} // namespace realscript::syntax
