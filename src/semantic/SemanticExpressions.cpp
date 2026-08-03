#include "realscript/semantic/Semantic.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace realscript::semantic {
namespace {

bool sameExactType(
    const BoundExpression& expression,
    PrimitiveType targetType,
    const std::string& targetTypeName,
    const TypeSymbolMap& visibleTypes) {
    if (expression.type != targetType) return false;
    if (!isExactType(targetType)) return true;
    return targetTypeName.empty() || expression.typeName.empty() ||
        expression.typeName == targetTypeName ||
        (targetType == PrimitiveType::Object &&
         isAssignable(visibleTypes, expression.typeName, targetTypeName));
}

const FieldSymbol* findField(const TypeSymbol& type, const std::string& name) {
    for (const auto& field : type.fields) {
        if (field.name == name) return &field;
    }
    return nullptr;
}

const PropertySymbol* findProperty(const TypeSymbol& type, const std::string& name) {
    for (const auto& property : type.properties) {
        if (property.name == name) return &property;
    }
    return nullptr;
}

const EnumMemberSymbol* findEnumMember(const TypeSymbol& type, const std::string& name) {
    for (const auto& member : type.enumMembers) {
        if (member.name == name) return &member;
    }
    return nullptr;
}

std::vector<const FunctionSymbol*> findMethods(
    const TypeSymbol& type,
    const std::string& name,
    bool staticMethod) {
    std::vector<const FunctionSymbol*> result;
    for (const auto& method : type.methods) {
        if (method.name == name && method.staticMethod == staticMethod) {
            result.push_back(&method);
        }
    }
    return result;
}

PrimitiveType commonNumericType(PrimitiveType left, PrimitiveType right) noexcept {
    if (!isNumericType(left) || !isNumericType(right)) return PrimitiveType::Error;
    if (left == PrimitiveType::Double || right == PrimitiveType::Double) return PrimitiveType::Double;
    if (left == PrimitiveType::Long || right == PrimitiveType::Long) return PrimitiveType::Long;
    return PrimitiveType::Int;
}

ParameterModifier syntaxModifier(
    const std::optional<syntax::SyntaxToken>& token) noexcept {
    if (!token) return ParameterModifier::None;
    switch (token->kind) {
    case syntax::SyntaxKind::RefKeyword: return ParameterModifier::Ref;
    case syntax::SyntaxKind::OutKeyword: return ParameterModifier::Out;
    case syntax::SyntaxKind::InKeyword: return ParameterModifier::In;
    default: return ParameterModifier::None;
    }
}

std::unique_ptr<BoundVariableExpression> variableExpression(
    const VariableSymbol& variable,
    text::TextSpan span = {}) {
    auto result = std::make_unique<BoundVariableExpression>();
    result->span = span;
    result->type = variable.type;
    result->typeName = variable.typeName;
    result->variable = variable;
    return result;
}

std::unique_ptr<BoundVariableExpression> storageVariableExpression(
    const VariableSymbol& variable,
    text::TextSpan span = {}) {
    auto result = std::make_unique<BoundVariableExpression>();
    result->span = span;
    result->type = storageTypeOf(variable);
    result->typeName = storageTypeNameOf(variable);
    result->variable = variable;
    result->variable.type = result->type;
    result->variable.typeName = result->typeName;
    return result;
}

std::size_t visibleParameterOffset(const FunctionSymbol& function) noexcept {
    return function.method && !function.staticMethod ? 1u : 0u;
}

bool exactParameterMatch(
    const BoundExpression& expression,
    const VariableSymbol& parameter,
    const TypeSymbolMap& visibleTypes) {
    return sameExactType(
        expression, parameter.type, parameter.typeName, visibleTypes);
}

const FunctionSymbol* selectBest(
    const std::vector<const FunctionSymbol*>& candidates,
    const std::vector<std::unique_ptr<BoundExpression>>& arguments,
    const std::vector<std::optional<syntax::SyntaxToken>>* modifiers,
    const TypeSymbolMap& visibleTypes,
    bool& ambiguous) {
    const FunctionSymbol* best = nullptr;
    int bestScore = std::numeric_limits<int>::max();
    ambiguous = false;
    for (const auto* candidate : candidates) {
        const auto offset = visibleParameterOffset(*candidate);
        if (candidate->parameters.size() != arguments.size() + offset) {
            continue;
        }
        int score = 0;
        bool applicable = true;
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            const auto& parameter = candidate->parameters[i + offset];
            const auto suppliedModifier = modifiers && i < modifiers->size()
                ? syntaxModifier((*modifiers)[i])
                : ParameterModifier::None;
            if (suppliedModifier != parameter.modifier) {
                applicable = false;
                break;
            }
            if (parameter.modifier == ParameterModifier::None) {
                const auto rank = conversionRank(
                    arguments[i]->type, parameter.type);
                if (rank < 0 ||
                    (arguments[i]->type == parameter.type &&
                     !exactParameterMatch(*arguments[i], parameter, visibleTypes))) {
                    applicable = false;
                    break;
                }
                score += rank;
            } else if (!exactParameterMatch(
                           *arguments[i], parameter, visibleTypes)) {
                applicable = false;
                break;
            }
        }
        if (!applicable) continue;
        if (score < bestScore) {
            best = candidate;
            bestScore = score;
            ambiguous = false;
        } else if (score == bestScore && best &&
                   best->id != candidate->id) {
            ambiguous = true;
        }
    }
    return best;
}

} // namespace

std::unique_ptr<BoundExpression> Binder::bindLiteralExpression(
    const syntax::LiteralExpressionSyntax& syntaxTree) {
    auto result = std::make_unique<BoundLiteralExpression>();
    result->span = syntaxTree.span();
    result->value = syntaxTree.literalToken.value;
    switch (syntaxTree.literalToken.kind) {
    case syntax::SyntaxKind::IntegerLiteralToken: {
        const auto value = std::holds_alternative<std::int64_t>(result->value)
            ? std::get<std::int64_t>(result->value)
            : 0;
        result->type = value < std::numeric_limits<std::int32_t>::min() ||
                value > std::numeric_limits<std::int32_t>::max()
            ? PrimitiveType::Long
            : PrimitiveType::Int;
        break;
    }
    case syntax::SyntaxKind::FloatLiteralToken:
        result->type = PrimitiveType::Double;
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
    default:
        result->type = PrimitiveType::Error;
        break;
    }
    return result;
}

std::unique_ptr<BoundExpression> Binder::bindThisExpression(
    const syntax::ThisExpressionSyntax& syntaxTree) {
    const auto* variable = lookupVariable("this");
    if (!variable || currentStaticMethod_) {
        diagnostics_.report("RS2470", "'this' is not available in a static context", syntaxTree.span());
        return makeError(syntaxTree.span());
    }
    return variableExpression(*variable, syntaxTree.span());
}

std::unique_ptr<BoundExpression> Binder::bindBaseExpression(
    const syntax::BaseExpressionSyntax& syntaxTree) {
    const auto* variable = lookupVariable("this");
    if (!variable || currentStaticMethod_ || !currentOwnerType_ ||
        currentOwnerType_->baseTypeName.empty()) {
        diagnostics_.report(
            "RS2504",
            "'base' is not available in this context",
            syntaxTree.span());
        return makeError(syntaxTree.span());
    }
    const auto found = visibleTypes_.find(
        currentOwnerType_->baseTypeName);
    if (found == visibleTypes_.end()) {
        diagnostics_.report(
            "RS2505", "base type descriptor is unavailable",
            syntaxTree.span());
        return makeError(syntaxTree.span());
    }
    auto result = std::make_unique<BoundConversionExpression>();
    result->span = syntaxTree.span();
    result->type = PrimitiveType::Object;
    result->typeName = currentOwnerType_->baseTypeName;
    result->conversion = ConversionKind::Identity;
    result->expression = variableExpression(*variable, syntaxTree.span());
    return result;
}

std::unique_ptr<BoundExpression> Binder::bindNameExpression(
    const syntax::NameExpressionSyntax& syntaxTree) {
    if (const auto* variable = lookupVariable(
            syntaxTree.identifierToken.text)) {
        if (variable->modifier == ParameterModifier::Ref ||
            variable->modifier == ParameterModifier::Out) {
            const auto wrapper = visibleTypes_.find(
                storageTypeNameOf(*variable));
            if (wrapper == visibleTypes_.end() ||
                wrapper->second.fields.empty()) {
                diagnostics_.report(
                    "RS8705",
                    "reference parameter storage descriptor is unavailable",
                    syntaxTree.span());
                return makeError(syntaxTree.span());
            }
            auto result =
                std::make_unique<BoundMemberAccessExpression>();
            result->span = syntaxTree.span();
            result->type = variable->type;
            result->typeName = variable->typeName;
            result->receiver = storageVariableExpression(
                *variable, syntaxTree.span());
            result->ownerType = wrapper->second;
            result->field = wrapper->second.fields.front();
            return result;
        }
        return variableExpression(*variable, syntaxTree.span());
    }

    if (currentOwnerType_ && !currentStaticMethod_) {
        const auto* thisVariable = lookupVariable("this");
        if (const auto* field = findField(*currentOwnerType_, syntaxTree.identifierToken.text)) {
            if (currentOwnerType_->kind == TypeKind::Struct) {
                auto result = std::make_unique<BoundStructFieldAccessExpression>();
                result->span = syntaxTree.span();
                result->type = field->type;
                result->typeName = field->typeName;
                result->receiver = variableExpression(*thisVariable, syntaxTree.span());
                result->ownerType = *currentOwnerType_;
                result->field = *field;
                return result;
            }
            auto result = std::make_unique<BoundMemberAccessExpression>();
            result->span = syntaxTree.span();
            result->type = field->type;
            result->typeName = field->typeName;
            result->receiver = variableExpression(*thisVariable, syntaxTree.span());
            result->ownerType = *currentOwnerType_;
            result->field = *field;
            return result;
        }
        if (const auto* property = findProperty(*currentOwnerType_, syntaxTree.identifierToken.text)) {
            if (property->getter) {
                auto result = std::make_unique<BoundCallExpression>();
                result->span = syntaxTree.span();
                result->type = property->type;
                result->typeName = property->typeName;
                result->function = *property->getter;
                result->arguments.push_back(variableExpression(*thisVariable, syntaxTree.span()));
                return result;
            }
        }
    }

    diagnostics_.report(
        "RS2102",
        "undefined name '" + syntaxTree.identifierToken.text + "'",
        syntaxTree.identifierToken.span);
    return makeError(syntaxTree.span());
}

std::unique_ptr<BoundExpression> Binder::bindUnaryExpression(
    const syntax::UnaryExpressionSyntax& syntaxTree) {
    auto operand = bindExpression(*syntaxTree.operand);
    if (operand->type == PrimitiveType::Error) return makeError(syntaxTree.span());

    BoundUnaryOperatorKind operatorKind = BoundUnaryOperatorKind::Identity;
    PrimitiveType resultType = operand->type;
    switch (syntaxTree.operatorToken.kind) {
    case syntax::SyntaxKind::PlusToken:
        if (!isNumericType(operand->type)) goto invalidOperator;
        operatorKind = BoundUnaryOperatorKind::Identity;
        break;
    case syntax::SyntaxKind::MinusToken:
        if (!isNumericType(operand->type)) goto invalidOperator;
        operatorKind = BoundUnaryOperatorKind::Negation;
        break;
    case syntax::SyntaxKind::BangToken:
        if (operand->type != PrimitiveType::Bool) goto invalidOperator;
        operatorKind = BoundUnaryOperatorKind::LogicalNegation;
        resultType = PrimitiveType::Bool;
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
    if (left->type == PrimitiveType::Error || right->type == PrimitiveType::Error) {
        return makeError(syntaxTree.span());
    }

    BoundBinaryOperatorKind operatorKind = BoundBinaryOperatorKind::Addition;
    PrimitiveType resultType = PrimitiveType::Error;
    const auto tokenKind = syntaxTree.operatorToken.kind;

    if (tokenKind == syntax::SyntaxKind::PlusToken ||
        tokenKind == syntax::SyntaxKind::MinusToken ||
        tokenKind == syntax::SyntaxKind::StarToken ||
        tokenKind == syntax::SyntaxKind::SlashToken ||
        tokenKind == syntax::SyntaxKind::PercentToken) {
        const auto common = commonNumericType(left->type, right->type);
        if (common == PrimitiveType::Error ||
            (tokenKind == syntax::SyntaxKind::PercentToken && common == PrimitiveType::Double)) {
            goto invalidOperator;
        }
        left = convertExpression(std::move(left), common, syntaxTree.left->span(), "numeric operand");
        right = convertExpression(std::move(right), common, syntaxTree.right->span(), "numeric operand");
        resultType = common;
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
        const auto common = commonNumericType(left->type, right->type);
        if (common == PrimitiveType::Error) goto invalidOperator;
        left = convertExpression(std::move(left), common, syntaxTree.left->span(), "comparison operand");
        right = convertExpression(std::move(right), common, syntaxTree.right->span(), "comparison operand");
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
            const auto leftToRight = classifyConversion(left->type, right->type);
            const auto rightToLeft = classifyConversion(right->type, left->type);
            if (leftToRight != ConversionKind::None) {
                left = convertExpression(std::move(left), right->type, syntaxTree.left->span(), "equality operand", right->typeName);
            } else if (rightToLeft != ConversionKind::None) {
                right = convertExpression(std::move(right), left->type, syntaxTree.right->span(), "equality operand", left->typeName);
            } else {
                goto invalidOperator;
            }
        }
        if (isExactType(left->type) && left->type == right->type &&
            !left->typeName.empty() && !right->typeName.empty() &&
            left->typeName != right->typeName) {
            goto invalidOperator;
        }
        resultType = PrimitiveType::Bool;
        operatorKind = tokenKind == syntax::SyntaxKind::EqualsEqualsToken
            ? BoundBinaryOperatorKind::Equals
            : BoundBinaryOperatorKind::NotEquals;
    } else if (tokenKind == syntax::SyntaxKind::AmpersandAmpersandToken ||
               tokenKind == syntax::SyntaxKind::PipePipeToken) {
        if (left->type != PrimitiveType::Bool || right->type != PrimitiveType::Bool) goto invalidOperator;
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
    if (const auto* variable = lookupVariable(
            syntaxTree.identifierToken.text)) {
        if (variable->modifier == ParameterModifier::In) {
            diagnostics_.report(
                "RS8702",
                "cannot assign to in parameter '" +
                    variable->name + "'",
                syntaxTree.identifierToken.span);
            return makeError(syntaxTree.span());
        }
        auto value = convertExpression(
            bindExpression(*syntaxTree.expression), variable->type,
            syntaxTree.expression->span(), "assignment",
            variable->typeName);
        if (variable->modifier == ParameterModifier::Ref ||
            variable->modifier == ParameterModifier::Out) {
            const auto wrapper = visibleTypes_.find(
                storageTypeNameOf(*variable));
            if (wrapper == visibleTypes_.end() ||
                wrapper->second.fields.empty()) {
                diagnostics_.report(
                    "RS8705",
                    "reference parameter storage descriptor is unavailable",
                    syntaxTree.span());
                return makeError(syntaxTree.span());
            }
            auto result =
                std::make_unique<BoundMemberAssignmentExpression>();
            result->span = syntaxTree.span();
            result->type = variable->type;
            result->typeName = variable->typeName;
            result->receiver = storageVariableExpression(
                *variable, syntaxTree.span());
            result->ownerType = wrapper->second;
            result->field = wrapper->second.fields.front();
            result->expression = std::move(value);
            return result;
        }
        auto result = std::make_unique<BoundAssignmentExpression>();
        result->span = syntaxTree.span();
        result->type = variable->type;
        result->typeName = variable->typeName;
        result->variable = *variable;
        result->expression = std::move(value);
        return result;
    }

    if (currentOwnerType_ && !currentStaticMethod_) {
        const auto* thisVariable = lookupVariable("this");
        if (const auto* field = findField(
                *currentOwnerType_, syntaxTree.identifierToken.text)) {
            auto value = convertExpression(
                bindExpression(*syntaxTree.expression), field->type,
                syntaxTree.expression->span(), "field assignment",
                field->typeName);
            if (currentOwnerType_->kind == TypeKind::Struct) {
                if (!currentConstructor_) {
                    diagnostics_.report(
                        "RS2485",
                        "struct instance methods are read-only; mutate a local struct variable or assign fields in a constructor",
                        syntaxTree.identifierToken.span);
                    return makeError(syntaxTree.span());
                }
                auto result = std::make_unique<BoundStructFieldAssignmentExpression>();
                result->span = syntaxTree.span();
                result->type = field->type;
                result->typeName = field->typeName;
                result->variable = *thisVariable;
                result->ownerType = *currentOwnerType_;
                result->field = *field;
                result->expression = std::move(value);
                return result;
            }
            auto receiver = variableExpression(*thisVariable, syntaxTree.span());
            auto result = std::make_unique<BoundMemberAssignmentExpression>();
            result->span = syntaxTree.span();
            result->type = field->type;
            result->typeName = field->typeName;
            result->receiver = std::move(receiver);
            result->ownerType = *currentOwnerType_;
            result->field = *field;
            result->expression = std::move(value);
            return result;
        }
        if (const auto* property = findProperty(
                *currentOwnerType_, syntaxTree.identifierToken.text)) {
            if (!property->setter) {
                diagnostics_.report(
                    "RS2483",
                    "property '" + property->name + "' is read-only",
                    syntaxTree.identifierToken.span);
                return makeError(syntaxTree.span());
            }
            if (currentOwnerType_->kind == TypeKind::Struct) {
                diagnostics_.report(
                    "RS2486",
                    "struct properties cannot be assigned through an instance method in the Phase 3E value model",
                    syntaxTree.identifierToken.span);
                return makeError(syntaxTree.span());
            }
            auto assigned = convertExpression(
                bindExpression(*syntaxTree.expression), property->type,
                syntaxTree.expression->span(), "property assignment",
                property->typeName);
            auto result = std::make_unique<BoundPropertyAssignmentExpression>();
            result->span = syntaxTree.span();
            result->type = property->type;
            result->typeName = property->typeName;
            result->setter = *property->setter;
            result->arguments.push_back(
                variableExpression(*thisVariable, syntaxTree.span()));
            result->assignedValue = std::move(assigned);
            return result;
        }
    }

    diagnostics_.report(
        "RS2102",
        "undefined name '" + syntaxTree.identifierToken.text + "'",
        syntaxTree.identifierToken.span);
    return makeError(syntaxTree.span());
}

std::unique_ptr<BoundExpression> Binder::bindSelectedCall(
    const FunctionSymbol& function,
    std::vector<std::unique_ptr<BoundExpression>> arguments,
    const std::vector<std::unique_ptr<syntax::ExpressionSyntax>>&
        syntaxArguments,
    const std::vector<std::optional<syntax::SyntaxToken>>&
        argumentModifiers,
    std::unique_ptr<BoundExpression> receiver,
    text::TextSpan span,
    const std::string& context,
    bool forceStaticDispatch) {
    const auto offset = visibleParameterOffset(function);
    const bool virtualDispatch = receiver &&
        function.method && !function.staticMethod &&
        !forceStaticDispatch &&
        function.virtualSlot != std::numeric_limits<std::uint32_t>::max();
    bool hasModifiers = false;
    for (std::size_t index = offset;
         index < function.parameters.size(); ++index) {
        hasModifiers = hasModifiers ||
            function.parameters[index].modifier !=
                ParameterModifier::None;
    }
    if (!hasModifiers) {
        auto result = std::make_unique<BoundCallExpression>();
        result->span = span;
        result->type = function.returnType;
        result->typeName = function.returnTypeName;
        result->function = function;
        result->virtualDispatch = virtualDispatch;
        result->virtualSlot = virtualDispatch
            ? function.virtualSlot
            : std::numeric_limits<std::uint32_t>::max();
        if (receiver) result->arguments.push_back(std::move(receiver));
        for (std::size_t index = 0;
             index < arguments.size(); ++index) {
            const auto& parameter =
                function.parameters[index + offset];
            result->arguments.push_back(convertExpression(
                std::move(arguments[index]),
                parameter.type,
                syntaxArguments[index]->span(),
                context,
                parameter.typeName));
        }
        return result;
    }

    auto result = std::make_unique<BoundReferenceCallExpression>();
    result->span = span;
    result->type = function.returnType;
    result->typeName = function.returnTypeName;
    result->function = function;
    result->virtualDispatch = virtualDispatch;
    result->virtualSlot = virtualDispatch
        ? function.virtualSlot
        : std::numeric_limits<std::uint32_t>::max();
    if (receiver) {
        BoundReferenceCallArgument receiverArgument;
        receiverArgument.value = std::move(receiver);
        result->arguments.push_back(std::move(receiverArgument));
    }

    for (std::size_t index = 0;
         index < arguments.size(); ++index) {
        const auto& parameter = function.parameters[index + offset];
        BoundReferenceCallArgument argument;
        argument.modifier = parameter.modifier;
        if (parameter.modifier == ParameterModifier::None) {
            argument.value = convertExpression(
                std::move(arguments[index]),
                parameter.type,
                syntaxArguments[index]->span(),
                context,
                parameter.typeName);
            result->arguments.push_back(std::move(argument));
            continue;
        }

        if (index >= argumentModifiers.size() ||
            syntaxModifier(argumentModifiers[index]) !=
                parameter.modifier) {
            diagnostics_.report(
                "RS8703",
                "reference argument must use the matching modifier",
                syntaxArguments[index]->span());
            argument.value = std::move(arguments[index]);
            result->arguments.push_back(std::move(argument));
            continue;
        }
        if (parameter.modifier == ParameterModifier::In) {
            argument.value = convertExpression(
                std::move(arguments[index]),
                parameter.type,
                syntaxArguments[index]->span(),
                context,
                parameter.typeName);
            result->arguments.push_back(std::move(argument));
            continue;
        }
        if (syntaxArguments[index]->kind() !=
            syntax::SyntaxKind::NameExpression) {
            diagnostics_.report(
                "RS8703",
                "ref and out arguments must name a variable",
                syntaxArguments[index]->span());
            argument.value = std::move(arguments[index]);
            result->arguments.push_back(std::move(argument));
            continue;
        }

        const auto& name = static_cast<
            const syntax::NameExpressionSyntax&>(
                *syntaxArguments[index]);
        const auto* variable = lookupVariable(
            name.identifierToken.text);
        if (!variable) {
            diagnostics_.report(
                "RS8703",
                "reference argument must name a local variable or "
                "parameter",
                syntaxArguments[index]->span());
            argument.value = std::move(arguments[index]);
            result->arguments.push_back(std::move(argument));
            continue;
        }
        if (variable->modifier == ParameterModifier::In &&
            parameter.modifier != ParameterModifier::In) {
            diagnostics_.report(
                "RS8703",
                "an in parameter cannot be forwarded as ref or out",
                syntaxArguments[index]->span());
        }
        argument.variable = *variable;

        const auto wrapper = visibleTypes_.find(
            storageTypeNameOf(parameter));
        if (wrapper == visibleTypes_.end() ||
            wrapper->second.fields.empty()) {
            diagnostics_.report(
                "RS8705",
                "reference argument storage descriptor is unavailable",
                syntaxArguments[index]->span());
            argument.value = std::move(arguments[index]);
            result->arguments.push_back(std::move(argument));
            continue;
        }
        argument.wrapperType = wrapper->second;
        argument.valueField = wrapper->second.fields.front();
        if (variable->modifier == ParameterModifier::Ref ||
            variable->modifier == ParameterModifier::Out) {
            if (storageTypeNameOf(*variable) !=
                storageTypeNameOf(parameter)) {
                diagnostics_.report(
                    "RS8703",
                    "forwarded reference parameter has an "
                    "incompatible storage type",
                    syntaxArguments[index]->span());
            }
            argument.forwarded = true;
            argument.value = storageVariableExpression(
                *variable, syntaxArguments[index]->span());
        } else if (parameter.modifier == ParameterModifier::Ref) {
            argument.value = std::move(arguments[index]);
        }
        result->arguments.push_back(std::move(argument));
    }
    return result;
}

std::unique_ptr<BoundExpression> Binder::bindCallExpression(
    const syntax::CallExpressionSyntax& syntaxTree) {
    std::vector<std::unique_ptr<BoundExpression>> arguments;
    for (const auto& argument : syntaxTree.arguments) {
        arguments.push_back(bindExpression(*argument));
    }

    if (currentOwnerType_ && !currentStaticMethod_) {
        for (const auto& event : currentOwnerType_->events) {
            if (event.name != syntaxTree.identifierToken.text) continue;
            auto result =
                std::make_unique<BoundEventInvocationExpression>();
            result->span = syntaxTree.span();
            result->type = PrimitiveType::Void;
            result->ownerType = *currentOwnerType_;
            result->event = event;
            const auto* thisVariable = lookupVariable("this");
            if (!thisVariable) return makeError(syntaxTree.span());
            result->receiver = variableExpression(
                *thisVariable, syntaxTree.span());
            if (arguments.size() != event.parameters.size()) {
                diagnostics_.report(
                    "RS8313",
                    "event argument count does not match delegate",
                    syntaxTree.span());
            }
            const auto count = std::min(
                arguments.size(), event.parameters.size());
            for (std::size_t index = 0; index < count; ++index) {
                if (index < syntaxTree.argumentModifiers.size() &&
                    syntaxTree.argumentModifiers[index]) {
                    diagnostics_.report(
                        "RS8314",
                        "event arguments cannot use ref, out, or in",
                        syntaxTree.arguments[index]->span());
                }
                result->arguments.push_back(convertExpression(
                    std::move(arguments[index]),
                    event.parameters[index].type,
                    syntaxTree.arguments[index]->span(),
                    "event argument",
                    event.parameters[index].typeName));
            }
            return result;
        }
    }

    std::vector<const FunctionSymbol*> candidates;
    const auto globals = visibleFunctions_.find(
        syntaxTree.identifierToken.text);
    if (globals != visibleFunctions_.end()) {
        for (const auto& function : globals->second) {
            if (!function.method) candidates.push_back(&function);
        }
    }
    if (currentOwnerType_ && !currentStaticMethod_) {
        auto methods = findMethods(
            *currentOwnerType_,
            syntaxTree.identifierToken.text,
            false);
        candidates.insert(
            candidates.end(), methods.begin(), methods.end());
    }

    bool ambiguous = false;
    const auto* best = selectBest(
        candidates,
        arguments,
        &syntaxTree.argumentModifiers, visibleTypes_, ambiguous);
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
            "call to function '" +
                syntaxTree.identifierToken.text +
                "' is ambiguous",
            syntaxTree.span());
        return makeError(syntaxTree.span());
    }

    std::unique_ptr<BoundExpression> receiver;
    if (best->method && !best->staticMethod) {
        const auto* thisVariable = lookupVariable("this");
        if (!thisVariable) return makeError(syntaxTree.span());
        receiver = variableExpression(
            *thisVariable, syntaxTree.span());
    }
    return bindSelectedCall(
        *best,
        std::move(arguments),
        syntaxTree.arguments,
        syntaxTree.argumentModifiers,
        std::move(receiver),
        syntaxTree.span(),
        "argument");
}

std::unique_ptr<BoundExpression> Binder::bindMemberCallExpression(
    const syntax::MemberCallExpressionSyntax& syntaxTree) {
    std::vector<std::unique_ptr<BoundExpression>> arguments;
    for (const auto& argument : syntaxTree.arguments) {
        arguments.push_back(bindExpression(*argument));
    }

    bool staticCall = false;
    TypeSymbol owner;
    std::unique_ptr<BoundExpression> receiver;
    if (syntaxTree.receiver->kind() ==
        syntax::SyntaxKind::NameExpression) {
        const auto& name = static_cast<
            const syntax::NameExpressionSyntax&>(
                *syntaxTree.receiver);
        const auto type = visibleTypes_.find(
            name.identifierToken.text);
        if (!lookupVariable(name.identifierToken.text) &&
            type != visibleTypes_.end()) {
            owner = type->second;
            staticCall = true;
        }
    }
    if (!staticCall) {
        receiver = bindExpression(*syntaxTree.receiver);
        if ((receiver->type != PrimitiveType::Object &&
             receiver->type != PrimitiveType::Struct) ||
            receiver->typeName.empty()) {
            diagnostics_.report(
                "RS2471",
                "member call requires a class or struct receiver",
                syntaxTree.receiver->span());
            return makeError(syntaxTree.span());
        }
        const auto type = visibleTypes_.find(
            receiver->typeName);
        if (type == visibleTypes_.end()) {
            diagnostics_.report(
                "RS2405",
                "type descriptor '" + receiver->typeName +
                    "' is unavailable",
                syntaxTree.receiver->span());
            return makeError(syntaxTree.span());
        }
        owner = type->second;
    }

    auto methods = findMethods(
        owner, syntaxTree.nameToken.text, staticCall);
    bool ambiguous = false;
    const auto* best = selectBest(
        methods,
        arguments,
        &syntaxTree.argumentModifiers, visibleTypes_, ambiguous);
    if (!best) {
        diagnostics_.report(
            "RS2472",
            "no applicable method '" +
                syntaxTree.nameToken.text +
                "' on type '" + canonicalTypeName(owner) + "'",
            syntaxTree.span());
        return makeError(syntaxTree.span());
    }
    if (ambiguous) {
        diagnostics_.report(
            "RS2473",
            "method call '" + syntaxTree.nameToken.text +
                "' is ambiguous",
            syntaxTree.span());
        return makeError(syntaxTree.span());
    }

    return bindSelectedCall(
        *best,
        std::move(arguments),
        syntaxTree.arguments,
        syntaxTree.argumentModifiers,
        std::move(receiver),
        syntaxTree.span(),
        "method argument",
        syntaxTree.receiver->kind() ==
            syntax::SyntaxKind::BaseExpression);
}

std::unique_ptr<BoundExpression> Binder::bindNewObjectExpression(
    const syntax::NewObjectExpressionSyntax& syntaxTree) {
    const auto found = visibleTypes_.find(syntaxTree.type.name.text);
    if (found == visibleTypes_.end() || found->second.kind == TypeKind::Enum) {
        diagnostics_.report("RS2403", "cannot allocate unknown or enum type '" + syntaxTree.type.name.text + "'", syntaxTree.type.span());
        return makeError(syntaxTree.span());
    }
    if (found->second.kind == TypeKind::Class && found->second.abstractType) {
        diagnostics_.report(
            "RS2524",
            "cannot instantiate abstract class '" +
                canonicalTypeName(found->second) + "'",
            syntaxTree.type.span());
        return makeError(syntaxTree.span());
    }
    std::vector<std::unique_ptr<BoundExpression>> arguments;
    for (const auto& argument : syntaxTree.arguments) arguments.push_back(bindExpression(*argument));
    std::vector<const FunctionSymbol*> constructors;
    for (const auto& ctor : found->second.constructors) constructors.push_back(&ctor);
    bool ambiguous = false;
    const FunctionSymbol* best = nullptr;
    if (!constructors.empty() || !arguments.empty()) best = selectBest(constructors, arguments, nullptr, visibleTypes_, ambiguous);
    const bool implicitStructDefault =
        found->second.kind == TypeKind::Struct && arguments.empty() && !best;
    if ((!arguments.empty() || !constructors.empty()) && !best &&
        !implicitStructDefault) {
        diagnostics_.report("RS2474", "no applicable constructor for type '" + canonicalTypeName(found->second) + "'", syntaxTree.span());
        return makeError(syntaxTree.span());
    }
    if (ambiguous) {
        diagnostics_.report("RS2475", "constructor call is ambiguous", syntaxTree.span());
        return makeError(syntaxTree.span());
    }

    if (found->second.kind == TypeKind::Struct) {
        auto result = std::make_unique<BoundNewStructExpression>();
        result->span = syntaxTree.span();
        result->type = PrimitiveType::Struct;
        result->typeName = canonicalTypeName(found->second);
        result->structType = found->second;
        if (best) result->constructor = *best;
        const auto offset = best ? visibleParameterOffset(*best) : 0;
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            result->arguments.push_back(convertExpression(
                std::move(arguments[i]), best->parameters[i + offset].type,
                syntaxTree.arguments[i]->span(), "constructor argument",
                best->parameters[i + offset].typeName));
        }
        return result;
    }

    auto result = std::make_unique<BoundNewObjectExpression>();
    result->span = syntaxTree.span();
    result->type = PrimitiveType::Object;
    result->typeName = canonicalTypeName(found->second);
    result->objectType = found->second;
    if (best) result->constructor = *best;
    const auto offset = best ? visibleParameterOffset(*best) : 0;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        result->arguments.push_back(convertExpression(
            std::move(arguments[i]), best->parameters[i + offset].type,
            syntaxTree.arguments[i]->span(), "constructor argument",
            best->parameters[i + offset].typeName));
    }
    return result;
}

std::unique_ptr<BoundExpression> Binder::bindNewArrayExpression(
    const syntax::NewArrayExpressionSyntax& syntaxTree) {
    std::string elementTypeName;
    const auto elementType = bindType(syntaxTree.elementType, false, &elementTypeName);
    if (elementType == PrimitiveType::Error || elementType == PrimitiveType::Void || elementType == PrimitiveType::Array) {
        diagnostics_.report("RS2420", "array element type is not supported", syntaxTree.elementType.span());
        return makeError(syntaxTree.span());
    }
    auto length = convertExpression(bindExpression(*syntaxTree.length), PrimitiveType::Int, syntaxTree.length->span(), "array length");
    auto result = std::make_unique<BoundNewArrayExpression>();
    result->span = syntaxTree.span();
    result->type = PrimitiveType::Array;
    result->typeName = arrayTypeName(elementType, elementTypeName);
    result->elementType = elementType;
    result->elementTypeName = std::move(elementTypeName);
    result->length = std::move(length);
    return result;
}

std::unique_ptr<BoundExpression> Binder::bindMemberAccessExpression(
    const syntax::MemberAccessExpressionSyntax& syntaxTree) {
    // Static enum member or static property.
    if (syntaxTree.receiver->kind() == syntax::SyntaxKind::NameExpression) {
        const auto& name = static_cast<const syntax::NameExpressionSyntax&>(*syntaxTree.receiver);
        const auto type = visibleTypes_.find(name.identifierToken.text);
        if (!lookupVariable(name.identifierToken.text) &&
            type != visibleTypes_.end()) {
            if (type->second.kind == TypeKind::Enum) {
                const auto* member = findEnumMember(type->second, syntaxTree.nameToken.text);
                if (!member) {
                    diagnostics_.report("RS2480", "enum '" + canonicalTypeName(type->second) + "' has no member '" + syntaxTree.nameToken.text + "'", syntaxTree.nameToken.span);
                    return makeError(syntaxTree.span());
                }
                auto result = std::make_unique<BoundLiteralExpression>();
                result->span = syntaxTree.span();
                result->type = PrimitiveType::Enum;
                result->typeName = canonicalTypeName(type->second);
                result->value = member->value;
                return result;
            }
            if (const auto* property = findProperty(
                    type->second,
                    syntaxTree.nameToken.text)) {
                if (!property->staticProperty) {
                    diagnostics_.report(
                        "RS2484",
                        "instance property '" + property->name +
                            "' requires a value receiver",
                        syntaxTree.nameToken.span);
                    return makeError(syntaxTree.span());
                }
                if (!property->getter) {
                    diagnostics_.report(
                        "RS2481",
                        "property '" + property->name + "' is write-only",
                        syntaxTree.nameToken.span);
                    return makeError(syntaxTree.span());
                }
                auto result = std::make_unique<BoundCallExpression>();
                result->span = syntaxTree.span();
                result->type = property->type;
                result->typeName = property->typeName;
                result->function = *property->getter;
                return result;
            }
        }
    }

    auto receiver = bindExpression(*syntaxTree.receiver);
    if (receiver->type == PrimitiveType::Array && syntaxTree.nameToken.text == "length") {
        auto result = std::make_unique<BoundArrayLengthExpression>();
        result->span = syntaxTree.span();
        result->type = PrimitiveType::Int;
        result->receiver = std::move(receiver);
        return result;
    }
    if ((receiver->type != PrimitiveType::Object && receiver->type != PrimitiveType::Struct) || receiver->typeName.empty()) {
        diagnostics_.report("RS2404", "member access requires a class or struct value", syntaxTree.receiver->span());
        return makeError(syntaxTree.span());
    }
    const auto type = visibleTypes_.find(receiver->typeName);
    if (type == visibleTypes_.end()) {
        diagnostics_.report("RS2405", "type descriptor '" + receiver->typeName + "' is unavailable", syntaxTree.receiver->span());
        return makeError(syntaxTree.span());
    }
    if (const auto* field = findField(type->second, syntaxTree.nameToken.text)) {
        if (type->second.kind == TypeKind::Struct) {
            auto result = std::make_unique<BoundStructFieldAccessExpression>();
            result->span = syntaxTree.span();
            result->type = field->type;
            result->typeName = field->typeName;
            result->receiver = std::move(receiver);
            result->ownerType = type->second;
            result->field = *field;
            return result;
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
    if (const auto* property = findProperty(type->second, syntaxTree.nameToken.text)) {
        if (property->staticProperty) {
            diagnostics_.report(
                "RS2484",
                "static property '" + property->name +
                    "' must be accessed through its type",
                syntaxTree.nameToken.span);
            return makeError(syntaxTree.span());
        }
        if (!property->getter) {
            diagnostics_.report("RS2481", "property '" + property->name + "' is write-only", syntaxTree.nameToken.span);
            return makeError(syntaxTree.span());
        }
        auto result = std::make_unique<BoundCallExpression>();
        result->span = syntaxTree.span();
        result->type = property->type;
        result->typeName = property->typeName;
        result->function = *property->getter;
        result->arguments.push_back(std::move(receiver));
        return result;
    }
    diagnostics_.report("RS2406", "type '" + receiver->typeName + "' has no member '" + syntaxTree.nameToken.text + "'", syntaxTree.nameToken.span);
    return makeError(syntaxTree.span());
}

std::unique_ptr<BoundExpression> Binder::bindElementAccessExpression(
    const syntax::ElementAccessExpressionSyntax& syntaxTree) {
    auto receiver = bindExpression(*syntaxTree.receiver);
    if (receiver->type != PrimitiveType::Array || receiver->typeName.empty()) {
        diagnostics_.report("RS2421", "element access requires an array", syntaxTree.receiver->span());
        return makeError(syntaxTree.span());
    }
    PrimitiveType elementType = PrimitiveType::Error;
    std::string elementTypeName;
    if (!decodeArrayTypeName(receiver->typeName, elementType, elementTypeName)) {
        diagnostics_.report("RS2422", "array element descriptor is invalid", syntaxTree.receiver->span());
        return makeError(syntaxTree.span());
    }
    // Resolve exact named element kind when available.
    if (!elementTypeName.empty()) {
        const auto found = visibleTypes_.find(elementTypeName);
        if (found != visibleTypes_.end()) {
            elementType = found->second.kind == TypeKind::Class ? PrimitiveType::Object :
                found->second.kind == TypeKind::Struct ? PrimitiveType::Struct : PrimitiveType::Enum;
        }
    }
    auto index = convertExpression(bindExpression(*syntaxTree.index), PrimitiveType::Int, syntaxTree.index->span(), "array index");
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
    // Static property assignment.
    if (syntaxTree.receiver->kind() == syntax::SyntaxKind::NameExpression) {
        const auto& name = static_cast<const syntax::NameExpressionSyntax&>(*syntaxTree.receiver);
        const auto type = visibleTypes_.find(name.identifierToken.text);
        if (!lookupVariable(name.identifierToken.text) &&
            type != visibleTypes_.end()) {
            if (const auto* property = findProperty(
                    type->second,
                    syntaxTree.nameToken.text)) {
                if (!property->staticProperty) {
                    diagnostics_.report(
                        "RS2484",
                        "instance property '" + property->name +
                            "' requires a value receiver",
                        syntaxTree.nameToken.span);
                    return makeError(syntaxTree.span());
                }
                if (!property->setter) {
                    diagnostics_.report(
                        "RS2483",
                        "property '" + property->name + "' is read-only",
                        syntaxTree.nameToken.span);
                    return makeError(syntaxTree.span());
                }
                auto assigned = convertExpression(
                    bindExpression(*syntaxTree.expression),
                    property->type,
                    syntaxTree.expression->span(),
                    "property assignment",
                    property->typeName);
                auto result = std::make_unique<BoundPropertyAssignmentExpression>();
                result->span = syntaxTree.span();
                result->type = property->type;
                result->typeName = property->typeName;
                result->setter = *property->setter;
                result->assignedValue = std::move(assigned);
                return result;
            }
        }
    }

    auto receiver = bindExpression(*syntaxTree.receiver);
    if ((receiver->type != PrimitiveType::Object && receiver->type != PrimitiveType::Struct) || receiver->typeName.empty()) {
        diagnostics_.report("RS2404", "member assignment requires a class or struct value", syntaxTree.receiver->span());
        return makeError(syntaxTree.span());
    }
    const auto type = visibleTypes_.find(receiver->typeName);
    if (type == visibleTypes_.end()) {
        diagnostics_.report("RS2405", "type descriptor '" + receiver->typeName + "' is unavailable", syntaxTree.receiver->span());
        return makeError(syntaxTree.span());
    }
    if (const auto* field = findField(type->second, syntaxTree.nameToken.text)) {
        auto value = convertExpression(bindExpression(*syntaxTree.expression), field->type, syntaxTree.expression->span(), "field assignment", field->typeName);
        if (type->second.kind == TypeKind::Struct) {
            const VariableSymbol* variable = nullptr;
            if (receiver->kind() == BoundNodeKind::VariableExpression) {
                variable = &static_cast<const BoundVariableExpression&>(*receiver).variable;
            }
            if (!variable) {
                diagnostics_.report("RS2482", "struct field assignment requires a variable receiver", syntaxTree.receiver->span());
                return makeError(syntaxTree.span());
            }
            if (variable->name == "this" && !currentConstructor_) {
                diagnostics_.report(
                    "RS2485",
                    "struct instance methods are read-only; mutate a local struct variable or assign fields in a constructor",
                    syntaxTree.receiver->span());
                return makeError(syntaxTree.span());
            }
            auto result = std::make_unique<BoundStructFieldAssignmentExpression>();
            result->span = syntaxTree.span();
            result->type = field->type;
            result->typeName = field->typeName;
            result->variable = *variable;
            result->ownerType = type->second;
            result->field = *field;
            result->expression = std::move(value);
            return result;
        }
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
    if (const auto* property = findProperty(type->second, syntaxTree.nameToken.text)) {
        if (property->staticProperty) {
            diagnostics_.report(
                "RS2484",
                "static property '" + property->name +
                    "' must be accessed through its type",
                syntaxTree.nameToken.span);
            return makeError(syntaxTree.span());
        }
        if (!property->setter) {
            diagnostics_.report("RS2483", "property '" + property->name + "' is read-only", syntaxTree.nameToken.span);
            return makeError(syntaxTree.span());
        }
        if (type->second.kind == TypeKind::Struct && property->backingFieldIndex != std::numeric_limits<std::size_t>::max()) {
            const VariableSymbol* variable = nullptr;
            if (receiver->kind() == BoundNodeKind::VariableExpression) variable = &static_cast<const BoundVariableExpression&>(*receiver).variable;
            if (!variable) {
                diagnostics_.report("RS2482", "struct property assignment requires a variable receiver", syntaxTree.receiver->span());
                return makeError(syntaxTree.span());
            }
            if (variable->name == "this" && !currentConstructor_) {
                diagnostics_.report(
                    "RS2486",
                    "struct properties cannot be assigned through an instance method in the Phase 3E value model",
                    syntaxTree.receiver->span());
                return makeError(syntaxTree.span());
            }
            const auto& field = type->second.fields[property->backingFieldIndex];
            auto result = std::make_unique<BoundStructFieldAssignmentExpression>();
            result->span = syntaxTree.span();
            result->type = property->type;
            result->typeName = property->typeName;
            result->variable = *variable;
            result->ownerType = type->second;
            result->field = field;
            result->expression = convertExpression(bindExpression(*syntaxTree.expression), property->type, syntaxTree.expression->span(), "property assignment", property->typeName);
            return result;
        }
        auto assigned = convertExpression(bindExpression(*syntaxTree.expression), property->type, syntaxTree.expression->span(), "property assignment", property->typeName);
        auto result = std::make_unique<BoundPropertyAssignmentExpression>();
        result->span = syntaxTree.span();
        result->type = property->type;
        result->typeName = property->typeName;
        result->setter = *property->setter;
        result->arguments.push_back(std::move(receiver));
        result->assignedValue = std::move(assigned);
        return result;
    }
    diagnostics_.report("RS2406", "type '" + receiver->typeName + "' has no member '" + syntaxTree.nameToken.text + "'", syntaxTree.nameToken.span);
    return makeError(syntaxTree.span());
}

std::unique_ptr<BoundExpression> Binder::bindElementAssignmentExpression(
    const syntax::ElementAssignmentExpressionSyntax& syntaxTree) {
    auto receiver = bindExpression(*syntaxTree.receiver);
    if (receiver->type != PrimitiveType::Array || receiver->typeName.empty()) {
        diagnostics_.report("RS2421", "element assignment requires an array", syntaxTree.receiver->span());
        return makeError(syntaxTree.span());
    }
    PrimitiveType elementType = PrimitiveType::Error;
    std::string elementTypeName;
    if (!decodeArrayTypeName(receiver->typeName, elementType, elementTypeName)) {
        diagnostics_.report("RS2422", "array element descriptor is invalid", syntaxTree.receiver->span());
        return makeError(syntaxTree.span());
    }
    if (!elementTypeName.empty()) {
        const auto found = visibleTypes_.find(elementTypeName);
        if (found != visibleTypes_.end()) {
            elementType = found->second.kind == TypeKind::Class ? PrimitiveType::Object :
                found->second.kind == TypeKind::Struct ? PrimitiveType::Struct : PrimitiveType::Enum;
        }
    }
    auto index = convertExpression(bindExpression(*syntaxTree.index), PrimitiveType::Int, syntaxTree.index->span(), "array index");
    auto value = convertExpression(bindExpression(*syntaxTree.expression), elementType, syntaxTree.expression->span(), "array element assignment", elementTypeName);
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
    if (!expression) return makeError(span);
    if (expression->type == PrimitiveType::Error || target == PrimitiveType::Error) return expression;

    if (isExactType(target) && expression->type == target &&
        !sameExactType(
            *expression, target, targetTypeName, visibleTypes_)) {
        diagnostics_.report(
            "RS2410",
            "cannot convert exact type '" + expression->typeName +
                "' to '" + targetTypeName + "' for " + context,
            span);
        return makeError(span);
    }
    const auto conversion = classifyConversion(expression->type, target);
    if (conversion == ConversionKind::None) {
        diagnostics_.report(
            "RS2106",
            "cannot convert '" + std::string(primitiveTypeName(expression->type)) +
                "' to '" + primitiveTypeName(target) + "' for " + context,
            span);
        return makeError(span);
    }
    if (conversion == ConversionKind::Identity) {
        if (isExactType(target) && !targetTypeName.empty()) expression->typeName = std::move(targetTypeName);
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
