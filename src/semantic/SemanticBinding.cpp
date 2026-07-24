#include "realscript/semantic/Semantic.h"
#include "FlowAnalysis.h"

#include <unordered_set>

namespace realscript::semantic {

const char* primitiveTypeName(PrimitiveType type) noexcept {
    switch (type) {
    case PrimitiveType::Error: return "<error>";
    case PrimitiveType::Void: return "void";
    case PrimitiveType::Bool: return "bool";
    case PrimitiveType::Int: return "int";
    case PrimitiveType::String: return "string";
    case PrimitiveType::Null: return "null";
    }
    return "<unknown>";
}

PrimitiveType resolvePrimitiveType(const std::string& name) noexcept {
    if (name == "void") return PrimitiveType::Void;
    if (name == "bool") return PrimitiveType::Bool;
    if (name == "int") return PrimitiveType::Int;
    if (name == "string") return PrimitiveType::String;
    return PrimitiveType::Error;
}

bool isNumericType(PrimitiveType type) noexcept {
    return type == PrimitiveType::Int;
}

Binder::Binder(diagnostics::DiagnosticBag& diagnostics)
    : diagnostics_(diagnostics) {}

SemanticModel Binder::bind(const syntax::CompilationUnitSyntax& syntaxTree) {
    SemanticModel model;
    if (syntaxTree.moduleDeclaration) {
        model.moduleName = syntaxTree.moduleDeclaration->fullName();
    }

    std::unordered_set<std::string> functionNames;
    for (const auto& functionSyntax : syntaxTree.functions) {
        if (!functionNames.insert(functionSyntax.identifierToken.text).second) {
            diagnostics_.report(
                "RS2000",
                "function '" + functionSyntax.identifierToken.text + "' is already declared",
                functionSyntax.identifierToken.span);
        }
        model.functions.push_back(bindFunction(functionSyntax));
    }
    return model;
}

BoundFunction Binder::bindFunction(const syntax::FunctionDeclarationSyntax& syntaxTree) {
    BoundFunction result;
    result.symbol.name = syntaxTree.identifierToken.text;
    result.symbol.returnType = bindType(syntaxTree.returnType, true);

    scopes_.clear();
    pushScope();
    currentReturnType_ = result.symbol.returnType;
    nextVariableIndex_ = 0;

    for (const auto& parameterSyntax : syntaxTree.parameters) {
        VariableSymbol parameter;
        parameter.name = parameterSyntax.identifierToken.text;
        parameter.type = bindType(parameterSyntax.type, false);
        parameter.index = nextVariableIndex_++;
        parameter.parameter = true;
        if (declareVariable(parameter, parameterSyntax.identifierToken.span)) {
            result.symbol.parameters.push_back(parameter);
        }
    }

    result.body = bindBlockStatement(syntaxTree.body, false);
    result.variableCount = nextVariableIndex_;

    if (result.symbol.returnType != PrimitiveType::Void &&
        result.symbol.returnType != PrimitiveType::Error &&
        detail::canReachFunctionEnd(result, diagnostics_)) {
        diagnostics_.report(
            "RS2001",
            "not all control-flow paths in function '" + result.symbol.name + "' return a value",
            syntaxTree.identifierToken.span);
    }

    popScope();
    return result;
}

std::unique_ptr<BoundBlockStatement> Binder::bindBlockStatement(
    const syntax::BlockStatementSyntax& syntaxTree,
    bool createScope) {
    if (createScope) {
        pushScope();
    }

    auto result = std::make_unique<BoundBlockStatement>();
    result->span = syntaxTree.span();
    for (const auto& statement : syntaxTree.statements) {
        result->statements.push_back(bindStatement(*statement));
    }

    if (createScope) {
        popScope();
    }
    return result;
}

std::unique_ptr<BoundStatement> Binder::bindEmbeddedStatement(
    const syntax::StatementSyntax& syntaxTree) {
    pushScope();
    auto result = bindStatement(syntaxTree);
    popScope();
    return result;
}

std::unique_ptr<BoundStatement> Binder::bindStatement(const syntax::StatementSyntax& syntaxTree) {
    switch (syntaxTree.kind()) {
    case syntax::SyntaxKind::BlockStatement:
        return bindBlockStatement(
            static_cast<const syntax::BlockStatementSyntax&>(syntaxTree), true);
    case syntax::SyntaxKind::ReturnStatement:
        return bindReturnStatement(
            static_cast<const syntax::ReturnStatementSyntax&>(syntaxTree));
    case syntax::SyntaxKind::IfStatement:
        return bindIfStatement(static_cast<const syntax::IfStatementSyntax&>(syntaxTree));
    case syntax::SyntaxKind::WhileStatement:
        return bindWhileStatement(static_cast<const syntax::WhileStatementSyntax&>(syntaxTree));
    case syntax::SyntaxKind::VariableDeclarationStatement:
        return bindVariableDeclaration(
            static_cast<const syntax::VariableDeclarationStatementSyntax&>(syntaxTree));
    case syntax::SyntaxKind::ExpressionStatement:
        return bindExpressionStatement(
            static_cast<const syntax::ExpressionStatementSyntax&>(syntaxTree));
    default:
        diagnostics_.report("RS2099", "unsupported statement kind", syntaxTree.span());
        return std::make_unique<BoundExpressionStatement>();
    }
}

std::unique_ptr<BoundStatement> Binder::bindReturnStatement(
    const syntax::ReturnStatementSyntax& syntaxTree) {
    auto result = std::make_unique<BoundReturnStatement>();
    result->span = syntaxTree.span();

    if (currentReturnType_ == PrimitiveType::Void) {
        if (syntaxTree.expression) {
            diagnostics_.report("RS2002", "void function cannot return a value", syntaxTree.expression->span());
            result->expression = bindExpression(*syntaxTree.expression);
        }
        return result;
    }

    if (!syntaxTree.expression) {
        diagnostics_.report(
            "RS2003",
            "function returning '" + std::string(primitiveTypeName(currentReturnType_)) +
                "' must return a value",
            syntaxTree.returnKeyword.span);
        return result;
    }

    result->expression = bindExpression(*syntaxTree.expression);
    if (result->expression->type != PrimitiveType::Error &&
        currentReturnType_ != PrimitiveType::Error &&
        result->expression->type != currentReturnType_) {
        diagnostics_.report(
            "RS2004",
            "cannot return '" + std::string(primitiveTypeName(result->expression->type)) +
                "' from function returning '" + primitiveTypeName(currentReturnType_) + "'",
            syntaxTree.expression->span());
    }
    return result;
}

std::unique_ptr<BoundStatement> Binder::bindIfStatement(
    const syntax::IfStatementSyntax& syntaxTree) {
    auto result = std::make_unique<BoundIfStatement>();
    result->span = syntaxTree.span();
    result->condition = bindExpression(*syntaxTree.condition);
    if (result->condition->type != PrimitiveType::Bool &&
        result->condition->type != PrimitiveType::Error) {
        diagnostics_.report("RS2007", "if condition must have type 'bool'", syntaxTree.condition->span());
    }
    result->thenStatement = bindEmbeddedStatement(*syntaxTree.thenStatement);
    if (syntaxTree.elseStatement) {
        result->elseStatement = bindEmbeddedStatement(*syntaxTree.elseStatement);
    }
    return result;
}

std::unique_ptr<BoundStatement> Binder::bindWhileStatement(
    const syntax::WhileStatementSyntax& syntaxTree) {
    auto result = std::make_unique<BoundWhileStatement>();
    result->span = syntaxTree.span();
    result->condition = bindExpression(*syntaxTree.condition);
    if (result->condition->type != PrimitiveType::Bool &&
        result->condition->type != PrimitiveType::Error) {
        diagnostics_.report("RS2008", "while condition must have type 'bool'", syntaxTree.condition->span());
    }
    result->body = bindEmbeddedStatement(*syntaxTree.body);
    return result;
}

std::unique_ptr<BoundStatement> Binder::bindVariableDeclaration(
    const syntax::VariableDeclarationStatementSyntax& syntaxTree) {
    auto result = std::make_unique<BoundVariableDeclarationStatement>();
    result->span = syntaxTree.span();
    result->variable = {
        syntaxTree.identifierToken.text,
        bindType(syntaxTree.type, false),
        nextVariableIndex_++,
        false,
    };

    (void)declareVariable(result->variable, syntaxTree.identifierToken.span);

    if (syntaxTree.initializer) {
        result->initializer = bindExpression(*syntaxTree.initializer);
        if (result->initializer->type != PrimitiveType::Error &&
            result->variable.type != PrimitiveType::Error &&
            result->initializer->type != result->variable.type) {
            diagnostics_.report(
                "RS2006",
                "cannot initialize '" + std::string(primitiveTypeName(result->variable.type)) +
                    "' with '" + primitiveTypeName(result->initializer->type) + "'",
                syntaxTree.initializer->span());
        }
    }
    return result;
}

std::unique_ptr<BoundStatement> Binder::bindExpressionStatement(
    const syntax::ExpressionStatementSyntax& syntaxTree) {
    auto result = std::make_unique<BoundExpressionStatement>();
    result->span = syntaxTree.span();
    result->expression = bindExpression(*syntaxTree.expression);
    return result;
}

std::unique_ptr<BoundExpression> Binder::bindExpression(
    const syntax::ExpressionSyntax& syntaxTree) {
    switch (syntaxTree.kind()) {
    case syntax::SyntaxKind::LiteralExpression:
        return bindLiteralExpression(
            static_cast<const syntax::LiteralExpressionSyntax&>(syntaxTree));
    case syntax::SyntaxKind::NameExpression:
        return bindNameExpression(
            static_cast<const syntax::NameExpressionSyntax&>(syntaxTree));
    case syntax::SyntaxKind::UnaryExpression:
        return bindUnaryExpression(
            static_cast<const syntax::UnaryExpressionSyntax&>(syntaxTree));
    case syntax::SyntaxKind::BinaryExpression:
        return bindBinaryExpression(
            static_cast<const syntax::BinaryExpressionSyntax&>(syntaxTree));
    case syntax::SyntaxKind::AssignmentExpression:
        return bindAssignmentExpression(
            static_cast<const syntax::AssignmentExpressionSyntax&>(syntaxTree));
    case syntax::SyntaxKind::ParenthesizedExpression:
        return bindExpression(
            *static_cast<const syntax::ParenthesizedExpressionSyntax&>(syntaxTree).expression);
    case syntax::SyntaxKind::CallExpression:
        diagnostics_.report(
            "RS2100",
            "function calls are parsed but not bound in the Phase 1B profile",
            syntaxTree.span());
        return makeError(syntaxTree.span());
    default:
        diagnostics_.report("RS2199", "unsupported expression kind", syntaxTree.span());
        return makeError(syntaxTree.span());
    }
}


} // namespace realscript::semantic
