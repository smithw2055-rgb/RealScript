#include "realscript/semantic/Semantic.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <unordered_set>

namespace realscript::semantic {
namespace {

SymbolId fnv1a(const std::string& value) noexcept {
    SymbolId hash = 14695981039346656037ull;
    for (const auto character : value) {
        hash ^= static_cast<unsigned char>(character);
        hash *= 1099511628211ull;
    }
    return hash;
}

struct ResolvedType {
    PrimitiveType type = PrimitiveType::Error;
    std::string name;
};

PrimitiveType typeKindPrimitive(TypeKind kind) noexcept {
    switch (kind) {
    case TypeKind::Class: return PrimitiveType::Object;
    case TypeKind::Struct: return PrimitiveType::Struct;
    case TypeKind::Enum: return PrimitiveType::Enum;
    }
    return PrimitiveType::Error;
}

ResolvedType resolveNamedType(
    const std::string& name,
    const TypeSymbolMap& visibleTypes,
    bool allowVoid) {
    const auto primitive = resolvePrimitiveType(name);
    if (primitive != PrimitiveType::Error) {
        if (!allowVoid && primitive == PrimitiveType::Void) return {};
        return {primitive, {}};
    }
    const auto found = visibleTypes.find(name);
    if (found != visibleTypes.end()) {
        return {typeKindPrimitive(found->second.kind), canonicalTypeName(found->second)};
    }
    return {};
}

ParameterModifier parameterModifier(
    const std::optional<syntax::SyntaxToken>& token) noexcept {
    if (!token) return ParameterModifier::None;
    switch (token->kind) {
    case syntax::SyntaxKind::RefKeyword: return ParameterModifier::Ref;
    case syntax::SyntaxKind::OutKeyword: return ParameterModifier::Out;
    case syntax::SyntaxKind::InKeyword: return ParameterModifier::In;
    default: return ParameterModifier::None;
    }
}

const char* parameterModifierName(ParameterModifier modifier) noexcept {
    switch (modifier) {
    case ParameterModifier::None: return "";
    case ParameterModifier::Ref: return "ref ";
    case ParameterModifier::Out: return "out ";
    case ParameterModifier::In: return "in ";
    }
    return "";
}

ResolvedType resolveType(
    const syntax::TypeSyntax& syntaxTree,
    const TypeSymbolMap& visibleTypes,
    bool allowVoid) {
    const auto element = resolveNamedType(
        syntaxTree.name.text, visibleTypes, allowVoid && !syntaxTree.isArray());
    if (element.type == PrimitiveType::Error) return {};
    if (!syntaxTree.isArray()) return element;
    if (element.type == PrimitiveType::Void || element.type == PrimitiveType::Array) return {};
    return {PrimitiveType::Array, arrayTypeName(element.type, element.name)};
}

std::string displayType(PrimitiveType type, const std::string& name) {
    return isExactType(type) && !name.empty() ? name : primitiveTypeName(type);
}

std::string tokenValueToString(const syntax::TokenValue& val) {
    if (const auto* s = std::get_if<std::string>(&val)) return *s;
    if (const auto* i = std::get_if<std::int64_t>(&val)) return std::to_string(*i);
    if (const auto* d = std::get_if<double>(&val)) {
        std::ostringstream out;
        out << *d;
        return out.str();
    }
    if (const auto* b = std::get_if<bool>(&val)) return *b ? "true" : "false";
    return "null";
}

bool evaluateConstantExpressionImpl(
    const syntax::ExpressionSyntax& expr,
    const TypeSymbol* currentType,
    const TypeSymbolMap& visibleTypes,
    syntax::TokenValue& outValue,
    PrimitiveType& outType,
    const std::vector<std::unordered_map<std::string, VariableSymbol>>* localScopes) {
    const auto findConstantFieldInHierarchy = [&](const TypeSymbol* startType,
                                                  const std::string& fieldName) -> const FieldSymbol* {
        std::unordered_set<SymbolId> visited;
        const TypeSymbol* curr = startType;
        while (curr && visited.insert(curr->id).second) {
            for (const auto& f : curr->fields) {
                if (f.name == fieldName && f.constField && f.hasConstantValue) {
                    return &f;
                }
            }
            if (curr->baseTypeName.empty()) break;
            const auto baseIt = visibleTypes.find(curr->baseTypeName);
            if (baseIt == visibleTypes.end()) break;
            curr = &baseIt->second;
        }
        return nullptr;
    };

    switch (expr.kind()) {
    case syntax::SyntaxKind::LiteralExpression: {
        const auto& lit = static_cast<const syntax::LiteralExpressionSyntax&>(expr).literalToken;
        outValue = lit.value;
        switch (lit.kind) {
        case syntax::SyntaxKind::IntegerLiteralToken:
            outType = (!lit.text.empty() && (lit.text.back() == 'l' || lit.text.back() == 'L'))
                ? PrimitiveType::Long
                : PrimitiveType::Int;
            return true;
        case syntax::SyntaxKind::FloatLiteralToken:
            outType = (!lit.text.empty() && (lit.text.back() == 'f' || lit.text.back() == 'F'))
                ? PrimitiveType::Float
                : PrimitiveType::Double;
            return true;
        case syntax::SyntaxKind::StringLiteralToken: outType = PrimitiveType::String; return true;
        case syntax::SyntaxKind::TrueKeyword:
        case syntax::SyntaxKind::FalseKeyword: outType = PrimitiveType::Bool; return true;
        case syntax::SyntaxKind::NullKeyword: outType = PrimitiveType::Null; return true;
        default: return false;
        }
    }
    case syntax::SyntaxKind::ParenthesizedExpression:
        return evaluateConstantExpressionImpl(
            *static_cast<const syntax::ParenthesizedExpressionSyntax&>(expr).expression,
            currentType, visibleTypes, outValue, outType, localScopes);
    case syntax::SyntaxKind::NameExpression: {
        const auto& name = static_cast<const syntax::NameExpressionSyntax&>(expr).identifierToken.text;
        if (localScopes) {
            for (auto it = localScopes->rbegin(); it != localScopes->rend(); ++it) {
                const auto found = it->find(name);
                if (found != it->end()) {
                    if (found->second.isConst && found->second.hasConstantValue) {
                        outValue = found->second.constantValue;
                        outType = found->second.type;
                        return true;
                    }
                    return false;
                }
            }
        }
        if (const auto* f = findConstantFieldInHierarchy(currentType, name)) {
            outValue = f->constantValue;
            outType = f->type;
            return true;
        }
        return false;
    }
    case syntax::SyntaxKind::MemberAccessExpression: {
        const auto& member = static_cast<const syntax::MemberAccessExpressionSyntax&>(expr);
        if (member.receiver->kind() == syntax::SyntaxKind::NameExpression) {
            const auto& typeName = static_cast<const syntax::NameExpressionSyntax&>(*member.receiver).identifierToken.text;
            const TypeSymbol* targetType = nullptr;
            if (currentType && (typeName == currentType->name || typeName == canonicalTypeName(*currentType))) {
                targetType = currentType;
            } else {
                const auto it = visibleTypes.find(typeName);
                if (it != visibleTypes.end()) targetType = &it->second;
            }
            if (targetType) {
                if (const auto* f = findConstantFieldInHierarchy(targetType, member.nameToken.text)) {
                    outValue = f->constantValue;
                    outType = f->type;
                    return true;
                }
                for (const auto& em : targetType->enumMembers) {
                    if (em.name == member.nameToken.text) {
                        outValue = em.value;
                        outType = PrimitiveType::Enum;
                        return true;
                    }
                }
            }
        }
        return false;
    }
    case syntax::SyntaxKind::CastExpression: {
        const auto& castExpr = static_cast<const syntax::CastExpressionSyntax&>(expr);
        syntax::TokenValue subVal;
        PrimitiveType subType = PrimitiveType::Error;
        if (!evaluateConstantExpressionImpl(
                *castExpr.expression, currentType, visibleTypes, subVal, subType, localScopes)) {
            return false;
        }
        const auto resolved = resolveType(castExpr.type, visibleTypes, false);
        if (resolved.type == PrimitiveType::Error) return false;
        if (isIntegralType(resolved.type) || resolved.type == PrimitiveType::Enum) {
            if (const auto* i = std::get_if<std::int64_t>(&subVal)) {
                outValue = *i;
                outType = resolved.type;
                return true;
            }
            if (const auto* d = std::get_if<double>(&subVal)) {
                outValue = static_cast<std::int64_t>(*d);
                outType = resolved.type;
                return true;
            }
            return false;
        }
        if (isFloatingPointType(resolved.type)) {
            if (const auto* d = std::get_if<double>(&subVal)) {
                outValue = *d;
                outType = resolved.type;
                return true;
            }
            if (const auto* i = std::get_if<std::int64_t>(&subVal)) {
                outValue = static_cast<double>(*i);
                outType = resolved.type;
                return true;
            }
            return false;
        }
        if (resolved.type == PrimitiveType::Bool && std::holds_alternative<bool>(subVal)) {
            outValue = subVal;
            outType = PrimitiveType::Bool;
            return true;
        }
        if (resolved.type == PrimitiveType::String && std::holds_alternative<std::string>(subVal)) {
            outValue = subVal;
            outType = PrimitiveType::String;
            return true;
        }
        return false;
    }
    case syntax::SyntaxKind::ConditionalExpression: {
        const auto& condExpr = static_cast<const syntax::ConditionalExpressionSyntax&>(expr);
        syntax::TokenValue condVal;
        PrimitiveType condType = PrimitiveType::Error;
        if (!evaluateConstantExpressionImpl(
                *condExpr.condition, currentType, visibleTypes, condVal, condType, localScopes) ||
            condType != PrimitiveType::Bool ||
            !std::holds_alternative<bool>(condVal)) {
            return false;
        }
        const auto& branch = std::get<bool>(condVal) ? *condExpr.whenTrue : *condExpr.whenFalse;
        return evaluateConstantExpressionImpl(
            branch, currentType, visibleTypes, outValue, outType, localScopes);
    }
    case syntax::SyntaxKind::UnaryExpression: {
        const auto& unary = static_cast<const syntax::UnaryExpressionSyntax&>(expr);
        syntax::TokenValue subVal;
        PrimitiveType subType = PrimitiveType::Error;
        if (!evaluateConstantExpressionImpl(
                *unary.operand, currentType, visibleTypes, subVal, subType, localScopes)) {
            return false;
        }
        if (unary.operatorToken.kind == syntax::SyntaxKind::PlusToken) {
            outValue = subVal;
            outType = subType;
            return isNumericType(subType);
        }
        if (unary.operatorToken.kind == syntax::SyntaxKind::MinusToken) {
            if (const auto* i = std::get_if<std::int64_t>(&subVal)) {
                outValue = -(*i);
                outType = subType;
                return true;
            }
            if (const auto* d = std::get_if<double>(&subVal)) {
                outValue = -(*d);
                outType = subType;
                return true;
            }
            return false;
        }
        if (unary.operatorToken.kind == syntax::SyntaxKind::BangToken) {
            if (const auto* b = std::get_if<bool>(&subVal)) {
                outValue = !(*b);
                outType = PrimitiveType::Bool;
                return true;
            }
            return false;
        }
        return false;
    }
    case syntax::SyntaxKind::BinaryExpression: {
        const auto& bin = static_cast<const syntax::BinaryExpressionSyntax&>(expr);
        syntax::TokenValue leftVal, rightVal;
        PrimitiveType leftType = PrimitiveType::Error, rightType = PrimitiveType::Error;
        if (!evaluateConstantExpressionImpl(
                *bin.left, currentType, visibleTypes, leftVal, leftType, localScopes) ||
            !evaluateConstantExpressionImpl(
                *bin.right, currentType, visibleTypes, rightVal, rightType, localScopes)) {
            return false;
        }
        if (bin.operatorToken.kind == syntax::SyntaxKind::PlusToken &&
            (leftType == PrimitiveType::String || rightType == PrimitiveType::String)) {
            outValue = tokenValueToString(leftVal) + tokenValueToString(rightVal);
            outType = PrimitiveType::String;
            return true;
        }
        if (leftType == PrimitiveType::String && rightType == PrimitiveType::String) {
            const auto* ls = std::get_if<std::string>(&leftVal);
            const auto* rs = std::get_if<std::string>(&rightVal);
            if (!ls || !rs) return false;
            if (bin.operatorToken.kind == syntax::SyntaxKind::EqualsEqualsToken) {
                outValue = (*ls == *rs);
                outType = PrimitiveType::Bool;
                return true;
            }
            if (bin.operatorToken.kind == syntax::SyntaxKind::BangEqualsToken) {
                outValue = (*ls != *rs);
                outType = PrimitiveType::Bool;
                return true;
            }
            return false;
        }
        if (leftType == PrimitiveType::Bool && rightType == PrimitiveType::Bool) {
            const auto* lb = std::get_if<bool>(&leftVal);
            const auto* rb = std::get_if<bool>(&rightVal);
            if (!lb || !rb) return false;
            outType = PrimitiveType::Bool;
            switch (bin.operatorToken.kind) {
            case syntax::SyntaxKind::AmpersandAmpersandToken: outValue = (*lb && *rb); return true;
            case syntax::SyntaxKind::PipePipeToken: outValue = (*lb || *rb); return true;
            case syntax::SyntaxKind::EqualsEqualsToken: outValue = (*lb == *rb); return true;
            case syntax::SyntaxKind::BangEqualsToken: outValue = (*lb != *rb); return true;
            default: return false;
            }
        }
        if (isFloatingPointType(leftType) || isFloatingPointType(rightType)) {
            const auto toDouble = [](const syntax::TokenValue& v, double& out) -> bool {
                if (const auto* d = std::get_if<double>(&v)) { out = *d; return true; }
                if (const auto* i = std::get_if<std::int64_t>(&v)) { out = static_cast<double>(*i); return true; }
                return false;
            };
            double l = 0.0, r = 0.0;
            if (!toDouble(leftVal, l) || !toDouble(rightVal, r)) return false;
            const PrimitiveType floatOutType =
                (leftType == PrimitiveType::Float && rightType == PrimitiveType::Float)
                ? PrimitiveType::Float
                : PrimitiveType::Double;
            switch (bin.operatorToken.kind) {
            case syntax::SyntaxKind::PlusToken: outValue = l + r; outType = floatOutType; return true;
            case syntax::SyntaxKind::MinusToken: outValue = l - r; outType = floatOutType; return true;
            case syntax::SyntaxKind::StarToken: outValue = l * r; outType = floatOutType; return true;
            case syntax::SyntaxKind::SlashToken:
                if (r == 0.0) return false;
                outValue = l / r;
                outType = floatOutType;
                return true;
            case syntax::SyntaxKind::EqualsEqualsToken: outValue = (l == r); outType = PrimitiveType::Bool; return true;
            case syntax::SyntaxKind::BangEqualsToken: outValue = (l != r); outType = PrimitiveType::Bool; return true;
            case syntax::SyntaxKind::LessToken: outValue = (l < r); outType = PrimitiveType::Bool; return true;
            case syntax::SyntaxKind::LessOrEqualsToken: outValue = (l <= r); outType = PrimitiveType::Bool; return true;
            case syntax::SyntaxKind::GreaterToken: outValue = (l > r); outType = PrimitiveType::Bool; return true;
            case syntax::SyntaxKind::GreaterOrEqualsToken: outValue = (l >= r); outType = PrimitiveType::Bool; return true;
            default: return false;
            }
        }
        if (const auto* l = std::get_if<std::int64_t>(&leftVal)) {
            if (const auto* r = std::get_if<std::int64_t>(&rightVal)) {
                outType = (leftType == PrimitiveType::Long || rightType == PrimitiveType::Long)
                    ? PrimitiveType::Long
                    : PrimitiveType::Int;
                const auto ul = static_cast<std::uint64_t>(*l);
                const auto ur = static_cast<std::uint64_t>(*r);
                switch (bin.operatorToken.kind) {
                case syntax::SyntaxKind::PlusToken:
                    outValue = static_cast<std::int64_t>(ul + ur);
                    return true;
                case syntax::SyntaxKind::MinusToken:
                    outValue = static_cast<std::int64_t>(ul - ur);
                    return true;
                case syntax::SyntaxKind::StarToken:
                    outValue = static_cast<std::int64_t>(ul * ur);
                    return true;
                case syntax::SyntaxKind::SlashToken:
                    if (*r == 0 || (*l == std::numeric_limits<std::int64_t>::min() && *r == -1)) {
                        return false;
                    }
                    outValue = *l / *r;
                    return true;
                case syntax::SyntaxKind::PercentToken:
                    if (*r == 0 || (*l == std::numeric_limits<std::int64_t>::min() && *r == -1)) {
                        return false;
                    }
                    outValue = *l % *r;
                    return true;
                case syntax::SyntaxKind::EqualsEqualsToken: outValue = (*l == *r); outType = PrimitiveType::Bool; return true;
                case syntax::SyntaxKind::BangEqualsToken: outValue = (*l != *r); outType = PrimitiveType::Bool; return true;
                case syntax::SyntaxKind::LessToken: outValue = (*l < *r); outType = PrimitiveType::Bool; return true;
                case syntax::SyntaxKind::LessOrEqualsToken: outValue = (*l <= *r); outType = PrimitiveType::Bool; return true;
                case syntax::SyntaxKind::GreaterToken: outValue = (*l > *r); outType = PrimitiveType::Bool; return true;
                case syntax::SyntaxKind::GreaterOrEqualsToken: outValue = (*l >= *r); outType = PrimitiveType::Bool; return true;
                default: return false;
                }
            }
        }
        return false;
    }
    default:
        return false;
    }
}

bool tryApplyEvaluatedConstantToField(
    FieldSymbol& field,
    syntax::TokenValue constVal,
    PrimitiveType constType,
    diagnostics::DiagnosticBag* diagnostics) {
    const auto conv = classifyConversion(constType, field.type);
    const bool nullRef = constType == PrimitiveType::Null && isReferenceType(field.type);
    bool implicitNumericConst = false;
    if ((constType == PrimitiveType::Int || constType == PrimitiveType::Long ||
         constType == PrimitiveType::Enum) &&
        std::holds_alternative<std::int64_t>(constVal)) {
        const auto v = std::get<std::int64_t>(constVal);
        switch (field.type) {
        case PrimitiveType::Byte: implicitNumericConst = (v >= 0 && v <= 255); break;
        case PrimitiveType::SByte: implicitNumericConst = (v >= -128 && v <= 127); break;
        case PrimitiveType::Short: implicitNumericConst = (v >= -32768 && v <= 32767); break;
        case PrimitiveType::UShort:
        case PrimitiveType::Char: implicitNumericConst = (v >= 0 && v <= 65535); break;
        case PrimitiveType::Int:
            implicitNumericConst =
                (constType == PrimitiveType::Enum) ||
                (v >= std::numeric_limits<std::int32_t>::min() &&
                 v <= std::numeric_limits<std::int32_t>::max());
            break;
        case PrimitiveType::UInt:
        case PrimitiveType::ULong: implicitNumericConst = (v >= 0); break;
        case PrimitiveType::Long: implicitNumericConst = true; break;
        case PrimitiveType::Float:
        case PrimitiveType::Double: implicitNumericConst = true; break;
        default: break;
        }
    } else if (constType == PrimitiveType::Double &&
               field.type == PrimitiveType::Float &&
               std::holds_alternative<double>(constVal)) {
        const auto v = std::get<double>(constVal);
        implicitNumericConst =
            std::isfinite(v) && std::abs(v) <= std::numeric_limits<float>::max();
    }
    if (conv != ConversionKind::None || nullRef || constType == field.type ||
        implicitNumericConst) {
        if (isFloatingPointType(field.type) &&
            std::holds_alternative<std::int64_t>(constVal)) {
            constVal = static_cast<double>(std::get<std::int64_t>(constVal));
        }
        field.hasConstantValue = true;
        field.constantValue = std::move(constVal);
        return true;
    }
    if (diagnostics && (field.constField || field.type != PrimitiveType::Struct)) {
        diagnostics->report(
            "RS2408",
            "cannot convert constant field initializer for '" + field.name + "' to '" +
                displayType(field.type, field.typeName) + "'",
            field.initializerSyntax ? field.initializerSyntax->span() : field.declarationSpan);
    }
    return false;
}

bool hasAttributeName(
    const std::vector<syntax::AttributeListSyntax>& lists,
    std::string_view name) noexcept {
    const std::string withSuffix = std::string(name) + "Attribute";
    for (const auto& list : lists) {
        for (const auto& attribute : list.attributes) {
            if (attribute.nameToken.text == name ||
                attribute.nameToken.text == withSuffix) {
                return true;
            }
        }
    }
    return false;
}

template <typename T>
bool populateFields(
    TypeSymbol& type,
    const T& syntaxTree,
    const TypeSymbolMap& visibleTypes,
    diagnostics::DiagnosticBag& diagnostics,
    bool deferConstValidation = false) {
    bool valid = true;
    std::unordered_set<std::string> fieldNames;
    type.fields.clear();
    for (const auto& fieldSyntax : syntaxTree.fields) {
        if (!fieldNames.insert(fieldSyntax.identifierToken.text).second) {
            diagnostics.report(
                "RS2401",
                "field '" + fieldSyntax.identifierToken.text +
                    "' is already declared in type '" + type.name + "'",
                fieldSyntax.identifierToken.span);
            valid = false;
        }
        const auto resolved = resolveType(fieldSyntax.type, visibleTypes, false);
        if (resolved.type == PrimitiveType::Error) {
            diagnostics.report(
                "RS2402",
                "unknown field type '" + fieldSyntax.type.name.text + "'",
                fieldSyntax.type.span());
            valid = false;
        }
        FieldSymbol field;
        field.name = fieldSyntax.identifierToken.text;
        field.accessibility = accessibilityFromModifiers(
            fieldSyntax.modifiers);
        field.declaringTypeId = type.id;
        field.declaringTypeName = canonicalTypeName(type);
        field.type = resolved.type;
        field.typeName = resolved.name;
        field.index = type.fields.size();
        field.synthetic = false;
        field.staticField = hasModifier(
            fieldSyntax.modifiers, syntax::SyntaxKind::StaticKeyword);
        field.constField = hasModifier(
            fieldSyntax.modifiers, syntax::SyntaxKind::ConstKeyword);
        field.readonlyField = hasModifier(
            fieldSyntax.modifiers, syntax::SyntaxKind::ReadonlyKeyword);
        if (field.constField && field.readonlyField) {
            diagnostics.report(
                "RS2483",
                "field '" + field.name + "' cannot be both const and readonly",
                fieldSyntax.identifierToken.span);
            valid = false;
        }
        if (field.constField) {
            field.staticField = true;
        }
        field.observable = !field.staticField && !field.constField &&
            (type.observable || hasAttributeName(fieldSyntax.attributes, "Observable"));
        if (field.observable) {
            field.observableBitIndex = (type.nextObservableBitIndex < 63u)
                ? type.nextObservableBitIndex++
                : 63u;
        }
        field.simState = type.simState || hasAttributeName(fieldSyntax.attributes, "SimState");
        if (type.staticType && !field.staticField && !field.constField) {
            diagnostics.report(
                "RS2489",
                "cannot declare instance field '" + field.name +
                    "' in static class '" + type.name + "'",
                fieldSyntax.identifierToken.span);
            valid = false;
        }
        field.initializerSyntax = fieldSyntax.initializer.get();
        if (!fieldSyntax.initializer && field.constField) {
            diagnostics.report(
                "RS2407",
                "const field '" + field.name + "' requires an initializer",
                fieldSyntax.identifierToken.span);
            valid = false;
        }
        field.declarationSpan = fieldSyntax.identifierToken.span;
        field.id = fnv1a(canonicalTypeName(type) + "::field:" + field.name);
        type.fields.push_back(std::move(field));
    }

    while (resolveConstantFieldsPass(type, visibleTypes)) {}
    if (!deferConstValidation) {
        if (!validateConstantFields(type, visibleTypes, diagnostics)) {
            valid = false;
        }
    }
    return valid;
}

void appendImplicitThis(
    FunctionSymbol& result,
    const TypeSymbol& owner,
    bool mutableStructReceiver = false) {
    if (result.staticMethod) return;
    VariableSymbol self;
    self.name = "this";
    self.type = typeKindPrimitive(owner.kind);
    self.typeName = canonicalTypeName(owner);
    self.index = 0;
    if (mutableStructReceiver && owner.kind == TypeKind::Struct) {
        self.modifier = ParameterModifier::Ref;
        self.storageType = PrimitiveType::Object;
        self.storageTypeName = referenceWrapperTypeName(
            result.moduleName, self.type, self.typeName);
    } else {
        self.storageType = self.type;
        self.storageTypeName = self.typeName;
    }
    self.parameter = true;
    result.parameters.push_back(std::move(self));
}

void appendSyntaxParameters(
    FunctionSymbol& result,
    const std::vector<syntax::ParameterSyntax>& parameters,
    const TypeSymbolMap& visibleTypes,
    diagnostics::DiagnosticBag& diagnostics) {
    bool sawOptional = false;
    for (std::size_t syntaxIndex = 0;
         syntaxIndex < parameters.size(); ++syntaxIndex) {
        const auto& parameterSyntax = parameters[syntaxIndex];
        if (parameterSyntax.modifierToken &&
            parameterSyntax.modifierToken->kind == syntax::SyntaxKind::ThisKeyword) {
            if (syntaxIndex == 0 && (!result.method || result.staticMethod) && !result.constructor) {
                result.extensionMethod = true;
            } else {
                diagnostics.report(
                    "RS8920",
                    "'this' parameter modifier is only valid on the first parameter of a static or free function",
                    parameterSyntax.modifierToken->span);
            }
        }
        const auto parameterType = resolveType(
            parameterSyntax.type, visibleTypes, false);
        if (parameterType.type == PrimitiveType::Error) {
            diagnostics.report(
                "RS2201",
                "invalid parameter type '" +
                    parameterSyntax.type.name.text + "'",
                parameterSyntax.type.span());
        }
        VariableSymbol parameter;
        parameter.name = parameterSyntax.identifierToken.text;
        parameter.type = parameterType.type;
        parameter.typeName = parameterType.name;
        parameter.modifier = parameterModifier(
            parameterSyntax.modifierToken);
        parameter.paramsArray = parameterSyntax.paramsKeyword.has_value();
        if (parameter.paramsArray &&
            (syntaxIndex + 1 != parameters.size() ||
             parameter.type != PrimitiveType::Array ||
             parameter.modifier != ParameterModifier::None)) {
            diagnostics.report(
                "RS8916",
                "params must be the final unmodified array parameter",
                parameterSyntax.span());
        }
        if (parameterSyntax.defaultValue) {
            parameter.hasDefaultValue = true;
            sawOptional = true;
            const syntax::ExpressionSyntax* value =
                parameterSyntax.defaultValue.get();
            bool negative = false;
            if (value->kind() == syntax::SyntaxKind::UnaryExpression) {
                const auto& unary = static_cast<const
                    syntax::UnaryExpressionSyntax&>(*value);
                negative = unary.operatorToken.kind ==
                    syntax::SyntaxKind::MinusToken;
                value = unary.operand.get();
            }
            if (value && value->kind() ==
                    syntax::SyntaxKind::LiteralExpression) {
                const auto& literal = static_cast<const
                    syntax::LiteralExpressionSyntax&>(*value).literalToken;
                parameter.defaultValue = literal.value;
                switch (literal.kind) {
                case syntax::SyntaxKind::IntegerLiteralToken:
                    parameter.defaultValueType = PrimitiveType::Int;
                    if (negative && std::holds_alternative<std::int64_t>(
                            parameter.defaultValue)) {
                        parameter.defaultValue = -std::get<std::int64_t>(
                            parameter.defaultValue);
                    }
                    break;
                case syntax::SyntaxKind::FloatLiteralToken:
                    parameter.defaultValueType = PrimitiveType::Double;
                    if (negative && std::holds_alternative<double>(
                            parameter.defaultValue)) {
                        parameter.defaultValue = -std::get<double>(
                            parameter.defaultValue);
                    }
                    break;
                case syntax::SyntaxKind::StringLiteralToken:
                    parameter.defaultValueType = PrimitiveType::String;
                    break;
                case syntax::SyntaxKind::TrueKeyword:
                case syntax::SyntaxKind::FalseKeyword:
                    parameter.defaultValueType = PrimitiveType::Bool;
                    break;
                case syntax::SyntaxKind::NullKeyword:
                    parameter.defaultValueType = PrimitiveType::Null;
                    break;
                default:
                    parameter.defaultValueType = PrimitiveType::Error;
                    break;
                }
            }
            if (parameter.defaultValueType == PrimitiveType::Error ||
                parameter.paramsArray) {
                diagnostics.report(
                    "RS8917",
                    "optional parameter default must be a literal constant and params cannot have a default",
                    parameterSyntax.defaultValue->span());
            } else {
                const auto conversion = classifyConversion(
                    parameter.defaultValueType, parameter.type);
                const bool nullReference =
                    parameter.defaultValueType == PrimitiveType::Null &&
                    isReferenceType(parameter.type);
                if (conversion == ConversionKind::None && !nullReference &&
                    parameter.defaultValueType != parameter.type) {
                    diagnostics.report(
                        "RS8918",
                        "optional parameter default is not convertible to its parameter type",
                        parameterSyntax.defaultValue->span());
                }
            }
        } else if (sawOptional && !parameter.paramsArray) {
            diagnostics.report(
                "RS8919",
                "required parameter cannot follow an optional parameter",
                parameterSyntax.span());
        }
        if (parameter.modifier == ParameterModifier::Ref ||
            parameter.modifier == ParameterModifier::Out) {
            parameter.storageType = PrimitiveType::Object;
            parameter.storageTypeName = referenceWrapperTypeName(
                result.moduleName,
                parameter.type,
                parameter.typeName);
        } else {
            parameter.storageType = parameter.type;
            parameter.storageTypeName = parameter.typeName;
        }
        parameter.index = result.parameters.size();
        parameter.parameter = true;
        parameter.declarationSpan =
            parameterSyntax.identifierToken.span;
        result.parameters.push_back(std::move(parameter));
    }
}

} // namespace

bool hasModifier(
    const std::vector<syntax::SyntaxToken>& modifiers,
    syntax::SyntaxKind kind) noexcept {
    for (const auto& modifier : modifiers) {
        if (modifier.kind == kind) return true;
    }
    return false;
}

Accessibility accessibilityFromModifiers(
    const std::vector<syntax::SyntaxToken>& modifiers,
    Accessibility fallback) noexcept {
    if (hasModifier(modifiers, syntax::SyntaxKind::PrivateKeyword)) {
        return Accessibility::Private;
    }
    if (hasModifier(modifiers, syntax::SyntaxKind::ProtectedKeyword)) {
        return Accessibility::Protected;
    }
    if (hasModifier(modifiers, syntax::SyntaxKind::InternalKeyword)) {
        return Accessibility::Internal;
    }
    if (hasModifier(modifiers, syntax::SyntaxKind::PublicKeyword)) {
        return Accessibility::Public;
    }
    return fallback;
}

bool isAssignable(
    const TypeSymbolMap& visibleTypes,
    const std::string& sourceTypeName,
    const std::string& targetTypeName) noexcept {
    if (sourceTypeName.empty() || targetTypeName.empty()) return false;
    if (sourceTypeName == targetTypeName) return true;
    const auto targetTypeId = stableTypeId(targetTypeName);
    auto currentName = sourceTypeName;
    std::unordered_set<SymbolId> visited;
    std::vector<std::string> ifaceStack;
    std::unordered_set<SymbolId> visitedIfaces;
    while (!currentName.empty()) {
        const auto found = visibleTypes.find(currentName);
        if (found == visibleTypes.end() ||
            !visited.insert(found->second.id).second) {
            break;
        }
        for (const auto& baseIfaceName : found->second.baseInterfaceNames) {
            if (baseIfaceName == targetTypeName) return true;
            ifaceStack.push_back(baseIfaceName);
        }
        for (const auto& interfaceMap :
             found->second.interfaceDispatchMaps) {
            if (interfaceMap.interfaceTypeId == targetTypeId) {
                return true;
            }
        }
        if (found->second.baseTypeName == targetTypeName) return true;
        currentName = found->second.baseTypeName;
    }
    while (!ifaceStack.empty()) {
        const auto ifaceName = std::move(ifaceStack.back());
        ifaceStack.pop_back();
        if (ifaceName == targetTypeName) return true;
        const auto foundIface = visibleTypes.find(ifaceName);
        if (foundIface == visibleTypes.end() ||
            !visitedIfaces.insert(foundIface->second.id).second) {
            continue;
        }
        for (const auto& parentIface : foundIface->second.baseInterfaceNames) {
            if (parentIface == targetTypeName) return true;
            ifaceStack.push_back(parentIface);
        }
    }
    return false;
}

const char* primitiveTypeName(PrimitiveType type) noexcept {
    switch (type) {
    case PrimitiveType::Error: return "<error>";
    case PrimitiveType::Void: return "void";
    case PrimitiveType::Bool: return "bool";
    case PrimitiveType::Byte: return "byte";
    case PrimitiveType::SByte: return "sbyte";
    case PrimitiveType::Short: return "short";
    case PrimitiveType::UShort: return "ushort";
    case PrimitiveType::Int: return "int";
    case PrimitiveType::UInt: return "uint";
    case PrimitiveType::Long: return "long";
    case PrimitiveType::ULong: return "ulong";
    case PrimitiveType::Float: return "float";
    case PrimitiveType::Double: return "double";
    case PrimitiveType::Char: return "char";
    case PrimitiveType::String: return "string";
    case PrimitiveType::Object: return "object";
    case PrimitiveType::Struct: return "struct";
    case PrimitiveType::Enum: return "enum";
    case PrimitiveType::Array: return "array";
    case PrimitiveType::Handle: return "handle";
    case PrimitiveType::Null: return "null";
    }
    return "<unknown>";
}

PrimitiveType resolvePrimitiveType(const std::string& name) noexcept {
    if (name == "void") return PrimitiveType::Void;
    if (name == "bool") return PrimitiveType::Bool;
    if (name == "byte") return PrimitiveType::Byte;
    if (name == "sbyte") return PrimitiveType::SByte;
    if (name == "short") return PrimitiveType::Short;
    if (name == "ushort") return PrimitiveType::UShort;
    if (name == "int") return PrimitiveType::Int;
    if (name == "uint") return PrimitiveType::UInt;
    if (name == "long") return PrimitiveType::Long;
    if (name == "ulong") return PrimitiveType::ULong;
    if (name == "float") return PrimitiveType::Float;
    if (name == "double") return PrimitiveType::Double;
    if (name == "char") return PrimitiveType::Char;
    if (name == "string") return PrimitiveType::String;
    if (name == "object") return PrimitiveType::Object;
    if (name == "handle") return PrimitiveType::Handle;
    return PrimitiveType::Error;
}

bool isNumericType(PrimitiveType type) noexcept {
    return isIntegralType(type) || isFloatingPointType(type);
}

bool isIntegralType(PrimitiveType type) noexcept {
    return type == PrimitiveType::Byte ||
        type == PrimitiveType::SByte ||
        type == PrimitiveType::Short ||
        type == PrimitiveType::UShort ||
        type == PrimitiveType::Int ||
        type == PrimitiveType::UInt ||
        type == PrimitiveType::Long ||
        type == PrimitiveType::ULong ||
        type == PrimitiveType::Char;
}

bool isUnsignedIntegralType(PrimitiveType type) noexcept {
    return type == PrimitiveType::Byte ||
        type == PrimitiveType::UShort ||
        type == PrimitiveType::UInt ||
        type == PrimitiveType::ULong ||
        type == PrimitiveType::Char;
}

bool isFloatingPointType(PrimitiveType type) noexcept {
    return type == PrimitiveType::Float || type == PrimitiveType::Double;
}

bool isReferenceType(PrimitiveType type) noexcept {
    return type == PrimitiveType::String ||
        type == PrimitiveType::Object ||
        type == PrimitiveType::Array;
}

bool isExactType(PrimitiveType type) noexcept {
    return type == PrimitiveType::Object ||
        type == PrimitiveType::Struct ||
        type == PrimitiveType::Enum ||
        type == PrimitiveType::Array;
}

std::string arrayTypeName(
    PrimitiveType elementType,
    const std::string& elementTypeName) {
    const auto base = isExactType(elementType)
        ? elementTypeName
        : std::string(primitiveTypeName(elementType));
    return base.empty() ? std::string{} : base + "[]";
}

bool decodeArrayTypeName(
    const std::string& name,
    PrimitiveType& elementType,
    std::string& elementTypeName) {
    if (name.size() < 3 || name.substr(name.size() - 2) != "[]") return false;
    const auto base = name.substr(0, name.size() - 2);
    const auto primitive = resolvePrimitiveType(base);
    if (primitive != PrimitiveType::Error && primitive != PrimitiveType::Void) {
        elementType = primitive;
        elementTypeName.clear();
        return true;
    }
    // Exact non-primitive arrays are resolved through descriptors by TypeId.
    elementType = PrimitiveType::Object;
    elementTypeName = base;
    return !base.empty();
}

ConversionKind classifyConversion(PrimitiveType from, PrimitiveType to) noexcept {
    if (from == PrimitiveType::Error || to == PrimitiveType::Error) return ConversionKind::Identity;
    if (from == to) return ConversionKind::Identity;
    if (from == PrimitiveType::Null && to == PrimitiveType::String) return ConversionKind::NullToString;
    if (from == PrimitiveType::Null && to == PrimitiveType::Object) return ConversionKind::NullToObject;
    if (from == PrimitiveType::Null && to == PrimitiveType::Array) return ConversionKind::NullToArray;
    if (from == PrimitiveType::Int && to == PrimitiveType::Long) return ConversionKind::IntToLong;
    if (from == PrimitiveType::Int && to == PrimitiveType::Double) return ConversionKind::IntToDouble;
    if (from == PrimitiveType::Long && to == PrimitiveType::Double) return ConversionKind::LongToDouble;
    const auto implicitNumeric = [](PrimitiveType source, PrimitiveType target) {
        switch (source) {
        case PrimitiveType::SByte:
            return target == PrimitiveType::Short ||
                target == PrimitiveType::Int || target == PrimitiveType::Long ||
                target == PrimitiveType::Float || target == PrimitiveType::Double;
        case PrimitiveType::Byte:
            return target == PrimitiveType::Short || target == PrimitiveType::UShort ||
                target == PrimitiveType::Int || target == PrimitiveType::UInt ||
                target == PrimitiveType::Long || target == PrimitiveType::ULong ||
                target == PrimitiveType::Float || target == PrimitiveType::Double;
        case PrimitiveType::Short:
            return target == PrimitiveType::Int || target == PrimitiveType::Long ||
                target == PrimitiveType::Float || target == PrimitiveType::Double;
        case PrimitiveType::UShort:
        case PrimitiveType::Char:
            return target == PrimitiveType::Int || target == PrimitiveType::UInt ||
                target == PrimitiveType::Long || target == PrimitiveType::ULong ||
                target == PrimitiveType::Float || target == PrimitiveType::Double;
        case PrimitiveType::Int:
            return target == PrimitiveType::Long || target == PrimitiveType::Float ||
                target == PrimitiveType::Double;
        case PrimitiveType::UInt:
            return target == PrimitiveType::Long || target == PrimitiveType::ULong ||
                target == PrimitiveType::Float || target == PrimitiveType::Double;
        case PrimitiveType::Long:
        case PrimitiveType::ULong:
            return target == PrimitiveType::Float || target == PrimitiveType::Double;
        case PrimitiveType::Float:
            return target == PrimitiveType::Double;
        default:
            return false;
        }
    };
    if (implicitNumeric(from, to)) return ConversionKind::Numeric;
    return ConversionKind::None;
}

int conversionRank(PrimitiveType from, PrimitiveType to) noexcept {
    switch (classifyConversion(from, to)) {
    case ConversionKind::Identity: return 0;
    case ConversionKind::NullToString:
    case ConversionKind::NullToObject:
    case ConversionKind::NullToArray:
    case ConversionKind::IntToLong:
        return 1;
    case ConversionKind::IntToDouble:
    case ConversionKind::LongToDouble:
        return 2;
    case ConversionKind::Numeric:
        return 3;
    case ConversionKind::None: return -1;
    }
    return -1;
}

std::string canonicalTypeName(const TypeSymbol& type) {
    return type.moduleName.empty() ? type.name : type.moduleName + "::" + type.name;
}

SymbolId stableTypeId(const TypeSymbol& type) { return stableTypeId(canonicalTypeName(type)); }
SymbolId stableTypeId(const std::string& canonicalName) {
    return canonicalName.empty() ? 0 : fnv1a(canonicalName);
}

std::string referenceWrapperTypeName(
    const std::string& moduleName,
    PrimitiveType sourceType,
    const std::string& sourceTypeName) {
    std::string key = isExactType(sourceType) &&
            !sourceTypeName.empty()
        ? sourceTypeName
        : primitiveTypeName(sourceType);
    for (auto& character : key) {
        const auto alphaNumeric =
            (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') ||
            character == '_';
        if (!alphaNumeric) character = '_';
    }
    std::string moduleKey = moduleName.empty() ? "Global" : moduleName;
    for (auto& character : moduleKey) {
        const auto alphaNumeric =
            (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') ||
            character == '_';
        if (!alphaNumeric) character = '_';
    }
    const auto simpleName =
        "__RsRef__" + moduleKey + "__" + key;
    return moduleName.empty()
        ? simpleName
        : moduleName + "::" + simpleName;
}

TypeSymbol declareTypeShell(
    const std::string& moduleName,
    const syntax::ClassDeclarationSyntax& syntaxTree) {
    TypeSymbol result;
    result.kind = TypeKind::Class;
    result.accessibility = accessibilityFromModifiers(syntaxTree.modifiers);
    result.staticType = hasModifier(
        syntaxTree.modifiers, syntax::SyntaxKind::StaticKeyword);
    result.abstractType = hasModifier(
        syntaxTree.modifiers, syntax::SyntaxKind::AbstractKeyword);
    result.sealedType = result.staticType || hasModifier(
        syntaxTree.modifiers, syntax::SyntaxKind::SealedKeyword);
    result.partialType = hasModifier(
        syntaxTree.modifiers, syntax::SyntaxKind::PartialKeyword);
    result.observable = hasAttributeName(
        syntaxTree.attributes, "Observable") ||
        std::any_of(
            syntaxTree.interfaces.begin(),
            syntaxTree.interfaces.end(),
            [](const syntax::TypeSyntax& iface) {
                return iface.name.text == "INotifyPropertyChanged" ||
                    iface.name.text == "ObservableObject" ||
                    iface.name.text == "ViewModelBase" ||
                    iface.name.text == "BindableBase";
            });
    result.simState = hasAttributeName(
        syntaxTree.attributes, "SimState");
    result.deterministic = hasAttributeName(
        syntaxTree.attributes, "Deterministic");
    result.moduleName = moduleName;
    result.name = syntaxTree.identifierToken.text;
    result.id = stableTypeId(result);
    result.declarationSpan = syntaxTree.identifierToken.span;
    return result;
}

TypeSymbol declareTypeShell(
    const std::string& moduleName,
    const syntax::StructDeclarationSyntax& syntaxTree) {
    TypeSymbol result;
    result.kind = TypeKind::Struct;
    result.accessibility = accessibilityFromModifiers(syntaxTree.modifiers);
    result.sealedType = true;
    result.simState = hasAttributeName(
        syntaxTree.attributes, "SimState");
    result.deterministic = hasAttributeName(
        syntaxTree.attributes, "Deterministic");
    result.moduleName = moduleName;
    result.name = syntaxTree.identifierToken.text;
    result.id = stableTypeId(result);
    result.declarationSpan = syntaxTree.identifierToken.span;
    return result;
}

TypeSymbol declareTypeShell(
    const std::string& moduleName,
    const syntax::EnumDeclarationSyntax& syntaxTree) {
    TypeSymbol result;
    result.kind = TypeKind::Enum;
    result.accessibility = accessibilityFromModifiers(syntaxTree.modifiers);
    result.sealedType = true;
    result.moduleName = moduleName;
    result.name = syntaxTree.identifierToken.text;
    result.id = stableTypeId(result);
    result.declarationSpan = syntaxTree.identifierToken.span;
    return result;
}

bool evaluateConstantSyntaxExpression(
    const syntax::ExpressionSyntax& expr,
    const TypeSymbol* currentType,
    const TypeSymbolMap& visibleTypes,
    syntax::TokenValue& outValue,
    PrimitiveType& outType,
    const std::vector<std::unordered_map<std::string, VariableSymbol>>* localScopes) {
    return evaluateConstantExpressionImpl(
        expr, currentType, visibleTypes, outValue, outType, localScopes);
}

bool resolveConstantFieldsPass(
    TypeSymbol& type,
    const TypeSymbolMap& visibleTypes) {
    bool progressed = false;
    for (auto& field : type.fields) {
        if (field.hasConstantValue || !field.initializerSyntax) continue;
        syntax::TokenValue constVal;
        PrimitiveType constType = PrimitiveType::Error;
        if (evaluateConstantExpressionImpl(
                *field.initializerSyntax, &type, visibleTypes, constVal, constType, nullptr)) {
            if (tryApplyEvaluatedConstantToField(field, std::move(constVal), constType, nullptr)) {
                progressed = true;
            }
        }
    }
    return progressed;
}

bool validateConstantFields(
    TypeSymbol& type,
    const TypeSymbolMap& visibleTypes,
    diagnostics::DiagnosticBag& diagnostics) {
    bool valid = true;
    for (auto& field : type.fields) {
        if (field.hasConstantValue || !field.initializerSyntax) continue;
        syntax::TokenValue constVal;
        PrimitiveType constType = PrimitiveType::Error;
        if (evaluateConstantExpressionImpl(
                *field.initializerSyntax, &type, visibleTypes, constVal, constType, nullptr)) {
            if (!tryApplyEvaluatedConstantToField(
                    field, std::move(constVal), constType, &diagnostics)) {
                if (field.constField || field.type != PrimitiveType::Struct) {
                    valid = false;
                }
            }
        } else if (field.constField) {
            diagnostics.report(
                "RS2407",
                "const field '" + field.name + "' requires a compile-time constant initializer",
                field.initializerSyntax->span());
            valid = false;
        }
    }
    return valid;
}

bool populateTypeFields(
    TypeSymbol& type,
    const syntax::ClassDeclarationSyntax& syntaxTree,
    const TypeSymbolMap& visibleTypes,
    diagnostics::DiagnosticBag& diagnostics,
    bool deferConstValidation) {
    return populateFields(type, syntaxTree, visibleTypes, diagnostics, deferConstValidation);
}

bool populateTypeFields(
    TypeSymbol& type,
    const syntax::StructDeclarationSyntax& syntaxTree,
    const TypeSymbolMap& visibleTypes,
    diagnostics::DiagnosticBag& diagnostics,
    bool deferConstValidation) {
    return populateFields(type, syntaxTree, visibleTypes, diagnostics, deferConstValidation);
}

bool populateEnumMembers(
    TypeSymbol& type,
    const syntax::EnumDeclarationSyntax& syntaxTree,
    diagnostics::DiagnosticBag& diagnostics) {
    bool valid = true;
    std::unordered_set<std::string> names;
    std::int64_t nextValue = 0;
    bool implicitValueAvailable = true;
    type.enumMembers.clear();
    for (const auto& member : syntaxTree.members) {
        if (!names.insert(member.identifierToken.text).second) {
            diagnostics.report(
                "RS2450",
                "duplicate enum member '" + member.identifierToken.text + "'",
                member.identifierToken.span);
            valid = false;
        }

        std::int64_t value = 0;
        if (member.valueToken &&
            std::holds_alternative<std::int64_t>(member.valueToken->value)) {
            value = std::get<std::int64_t>(member.valueToken->value);
        } else if (implicitValueAvailable) {
            value = nextValue;
        } else {
            diagnostics.report(
                "RS2451",
                "implicit enum value overflows signed 64-bit range",
                member.identifierToken.span);
            valid = false;
        }

        EnumMemberSymbol symbol;
        symbol.name = member.identifierToken.text;
        symbol.value = value;
        symbol.declarationSpan = member.identifierToken.span;
        symbol.id = fnv1a(canonicalTypeName(type) + "::enum:" + symbol.name);
        type.enumMembers.push_back(std::move(symbol));
        implicitValueAvailable =
            value != std::numeric_limits<std::int64_t>::max();
        if (implicitValueAvailable) nextValue = value + 1;
    }
    return valid;
}

std::string canonicalFunctionKey(const FunctionSymbol& function) {
    std::ostringstream out;
    out << function.moduleName << "::";
    if (!function.ownerTypeName.empty()) out << function.ownerTypeName << '.';
    out << function.name << '(';
    const auto firstVisible = function.method && !function.staticMethod &&
            !function.parameters.empty()
        ? 1u
        : 0u;
    for (std::size_t i = firstVisible;
         i < function.parameters.size(); ++i) {
        if (i != firstVisible) out << ',';
        out << parameterModifierName(function.parameters[i].modifier)
            << displayType(
                function.parameters[i].type,
                function.parameters[i].typeName);
    }
    out << ')';
    if (function.name == "op_Implicit" || function.name == "op_Explicit") {
        out << "->" << displayType(function.returnType, function.returnTypeName);
    }
    return out.str();
}

std::string canonicalFunctionSignature(const FunctionSymbol& function) {
    if (function.name == "op_Implicit" || function.name == "op_Explicit") {
        return canonicalFunctionKey(function);
    }
    return canonicalFunctionKey(function) + "->" + displayType(function.returnType, function.returnTypeName);
}

SymbolId stableFunctionId(const FunctionSymbol& function) { return fnv1a(canonicalFunctionKey(function)); }

FunctionSymbol declareFunctionSymbol(
    const std::string& moduleName,
    const syntax::FunctionDeclarationSyntax& syntaxTree,
    const TypeSymbolMap& visibleTypes,
    diagnostics::DiagnosticBag& diagnostics,
    const TypeSymbol* owner) {
    FunctionSymbol result;
    result.moduleName = moduleName;
    result.name = syntaxTree.identifierToken.text;
    result.method = owner != nullptr;
    result.staticMethod = syntaxTree.staticKeyword.has_value();
    result.accessibility = accessibilityFromModifiers(
        syntaxTree.modifiers);
    result.virtualMethod = hasModifier(
        syntaxTree.modifiers, syntax::SyntaxKind::VirtualKeyword);
    result.overrideMethod = hasModifier(
        syntaxTree.modifiers, syntax::SyntaxKind::OverrideKeyword);
    result.abstractMethod = hasModifier(
        syntaxTree.modifiers, syntax::SyntaxKind::AbstractKeyword);
    result.sealedMethod = hasModifier(
        syntaxTree.modifiers, syntax::SyntaxKind::SealedKeyword);
    result.nonDeterministic = hasAttributeName(
        syntaxTree.attributes, "NonDeterministic");
    result.deterministic = !result.nonDeterministic &&
        ((owner && owner->deterministic) ||
         hasAttributeName(syntaxTree.attributes, "Deterministic"));
    result.simCommand = hasAttributeName(
        syntaxTree.attributes, "SimCommand");
    if (owner) {
        result.declaringTypeId = owner->id;
        result.declaringTypeName = canonicalTypeName(*owner);
        result.ownerTypeName = owner->name;
        result.ownerTypeId = owner->id;
        const bool mutableStructReceiver =
            owner->sourceName != "$builtin/RealScript.Math.rs";
        appendImplicitThis(result, *owner, mutableStructReceiver);
    }
    const auto returnType = resolveType(syntaxTree.returnType, visibleTypes, true);
    result.returnType = returnType.type;
    result.returnTypeName = returnType.name;
    if (syntaxTree.refReturnKeyword) {
        result.returnModifier = ParameterModifier::Ref;
        result.storageReturnType = PrimitiveType::Object;
        result.storageReturnTypeName = referenceWrapperTypeName(
            moduleName, result.returnType, result.returnTypeName);
    }
    if (result.returnType == PrimitiveType::Error) {
        diagnostics.report("RS2200", "unknown return type '" + syntaxTree.returnType.name.text + "'", syntaxTree.returnType.span());
    }
    appendSyntaxParameters(result, syntaxTree.parameters, visibleTypes, diagnostics);
    result.id = stableFunctionId(result);
    result.declarationSpan = syntaxTree.identifierToken.span;
    result.bodySpan = syntaxTree.semicolonToken
        ? syntaxTree.semicolonToken->span
        : syntaxTree.body.span();
    for (auto& parameter : result.parameters) {
        parameter.id = fnv1a(std::to_string(result.id) + "::local:" +
            std::to_string(parameter.index) + ":" + parameter.name);
    }
    return result;
}

FunctionSymbol declareFunctionSymbol(
    const std::string& moduleName,
    const syntax::InterfaceMethodDeclarationSyntax& syntaxTree,
    const TypeSymbolMap& visibleTypes,
    diagnostics::DiagnosticBag& diagnostics,
    const TypeSymbol* owner) {
    FunctionSymbol result;
    result.moduleName = moduleName;
    result.name = syntaxTree.identifierToken.text;
    result.method = owner != nullptr;
    result.staticMethod = false;
    result.accessibility = accessibilityFromModifiers(
        syntaxTree.modifiers);
    if (owner) {
        result.declaringTypeId = owner->id;
        result.declaringTypeName = canonicalTypeName(*owner);
        result.ownerTypeName = owner->name;
        result.ownerTypeId = owner->id;
        appendImplicitThis(result, *owner);
    }
    const auto returnType = resolveType(
        syntaxTree.returnType, visibleTypes, true);
    result.returnType = returnType.type;
    result.returnTypeName = returnType.name;
    if (result.returnType == PrimitiveType::Error) {
        diagnostics.report(
            "RS2200",
            "unknown return type '" + syntaxTree.returnType.name.text + "'",
            syntaxTree.returnType.span());
    }
    appendSyntaxParameters(
        result, syntaxTree.parameters, visibleTypes, diagnostics);
    result.id = stableFunctionId(result);
    result.declarationSpan = syntaxTree.identifierToken.span;
    result.bodySpan = syntaxTree.semicolonToken.span;
    for (auto& parameter : result.parameters) {
        parameter.id = fnv1a(std::to_string(result.id) + "::local:" +
            std::to_string(parameter.index) + ":" + parameter.name);
    }
    return result;
}

FunctionSymbol declareConstructorSymbol(
    const std::string& moduleName,
    const syntax::ConstructorDeclarationSyntax& syntaxTree,
    const TypeSymbol& owner,
    const TypeSymbolMap& visibleTypes,
    diagnostics::DiagnosticBag& diagnostics) {
    FunctionSymbol result;
    result.moduleName = moduleName;
    result.staticMethod = hasModifier(
        syntaxTree.modifiers, syntax::SyntaxKind::StaticKeyword);
    result.name = result.staticMethod ? ".cctor" : ".ctor";
    result.ownerTypeName = owner.name;
    result.ownerTypeId = owner.id;
    result.returnType = (!result.staticMethod && owner.kind == TypeKind::Struct)
        ? PrimitiveType::Struct
        : PrimitiveType::Void;
    result.returnTypeName = (!result.staticMethod && owner.kind == TypeKind::Struct)
        ? canonicalTypeName(owner)
        : std::string{};
    result.method = true;
    result.constructor = true;
    result.nonDeterministic = hasAttributeName(
        syntaxTree.attributes, "NonDeterministic");
    result.deterministic = !result.nonDeterministic &&
        (owner.deterministic ||
         hasAttributeName(syntaxTree.attributes, "Deterministic"));
    result.accessibility = accessibilityFromModifiers(
        syntaxTree.modifiers);
    result.declaringTypeId = owner.id;
    result.declaringTypeName = canonicalTypeName(owner);
    if (result.staticMethod) {
        if (hasModifier(syntaxTree.modifiers, syntax::SyntaxKind::PublicKeyword) ||
            hasModifier(syntaxTree.modifiers, syntax::SyntaxKind::PrivateKeyword) ||
            hasModifier(syntaxTree.modifiers, syntax::SyntaxKind::ProtectedKeyword) ||
            hasModifier(syntaxTree.modifiers, syntax::SyntaxKind::InternalKeyword)) {
            diagnostics.report(
                "RS2497",
                "access modifiers are not allowed on static constructors",
                syntaxTree.identifierToken.span);
        }
        if (!syntaxTree.parameters.empty()) {
            diagnostics.report(
                "RS2497",
                "a static constructor must be parameterless",
                syntaxTree.identifierToken.span);
        }
        if (syntaxTree.baseKeyword || syntaxTree.thisKeyword) {
            diagnostics.report(
                "RS2497",
                "a static constructor cannot have a base constructor initializer",
                syntaxTree.baseKeyword
                    ? syntaxTree.baseKeyword->span
                    : syntaxTree.thisKeyword->span);
        }
    }
    appendImplicitThis(result, owner);
    if (!result.staticMethod) {
        appendSyntaxParameters(result, syntaxTree.parameters, visibleTypes, diagnostics);
    }
    result.id = stableFunctionId(result);
    result.declarationSpan = syntaxTree.identifierToken.span;
    result.bodySpan = syntaxTree.body.span();
    for (auto& parameter : result.parameters) {
        parameter.id = fnv1a(std::to_string(result.id) + "::local:" +
            std::to_string(parameter.index) + ":" + parameter.name);
    }
    return result;
}

PropertySymbol declarePropertySymbol(
    const std::string& moduleName,
    const syntax::PropertyDeclarationSyntax& syntaxTree,
    const TypeSymbol& owner,
    const TypeSymbolMap& visibleTypes,
    diagnostics::DiagnosticBag& diagnostics) {
    PropertySymbol result;
    result.name = syntaxTree.identifierToken.text;
    result.accessibility = accessibilityFromModifiers(
        syntaxTree.modifiers);
    result.declaringTypeId = owner.id;
    result.declaringTypeName = canonicalTypeName(owner);
    result.declarationSpan = syntaxTree.identifierToken.span;
    result.id = fnv1a(canonicalTypeName(owner) + "::property:" + result.name);
    result.staticProperty = syntaxTree.staticKeyword.has_value();
    result.observable = !result.staticProperty &&
        (owner.observable || hasAttributeName(syntaxTree.attributes, "Observable"));
    result.simState = owner.simState || hasAttributeName(syntaxTree.attributes, "SimState");
    const bool propSimCommand = hasAttributeName(syntaxTree.attributes, "SimCommand");
    const auto resolved = resolveType(syntaxTree.type, visibleTypes, false);
    result.type = resolved.type;
    result.typeName = resolved.name;
    if (resolved.type == PrimitiveType::Error) {
        diagnostics.report("RS2460", "unknown property type '" + syntaxTree.type.name.text + "'", syntaxTree.type.span());
    }
    if (syntaxTree.getter) {
        FunctionSymbol getter;
        getter.moduleName = moduleName;
        getter.name = "get_" + result.name;
        getter.ownerTypeName = owner.name;
        getter.ownerTypeId = owner.id;
        getter.declaringTypeId = owner.id;
        getter.declaringTypeName = canonicalTypeName(owner);
        getter.returnType = result.type;
        getter.returnTypeName = result.typeName;
        getter.method = true;
        getter.staticMethod = result.staticProperty;
        getter.propertyGetter = true;
        getter.deterministic = owner.deterministic;
        appendImplicitThis(getter, owner);
        getter.id = stableFunctionId(getter);
        getter.declarationSpan = syntaxTree.identifierToken.span;
        getter.bodySpan = syntaxTree.getter->span();
        for (auto& parameter : getter.parameters) {
            parameter.id = fnv1a(std::to_string(getter.id) + "::local:" +
                std::to_string(parameter.index) + ":" + parameter.name);
        }
        result.getter = std::move(getter);
    }
    if (syntaxTree.setter) {
        FunctionSymbol setter;
        setter.moduleName = moduleName;
        setter.name = "set_" + result.name;
        setter.ownerTypeName = owner.name;
        setter.ownerTypeId = owner.id;
        setter.declaringTypeId = owner.id;
        setter.declaringTypeName = canonicalTypeName(owner);
        setter.returnType = PrimitiveType::Void;
        setter.method = true;
        setter.staticMethod = result.staticProperty;
        setter.propertySetter = true;
        setter.deterministic = owner.deterministic;
        setter.simCommand = propSimCommand;
        appendImplicitThis(setter, owner);
        VariableSymbol valueParameter;
        valueParameter.name = "value";
        valueParameter.type = result.type;
        valueParameter.typeName = result.typeName;
        valueParameter.index = setter.parameters.size();
        valueParameter.storageType = valueParameter.type;
        valueParameter.storageTypeName = valueParameter.typeName;
        valueParameter.parameter = true;
        valueParameter.declarationSpan = syntaxTree.identifierToken.span;
        setter.parameters.push_back(std::move(valueParameter));
        setter.id = stableFunctionId(setter);
        setter.declarationSpan = syntaxTree.identifierToken.span;
        setter.bodySpan = syntaxTree.setter->span();
        for (auto& parameter : setter.parameters) {
            parameter.id = fnv1a(std::to_string(setter.id) + "::local:" +
                std::to_string(parameter.index) + ":" + parameter.name);
        }
        result.setter = std::move(setter);
    }
    if (!result.getter && !result.setter) {
        diagnostics.report("RS2461", "property must declare get or set accessor", syntaxTree.span());
    }
    return result;
}

} // namespace realscript::semantic
