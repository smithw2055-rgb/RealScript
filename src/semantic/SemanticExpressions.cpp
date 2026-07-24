#include "realscript/semantic/Semantic.h"

#include <limits>
#include <utility>

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
        if (operand->type != resultType) goto invalidOperator;
        break;
    case syntax::SyntaxKind::MinusToken:
        operatorKind = BoundUnaryOperatorKind::Negation;
        resultType = PrimitiveType::Int;
        if (operand->type != resultType) goto invalidOperator;
        break;
    case syntax::SyntaxKind::BangToken:
        operatorKind = BoundUnaryOperatorKind::LogicalNegation;
        resultType = PrimitiveType::Bool;
        if (operand->type != resultType) goto invalidOperator;
        break;
    default:
        goto invalidOperator;
    }

    {
        auto result = std::make_unique<BoundUnaryExpression>();
        result->span = syntaxTree.span();
        result->type = resultType;
        result->operatorKind = operatorKind;
        result->operand = std::move(operand);
        return result;
    }

invalidOperator:
    diagnostics_.report(
        "RS2103",
        "unary operator '" + syntaxTree.operatorToken.text +
            "' is not defined for '" + primitiveTypeName(operand->type) + "'",
        syntaxTree.operatorToken.span);
    return makeError(syntaxTree.span());
}

std::unique_ptr<BoundExpression> Binder::bindBinaryExpression(
    const syntax::BinaryExpressionSyntax& syntaxTree) {
    auto left = bindExpression(*syntaxTree.left);
    auto right = bindExpression(*syntaxTree.right);
    if (left->type == PrimitiveType::Error ||
        right->type == PrimitiveType::Error) {
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
        if (left->type != PrimitiveType::Int ||
            right->type != PrimitiveType::Int) {
            goto invalidOperator;
        }
        resultType = PrimitiveType::Int;
        switch (tokenKind) {
        case syntax::SyntaxKind::PlusToken:
            operatorKind = BoundBinaryOperatorKind::Addition;
            break;
        case syntax::SyntaxKind::MinusToken:
            operatorKind = BoundBinaryOperatorKind::Subtraction;
            break;
        case syntax::SyntaxKind::StarToken:
            operatorKind = BoundBinaryOperatorKind::Multiplication;
            break;
        case syntax::SyntaxKind::SlashToken:
            operatorKind = BoundBinaryOperatorKind::Division;
            break;
        default:
            operatorKind = BoundBinaryOperatorKind::Remainder;
            break;
        }
    } else if (tokenKind == syntax::SyntaxKind::LessToken ||
               tokenKind == syntax::SyntaxKind::LessOrEqualsToken ||
               tokenKind == syntax::SyntaxKind::GreaterToken ||
               tokenKind == syntax::SyntaxKind::GreaterOrEqualsToken) {
        if (left->type != PrimitiveType::Int ||
            right->type != PrimitiveType::Int) {
            goto invalidOperator;
        }
        resultType = PrimitiveType::Bool;
        switch (tokenKind) {
        case syntax::SyntaxKind::LessToken:
            operatorKind = BoundBinaryOperatorKind::Less;
            break;
        case syntax::SyntaxKind::LessOrEqualsToken:
            operatorKind = BoundBinaryOperatorKind::LessOrEquals;
            break;
        case syntax::SyntaxKind::GreaterToken:
            operatorKind = BoundBinaryOperatorKind::Greater;
            break;
        default:
            operatorKind = BoundBinaryOperatorKind::GreaterOrEquals;
            break;
        }
    } else if (tokenKind == syntax::SyntaxKind::EqualsEqualsToken ||
               tokenKind == syntax::SyntaxKind::BangEqualsToken) {
        if (left->type != right->type) {
            if (classifyConversion(left->type, right->type) !=
                ConversionKind::None) {
                left = convertExpression(
                    std::move(left),
                    right->type,
                    syntaxTree.left->span(),
                    "equality operand");
            } else if (classifyConversion(right->type, left->type) !=
                       ConversionKind::None) {
                right = convertExpression(
                    std::move(right),
                    left->type,
                    syntaxTree.right->span(),
                    "equality operand");
            } else {
                goto invalidOperator;
            }
        }
        resultType = PrimitiveType::Bool;
        operatorKind = tokenKind == syntax::SyntaxKind::EqualsEqualsToken
            ? BoundBinaryOperatorKind::Equals
            : BoundBinaryOperatorKind::NotEquals;
    } else if (tokenKind == syntax::SyntaxKind::AmpersandAmpersandToken ||
               tokenKind == syntax::SyntaxKind::PipePipeToken) {
        if (left->type != PrimitiveType::Bool ||
            right->type != PrimitiveType::Bool) {
            goto invalidOperator;
        }
        resultType = PrimitiveType::Bool;
        operatorKind = tokenKind == syntax::SyntaxKind::AmpersandAmpersandToken
            ? BoundBinaryOperatorKind::LogicalAnd
            : BoundBinaryOperatorKind::LogicalOr;
    } else {
        goto invalidOperator;
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

invalidOperator:
    diagnostics_.report(
        "RS2104",
        "binary operator '" + syntaxTree.operatorToken.text +
            "' is not defined for '" + primitiveTypeName(left->type) +
            "' and '" + primitiveTypeName(right->type) + "'",
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

    auto value = convertExpression(
        bindExpression(*syntaxTree.expression),
        variable->type,
        syntaxTree.expression->span(),
        "assignment");

    auto result = std::make_unique<BoundAssignmentExpression>();
    result->span = syntaxTree.span();
    result->type = variable->type;
    result->variable = *variable;
    result->expression = std::move(value);
    return result;
}

std::unique_ptr<BoundExpression> Binder::bindCallExpression(
    const syntax::CallExpressionSyntax& syntaxTree) {
    std::vector<std::unique_ptr<BoundExpression>> arguments;
    arguments.reserve(syntaxTree.arguments.size());
    for (const auto& argumentSyntax : syntaxTree.arguments) {
        arguments.push_back(bindExpression(*argumentSyntax));
    }

    const auto overloads = visibleFunctions_.find(syntaxTree.identifierToken.text);
    if (overloads == visibleFunctions_.end()) {
        diagnostics_.report(
            "RS2100",
            "undefined function '" + syntaxTree.identifierToken.text + "'",
            syntaxTree.identifierToken.span);
        return makeError(syntaxTree.span());
    }

    const FunctionSymbol* best = nullptr;
    int bestScore = std::numeric_limits<int>::max();
    bool ambiguous = false;

    for (const auto& candidate : overloads->second) {
        if (candidate.parameters.size() != arguments.size()) {
            continue;
        }

        int score = 0;
        bool applicable = true;
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            const auto rank = conversionRank(
                arguments[i]->type,
                candidate.parameters[i].type);
            if (rank < 0) {
                applicable = false;
                break;
            }
            score += rank;
        }

        if (!applicable) {
            continue;
        }
        if (score < bestScore) {
            best = &candidate;
            bestScore = score;
            ambiguous = false;
        } else if (score == bestScore && best && best->id != candidate.id) {
            ambiguous = true;
        }
    }

    if (!best) {
        diagnostics_.report(
            "RS2107",
            "no applicable overload for function '" +
                syntaxTree.identifierToken.text + "'",
            syntaxTree.span());
        return makeError(syntaxTree.span());
    }
    if (ambiguous) {
        diagnostics_.report(
            "RS2108",
            "call to function '" + syntaxTree.identifierToken.text +
                "' is ambiguous",
            syntaxTree.span());
        return makeError(syntaxTree.span());
    }

    for (std::size_t i = 0; i < arguments.size(); ++i) {
        arguments[i] = convertExpression(
            std::move(arguments[i]),
            best->parameters[i].type,
            syntaxTree.arguments[i]->span(),
            "argument");
    }

    auto result = std::make_unique<BoundCallExpression>();
    result->span = syntaxTree.span();
    result->type = best->returnType;
    result->function = *best;
    result->arguments = std::move(arguments);
    return result;
}

std::unique_ptr<BoundExpression> Binder::convertExpression(
    std::unique_ptr<BoundExpression> expression,
    PrimitiveType target,
    text::TextSpan span,
    const std::string& context) {
    if (!expression) {
        return makeError(span);
    }
    if (expression->type == PrimitiveType::Error ||
        target == PrimitiveType::Error) {
        return expression;
    }

    const auto conversion = classifyConversion(expression->type, target);
    if (conversion == ConversionKind::None) {
        diagnostics_.report(
            "RS2106",
            "cannot convert '" +
                std::string(primitiveTypeName(expression->type)) +
                "' to '" + primitiveTypeName(target) + "' for " + context,
            span);
        return makeError(span);
    }
    if (conversion == ConversionKind::Identity) {
        return expression;
    }

    auto result = std::make_unique<BoundConversionExpression>();
    result->span = span;
    result->type = target;
    result->conversion = conversion;
    result->expression = std::move(expression);
    return result;
}

} // namespace realscript::semantic
