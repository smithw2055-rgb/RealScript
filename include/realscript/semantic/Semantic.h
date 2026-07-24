#pragma once

#include "realscript/diagnostics/Diagnostic.h"
#include "realscript/syntax/Syntax.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace realscript::semantic {

enum class PrimitiveType {
    Error,
    Void,
    Bool,
    Int,
    String,
    Null,
};

[[nodiscard]] const char* primitiveTypeName(PrimitiveType type) noexcept;
[[nodiscard]] PrimitiveType resolvePrimitiveType(const std::string& name) noexcept;
[[nodiscard]] bool isNumericType(PrimitiveType type) noexcept;

struct VariableSymbol {
    std::string name;
    PrimitiveType type = PrimitiveType::Error;
    std::size_t index = 0;
    bool parameter = false;
};

struct FunctionSymbol {
    std::string name;
    PrimitiveType returnType = PrimitiveType::Error;
    std::vector<VariableSymbol> parameters;
};

enum class BoundNodeKind {
    ErrorExpression,
    LiteralExpression,
    VariableExpression,
    UnaryExpression,
    BinaryExpression,
    BlockStatement,
    ReturnStatement,
    VariableDeclarationStatement,
    ExpressionStatement,
};

struct BoundNode {
    virtual ~BoundNode() = default;
    [[nodiscard]] virtual BoundNodeKind kind() const noexcept = 0;
};

struct BoundExpression : BoundNode {
    PrimitiveType type = PrimitiveType::Error;
    text::TextSpan span;
};

struct BoundStatement : BoundNode {
    text::TextSpan span;
};

struct BoundErrorExpression final : BoundExpression {
    [[nodiscard]] BoundNodeKind kind() const noexcept override { return BoundNodeKind::ErrorExpression; }
};

struct BoundLiteralExpression final : BoundExpression {
    syntax::TokenValue value;
    [[nodiscard]] BoundNodeKind kind() const noexcept override { return BoundNodeKind::LiteralExpression; }
};

struct BoundVariableExpression final : BoundExpression {
    VariableSymbol variable;
    [[nodiscard]] BoundNodeKind kind() const noexcept override { return BoundNodeKind::VariableExpression; }
};

enum class BoundUnaryOperatorKind {
    Identity,
    Negation,
    LogicalNegation,
};

struct BoundUnaryExpression final : BoundExpression {
    BoundUnaryOperatorKind operatorKind = BoundUnaryOperatorKind::Identity;
    std::unique_ptr<BoundExpression> operand;
    [[nodiscard]] BoundNodeKind kind() const noexcept override { return BoundNodeKind::UnaryExpression; }
};

enum class BoundBinaryOperatorKind {
    Addition,
    Subtraction,
    Multiplication,
    Division,
    Remainder,
    Equals,
    NotEquals,
    Less,
    LessOrEquals,
    Greater,
    GreaterOrEquals,
    LogicalAnd,
    LogicalOr,
};

struct BoundBinaryExpression final : BoundExpression {
    BoundBinaryOperatorKind operatorKind = BoundBinaryOperatorKind::Addition;
    std::unique_ptr<BoundExpression> left;
    std::unique_ptr<BoundExpression> right;
    [[nodiscard]] BoundNodeKind kind() const noexcept override { return BoundNodeKind::BinaryExpression; }
};

struct BoundBlockStatement final : BoundStatement {
    std::vector<std::unique_ptr<BoundStatement>> statements;
    [[nodiscard]] BoundNodeKind kind() const noexcept override { return BoundNodeKind::BlockStatement; }
};

struct BoundReturnStatement final : BoundStatement {
    std::unique_ptr<BoundExpression> expression;
    [[nodiscard]] BoundNodeKind kind() const noexcept override { return BoundNodeKind::ReturnStatement; }
};

struct BoundVariableDeclarationStatement final : BoundStatement {
    VariableSymbol variable;
    std::unique_ptr<BoundExpression> initializer;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::VariableDeclarationStatement;
    }
};

struct BoundExpressionStatement final : BoundStatement {
    std::unique_ptr<BoundExpression> expression;
    [[nodiscard]] BoundNodeKind kind() const noexcept override { return BoundNodeKind::ExpressionStatement; }
};

struct BoundFunction {
    FunctionSymbol symbol;
    std::unique_ptr<BoundBlockStatement> body;
    std::size_t variableCount = 0;
};

struct SemanticModel {
    std::string moduleName;
    std::vector<BoundFunction> functions;
};

class Binder {
public:
    explicit Binder(diagnostics::DiagnosticBag& diagnostics);

    [[nodiscard]] SemanticModel bind(const syntax::CompilationUnitSyntax& syntax);

private:
    [[nodiscard]] BoundFunction bindFunction(const syntax::FunctionDeclarationSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundBlockStatement> bindBlockStatement(
        const syntax::BlockStatementSyntax& syntax,
        bool createScope);
    [[nodiscard]] std::unique_ptr<BoundStatement> bindStatement(const syntax::StatementSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundStatement> bindReturnStatement(
        const syntax::ReturnStatementSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundStatement> bindVariableDeclaration(
        const syntax::VariableDeclarationStatementSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundStatement> bindExpressionStatement(
        const syntax::ExpressionStatementSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundExpression> bindExpression(
        const syntax::ExpressionSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundExpression> bindLiteralExpression(
        const syntax::LiteralExpressionSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundExpression> bindNameExpression(
        const syntax::NameExpressionSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundExpression> bindUnaryExpression(
        const syntax::UnaryExpressionSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundExpression> bindBinaryExpression(
        const syntax::BinaryExpressionSyntax& syntax);

    [[nodiscard]] PrimitiveType bindType(const syntax::TypeSyntax& syntax, bool allowVoid);
    [[nodiscard]] const VariableSymbol* lookupVariable(const std::string& name) const noexcept;
    bool declareVariable(VariableSymbol variable, text::TextSpan span);
    void pushScope();
    void popScope();
    [[nodiscard]] std::unique_ptr<BoundErrorExpression> makeError(text::TextSpan span) const;

    diagnostics::DiagnosticBag& diagnostics_;
    std::vector<std::unordered_map<std::string, VariableSymbol>> scopes_;
    PrimitiveType currentReturnType_ = PrimitiveType::Error;
    std::size_t nextVariableIndex_ = 0;
    bool sawReturn_ = false;
};

} // namespace realscript::semantic
