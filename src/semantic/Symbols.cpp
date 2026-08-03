#include "realscript/semantic/Semantic.h"

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

template <typename T>
bool populateFields(
    TypeSymbol& type,
    const T& syntaxTree,
    const TypeSymbolMap& visibleTypes,
    diagnostics::DiagnosticBag& diagnostics) {
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
        field.type = resolved.type;
        field.typeName = resolved.name;
        field.index = type.fields.size();
        field.synthetic = false;
        field.declarationSpan = fieldSyntax.identifierToken.span;
        field.id = fnv1a(canonicalTypeName(type) + "::field:" + field.name);
        type.fields.push_back(std::move(field));
    }
    return valid;
}

void appendImplicitThis(FunctionSymbol& result, const TypeSymbol& owner) {
    if (result.staticMethod) return;
    VariableSymbol self;
    self.name = "this";
    self.type = typeKindPrimitive(owner.kind);
    self.typeName = canonicalTypeName(owner);
    self.index = 0;
    self.storageType = self.type;
    self.storageTypeName = self.typeName;
    self.parameter = true;
    result.parameters.push_back(std::move(self));
}

void appendSyntaxParameters(
    FunctionSymbol& result,
    const std::vector<syntax::ParameterSyntax>& parameters,
    const TypeSymbolMap& visibleTypes,
    diagnostics::DiagnosticBag& diagnostics) {
    for (const auto& parameterSyntax : parameters) {
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

const char* primitiveTypeName(PrimitiveType type) noexcept {
    switch (type) {
    case PrimitiveType::Error: return "<error>";
    case PrimitiveType::Void: return "void";
    case PrimitiveType::Bool: return "bool";
    case PrimitiveType::Int: return "int";
    case PrimitiveType::Long: return "long";
    case PrimitiveType::Double: return "double";
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
    if (name == "int" || name == "byte" || name == "sbyte" ||
        name == "short" || name == "ushort" || name == "char") {
        return PrimitiveType::Int;
    }
    if (name == "long" || name == "uint" || name == "ulong") {
        return PrimitiveType::Long;
    }
    if (name == "double" || name == "float") {
        return PrimitiveType::Double;
    }
    if (name == "string") return PrimitiveType::String;
    if (name == "handle") return PrimitiveType::Handle;
    return PrimitiveType::Error;
}

bool isNumericType(PrimitiveType type) noexcept {
    return type == PrimitiveType::Int ||
        type == PrimitiveType::Long ||
        type == PrimitiveType::Double;
}

bool isIntegralType(PrimitiveType type) noexcept {
    return type == PrimitiveType::Int || type == PrimitiveType::Long;
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
    result.moduleName = moduleName;
    result.name = syntaxTree.identifierToken.text;
    result.id = stableTypeId(result);
    result.declarationSpan = syntaxTree.identifierToken.span;
    return result;
}

bool populateTypeFields(
    TypeSymbol& type,
    const syntax::ClassDeclarationSyntax& syntaxTree,
    const TypeSymbolMap& visibleTypes,
    diagnostics::DiagnosticBag& diagnostics) {
    return populateFields(type, syntaxTree, visibleTypes, diagnostics);
}

bool populateTypeFields(
    TypeSymbol& type,
    const syntax::StructDeclarationSyntax& syntaxTree,
    const TypeSymbolMap& visibleTypes,
    diagnostics::DiagnosticBag& diagnostics) {
    return populateFields(type, syntaxTree, visibleTypes, diagnostics);
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
    return out.str();
}

std::string canonicalFunctionSignature(const FunctionSymbol& function) {
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
    if (owner) {
        result.ownerTypeName = owner->name;
        result.ownerTypeId = owner->id;
        appendImplicitThis(result, *owner);
    }
    const auto returnType = resolveType(syntaxTree.returnType, visibleTypes, true);
    result.returnType = returnType.type;
    result.returnTypeName = returnType.name;
    if (result.returnType == PrimitiveType::Error) {
        diagnostics.report("RS2200", "unknown return type '" + syntaxTree.returnType.name.text + "'", syntaxTree.returnType.span());
    }
    appendSyntaxParameters(result, syntaxTree.parameters, visibleTypes, diagnostics);
    result.id = stableFunctionId(result);
    result.declarationSpan = syntaxTree.identifierToken.span;
    result.bodySpan = syntaxTree.body.span();
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
    result.name = ".ctor";
    result.ownerTypeName = owner.name;
    result.ownerTypeId = owner.id;
    result.returnType = owner.kind == TypeKind::Struct
        ? PrimitiveType::Struct
        : PrimitiveType::Void;
    result.returnTypeName = owner.kind == TypeKind::Struct
        ? canonicalTypeName(owner)
        : std::string{};
    result.method = true;
    result.constructor = true;
    appendImplicitThis(result, owner);
    appendSyntaxParameters(result, syntaxTree.parameters, visibleTypes, diagnostics);
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
    result.declarationSpan = syntaxTree.identifierToken.span;
    result.id = fnv1a(canonicalTypeName(owner) + "::property:" + result.name);
    result.staticProperty = syntaxTree.staticKeyword.has_value();
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
        getter.returnType = result.type;
        getter.returnTypeName = result.typeName;
        getter.method = true;
        getter.staticMethod = result.staticProperty;
        getter.propertyGetter = true;
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
        setter.returnType = PrimitiveType::Void;
        setter.method = true;
        setter.staticMethod = result.staticProperty;
        setter.propertySetter = true;
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
