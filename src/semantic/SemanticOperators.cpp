#include "SemanticInternal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <unordered_set>
#include <utility>

namespace realscript::semantic {

using namespace detail;

std::unique_ptr<BoundExpression> Binder::bindStaticFieldInitializer(
    const TypeSymbol& ownerType,
    const FieldSymbol& field) {
    if (!field.initializerSyntax || !bindingStaticFields_.insert(field.id).second) {
        return nullptr;
    }
    const TypeSymbol* declType = &ownerType;
    if (field.declaringTypeId != 0 && field.declaringTypeId != ownerType.id) {
        if (const auto* foundDecl = findVisibleType(field.declaringTypeId)) {
            declType = foundDecl;
        }
    }
    const auto savedOwner = currentOwnerType_;
    const auto savedStatic = currentStaticMethod_;
    const auto savedConstructor = currentConstructor_;
    const auto savedModule = currentModuleName_;
    const auto savedHygienic = hygienicAccessibleModules_;
    std::vector<std::unordered_map<std::string, VariableSymbol>> savedScopes;
    std::vector<std::unordered_map<std::string, VariableSymbol>> savedAliases;
    std::swap(scopes_, savedScopes);
    std::swap(referenceAliasScopes_, savedAliases);
    currentOwnerType_ = *declType;
    currentStaticMethod_ = true;
    currentConstructor_ = false;
    if (!declType->moduleName.empty()) {
        currentModuleName_ = declType->moduleName;
    }
    hygienicAccessibleModules_.insert(
        declType->hygienicAccessibleModules.begin(),
        declType->hygienicAccessibleModules.end());
    auto bound = bindTargetExpression(
        *field.initializerSyntax,
        field.type,
        field.typeName,
        field.staticField ? "static field initializer" : "field initializer");
    hygienicAccessibleModules_ = savedHygienic;
    currentModuleName_ = savedModule;
    currentConstructor_ = savedConstructor;
    currentStaticMethod_ = savedStatic;
    currentOwnerType_ = savedOwner;
    std::swap(scopes_, savedScopes);
    std::swap(referenceAliasScopes_, savedAliases);
    bindingStaticFields_.erase(field.id);
    return bound;
}


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
        const auto& txt = syntaxTree.literalToken.text;
        const bool hasUnsignedSuffix =
            txt.find('u') != std::string::npos ||
            txt.find('U') != std::string::npos;
        const bool hasLongSuffix =
            txt.find('l') != std::string::npos ||
            txt.find('L') != std::string::npos;
        if (hasUnsignedSuffix && hasLongSuffix) {
            result->type = PrimitiveType::ULong;
        } else if (hasUnsignedSuffix) {
            const auto uval = static_cast<std::uint64_t>(value);
            result->type = uval > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())
                ? PrimitiveType::ULong
                : PrimitiveType::UInt;
        } else if (hasLongSuffix ||
                   value < std::numeric_limits<std::int32_t>::min() ||
                   value > std::numeric_limits<std::int32_t>::max()) {
            result->type = PrimitiveType::Long;
        } else {
            result->type = PrimitiveType::Int;
        }
        break;
    }
    case syntax::SyntaxKind::CharLiteralToken:
        result->type = PrimitiveType::Char;
        break;
    case syntax::SyntaxKind::FloatLiteralToken: {
        const bool hasFloatSuffix = !syntaxTree.literalToken.text.empty() &&
            (syntaxTree.literalToken.text.back() == 'f' ||
             syntaxTree.literalToken.text.back() == 'F');
        result->type = hasFloatSuffix
            ? PrimitiveType::Float
            : PrimitiveType::Double;
        break;
    }
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
    return makeVariableAccess(*variable, syntaxTree.span());
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


std::unique_ptr<BoundExpression> Binder::bindUnaryExpression(
    const syntax::UnaryExpressionSyntax& syntaxTree) {
    if (syntaxTree.operatorToken.kind == syntax::SyntaxKind::AwaitKeyword) {
        if (!currentFunctionIsAsync_) {
            diagnostics_.report(
                "RS2604",
                "the 'await' operator can only be used within an 'async' function",
                syntaxTree.operatorToken.span);
        }
        auto operand = bindExpression(*syntaxTree.operand);
        if (operand->type == PrimitiveType::Error) {
            return makeError(syntaxTree.span());
        }
        if (operand->type == PrimitiveType::Struct && !operand->typeName.empty()) {
            const auto foundType = visibleTypes_.find(operand->typeName);
            if (operand->typeName == "RealScript.Async::Awaitable" ||
                operand->typeName == "Awaitable" ||
                (foundType != visibleTypes_.end() &&
                 foundType->second.name == "Awaitable")) {
                auto res = std::make_unique<BoundAwaitExpression>();
                res->span = syntaxTree.span();
                res->type = PrimitiveType::Void;
                res->operandTypeId = foundType != visibleTypes_.end()
                    ? foundType->second.id
                    : stableTypeId("RealScript.Async::Awaitable");
                res->resultTypeId = 0;
                res->operand = std::move(operand);
                return res;
            }
            if (foundType != visibleTypes_.end() &&
                foundType->second.name.rfind("Awaitable__", 0) == 0) {
                for (const auto& m : foundType->second.methods) {
                    if (m.staticMethod && m.name == "FromResult" &&
                        m.parameters.size() == 1) {
                        auto res = std::make_unique<BoundAwaitExpression>();
                        res->span = syntaxTree.span();
                        res->type = m.parameters[0].type;
                        res->typeName = m.parameters[0].typeName;
                        res->operandTypeId = foundType->second.id;
                        res->resultTypeId =
                            (isExactType(res->type) || res->type == PrimitiveType::Handle) &&
                            !res->typeName.empty()
                                ? stableTypeId(res->typeName)
                                : 0;
                        res->operand = std::move(operand);
                        return res;
                    }
                }
            }
        }
        diagnostics_.report(
            "RS2605",
            "type '" + (operand->typeName.empty()
                ? std::string(primitiveTypeName(operand->type))
                : operand->typeName) + "' is not awaitable",
            syntaxTree.operand->span());
        return makeError(syntaxTree.span());
    }

    auto operand = bindExpression(*syntaxTree.operand);
    if (operand->type == PrimitiveType::Error) return makeError(syntaxTree.span());

    if ((operand->type == PrimitiveType::Struct ||
         operand->type == PrimitiveType::Object) &&
        !operand->typeName.empty()) {
        const auto opMethodName = unaryOperatorMethodName(
            syntaxTree.operatorToken.kind);
        const auto owner = visibleTypes_.find(operand->typeName);
        if (!opMethodName.empty() && owner != visibleTypes_.end()) {
            auto methods = findMethods(owner->second, opMethodName, true);
            methods.erase(
                std::remove_if(
                    methods.begin(), methods.end(),
                    [&](const auto* method) {
                        return !isMemberAccessible(
                            method->accessibility,
                            method->declaringTypeId,
                            method->moduleName);
                    }),
                methods.end());
            if (!methods.empty()) {
                std::vector<std::unique_ptr<BoundExpression>> arguments;
                arguments.push_back(std::move(operand));
                bool ambiguous = false;
                const auto* best = selectBest(
                    methods, arguments, nullptr, visibleTypes_, ambiguous);
                if (ambiguous) {
                    diagnostics_.report(
                        "RS2473",
                        "unary operator '" + syntaxTree.operatorToken.text +
                            "' is ambiguous",
                        syntaxTree.operatorToken.span);
                    return makeError(syntaxTree.span());
                }
                if (best) {
                    auto result = std::make_unique<BoundCallExpression>();
                    result->span = syntaxTree.span();
                    result->type = storageReturnTypeOf(*best);
                    result->typeName = storageReturnTypeNameOf(*best);
                    result->function = *best;
                    result->arguments.push_back(convertExpression(
                        std::move(arguments[0]),
                        best->parameters[0].type,
                        syntaxTree.operand->span(),
                        "unary operator operand",
                        best->parameters[0].typeName));
                    result->argumentEvaluationOrder = {0};
                    return result;
                }
                operand = std::move(arguments[0]);
            }
        }
    }

    BoundUnaryOperatorKind operatorKind = BoundUnaryOperatorKind::Identity;
    PrimitiveType resultType = operand->type;
    switch (syntaxTree.operatorToken.kind) {
    case syntax::SyntaxKind::PlusToken:
        if (!isNumericType(operand->type)) goto invalidOperator;
        if (operand->type == PrimitiveType::Byte ||
            operand->type == PrimitiveType::SByte ||
            operand->type == PrimitiveType::Short ||
            operand->type == PrimitiveType::UShort ||
            operand->type == PrimitiveType::Char) {
            operand = convertExpression(
                std::move(operand), PrimitiveType::Int,
                syntaxTree.operand->span(), "unary numeric promotion");
            resultType = PrimitiveType::Int;
        }
        operatorKind = BoundUnaryOperatorKind::Identity;
        break;
    case syntax::SyntaxKind::MinusToken:
        if (!isNumericType(operand->type) ||
            operand->type == PrimitiveType::ULong) goto invalidOperator;
        if (operand->type == PrimitiveType::UInt) {
            operand = convertExpression(
                std::move(operand), PrimitiveType::Long,
                syntaxTree.operand->span(), "unary numeric promotion");
            resultType = PrimitiveType::Long;
        } else if (operand->type == PrimitiveType::Byte ||
                   operand->type == PrimitiveType::SByte ||
                   operand->type == PrimitiveType::Short ||
                   operand->type == PrimitiveType::UShort ||
                   operand->type == PrimitiveType::Char) {
            operand = convertExpression(
                std::move(operand), PrimitiveType::Int,
                syntaxTree.operand->span(), "unary numeric promotion");
            resultType = PrimitiveType::Int;
        }
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
        result->checkedArithmetic = checkedArithmetic_;
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
    return bindBinaryOperation(
        std::move(left),
        syntaxTree.left->span(),
        syntaxTree.operatorToken,
        std::move(right),
        syntaxTree.right->span(),
        syntaxTree.span());
}

std::unique_ptr<BoundExpression> Binder::bindBinaryOperation(
    std::unique_ptr<BoundExpression> left,
    text::TextSpan leftSpan,
    const syntax::SyntaxToken& operatorToken,
    std::unique_ptr<BoundExpression> right,
    text::TextSpan rightSpan,
    text::TextSpan span) {
    if (!left || !right ||
        left->type == PrimitiveType::Error ||
        right->type == PrimitiveType::Error) {
        return makeError(span);
    }

    if (operatorToken.kind ==
        syntax::SyntaxKind::QuestionQuestionToken) {
        const auto nullable = left->type == PrimitiveType::Struct
            ? visibleTypes_.find(left->typeName)
            : visibleTypes_.end();
        const bool nullableValue = nullable != visibleTypes_.end() &&
            nullable->second.name.rfind("Nullable__", 0) == 0 &&
            nullable->second.fields.size() >= 2;
        if (!nullableValue && left->type != PrimitiveType::String &&
            left->type != PrimitiveType::Object &&
            left->type != PrimitiveType::Array) {
            diagnostics_.report(
                "RS8905",
                "left operand of '" "??" "' must be a nullable reference value",
                leftSpan);
            return makeError(span);
        }
        auto result = std::make_unique<BoundNullCoalescingExpression>();
        result->span = span;
        result->nullableValue = nullableValue;
        if (nullableValue) {
            result->nullableType = nullable->second;
            result->hasValueField = nullable->second.fields[0];
            result->valueField = nullable->second.fields[1];
            result->type = result->valueField.type;
            result->typeName = result->valueField.typeName;
        } else {
            result->type = left->type;
            result->typeName = left->typeName;
        }
        result->left = std::move(left);
        result->right = convertExpression(
            std::move(right), result->type, rightSpan,
            "null-coalescing fallback", result->typeName);
        return result;
    }

    BoundBinaryOperatorKind operatorKind = BoundBinaryOperatorKind::Addition;
    PrimitiveType resultType = PrimitiveType::Error;
    const auto tokenKind = operatorToken.kind;

    if ((tokenKind == syntax::SyntaxKind::PlusToken ||
         tokenKind == syntax::SyntaxKind::MinusToken) &&
        left->type == PrimitiveType::Object &&
        right->type == PrimitiveType::Object &&
        !left->typeName.empty() &&
        left->typeName == right->typeName) {
        const auto found = visibleTypes_.find(left->typeName);
        if (found != visibleTypes_.end() &&
            found->second.delegateType) {
            auto result = std::make_unique<
                BoundDelegateCombinationExpression>();
            result->span = span;
            result->type = PrimitiveType::Object;
            result->typeName = left->typeName;
            result->delegateType = found->second;
            result->remove = tokenKind == syntax::SyntaxKind::MinusToken;
            result->left = std::move(left);
            result->right = std::move(right);
            return result;
        }
    }

    if (left->type == PrimitiveType::Struct ||
        right->type == PrimitiveType::Struct ||
        left->type == PrimitiveType::Object ||
        right->type == PrimitiveType::Object) {
        const auto opMethodName = binaryOperatorMethodName(tokenKind);
        if (!opMethodName.empty()) {
            std::vector<const FunctionSymbol*> candidates;
            std::unordered_set<SymbolId> seenIds;
            const auto addOwnerCandidates = [&](const std::string& typeName) {
                if (typeName.empty()) return;
                const auto owner = visibleTypes_.find(typeName);
                if (owner == visibleTypes_.end()) return;
                for (const auto* method :
                     findMethods(owner->second, opMethodName, true)) {
                    if (isMemberAccessible(
                            method->accessibility,
                            method->declaringTypeId,
                            method->moduleName) &&
                        seenIds.insert(method->id).second) {
                        candidates.push_back(method);
                    }
                }
            };
            if (left->type == PrimitiveType::Struct ||
                left->type == PrimitiveType::Object) {
                addOwnerCandidates(left->typeName);
            }
            if (right->type == PrimitiveType::Struct ||
                right->type == PrimitiveType::Object) {
                addOwnerCandidates(right->typeName);
            }
            if (!candidates.empty()) {
                std::vector<std::unique_ptr<BoundExpression>> arguments;
                arguments.push_back(std::move(left));
                arguments.push_back(std::move(right));
                bool ambiguous = false;
                const auto* best = selectBest(
                    candidates, arguments, nullptr, visibleTypes_, ambiguous);
                if (ambiguous) {
                    diagnostics_.report(
                        "RS2473",
                        "binary operator '" + operatorToken.text +
                            "' is ambiguous",
                        operatorToken.span);
                    return makeError(span);
                }
                if (best) {
                    auto result = std::make_unique<BoundCallExpression>();
                    result->span = span;
                    result->type = storageReturnTypeOf(*best);
                    result->typeName = storageReturnTypeNameOf(*best);
                    result->function = *best;
                    result->arguments.push_back(convertExpression(
                        std::move(arguments[0]),
                        best->parameters[0].type,
                        leftSpan,
                        "binary operator left operand",
                        best->parameters[0].typeName));
                    result->arguments.push_back(convertExpression(
                        std::move(arguments[1]),
                        best->parameters[1].type,
                        rightSpan,
                        "binary operator right operand",
                        best->parameters[1].typeName));
                    result->argumentEvaluationOrder = {0, 1};
                    return result;
                }
                left = std::move(arguments[0]);
                right = std::move(arguments[1]);
            }
        }
    }

    if (tokenKind == syntax::SyntaxKind::PlusToken &&
        (left->type == PrimitiveType::String || right->type == PrimitiveType::String)) {
        if (left->type == PrimitiveType::Void || right->type == PrimitiveType::Void) {
            goto invalidOperator;
        }
        auto ensureStringOperand = [&](std::unique_ptr<BoundExpression> expr,
                                       text::TextSpan exprSpan)
            -> std::unique_ptr<BoundExpression> {
            if (expr->type == PrimitiveType::String) return expr;
            if (expr->kind() == BoundNodeKind::LiteralExpression &&
                expr->type != PrimitiveType::Enum) {
                const auto& lit = static_cast<const BoundLiteralExpression&>(*expr);
                auto strLit = std::make_unique<BoundLiteralExpression>();
                strLit->span = exprSpan;
                strLit->type = PrimitiveType::String;
                strLit->value = formatLiteralTokenToString(lit.value);
                return strLit;
            }
            const auto intrinsicName = "to_string::" +
                std::to_string(static_cast<int>(expr->type)) + "::" +
                expr->typeName;
            auto call = std::make_unique<BoundCallExpression>();
            call->span = exprSpan;
            call->type = PrimitiveType::String;
            call->function = makeIntrinsicFunction(
                intrinsicName,
                PrimitiveType::String,
                {},
                {{expr->type, expr->typeName}});
            call->arguments.push_back(std::move(expr));
            call->argumentEvaluationOrder = {0};
            return call;
        };
        left = ensureStringOperand(std::move(left), leftSpan);
        right = ensureStringOperand(std::move(right), rightSpan);
        const auto* leftLit = left->kind() == BoundNodeKind::LiteralExpression
            ? static_cast<const BoundLiteralExpression*>(left.get())
            : nullptr;
        const auto* rightLit = right->kind() == BoundNodeKind::LiteralExpression
            ? static_cast<const BoundLiteralExpression*>(right.get())
            : nullptr;
        if (leftLit && rightLit &&
            std::holds_alternative<std::string>(leftLit->value) &&
            std::holds_alternative<std::string>(rightLit->value)) {
            auto folded = std::make_unique<BoundLiteralExpression>();
            folded->span = span;
            folded->type = PrimitiveType::String;
            folded->value = std::get<std::string>(leftLit->value) +
                std::get<std::string>(rightLit->value);
            return folded;
        }
        if (leftLit && std::holds_alternative<std::string>(leftLit->value) &&
            std::get<std::string>(leftLit->value).empty()) {
            right->span = span;
            return right;
        }
        if (rightLit && std::holds_alternative<std::string>(rightLit->value) &&
            std::get<std::string>(rightLit->value).empty()) {
            left->span = span;
            return left;
        }
        auto concatCall = std::make_unique<BoundCallExpression>();
        concatCall->span = span;
        concatCall->type = PrimitiveType::String;
        concatCall->function = makeIntrinsicFunction(
            "string_concat",
            PrimitiveType::String,
            {},
            {{PrimitiveType::String, {}}, {PrimitiveType::String, {}}});
        concatCall->arguments.push_back(std::move(left));
        concatCall->arguments.push_back(std::move(right));
        concatCall->argumentEvaluationOrder = {0, 1};
        return concatCall;
    }

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
        left = convertExpression(std::move(left), common, leftSpan, "numeric operand");
        right = convertExpression(std::move(right), common, rightSpan, "numeric operand");
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
        left = convertExpression(std::move(left), common, leftSpan, "comparison operand");
        right = convertExpression(std::move(right), common, rightSpan, "comparison operand");
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
                left = convertExpression(std::move(left), right->type, leftSpan, "equality operand", right->typeName);
            } else if (rightToLeft != ConversionKind::None) {
                right = convertExpression(std::move(right), left->type, rightSpan, "equality operand", left->typeName);
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
        result->span = span;
        result->type = resultType;
        result->operatorKind = operatorKind;
        result->checkedArithmetic = checkedArithmetic_;
        result->left = std::move(left);
        result->right = std::move(right);
        return result;
    }
invalidOperator:
    diagnostics_.report(
        "RS2104",
        "binary operator '" + operatorToken.text +
            "' is not defined for '" + primitiveTypeName(left->type) +
            "' and '" + primitiveTypeName(right->type) + "'",
        operatorToken.span);
    return makeError(span);
}

std::unique_ptr<BoundExpression> Binder::bindConditionalExpression(
    const syntax::ConditionalExpressionSyntax& syntaxTree) {
    pushScope(syntaxTree.span());
    struct ScopeGuard {
        Binder& binder;
        ~ScopeGuard() { binder.popScope(); }
    } scopeGuard{*this};
    auto condition = convertExpression(
        bindExpression(*syntaxTree.condition), PrimitiveType::Bool,
        syntaxTree.condition->span(), "conditional expression condition");
    auto whenTrue = bindExpression(*syntaxTree.whenTrue);
    auto whenFalse = bindExpression(*syntaxTree.whenFalse);
    if (condition->type == PrimitiveType::Error ||
        whenTrue->type == PrimitiveType::Error ||
        whenFalse->type == PrimitiveType::Error) {
        return makeError(syntaxTree.span());
    }

    PrimitiveType resultType = PrimitiveType::Error;
    std::string resultTypeName;
    if (whenTrue->type == PrimitiveType::Null &&
        classifyConversion(PrimitiveType::Null, whenFalse->type) !=
            ConversionKind::None) {
        resultType = whenFalse->type;
        resultTypeName = whenFalse->typeName;
    } else if (whenFalse->type == PrimitiveType::Null &&
               classifyConversion(PrimitiveType::Null, whenTrue->type) !=
                   ConversionKind::None) {
        resultType = whenTrue->type;
        resultTypeName = whenTrue->typeName;
    } else if (whenTrue->type == whenFalse->type &&
               (!isExactType(whenTrue->type) ||
                whenTrue->typeName == whenFalse->typeName)) {
        resultType = whenTrue->type;
        resultTypeName = whenTrue->typeName;
    } else if (isNumericType(whenTrue->type) &&
               isNumericType(whenFalse->type)) {
        resultType = commonNumericType(whenTrue->type, whenFalse->type);
    } else if (whenTrue->type == PrimitiveType::Object &&
               whenFalse->type == PrimitiveType::Object) {
        std::unordered_set<SymbolId> trueAncestors;
        const auto* current = findVisibleType(
            stableTypeId(whenTrue->typeName));
        while (current && trueAncestors.insert(current->id).second) {
            current = current->baseTypeId != 0
                ? findVisibleType(current->baseTypeId)
                : nullptr;
        }
        current = findVisibleType(stableTypeId(whenFalse->typeName));
        while (current) {
            if (trueAncestors.find(current->id) != trueAncestors.end()) {
                resultType = PrimitiveType::Object;
                resultTypeName = canonicalTypeName(*current);
                break;
            }
            current = current->baseTypeId != 0
                ? findVisibleType(current->baseTypeId)
                : nullptr;
        }
    } else if (classifyConversion(whenTrue->type, whenFalse->type) !=
               ConversionKind::None) {
        resultType = whenFalse->type;
        resultTypeName = whenFalse->typeName;
    } else if (classifyConversion(whenFalse->type, whenTrue->type) !=
               ConversionKind::None) {
        resultType = whenTrue->type;
        resultTypeName = whenTrue->typeName;
    }
    if (resultType == PrimitiveType::Error) {
        diagnostics_.report(
            "RS8904",
            "conditional expression arms do not have a common type",
            syntaxTree.span());
        return makeError(syntaxTree.span());
    }

    auto result = std::make_unique<BoundConditionalExpression>();
    result->span = syntaxTree.span();
    result->type = resultType;
    result->typeName = resultTypeName;
    result->condition = std::move(condition);
    result->whenTrue = convertExpression(
        std::move(whenTrue), resultType, syntaxTree.whenTrue->span(),
        "conditional expression arm", resultTypeName);
    result->whenFalse = convertExpression(
        std::move(whenFalse), resultType, syntaxTree.whenFalse->span(),
        "conditional expression arm", resultTypeName);
    return result;
}


std::unique_ptr<BoundExpression> Binder::bindCastExpression(
    const syntax::CastExpressionSyntax& syntaxTree) {
    std::string targetTypeName;
    const auto target = bindType(
        syntaxTree.type, false, &targetTypeName);
    auto expression = bindExpression(*syntaxTree.expression);
    if (!expression || expression->type == PrimitiveType::Error ||
        target == PrimitiveType::Error) {
        return makeError(syntaxTree.span());
    }
    if (expression->type == target &&
        (!isExactType(target) || targetTypeName.empty() ||
         expression->typeName == targetTypeName)) {
        return expression;
    }
    ConversionKind conversion = classifyConversion(
        expression->type, target);
    if (isNumericType(expression->type) && isNumericType(target)) {
        conversion = ConversionKind::Numeric;
    }
    if (conversion == ConversionKind::None ||
        (isExactType(target) && expression->type == target &&
         !sameExactType(*expression, target, targetTypeName, visibleTypes_))) {
        bool convAmbiguous = false;
        const auto* userConv = findUserDefinedConversion(
            expression->type,
            expression->typeName,
            target,
            targetTypeName,
            true,
            visibleTypes_,
            convAmbiguous);
        if (convAmbiguous) {
            diagnostics_.report(
                "RS2473",
                "user-defined conversion is ambiguous",
                syntaxTree.span());
            return makeError(syntaxTree.span());
        }
        if (userConv &&
            isMemberAccessible(
                userConv->accessibility,
                userConv->declaringTypeId,
                userConv->moduleName)) {
            auto call = std::make_unique<BoundCallExpression>();
            call->span = syntaxTree.span();
            call->type = storageReturnTypeOf(*userConv);
            call->typeName = storageReturnTypeNameOf(*userConv);
            call->function = *userConv;
            call->arguments.push_back(convertExpression(
                std::move(expression),
                userConv->parameters[0].type,
                syntaxTree.expression->span(),
                "conversion operator operand",
                userConv->parameters[0].typeName));
            call->argumentEvaluationOrder = {0};
            return call;
        }
    }
    if (conversion == ConversionKind::None) {
        diagnostics_.report(
            "RS2106",
            "cannot explicitly convert '" +
                std::string(primitiveTypeName(expression->type)) +
                "' to '" + primitiveTypeName(target) + "'",
            syntaxTree.span());
        return makeError(syntaxTree.span());
    }
    auto result = std::make_unique<BoundConversionExpression>();
    result->span = syntaxTree.span();
    result->type = target;
    result->typeName = std::move(targetTypeName);
    result->conversion = conversion;
    result->checkedArithmetic = checkedArithmetic_;
    result->expression = std::move(expression);
    return result;
}


} // namespace realscript::semantic
