#include "realscript/semantic/Semantic.h"

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
    sawReturn_ = false;

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
        result.symbol.returnType != PrimitiveType::Error && !sawReturn_) {
        diagnostics_.report(
            "RS2001",
            "non-void function '" + result.symbol.name + "' must return a value",
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

std::unique_ptr<BoundStatement> Binder::bindStatement(const syntax::StatementSyntax& syntaxTree) {
    switch (syntaxTree.kind()) {
    case syntax::SyntaxKind::BlockStatement:
        return bindBlockStatement(
            static_cast<const syntax::BlockStatementSyntax&>(syntaxTree), true);
    case syntax::SyntaxKind::ReturnStatement:
        return bindReturnStatement(
            static_cast<const syntax::ReturnStatementSyntax&>(syntaxTree));
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
    sawReturn_ = true;

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

    if (!syntaxTree.initializer) {
        diagnostics_.report(
            "RS2005",
            "local variable must have an initializer in the Phase 1A profile",
            syntaxTree.identifierToken.span);
        result->initializer = makeError(syntaxTree.identifierToken.span);
    } else {
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

    declareVariable(result->variable, syntaxTree.identifierToken.span);
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
    case syntax::SyntaxKind::ParenthesizedExpression:
        return bindExpression(
            *static_cast<const syntax::ParenthesizedExpressionSyntax&>(syntaxTree).expression);
    case syntax::SyntaxKind::CallExpression:
        diagnostics_.report(
            "RS2100",
            "function calls are parsed but not bound in the Phase 1A profile",
            syntaxTree.span());
        return makeError(syntaxTree.span());
    default:
        diagnostics_.report("RS2199", "unsupported expression kind", syntaxTree.span());
        return makeError(syntaxTree.span());
    }
}

std::unique_ptr<BoundExpression> Binder::bindLiteralExpression(
    const syntax::LiteralExpressionSyntax& syntaxTree) {
    auto result = std::make_unique<BoundLiteralExpression>();
    result->span = syntaxTree.span();
    result->value = syntaxTree.literalToken.value;

    switch (syntaxTree.literalToken.kind) {
    case syntax::SyntaxKind::IntegerLiteralToken:
        result->type = PrimitiveType::Int;
        break;
    case syntax::SyntaxKind::StringLiteralToken:
        result->type = PrimitiveType::String;
        break;
    case syntax::SyntaxKind::TrueKeyword:
    case syntax::SyntaxKind::FalseKeyword:
        result->type = PrimitiveType::Bool;
        break;
    case syntax::SyntaxKind::NullKeyword:
        result->type = PrimitiveType::Null;
        break;
    case syntax::SyntaxKind::FloatLiteralToken:
        diagnostics_.report(
            "RS2101",
            "floating-point binding is reserved for the next language slice",
            syntaxTree.span());
        result->type = PrimitiveType::Error;
        break;
    default:
        result->type = PrimitiveType::Error;
        break;
    }
    return result;
}

std::unique_ptr<BoundExpression> Binder::bindNameExpression(
    const syntax::NameExpressionSyntax& syntaxTree) {
    const auto* variable = lookupVariable(syntaxTree.identifierToken.text);
    if (!variable) {
        diagnostics_.report(
            "RS2102",
            "undefined name '" + syntaxTree.identifierToken.text + "'",
            syntaxTree.identifierToken.span);
        return makeError(syntaxTree.span());
    }

    auto result = std::make_unique<BoundVariableExpression>();
    result->span = syntaxTree.span();
    result->type = variable->type;
    result->variable = *variable;
    return result;
}

std::unique_ptr<BoundExpression> Binder::bindUnaryExpression(
    const syntax::UnaryExpressionSyntax& syntaxTree) {
    auto operand = bindExpression(*syntaxTree.operand);
    if (operand->type == PrimitiveType::Error) {
        return makeError(syntaxTree.span());
    }

    BoundUnaryOperatorKind operatorKind;
    PrimitiveType resultType;
    switch (syntaxTree.operatorToken.kind) {
    case syntax::SyntaxKind::PlusToken:
        operatorKind = BoundUnaryOperatorKind::Identity;
        resultType = PrimitiveType::Int;
        if (operand->type != PrimitiveType::Int) goto invalid_operator;
        break;
    case syntax::SyntaxKind::MinusToken:
        operatorKind = BoundUnaryOperatorKind::Negation;
        resultType = PrimitiveType::Int;
        if (operand->type != PrimitiveType::Int) goto invalid_operator;
        break;
    case syntax::SyntaxKind::BangToken:
        operatorKind = BoundUnaryOperatorKind::LogicalNegation;
        resultType = PrimitiveType::Bool;
        if (operand->type != PrimitiveType::Bool) goto invalid_operator;
        break;
    default:
        goto invalid_operator;
    }

    {
        auto result = std::make_unique<BoundUnaryExpression>();
        result->span = syntaxTree.span();
        result->type = resultType;
        result->operatorKind = operatorKind;
        result->operand = std::move(operand);
        return result;
    }

invalid_operator:
    diagnostics_.report(
        "RS2103",
        "unary operator '" + syntaxTree.operatorToken.text + "' is not defined for '" +
            primitiveTypeName(operand->type) + "'",
        syntaxTree.operatorToken.span);
    return makeError(syntaxTree.span());
}

std::unique_ptr<BoundExpression> Binder::bindBinaryExpression(
    const syntax::BinaryExpressionSyntax& syntaxTree) {
    auto left = bindExpression(*syntaxTree.left);
    auto right = bindExpression(*syntaxTree.right);
    if (left->type == PrimitiveType::Error || right->type == PrimitiveType::Error) {
        return makeError(syntaxTree.span());
    }

    BoundBinaryOperatorKind operatorKind;
    PrimitiveType resultType = PrimitiveType::Error;
    const auto tokenKind = syntaxTree.operatorToken.kind;

    if (tokenKind == syntax::SyntaxKind::PlusToken ||
        tokenKind == syntax::SyntaxKind::MinusToken ||
        tokenKind == syntax::SyntaxKind::StarToken ||
        tokenKind == syntax::SyntaxKind::SlashToken ||
        tokenKind == syntax::SyntaxKind::PercentToken) {
        if (left->type != PrimitiveType::Int || right->type != PrimitiveType::Int) {
            goto invalid_operator;
        }
        resultType = PrimitiveType::Int;
        switch (tokenKind) {
        case syntax::SyntaxKind::PlusToken: operatorKind = BoundBinaryOperatorKind::Addition; break;
        case syntax::SyntaxKind::MinusToken: operatorKind = BoundBinaryOperatorKind::Subtraction; break;
        case syntax::SyntaxKind::StarToken: operatorKind = BoundBinaryOperatorKind::Multiplication; break;
        case syntax::SyntaxKind::SlashToken: operatorKind = BoundBinaryOperatorKind::Division; break;
        default: operatorKind = BoundBinaryOperatorKind::Remainder; break;
        }
    } else if (tokenKind == syntax::SyntaxKind::LessToken ||
               tokenKind == syntax::SyntaxKind::LessOrEqualsToken ||
               tokenKind == syntax::SyntaxKind::GreaterToken ||
               tokenKind == syntax::SyntaxKind::GreaterOrEqualsToken) {
        if (left->type != PrimitiveType::Int || right->type != PrimitiveType::Int) {
            goto invalid_operator;
        }
        resultType = PrimitiveType::Bool;
        switch (tokenKind) {
        case syntax::SyntaxKind::LessToken: operatorKind = BoundBinaryOperatorKind::Less; break;
        case syntax::SyntaxKind::LessOrEqualsToken: operatorKind = BoundBinaryOperatorKind::LessOrEquals; break;
        case syntax::SyntaxKind::GreaterToken: operatorKind = BoundBinaryOperatorKind::Greater; break;
        default: operatorKind = BoundBinaryOperatorKind::GreaterOrEquals; break;
        }
    } else if (tokenKind == syntax::SyntaxKind::EqualsEqualsToken ||
               tokenKind == syntax::SyntaxKind::BangEqualsToken) {
        if (left->type != right->type) {
            goto invalid_operator;
        }
        resultType = PrimitiveType::Bool;
        operatorKind = tokenKind == syntax::SyntaxKind::EqualsEqualsToken
            ? BoundBinaryOperatorKind::Equals
            : BoundBinaryOperatorKind::NotEquals;
    } else if (tokenKind == syntax::SyntaxKind::AmpersandAmpersandToken ||
               tokenKind == syntax::SyntaxKind::PipePipeToken) {
        diagnostics_.report(
            "RS2105",
            "short-circuit logical operators require Phase 1B control-flow MIR",
            syntaxTree.operatorToken.span);
        return makeError(syntaxTree.span());
    } else {
        goto invalid_operator;
    }

    {
        auto result = std::make_unique<BoundBinaryExpression>();
        result->span = syntaxTree.span();
        result->type = resultType;
        result->operatorKind = operatorKind;
        result->left = std::move(left);
        result->right = std::move(right);
        return result;
    }

invalid_operator:
    diagnostics_.report(
        "RS2104",
        "binary operator '" + syntaxTree.operatorToken.text + "' is not defined for '" +
            primitiveTypeName(left->type) + "' and '" + primitiveTypeName(right->type) + "'",
        syntaxTree.operatorToken.span);
    return makeError(syntaxTree.span());
}

PrimitiveType Binder::bindType(const syntax::TypeSyntax& syntaxTree, bool allowVoid) {
    const auto type = resolvePrimitiveType(syntaxTree.name.text);
    if (type == PrimitiveType::Error) {
        diagnostics_.report(
            "RS2200",
            "type '" + syntaxTree.name.text + "' is not implemented in the Phase 1A profile",
            syntaxTree.span());
        return PrimitiveType::Error;
    }
    if (type == PrimitiveType::Void && !allowVoid) {
        diagnostics_.report("RS2201", "void is not valid in this type position", syntaxTree.span());
        return PrimitiveType::Error;
    }
    return type;
}

const VariableSymbol* Binder::lookupVariable(const std::string& name) const noexcept {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        const auto found = it->find(name);
        if (found != it->end()) {
            return &found->second;
        }
    }
    return nullptr;
}

bool Binder::declareVariable(VariableSymbol variable, text::TextSpan span) {
    auto& scope = scopes_.back();
    if (scope.find(variable.name) != scope.end()) {
        diagnostics_.report(
            "RS2202",
            "name '" + variable.name + "' is already declared in this scope",
            span);
        return false;
    }
    scope.emplace(variable.name, std::move(variable));
    return true;
}

void Binder::pushScope() {
    scopes_.emplace_back();
}

void Binder::popScope() {
    scopes_.pop_back();
}

std::unique_ptr<BoundErrorExpression> Binder::makeError(text::TextSpan span) const {
    auto result = std::make_unique<BoundErrorExpression>();
    result->type = PrimitiveType::Error;
    result->span = span;
    return result;
}

} // namespace realscript::semantic
