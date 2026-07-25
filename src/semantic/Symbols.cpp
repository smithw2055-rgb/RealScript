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

ResolvedType resolveType(
    const std::string& name,
    const TypeSymbolMap& visibleTypes,
    bool allowVoid) {
    const auto primitive = resolvePrimitiveType(name);
    if (primitive != PrimitiveType::Error) {
        if (!allowVoid && primitive == PrimitiveType::Void) {
            return {};
        }
        return {primitive, {}};
    }
    const auto found = visibleTypes.find(name);
    if (found != visibleTypes.end()) {
        return {PrimitiveType::Object, canonicalTypeName(found->second)};
    }
    return {};
}

std::string displayType(PrimitiveType type, const std::string& name) {
    return type == PrimitiveType::Object ? name : primitiveTypeName(type);
}

} // namespace

const char* primitiveTypeName(PrimitiveType type) noexcept {
    switch (type) {
    case PrimitiveType::Error: return "<error>";
    case PrimitiveType::Void: return "void";
    case PrimitiveType::Bool: return "bool";
    case PrimitiveType::Int: return "int";
    case PrimitiveType::String: return "string";
    case PrimitiveType::Object: return "object";
    case PrimitiveType::Null: return "null";
    }
    return "<unknown>";
}

PrimitiveType resolvePrimitiveType(const std::string& name) noexcept {
    if (name == "void") return PrimitiveType::Void;
    if (name == "bool") return PrimitiveType::Bool;
    if (name == "int") return PrimitiveType::Int;
    if (name == "string") return PrimitiveType::String;
    return PrimitiveType::Error;
}

bool isNumericType(PrimitiveType type) noexcept {
    return type == PrimitiveType::Int;
}

ConversionKind classifyConversion(
    PrimitiveType from,
    PrimitiveType to) noexcept {
    if (from == PrimitiveType::Error || to == PrimitiveType::Error) {
        return ConversionKind::Identity;
    }
    if (from == to) {
        return ConversionKind::Identity;
    }
    if (from == PrimitiveType::Null && to == PrimitiveType::String) {
        return ConversionKind::NullToString;
    }
    if (from == PrimitiveType::Null && to == PrimitiveType::Object) {
        return ConversionKind::NullToObject;
    }
    return ConversionKind::None;
}

int conversionRank(PrimitiveType from, PrimitiveType to) noexcept {
    switch (classifyConversion(from, to)) {
    case ConversionKind::Identity: return 0;
    case ConversionKind::NullToString:
    case ConversionKind::NullToObject:
        return 1;
    case ConversionKind::None: return -1;
    }
    return -1;
}

std::string canonicalTypeName(const TypeSymbol& type) {
    return type.moduleName.empty()
        ? type.name
        : type.moduleName + "::" + type.name;
}

SymbolId stableTypeId(const TypeSymbol& type) {
    return stableTypeId(canonicalTypeName(type));
}

SymbolId stableTypeId(const std::string& canonicalName) {
    return canonicalName.empty() ? 0 : fnv1a(canonicalName);
}

TypeSymbol declareTypeShell(
    const std::string& moduleName,
    const syntax::ClassDeclarationSyntax& syntaxTree) {
    TypeSymbol result;
    result.moduleName = moduleName;
    result.name = syntaxTree.identifierToken.text;
    result.id = stableTypeId(result);
    return result;
}

bool populateTypeFields(
    TypeSymbol& type,
    const syntax::ClassDeclarationSyntax& syntaxTree,
    const TypeSymbolMap& visibleTypes,
    diagnostics::DiagnosticBag& diagnostics) {
    bool valid = true;
    std::unordered_set<std::string> fieldNames;
    type.fields.clear();
    for (std::size_t index = 0; index < syntaxTree.fields.size(); ++index) {
        const auto& fieldSyntax = syntaxTree.fields[index];
        if (!fieldNames.insert(fieldSyntax.identifierToken.text).second) {
            diagnostics.report(
                "RS2401",
                "field '" + fieldSyntax.identifierToken.text +
                    "' is already declared in type '" + type.name + "'",
                fieldSyntax.identifierToken.span);
            valid = false;
        }
        const auto resolved = resolveType(
            fieldSyntax.type.name.text,
            visibleTypes,
            false);
        if (resolved.type == PrimitiveType::Error) {
            diagnostics.report(
                "RS2402",
                "unknown field type '" + fieldSyntax.type.name.text + "'",
                fieldSyntax.type.span());
            valid = false;
        }
        type.fields.push_back({
            fieldSyntax.identifierToken.text,
            resolved.type,
            resolved.name,
            index,
        });
    }
    return valid;
}

std::string canonicalFunctionKey(const FunctionSymbol& function) {
    std::ostringstream out;
    out << function.moduleName << "::" << function.name << '(';
    for (std::size_t i = 0; i < function.parameters.size(); ++i) {
        if (i != 0) out << ',';
        out << displayType(
            function.parameters[i].type,
            function.parameters[i].typeName);
    }
    out << ')';
    return out.str();
}

std::string canonicalFunctionSignature(const FunctionSymbol& function) {
    return canonicalFunctionKey(function) + "->" +
        displayType(function.returnType, function.returnTypeName);
}

SymbolId stableFunctionId(const FunctionSymbol& function) {
    return fnv1a(canonicalFunctionKey(function));
}

FunctionSymbol declareFunctionSymbol(
    const std::string& moduleName,
    const syntax::FunctionDeclarationSyntax& syntaxTree,
    const TypeSymbolMap& visibleTypes,
    diagnostics::DiagnosticBag& diagnostics) {
    FunctionSymbol result;
    result.moduleName = moduleName;
    result.name = syntaxTree.identifierToken.text;

    const auto returnType = resolveType(
        syntaxTree.returnType.name.text,
        visibleTypes,
        true);
    result.returnType = returnType.type;
    result.returnTypeName = returnType.name;
    if (result.returnType == PrimitiveType::Error) {
        diagnostics.report(
            "RS2200",
            "unknown return type '" + syntaxTree.returnType.name.text + "'",
            syntaxTree.returnType.span());
    }

    for (std::size_t index = 0; index < syntaxTree.parameters.size(); ++index) {
        const auto& parameterSyntax = syntaxTree.parameters[index];
        const auto parameterType = resolveType(
            parameterSyntax.type.name.text,
            visibleTypes,
            false);
        if (parameterType.type == PrimitiveType::Error) {
            diagnostics.report(
                "RS2201",
                "invalid parameter type '" + parameterSyntax.type.name.text + "'",
                parameterSyntax.type.span());
        }
        result.parameters.push_back({
            parameterSyntax.identifierToken.text,
            parameterType.type,
            parameterType.name,
            index,
            true,
        });
    }

    result.id = stableFunctionId(result);
    return result;
}

} // namespace realscript::semantic
