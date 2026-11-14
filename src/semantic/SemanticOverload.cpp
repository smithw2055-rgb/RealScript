#include "SemanticInternal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <unordered_set>
#include <utility>

namespace realscript::semantic::detail {

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

std::string unaryOperatorMethodName(syntax::SyntaxKind kind) {
    switch (kind) {
    case syntax::SyntaxKind::PlusToken: return "op_UnaryPlus";
    case syntax::SyntaxKind::MinusToken: return "op_UnaryNegation";
    case syntax::SyntaxKind::BangToken: return "op_LogicalNot";
    default: return {};
    }
}

std::string binaryOperatorMethodName(syntax::SyntaxKind kind) {
    switch (kind) {
    case syntax::SyntaxKind::PlusToken: return "op_Addition";
    case syntax::SyntaxKind::MinusToken: return "op_Subtraction";
    case syntax::SyntaxKind::StarToken: return "op_Multiply";
    case syntax::SyntaxKind::SlashToken: return "op_Division";
    case syntax::SyntaxKind::PercentToken: return "op_Modulus";
    case syntax::SyntaxKind::EqualsEqualsToken: return "op_Equality";
    case syntax::SyntaxKind::BangEqualsToken: return "op_Inequality";
    case syntax::SyntaxKind::LessToken: return "op_LessThan";
    case syntax::SyntaxKind::LessOrEqualsToken: return "op_LessThanOrEqual";
    case syntax::SyntaxKind::GreaterToken: return "op_GreaterThan";
    case syntax::SyntaxKind::GreaterOrEqualsToken: return "op_GreaterThanOrEqual";
    default: return {};
    }
}

std::optional<syntax::SyntaxToken> compoundBinaryOperatorToken(
    const syntax::SyntaxToken& assignmentToken) {
    syntax::SyntaxToken token = assignmentToken;
    switch (assignmentToken.kind) {
    case syntax::SyntaxKind::PlusEqualsToken:
        token.kind = syntax::SyntaxKind::PlusToken;
        token.text = "+";
        return token;
    case syntax::SyntaxKind::MinusEqualsToken:
        token.kind = syntax::SyntaxKind::MinusToken;
        token.text = "-";
        return token;
    case syntax::SyntaxKind::StarEqualsToken:
        token.kind = syntax::SyntaxKind::StarToken;
        token.text = "*";
        return token;
    case syntax::SyntaxKind::SlashEqualsToken:
        token.kind = syntax::SyntaxKind::SlashToken;
        token.text = "/";
        return token;
    case syntax::SyntaxKind::PercentEqualsToken:
        token.kind = syntax::SyntaxKind::PercentToken;
        token.text = "%";
        return token;
    default:
        return std::nullopt;
    }
}

std::unique_ptr<BoundVariableExpression> variableExpression(
    const VariableSymbol& variable,
    text::TextSpan span) {
    auto result = std::make_unique<BoundVariableExpression>();
    result->span = span;
    result->type = variable.type;
    result->typeName = variable.typeName;
    result->variable = variable;
    return result;
}

std::unique_ptr<BoundVariableExpression> storageVariableExpression(
    const VariableSymbol& variable,
    text::TextSpan span) {
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

void recordSymbolReference(
    std::vector<SymbolOccurrence>& occurrences,
    const std::string& sourceName,
    SymbolId id,
    SymbolKind kind,
    const std::string& name,
    const std::string& detail,
    text::TextSpan span) {
    if (id == 0 || span.empty()) return;
    SymbolOccurrence occ;
    occ.id = id;
    occ.kind = kind;
    occ.name = name;
    occ.detail = detail;
    occ.sourceName = sourceName;
    occ.span = span;
    occ.definition = false;
    occurrences.push_back(std::move(occ));
}

void recordSymbolReference(
    std::vector<SymbolOccurrence>& occurrences,
    const std::string& sourceName,
    SymbolId id,
    SymbolKind kind,
    const std::string& name,
    text::TextSpan span) {
    recordSymbolReference(
        occurrences, sourceName, id, kind, name, std::string{}, span);
}

FunctionSymbol makeIntrinsicFunction(
    std::string intrinsicName,
    PrimitiveType returnType,
    const std::string& returnTypeName,
    const std::vector<std::pair<PrimitiveType, std::string>>& params) {
    constexpr std::string_view prefix = "$intrinsic::";
    if (intrinsicName.rfind(prefix, 0) == 0) {
        intrinsicName.erase(0, prefix.size());
    }
    FunctionSymbol fn;
    fn.moduleName = "$intrinsic";
    fn.name = intrinsicName;
    fn.returnType = returnType;
    fn.returnTypeName = returnTypeName;
    fn.storageReturnType = returnType;
    fn.storageReturnTypeName = returnTypeName;
    std::size_t idx = 0;
    for (const auto& [pType, pTypeName] : params) {
        VariableSymbol p;
        p.name = "arg" + std::to_string(idx);
        p.type = pType;
        p.typeName = pTypeName;
        p.storageType = pType;
        p.storageTypeName = pTypeName;
        p.index = idx++;
        p.parameter = true;
        fn.parameters.push_back(std::move(p));
    }
    fn.id = stableTypeId("$intrinsic::" + intrinsicName);
    return fn;
}

std::string formatLiteralTokenToString(const syntax::TokenValue& val) {
    if (const auto* s = std::get_if<std::string>(&val)) return *s;
    if (const auto* i = std::get_if<std::int64_t>(&val)) return std::to_string(*i);
    if (const auto* d = std::get_if<double>(&val)) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%g", *d);
        return std::string(buf);
    }
    if (const auto* b = std::get_if<bool>(&val)) return *b ? "true" : "false";
    return "null";
}

std::vector<const syntax::ExpressionSyntax*> syntaxPointers(
    const std::vector<std::unique_ptr<syntax::ExpressionSyntax>>& values) {
    std::vector<const syntax::ExpressionSyntax*> result;
    result.reserve(values.size());
    for (const auto& value : values) result.push_back(value.get());
    return result;
}

const FunctionSymbol* findUserDefinedConversion(
    PrimitiveType sourceType,
    const std::string& sourceTypeName,
    PrimitiveType targetType,
    const std::string& targetTypeName,
    bool allowExplicit,
    const TypeSymbolMap& visibleTypes,
    bool& ambiguous) {
    ambiguous = false;
    std::vector<const FunctionSymbol*> candidates;
    std::unordered_set<SymbolId> seenIds;
    const auto collectFromOwner = [&](const std::string& ownerTypeName) {
        if (ownerTypeName.empty()) return;
        const auto ownerIt = visibleTypes.find(ownerTypeName);
        if (ownerIt == visibleTypes.end()) return;
        for (const auto& method : ownerIt->second.methods) {
            if (!method.staticMethod || method.parameters.size() != 1) continue;
            if (method.name != "op_Implicit" &&
                (!allowExplicit || method.name != "op_Explicit")) {
                continue;
            }
            if (method.returnType != targetType) continue;
            if (isExactType(targetType) &&
                !targetTypeName.empty() &&
                !method.returnTypeName.empty() &&
                method.returnTypeName != targetTypeName) {
                continue;
            }
            if (seenIds.insert(method.id).second) {
                candidates.push_back(&method);
            }
        }
    };
    if ((sourceType == PrimitiveType::Struct ||
         sourceType == PrimitiveType::Object) &&
        !sourceTypeName.empty()) {
        collectFromOwner(sourceTypeName);
    }
    if ((targetType == PrimitiveType::Struct ||
         targetType == PrimitiveType::Object) &&
        !targetTypeName.empty()) {
        collectFromOwner(targetTypeName);
    }
    if (candidates.empty()) return nullptr;

    const FunctionSymbol* best = nullptr;
    int bestScore = std::numeric_limits<int>::max();
    for (const auto* candidate : candidates) {
        const auto& param = candidate->parameters[0];
        if (param.modifier != ParameterModifier::None &&
            param.modifier != ParameterModifier::In) {
            continue;
        }
        int rank = -1;
        if (sourceType == param.type &&
            (!isExactType(param.type) ||
             param.typeName.empty() ||
             sourceTypeName.empty() ||
             sourceTypeName == param.typeName ||
             (param.type == PrimitiveType::Object &&
              isAssignable(visibleTypes, sourceTypeName, param.typeName)))) {
            rank = (sourceTypeName == param.typeName) ? 0 : 1;
        } else if (!isExactType(sourceType) && !isExactType(param.type)) {
            rank = conversionRank(sourceType, param.type);
            if (rank >= 0) rank += 2;
        }
        if (rank < 0) continue;
        if (candidate->name == "op_Explicit") rank += 1;
        if (rank < bestScore) {
            best = candidate;
            bestScore = rank;
            ambiguous = false;
        } else if (rank == bestScore && best && best->id != candidate->id) {
            ambiguous = true;
        }
    }
    return best;
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
                auto rank = conversionRank(
                    arguments[i]->type, parameter.type);
                if (rank < 0 ||
                    (arguments[i]->type == parameter.type &&
                     !exactParameterMatch(*arguments[i], parameter, visibleTypes))) {
                    bool convAmbig = false;
                    const auto* userConv = findUserDefinedConversion(
                        arguments[i]->type,
                        arguments[i]->typeName,
                        parameter.type,
                        parameter.typeName,
                        false,
                        visibleTypes,
                        convAmbig);
                    if (userConv && !convAmbig) {
                        rank = 5;
                    } else {
                        applicable = false;
                        break;
                    }
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
            const bool implicitExtReceiver =
                candidate->extensionMethod && index == 0 && source == 0;
            const auto supplied = modifiers && source < modifiers->size()
                ? syntaxModifier((*modifiers)[source])
                : ParameterModifier::None;
            if (!implicitExtReceiver && supplied != parameter.modifier) {
                applicable = false;
                break;
            }
            if (implicitExtReceiver &&
                supplied != ParameterModifier::None &&
                supplied != parameter.modifier) {
                applicable = false;
                break;
            }
            if (parameter.modifier == ParameterModifier::None) {
                auto rank = conversionRank(
                    arguments[source]->type, parameter.type);
                if (rank < 0 ||
                    (arguments[source]->type == parameter.type &&
                     !exactParameterMatch(
                         *arguments[source], parameter, visibleTypes))) {
                    bool convAmbig = false;
                    const auto* userConv = findUserDefinedConversion(
                        arguments[source]->type,
                        arguments[source]->typeName,
                        parameter.type,
                        parameter.typeName,
                        false,
                        visibleTypes,
                        convAmbig);
                    if (userConv && !convAmbig) {
                        rank = 5;
                    } else {
                        applicable = false;
                        break;
                    }
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

namespace {

bool isShadowedLambdaParameter(
    const std::vector<std::unordered_set<std::string>>& scopes,
    const std::string& name) noexcept {
    for (const auto& scope : scopes) {
        if (scope.find(name) != scope.end()) return true;
    }
    return false;
}

void collectLambdaNamesImpl(
    const syntax::ExpressionSyntax& expression,
    std::vector<std::unordered_set<std::string>>& lambdaParamScopes,
    std::vector<syntax::SyntaxToken>& names) {
    const auto visit = [&](const auto& values) {
        for (const auto& value : values) {
            if (value) collectLambdaNamesImpl(*value, lambdaParamScopes, names);
        }
    };
    switch (expression.kind()) {
    case syntax::SyntaxKind::NameExpression: {
        const auto& tok = static_cast<const
            syntax::NameExpressionSyntax&>(expression).identifierToken;
        if (!isShadowedLambdaParameter(lambdaParamScopes, tok.text)) {
            names.push_back(tok);
        }
        return;
    }
    case syntax::SyntaxKind::UnaryExpression:
        collectLambdaNamesImpl(*static_cast<const
            syntax::UnaryExpressionSyntax&>(expression).operand,
            lambdaParamScopes, names);
        return;
    case syntax::SyntaxKind::BinaryExpression: {
        const auto& value = static_cast<const
            syntax::BinaryExpressionSyntax&>(expression);
        collectLambdaNamesImpl(*value.left, lambdaParamScopes, names);
        collectLambdaNamesImpl(*value.right, lambdaParamScopes, names);
        return;
    }
    case syntax::SyntaxKind::AssignmentExpression: {
        const auto& value = static_cast<const
            syntax::AssignmentExpressionSyntax&>(expression);
        if (!isShadowedLambdaParameter(
                lambdaParamScopes, value.identifierToken.text)) {
            names.push_back(value.identifierToken);
        }
        collectLambdaNamesImpl(*value.expression, lambdaParamScopes, names);
        return;
    }
    case syntax::SyntaxKind::ParenthesizedExpression:
        collectLambdaNamesImpl(*static_cast<const
            syntax::ParenthesizedExpressionSyntax&>(expression).expression,
            lambdaParamScopes, names);
        return;
    case syntax::SyntaxKind::CastExpression:
        collectLambdaNamesImpl(*static_cast<const
            syntax::CastExpressionSyntax&>(expression).expression,
            lambdaParamScopes, names);
        return;
    case syntax::SyntaxKind::CallExpression: {
        const auto& value = static_cast<const
            syntax::CallExpressionSyntax&>(expression);
        if (!isShadowedLambdaParameter(
                lambdaParamScopes, value.identifierToken.text)) {
            names.push_back(value.identifierToken);
        }
        visit(value.arguments);
        return;
    }
    case syntax::SyntaxKind::MemberCallExpression: {
        const auto& value = static_cast<const
            syntax::MemberCallExpressionSyntax&>(expression);
        collectLambdaNamesImpl(*value.receiver, lambdaParamScopes, names);
        visit(value.arguments);
        return;
    }
    case syntax::SyntaxKind::MemberAccessExpression:
        collectLambdaNamesImpl(*static_cast<const
            syntax::MemberAccessExpressionSyntax&>(expression).receiver,
            lambdaParamScopes, names);
        return;
    case syntax::SyntaxKind::ElementAccessExpression: {
        const auto& value = static_cast<const
            syntax::ElementAccessExpressionSyntax&>(expression);
        collectLambdaNamesImpl(*value.receiver, lambdaParamScopes, names);
        collectLambdaNamesImpl(*value.index, lambdaParamScopes, names);
        return;
    }
    case syntax::SyntaxKind::ElementAssignmentExpression: {
        const auto& value = static_cast<const
            syntax::ElementAssignmentExpressionSyntax&>(expression);
        collectLambdaNamesImpl(*value.receiver, lambdaParamScopes, names);
        collectLambdaNamesImpl(*value.index, lambdaParamScopes, names);
        collectLambdaNamesImpl(*value.expression, lambdaParamScopes, names);
        return;
    }
    case syntax::SyntaxKind::MemberAssignmentExpression: {
        const auto& value = static_cast<const
            syntax::MemberAssignmentExpressionSyntax&>(expression);
        collectLambdaNamesImpl(*value.receiver, lambdaParamScopes, names);
        collectLambdaNamesImpl(*value.expression, lambdaParamScopes, names);
        return;
    }
    case syntax::SyntaxKind::NewObjectExpression: {
        const auto& obj = static_cast<const
            syntax::NewObjectExpressionSyntax&>(expression);
        visit(obj.arguments);
        for (const auto& init : obj.initializers) {
            visit(init.expressions);
        }
        return;
    }
    case syntax::SyntaxKind::NewArrayExpression: {
        const auto& arr = static_cast<const
            syntax::NewArrayExpressionSyntax&>(expression);
        if (arr.length) {
            collectLambdaNamesImpl(*arr.length, lambdaParamScopes, names);
        }
        visit(arr.initialValues);
        return;
    }
    case syntax::SyntaxKind::TypeBinaryExpression:
        collectLambdaNamesImpl(*static_cast<const
            syntax::TypeBinaryExpressionSyntax&>(expression).expression,
            lambdaParamScopes, names);
        return;
    case syntax::SyntaxKind::ConditionalExpression: {
        const auto& cond = static_cast<const
            syntax::ConditionalExpressionSyntax&>(expression);
        collectLambdaNamesImpl(*cond.condition, lambdaParamScopes, names);
        collectLambdaNamesImpl(*cond.whenTrue, lambdaParamScopes, names);
        collectLambdaNamesImpl(*cond.whenFalse, lambdaParamScopes, names);
        return;
    }
    case syntax::SyntaxKind::SwitchExpression: {
        const auto& sw = static_cast<const
            syntax::SwitchExpressionSyntax&>(expression);
        collectLambdaNamesImpl(*sw.expression, lambdaParamScopes, names);
        for (const auto& arm : sw.arms) {
            if (arm.label) {
                collectLambdaNamesImpl(*arm.label, lambdaParamScopes, names);
            }
            if (arm.guard) {
                collectLambdaNamesImpl(*arm.guard, lambdaParamScopes, names);
            }
            if (arm.value) {
                collectLambdaNamesImpl(*arm.value, lambdaParamScopes, names);
            }
        }
        return;
    }
    case syntax::SyntaxKind::LambdaExpression: {
        const auto& nested = static_cast<const
            syntax::LambdaExpressionSyntax&>(expression);
        std::unordered_set<std::string> params;
        for (const auto& tok : nested.parameterTokens) {
            params.insert(tok.text);
        }
        lambdaParamScopes.push_back(std::move(params));
        if (nested.body) {
            collectLambdaNamesImpl(*nested.body, lambdaParamScopes, names);
        }
        lambdaParamScopes.pop_back();
        return;
    }
    case syntax::SyntaxKind::ThisExpression:
        names.push_back(static_cast<const
            syntax::ThisExpressionSyntax&>(expression).thisKeyword);
        return;
    case syntax::SyntaxKind::TaggedStringExpression:
    case syntax::SyntaxKind::TypeOfExpression:
    case syntax::SyntaxKind::LiteralExpression:
    case syntax::SyntaxKind::BaseExpression:
        return;
    default:
        return;
    }
}

} // namespace

void collectLambdaNames(
    const syntax::ExpressionSyntax& expression,
    std::vector<syntax::SyntaxToken>& names) {
    std::vector<std::unordered_set<std::string>> scopes;
    collectLambdaNamesImpl(expression, scopes, names);
}

bool isReadonlyFieldWritable(
    const FieldSymbol& field,
    bool currentConstructor,
    bool currentStaticMethod,
    const std::optional<TypeSymbol>& currentOwnerType) noexcept {
    if (!field.readonlyField) return true;
    if (!currentConstructor || !currentOwnerType ||
        currentOwnerType->id != field.declaringTypeId) {
        return false;
    }
    return field.staticField ? currentStaticMethod : !currentStaticMethod;
}

std::optional<FunctionSymbol> findStaticConstructorForFunction(
    const TypeSymbolMap& visibleTypes,
    const FunctionSymbol& function) {
    if (!function.method || !function.staticMethod || function.constructor) {
        return std::nullopt;
    }
    if (!function.declaringTypeName.empty()) {
        const auto found = visibleTypes.find(function.declaringTypeName);
        if (found != visibleTypes.end()) return found->second.staticConstructor;
    }
    if (!function.ownerTypeName.empty()) {
        const auto found = visibleTypes.find(function.ownerTypeName);
        if (found != visibleTypes.end()) return found->second.staticConstructor;
    }
    return std::nullopt;
}

} // namespace realscript::semantic::detail

namespace realscript::semantic {

using namespace detail;

std::unique_ptr<BoundExpression> Binder::bindTargetExpression(
    const syntax::ExpressionSyntax& syntaxTree,
    PrimitiveType target,
    const std::string& targetTypeName,
    const std::string& context) {
    if (syntaxTree.kind() == syntax::SyntaxKind::NameExpression) {
        const auto& name = static_cast<const
            syntax::NameExpressionSyntax&>(syntaxTree);
        const auto* variable = lookupVariable(name.identifierToken.text);
        const auto* field = currentOwnerType_
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
            if (parameter.defaultValueType == PrimitiveType::Enum &&
                parameter.type == PrimitiveType::Enum) {
                literal->typeName = parameter.typeName;
            }
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


std::unique_ptr<BoundExpression> Binder::convertExpression(
    std::unique_ptr<BoundExpression> expression,
    PrimitiveType target,
    text::TextSpan span,
    const std::string& context,
    std::string targetTypeName) {
    if (!expression) return makeError(span);
    if (expression->type == PrimitiveType::Error || target == PrimitiveType::Error) return expression;

    if (target == PrimitiveType::Object &&
        isStackOnlyType(expression->type, expression->typeName, visibleTypes_)) {
        diagnostics_.report(
            "RS2694",
            "stack-only struct '" +
                stackOnlyTypeDisplayName(expression->type, expression->typeName, visibleTypes_) +
                "' cannot be boxed to reference type '" +
                (targetTypeName.empty() ? std::string("object") : targetTypeName) + "'",
            span);
        return makeError(span);
    }

    const auto nullable = target == PrimitiveType::Struct
        ? visibleTypes_.find(targetTypeName)
        : visibleTypes_.end();
    if (nullable != visibleTypes_.end() &&
        nullable->second.name.rfind("Nullable__", 0) == 0 &&
        nullable->second.fields.size() >= 2 &&
        !(expression->type == PrimitiveType::Struct &&
          sameExactType(*expression, target, targetTypeName, visibleTypes_))) {
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
        bool convAmbiguous = false;
        const auto* userConv = findUserDefinedConversion(
            expression->type,
            expression->typeName,
            target,
            targetTypeName,
            false,
            visibleTypes_,
            convAmbiguous);
        if (convAmbiguous) {
            diagnostics_.report(
                "RS2473",
                "user-defined implicit conversion is ambiguous for " + context,
                span);
            return makeError(span);
        }
        if (userConv &&
            isMemberAccessible(
                userConv->accessibility,
                userConv->declaringTypeId,
                userConv->moduleName)) {
            auto call = std::make_unique<BoundCallExpression>();
            call->span = span;
            call->type = storageReturnTypeOf(*userConv);
            call->typeName = storageReturnTypeNameOf(*userConv);
            call->function = *userConv;
            call->arguments.push_back(std::move(expression));
            call->argumentEvaluationOrder = {0};
            return call;
        }
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
        bool convAmbiguous = false;
        const auto* userConv = findUserDefinedConversion(
            expression->type,
            expression->typeName,
            target,
            targetTypeName,
            false,
            visibleTypes_,
            convAmbiguous);
        if (convAmbiguous) {
            diagnostics_.report(
                "RS2473",
                "user-defined implicit conversion is ambiguous for " + context,
                span);
            return makeError(span);
        }
        if (userConv &&
            isMemberAccessible(
                userConv->accessibility,
                userConv->declaringTypeId,
                userConv->moduleName)) {
            auto call = std::make_unique<BoundCallExpression>();
            call->span = span;
            call->type = storageReturnTypeOf(*userConv);
            call->typeName = storageReturnTypeNameOf(*userConv);
            call->function = *userConv;
            if (expression->type == userConv->parameters[0].type) {
                call->arguments.push_back(std::move(expression));
            } else {
                call->arguments.push_back(convertExpression(
                    std::move(expression),
                    userConv->parameters[0].type,
                    span,
                    "implicit conversion operand",
                    userConv->parameters[0].typeName));
            }
            call->argumentEvaluationOrder = {0};
            return call;
        }
        diagnostics_.report(
            "RS2106",
            "cannot convert '" + std::string(primitiveTypeName(expression->type)) +
                "' to '" + primitiveTypeName(target) + "' for " + context,
            span);
        return makeError(span);
    }
    if (conversion == ConversionKind::Identity) {
        if (isExactType(target) && !targetTypeName.empty()) expression->typeName = std::move(targetTypeName);
        if (context == "in argument" ||
            context == "using null check" ||
            context == "equality operand" ||
            context == "comparison operand") {
            return expression;
        }
        const bool isReturnCtx =
            context == "return value" ||
            context == "lambda return value" ||
            context == "sequence result";
        return cloneResourceHandleIfNeeded(std::move(expression), span, isReturnCtx);
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
