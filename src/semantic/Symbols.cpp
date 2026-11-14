#include "realscript/semantic/Semantic.h"

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
        case syntax::SyntaxKind::IntegerLiteralToken: {
            const bool hasU = lit.text.find('u') != std::string::npos ||
                lit.text.find('U') != std::string::npos;
            const bool hasL = lit.text.find('l') != std::string::npos ||
                lit.text.find('L') != std::string::npos;
            if (hasU && hasL) outType = PrimitiveType::ULong;
            else if (hasU) outType = PrimitiveType::UInt;
            else if (hasL) outType = PrimitiveType::Long;
            else outType = PrimitiveType::Int;
            return true;
        }
        case syntax::SyntaxKind::CharLiteralToken:
            outType = PrimitiveType::Char;
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
            field.initializerSyntax ? field.initializerSyntax->span() : field.declarationSpan,
            diagnostics::DiagnosticSeverity::Error,
            field.sourceName);
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
                fieldSyntax.identifierToken.span,
                diagnostics::DiagnosticSeverity::Error,
                fieldSyntax.originSourceName);
            valid = false;
        }
        const auto resolved = resolveType(fieldSyntax.type, visibleTypes, false);
        if (resolved.type == PrimitiveType::Error) {
            diagnostics.report(
                "RS2402",
                "unknown field type '" + fieldSyntax.type.name.text + "'",
                fieldSyntax.type.span(),
                diagnostics::DiagnosticSeverity::Error,
                fieldSyntax.originSourceName);
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
                fieldSyntax.identifierToken.span,
                diagnostics::DiagnosticSeverity::Error,
                fieldSyntax.originSourceName);
            valid = false;
        }
        if (field.constField) {
            field.staticField = true;
        }
        if (type.staticType && !field.staticField && !field.constField) {
            diagnostics.report(
                "RS2489",
                "cannot declare instance field '" + field.name +
                    "' in static class '" + type.name + "'",
                fieldSyntax.identifierToken.span,
                diagnostics::DiagnosticSeverity::Error,
                fieldSyntax.originSourceName);
            valid = false;
        }
        field.initializerSyntax = fieldSyntax.initializer.get();
        if (!fieldSyntax.initializer && field.constField) {
            diagnostics.report(
                "RS2407",
                "const field '" + field.name + "' requires an initializer",
                fieldSyntax.identifierToken.span,
                diagnostics::DiagnosticSeverity::Error,
                fieldSyntax.originSourceName);
            valid = false;
        }
        field.declarationSpan = fieldSyntax.identifierToken.span;
        field.sourceName = fieldSyntax.originSourceName;
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
    diagnostics::DiagnosticBag& diagnostics,
    const TypeSymbol* owner = nullptr) {
    bool sawOptional = false;
    for (std::size_t syntaxIndex = 0;
         syntaxIndex < parameters.size(); ++syntaxIndex) {
        const auto& parameterSyntax = parameters[syntaxIndex];
        if (parameterSyntax.hasThisModifier()) {
            const auto thisSpan = parameterSyntax.thisKeyword
                ? parameterSyntax.thisKeyword->span
                : parameterSyntax.modifierToken->span;
            if (syntaxIndex == 0 && (!result.method || result.staticMethod) && !result.constructor) {
                result.extensionMethod = true;
                if (parameterSyntax.modifierToken &&
                    parameterSyntax.modifierToken->kind == syntax::SyntaxKind::OutKeyword) {
                    diagnostics.report(
                        "RS8920",
                        "extension receiver parameter cannot use 'out' modifier",
                        parameterSyntax.modifierToken->span);
                }
            } else {
                diagnostics.report(
                    "RS8920",
                    "'this' parameter modifier is only valid on the first parameter of a static or free function",
                    thisSpan);
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
        } else if (isStackOnlyArrayType(parameterType.type, parameterType.name, visibleTypes)) {
            diagnostics.report(
                "RS2695",
                "stack-only struct '" +
                    stackOnlyTypeDisplayName(parameterType.type, parameterType.name, visibleTypes) +
                    "' cannot be an array element type",
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
                case syntax::SyntaxKind::CharLiteralToken:
                    parameter.defaultValueType = PrimitiveType::Char;
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
            if (parameter.defaultValueType == PrimitiveType::Error) {
                syntax::TokenValue evalVal;
                PrimitiveType evalType = PrimitiveType::Error;
                if (evaluateConstantExpressionImpl(
                        *parameterSyntax.defaultValue,
                        owner,
                        visibleTypes,
                        evalVal,
                        evalType,
                        nullptr)) {
                    parameter.defaultValue = std::move(evalVal);
                    parameter.defaultValueType = evalType;
                }
            }
            if (parameter.defaultValueType == PrimitiveType::Error ||
                parameter.paramsArray) {
                diagnostics.report(
                    "RS8917",
                    "optional parameter default must be a compile-time constant and params cannot have a default",
                    parameterSyntax.defaultValue->span());
            } else {
                const auto conversion = classifyConversion(
                    parameter.defaultValueType, parameter.type);
                const bool nullReference =
                    parameter.defaultValueType == PrimitiveType::Null &&
                    isReferenceType(parameter.type);
                bool implicitConstNumeric = false;
                if ((parameter.defaultValueType == PrimitiveType::Int ||
                     parameter.defaultValueType == PrimitiveType::Long) &&
                    std::holds_alternative<std::int64_t>(parameter.defaultValue)) {
                    const auto v = std::get<std::int64_t>(parameter.defaultValue);
                    switch (parameter.type) {
                    case PrimitiveType::Byte: implicitConstNumeric = (v >= 0 && v <= 255); break;
                    case PrimitiveType::SByte: implicitConstNumeric = (v >= -128 && v <= 127); break;
                    case PrimitiveType::Short: implicitConstNumeric = (v >= -32768 && v <= 32767); break;
                    case PrimitiveType::UShort:
                    case PrimitiveType::Char: implicitConstNumeric = (v >= 0 && v <= 65535); break;
                    case PrimitiveType::Int:
                        implicitConstNumeric =
                            v >= std::numeric_limits<std::int32_t>::min() &&
                            v <= std::numeric_limits<std::int32_t>::max();
                        break;
                    case PrimitiveType::UInt:
                    case PrimitiveType::ULong: implicitConstNumeric = (v >= 0); break;
                    case PrimitiveType::Long: implicitConstNumeric = true; break;
                    default: break;
                    }
                    if (implicitConstNumeric) {
                        parameter.defaultValueType = parameter.type;
                    }
                } else if (parameter.defaultValueType == PrimitiveType::Double &&
                           parameter.type == PrimitiveType::Float &&
                           std::holds_alternative<double>(parameter.defaultValue)) {
                    const auto v = std::get<double>(parameter.defaultValue);
                    if (std::isfinite(v) && std::abs(v) <= std::numeric_limits<float>::max()) {
                        implicitConstNumeric = true;
                        parameter.defaultValueType = PrimitiveType::Float;
                    }
                }
                if (conversion == ConversionKind::None && !nullReference &&
                    !implicitConstNumeric &&
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
            if (isStackOnlyType(parameter.type, parameter.typeName, visibleTypes)) {
                diagnostics.report(
                    "RS2697",
                    "stack-only struct '" +
                        stackOnlyTypeDisplayName(
                            parameter.type, parameter.typeName, visibleTypes) +
                        "' cannot be passed by 'ref' or 'out'",
                    parameterSyntax.identifierToken.span);
            }
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

namespace {

bool hasAttributeName(
    const std::vector<syntax::AttributeListSyntax>& lists,
    std::string_view name) noexcept {
    for (const auto& list : lists) {
        for (const auto& attribute : list.attributes) {
            if (attribute.nameToken.text == name) return true;
        }
    }
    return false;
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
    while (!currentName.empty()) {
        const auto found = visibleTypes.find(currentName);
        if (found == visibleTypes.end() ||
            !visited.insert(found->second.id).second) {
            return false;
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
    return false;
}

namespace {

bool isStackOnlyTypeImpl(
    PrimitiveType type,
    const std::string& typeName,
    const TypeSymbolMap& visibleTypes,
    std::unordered_set<SymbolId>& visited) {
    if (type != PrimitiveType::Struct || typeName.empty()) {
        return false;
    }
    const auto found = visibleTypes.find(typeName);
    if (found == visibleTypes.end()) {
        return false;
    }
    if (found->second.stackOnly) {
        return true;
    }
    const SymbolId visitId = found->second.id != 0
        ? found->second.id
        : stableTypeId(found->second);
    if (!visited.insert(visitId).second) {
        return false;
    }
    for (const auto& field : found->second.fields) {
        if (field.staticField || field.constField) continue;
        if (isStackOnlyTypeImpl(field.type, field.typeName, visibleTypes, visited)) {
            return true;
        }
    }
    return false;
}

} // namespace

bool isStackOnlyType(
    PrimitiveType type,
    const std::string& typeName,
    const TypeSymbolMap& visibleTypes) noexcept {
    std::unordered_set<SymbolId> visited;
    return isStackOnlyTypeImpl(type, typeName, visibleTypes, visited);
}

bool isStackOnlyArrayType(
    PrimitiveType type,
    const std::string& typeName,
    const TypeSymbolMap& visibleTypes) noexcept {
    if (type != PrimitiveType::Array || typeName.empty()) {
        return false;
    }
    PrimitiveType elemType = PrimitiveType::Error;
    std::string elemTypeName;
    if (!decodeArrayTypeName(typeName, elemType, elemTypeName)) {
        return false;
    }
    return isStackOnlyType(elemType, elemTypeName, visibleTypes);
}

std::string stackOnlyTypeDisplayName(
    PrimitiveType type,
    const std::string& typeName,
    const TypeSymbolMap& visibleTypes) {
    std::string lookupName = typeName;
    if (type == PrimitiveType::Array) {
        PrimitiveType elemType = PrimitiveType::Error;
        std::string elemTypeName;
        if (decodeArrayTypeName(typeName, elemType, elemTypeName)) {
            lookupName = std::move(elemTypeName);
        }
    }
    const auto found = visibleTypes.find(lookupName);
    if (found != visibleTypes.end() && !found->second.name.empty()) {
        return found->second.name;
    }
    return !lookupName.empty() ? lookupName : std::string(primitiveTypeName(type));
}

bool propagateStackOnlyTypes(
    std::vector<TypeSymbol>& types,
    TypeSymbolMap& visibleTypes) {
    bool anyChanged = false;
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& type : types) {
            if (type.kind != TypeKind::Struct || type.stackOnly) {
                continue;
            }
            bool containsStackOnly = false;
            for (const auto& field : type.fields) {
                if (field.staticField || field.constField) continue;
                if (isStackOnlyType(field.type, field.typeName, visibleTypes)) {
                    containsStackOnly = true;
                    break;
                }
            }
            if (containsStackOnly) {
                type.stackOnly = true;
                visibleTypes[type.name] = type;
                visibleTypes[canonicalTypeName(type)] = type;
                changed = true;
                anyChanged = true;
            }
        }
    }
    return anyChanged;
}

bool validateStackOnlyFields(
    const TypeSymbol& type,
    const TypeSymbolMap& visibleTypes,
    diagnostics::DiagnosticBag& diagnostics) {
    bool valid = true;
    for (const auto& field : type.fields) {
        if (field.declaringTypeId != type.id) {
            continue;
        }
        if (field.name.rfind("$sequence_", 0) == 0) {
            continue;
        }
        const auto& sourceName = field.sourceName.empty()
            ? type.sourceName
            : field.sourceName;
        if (isStackOnlyArrayType(field.type, field.typeName, visibleTypes)) {
            diagnostics.report(
                "RS2695",
                "stack-only struct '" +
                    stackOnlyTypeDisplayName(field.type, field.typeName, visibleTypes) +
                    "' cannot be an array element type",
                field.declarationSpan,
                diagnostics::DiagnosticSeverity::Error,
                sourceName);
            valid = false;
            continue;
        }
        if (isStackOnlyType(field.type, field.typeName, visibleTypes) &&
            (type.kind == TypeKind::Class || field.staticField)) {
            diagnostics.report(
                "RS2692",
                "stack-only struct '" +
                    stackOnlyTypeDisplayName(field.type, field.typeName, visibleTypes) +
                    "' cannot be declared as a class instance or static field",
                field.declarationSpan,
                diagnostics::DiagnosticSeverity::Error,
                sourceName);
            valid = false;
        }
    }
    return valid;
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
    result.awaitableProtocol = hasAttributeName(
        syntaxTree.attributes, "Awaitable");
    result.hasBlittableAttribute =
        hasAttributeName(syntaxTree.attributes, "Blittable") ||
        hasAttributeName(syntaxTree.attributes, "BlittableAttribute");
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
    result.partialType = hasModifier(
        syntaxTree.modifiers, syntax::SyntaxKind::PartialKeyword);
    result.stackOnly = hasAttributeName(
        syntaxTree.attributes, "StackOnly");
    result.hasValueLifecycle =
        hasAttributeName(syntaxTree.attributes, "ValueLifecycle") ||
        hasAttributeName(syntaxTree.attributes, "RefCounted");
    result.hasBlittableAttribute =
        hasAttributeName(syntaxTree.attributes, "Blittable") ||
        hasAttributeName(syntaxTree.attributes, "BlittableAttribute");
    result.awaitableProtocol = hasAttributeName(
        syntaxTree.attributes, "Awaitable");
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
    result.hasBlittableAttribute =
        hasAttributeName(syntaxTree.attributes, "Blittable") ||
        hasAttributeName(syntaxTree.attributes, "BlittableAttribute");
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
                field.initializerSyntax->span(),
                diagnostics::DiagnosticSeverity::Error,
                field.sourceName);
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
    result.asyncFunction = hasModifier(
        syntaxTree.modifiers, syntax::SyntaxKind::AsyncKeyword);
    result.nonDeterministic = hasAttributeName(
        syntaxTree.attributes, "NonDeterministic");
    result.deterministic = !result.nonDeterministic &&
        ((owner && owner->deterministic) ||
         hasAttributeName(syntaxTree.attributes, "Deterministic"));
    if (owner) {
        result.declaringTypeId = owner->id;
        result.declaringTypeName = canonicalTypeName(*owner);
        result.ownerTypeName = owner->name;
        result.ownerTypeId = owner->id;
        const bool mutableStructReceiver =
            owner->sourceName != "$builtin/RealScript.Math.rscript" &&
            owner->sourceName != "$builtin/RealScript.Async.rscript" &&
            owner->name.rfind("Awaitable__", 0) != 0;
        appendImplicitThis(result, *owner, mutableStructReceiver);
    }
    const auto returnType = resolveType(syntaxTree.returnType, visibleTypes, true);
    result.returnType = returnType.type;
    result.returnTypeName = returnType.name;
    if (syntaxTree.refReturnKeyword) {
        result.returnModifier = ParameterModifier::Ref;
        if (isStackOnlyType(result.returnType, result.returnTypeName, visibleTypes)) {
            diagnostics.report(
                "RS2697",
                "stack-only struct '" +
                    stackOnlyTypeDisplayName(
                        result.returnType, result.returnTypeName, visibleTypes) +
                    "' cannot be returned by 'ref'",
                syntaxTree.refReturnKeyword->span);
        }
        result.storageReturnType = PrimitiveType::Object;
        result.storageReturnTypeName = referenceWrapperTypeName(
            moduleName, result.returnType, result.returnTypeName);
    }
    if (result.returnType == PrimitiveType::Error) {
        diagnostics.report("RS2200", "unknown return type '" + syntaxTree.returnType.name.text + "'", syntaxTree.returnType.span());
    } else if (isStackOnlyArrayType(result.returnType, result.returnTypeName, visibleTypes)) {
        diagnostics.report(
            "RS2695",
            "stack-only struct '" +
                stackOnlyTypeDisplayName(
                    result.returnType, result.returnTypeName, visibleTypes) +
                "' cannot be an array element type",
            syntaxTree.returnType.span());
    }
    appendSyntaxParameters(result, syntaxTree.parameters, visibleTypes, diagnostics, owner);
    if (result.asyncFunction) {
        if (result.abstractMethod) {
            diagnostics.report(
                "RS2603",
                "abstract method '" + result.name + "' cannot be marked 'async'",
                syntaxTree.identifierToken.span);
        }
        if (syntaxTree.refReturnKeyword) {
            diagnostics.report(
                "RS2601",
                "async function '" + result.name + "' cannot return by reference",
                syntaxTree.refReturnKeyword->span);
        }
        for (const auto& param : result.parameters) {
            if (!param.name.empty() && param.name != "this" &&
                (param.modifier == ParameterModifier::Ref ||
                 param.modifier == ParameterModifier::Out)) {
                diagnostics.report(
                    "RS2602",
                    "async function '" + result.name + "' cannot declare ref or out parameter '" +
                        param.name + "'",
                    param.declarationSpan);
            }
            if (!param.name.empty() && param.name != "this" &&
                isStackOnlyType(param.type, param.typeName, visibleTypes)) {
                diagnostics.report(
                    "RS2696",
                    "async function '" + result.name + "' cannot declare stack-only parameter '" +
                        param.name + "'",
                    param.declarationSpan);
            }
        }
        bool validAsyncRet = false;
        if (result.returnType == PrimitiveType::Struct) {
            if (result.returnTypeName == "RealScript.Async::Awaitable" ||
                result.returnTypeName == "Awaitable") {
                validAsyncRet = true;
            } else {
                const auto foundRet = visibleTypes.find(result.returnTypeName);
                if (foundRet != visibleTypes.end() &&
                    (foundRet->second.name.rfind("Awaitable__", 0) == 0 ||
                     foundRet->second.name == "Awaitable" ||
                     foundRet->second.awaitableProtocol)) {
                    validAsyncRet = true;
                }
            }
        }
        if (result.returnType != PrimitiveType::Error && !validAsyncRet) {
            diagnostics.report(
                "RS2600",
                "return type of async function '" + result.name +
                    "' must be 'Awaitable' or 'Awaitable<T>'",
                syntaxTree.returnType.span());
        }
    }
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
    } else if (isStackOnlyArrayType(result.returnType, result.returnTypeName, visibleTypes)) {
        diagnostics.report(
            "RS2695",
            "stack-only struct '" +
                stackOnlyTypeDisplayName(
                    result.returnType, result.returnTypeName, visibleTypes) +
                "' cannot be an array element type",
            syntaxTree.returnType.span());
    }
    appendSyntaxParameters(
        result, syntaxTree.parameters, visibleTypes, diagnostics, owner);
    result.id = stableFunctionId(result);
    result.declarationSpan = syntaxTree.identifierToken.span;
    result.bodySpan = syntaxTree.semicolonToken
        ? syntaxTree.semicolonToken->span
        : (syntaxTree.body ? syntaxTree.body->span() : syntaxTree.closeParenToken.span);
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
    if (hasModifier(syntaxTree.modifiers, syntax::SyntaxKind::AsyncKeyword)) {
        diagnostics.report(
            "RS2603",
            "constructor cannot be marked 'async'",
            syntaxTree.identifierToken.span);
    }
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
        if (syntaxTree.baseKeyword) {
            diagnostics.report(
                "RS2497",
                "a static constructor cannot have a base constructor initializer",
                syntaxTree.baseKeyword->span);
        }
    }
    appendImplicitThis(result, owner);
    if (!result.staticMethod) {
        appendSyntaxParameters(result, syntaxTree.parameters, visibleTypes, diagnostics, &owner);
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
    const auto resolved = resolveType(syntaxTree.type, visibleTypes, false);
    result.type = resolved.type;
    result.typeName = resolved.name;
    if (resolved.type == PrimitiveType::Error) {
        diagnostics.report("RS2460", "unknown property type '" + syntaxTree.type.name.text + "'", syntaxTree.type.span());
    } else if (isStackOnlyArrayType(result.type, result.typeName, visibleTypes)) {
        diagnostics.report(
            "RS2695",
            "stack-only struct '" +
                stackOnlyTypeDisplayName(result.type, result.typeName, visibleTypes) +
                "' cannot be an array element type",
            syntaxTree.type.span());
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
