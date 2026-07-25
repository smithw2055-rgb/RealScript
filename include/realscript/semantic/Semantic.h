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

using SymbolId = std::uint64_t;

enum class PrimitiveType {
    Error,
    Void,
    Bool,
    Int,
    String,
    Null,
    Object,
};

[[nodiscard]] const char* primitiveTypeName(PrimitiveType type) noexcept;
[[nodiscard]] PrimitiveType resolvePrimitiveType(const std::string& name) noexcept;
[[nodiscard]] bool isNumericType(PrimitiveType type) noexcept;

enum class ConversionKind {
    None,
    Identity,
    NullToString,
};

[[nodiscard]] ConversionKind classifyConversion(
    PrimitiveType from,
    PrimitiveType to) noexcept;
[[nodiscard]] int conversionRank(PrimitiveType from, PrimitiveType to) noexcept;

struct VariableSymbol {
    std::string name;
    PrimitiveType type = PrimitiveType::Error;
    std::size_t index = 0;
    bool parameter = false;
};

struct FunctionSymbol {
    SymbolId id = 0;
    std::string moduleName;
    std::string name;
    PrimitiveType returnType = PrimitiveType::Error;
    std::vector<VariableSymbol> parameters;
};

[[nodiscard]] std::string canonicalFunctionKey(const FunctionSymbol& function);
[[nodiscard]] std::string canonicalFunctionSignature(const FunctionSymbol& function);
[[nodiscard]] SymbolId stableFunctionId(const FunctionSymbol& function);
[[nodiscard]] FunctionSymbol declareFunctionSymbol(
    const std::string& moduleName,
    const syntax::FunctionDeclarationSyntax& syntax,
    diagnostics::DiagnosticBag& diagnostics);

using FunctionOverloadMap =
    std::unordered_map<std::string, std::vector<FunctionSymbol>>;

struct ModuleBindingInput {
    std::string moduleName;
    std::vector<const syntax::CompilationUnitSyntax*> units;
    std::vector<FunctionSymbol> declarations;
    FunctionOverloadMap visibleFunctions;
};

enum class BoundNodeKind {
    ErrorExpression,
    LiteralExpression,
    VariableExpression,
    UnaryExpression,
    BinaryExpression,
    AssignmentExpression,
    ConversionExpression,
    CallExpression,
    BlockStatement,
    ReturnStatement,
    IfStatement,
    WhileStatement,
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
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::ErrorExpression;
    }
};

struct BoundLiteralExpression final : BoundExpression {
    syntax::TokenValue value;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::LiteralExpression;
    }
};

struct BoundVariableExpression final : BoundExpression {
    VariableSymbol variable;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::VariableExpression;
    }
};

enum class BoundUnaryOperatorKind {
    Identity,
    Negation,
    LogicalNegation,
};

struct BoundUnaryExpression final : BoundExpression {
    BoundUnaryOperatorKind operatorKind = BoundUnaryOperatorKind::Identity;
    std::unique_ptr<BoundExpression> operand;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::UnaryExpression;
    }
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
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::BinaryExpression;
    }
};

struct BoundAssignmentExpression final : BoundExpression {
    VariableSymbol variable;
    std::unique_ptr<BoundExpression> expression;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::AssignmentExpression;
    }
};

struct BoundConversionExpression final : BoundExpression {
    ConversionKind conversion = ConversionKind::None;
    std::unique_ptr<BoundExpression> expression;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::ConversionExpression;
    }
};

struct BoundCallExpression final : BoundExpression {
    FunctionSymbol function;
    std::vector<std::unique_ptr<BoundExpression>> arguments;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::CallExpression;
    }
};

struct BoundBlockStatement final : BoundStatement {
    std::vector<std::unique_ptr<BoundStatement>> statements;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::BlockStatement;
    }
};

struct BoundReturnStatement final : BoundStatement {
    std::unique_ptr<BoundExpression> expression;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::ReturnStatement;
    }
};

struct BoundIfStatement final : BoundStatement {
    std::unique_ptr<BoundExpression> condition;
    std::unique_ptr<BoundStatement> thenStatement;
    std::unique_ptr<BoundStatement> elseStatement;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::IfStatement;
    }
};

struct BoundWhileStatement final : BoundStatement {
    std::unique_ptr<BoundExpression> condition;
    std::unique_ptr<BoundStatement> body;
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::WhileStatement;
    }
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
    [[nodiscard]] BoundNodeKind kind() const noexcept override {
        return BoundNodeKind::ExpressionStatement;
    }
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
    [[nodiscard]] SemanticModel bindModule(const ModuleBindingInput& input);

private:
    [[nodiscard]] BoundFunction bindFunction(
        const syntax::FunctionDeclarationSyntax& syntax,
        const FunctionSymbol& symbol);
    [[nodiscard]] std::unique_ptr<BoundBlockStatement> bindBlockStatement(
        const syntax::BlockStatementSyntax& syntax,
        bool createScope);
    [[nodiscard]] std::unique_ptr<BoundStatement> bindEmbeddedStatement(
        const syntax::StatementSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundStatement> bindStatement(
        const syntax::StatementSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundStatement> bindReturnStatement(
        const syntax::ReturnStatementSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundStatement> bindIfStatement(
        const syntax::IfStatementSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundStatement> bindWhileStatement(
        const syntax::WhileStatementSyntax& syntax);
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
    [[nodiscard]] std::unique_ptr<BoundExpression> bindAssignmentExpression(
        const syntax::AssignmentExpressionSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundExpression> bindCallExpression(
        const syntax::CallExpressionSyntax& syntax);
    [[nodiscard]] std::unique_ptr<BoundExpression> convertExpression(
        std::unique_ptr<BoundExpression> expression,
        PrimitiveType target,
        text::TextSpan span,
        const std::string& context);

    [[nodiscard]] PrimitiveType bindType(
        const syntax::TypeSyntax& syntax,
        bool allowVoid);
    [[nodiscard]] const VariableSymbol* lookupVariable(
        const std::string& name) const noexcept;
    bool declareVariable(VariableSymbol variable, text::TextSpan span);
    void pushScope();
    void popScope();
    [[nodiscard]] std::unique_ptr<BoundErrorExpression> makeError(
        text::TextSpan span) const;

    diagnostics::DiagnosticBag& diagnostics_;
    std::vector<std::unordered_map<std::string, VariableSymbol>> scopes_;
    PrimitiveType currentReturnType_ = PrimitiveType::Error;
    std::size_t nextVariableIndex_ = 0;
    FunctionOverloadMap visibleFunctions_;
};

} // namespace realscript::semantic
