#include "realscript/semantic/Semantic.h"

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

} // namespace realscript::semantic
