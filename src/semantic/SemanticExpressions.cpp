#include "realscript/semantic/Semantic.h"

#include <limits>
#include <utility>

namespace realscript::semantic {
namespace {

bool sameReferenceType(
    const BoundExpression& expression,
    PrimitiveType targetType,
    const std::string& targetTypeName) {
    if (expression.type != targetType) return false;
    return targetTypeName.empty() ||
        expression.typeName.empty() ||
        expression.typeName == targetTypeName;
}

const FieldSymbol* findField(
    const TypeSymbol& type,
    const std::string& name) {
    for (const auto& field : type.fields) {
        if (field.name == name) return &field;
    }
    return nullptr;
}

} // namespace

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
    result->typeName = variable->typeName;
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
                    "equality operand",
                    right->typeName);
            } else if (classifyConversion(right->type, left->type) !=
                       ConversionKind::None) {
                right = convertExpression(
                    std::move(right),
                    left->type,
                    syntaxTree.right->span(),
                    "equality operand",
                    left->typeName);
            } else {
                goto invalidOperator;
            }
        }
        if ((left->type == PrimitiveType::Object ||
             left->type == PrimitiveType::Array ||
             left->type == PrimitiveType::Handle) &&
            left->type == right->type &&
            !left->typeName.empty() && !right->typeName.empty() &&
            left->typeName != right->typeName) {
            goto invalidOperator;
        }
        if ((left->type == PrimitiveType::Object ||
             left->type == PrimitiveType::Array ||
             left->type == PrimitiveType::Handle) &&
            left->type == right->type) {
            if (left->typeName.empty()) left->typeName = right->typeName;
            if (right->typeName.empty()) right->typeName = left->typeName;
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
        "assignment",
        variable->typeName);

    auto result = std::make_unique<BoundAssignmentExpression>();
    result->span = syntaxTree.span();
    result->type = variable->type;
    result->typeName = variable->typeName;
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
            if (rank < 0 ||
                ((candidate.parameters[i].type == PrimitiveType::Object ||
                  candidate.parameters[i].type == PrimitiveType::Array ||
                  candidate.parameters[i].type == PrimitiveType::Handle) &&
                 arguments[i]->type == candidate.parameters[i].type &&
                 !sameReferenceType(
                     *arguments[i],
                     candidate.parameters[i].type,
                     candidate.parameters[i].typeName))) {
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
            "argument",
            best->parameters[i].typeName);
    }

    auto result = std::make_unique<BoundCallExpression>();
    result->span = syntaxTree.span();
    result->type = best->returnType;
    result->typeName = best->returnTypeName;
    result->function = *best;
    result->arguments = std::move(arguments);
    return result;
}

std::unique_ptr<BoundExpression> Binder::bindNewArrayExpression(
    const syntax::NewArrayExpressionSyntax& syntaxTree) {
    std::string elementTypeName;
    const auto elementType = bindType(
        syntaxTree.elementType, false, &elementTypeName);
    if (elementType == PrimitiveType::Error ||
        elementType == PrimitiveType::Void ||
        elementType == PrimitiveType::Array) {
        diagnostics_.report(
            "RS2420",
            "array element type is not supported",
            syntaxTree.elementType.span());
        return makeError(syntaxTree.span());
    }
    auto length = convertExpression(
        bindExpression(*syntaxTree.length),
        PrimitiveType::Int,
        syntaxTree.length->span(),
        "array length");
    auto result = std::make_unique<BoundNewArrayExpression>();
    result->span = syntaxTree.span();
    result->type = PrimitiveType::Array;
    result->typeName = arrayTypeName(elementType, elementTypeName);
    result->elementType = elementType;
    result->elementTypeName = std::move(elementTypeName);
    result->length = std::move(length);
    return result;
}

std::unique_ptr<BoundExpression> Binder::bindNewObjectExpression(
    const syntax::NewObjectExpressionSyntax& syntaxTree) {
    const auto found = visibleTypes_.find(syntaxTree.type.name.text);
    if (found == visibleTypes_.end()) {
        diagnostics_.report(
            "RS2403",
            "cannot allocate unknown type '" + syntaxTree.type.name.text + "'",
            syntaxTree.type.span());
        return makeError(syntaxTree.span());
    }
    auto result = std::make_unique<BoundNewObjectExpression>();
    result->span = syntaxTree.span();
    result->type = PrimitiveType::Object;
    result->typeName = canonicalTypeName(found->second);
    result->objectType = found->second;
    return result;
}

std::unique_ptr<BoundExpression> Binder::bindMemberAccessExpression(
    const syntax::MemberAccessExpressionSyntax& syntaxTree) {
    auto receiver = bindExpression(*syntaxTree.receiver);
    if (receiver->type == PrimitiveType::Array &&
        syntaxTree.nameToken.text == "length") {
        auto result = std::make_unique<BoundArrayLengthExpression>();
        result->span = syntaxTree.span();
        result->type = PrimitiveType::Int;
        result->receiver = std::move(receiver);
        return result;
    }
    if (receiver->type != PrimitiveType::Object || receiver->typeName.empty()) {
        diagnostics_.report(
            "RS2404",
            "member access requires a class reference",
            syntaxTree.receiver->span());
        return makeError(syntaxTree.span());
    }
    const auto type = visibleTypes_.find(receiver->typeName);
    if (type == visibleTypes_.end()) {
        diagnostics_.report(
            "RS2405",
            "type descriptor '" + receiver->typeName + "' is unavailable",
            syntaxTree.receiver->span());
        return makeError(syntaxTree.span());
    }
    const auto* field = findField(type->second, syntaxTree.nameToken.text);
    if (!field) {
        diagnostics_.report(
            "RS2406",
            "type '" + receiver->typeName + "' has no field '" +
                syntaxTree.nameToken.text + "'",
            syntaxTree.nameToken.span);
        return makeError(syntaxTree.span());
    }
    auto result = std::make_unique<BoundMemberAccessExpression>();
    result->span = syntaxTree.span();
    result->type = field->type;
    result->typeName = field->typeName;
    result->receiver = std::move(receiver);
    result->ownerType = type->second;
    result->field = *field;
    return result;
}

std::unique_ptr<BoundExpression> Binder::bindElementAccessExpression(
    const syntax::ElementAccessExpressionSyntax& syntaxTree) {
    auto receiver = bindExpression(*syntaxTree.receiver);
    if (receiver->type != PrimitiveType::Array || receiver->typeName.empty()) {
        diagnostics_.report(
            "RS2421",
            "element access requires an array",
            syntaxTree.receiver->span());
        return makeError(syntaxTree.span());
    }
    PrimitiveType elementType = PrimitiveType::Error;
    std::string elementTypeName;
    if (!decodeArrayTypeName(
            receiver->typeName, elementType, elementTypeName)) {
        diagnostics_.report(
            "RS2422",
            "array element descriptor is invalid",
            syntaxTree.receiver->span());
        return makeError(syntaxTree.span());
    }
    auto index = convertExpression(
        bindExpression(*syntaxTree.index),
        PrimitiveType::Int,
        syntaxTree.index->span(),
        "array index");
    auto result = std::make_unique<BoundElementAccessExpression>();
    result->span = syntaxTree.span();
    result->type = elementType;
    result->typeName = elementTypeName;
    result->receiver = std::move(receiver);
    result->index = std::move(index);
    result->elementType = elementType;
    result->elementTypeName = std::move(elementTypeName);
    return result;
}

std::unique_ptr<BoundExpression> Binder::bindMemberAssignmentExpression(
    const syntax::MemberAssignmentExpressionSyntax& syntaxTree) {
    auto receiver = bindExpression(*syntaxTree.receiver);
    if (receiver->type != PrimitiveType::Object || receiver->typeName.empty()) {
        diagnostics_.report(
            "RS2404",
            "member assignment requires a class reference",
            syntaxTree.receiver->span());
        return makeError(syntaxTree.span());
    }
    const auto type = visibleTypes_.find(receiver->typeName);
    if (type == visibleTypes_.end()) {
        diagnostics_.report(
            "RS2405",
            "type descriptor '" + receiver->typeName + "' is unavailable",
            syntaxTree.receiver->span());
        return makeError(syntaxTree.span());
    }
    const auto* field = findField(type->second, syntaxTree.nameToken.text);
    if (!field) {
        diagnostics_.report(
            "RS2406",
            "type '" + receiver->typeName + "' has no field '" +
                syntaxTree.nameToken.text + "'",
            syntaxTree.nameToken.span);
        return makeError(syntaxTree.span());
    }
    auto value = convertExpression(
        bindExpression(*syntaxTree.expression),
        field->type,
        syntaxTree.expression->span(),
        "field assignment",
        field->typeName);
    auto result = std::make_unique<BoundMemberAssignmentExpression>();
    result->span = syntaxTree.span();
    result->type = field->type;
    result->typeName = field->typeName;
    result->receiver = std::move(receiver);
    result->ownerType = type->second;
    result->field = *field;
    result->expression = std::move(value);
    return result;
}

std::unique_ptr<BoundExpression> Binder::bindElementAssignmentExpression(
    const syntax::ElementAssignmentExpressionSyntax& syntaxTree) {
    auto receiver = bindExpression(*syntaxTree.receiver);
    if (receiver->type != PrimitiveType::Array || receiver->typeName.empty()) {
        diagnostics_.report(
            "RS2421",
            "element assignment requires an array",
            syntaxTree.receiver->span());
        return makeError(syntaxTree.span());
    }
    PrimitiveType elementType = PrimitiveType::Error;
    std::string elementTypeName;
    if (!decodeArrayTypeName(
            receiver->typeName, elementType, elementTypeName)) {
        diagnostics_.report(
            "RS2422",
            "array element descriptor is invalid",
            syntaxTree.receiver->span());
        return makeError(syntaxTree.span());
    }
    auto index = convertExpression(
        bindExpression(*syntaxTree.index),
        PrimitiveType::Int,
        syntaxTree.index->span(),
        "array index");
    auto value = convertExpression(
        bindExpression(*syntaxTree.expression),
        elementType,
        syntaxTree.expression->span(),
        "array element assignment",
        elementTypeName);
    auto result = std::make_unique<BoundElementAssignmentExpression>();
    result->span = syntaxTree.span();
    result->type = elementType;
    result->typeName = elementTypeName;
    result->receiver = std::move(receiver);
    result->index = std::move(index);
    result->expression = std::move(value);
    result->elementType = elementType;
    result->elementTypeName = std::move(elementTypeName);
    return result;
}

std::unique_ptr<BoundExpression> Binder::convertExpression(
    std::unique_ptr<BoundExpression> expression,
    PrimitiveType target,
    text::TextSpan span,
    const std::string& context,
    std::string targetTypeName) {
    if (!expression) {
        return makeError(span);
    }
    if (expression->type == PrimitiveType::Error ||
        target == PrimitiveType::Error) {
        return expression;
    }

    if ((target == PrimitiveType::Object ||
         target == PrimitiveType::Array ||
         target == PrimitiveType::Handle) &&
        expression->type == target &&
        !sameReferenceType(*expression, target, targetTypeName)) {
        diagnostics_.report(
            "RS2410",
            "cannot convert reference type '" + expression->typeName +
                "' to '" + targetTypeName + "' for " + context,
            span);
        return makeError(span);
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
        if ((target == PrimitiveType::Object ||
             target == PrimitiveType::Array ||
             target == PrimitiveType::Handle) &&
            !targetTypeName.empty()) {
            expression->typeName = std::move(targetTypeName);
        }
        return expression;
    }

    auto result = std::make_unique<BoundConversionExpression>();
    result->span = span;
    result->type = target;
    result->typeName = std::move(targetTypeName);
    result->conversion = conversion;
    result->expression = std::move(expression);
    return result;
}

} // namespace realscript::semantic
