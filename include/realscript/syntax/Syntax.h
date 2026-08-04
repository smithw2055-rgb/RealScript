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
    PlusEqualsToken,
    MinusToken,
    MinusEqualsToken,
    ArrowToken,
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
    OpenBracketToken,
    CloseBracketToken,
    CommaToken,
    DotToken,
    ColonToken,
    QuestionToken,
    QuestionQuestionToken,
    QuestionDotToken,
    SemicolonToken,

    ModuleKeyword,
    ImportKeyword,
    ReturnKeyword,
    IfKeyword,
    ElseKeyword,
    WhileKeyword,
    ForKeyword,
    ForeachKeyword,
    InKeyword,
    RefKeyword,
    OutKeyword,
    ParamsKeyword,
    DoKeyword,
    BreakKeyword,
    ContinueKeyword,
    SwitchKeyword,
    CaseKeyword,
    DefaultKeyword,
    SequenceKeyword,
    YieldKeyword,
    ClassKeyword,
    StructKeyword,
    EnumKeyword,
    InterfaceKeyword,
    DelegateKeyword,
    EventKeyword,
    StaticKeyword,
    PublicKeyword,
    PrivateKeyword,
    ProtectedKeyword,
    InternalKeyword,
    AbstractKeyword,
    VirtualKeyword,
    OverrideKeyword,
    SealedKeyword,
    BaseKeyword,
    GetKeyword,
    SetKeyword,
    ThisKeyword,
    NewKeyword,
    TrueKeyword,
    FalseKeyword,
    NullKeyword,
    IsKeyword,
    AsKeyword,
    TypeofKeyword,
    WhenKeyword,
    ThrowKeyword,
    TryKeyword,
    CatchKeyword,
    FinallyKeyword,
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
    HandleKeyword,
    VoidKeyword,

    CompilationUnit,
    AttributeList,
    Attribute,
    AttributeArgument,
    ModuleDeclaration,
    ImportDeclaration,
    ClassDeclaration,
    StructDeclaration,
    EnumDeclaration,
    InterfaceDeclaration,
    InterfaceMethodDeclaration,
    DelegateDeclaration,
    EventDeclaration,
    EnumMemberDeclaration,
    FieldDeclaration,
    ConstructorDeclaration,
    PropertyDeclaration,
    AccessorDeclaration,
    GenericConstraintClause,
    FunctionDeclaration,
    SequenceDeclaration,
    Parameter,
    InitializerElement,
    TypeName,
    BlockStatement,
    ReturnStatement,
    IfStatement,
    WhileStatement,
    ForStatement,
    ForeachStatement,
    DoWhileStatement,
    BreakStatement,
    ContinueStatement,
    SwitchSection,
    SwitchStatement,
    ThrowStatement,
    TryStatement,
    CatchClause,
    YieldWaitStatement,
    YieldBreakStatement,
    EventSubscriptionStatement,
    VariableDeclarationStatement,
    ExpressionStatement,
    LiteralExpression,
    NameExpression,
    LambdaExpression,
    UnaryExpression,
    BinaryExpression,
    TypeBinaryExpression,
    TypeOfExpression,
    SwitchExpression,
    SwitchExpressionArm,
    ConditionalExpression,
    AssignmentExpression,
    MemberAssignmentExpression,
    ElementAssignmentExpression,
    ParenthesizedExpression,
    CastExpression,
    CallExpression,
    MemberCallExpression,
    ThisExpression,
    BaseExpression,
    MemberAccessExpression,
    ElementAccessExpression,
    NewObjectExpression,
    NewArrayExpression,
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
[[nodiscard]] bool isDeclarationModifier(SyntaxKind kind) noexcept;

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

struct AttributeArgumentSyntax final : SyntaxNode {
    std::optional<SyntaxToken> nameToken;
    std::optional<SyntaxToken> equalsToken;
    std::vector<SyntaxToken> valueTokens;

    [[nodiscard]] SyntaxKind kind() const noexcept override {
        return SyntaxKind::AttributeArgument;
    }
    [[nodiscard]] text::TextSpan valueSpan() const noexcept;
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct AttributeSyntax final : SyntaxNode {
    SyntaxToken nameToken;
    std::optional<SyntaxToken> openParenToken;
    std::vector<AttributeArgumentSyntax> arguments;
    std::vector<SyntaxToken> commaTokens;
    std::optional<SyntaxToken> closeParenToken;

    [[nodiscard]] SyntaxKind kind() const noexcept override {
        return SyntaxKind::Attribute;
    }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct AttributeListSyntax final : SyntaxNode {
    SyntaxToken openBracketToken;
    std::vector<AttributeSyntax> attributes;
    std::vector<SyntaxToken> commaTokens;
    SyntaxToken closeBracketToken;

    [[nodiscard]] SyntaxKind kind() const noexcept override {
        return SyntaxKind::AttributeList;
    }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct TypeSyntax final : SyntaxNode {
    SyntaxToken name;
    std::optional<SyntaxToken> lessToken;
    std::vector<TypeSyntax> typeArguments;
    std::vector<SyntaxToken> typeArgumentCommaTokens;
    std::optional<SyntaxToken> greaterToken;
    std::optional<SyntaxToken> nullableToken;
    std::optional<SyntaxToken> openBracketToken;
    std::optional<SyntaxToken> closeBracketToken;

    [[nodiscard]] bool isGeneric() const noexcept {
        return lessToken.has_value() && greaterToken.has_value();
    }
    [[nodiscard]] bool isArray() const noexcept {
        return openBracketToken.has_value() && closeBracketToken.has_value();
    }
    [[nodiscard]] bool isNullable() const noexcept {
        return nullableToken.has_value();
    }
    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::TypeName; }
    [[nodiscard]] text::TextSpan span() const noexcept override;
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

struct LambdaExpressionSyntax final : ExpressionSyntax {
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

struct TypeBinaryExpressionSyntax final : ExpressionSyntax {
    std::unique_ptr<ExpressionSyntax> expression;
    SyntaxToken operatorToken;
    TypeSyntax type;
    std::optional<SyntaxToken> designationToken;
    [[nodiscard]] SyntaxKind kind() const noexcept override {
        return SyntaxKind::TypeBinaryExpression;
    }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct TypeOfExpressionSyntax final : ExpressionSyntax {
    SyntaxToken typeofKeyword;
    SyntaxToken openParenToken;
    TypeSyntax type;
    SyntaxToken closeParenToken;
    [[nodiscard]] SyntaxKind kind() const noexcept override {
        return SyntaxKind::TypeOfExpression;
    }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct SwitchExpressionArmSyntax final : SyntaxNode {
    std::unique_ptr<ExpressionSyntax> label;
    std::optional<TypeSyntax> patternType;
    std::optional<SyntaxToken> patternDesignation;
    std::optional<SyntaxToken> discardToken;
    std::optional<SyntaxToken> whenKeyword;
    std::unique_ptr<ExpressionSyntax> guard;
    SyntaxToken arrowToken;
    std::unique_ptr<ExpressionSyntax> value;
    [[nodiscard]] SyntaxKind kind() const noexcept override {
        return SyntaxKind::SwitchExpressionArm;
    }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct SwitchExpressionSyntax final : ExpressionSyntax {
    std::unique_ptr<ExpressionSyntax> expression;
    SyntaxToken switchKeyword;
    SyntaxToken openBraceToken;
    std::vector<SwitchExpressionArmSyntax> arms;
    std::vector<SyntaxToken> commaTokens;
    SyntaxToken closeBraceToken;
    [[nodiscard]] SyntaxKind kind() const noexcept override {
        return SyntaxKind::SwitchExpression;
    }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct ConditionalExpressionSyntax final : ExpressionSyntax {
    std::unique_ptr<ExpressionSyntax> condition;
    SyntaxToken questionToken;
    std::unique_ptr<ExpressionSyntax> whenTrue;
    SyntaxToken colonToken;
    std::unique_ptr<ExpressionSyntax> whenFalse;

    [[nodiscard]] SyntaxKind kind() const noexcept override {
        return SyntaxKind::ConditionalExpression;
    }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct AssignmentExpressionSyntax final : ExpressionSyntax {
    SyntaxToken identifierToken;
    SyntaxToken equalsToken;
    std::unique_ptr<ExpressionSyntax> expression;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::AssignmentExpression; }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct ParenthesizedExpressionSyntax final : ExpressionSyntax {
    SyntaxToken openParenToken;
    std::unique_ptr<ExpressionSyntax> expression;
    SyntaxToken closeParenToken;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::ParenthesizedExpression; }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct CastExpressionSyntax final : ExpressionSyntax {
    SyntaxToken openParenToken;
    TypeSyntax type;
    SyntaxToken closeParenToken;
    std::unique_ptr<ExpressionSyntax> expression;

    [[nodiscard]] SyntaxKind kind() const noexcept override {
        return SyntaxKind::CastExpression;
    }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct CallExpressionSyntax final : ExpressionSyntax {
    SyntaxToken identifierToken;
    std::optional<SyntaxToken> lessToken;
    std::vector<TypeSyntax> typeArguments;
    std::vector<SyntaxToken> typeArgumentCommaTokens;
    std::optional<SyntaxToken> greaterToken;
    SyntaxToken openParenToken;
    std::vector<std::optional<SyntaxToken>> argumentModifiers;
    std::vector<std::optional<SyntaxToken>> argumentNames;
    std::vector<std::optional<SyntaxToken>> argumentNameColonTokens;
    std::vector<std::unique_ptr<ExpressionSyntax>> arguments;
    std::vector<SyntaxToken> commaTokens;
    SyntaxToken closeParenToken;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::CallExpression; }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct ThisExpressionSyntax final : ExpressionSyntax {
    SyntaxToken thisKeyword;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::ThisExpression; }
    [[nodiscard]] text::TextSpan span() const noexcept override { return thisKeyword.span; }
};

struct BaseExpressionSyntax final : ExpressionSyntax {
    SyntaxToken baseKeyword;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::BaseExpression; }
    [[nodiscard]] text::TextSpan span() const noexcept override { return baseKeyword.span; }
};

struct MemberCallExpressionSyntax final : ExpressionSyntax {
    std::unique_ptr<ExpressionSyntax> receiver;
    SyntaxToken dotToken;
    SyntaxToken nameToken;
    std::optional<SyntaxToken> lessToken;
    std::vector<TypeSyntax> typeArguments;
    std::vector<SyntaxToken> typeArgumentCommaTokens;
    std::optional<SyntaxToken> greaterToken;
    SyntaxToken openParenToken;
    std::vector<std::optional<SyntaxToken>> argumentModifiers;
    std::vector<std::optional<SyntaxToken>> argumentNames;
    std::vector<std::optional<SyntaxToken>> argumentNameColonTokens;
    std::vector<std::unique_ptr<ExpressionSyntax>> arguments;
    std::vector<SyntaxToken> commaTokens;
    SyntaxToken closeParenToken;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::MemberCallExpression; }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};


struct MemberAccessExpressionSyntax final : ExpressionSyntax {
    std::unique_ptr<ExpressionSyntax> receiver;
    SyntaxToken dotToken;
    SyntaxToken nameToken;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::MemberAccessExpression; }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};


struct ElementAccessExpressionSyntax final : ExpressionSyntax {
    std::unique_ptr<ExpressionSyntax> receiver;
    SyntaxToken openBracketToken;
    std::unique_ptr<ExpressionSyntax> index;
    SyntaxToken closeBracketToken;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::ElementAccessExpression; }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct ElementAssignmentExpressionSyntax final : ExpressionSyntax {
    std::unique_ptr<ExpressionSyntax> receiver;
    SyntaxToken openBracketToken;
    std::unique_ptr<ExpressionSyntax> index;
    SyntaxToken closeBracketToken;
    SyntaxToken equalsToken;
    std::unique_ptr<ExpressionSyntax> expression;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::ElementAssignmentExpression; }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct MemberAssignmentExpressionSyntax final : ExpressionSyntax {
    std::unique_ptr<ExpressionSyntax> receiver;
    SyntaxToken dotToken;
    SyntaxToken nameToken;
    SyntaxToken equalsToken;
    std::unique_ptr<ExpressionSyntax> expression;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::MemberAssignmentExpression; }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct InitializerElementSyntax final : SyntaxNode {
    std::optional<SyntaxToken> nameToken;
    std::optional<SyntaxToken> equalsToken;
    std::optional<SyntaxToken> openBraceToken;
    std::vector<std::unique_ptr<ExpressionSyntax>> expressions;
    std::vector<SyntaxToken> commaTokens;
    std::optional<SyntaxToken> closeBraceToken;

    [[nodiscard]] SyntaxKind kind() const noexcept override {
        return SyntaxKind::InitializerElement;
    }
    [[nodiscard]] text::TextSpan span() const noexcept override;
    [[nodiscard]] bool isMemberInitializer() const noexcept {
        return nameToken.has_value();
    }
};

struct NewObjectExpressionSyntax final : ExpressionSyntax {
    SyntaxToken newKeyword;
    TypeSyntax type;
    SyntaxToken openParenToken;
    std::vector<std::unique_ptr<ExpressionSyntax>> arguments;
    std::vector<std::optional<SyntaxToken>> argumentNames;
    std::vector<std::optional<SyntaxToken>> argumentNameColonTokens;
    std::vector<SyntaxToken> commaTokens;
    SyntaxToken closeParenToken;
    std::optional<SyntaxToken> initializerOpenBraceToken;
    std::vector<InitializerElementSyntax> initializers;
    std::vector<SyntaxToken> initializerCommaTokens;
    std::optional<SyntaxToken> initializerCloseBraceToken;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::NewObjectExpression; }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};


struct NewArrayExpressionSyntax final : ExpressionSyntax {
    SyntaxToken newKeyword;
    TypeSyntax elementType;
    SyntaxToken openBracketToken;
    std::unique_ptr<ExpressionSyntax> length;
    SyntaxToken closeBracketToken;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::NewArrayExpression; }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct ReturnStatementSyntax final : StatementSyntax {
    SyntaxToken returnKeyword;
    std::optional<SyntaxToken> refKeyword;
    std::unique_ptr<ExpressionSyntax> expression;
    SyntaxToken semicolonToken;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::ReturnStatement; }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct IfStatementSyntax final : StatementSyntax {
    SyntaxToken ifKeyword;
    SyntaxToken openParenToken;
    std::unique_ptr<ExpressionSyntax> condition;
    SyntaxToken closeParenToken;
    std::unique_ptr<StatementSyntax> thenStatement;
    std::optional<SyntaxToken> elseKeyword;
    std::unique_ptr<StatementSyntax> elseStatement;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::IfStatement; }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct WhileStatementSyntax final : StatementSyntax {
    SyntaxToken whileKeyword;
    SyntaxToken openParenToken;
    std::unique_ptr<ExpressionSyntax> condition;
    SyntaxToken closeParenToken;
    std::unique_ptr<StatementSyntax> body;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::WhileStatement; }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct ForStatementSyntax final : StatementSyntax {
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
    std::optional<TypeSyntax> patternType;
    std::optional<SyntaxToken> patternDesignation;
    std::optional<SyntaxToken> whenKeyword;
    std::unique_ptr<ExpressionSyntax> guard;
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

struct ThrowStatementSyntax final : StatementSyntax {
    SyntaxToken throwKeyword;
    std::unique_ptr<ExpressionSyntax> expression;
    SyntaxToken semicolonToken;
    [[nodiscard]] SyntaxKind kind() const noexcept override {
        return SyntaxKind::ThrowStatement;
    }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct YieldWaitStatementSyntax final : StatementSyntax {
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

struct YieldBreakStatementSyntax final : StatementSyntax {
    SyntaxToken yieldKeyword;
    SyntaxToken breakKeyword;
    SyntaxToken semicolonToken;

    [[nodiscard]] SyntaxKind kind() const noexcept override {
        return SyntaxKind::YieldBreakStatement;
    }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct EventSubscriptionStatementSyntax final : StatementSyntax {
    std::unique_ptr<ExpressionSyntax> receiver;
    std::optional<SyntaxToken> dotToken;
    SyntaxToken eventNameToken;
    SyntaxToken operatorToken;
    std::unique_ptr<ExpressionSyntax> handler;
    SyntaxToken semicolonToken;

    [[nodiscard]] SyntaxKind kind() const noexcept override {
        return SyntaxKind::EventSubscriptionStatement;
    }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct VariableDeclarationStatementSyntax final : StatementSyntax {
    std::optional<SyntaxToken> refKeyword;
    TypeSyntax type;
    SyntaxToken identifierToken;
    std::optional<SyntaxToken> equalsToken;
    std::optional<SyntaxToken> initializerRefKeyword;
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

struct CatchClauseSyntax final : SyntaxNode {
    SyntaxToken catchKeyword;
    std::optional<SyntaxToken> openParenToken;
    std::optional<TypeSyntax> type;
    std::optional<SyntaxToken> identifierToken;
    std::optional<SyntaxToken> closeParenToken;
    BlockStatementSyntax body;
    [[nodiscard]] SyntaxKind kind() const noexcept override {
        return SyntaxKind::CatchClause;
    }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct TryStatementSyntax final : StatementSyntax {
    SyntaxToken tryKeyword;
    BlockStatementSyntax body;
    std::vector<CatchClauseSyntax> catches;
    std::optional<SyntaxToken> finallyKeyword;
    std::optional<BlockStatementSyntax> finallyBody;
    [[nodiscard]] SyntaxKind kind() const noexcept override {
        return SyntaxKind::TryStatement;
    }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct ParameterSyntax final : SyntaxNode {
    std::optional<SyntaxToken> paramsKeyword;
    std::optional<SyntaxToken> modifierToken;
    TypeSyntax type;
    SyntaxToken identifierToken;
    std::optional<SyntaxToken> equalsToken;
    std::unique_ptr<ExpressionSyntax> defaultValue;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::Parameter; }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};


struct FieldDeclarationSyntax final : SyntaxNode {
    std::vector<AttributeListSyntax> attributes;
    std::vector<SyntaxToken> modifiers;
    TypeSyntax type;
    SyntaxToken identifierToken;
    SyntaxToken semicolonToken;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::FieldDeclaration; }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct FunctionDeclarationSyntax;
struct SequenceDeclarationSyntax;

struct EventDeclarationSyntax final : SyntaxNode {
    std::vector<AttributeListSyntax> attributes;
    std::vector<SyntaxToken> modifiers;
    SyntaxToken eventKeyword;
    TypeSyntax delegateType;
    SyntaxToken identifierToken;
    SyntaxToken semicolonToken;

    [[nodiscard]] SyntaxKind kind() const noexcept override {
        return SyntaxKind::EventDeclaration;
    }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct ConstructorDeclarationSyntax final : SyntaxNode {
    std::vector<AttributeListSyntax> attributes;
    std::vector<SyntaxToken> modifiers;
    SyntaxToken identifierToken;
    SyntaxToken openParenToken;
    std::vector<ParameterSyntax> parameters;
    std::vector<SyntaxToken> commaTokens;
    SyntaxToken closeParenToken;
    std::optional<SyntaxToken> initializerColonToken;
    std::optional<SyntaxToken> baseKeyword;
    std::optional<SyntaxToken> initializerOpenParenToken;
    std::vector<std::unique_ptr<ExpressionSyntax>> baseArguments;
    std::vector<SyntaxToken> baseArgumentCommaTokens;
    std::optional<SyntaxToken> initializerCloseParenToken;
    BlockStatementSyntax body;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::ConstructorDeclaration; }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct AccessorDeclarationSyntax final : SyntaxNode {
    SyntaxToken keyword;
    std::optional<SyntaxToken> semicolonToken;
    std::unique_ptr<BlockStatementSyntax> body;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::AccessorDeclaration; }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct PropertyDeclarationSyntax final : SyntaxNode {
    std::vector<AttributeListSyntax> attributes;
    std::vector<SyntaxToken> modifiers;
    std::optional<SyntaxToken> staticKeyword;
    TypeSyntax type;
    SyntaxToken identifierToken;
    SyntaxToken openBraceToken;
    std::unique_ptr<AccessorDeclarationSyntax> getter;
    std::unique_ptr<AccessorDeclarationSyntax> setter;
    SyntaxToken closeBraceToken;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::PropertyDeclaration; }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct GenericConstraintClauseSyntax final : SyntaxNode {
    SyntaxToken whereKeyword;
    SyntaxToken typeParameter;
    SyntaxToken colonToken;
    std::vector<SyntaxToken> constraints;

    [[nodiscard]] SyntaxKind kind() const noexcept override {
        return SyntaxKind::GenericConstraintClause;
    }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct ClassDeclarationSyntax final : SyntaxNode {
    std::vector<AttributeListSyntax> attributes;
    std::vector<SyntaxToken> modifiers;
    SyntaxToken classKeyword;
    SyntaxToken identifierToken;
    std::optional<SyntaxToken> typeParameterLessToken;
    std::vector<SyntaxToken> typeParameters;
    std::vector<SyntaxToken> typeParameterCommaTokens;
    std::optional<SyntaxToken> typeParameterGreaterToken;
    std::vector<GenericConstraintClauseSyntax> constraints;
    std::optional<SyntaxToken> colonToken;
    std::vector<TypeSyntax> interfaces;
    std::vector<SyntaxToken> interfaceCommaTokens;
    SyntaxToken openBraceToken;
    std::vector<FieldDeclarationSyntax> fields;
    std::vector<EventDeclarationSyntax> events;
    std::vector<FunctionDeclarationSyntax> methods;
    std::vector<SequenceDeclarationSyntax> sequences;
    std::vector<ConstructorDeclarationSyntax> constructors;
    std::vector<PropertyDeclarationSyntax> properties;
    SyntaxToken closeBraceToken;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::ClassDeclaration; }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct StructDeclarationSyntax final : SyntaxNode {
    std::vector<AttributeListSyntax> attributes;
    std::vector<SyntaxToken> modifiers;
    SyntaxToken structKeyword;
    SyntaxToken identifierToken;
    std::optional<SyntaxToken> typeParameterLessToken;
    std::vector<SyntaxToken> typeParameters;
    std::vector<SyntaxToken> typeParameterCommaTokens;
    std::optional<SyntaxToken> typeParameterGreaterToken;
    std::vector<GenericConstraintClauseSyntax> constraints;
    std::optional<SyntaxToken> colonToken;
    std::vector<TypeSyntax> interfaces;
    std::vector<SyntaxToken> interfaceCommaTokens;
    SyntaxToken openBraceToken;
    std::vector<FieldDeclarationSyntax> fields;
    std::vector<EventDeclarationSyntax> events;
    std::vector<FunctionDeclarationSyntax> methods;
    std::vector<SequenceDeclarationSyntax> sequences;
    std::vector<ConstructorDeclarationSyntax> constructors;
    std::vector<PropertyDeclarationSyntax> properties;
    SyntaxToken closeBraceToken;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::StructDeclaration; }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct InterfaceMethodDeclarationSyntax final : SyntaxNode {
    std::vector<AttributeListSyntax> attributes;
    std::vector<SyntaxToken> modifiers;
    TypeSyntax returnType;
    SyntaxToken identifierToken;
    SyntaxToken openParenToken;
    std::vector<ParameterSyntax> parameters;
    std::vector<SyntaxToken> commaTokens;
    SyntaxToken closeParenToken;
    SyntaxToken semicolonToken;

    [[nodiscard]] SyntaxKind kind() const noexcept override {
        return SyntaxKind::InterfaceMethodDeclaration;
    }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct InterfaceDeclarationSyntax final : SyntaxNode {
    std::vector<AttributeListSyntax> attributes;
    std::vector<SyntaxToken> modifiers;
    SyntaxToken interfaceKeyword;
    SyntaxToken identifierToken;
    std::optional<SyntaxToken> typeParameterLessToken;
    std::vector<SyntaxToken> typeParameters;
    std::vector<SyntaxToken> typeParameterCommaTokens;
    std::optional<SyntaxToken> typeParameterGreaterToken;
    std::vector<GenericConstraintClauseSyntax> constraints;
    SyntaxToken openBraceToken;
    std::vector<InterfaceMethodDeclarationSyntax> methods;
    SyntaxToken closeBraceToken;

    [[nodiscard]] SyntaxKind kind() const noexcept override {
        return SyntaxKind::InterfaceDeclaration;
    }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct EnumMemberDeclarationSyntax final : SyntaxNode {
    std::vector<AttributeListSyntax> attributes;
    SyntaxToken identifierToken;
    std::optional<SyntaxToken> equalsToken;
    std::optional<SyntaxToken> valueToken;
    std::optional<SyntaxToken> commaToken;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::EnumMemberDeclaration; }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct EnumDeclarationSyntax final : SyntaxNode {
    std::vector<AttributeListSyntax> attributes;
    std::vector<SyntaxToken> modifiers;
    SyntaxToken enumKeyword;
    SyntaxToken identifierToken;
    SyntaxToken openBraceToken;
    std::vector<EnumMemberDeclarationSyntax> members;
    SyntaxToken closeBraceToken;

    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::EnumDeclaration; }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct FunctionDeclarationSyntax final : SyntaxNode {
    std::vector<AttributeListSyntax> attributes;
    std::vector<SyntaxToken> modifiers;
    std::optional<SyntaxToken> staticKeyword;
    std::optional<SyntaxToken> refReturnKeyword;
    TypeSyntax returnType;
    SyntaxToken identifierToken;
    std::optional<SyntaxToken> typeParameterLessToken;
    std::vector<SyntaxToken> typeParameters;
    std::vector<SyntaxToken> typeParameterCommaTokens;
    std::optional<SyntaxToken> typeParameterGreaterToken;
    std::vector<GenericConstraintClauseSyntax> constraints;
    SyntaxToken openParenToken;
    std::vector<ParameterSyntax> parameters;
    std::vector<SyntaxToken> commaTokens;
    SyntaxToken closeParenToken;
    std::optional<SyntaxToken> semicolonToken;
    BlockStatementSyntax body;

    [[nodiscard]] bool hasBody() const noexcept { return !semicolonToken.has_value(); }
    [[nodiscard]] SyntaxKind kind() const noexcept override { return SyntaxKind::FunctionDeclaration; }
    [[nodiscard]] text::TextSpan span() const noexcept override;
};

struct DelegateDeclarationSyntax final : SyntaxNode {
    std::vector<AttributeListSyntax> attributes;
    std::vector<SyntaxToken> modifiers;
    SyntaxToken delegateKeyword;
    TypeSyntax returnType;
    SyntaxToken identifierToken;
    std::optional<SyntaxToken> typeParameterLessToken;
    std::vector<SyntaxToken> typeParameters;
    std::vector<SyntaxToken> typeParameterCommaTokens;
    std::optional<SyntaxToken> typeParameterGreaterToken;
    std::vector<GenericConstraintClauseSyntax> constraints;
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

struct SequenceDeclarationSyntax final : SyntaxNode {
    std::vector<AttributeListSyntax> attributes;
    std::vector<SyntaxToken> modifiers;
    SyntaxToken sequenceKeyword;
    std::optional<SyntaxToken> resultLessToken;
    std::unique_ptr<TypeSyntax> resultType;
    std::optional<SyntaxToken> resultGreaterToken;
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
    std::vector<ClassDeclarationSyntax> classes;
    std::vector<StructDeclarationSyntax> structs;
    std::vector<EnumDeclarationSyntax> enums;
    std::vector<InterfaceDeclarationSyntax> interfaces;
    std::vector<DelegateDeclarationSyntax> delegates;
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
    [[nodiscard]] std::vector<AttributeListSyntax> parseAttributeLists();
    [[nodiscard]] std::vector<SyntaxToken> parseModifiers();
    [[nodiscard]] AttributeListSyntax parseAttributeList();
    [[nodiscard]] AttributeSyntax parseAttribute();
    [[nodiscard]] AttributeArgumentSyntax parseAttributeArgument();
    [[nodiscard]] ClassDeclarationSyntax parseClassDeclaration(
        std::vector<AttributeListSyntax> attributes,
        std::vector<SyntaxToken> modifiers);
    [[nodiscard]] StructDeclarationSyntax parseStructDeclaration(
        std::vector<AttributeListSyntax> attributes,
        std::vector<SyntaxToken> modifiers);
    [[nodiscard]] EnumDeclarationSyntax parseEnumDeclaration(
        std::vector<AttributeListSyntax> attributes,
        std::vector<SyntaxToken> modifiers);
    [[nodiscard]] InterfaceDeclarationSyntax parseInterfaceDeclaration(
        std::vector<AttributeListSyntax> attributes,
        std::vector<SyntaxToken> modifiers);
    [[nodiscard]] InterfaceMethodDeclarationSyntax parseInterfaceMethodDeclaration(
        std::vector<AttributeListSyntax> attributes,
        std::vector<SyntaxToken> modifiers = {});
    [[nodiscard]] DelegateDeclarationSyntax parseDelegateDeclaration(
        std::vector<AttributeListSyntax> attributes,
        std::vector<SyntaxToken> modifiers);
    [[nodiscard]] EventDeclarationSyntax parseEventDeclaration(
        std::vector<AttributeListSyntax> attributes,
        std::vector<SyntaxToken> modifiers);
    void parseInterfaceList(
        std::optional<SyntaxToken>& colonToken,
        std::vector<TypeSyntax>& interfaces,
        std::vector<SyntaxToken>& commaTokens);
    [[nodiscard]] FieldDeclarationSyntax parseFieldDeclaration(
        TypeSyntax type,
        SyntaxToken identifier,
        std::vector<AttributeListSyntax> attributes,
        std::vector<SyntaxToken> modifiers);
    [[nodiscard]] FunctionDeclarationSyntax parseFunctionDeclaration(
        std::vector<SyntaxToken> modifiers = {},
        std::optional<TypeSyntax> returnType = std::nullopt,
        std::optional<SyntaxToken> identifier = std::nullopt,
        std::vector<AttributeListSyntax> attributes = {});
    [[nodiscard]] SequenceDeclarationSyntax parseSequenceDeclaration(
        std::vector<AttributeListSyntax> attributes,
        std::vector<SyntaxToken> modifiers);
    [[nodiscard]] ConstructorDeclarationSyntax parseConstructorDeclaration(
        std::vector<AttributeListSyntax> attributes,
        std::vector<SyntaxToken> modifiers);
    [[nodiscard]] PropertyDeclarationSyntax parsePropertyDeclaration(
        std::vector<SyntaxToken> modifiers,
        TypeSyntax type,
        SyntaxToken identifier,
        std::vector<AttributeListSyntax> attributes);
    [[nodiscard]] AccessorDeclarationSyntax parseAccessorDeclaration();
    [[nodiscard]] TypeSyntax parseType();
    void parseTypeArgumentList(
        std::optional<SyntaxToken>& lessToken,
        std::vector<TypeSyntax>& arguments,
        std::vector<SyntaxToken>& commaTokens,
        std::optional<SyntaxToken>& greaterToken);
    void parseTypeParameterList(
        std::optional<SyntaxToken>& lessToken,
        std::vector<SyntaxToken>& parameters,
        std::vector<SyntaxToken>& commaTokens,
        std::optional<SyntaxToken>& greaterToken);
    void parseGenericConstraintClauses(
        std::vector<GenericConstraintClauseSyntax>& clauses);
    [[nodiscard]] bool isGenericCallStart() const noexcept;
    [[nodiscard]] ParameterSyntax parseParameter();
    [[nodiscard]] BlockStatementSyntax parseBlockStatement();
    [[nodiscard]] std::unique_ptr<StatementSyntax> parseStatement();
    [[nodiscard]] std::unique_ptr<StatementSyntax> parseReturnStatement();
    [[nodiscard]] std::unique_ptr<StatementSyntax> parseIfStatement();
    [[nodiscard]] std::unique_ptr<StatementSyntax> parseWhileStatement();
    [[nodiscard]] std::unique_ptr<StatementSyntax> parseForStatement();
    [[nodiscard]] std::unique_ptr<StatementSyntax> parseForeachStatement();
    [[nodiscard]] std::unique_ptr<StatementSyntax> parseDoWhileStatement();
    [[nodiscard]] std::unique_ptr<StatementSyntax> parseBreakStatement();
    [[nodiscard]] std::unique_ptr<StatementSyntax> parseContinueStatement();
    [[nodiscard]] std::unique_ptr<StatementSyntax> parseSwitchStatement();
    [[nodiscard]] std::unique_ptr<StatementSyntax> parseThrowStatement();
    [[nodiscard]] std::unique_ptr<StatementSyntax> parseTryStatement();
    [[nodiscard]] std::unique_ptr<StatementSyntax> parseYieldStatement();
    [[nodiscard]] std::unique_ptr<StatementSyntax> parseEventSubscriptionStatement();
    [[nodiscard]] std::unique_ptr<ExpressionSyntax> parseEventHandler();
    [[nodiscard]] bool isLambdaExpressionStart() const noexcept;
    [[nodiscard]] std::unique_ptr<StatementSyntax> parseVariableDeclarationStatement();
    [[nodiscard]] std::unique_ptr<StatementSyntax> parseExpressionStatement();
    [[nodiscard]] std::unique_ptr<ExpressionSyntax> parseExpression();
    [[nodiscard]] std::unique_ptr<ExpressionSyntax> parseAssignmentExpression();
    [[nodiscard]] std::unique_ptr<ExpressionSyntax> parseConditionalExpression();
    [[nodiscard]] std::unique_ptr<ExpressionSyntax> parseNullCoalescingExpression();
    [[nodiscard]] std::unique_ptr<ExpressionSyntax> parseBinaryExpression(int parentPrecedence = 0);
    [[nodiscard]] std::unique_ptr<ExpressionSyntax> parsePrimaryExpression();
    [[nodiscard]] std::unique_ptr<ExpressionSyntax> parsePostfixExpression();
    [[nodiscard]] std::unique_ptr<ExpressionSyntax> parseCallExpression();
    [[nodiscard]] std::unique_ptr<ExpressionSyntax> parseNewExpression();
    void parseArgumentList(
        std::vector<std::unique_ptr<ExpressionSyntax>>& arguments,
        std::vector<std::optional<SyntaxToken>>* argumentModifiers,
        std::vector<SyntaxToken>& commaTokens,
        SyntaxToken& closeParenToken,
        std::vector<std::optional<SyntaxToken>>* argumentNames = nullptr,
        std::vector<std::optional<SyntaxToken>>* argumentNameColonTokens = nullptr);
    [[nodiscard]] bool isVariableDeclarationStart() const noexcept;

    const text::SourceText& source_;
    diagnostics::DiagnosticBag& diagnostics_;
    std::vector<SyntaxToken> tokens_;
    std::size_t position_ = 0;
};

} // namespace realscript::syntax
