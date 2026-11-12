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
    case syntax::SyntaxKind::NewArrayExpression: {
        const auto& arr = static_cast<const
            syntax::NewArrayExpressionSyntax&>(expression);
        if (arr.length) collectLambdaNames(*arr.length, names);
        visit(arr.initialValues);
        return;
    }
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
    if (type.accessibility == Accessibility::Internal &&
        hygienicAccessibleModules_.find(type.moduleName) != hygienicAccessibleModules_.end()) {
        return true;
    }
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
        return module == currentModuleName_ ||
            hygienicAccessibleModules_.find(module) != hygienicAccessibleModules_.end();
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

bool Binder::isManagedResourceHandleType(
    PrimitiveType type,
    const std::string& typeName) const noexcept {
    if (type != PrimitiveType::Struct || typeName.empty()) return false;
    const auto sep = typeName.rfind("::");
    const std::string_view shortName = sep == std::string::npos
        ? std::string_view(typeName)
        : std::string_view(typeName).substr(sep + 2);
    if (shortName != "ResourceHandle" &&
        shortName.rfind("ResourceHandle__", 0) != 0) {
        return false;
    }
    const auto found = visibleTypes_.find(typeName);
    if (found == visibleTypes_.end() || found->second.kind != TypeKind::Struct) {
        return false;
    }
    for (const auto& m : found->second.methods) {
        if (!m.staticMethod && m.name == "Retain" &&
            m.parameters.size() == 1 && m.returnType == PrimitiveType::Struct) {
            return true;
        }
    }
    return false;
}

bool Binder::isInsideResourceHandleImplementation() const noexcept {
    if (!currentOwnerType_) return false;
    const auto ownerCanon = canonicalTypeName(*currentOwnerType_);
    if (isManagedResourceHandleType(PrimitiveType::Struct, ownerCanon)) {
        return true;
    }
    if (currentModuleName_ == "RealScript.Resource" &&
        (currentOwnerType_->name == "Resource" ||
         currentOwnerType_->name == "ResourceServer")) {
        return true;
    }
    return false;
}

bool Binder::shouldCloneResourceHandleValue(
    const BoundExpression& expr,
    bool isReturnContext) const noexcept {
    if (isInsideResourceHandleImplementation()) return false;
    if (!isManagedResourceHandleType(expr.type, expr.typeName)) return false;
    switch (expr.kind()) {
    case BoundNodeKind::VariableExpression: {
        if (!isReturnContext) return true;
        const auto& v = static_cast<const BoundVariableExpression&>(expr).variable;
        return v.isUsingResource || v.parameter || storageTypeOf(v) != v.type || storageTypeNameOf(v) != v.typeName;
    }
    case BoundNodeKind::MemberAccessExpression:
    case BoundNodeKind::StructFieldAccessExpression:
    case BoundNodeKind::ElementAccessExpression:
    case BoundNodeKind::AssignmentExpression:
    case BoundNodeKind::MemberAssignmentExpression:
    case BoundNodeKind::StructFieldAssignmentExpression:
    case BoundNodeKind::PropertyAssignmentExpression:
    case BoundNodeKind::ElementAssignmentExpression:
        return true;
    default:
        return false;
    }
}

std::unique_ptr<BoundExpression> Binder::cloneResourceHandleIfNeeded(
    std::unique_ptr<BoundExpression> expr,
    text::TextSpan span,
    bool isReturnContext) {
    if (!expr || !shouldCloneResourceHandleValue(*expr, isReturnContext)) {
        return expr;
    }
    const auto ownerIt = visibleTypes_.find(expr->typeName);
    if (ownerIt == visibleTypes_.end()) return expr;
    const FunctionSymbol* retainMethod = nullptr;
    for (const auto& m : ownerIt->second.methods) {
        if (!m.staticMethod && m.name == "Retain" &&
            m.parameters.size() == 1 && m.returnType == PrimitiveType::Struct) {
            retainMethod = &m;
            break;
        }
    }
    if (!retainMethod) return expr;
    std::vector<const syntax::ExpressionSyntax*> emptySyntaxArgs;
    std::vector<std::optional<syntax::SyntaxToken>> emptyMods;
    return bindSelectedCall(
        *retainMethod,
        {},
        emptySyntaxArgs,
        emptyMods,
        {},
        std::move(expr),
        span,
        "resource handle clone",
        true);
}

std::unique_ptr<BoundExpression> Binder::wrapResourceHandleOverwriteRelease(
    std::unique_ptr<BoundExpression> oldValueExpr,
    std::unique_ptr<BoundExpression> newValueExpr,
    text::TextSpan span) {
    if (!oldValueExpr || !newValueExpr ||
        isInsideResourceHandleImplementation() ||
        !isManagedResourceHandleType(newValueExpr->type, newValueExpr->typeName)) {
        return newValueExpr;
    }
    const auto ownerIt = visibleTypes_.find(newValueExpr->typeName);
    if (ownerIt == visibleTypes_.end()) return newValueExpr;
    const FunctionSymbol* assignHelper = nullptr;
    for (const auto& m : ownerIt->second.methods) {
        if (m.staticMethod && m.name == "__AssignHandle" &&
            m.parameters.size() == 2 && m.returnType == PrimitiveType::Struct) {
            assignHelper = &m;
            break;
        }
    }
    if (!assignHelper) return newValueExpr;
    auto markSyntheticRead = [](BoundExpression* expr) {
        while (expr) {
            if (expr->kind() == BoundNodeKind::VariableExpression) {
                static_cast<BoundVariableExpression*>(expr)->syntheticOverwriteRead = true;
                return;
            }
            if (expr->kind() == BoundNodeKind::MemberAccessExpression) {
                expr = static_cast<BoundMemberAccessExpression*>(expr)->receiver.get();
                continue;
            }
            break;
        }
    };
    markSyntheticRead(oldValueExpr.get());
    auto call = std::make_unique<BoundCallExpression>();
    call->span = span;
    call->type = newValueExpr->type;
    call->typeName = newValueExpr->typeName;
    call->function = *assignHelper;
    call->arguments.push_back(std::move(oldValueExpr));
    call->arguments.push_back(std::move(newValueExpr));
    call->argumentEvaluationOrder = {1, 0};
    return call;
}

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
        binding.hygienicAccessibleModules = hygienicAccessibleModules_;
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
                (storageTypeOf(variable) != variable.type || storageTypeNameOf(variable) != variable.typeName)
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
    checkObsoleteReference(
        selected->id, "function", selected->name, syntaxTree.span());
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

namespace {

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

} // namespace

std::unique_ptr<BoundStatement> Binder::bindBaseConstructorInitializer(
    const FunctionBindingInput& input) {
    if ((!input.constructorSyntax && !input.symbol.constructor) ||
        input.symbol.staticMethod || !currentOwnerType_ ||
        currentOwnerType_->kind != TypeKind::Class ||
        currentOwnerType_->baseTypeId == 0) {
        return nullptr;
    }

    const auto diagSpan = input.constructorSyntax
        ? input.constructorSyntax->identifierToken.span
        : input.symbol.declarationSpan;
    const auto callSpan = input.constructorSyntax
        ? input.constructorSyntax->span()
        : input.symbol.declarationSpan;

    const auto* baseType = findVisibleType(currentOwnerType_->baseTypeId);
    if (!baseType) {
        diagnostics_.report(
            "RS2531",
            "base constructor type is not visible for '" +
                canonicalTypeName(*currentOwnerType_) + "'",
            diagSpan);
        return nullptr;
    }

    std::vector<std::unique_ptr<BoundExpression>> arguments;
    if (input.constructorSyntax) {
        arguments.reserve(input.constructorSyntax->baseArguments.size());
        for (const auto& argument : input.constructorSyntax->baseArguments) {
            arguments.push_back(bindExpression(*argument));
        }
    }

    if (baseType->constructors.empty()) {
        if (!arguments.empty()) {
            diagnostics_.report(
                "RS2532",
                "base type '" + canonicalTypeName(*baseType) +
                    "' has no constructor accepting the supplied arguments",
                callSpan);
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
            callSpan);
        return nullptr;
    }
    if (ambiguous) {
        diagnostics_.report(
            "RS2533",
            "base constructor call is ambiguous for type '" +
                canonicalTypeName(*baseType) + "'",
            callSpan);
        return nullptr;
    }

    const auto* thisVariable = lookupVariable("this");
    if (!thisVariable) return nullptr;
    std::vector<std::optional<syntax::SyntaxToken>> modifiers;
    const std::vector<const syntax::ExpressionSyntax*> emptySyntaxArgs;
    auto call = bindSelectedCall(
        *selected,
        std::move(arguments),
        input.constructorSyntax
            ? syntaxPointers(input.constructorSyntax->baseArguments)
            : emptySyntaxArgs,
        modifiers,
        {},
        variableExpression(*thisVariable, diagSpan),
        callSpan,
        "base constructor argument",
        true);
    auto statement = std::make_unique<BoundExpressionStatement>();
    statement->span = callSpan;
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

std::unique_ptr<BoundExpression> Binder::bindNameExpression(
    const syntax::NameExpressionSyntax& syntaxTree) {
    if (const auto* variable = lookupVariable(
            syntaxTree.identifierToken.text)) {
        if (variable->name != "this") {
            recordSymbolReference(
                occurrences_,
                currentSourceName_,
                variable->id,
                variable->parameter ? SymbolKind::Parameter : SymbolKind::Local,
                variable->name,
                isExactType(variable->type) && !variable->typeName.empty()
                    ? variable->typeName
                    : primitiveTypeName(variable->type),
                syntaxTree.identifierToken.span);
        }
        if (storageTypeOf(*variable) != variable->type || storageTypeNameOf(*variable) != variable->typeName) {
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

    if (currentOwnerType_) {
        const auto* thisVariable = lookupVariable("this");
        if (const auto* field = findField(*currentOwnerType_, syntaxTree.identifierToken.text)) {
            if (!isMemberAccessible(
                    field->accessibility, field->declaringTypeId)) {
                diagnostics_.report(
                    "RS2534", "field '" + field->name + "' is inaccessible",
                    syntaxTree.span());
                return makeError(syntaxTree.span());
            }
            checkObsoleteReference(
                field->id, "field", field->name, syntaxTree.identifierToken.span);
            recordSymbolReference(
                occurrences_,
                currentSourceName_,
                field->id,
                SymbolKind::Field,
                field->name,
                isExactType(field->type) && !field->typeName.empty()
                    ? field->typeName
                    : primitiveTypeName(field->type),
                syntaxTree.identifierToken.span);
            if (field->constField && field->hasConstantValue) {
                auto lit = std::make_unique<BoundLiteralExpression>();
                lit->span = syntaxTree.span();
                lit->type = field->type;
                lit->typeName = field->typeName;
                lit->value = field->constantValue;
                return lit;
            }
            if (field->staticField) {
                auto result = std::make_unique<BoundMemberAccessExpression>();
                result->span = syntaxTree.span();
                result->type = field->type;
                result->typeName = field->typeName;
                result->ownerType = *currentOwnerType_;
                result->field = *field;
                if (!currentOwnerType_->staticConstructor.has_value() &&
                    !field->hasConstantValue && field->initializerSyntax) {
                    result->staticInitializer = bindStaticFieldInitializer(
                        *currentOwnerType_, *field);
                }
                return result;
            }
            if (currentStaticMethod_ || !thisVariable) {
                diagnostics_.report(
                    "RS2484",
                    "instance field '" + field->name + "' requires a value receiver",
                    syntaxTree.identifierToken.span);
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
        if (!currentStaticMethod_ && thisVariable && currentOwnerType_->synthetic) {
            const auto* capturedThis = findField(*currentOwnerType_, "this");
            const auto outer = capturedThis
                ? visibleTypes_.find(capturedThis->typeName)
                : visibleTypes_.end();
            const auto* outerField = outer != visibleTypes_.end()
                ? findField(outer->second, syntaxTree.identifierToken.text)
                : nullptr;
            if (capturedThis && outerField) {
                recordSymbolReference(
                    occurrences_,
                    currentSourceName_,
                    outerField->id,
                    SymbolKind::Field,
                    outerField->name,
                    isExactType(outerField->type) && !outerField->typeName.empty()
                        ? outerField->typeName
                        : primitiveTypeName(outerField->type),
                    syntaxTree.identifierToken.span);
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
            checkObsoleteReference(
                property->id, "property", property->name, syntaxTree.identifierToken.span);
            recordSymbolReference(
                occurrences_,
                currentSourceName_,
                property->id,
                SymbolKind::Property,
                property->name,
                isExactType(property->type) && !property->typeName.empty()
                    ? property->typeName
                    : primitiveTypeName(property->type),
                syntaxTree.identifierToken.span);
            if (property->staticProperty) {
                if (property->getter) {
                    auto result = std::make_unique<BoundCallExpression>();
                    result->span = syntaxTree.span();
                    result->type = property->type;
                    result->typeName = property->typeName;
                    result->function = *property->getter;
                    result->staticConstructor = findStaticConstructorForFunction(
                        visibleTypes_, *property->getter);
                    if (!result->staticConstructor) {
                        result->staticConstructor = currentOwnerType_->staticConstructor;
                    }
                    return result;
                }
            } else if (!currentStaticMethod_ && thisVariable && property->getter) {
                auto result = std::make_unique<BoundCallExpression>();
                result->span = syntaxTree.span();
                result->type = property->type;
                result->typeName = property->typeName;
                result->function = *property->getter;
                result->arguments.push_back(makeVariableAccess(*thisVariable, syntaxTree.span()));
                return result;
            } else if (!property->staticProperty && (currentStaticMethod_ || !thisVariable)) {
                diagnostics_.report(
                    "RS2484",
                    "instance property '" + property->name + "' requires a value receiver",
                    syntaxTree.identifierToken.span);
                return makeError(syntaxTree.span());
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

std::unique_ptr<BoundPattern> Binder::bindPattern(
    const syntax::PatternSyntax& syntaxTree,
    PrimitiveType inputType,
    const std::string& inputTypeName,
    bool allowVariables,
    bool isSwitchPattern) {
    auto pat = std::make_unique<BoundPattern>();
    pat->span = syntaxTree.span();
    pat->inputType = inputType;
    pat->inputTypeName = inputTypeName;

    if (inputType == PrimitiveType::Error) {
        pat->kind = BoundPatternKind::Discard;
        return pat;
    }

    // Check if inputType is a Nullable<T> struct wrapper
    bool inputIsNullableStruct = false;
    TypeSymbol nullableDesc;
    FieldSymbol nullableHasVal;
    FieldSymbol nullableVal;
    if (inputType == PrimitiveType::Struct && !inputTypeName.empty()) {
        const auto found = visibleTypes_.find(inputTypeName);
        if (found != visibleTypes_.end() &&
            found->second.name.rfind("Nullable__", 0) == 0 &&
            found->second.fields.size() >= 2) {
            inputIsNullableStruct = true;
            nullableDesc = found->second;
            nullableHasVal = found->second.fields[0];
            nullableVal = found->second.fields[1];
        }
    }

    const auto declarePatternDesignation = [&](
        const syntax::SyntaxToken& tok,
        PrimitiveType varType,
        const std::string& varTypeName) -> std::optional<VariableSymbol> {
        if (tok.text.empty() || tok.text == "_") return std::nullopt;
        if (!allowVariables) {
            diagnostics_.report(
                "RS8935",
                "pattern variable '" + tok.text +
                    "' cannot be introduced inside a negated or disjunctive ('or') pattern",
                tok.span);
            return std::nullopt;
        }
        VariableSymbol variable;
        variable.name = tok.text;
        variable.type = varType;
        variable.typeName = varTypeName;
        variable.storageType = varType;
        variable.storageTypeName = varTypeName;
        variable.index = nextVariableIndex_++;
        if (declareVariable(variable, tok.span)) {
            if (const auto* declared = lookupVariable(variable.name)) {
                return *declared;
            }
        }
        return std::nullopt;
    };

    const auto bindConstantComparison = [&](const syntax::ExpressionSyntax& constSyntax) {
        pat->kind = BoundPatternKind::Constant;
        pat->comparisonOperator = BoundBinaryOperatorKind::Equals;
        syntax::TokenValue evalVal;
        PrimitiveType evalType = PrimitiveType::Error;
        const TypeSymbol* ownerPtr = currentOwnerType_ ? &(*currentOwnerType_) : nullptr;
        const bool isConst = evaluateConstantSyntaxExpression(
            constSyntax, ownerPtr, visibleTypes_, evalVal, evalType, &scopes_);
        auto boundConst = bindExpression(constSyntax);
        if (boundConst->type == PrimitiveType::Error) return;
        if (!isConst && boundConst->type != PrimitiveType::Null) {
            diagnostics_.report(
                "RS8936",
                "pattern constant requires a compile-time constant value",
                constSyntax.span());
            return;
        }
        if (boundConst->type == PrimitiveType::Null) {
            if (inputIsNullableStruct) {
                pat->unwrapsNullableStruct = true;
                pat->nullableStructType = nullableDesc;
                pat->nullableHasValueField = nullableHasVal;
                pat->nullableValueField = nullableVal;
                pat->constantNullCheck = true;
                return;
            }
            if (!isReferenceType(inputType)) {
                diagnostics_.report(
                    "RS8921",
                    "cannot match 'null' pattern against non-nullable value type '" +
                        (inputTypeName.empty()
                            ? std::string(primitiveTypeName(inputType))
                            : inputTypeName) + "'",
                    constSyntax.span());
                return;
            }
            pat->constantNullCheck = true;
            pat->constantValue = convertExpression(
                std::move(boundConst),
                inputType,
                constSyntax.span(),
                "null pattern",
                inputTypeName);
            pat->comparisonType = inputType;
            return;
        }

        PrimitiveType effectiveInput = inputType;
        std::string effectiveInputName = inputTypeName;
        if (inputIsNullableStruct) {
            pat->unwrapsNullableStruct = true;
            pat->nullableStructType = nullableDesc;
            pat->nullableHasValueField = nullableHasVal;
            pat->nullableValueField = nullableVal;
            pat->unwrappedTempVariable = allocateTempVariable(
                "pat_unwrap", nullableVal.type, nullableVal.typeName, syntaxTree.span());
            effectiveInput = nullableVal.type;
            effectiveInputName = nullableVal.typeName;
        }

        // Convert constant to effectiveInput (or common comparison type)
        if (effectiveInput == PrimitiveType::Object && effectiveInputName.empty()) {
            diagnostics_.report(
                "RS8921",
                "constant pattern cannot be applied directly to untyped object",
                constSyntax.span());
            return;
        }
        if (effectiveInput == PrimitiveType::Enum) {
            pat->comparisonType = PrimitiveType::Enum;
            pat->constantValue = convertExpression(
                std::move(boundConst),
                effectiveInput,
                constSyntax.span(),
                "constant pattern",
                effectiveInputName);
            return;
        }
        if (isNumericType(effectiveInput) && isNumericType(boundConst->type)) {
            if ((effectiveInput == PrimitiveType::ULong || effectiveInput == PrimitiveType::UInt) &&
                std::holds_alternative<std::int64_t>(evalVal) &&
                std::get<std::int64_t>(evalVal) >= 0) {
                auto lit = std::make_unique<BoundLiteralExpression>();
                lit->span = constSyntax.span();
                lit->type = effectiveInput;
                lit->value = evalVal;
                pat->comparisonType = effectiveInput;
                pat->constantValue = std::move(lit);
                return;
            }
            pat->comparisonType = effectiveInput;
            pat->constantValue = convertExpression(
                std::move(boundConst),
                effectiveInput,
                constSyntax.span(),
                "constant pattern",
                effectiveInputName);
            return;
        }
        pat->comparisonType = effectiveInput;
        pat->constantValue = convertExpression(
            std::move(boundConst),
            effectiveInput,
            constSyntax.span(),
            "constant pattern",
            effectiveInputName);
    };

    switch (syntaxTree.kind()) {
    case syntax::SyntaxKind::ParenthesizedPattern: {
        const auto& paren = static_cast<const syntax::ParenthesizedPatternSyntax&>(syntaxTree);
        if (!paren.pattern) return pat;
        return bindPattern(*paren.pattern, inputType, inputTypeName, allowVariables, isSwitchPattern);
    }
    case syntax::SyntaxKind::DiscardPattern: {
        pat->kind = BoundPatternKind::Discard;
        return pat;
    }
    case syntax::SyntaxKind::VarPattern: {
        const auto& vp = static_cast<const syntax::VarPatternSyntax&>(syntaxTree);
        pat->kind = BoundPatternKind::Var;
        pat->variable = declarePatternDesignation(
            vp.designationToken, inputType, inputTypeName);
        return pat;
    }
    case syntax::SyntaxKind::ConstantPattern: {
        const auto& cp = static_cast<const syntax::ConstantPatternSyntax&>(syntaxTree);
        if (cp.expression) bindConstantComparison(*cp.expression);
        return pat;
    }
    case syntax::SyntaxKind::RelationalPattern: {
        const auto& rp = static_cast<const syntax::RelationalPatternSyntax&>(syntaxTree);
        pat->kind = BoundPatternKind::Relational;
        switch (rp.operatorToken.kind) {
        case syntax::SyntaxKind::LessToken:
            pat->comparisonOperator = BoundBinaryOperatorKind::Less;
            break;
        case syntax::SyntaxKind::LessOrEqualsToken:
            pat->comparisonOperator = BoundBinaryOperatorKind::LessOrEquals;
            break;
        case syntax::SyntaxKind::GreaterToken:
            pat->comparisonOperator = BoundBinaryOperatorKind::Greater;
            break;
        default:
            pat->comparisonOperator = BoundBinaryOperatorKind::GreaterOrEquals;
            break;
        }
        if (!rp.operand) return pat;

        syntax::TokenValue evalVal;
        PrimitiveType evalType = PrimitiveType::Error;
        const TypeSymbol* ownerPtr = currentOwnerType_ ? &(*currentOwnerType_) : nullptr;
        const bool isConst = evaluateConstantSyntaxExpression(
            *rp.operand, ownerPtr, visibleTypes_, evalVal, evalType, &scopes_);
        auto boundConst = bindExpression(*rp.operand);
        if (boundConst->type == PrimitiveType::Error) return pat;
        if (!isConst || boundConst->type == PrimitiveType::Null) {
            diagnostics_.report(
                "RS8936",
                "relational pattern requires a compile-time constant operand",
                rp.operand->span());
            return pat;
        }

        PrimitiveType effectiveInput = inputType;
        std::string effectiveInputName = inputTypeName;
        if (inputIsNullableStruct) {
            pat->unwrapsNullableStruct = true;
            pat->nullableStructType = nullableDesc;
            pat->nullableHasValueField = nullableHasVal;
            pat->nullableValueField = nullableVal;
            pat->unwrappedTempVariable = allocateTempVariable(
                "pat_unwrap", nullableVal.type, nullableVal.typeName, syntaxTree.span());
            effectiveInput = nullableVal.type;
            effectiveInputName = nullableVal.typeName;
        }

        if (effectiveInput == PrimitiveType::Enum) {
            if (boundConst->type != PrimitiveType::Enum ||
                (!effectiveInputName.empty() && !boundConst->typeName.empty() &&
                 effectiveInputName != boundConst->typeName)) {
                diagnostics_.report(
                    "RS8937",
                    "enum relational pattern requires a constant of the same enum type '" +
                        effectiveInputName + "'",
                    rp.operand->span());
                return pat;
            }
            const std::int64_t rhsVal = std::holds_alternative<std::int64_t>(evalVal)
                ? std::get<std::int64_t>(evalVal)
                : std::int64_t{0};
            std::vector<std::int64_t> domainValues{0};
            const auto enumIt = visibleTypes_.find(effectiveInputName);
            if (enumIt != visibleTypes_.end()) {
                for (const auto& em : enumIt->second.enumMembers) {
                    if (std::find(domainValues.begin(), domainValues.end(), em.value) ==
                        domainValues.end()) {
                        domainValues.push_back(em.value);
                    }
                }
            }
            std::vector<std::int64_t> matchingVals;
            std::vector<std::int64_t> nonMatchingVals;
            for (const auto v : domainValues) {
                bool ok = false;
                switch (pat->comparisonOperator) {
                case BoundBinaryOperatorKind::Less: ok = (v < rhsVal); break;
                case BoundBinaryOperatorKind::LessOrEquals: ok = (v <= rhsVal); break;
                case BoundBinaryOperatorKind::Greater: ok = (v > rhsVal); break;
                case BoundBinaryOperatorKind::GreaterOrEquals: ok = (v >= rhsVal); break;
                default: break;
                }
                if (ok) matchingVals.push_back(v);
                else nonMatchingVals.push_back(v);
            }
            auto makeEnumEqPat = [&](std::int64_t v) {
                auto eqPat = std::make_unique<BoundPattern>();
                eqPat->kind = BoundPatternKind::Constant;
                eqPat->span = rp.span();
                eqPat->inputType = effectiveInput;
                eqPat->inputTypeName = effectiveInputName;
                eqPat->comparisonOperator = BoundBinaryOperatorKind::Equals;
                eqPat->comparisonType = PrimitiveType::Enum;
                auto enumLit = std::make_unique<BoundLiteralExpression>();
                enumLit->span = rp.operand->span();
                enumLit->type = PrimitiveType::Enum;
                enumLit->typeName = effectiveInputName;
                enumLit->value = v;
                eqPat->constantValue = std::move(enumLit);
                return eqPat;
            };
            auto buildOrTree = [&](const std::vector<std::int64_t>& vals) {
                auto acc = makeEnumEqPat(vals.front());
                for (std::size_t idx = 1; idx < vals.size(); ++idx) {
                    auto orNode = std::make_unique<BoundPattern>();
                    orNode->kind = BoundPatternKind::LogicalOr;
                    orNode->span = rp.span();
                    orNode->inputType = effectiveInput;
                    orNode->inputTypeName = effectiveInputName;
                    orNode->left = std::move(acc);
                    orNode->right = makeEnumEqPat(vals[idx]);
                    acc = std::move(orNode);
                }
                return acc;
            };
            if (matchingVals.empty()) {
                pat->kind = BoundPatternKind::Constant;
                pat->hasStaticBool = true;
                pat->staticBoolValue = false;
                return pat;
            }
            if (nonMatchingVals.empty()) {
                pat->kind = BoundPatternKind::Constant;
                pat->hasStaticBool = true;
                pat->staticBoolValue = true;
                return pat;
            }
            if (matchingVals.size() <= nonMatchingVals.size()) {
                auto built = buildOrTree(matchingVals);
                built->unwrapsNullableStruct = pat->unwrapsNullableStruct;
                built->nullableStructType = pat->nullableStructType;
                built->nullableHasValueField = pat->nullableHasValueField;
                built->nullableValueField = pat->nullableValueField;
                built->unwrappedTempVariable = pat->unwrappedTempVariable;
                built->inputType = inputType;
                built->inputTypeName = inputTypeName;
                return built;
            }
            auto notNode = std::make_unique<BoundPattern>();
            notNode->kind = BoundPatternKind::LogicalNot;
            notNode->span = rp.span();
            notNode->inputType = inputType;
            notNode->inputTypeName = inputTypeName;
            notNode->unwrapsNullableStruct = pat->unwrapsNullableStruct;
            notNode->nullableStructType = pat->nullableStructType;
            notNode->nullableHasValueField = pat->nullableHasValueField;
            notNode->nullableValueField = pat->nullableValueField;
            notNode->unwrappedTempVariable = pat->unwrappedTempVariable;
            notNode->left = buildOrTree(nonMatchingVals);
            return notNode;
        }

        if (!isNumericType(effectiveInput) || !isNumericType(boundConst->type)) {
            diagnostics_.report(
                "RS8937",
                "relational pattern is only valid on numeric, char, or enum values",
                rp.span());
            return pat;
        }

        if (effectiveInput == PrimitiveType::Char) {
            if (boundConst->type != PrimitiveType::Char && !isIntegralType(boundConst->type)) {
                diagnostics_.report(
                    "RS8937",
                    "char relational pattern requires a char or integral constant",
                    rp.operand->span());
                return pat;
            }
            pat->comparisonType = PrimitiveType::Int;
            pat->constantValue = convertExpression(
                std::move(boundConst),
                PrimitiveType::Int,
                rp.operand->span(),
                "relational pattern operand");
            return pat;
        }

        if (effectiveInput == PrimitiveType::ULong) {
            if (isFloatingPointType(boundConst->type)) {
                pat->comparisonType = PrimitiveType::Double;
                pat->constantValue = convertExpression(
                    std::move(boundConst),
                    PrimitiveType::Double,
                    rp.operand->span(),
                    "relational pattern operand");
                return pat;
            }
            if (boundConst->type != PrimitiveType::ULong &&
                std::holds_alternative<std::int64_t>(evalVal) &&
                std::get<std::int64_t>(evalVal) < 0) {
                diagnostics_.report(
                    "RS8937",
                    "cannot compare unsigned 'ulong' with negative constant in relational pattern",
                    rp.operand->span());
                return pat;
            }
            pat->comparisonType = PrimitiveType::ULong;
            auto ulLit = std::make_unique<BoundLiteralExpression>();
            ulLit->span = rp.operand->span();
            ulLit->type = PrimitiveType::ULong;
            ulLit->value = evalVal;
            pat->constantValue = std::move(ulLit);
            return pat;
        }

        if (effectiveInput == PrimitiveType::UInt) {
            if (boundConst->type == PrimitiveType::UInt ||
                (isIntegralType(boundConst->type) &&
                 std::holds_alternative<std::int64_t>(evalVal) &&
                 std::get<std::int64_t>(evalVal) >= 0 &&
                 static_cast<std::uint64_t>(std::get<std::int64_t>(evalVal)) <=
                     static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()))) {
                pat->comparisonType = PrimitiveType::UInt;
                auto uiLit = std::make_unique<BoundLiteralExpression>();
                uiLit->span = rp.operand->span();
                uiLit->type = PrimitiveType::UInt;
                uiLit->value = evalVal;
                pat->constantValue = std::move(uiLit);
                return pat;
            }
        }

        // General signed/floating carrier: Int, Long, Float, or Double
        PrimitiveType carrier = commonNumericType(effectiveInput, boundConst->type);
        if (carrier == PrimitiveType::Error) {
            diagnostics_.report(
                "RS8937",
                "incompatible types in relational pattern",
                rp.span());
            return pat;
        }
        if (carrier == PrimitiveType::Byte || carrier == PrimitiveType::SByte ||
            carrier == PrimitiveType::Short || carrier == PrimitiveType::UShort ||
            carrier == PrimitiveType::Char) {
            carrier = PrimitiveType::Int;
        }
        pat->comparisonType = carrier;
        pat->constantValue = convertExpression(
            std::move(boundConst),
            carrier,
            rp.operand->span(),
            "relational pattern operand");
        return pat;
    }
    case syntax::SyntaxKind::UnaryPattern: {
        const auto& un = static_cast<const syntax::UnaryPatternSyntax&>(syntaxTree);
        pat->kind = BoundPatternKind::LogicalNot;
        if (un.pattern) {
            pat->left = bindPattern(*un.pattern, inputType, inputTypeName, false, isSwitchPattern);
        }
        return pat;
    }
    case syntax::SyntaxKind::BinaryPattern: {
        const auto& bin = static_cast<const syntax::BinaryPatternSyntax&>(syntaxTree);
        const bool isAnd = (bin.operatorToken.text == "and");
        pat->kind = isAnd ? BoundPatternKind::LogicalAnd : BoundPatternKind::LogicalOr;
        const bool childAllowVars = isAnd && allowVariables;
        if (bin.left) {
            pat->left = bindPattern(*bin.left, inputType, inputTypeName, childAllowVars, isSwitchPattern);
        }
        if (bin.right) {
            pat->right = bindPattern(*bin.right, inputType, inputTypeName, childAllowVars, isSwitchPattern);
        }
        return pat;
    }
    case syntax::SyntaxKind::TypePattern: {
        const auto& tp = static_cast<const syntax::TypePatternSyntax&>(syntaxTree);
        // Check if bare identifier without designation refers to a local const or const field
        if (!tp.designationToken && !tp.type.isGeneric() &&
            !tp.type.isArray() && !tp.type.isNullable() &&
            resolvePrimitiveType(tp.type.name.text) == PrimitiveType::Error) {
            const auto* localVar = lookupVariable(tp.type.name.text);
            const FieldSymbol* constFld = (!localVar && currentOwnerType_)
                ? findField(*currentOwnerType_, tp.type.name.text)
                : nullptr;
            if ((localVar && localVar->isConst) ||
                (constFld && constFld->constField)) {
                syntax::NameExpressionSyntax nameSyn;
                nameSyn.identifierToken = tp.type.name;
                bindConstantComparison(nameSyn);
                return pat;
            }
        }

        pat->kind = BoundPatternKind::Type;
        pat->hasTypeCheck = true;
        pat->targetType = bindType(tp.type, false, &pat->targetTypeName);
        if (pat->targetType == PrimitiveType::Error) return pat;
        pat->targetTypeId = isExactType(pat->targetType) && !pat->targetTypeName.empty()
            ? stableTypeId(pat->targetTypeName)
            : 0;

        PrimitiveType effectiveInput = inputType;
        std::string effectiveInputName = inputTypeName;
        if (inputIsNullableStruct &&
            (pat->targetType != PrimitiveType::Struct || pat->targetTypeName != inputTypeName)) {
            pat->unwrapsNullableStruct = true;
            pat->nullableStructType = nullableDesc;
            pat->nullableHasValueField = nullableHasVal;
            pat->nullableValueField = nullableVal;
            pat->unwrappedTempVariable = allocateTempVariable(
                "pat_unwrap", nullableVal.type, nullableVal.typeName, syntaxTree.span());
            effectiveInput = nullableVal.type;
            effectiveInputName = nullableVal.typeName;
        }

        if (isReferenceType(effectiveInput) && isReferenceType(pat->targetType)) {
            pat->staticTypeMatch = false;
        } else if (!isSwitchPattern && isReferenceType(effectiveInput)) {
            // In `is` expressions, reference/boxed operands may test against value types via Opcode::IsType
            pat->staticTypeMatch = false;
            if (tp.designationToken) {
                diagnostics_.report(
                    "RS8920",
                    "type pattern designation requires an 'is' reference-type pattern",
                    tp.span());
                return pat;
            }
        } else if (effectiveInput == pat->targetType &&
                   (!isExactType(effectiveInput) || effectiveInputName == pat->targetTypeName) &&
                   (!isSwitchPattern || inputIsNullableStruct || !tp.designationToken || isReferenceType(pat->targetType))) {
            pat->staticTypeMatch = true;
        } else if (!isSwitchPattern && !tp.designationToken) {
            // Value-type tested against unrelated type in `is` expression (e.g. generic specialization `val is RefType`)
            pat->kind = BoundPatternKind::Constant;
            pat->hasStaticBool = true;
            pat->staticBoolValue = false;
            return pat;
        } else {
            diagnostics_.report(
                "RS8921",
                "switch type patterns require reference types",
                tp.span());
            return pat;
        }

        if (tp.designationToken) {
            if (!isSwitchPattern && !isReferenceType(pat->targetType) && !inputIsNullableStruct && !pat->staticTypeMatch) {
                diagnostics_.report(
                    "RS8920",
                    "type pattern designation requires an 'is' reference-type pattern",
                    tp.span());
                return pat;
            }
            pat->variable = declarePatternDesignation(
                *tp.designationToken, pat->targetType, pat->targetTypeName);
        }
        return pat;
    }
    case syntax::SyntaxKind::PropertyPattern: {
        const auto& pp = static_cast<const syntax::PropertyPatternSyntax&>(syntaxTree);
        pat->kind = BoundPatternKind::Property;

        PrimitiveType effectiveType = inputType;
        std::string effectiveTypeName = inputTypeName;

        if (pp.type) {
            pat->hasTypeCheck = true;
            pat->targetType = bindType(*pp.type, false, &pat->targetTypeName);
            if (pat->targetType == PrimitiveType::Error) return pat;
            pat->targetTypeId = isExactType(pat->targetType) && !pat->targetTypeName.empty()
                ? stableTypeId(pat->targetTypeName)
                : 0;

            PrimitiveType checkInput = inputType;
            std::string checkInputName = inputTypeName;
            if (inputIsNullableStruct &&
                (pat->targetType != PrimitiveType::Struct || pat->targetTypeName != inputTypeName)) {
                pat->unwrapsNullableStruct = true;
                pat->nullableStructType = nullableDesc;
                pat->nullableHasValueField = nullableHasVal;
                pat->nullableValueField = nullableVal;
                pat->unwrappedTempVariable = allocateTempVariable(
                    "pat_unwrap", nullableVal.type, nullableVal.typeName, syntaxTree.span());
                checkInput = nullableVal.type;
                checkInputName = nullableVal.typeName;
            }

            if (isReferenceType(checkInput) && isReferenceType(pat->targetType)) {
                pat->staticTypeMatch = false;
            } else if (checkInput == pat->targetType &&
                       (!isExactType(checkInput) || checkInputName == pat->targetTypeName)) {
                pat->staticTypeMatch = true;
            } else {
                diagnostics_.report(
                    "RS8921",
                    "cannot match property pattern type '" +
                        (pat->targetTypeName.empty()
                            ? std::string(primitiveTypeName(pat->targetType))
                            : pat->targetTypeName) +
                        "' against value of type '" +
                        (inputTypeName.empty()
                            ? std::string(primitiveTypeName(inputType))
                            : inputTypeName) + "'",
                    pp.type->span());
                return pat;
            }
            effectiveType = pat->targetType;
            effectiveTypeName = pat->targetTypeName;
        } else if (inputIsNullableStruct) {
            pat->unwrapsNullableStruct = true;
            pat->nullableStructType = nullableDesc;
            pat->nullableHasValueField = nullableHasVal;
            pat->nullableValueField = nullableVal;
            pat->unwrappedTempVariable = allocateTempVariable(
                "pat_unwrap", nullableVal.type, nullableVal.typeName, syntaxTree.span());
            effectiveType = nullableVal.type;
            effectiveTypeName = nullableVal.typeName;
            pat->targetType = effectiveType;
            pat->targetTypeName = effectiveTypeName;
            pat->targetTypeId = isExactType(effectiveType) && !effectiveTypeName.empty()
                ? stableTypeId(effectiveTypeName)
                : 0;
        } else {
            pat->targetType = effectiveType;
            pat->targetTypeName = effectiveTypeName;
            pat->targetTypeId = isExactType(effectiveType) && !effectiveTypeName.empty()
                ? stableTypeId(effectiveTypeName)
                : 0;
        }

        // Allocate temporary slot for narrowed/non-null receiver so subpattern accesses & designation reuse it
        pat->receiverTempVariable = allocateTempVariable(
            "pat_recv", effectiveType, effectiveTypeName, pp.span());

        for (const auto& subSyn : pp.properties) {
            BoundPropertySubpattern boundSub;
            boundSub.span = subSyn.span();

            auto makeRecvExpr = [&]() {
                return makeVariableAccess(*pat->receiverTempVariable, subSyn.memberToken.span);
            };

            const auto& memberName = subSyn.memberToken.text;
            std::unique_ptr<BoundExpression> accessExpr;
            if (effectiveType == PrimitiveType::Array &&
                (memberName == "Length" || memberName == "Count" || memberName == "length")) {
                auto arrLen = std::make_unique<BoundArrayLengthExpression>();
                arrLen->span = subSyn.memberToken.span;
                arrLen->type = PrimitiveType::Int;
                arrLen->receiver = makeRecvExpr();
                accessExpr = std::move(arrLen);
            } else if (effectiveType == PrimitiveType::String &&
                       (memberName == "Length" || memberName == "Count" || memberName == "length")) {
                auto call = std::make_unique<BoundCallExpression>();
                call->span = subSyn.memberToken.span;
                call->type = PrimitiveType::Int;
                call->function = makeIntrinsicFunction(
                    "$intrinsic::string_length",
                    PrimitiveType::Int,
                    "",
                    {{PrimitiveType::String, ""}});
                call->function.method = true;
                call->function.staticMethod = false;
                call->arguments.push_back(makeRecvExpr());
                accessExpr = std::move(call);
            } else if ((effectiveType == PrimitiveType::Object ||
                        effectiveType == PrimitiveType::Struct) &&
                       !effectiveTypeName.empty()) {
                const auto ownerIt = visibleTypes_.find(effectiveTypeName);
                if (ownerIt == visibleTypes_.end()) {
                    diagnostics_.report(
                        "RS2405",
                        "type descriptor '" + effectiveTypeName + "' is unavailable",
                        subSyn.memberToken.span);
                } else {
                    const auto& ownerDesc = ownerIt->second;
                    if (const auto* field = findField(ownerDesc, memberName)) {
                        if (!isMemberAccessible(field->accessibility, field->declaringTypeId)) {
                            diagnostics_.report(
                                "RS2534",
                                "field '" + field->name + "' is inaccessible",
                                subSyn.memberToken.span);
                        }
                        if (field->staticField || field->constField) {
                            diagnostics_.report(
                                "RS2484",
                                "property pattern cannot match static or const field '" +
                                    field->name + "'",
                                subSyn.memberToken.span);
                        }
                        checkObsoleteReference(
                            field->id, "field", field->name, subSyn.memberToken.span);
                        recordSymbolReference(
                            occurrences_,
                            currentSourceName_,
                            field->id,
                            SymbolKind::Field,
                            field->name,
                            subSyn.memberToken.span);
                        if (ownerDesc.kind == TypeKind::Struct) {
                            auto sfa = std::make_unique<BoundStructFieldAccessExpression>();
                            sfa->span = subSyn.memberToken.span;
                            sfa->type = field->type;
                            sfa->typeName = field->typeName;
                            sfa->receiver = makeRecvExpr();
                            sfa->ownerType = ownerDesc;
                            sfa->field = *field;
                            accessExpr = std::move(sfa);
                        } else {
                            auto mfa = std::make_unique<BoundMemberAccessExpression>();
                            mfa->span = subSyn.memberToken.span;
                            mfa->type = field->type;
                            mfa->typeName = field->typeName;
                            mfa->receiver = makeRecvExpr();
                            mfa->ownerType = ownerDesc;
                            mfa->field = *field;
                            accessExpr = std::move(mfa);
                        }
                    } else if (const auto* prop = findProperty(ownerDesc, memberName)) {
                        if (!isMemberAccessible(prop->accessibility, prop->declaringTypeId)) {
                            diagnostics_.report(
                                "RS2534",
                                "property '" + prop->name + "' is inaccessible",
                                subSyn.memberToken.span);
                        }
                        if (prop->staticProperty) {
                            diagnostics_.report(
                                "RS2484",
                                "property pattern requires an instance property, not static '" +
                                    prop->name + "'",
                                subSyn.memberToken.span);
                        } else if (!prop->getter) {
                            diagnostics_.report(
                                "RS2481",
                                "property '" + prop->name + "' is write-only",
                                subSyn.memberToken.span);
                        } else {
                            checkObsoleteReference(
                                prop->id, "property", prop->name, subSyn.memberToken.span);
                            recordSymbolReference(
                                occurrences_,
                                currentSourceName_,
                                prop->id,
                                SymbolKind::Property,
                                prop->name,
                                subSyn.memberToken.span);
                            std::vector<const syntax::ExpressionSyntax*> emptySyntaxArgs;
                            std::vector<std::optional<syntax::SyntaxToken>> emptyMods;
                            accessExpr = bindSelectedCall(
                                *prop->getter,
                                {},
                                emptySyntaxArgs,
                                emptyMods,
                                {},
                                makeRecvExpr(),
                                subSyn.memberToken.span,
                                "property pattern getter");
                        }
                    } else if (memberName == "Count" || memberName == "Length") {
                        auto getterMethods = findMethods(ownerDesc, memberName, false);
                        if (getterMethods.empty() && memberName == "Length") {
                            getterMethods = findMethods(ownerDesc, "Count", false);
                        }
                        for (const auto* m : getterMethods) {
                            if (m->parameters.size() == visibleParameterOffset(*m) &&
                                isMemberAccessible(m->accessibility, m->declaringTypeId, m->moduleName)) {
                                std::vector<const syntax::ExpressionSyntax*> emptySyntaxArgs;
                                std::vector<std::optional<syntax::SyntaxToken>> emptyMods;
                                accessExpr = bindSelectedCall(
                                    *m,
                                    {},
                                    emptySyntaxArgs,
                                    emptyMods,
                                    {},
                                    makeRecvExpr(),
                                    subSyn.memberToken.span,
                                    "property pattern getter");
                                break;
                            }
                        }
                    }
                    if (!accessExpr) {
                        diagnostics_.report(
                            "RS2406",
                            "type '" + effectiveTypeName + "' has no readable field or property '" +
                                memberName + "'",
                            subSyn.memberToken.span);
                    }
                }
            } else {
                diagnostics_.report(
                    "RS2404",
                    "property pattern member '" + memberName +
                        "' requires a class, struct, string, or array value",
                    subSyn.memberToken.span);
            }

            if (!accessExpr) {
                accessExpr = makeError(subSyn.memberToken.span);
            }
            const auto memberType = accessExpr->type;
            const auto memberTypeName = accessExpr->typeName;
            if (memberType != PrimitiveType::Error) {
                boundSub.memberTempVariable = allocateTempVariable(
                    "pat_mem", memberType, memberTypeName, subSyn.span());
            }
            boundSub.memberAccess = std::move(accessExpr);
            if (subSyn.pattern) {
                boundSub.pattern = bindPattern(
                    *subSyn.pattern, memberType, memberTypeName, allowVariables);
            }
            pat->properties.push_back(std::move(boundSub));
        }

        if (pp.designationToken) {
            pat->variable = declarePatternDesignation(
                *pp.designationToken, effectiveType, effectiveTypeName);
        }
        return pat;
    }
    default:
        return pat;
    }
}

std::unique_ptr<BoundExpression> Binder::bindTypeBinaryExpression(
    const syntax::TypeBinaryExpressionSyntax& syntaxTree) {
    auto operand = bindExpression(*syntaxTree.expression);
    if (operand->type == PrimitiveType::Error) {
        return makeError(syntaxTree.span());
    }
    const bool safeCast = syntaxTree.operatorToken.kind ==
        syntax::SyntaxKind::AsKeyword;
    if (safeCast) {
        std::string targetTypeName;
        const auto targetType = bindType(
            syntaxTree.type, false, &targetTypeName);
        if (targetType == PrimitiveType::Error) {
            return makeError(syntaxTree.span());
        }
        if (!isReferenceType(operand->type) ||
            !isReferenceType(targetType)) {
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
        result->safeCast = true;
        result->type = targetType;
        result->typeName = targetTypeName;
        return result;
    }

    if (operand->type == PrimitiveType::Null) {
        operand = convertExpression(
            std::move(operand),
            PrimitiveType::Object,
            syntaxTree.expression->span(),
            "pattern operand");
    }

    auto result = std::make_unique<BoundTypeBinaryExpression>();
    result->span = syntaxTree.span();
    result->safeCast = false;
    result->type = PrimitiveType::Bool;
    result->patternInputVariable = allocateTempVariable(
        "is_input", operand->type, operand->typeName, syntaxTree.expression->span());
    const auto opType = operand->type;
    const auto opTypeName = operand->typeName;
    result->expression = std::move(operand);

    if (syntaxTree.pattern) {
        result->pattern = bindPattern(
            *syntaxTree.pattern, opType, opTypeName, true);
        if (result->pattern && result->pattern->variable) {
            result->patternVariable = result->pattern->variable;
        }
    } else {
        std::string targetTypeName;
        const auto targetType = bindType(syntaxTree.type, false, &targetTypeName);
        if (targetType == PrimitiveType::Error) return makeError(syntaxTree.span());
        result->targetType = targetType;
        result->targetTypeName = targetTypeName;
        result->targetTypeId = isExactType(targetType) ? stableTypeId(targetTypeName) : 0;
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
    if (result->expression->type == PrimitiveType::Error) {
        return makeError(syntaxTree.span());
    }
    if (result->expression->type == PrimitiveType::Null) {
        result->expression = convertExpression(
            std::move(result->expression),
            PrimitiveType::Object,
            syntaxTree.expression->span(),
            "switch expression");
    }
    result->valueVariable = allocateTempVariable(
        "sw_expr_val",
        result->expression->type,
        result->expression->typeName,
        syntaxTree.expression->span());

    PrimitiveType commonType = PrimitiveType::Error;
    std::string commonTypeName;
    bool hasCatchAll = false;
    bool hasExplicitDiscard = false;
    bool sawBoolTrue = false;
    bool sawBoolFalse = false;
    bool sawNullArm = false;
    bool sawNotNullArm = false;
    std::unordered_set<std::int64_t> coveredEnumValues;
    bool hasLowerUnbounded = false;
    std::int64_t maxLowerCovered = 0; // covers (-inf, maxLowerCovered] or track complement
    std::vector<std::pair<BoundBinaryOperatorKind, std::int64_t>> intRelArms;
    std::vector<std::pair<BoundBinaryOperatorKind, double>> floatRelArms;

    const auto isUnconditionalPattern = [](const BoundPattern& pat) noexcept -> bool {
        if (pat.kind == BoundPatternKind::Discard || pat.kind == BoundPatternKind::Var) {
            return true;
        }
        if (pat.kind == BoundPatternKind::Type &&
            pat.staticTypeMatch && !pat.unwrapsNullableStruct) {
            return true;
        }
        if (pat.kind == BoundPatternKind::Property &&
            pat.properties.empty() && !isReferenceType(pat.inputType) &&
            !pat.unwrapsNullableStruct && (!pat.hasTypeCheck || pat.staticTypeMatch)) {
            return true;
        }
        return false;
    };

    for (const auto& sourceArm : syntaxTree.arms) {
        BoundSwitchExpressionArm arm;
        arm.span = sourceArm.span();
        const bool explicitDiscard =
            sourceArm.discardToken.has_value() ||
            (sourceArm.pattern && sourceArm.pattern->kind() == syntax::SyntaxKind::DiscardPattern);
        arm.discard = explicitDiscard;
        if (explicitDiscard) {
            if (hasExplicitDiscard) {
                diagnostics_.report(
                    "RS8923", "switch expression has more than one discard arm",
                    sourceArm.span());
            }
            hasExplicitDiscard = true;
        }
        pushScope(sourceArm.span());
        if (sourceArm.pattern) {
            arm.pattern = bindPattern(
                *sourceArm.pattern,
                result->expression->type,
                result->expression->typeName,
                true,
                true);
        } else if (sourceArm.label) {
            arm.label = convertExpression(
                bindExpression(*sourceArm.label), result->expression->type,
                sourceArm.label->span(), "switch expression pattern",
                result->expression->typeName);
        }
        if (sourceArm.guard) {
            if (explicitDiscard) {
                diagnostics_.report(
                    "RS8925", "discard switch arm cannot have a guard",
                    sourceArm.guard->span());
            }
            arm.guard = convertExpression(
                bindExpression(*sourceArm.guard), PrimitiveType::Bool,
                sourceArm.guard->span(), "switch expression guard");
        } else {
            (void)hasLowerUnbounded;
            (void)maxLowerCovered;
            if (explicitDiscard) {
                hasCatchAll = true;
                arm.discard = true;
            } else if (arm.pattern && isUnconditionalPattern(*arm.pattern)) {
                hasCatchAll = true;
            } else if (arm.pattern) {
                const auto& p = *arm.pattern;
                if (p.kind == BoundPatternKind::Constant && p.constantNullCheck) {
                    sawNullArm = true;
                } else if (p.kind == BoundPatternKind::LogicalNot &&
                           p.left && p.left->kind == BoundPatternKind::Constant &&
                           p.left->constantNullCheck) {
                    sawNotNullArm = true;
                } else if (p.kind == BoundPatternKind::Property &&
                           p.properties.empty() &&
                           (!p.hasTypeCheck || p.staticTypeMatch)) {
                    sawNotNullArm = true;
                } else if (result->expression->type == PrimitiveType::Bool &&
                           p.kind == BoundPatternKind::Constant &&
                           p.constantValue &&
                           p.constantValue->kind() == BoundNodeKind::LiteralExpression) {
                    const auto& lit = static_cast<const BoundLiteralExpression&>(*p.constantValue);
                    if (const auto* bv = std::get_if<bool>(&lit.value)) {
                        if (*bv) sawBoolTrue = true;
                        else sawBoolFalse = true;
                    }
                } else if (result->expression->type == PrimitiveType::Enum &&
                           p.kind == BoundPatternKind::Constant &&
                           p.constantValue &&
                           p.constantValue->kind() == BoundNodeKind::LiteralExpression) {
                    const auto& lit = static_cast<const BoundLiteralExpression&>(*p.constantValue);
                    if (const auto* iv = std::get_if<std::int64_t>(&lit.value)) {
                        coveredEnumValues.insert(*iv);
                    }
                } else if (p.kind == BoundPatternKind::Relational &&
                           p.constantValue &&
                           p.constantValue->kind() == BoundNodeKind::LiteralExpression) {
                    const auto& lit = static_cast<const BoundLiteralExpression&>(*p.constantValue);
                    if (const auto* iv = std::get_if<std::int64_t>(&lit.value)) {
                        intRelArms.emplace_back(p.comparisonOperator, *iv);
                    } else if (const auto* dv = std::get_if<double>(&lit.value)) {
                        floatRelArms.emplace_back(p.comparisonOperator, *dv);
                    }
                }
            }
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
    bool exhaustiveByCases =
        (result->expression->type == PrimitiveType::Bool && sawBoolTrue && sawBoolFalse) ||
        (sawNullArm && sawNotNullArm);
    if (!exhaustiveByCases && result->expression->type == PrimitiveType::Enum) {
        const auto enumIt = visibleTypes_.find(result->expression->typeName);
        if (enumIt != visibleTypes_.end() && !enumIt->second.enumMembers.empty()) {
            bool allEnumCovered = true;
            for (const auto& em : enumIt->second.enumMembers) {
                if (coveredEnumValues.find(em.value) == coveredEnumValues.end()) {
                    allEnumCovered = false;
                    break;
                }
            }
            if (allEnumCovered) exhaustiveByCases = true;
        }
    }
    if (!exhaustiveByCases && isIntegralType(result->expression->type) && !intRelArms.empty()) {
        // Check if (-inf, loMax] and [hiMin, +inf) meet or overlap: loMax + 1 >= hiMin
        bool hasLo = false;
        std::int64_t loMax = std::numeric_limits<std::int64_t>::min();
        bool hasHi = false;
        std::int64_t hiMin = std::numeric_limits<std::int64_t>::max();
        for (const auto& [op, val] : intRelArms) {
            if (op == BoundBinaryOperatorKind::Less && val > std::numeric_limits<std::int64_t>::min()) {
                loMax = hasLo ? std::max(loMax, val - 1) : (val - 1);
                hasLo = true;
            } else if (op == BoundBinaryOperatorKind::LessOrEquals) {
                loMax = hasLo ? std::max(loMax, val) : val;
                hasLo = true;
            } else if (op == BoundBinaryOperatorKind::Greater && val < std::numeric_limits<std::int64_t>::max()) {
                hiMin = hasHi ? std::min(hiMin, val + 1) : (val + 1);
                hasHi = true;
            } else if (op == BoundBinaryOperatorKind::GreaterOrEquals) {
                hiMin = hasHi ? std::min(hiMin, val) : val;
                hasHi = true;
            }
        }
        if (hasLo && hasHi && (loMax >= hiMin || loMax + 1 >= hiMin)) {
            exhaustiveByCases = true;
        }
    }
    if (!exhaustiveByCases && isFloatingPointType(result->expression->type) && !floatRelArms.empty()) {
        for (const auto& [op1, v1] : floatRelArms) {
            for (const auto& [op2, v2] : floatRelArms) {
                const bool leftCoversBelow =
                    (op1 == BoundBinaryOperatorKind::Less || op1 == BoundBinaryOperatorKind::LessOrEquals);
                const bool rightCoversAbove =
                    (op2 == BoundBinaryOperatorKind::Greater || op2 == BoundBinaryOperatorKind::GreaterOrEquals);
                if (leftCoversBelow && rightCoversAbove) {
                    if (v1 > v2 ||
                        (v1 == v2 && (op1 == BoundBinaryOperatorKind::LessOrEquals ||
                                      op2 == BoundBinaryOperatorKind::GreaterOrEquals))) {
                        exhaustiveByCases = true;
                    }
                }
            }
        }
    }
    if (!hasCatchAll && !exhaustiveByCases) {
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
    const auto compoundOp = compoundBinaryOperatorToken(syntaxTree.equalsToken);
    const auto bindAssignedValue = [&](
        PrimitiveType targetType,
        const std::string& targetTypeName,
        const std::string& context) -> std::unique_ptr<BoundExpression> {
        if (!compoundOp) {
            return bindTargetExpression(
                *syntaxTree.expression, targetType, targetTypeName, context);
        }
        syntax::NameExpressionSyntax lhsSyntax;
        lhsSyntax.identifierToken = syntaxTree.identifierToken;
        auto lhsValue = bindNameExpression(lhsSyntax);
        auto rhsValue = bindExpression(*syntaxTree.expression);
        auto combined = bindBinaryOperation(
            std::move(lhsValue),
            syntaxTree.identifierToken.span,
            *compoundOp,
            std::move(rhsValue),
            syntaxTree.expression->span(),
            syntaxTree.span());
        return convertExpression(
            std::move(combined), targetType, syntaxTree.span(),
            context, targetTypeName);
    };

    if (const auto* foundVariable = lookupVariable(
            syntaxTree.identifierToken.text)) {
        const VariableSymbol variable = *foundVariable;
        if (variable.name != "this" && !compoundOp) {
            recordSymbolReference(
                occurrences_,
                currentSourceName_,
                variable.id,
                variable.parameter ? SymbolKind::Parameter : SymbolKind::Local,
                variable.name,
                isExactType(variable.type) && !variable.typeName.empty()
                    ? variable.typeName
                    : primitiveTypeName(variable.type),
                syntaxTree.identifierToken.span);
        }
        if (variable.isConst) {
            diagnostics_.report(
                "RS2483",
                "cannot assign to const local variable '" +
                    variable.name + "'",
                syntaxTree.identifierToken.span);
            return makeError(syntaxTree.span());
        }
        if (variable.modifier == ParameterModifier::In) {
            diagnostics_.report(
                "RS8702",
                "cannot assign to in parameter '" +
                    variable.name + "'",
                syntaxTree.identifierToken.span);
            return makeError(syntaxTree.span());
        }
        if (variable.isUsingResource) {
            diagnostics_.report(
                "RS2651",
                "cannot assign to '" + variable.name +
                    "' because it is a 'using' variable",
                syntaxTree.identifierToken.span);
            return makeError(syntaxTree.span());
        }
        auto value = bindAssignedValue(
            variable.type, variable.typeName, "assignment");
        if (storageTypeOf(variable) != variable.type || storageTypeNameOf(variable) != variable.typeName) {
            const auto wrapper = visibleTypes_.find(
                storageTypeNameOf(variable));
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
            result->type = variable.type;
            result->typeName = variable.typeName;
            result->receiver = storageVariableExpression(
                variable, syntaxTree.span());
            result->ownerType = wrapper->second;
            result->field = wrapper->second.fields.front();
            result->expression = wrapResourceHandleOverwriteRelease(
                makeVariableAccess(variable, syntaxTree.identifierToken.span),
                std::move(value),
                syntaxTree.span());
            return result;
        }
        auto result = std::make_unique<BoundAssignmentExpression>();
        result->span = syntaxTree.span();
        result->type = variable.type;
        result->typeName = variable.typeName;
        result->variable = variable;
        result->expression = wrapResourceHandleOverwriteRelease(
            makeVariableAccess(variable, syntaxTree.identifierToken.span),
            std::move(value),
            syntaxTree.span());
        return result;
    }

    if (const auto field = sequenceLocalFields_.find(
            syntaxTree.identifierToken.text);
        field != sequenceLocalFields_.end()) {
        if (field->second.isUsingResource) {
            diagnostics_.report(
                "RS2651",
                "cannot assign to '" + field->first +
                    "' because it is a 'using' variable",
                syntaxTree.identifierToken.span);
            return makeError(syntaxTree.span());
        }
        const auto* foundReceiver = lookupVariable("this");
        if (!foundReceiver || !currentOwnerType_) return makeError(syntaxTree.span());
        const VariableSymbol receiver = *foundReceiver;
        auto result = std::make_unique<BoundMemberAssignmentExpression>();
        result->span = syntaxTree.span();
        result->type = field->second.type;
        result->typeName = field->second.typeName;
        result->receiver = variableExpression(receiver, syntaxTree.span());
        result->ownerType = *currentOwnerType_;
        result->field = field->second;
        auto seqVal = bindAssignedValue(
            field->second.type,
            field->second.typeName,
            "sequence local assignment");
        result->expression = wrapResourceHandleOverwriteRelease(
            makeSequenceFieldAccess(field->second, syntaxTree.identifierToken.span),
            std::move(seqVal),
            syntaxTree.span());
        return result;
    }

    if (currentOwnerType_) {
        std::optional<VariableSymbol> thisVariable;
        if (const auto* foundThis = lookupVariable("this")) {
            thisVariable = *foundThis;
        }
        if (const auto* field = findField(
                *currentOwnerType_, syntaxTree.identifierToken.text)) {
            if (!isMemberAccessible(
                    field->accessibility, field->declaringTypeId)) {
                diagnostics_.report(
                    "RS2534", "field '" + field->name + "' is inaccessible",
                    syntaxTree.span());
                return makeError(syntaxTree.span());
            }
            if (!compoundOp) {
                checkObsoleteReference(
                    field->id, "field", field->name, syntaxTree.identifierToken.span);
                recordSymbolReference(
                    occurrences_,
                    currentSourceName_,
                    field->id,
                    SymbolKind::Field,
                    field->name,
                    isExactType(field->type) && !field->typeName.empty()
                        ? field->typeName
                        : primitiveTypeName(field->type),
                    syntaxTree.identifierToken.span);
            }
            if (field->constField) {
                diagnostics_.report(
                    "RS2483",
                    "cannot assign to const field '" + field->name + "'",
                    syntaxTree.identifierToken.span);
                return makeError(syntaxTree.span());
            }
            if (field->readonlyField &&
                !isReadonlyFieldWritable(
                    *field, currentConstructor_, currentStaticMethod_, currentOwnerType_)) {
                diagnostics_.report(
                    "RS2483",
                    "cannot assign to readonly field '" + field->name + "'",
                    syntaxTree.identifierToken.span);
                return makeError(syntaxTree.span());
            }
            if (field->staticField) {
                auto value = bindAssignedValue(
                    field->type, field->typeName, "field assignment");
                auto oldAccess = std::make_unique<BoundMemberAccessExpression>();
                oldAccess->span = syntaxTree.identifierToken.span;
                oldAccess->type = field->type;
                oldAccess->typeName = field->typeName;
                oldAccess->ownerType = *currentOwnerType_;
                oldAccess->field = *field;
                auto result = std::make_unique<BoundMemberAssignmentExpression>();
                result->span = syntaxTree.span();
                result->type = field->type;
                result->typeName = field->typeName;
                result->ownerType = *currentOwnerType_;
                result->field = *field;
                result->expression = wrapResourceHandleOverwriteRelease(
                    std::move(oldAccess), std::move(value), syntaxTree.span());
                return result;
            }
            if (currentStaticMethod_ || !thisVariable) {
                diagnostics_.report(
                    "RS2484",
                    "instance field '" + field->name + "' requires a value receiver",
                    syntaxTree.identifierToken.span);
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
                auto value = bindAssignedValue(
                    valueField.type,
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
                auto oldCell =
                    std::make_unique<BoundMemberAccessExpression>();
                oldCell->span = syntaxTree.span();
                oldCell->type = field->type;
                oldCell->typeName = field->typeName;
                oldCell->receiver = variableExpression(
                    *thisVariable, syntaxTree.span());
                oldCell->ownerType = *currentOwnerType_;
                oldCell->field = *field;
                auto oldAccess =
                    std::make_unique<BoundMemberAccessExpression>();
                oldAccess->span = syntaxTree.span();
                oldAccess->type = valueField.type;
                oldAccess->typeName = valueField.typeName;
                oldAccess->receiver = std::move(oldCell);
                oldAccess->ownerType = captureStorage->second;
                oldAccess->field = valueField;
                auto result =
                    std::make_unique<BoundMemberAssignmentExpression>();
                result->span = syntaxTree.span();
                result->type = valueField.type;
                result->typeName = valueField.typeName;
                result->receiver = std::move(cell);
                result->ownerType = captureStorage->second;
                result->field = valueField;
                result->expression = wrapResourceHandleOverwriteRelease(
                    std::move(oldAccess), std::move(value), syntaxTree.span());
                return result;
            }
            auto value = bindAssignedValue(
                field->type,
                field->typeName, "field assignment");
            if (currentOwnerType_->kind == TypeKind::Struct) {
                auto oldAccess = std::make_unique<BoundStructFieldAccessExpression>();
                oldAccess->span = syntaxTree.identifierToken.span;
                oldAccess->type = field->type;
                oldAccess->typeName = field->typeName;
                oldAccess->receiver = makeVariableAccess(*thisVariable, syntaxTree.span());
                oldAccess->ownerType = *currentOwnerType_;
                oldAccess->field = *field;
                auto result = std::make_unique<BoundStructFieldAssignmentExpression>();
                result->span = syntaxTree.span();
                result->type = field->type;
                result->typeName = field->typeName;
                result->variable = *thisVariable;
                result->ownerType = *currentOwnerType_;
                result->field = *field;
                result->expression = wrapResourceHandleOverwriteRelease(
                    std::move(oldAccess), std::move(value), syntaxTree.span());
                if (storageTypeOf(*thisVariable) != thisVariable->type || storageTypeNameOf(*thisVariable) != thisVariable->typeName) {
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
            auto oldAccess = std::make_unique<BoundMemberAccessExpression>();
            oldAccess->span = syntaxTree.identifierToken.span;
            oldAccess->type = field->type;
            oldAccess->typeName = field->typeName;
            oldAccess->receiver = variableExpression(*thisVariable, syntaxTree.span());
            oldAccess->ownerType = *currentOwnerType_;
            oldAccess->field = *field;
            auto receiver = variableExpression(*thisVariable, syntaxTree.span());
            auto result = std::make_unique<BoundMemberAssignmentExpression>();
            result->span = syntaxTree.span();
            result->type = field->type;
            result->typeName = field->typeName;
            result->receiver = std::move(receiver);
            result->ownerType = *currentOwnerType_;
            result->field = *field;
            result->expression = wrapResourceHandleOverwriteRelease(
                std::move(oldAccess), std::move(value), syntaxTree.span());
            return result;
        }
        if (!currentStaticMethod_ && thisVariable && currentOwnerType_->synthetic) {
            const auto* capturedThis = findField(*currentOwnerType_, "this");
            const auto outer = capturedThis
                ? visibleTypes_.find(capturedThis->typeName)
                : visibleTypes_.end();
            const auto* outerField = outer != visibleTypes_.end()
                ? findField(outer->second, syntaxTree.identifierToken.text)
                : nullptr;
            if (capturedThis && outerField) {
                if (!compoundOp) {
                    recordSymbolReference(
                        occurrences_,
                        currentSourceName_,
                        outerField->id,
                        SymbolKind::Field,
                        outerField->name,
                        isExactType(outerField->type) && !outerField->typeName.empty()
                            ? outerField->typeName
                            : primitiveTypeName(outerField->type),
                        syntaxTree.identifierToken.span);
                }
                auto value = bindAssignedValue(
                    outerField->type,
                    outerField->typeName, "captured field assignment");
                auto oldCapRecv =
                    std::make_unique<BoundMemberAccessExpression>();
                oldCapRecv->span = syntaxTree.span();
                oldCapRecv->type = capturedThis->type;
                oldCapRecv->typeName = capturedThis->typeName;
                oldCapRecv->receiver = variableExpression(
                    *thisVariable, syntaxTree.span());
                oldCapRecv->ownerType = *currentOwnerType_;
                oldCapRecv->field = *capturedThis;
                auto oldAccess =
                    std::make_unique<BoundMemberAccessExpression>();
                oldAccess->span = syntaxTree.span();
                oldAccess->type = outerField->type;
                oldAccess->typeName = outerField->typeName;
                oldAccess->receiver = std::move(oldCapRecv);
                oldAccess->ownerType = outer->second;
                oldAccess->field = *outerField;
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
                result->expression = wrapResourceHandleOverwriteRelease(
                    std::move(oldAccess), std::move(value), syntaxTree.span());
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
            if (!compoundOp) {
                checkObsoleteReference(
                    property->id, "property", property->name, syntaxTree.identifierToken.span);
                recordSymbolReference(
                    occurrences_,
                    currentSourceName_,
                    property->id,
                    SymbolKind::Property,
                    property->name,
                    isExactType(property->type) && !property->typeName.empty()
                        ? property->typeName
                        : primitiveTypeName(property->type),
                    syntaxTree.identifierToken.span);
            }
            if (!property->staticProperty && (currentStaticMethod_ || !thisVariable)) {
                diagnostics_.report(
                    "RS2484",
                    "instance property '" + property->name + "' requires a value receiver",
                    syntaxTree.identifierToken.span);
                return makeError(syntaxTree.span());
            }
            if (!property->setter) {
                if (property->backingFieldIndex < currentOwnerType_->fields.size()) {
                    const auto& backingField = currentOwnerType_->fields[property->backingFieldIndex];
                    if (isReadonlyFieldWritable(
                            backingField,
                            currentConstructor_,
                            currentStaticMethod_,
                            currentOwnerType_)) {
                        auto assigned = bindAssignedValue(
                            property->type,
                            property->typeName,
                            "property assignment");
                        if (!property->staticProperty &&
                            currentOwnerType_->kind == TypeKind::Struct) {
                            auto oldAccess = std::make_unique<BoundStructFieldAccessExpression>();
                            oldAccess->span = syntaxTree.identifierToken.span;
                            oldAccess->type = backingField.type;
                            oldAccess->typeName = backingField.typeName;
                            oldAccess->receiver = makeVariableAccess(*thisVariable, syntaxTree.span());
                            oldAccess->ownerType = *currentOwnerType_;
                            oldAccess->field = backingField;
                            auto result = std::make_unique<BoundStructFieldAssignmentExpression>();
                            result->span = syntaxTree.span();
                            result->type = property->type;
                            result->typeName = property->typeName;
                            result->variable = *thisVariable;
                            result->ownerType = *currentOwnerType_;
                            result->field = backingField;
                            result->expression = wrapResourceHandleOverwriteRelease(
                                std::move(oldAccess), std::move(assigned), syntaxTree.span());
                            return result;
                        }
                        auto oldAccess = std::make_unique<BoundMemberAccessExpression>();
                        oldAccess->span = syntaxTree.identifierToken.span;
                        oldAccess->type = backingField.type;
                        oldAccess->typeName = backingField.typeName;
                        if (!property->staticProperty && thisVariable) {
                            oldAccess->receiver = makeVariableAccess(*thisVariable, syntaxTree.span());
                        }
                        oldAccess->ownerType = *currentOwnerType_;
                        oldAccess->field = backingField;
                        auto result = std::make_unique<BoundMemberAssignmentExpression>();
                        result->span = syntaxTree.span();
                        result->type = property->type;
                        result->typeName = property->typeName;
                        if (!property->staticProperty && thisVariable) {
                            result->receiver = makeVariableAccess(*thisVariable, syntaxTree.span());
                        }
                        result->ownerType = *currentOwnerType_;
                        result->field = backingField;
                        result->expression = wrapResourceHandleOverwriteRelease(
                            std::move(oldAccess), std::move(assigned), syntaxTree.span());
                        return result;
                    }
                }
                diagnostics_.report(
                    "RS2483",
                    "property '" + property->name + "' is read-only",
                    syntaxTree.identifierToken.span);
                return makeError(syntaxTree.span());
            }
            if (!property->staticProperty && currentOwnerType_->kind == TypeKind::Struct) {
                if (!currentConstructor_ ||
                    property->backingFieldIndex >= currentOwnerType_->fields.size()) {
                    diagnostics_.report(
                        "RS2486",
                        "struct properties cannot be assigned through an instance method in the Phase 3E value model",
                        syntaxTree.identifierToken.span);
                    return makeError(syntaxTree.span());
                }
                const auto& backingField = currentOwnerType_->fields[property->backingFieldIndex];
                auto assigned = bindAssignedValue(
                    property->type,
                    property->typeName, "property assignment");
                auto oldAccess = std::make_unique<BoundStructFieldAccessExpression>();
                oldAccess->span = syntaxTree.identifierToken.span;
                oldAccess->type = backingField.type;
                oldAccess->typeName = backingField.typeName;
                oldAccess->receiver = makeVariableAccess(*thisVariable, syntaxTree.span());
                oldAccess->ownerType = *currentOwnerType_;
                oldAccess->field = backingField;
                auto result = std::make_unique<BoundStructFieldAssignmentExpression>();
                result->span = syntaxTree.span();
                result->type = property->type;
                result->typeName = property->typeName;
                result->variable = *thisVariable;
                result->ownerType = *currentOwnerType_;
                result->field = backingField;
                result->expression = wrapResourceHandleOverwriteRelease(
                    std::move(oldAccess), std::move(assigned), syntaxTree.span());
                return result;
            }
            auto assigned = bindAssignedValue(
                property->type,
                property->typeName, "property assignment");
            auto result = std::make_unique<BoundPropertyAssignmentExpression>();
            result->span = syntaxTree.span();
            result->type = property->type;
            result->typeName = property->typeName;
            result->setter = *property->setter;
            if (property->staticProperty) {
                result->staticConstructor = findStaticConstructorForFunction(
                    visibleTypes_, *property->setter);
                if (!result->staticConstructor) {
                    result->staticConstructor = currentOwnerType_->staticConstructor;
                }
            }
            if (!property->staticProperty && thisVariable) {
                result->arguments.push_back(
                    makeVariableAccess(*thisVariable, syntaxTree.span()));
            }
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
    if (!function.synthetic) {
        checkObsoleteReference(
            function.id,
            function.constructor ? "constructor" : (function.method ? "method" : "function"),
            function.ownerTypeName.empty()
                ? function.name
                : (function.ownerTypeName + "." + function.name),
            span);
    }
    if (currentDeterministicContext_ && function.nonDeterministic) {
        diagnostics_.report(
            "RS2495",
            "cannot call non-deterministic function '" + function.name +
                "' from a [Deterministic] context",
            span);
    }
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
        result->staticConstructor = findStaticConstructorForFunction(
            visibleTypes_, function);
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
    result->staticConstructor = findStaticConstructorForFunction(
        visibleTypes_, function);
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
                                         text::TextSpan targetSpan,
                                         bool isImplicitReceiver) {
        if (value->kind() == BoundNodeKind::VariableExpression) {
            argument.targetKind = ReferenceTargetKind::Variable;
            argument.variable = static_cast<const
                BoundVariableExpression&>(*value).variable;
            if (argument.variable.isConst &&
                argument.modifier != ParameterModifier::In) {
                diagnostics_.report(
                    "RS2483",
                    "cannot pass const variable '" +
                        argument.variable.name + "' as ref or out",
                    targetSpan);
                argument.value = std::move(value);
                return false;
            }
            if (!isImplicitReceiver &&
                argument.variable.isUsingResource &&
                argument.modifier != ParameterModifier::In) {
                diagnostics_.report(
                    "RS2651",
                    "cannot pass '" + argument.variable.name +
                        "' as a ref or out argument because it is a 'using' variable",
                    targetSpan);
                argument.value = std::move(value);
                return false;
            }
            argument.value = std::move(value);
            return true;
        }
        if (value->kind() == BoundNodeKind::MemberAccessExpression) {
            auto& member = static_cast<
                BoundMemberAccessExpression&>(*value);
            if (!isImplicitReceiver &&
                member.field.isUsingResource &&
                argument.modifier != ParameterModifier::In) {
                diagnostics_.report(
                    "RS2651",
                    "cannot pass 'using' variable as a ref or out argument",
                    targetSpan);
                argument.value = std::move(value);
                return false;
            }
            if (member.field.constField &&
                argument.modifier != ParameterModifier::In) {
                diagnostics_.report(
                    "RS2483",
                    "cannot pass const field '" +
                        member.field.name + "' as ref or out",
                    targetSpan);
                argument.value = std::move(value);
                return false;
            }
            if (member.field.readonlyField &&
                argument.modifier != ParameterModifier::In &&
                !isReadonlyFieldWritable(
                    member.field, currentConstructor_, currentStaticMethod_, currentOwnerType_)) {
                diagnostics_.report(
                    "RS2483",
                    "cannot pass readonly field '" +
                        member.field.name + "' as ref or out",
                    targetSpan);
                argument.value = std::move(value);
                return false;
            }
            argument.targetKind = ReferenceTargetKind::ObjectField;
            argument.targetOwnerType = member.ownerType;
            argument.targetField = member.field;
            if (member.field.staticField) {
                argument.value = std::move(value);
            } else {
                argument.targetReceiver = std::move(member.receiver);
            }
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
            if ((member.field.constField || member.field.readonlyField) &&
                argument.modifier != ParameterModifier::In &&
                !isReadonlyFieldWritable(
                    member.field, currentConstructor_, currentStaticMethod_, currentOwnerType_)) {
                diagnostics_.report(
                    "RS2483",
                    std::string("cannot pass ") +
                        (member.field.constField ? "const" : "readonly") +
                        " field '" + member.field.name + "' as ref or out",
                    targetSpan);
                argument.value = std::move(value);
                return false;
            }
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
            bool readonlyReceiver = false;
            if (receiver->kind() == BoundNodeKind::MemberAccessExpression) {
                const auto& mem = static_cast<const BoundMemberAccessExpression&>(*receiver);
                readonlyReceiver = (mem.field.constField || mem.field.readonlyField) &&
                    !isReadonlyFieldWritable(
                        mem.field, currentConstructor_, currentStaticMethod_, currentOwnerType_);
            } else if (receiver->kind() == BoundNodeKind::StructFieldAccessExpression) {
                const auto& mem = static_cast<const BoundStructFieldAccessExpression&>(*receiver);
                readonlyReceiver = (mem.field.constField || mem.field.readonlyField) &&
                    !isReadonlyFieldWritable(
                        mem.field, currentConstructor_, currentStaticMethod_, currentOwnerType_);
            }
            const bool directWritebackTarget = !readonlyReceiver && (
                receiver->kind() == BoundNodeKind::VariableExpression ||
                receiver->kind() == BoundNodeKind::MemberAccessExpression ||
                receiver->kind() == BoundNodeKind::ElementAccessExpression ||
                (receiver->kind() == BoundNodeKind::StructFieldAccessExpression &&
                 static_cast<const BoundStructFieldAccessExpression&>(*receiver)
                         .receiver->kind() == BoundNodeKind::VariableExpression));
            if (!directWritebackTarget) {
                receiverArgument.targetKind = ReferenceTargetKind::None;
                receiverArgument.defensiveCopy = true;
                receiverArgument.value = std::move(receiver);
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
                }
            } else if (bindReferenceTarget(
                    receiverArgument, std::move(receiver), receiverSpan, true)) {
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
                "in argument",
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
                if (namedVariable->isConst &&
                    parameter.modifier != ParameterModifier::In) {
                    diagnostics_.report(
                        "RS2483",
                        "cannot pass const variable '" +
                            namedVariable->name + "' as ref or out",
                        argumentSpan(index));
                }
                if (namedVariable->isUsingResource &&
                    parameter.modifier != ParameterModifier::In) {
                    diagnostics_.report(
                        "RS2651",
                        "cannot pass '" + namedVariable->name +
                            "' as a ref or out argument because it is a 'using' variable",
                        argumentSpan(index));
                }
                argument.targetKind = ReferenceTargetKind::Variable;
                argument.variable = *namedVariable;
                argument.value = std::move(arguments[index]);
                boundTarget = true;
            }
        }
        if (!boundTarget && !bindReferenceTarget(
                argument, std::move(arguments[index]),
                argumentSpan(index), false)) {
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
        } else if (parameter.modifier == ParameterModifier::Out &&
                   !argument.defensiveCopy &&
                   !isInsideResourceHandleImplementation() &&
                   isManagedResourceHandleType(parameter.type, parameter.typeName)) {
            const auto rhIt = visibleTypes_.find(parameter.typeName);
            if (rhIt != visibleTypes_.end()) {
                for (const auto& m : rhIt->second.methods) {
                    if (m.staticMethod && m.name == "__AssignHandle" &&
                        m.parameters.size() == 2 &&
                        m.returnType == PrimitiveType::Struct) {
                        argument.writebackAssignHandle = m;
                        break;
                    }
                }
            }
        }
        result->arguments.push_back(std::move(argument));
    }
    return result;
}

std::unique_ptr<BoundExpression> Binder::bindCallExpression(
    const syntax::CallExpressionSyntax& syntaxTree) {
    if (syntaxTree.identifierToken.text == "__rs_stable_hash" &&
        syntaxTree.arguments.size() == 1 &&
        syntaxTree.typeArguments.empty()) {
        auto arg = bindExpression(*syntaxTree.arguments.front());
        const auto argType = arg->type;
        const auto argTypeName = arg->typeName;
        const std::string typeTag = argTypeName.empty()
            ? std::string(primitiveTypeName(argType))
            : argTypeName;
        auto call = std::make_unique<BoundCallExpression>();
        call->span = syntaxTree.span();
        call->type = PrimitiveType::Int;
        call->function = makeIntrinsicFunction(
            "$intrinsic::stable_hash::" + typeTag,
            PrimitiveType::Int,
            "",
            {{argType, argTypeName}});
        call->arguments.push_back(std::move(arg));
        return call;
    }
    if (syntaxTree.identifierToken.text == "__rs_array_sort_range" &&
        syntaxTree.arguments.size() == 2 &&
        syntaxTree.typeArguments.empty()) {
        auto arrArg = bindExpression(*syntaxTree.arguments[0]);
        auto countArg = convertExpression(
            bindExpression(*syntaxTree.arguments[1]),
            PrimitiveType::Int,
            syntaxTree.arguments[1]->span(),
            "sort count");
        const auto arrTypeName = arrArg->typeName;
        auto call = std::make_unique<BoundCallExpression>();
        call->span = syntaxTree.span();
        call->type = PrimitiveType::Void;
        call->function = makeIntrinsicFunction(
            "$intrinsic::array_sort_range::" + arrTypeName,
            PrimitiveType::Void,
            "",
            {{PrimitiveType::Array, arrTypeName}, {PrimitiveType::Int, ""}});
        call->arguments.push_back(std::move(arrArg));
        call->arguments.push_back(std::move(countArg));
        return call;
    }
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
    if (!calledDelegateType && currentOwnerType_) {
        const auto* field = findField(
            *currentOwnerType_, syntaxTree.identifierToken.text);
        if (field && (!currentStaticMethod_ || field->staticField) &&
            field->type == PrimitiveType::Object &&
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
            if (variable->isConst &&
                parameter.modifier != ParameterModifier::In) {
                diagnostics_.report(
                    "RS2483",
                    "cannot pass const variable '" +
                        variable->name + "' as ref or out",
                    syntaxTree.arguments[index]->span());
            }
            if (variable->isUsingResource &&
                parameter.modifier != ParameterModifier::In) {
                diagnostics_.report(
                    "RS2651",
                    "cannot pass '" + variable->name +
                        "' as a ref or out argument because it is a 'using' variable",
                    syntaxTree.arguments[index]->span());
            }
            if (variable->modifier == ParameterModifier::In &&
                parameter.modifier != ParameterModifier::In) {
                diagnostics_.report(
                    "RS8703",
                    "an in parameter cannot be forwarded as ref or out",
                    syntaxTree.arguments[index]->span());
            }
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
            if ((variable->modifier == ParameterModifier::Ref ||
                 variable->modifier == ParameterModifier::Out) &&
                storageTypeOf(*variable) == PrimitiveType::Object &&
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
            } else if (parameter.modifier == ParameterModifier::Out) {
                (void)bindTargetExpression(
                    *syntaxTree.arguments[index],
                    parameter.type,
                    parameter.typeName,
                    "delegate out argument");
                if (!isInsideResourceHandleImplementation() &&
                    isManagedResourceHandleType(parameter.type, parameter.typeName)) {
                    const auto rhIt = visibleTypes_.find(parameter.typeName);
                    if (rhIt != visibleTypes_.end()) {
                        for (const auto& m : rhIt->second.methods) {
                            if (m.staticMethod && m.name == "__AssignHandle" &&
                                m.parameters.size() == 2 &&
                                m.returnType == PrimitiveType::Struct) {
                                argument.writebackAssignHandle = m;
                                break;
                            }
                        }
                    }
                }
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
    if (currentOwnerType_) {
        if (!currentStaticMethod_) {
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
        for (const auto* method : findMethods(
                 *currentOwnerType_,
                 syntaxTree.identifierToken.text,
                 true)) {
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
    if (currentOwnerType_) {
        if (!currentStaticMethod_) {
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
        auto staticMethods = findMethods(
            *currentOwnerType_,
            syntaxTree.identifierToken.text,
            true);
        for (const auto* method : staticMethods) {
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

    recordSymbolReference(
        occurrences_,
        currentSourceName_,
        best->id,
        SymbolKind::Function,
        best->name,
        syntaxTree.identifierToken.span);

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

std::unique_ptr<BoundExpression> Binder::tryBindExtensionMethodCall(
    const syntax::MemberCallExpressionSyntax& syntaxTree,
    std::unique_ptr<BoundExpression>& receiver,
    std::vector<std::unique_ptr<BoundExpression>>& arguments) {
    if (!receiver) return nullptr;
    std::vector<const FunctionSymbol*> extCandidates;
    const auto globals = visibleFunctions_.find(syntaxTree.nameToken.text);
    if (globals != visibleFunctions_.end()) {
        for (const auto& fn : globals->second) {
            if (!fn.method && fn.extensionMethod && !fn.parameters.empty() &&
                isMemberAccessible(
                    fn.accessibility, fn.declaringTypeId, fn.moduleName)) {
                extCandidates.push_back(&fn);
            }
        }
    }
    for (const auto& [visName, visType] : visibleTypes_) {
        (void)visName;
        for (const auto& m : visType.methods) {
            if (m.staticMethod && m.extensionMethod &&
                m.name == syntaxTree.nameToken.text && !m.parameters.empty() &&
                isMemberAccessible(
                    m.accessibility, m.declaringTypeId, m.moduleName)) {
                extCandidates.push_back(&m);
            }
        }
    }
    if (extCandidates.empty()) return nullptr;

    std::vector<std::unique_ptr<BoundExpression>> extArgs;
    extArgs.reserve(arguments.size() + 1);
    extArgs.push_back(std::move(receiver));
    for (auto& arg : arguments) extArgs.push_back(std::move(arg));

    std::vector<std::optional<syntax::SyntaxToken>> extNames;
    if (!syntaxTree.argumentNames.empty()) {
        extNames.push_back(std::nullopt);
        for (const auto& n : syntaxTree.argumentNames) extNames.push_back(n);
    }
    std::vector<std::optional<syntax::SyntaxToken>> extMods;
    if (!syntaxTree.argumentModifiers.empty()) {
        extMods.push_back(std::nullopt);
        for (const auto& m : syntaxTree.argumentModifiers) extMods.push_back(m);
    }

    bool extAmbiguous = false;
    const auto extPlan = selectFlexibleCall(
        extCandidates,
        extArgs,
        extNames.empty() ? nullptr : &extNames,
        extMods.empty() ? nullptr : &extMods,
        visibleTypes_,
        extAmbiguous);
    if (!extPlan.function || extAmbiguous) {
        receiver = std::move(extArgs.front());
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            arguments[i] = std::move(extArgs[i + 1]);
        }
        return nullptr;
    }

    recordSymbolReference(
        occurrences_,
        currentSourceName_,
        extPlan.function->id,
        SymbolKind::Function,
        extPlan.function->name,
        syntaxTree.nameToken.span);
    if (extMods.empty()) {
        extMods.resize(syntaxTree.arguments.size() + 1);
    }
    if (!extPlan.function->parameters.empty()) {
        const auto recvMod = extPlan.function->parameters.front().modifier;
        if (recvMod == ParameterModifier::Ref) {
            extMods[0] = syntax::SyntaxToken{
                syntax::SyntaxKind::RefKeyword,
                syntaxTree.receiver->span(),
                "ref",
                {},
                true};
        } else if (recvMod == ParameterModifier::In) {
            extMods[0] = syntax::SyntaxToken{
                syntax::SyntaxKind::InKeyword,
                syntaxTree.receiver->span(),
                "in",
                {},
                true};
        }
    }
    std::vector<std::unique_ptr<syntax::ExpressionSyntax>> extSyntaxArgs;
    extSyntaxArgs.reserve(syntaxTree.arguments.size() + 1);
    extSyntaxArgs.push_back(syntax::cloneExpressionSyntax(*syntaxTree.receiver));
    for (const auto& a : syntaxTree.arguments) {
        extSyntaxArgs.push_back(a ? syntax::cloneExpressionSyntax(*a) : nullptr);
    }
    auto prepared = prepareCallArguments(
        *extPlan.function,
        extPlan.sources,
        std::move(extArgs),
        extSyntaxArgs,
        extMods,
        syntaxTree.span(),
        "extension method argument");
    return applyNullConditional(
        bindSelectedCall(
            *extPlan.function,
            std::move(prepared.arguments),
            prepared.syntaxArguments,
            prepared.modifiers,
            std::move(prepared.evaluationOrder),
            nullptr,
            syntaxTree.span(),
            "extension method argument",
            false),
        syntaxTree.dotToken,
        syntaxTree.span());
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
            checkObsoleteReference(
                type->second.id,
                "type",
                canonicalTypeName(type->second),
                name.identifierToken.span);
            recordSymbolReference(
                occurrences_,
                currentSourceName_,
                type->second.id,
                SymbolKind::Type,
                type->second.name,
                name.identifierToken.span);
            owner = type->second;
            staticCall = true;
        }
    }
    if (!staticCall) {
        receiver = bindExpression(*syntaxTree.receiver);
        if (receiver->type == PrimitiveType::Array) {
            const auto& methodName = syntaxTree.nameToken.text;
            if ((methodName == "Length" || methodName == "Count") &&
                arguments.empty()) {
                auto result = std::make_unique<BoundArrayLengthExpression>();
                result->span = syntaxTree.span();
                result->type = PrimitiveType::Int;
                result->receiver = std::move(receiver);
                return applyNullConditional(
                    std::move(result), syntaxTree.dotToken, syntaxTree.span());
            }
            PrimitiveType elemType = PrimitiveType::Error;
            std::string elemTypeName;
            if (decodeArrayTypeName(receiver->typeName, elemType, elemTypeName)) {
                if (!elemTypeName.empty()) {
                    const auto foundElem = visibleTypes_.find(elemTypeName);
                    if (foundElem != visibleTypes_.end()) {
                        elemType = foundElem->second.kind == TypeKind::Class
                            ? PrimitiveType::Object
                            : (foundElem->second.kind == TypeKind::Struct
                                ? PrimitiveType::Struct
                                : PrimitiveType::Enum);
                    }
                }
                std::string intrinsicName;
                PrimitiveType returnType = PrimitiveType::Error;
                std::vector<std::pair<PrimitiveType, std::string>> expectedParams;
                if (methodName == "IndexOf" && arguments.size() == 1) {
                    intrinsicName = "$intrinsic::array_index_of::" + receiver->typeName;
                    returnType = PrimitiveType::Int;
                    expectedParams = {{elemType, elemTypeName}};
                } else if (methodName == "LastIndexOf" && arguments.size() == 1) {
                    intrinsicName = "$intrinsic::array_last_index_of::" + receiver->typeName;
                    returnType = PrimitiveType::Int;
                    expectedParams = {{elemType, elemTypeName}};
                } else if (methodName == "Contains" && arguments.size() == 1) {
                    intrinsicName = "$intrinsic::array_contains::" + receiver->typeName;
                    returnType = PrimitiveType::Bool;
                    expectedParams = {{elemType, elemTypeName}};
                } else if (methodName == "Reverse" && arguments.empty()) {
                    intrinsicName = "$intrinsic::array_reverse::" + receiver->typeName;
                    returnType = PrimitiveType::Void;
                } else if (methodName == "Sort" && arguments.empty()) {
                    intrinsicName = "$intrinsic::array_sort::" + receiver->typeName;
                    returnType = PrimitiveType::Void;
                }
                if (!intrinsicName.empty()) {
                    std::vector<std::pair<PrimitiveType, std::string>> fullParams;
                    fullParams.push_back({PrimitiveType::Array, receiver->typeName});
                    for (const auto& p : expectedParams) fullParams.push_back(p);
                    auto call = std::make_unique<BoundCallExpression>();
                    call->span = syntaxTree.span();
                    call->type = returnType;
                    call->function = makeIntrinsicFunction(
                        intrinsicName, returnType, "", fullParams);
                    call->function.method = true;
                    call->function.staticMethod = false;
                    call->arguments.push_back(std::move(receiver));
                    for (std::size_t i = 0; i < arguments.size(); ++i) {
                        call->arguments.push_back(convertExpression(
                            std::move(arguments[i]),
                            expectedParams[i].first,
                            syntaxTree.arguments[i]->span(),
                            "array method argument",
                            expectedParams[i].second));
                    }
                    return applyNullConditional(
                        std::move(call), syntaxTree.dotToken, syntaxTree.span());
                }
            }
        }
        if (receiver->type == PrimitiveType::String) {
            const auto& methodName = syntaxTree.nameToken.text;
            std::string intrinsicName;
            PrimitiveType returnType = PrimitiveType::Error;
            std::string returnTypeName;
            std::vector<std::pair<PrimitiveType, std::string>> expectedParams;
            if ((methodName == "Length" || methodName == "Count") && arguments.empty()) {
                intrinsicName = "$intrinsic::string_length";
                returnType = PrimitiveType::Int;
            } else if (methodName == "IsEmpty" && arguments.empty()) {
                intrinsicName = "$intrinsic::string_is_empty";
                returnType = PrimitiveType::Bool;
            } else if (methodName == "Contains" && arguments.size() == 1) {
                intrinsicName = "$intrinsic::string_contains";
                returnType = PrimitiveType::Bool;
                expectedParams = {{PrimitiveType::String, ""}};
            } else if (methodName == "StartsWith" && arguments.size() == 1) {
                intrinsicName = "$intrinsic::string_starts_with";
                returnType = PrimitiveType::Bool;
                expectedParams = {{PrimitiveType::String, ""}};
            } else if (methodName == "EndsWith" && arguments.size() == 1) {
                intrinsicName = "$intrinsic::string_ends_with";
                returnType = PrimitiveType::Bool;
                expectedParams = {{PrimitiveType::String, ""}};
            } else if (methodName == "IndexOf" && arguments.size() == 1) {
                intrinsicName = "$intrinsic::string_index_of";
                returnType = PrimitiveType::Int;
                expectedParams = {{PrimitiveType::String, ""}};
            } else if (methodName == "LastIndexOf" && arguments.size() == 1) {
                intrinsicName = "$intrinsic::string_last_index_of";
                returnType = PrimitiveType::Int;
                expectedParams = {{PrimitiveType::String, ""}};
            } else if (methodName == "Substring" && arguments.size() == 1) {
                intrinsicName = "$intrinsic::string_substring_1";
                returnType = PrimitiveType::String;
                expectedParams = {{PrimitiveType::Int, ""}};
            } else if (methodName == "Substring" && arguments.size() == 2) {
                intrinsicName = "$intrinsic::string_substring_2";
                returnType = PrimitiveType::String;
                expectedParams = {{PrimitiveType::Int, ""}, {PrimitiveType::Int, ""}};
            } else if (methodName == "Trim" && arguments.empty()) {
                intrinsicName = "$intrinsic::string_trim";
                returnType = PrimitiveType::String;
            } else if (methodName == "TrimStart" && arguments.empty()) {
                intrinsicName = "$intrinsic::string_trim_start";
                returnType = PrimitiveType::String;
            } else if (methodName == "TrimEnd" && arguments.empty()) {
                intrinsicName = "$intrinsic::string_trim_end";
                returnType = PrimitiveType::String;
            } else if (methodName == "Replace" && arguments.size() == 2) {
                intrinsicName = "$intrinsic::string_replace";
                returnType = PrimitiveType::String;
                expectedParams = {{PrimitiveType::String, ""}, {PrimitiveType::String, ""}};
            } else if (methodName == "ToUpper" && arguments.empty()) {
                intrinsicName = "$intrinsic::string_to_upper";
                returnType = PrimitiveType::String;
            } else if (methodName == "ToLower" && arguments.empty()) {
                intrinsicName = "$intrinsic::string_to_lower";
                returnType = PrimitiveType::String;
            } else if (methodName == "Split" && arguments.size() == 1) {
                intrinsicName = "$intrinsic::string_split";
                returnType = PrimitiveType::Array;
                returnTypeName = arrayTypeName(PrimitiveType::String, "");
                expectedParams = {{PrimitiveType::String, ""}};
            }
            if (!intrinsicName.empty()) {
                std::vector<std::pair<PrimitiveType, std::string>> fullParams;
                fullParams.push_back({PrimitiveType::String, ""});
                for (const auto& p : expectedParams) fullParams.push_back(p);
                auto call = std::make_unique<BoundCallExpression>();
                call->span = syntaxTree.span();
                call->type = returnType;
                call->typeName = returnTypeName;
                call->function = makeIntrinsicFunction(
                    intrinsicName, returnType, returnTypeName, fullParams);
                call->function.method = true;
                call->function.staticMethod = false;
                call->arguments.push_back(std::move(receiver));
                for (std::size_t i = 0; i < arguments.size(); ++i) {
                    call->arguments.push_back(convertExpression(
                        std::move(arguments[i]),
                        expectedParams[i].first,
                        syntaxTree.arguments[i]->span(),
                        "string method argument",
                        expectedParams[i].second));
                }
                return applyNullConditional(
                    std::move(call), syntaxTree.dotToken, syntaxTree.span());
            }
            if (auto extCall = tryBindExtensionMethodCall(syntaxTree, receiver, arguments)) {
                return extCall;
            }
            diagnostics_.report(
                "RS2472",
                "no applicable method '" + methodName + "' on type 'string'",
                syntaxTree.span());
            return makeError(syntaxTree.span());
        }
        if ((receiver->type != PrimitiveType::Object &&
             receiver->type != PrimitiveType::Struct) ||
            receiver->typeName.empty()) {
            if (auto extCall = tryBindExtensionMethodCall(syntaxTree, receiver, arguments)) {
                return extCall;
            }
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
    if (!best && !staticCall && receiver) {
        if (auto extCall = tryBindExtensionMethodCall(syntaxTree, receiver, arguments)) {
            return extCall;
        }
    }
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

    recordSymbolReference(
        occurrences_,
        currentSourceName_,
        best->id,
        SymbolKind::Function,
        best->name,
        syntaxTree.nameToken.span);

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
    checkObsoleteReference(
        found->second.id,
        "type",
        canonicalTypeName(found->second),
        syntaxTree.type.span());
    recordSymbolReference(
        occurrences_,
        currentSourceName_,
        found->second.id,
        SymbolKind::Type,
        found->second.name,
        syntaxTree.type.name.span);
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
    if (found->second.kind == TypeKind::Class && found->second.staticType) {
        diagnostics_.report(
            "RS2488",
            "cannot instantiate static class '" +
                canonicalTypeName(found->second) + "'",
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
        checkObsoleteReference(
            best->id,
            "constructor",
            canonicalTypeName(found->second) + "." + best->name,
            syntaxTree.span());
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
                    if (field->constField || field->readonlyField) {
                        diagnostics_.report(
                            "RS2483",
                            std::string("cannot assign to ") +
                                (field->constField ? "const" : "readonly") +
                                " field '" + field->name + "' in object initializer",
                            syntaxInitializer.nameToken->span);
                        continue;
                    }
                    if (field->staticField) {
                        diagnostics_.report(
                            "RS2484",
                            "cannot initialize static field '" + field->name +
                                "' in object initializer",
                            syntaxInitializer.nameToken->span);
                        continue;
                    }
                    checkObsoleteReference(
                        field->id, "field", field->name, syntaxInitializer.nameToken->span);
                    recordSymbolReference(
                        occurrences_,
                        currentSourceName_,
                        field->id,
                        SymbolKind::Field,
                        field->name,
                        syntaxInitializer.nameToken->span);
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
                    if (property->staticProperty) {
                        diagnostics_.report(
                            "RS2484",
                            "cannot initialize static property '" + property->name +
                                "' in object initializer",
                            syntaxInitializer.nameToken->span);
                        continue;
                    }
                    if (!property->setter || !isMemberAccessible(
                            property->accessibility,
                            property->declaringTypeId)) {
                        diagnostics_.report(
                            "RS8912", "property '" + name +
                                "' is not settable here",
                            syntaxInitializer.span());
                        continue;
                    }
                    recordSymbolReference(
                        occurrences_,
                        currentSourceName_,
                        property->id,
                        SymbolKind::Property,
                        property->name,
                        syntaxInitializer.nameToken->span);
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
        if (!best) {
            for (const auto& field : found->second.fields) {
                if (field.staticField || field.constField ||
                    field.hasConstantValue || !field.initializerSyntax) {
                    continue;
                }
                auto initExpr = bindStaticFieldInitializer(found->second, field);
                if (!initExpr) continue;
                BoundNewObjectExpression::Initializer defaultInit;
                defaultInit.kind = BoundNewObjectExpression::Initializer::Kind::Field;
                defaultInit.field = field;
                defaultInit.arguments.push_back(std::move(initExpr));
                result->defaultFieldInitializers.push_back(std::move(defaultInit));
            }
        }
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
    if (!best) {
        for (const auto& field : found->second.fields) {
            if (field.staticField || field.constField ||
                field.hasConstantValue || !field.initializerSyntax) {
                continue;
            }
            auto initExpr = bindStaticFieldInitializer(found->second, field);
            if (!initExpr) continue;
            BoundNewObjectExpression::Initializer defaultInit;
            defaultInit.kind = BoundNewObjectExpression::Initializer::Kind::Field;
            defaultInit.field = field;
            defaultInit.arguments.push_back(std::move(initExpr));
            result->defaultFieldInitializers.push_back(std::move(defaultInit));
        }
    }
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
    if (syntaxTree.explicitLength && !syntaxTree.initialValues.empty()) {
        syntax::TokenValue constLen;
        PrimitiveType constLenType = PrimitiveType::Error;
        const bool isConstInt = evaluateConstantSyntaxExpression(
            *syntaxTree.length,
            currentOwnerType_ ? &*currentOwnerType_ : nullptr,
            visibleTypes_,
            constLen,
            constLenType,
            &scopes_) &&
            std::holds_alternative<std::int64_t>(constLen);
        if (!isConstInt ||
            std::get<std::int64_t>(constLen) !=
                static_cast<std::int64_t>(syntaxTree.initialValues.size())) {
            diagnostics_.report(
                "RS2421",
                "array initializer length does not match explicit array size",
                syntaxTree.length->span());
        }
    }
    auto length = convertExpression(bindExpression(*syntaxTree.length), PrimitiveType::Int, syntaxTree.length->span(), "array length");
    auto result = std::make_unique<BoundNewArrayExpression>();
    result->span = syntaxTree.span();
    result->type = PrimitiveType::Array;
    result->typeName = arrayTypeName(elementType, elementTypeName);
    result->elementType = elementType;
    result->elementTypeName = elementTypeName;
    result->length = std::move(length);
    for (const auto& itemSyntax : syntaxTree.initialValues) {
        if (!itemSyntax) continue;
        result->initialValues.push_back(bindTargetExpression(
            *itemSyntax,
            elementType,
            elementTypeName,
            "array initializer"));
    }
    return result;
}

std::unique_ptr<BoundExpression> Binder::bindMemberAccessExpression(
    const syntax::MemberAccessExpressionSyntax& syntaxTree) {
    // Static enum member, static/const field, or static property.
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
            recordSymbolReference(
                occurrences_,
                currentSourceName_,
                type->second.id,
                SymbolKind::Type,
                type->second.name,
                name.identifierToken.span);
            if (syntaxTree.nameToken.missing) {
                return makeError(syntaxTree.span());
            }
            if (type->second.kind == TypeKind::Enum) {
                const auto* member = findEnumMember(type->second, syntaxTree.nameToken.text);
                if (!member) {
                    diagnostics_.report("RS2480", "enum '" + canonicalTypeName(type->second) + "' has no member '" + syntaxTree.nameToken.text + "'", syntaxTree.nameToken.span);
                    return makeError(syntaxTree.span());
                }
                checkObsoleteReference(
                    type->second.id, "type", canonicalTypeName(type->second), name.identifierToken.span);
                checkObsoleteReference(
                    member->id, "enum member", member->name, syntaxTree.nameToken.span);
                recordSymbolReference(
                    occurrences_,
                    currentSourceName_,
                    member->id,
                    SymbolKind::EnumMember,
                    member->name,
                    syntaxTree.nameToken.span);
                auto result = std::make_unique<BoundLiteralExpression>();
                result->span = syntaxTree.span();
                result->type = PrimitiveType::Enum;
                result->typeName = canonicalTypeName(type->second);
                result->value = member->value;
                return result;
            }
            if (const auto* field = findField(
                    type->second,
                    syntaxTree.nameToken.text)) {
                if (!isMemberAccessible(
                        field->accessibility,
                        field->declaringTypeId)) {
                    diagnostics_.report(
                        "RS2534",
                        "field '" + field->name + "' is inaccessible",
                        syntaxTree.span());
                    return makeError(syntaxTree.span());
                }
                checkObsoleteReference(
                    type->second.id, "type", canonicalTypeName(type->second), name.identifierToken.span);
                checkObsoleteReference(
                    field->id, "field", field->name, syntaxTree.nameToken.span);
                recordSymbolReference(
                    occurrences_,
                    currentSourceName_,
                    field->id,
                    SymbolKind::Field,
                    field->name,
                    syntaxTree.nameToken.span);
                if (!field->staticField && !field->constField) {
                    diagnostics_.report(
                        "RS2484",
                        "instance field '" + field->name +
                            "' requires a value receiver",
                        syntaxTree.nameToken.span);
                    return makeError(syntaxTree.span());
                }
                if (field->constField && field->hasConstantValue) {
                    auto literal = std::make_unique<BoundLiteralExpression>();
                    literal->span = syntaxTree.span();
                    literal->type = field->type;
                    literal->typeName = field->typeName;
                    literal->value = field->constantValue;
                    return literal;
                }
                auto result = std::make_unique<BoundMemberAccessExpression>();
                result->span = syntaxTree.span();
                result->type = field->type;
                result->typeName = field->typeName;
                result->ownerType = type->second;
                result->field = *field;
                if (field->staticField &&
                    !type->second.staticConstructor.has_value() &&
                    !field->hasConstantValue && field->initializerSyntax) {
                    result->staticInitializer = bindStaticFieldInitializer(
                        type->second, *field);
                }
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
                checkObsoleteReference(
                    type->second.id, "type", canonicalTypeName(type->second), name.identifierToken.span);
                checkObsoleteReference(
                    property->id, "property", property->name, syntaxTree.nameToken.span);
                recordSymbolReference(
                    occurrences_,
                    currentSourceName_,
                    property->id,
                    SymbolKind::Property,
                    property->name,
                    syntaxTree.nameToken.span);
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
                result->staticConstructor = findStaticConstructorForFunction(
                    visibleTypes_, *property->getter);
                if (!result->staticConstructor) {
                    result->staticConstructor = type->second.staticConstructor;
                }
                return result;
            }
        }
    }

    auto receiver = bindExpression(*syntaxTree.receiver);
    if (syntaxTree.nameToken.missing) {
        return makeError(syntaxTree.span());
    }
    if (receiver->type == PrimitiveType::Array &&
        (syntaxTree.nameToken.text == "length" ||
         syntaxTree.nameToken.text == "Length" ||
         syntaxTree.nameToken.text == "Count")) {
        auto result = std::make_unique<BoundArrayLengthExpression>();
        result->span = syntaxTree.span();
        result->type = PrimitiveType::Int;
        result->receiver = std::move(receiver);
        return applyNullConditional(
            std::move(result), syntaxTree.dotToken, syntaxTree.span());
    }
    if (receiver->type == PrimitiveType::String &&
        (syntaxTree.nameToken.text == "length" ||
         syntaxTree.nameToken.text == "Length" ||
         syntaxTree.nameToken.text == "Count")) {
        auto call = std::make_unique<BoundCallExpression>();
        call->span = syntaxTree.span();
        call->type = PrimitiveType::Int;
        call->function = makeIntrinsicFunction(
            "$intrinsic::string_length",
            PrimitiveType::Int,
            "",
            {{PrimitiveType::String, ""}});
        call->function.method = true;
        call->function.staticMethod = false;
        call->arguments.push_back(std::move(receiver));
        return applyNullConditional(
            std::move(call), syntaxTree.dotToken, syntaxTree.span());
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
        checkObsoleteReference(
            field->id, "field", field->name, syntaxTree.nameToken.span);
        recordSymbolReference(
            occurrences_,
            currentSourceName_,
            field->id,
            SymbolKind::Field,
            field->name,
            syntaxTree.nameToken.span);
        if (field->constField && field->hasConstantValue) {
            auto literal = std::make_unique<BoundLiteralExpression>();
            literal->span = syntaxTree.span();
            literal->type = field->type;
            literal->typeName = field->typeName;
            literal->value = field->constantValue;
            return literal;
        }
        if (field->staticField) {
            auto result = std::make_unique<BoundMemberAccessExpression>();
            result->span = syntaxTree.span();
            result->type = field->type;
            result->typeName = field->typeName;
            result->ownerType = type->second;
            result->field = *field;
            if (!type->second.staticConstructor.has_value() &&
                !field->hasConstantValue && field->initializerSyntax) {
                result->staticInitializer = bindStaticFieldInitializer(
                    type->second, *field);
            }
            return result;
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
        checkObsoleteReference(
            property->id, "property", property->name, syntaxTree.nameToken.span);
        recordSymbolReference(
            occurrences_,
            currentSourceName_,
            property->id,
            SymbolKind::Property,
            property->name,
            syntaxTree.nameToken.span);
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
    if (syntaxTree.nameToken.text == "Count" ||
        syntaxTree.nameToken.text == "Length" ||
        syntaxTree.nameToken.text == "Capacity" ||
        syntaxTree.nameToken.text == "IsEmpty" ||
        syntaxTree.nameToken.text == "UseCount" ||
        syntaxTree.nameToken.text == "IsDisposed" ||
        syntaxTree.nameToken.text == "IsValid" ||
        syntaxTree.nameToken.text == "ResourceId") {
        auto getterMethods = findMethods(type->second, syntaxTree.nameToken.text, false);
        if (getterMethods.empty() && syntaxTree.nameToken.text == "Length") {
            getterMethods = findMethods(type->second, "Count", false);
        }
        for (const auto* method : getterMethods) {
            if (method->parameters.size() == visibleParameterOffset(*method) &&
                isMemberAccessible(method->accessibility, method->declaringTypeId, method->moduleName)) {
                return applyNullConditional(
                    bindSelectedCall(
                        *method,
                        {},
                        {},
                        {},
                        {},
                        std::move(receiver),
                        syntaxTree.span(),
                        "property getter"),
                    syntaxTree.dotToken, syntaxTree.span());
            }
        }
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
    if ((receiver->type == PrimitiveType::Object || receiver->type == PrimitiveType::Struct) &&
        !receiver->typeName.empty()) {
        const auto ownerIt = visibleTypes_.find(receiver->typeName);
        if (ownerIt != visibleTypes_.end()) {
            std::vector<std::unique_ptr<BoundExpression>> args;
            args.push_back(bindExpression(*syntaxTree.index));
            auto methods = findMethods(ownerIt->second, "Get", false);
            bool ambiguous = false;
            const auto plan = selectFlexibleCall(
                methods, args, nullptr, nullptr, visibleTypes_, ambiguous);
            if (plan.function && !ambiguous) {
                std::vector<std::unique_ptr<syntax::ExpressionSyntax>> synthArgs;
                auto dn0 = std::make_unique<syntax::NameExpressionSyntax>();
                dn0->identifierToken.span = syntaxTree.index->span();
                synthArgs.push_back(std::move(dn0));
                std::vector<std::optional<syntax::SyntaxToken>> synthMods;
                auto prepared = prepareCallArguments(
                    *plan.function, plan.sources, std::move(args),
                    synthArgs, synthMods, syntaxTree.span(), "indexer argument");
                return bindSelectedCall(
                    *plan.function,
                    std::move(prepared.arguments),
                    prepared.syntaxArguments,
                    prepared.modifiers,
                    std::move(prepared.evaluationOrder),
                    std::move(receiver),
                    syntaxTree.span(),
                    "indexer argument",
                    false);
            }
        }
    }
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
    const auto compoundOp = compoundBinaryOperatorToken(syntaxTree.equalsToken);
    const auto bindReceiverAccess = [&]() -> std::unique_ptr<BoundExpression> {
        if (syntaxTree.receiver->kind() == syntax::SyntaxKind::ThisExpression &&
            currentOwnerType_ && currentOwnerType_->kind == TypeKind::Struct) {
            const auto* self = lookupVariable("this");
            if (self) {
                return variableExpression(*self, syntaxTree.receiver->span());
            }
            return makeError(syntaxTree.receiver->span());
        }
        return bindExpression(*syntaxTree.receiver);
    };

    // Static field or static property assignment.
    if (syntaxTree.receiver->kind() == syntax::SyntaxKind::NameExpression) {
        const auto& name = static_cast<const syntax::NameExpressionSyntax&>(*syntaxTree.receiver);
        const auto type = visibleTypes_.find(name.identifierToken.text);
        if (!lookupVariable(name.identifierToken.text) &&
            type != visibleTypes_.end()) {
            recordSymbolReference(
                occurrences_,
                currentSourceName_,
                type->second.id,
                SymbolKind::Type,
                type->second.name,
                name.identifierToken.span);
            if (!isTypeAccessible(type->second)) {
                diagnostics_.report(
                    "RS2534",
                    "type '" + canonicalTypeName(type->second) +
                        "' is inaccessible",
                    name.identifierToken.span);
                return makeError(syntaxTree.span());
            }
            if (const auto* field = findField(
                    type->second,
                    syntaxTree.nameToken.text)) {
                if (!isMemberAccessible(
                        field->accessibility,
                        field->declaringTypeId)) {
                    diagnostics_.report(
                        "RS2534",
                        "field '" + field->name + "' is inaccessible",
                        syntaxTree.span());
                    return makeError(syntaxTree.span());
                }
                checkObsoleteReference(
                    type->second.id, "type", canonicalTypeName(type->second), name.identifierToken.span);
                checkObsoleteReference(
                    field->id, "field", field->name, syntaxTree.nameToken.span);
                recordSymbolReference(
                    occurrences_,
                    currentSourceName_,
                    field->id,
                    SymbolKind::Field,
                    field->name,
                    syntaxTree.nameToken.span);
                if (field->constField) {
                    diagnostics_.report(
                        "RS2483",
                        "cannot assign to const field '" + field->name + "'",
                        syntaxTree.nameToken.span);
                    return makeError(syntaxTree.span());
                }
                if (!field->staticField) {
                    diagnostics_.report(
                        "RS2484",
                        "instance field '" + field->name +
                            "' requires a value receiver",
                        syntaxTree.nameToken.span);
                    return makeError(syntaxTree.span());
                }
                if (field->readonlyField &&
                    !isReadonlyFieldWritable(
                        *field, currentConstructor_, currentStaticMethod_, currentOwnerType_)) {
                    diagnostics_.report(
                        "RS2483",
                        "cannot assign to readonly field '" + field->name + "'",
                        syntaxTree.nameToken.span);
                    return makeError(syntaxTree.span());
                }
                std::unique_ptr<BoundExpression> value;
                if (compoundOp) {
                    auto lhsValue = std::make_unique<BoundMemberAccessExpression>();
                    lhsValue->span = syntaxTree.nameToken.span;
                    lhsValue->type = field->type;
                    lhsValue->typeName = field->typeName;
                    lhsValue->ownerType = type->second;
                    lhsValue->field = *field;
                    if (!type->second.staticConstructor.has_value() &&
                        !field->hasConstantValue && field->initializerSyntax) {
                        lhsValue->staticInitializer = bindStaticFieldInitializer(
                            type->second, *field);
                    }
                    auto rhsValue = bindExpression(*syntaxTree.expression);
                    auto combined = bindBinaryOperation(
                        std::move(lhsValue),
                        syntaxTree.nameToken.span,
                        *compoundOp,
                        std::move(rhsValue),
                        syntaxTree.expression->span(),
                        syntaxTree.span());
                    value = convertExpression(
                        std::move(combined),
                        field->type,
                        syntaxTree.span(),
                        "field assignment",
                        field->typeName);
                } else {
                    value = bindTargetExpression(
                        *syntaxTree.expression,
                        field->type,
                        field->typeName,
                        "field assignment");
                }
                auto oldAccess = std::make_unique<BoundMemberAccessExpression>();
                oldAccess->span = syntaxTree.nameToken.span;
                oldAccess->type = field->type;
                oldAccess->typeName = field->typeName;
                oldAccess->ownerType = type->second;
                oldAccess->field = *field;
                auto result = std::make_unique<BoundMemberAssignmentExpression>();
                result->span = syntaxTree.span();
                result->type = field->type;
                result->typeName = field->typeName;
                result->ownerType = type->second;
                result->field = *field;
                result->expression = wrapResourceHandleOverwriteRelease(
                    std::move(oldAccess), std::move(value), syntaxTree.span());
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
                checkObsoleteReference(
                    type->second.id, "type", canonicalTypeName(type->second), name.identifierToken.span);
                checkObsoleteReference(
                    property->id, "property", property->name, syntaxTree.nameToken.span);
                recordSymbolReference(
                    occurrences_,
                    currentSourceName_,
                    property->id,
                    SymbolKind::Property,
                    property->name,
                    syntaxTree.nameToken.span);
                if (!property->staticProperty) {
                    diagnostics_.report(
                        "RS2484",
                        "instance property '" + property->name +
                            "' requires a value receiver",
                        syntaxTree.nameToken.span);
                    return makeError(syntaxTree.span());
                }
                const FieldSymbol* writableBackingField = nullptr;
                if (!property->setter) {
                    if (property->backingFieldIndex < type->second.fields.size()) {
                        const auto& candidate = type->second.fields[property->backingFieldIndex];
                        if (isReadonlyFieldWritable(
                                candidate,
                                currentConstructor_,
                                currentStaticMethod_,
                                currentOwnerType_)) {
                            writableBackingField = &candidate;
                        }
                    }
                    if (!writableBackingField) {
                        diagnostics_.report(
                            "RS2483",
                            "property '" + property->name + "' is read-only",
                            syntaxTree.nameToken.span);
                        return makeError(syntaxTree.span());
                    }
                }
                std::unique_ptr<BoundExpression> assigned;
                if (compoundOp) {
                    if (!property->getter) {
                        diagnostics_.report(
                            "RS2481",
                            "property '" + property->name + "' is write-only",
                            syntaxTree.nameToken.span);
                        return makeError(syntaxTree.span());
                    }
                    auto lhsValue = std::make_unique<BoundCallExpression>();
                    lhsValue->span = syntaxTree.nameToken.span;
                    lhsValue->type = property->type;
                    lhsValue->typeName = property->typeName;
                    lhsValue->function = *property->getter;
                    lhsValue->staticConstructor = findStaticConstructorForFunction(
                        visibleTypes_, *property->getter);
                    if (!lhsValue->staticConstructor) {
                        lhsValue->staticConstructor = type->second.staticConstructor;
                    }
                    auto rhsValue = bindExpression(*syntaxTree.expression);
                    auto combined = bindBinaryOperation(
                        std::move(lhsValue),
                        syntaxTree.nameToken.span,
                        *compoundOp,
                        std::move(rhsValue),
                        syntaxTree.expression->span(),
                        syntaxTree.span());
                    assigned = convertExpression(
                        std::move(combined),
                        property->type,
                        syntaxTree.span(),
                        "property assignment",
                        property->typeName);
                } else {
                    assigned = bindTargetExpression(
                        *syntaxTree.expression,
                        property->type,
                        property->typeName,
                        "property assignment");
                }
                if (writableBackingField) {
                    auto oldAccess = std::make_unique<BoundMemberAccessExpression>();
                    oldAccess->span = syntaxTree.nameToken.span;
                    oldAccess->type = writableBackingField->type;
                    oldAccess->typeName = writableBackingField->typeName;
                    oldAccess->ownerType = type->second;
                    oldAccess->field = *writableBackingField;
                    auto result = std::make_unique<BoundMemberAssignmentExpression>();
                    result->span = syntaxTree.span();
                    result->type = property->type;
                    result->typeName = property->typeName;
                    result->ownerType = type->second;
                    result->field = *writableBackingField;
                    result->expression = wrapResourceHandleOverwriteRelease(
                        std::move(oldAccess), std::move(assigned), syntaxTree.span());
                    return result;
                }
                auto result = std::make_unique<BoundPropertyAssignmentExpression>();
                result->span = syntaxTree.span();
                result->type = property->type;
                result->typeName = property->typeName;
                result->setter = *property->setter;
                result->staticConstructor = findStaticConstructorForFunction(
                    visibleTypes_, *property->setter);
                if (!result->staticConstructor) {
                    result->staticConstructor = type->second.staticConstructor;
                }
                result->assignedValue = std::move(assigned);
                return result;
            }
        }
    }

    std::unique_ptr<BoundExpression> receiver = bindReceiverAccess();
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
        checkObsoleteReference(
            field->id, "field", field->name, syntaxTree.nameToken.span);
        recordSymbolReference(
            occurrences_,
            currentSourceName_,
            field->id,
            SymbolKind::Field,
            field->name,
            syntaxTree.nameToken.span);
        if (field->constField) {
            diagnostics_.report(
                "RS2483",
                "cannot assign to const field '" + field->name + "'",
                syntaxTree.nameToken.span);
            return makeError(syntaxTree.span());
        }
        if (field->readonlyField &&
            !isReadonlyFieldWritable(
                *field, currentConstructor_, currentStaticMethod_, currentOwnerType_)) {
            diagnostics_.report(
                "RS2483",
                "cannot assign to readonly field '" + field->name + "'",
                syntaxTree.nameToken.span);
            return makeError(syntaxTree.span());
        }
        std::unique_ptr<BoundExpression> value;
        if (compoundOp) {
            std::unique_ptr<BoundExpression> lhsValue;
            if (field->staticField) {
                auto access = std::make_unique<BoundMemberAccessExpression>();
                access->span = syntaxTree.nameToken.span;
                access->type = field->type;
                access->typeName = field->typeName;
                access->ownerType = type->second;
                access->field = *field;
                if (!type->second.staticConstructor.has_value() &&
                    !field->hasConstantValue && field->initializerSyntax) {
                    access->staticInitializer = bindStaticFieldInitializer(
                        type->second, *field);
                }
                lhsValue = std::move(access);
            } else if (type->second.kind == TypeKind::Struct) {
                auto access = std::make_unique<BoundStructFieldAccessExpression>();
                access->span = syntaxTree.nameToken.span;
                access->type = field->type;
                access->typeName = field->typeName;
                access->receiver = bindReceiverAccess();
                access->ownerType = type->second;
                access->field = *field;
                lhsValue = std::move(access);
            } else {
                auto access = std::make_unique<BoundMemberAccessExpression>();
                access->span = syntaxTree.nameToken.span;
                access->type = field->type;
                access->typeName = field->typeName;
                access->receiver = bindReceiverAccess();
                access->ownerType = type->second;
                access->field = *field;
                lhsValue = std::move(access);
            }
            auto rhsValue = bindExpression(*syntaxTree.expression);
            auto combined = bindBinaryOperation(
                std::move(lhsValue),
                syntaxTree.nameToken.span,
                *compoundOp,
                std::move(rhsValue),
                syntaxTree.expression->span(),
                syntaxTree.span());
            value = convertExpression(
                std::move(combined),
                field->type,
                syntaxTree.span(),
                "field assignment",
                field->typeName);
        } else {
            value = bindTargetExpression(
                *syntaxTree.expression, field->type, field->typeName,
                "field assignment");
        }
        if (field->staticField) {
            auto oldAccess = std::make_unique<BoundMemberAccessExpression>();
            oldAccess->span = syntaxTree.nameToken.span;
            oldAccess->type = field->type;
            oldAccess->typeName = field->typeName;
            oldAccess->ownerType = type->second;
            oldAccess->field = *field;
            auto result = std::make_unique<BoundMemberAssignmentExpression>();
            result->span = syntaxTree.span();
            result->type = field->type;
            result->typeName = field->typeName;
            result->ownerType = type->second;
            result->field = *field;
            result->expression = wrapResourceHandleOverwriteRelease(
                std::move(oldAccess), std::move(value), syntaxTree.span());
            return result;
        }
        if (type->second.kind == TypeKind::Struct) {
            const VariableSymbol* variable = nullptr;
            if (receiver->kind() == BoundNodeKind::VariableExpression) {
                variable = &static_cast<const BoundVariableExpression&>(*receiver).variable;
            } else if (receiver->kind() == BoundNodeKind::MemberAccessExpression) {
                const auto& mRecv = static_cast<const BoundMemberAccessExpression&>(*receiver);
                if (mRecv.receiver && mRecv.receiver->kind() == BoundNodeKind::VariableExpression) {
                    const auto& candVar = static_cast<const BoundVariableExpression&>(*mRecv.receiver).variable;
                    if (const auto* origVar = lookupVariable(candVar.name);
                        origVar && (storageTypeOf(*origVar) != origVar->type || storageTypeNameOf(*origVar) != origVar->typeName)) {
                        variable = origVar;
                    }
                }
            }
            if (!variable) {
                if (receiver->kind() == BoundNodeKind::MemberAccessExpression) {
                    auto& memberRecv = static_cast<BoundMemberAccessExpression&>(*receiver);
                    if (memberRecv.field.constField) {
                        diagnostics_.report(
                            "RS2483",
                            "cannot assign to field of const struct '" +
                                memberRecv.field.name + "'",
                            syntaxTree.receiver->span());
                        return makeError(syntaxTree.span());
                    }
                    if (memberRecv.field.readonlyField &&
                        !isReadonlyFieldWritable(
                            memberRecv.field, currentConstructor_, currentStaticMethod_, currentOwnerType_)) {
                        diagnostics_.report(
                            "RS2483",
                            "cannot assign to field of readonly struct '" +
                                memberRecv.field.name + "'",
                            syntaxTree.receiver->span());
                        return makeError(syntaxTree.span());
                    }
                    if (memberRecv.field.staticField) {
                        auto oldStaticRecv = std::make_unique<BoundMemberAccessExpression>();
                        oldStaticRecv->span = syntaxTree.receiver->span();
                        oldStaticRecv->type = memberRecv.field.type;
                        oldStaticRecv->typeName = memberRecv.field.typeName;
                        oldStaticRecv->ownerType = memberRecv.ownerType;
                        oldStaticRecv->field = memberRecv.field;
                        auto oldAccess = std::make_unique<BoundStructFieldAccessExpression>();
                        oldAccess->span = syntaxTree.nameToken.span;
                        oldAccess->type = field->type;
                        oldAccess->typeName = field->typeName;
                        oldAccess->receiver = std::move(oldStaticRecv);
                        oldAccess->ownerType = type->second;
                        oldAccess->field = *field;
                        auto result = std::make_unique<BoundStructFieldAssignmentExpression>();
                        result->span = syntaxTree.span();
                        result->type = field->type;
                        result->typeName = field->typeName;
                        result->ownerType = type->second;
                        result->field = *field;
                        result->expression = wrapResourceHandleOverwriteRelease(
                            std::move(oldAccess), std::move(value), syntaxTree.span());
                        result->staticFieldReceiver = true;
                        result->staticReceiverOwnerType = memberRecv.ownerType;
                        result->staticReceiverField = memberRecv.field;
                        result->staticReceiverInitializer = std::move(memberRecv.staticInitializer);
                        return result;
                    }
                } else if (receiver->kind() == BoundNodeKind::StructFieldAccessExpression) {
                    auto& structRecv = static_cast<BoundStructFieldAccessExpression&>(*receiver);
                    if (structRecv.field.constField ||
                        (structRecv.field.readonlyField &&
                         !isReadonlyFieldWritable(
                             structRecv.field, currentConstructor_, currentStaticMethod_, currentOwnerType_))) {
                        diagnostics_.report(
                            "RS2483",
                            std::string("cannot assign to field of ") +
                                (structRecv.field.constField ? "const" : "readonly") +
                                " struct '" + structRecv.field.name + "'",
                            syntaxTree.receiver->span());
                        return makeError(syntaxTree.span());
                    }
                }
                diagnostics_.report("RS2482", "struct field assignment requires a variable receiver", syntaxTree.receiver->span());
                return makeError(syntaxTree.span());
            }
            if (variable->isConst || variable->modifier == ParameterModifier::In) {
                diagnostics_.report(
                    "RS8702",
                    "cannot assign to field of read-only 'in' or 'Read<T>' variable '" +
                        variable->name + "'",
                    syntaxTree.receiver->span());
                return makeError(syntaxTree.span());
            }
            if (variable->isUsingResource) {
                diagnostics_.report(
                    "RS2651",
                    "cannot modify members of '" + variable->name +
                        "' because it is a 'using' variable",
                    syntaxTree.receiver->span());
                return makeError(syntaxTree.span());
            }
            auto oldAccess = std::make_unique<BoundStructFieldAccessExpression>();
            oldAccess->span = syntaxTree.nameToken.span;
            oldAccess->type = field->type;
            oldAccess->typeName = field->typeName;
            oldAccess->receiver = makeVariableAccess(*variable, syntaxTree.receiver->span());
            oldAccess->ownerType = type->second;
            oldAccess->field = *field;
            auto result = std::make_unique<BoundStructFieldAssignmentExpression>();
            result->span = syntaxTree.span();
            result->type = field->type;
            result->typeName = field->typeName;
            result->variable = *variable;
            result->ownerType = type->second;
            result->field = *field;
            result->expression = wrapResourceHandleOverwriteRelease(
                std::move(oldAccess), std::move(value), syntaxTree.span());
            if (storageTypeOf(*variable) != variable->type || storageTypeNameOf(*variable) != variable->typeName) {
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
        std::unique_ptr<BoundExpression> wrappedExpr = std::move(value);
        if (!isInsideResourceHandleImplementation() &&
            isManagedResourceHandleType(field->type, field->typeName)) {
            std::unique_ptr<BoundExpression> oldRecv;
            if (receiver->kind() == BoundNodeKind::VariableExpression) {
                const auto& rv = static_cast<const BoundVariableExpression&>(*receiver).variable;
                oldRecv = variableExpression(rv, syntaxTree.receiver->span());
            } else {
                const auto recvTemp = allocateTempVariable(
                    "rh_recv", receiver->type, receiver->typeName, syntaxTree.receiver->span());
                auto recvAssign = std::make_unique<BoundAssignmentExpression>();
                recvAssign->span = syntaxTree.receiver->span();
                recvAssign->type = receiver->type;
                recvAssign->typeName = receiver->typeName;
                recvAssign->variable = recvTemp;
                recvAssign->expression = std::move(receiver);
                receiver = std::move(recvAssign);
                oldRecv = makeVariableAccess(recvTemp, syntaxTree.receiver->span());
            }
            auto oldAccess = std::make_unique<BoundMemberAccessExpression>();
            oldAccess->span = syntaxTree.nameToken.span;
            oldAccess->type = field->type;
            oldAccess->typeName = field->typeName;
            oldAccess->receiver = std::move(oldRecv);
            oldAccess->ownerType = type->second;
            oldAccess->field = *field;
            wrappedExpr = wrapResourceHandleOverwriteRelease(
                std::move(oldAccess), std::move(wrappedExpr), syntaxTree.span());
        }
        auto result = std::make_unique<BoundMemberAssignmentExpression>();
        result->span = syntaxTree.span();
        result->type = field->type;
        result->typeName = field->typeName;
        result->receiver = std::move(receiver);
        result->ownerType = type->second;
        result->field = *field;
        result->expression = std::move(wrappedExpr);
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
        checkObsoleteReference(
            property->id, "property", property->name, syntaxTree.nameToken.span);
        recordSymbolReference(
            occurrences_,
            currentSourceName_,
            property->id,
            SymbolKind::Property,
            property->name,
            syntaxTree.nameToken.span);
        if (property->staticProperty) {
            diagnostics_.report(
                "RS2484",
                "static property '" + property->name +
                    "' must be accessed through its type",
                syntaxTree.nameToken.span);
            return makeError(syntaxTree.span());
        }
        const FieldSymbol* writableBackingField = nullptr;
        if (!property->setter) {
            const bool isOnThis =
                receiver->kind() == BoundNodeKind::VariableExpression &&
                static_cast<const BoundVariableExpression&>(*receiver).variable.name == "this";
            if (isOnThis && property->backingFieldIndex < type->second.fields.size()) {
                const auto& candidate = type->second.fields[property->backingFieldIndex];
                if (isReadonlyFieldWritable(
                        candidate,
                        currentConstructor_,
                        currentStaticMethod_,
                        currentOwnerType_)) {
                    writableBackingField = &candidate;
                }
            }
            if (!writableBackingField) {
                diagnostics_.report("RS2483", "property '" + property->name + "' is read-only", syntaxTree.nameToken.span);
                return makeError(syntaxTree.span());
            }
        }
        const auto bindPropertyAssignedValue = [&]() -> std::unique_ptr<BoundExpression> {
            if (!compoundOp) {
                return bindTargetExpression(
                    *syntaxTree.expression, property->type,
                    property->typeName, "property assignment");
            }
            if (!property->getter) {
                diagnostics_.report(
                    "RS2481",
                    "property '" + property->name + "' is write-only",
                    syntaxTree.nameToken.span);
                return makeError(syntaxTree.span());
            }
            auto lhsValue = std::make_unique<BoundCallExpression>();
            lhsValue->span = syntaxTree.nameToken.span;
            lhsValue->type = property->type;
            lhsValue->typeName = property->typeName;
            lhsValue->function = *property->getter;
            lhsValue->arguments.push_back(bindReceiverAccess());
            auto rhsValue = bindExpression(*syntaxTree.expression);
            auto combined = bindBinaryOperation(
                std::move(lhsValue),
                syntaxTree.nameToken.span,
                *compoundOp,
                std::move(rhsValue),
                syntaxTree.expression->span(),
                syntaxTree.span());
            return convertExpression(
                std::move(combined),
                property->type,
                syntaxTree.span(),
                "property assignment",
                property->typeName);
        };
        if (type->second.kind == TypeKind::Struct && property->backingFieldIndex != std::numeric_limits<std::size_t>::max()) {
            const VariableSymbol* variable = nullptr;
            if (receiver->kind() == BoundNodeKind::VariableExpression) {
                variable = &static_cast<const BoundVariableExpression&>(*receiver).variable;
            } else if (receiver->kind() == BoundNodeKind::MemberAccessExpression) {
                const auto& mRecv = static_cast<const BoundMemberAccessExpression&>(*receiver);
                if (mRecv.receiver && mRecv.receiver->kind() == BoundNodeKind::VariableExpression) {
                    const auto& candVar = static_cast<const BoundVariableExpression&>(*mRecv.receiver).variable;
                    if (const auto* origVar = lookupVariable(candVar.name);
                        origVar && (storageTypeOf(*origVar) != origVar->type || storageTypeNameOf(*origVar) != origVar->typeName)) {
                        variable = origVar;
                    }
                }
            }
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
            auto oldAccess = std::make_unique<BoundStructFieldAccessExpression>();
            oldAccess->span = syntaxTree.nameToken.span;
            oldAccess->type = field.type;
            oldAccess->typeName = field.typeName;
            oldAccess->receiver = makeVariableAccess(*variable, syntaxTree.receiver->span());
            oldAccess->ownerType = type->second;
            oldAccess->field = field;
            auto result = std::make_unique<BoundStructFieldAssignmentExpression>();
            result->span = syntaxTree.span();
            result->type = property->type;
            result->typeName = property->typeName;
            result->variable = *variable;
            result->ownerType = type->second;
            result->field = field;
            result->expression = wrapResourceHandleOverwriteRelease(
                std::move(oldAccess), bindPropertyAssignedValue(), syntaxTree.span());
            return result;
        }
        auto assigned = bindPropertyAssignedValue();
        if (writableBackingField) {
            const auto& thisVar = static_cast<const BoundVariableExpression&>(*receiver).variable;
            auto oldAccess = std::make_unique<BoundMemberAccessExpression>();
            oldAccess->span = syntaxTree.nameToken.span;
            oldAccess->type = writableBackingField->type;
            oldAccess->typeName = writableBackingField->typeName;
            oldAccess->receiver = makeVariableAccess(thisVar, syntaxTree.receiver->span());
            oldAccess->ownerType = type->second;
            oldAccess->field = *writableBackingField;
            auto result = std::make_unique<BoundMemberAssignmentExpression>();
            result->span = syntaxTree.span();
            result->type = property->type;
            result->typeName = property->typeName;
            result->receiver = std::move(receiver);
            result->ownerType = type->second;
            result->field = *writableBackingField;
            result->expression = wrapResourceHandleOverwriteRelease(
                std::move(oldAccess), std::move(assigned), syntaxTree.span());
            return result;
        }
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
    if ((receiver->type == PrimitiveType::Object || receiver->type == PrimitiveType::Struct) &&
        !receiver->typeName.empty()) {
        const auto ownerIt = visibleTypes_.find(receiver->typeName);
        if (ownerIt != visibleTypes_.end()) {
            auto idxExpr = bindExpression(*syntaxTree.index);
            std::unique_ptr<BoundExpression> valExpr;
            if (const auto compoundOp = compoundBinaryOperatorToken(syntaxTree.equalsToken)) {
                const bool isStructReceiver = (receiver->type == PrimitiveType::Struct);
                VariableSymbol recvTemp{};
                if (!isStructReceiver) {
                    recvTemp = allocateTempVariable(
                        "indexer_recv", receiver->type, receiver->typeName, syntaxTree.receiver->span());
                    auto recvAssign = std::make_unique<BoundAssignmentExpression>();
                    recvAssign->span = syntaxTree.receiver->span();
                    recvAssign->type = receiver->type;
                    recvAssign->typeName = receiver->typeName;
                    recvAssign->variable = recvTemp;
                    recvAssign->expression = std::move(receiver);
                    receiver = std::move(recvAssign);
                }

                const auto idxTemp = allocateTempVariable(
                    "indexer_idx", idxExpr->type, idxExpr->typeName, syntaxTree.index->span());
                auto idxAssign = std::make_unique<BoundAssignmentExpression>();
                idxAssign->span = syntaxTree.index->span();
                idxAssign->type = idxExpr->type;
                idxAssign->typeName = idxExpr->typeName;
                idxAssign->variable = idxTemp;
                idxAssign->expression = std::move(idxExpr);
                idxExpr = std::move(idxAssign);

                auto getMethods = findMethods(ownerIt->second, "Get", false);
                std::vector<std::unique_ptr<BoundExpression>> getArgs;
                getArgs.push_back(makeVariableAccess(idxTemp, syntaxTree.index->span()));
                bool getAmbig = false;
                const auto getPlan = selectFlexibleCall(
                    getMethods, getArgs, nullptr, nullptr, visibleTypes_, getAmbig);
                if (getPlan.function && !getAmbig) {
                    std::vector<std::unique_ptr<syntax::ExpressionSyntax>> emptySyntaxArgs;
                    auto dn0 = std::make_unique<syntax::NameExpressionSyntax>();
                    dn0->identifierToken.span = syntaxTree.index->span();
                    emptySyntaxArgs.push_back(std::move(dn0));
                    std::vector<std::optional<syntax::SyntaxToken>> emptyMods;
                    auto getPrep = prepareCallArguments(
                        *getPlan.function, getPlan.sources, std::move(getArgs),
                        emptySyntaxArgs, emptyMods, syntaxTree.span(), "indexer argument");
                    auto getReceiver = isStructReceiver
                        ? bindExpression(*syntaxTree.receiver)
                        : makeVariableAccess(recvTemp, syntaxTree.receiver->span());
                    auto lhsVal = bindSelectedCall(
                        *getPlan.function,
                        std::move(getPrep.arguments),
                        getPrep.syntaxArguments,
                        getPrep.modifiers,
                        std::move(getPrep.evaluationOrder),
                        std::move(getReceiver),
                        syntaxTree.span(),
                        "indexer argument",
                        false);
                    auto rhsVal = bindExpression(*syntaxTree.expression);
                    valExpr = bindBinaryOperation(
                        std::move(lhsVal),
                        syntaxTree.receiver->span(),
                        *compoundOp,
                        std::move(rhsVal),
                        syntaxTree.expression->span(),
                        syntaxTree.span());
                } else {
                    valExpr = bindExpression(*syntaxTree.expression);
                }
            } else {
                valExpr = bindExpression(*syntaxTree.expression);
            }
            std::vector<std::unique_ptr<BoundExpression>> args;
            args.push_back(std::move(idxExpr));
            args.push_back(std::move(valExpr));
            auto methods = findMethods(ownerIt->second, "Put", false);
            if (methods.empty()) {
                methods = findMethods(ownerIt->second, "Set", false);
            }
            bool ambiguous = false;
            const auto plan = selectFlexibleCall(
                methods, args, nullptr, nullptr, visibleTypes_, ambiguous);
            if (plan.function && !ambiguous) {
                std::vector<std::unique_ptr<syntax::ExpressionSyntax>> synthArgs;
                auto dn0 = std::make_unique<syntax::NameExpressionSyntax>();
                dn0->identifierToken.span = syntaxTree.index->span();
                synthArgs.push_back(std::move(dn0));
                auto dn1 = std::make_unique<syntax::NameExpressionSyntax>();
                dn1->identifierToken.span = syntaxTree.expression->span();
                synthArgs.push_back(std::move(dn1));
                std::vector<std::optional<syntax::SyntaxToken>> synthMods;
                auto prepared = prepareCallArguments(
                    *plan.function, plan.sources, std::move(args),
                    synthArgs, synthMods, syntaxTree.span(), "indexer assignment");
                auto callExpr = bindSelectedCall(
                    *plan.function,
                    std::move(prepared.arguments),
                    prepared.syntaxArguments,
                    prepared.modifiers,
                    std::move(prepared.evaluationOrder),
                    std::move(receiver),
                    syntaxTree.span(),
                    "indexer assignment",
                    false);
                return callExpr;
            }
        }
    }
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
    std::unique_ptr<BoundExpression> value;
    if (const auto compoundOp = compoundBinaryOperatorToken(syntaxTree.equalsToken)) {
        const auto recvTemp = allocateTempVariable(
            "arr_recv", receiver->type, receiver->typeName, syntaxTree.receiver->span());
        auto recvAssign = std::make_unique<BoundAssignmentExpression>();
        recvAssign->span = syntaxTree.receiver->span();
        recvAssign->type = receiver->type;
        recvAssign->typeName = receiver->typeName;
        recvAssign->variable = recvTemp;
        recvAssign->expression = std::move(receiver);
        receiver = std::move(recvAssign);

        const auto idxTemp = allocateTempVariable(
            "arr_idx", PrimitiveType::Int, {}, syntaxTree.index->span());
        auto idxAssign = std::make_unique<BoundAssignmentExpression>();
        idxAssign->span = syntaxTree.index->span();
        idxAssign->type = PrimitiveType::Int;
        idxAssign->variable = idxTemp;
        idxAssign->expression = std::move(index);
        index = std::move(idxAssign);

        auto lhsValue = std::make_unique<BoundElementAccessExpression>();
        lhsValue->span = syntaxTree.span();
        lhsValue->type = elementType;
        lhsValue->typeName = elementTypeName;
        lhsValue->receiver = makeVariableAccess(recvTemp, syntaxTree.receiver->span());
        lhsValue->index = makeVariableAccess(idxTemp, syntaxTree.index->span());
        lhsValue->elementType = elementType;
        lhsValue->elementTypeName = elementTypeName;
        auto rhsValue = bindExpression(*syntaxTree.expression);
        auto combined = bindBinaryOperation(
            std::move(lhsValue),
            syntaxTree.receiver->span(),
            *compoundOp,
            std::move(rhsValue),
            syntaxTree.expression->span(),
            syntaxTree.span());
        value = convertExpression(
            std::move(combined),
            elementType,
            syntaxTree.span(),
            "array element assignment",
            elementTypeName);
    } else {
        value = convertExpression(bindExpression(*syntaxTree.expression), elementType, syntaxTree.expression->span(), "array element assignment", elementTypeName);
    }
    if (!isInsideResourceHandleImplementation() &&
        isManagedResourceHandleType(elementType, elementTypeName)) {
        std::unique_ptr<BoundExpression> oldArrRecv;
        if (receiver->kind() == BoundNodeKind::VariableExpression) {
            const auto& rv = static_cast<const BoundVariableExpression&>(*receiver).variable;
            oldArrRecv = variableExpression(rv, syntaxTree.receiver->span());
        } else {
            const auto recvTemp = allocateTempVariable(
                "rh_arr", receiver->type, receiver->typeName, syntaxTree.receiver->span());
            auto recvAssign = std::make_unique<BoundAssignmentExpression>();
            recvAssign->span = syntaxTree.receiver->span();
            recvAssign->type = receiver->type;
            recvAssign->typeName = receiver->typeName;
            recvAssign->variable = recvTemp;
            recvAssign->expression = std::move(receiver);
            receiver = std::move(recvAssign);
            oldArrRecv = makeVariableAccess(recvTemp, syntaxTree.receiver->span());
        }
        std::unique_ptr<BoundExpression> oldIdx;
        if (index->kind() == BoundNodeKind::LiteralExpression) {
            const auto& lit = static_cast<const BoundLiteralExpression&>(*index);
            auto copyLit = std::make_unique<BoundLiteralExpression>();
            copyLit->span = lit.span;
            copyLit->type = lit.type;
            copyLit->typeName = lit.typeName;
            copyLit->value = lit.value;
            oldIdx = std::move(copyLit);
        } else if (index->kind() == BoundNodeKind::VariableExpression) {
            const auto& iv = static_cast<const BoundVariableExpression&>(*index).variable;
            oldIdx = variableExpression(iv, syntaxTree.index->span());
        } else {
            const auto idxTemp = allocateTempVariable(
                "rh_idx", PrimitiveType::Int, {}, syntaxTree.index->span());
            auto idxAssign = std::make_unique<BoundAssignmentExpression>();
            idxAssign->span = syntaxTree.index->span();
            idxAssign->type = PrimitiveType::Int;
            idxAssign->variable = idxTemp;
            idxAssign->expression = std::move(index);
            index = std::move(idxAssign);
            oldIdx = makeVariableAccess(idxTemp, syntaxTree.index->span());
        }
        auto oldElem = std::make_unique<BoundElementAccessExpression>();
        oldElem->span = syntaxTree.span();
        oldElem->type = elementType;
        oldElem->typeName = elementTypeName;
        oldElem->receiver = std::move(oldArrRecv);
        oldElem->index = std::move(oldIdx);
        oldElem->elementType = elementType;
        oldElem->elementTypeName = elementTypeName;
        value = wrapResourceHandleOverwriteRelease(
            std::move(oldElem), std::move(value), syntaxTree.span());
    }
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
