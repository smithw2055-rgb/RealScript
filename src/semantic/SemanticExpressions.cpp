#include "realscript/semantic/Semantic.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>
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
    if (left == PrimitiveType::Float || right == PrimitiveType::Float) return PrimitiveType::Float;
    if (left == PrimitiveType::ULong || right == PrimitiveType::ULong) {
        const auto other = left == PrimitiveType::ULong ? right : left;
        return isUnsignedIntegralType(other)
            ? PrimitiveType::ULong
            : PrimitiveType::Error;
    }
    if (left == PrimitiveType::Long || right == PrimitiveType::Long) return PrimitiveType::Long;
    if (left == PrimitiveType::UInt || right == PrimitiveType::UInt) {
        const auto other = left == PrimitiveType::UInt ? right : left;
        return other == PrimitiveType::Int ||
               other == PrimitiveType::SByte ||
               other == PrimitiveType::Short
            ? PrimitiveType::Long
            : PrimitiveType::UInt;
    }
    return PrimitiveType::Int;
}

bool isImplicitNumericConstant(
    const BoundExpression& expression,
    PrimitiveType target) noexcept {
    if (expression.kind() != BoundNodeKind::LiteralExpression) return false;
    const auto& literal = static_cast<const BoundLiteralExpression&>(expression);
    if (literal.type == PrimitiveType::Int &&
        std::holds_alternative<std::int64_t>(literal.value)) {
        const auto value = std::get<std::int64_t>(literal.value);
        switch (target) {
        case PrimitiveType::Byte: return value >= 0 && value <= 255;
        case PrimitiveType::SByte: return value >= -128 && value <= 127;
        case PrimitiveType::Short: return value >= -32768 && value <= 32767;
        case PrimitiveType::UShort:
        case PrimitiveType::Char: return value >= 0 && value <= 65535;
        case PrimitiveType::UInt:
        case PrimitiveType::ULong: return value >= 0;
        case PrimitiveType::Float: return true;
        default: return false;
        }
    }
    if (literal.type == PrimitiveType::Double &&
        target == PrimitiveType::Float &&
        std::holds_alternative<double>(literal.value)) {
        const auto value = std::get<double>(literal.value);
        return std::isfinite(value) &&
            std::abs(value) <= std::numeric_limits<float>::max();
    }
    return false;
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

std::vector<const syntax::ExpressionSyntax*> syntaxPointers(
    const std::vector<std::unique_ptr<syntax::ExpressionSyntax>>& values) {
    std::vector<const syntax::ExpressionSyntax*> result;
    result.reserve(values.size());
    for (const auto& value : values) result.push_back(value.get());
    return result;
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

struct FlexibleCallPlan {
    const FunctionSymbol* function = nullptr;
    std::vector<std::vector<std::size_t>> sources;
    bool expandedParams = false;
    int score = std::numeric_limits<int>::max();
};

FlexibleCallPlan selectFlexibleCall(
    const std::vector<const FunctionSymbol*>& candidates,
    const std::vector<std::unique_ptr<BoundExpression>>& arguments,
    const std::vector<std::optional<syntax::SyntaxToken>>* names,
    const std::vector<std::optional<syntax::SyntaxToken>>* modifiers,
    const TypeSymbolMap& visibleTypes,
    bool& ambiguous) {
    FlexibleCallPlan best;
    ambiguous = false;
    for (const auto* candidate : candidates) {
        const auto offset = visibleParameterOffset(*candidate);
        const auto visibleCount = candidate->parameters.size() - offset;
        FlexibleCallPlan plan;
        plan.function = candidate;
        plan.sources.resize(visibleCount);
        bool applicable = true;
        std::size_t nextPositional = 0;
        for (std::size_t source = 0; source < arguments.size(); ++source) {
            std::size_t parameterIndex = visibleCount;
            if (names && source < names->size() && (*names)[source]) {
                const auto& name = (*names)[source]->text;
                for (std::size_t index = 0; index < visibleCount; ++index) {
                    if (candidate->parameters[index + offset].name == name) {
                        parameterIndex = index;
                        break;
                    }
                }
            } else {
                while (nextPositional < visibleCount &&
                       !plan.sources[nextPositional].empty() &&
                       !candidate->parameters[nextPositional + offset].paramsArray) {
                    ++nextPositional;
                }
                parameterIndex = nextPositional;
                if (nextPositional < visibleCount &&
                    !candidate->parameters[nextPositional + offset].paramsArray) {
                    ++nextPositional;
                }
            }
            if (parameterIndex >= visibleCount) {
                applicable = false;
                break;
            }
            const auto& parameter =
                candidate->parameters[parameterIndex + offset];
            if (!plan.sources[parameterIndex].empty() &&
                !parameter.paramsArray) {
                applicable = false;
                break;
            }
            plan.sources[parameterIndex].push_back(source);
        }
        if (!applicable) continue;

        int score = 0;
        for (std::size_t index = 0; index < visibleCount; ++index) {
            const auto& parameter = candidate->parameters[index + offset];
            auto& sources = plan.sources[index];
            if (parameter.paramsArray) {
                if (sources.size() == 1 &&
                    arguments[sources.front()]->type == PrimitiveType::Array &&
                    (arguments[sources.front()]->typeName.empty() ||
                     parameter.typeName.empty() ||
                     arguments[sources.front()]->typeName == parameter.typeName)) {
                    const auto supplied = modifiers && sources.front() < modifiers->size()
                        ? syntaxModifier((*modifiers)[sources.front()])
                        : ParameterModifier::None;
                    if (supplied != ParameterModifier::None) applicable = false;
                    continue;
                }
                PrimitiveType elementType = PrimitiveType::Error;
                std::string elementTypeName;
                if (!decodeArrayTypeName(
                        parameter.typeName, elementType, elementTypeName)) {
                    applicable = false;
                    break;
                }
                plan.expandedParams = true;
                score += 2;
                for (const auto source : sources) {
                    const auto supplied = modifiers && source < modifiers->size()
                        ? syntaxModifier((*modifiers)[source])
                        : ParameterModifier::None;
                    const auto rank = conversionRank(
                        arguments[source]->type, elementType);
                    if (supplied != ParameterModifier::None || rank < 0 ||
                        (arguments[source]->type == elementType &&
                         isExactType(elementType) &&
                         !elementTypeName.empty() &&
                         !arguments[source]->typeName.empty() &&
                         arguments[source]->typeName != elementTypeName)) {
                        applicable = false;
                        break;
                    }
                    score += rank;
                }
                if (!applicable) break;
                continue;
            }
            if (sources.empty()) {
                if (!parameter.hasDefaultValue) {
                    applicable = false;
                    break;
                }
                ++score;
                continue;
            }
            if (sources.size() != 1) {
                applicable = false;
                break;
            }
            const auto source = sources.front();
            const auto supplied = modifiers && source < modifiers->size()
                ? syntaxModifier((*modifiers)[source])
                : ParameterModifier::None;
            if (supplied != parameter.modifier) {
                applicable = false;
                break;
            }
            if (parameter.modifier == ParameterModifier::None) {
                const auto rank = conversionRank(
                    arguments[source]->type, parameter.type);
                if (rank < 0 ||
                    (arguments[source]->type == parameter.type &&
                     !exactParameterMatch(
                         *arguments[source], parameter, visibleTypes))) {
                    applicable = false;
                    break;
                }
                score += rank;
            } else if (!exactParameterMatch(
                           *arguments[source], parameter, visibleTypes)) {
                applicable = false;
                break;
            }
        }
        if (!applicable) continue;
        plan.score = score;
        if (!best.function || score < best.score) {
            best = std::move(plan);
            ambiguous = false;
        } else if (score == best.score &&
                   best.function->id != candidate->id) {
            ambiguous = true;
        }
    }
    return best;
}

const FunctionSymbol* delegateInvokeMethod(
    const TypeSymbol& delegateType) noexcept {
    if (!delegateType.delegateType) return nullptr;
    for (const auto& method : delegateType.methods) {
        if (method.name == "Invoke" && method.method &&
            !method.staticMethod) {
            return &method;
        }
    }
    return nullptr;
}

bool delegateMethodMatches(
    const FunctionSymbol& candidate,
    const FunctionSymbol& invoke) {
    const auto candidateOffset = visibleParameterOffset(candidate);
    const auto invokeOffset = visibleParameterOffset(invoke);
    if (candidate.parameters.size() - candidateOffset !=
        invoke.parameters.size() - invokeOffset) {
        return false;
    }
    if (candidate.returnType != invoke.returnType ||
        (isExactType(candidate.returnType) &&
         candidate.returnTypeName != invoke.returnTypeName)) {
        return false;
    }
    for (std::size_t index = 0;
         index + invokeOffset < invoke.parameters.size(); ++index) {
        const auto& left = candidate.parameters[index + candidateOffset];
        const auto& right = invoke.parameters[index + invokeOffset];
        if (left.type != right.type || left.modifier != right.modifier ||
            (isExactType(left.type) && left.typeName != right.typeName)) {
            return false;
        }
    }
    return true;
}

void collectLambdaNames(
    const syntax::ExpressionSyntax& expression,
    std::vector<syntax::SyntaxToken>& names) {
    const auto visit = [&](const auto& values) {
        for (const auto& value : values) {
            collectLambdaNames(*value, names);
        }
    };
    switch (expression.kind()) {
    case syntax::SyntaxKind::NameExpression:
        names.push_back(static_cast<const
            syntax::NameExpressionSyntax&>(expression).identifierToken);
        return;
    case syntax::SyntaxKind::UnaryExpression:
        collectLambdaNames(*static_cast<const
            syntax::UnaryExpressionSyntax&>(expression).operand, names);
        return;
    case syntax::SyntaxKind::BinaryExpression: {
        const auto& value = static_cast<const
            syntax::BinaryExpressionSyntax&>(expression);
        collectLambdaNames(*value.left, names);
        collectLambdaNames(*value.right, names);
        return;
    }
    case syntax::SyntaxKind::AssignmentExpression: {
        const auto& value = static_cast<const
            syntax::AssignmentExpressionSyntax&>(expression);
        names.push_back(value.identifierToken);
        collectLambdaNames(*value.expression, names);
        return;
    }
    case syntax::SyntaxKind::ParenthesizedExpression:
        collectLambdaNames(*static_cast<const
            syntax::ParenthesizedExpressionSyntax&>(expression).expression,
            names);
        return;
    case syntax::SyntaxKind::CastExpression:
        collectLambdaNames(*static_cast<const
            syntax::CastExpressionSyntax&>(expression).expression, names);
        return;
    case syntax::SyntaxKind::CallExpression: {
        const auto& value = static_cast<const
            syntax::CallExpressionSyntax&>(expression);
        names.push_back(value.identifierToken);
        visit(value.arguments);
        return;
    }
    case syntax::SyntaxKind::MemberCallExpression: {
        const auto& value = static_cast<const
            syntax::MemberCallExpressionSyntax&>(expression);
        collectLambdaNames(*value.receiver, names);
        visit(value.arguments);
        return;
    }
    case syntax::SyntaxKind::MemberAccessExpression:
        collectLambdaNames(*static_cast<const
            syntax::MemberAccessExpressionSyntax&>(expression).receiver,
            names);
        return;
    case syntax::SyntaxKind::ElementAccessExpression: {
        const auto& value = static_cast<const
            syntax::ElementAccessExpressionSyntax&>(expression);
        collectLambdaNames(*value.receiver, names);
        collectLambdaNames(*value.index, names);
        return;
    }
    case syntax::SyntaxKind::ElementAssignmentExpression: {
        const auto& value = static_cast<const
            syntax::ElementAssignmentExpressionSyntax&>(expression);
        collectLambdaNames(*value.receiver, names);
        collectLambdaNames(*value.index, names);
        collectLambdaNames(*value.expression, names);
        return;
    }
    case syntax::SyntaxKind::MemberAssignmentExpression: {
        const auto& value = static_cast<const
            syntax::MemberAssignmentExpressionSyntax&>(expression);
        collectLambdaNames(*value.receiver, names);
        collectLambdaNames(*value.expression, names);
        return;
    }
    case syntax::SyntaxKind::NewObjectExpression:
        visit(static_cast<const
            syntax::NewObjectExpressionSyntax&>(expression).arguments);
        return;
    case syntax::SyntaxKind::NewArrayExpression:
        collectLambdaNames(*static_cast<const
            syntax::NewArrayExpressionSyntax&>(expression).length, names);
        return;
    case syntax::SyntaxKind::ThisExpression:
        names.push_back(static_cast<const
            syntax::ThisExpressionSyntax&>(expression).thisKeyword);
        return;
    case syntax::SyntaxKind::LambdaExpression:
    case syntax::SyntaxKind::LiteralExpression:
    case syntax::SyntaxKind::BaseExpression:
        return;
    default:
        return;
    }
}

} // namespace

const TypeSymbol* Binder::findVisibleType(SymbolId id) const noexcept {
    for (const auto& [name, type] : visibleTypes_) {
        (void)name;
        if (type.id == id) return &type;
    }
    return nullptr;
}

bool Binder::isTypeAccessible(const TypeSymbol& type) const noexcept {
    if (type.accessibility == Accessibility::Public) return true;
    return type.moduleName == currentModuleName_;
}

bool Binder::isMemberAccessible(
    Accessibility accessibility,
    SymbolId declaringTypeId,
    const std::string& declaringModule) const noexcept {
    if (accessibility == Accessibility::Public) return true;
    const auto* declaringType = declaringTypeId != 0
        ? findVisibleType(declaringTypeId)
        : nullptr;
    const auto& module = declaringType
        ? declaringType->moduleName
        : declaringModule;
    if (accessibility == Accessibility::Internal) {
        return module == currentModuleName_;
    }
    if (!currentOwnerType_ || declaringTypeId == 0) {
        return false;
    }
    if (currentOwnerType_->id == declaringTypeId) return true;
    if (accessibility == Accessibility::Private) return false;

    auto baseTypeId = currentOwnerType_->baseTypeId;
    while (baseTypeId != 0) {
        if (baseTypeId == declaringTypeId) return true;
        const auto* baseType = findVisibleType(baseTypeId);
        if (!baseType || baseType->baseTypeId == baseTypeId) break;
        baseTypeId = baseType->baseTypeId;
    }
    return false;
}

std::unique_ptr<BoundExpression> Binder::bindTargetExpression(
    const syntax::ExpressionSyntax& syntaxTree,
    PrimitiveType target,
    const std::string& targetTypeName,
    const std::string& context) {
    if (syntaxTree.kind() == syntax::SyntaxKind::NameExpression) {
        const auto& name = static_cast<const
            syntax::NameExpressionSyntax&>(syntaxTree);
        const auto* variable = lookupVariable(name.identifierToken.text);
        const auto* field = currentOwnerType_ && !currentStaticMethod_
            ? findField(*currentOwnerType_, name.identifierToken.text)
            : nullptr;
        if ((variable && variable->type == target &&
             variable->typeName == targetTypeName) ||
            (field && field->type == target &&
             field->typeName == targetTypeName)) {
            return convertExpression(
                bindExpression(syntaxTree), target, syntaxTree.span(),
                context, targetTypeName);
        }
    }
    if (target == PrimitiveType::Object && !targetTypeName.empty()) {
        const auto found = visibleTypes_.find(targetTypeName);
        if (found != visibleTypes_.end() && found->second.delegateType &&
            (syntaxTree.kind() == syntax::SyntaxKind::NameExpression ||
             syntaxTree.kind() == syntax::SyntaxKind::MemberAccessExpression ||
             syntaxTree.kind() == syntax::SyntaxKind::LambdaExpression ||
             syntaxTree.kind() == syntax::SyntaxKind::ParenthesizedExpression)) {
            return bindDelegateCreation(
                syntaxTree, found->second, context);
        }
    }
    return convertExpression(
        bindExpression(syntaxTree), target, syntaxTree.span(), context,
        targetTypeName);
}

std::unique_ptr<BoundExpression> Binder::bindDelegateCreation(
    const syntax::ExpressionSyntax& syntaxTree,
    const TypeSymbol& delegateType,
    const std::string& context) {
    if (syntaxTree.kind() == syntax::SyntaxKind::ParenthesizedExpression) {
        return bindDelegateCreation(
            *static_cast<const syntax::ParenthesizedExpressionSyntax&>(
                syntaxTree).expression,
            delegateType,
            context);
    }
    const auto* invoke = delegateInvokeMethod(delegateType);
    if (!invoke) {
        diagnostics_.report(
            "RS8801",
            "delegate type '" + canonicalTypeName(delegateType) +
                "' has no Invoke signature",
            syntaxTree.span());
        return makeError(syntaxTree.span());
    }

    if (syntaxTree.kind() == syntax::SyntaxKind::LambdaExpression) {
        const auto& lambda = static_cast<const
            syntax::LambdaExpressionSyntax&>(syntaxTree);
        const auto lambdaOrdinal = nextLambdaOrdinal_++;
        const auto invokeOffset = visibleParameterOffset(*invoke);
        if (lambda.parameterTokens.size() + invokeOffset !=
            invoke->parameters.size()) {
            diagnostics_.report(
                "RS8804",
                "lambda parameter count does not match delegate '" +
                    canonicalTypeName(delegateType) + "'",
                lambda.span());
            return makeError(lambda.span());
        }
        std::unordered_set<std::string> lambdaParameters;
        for (const auto& parameter : lambda.parameterTokens) {
            if (!lambdaParameters.insert(parameter.text).second) {
                diagnostics_.report(
                    "RS8804", "duplicate lambda parameter '" +
                        parameter.text + "'",
                    parameter.span);
            }
        }

        std::vector<syntax::SyntaxToken> referencedNames;
        collectLambdaNames(*lambda.body, referencedNames);
        std::vector<VariableSymbol> captured;
        std::unordered_set<SymbolId> capturedIds;
        for (const auto& name : referencedNames) {
            if (lambdaParameters.find(name.text) !=
                lambdaParameters.end()) {
                continue;
            }
            const auto* variable = lookupVariable(name.text);
            if (variable && capturedIds.insert(variable->id).second) {
                captured.push_back(*variable);
                continue;
            }
            if (currentOwnerType_ && !currentStaticMethod_ &&
                (findField(*currentOwnerType_, name.text) ||
                 findProperty(*currentOwnerType_, name.text))) {
                const auto* self = lookupVariable("this");
                if (self && capturedIds.insert(self->id).second) {
                    captured.push_back(*self);
                }
            }
        }
        std::sort(
            captured.begin(), captured.end(),
            [](const auto& left, const auto& right) {
                if (left.declarationSpan.start !=
                    right.declarationSpan.start) {
                    return left.declarationSpan.start <
                        right.declarationSpan.start;
                }
                return left.id < right.id;
            });

        TypeSymbol closure;
        closure.kind = TypeKind::Class;
        closure.accessibility = Accessibility::Private;
        closure.synthetic = true;
        closure.sealedType = true;
        closure.moduleName = currentModuleName_;
        closure.name = "$closure_" +
            std::to_string(currentFunctionId_) + "_" +
            std::to_string(lambdaOrdinal);
        closure.id = stableTypeId(closure);
        closure.sourceName = currentSourceName_;
        closure.declarationSpan = lambda.arrowToken.span;
        for (const auto& variable : captured) {
            FieldSymbol field;
            field.name = variable.name;
            field.accessibility = Accessibility::Private;
            field.declaringTypeId = closure.id;
            field.declaringTypeName = canonicalTypeName(closure);
            field.type = storageTypeOf(variable);
            field.typeName = storageTypeNameOf(variable);
            field.index = closure.fields.size();
            field.synthetic = true;
            field.sourceName = currentSourceName_;
            field.declarationSpan = variable.declarationSpan;
            field.id = stableTypeId(
                canonicalTypeName(closure) + "::capture:" +
                variable.name + ":" + std::to_string(variable.id));
            closure.fields.push_back(std::move(field));
        }

        FunctionSymbol lambdaFunction;
        lambdaFunction.accessibility = Accessibility::Private;
        lambdaFunction.declaringTypeId = closure.id;
        lambdaFunction.declaringTypeName = canonicalTypeName(closure);
        lambdaFunction.moduleName = currentModuleName_;
        lambdaFunction.name = "$lambda_" +
            std::to_string(lambdaOrdinal);
        lambdaFunction.ownerTypeName = closure.name;
        lambdaFunction.ownerTypeId = closure.id;
        lambdaFunction.returnType = invoke->returnType;
        lambdaFunction.returnTypeName = invoke->returnTypeName;
        lambdaFunction.method = true;
        lambdaFunction.synthetic = true;
        lambdaFunction.sourceName = currentSourceName_;
        lambdaFunction.declarationSpan = lambda.arrowToken.span;
        lambdaFunction.bodySpan = lambda.span();
        VariableSymbol self;
        self.name = "this";
        self.type = PrimitiveType::Object;
        self.typeName = canonicalTypeName(closure);
        self.parameter = true;
        self.index = 0;
        self.declarationSpan = lambda.arrowToken.span;
        lambdaFunction.parameters.push_back(std::move(self));
        for (std::size_t index = 0;
             index < lambda.parameterTokens.size(); ++index) {
            auto parameter = invoke->parameters[index + invokeOffset];
            parameter.name = lambda.parameterTokens[index].text;
            parameter.parameter = true;
            parameter.index = lambdaFunction.parameters.size();
            parameter.declarationSpan =
                lambda.parameterTokens[index].span;
            lambdaFunction.parameters.push_back(std::move(parameter));
        }
        lambdaFunction.id = stableFunctionId(lambdaFunction);
        for (auto& parameter : lambdaFunction.parameters) {
            parameter.id = stableTypeId(
                std::to_string(lambdaFunction.id) + "::local:" +
                std::to_string(parameter.index) + ":" +
                parameter.name);
        }
        closure.methods.push_back(lambdaFunction);

        FunctionBindingInput binding;
        binding.symbol = lambdaFunction;
        binding.sourceName = currentSourceName_;
        binding.eventLambda = &lambda;
        binding.parameterNames.push_back("this");
        binding.parameterSpans.push_back(lambda.arrowToken.span);
        for (const auto& parameter : lambda.parameterTokens) {
            binding.parameterNames.push_back(parameter.text);
            binding.parameterSpans.push_back(parameter.span);
        }
        pendingFunctionBindings_.push_back(std::move(binding));
        pendingTypes_.push_back(closure);
        visibleTypes_[closure.name] = closure;
        visibleTypes_[canonicalTypeName(closure)] = closure;

        auto result = std::make_unique<BoundDelegateCreationExpression>();
        result->span = lambda.span();
        result->type = PrimitiveType::Object;
        result->typeName = canonicalTypeName(delegateType);
        result->delegateType = delegateType;
        result->function = lambdaFunction;
        result->closureType = closure;
        result->captureFields = closure.fields;
        for (const auto& variable : captured) {
            result->captures.push_back(
                storageTypeOf(variable) != variable.type
                    ? storageVariableExpression(variable, lambda.span())
                    : variableExpression(variable, lambda.span()));
        }
        return result;
    }

    std::string methodName;
    std::vector<const FunctionSymbol*> candidates;
    std::unique_ptr<BoundExpression> receiver;
    bool staticMemberGroup = false;
    TypeSymbol methodOwner;
    if (syntaxTree.kind() == syntax::SyntaxKind::NameExpression) {
        const auto& name = static_cast<
            const syntax::NameExpressionSyntax&>(syntaxTree);
        methodName = name.identifierToken.text;
        const auto globals = visibleFunctions_.find(methodName);
        if (globals != visibleFunctions_.end()) {
            for (const auto& function : globals->second) {
                if (!function.method && isMemberAccessible(
                        function.accessibility,
                        function.declaringTypeId,
                        function.moduleName)) {
                    candidates.push_back(&function);
                }
            }
        }
        if (currentOwnerType_ && !currentStaticMethod_) {
            for (const auto* method : findMethods(
                     *currentOwnerType_, methodName, false)) {
                if (isMemberAccessible(
                        method->accessibility,
                        method->declaringTypeId,
                        method->moduleName)) {
                    candidates.push_back(method);
                }
            }
            if (!candidates.empty()) {
                const auto* self = lookupVariable("this");
                if (self) receiver = variableExpression(
                    *self, syntaxTree.span());
            }
        }
    } else if (syntaxTree.kind() ==
               syntax::SyntaxKind::MemberAccessExpression) {
        const auto& member = static_cast<
            const syntax::MemberAccessExpressionSyntax&>(syntaxTree);
        methodName = member.nameToken.text;
        if (member.receiver->kind() ==
            syntax::SyntaxKind::NameExpression) {
            const auto& name = static_cast<
                const syntax::NameExpressionSyntax&>(*member.receiver);
            const auto found = visibleTypes_.find(
                name.identifierToken.text);
            if (!lookupVariable(name.identifierToken.text) &&
                found != visibleTypes_.end()) {
                methodOwner = found->second;
                staticMemberGroup = true;
            }
        }
        if (!staticMemberGroup) {
            receiver = bindExpression(*member.receiver);
            if (receiver->type != PrimitiveType::Object ||
                receiver->typeName.empty()) {
                diagnostics_.report(
                    "RS8802",
                    "delegate instance method group requires an object receiver",
                    member.receiver->span());
                return makeError(syntaxTree.span());
            }
            const auto found = visibleTypes_.find(receiver->typeName);
            if (found == visibleTypes_.end()) {
                diagnostics_.report(
                    "RS8802", "delegate receiver type is unavailable",
                    member.receiver->span());
                return makeError(syntaxTree.span());
            }
            methodOwner = found->second;
        }
        for (const auto* method : findMethods(
                 methodOwner, methodName, staticMemberGroup)) {
            if (isMemberAccessible(
                    method->accessibility,
                    method->declaringTypeId,
                    method->moduleName)) {
                candidates.push_back(method);
            }
        }
    } else {
        diagnostics_.report(
            "RS8802", context + " requires a method group or lambda",
            syntaxTree.span());
        return makeError(syntaxTree.span());
    }

    const FunctionSymbol* selected = nullptr;
    for (const auto* candidate : candidates) {
        if (!delegateMethodMatches(*candidate, *invoke)) continue;
        if (selected && selected->id != candidate->id) {
            diagnostics_.report(
                "RS8803",
                "method group '" + methodName +
                    "' is ambiguous for delegate '" +
                    canonicalTypeName(delegateType) + "'",
                syntaxTree.span());
            return makeError(syntaxTree.span());
        }
        selected = candidate;
    }
    if (!selected) {
        diagnostics_.report(
            "RS8802",
            "method group '" + methodName +
                "' does not match delegate '" +
                canonicalTypeName(delegateType) + "'",
            syntaxTree.span());
        return makeError(syntaxTree.span());
    }
    if (selected->staticMethod || !selected->method) receiver.reset();
    if (selected->method && !selected->staticMethod && !receiver) {
        const auto* self = lookupVariable("this");
        if (!self) {
            diagnostics_.report(
                "RS8802", "instance method group has no receiver",
                syntaxTree.span());
            return makeError(syntaxTree.span());
        }
        receiver = variableExpression(*self, syntaxTree.span());
    }

    auto result = std::make_unique<BoundDelegateCreationExpression>();
    result->span = syntaxTree.span();
    result->type = PrimitiveType::Object;
    result->typeName = canonicalTypeName(delegateType);
    result->delegateType = delegateType;
    result->function = *selected;
    result->receiver = std::move(receiver);
    result->interfaceDispatch = result->receiver &&
        selected->interfaceMethod &&
        selected->interfaceSlot != std::numeric_limits<std::uint32_t>::max();
    result->interfaceTypeId = result->interfaceDispatch
        ? selected->ownerTypeId
        : 0;
    result->interfaceSlot = result->interfaceDispatch
        ? selected->interfaceSlot
        : std::numeric_limits<std::uint32_t>::max();
    result->virtualDispatch = result->receiver &&
        !result->interfaceDispatch &&
        selected->virtualSlot != std::numeric_limits<std::uint32_t>::max();
    result->virtualSlot = result->virtualDispatch
        ? selected->virtualSlot
        : std::numeric_limits<std::uint32_t>::max();
    return result;
}

std::unique_ptr<BoundStatement> Binder::bindBaseConstructorInitializer(
    const FunctionBindingInput& input) {
    if (!input.constructorSyntax || !currentOwnerType_ ||
        currentOwnerType_->kind != TypeKind::Class ||
        currentOwnerType_->baseTypeId == 0) {
        return nullptr;
    }

    const auto* baseType = findVisibleType(currentOwnerType_->baseTypeId);
    if (!baseType) {
        diagnostics_.report(
            "RS2531",
            "base constructor type is not visible for '" +
                canonicalTypeName(*currentOwnerType_) + "'",
            input.constructorSyntax->identifierToken.span);
        return nullptr;
    }

    std::vector<std::unique_ptr<BoundExpression>> arguments;
    arguments.reserve(input.constructorSyntax->baseArguments.size());
    for (const auto& argument : input.constructorSyntax->baseArguments) {
        arguments.push_back(bindExpression(*argument));
    }

    if (baseType->constructors.empty()) {
        if (!arguments.empty()) {
            diagnostics_.report(
                "RS2532",
                "base type '" + canonicalTypeName(*baseType) +
                    "' has no constructor accepting the supplied arguments",
                input.constructorSyntax->span());
        }
        return nullptr;
    }

    std::vector<const FunctionSymbol*> constructors;
    constructors.reserve(baseType->constructors.size());
    for (const auto& constructor : baseType->constructors) {
        if (isMemberAccessible(
                constructor.accessibility,
                constructor.declaringTypeId,
                constructor.moduleName)) {
            constructors.push_back(&constructor);
        }
    }
    bool ambiguous = false;
    const auto* selected = selectBest(
        constructors, arguments, nullptr, visibleTypes_, ambiguous);
    if (!selected) {
        diagnostics_.report(
            "RS2532",
            "no applicable base constructor for type '" +
                canonicalTypeName(*baseType) + "'",
            input.constructorSyntax->span());
        return nullptr;
    }
    if (ambiguous) {
        diagnostics_.report(
            "RS2533",
            "base constructor call is ambiguous for type '" +
                canonicalTypeName(*baseType) + "'",
            input.constructorSyntax->span());
        return nullptr;
    }

    const auto* thisVariable = lookupVariable("this");
    if (!thisVariable) return nullptr;
    std::vector<std::optional<syntax::SyntaxToken>> modifiers;
    auto call = bindSelectedCall(
        *selected,
        std::move(arguments),
        syntaxPointers(input.constructorSyntax->baseArguments),
        modifiers,
        {},
        variableExpression(
            *thisVariable, input.constructorSyntax->identifierToken.span),
        input.constructorSyntax->span(),
        "base constructor argument",
        true);
    auto statement = std::make_unique<BoundExpressionStatement>();
    statement->span = input.constructorSyntax->span();
    statement->expression = std::move(call);
    return statement;
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

std::unique_ptr<BoundExpression> Binder::bindNameExpression(
    const syntax::NameExpressionSyntax& syntaxTree) {
    if (const auto* variable = lookupVariable(
            syntaxTree.identifierToken.text)) {
        if (storageTypeOf(*variable) != variable->type) {
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

    if (const auto field = sequenceLocalFields_.find(
            syntaxTree.identifierToken.text);
        field != sequenceLocalFields_.end()) {
        return makeSequenceFieldAccess(field->second, syntaxTree.span());
    }

    if (currentOwnerType_ && !currentStaticMethod_) {
        const auto* thisVariable = lookupVariable("this");
        if (const auto* field = findField(*currentOwnerType_, syntaxTree.identifierToken.text)) {
            if (!isMemberAccessible(
                    field->accessibility, field->declaringTypeId)) {
                diagnostics_.report(
                    "RS2534", "field '" + field->name + "' is inaccessible",
                    syntaxTree.span());
                return makeError(syntaxTree.span());
            }
            const auto captureStorage = currentOwnerType_->synthetic &&
                    field->type == PrimitiveType::Object
                ? visibleTypes_.find(field->typeName)
                : visibleTypes_.end();
            if (captureStorage != visibleTypes_.end() &&
                captureStorage->second.synthetic &&
                captureStorage->second.fields.size() == 1 &&
                captureStorage->second.fields.front().name == "Value") {
                auto cell =
                    std::make_unique<BoundMemberAccessExpression>();
                cell->span = syntaxTree.span();
                cell->type = field->type;
                cell->typeName = field->typeName;
                cell->receiver = variableExpression(
                    *thisVariable, syntaxTree.span());
                cell->ownerType = *currentOwnerType_;
                cell->field = *field;
                const auto& valueField =
                    captureStorage->second.fields.front();
                auto result =
                    std::make_unique<BoundMemberAccessExpression>();
                result->span = syntaxTree.span();
                result->type = valueField.type;
                result->typeName = valueField.typeName;
                result->receiver = std::move(cell);
                result->ownerType = captureStorage->second;
                result->field = valueField;
                return result;
            }
            if (currentOwnerType_->kind == TypeKind::Struct) {
                auto result = std::make_unique<BoundStructFieldAccessExpression>();
                result->span = syntaxTree.span();
                result->type = field->type;
                result->typeName = field->typeName;
                result->receiver = makeVariableAccess(
                    *thisVariable, syntaxTree.span());
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
        if (currentOwnerType_->synthetic) {
            const auto* capturedThis = findField(*currentOwnerType_, "this");
            const auto outer = capturedThis
                ? visibleTypes_.find(capturedThis->typeName)
                : visibleTypes_.end();
            const auto* outerField = outer != visibleTypes_.end()
                ? findField(outer->second, syntaxTree.identifierToken.text)
                : nullptr;
            if (capturedThis && outerField) {
                auto capturedReceiver =
                    std::make_unique<BoundMemberAccessExpression>();
                capturedReceiver->span = syntaxTree.span();
                capturedReceiver->type = capturedThis->type;
                capturedReceiver->typeName = capturedThis->typeName;
                capturedReceiver->receiver = variableExpression(
                    *thisVariable, syntaxTree.span());
                capturedReceiver->ownerType = *currentOwnerType_;
                capturedReceiver->field = *capturedThis;
                auto result =
                    std::make_unique<BoundMemberAccessExpression>();
                result->span = syntaxTree.span();
                result->type = outerField->type;
                result->typeName = outerField->typeName;
                result->receiver = std::move(capturedReceiver);
                result->ownerType = outer->second;
                result->field = *outerField;
                return result;
            }
        }
        if (const auto* property = findProperty(*currentOwnerType_, syntaxTree.identifierToken.text)) {
            if (!isMemberAccessible(
                    property->accessibility,
                    property->declaringTypeId)) {
                diagnostics_.report(
                    "RS2534",
                    "property '" + property->name + "' is inaccessible",
                    syntaxTree.span());
                return makeError(syntaxTree.span());
            }
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
    if (left->type == PrimitiveType::Error || right->type == PrimitiveType::Error) {
        return makeError(syntaxTree.span());
    }

    if (syntaxTree.operatorToken.kind ==
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
                syntaxTree.left->span());
            return makeError(syntaxTree.span());
        }
        auto result = std::make_unique<BoundNullCoalescingExpression>();
        result->span = syntaxTree.span();
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
            std::move(right), result->type, syntaxTree.right->span(),
            "null-coalescing fallback", result->typeName);
        return result;
    }

    BoundBinaryOperatorKind operatorKind = BoundBinaryOperatorKind::Addition;
    PrimitiveType resultType = PrimitiveType::Error;
    const auto tokenKind = syntaxTree.operatorToken.kind;

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
            result->span = syntaxTree.span();
            result->type = PrimitiveType::Object;
            result->typeName = left->typeName;
            result->delegateType = found->second;
            result->remove = tokenKind == syntax::SyntaxKind::MinusToken;
            result->left = std::move(left);
            result->right = std::move(right);
            return result;
        }
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
        result->checkedArithmetic = checkedArithmetic_;
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

std::unique_ptr<BoundExpression> Binder::bindConditionalExpression(
    const syntax::ConditionalExpressionSyntax& syntaxTree) {
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

std::unique_ptr<BoundExpression> Binder::bindTypeBinaryExpression(
    const syntax::TypeBinaryExpressionSyntax& syntaxTree) {
    auto operand = bindExpression(*syntaxTree.expression);
    std::string targetTypeName;
    const auto targetType = bindType(
        syntaxTree.type, false, &targetTypeName);
    if (operand->type == PrimitiveType::Error ||
        targetType == PrimitiveType::Error) {
        return makeError(syntaxTree.span());
    }
    const bool safeCast = syntaxTree.operatorToken.kind ==
        syntax::SyntaxKind::AsKeyword;
    if (safeCast &&
        (!isReferenceType(operand->type) ||
         !isReferenceType(targetType))) {
        diagnostics_.report(
            "RS8910", "the 'as' operator requires reference types",
            syntaxTree.span());
        return makeError(syntaxTree.span());
    }
    auto result = std::make_unique<BoundTypeBinaryExpression>();
    result->span = syntaxTree.span();
    result->expression = std::move(operand);
    result->targetType = targetType;
    result->targetTypeName = targetTypeName;
    result->targetTypeId = isExactType(targetType)
        ? stableTypeId(targetTypeName)
        : 0;
    result->safeCast = safeCast;
    result->type = safeCast ? targetType : PrimitiveType::Bool;
    result->typeName = safeCast ? targetTypeName : std::string{};
    if (syntaxTree.designationToken) {
        if (safeCast || !isReferenceType(targetType)) {
            diagnostics_.report(
                "RS8920",
                "type pattern designation requires an 'is' reference-type pattern",
                syntaxTree.span());
        } else {
            VariableSymbol variable;
            variable.name = syntaxTree.designationToken->text;
            variable.type = targetType;
            variable.typeName = targetTypeName;
            variable.storageType = targetType;
            variable.storageTypeName = targetTypeName;
            variable.index = nextVariableIndex_++;
            if (declareVariable(variable, syntaxTree.designationToken->span)) {
                result->patternVariable = *lookupVariable(variable.name);
            }
        }
    }
    return result;
}

std::unique_ptr<BoundExpression> Binder::bindTypeOfExpression(
    const syntax::TypeOfExpressionSyntax& syntaxTree) {
    std::string typeName;
    const auto type = bindType(syntaxTree.type, false, &typeName);
    if (type == PrimitiveType::Error) return makeError(syntaxTree.span());
    auto result = std::make_unique<BoundTypeOfExpression>();
    result->span = syntaxTree.span();
    result->type = PrimitiveType::ULong;
    result->queriedType = type;
    result->queriedTypeName = typeName;
    result->queriedTypeId = isExactType(type)
        ? stableTypeId(typeName)
        : stableTypeId(
            "$primitive::" + std::string(primitiveTypeName(type)));
    return result;
}

std::unique_ptr<BoundExpression> Binder::bindSwitchExpression(
    const syntax::SwitchExpressionSyntax& syntaxTree) {
    auto result = std::make_unique<BoundSwitchExpression>();
    result->span = syntaxTree.span();
    result->expression = bindExpression(*syntaxTree.expression);
    PrimitiveType commonType = PrimitiveType::Error;
    std::string commonTypeName;
    bool hasDiscard = false;
    for (const auto& sourceArm : syntaxTree.arms) {
        BoundSwitchExpressionArm arm;
        arm.span = sourceArm.span();
        arm.discard = sourceArm.discardToken.has_value();
        if (arm.discard) {
            if (hasDiscard) {
                diagnostics_.report(
                    "RS8923", "switch expression has more than one discard arm",
                    sourceArm.span());
            }
            hasDiscard = true;
        }
        pushScope(sourceArm.span());
        if (sourceArm.label) {
            arm.label = convertExpression(
                bindExpression(*sourceArm.label), result->expression->type,
                sourceArm.label->span(), "switch expression pattern",
                result->expression->typeName);
        }
        if (sourceArm.patternType) {
            arm.patternType = bindType(
                *sourceArm.patternType, false, &arm.patternTypeName);
            arm.patternTypeId = isExactType(arm.patternType)
                ? stableTypeId(arm.patternTypeName)
                : 0;
            if (!isReferenceType(result->expression->type) ||
                !isReferenceType(arm.patternType)) {
                diagnostics_.report(
                    "RS8921", "switch type patterns require reference types",
                    sourceArm.span());
            }
            if (sourceArm.patternDesignation) {
                VariableSymbol variable;
                variable.name = sourceArm.patternDesignation->text;
                variable.type = arm.patternType;
                variable.typeName = arm.patternTypeName;
                variable.storageType = arm.patternType;
                variable.storageTypeName = arm.patternTypeName;
                variable.index = nextVariableIndex_++;
                if (declareVariable(
                        variable, sourceArm.patternDesignation->span)) {
                    arm.patternVariable = *lookupVariable(variable.name);
                }
            }
        }
        if (sourceArm.guard) {
            if (arm.discard) {
                diagnostics_.report(
                    "RS8925", "discard switch arm cannot have a guard",
                    sourceArm.guard->span());
            }
            arm.guard = convertExpression(
                bindExpression(*sourceArm.guard), PrimitiveType::Bool,
                sourceArm.guard->span(), "switch expression guard");
        }
        arm.value = bindExpression(*sourceArm.value);
        if (commonType == PrimitiveType::Error) {
            commonType = arm.value->type;
            commonTypeName = arm.value->typeName;
        } else if (commonType == arm.value->type &&
                   (!isExactType(commonType) ||
                    commonTypeName == arm.value->typeName)) {
            // exact match
        } else if (isNumericType(commonType) &&
                   isNumericType(arm.value->type)) {
            commonType = commonNumericType(commonType, arm.value->type);
            commonTypeName.clear();
        } else if (arm.value->type == PrimitiveType::Null &&
                   isReferenceType(commonType)) {
            // keep the current reference type
        } else if (commonType == PrimitiveType::Null &&
                   isReferenceType(arm.value->type)) {
            commonType = arm.value->type;
            commonTypeName = arm.value->typeName;
        } else if (commonType == PrimitiveType::Object &&
                   arm.value->type == PrimitiveType::Object &&
                   isAssignable(visibleTypes_, arm.value->typeName,
                                commonTypeName)) {
            // keep current base type
        } else if (commonType == PrimitiveType::Object &&
                   arm.value->type == PrimitiveType::Object &&
                   isAssignable(visibleTypes_, commonTypeName,
                                arm.value->typeName)) {
            commonTypeName = arm.value->typeName;
        } else {
            diagnostics_.report(
                "RS8924", "switch expression arms have no common type",
                sourceArm.value->span());
            commonType = PrimitiveType::Error;
        }
        popScope();
        result->arms.push_back(std::move(arm));
    }
    if (!hasDiscard) {
        diagnostics_.report(
            "RS8922", "switch expression requires an exhaustive discard arm",
            syntaxTree.span());
    }
    if (commonType == PrimitiveType::Error) return makeError(syntaxTree.span());
    result->type = commonType;
    result->typeName = commonTypeName;
    for (auto& arm : result->arms) {
        arm.value = convertExpression(
            std::move(arm.value), commonType, arm.span,
            "switch expression arm", commonTypeName);
    }
    return result;
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
        auto value = bindTargetExpression(
            *syntaxTree.expression, variable->type,
            variable->typeName, "assignment");
        if (storageTypeOf(*variable) != variable->type) {
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

    if (const auto field = sequenceLocalFields_.find(
            syntaxTree.identifierToken.text);
        field != sequenceLocalFields_.end()) {
        const auto* receiver = lookupVariable("this");
        if (!receiver || !currentOwnerType_) return makeError(syntaxTree.span());
        auto result = std::make_unique<BoundMemberAssignmentExpression>();
        result->span = syntaxTree.span();
        result->type = field->second.type;
        result->typeName = field->second.typeName;
        result->receiver = variableExpression(*receiver, syntaxTree.span());
        result->ownerType = *currentOwnerType_;
        result->field = field->second;
        result->expression = bindTargetExpression(
            *syntaxTree.expression,
            field->second.type,
            field->second.typeName,
            "sequence local assignment");
        return result;
    }

    if (currentOwnerType_ && !currentStaticMethod_) {
        const auto* thisVariable = lookupVariable("this");
        if (const auto* field = findField(
                *currentOwnerType_, syntaxTree.identifierToken.text)) {
            if (!isMemberAccessible(
                    field->accessibility, field->declaringTypeId)) {
                diagnostics_.report(
                    "RS2534", "field '" + field->name + "' is inaccessible",
                    syntaxTree.span());
                return makeError(syntaxTree.span());
            }
            const auto captureStorage = currentOwnerType_->synthetic &&
                    field->type == PrimitiveType::Object
                ? visibleTypes_.find(field->typeName)
                : visibleTypes_.end();
            if (captureStorage != visibleTypes_.end() &&
                captureStorage->second.synthetic &&
                captureStorage->second.fields.size() == 1 &&
                captureStorage->second.fields.front().name == "Value") {
                const auto& valueField =
                    captureStorage->second.fields.front();
                auto value = bindTargetExpression(
                    *syntaxTree.expression, valueField.type,
                    valueField.typeName, "captured variable assignment");
                auto cell =
                    std::make_unique<BoundMemberAccessExpression>();
                cell->span = syntaxTree.span();
                cell->type = field->type;
                cell->typeName = field->typeName;
                cell->receiver = variableExpression(
                    *thisVariable, syntaxTree.span());
                cell->ownerType = *currentOwnerType_;
                cell->field = *field;
                auto result =
                    std::make_unique<BoundMemberAssignmentExpression>();
                result->span = syntaxTree.span();
                result->type = valueField.type;
                result->typeName = valueField.typeName;
                result->receiver = std::move(cell);
                result->ownerType = captureStorage->second;
                result->field = valueField;
                result->expression = std::move(value);
                return result;
            }
            auto value = bindTargetExpression(
                *syntaxTree.expression, field->type,
                field->typeName, "field assignment");
            if (currentOwnerType_->kind == TypeKind::Struct) {
                auto result = std::make_unique<BoundStructFieldAssignmentExpression>();
                result->span = syntaxTree.span();
                result->type = field->type;
                result->typeName = field->typeName;
                result->variable = *thisVariable;
                result->ownerType = *currentOwnerType_;
                result->field = *field;
                result->expression = std::move(value);
                if (storageTypeOf(*thisVariable) != thisVariable->type) {
                    const auto wrapper = visibleTypes_.find(
                        storageTypeNameOf(*thisVariable));
                    if (wrapper == visibleTypes_.end() ||
                        wrapper->second.fields.empty()) {
                        diagnostics_.report(
                            "RS8705",
                            "mutable struct receiver storage descriptor is unavailable",
                            syntaxTree.span());
                        return makeError(syntaxTree.span());
                    }
                    result->wrappedVariable = true;
                    result->wrapperType = wrapper->second;
                    result->wrapperValueField =
                        wrapper->second.fields.front();
                }
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
        if (currentOwnerType_->synthetic) {
            const auto* capturedThis = findField(*currentOwnerType_, "this");
            const auto outer = capturedThis
                ? visibleTypes_.find(capturedThis->typeName)
                : visibleTypes_.end();
            const auto* outerField = outer != visibleTypes_.end()
                ? findField(outer->second, syntaxTree.identifierToken.text)
                : nullptr;
            if (capturedThis && outerField) {
                auto value = bindTargetExpression(
                    *syntaxTree.expression, outerField->type,
                    outerField->typeName, "captured field assignment");
                auto capturedReceiver =
                    std::make_unique<BoundMemberAccessExpression>();
                capturedReceiver->span = syntaxTree.span();
                capturedReceiver->type = capturedThis->type;
                capturedReceiver->typeName = capturedThis->typeName;
                capturedReceiver->receiver = variableExpression(
                    *thisVariable, syntaxTree.span());
                capturedReceiver->ownerType = *currentOwnerType_;
                capturedReceiver->field = *capturedThis;
                auto result =
                    std::make_unique<BoundMemberAssignmentExpression>();
                result->span = syntaxTree.span();
                result->type = outerField->type;
                result->typeName = outerField->typeName;
                result->receiver = std::move(capturedReceiver);
                result->ownerType = outer->second;
                result->field = *outerField;
                result->expression = std::move(value);
                return result;
            }
        }
        if (const auto* property = findProperty(
                *currentOwnerType_, syntaxTree.identifierToken.text)) {
            if (!isMemberAccessible(
                    property->accessibility,
                    property->declaringTypeId)) {
                diagnostics_.report(
                    "RS2534",
                    "property '" + property->name + "' is inaccessible",
                    syntaxTree.span());
                return makeError(syntaxTree.span());
            }
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
            auto assigned = bindTargetExpression(
                *syntaxTree.expression, property->type,
                property->typeName, "property assignment");
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

PreparedCallArguments Binder::prepareCallArguments(
    const FunctionSymbol& function,
    const std::vector<std::vector<std::size_t>>& sources,
    std::vector<std::unique_ptr<BoundExpression>> arguments,
    const std::vector<std::unique_ptr<syntax::ExpressionSyntax>>& syntaxArguments,
    const std::vector<std::optional<syntax::SyntaxToken>>& modifiers,
    text::TextSpan callSpan,
    const std::string& context) {
    PreparedCallArguments result;
    const auto offset = visibleParameterOffset(function);
    struct OrderedParameter {
        std::size_t source;
        std::size_t parameter;
    };
    std::vector<OrderedParameter> order;
    for (std::size_t index = 0; index < sources.size(); ++index) {
        const auto& parameter = function.parameters[index + offset];
        const auto& mapped = sources[index];
        std::unique_ptr<BoundExpression> value;
        const syntax::ExpressionSyntax* syntaxValue = nullptr;
        std::optional<syntax::SyntaxToken> modifier;
        if (parameter.paramsArray &&
            !(mapped.size() == 1 &&
              arguments[mapped.front()]->type == PrimitiveType::Array &&
              (arguments[mapped.front()]->typeName.empty() ||
               parameter.typeName.empty() ||
               arguments[mapped.front()]->typeName == parameter.typeName))) {
            PrimitiveType elementType = PrimitiveType::Error;
            std::string elementTypeName;
            (void)decodeArrayTypeName(
                parameter.typeName, elementType, elementTypeName);
            auto array = std::make_unique<BoundNewArrayExpression>();
            array->span = callSpan;
            array->type = PrimitiveType::Array;
            array->typeName = parameter.typeName;
            array->elementType = elementType;
            array->elementTypeName = elementTypeName;
            auto length = std::make_unique<BoundLiteralExpression>();
            length->span = callSpan;
            length->type = PrimitiveType::Int;
            length->value = static_cast<std::int64_t>(mapped.size());
            array->length = std::move(length);
            for (const auto source : mapped) {
                array->initialValues.push_back(convertExpression(
                    std::move(arguments[source]), elementType,
                    syntaxArguments[source]->span(), context,
                    elementTypeName));
            }
            value = std::move(array);
            order.push_back({
                mapped.empty() ? std::numeric_limits<std::size_t>::max()
                               : mapped.front(),
                index});
        } else if (!mapped.empty()) {
            const auto source = mapped.front();
            value = std::move(arguments[source]);
            syntaxValue = syntaxArguments[source].get();
            if (source < modifiers.size()) modifier = modifiers[source];
            order.push_back({source, index});
        } else {
            auto literal = std::make_unique<BoundLiteralExpression>();
            literal->span = callSpan;
            literal->type = parameter.defaultValueType;
            literal->value = parameter.defaultValue;
            value = std::move(literal);
            order.push_back({
                std::numeric_limits<std::size_t>::max() -
                    (sources.size() - index),
                index});
        }
        result.arguments.push_back(std::move(value));
        result.syntaxArguments.push_back(syntaxValue);
        result.modifiers.push_back(std::move(modifier));
    }
    std::stable_sort(order.begin(), order.end(), [](const auto& left,
                                                     const auto& right) {
        return left.source < right.source;
    });
    for (const auto& item : order) {
        result.evaluationOrder.push_back(item.parameter);
    }
    return result;
}

std::unique_ptr<BoundExpression> Binder::bindSelectedCall(
    const FunctionSymbol& function,
    std::vector<std::unique_ptr<BoundExpression>> arguments,
    const std::vector<const syntax::ExpressionSyntax*>& syntaxArguments,
    const std::vector<std::optional<syntax::SyntaxToken>>&
        argumentModifiers,
    std::vector<std::size_t> argumentEvaluationOrder,
    std::unique_ptr<BoundExpression> receiver,
    text::TextSpan span,
    const std::string& context,
    bool forceStaticDispatch) {
    const auto offset = visibleParameterOffset(function);
    const auto argumentSpan = [&](std::size_t index) {
        return index < syntaxArguments.size() && syntaxArguments[index]
            ? syntaxArguments[index]->span()
            : arguments[index]->span;
    };
    const bool interfaceDispatch = receiver &&
        function.method && !function.staticMethod &&
        !forceStaticDispatch && function.interfaceMethod &&
        function.interfaceSlot != std::numeric_limits<std::uint32_t>::max();
    const bool virtualDispatch = receiver &&
        function.method && !function.staticMethod &&
        !forceStaticDispatch && !interfaceDispatch &&
        function.virtualSlot != std::numeric_limits<std::uint32_t>::max();
    bool hasModifiers = function.method && !function.staticMethod &&
        !function.parameters.empty() &&
        function.parameters.front().modifier != ParameterModifier::None;
    for (std::size_t index = offset;
         index < function.parameters.size(); ++index) {
        hasModifiers = hasModifiers ||
            function.parameters[index].modifier !=
                ParameterModifier::None;
    }
    if (!hasModifiers) {
        auto result = std::make_unique<BoundCallExpression>();
        result->span = span;
        result->type = storageReturnTypeOf(function);
        result->typeName = storageReturnTypeNameOf(function);
        result->function = function;
        result->virtualDispatch = virtualDispatch;
        result->virtualSlot = virtualDispatch
            ? function.virtualSlot
            : std::numeric_limits<std::uint32_t>::max();
        result->interfaceDispatch = interfaceDispatch;
        result->interfaceTypeId = interfaceDispatch
            ? function.ownerTypeId
            : 0;
        result->interfaceSlot = interfaceDispatch
            ? function.interfaceSlot
            : std::numeric_limits<std::uint32_t>::max();
        const auto receiverOffset = receiver ? std::size_t{1} : std::size_t{0};
        if (receiver) result->arguments.push_back(std::move(receiver));
        for (std::size_t index = 0;
             index < arguments.size(); ++index) {
            const auto& parameter =
                function.parameters[index + offset];
            result->arguments.push_back(convertExpression(
                std::move(arguments[index]),
                parameter.type,
                argumentSpan(index),
                context,
                parameter.typeName));
        }
        if (receiverOffset) result->argumentEvaluationOrder.push_back(0);
        if (argumentEvaluationOrder.empty()) {
            for (std::size_t index = 0; index < arguments.size(); ++index) {
                result->argumentEvaluationOrder.push_back(
                    index + receiverOffset);
            }
        } else {
            for (const auto index : argumentEvaluationOrder) {
                result->argumentEvaluationOrder.push_back(
                    index + receiverOffset);
            }
        }
        return result;
    }

    auto result = std::make_unique<BoundReferenceCallExpression>();
    result->span = span;
    result->type = storageReturnTypeOf(function);
    result->typeName = storageReturnTypeNameOf(function);
    result->function = function;
    result->virtualDispatch = virtualDispatch;
    result->virtualSlot = virtualDispatch
        ? function.virtualSlot
        : std::numeric_limits<std::uint32_t>::max();
    result->interfaceDispatch = interfaceDispatch;
    result->interfaceTypeId = interfaceDispatch
        ? function.ownerTypeId
        : 0;
    result->interfaceSlot = interfaceDispatch
        ? function.interfaceSlot
        : std::numeric_limits<std::uint32_t>::max();
    const auto bindReferenceTarget = [&](BoundReferenceCallArgument& argument,
                                         std::unique_ptr<BoundExpression> value,
                                         text::TextSpan targetSpan) {
        if (value->kind() == BoundNodeKind::VariableExpression) {
            argument.targetKind = ReferenceTargetKind::Variable;
            argument.variable = static_cast<const
                BoundVariableExpression&>(*value).variable;
            argument.value = std::move(value);
            return true;
        }
        if (value->kind() == BoundNodeKind::MemberAccessExpression) {
            auto& member = static_cast<
                BoundMemberAccessExpression&>(*value);
            argument.targetKind = ReferenceTargetKind::ObjectField;
            argument.targetReceiver = std::move(member.receiver);
            argument.targetOwnerType = member.ownerType;
            argument.targetField = member.field;
            return true;
        }
        if (value->kind() == BoundNodeKind::ElementAccessExpression) {
            auto& element = static_cast<
                BoundElementAccessExpression&>(*value);
            argument.targetKind = ReferenceTargetKind::ArrayElement;
            argument.targetReceiver = std::move(element.receiver);
            argument.targetIndex = std::move(element.index);
            argument.targetElementType = element.elementType;
            argument.targetElementTypeName = element.elementTypeName;
            return true;
        }
        if (value->kind() == BoundNodeKind::StructFieldAccessExpression) {
            auto& member = static_cast<
                BoundStructFieldAccessExpression&>(*value);
            if (member.receiver->kind() !=
                BoundNodeKind::VariableExpression) {
                diagnostics_.report(
                    "RS8821",
                    "nested struct reference target requires a variable owner",
                    targetSpan);
                return false;
            }
            argument.targetKind = ReferenceTargetKind::StructField;
            argument.variable = static_cast<const
                BoundVariableExpression&>(*member.receiver).variable;
            argument.targetReceiver = std::move(member.receiver);
            argument.targetOwnerType = member.ownerType;
            argument.targetField = member.field;
            return true;
        }
        diagnostics_.report(
            "RS8703",
            "reference argument requires a variable, field, or indexer l-value",
            targetSpan);
        argument.value = std::move(value);
        return false;
    };
    if (receiver) {
        BoundReferenceCallArgument receiverArgument;
        const auto& self = function.parameters.front();
        receiverArgument.modifier = self.modifier;
        if (self.modifier == ParameterModifier::Ref) {
            const auto receiverSpan = receiver->span;
            if (bindReferenceTarget(
                    receiverArgument, std::move(receiver), receiverSpan)) {
                const auto& variable = receiverArgument.variable;
                const auto wrapper = visibleTypes_.find(
                    storageTypeNameOf(self));
                if (wrapper == visibleTypes_.end() ||
                    wrapper->second.fields.empty()) {
                    diagnostics_.report(
                        "RS8705",
                        "mutable struct receiver storage descriptor is unavailable",
                        receiverSpan);
                } else {
                    receiverArgument.wrapperType = wrapper->second;
                    receiverArgument.valueField =
                        wrapper->second.fields.front();
                    if (receiverArgument.targetKind ==
                            ReferenceTargetKind::Variable &&
                        (variable.modifier == ParameterModifier::Ref ||
                         variable.modifier == ParameterModifier::Out) &&
                        storageTypeNameOf(variable) ==
                            storageTypeNameOf(self)) {
                        receiverArgument.forwarded = true;
                        receiverArgument.value =
                            storageVariableExpression(variable, receiverSpan);
                    } else if (receiverArgument.targetKind ==
                            ReferenceTargetKind::Variable &&
                        variable.modifier == ParameterModifier::In) {
                        receiverArgument.defensiveCopy = true;
                    }
                }
            }
        } else {
            receiverArgument.value = std::move(receiver);
        }
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
                argumentSpan(index),
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
                argumentSpan(index));
            argument.value = std::move(arguments[index]);
            result->arguments.push_back(std::move(argument));
            continue;
        }
        if (parameter.modifier == ParameterModifier::In) {
            argument.value = convertExpression(
                std::move(arguments[index]),
                parameter.type,
                argumentSpan(index),
                context,
                parameter.typeName);
            result->arguments.push_back(std::move(argument));
            continue;
        }
        bool boundTarget = false;
        if (syntaxArguments[index] && syntaxArguments[index]->kind() ==
            syntax::SyntaxKind::NameExpression) {
            const auto& name = static_cast<const
                syntax::NameExpressionSyntax&>(*syntaxArguments[index]);
            if (const auto* namedVariable = lookupVariable(
                    name.identifierToken.text)) {
                argument.targetKind = ReferenceTargetKind::Variable;
                argument.variable = *namedVariable;
                argument.value = std::move(arguments[index]);
                boundTarget = true;
            }
        }
        if (!boundTarget && !bindReferenceTarget(
                argument, std::move(arguments[index]),
                argumentSpan(index))) {
            result->arguments.push_back(std::move(argument));
            continue;
        }
        const auto* variable = argument.targetKind ==
                ReferenceTargetKind::Variable
            ? &argument.variable
            : nullptr;
        if (variable && variable->modifier == ParameterModifier::In &&
            parameter.modifier != ParameterModifier::In) {
            diagnostics_.report(
                "RS8703",
                "an in parameter cannot be forwarded as ref or out",
                argumentSpan(index));
        }
        const auto wrapper = visibleTypes_.find(
            storageTypeNameOf(parameter));
        if (wrapper == visibleTypes_.end() ||
            wrapper->second.fields.empty()) {
            diagnostics_.report(
                "RS8705",
                "reference argument storage descriptor is unavailable",
                argumentSpan(index));
            argument.value = std::move(arguments[index]);
            result->arguments.push_back(std::move(argument));
            continue;
        }
        argument.wrapperType = wrapper->second;
        argument.valueField = wrapper->second.fields.front();
        if (variable &&
            (variable->modifier == ParameterModifier::Ref ||
             variable->modifier == ParameterModifier::Out)) {
            if (storageTypeNameOf(*variable) !=
                storageTypeNameOf(parameter)) {
                diagnostics_.report(
                    "RS8703",
                    "forwarded reference parameter has an "
                    "incompatible storage type",
                    argumentSpan(index));
            }
            argument.forwarded = true;
            argument.value = storageVariableExpression(
                *variable, argumentSpan(index));
        }
        result->arguments.push_back(std::move(argument));
    }
    return result;
}

std::unique_ptr<BoundExpression> Binder::bindCallExpression(
    const syntax::CallExpressionSyntax& syntaxTree) {
    if ((syntaxTree.identifierToken.text == "checked" ||
         syntaxTree.identifierToken.text == "unchecked") &&
        syntaxTree.arguments.size() == 1 &&
        syntaxTree.typeArguments.empty() &&
        (syntaxTree.argumentModifiers.empty() ||
         !syntaxTree.argumentModifiers.front())) {
        const auto previous = checkedArithmetic_;
        checkedArithmetic_ = syntaxTree.identifierToken.text == "checked";
        auto result = bindExpression(*syntaxTree.arguments.front());
        checkedArithmetic_ = previous;
        return result;
    }
    const TypeSymbol* calledDelegateType = nullptr;
    if (const auto* variable = lookupVariable(
            syntaxTree.identifierToken.text);
        variable && variable->type == PrimitiveType::Object &&
        !variable->typeName.empty()) {
        const auto found = visibleTypes_.find(variable->typeName);
        if (found != visibleTypes_.end() && found->second.delegateType) {
            calledDelegateType = &found->second;
        }
    }
    if (!calledDelegateType && currentOwnerType_ &&
        !currentStaticMethod_) {
        const auto* field = findField(
            *currentOwnerType_, syntaxTree.identifierToken.text);
        if (field && field->type == PrimitiveType::Object &&
            !field->typeName.empty()) {
            const auto found = visibleTypes_.find(field->typeName);
            if (found != visibleTypes_.end() && found->second.delegateType) {
                calledDelegateType = &found->second;
            }
        }
    }
    if (calledDelegateType) {
        const auto* invoke = delegateInvokeMethod(*calledDelegateType);
        if (!invoke) {
            diagnostics_.report(
                "RS8801", "delegate has no Invoke signature",
                syntaxTree.span());
            return makeError(syntaxTree.span());
        }
        const auto offset = visibleParameterOffset(*invoke);
        if (syntaxTree.arguments.size() + offset !=
            invoke->parameters.size()) {
            diagnostics_.report(
                "RS8805", "delegate invocation argument count mismatch",
                syntaxTree.span());
            return makeError(syntaxTree.span());
        }
        auto result =
            std::make_unique<BoundDelegateInvocationExpression>();
        result->span = syntaxTree.span();
        result->type = invoke->returnType;
        result->typeName = invoke->returnTypeName;
        result->delegateType = *calledDelegateType;
        syntax::NameExpressionSyntax delegateName;
        delegateName.identifierToken = syntaxTree.identifierToken;
        result->delegate = bindNameExpression(delegateName);
        for (std::size_t index = 0;
             index < syntaxTree.arguments.size(); ++index) {
            const auto& parameter = invoke->parameters[index + offset];
            BoundDelegateInvocationExpression::Argument argument;
            argument.modifier = parameter.modifier;
            const auto supplied = index <
                    syntaxTree.argumentModifiers.size()
                ? syntaxModifier(
                    syntaxTree.argumentModifiers[index])
                : ParameterModifier::None;
            if (supplied != parameter.modifier) {
                diagnostics_.report(
                    "RS8806",
                    "delegate argument modifier does not match parameter",
                    syntaxTree.arguments[index]->span());
            }
            if (parameter.modifier == ParameterModifier::None ||
                parameter.modifier == ParameterModifier::In) {
                argument.value = bindTargetExpression(
                    *syntaxTree.arguments[index],
                    parameter.type,
                    parameter.typeName,
                    "delegate argument");
                result->arguments.push_back(std::move(argument));
                continue;
            }
            if (syntaxTree.arguments[index]->kind() !=
                syntax::SyntaxKind::NameExpression) {
                diagnostics_.report(
                    "RS8703",
                    "ref and out delegate arguments must name a variable",
                    syntaxTree.arguments[index]->span());
                argument.value = bindExpression(
                    *syntaxTree.arguments[index]);
                result->arguments.push_back(std::move(argument));
                continue;
            }
            const auto& name = static_cast<const
                syntax::NameExpressionSyntax&>(
                    *syntaxTree.arguments[index]);
            const auto* variable = lookupVariable(
                name.identifierToken.text);
            if (!variable) {
                diagnostics_.report(
                    "RS8703",
                    "reference delegate argument must name a local or parameter",
                    syntaxTree.arguments[index]->span());
                result->arguments.push_back(std::move(argument));
                continue;
            }
            argument.variable = *variable;
            const auto wrapper = visibleTypes_.find(
                storageTypeNameOf(parameter));
            if (wrapper == visibleTypes_.end() ||
                wrapper->second.fields.empty()) {
                diagnostics_.report(
                    "RS8705",
                    "reference delegate argument storage descriptor is unavailable",
                    syntaxTree.arguments[index]->span());
                result->arguments.push_back(std::move(argument));
                continue;
            }
            argument.wrapperType = wrapper->second;
            argument.valueField = wrapper->second.fields.front();
            if (storageTypeOf(*variable) == PrimitiveType::Object &&
                storageTypeNameOf(*variable) ==
                    storageTypeNameOf(parameter)) {
                argument.forwarded = true;
                argument.value = storageVariableExpression(
                    *variable, syntaxTree.arguments[index]->span());
            } else if (parameter.modifier == ParameterModifier::Ref) {
                argument.value = bindTargetExpression(
                    *syntaxTree.arguments[index],
                    parameter.type,
                    parameter.typeName,
                    "delegate ref argument");
            }
            result->arguments.push_back(std::move(argument));
        }
        return result;
    }

    std::vector<const FunctionSymbol*> targetCandidates;
    const auto targetGlobals = visibleFunctions_.find(
        syntaxTree.identifierToken.text);
    if (targetGlobals != visibleFunctions_.end()) {
        for (const auto& function : targetGlobals->second) {
            if (!function.method && isMemberAccessible(
                    function.accessibility,
                    function.declaringTypeId,
                    function.moduleName)) {
                targetCandidates.push_back(&function);
            }
        }
    }
    if (currentOwnerType_ && !currentStaticMethod_) {
        for (const auto* method : findMethods(
                 *currentOwnerType_,
                 syntaxTree.identifierToken.text,
                 false)) {
            if (isMemberAccessible(
                    method->accessibility,
                    method->declaringTypeId,
                    method->moduleName)) {
                targetCandidates.push_back(method);
            }
        }
    }
    const FunctionSymbol* targetCandidate =
        targetCandidates.size() == 1
            ? targetCandidates.front()
            : nullptr;
    const auto targetOffset = targetCandidate
        ? visibleParameterOffset(*targetCandidate)
        : 0u;
    if (targetCandidate &&
        targetCandidate->parameters.size() !=
            syntaxTree.arguments.size() + targetOffset) {
        targetCandidate = nullptr;
    }
    std::vector<std::unique_ptr<BoundExpression>> arguments;
    for (std::size_t index = 0;
         index < syntaxTree.arguments.size(); ++index) {
        if (targetCandidate) {
            const auto& parameter =
                targetCandidate->parameters[index + targetOffset];
            const auto targetType =
                visibleTypes_.find(parameter.typeName);
            const bool delegateTarget =
                parameter.type == PrimitiveType::Object &&
                targetType != visibleTypes_.end() &&
                targetType->second.delegateType;
            arguments.push_back(delegateTarget
                ? bindTargetExpression(
                    *syntaxTree.arguments[index],
                    parameter.type,
                    parameter.typeName,
                    "argument")
                : bindExpression(*syntaxTree.arguments[index]));
        } else {
            arguments.push_back(bindExpression(
                *syntaxTree.arguments[index]));
        }
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
            const auto delegate = visibleTypes_.find(event.delegateName);
            if (delegate == visibleTypes_.end() ||
                !delegate->second.delegateType) {
                diagnostics_.report(
                    "RS8312",
                    "event delegate type was not resolved",
                    syntaxTree.span());
                return makeError(syntaxTree.span());
            }
            result->delegateType = delegate->second;
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
            if (!function.method && isMemberAccessible(
                    function.accessibility,
                    function.declaringTypeId,
                    function.moduleName)) {
                candidates.push_back(&function);
            }
        }
    }
    if (currentOwnerType_ && !currentStaticMethod_) {
        auto methods = findMethods(
            *currentOwnerType_,
            syntaxTree.identifierToken.text,
            false);
        for (const auto* method : methods) {
            if (isMemberAccessible(
                    method->accessibility,
                    method->declaringTypeId,
                    method->moduleName)) {
                candidates.push_back(method);
            }
        }
    }

    bool ambiguous = false;
    const auto plan = selectFlexibleCall(
        candidates, arguments, &syntaxTree.argumentNames,
        &syntaxTree.argumentModifiers, visibleTypes_, ambiguous);
    const auto* best = plan.function;
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
    auto prepared = prepareCallArguments(
        *best, plan.sources, std::move(arguments),
        syntaxTree.arguments, syntaxTree.argumentModifiers,
        syntaxTree.span(), "argument");
    return bindSelectedCall(
        *best,
        std::move(prepared.arguments),
        prepared.syntaxArguments,
        prepared.modifiers,
        std::move(prepared.evaluationOrder),
        std::move(receiver),
        syntaxTree.span(),
        "argument");
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
            if (!isTypeAccessible(type->second)) {
                diagnostics_.report(
                    "RS2534",
                    "type '" + canonicalTypeName(type->second) +
                        "' is inaccessible",
                    name.identifierToken.span);
                return makeError(syntaxTree.span());
            }
            if (!isTypeAccessible(type->second)) {
                diagnostics_.report(
                    "RS2534",
                    "type '" + canonicalTypeName(type->second) +
                        "' is inaccessible",
                    name.identifierToken.span);
                return makeError(syntaxTree.span());
            }
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

    if (!staticCall && owner.kind == TypeKind::Struct && owner.synthetic &&
        owner.name.rfind("Nullable__", 0) == 0 &&
        syntaxTree.arguments.empty() && owner.fields.size() >= 2) {
        const FieldSymbol* intrinsicField = nullptr;
        if (syntaxTree.nameToken.text == "HasValue") {
            intrinsicField = &owner.fields[0];
        } else if (syntaxTree.nameToken.text == "Value" ||
                   syntaxTree.nameToken.text == "GetValueOrDefault") {
            intrinsicField = &owner.fields[1];
        }
        if (intrinsicField) {
            auto result = std::make_unique<BoundStructFieldAccessExpression>();
            result->span = syntaxTree.span();
            result->type = intrinsicField->type;
            result->typeName = intrinsicField->typeName;
            result->receiver = std::move(receiver);
            result->ownerType = owner;
            result->field = *intrinsicField;
            return result;
        }
    }

    auto methods = findMethods(
        owner, syntaxTree.nameToken.text, staticCall);
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
    bool ambiguous = false;
    const auto plan = selectFlexibleCall(
        methods, arguments, &syntaxTree.argumentNames,
        &syntaxTree.argumentModifiers, visibleTypes_, ambiguous);
    const auto* best = plan.function;
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

    auto prepared = prepareCallArguments(
        *best, plan.sources, std::move(arguments),
        syntaxTree.arguments, syntaxTree.argumentModifiers,
        syntaxTree.span(), "method argument");
    return applyNullConditional(bindSelectedCall(
        *best,
        std::move(prepared.arguments),
        prepared.syntaxArguments,
        prepared.modifiers,
        std::move(prepared.evaluationOrder),
        std::move(receiver),
        syntaxTree.span(),
        "method argument",
        syntaxTree.receiver->kind() ==
            syntax::SyntaxKind::BaseExpression),
        syntaxTree.dotToken, syntaxTree.span());
}

std::unique_ptr<BoundExpression> Binder::bindNewObjectExpression(
    const syntax::NewObjectExpressionSyntax& syntaxTree) {
    const auto found = visibleTypes_.find(syntaxTree.type.name.text);
    if (found == visibleTypes_.end() || found->second.kind == TypeKind::Enum) {
        diagnostics_.report("RS2403", "cannot allocate unknown or enum type '" + syntaxTree.type.name.text + "'", syntaxTree.type.span());
        return makeError(syntaxTree.span());
    }
    if (!isTypeAccessible(found->second)) {
        diagnostics_.report(
            "RS2534",
            "type '" + canonicalTypeName(found->second) + "' is inaccessible",
            syntaxTree.type.span());
        return makeError(syntaxTree.span());
    }
    if (found->second.interfaceType) {
        diagnostics_.report(
            "RS2530",
            "cannot instantiate interface '" +
                canonicalTypeName(found->second) + "'",
            syntaxTree.type.span());
        return makeError(syntaxTree.span());
    }
    if (found->second.delegateType) {
        diagnostics_.report(
            "RS8807",
            "delegate values must be created from a method group or lambda",
            syntaxTree.type.span());
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
    for (const auto& ctor : found->second.constructors) {
        if (isMemberAccessible(
                ctor.accessibility, ctor.declaringTypeId, ctor.moduleName)) {
            constructors.push_back(&ctor);
        }
    }
    bool ambiguous = false;
    const FunctionSymbol* best = nullptr;
    FlexibleCallPlan constructorPlan;
    if (!constructors.empty() || !arguments.empty()) {
        constructorPlan = selectFlexibleCall(
            constructors, arguments, &syntaxTree.argumentNames,
            nullptr, visibleTypes_, ambiguous);
        best = constructorPlan.function;
    }
    const bool implicitStructDefault =
        found->second.kind == TypeKind::Struct && arguments.empty() && !best;
    if ((!arguments.empty() || !found->second.constructors.empty()) && !best &&
        !implicitStructDefault) {
        diagnostics_.report("RS2474", "no applicable constructor for type '" + canonicalTypeName(found->second) + "'", syntaxTree.span());
        return makeError(syntaxTree.span());
    }
    if (ambiguous) {
        diagnostics_.report("RS2475", "constructor call is ambiguous", syntaxTree.span());
        return makeError(syntaxTree.span());
    }
    PreparedCallArguments preparedConstructor;
    if (best) {
        const std::vector<std::optional<syntax::SyntaxToken>> noModifiers;
        preparedConstructor = prepareCallArguments(
            *best, constructorPlan.sources, std::move(arguments),
            syntaxTree.arguments, noModifiers, syntaxTree.span(),
            "constructor argument");
    }

    const auto bindInitializers = [&]() {
        std::vector<BoundNewObjectExpression::Initializer> initializers;
        std::unordered_set<std::string> initializedMembers;
        for (const auto& syntaxInitializer : syntaxTree.initializers) {
            BoundNewObjectExpression::Initializer initializer;
            if (syntaxInitializer.isMemberInitializer()) {
                const auto& name = syntaxInitializer.nameToken->text;
                if (!initializedMembers.insert(name).second) {
                    diagnostics_.report(
                        "RS8911", "member '" + name +
                            "' is initialized more than once",
                        syntaxInitializer.span());
                    continue;
                }
                if (syntaxInitializer.expressions.size() != 1) continue;
                if (const auto* field = findField(found->second, name)) {
                    if (!isMemberAccessible(
                            field->accessibility, field->declaringTypeId)) {
                        diagnostics_.report(
                            "RS2534", "field '" + name +
                                "' is inaccessible",
                            syntaxInitializer.span());
                        continue;
                    }
                    initializer.kind = BoundNewObjectExpression::Initializer::Kind::Field;
                    initializer.field = *field;
                    initializer.arguments.push_back(bindTargetExpression(
                        *syntaxInitializer.expressions.front(),
                        field->type, field->typeName,
                        "object initializer"));
                    initializers.push_back(std::move(initializer));
                    continue;
                }
                if (const auto* property = findProperty(found->second, name)) {
                    if (!property->setter || !isMemberAccessible(
                            property->accessibility,
                            property->declaringTypeId)) {
                        diagnostics_.report(
                            "RS8912", "property '" + name +
                                "' is not settable here",
                            syntaxInitializer.span());
                        continue;
                    }
                    initializer.kind = BoundNewObjectExpression::Initializer::Kind::Property;
                    initializer.function = *property->setter;
                    initializer.arguments.push_back(bindTargetExpression(
                        *syntaxInitializer.expressions.front(),
                        property->type, property->typeName,
                        "object initializer"));
                    initializers.push_back(std::move(initializer));
                    continue;
                }
                diagnostics_.report(
                    "RS8913", "type '" + canonicalTypeName(found->second) +
                        "' has no initializable member named '" + name + "'",
                    syntaxInitializer.span());
                continue;
            }

            std::vector<std::unique_ptr<BoundExpression>> values;
            for (const auto& expression : syntaxInitializer.expressions) {
                values.push_back(bindExpression(*expression));
            }
            const auto addMethods = findMethods(found->second, "Add", false);
            bool addAmbiguous = false;
            const auto* add = selectBest(
                addMethods, values, nullptr, visibleTypes_, addAmbiguous);
            if (!add || addAmbiguous) {
                diagnostics_.report(
                    addAmbiguous ? "RS8915" : "RS8914",
                    addAmbiguous
                        ? "collection initializer Add call is ambiguous"
                        : "no applicable Add method for collection initializer",
                    syntaxInitializer.span());
                continue;
            }
            initializer.kind = BoundNewObjectExpression::Initializer::Kind::Collection;
            initializer.function = *add;
            const auto addOffset = visibleParameterOffset(*add);
            for (std::size_t index = 0; index < values.size(); ++index) {
                initializer.arguments.push_back(convertExpression(
                    std::move(values[index]),
                    add->parameters[index + addOffset].type,
                    syntaxInitializer.expressions[index]->span(),
                    "collection initializer",
                    add->parameters[index + addOffset].typeName));
            }
            initializers.push_back(std::move(initializer));
        }
        return initializers;
    };
    auto boundInitializers = bindInitializers();

    if (found->second.kind == TypeKind::Struct) {
        auto result = std::make_unique<BoundNewStructExpression>();
        result->span = syntaxTree.span();
        result->type = PrimitiveType::Struct;
        result->typeName = canonicalTypeName(found->second);
        result->structType = found->second;
        if (best) result->constructor = *best;
        const auto offset = best ? visibleParameterOffset(*best) : 0;
        for (std::size_t i = 0;
             i < preparedConstructor.arguments.size(); ++i) {
            const auto argumentSpan = preparedConstructor.arguments[i]->span;
            result->arguments.push_back(convertExpression(
                std::move(preparedConstructor.arguments[i]),
                best->parameters[i + offset].type,
                argumentSpan, "constructor argument",
                best->parameters[i + offset].typeName));
        }
        result->argumentEvaluationOrder =
            std::move(preparedConstructor.evaluationOrder);
        result->initializers = std::move(boundInitializers);
        return result;
    }

    auto result = std::make_unique<BoundNewObjectExpression>();
    result->span = syntaxTree.span();
    result->type = PrimitiveType::Object;
    result->typeName = canonicalTypeName(found->second);
    result->objectType = found->second;
    if (best) result->constructor = *best;
    const auto offset = best ? visibleParameterOffset(*best) : 0;
    for (std::size_t i = 0;
         i < preparedConstructor.arguments.size(); ++i) {
        const auto argumentSpan = preparedConstructor.arguments[i]->span;
        result->arguments.push_back(convertExpression(
            std::move(preparedConstructor.arguments[i]),
            best->parameters[i + offset].type,
            argumentSpan, "constructor argument",
            best->parameters[i + offset].typeName));
    }
    result->argumentEvaluationOrder =
        std::move(preparedConstructor.evaluationOrder);
    result->initializers = std::move(boundInitializers);
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
                if (!isMemberAccessible(
                        property->accessibility,
                        property->declaringTypeId)) {
                    diagnostics_.report(
                        "RS2534",
                        "property '" + property->name + "' is inaccessible",
                        syntaxTree.span());
                    return makeError(syntaxTree.span());
                }
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
        return applyNullConditional(
            std::move(result), syntaxTree.dotToken, syntaxTree.span());
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
        if (!isMemberAccessible(
                field->accessibility, field->declaringTypeId)) {
            diagnostics_.report(
                "RS2534", "field '" + field->name + "' is inaccessible",
                syntaxTree.span());
            return makeError(syntaxTree.span());
        }
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
        return applyNullConditional(
            std::move(result), syntaxTree.dotToken, syntaxTree.span());
    }
    if (const auto* property = findProperty(type->second, syntaxTree.nameToken.text)) {
        if (!isMemberAccessible(
                property->accessibility,
                property->declaringTypeId)) {
            diagnostics_.report(
                "RS2534",
                "property '" + property->name + "' is inaccessible",
                syntaxTree.span());
            return makeError(syntaxTree.span());
        }
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
        return applyNullConditional(
            std::move(result), syntaxTree.dotToken, syntaxTree.span());
    }
    diagnostics_.report("RS2406", "type '" + receiver->typeName + "' has no member '" + syntaxTree.nameToken.text + "'", syntaxTree.nameToken.span);
    return makeError(syntaxTree.span());
}

std::unique_ptr<BoundExpression> Binder::applyNullConditional(
    std::unique_ptr<BoundExpression> expression,
    const syntax::SyntaxToken& accessToken,
    text::TextSpan span) {
    if (accessToken.kind != syntax::SyntaxKind::QuestionDotToken) {
        return expression;
    }
    if (!expression || expression->type == PrimitiveType::Void ||
        expression->type == PrimitiveType::Null ||
        expression->type == PrimitiveType::Error) {
        diagnostics_.report(
            "RS8906",
            "null-conditional access requires a value-producing member",
            span);
        return makeError(span);
    }
    const bool referenceResult =
        expression->type == PrimitiveType::String ||
        expression->type == PrimitiveType::Object ||
        expression->type == PrimitiveType::Array;
    PrimitiveType valueType = expression->type;
    std::string valueTypeName = expression->typeName;
    TypeSymbol nullableType;
    FieldSymbol hasValueField;
    FieldSymbol valueField;
    if (!referenceResult) {
        auto typeText = valueTypeName.empty()
            ? std::string(primitiveTypeName(valueType))
            : valueTypeName;
        for (auto& character : typeText) {
            const bool alphaNumeric =
                (character >= 'a' && character <= 'z') ||
                (character >= 'A' && character <= 'Z') ||
                (character >= '0' && character <= '9') ||
                character == '_';
            if (!alphaNumeric) character = '_';
        }
        const auto nullableName = "Nullable__" + typeText;
        auto found = visibleTypes_.find(nullableName);
        if (found == visibleTypes_.end()) {
            nullableType.moduleName = currentModuleName_;
            nullableType.name = nullableName;
            nullableType.kind = TypeKind::Struct;
            nullableType.synthetic = true;
            nullableType.sourceName = currentSourceName_;
            nullableType.declarationSpan = span;
            nullableType.id = stableTypeId(canonicalTypeName(nullableType));
            hasValueField.name = "hasValue";
            hasValueField.type = PrimitiveType::Bool;
            hasValueField.index = 0;
            hasValueField.synthetic = true;
            hasValueField.sourceName = currentSourceName_;
            hasValueField.declarationSpan = span;
            hasValueField.id = stableTypeId(
                canonicalTypeName(nullableType) + "::field:hasValue");
            valueField.name = "value";
            valueField.type = valueType;
            valueField.typeName = valueTypeName;
            valueField.index = 1;
            valueField.synthetic = true;
            valueField.sourceName = currentSourceName_;
            valueField.declarationSpan = span;
            valueField.id = stableTypeId(
                canonicalTypeName(nullableType) + "::field:value");
            nullableType.fields = {hasValueField, valueField};
            pendingTypes_.push_back(nullableType);
            visibleTypes_[nullableName] = nullableType;
            visibleTypes_[canonicalTypeName(nullableType)] = nullableType;
        } else {
            nullableType = found->second;
            if (nullableType.fields.size() < 2) {
                diagnostics_.report(
                    "RS8908", "nullable descriptor is incomplete", span);
                return makeError(span);
            }
            hasValueField = nullableType.fields[0];
            valueField = nullableType.fields[1];
        }
        expression->type = PrimitiveType::Struct;
        expression->typeName = canonicalTypeName(nullableType);
    }
    if (expression->kind() == BoundNodeKind::MemberAccessExpression) {
        auto& member = static_cast<BoundMemberAccessExpression&>(*expression);
        member.nullConditional = true;
        member.nullConditionalValueType = valueType;
        member.nullConditionalValueTypeName = valueTypeName;
        member.nullConditionalNullableType = nullableType;
        member.nullConditionalHasValueField = hasValueField;
        member.nullConditionalValueField = valueField;
        return expression;
    }
    if (expression->kind() == BoundNodeKind::CallExpression) {
        auto& call = static_cast<BoundCallExpression&>(*expression);
        if (call.arguments.empty() || !call.function.method ||
            call.function.staticMethod) {
            diagnostics_.report(
                "RS8907",
                "null-conditional access requires an instance receiver",
                span);
            return makeError(span);
        }
        call.nullConditional = true;
        call.nullConditionalValueType = valueType;
        call.nullConditionalValueTypeName = valueTypeName;
        call.nullConditionalNullableType = nullableType;
        call.nullConditionalHasValueField = hasValueField;
        call.nullConditionalValueField = valueField;
        return expression;
    }
    diagnostics_.report(
        "RS8907",
        "null-conditional access is not valid for this member",
        span);
    return makeError(span);
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
            if (!isTypeAccessible(type->second)) {
                diagnostics_.report(
                    "RS2534",
                    "type '" + canonicalTypeName(type->second) +
                        "' is inaccessible",
                    name.identifierToken.span);
                return makeError(syntaxTree.span());
            }
            if (const auto* property = findProperty(
                    type->second,
                    syntaxTree.nameToken.text)) {
                if (!isMemberAccessible(
                        property->accessibility,
                        property->declaringTypeId)) {
                    diagnostics_.report(
                        "RS2534",
                        "property '" + property->name + "' is inaccessible",
                        syntaxTree.span());
                    return makeError(syntaxTree.span());
                }
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
                auto assigned = bindTargetExpression(
                    *syntaxTree.expression,
                    property->type,
                    property->typeName,
                    "property assignment");
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

    std::unique_ptr<BoundExpression> receiver;
    if (syntaxTree.receiver->kind() == syntax::SyntaxKind::ThisExpression &&
        currentOwnerType_ && currentOwnerType_->kind == TypeKind::Struct) {
        const auto* self = lookupVariable("this");
        if (self) {
            receiver = variableExpression(
                *self, syntaxTree.receiver->span());
        } else {
            receiver = makeError(syntaxTree.receiver->span());
        }
    } else {
        receiver = bindExpression(*syntaxTree.receiver);
    }
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
        if (!isMemberAccessible(
                field->accessibility, field->declaringTypeId)) {
            diagnostics_.report(
                "RS2534", "field '" + field->name + "' is inaccessible",
                syntaxTree.span());
            return makeError(syntaxTree.span());
        }
        auto value = bindTargetExpression(
            *syntaxTree.expression, field->type, field->typeName,
            "field assignment");
        if (type->second.kind == TypeKind::Struct) {
            const VariableSymbol* variable = nullptr;
            if (receiver->kind() == BoundNodeKind::VariableExpression) {
                variable = &static_cast<const BoundVariableExpression&>(*receiver).variable;
            }
            if (!variable) {
                diagnostics_.report("RS2482", "struct field assignment requires a variable receiver", syntaxTree.receiver->span());
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
            if (storageTypeOf(*variable) != variable->type) {
                const auto wrapper = visibleTypes_.find(
                    storageTypeNameOf(*variable));
                if (wrapper == visibleTypes_.end() ||
                    wrapper->second.fields.empty()) {
                    diagnostics_.report(
                        "RS8705",
                        "mutable struct receiver storage descriptor is unavailable",
                        syntaxTree.receiver->span());
                    return makeError(syntaxTree.span());
                }
                result->wrappedVariable = true;
                result->wrapperType = wrapper->second;
                result->wrapperValueField =
                    wrapper->second.fields.front();
            }
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
        if (!isMemberAccessible(
                property->accessibility,
                property->declaringTypeId)) {
            diagnostics_.report(
                "RS2534",
                "property '" + property->name + "' is inaccessible",
                syntaxTree.span());
            return makeError(syntaxTree.span());
        }
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
            result->expression = bindTargetExpression(
                *syntaxTree.expression, property->type,
                property->typeName, "property assignment");
            return result;
        }
        auto assigned = bindTargetExpression(
            *syntaxTree.expression, property->type,
            property->typeName, "property assignment");
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

    const auto nullable = target == PrimitiveType::Struct
        ? visibleTypes_.find(targetTypeName)
        : visibleTypes_.end();
    if (nullable != visibleTypes_.end() &&
        nullable->second.name.rfind("Nullable__", 0) == 0 &&
        nullable->second.fields.size() >= 2) {
        auto result = std::make_unique<BoundNewStructExpression>();
        result->span = span;
        result->type = PrimitiveType::Struct;
        result->typeName = targetTypeName;
        result->structType = nullable->second;
        if (expression->type == PrimitiveType::Null) {
            return result;
        }
        const auto& valueField = nullable->second.fields[1];
        const FunctionSymbol* constructor = nullptr;
        for (const auto& candidate : nullable->second.constructors) {
            if (candidate.parameters.size() == 2) {
                constructor = &candidate;
                break;
            }
        }
        if (!constructor) {
            diagnostics_.report(
                "RS8826", "nullable value constructor is unavailable",
                span);
            return makeError(span);
        }
        result->constructor = *constructor;
        result->arguments.push_back(convertExpression(
            std::move(expression), valueField.type, span,
            "nullable value", valueField.typeName));
        return result;
    }

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
    auto conversion = classifyConversion(expression->type, target);
    if (conversion == ConversionKind::None &&
        isImplicitNumericConstant(*expression, target)) {
        conversion = ConversionKind::Numeric;
    }
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
    result->checkedArithmetic = checkedArithmetic_;
    result->expression = std::move(expression);
    return result;
}

} // namespace realscript::semantic
