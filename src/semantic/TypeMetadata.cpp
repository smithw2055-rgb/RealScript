#include "realscript/semantic/Semantic.h"

#include <algorithm>
#include <string_view>

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

namespace {

[[nodiscard]] constexpr std::uint32_t alignUpBlittable(
    std::uint32_t offset,
    std::uint32_t align) noexcept {
    return (offset + align - 1u) & ~(align - 1u);
}

[[nodiscard]] bool isNativeHandleName(std::string_view name) noexcept {
    if (name == "NativeHandle") return true;
    constexpr std::string_view suffix = "::NativeHandle";
    return name.size() >= suffix.size() &&
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
}

[[nodiscard]] bool classifyBuiltinMathStruct(
    SymbolId typeId,
    std::uint32_t& outSize,
    std::uint32_t& outAlign) noexcept {
    static const SymbolId kFixed = stableTypeId("RealScript.Math::Fixed");
    static const SymbolId kFixed16 = stableTypeId("RealScript.Math::Fixed16");
    static const SymbolId kFixed64 = stableTypeId("RealScript.Math::Fixed64");
    static const SymbolId kVec2 = stableTypeId("RealScript.Math::Vec2");
    static const SymbolId kVec3 = stableTypeId("RealScript.Math::Vec3");
    static const SymbolId kQuat = stableTypeId("RealScript.Math::Quat");
    static const SymbolId kVec2f = stableTypeId("RealScript.Math::Vec2f");
    static const SymbolId kVec3f = stableTypeId("RealScript.Math::Vec3f");
    static const SymbolId kQuatf = stableTypeId("RealScript.Math::Quatf");
    if (typeId == kFixed)   { outSize = 4u;  outAlign = 4u; return true; }
    if (typeId == kFixed16) { outSize = 8u;  outAlign = 8u; return true; }
    if (typeId == kFixed64) { outSize = 8u;  outAlign = 8u; return true; }
    if (typeId == kVec2)    { outSize = 8u;  outAlign = 4u; return true; }
    if (typeId == kVec3)    { outSize = 12u; outAlign = 4u; return true; }
    if (typeId == kQuat)    { outSize = 16u; outAlign = 4u; return true; }
    if (typeId == kVec2f)   { outSize = 8u;  outAlign = 4u; return true; }
    if (typeId == kVec3f)   { outSize = 12u; outAlign = 4u; return true; }
    if (typeId == kQuatf)   { outSize = 16u; outAlign = 4u; return true; }
    return false;
}

template <typename LookupFn>
[[nodiscard]] std::optional<BlittableStructLayout> computeBlittableLayoutImpl(
    const TypeSymbol& type,
    const LookupFn& lookupType,
    std::unordered_set<SymbolId>& visiting,
    std::uint32_t depth,
    std::string* outReason) {
    const auto fail = [&](std::string msg) -> std::optional<BlittableStructLayout> {
        if (outReason && outReason->empty()) *outReason = std::move(msg);
        return std::nullopt;
    };

    const SymbolId typeId = type.id != 0 ? type.id : stableTypeId(type);
    if (depth > 16u) {
        return fail("blittable struct nesting exceeds maximum depth (16)");
    }
    if (type.kind != TypeKind::Struct) {
        return fail("type '" + canonicalTypeName(type) + "' is not a struct");
    }
    if (type.hasValueLifecycle) {
        return fail("struct '" + canonicalTypeName(type) + "' has [ValueLifecycle] and cannot be blittable");
    }
    if (isNativeHandleName(type.name) || isNativeHandleName(canonicalTypeName(type))) {
        return fail("type '" + canonicalTypeName(type) + "' is a native handle and cannot be blittable");
    }
    if (typeId != 0 && !visiting.insert(typeId).second) {
        return fail("recursive struct cycle involving '" + canonicalTypeName(type) + "'");
    }

    struct ScopeErase {
        std::unordered_set<SymbolId>& set;
        SymbolId id;
        ~ScopeErase() {
            if (id != 0) set.erase(id);
        }
    } guard{visiting, typeId};

    std::uint32_t offset = 0u;
    std::uint32_t maxAlign = 1u;
    std::vector<BlittableFieldLayout> fields;

    for (const auto& field : type.fields) {
        if (field.staticField || field.constField) continue;
        if (field.name.rfind("$sequence_", 0) == 0) {
            return fail("blittable struct '" + canonicalTypeName(type) + "' cannot contain state-machine fields");
        }
        if (isNativeHandleName(field.typeName) || field.type == PrimitiveType::Handle) {
            return fail("field '" + field.name + "' uses NativeHandle which is not blittable");
        }

        std::uint32_t fSize = 0u;
        std::uint32_t fAlign = 1u;
        SymbolId fTypeId = 0u;
        bool isNested = false;

        switch (field.type) {
        case PrimitiveType::Bool:
        case PrimitiveType::Byte:
        case PrimitiveType::SByte:
            fSize = 1u;
            fAlign = 1u;
            break;
        case PrimitiveType::Short:
        case PrimitiveType::UShort:
        case PrimitiveType::Char:
            fSize = 2u;
            fAlign = 2u;
            break;
        case PrimitiveType::Int:
        case PrimitiveType::UInt:
        case PrimitiveType::Float:
            fSize = 4u;
            fAlign = 4u;
            break;
        case PrimitiveType::Enum:
            if (field.typeName.empty()) {
                return fail("enum field '" + field.name + "' is missing canonical type identity");
            }
            fTypeId = stableTypeId(field.typeName);
            fSize = 4u;
            fAlign = 4u;
            break;
        case PrimitiveType::Long:
        case PrimitiveType::ULong:
        case PrimitiveType::Double:
            fSize = 8u;
            fAlign = 8u;
            break;
        case PrimitiveType::Struct: {
            if (field.typeName.empty()) {
                return fail("struct field '" + field.name + "' has unresolved type");
            }
            fTypeId = stableTypeId(field.typeName);
            if (classifyBuiltinMathStruct(fTypeId, fSize, fAlign)) {
                break;
            }
            const TypeSymbol* nestedType = lookupType(fTypeId, field.typeName);
            if (!nestedType) {
                return fail("struct field '" + field.name + "' references unknown type '" + field.typeName + "'");
            }
            if (nestedType->hasValueLifecycle) {
                return fail("field '" + field.name + "' has [ValueLifecycle] struct type '" +
                    field.typeName + "' which is not blittable");
            }
            auto nestedLayout = computeBlittableLayoutImpl(
                *nestedType, lookupType, visiting, depth + 1u, outReason);
            if (!nestedLayout) {
                return std::nullopt;
            }
            fSize = nestedLayout->strideBytes;
            fAlign = nestedLayout->alignBytes;
            fTypeId = nestedLayout->typeId;
            isNested = true;
            break;
        }
        default:
            return fail("field '" + field.name + "' of type '" +
                (field.typeName.empty() ? std::string(primitiveTypeName(field.type)) : field.typeName) +
                "' is not blittable");
        }

        offset = alignUpBlittable(offset, fAlign);
        maxAlign = std::max(maxAlign, fAlign);
        fields.push_back(BlittableFieldLayout{
            field.name,
            field.type,
            fTypeId,
            offset,
            fSize,
            fAlign,
            isNested,
        });
        offset += fSize;
    }

    if (fields.empty()) {
        return fail("blittable struct '" + canonicalTypeName(type) + "' must have at least one instance field");
    }
    std::uint32_t mathSize = 0u;
    std::uint32_t mathAlign = 1u;
    if (typeId != 0 && classifyBuiltinMathStruct(typeId, mathSize, mathAlign)) {
        if (fields.size() == 1u) {
            fields[0].sizeBytes = mathSize;
            fields[0].alignBytes = mathAlign;
        }
        return BlittableStructLayout{typeId, mathSize, mathAlign, std::move(fields)};
    }
    const std::uint32_t stride = alignUpBlittable(offset, maxAlign);
    if (stride == 0u || stride > (1u << 20u)) {
        return fail("blittable struct '" + canonicalTypeName(type) + "' exceeds maximum blittable stride");
    }

    return BlittableStructLayout{typeId, stride, maxAlign, std::move(fields)};
}

} // namespace

std::optional<BlittableStructLayout> computeBlittableStructLayout(
    const TypeSymbol& type,
    const TypeSymbolMap& visibleTypes,
    std::string* outReason) {
    if (outReason) outReason->clear();
    std::unordered_map<SymbolId, const TypeSymbol*> byId;
    byId.reserve(visibleTypes.size());
    for (const auto& [name, sym] : visibleTypes) {
        (void)name;
        const SymbolId sid = sym.id != 0 ? sym.id : stableTypeId(sym);
        if (sid != 0) byId[sid] = &sym;
    }
    const auto lookup = [&](SymbolId tid, const std::string& name) -> const TypeSymbol* {
        if (const auto it = byId.find(tid); it != byId.end()) return it->second;
        if (const auto it = visibleTypes.find(name); it != visibleTypes.end()) return &it->second;
        return nullptr;
    };
    std::unordered_set<SymbolId> visiting;
    return computeBlittableLayoutImpl(type, lookup, visiting, 0u, outReason);
}

std::optional<BlittableStructLayout> computeBlittableStructLayout(
    const TypeSymbol& type,
    const std::unordered_map<SymbolId, const TypeSymbol*>& typesById,
    std::string* outReason) {
    if (outReason) outReason->clear();
    const auto lookup = [&](SymbolId tid, const std::string&) -> const TypeSymbol* {
        const auto it = typesById.find(tid);
        return it != typesById.end() ? it->second : nullptr;
    };
    std::unordered_set<SymbolId> visiting;
    return computeBlittableLayoutImpl(type, lookup, visiting, 0u, outReason);
}

std::optional<BlittableStructLayout> computeBlittableStructLayout(
    const TypeSymbol& type,
    const std::vector<TypeSymbol>& moduleTypes,
    std::string* outReason) {
    if (outReason) outReason->clear();
    std::unordered_map<SymbolId, const TypeSymbol*> byId;
    byId.reserve(moduleTypes.size());
    for (const auto& sym : moduleTypes) {
        const SymbolId sid = sym.id != 0 ? sym.id : stableTypeId(sym);
        if (sid != 0) byId[sid] = &sym;
    }
    return computeBlittableStructLayout(type, byId, outReason);
}

} // namespace realscript::semantic
