#include "realscript/semantic/Semantic.h"

namespace realscript::semantic {

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
            "floating-point binding is reserved for a later language slice",
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
        if (left->type != PrimitiveType::Bool || right->type != PrimitiveType::Bool) {
            goto invalid_operator;
        }
        resultType = PrimitiveType::Bool;
        operatorKind = tokenKind == syntax::SyntaxKind::AmpersandAmpersandToken
            ? BoundBinaryOperatorKind::LogicalAnd
            : BoundBinaryOperatorKind::LogicalOr;
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

std::unique_ptr<BoundExpression> Binder::bindAssignmentExpression(
    const syntax::AssignmentExpressionSyntax& syntaxTree) {
    const auto* variable = lookupVariable(syntaxTree.identifierToken.text);
    if (!variable) {
        diagnostics_.report(
            "RS2102",
            "undefined name '" + syntaxTree.identifierToken.text + "'",
            syntaxTree.identifierToken.span);
        return makeError(syntaxTree.span());
    }

    auto value = bindExpression(*syntaxTree.expression);
    if (value->type != PrimitiveType::Error && variable->type != PrimitiveType::Error &&
        value->type != variable->type) {
        diagnostics_.report(
            "RS2106",
            "cannot assign '" + std::string(primitiveTypeName(value->type)) + "' to variable '" +
                variable->name + "' of type '" + primitiveTypeName(variable->type) + "'",
            syntaxTree.expression->span());
    }

    auto result = std::make_unique<BoundAssignmentExpression>();
    result->span = syntaxTree.span();
    result->type = variable->type;
    result->variable = *variable;
    result->expression = std::move(value);
    return result;
}

PrimitiveType Binder::bindType(const syntax::TypeSyntax& syntaxTree, bool allowVoid) {
    const auto type = resolvePrimitiveType(syntaxTree.name.text);
    if (type == PrimitiveType::Error) {
        diagnostics_.report(
            "RS2200",
            "type '" + syntaxTree.name.text + "' is not implemented in the Phase 1B profile",
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
