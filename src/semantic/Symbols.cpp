#include "realscript/semantic/Semantic.h"

#include <sstream>

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

} // namespace

const char* primitiveTypeName(PrimitiveType type) noexcept {
    switch (type) {
    case PrimitiveType::Error: return "<error>";
    case PrimitiveType::Void: return "void";
    case PrimitiveType::Bool: return "bool";
    case PrimitiveType::Int: return "int";
    case PrimitiveType::String: return "string";
    case PrimitiveType::Null: return "null";
    case PrimitiveType::Object: return "object";
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
    return ConversionKind::None;
}

int conversionRank(PrimitiveType from, PrimitiveType to) noexcept {
    switch (classifyConversion(from, to)) {
    case ConversionKind::Identity: return 0;
    case ConversionKind::NullToString: return 1;
    case ConversionKind::None: return -1;
    }
    return -1;
}

std::string canonicalFunctionKey(const FunctionSymbol& function) {
    std::ostringstream out;
    out << function.moduleName << "::" << function.name << '(';
    for (std::size_t i = 0; i < function.parameters.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        out << primitiveTypeName(function.parameters[i].type);
    }
    out << ')';
    return out.str();
}

std::string canonicalFunctionSignature(const FunctionSymbol& function) {
    return canonicalFunctionKey(function) + "->" +
        primitiveTypeName(function.returnType);
}

SymbolId stableFunctionId(const FunctionSymbol& function) {
    return fnv1a(canonicalFunctionKey(function));
}

FunctionSymbol declareFunctionSymbol(
    const std::string& moduleName,
    const syntax::FunctionDeclarationSyntax& syntaxTree,
    diagnostics::DiagnosticBag& diagnostics) {
    FunctionSymbol result;
    result.moduleName = moduleName;
    result.name = syntaxTree.identifierToken.text;
    result.returnType = resolvePrimitiveType(syntaxTree.returnType.name.text);

    if (result.returnType == PrimitiveType::Error) {
        diagnostics.report(
            "RS2200",
            "type '" + syntaxTree.returnType.name.text +
                "' is not implemented in the Phase 1C profile",
            syntaxTree.returnType.span());
    }

    for (std::size_t index = 0; index < syntaxTree.parameters.size(); ++index) {
        const auto& parameterSyntax = syntaxTree.parameters[index];
        auto parameterType = resolvePrimitiveType(parameterSyntax.type.name.text);
        if (parameterType == PrimitiveType::Error ||
            parameterType == PrimitiveType::Void) {
            diagnostics.report(
                "RS2201",
                "invalid parameter type '" + parameterSyntax.type.name.text + "'",
                parameterSyntax.type.span());
            parameterType = PrimitiveType::Error;
        }
        result.parameters.push_back({
            parameterSyntax.identifierToken.text,
            parameterType,
            index,
            true,
        });
    }

    result.id = stableFunctionId(result);
    return result;
}

} // namespace realscript::semantic
