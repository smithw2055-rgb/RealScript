#include "realscript/runtime/ParallelJob.h"
#include "ExecutionMirror.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>

namespace realscript::runtime {
namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
constexpr std::uint64_t kUnknownArrayOriginBit = std::uint64_t{1} << 63u;
constexpr std::size_t kMaxTrackedOriginParams = 62;

inline void fnvMixBytes(std::uint64_t& hash, const void* data, std::size_t size) noexcept {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= kFnvPrime;
    }
}

template <typename T>
inline void fnvMixScalar(std::uint64_t& hash, T value) noexcept {
    using RawT = std::remove_cv_t<T>;
    if constexpr (std::is_same_v<RawT, bool>) {
        const std::uint8_t b = value ? 1u : 0u;
        fnvMixBytes(hash, &b, sizeof(b));
    } else if constexpr (std::is_enum_v<RawT>) {
        using U = std::make_unsigned_t<std::underlying_type_t<RawT>>;
        const U encoded = static_cast<U>(value);
        for (std::size_t i = 0; i < sizeof(U); ++i) {
            const std::uint8_t b = static_cast<std::uint8_t>((encoded >> (i * 8u)) & 0xFFu);
            fnvMixBytes(hash, &b, 1u);
        }
    } else {
        using U = std::make_unsigned_t<RawT>;
        const U encoded = static_cast<U>(value);
        for (std::size_t i = 0; i < sizeof(U); ++i) {
            const std::uint8_t b = static_cast<std::uint8_t>((encoded >> (i * 8u)) & 0xFFu);
            fnvMixBytes(hash, &b, 1u);
        }
    }
}

[[nodiscard]] inline double canonicalizeDoubleForStrict(double v, bool strict) noexcept {
    if (!strict) return v;
    if (std::isnan(v)) {
        double qnan = 0.0;
        const std::uint64_t bits = 0x7ff8000000000000ULL;
        std::memcpy(&qnan, &bits, sizeof(qnan));
        return qnan;
    }
    return (v == 0.0) ? 0.0 : v;
}

[[nodiscard]] inline float canonicalizeFloatForStrict(float v, bool strict) noexcept {
    if (!strict) return v;
    if (std::isnan(v)) {
        float qnan = 0.0f;
        const std::uint32_t bits = 0x7fc00000U;
        std::memcpy(&qnan, &bits, sizeof(qnan));
        return qnan;
    }
    return (v == 0.0f) ? 0.0f : v;
}

[[nodiscard]] std::string_view stripModuleQualifier(std::string_view name) noexcept {
    const auto pos = name.rfind("::");
    return (pos == std::string_view::npos) ? name : name.substr(pos + 2u);
}

enum class BuiltInPrngOp : std::uint8_t {
    None = 0,
    NextUInt32,
    NextBits32,
    NextInt,
    NextRange,
    NextByte255,
    NextDouble,
};

[[nodiscard]] BuiltInPrngOp classifyBuiltInPrngName(std::string_view name) noexcept {
    std::string_view tail = stripModuleQualifier(name);
    if (const auto dot = tail.rfind('.'); dot != std::string_view::npos) {
        const std::string_view owner = tail.substr(0, dot);
        if (owner == "ParallelRandom") {
            const std::string_view method = tail.substr(dot + 1u);
            if (method == "NextUInt32") return BuiltInPrngOp::NextUInt32;
            if (method == "NextBits32") return BuiltInPrngOp::NextBits32;
            if (method == "NextInt") return BuiltInPrngOp::NextInt;
            if (method == "NextRange") return BuiltInPrngOp::NextRange;
            if (method == "NextByte255") return BuiltInPrngOp::NextByte255;
            if (method == "NextDouble") return BuiltInPrngOp::NextDouble;
        }
    }
    if (tail == "ParallelRandomUInt32") return BuiltInPrngOp::NextUInt32;
    if (tail == "ParallelRandomBits32") return BuiltInPrngOp::NextBits32;
    if (tail == "ParallelRandomInt") return BuiltInPrngOp::NextInt;
    if (tail == "ParallelRandomRange") return BuiltInPrngOp::NextRange;
    if (tail == "ParallelRandomByte255") return BuiltInPrngOp::NextByte255;
    if (tail == "ParallelRandomDouble") return BuiltInPrngOp::NextDouble;
    return BuiltInPrngOp::None;
}

thread_local DeterministicPRNG* tlsActiveChunkPrng = nullptr;

DeterministicPRNG& currentThreadParallelPrng() noexcept {
    if (tlsActiveChunkPrng != nullptr) {
        return *tlsActiveChunkPrng;
    }
    thread_local DeterministicPRNG fallbackPrng = [] {
        DeterministicPRNG p;
        p.seed(0x53434257u);
        return p;
    }();
    return fallbackPrng;
}

struct ScopedChunkPrngBinding {
    DeterministicPRNG* prev = nullptr;
    explicit ScopedChunkPrngBinding(DeterministicPRNG& active) noexcept
        : prev(tlsActiveChunkPrng) {
        tlsActiveChunkPrng = &active;
    }
    ~ScopedChunkPrngBinding() noexcept {
        tlsActiveChunkPrng = prev;
    }
    ScopedChunkPrngBinding(const ScopedChunkPrngBinding&) = delete;
    ScopedChunkPrngBinding& operator=(const ScopedChunkPrngBinding&) = delete;
};

[[nodiscard]] std::optional<Value> invokeBuiltInPrngOp(
    BuiltInPrngOp op,
    ValueSpan arguments,
    RuntimeError& error) noexcept {
    auto& prng = currentThreadParallelPrng();
    switch (op) {
    case BuiltInPrngOp::NextUInt32: {
        if (!arguments.empty()) {
            error.code = ErrorCode::InvalidArguments;
            error.message = "ParallelRandomUInt32 expects 0 arguments";
            return std::nullopt;
        }
        return Value{static_cast<std::int64_t>(prng.nextUInt32() & 0x7FFFFFFFu)};
    }
    case BuiltInPrngOp::NextBits32: {
        if (!arguments.empty()) {
            error.code = ErrorCode::InvalidArguments;
            error.message = "ParallelRandomBits32 expects 0 arguments";
            return std::nullopt;
        }
        return Value{static_cast<std::int64_t>(static_cast<std::int32_t>(prng.nextUInt32()))};
    }
    case BuiltInPrngOp::NextByte255: {
        if (!arguments.empty()) {
            error.code = ErrorCode::InvalidArguments;
            error.message = "ParallelRandomByte255 expects 0 arguments";
            return std::nullopt;
        }
        return Value{static_cast<std::int64_t>(prng.nextByte255())};
    }
    case BuiltInPrngOp::NextInt: {
        if (arguments.size() != 2u) {
            error.code = ErrorCode::InvalidArguments;
            error.message = "ParallelRandomInt expects (int minInclusive, int maxExclusive)";
            return std::nullopt;
        }
        const auto* lo = std::get_if<std::int64_t>(&arguments[0]);
        const auto* hi = std::get_if<std::int64_t>(&arguments[1]);
        if (!lo || !hi) {
            error.code = ErrorCode::TypeMismatch;
            error.message = "ParallelRandomInt arguments must be int";
            return std::nullopt;
        }
        if (*hi <= *lo) {
            return Value{static_cast<std::int64_t>(*lo)};
        }
        const std::uint64_t span =
            static_cast<std::uint64_t>(*hi) - static_cast<std::uint64_t>(*lo);
        const std::uint32_t r = prng.nextUInt32();
        const std::int64_t sampled = static_cast<std::int64_t>(
            static_cast<std::uint64_t>(*lo) + (static_cast<std::uint64_t>(r) % span));
        return Value{sampled};
    }
    case BuiltInPrngOp::NextRange: {
        if (arguments.size() != 2u) {
            error.code = ErrorCode::InvalidArguments;
            error.message = "ParallelRandomRange expects (int minInclusive, int maxInclusive)";
            return std::nullopt;
        }
        const auto* lo = std::get_if<std::int64_t>(&arguments[0]);
        const auto* hi = std::get_if<std::int64_t>(&arguments[1]);
        if (!lo || !hi) {
            error.code = ErrorCode::TypeMismatch;
            error.message = "ParallelRandomRange arguments must be int";
            return std::nullopt;
        }
        if (*hi <= *lo) {
            return Value{static_cast<std::int64_t>(*lo)};
        }
        const std::uint64_t diff =
            static_cast<std::uint64_t>(*hi) - static_cast<std::uint64_t>(*lo);
        if (diff == std::numeric_limits<std::uint64_t>::max()) {
            return Value{static_cast<std::int64_t>(static_cast<std::int32_t>(prng.nextUInt32()))};
        }
        const std::uint64_t span = diff + 1ULL;
        const std::uint32_t r = prng.nextUInt32();
        const std::int64_t sampled = static_cast<std::int64_t>(
            static_cast<std::uint64_t>(*lo) + (static_cast<std::uint64_t>(r) % span));
        return Value{sampled};
    }
    case BuiltInPrngOp::NextDouble: {
        if (!arguments.empty()) {
            error.code = ErrorCode::InvalidArguments;
            error.message = "ParallelRandomDouble expects 0 arguments";
            return std::nullopt;
        }
        // Exact 53-bit IEEE-754 mantissa scaling by 2^-53
        const std::uint64_t hi27 = static_cast<std::uint64_t>(prng.nextUInt32() >> 5u);
        const std::uint64_t lo26 = static_cast<std::uint64_t>(prng.nextUInt32() >> 6u);
        const std::uint64_t mantissa53 = (hi27 << 26u) | lo26;
        constexpr double kInv2Pow53 = 1.0 / 9007199254740992.0;
        return Value{static_cast<double>(mantissa53) * kInv2Pow53};
    }
    case BuiltInPrngOp::None:
        break;
    }
    error.code = ErrorCode::ExternalFunctionUnresolved;
    error.message = "unknown ParallelRandom operation";
    return std::nullopt;
}

[[nodiscard]] bool isInlineBuiltInMathStruct(semantic::SymbolId typeId) noexcept {
    return typeId != 0 && (
        typeId == fixedTypeId() ||
        typeId == fixed16TypeId() ||
        typeId == fixed64TypeId() ||
        typeId == vec2TypeId() ||
        typeId == vec3TypeId() ||
        typeId == quatTypeId() ||
        typeId == vec2fTypeId() ||
        typeId == vec3fTypeId() ||
        typeId == quatfTypeId());
}

constexpr std::size_t kMaxParallelFlatStructDepth = 8u;
constexpr std::size_t kMaxParallelFlatStructLeafSlots = 256u;

[[nodiscard]] bool isFlatStackSafeCustomStruct(
    const ProgramImage& program,
    semantic::SymbolId typeId,
    std::size_t depth = 0u,
    std::size_t* ioLeafSlots = nullptr) noexcept {
    if (isInlineBuiltInMathStruct(typeId)) {
        if (ioLeafSlots) {
            if (++(*ioLeafSlots) > kMaxParallelFlatStructLeafSlots) return false;
        }
        return true;
    }
    if (typeId == 0 || depth > kMaxParallelFlatStructDepth) return false;
    const auto* type = program.resolveType(typeId);
    if (!type ||
        type->kind != semantic::TypeKind::Struct ||
        isSynthesizedRefWrapperTypeName(type->name)) {
        return false;
    }

    if (depth == 0u) {
        const FlatStructSchema* flatSchema = findFlatStructSchemaByTypeId(typeId);
        if (!flatSchema || flatSchema->containsManagedReferences) {
            return false;
        }
    }

    std::size_t rootLeafSlots = 0u;
    std::size_t* const leafSlots = ioLeafSlots ? ioLeafSlots : &rootLeafSlots;

    std::size_t nonStaticFields = 0u;
    for (const auto& field : type->fields) {
        if (field.staticField || field.constField) continue;
        ++nonStaticFields;
        switch (field.type) {
        case semantic::PrimitiveType::Bool:
        case semantic::PrimitiveType::Byte:
        case semantic::PrimitiveType::SByte:
        case semantic::PrimitiveType::Short:
        case semantic::PrimitiveType::UShort:
        case semantic::PrimitiveType::Int:
        case semantic::PrimitiveType::UInt:
        case semantic::PrimitiveType::Long:
        case semantic::PrimitiveType::ULong:
        case semantic::PrimitiveType::Float:
        case semantic::PrimitiveType::Double:
        case semantic::PrimitiveType::Char:
        case semantic::PrimitiveType::Enum:
            if (++(*leafSlots) > kMaxParallelFlatStructLeafSlots) {
                return false;
            }
            break;
        case semantic::PrimitiveType::Struct: {
            const semantic::SymbolId nestedTid =
                field.typeName.empty() ? 0 : semantic::stableTypeId(field.typeName);
            if (!isFlatStackSafeCustomStruct(program, nestedTid, depth + 1u, leafSlots)) {
                return false;
            }
            break;
        }
        default:
            return false;
        }
    }
    if (nonStaticFields == 0u) {
        if (++(*leafSlots) > kMaxParallelFlatStructLeafSlots) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool isInlineReturnSafeStruct(
    const ProgramImage& program,
    semantic::SymbolId typeId) noexcept {
    if (isInlineBuiltInMathStruct(typeId)) return true;
    if (typeId == 0) return false;
    const auto* type = program.resolveType(typeId);
    if (!type ||
        type->kind != semantic::TypeKind::Struct ||
        isSynthesizedRefWrapperTypeName(type->name)) {
        return false;
    }

    std::size_t inlineFieldCount = 0u;
    for (const auto& field : type->fields) {
        if (field.staticField || field.constField) continue;
        if (++inlineFieldCount > StructValue::MaxInlineFields) {
            return false;
        }
        switch (field.type) {
        case semantic::PrimitiveType::Bool:
        case semantic::PrimitiveType::Byte:
        case semantic::PrimitiveType::SByte:
        case semantic::PrimitiveType::Short:
        case semantic::PrimitiveType::UShort:
        case semantic::PrimitiveType::Int:
        case semantic::PrimitiveType::UInt:
        case semantic::PrimitiveType::Long:
        case semantic::PrimitiveType::ULong:
        case semantic::PrimitiveType::Float:
        case semantic::PrimitiveType::Double:
        case semantic::PrimitiveType::Char:
            break;
        case semantic::PrimitiveType::Struct: {
            const semantic::SymbolId nestedTid =
                field.typeName.empty() ? 0 : semantic::stableTypeId(field.typeName);
            if (nestedTid != fixedTypeId() &&
                nestedTid != fixed16TypeId() &&
                nestedTid != fixed64TypeId() &&
                nestedTid != vec2TypeId() &&
                nestedTid != vec2fTypeId()) {
                return false;
            }
            break;
        }
        default:
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::unordered_set<semantic::SymbolId> buildAllowedPackedArrayTypeIds(
    const ProgramImage& program) {
    std::unordered_set<semantic::SymbolId> allowed;
    static constexpr std::array<semantic::PrimitiveType, 12> kPackedPrims{{
        semantic::PrimitiveType::Bool,
        semantic::PrimitiveType::Byte,
        semantic::PrimitiveType::SByte,
        semantic::PrimitiveType::Short,
        semantic::PrimitiveType::UShort,
        semantic::PrimitiveType::Char,
        semantic::PrimitiveType::Int,
        semantic::PrimitiveType::UInt,
        semantic::PrimitiveType::Long,
        semantic::PrimitiveType::ULong,
        semantic::PrimitiveType::Float,
        semantic::PrimitiveType::Double,
    }};
    for (const auto prim : kPackedPrims) {
        allowed.insert(semantic::stableTypeId(semantic::arrayTypeName(prim, {})));
    }
    for (const auto& mod : program.modules()) {
        for (const auto& type : mod.types) {
            if (type.kind == semantic::TypeKind::Enum) {
                const std::string canon = semantic::canonicalTypeName(type);
                allowed.insert(semantic::stableTypeId(
                    semantic::arrayTypeName(semantic::PrimitiveType::Enum, canon)));
                if (!type.name.empty()) {
                    allowed.insert(semantic::stableTypeId(
                        semantic::arrayTypeName(semantic::PrimitiveType::Enum, type.name)));
                }
            }
        }
    }
    return allowed;
}

[[nodiscard]] std::unordered_set<semantic::SymbolId> buildAllowedRawColumnArrayTypeIds(
    const ProgramImage& program) {
    std::unordered_set<semantic::SymbolId> allowed;
    auto insertStructArray = [&](const std::string& elemName) {
        if (elemName.empty()) return;
        allowed.insert(semantic::stableTypeId(
            semantic::arrayTypeName(semantic::PrimitiveType::Struct, elemName)));
    };
    insertStructArray("Fixed");
    insertStructArray("RealScript.Math::Fixed");
    insertStructArray("Fixed16");
    insertStructArray("RealScript.Math::Fixed16");
    insertStructArray("Fixed64");
    insertStructArray("RealScript.Math::Fixed64");
    insertStructArray("Vec2");
    insertStructArray("RealScript.Math::Vec2");
    insertStructArray("Vec2f");
    insertStructArray("RealScript.Math::Vec2f");
    insertStructArray("Vec3");
    insertStructArray("RealScript.Math::Vec3");
    insertStructArray("Vec3f");
    insertStructArray("RealScript.Math::Vec3f");
    insertStructArray("Quat");
    insertStructArray("RealScript.Math::Quat");
    insertStructArray("Quatf");
    insertStructArray("RealScript.Math::Quatf");
    for (const auto& mod : program.modules()) {
        for (const auto& type : mod.types) {
            if (type.kind == semantic::TypeKind::Struct &&
                isFlatStackSafeCustomStruct(program, type.id)) {
                insertStructArray(semantic::canonicalTypeName(type));
                insertStructArray(type.name);
            }
        }
    }
    return allowed;
}

[[nodiscard]] bool tryParseReadOnlyEcsColDenseId(
    std::string_view subOp,
    semantic::SymbolId& outDenseId) {
    constexpr std::string_view prefix = "ecs_col::";
    if (subOp.rfind(prefix, 0) != 0) return false;
    const auto rest = subOp.substr(prefix.size());
    const auto sep = rest.find("::");
    if (sep == std::string_view::npos || sep == 0) return false;
    const auto idxStr = rest.substr(0, sep);
    std::size_t colIdx = 0;
    for (const char ch : idxStr) {
        if (ch < '0' || ch > '9') return false;
        colIdx = colIdx * 10u + static_cast<std::size_t>(ch - '0');
    }
    const auto specView = rest.substr(sep + 2u);
    const auto p1 = specView.find(';');
    const std::string_view colsPart =
        p1 == std::string_view::npos ? specView : specView.substr(0, p1);
    if (p1 != std::string_view::npos) {
        const auto afterCols = specView.substr(p1 + 1u);
        for (const char ch : afterCols) {
            if (ch != ';') return false; // `with` / `without` filters require dynamic sparse queries
        }
    }
    std::vector<std::string> cols;
    std::size_t pos = 0;
    while (pos <= colsPart.size()) {
        const auto comma = colsPart.find(',', pos);
        const auto len = (comma == std::string_view::npos) ? (colsPart.size() - pos) : (comma - pos);
        if (len > 0) {
            cols.emplace_back(colsPart.substr(pos, len));
        }
        if (comma == std::string_view::npos) break;
        pos = comma + 1u;
    }
    if (colIdx >= cols.size()) return false;
    outDenseId = semantic::stableTypeId("$ecs_column::" + cols[colIdx]);
    return true;
}

[[nodiscard]] bool isAllowedKernelReturnType(
    const ProgramImage& program,
    semantic::PrimitiveType type,
    semantic::SymbolId typeId) noexcept {
    switch (type) {
    case semantic::PrimitiveType::Void:
    case semantic::PrimitiveType::Bool:
    case semantic::PrimitiveType::Byte:
    case semantic::PrimitiveType::SByte:
    case semantic::PrimitiveType::Short:
    case semantic::PrimitiveType::UShort:
    case semantic::PrimitiveType::Int:
    case semantic::PrimitiveType::UInt:
    case semantic::PrimitiveType::Long:
    case semantic::PrimitiveType::ULong:
    case semantic::PrimitiveType::Float:
    case semantic::PrimitiveType::Double:
    case semantic::PrimitiveType::Char:
    case semantic::PrimitiveType::Enum:
        return true;
    case semantic::PrimitiveType::Struct:
        return isInlineReturnSafeStruct(program, typeId);
    default:
        return false;
    }
}

[[nodiscard]] bool tryParseNumericStaticFieldId(
    std::string_view digits,
    semantic::SymbolId& outId) noexcept {
    if (digits.empty()) return false;
    std::uint64_t value = 0;
    for (const char ch : digits) {
        if (ch < '0' || ch > '9') return false;
        const std::uint64_t next = value * 10u + static_cast<std::uint64_t>(ch - '0');
        value = next;
    }
    outId = static_cast<semantic::SymbolId>(value);
    return true;
}

struct FunctionOriginSummary {
    const ProgramImage::FunctionLocation* location = nullptr;
    std::vector<std::uint64_t> paramOrigins;
    std::vector<std::uint64_t> localOrigins;
    std::vector<std::uint64_t> regOrigins;
    std::uint64_t returnOrigins = 0;
};

} // namespace

const char* parallelVerifyViolationKindName(
    ParallelVerifyViolationKind kind) noexcept {
    switch (kind) {
    case ParallelVerifyViolationKind::None: return "none";
    case ParallelVerifyViolationKind::FunctionNotFound: return "function-not-found";
    case ParallelVerifyViolationKind::InvalidKernelSignature: return "invalid-kernel-signature";
    case ParallelVerifyViolationKind::UnsupportedParameterType: return "unsupported-parameter-type";
    case ParallelVerifyViolationKind::NonPackedArrayParameter: return "non-packed-array-parameter";
    case ParallelVerifyViolationKind::HeapObjectAllocation: return "heap-object-allocation";
    case ParallelVerifyViolationKind::HeapArrayAllocation: return "heap-array-allocation";
    case ParallelVerifyViolationKind::DelegateAllocationOrCombine: return "delegate-allocation-or-combine";
    case ParallelVerifyViolationKind::DelegateInvocation: return "delegate-invocation";
    case ParallelVerifyViolationKind::InstanceFieldMutation: return "instance-field-mutation";
    case ParallelVerifyViolationKind::StaticFieldMutation: return "static-field-mutation";
    case ParallelVerifyViolationKind::AsyncOrAwaitOperation: return "async-or-await-operation";
    case ParallelVerifyViolationKind::StringAllocationOrMutation: return "string-allocation-or-mutation";
    case ParallelVerifyViolationKind::ExceptionThrow: return "exception-throw";
    case ParallelVerifyViolationKind::NonPackedArrayElementAccess: return "non-packed-array-element-access";
    case ParallelVerifyViolationKind::UntrackedArrayMutation: return "untracked-array-mutation";
    case ParallelVerifyViolationKind::DynamicPolymorphicCall: return "dynamic-polymorphic-call";
    case ParallelVerifyViolationKind::ImpureExternalHostCall: return "impure-external-host-call";
    case ParallelVerifyViolationKind::ImpureRuntimeIntrinsic: return "impure-runtime-intrinsic";
    case ParallelVerifyViolationKind::NonInlineStructMutation: return "non-inline-struct-mutation";
    }
    return "unknown";
}

bool isPackedParallelElementType(semantic::PrimitiveType elementType) noexcept {
    switch (elementType) {
    case semantic::PrimitiveType::Bool:
    case semantic::PrimitiveType::Byte:
    case semantic::PrimitiveType::SByte:
    case semantic::PrimitiveType::Short:
    case semantic::PrimitiveType::UShort:
    case semantic::PrimitiveType::Char:
    case semantic::PrimitiveType::Int:
    case semantic::PrimitiveType::UInt:
    case semantic::PrimitiveType::Long:
    case semantic::PrimitiveType::ULong:
    case semantic::PrimitiveType::Float:
    case semantic::PrimitiveType::Double:
    case semantic::PrimitiveType::Enum:
        return true;
    default:
        return false;
    }
}

bool isBuiltInParallelPrngFunctionName(
    std::string_view canonicalOrShortName) noexcept {
    return classifyBuiltInPrngName(canonicalOrShortName) != BuiltInPrngOp::None;
}

std::string ParallelJobVerificationReport::summaryMessage() const {
    if (valid) {
        return "parallel job verification succeeded for '" + qualifiedName + "'";
    }
    if (violations.empty()) {
        return "parallel job verification failed for '" + qualifiedName + "'";
    }
    std::ostringstream oss;
    oss << "parallel job verification failed (" << violations.size() << " violation"
        << (violations.size() == 1u ? "" : "s") << "): ";
    for (std::size_t i = 0; i < violations.size(); ++i) {
        if (i != 0u) oss << "; ";
        const auto& v = violations[i];
        oss << "[" << parallelVerifyViolationKindName(v.kind) << " in "
            << (v.functionName.empty() ? "<unknown>" : v.functionName) << "] "
            << v.message;
    }
    return oss.str();
}

ParallelJobVerificationReport verifyParallelJobFunction(
    const ProgramImage& program,
    const std::string& qualifiedName,
    const ParallelJobVerifyOptions& options) {
    if (const auto symId = program.findFunction(qualifiedName)) {
        return verifyParallelJobFunction(program, *symId, options);
    }
    ParallelJobVerificationReport report;
    report.valid = false;
    report.qualifiedName = qualifiedName;
    report.violations.push_back(ParallelVerifyViolation{
        0,
        qualifiedName,
        0,
        0,
        {},
        ParallelVerifyViolationKind::FunctionNotFound,
        "target kernel function '" + qualifiedName + "' was not found in ProgramImage",
    });
    return report;
}

ParallelJobVerificationReport verifyParallelJobFunction(
    const ProgramImage& program,
    semantic::SymbolId symbolId,
    const ParallelJobVerifyOptions& options) {
    ParallelJobVerificationReport report;
    report.entrySymbolId = symbolId;

    const auto* entryLoc = program.resolveFunction(symbolId);
    if (!entryLoc || !entryLoc->function || !entryLoc->module) {
        report.violations.push_back(ParallelVerifyViolation{
            symbolId,
            {},
            0,
            0,
            {},
            ParallelVerifyViolationKind::FunctionNotFound,
            "target kernel symbolId was not found in ProgramImage",
        });
        return report;
    }

    const auto& entryFn = *entryLoc->function;
    report.qualifiedName = entryLoc->qualifiedName;
    report.returnType = entryFn.returnType;
    report.returnTypeId = entryFn.returnTypeId;
    report.parameterTypes = entryFn.parameterTypes;
    report.parameterTypeIds = entryFn.parameterTypeIds;

    auto addViolation = [&](const ProgramImage::FunctionLocation& loc,
                            bytecode::BlockId blockId,
                            std::uint32_t instIdx,
                            std::string opcodeStr,
                            ParallelVerifyViolationKind kind,
                            std::string msg) {
        report.violations.push_back(ParallelVerifyViolation{
            loc.function ? loc.function->symbolId : 0,
            loc.qualifiedName,
            blockId,
            instIdx,
            std::move(opcodeStr),
            kind,
            std::move(msg),
        });
    };

    // 1. Validate kernel entry signature & parameter types
    const bool hasIndex0 = !entryFn.parameterTypes.empty() &&
        entryFn.parameterTypes[0] == semantic::PrimitiveType::Int;
    const bool hasRange01 = entryFn.parameterTypes.size() >= 2u &&
        entryFn.parameterTypes[0] == semantic::PrimitiveType::Int &&
        entryFn.parameterTypes[1] == semantic::PrimitiveType::Int;

    report.supportsElementKernel = hasIndex0;
    report.supportsRangeKernel = hasRange01;

    if (options.expectedKind == ParallelKernelKind::Element && !hasIndex0) {
        addViolation(
            *entryLoc, 0, 0, "signature",
            ParallelVerifyViolationKind::InvalidKernelSignature,
            "Element parallel kernel must accept 'int index' as parameter 0");
    } else if (options.expectedKind == ParallelKernelKind::Range && !hasRange01) {
        addViolation(
            *entryLoc, 0, 0, "signature",
            ParallelVerifyViolationKind::InvalidKernelSignature,
            "Range parallel kernel must accept '(int start, int count, ...)' as parameters 0 and 1");
    } else if (options.expectedKind == ParallelKernelKind::Auto && !hasIndex0) {
        addViolation(
            *entryLoc, 0, 0, "signature",
            ParallelVerifyViolationKind::InvalidKernelSignature,
            "Parallel kernel must start with 'int index' or '(int start, int count)'");
    }

    if (!isAllowedKernelReturnType(program, entryFn.returnType, entryFn.returnTypeId)) {
        addViolation(
            *entryLoc, 0, 0, "signature",
            ParallelVerifyViolationKind::InvalidKernelSignature,
            "Parallel kernel return type must be void, primitive/enum, or inline value struct");
    }

    const auto allowedPackedArrayIds = buildAllowedPackedArrayTypeIds(program);
    std::unordered_set<semantic::SymbolId> allowedRawColumnArrayIds;
    if (options.allowRawColumnArrays) {
        allowedRawColumnArrayIds = buildAllowedRawColumnArrayTypeIds(program);
    }
    report.allowRawColumnArrays = options.allowRawColumnArrays;
    report.allowReadOnlyStaticColumnLoads = options.allowReadOnlyStaticColumnLoads;
    auto isAllowedArrayTypeId = [&](semantic::SymbolId arrTid) -> bool {
        if (arrTid == 0) return false;
        if (allowedPackedArrayIds.find(arrTid) != allowedPackedArrayIds.end()) return true;
        return options.allowRawColumnArrays &&
               allowedRawColumnArrayIds.find(arrTid) != allowedRawColumnArrayIds.end();
    };

    for (std::size_t p = 0; p < entryFn.parameterTypes.size(); ++p) {
        const auto pType = entryFn.parameterTypes[p];
        const auto pTid = p < entryFn.parameterTypeIds.size() ? entryFn.parameterTypeIds[p] : 0;
        switch (pType) {
        case semantic::PrimitiveType::Bool:
        case semantic::PrimitiveType::Byte:
        case semantic::PrimitiveType::SByte:
        case semantic::PrimitiveType::Short:
        case semantic::PrimitiveType::UShort:
        case semantic::PrimitiveType::Int:
        case semantic::PrimitiveType::UInt:
        case semantic::PrimitiveType::Long:
        case semantic::PrimitiveType::ULong:
        case semantic::PrimitiveType::Float:
        case semantic::PrimitiveType::Double:
        case semantic::PrimitiveType::Char:
        case semantic::PrimitiveType::Enum:
            break;
        case semantic::PrimitiveType::Struct:
            if (!isFlatStackSafeCustomStruct(program, pTid)) {
                addViolation(
                    *entryLoc, 0, 0, "param",
                    ParallelVerifyViolationKind::UnsupportedParameterType,
                    "Struct parameter " + std::to_string(p) +
                        " contains managed references or exceeds flat-stack parallel limits");
            }
            break;
        case semantic::PrimitiveType::Object: {
            const auto* objType = pTid != 0 ? program.resolveType(pTid) : nullptr;
            if (objType && objType->delegateType) {
                addViolation(
                    *entryLoc, 0, 0, "param",
                    ParallelVerifyViolationKind::UnsupportedParameterType,
                    "Delegate parameter " + std::to_string(p) +
                        " is forbidden in ParallelJob kernels");
            }
            break;
        }
        case semantic::PrimitiveType::Array:
            if (!isAllowedArrayTypeId(pTid)) {
                addViolation(
                    *entryLoc, 0, 0, "param",
                    ParallelVerifyViolationKind::NonPackedArrayParameter,
                    "Array parameter " + std::to_string(p) +
                        " is not a P0 packed primitive/enum array");
            }
            break;
        default:
            addViolation(
                *entryLoc, 0, 0, "param",
                ParallelVerifyViolationKind::UnsupportedParameterType,
                "Parameter " + std::to_string(p) + " has unsupported parallel type '" +
                    std::string(semantic::primitiveTypeName(pType)) + "'");
            break;
        }
    }

    // 2. Recursive call-graph bytecode inspection
    std::unordered_map<semantic::SymbolId, FunctionOriginSummary> visitedFunctions;
    std::vector<semantic::SymbolId> visitWorklist;
    std::unordered_set<semantic::SymbolId> staticFieldIdSet;

    auto enqueueFunction = [&](semantic::SymbolId symId) {
        if (visitedFunctions.find(symId) != visitedFunctions.end()) return;
        const auto* loc = program.resolveFunction(symId);
        if (!loc || !loc->function || !loc->module) return;
        FunctionOriginSummary summary;
        summary.location = loc;
        summary.paramOrigins.assign(loc->function->parameterTypes.size(), 0ULL);
        summary.localOrigins.assign(loc->function->localTypes.size(), 0ULL);
        summary.regOrigins.assign(loc->function->registerTypes.size(), 0ULL);
        visitedFunctions.emplace(symId, std::move(summary));
        visitWorklist.push_back(symId);
    };

    enqueueFunction(symbolId);
    {
        auto& entrySummary = visitedFunctions.at(symbolId);
        for (std::size_t p = 0; p < entryFn.parameterTypes.size(); ++p) {
            if (entryFn.parameterTypes[p] == semantic::PrimitiveType::Array) {
                entrySummary.paramOrigins[p] = (p < kMaxTrackedOriginParams)
                    ? (std::uint64_t{1} << p)
                    : kUnknownArrayOriginBit;
            }
        }
    }

    for (std::size_t cursor = 0; cursor < visitWorklist.size(); ++cursor) {
        const semantic::SymbolId curSymId = visitWorklist[cursor];
        const auto* loc = visitedFunctions.at(curSymId).location;
        const auto& mod = *loc->module;
        const auto& fn = *loc->function;

        if (curSymId != symbolId) {
            bool validHelperReturn = true;
            switch (fn.returnType) {
            case semantic::PrimitiveType::Void:
            case semantic::PrimitiveType::Bool:
            case semantic::PrimitiveType::Byte:
            case semantic::PrimitiveType::SByte:
            case semantic::PrimitiveType::Short:
            case semantic::PrimitiveType::UShort:
            case semantic::PrimitiveType::Int:
            case semantic::PrimitiveType::UInt:
            case semantic::PrimitiveType::Long:
            case semantic::PrimitiveType::ULong:
            case semantic::PrimitiveType::Float:
            case semantic::PrimitiveType::Double:
            case semantic::PrimitiveType::Char:
            case semantic::PrimitiveType::Enum:
                break;
            case semantic::PrimitiveType::Struct:
                validHelperReturn = isFlatStackSafeCustomStruct(program, fn.returnTypeId);
                break;
            case semantic::PrimitiveType::Object: {
                const auto* objType =
                    fn.returnTypeId != 0 ? program.resolveType(fn.returnTypeId) : nullptr;
                validHelperReturn = !(objType && objType->delegateType);
                break;
            }
            case semantic::PrimitiveType::Array:
                validHelperReturn = fn.returnTypeId != 0 &&
                    allowedPackedArrayIds.find(fn.returnTypeId) != allowedPackedArrayIds.end();
                break;
            default:
                validHelperReturn = false;
                break;
            }
            if (!validHelperReturn) {
                addViolation(
                    *loc, 0, 0, "signature",
                    ParallelVerifyViolationKind::InvalidKernelSignature,
                    "Parallel helper return type uses unsupported or unsafe type '" +
                        std::string(semantic::primitiveTypeName(fn.returnType)) + "'");
            }

            for (std::size_t p = 0; p < fn.parameterTypes.size(); ++p) {
                const auto pType = fn.parameterTypes[p];
                const auto pTid = p < fn.parameterTypeIds.size() ? fn.parameterTypeIds[p] : 0;
                switch (pType) {
                case semantic::PrimitiveType::Bool:
                case semantic::PrimitiveType::Byte:
                case semantic::PrimitiveType::SByte:
                case semantic::PrimitiveType::Short:
                case semantic::PrimitiveType::UShort:
                case semantic::PrimitiveType::Int:
                case semantic::PrimitiveType::UInt:
                case semantic::PrimitiveType::Long:
                case semantic::PrimitiveType::ULong:
                case semantic::PrimitiveType::Float:
                case semantic::PrimitiveType::Double:
                case semantic::PrimitiveType::Char:
                case semantic::PrimitiveType::Enum:
                    break;
                case semantic::PrimitiveType::Struct:
                    if (!isFlatStackSafeCustomStruct(program, pTid)) {
                        addViolation(
                            *loc, 0, 0, "param",
                            ParallelVerifyViolationKind::UnsupportedParameterType,
                            "Helper parameter " + std::to_string(p) +
                                " struct contains managed references or exceeds flat-stack parallel limits");
                    }
                    break;
                case semantic::PrimitiveType::Object: {
                    const auto* objType = pTid != 0 ? program.resolveType(pTid) : nullptr;
                    if (objType && objType->delegateType) {
                        addViolation(
                            *loc, 0, 0, "param",
                            ParallelVerifyViolationKind::UnsupportedParameterType,
                            "Delegate helper parameter " + std::to_string(p) +
                                " is forbidden in ParallelJob");
                    }
                    break;
                }
                case semantic::PrimitiveType::Array:
                    if (!isAllowedArrayTypeId(pTid)) {
                        addViolation(
                            *loc, 0, 0, "param",
                            ParallelVerifyViolationKind::NonPackedArrayParameter,
                            "Helper array parameter " + std::to_string(p) +
                                " is not a P0 packed primitive/enum array");
                    }
                    break;
                default:
                    addViolation(
                        *loc, 0, 0, "param",
                        ParallelVerifyViolationKind::UnsupportedParameterType,
                        "Helper parameter " + std::to_string(p) + " has unsupported parallel type '" +
                            std::string(semantic::primitiveTypeName(pType)) + "'");
                    break;
                }
            }
        }

        for (std::size_t l = 0; l < fn.localTypes.size(); ++l) {
            const auto lType = fn.localTypes[l];
            const auto lTid = l < fn.localTypeIds.size() ? fn.localTypeIds[l] : 0;
            if (lType == semantic::PrimitiveType::Struct) {
                if (!isFlatStackSafeCustomStruct(program, lTid)) {
                    addViolation(
                        *loc, 0, 0, "local",
                        ParallelVerifyViolationKind::NonInlineStructMutation,
                        "Local " + std::to_string(l) +
                            " struct contains managed references or exceeds flat-stack parallel limits");
                }
            } else if (lType == semantic::PrimitiveType::Object) {
                const auto* objType = lTid != 0 ? program.resolveType(lTid) : nullptr;
                if (objType && objType->delegateType) {
                    addViolation(
                        *loc, 0, 0, "local",
                        ParallelVerifyViolationKind::DelegateAllocationOrCombine,
                        "Delegate local " + std::to_string(l) + " is forbidden in ParallelJob");
                }
            } else if (lType == semantic::PrimitiveType::Array) {
                if (!isAllowedArrayTypeId(lTid)) {
                    addViolation(
                        *loc, 0, 0, "local",
                        ParallelVerifyViolationKind::NonPackedArrayParameter,
                        "Local " + std::to_string(l) + " uses non-packed array type");
                }
            } else if (lType == semantic::PrimitiveType::String ||
                       lType == semantic::PrimitiveType::Handle) {
                addViolation(
                    *loc, 0, 0, "local",
                    ParallelVerifyViolationKind::StringAllocationOrMutation,
                    "Local " + std::to_string(l) + " uses forbidden parallel type '" +
                        std::string(semantic::primitiveTypeName(lType)) + "'");
            }
        }

        if (!fn.exceptionHandlers.empty()) {
            addViolation(
                *loc, 0, 0, "exception-handler",
                ParallelVerifyViolationKind::ExceptionThrow,
                "Exception handlers are forbidden in ParallelJob functions");
        }

        for (const auto& block : fn.blocks) {
            for (std::size_t iIdx = 0; iIdx < block.instructions.size(); ++iIdx) {
                const auto& inst = block.instructions[iIdx];
                const char* opName = bytecode::opcodeName(inst.opcode);
                const auto uInstIdx = static_cast<std::uint32_t>(iIdx);

                switch (inst.opcode) {
                case bytecode::Opcode::NewObject:
                    addViolation(
                        *loc, block.id, uInstIdx, opName,
                        ParallelVerifyViolationKind::HeapObjectAllocation,
                        "NewObject heap allocation is forbidden in ParallelJob");
                    break;
                case bytecode::Opcode::NewArray:
                    addViolation(
                        *loc, block.id, uInstIdx, opName,
                        ParallelVerifyViolationKind::HeapArrayAllocation,
                        "NewArray heap allocation is forbidden in ParallelJob");
                    break;
                case bytecode::Opcode::NewDelegate:
                case bytecode::Opcode::CombineDelegate:
                case bytecode::Opcode::RemoveDelegate:
                    addViolation(
                        *loc, block.id, uInstIdx, opName,
                        ParallelVerifyViolationKind::DelegateAllocationOrCombine,
                        std::string(opName) + " is forbidden in ParallelJob");
                    break;
                case bytecode::Opcode::InvokeDelegate:
                    addViolation(
                        *loc, block.id, uInstIdx, opName,
                        ParallelVerifyViolationKind::DelegateInvocation,
                        "InvokeDelegate is forbidden in ParallelJob");
                    break;
                case bytecode::Opcode::StoreField:
                    addViolation(
                        *loc, block.id, uInstIdx, opName,
                        ParallelVerifyViolationKind::InstanceFieldMutation,
                        "StoreField (instance field write) is forbidden in ParallelJob");
                    break;
                case bytecode::Opcode::AsyncEnter:
                case bytecode::Opcode::AwaitSuspend:
                case bytecode::Opcode::AwaitResume:
                case bytecode::Opcode::AsyncResolve:
                    addViolation(
                        *loc, block.id, uInstIdx, opName,
                        ParallelVerifyViolationKind::AsyncOrAwaitOperation,
                        std::string(opName) + " coroutine operation is forbidden in ParallelJob");
                    break;
                case bytecode::Opcode::ConstantString:
                    addViolation(
                        *loc, block.id, uInstIdx, opName,
                        ParallelVerifyViolationKind::StringAllocationOrMutation,
                        "ConstantString interns on ManagedHeap and is forbidden in ParallelJob");
                    break;
                case bytecode::Opcode::LoadField: {
                    const auto* objType =
                        inst.typeIndex < mod.types.size() ? &mod.types[inst.typeIndex] : nullptr;
                    if (objType && inst.index < objType->fields.size()) {
                        const auto& field = objType->fields[inst.index];
                        if (field.type == semantic::PrimitiveType::Struct) {
                            const semantic::SymbolId fieldTid =
                                field.typeName.empty() ? 0 : semantic::stableTypeId(field.typeName);
                            if (!isFlatStackSafeCustomStruct(program, fieldTid)) {
                                addViolation(
                                    *loc, block.id, uInstIdx, opName,
                                    ParallelVerifyViolationKind::NonInlineStructMutation,
                                    "LoadField on struct containing managed references or exceeding flat-stack limits");
                            }
                        }
                    }
                    break;
                }
                case bytecode::Opcode::LoadStructField: {
                    const auto* structType =
                        inst.typeIndex < mod.types.size() ? &mod.types[inst.typeIndex] : nullptr;
                    if (!structType || !isFlatStackSafeCustomStruct(program, structType->id)) {
                        addViolation(
                            *loc, block.id, uInstIdx, opName,
                            ParallelVerifyViolationKind::NonInlineStructMutation,
                            "LoadStructField on struct containing managed references or exceeding flat-stack limits");
                    } else if (inst.index < structType->fields.size()) {
                        const auto& field = structType->fields[inst.index];
                        if (field.type == semantic::PrimitiveType::Struct) {
                            const semantic::SymbolId fieldTid =
                                field.typeName.empty() ? 0 : semantic::stableTypeId(field.typeName);
                            if (!isFlatStackSafeCustomStruct(program, fieldTid)) {
                                addViolation(
                                    *loc, block.id, uInstIdx, opName,
                                    ParallelVerifyViolationKind::NonInlineStructMutation,
                                    "LoadStructField field struct contains managed references or exceeds flat-stack limits");
                            }
                        }
                    }
                    break;
                }
                case bytecode::Opcode::NewStruct:
                case bytecode::Opcode::StoreStructField: {
                    const auto* structType =
                        inst.typeIndex < mod.types.size() ? &mod.types[inst.typeIndex] : nullptr;
                    if (!structType || !isFlatStackSafeCustomStruct(program, structType->id)) {
                        addViolation(
                            *loc, block.id, uInstIdx, opName,
                            ParallelVerifyViolationKind::NonInlineStructMutation,
                            std::string(opName) +
                                " on struct containing managed references or exceeding flat-stack limits");
                    }
                    break;
                }
                case bytecode::Opcode::LoadElement:
                case bytecode::Opcode::StoreElement: {
                    const bool elemOk = isPackedParallelElementType(inst.elementType) ||
                        (options.allowRawColumnArrays &&
                         inst.elementType == semantic::PrimitiveType::Struct);
                    if (!elemOk) {
                        addViolation(
                            *loc, block.id, uInstIdx, opName,
                            ParallelVerifyViolationKind::NonPackedArrayElementAccess,
                            std::string(opName) + " on non-packed element type '" +
                                std::string(semantic::primitiveTypeName(inst.elementType)) +
                                "' is forbidden in ParallelJob");
                    }
                    break;
                }
                case bytecode::Opcode::Call: {
                    if (inst.index >= mod.functionReferences.size()) {
                        addViolation(
                            *loc, block.id, uInstIdx, opName,
                            ParallelVerifyViolationKind::ImpureExternalHostCall,
                            "Call has out-of-range FunctionReference index");
                        break;
                    }
                    const auto& ref = mod.functionReferences[inst.index];
                    if (ref.virtualDispatch || ref.interfaceDispatch) {
                        addViolation(
                            *loc, block.id, uInstIdx, opName,
                            ParallelVerifyViolationKind::DynamicPolymorphicCall,
                            "Virtual/interface polymorphic call '" + ref.name +
                                "' is forbidden in ParallelJob");
                        break;
                    }
                    if (const auto* targetLoc = program.resolveFunction(ref.symbolId)) {
                        enqueueFunction(targetLoc->function->symbolId);
                        break;
                    }
                    const std::string_view refName(ref.name);
                    if (refName.rfind("$intrinsic::", 0) == 0) {
                        const std::string_view op = refName.substr(12u);
                        if (op.rfind("static_set::", 0) == 0) {
                            addViolation(
                                *loc, block.id, uInstIdx, opName,
                                ParallelVerifyViolationKind::StaticFieldMutation,
                                "Static field write ('" + ref.name +
                                    "') is forbidden in ParallelJob");
                        } else if (op.rfind("Async.__", 0) == 0) {
                            addViolation(
                                *loc, block.id, uInstIdx, opName,
                                ParallelVerifyViolationKind::AsyncOrAwaitOperation,
                                "Async intrinsic '" + ref.name + "' is forbidden in ParallelJob");
                        } else if (op == "string_concat" || op.rfind("to_string::", 0) == 0) {
                            addViolation(
                                *loc, block.id, uInstIdx, opName,
                                ParallelVerifyViolationKind::StringAllocationOrMutation,
                                "String allocation intrinsic '" + ref.name +
                                    "' is forbidden in ParallelJob");
                        } else if (op.rfind("static_has::", 0) == 0) {
                            // Pure read-only check on frozen static field table
                        } else if (op.rfind("static_get::", 0) == 0) {
                            const std::string_view subOp = op.substr(12u);
                            semantic::SymbolId staticId = 0;
                            semantic::SymbolId ecsColDenseId = 0;
                            if (options.allowReadOnlyStaticColumnLoads &&
                                tryParseReadOnlyEcsColDenseId(subOp, ecsColDenseId)) {
                                staticFieldIdSet.insert(ecsColDenseId);
                                report.usesReadOnlyStaticColumnLoads = true;
                            } else if (subOp.rfind("ecs_col::", 0) == 0 ||
                                       subOp.rfind("ecs_ent::", 0) == 0 ||
                                       !tryParseNumericStaticFieldId(subOp, staticId)) {
                                addViolation(
                                    *loc, block.id, uInstIdx, opName,
                                    ParallelVerifyViolationKind::ImpureRuntimeIntrinsic,
                                    "ECS/dynamic intrinsic '" + ref.name +
                                        "' may allocate or mutate ManagedHeap");
                            } else {
                                staticFieldIdSet.insert(staticId);
                                if (fn.registerTypes.size() > inst.result &&
                                    inst.result != bytecode::InvalidRegister &&
                                    fn.registerTypes[inst.result] == semantic::PrimitiveType::Array) {
                                    if (!options.allowReadOnlyStaticColumnLoads) {
                                        addViolation(
                                            *loc, block.id, uInstIdx, opName,
                                            ParallelVerifyViolationKind::ImpureRuntimeIntrinsic,
                                            "Static column array load '" + ref.name +
                                                "' requires allowReadOnlyStaticColumnLoads");
                                    } else {
                                        report.usesReadOnlyStaticColumnLoads = true;
                                    }
                                }
                            }
                        } else if (op == "stable_hash" ||
                                   op.rfind("stable_hash::", 0) == 0 ||
                                   op.rfind("array_index_of::", 0) == 0 ||
                                   op.rfind("array_last_index_of::", 0) == 0 ||
                                   op.rfind("array_contains::", 0) == 0) {
                            // Pure read-only runtime intrinsics
                        } else {
                            addViolation(
                                *loc, block.id, uInstIdx, opName,
                                ParallelVerifyViolationKind::ImpureRuntimeIntrinsic,
                                "Impure or mutating runtime intrinsic '" + ref.name +
                                    "' is forbidden in ParallelJob");
                        }
                        break;
                    }

                    // External host function call
                    if (options.allowBuiltInParallelPrng &&
                        isBuiltInParallelPrngFunctionName(refName)) {
                        report.usesParallelPrng = true;
                        break;
                    }
                    const std::string_view shortRefName = stripModuleQualifier(refName);
                    const bool explicitlyPure =
                        (ref.symbolId != 0 &&
                         options.pureExternalSymbols.find(ref.symbolId) !=
                             options.pureExternalSymbols.end()) ||
                        options.pureExternalNames.find(ref.name) !=
                            options.pureExternalNames.end() ||
                        options.pureExternalNames.find(std::string(shortRefName)) !=
                            options.pureExternalNames.end();
                    if (!explicitlyPure) {
                        addViolation(
                            *loc, block.id, uInstIdx, opName,
                            ParallelVerifyViolationKind::ImpureExternalHostCall,
                            "External host call '" + ref.name +
                                "' is not whitelisted as a pure parallel host function");
                        break;
                    }
                    if (!isAllowedKernelReturnType(program, ref.returnType, ref.returnTypeId)) {
                        addViolation(
                            *loc, block.id, uInstIdx, opName,
                            ParallelVerifyViolationKind::ImpureExternalHostCall,
                            "External host call '" + ref.name +
                                "' returns non-inline-safe or non-primitive type in ParallelJob");
                        break;
                    }
                    for (std::size_t p = 0; p < ref.parameterTypes.size(); ++p) {
                        const auto pType = ref.parameterTypes[p];
                        const auto pTid =
                            p < ref.parameterTypeIds.size() ? ref.parameterTypeIds[p] : 0;
                        if ((pType == semantic::PrimitiveType::Struct &&
                             !isInlineReturnSafeStruct(program, pTid)) ||
                            pType == semantic::PrimitiveType::String ||
                            pType == semantic::PrimitiveType::Handle) {
                            addViolation(
                                *loc, block.id, uInstIdx, opName,
                                ParallelVerifyViolationKind::ImpureExternalHostCall,
                                "External host call '" + ref.name +
                                    "' takes non-inline struct or managed/handle parameter in ParallelJob");
                            break;
                        }
                    }
                    if (options.bindings != nullptr) {
                        const auto resolved = options.bindings->resolve(ref);
                        if (!resolved ||
                            resolved->determinism != BindingDeterminism::Deterministic) {
                            addViolation(
                                *loc, block.id, uInstIdx, opName,
                                ParallelVerifyViolationKind::ImpureExternalHostCall,
                                "External host call '" + ref.name +
                                    "' is not bound with BindingDeterminism::Deterministic");
                        }
                    }
                    break;
                }
                default:
                    // Scalar/Fixed/Vec/Quat arithmetic, numeric conversions,
                    // LoadParameter, LoadLocal, StoreLocal, CheckNotNull,
                    // ArrayLength, LoadField, LoadStructField, IsType, AsType
                    break;
                }
            }

            if (block.terminator.kind == bytecode::TerminatorKind::Throw) {
                addViolation(
                    *loc,
                    block.id,
                    static_cast<std::uint32_t>(block.instructions.size()),
                    bytecode::terminatorName(block.terminator.kind),
                    ParallelVerifyViolationKind::ExceptionThrow,
                    "Throw terminator is forbidden in ParallelJob");
            }
        }
    }

    report.verifiedFunctionCount = visitedFunctions.size();
    report.referencedStaticFieldIds.assign(staticFieldIdSet.begin(), staticFieldIdSet.end());
    std::sort(report.referencedStaticFieldIds.begin(), report.referencedStaticFieldIds.end());

    // 3. Inter-procedural Array Parameter Origin Fixed-Point Dataflow
    bool changed = true;
    std::size_t iterGuard = 0;
    while (changed && iterGuard++ < 128u) {
        changed = false;
        for (const semantic::SymbolId symId : visitWorklist) {
            auto& summary = visitedFunctions.at(symId);
            const auto& mod = *summary.location->module;
            const auto& fn = *summary.location->function;

            std::unordered_map<bytecode::BlockId, const bytecode::BasicBlock*> blockMap;
            for (const auto& b : fn.blocks) {
                blockMap.emplace(b.id, &b);
            }

            auto mergeMask = [&](std::uint64_t& dst, std::uint64_t incoming) {
                const std::uint64_t combined = dst | incoming;
                if (combined != dst) {
                    dst = combined;
                    changed = true;
                }
            };

            for (const auto& block : fn.blocks) {
                for (const auto& inst : block.instructions) {
                    const bool dstIsArray =
                        inst.result != bytecode::InvalidRegister &&
                        inst.result < fn.registerTypes.size() &&
                        fn.registerTypes[inst.result] == semantic::PrimitiveType::Array;

                    switch (inst.opcode) {
                    case bytecode::Opcode::LoadParameter:
                        if (dstIsArray && inst.index < summary.paramOrigins.size()) {
                            mergeMask(
                                summary.regOrigins[inst.result],
                                summary.paramOrigins[inst.index]);
                        }
                        break;
                    case bytecode::Opcode::LoadLocal:
                        if (dstIsArray && inst.index < summary.localOrigins.size()) {
                            mergeMask(
                                summary.regOrigins[inst.result],
                                summary.localOrigins[inst.index]);
                        }
                        break;
                    case bytecode::Opcode::StoreLocal:
                        if (!inst.operands.empty() &&
                            inst.index < fn.localTypes.size() &&
                            fn.localTypes[inst.index] == semantic::PrimitiveType::Array &&
                            inst.operands[0] < summary.regOrigins.size()) {
                            mergeMask(
                                summary.localOrigins[inst.index],
                                summary.regOrigins[inst.operands[0]]);
                        }
                        break;
                    case bytecode::Opcode::LoadField:
                    case bytecode::Opcode::LoadStructField:
                    case bytecode::Opcode::AsType:
                        if (dstIsArray) {
                            mergeMask(summary.regOrigins[inst.result], kUnknownArrayOriginBit);
                        }
                        break;
                    case bytecode::Opcode::Call: {
                        if (inst.index >= mod.functionReferences.size()) break;
                        const auto& ref = mod.functionReferences[inst.index];
                        const auto calleeIt = visitedFunctions.find(ref.symbolId);
                        if (calleeIt != visitedFunctions.end()) {
                            auto& calleeSum = calleeIt->second;
                            const std::size_t nArgs = std::min(
                                inst.operands.size(), calleeSum.paramOrigins.size());
                            for (std::size_t a = 0; a < nArgs; ++a) {
                                const auto opReg = inst.operands[a];
                                if (opReg < summary.regOrigins.size() &&
                                    calleeSum.location->function->parameterTypes[a] ==
                                        semantic::PrimitiveType::Array) {
                                    mergeMask(
                                        calleeSum.paramOrigins[a],
                                        summary.regOrigins[opReg]);
                                }
                            }
                            if (dstIsArray) {
                                mergeMask(
                                    summary.regOrigins[inst.result],
                                    calleeSum.returnOrigins);
                            }
                        } else if (dstIsArray) {
                            mergeMask(summary.regOrigins[inst.result], kUnknownArrayOriginBit);
                        }
                        break;
                    }
                    default:
                        break;
                    }
                }

                auto propagateEdge = [&](bytecode::BlockId targetId,
                                         const bytecode::SmallRegisterVector& edgeArgs) {
                    const auto bIt = blockMap.find(targetId);
                    if (bIt == blockMap.end()) return;
                    const auto& targetParams = bIt->second->parameters;
                    const std::size_t cnt = std::min(edgeArgs.size(), targetParams.size());
                    for (std::size_t k = 0; k < cnt; ++k) {
                        if (targetParams[k].type == semantic::PrimitiveType::Array &&
                            edgeArgs[k] < summary.regOrigins.size() &&
                            targetParams[k].target < summary.regOrigins.size()) {
                            mergeMask(
                                summary.regOrigins[targetParams[k].target],
                                summary.regOrigins[edgeArgs[k]]);
                        }
                    }
                };

                if (block.terminator.kind == bytecode::TerminatorKind::Jump) {
                    propagateEdge(block.terminator.target, block.terminator.arguments);
                } else if (block.terminator.kind == bytecode::TerminatorKind::Branch) {
                    propagateEdge(block.terminator.target, block.terminator.arguments);
                    propagateEdge(block.terminator.falseTarget, block.terminator.falseArguments);
                } else if (block.terminator.kind == bytecode::TerminatorKind::ReturnValue &&
                           fn.returnType == semantic::PrimitiveType::Array &&
                           block.terminator.value < summary.regOrigins.size()) {
                    mergeMask(summary.returnOrigins, summary.regOrigins[block.terminator.value]);
                }
            }
        }
    }

    std::uint64_t readMask = 0;
    std::uint64_t writeMask = 0;
    for (const semantic::SymbolId symId : visitWorklist) {
        const auto& summary = visitedFunctions.at(symId);
        const auto& fn = *summary.location->function;
        for (const auto& block : fn.blocks) {
            for (std::size_t iIdx = 0; iIdx < block.instructions.size(); ++iIdx) {
                const auto& inst = block.instructions[iIdx];
                if ((inst.opcode == bytecode::Opcode::ArrayLength ||
                     inst.opcode == bytecode::Opcode::LoadElement) &&
                    !inst.operands.empty() &&
                    inst.operands[0] < summary.regOrigins.size()) {
                    readMask |= summary.regOrigins[inst.operands[0]];
                } else if (inst.opcode == bytecode::Opcode::StoreElement &&
                           !inst.operands.empty() &&
                           inst.operands[0] < summary.regOrigins.size()) {
                    const std::uint64_t origin = summary.regOrigins[inst.operands[0]];
                    if (origin == 0ULL || (origin & kUnknownArrayOriginBit) != 0ULL) {
                        addViolation(
                            *summary.location,
                            block.id,
                            static_cast<std::uint32_t>(iIdx),
                            bytecode::opcodeName(inst.opcode),
                            ParallelVerifyViolationKind::UntrackedArrayMutation,
                            "StoreElement receiver does not originate solely from top-level "
                            "kernel array parameters (cannot verify ObjectRef aliasing)");
                    }
                    writeMask |= origin;
                }
            }
        }
    }

    for (std::size_t p = 0; p < std::min<std::size_t>(entryFn.parameterTypes.size(), kMaxTrackedOriginParams); ++p) {
        const std::uint64_t bit = std::uint64_t{1} << p;
        if ((readMask & bit) != 0ULL) {
            report.readArrayParamIndices.push_back(p);
        }
        if ((writeMask & bit) != 0ULL) {
            report.writtenArrayParamIndices.push_back(p);
        }
    }

    report.valid = report.violations.empty();
    return report;
}

void registerParallelPrngBindings(
    BindingRegistry& registry,
    std::string_view moduleName) {
    auto registerOne = [&](std::string_view shortName, BuiltInPrngOp op) {
        ExternalFunction fn = [op](
            const bytecode::FunctionReference&,
            ValueSpan args,
            RuntimeError& err) -> std::optional<Value> {
            return invokeBuiltInPrngOp(op, args, err);
        };
        if (!moduleName.empty()) {
            const std::string qualified = std::string(moduleName) + "::" + std::string(shortName);
            (void)registry.bind(qualified, fn, BindingDeterminism::Deterministic);
        } else {
            (void)registry.bind(std::string(shortName), fn, BindingDeterminism::Deterministic);
        }
    };
    registerOne("ParallelRandomUInt32", BuiltInPrngOp::NextUInt32);
    registerOne("ParallelRandomBits32", BuiltInPrngOp::NextBits32);
    registerOne("ParallelRandomInt", BuiltInPrngOp::NextInt);
    registerOne("ParallelRandomRange", BuiltInPrngOp::NextRange);
    registerOne("ParallelRandomByte255", BuiltInPrngOp::NextByte255);
    registerOne("ParallelRandomDouble", BuiltInPrngOp::NextDouble);
}

namespace {

[[nodiscard]] bool reduceTwoValues(
    ParallelReduceOp op,
    const ParallelCustomReducer& customReducer,
    bool strictDeterminism,
    const Value& left,
    const Value& right,
    Value& outReduced,
    RuntimeError& outError) {
    switch (op) {
    case ParallelReduceOp::None:
        outReduced = std::monostate{};
        return true;

    case ParallelReduceOp::Custom:
        if (!customReducer) {
            outError.code = ErrorCode::InvalidArguments;
            outError.message = "ParallelReduceOp::Custom requires a valid customReducer callback";
            return false;
        }
        return customReducer(left, right, outReduced, outError);

    case ParallelReduceOp::Sum: {
        if (const auto* l = std::get_if<std::int64_t>(&left)) {
            const auto* r = std::get_if<std::int64_t>(&right);
            if (!r) break;
            // Deterministic 64-bit exact sum (or 32-bit wrapped? Wait: kernel returning `int`
            // in RealScript stores int64_t in Value; wrapping or clamping? Let's check:
            // ExecutionResult::value for `int` holds `std::int64_t` in [-2^31, 2^31-1].
            // Summing in two's-complement int32_t vs int64_t: wait! If `int KernelRange(...)`
            // returns `int`, in RealScript `int` is 32-bit signed (stored in Value::int64Cell)!
            // Let's wrap to signed 32-bit `int32_t` (stored as `int64_t`) so `1` batch vs `N`
            // batches doing `unchecked(sum + x)` inside RealScript matches 100% bit-for-bit!!
            const auto sum32 = static_cast<std::int32_t>(
                static_cast<std::uint32_t>(static_cast<std::int32_t>(*l)) +
                static_cast<std::uint32_t>(static_cast<std::int32_t>(*r)));
            outReduced = Value{static_cast<std::int64_t>(sum32)};
            return true;
        }
        if (const auto* l = std::get_if<LongValue>(&left)) {
            const auto* r = std::get_if<LongValue>(&right);
            if (!r) break;
            const auto sum64 = static_cast<std::int64_t>(
                static_cast<std::uint64_t>(l->value) +
                static_cast<std::uint64_t>(r->value));
            outReduced = Value{LongValue{sum64}};
            return true;
        }
        if (const auto* l = std::get_if<UIntValue>(&left)) {
            const auto* r = std::get_if<UIntValue>(&right);
            if (!r) break;
            outReduced = Value{UIntValue{l->value + r->value}};
            return true;
        }
        if (const auto* l = std::get_if<ULongValue>(&left)) {
            const auto* r = std::get_if<ULongValue>(&right);
            if (!r) break;
            outReduced = Value{ULongValue{l->value + r->value}};
            return true;
        }
        if (const auto* l = std::get_if<double>(&left)) {
            const auto* r = std::get_if<double>(&right);
            if (!r) break;
            outReduced = Value{canonicalizeDoubleForStrict(*l + *r, strictDeterminism)};
            return true;
        }
        if (const auto* l = std::get_if<FloatValue>(&left)) {
            const auto* r = std::get_if<FloatValue>(&right);
            if (!r) break;
            outReduced = Value{FloatValue{canonicalizeFloatForStrict(l->value + r->value, strictDeterminism)}};
            return true;
        }
        if (const auto* l = std::get_if<FixedValue>(&left)) {
            const auto* r = std::get_if<FixedValue>(&right);
            if (!r) break;
            const auto sumRaw = static_cast<std::int32_t>(
                static_cast<std::uint32_t>(l->raw) + static_cast<std::uint32_t>(r->raw));
            outReduced = Value{FixedValue{sumRaw}};
            return true;
        }
        if (const auto* l = std::get_if<Fixed16Value>(&left)) {
            const auto* r = std::get_if<Fixed16Value>(&right);
            if (!r) break;
            const auto sumRaw = static_cast<std::int64_t>(
                static_cast<std::uint64_t>(l->raw) + static_cast<std::uint64_t>(r->raw));
            outReduced = Value{Fixed16Value{sumRaw}};
            return true;
        }
        if (const auto* l = std::get_if<Vec2Value>(&left)) {
            const auto* r = std::get_if<Vec2Value>(&right);
            if (!r) break;
            outReduced = Value{Vec2Value{
                static_cast<std::int32_t>(static_cast<std::uint32_t>(l->x) + static_cast<std::uint32_t>(r->x)),
                static_cast<std::int32_t>(static_cast<std::uint32_t>(l->y) + static_cast<std::uint32_t>(r->y)),
            }};
            return true;
        }
        if (const auto* l = std::get_if<Vec3Value>(&left)) {
            const auto* r = std::get_if<Vec3Value>(&right);
            if (!r) break;
            outReduced = Value{Vec3Value{
                static_cast<std::int32_t>(static_cast<std::uint32_t>(l->x) + static_cast<std::uint32_t>(r->x)),
                static_cast<std::int32_t>(static_cast<std::uint32_t>(l->y) + static_cast<std::uint32_t>(r->y)),
                static_cast<std::int32_t>(static_cast<std::uint32_t>(l->z) + static_cast<std::uint32_t>(r->z)),
            }};
            return true;
        }
        if (const auto* l = std::get_if<Fixed64Value>(&left)) {
            const auto* r = std::get_if<Fixed64Value>(&right);
            if (!r) break;
            const auto sumRaw = static_cast<std::int64_t>(
                static_cast<std::uint64_t>(l->raw) + static_cast<std::uint64_t>(r->raw));
            outReduced = Value{Fixed64Value{sumRaw}};
            return true;
        }
        if (const auto* l = std::get_if<Vec2fValue>(&left)) {
            const auto* r = std::get_if<Vec2fValue>(&right);
            if (!r) break;
            outReduced = Value{Vec2fValue{l->x + r->x, l->y + r->y}};
            return true;
        }
        if (const auto* l = std::get_if<Vec3fValue>(&left)) {
            const auto* r = std::get_if<Vec3fValue>(&right);
            if (!r) break;
            outReduced = Value{Vec3fValue{l->x + r->x, l->y + r->y, l->z + r->z}};
            return true;
        }
        break;
    }

    case ParallelReduceOp::Min:
    case ParallelReduceOp::Max: {
        const bool isMin = (op == ParallelReduceOp::Min);
        if (const auto* l = std::get_if<std::int64_t>(&left)) {
            const auto* r = std::get_if<std::int64_t>(&right);
            if (!r) break;
            outReduced = Value{isMin ? std::min(*l, *r) : std::max(*l, *r)};
            return true;
        }
        if (const auto* l = std::get_if<LongValue>(&left)) {
            const auto* r = std::get_if<LongValue>(&right);
            if (!r) break;
            outReduced = Value{LongValue{isMin ? std::min(l->value, r->value) : std::max(l->value, r->value)}};
            return true;
        }
        if (const auto* l = std::get_if<UIntValue>(&left)) {
            const auto* r = std::get_if<UIntValue>(&right);
            if (!r) break;
            outReduced = Value{UIntValue{isMin ? std::min(l->value, r->value) : std::max(l->value, r->value)}};
            return true;
        }
        if (const auto* l = std::get_if<ULongValue>(&left)) {
            const auto* r = std::get_if<ULongValue>(&right);
            if (!r) break;
            outReduced = Value{ULongValue{isMin ? std::min(l->value, r->value) : std::max(l->value, r->value)}};
            return true;
        }
        if (const auto* l = std::get_if<double>(&left)) {
            const auto* r = std::get_if<double>(&right);
            if (!r) break;
            double chosen = 0.0;
            if (std::isnan(*l) || std::isnan(*r)) {
                chosen = std::numeric_limits<double>::quiet_NaN();
            } else {
                chosen = isMin ? ((*r < *l) ? *r : *l) : ((*r > *l) ? *r : *l);
            }
            outReduced = Value{canonicalizeDoubleForStrict(chosen, strictDeterminism)};
            return true;
        }
        if (const auto* l = std::get_if<FloatValue>(&left)) {
            const auto* r = std::get_if<FloatValue>(&right);
            if (!r) break;
            float chosen = 0.0f;
            if (std::isnan(l->value) || std::isnan(r->value)) {
                chosen = std::numeric_limits<float>::quiet_NaN();
            } else {
                chosen = isMin
                    ? ((r->value < l->value) ? r->value : l->value)
                    : ((r->value > l->value) ? r->value : l->value);
            }
            outReduced = Value{FloatValue{canonicalizeFloatForStrict(chosen, strictDeterminism)}};
            return true;
        }
        if (const auto* l = std::get_if<FixedValue>(&left)) {
            const auto* r = std::get_if<FixedValue>(&right);
            if (!r) break;
            outReduced = Value{FixedValue{isMin ? std::min(l->raw, r->raw) : std::max(l->raw, r->raw)}};
            return true;
        }
        if (const auto* l = std::get_if<Fixed16Value>(&left)) {
            const auto* r = std::get_if<Fixed16Value>(&right);
            if (!r) break;
            outReduced = Value{Fixed16Value{isMin ? std::min(l->raw, r->raw) : std::max(l->raw, r->raw)}};
            return true;
        }
        break;
    }

    case ParallelReduceOp::BitwiseXor:
    case ParallelReduceOp::BitwiseOr:
    case ParallelReduceOp::BitwiseAnd: {
        if (const auto* l = std::get_if<std::int64_t>(&left)) {
            const auto* r = std::get_if<std::int64_t>(&right);
            if (!r) break;
            const auto lu = static_cast<std::uint32_t>(static_cast<std::int32_t>(*l));
            const auto ru = static_cast<std::uint32_t>(static_cast<std::int32_t>(*r));
            const auto res = (op == ParallelReduceOp::BitwiseXor)
                ? (lu ^ ru)
                : ((op == ParallelReduceOp::BitwiseOr) ? (lu | ru) : (lu & ru));
            outReduced = Value{static_cast<std::int64_t>(static_cast<std::int32_t>(res))};
            return true;
        }
        if (const auto* l = std::get_if<LongValue>(&left)) {
            const auto* r = std::get_if<LongValue>(&right);
            if (!r) break;
            const auto lu = static_cast<std::uint64_t>(l->value);
            const auto ru = static_cast<std::uint64_t>(r->value);
            const auto res = (op == ParallelReduceOp::BitwiseXor)
                ? (lu ^ ru)
                : ((op == ParallelReduceOp::BitwiseOr) ? (lu | ru) : (lu & ru));
            outReduced = Value{LongValue{static_cast<std::int64_t>(res)}};
            return true;
        }
        if (const auto* l = std::get_if<bool>(&left)) {
            const auto* r = std::get_if<bool>(&right);
            if (!r) break;
            const bool res = (op == ParallelReduceOp::BitwiseXor)
                ? (*l != *r)
                : ((op == ParallelReduceOp::BitwiseOr) ? (*l || *r) : (*l && *r));
            outReduced = Value{res};
            return true;
        }
        break;
    }
    }

    outError.code = ErrorCode::TypeMismatch;
    outError.message = "unsupported return value type for requested ParallelReduceOp";
    return false;
}

[[nodiscard]] Value neutralIdentityValue(
    ParallelReduceOp op,
    semantic::PrimitiveType returnType,
    semantic::SymbolId returnTypeId) noexcept {
    if (op == ParallelReduceOp::None || returnType == semantic::PrimitiveType::Void) {
        return std::monostate{};
    }
    switch (returnType) {
    case semantic::PrimitiveType::Bool:
        return Value{op == ParallelReduceOp::BitwiseAnd};
    case semantic::PrimitiveType::Int:
        if (op == ParallelReduceOp::Min) return Value{static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max())};
        if (op == ParallelReduceOp::Max) return Value{static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min())};
        if (op == ParallelReduceOp::BitwiseAnd) return Value{std::int64_t{-1}};
        return Value{std::int64_t{0}};
    case semantic::PrimitiveType::Long:
        if (op == ParallelReduceOp::Min) return Value{LongValue{std::numeric_limits<std::int64_t>::max()}};
        if (op == ParallelReduceOp::Max) return Value{LongValue{std::numeric_limits<std::int64_t>::min()}};
        if (op == ParallelReduceOp::BitwiseAnd) return Value{LongValue{-1}};
        return Value{LongValue{0}};
    case semantic::PrimitiveType::UInt:
        if (op == ParallelReduceOp::Min) return Value{UIntValue{std::numeric_limits<std::uint32_t>::max()}};
        return Value{UIntValue{0}};
    case semantic::PrimitiveType::ULong:
        if (op == ParallelReduceOp::Min) return Value{ULongValue{std::numeric_limits<std::uint64_t>::max()}};
        return Value{ULongValue{0}};
    case semantic::PrimitiveType::Float:
        if (op == ParallelReduceOp::Min) return Value{FloatValue{std::numeric_limits<float>::infinity()}};
        if (op == ParallelReduceOp::Max) return Value{FloatValue{-std::numeric_limits<float>::infinity()}};
        return Value{FloatValue{0.0f}};
    case semantic::PrimitiveType::Double:
        if (op == ParallelReduceOp::Min) return Value{std::numeric_limits<double>::infinity()};
        if (op == ParallelReduceOp::Max) return Value{-std::numeric_limits<double>::infinity()};
        return Value{0.0};
    case semantic::PrimitiveType::Struct:
        if (returnTypeId == fixedTypeId()) {
            if (op == ParallelReduceOp::Min) return Value{FixedValue{std::numeric_limits<std::int32_t>::max()}};
            if (op == ParallelReduceOp::Max) return Value{FixedValue{std::numeric_limits<std::int32_t>::min()}};
            return Value{FixedValue{0}};
        }
        if (returnTypeId == fixed16TypeId()) {
            if (op == ParallelReduceOp::Min) return Value{Fixed16Value{std::numeric_limits<std::int64_t>::max()}};
            if (op == ParallelReduceOp::Max) return Value{Fixed16Value{std::numeric_limits<std::int64_t>::min()}};
            return Value{Fixed16Value{0}};
        }
        if (returnTypeId == fixed64TypeId()) {
            if (op == ParallelReduceOp::Min) return Value{Fixed64Value{std::numeric_limits<std::int64_t>::max()}};
            if (op == ParallelReduceOp::Max) return Value{Fixed64Value{std::numeric_limits<std::int64_t>::min()}};
            return Value{Fixed64Value{0}};
        }
        if (returnTypeId == vec2TypeId()) return Value{Vec2Value{0, 0}};
        if (returnTypeId == vec3TypeId()) return Value{Vec3Value{0, 0, 0}};
        if (returnTypeId == vec2fTypeId()) return Value{Vec2fValue{0.0f, 0.0f}};
        if (returnTypeId == vec3fTypeId()) return Value{Vec3fValue{0.0f, 0.0f, 0.0f}};
        break;
    default:
        break;
    }
    return std::monostate{};
}

[[nodiscard]] bool runtimeTypeAssignable(
    const ProgramImage& program,
    semantic::SymbolId actualTypeId,
    semantic::SymbolId expectedTypeId) noexcept {
    if (expectedTypeId == 0 || actualTypeId == expectedTypeId) return true;
    semantic::SymbolId cur = actualTypeId;
    for (std::size_t depth = 0; cur != 0 && depth < 128u; ++depth) {
        const auto* t = program.resolveType(cur);
        if (!t) return false;
        for (const auto& imap : t->interfaceDispatchMaps) {
            if (imap.interfaceTypeId == expectedTypeId) return true;
        }
        if (t->baseTypeId == expectedTypeId) return true;
        cur = t->baseTypeId;
    }
    return false;
}

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
struct alignas(64) ParallelBatchOutcome {
    bool succeeded = false;
    bool hasReducedValue = false;
    Value value{};
    RuntimeError error{};
    RuntimeStatistics statistics{};
    std::uint64_t determinismDigest = 0;
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

enum class PodValKind : std::uint8_t {
    Unset = 0,
    Null,
    Bool,
    Int32,
    Int64,
    Float32,
    Float64,
    Enum64,
    ColumnRef,
    PodStruct,
};

struct PodStructDesc {
    semantic::SymbolId typeId = 0;
    std::uint8_t fieldCount = 0;
    std::array<semantic::PrimitiveType, 16> fieldTypes{};
};

struct PodSlot {
    PodValKind kind = PodValKind::Unset;
    std::int64_t i64 = 0;
    double f64 = 0.0;
    float f32 = 0.0f;
    bool b = false;
    std::int32_t colIndex = -1;
    semantic::SymbolId typeId = 0;
    std::uint8_t structFieldCount = 0;
    std::array<PodValKind, 16> fieldKinds{};
    std::array<std::int64_t, 16> fieldI64{};
    std::array<double, 16> fieldF64{};
    std::array<float, 16> fieldF32{};
    std::array<bool, 16> fieldBool{};
    std::array<semantic::SymbolId, 16> fieldTypeIds{};
};

[[nodiscard]] bool isNativePodFieldKind(NativeFieldKind k) noexcept {
    switch (k) {
    case NativeFieldKind::Int8:
    case NativeFieldKind::UInt8:
    case NativeFieldKind::Int16:
    case NativeFieldKind::UInt16:
    case NativeFieldKind::Int32:
    case NativeFieldKind::UInt32:
    case NativeFieldKind::Int64:
    case NativeFieldKind::UInt64:
    case NativeFieldKind::Float32:
    case NativeFieldKind::Float64:
    case NativeFieldKind::Bool:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool isNativeSupportedScalarPrim(semantic::PrimitiveType prim) noexcept {
    switch (prim) {
    case semantic::PrimitiveType::Bool:
    case semantic::PrimitiveType::Int:
    case semantic::PrimitiveType::Long:
    case semantic::PrimitiveType::Float:
    case semantic::PrimitiveType::Double:
    case semantic::PrimitiveType::Enum:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool podSlotFromValue(const Value& v, semantic::PrimitiveType expectedPrim, PodSlot& out) noexcept {
    switch (expectedPrim) {
    case semantic::PrimitiveType::Bool: {
        const auto* p = std::get_if<bool>(&v);
        if (!p) return false;
        out.kind = PodValKind::Bool;
        out.b = *p;
        return true;
    }
    case semantic::PrimitiveType::Int: {
        const auto* p = std::get_if<std::int64_t>(&v);
        if (!p) return false;
        out.kind = PodValKind::Int32;
        out.i64 = *p;
        return true;
    }
    case semantic::PrimitiveType::Long: {
        const auto* p = std::get_if<LongValue>(&v);
        if (!p) return false;
        out.kind = PodValKind::Int64;
        out.i64 = p->value;
        return true;
    }
    case semantic::PrimitiveType::Float: {
        const auto* p = std::get_if<FloatValue>(&v);
        if (!p) return false;
        out.kind = PodValKind::Float32;
        out.f32 = p->value;
        return true;
    }
    case semantic::PrimitiveType::Double: {
        const auto* p = std::get_if<double>(&v);
        if (!p) return false;
        out.kind = PodValKind::Float64;
        out.f64 = *p;
        return true;
    }
    case semantic::PrimitiveType::Enum: {
        EnumValue ev{};
        if (!v.tryGetEnum(ev)) return false;
        out.kind = PodValKind::Enum64;
        out.i64 = ev.value;
        out.typeId = ev.typeId;
        return true;
    }
    default:
        return false;
    }
}

[[nodiscard]] Value podSlotToValue(const PodSlot& s, semantic::PrimitiveType expectedPrim) noexcept {
    if (expectedPrim == semantic::PrimitiveType::Void || s.kind == PodValKind::Unset) {
        return Value{std::monostate{}};
    }
    switch (s.kind) {
    case PodValKind::Bool: return Value{s.b};
    case PodValKind::Int32: return Value{s.i64};
    case PodValKind::Int64: return Value{LongValue{s.i64}};
    case PodValKind::Float32: return Value{FloatValue{s.f32}};
    case PodValKind::Float64: return Value{s.f64};
    case PodValKind::Enum64: return Value{EnumValue{s.typeId, s.i64}};
    case PodValKind::Null: return Value{NullObject{}};
    default: return Value{std::monostate{}};
    }
}

[[nodiscard]] bool readNativeFieldIntoPod(
    const std::uint8_t* elemBase,
    const NativeFieldLayout& layout,
    PodSlot& dst,
    std::size_t fieldIdx) noexcept {
    const std::uint8_t* ptr = elemBase + layout.byteOffset;
    auto assignInt = [&](std::int64_t val, semantic::PrimitiveType req) {
        if (req == semantic::PrimitiveType::Long) {
            dst.fieldKinds[fieldIdx] = PodValKind::Int64;
            dst.fieldI64[fieldIdx] = val;
        } else {
            dst.fieldKinds[fieldIdx] = PodValKind::Int32;
            dst.fieldI64[fieldIdx] = val;
        }
    };
    switch (layout.kind) {
    case NativeFieldKind::Int8: {
        std::int8_t v = 0;
        std::memcpy(&v, ptr, sizeof(v));
        assignInt(static_cast<std::int64_t>(v), layout.scriptType);
        return true;
    }
    case NativeFieldKind::UInt8: {
        std::uint8_t v = 0;
        std::memcpy(&v, ptr, sizeof(v));
        assignInt(static_cast<std::int64_t>(v), layout.scriptType);
        return true;
    }
    case NativeFieldKind::Int16: {
        std::int16_t v = 0;
        std::memcpy(&v, ptr, sizeof(v));
        assignInt(static_cast<std::int64_t>(v), layout.scriptType);
        return true;
    }
    case NativeFieldKind::UInt16: {
        std::uint16_t v = 0;
        std::memcpy(&v, ptr, sizeof(v));
        assignInt(static_cast<std::int64_t>(v), layout.scriptType);
        return true;
    }
    case NativeFieldKind::Int32: {
        std::int32_t v = 0;
        std::memcpy(&v, ptr, sizeof(v));
        assignInt(static_cast<std::int64_t>(v), layout.scriptType);
        return true;
    }
    case NativeFieldKind::UInt32: {
        std::uint32_t v = 0;
        std::memcpy(&v, ptr, sizeof(v));
        assignInt(static_cast<std::int64_t>(v), layout.scriptType);
        return true;
    }
    case NativeFieldKind::Int64: {
        std::int64_t v = 0;
        std::memcpy(&v, ptr, sizeof(v));
        assignInt(v, layout.scriptType);
        return true;
    }
    case NativeFieldKind::UInt64: {
        std::uint64_t v = 0;
        std::memcpy(&v, ptr, sizeof(v));
        assignInt(static_cast<std::int64_t>(v), layout.scriptType);
        return true;
    }
    case NativeFieldKind::Float32: {
        float v = 0.0f;
        std::memcpy(&v, ptr, sizeof(v));
        if (layout.scriptType == semantic::PrimitiveType::Double) {
            dst.fieldKinds[fieldIdx] = PodValKind::Float64;
            dst.fieldF64[fieldIdx] = static_cast<double>(v);
        } else {
            dst.fieldKinds[fieldIdx] = PodValKind::Float32;
            dst.fieldF32[fieldIdx] = v;
        }
        return true;
    }
    case NativeFieldKind::Float64: {
        double v = 0.0;
        std::memcpy(&v, ptr, sizeof(v));
        if (layout.scriptType == semantic::PrimitiveType::Float) {
            dst.fieldKinds[fieldIdx] = PodValKind::Float32;
            dst.fieldF32[fieldIdx] = static_cast<float>(v);
        } else {
            dst.fieldKinds[fieldIdx] = PodValKind::Float64;
            dst.fieldF64[fieldIdx] = v;
        }
        return true;
    }
    case NativeFieldKind::Bool: {
        bool v = false;
        std::memcpy(&v, ptr, sizeof(v));
        dst.fieldKinds[fieldIdx] = PodValKind::Bool;
        dst.fieldBool[fieldIdx] = v;
        return true;
    }
    default:
        return false;
    }
}

[[nodiscard]] bool writePodFieldToNativeBytes(
    std::uint8_t* elemBase,
    const NativeFieldLayout& layout,
    const PodSlot& src,
    std::size_t fieldIdx) noexcept {
    std::uint8_t* ptr = elemBase + layout.byteOffset;
    const auto k = src.fieldKinds[fieldIdx];
    const auto getInt64 = [&]() -> std::pair<bool, std::int64_t> {
        if (k == PodValKind::Int32 || k == PodValKind::Int64 || k == PodValKind::Enum64) {
            return {true, src.fieldI64[fieldIdx]};
        }
        return {false, 0};
    };
    switch (layout.kind) {
    case NativeFieldKind::Int8: {
        auto [ok, iv] = getInt64();
        if (!ok) return false;
        const auto v = static_cast<std::int8_t>(iv);
        std::memcpy(ptr, &v, sizeof(v));
        return true;
    }
    case NativeFieldKind::UInt8: {
        auto [ok, iv] = getInt64();
        if (!ok) return false;
        const auto v = static_cast<std::uint8_t>(iv);
        std::memcpy(ptr, &v, sizeof(v));
        return true;
    }
    case NativeFieldKind::Int16: {
        auto [ok, iv] = getInt64();
        if (!ok) return false;
        const auto v = static_cast<std::int16_t>(iv);
        std::memcpy(ptr, &v, sizeof(v));
        return true;
    }
    case NativeFieldKind::UInt16: {
        auto [ok, iv] = getInt64();
        if (!ok) return false;
        const auto v = static_cast<std::uint16_t>(iv);
        std::memcpy(ptr, &v, sizeof(v));
        return true;
    }
    case NativeFieldKind::Int32: {
        auto [ok, iv] = getInt64();
        if (!ok) return false;
        const auto v = static_cast<std::int32_t>(iv);
        std::memcpy(ptr, &v, sizeof(v));
        return true;
    }
    case NativeFieldKind::UInt32: {
        auto [ok, iv] = getInt64();
        if (!ok) return false;
        const auto v = static_cast<std::uint32_t>(iv);
        std::memcpy(ptr, &v, sizeof(v));
        return true;
    }
    case NativeFieldKind::Int64: {
        auto [ok, iv] = getInt64();
        if (!ok) return false;
        std::memcpy(ptr, &iv, sizeof(iv));
        return true;
    }
    case NativeFieldKind::UInt64: {
        auto [ok, iv] = getInt64();
        if (!ok) return false;
        const auto v = static_cast<std::uint64_t>(iv);
        std::memcpy(ptr, &v, sizeof(v));
        return true;
    }
    case NativeFieldKind::Float32: {
        float v = 0.0f;
        if (k == PodValKind::Float32) v = src.fieldF32[fieldIdx];
        else if (k == PodValKind::Float64) v = static_cast<float>(src.fieldF64[fieldIdx]);
        else return false;
        std::memcpy(ptr, &v, sizeof(v));
        return true;
    }
    case NativeFieldKind::Float64: {
        double v = 0.0;
        if (k == PodValKind::Float64) v = src.fieldF64[fieldIdx];
        else if (k == PodValKind::Float32) v = static_cast<double>(src.fieldF32[fieldIdx]);
        else return false;
        std::memcpy(ptr, &v, sizeof(v));
        return true;
    }
    case NativeFieldKind::Bool: {
        if (k != PodValKind::Bool) return false;
        const bool v = src.fieldBool[fieldIdx];
        std::memcpy(ptr, &v, sizeof(v));
        return true;
    }
    default:
        return false;
    }
}

[[nodiscard]] bool readColumnElementIntoPod(
    const ParallelNativeColumnBinding& col,
    std::size_t index,
    PodSlot& out) noexcept {
    if (index >= col.count) return false;
    if (col.rawColumnView && col.rawColumnView->mode == RawColumnMode::EcsFields) {
        const auto& view = *col.rawColumnView;
        if (!view.basePtr || view.fields.size() > 16u) return false;
        const auto* elemBase = static_cast<const std::uint8_t*>(view.basePtr) + index * view.strideBytes;
        out.kind = PodValKind::PodStruct;
        out.typeId = view.structTypeId;
        out.structFieldCount = static_cast<std::uint8_t>(view.fields.size());
        for (std::size_t f = 0; f < view.fields.size(); ++f) {
            if (!readNativeFieldIntoPod(elemBase, view.fields[f], out, f)) return false;
            out.fieldTypeIds[f] = view.fields[f].scriptTypeId;
        }
        return true;
    }
    const auto* rawBytes = static_cast<const std::uint8_t*>(col.basePtr);
    if (!rawBytes) return false;
    const std::size_t stride = col.strideBytes;
    const PackedElemEncoding enc = (col.rawColumnView && col.rawColumnView->mode == RawColumnMode::ScalarPacked)
        ? col.rawColumnView->packedEncoding
        : col.packedEncoding;
    const semantic::SymbolId elemTid = col.elementTypeId;
    const auto* ptr = rawBytes + index * stride;
    switch (enc) {
    case PackedElemEncoding::Bool: {
        std::uint8_t v = 0;
        std::memcpy(&v, ptr, 1u);
        out.kind = PodValKind::Bool;
        out.b = (v != 0u);
        return true;
    }
    case PackedElemEncoding::Int32: {
        std::int32_t v = 0;
        std::memcpy(&v, ptr, sizeof(v));
        out.kind = PodValKind::Int32;
        out.i64 = static_cast<std::int64_t>(v);
        return true;
    }
    case PackedElemEncoding::Int64: {
        std::int64_t v = 0;
        std::memcpy(&v, ptr, sizeof(v));
        out.kind = PodValKind::Int64;
        out.i64 = v;
        return true;
    }
    case PackedElemEncoding::Int64Wide: {
        std::int64_t v = 0;
        std::memcpy(&v, ptr, sizeof(v));
        out.kind = PodValKind::Int32;
        out.i64 = v;
        return true;
    }
    case PackedElemEncoding::Float32: {
        float v = 0.0f;
        std::memcpy(&v, ptr, sizeof(v));
        out.kind = PodValKind::Float32;
        out.f32 = v;
        return true;
    }
    case PackedElemEncoding::Float64: {
        double v = 0.0;
        std::memcpy(&v, ptr, sizeof(v));
        out.kind = PodValKind::Float64;
        out.f64 = v;
        return true;
    }
    case PackedElemEncoding::EnumInline: {
        std::int32_t v = 0;
        std::memcpy(&v, ptr, sizeof(v));
        out.kind = PodValKind::Enum64;
        out.i64 = static_cast<std::int64_t>(v);
        out.typeId = elemTid;
        return true;
    }
    case PackedElemEncoding::EnumWide: {
        std::int64_t v = 0;
        std::memcpy(&v, ptr, sizeof(v));
        out.kind = PodValKind::Enum64;
        out.i64 = v;
        out.typeId = elemTid;
        return true;
    }
    default:
        return false;
    }
}

[[nodiscard]] bool writePodIntoColumnElement(
    const ParallelNativeColumnBinding& col,
    std::size_t index,
    const PodSlot& val,
    bool& outNeedsWidenOrTypeErr) noexcept {
    outNeedsWidenOrTypeErr = false;
    if (col.readOnly || index >= col.count || !col.basePtr) return false;
    if (col.rawColumnView && col.rawColumnView->mode == RawColumnMode::EcsFields) {
        const auto& view = *col.rawColumnView;
        if (val.kind != PodValKind::PodStruct || val.typeId != view.structTypeId ||
            val.structFieldCount != view.fields.size()) {
            outNeedsWidenOrTypeErr = true;
            return false;
        }
        auto* elemBase = static_cast<std::uint8_t*>(view.basePtr) + index * view.strideBytes;
        for (std::size_t f = 0; f < view.fields.size(); ++f) {
            if (!writePodFieldToNativeBytes(elemBase, view.fields[f], val, f)) {
                outNeedsWidenOrTypeErr = true;
                return false;
            }
        }
        return true;
    }
    auto* ptr = static_cast<std::uint8_t*>(col.basePtr) + index * col.strideBytes;
    const PackedElemEncoding enc = (col.rawColumnView && col.rawColumnView->mode == RawColumnMode::ScalarPacked)
        ? col.rawColumnView->packedEncoding
        : col.packedEncoding;
    switch (enc) {
    case PackedElemEncoding::Bool: {
        if (val.kind != PodValKind::Bool) { outNeedsWidenOrTypeErr = true; return false; }
        const std::uint8_t b = val.b ? 1u : 0u;
        std::memcpy(ptr, &b, 1u);
        return true;
    }
    case PackedElemEncoding::Int32: {
        if (val.kind != PodValKind::Int32) { outNeedsWidenOrTypeErr = true; return false; }
        if (val.i64 < std::numeric_limits<std::int32_t>::min() ||
            val.i64 > std::numeric_limits<std::int32_t>::max()) {
            outNeedsWidenOrTypeErr = true;
            return false;
        }
        const auto v = static_cast<std::int32_t>(val.i64);
        std::memcpy(ptr, &v, sizeof(v));
        return true;
    }
    case PackedElemEncoding::Int64:
    case PackedElemEncoding::Int64Wide: {
        if (val.kind != PodValKind::Int64 && val.kind != PodValKind::Int32) {
            outNeedsWidenOrTypeErr = true;
            return false;
        }
        std::memcpy(ptr, &val.i64, sizeof(val.i64));
        return true;
    }
    case PackedElemEncoding::Float32: {
        if (val.kind != PodValKind::Float32) { outNeedsWidenOrTypeErr = true; return false; }
        std::memcpy(ptr, &val.f32, sizeof(val.f32));
        return true;
    }
    case PackedElemEncoding::Float64: {
        if (val.kind != PodValKind::Float64) { outNeedsWidenOrTypeErr = true; return false; }
        std::memcpy(ptr, &val.f64, sizeof(val.f64));
        return true;
    }
    case PackedElemEncoding::EnumInline: {
        if (val.kind != PodValKind::Enum64) { outNeedsWidenOrTypeErr = true; return false; }
        if (val.i64 < std::numeric_limits<std::int32_t>::min() ||
            val.i64 > std::numeric_limits<std::int32_t>::max()) {
            outNeedsWidenOrTypeErr = true;
            return false;
        }
        const auto v = static_cast<std::int32_t>(val.i64);
        std::memcpy(ptr, &v, sizeof(v));
        return true;
    }
    case PackedElemEncoding::EnumWide: {
        if (val.kind != PodValKind::Enum64) { outNeedsWidenOrTypeErr = true; return false; }
        std::memcpy(ptr, &val.i64, sizeof(val.i64));
        return true;
    }
    default:
        outNeedsWidenOrTypeErr = true;
        return false;
    }
}

[[nodiscard]] bool isNativeBatchColumnCompatible(const ParallelNativeColumnBinding& col) noexcept {
    if (col.rawColumnView && col.rawColumnView->mode == RawColumnMode::EcsFields) {
        if (col.rawColumnView->fields.empty() || col.rawColumnView->fields.size() > 16u) {
            return false;
        }
        for (const auto& f : col.rawColumnView->fields) {
            if (!isNativePodFieldKind(f.kind)) return false;
        }
        return true;
    }
    const PackedElemEncoding enc = (col.rawColumnView && col.rawColumnView->mode == RawColumnMode::ScalarPacked)
        ? col.rawColumnView->packedEncoding
        : col.packedEncoding;
    switch (enc) {
    case PackedElemEncoding::Bool:
    case PackedElemEncoding::Int32:
    case PackedElemEncoding::Int64:
    case PackedElemEncoding::Int64Wide:
    case PackedElemEncoding::Float32:
    case PackedElemEncoding::Float64:
    case PackedElemEncoding::EnumInline:
    case PackedElemEncoding::EnumWide:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] ParallelNativeBatchKernel tryBuildAutoNativeBatchKernel(
    const ProgramImage& program,
    const ProgramImage::FunctionLocation& entryLoc,
    ParallelKernelKind kernelKind,
    std::size_t prefixParamCount,
    const std::vector<ParallelNativeColumnBinding>& paramColumns,
    const std::unordered_map<semantic::SymbolId, ParallelNativeColumnBinding>& staticColumnBindings,
    const ParallelJobConfig& config,
    std::vector<ParallelNativeColumnBinding>& outCombinedColumns) {
    outCombinedColumns.clear();
    if (!entryLoc.function || !entryLoc.module) return {};
    if (config.executionOptions.profile != nullptr ||
        config.executionOptions.trace ||
        config.executionOptions.executionPolicy != nullptr ||
        config.executionOptions.debugger != nullptr) {
        return {};
    }
    const auto& fn = *entryLoc.function;
    const auto& mod = *entryLoc.module;
    if (fn.blocks.empty()) return {};

    // Check parameter & return & local & register types
    if (fn.returnType != semantic::PrimitiveType::Void &&
        !isNativeSupportedScalarPrim(fn.returnType)) {
        return {};
    }
    std::unordered_map<semantic::SymbolId, PodStructDesc> structDescs;
    auto inspectStructType = [&](semantic::SymbolId tid) -> bool {
        if (tid == 0) return false;
        if (structDescs.find(tid) != structDescs.end()) return true;
        if (isInlineBuiltInMathStruct(tid)) return false; // fall back for complex dual math structs
        const auto* t = program.resolveType(tid);
        if (!t || t->kind != semantic::TypeKind::Struct) return false;
        PodStructDesc desc;
        desc.typeId = tid;
        std::size_t count = 0;
        for (const auto& f : t->fields) {
            if (f.staticField || f.constField) continue;
            if (count >= 16u || !isNativeSupportedScalarPrim(f.type)) return false;
            desc.fieldTypes[count++] = f.type;
        }
        desc.fieldCount = static_cast<std::uint8_t>(count);
        structDescs.emplace(tid, desc);
        return true;
    };

    for (std::size_t p = 0; p < fn.parameterTypes.size(); ++p) {
        const auto pt = fn.parameterTypes[p];
        if (pt == semantic::PrimitiveType::Array) continue;
        if (pt == semantic::PrimitiveType::Struct) {
            const auto ptid = p < fn.parameterTypeIds.size() ? fn.parameterTypeIds[p] : 0;
            if (!inspectStructType(ptid)) return {};
        } else if (!isNativeSupportedScalarPrim(pt)) {
            return {};
        }
    }
    for (std::size_t l = 0; l < fn.localTypes.size(); ++l) {
        const auto lt = fn.localTypes[l];
        if (lt == semantic::PrimitiveType::Array) continue;
        if (lt == semantic::PrimitiveType::Struct) {
            const auto ltid = l < fn.localTypeIds.size() ? fn.localTypeIds[l] : 0;
            if (!inspectStructType(ltid)) return {};
        } else if (!isNativeSupportedScalarPrim(lt)) {
            return {};
        }
    }
    for (std::size_t r = 0; r < fn.registerTypes.size(); ++r) {
        const auto rt = fn.registerTypes[r];
        if (rt == semantic::PrimitiveType::Array || rt == semantic::PrimitiveType::Null ||
            rt == semantic::PrimitiveType::Void) {
            continue;
        }
        if (rt == semantic::PrimitiveType::Struct) {
            const auto rtid = r < fn.registerTypeIds.size() ? fn.registerTypeIds[r] : 0;
            if (!inspectStructType(rtid)) return {};
        } else if (!isNativeSupportedScalarPrim(rt)) {
            return {};
        }
    }

    // Check column bindings compatibility
    std::vector<std::int32_t> paramToCombinedCol(fn.parameterTypes.size(), -1);
    for (const auto& binding : paramColumns) {
        if (!isNativeBatchColumnCompatible(binding)) return {};
        const std::int32_t idx = static_cast<std::int32_t>(outCombinedColumns.size());
        outCombinedColumns.push_back(binding);
        if (binding.parameterIndex < paramToCombinedCol.size()) {
            paramToCombinedCol[binding.parameterIndex] = idx;
        }
    }

    std::unordered_map<semantic::SymbolId, std::int32_t> staticIdToCombinedCol;
    for (const auto& [sid, binding] : staticColumnBindings) {
        if (!isNativeBatchColumnCompatible(binding)) return {};
        const std::int32_t idx = static_cast<std::int32_t>(outCombinedColumns.size());
        outCombinedColumns.push_back(binding);
        staticIdToCombinedCol.emplace(sid, idx);
    }

    // Build dense BlockId map and inspect all instructions
    std::unordered_map<bytecode::BlockId, std::size_t> blockIdToDense;
    blockIdToDense.reserve(fn.blocks.size());
    for (std::size_t i = 0; i < fn.blocks.size(); ++i) {
        blockIdToDense.emplace(fn.blocks[i].id, i);
    }

    struct ResolvedCallTarget {
        bool isStaticColLoad = false;
        std::int32_t colIndex = -1;
    };
    struct CompiledBlock {
        const bytecode::BasicBlock* bc = nullptr;
        std::size_t jumpTargetDense = 0;
        std::size_t falseTargetDense = 0;
        std::vector<ResolvedCallTarget> callTargets;
    };
    std::vector<CompiledBlock> compiledBlocks(fn.blocks.size());

    for (std::size_t b = 0; b < fn.blocks.size(); ++b) {
        const auto& block = fn.blocks[b];
        auto& cb = compiledBlocks[b];
        cb.bc = &block;
        cb.callTargets.resize(block.instructions.size());

        for (std::size_t ii = 0; ii < block.instructions.size(); ++ii) {
            const auto& inst = block.instructions[ii];
            switch (inst.opcode) {
            case bytecode::Opcode::LoadParameter:
            case bytecode::Opcode::LoadLocal:
            case bytecode::Opcode::StoreLocal:
            case bytecode::Opcode::ConstantInt:
            case bytecode::Opcode::ConstantDouble:
            case bytecode::Opcode::ConstantBool:
            case bytecode::Opcode::ConstantNull:
            case bytecode::Opcode::ConvertNullToArray:
            case bytecode::Opcode::CheckNotNull:
            case bytecode::Opcode::ConvertIntToLong:
            case bytecode::Opcode::ConvertIntToDouble:
            case bytecode::Opcode::ConvertLongToDouble:
            case bytecode::Opcode::ConvertNumeric:
            case bytecode::Opcode::ArrayLength:
            case bytecode::Opcode::LoadElement:
            case bytecode::Opcode::StoreElement:
            case bytecode::Opcode::NegateInt:
            case bytecode::Opcode::NegateLong:
            case bytecode::Opcode::NegateDouble:
            case bytecode::Opcode::LogicalNot:
            case bytecode::Opcode::Equal:
            case bytecode::Opcode::NotEqual:
            case bytecode::Opcode::AddInt:
            case bytecode::Opcode::SubtractInt:
            case bytecode::Opcode::MultiplyInt:
            case bytecode::Opcode::DivideInt:
            case bytecode::Opcode::RemainderInt:
            case bytecode::Opcode::LessInt:
            case bytecode::Opcode::LessOrEqualInt:
            case bytecode::Opcode::GreaterInt:
            case bytecode::Opcode::GreaterOrEqualInt:
            case bytecode::Opcode::AddLong:
            case bytecode::Opcode::SubtractLong:
            case bytecode::Opcode::MultiplyLong:
            case bytecode::Opcode::DivideLong:
            case bytecode::Opcode::RemainderLong:
            case bytecode::Opcode::LessLong:
            case bytecode::Opcode::LessOrEqualLong:
            case bytecode::Opcode::GreaterLong:
            case bytecode::Opcode::GreaterOrEqualLong:
            case bytecode::Opcode::AddDouble:
            case bytecode::Opcode::SubtractDouble:
            case bytecode::Opcode::MultiplyDouble:
            case bytecode::Opcode::DivideDouble:
            case bytecode::Opcode::LessDouble:
            case bytecode::Opcode::LessOrEqualDouble:
            case bytecode::Opcode::GreaterDouble:
            case bytecode::Opcode::GreaterOrEqualDouble:
                break;
            case bytecode::Opcode::NewStruct: {
                const semantic::SymbolId tid = inst.typeIndex < mod.types.size()
                    ? mod.types[inst.typeIndex].id
                    : inst.activeElementTypeId();
                if (!inspectStructType(tid)) return {};
                break;
            }
            case bytecode::Opcode::LoadStructField:
            case bytecode::Opcode::StoreStructField: {
                const semantic::SymbolId tid = inst.typeIndex < mod.types.size()
                    ? mod.types[inst.typeIndex].id
                    : 0;
                if (!inspectStructType(tid)) return {};
                const auto& desc = structDescs.at(tid);
                if (inst.index >= desc.fieldCount) return {};
                break;
            }
            case bytecode::Opcode::Call: {
                if (inst.index >= mod.functionReferences.size()) return {};
                const auto& ref = mod.functionReferences[inst.index];
                constexpr std::string_view kStaticGet = "$intrinsic::static_get::";
                if (ref.name.rfind(kStaticGet, 0) != 0) return {};
                const std::string_view subOp = std::string_view(ref.name).substr(kStaticGet.size());
                semantic::SymbolId sid = 0;
                if (!tryParseReadOnlyEcsColDenseId(subOp, sid) &&
                    !tryParseNumericStaticFieldId(subOp, sid)) {
                    return {};
                }
                const auto sit = staticIdToCombinedCol.find(sid);
                if (sit == staticIdToCombinedCol.end()) return {};
                cb.callTargets[ii] = ResolvedCallTarget{true, sit->second};
                break;
            }
            default:
                return {};
            }

            if ((inst.opcode >= bytecode::Opcode::AddInt &&
                 inst.opcode <= bytecode::Opcode::GreaterOrEqualInt) &&
                inst.opcode != bytecode::Opcode::Equal &&
                inst.opcode != bytecode::Opcode::NotEqual) {
                if (inst.operands.size() < 2u ||
                    inst.operands[0] >= fn.registerTypes.size() ||
                    inst.operands[1] >= fn.registerTypes.size() ||
                    fn.registerTypes[inst.operands[0]] != semantic::PrimitiveType::Int ||
                    fn.registerTypes[inst.operands[1]] != semantic::PrimitiveType::Int) {
                    return {};
                }
            } else if (inst.opcode >= bytecode::Opcode::AddLong &&
                       inst.opcode <= bytecode::Opcode::GreaterOrEqualLong) {
                if (inst.operands.size() < 2u ||
                    inst.operands[0] >= fn.registerTypes.size() ||
                    inst.operands[1] >= fn.registerTypes.size() ||
                    fn.registerTypes[inst.operands[0]] != semantic::PrimitiveType::Long ||
                    fn.registerTypes[inst.operands[1]] != semantic::PrimitiveType::Long) {
                    return {};
                }
            }
        }

        switch (block.terminator.kind) {
        case bytecode::TerminatorKind::ReturnVoid:
        case bytecode::TerminatorKind::ReturnValue:
            break;
        case bytecode::TerminatorKind::Jump: {
            const auto it = blockIdToDense.find(block.terminator.target);
            if (it == blockIdToDense.end()) return {};
            cb.jumpTargetDense = it->second;
            break;
        }
        case bytecode::TerminatorKind::Branch: {
            const auto tIt = blockIdToDense.find(block.terminator.target);
            const auto fIt = blockIdToDense.find(block.terminator.falseTarget);
            if (tIt == blockIdToDense.end() || fIt == blockIdToDense.end()) return {};
            cb.jumpTargetDense = tIt->second;
            cb.falseTargetDense = fIt->second;
            break;
        }
        default:
            return {};
        }
    }

    // Build self-contained native execution lambda (zero VM/Interpreter objects!)
    struct SharedNativePlan {
        const bytecode::Function* fn = nullptr;
        const bytecode::Module* mod = nullptr;
        ParallelKernelKind kernelKind = ParallelKernelKind::Auto;
        std::size_t prefixParamCount = 1u;
        std::vector<std::int32_t> paramToCombinedCol;
        std::unordered_map<semantic::SymbolId, PodStructDesc> structDescs;
        std::vector<CompiledBlock> blocks;
        ParallelReduceOp reduceOp = ParallelReduceOp::None;
        ParallelCustomReducer customReducer;
        bool strictDet = false;
        std::uint64_t instructionBudget = 0;
    };
    auto plan = std::make_shared<SharedNativePlan>();
    plan->fn = &fn;
    plan->mod = &mod;
    plan->kernelKind = kernelKind;
    plan->prefixParamCount = prefixParamCount;
    plan->paramToCombinedCol = std::move(paramToCombinedCol);
    plan->structDescs = std::move(structDescs);
    plan->blocks = std::move(compiledBlocks);
    plan->reduceOp = config.reduceOp;
    plan->customReducer = config.customReducer;
    plan->strictDet = (config.executionOptions.determinism.mode == DeterminismMode::Strict);
    plan->instructionBudget = config.executionOptions.limits.instructionBudget;

    return [plan](ParallelNativeBatchContext& ctx) -> bool {
        if (!ctx.columns || !ctx.userArguments) return false;
        const auto& fnRef = *plan->fn;
        const auto& cols = *ctx.columns;
        const std::size_t nRegs = fnRef.registerTypes.size();
        const std::size_t nLocals = fnRef.localTypes.size();

        std::vector<PodSlot> params(fnRef.parameterTypes.size());
        for (std::size_t p = 0; p < fnRef.parameterTypes.size(); ++p) {
            if (fnRef.parameterTypes[p] == semantic::PrimitiveType::Array) {
                params[p].kind = PodValKind::ColumnRef;
                params[p].colIndex = plan->paramToCombinedCol[p];
            } else if (p >= plan->prefixParamCount) {
                const std::size_t u = p - plan->prefixParamCount;
                if (!podSlotFromValue((*ctx.userArguments)[u], fnRef.parameterTypes[p], params[p])) {
                    ctx.error.code = ErrorCode::TypeMismatch;
                    ctx.error.message = "native batch parameter conversion failed";
                    return false;
                }
            }
        }

        std::vector<PodSlot> defaultLocals(nLocals);
        for (std::size_t l = 0; l < nLocals; ++l) {
            switch (fnRef.localTypes[l]) {
            case semantic::PrimitiveType::Bool:
                defaultLocals[l].kind = PodValKind::Bool;
                defaultLocals[l].b = false;
                break;
            case semantic::PrimitiveType::Int:
                defaultLocals[l].kind = PodValKind::Int32;
                defaultLocals[l].i64 = 0;
                break;
            case semantic::PrimitiveType::Long:
                defaultLocals[l].kind = PodValKind::Int64;
                defaultLocals[l].i64 = 0;
                break;
            case semantic::PrimitiveType::Float:
                defaultLocals[l].kind = PodValKind::Float32;
                defaultLocals[l].f32 = 0.0f;
                break;
            case semantic::PrimitiveType::Double:
                defaultLocals[l].kind = PodValKind::Float64;
                defaultLocals[l].f64 = 0.0;
                break;
            case semantic::PrimitiveType::Enum:
                defaultLocals[l].kind = PodValKind::Enum64;
                defaultLocals[l].i64 = 0;
                defaultLocals[l].typeId = l < fnRef.localTypeIds.size() ? fnRef.localTypeIds[l] : 0;
                break;
            case semantic::PrimitiveType::Array:
                defaultLocals[l].kind = PodValKind::ColumnRef;
                defaultLocals[l].colIndex = -1;
                break;
            case semantic::PrimitiveType::Struct: {
                const auto ltid = l < fnRef.localTypeIds.size() ? fnRef.localTypeIds[l] : 0;
                const auto dIt = plan->structDescs.find(ltid);
                if (dIt == plan->structDescs.end()) {
                    ctx.error.code = ErrorCode::InvalidProgram;
                    ctx.error.message = "missing native POD struct layout";
                    return false;
                }
                const auto& desc = dIt->second;
                defaultLocals[l].kind = PodValKind::PodStruct;
                defaultLocals[l].typeId = ltid;
                defaultLocals[l].structFieldCount = desc.fieldCount;
                for (std::size_t f = 0; f < desc.fieldCount; ++f) {
                    const auto ft = desc.fieldTypes[f];
                    if (ft == semantic::PrimitiveType::Bool) defaultLocals[l].fieldKinds[f] = PodValKind::Bool;
                    else if (ft == semantic::PrimitiveType::Long) defaultLocals[l].fieldKinds[f] = PodValKind::Int64;
                    else if (ft == semantic::PrimitiveType::Float) defaultLocals[l].fieldKinds[f] = PodValKind::Float32;
                    else if (ft == semantic::PrimitiveType::Double) defaultLocals[l].fieldKinds[f] = PodValKind::Float64;
                    else if (ft == semantic::PrimitiveType::Enum) defaultLocals[l].fieldKinds[f] = PodValKind::Enum64;
                    else defaultLocals[l].fieldKinds[f] = PodValKind::Int32;
                }
                break;
            }
            default:
                break;
            }
        }

        std::vector<PodSlot> regs(nRegs);
        std::vector<PodSlot> locals(nLocals);
        std::vector<PodSlot> edgeArgs;
        edgeArgs.reserve(16u);

        auto failNative = [&](ErrorCode code, std::string msg) -> bool {
            ctx.error.code = code;
            ctx.error.message = std::move(msg);
            return false;
        };

        auto checkedInt32Op = [&](std::int64_t val, bool checked, PodSlot& out) -> bool {
            if (!checked) {
                const auto u32 = static_cast<std::uint32_t>(val);
                const auto s32 = static_cast<std::int32_t>(u32);
                out.kind = PodValKind::Int32;
                out.i64 = static_cast<std::int64_t>(s32);
                return true;
            }
            if (val < std::numeric_limits<std::int32_t>::min() ||
                val > std::numeric_limits<std::int32_t>::max()) {
                return failNative(ErrorCode::IntegerOverflow, "integer overflow");
            }
            out.kind = PodValKind::Int32;
            out.i64 = val;
            return true;
        };

        auto executeSingleInvocation = [&](Value& outReturnValue) -> bool {
            locals = defaultLocals;
            std::size_t denseBlockIdx = 0u;
            std::uint64_t executed = 0u;
            const std::uint64_t budget = plan->instructionBudget;

            for (;;) {
                const auto& cb = plan->blocks[denseBlockIdx];
                const auto& block = *cb.bc;

                for (std::size_t ii = 0; ii < block.instructions.size(); ++ii) {
                    if (executed >= budget) {
                        return failNative(ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
                    }
                    ++executed;
                    ++ctx.statistics.instructionsExecuted;

                    const auto& inst = block.instructions[ii];
                    const auto res = inst.result;
                    auto storeScalar = [&](PodSlot v) {
                        if (res != bytecode::InvalidRegister && res < nRegs) regs[res] = v;
                    };

                    switch (inst.opcode) {
                    case bytecode::Opcode::LoadParameter:
                        if (inst.index < params.size()) storeScalar(params[inst.index]);
                        break;
                    case bytecode::Opcode::LoadLocal:
                        if (inst.index < locals.size()) storeScalar(locals[inst.index]);
                        break;
                    case bytecode::Opcode::StoreLocal:
                        if (inst.index < locals.size() && !inst.operands.empty() && inst.operands[0] < nRegs) {
                            locals[inst.index] = regs[inst.operands[0]];
                        }
                        break;
                    case bytecode::Opcode::ConstantInt: {
                        PodSlot s{};
                        if (res < fnRef.registerTypes.size() && fnRef.registerTypes[res] == semantic::PrimitiveType::Long) {
                            s.kind = PodValKind::Int64;
                        } else if (res < fnRef.registerTypes.size() && fnRef.registerTypes[res] == semantic::PrimitiveType::Enum) {
                            s.kind = PodValKind::Enum64;
                            s.typeId = res < fnRef.registerTypeIds.size() ? fnRef.registerTypeIds[res] : 0;
                        } else {
                            s.kind = PodValKind::Int32;
                        }
                        s.i64 = inst.integerImmediate;
                        storeScalar(s);
                        break;
                    }
                    case bytecode::Opcode::ConstantDouble: {
                        PodSlot s{};
                        if (res < fnRef.registerTypes.size() && fnRef.registerTypes[res] == semantic::PrimitiveType::Float) {
                            s.kind = PodValKind::Float32;
                            s.f32 = static_cast<float>(inst.doubleImmediate);
                        } else {
                            s.kind = PodValKind::Float64;
                            s.f64 = inst.doubleImmediate;
                        }
                        storeScalar(s);
                        break;
                    }
                    case bytecode::Opcode::ConstantBool: {
                        PodSlot s{};
                        s.kind = PodValKind::Bool;
                        s.b = inst.boolImmediate;
                        storeScalar(s);
                        break;
                    }
                    case bytecode::Opcode::ConstantNull:
                    case bytecode::Opcode::ConvertNullToArray: {
                        PodSlot s{};
                        s.kind = (inst.opcode == bytecode::Opcode::ConvertNullToArray)
                            ? PodValKind::ColumnRef
                            : PodValKind::Null;
                        s.colIndex = -1;
                        storeScalar(s);
                        break;
                    }
                    case bytecode::Opcode::CheckNotNull: {
                        const auto& v = regs[inst.operands[0]];
                        if (v.kind == PodValKind::Null ||
                            (v.kind == PodValKind::ColumnRef && v.colIndex < 0)) {
                            return failNative(ErrorCode::NullReference, "null reference");
                        }
                        storeScalar(v);
                        break;
                    }
                    case bytecode::Opcode::ConvertIntToLong: {
                        const auto& v = regs[inst.operands[0]];
                        PodSlot s{};
                        s.kind = PodValKind::Int64;
                        s.i64 = v.i64;
                        storeScalar(s);
                        break;
                    }
                    case bytecode::Opcode::ConvertIntToDouble:
                    case bytecode::Opcode::ConvertLongToDouble: {
                        const auto& v = regs[inst.operands[0]];
                        PodSlot s{};
                        if (res < fnRef.registerTypes.size() && fnRef.registerTypes[res] == semantic::PrimitiveType::Float) {
                            s.kind = PodValKind::Float32;
                            s.f32 = static_cast<float>(v.i64);
                        } else {
                            s.kind = PodValKind::Float64;
                            s.f64 = static_cast<double>(v.i64);
                        }
                        storeScalar(s);
                        break;
                    }
                    case bytecode::Opcode::ConvertNumeric: {
                        const auto srcPrim = fnRef.registerTypes[inst.operands[0]];
                        const auto dstPrim = fnRef.registerTypes[res];
                        const Value boxed = podSlotToValue(regs[inst.operands[0]], srcPrim);
                        Value conv{};
                        if (!tryConvertNumeric(boxed, dstPrim, conv, inst.checkedArithmetic)) {
                            return failNative(ErrorCode::IntegerOverflow, "numeric conversion overflow");
                        }
                        PodSlot s{};
                        if (!podSlotFromValue(conv, dstPrim, s)) {
                            return failNative(ErrorCode::TypeMismatch, "numeric conversion failed");
                        }
                        storeScalar(s);
                        break;
                    }
                    case bytecode::Opcode::Call: {
                        const auto& ct = cb.callTargets[ii];
                        PodSlot s{};
                        s.kind = PodValKind::ColumnRef;
                        s.colIndex = ct.colIndex;
                        storeScalar(s);
                        break;
                    }
                    case bytecode::Opcode::ArrayLength: {
                        const auto& arr = regs[inst.operands[0]];
                        if (arr.kind != PodValKind::ColumnRef || arr.colIndex < 0 ||
                            static_cast<std::size_t>(arr.colIndex) >= cols.size()) {
                            return failNative(ErrorCode::NullReference, "null array reference");
                        }
                        PodSlot s{};
                        s.kind = PodValKind::Int32;
                        s.i64 = static_cast<std::int64_t>(cols[arr.colIndex].count);
                        storeScalar(s);
                        break;
                    }
                    case bytecode::Opcode::LoadElement: {
                        const auto& arr = regs[inst.operands[0]];
                        const auto& idxReg = regs[inst.operands[1]];
                        if (arr.kind != PodValKind::ColumnRef || arr.colIndex < 0 ||
                            static_cast<std::size_t>(arr.colIndex) >= cols.size()) {
                            return failNative(ErrorCode::NullReference, "null array reference");
                        }
                        if (idxReg.i64 < 0) {
                            return failNative(ErrorCode::IndexOutOfRange, "array index out of range");
                        }
                        const auto& col = cols[arr.colIndex];
                        const std::size_t elemIdx = static_cast<std::size_t>(idxReg.i64);
                        if (elemIdx >= col.count) {
                            return failNative(ErrorCode::IndexOutOfRange, "array index out of range");
                        }
                        PodSlot out{};
                        if (!readColumnElementIntoPod(col, elemIdx, out)) {
                            return failNative(ErrorCode::TypeMismatch, "native column read failed");
                        }
                        storeScalar(out);
                        break;
                    }
                    case bytecode::Opcode::StoreElement: {
                        const auto& arr = regs[inst.operands[0]];
                        const auto& idxReg = regs[inst.operands[1]];
                        const auto& valReg = regs[inst.operands[2]];
                        if (arr.kind != PodValKind::ColumnRef || arr.colIndex < 0 ||
                            static_cast<std::size_t>(arr.colIndex) >= cols.size()) {
                            return failNative(ErrorCode::NullReference, "null array reference");
                        }
                        if (idxReg.i64 < 0) {
                            return failNative(ErrorCode::IndexOutOfRange, "array index out of range");
                        }
                        const auto& col = cols[arr.colIndex];
                        if (col.readOnly) {
                            return failNative(ErrorCode::CapabilityDenied, "cannot write to read-only raw column array");
                        }
                        const std::size_t elemIdx = static_cast<std::size_t>(idxReg.i64);
                        if (elemIdx >= col.count) {
                            return failNative(ErrorCode::IndexOutOfRange, "array index out of range");
                        }
                        bool typeOrWidenErr = false;
                        if (!writePodIntoColumnElement(col, elemIdx, valReg, typeOrWidenErr)) {
                            return failNative(
                                typeOrWidenErr ? ErrorCode::TypeMismatch : ErrorCode::InvalidArguments,
                                "native column write failed");
                        }
                        break;
                    }
                    case bytecode::Opcode::NewStruct: {
                        const semantic::SymbolId tid = inst.typeIndex < plan->mod->types.size()
                            ? plan->mod->types[inst.typeIndex].id
                            : inst.activeElementTypeId();
                        const auto& desc = plan->structDescs.at(tid);
                        PodSlot s{};
                        s.kind = PodValKind::PodStruct;
                        s.typeId = tid;
                        s.structFieldCount = desc.fieldCount;
                        for (std::size_t f = 0; f < desc.fieldCount; ++f) {
                            const auto ft = desc.fieldTypes[f];
                            if (ft == semantic::PrimitiveType::Bool) s.fieldKinds[f] = PodValKind::Bool;
                            else if (ft == semantic::PrimitiveType::Long) s.fieldKinds[f] = PodValKind::Int64;
                            else if (ft == semantic::PrimitiveType::Float) s.fieldKinds[f] = PodValKind::Float32;
                            else if (ft == semantic::PrimitiveType::Double) s.fieldKinds[f] = PodValKind::Float64;
                            else if (ft == semantic::PrimitiveType::Enum) s.fieldKinds[f] = PodValKind::Enum64;
                            else s.fieldKinds[f] = PodValKind::Int32;
                        }
                        storeScalar(s);
                        break;
                    }
                    case bytecode::Opcode::LoadStructField: {
                        const auto& recv = regs[inst.operands[0]];
                        if (recv.kind != PodValKind::PodStruct || inst.index >= recv.structFieldCount) {
                            return failNative(ErrorCode::InvalidProgram, "invalid struct field read");
                        }
                        PodSlot fval{};
                        fval.kind = recv.fieldKinds[inst.index];
                        fval.i64 = recv.fieldI64[inst.index];
                        fval.f64 = recv.fieldF64[inst.index];
                        fval.f32 = recv.fieldF32[inst.index];
                        fval.b = recv.fieldBool[inst.index];
                        fval.typeId = recv.fieldTypeIds[inst.index];
                        storeScalar(fval);
                        break;
                    }
                    case bytecode::Opcode::StoreStructField: {
                        PodSlot copy = regs[inst.operands[0]];
                        if (copy.kind != PodValKind::PodStruct || inst.index >= copy.structFieldCount) {
                            return failNative(ErrorCode::InvalidProgram, "invalid struct field write");
                        }
                        const auto& newVal = regs[inst.operands[1]];
                        copy.fieldKinds[inst.index] = newVal.kind;
                        copy.fieldI64[inst.index] = newVal.i64;
                        copy.fieldF64[inst.index] = newVal.f64;
                        copy.fieldF32[inst.index] = newVal.f32;
                        copy.fieldBool[inst.index] = newVal.b;
                        copy.fieldTypeIds[inst.index] = newVal.typeId;
                        storeScalar(copy);
                        break;
                    }
                    case bytecode::Opcode::NegateInt: {
                        const std::int64_t a = regs[inst.operands[0]].i64;
                        if (inst.checkedArithmetic && a == std::numeric_limits<std::int32_t>::min()) {
                            return failNative(ErrorCode::IntegerOverflow, "integer negation overflow");
                        }
                        PodSlot out{};
                        if (!checkedInt32Op(-a, inst.checkedArithmetic, out)) return false;
                        storeScalar(out);
                        break;
                    }
                    case bytecode::Opcode::NegateLong: {
                        const std::int64_t a = regs[inst.operands[0]].i64;
                        if (inst.checkedArithmetic && a == std::numeric_limits<std::int64_t>::min()) {
                            return failNative(ErrorCode::IntegerOverflow, "long negation overflow");
                        }
                        PodSlot out{};
                        out.kind = PodValKind::Int64;
                        out.i64 = static_cast<std::int64_t>(0ULL - static_cast<std::uint64_t>(a));
                        storeScalar(out);
                        break;
                    }
                    case bytecode::Opcode::NegateDouble: {
                        const auto& v = regs[inst.operands[0]];
                        PodSlot out{};
                        out.kind = v.kind;
                        if (v.kind == PodValKind::Float32) out.f32 = -v.f32;
                        else out.f64 = -v.f64;
                        storeScalar(out);
                        break;
                    }
                    case bytecode::Opcode::LogicalNot: {
                        PodSlot out{};
                        out.kind = PodValKind::Bool;
                        out.b = !regs[inst.operands[0]].b;
                        storeScalar(out);
                        break;
                    }
                    case bytecode::Opcode::Equal:
                    case bytecode::Opcode::NotEqual: {
                        const auto& l = regs[inst.operands[0]];
                        const auto& r = regs[inst.operands[1]];
                        bool eq = false;
                        if (l.kind == r.kind) {
                            switch (l.kind) {
                            case PodValKind::Bool: eq = (l.b == r.b); break;
                            case PodValKind::Int32:
                            case PodValKind::Int64: eq = (l.i64 == r.i64); break;
                            case PodValKind::Enum64: eq = (l.i64 == r.i64 && l.typeId == r.typeId); break;
                            case PodValKind::Float32: eq = (l.f32 == r.f32); break;
                            case PodValKind::Float64: eq = (l.f64 == r.f64); break;
                            case PodValKind::ColumnRef: eq = (l.colIndex == r.colIndex); break;
                            case PodValKind::Null: eq = true; break;
                            default: eq = false; break;
                            }
                        }
                        PodSlot out{};
                        out.kind = PodValKind::Bool;
                        out.b = (inst.opcode == bytecode::Opcode::Equal) ? eq : !eq;
                        storeScalar(out);
                        break;
                    }
                    case bytecode::Opcode::AddInt:
                    case bytecode::Opcode::SubtractInt:
                    case bytecode::Opcode::MultiplyInt:
                    case bytecode::Opcode::DivideInt:
                    case bytecode::Opcode::RemainderInt: {
                        const std::int64_t a = regs[inst.operands[0]].i64;
                        const std::int64_t b = regs[inst.operands[1]].i64;
                        PodSlot out{};
                        if (inst.opcode == bytecode::Opcode::AddInt) {
                            if (!checkedInt32Op(a + b, inst.checkedArithmetic, out)) return false;
                        } else if (inst.opcode == bytecode::Opcode::SubtractInt) {
                            if (!checkedInt32Op(a - b, inst.checkedArithmetic, out)) return false;
                        } else if (inst.opcode == bytecode::Opcode::MultiplyInt) {
                            if (!checkedInt32Op(a * b, inst.checkedArithmetic, out)) return false;
                        } else if (inst.opcode == bytecode::Opcode::DivideInt) {
                            if (b == 0) return failNative(ErrorCode::DivisionByZero, "division by zero");
                            if (a == std::numeric_limits<std::int32_t>::min() && b == -1) {
                                return failNative(ErrorCode::IntegerOverflow, "integer division overflow");
                            }
                            out.kind = PodValKind::Int32;
                            out.i64 = a / b;
                        } else {
                            if (b == 0) return failNative(ErrorCode::DivisionByZero, "remainder by zero");
                            out.kind = PodValKind::Int32;
                            out.i64 = (a == std::numeric_limits<std::int32_t>::min() && b == -1) ? 0 : (a % b);
                        }
                        storeScalar(out);
                        break;
                    }
                    case bytecode::Opcode::LessInt:
                    case bytecode::Opcode::LessOrEqualInt:
                    case bytecode::Opcode::GreaterInt:
                    case bytecode::Opcode::GreaterOrEqualInt: {
                        const std::int64_t a = regs[inst.operands[0]].i64;
                        const std::int64_t b = regs[inst.operands[1]].i64;
                        PodSlot out{};
                        out.kind = PodValKind::Bool;
                        if (inst.opcode == bytecode::Opcode::LessInt) out.b = (a < b);
                        else if (inst.opcode == bytecode::Opcode::LessOrEqualInt) out.b = (a <= b);
                        else if (inst.opcode == bytecode::Opcode::GreaterInt) out.b = (a > b);
                        else out.b = (a >= b);
                        storeScalar(out);
                        break;
                    }
                    case bytecode::Opcode::AddLong:
                    case bytecode::Opcode::SubtractLong:
                    case bytecode::Opcode::MultiplyLong:
                    case bytecode::Opcode::DivideLong:
                    case bytecode::Opcode::RemainderLong: {
                        const std::int64_t a = regs[inst.operands[0]].i64;
                        const std::int64_t b = regs[inst.operands[1]].i64;
                        PodSlot out{};
                        out.kind = PodValKind::Int64;
                        if (inst.opcode == bytecode::Opcode::AddLong) {
                            if (inst.checkedArithmetic &&
                                ((b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) ||
                                 (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b))) {
                                return failNative(ErrorCode::IntegerOverflow, "long addition overflow");
                            }
                            out.i64 = static_cast<std::int64_t>(
                                static_cast<std::uint64_t>(a) + static_cast<std::uint64_t>(b));
                        } else if (inst.opcode == bytecode::Opcode::SubtractLong) {
                            if (inst.checkedArithmetic &&
                                ((b < 0 && a > std::numeric_limits<std::int64_t>::max() + b) ||
                                 (b > 0 && a < std::numeric_limits<std::int64_t>::min() + b))) {
                                return failNative(ErrorCode::IntegerOverflow, "long subtraction overflow");
                            }
                            out.i64 = static_cast<std::int64_t>(
                                static_cast<std::uint64_t>(a) - static_cast<std::uint64_t>(b));
                        } else if (inst.opcode == bytecode::Opcode::MultiplyLong) {
                            if (inst.checkedArithmetic && a != 0 && b != 0) {
                                if ((a == -1 && b == std::numeric_limits<std::int64_t>::min()) ||
                                    (b == -1 && a == std::numeric_limits<std::int64_t>::min())) {
                                    return failNative(ErrorCode::IntegerOverflow, "long multiplication overflow");
                                }
                                const auto absA = a < 0 ? (0ULL - static_cast<std::uint64_t>(a)) : static_cast<std::uint64_t>(a);
                                const auto absB = b < 0 ? (0ULL - static_cast<std::uint64_t>(b)) : static_cast<std::uint64_t>(b);
                                const auto limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) +
                                    ((a < 0) != (b < 0) ? 1ULL : 0ULL);
                                if (absA > limit / absB) {
                                    return failNative(ErrorCode::IntegerOverflow, "long multiplication overflow");
                                }
                            }
                            out.i64 = static_cast<std::int64_t>(
                                static_cast<std::uint64_t>(a) * static_cast<std::uint64_t>(b));
                        } else if (inst.opcode == bytecode::Opcode::DivideLong) {
                            if (b == 0) return failNative(ErrorCode::DivisionByZero, "division by zero");
                            if (a == std::numeric_limits<std::int64_t>::min() && b == -1) {
                                return failNative(ErrorCode::IntegerOverflow, "long division overflow");
                            }
                            out.i64 = a / b;
                        } else {
                            if (b == 0) return failNative(ErrorCode::DivisionByZero, "remainder by zero");
                            out.i64 = (a == std::numeric_limits<std::int64_t>::min() && b == -1) ? 0 : (a % b);
                        }
                        storeScalar(out);
                        break;
                    }
                    case bytecode::Opcode::LessLong:
                    case bytecode::Opcode::LessOrEqualLong:
                    case bytecode::Opcode::GreaterLong:
                    case bytecode::Opcode::GreaterOrEqualLong: {
                        const std::int64_t a = regs[inst.operands[0]].i64;
                        const std::int64_t b = regs[inst.operands[1]].i64;
                        PodSlot out{};
                        out.kind = PodValKind::Bool;
                        if (inst.opcode == bytecode::Opcode::LessLong) out.b = (a < b);
                        else if (inst.opcode == bytecode::Opcode::LessOrEqualLong) out.b = (a <= b);
                        else if (inst.opcode == bytecode::Opcode::GreaterLong) out.b = (a > b);
                        else out.b = (a >= b);
                        storeScalar(out);
                        break;
                    }
                    case bytecode::Opcode::AddDouble:
                    case bytecode::Opcode::SubtractDouble:
                    case bytecode::Opcode::MultiplyDouble:
                    case bytecode::Opcode::DivideDouble:
                    case bytecode::Opcode::LessDouble:
                    case bytecode::Opcode::LessOrEqualDouble:
                    case bytecode::Opcode::GreaterDouble:
                    case bytecode::Opcode::GreaterOrEqualDouble: {
                        const auto& l = regs[inst.operands[0]];
                        const auto& r = regs[inst.operands[1]];
                        PodSlot out{};
                        if (l.kind == PodValKind::Float32 && r.kind == PodValKind::Float32) {
                            const float a = l.f32;
                            const float b = r.f32;
                            if (inst.opcode >= bytecode::Opcode::LessDouble) {
                                out.kind = PodValKind::Bool;
                                if (inst.opcode == bytecode::Opcode::LessDouble) out.b = (a < b);
                                else if (inst.opcode == bytecode::Opcode::LessOrEqualDouble) out.b = (a <= b);
                                else if (inst.opcode == bytecode::Opcode::GreaterDouble) out.b = (a > b);
                                else out.b = (a >= b);
                            } else {
                                out.kind = PodValKind::Float32;
                                if (inst.opcode == bytecode::Opcode::AddDouble) out.f32 = a + b;
                                else if (inst.opcode == bytecode::Opcode::SubtractDouble) out.f32 = a - b;
                                else if (inst.opcode == bytecode::Opcode::MultiplyDouble) out.f32 = a * b;
                                else out.f32 = a / b;
                            }
                        } else {
                            const double a = (l.kind == PodValKind::Float32) ? static_cast<double>(l.f32) : l.f64;
                            const double b = (r.kind == PodValKind::Float32) ? static_cast<double>(r.f32) : r.f64;
                            if (inst.opcode >= bytecode::Opcode::LessDouble) {
                                out.kind = PodValKind::Bool;
                                if (inst.opcode == bytecode::Opcode::LessDouble) out.b = (a < b);
                                else if (inst.opcode == bytecode::Opcode::LessOrEqualDouble) out.b = (a <= b);
                                else if (inst.opcode == bytecode::Opcode::GreaterDouble) out.b = (a > b);
                                else out.b = (a >= b);
                            } else {
                                out.kind = PodValKind::Float64;
                                if (inst.opcode == bytecode::Opcode::AddDouble) out.f64 = a + b;
                                else if (inst.opcode == bytecode::Opcode::SubtractDouble) out.f64 = a - b;
                                else if (inst.opcode == bytecode::Opcode::MultiplyDouble) out.f64 = a * b;
                                else out.f64 = a / b;
                            }
                        }
                        storeScalar(out);
                        break;
                    }
                    default:
                        return failNative(ErrorCode::InvalidProgram, "unsupported native batch opcode");
                    }
                }

                const auto& term = block.terminator;
                if (term.kind == bytecode::TerminatorKind::ReturnVoid) {
                    outReturnValue = Value{std::monostate{}};
                    return true;
                }
                if (term.kind == bytecode::TerminatorKind::ReturnValue) {
                    outReturnValue = podSlotToValue(regs[term.value], fnRef.returnType);
                    return true;
                }
                std::size_t nextBlockIdx = cb.jumpTargetDense;
                const bytecode::SmallRegisterVector* eRegs = &term.arguments;
                if (term.kind == bytecode::TerminatorKind::Branch) {
                    ++ctx.statistics.branchesTaken;
                    const bool cond = regs[term.condition].b;
                    if (!cond) {
                        nextBlockIdx = cb.falseTargetDense;
                        eRegs = &term.falseArguments;
                    }
                }
                const auto& targetParams = plan->blocks[nextBlockIdx].bc->parameters;
                edgeArgs.clear();
                for (std::size_t k = 0; k < std::min(eRegs->size(), targetParams.size()); ++k) {
                    edgeArgs.push_back(regs[(*eRegs)[k]]);
                }
                for (std::size_t k = 0; k < edgeArgs.size(); ++k) {
                    regs[targetParams[k].target] = edgeArgs[k];
                }
                denseBlockIdx = nextBlockIdx;
            }
        };

        if (plan->kernelKind == ParallelKernelKind::Range) {
            params[0].kind = PodValKind::Int32;
            params[0].i64 = static_cast<std::int64_t>(ctx.start);
            params[1].kind = PodValKind::Int32;
            params[1].i64 = static_cast<std::int64_t>(ctx.count);
            Value retVal{};
            if (!executeSingleInvocation(retVal)) return false;
            ctx.reducedValue = std::move(retVal);
            ctx.hasReducedValue = true;
            ctx.statistics.functionCalls += 1u;
            ctx.statistics.maximumCallDepth = 1u;
            return true;
        }

        bool hasAcc = false;
        Value accValue{};
        for (std::size_t off = 0; off < ctx.count; ++off) {
            params[0].kind = PodValKind::Int32;
            params[0].i64 = static_cast<std::int64_t>(ctx.start + off);
            Value elemVal{};
            if (!executeSingleInvocation(elemVal)) return false;
            ctx.statistics.functionCalls += 1u;
            ctx.statistics.maximumCallDepth = 1u;
            if (plan->reduceOp != ParallelReduceOp::None) {
                if (!hasAcc) {
                    accValue = std::move(elemVal);
                    hasAcc = true;
                } else {
                    Value nextAcc{};
                    if (!reduceTwoValues(
                            plan->reduceOp,
                            plan->customReducer,
                            plan->strictDet,
                            accValue,
                            elemVal,
                            nextAcc,
                            ctx.error)) {
                        return false;
                    }
                    accValue = std::move(nextAcc);
                }
            }
        }
        ctx.hasReducedValue = hasAcc;
        ctx.reducedValue = (plan->reduceOp == ParallelReduceOp::None || !hasAcc)
            ? Value{std::monostate{}}
            : std::move(accValue);
        return true;
    };
}

} // namespace

struct ParallelJobScheduler::Impl {
    std::size_t defaultWorkers = 0;

    std::mutex dispatchMutex;
    std::mutex poolMutex;
    std::condition_variable cvStart;
    std::condition_variable cvDone;
    std::vector<std::thread> bgThreads;
    std::function<void(std::size_t)> activeWorkerFn;
    std::size_t activeWorkerCount = 0;
    std::size_t finishedBgWorkers = 0;
    std::uint64_t poolGeneration = 0;
    bool stopping = false;

    // Reusable per-worker Interpreter instances (protected by dispatchMutex)
    std::vector<std::unique_ptr<Interpreter>> workerInterpreters;
    const ProgramImage* boundProgramPtr = nullptr;
    ManagedHeap* boundHeapPtr = nullptr;
    const BindingRegistry* boundBindingsPtr = nullptr;
    std::shared_ptr<const BindingRegistry> augmentedBindings;

    // Verification cache for fast repeated frame dispatches (protected by dispatchMutex)
    struct CachedVerifyKey {
        const ProgramImage* program = nullptr;
        std::uint64_t mirrorInstanceId = 0;
        semantic::SymbolId symbolId = 0;
        ParallelKernelKind kind = ParallelKernelKind::Auto;
        const BindingRegistry* bindings = nullptr;
        std::size_t bindingsSize = 0;
        bool allowRawColumnArrays = false;
        bool allowReadOnlyStaticColumnLoads = false;

        bool operator==(const CachedVerifyKey& other) const noexcept {
            return program == other.program &&
                   mirrorInstanceId == other.mirrorInstanceId &&
                   symbolId == other.symbolId &&
                   kind == other.kind &&
                   bindings == other.bindings &&
                   bindingsSize == other.bindingsSize &&
                   allowRawColumnArrays == other.allowRawColumnArrays &&
                   allowReadOnlyStaticColumnLoads == other.allowReadOnlyStaticColumnLoads;
        }
    };
    struct CachedVerifyKeyHash {
        std::size_t operator()(const CachedVerifyKey& k) const noexcept {
            std::size_t h = std::hash<const void*>{}(k.program);
            h ^= std::hash<std::uint64_t>{}(k.mirrorInstanceId) + 0x9e3779b9u + (h << 6u) + (h >> 2u);
            h ^= std::hash<semantic::SymbolId>{}(k.symbolId) + 0x9e3779b9u + (h << 6u) + (h >> 2u);
            h ^= static_cast<std::size_t>(k.kind) * 0x517cc1b7u;
            h ^= std::hash<const void*>{}(k.bindings) << 1u;
            h ^= k.bindingsSize * 0x85ebca6bu;
            h ^= (k.allowRawColumnArrays ? 0x27d4eb2du : 0u);
            h ^= (k.allowReadOnlyStaticColumnLoads ? 0x165667b1u : 0u);
            return h;
        }
    };
    std::unordered_map<CachedVerifyKey, ParallelJobVerificationReport, CachedVerifyKeyHash> verifyCache;

    explicit Impl(std::size_t workers) noexcept : defaultWorkers(workers) {}

    ~Impl() {
        {
            std::lock_guard<std::mutex> lock(poolMutex);
            stopping = true;
            ++poolGeneration;
        }
        cvStart.notify_all();
        for (auto& t : bgThreads) {
            if (t.joinable()) t.join();
        }
    }

    [[nodiscard]] std::size_t resolveWorkerCount(std::size_t requested) const noexcept {
        if (requested > 0u) return requested;
        if (defaultWorkers > 0u) return defaultWorkers;
        const unsigned hw = std::thread::hardware_concurrency();
        return hw > 0u ? static_cast<std::size_t>(hw) : 4u;
    }

    void ensureBackgroundThreads(std::size_t bgNeeded) {
        while (bgThreads.size() < bgNeeded) {
            const std::size_t workerSlot = bgThreads.size() + 1u;
            const std::uint64_t startGen = poolGeneration;
            bgThreads.emplace_back([this, workerSlot, startGen]() {
                std::uint64_t seenGen = startGen;
                std::unique_lock<std::mutex> lock(poolMutex);
                for (;;) {
                    cvStart.wait(lock, [&]() {
                        return stopping || poolGeneration != seenGen;
                    });
                    if (stopping) return;
                    seenGen = poolGeneration;
                    if (workerSlot < activeWorkerCount && activeWorkerFn) {
                        auto fn = activeWorkerFn;
                        lock.unlock();
                        fn(workerSlot);
                        lock.lock();
                        if (++finishedBgWorkers == activeWorkerCount - 1u) {
                            cvDone.notify_one();
                        }
                    }
                }
            });
        }
    }

    void runOnWorkerPool(std::size_t numWorkers, const std::function<void(std::size_t)>& fn) {
        if (numWorkers <= 1u) {
            fn(0u);
            return;
        }
        {
            std::unique_lock<std::mutex> lock(poolMutex);
            ensureBackgroundThreads(numWorkers - 1u);
            activeWorkerFn = fn;
            activeWorkerCount = numWorkers;
            finishedBgWorkers = 0u;
            ++poolGeneration;
        }
        cvStart.notify_all();
        fn(0u);
        {
            std::unique_lock<std::mutex> lock(poolMutex);
            cvDone.wait(lock, [&]() {
                return finishedBgWorkers == numWorkers - 1u;
            });
            activeWorkerFn = nullptr;
        }
    }

    std::shared_ptr<const BindingRegistry> buildEffectiveBindings(
        const ProgramImage& program,
        const std::shared_ptr<const BindingRegistry>& userBindings,
        bool usesParallelPrng) {
        if (!usesParallelPrng) {
            return userBindings;
        }
        auto merged = std::make_shared<BindingRegistry>();
        for (const auto& mod : program.modules()) {
            for (const auto& ref : mod.functionReferences) {
                const BuiltInPrngOp prngOp = classifyBuiltInPrngName(ref.name);
                if (prngOp != BuiltInPrngOp::None) {
                    ExternalFunction fn = [prngOp](
                        const bytecode::FunctionReference&,
                        ValueSpan args,
                        RuntimeError& err) -> std::optional<Value> {
                        return invokeBuiltInPrngOp(prngOp, args, err);
                    };
                    if (ref.symbolId != 0) {
                        (void)merged->bind(ref.symbolId, fn, BindingDeterminism::Deterministic);
                    }
                    if (!ref.name.empty()) {
                        (void)merged->bind(ref.name, std::move(fn), BindingDeterminism::Deterministic);
                    }
                } else if (userBindings != nullptr) {
                    if (const auto resolved = userBindings->resolve(ref);
                        resolved && resolved->function && *resolved->function) {
                        if (ref.symbolId != 0) {
                            (void)merged->bind(
                                ref.symbolId,
                                *resolved->function,
                                resolved->determinism,
                                resolved->requiredCapability);
                        }
                        if (!ref.name.empty()) {
                            (void)merged->bind(
                                ref.name,
                                *resolved->function,
                                resolved->determinism,
                                resolved->requiredCapability);
                        }
                    }
                }
            }
        }
        return merged;
    }

    std::size_t prepareWorkerInterpreters(
        std::size_t numWorkers,
        const std::shared_ptr<const ProgramImage>& program,
        const std::shared_ptr<ManagedHeap>& heap,
        const std::shared_ptr<const BindingRegistry>& userBindings,
        bool usesParallelPrng) {
        const bool bindingsChanged =
            boundProgramPtr != program.get() ||
            boundHeapPtr != heap.get() ||
            boundBindingsPtr != userBindings.get() ||
            (usesParallelPrng && !augmentedBindings);

        if (bindingsChanged) {
            augmentedBindings = buildEffectiveBindings(*program, userBindings, usesParallelPrng);
            boundProgramPtr = program.get();
            boundHeapPtr = heap.get();
            boundBindingsPtr = userBindings.get();
            workerInterpreters.clear();
        }

        std::size_t constructed = 0u;
        while (workerInterpreters.size() < numWorkers) {
            auto interp = std::make_unique<Interpreter>(program, heap);
            interp->setBindingRegistry(augmentedBindings);
            workerInterpreters.push_back(std::move(interp));
            ++constructed;
        }

        // Pre-warm ExecutionMirror and BindingRegistry symbol cache on main thread
        // before entering multi-core lock-free dispatch.
        (void)detail::executionMirror(*program);
        if (augmentedBindings) {
            for (const auto& mod : program->modules()) {
                for (const auto& ref : mod.functionReferences) {
                    (void)augmentedBindings->resolve(ref);
                }
            }
        }
        return constructed;
    }

    ParallelJobResult executeDispatch(
        ParallelKernelKind kernelKind,
        const std::shared_ptr<const ProgramImage>& program,
        const std::shared_ptr<ManagedHeap>& heap,
        semantic::SymbolId symbolId,
        std::size_t totalCount,
        const std::vector<Value>& userArguments,
        const ParallelJobConfig& config,
        const std::shared_ptr<const BindingRegistry>& bindings) {
        ParallelJobResult result;

        if (!program) {
            result.error.code = ErrorCode::InvalidProgram;
            result.error.message = "ParallelJobScheduler requires a non-null ProgramImage";
            return result;
        }
        if (!heap) {
            result.error.code = ErrorCode::InvalidProgram;
            result.error.message = "ParallelJobScheduler requires a non-null ManagedHeap";
            return result;
        }
        if (config.batchSize == 0u) {
            result.error.code = ErrorCode::InvalidArguments;
            result.error.message = "ParallelJobConfig::batchSize must be >= 1";
            return result;
        }
        if (totalCount > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
            result.error.code = ErrorCode::InvalidArguments;
            result.error.message = "ParallelJob totalCount exceeds 32-bit int index range";
            return result;
        }
        if (config.executionOptions.determinism.mode == DeterminismMode::Record ||
            config.executionOptions.determinism.mode == DeterminismMode::Replay) {
            result.error.code = ErrorCode::DeterminismViolation;
            result.error.message =
                "ParallelJobScheduler supports DeterminismMode::Off and DeterminismMode::Strict";
            return result;
        }

        std::lock_guard<std::mutex> dispatchLock(dispatchMutex);

        // 1. Static parallel-safety bytecode verification
        ParallelJobVerificationReport report;
        const bool canUseVerifyCache =
            config.pureExternalSymbols.empty() && config.pureExternalNames.empty();
        const CachedVerifyKey cacheKey{
            program.get(),
            detail::executionMirror(*program).instanceId,
            symbolId,
            kernelKind,
            bindings.get(),
            bindings ? bindings->size() : 0u,
            config.allowRawColumnArrays,
            config.allowReadOnlyStaticColumnLoads};
        if (canUseVerifyCache) {
            const auto foundIt = verifyCache.find(cacheKey);
            if (foundIt != verifyCache.end()) {
                report = foundIt->second;
            } else {
                ParallelJobVerifyOptions vOpts;
                vOpts.expectedKind = kernelKind;
                vOpts.bindings = bindings.get();
                vOpts.allowRawColumnArrays = config.allowRawColumnArrays;
                vOpts.allowReadOnlyStaticColumnLoads = config.allowReadOnlyStaticColumnLoads;
                report = verifyParallelJobFunction(*program, symbolId, vOpts);
                if (report.valid) {
                    verifyCache.emplace(cacheKey, report);
                }
            }
        } else {
            ParallelJobVerifyOptions vOpts;
            vOpts.expectedKind = kernelKind;
            vOpts.bindings = bindings.get();
            vOpts.pureExternalSymbols = config.pureExternalSymbols;
            vOpts.pureExternalNames = config.pureExternalNames;
            vOpts.allowRawColumnArrays = config.allowRawColumnArrays;
            vOpts.allowReadOnlyStaticColumnLoads = config.allowReadOnlyStaticColumnLoads;
            report = verifyParallelJobFunction(*program, symbolId, vOpts);
        }

        if (!report.valid) {
            result.error.code = (!report.violations.empty() &&
                                 report.violations.front().kind ==
                                     ParallelVerifyViolationKind::FunctionNotFound)
                ? ErrorCode::FunctionNotFound
                : ErrorCode::DeterminismViolation;
            result.error.message = report.summaryMessage();
            return result;
        }

        const std::size_t prefixParamCount =
            (kernelKind == ParallelKernelKind::Range) ? 2u : 1u;
        if (report.parameterTypes.size() != prefixParamCount + userArguments.size()) {
            result.error.code = ErrorCode::InvalidArguments;
            result.error.message = "ParallelJob userArguments count mismatch: expected " +
                std::to_string(report.parameterTypes.size() - prefixParamCount) +
                ", got " + std::to_string(userArguments.size());
            return result;
        }

        if (config.reduceOp != ParallelReduceOp::None &&
            report.returnType == semantic::PrimitiveType::Void) {
            result.error.code = ErrorCode::TypeMismatch;
            result.error.message =
                "ParallelReduceOp requires a non-void scalar return type on kernel '" +
                report.qualifiedName + "'";
            return result;
        }

        // 2. Stabilize any pending background wall-clock GC cycle on main thread
        if (heap->isWallClockCycleActive()) {
            ShadowStack emptyRoots;
            heap->finishWallClockCycleForDeterminism(emptyRoots);
        }

        // Verify referenced static fields are already materialized so workers never lazily write,
        // and pre-bind any read-only static column arrays for zero-copy native execution + alias checks.
        std::unordered_map<semantic::SymbolId, ParallelNativeColumnBinding> staticColumnBindings;
        struct StaticBufferSpan {
            ObjectRef ref{};
            const void* basePtr = nullptr;
            std::size_t byteExtent = 0;
        };
        std::vector<StaticBufferSpan> readOnlyStaticBuffers;
        for (const semantic::SymbolId staticId : report.referencedStaticFieldIds) {
            const auto staticValOpt = heap->getStaticField(staticId);
            if (!staticValOpt) {
                result.error.code = ErrorCode::DeterminismViolation;
                result.error.message =
                    "ParallelJob referenced uninitialized static field (lazy init forbidden in parallel region)";
                return result;
            }
            if (const auto* sRef = std::get_if<ObjectRef>(&*staticValOpt)) {
                if (heap->isAlive(*sRef) && heap->arrayTypeId(*sRef).has_value()) {
                    if (heap->isRawColumnArray(*sRef)) {
                        if (!config.allowRawColumnArrays) {
                            result.error.code = ErrorCode::CapabilityDenied;
                            result.error.message =
                                "ParallelJob static raw column load requires allowRawColumnArrays = true";
                            return result;
                        }
                        const NativeRawColumnView* rawView = heap->rawColumnView(*sRef);
                        if (!rawView || (rawView->count > 0u && rawView->basePtr == nullptr)) {
                            result.error.code = ErrorCode::InvalidArguments;
                            result.error.message =
                                "ParallelJob static raw column has invalid storage";
                            return result;
                        }
                        ParallelNativeColumnBinding col{};
                        col.parameterIndex = std::numeric_limits<std::size_t>::max();
                        col.userArgIndex = std::numeric_limits<std::size_t>::max();
                        col.basePtr = rawView->basePtr;
                        col.count = rawView->count;
                        col.strideBytes = rawView->strideBytes;
                        col.readOnly = true;
                        col.isWritten = false;
                        col.mode = rawView->mode;
                        col.elementType = rawView->elementType;
                        col.elementTypeId = rawView->elementTypeId;
                        col.packedEncoding = rawView->packedEncoding;
                        col.rawColumnView = rawView;
                        staticColumnBindings.emplace(staticId, col);
                        readOnlyStaticBuffers.push_back(StaticBufferSpan{
                            *sRef, rawView->basePtr, rawView->count * rawView->strideBytes});
                    } else {
                        PackedArrayFastView packedView{};
                        if (heap->tryBindPackedArrayFastView(*sRef, packedView)) {
                            ParallelNativeColumnBinding col{};
                            col.parameterIndex = std::numeric_limits<std::size_t>::max();
                            col.userArgIndex = std::numeric_limits<std::size_t>::max();
                            col.basePtr = packedView.rawData;
                            col.count = packedView.length;
                            col.strideBytes = packedView.strideBytes;
                            col.readOnly = true;
                            col.isWritten = false;
                            col.mode = RawColumnMode::ScalarPacked;
                            col.elementType = packedView.elementType;
                            col.elementTypeId = packedView.elementTypeId;
                            col.packedEncoding = packedView.encoding;
                            col.packedView = packedView;
                            staticColumnBindings.emplace(staticId, col);
                            readOnlyStaticBuffers.push_back(StaticBufferSpan{
                                *sRef,
                                packedView.rawData,
                                static_cast<std::size_t>(packedView.length) * packedView.strideBytes});
                        }
                    }
                }
            }
        }

        // 3. Pre-validate all user arguments (PackedArray / RawColumn liveness, encoding & Object liveness)
        //    and check output vs input ObjectRef & raw-buffer non-aliasing on the main thread!
        std::unordered_set<std::size_t> readParamSet(
            report.readArrayParamIndices.begin(),
            report.readArrayParamIndices.end());
        std::unordered_set<std::size_t> writeParamSet(
            report.writtenArrayParamIndices.begin(),
            report.writtenArrayParamIndices.end());

        for (const std::size_t uIdx : config.explicitReadOnlyUserArgIndices) {
            const std::size_t fullIdx = prefixParamCount + uIdx;
            if (writeParamSet.find(fullIdx) != writeParamSet.end()) {
                result.error.code = ErrorCode::CapabilityDenied;
                result.error.message = "ParallelJob user argument " + std::to_string(uIdx) +
                    " is marked read-only but written by StoreElement in kernel";
                return result;
            }
            readParamSet.insert(fullIdx);
        }
        for (const std::size_t uIdx : config.explicitWriteUserArgIndices) {
            writeParamSet.insert(prefixParamCount + uIdx);
        }

        struct ValidatedArrayArg {
            std::size_t fullParamIndex = 0;
            std::size_t userArgIndex = 0;
            ObjectRef ref{};
            const void* basePtr = nullptr;
            std::size_t byteExtent = 0;
            bool isRead = false;
            bool isWrite = false;
        };
        std::vector<ValidatedArrayArg> arrayArgs;
        arrayArgs.reserve(userArguments.size());
        std::vector<ParallelNativeColumnBinding> paramColumns;
        paramColumns.reserve(userArguments.size());
        std::vector<Value> normalizedUserArguments;
        normalizedUserArguments.reserve(userArguments.size());

        for (std::size_t u = 0; u < userArguments.size(); ++u) {
            const std::size_t pIdx = prefixParamCount + u;
            const auto expectedPrim = report.parameterTypes[pIdx];
            const auto expectedTid = pIdx < report.parameterTypeIds.size()
                ? report.parameterTypeIds[pIdx]
                : 0;
            const Value& argVal = userArguments[u];
            Value normalizedArg = argVal;

            if (expectedPrim == semantic::PrimitiveType::Array) {
                if (std::holds_alternative<NullArray>(argVal)) {
                    result.error.code = ErrorCode::NullReference;
                    result.error.message = "ParallelJob array argument " + std::to_string(u) +
                        " cannot be null";
                    return result;
                }
                const auto* ref = std::get_if<ObjectRef>(&argVal);
                if (!ref || !heap->isAlive(*ref)) {
                    result.error.code = ErrorCode::InvalidObjectReference;
                    result.error.message = "ParallelJob array argument " + std::to_string(u) +
                        " is not a live managed array";
                    return result;
                }
                const auto actualArrTid = heap->arrayTypeId(*ref);
                if (!actualArrTid || (expectedTid != 0 && *actualArrTid != expectedTid)) {
                    result.error.code = ErrorCode::TypeMismatch;
                    result.error.message = "ParallelJob array argument " + std::to_string(u) +
                        " runtime array type mismatch";
                    return result;
                }
                const bool isRead = readParamSet.find(pIdx) != readParamSet.end();
                const bool isWrite = writeParamSet.find(pIdx) != writeParamSet.end();
                if (isRead && isWrite && !config.allowInPlaceSingleArrayReadWrite) {
                    result.error.code = ErrorCode::InvalidArguments;
                    result.error.message = "ParallelJob array argument " + std::to_string(u) +
                        " performs both read and write while allowInPlaceSingleArrayReadWrite is false";
                    return result;
                }
                if (heap->isRawColumnArray(*ref)) {
                    if (!config.allowRawColumnArrays) {
                        result.error.code = ErrorCode::CapabilityDenied;
                        result.error.message = "ParallelJob raw column array argument " +
                            std::to_string(u) + " requires allowRawColumnArrays = true";
                        return result;
                    }
                    const NativeRawColumnView* rawView = heap->rawColumnView(*ref);
                    if (!rawView || (rawView->count > 0u && rawView->basePtr == nullptr)) {
                        result.error.code = ErrorCode::InvalidArguments;
                        result.error.message = "ParallelJob raw column array argument " +
                            std::to_string(u) + " has invalid host column view";
                        return result;
                    }
                    if (rawView->readOnly && isWrite) {
                        result.error.code = ErrorCode::CapabilityDenied;
                        result.error.message = "ParallelJob read-only raw column array argument " +
                            std::to_string(u) + " cannot be written";
                        return result;
                    }
                    ParallelNativeColumnBinding col{};
                    col.parameterIndex = pIdx;
                    col.userArgIndex = u;
                    col.basePtr = rawView->basePtr;
                    col.count = rawView->count;
                    col.strideBytes = rawView->strideBytes;
                    col.readOnly = rawView->readOnly;
                    col.isWritten = isWrite;
                    col.mode = rawView->mode;
                    col.elementType = rawView->elementType;
                    col.elementTypeId = rawView->elementTypeId;
                    col.packedEncoding = rawView->packedEncoding;
                    col.rawColumnView = rawView;
                    paramColumns.push_back(col);
                    arrayArgs.push_back(ValidatedArrayArg{
                        pIdx, u, *ref, rawView->basePtr, rawView->count * rawView->strideBytes,
                        isRead, isWrite});
                } else {
                    PackedArrayFastView view{};
                    if (!heap->tryBindPackedArrayFastView(*ref, view)) {
                        result.error.code = ErrorCode::TypeMismatch;
                        result.error.message = "ParallelJob array argument " + std::to_string(u) +
                            " is not backed by P0 PackedArray storage";
                        return result;
                    }
                    ParallelNativeColumnBinding col{};
                    col.parameterIndex = pIdx;
                    col.userArgIndex = u;
                    col.basePtr = view.rawData;
                    col.count = view.length;
                    col.strideBytes = view.strideBytes;
                    col.readOnly = (config.explicitReadOnlyUserArgIndices.find(u) !=
                                    config.explicitReadOnlyUserArgIndices.end());
                    col.isWritten = isWrite;
                    col.mode = RawColumnMode::ScalarPacked;
                    col.elementType = view.elementType;
                    col.elementTypeId = view.elementTypeId;
                    col.packedEncoding = view.encoding;
                    col.packedView = view;
                    paramColumns.push_back(col);
                    arrayArgs.push_back(ValidatedArrayArg{
                        pIdx, u, *ref, view.rawData,
                        static_cast<std::size_t>(view.length) * view.strideBytes,
                        isRead, isWrite});
                }
            } else if (expectedPrim == semantic::PrimitiveType::Object) {
                if (!std::holds_alternative<NullObject>(argVal)) {
                    const auto* ref = std::get_if<ObjectRef>(&argVal);
                    if (!ref || !heap->isAlive(*ref)) {
                        result.error.code = ErrorCode::InvalidObjectReference;
                        result.error.message = "ParallelJob object argument " + std::to_string(u) +
                            " is not a live managed object";
                        return result;
                    }
                    const auto actualObjTid = heap->objectTypeId(*ref);
                    if (!actualObjTid ||
                        !runtimeTypeAssignable(*program, *actualObjTid, expectedTid)) {
                        result.error.code = ErrorCode::TypeMismatch;
                        result.error.message = "ParallelJob object argument " + std::to_string(u) +
                            " runtime object type mismatch";
                        return result;
                    }
                }
            } else if (expectedPrim == semantic::PrimitiveType::Struct) {
                if (valueType(argVal) != semantic::PrimitiveType::Struct) {
                    result.error.code = ErrorCode::TypeMismatch;
                    result.error.message = "ParallelJob struct argument " + std::to_string(u) +
                        " type mismatch";
                    return result;
                }
                const auto actualStructTid = structValueTypeId(argVal);
                if (!actualStructTid || *actualStructTid == 0 ||
                    (expectedTid != 0 && *actualStructTid != expectedTid)) {
                    result.error.code = ErrorCode::TypeMismatch;
                    result.error.message = "ParallelJob struct argument " + std::to_string(u) +
                        " runtime struct type mismatch";
                    return result;
                }
                normalizedArg = detachFrameSliceValue(argVal);
            } else if (expectedPrim == semantic::PrimitiveType::Enum) {
                EnumValue enumVal{};
                if (!argVal.tryGetEnum(enumVal) ||
                    (expectedTid != 0 && enumVal.typeId != expectedTid)) {
                    result.error.code = ErrorCode::TypeMismatch;
                    result.error.message = "ParallelJob enum argument " + std::to_string(u) +
                        " runtime enum type mismatch";
                    return result;
                }
            } else if (valueType(argVal) != expectedPrim) {
                result.error.code = ErrorCode::TypeMismatch;
                result.error.message = "ParallelJob scalar argument " + std::to_string(u) +
                    " type mismatch";
                return result;
            }
            normalizedUserArguments.push_back(std::move(normalizedArg));
        }

        // Cross-parameter ObjectRef & raw-buffer non-aliasing check:
        // Any written output array must NOT share an ObjectRef or overlapping host memory
        // with any other written or read-only array parameter or read-only static column!
        const auto rawRangesOverlap = [](
            const void* ptrA,
            std::size_t bytesA,
            const void* ptrB,
            std::size_t bytesB) noexcept -> bool {
            if (!ptrA || !ptrB || bytesA == 0u || bytesB == 0u) return false;
            const auto a0 = reinterpret_cast<std::uintptr_t>(ptrA);
            const auto b0 = reinterpret_cast<std::uintptr_t>(ptrB);
            if (a0 == b0) return true;
            if (a0 < b0) {
                return (b0 - a0) < bytesA;
            }
            return (a0 - b0) < bytesB;
        };
        for (std::size_t i = 0; i < arrayArgs.size(); ++i) {
            for (std::size_t j = i + 1u; j < arrayArgs.size(); ++j) {
                if ((arrayArgs[i].isWrite || arrayArgs[j].isWrite) &&
                    (arrayArgs[i].ref == arrayArgs[j].ref ||
                     rawRangesOverlap(
                         arrayArgs[i].basePtr, arrayArgs[i].byteExtent,
                         arrayArgs[j].basePtr, arrayArgs[j].byteExtent))) {
                    result.error.code = ErrorCode::InvalidArguments;
                    result.error.message =
                        "ParallelJob aliasing violation: array arguments " +
                        std::to_string(arrayArgs[i].userArgIndex) + " and " +
                        std::to_string(arrayArgs[j].userArgIndex) +
                        " alias or overlap while at least one is written";
                    return result;
                }
            }
            if (arrayArgs[i].isWrite) {
                for (const auto& sc : readOnlyStaticBuffers) {
                    if (arrayArgs[i].ref == sc.ref ||
                        rawRangesOverlap(
                            arrayArgs[i].basePtr, arrayArgs[i].byteExtent,
                            sc.basePtr, sc.byteExtent)) {
                        result.error.code = ErrorCode::InvalidArguments;
                        result.error.message =
                            "ParallelJob aliasing violation: written array argument " +
                            std::to_string(arrayArgs[i].userArgIndex) +
                            " aliases or overlaps a read-only static column";
                        return result;
                    }
                }
            }
        }

        const bool strictDet =
            (config.executionOptions.determinism.mode == DeterminismMode::Strict);

        // Handle empty range deterministically (0 batches)
        if (totalCount == 0u) {
            result.succeeded = true;
            result.value = neutralIdentityValue(
                config.reduceOp, report.returnType, report.returnTypeId);
            result.numBatches = 0u;
            result.effectiveWorkerCount = 0u;
            result.totalItemsProcessed = 0u;
            std::uint64_t digest = kFnvOffset;
            fnvMixScalar(digest, config.executionOptions.determinism.mode);
            fnvMixScalar(digest, symbolId);
            fnvMixScalar(digest, static_cast<std::uint64_t>(totalCount));
            fnvMixScalar(digest, static_cast<std::uint64_t>(config.batchSize));
            fnvMixScalar(digest, static_cast<std::uint64_t>(0u));
            fnvMixScalar(digest, config.baseSeed);
            fnvMixScalar(digest, stableValueHash(result.value));
            result.determinismDigest = digest;
            return result;
        }

        const std::size_t numBatches =
            (totalCount + config.batchSize - 1u) / config.batchSize;
        const std::size_t requestedWorkers = resolveWorkerCount(config.workerCount);
        const std::size_t effectiveWorkers = std::min(requestedWorkers, numBatches);

        ParallelNativeBatchKernel activeNativeKernel;
        std::vector<ParallelNativeColumnBinding> nativeColumns;
        if (config.enableNativeBatchKernel) {
            if (config.nativeBatchKernel) {
                activeNativeKernel = config.nativeBatchKernel;
                nativeColumns = paramColumns;
                for (const auto& [sid, scb] : staticColumnBindings) {
                    (void)sid;
                    nativeColumns.push_back(scb);
                }
            } else {
                const auto* entryLoc = program->resolveFunction(symbolId);
                if (entryLoc != nullptr) {
                    activeNativeKernel = tryBuildAutoNativeBatchKernel(
                        *program,
                        *entryLoc,
                        kernelKind,
                        prefixParamCount,
                        paramColumns,
                        staticColumnBindings,
                        config,
                        nativeColumns);
                }
            }
        }

        std::vector<ParallelBatchOutcome> batchOutcomes(numBatches);
        std::atomic<std::size_t> nextChunkIndex{0u};

        if (activeNativeKernel) {
            result.usedNativeBatchKernel = true;
            result.workerInterpretersConstructed = 0u;

            // Enter zero-mutex lock-free parallel heap epoch without constructing worker Interpreters!
            ManagedHeap::ParallelLockFreeScope lockFreeHeapScope(*heap);

            runOnWorkerPool(effectiveWorkers, [&](std::size_t) {
                for (;;) {
                    const std::size_t chunkIndex =
                        nextChunkIndex.fetch_add(1u, std::memory_order_relaxed);
                    if (chunkIndex >= numBatches) break;

                    const std::size_t batchStart = chunkIndex * config.batchSize;
                    const std::size_t batchCount =
                        std::min(config.batchSize, totalCount - batchStart);

                    ParallelNativeBatchContext ctx{};
                    ctx.batchIndex = chunkIndex;
                    ctx.start = batchStart;
                    ctx.count = batchCount;
                    ctx.batchSeed = config.baseSeed ^ static_cast<std::uint32_t>(chunkIndex);
                    ctx.heap = heap.get();
                    ctx.userArguments = &normalizedUserArguments;
                    ctx.columns = &nativeColumns;

                    const bool ok = activeNativeKernel(ctx);
                    auto& outSlot = batchOutcomes[chunkIndex];
                    outSlot.succeeded = ok;
                    outSlot.hasReducedValue = ok && ctx.hasReducedValue;
                    outSlot.value = ok ? std::move(ctx.reducedValue) : Value{};
                    outSlot.error = std::move(ctx.error);
                    if (!ok && outSlot.error.code == ErrorCode::None) {
                        outSlot.error.code = ErrorCode::ExecutionTerminated;
                        outSlot.error.message = "ParallelNativeBatchKernel failed";
                    }
                    outSlot.statistics = ctx.statistics;

                    std::uint64_t batchDigest = kFnvOffset;
                    fnvMixScalar(batchDigest, static_cast<std::uint64_t>(chunkIndex));
                    fnvMixScalar(batchDigest, static_cast<std::uint64_t>(batchCount));
                    fnvMixScalar(batchDigest, ctx.batchSeed);
                    fnvMixScalar(batchDigest, ok);
                    if (outSlot.hasReducedValue) {
                        fnvMixScalar(batchDigest, stableValueHash(outSlot.value));
                    }
                    outSlot.determinismDigest = batchDigest;
                }
            });
        } else {
            result.usedNativeBatchKernel = false;
            result.workerInterpretersConstructed = prepareWorkerInterpreters(
                effectiveWorkers, program, heap, bindings, report.usesParallelPrng);

            // Base per-invocation ExecutionOptions: force gcWorkBudget = 0 and strip
            // non-thread-safe debugger hooks so worker Interpreters run 100% lock-free.
            ExecutionOptions baseWorkerOpts = config.executionOptions;
            baseWorkerOpts.limits.gcWorkBudget = 0u;
            baseWorkerOpts.debugger.reset();

            {
                // Enter zero-mutex lock-free parallel heap epoch on main thread!
                ManagedHeap::ParallelLockFreeScope lockFreeHeapScope(*heap);

                runOnWorkerPool(effectiveWorkers, [&](std::size_t workerSlot) {
                    // Hold a single ActiveMutatorScope per worker thread across the entire
                    // job so inner Interpreter::invoke() calls hit the 1ns TLS depth fast-path!
                    detail::ActiveMutatorScope outerWorkerMutatorScope;
                Interpreter& workerInterp = *workerInterpreters[workerSlot];

                std::vector<Value> callArgs;
                callArgs.resize(prefixParamCount + normalizedUserArguments.size());
                for (std::size_t u = 0; u < normalizedUserArguments.size(); ++u) {
                    callArgs[prefixParamCount + u] = normalizedUserArguments[u];
                }

                for (;;) {
                    const std::size_t chunkIndex =
                        nextChunkIndex.fetch_add(1u, std::memory_order_relaxed);
                    if (chunkIndex >= numBatches) break;

                    const std::size_t batchStart = chunkIndex * config.batchSize;
                    const std::size_t batchCount =
                        std::min(config.batchSize, totalCount - batchStart);

                    // Deterministically seed shard PRNG via `baseSeed ^ chunkIndex`
                    DeterministicPRNG chunkPrng;
                    chunkPrng.seed(config.baseSeed ^ static_cast<std::uint32_t>(chunkIndex));
                    ScopedChunkPrngBinding prngScope(chunkPrng);

                    auto& outSlot = batchOutcomes[chunkIndex];

                    if (kernelKind == ParallelKernelKind::Range) {
                        callArgs[0] = Value{static_cast<std::int64_t>(batchStart)};
                        callArgs[1] = Value{static_cast<std::int64_t>(batchCount)};

                        ExecutionResult execRes =
                            workerInterp.invoke(symbolId, callArgs, baseWorkerOpts);
                        outSlot.succeeded = execRes.succeeded;
                        outSlot.hasReducedValue = execRes.succeeded;
                        outSlot.value = std::move(execRes.value);
                        outSlot.error = std::move(execRes.error);
                        outSlot.statistics = execRes.statistics;
                        outSlot.determinismDigest = execRes.determinismDigest;
                    } else {
                        std::uint64_t batchDigest = kFnvOffset;
                        fnvMixScalar(batchDigest, static_cast<std::uint64_t>(chunkIndex));
                        fnvMixScalar(batchDigest, static_cast<std::uint64_t>(batchCount));

                        bool batchOk = true;
                        bool hasAcc = false;
                        Value accValue{};
                        RuntimeError batchErr{};
                        RuntimeStatistics batchStats{};

                        for (std::size_t off = 0; off < batchCount; ++off) {
                            callArgs[0] = Value{static_cast<std::int64_t>(batchStart + off)};
                            ExecutionResult elemRes =
                                workerInterp.invoke(symbolId, callArgs, baseWorkerOpts);

                            batchStats.instructionsExecuted += elemRes.statistics.instructionsExecuted;
                            batchStats.functionCalls += elemRes.statistics.functionCalls;
                            batchStats.externalCalls += elemRes.statistics.externalCalls;
                            batchStats.branchesTaken += elemRes.statistics.branchesTaken;
                            batchStats.maximumCallDepth = std::max(
                                batchStats.maximumCallDepth, elemRes.statistics.maximumCallDepth);
                            batchStats.gcWorkPerformed += elemRes.statistics.gcWorkPerformed;

                            fnvMixScalar(batchDigest, elemRes.determinismDigest);

                            if (!elemRes.succeeded) {
                                batchOk = false;
                                batchErr = std::move(elemRes.error);
                                break;
                            }

                            if (config.reduceOp != ParallelReduceOp::None) {
                                if (!hasAcc) {
                                    accValue = std::move(elemRes.value);
                                    hasAcc = true;
                                } else {
                                    Value nextAcc{};
                                    if (!reduceTwoValues(
                                            config.reduceOp,
                                            config.customReducer,
                                            strictDet,
                                            accValue,
                                            elemRes.value,
                                            nextAcc,
                                            batchErr)) {
                                        batchOk = false;
                                        break;
                                    }
                                    accValue = std::move(nextAcc);
                                }
                            }
                        }

                        outSlot.succeeded = batchOk;
                        outSlot.hasReducedValue = hasAcc;
                        outSlot.value = batchOk
                            ? (config.reduceOp == ParallelReduceOp::None
                                ? Value{std::monostate{}}
                                : std::move(accValue))
                            : Value{};
                        outSlot.error = std::move(batchErr);
                        outSlot.statistics = batchStats;
                        outSlot.determinismDigest = batchDigest;
                    }
                }
            });
            } // Leaves ManagedHeap::ParallelLockFreeScope with release/acquire barrier
        } // Fallback workerInterpreter execution

        // 4. Deterministic ordered reduction across `chunkIndex = 0 .. numBatches - 1`
        result.numBatches = numBatches;
        result.effectiveWorkerCount = effectiveWorkers;
        result.batchDigests.resize(numBatches);

        std::uint64_t jobDigest = kFnvOffset;
        fnvMixScalar(jobDigest, config.executionOptions.determinism.mode);
        fnvMixScalar(jobDigest, symbolId);
        fnvMixScalar(jobDigest, static_cast<std::uint64_t>(totalCount));
        fnvMixScalar(jobDigest, static_cast<std::uint64_t>(config.batchSize));
        fnvMixScalar(jobDigest, static_cast<std::uint64_t>(numBatches));
        fnvMixScalar(jobDigest, config.baseSeed);

        bool hasJobAcc = false;
        Value jobAcc{};

        for (std::size_t chunkIndex = 0; chunkIndex < numBatches; ++chunkIndex) {
            auto& b = batchOutcomes[chunkIndex];
            result.batchDigests[chunkIndex] = b.determinismDigest;

            result.statistics.instructionsExecuted += b.statistics.instructionsExecuted;
            result.statistics.functionCalls += b.statistics.functionCalls;
            result.statistics.externalCalls += b.statistics.externalCalls;
            result.statistics.branchesTaken += b.statistics.branchesTaken;
            result.statistics.maximumCallDepth = std::max(
                result.statistics.maximumCallDepth, b.statistics.maximumCallDepth);
            result.statistics.gcWorkPerformed += b.statistics.gcWorkPerformed;

            fnvMixScalar(jobDigest, static_cast<std::uint64_t>(chunkIndex));
            fnvMixScalar(jobDigest, b.determinismDigest);
            fnvMixScalar(jobDigest, b.statistics.instructionsExecuted);

            if (!b.succeeded) {
                result.succeeded = false;
                result.error = std::move(b.error);
                fnvMixScalar(jobDigest, false);
                fnvMixScalar(jobDigest, static_cast<std::uint16_t>(result.error.code));
                result.determinismDigest = jobDigest;
                return result;
            }

            if (config.reduceOp != ParallelReduceOp::None && b.hasReducedValue) {
                if (!hasJobAcc) {
                    if (const auto* d = std::get_if<double>(&b.value)) {
                        jobAcc = Value{canonicalizeDoubleForStrict(*d, strictDet)};
                    } else if (const auto* f = std::get_if<FloatValue>(&b.value)) {
                        jobAcc = Value{FloatValue{canonicalizeFloatForStrict(f->value, strictDet)}};
                    } else {
                        jobAcc = b.value;
                    }
                    hasJobAcc = true;
                } else {
                    Value nextAcc{};
                    RuntimeError redErr{};
                    if (!reduceTwoValues(
                            config.reduceOp,
                            config.customReducer,
                            strictDet,
                            jobAcc,
                            b.value,
                            nextAcc,
                            redErr)) {
                        result.succeeded = false;
                        result.error = std::move(redErr);
                        fnvMixScalar(jobDigest, false);
                        fnvMixScalar(jobDigest, static_cast<std::uint16_t>(result.error.code));
                        result.determinismDigest = jobDigest;
                        return result;
                    }
                    jobAcc = std::move(nextAcc);
                }
            }
        }

        result.succeeded = true;
        result.totalItemsProcessed = totalCount;
        result.value = (config.reduceOp == ParallelReduceOp::None)
            ? (kernelKind == ParallelKernelKind::Range &&
                       report.returnType != semantic::PrimitiveType::Void &&
                       numBatches == 1u
                   ? batchOutcomes[0].value
                   : Value{std::monostate{}})
            : (hasJobAcc
                   ? std::move(jobAcc)
                   : neutralIdentityValue(
                         config.reduceOp, report.returnType, report.returnTypeId));

        fnvMixScalar(jobDigest, true);
        fnvMixScalar(jobDigest, stableValueHash(result.value));
        fnvMixScalar(jobDigest, result.statistics.instructionsExecuted);
        fnvMixScalar(jobDigest, result.statistics.functionCalls);
        fnvMixScalar(jobDigest, result.statistics.externalCalls);
        fnvMixScalar(jobDigest, result.statistics.branchesTaken);
        fnvMixScalar(jobDigest, static_cast<std::uint64_t>(result.statistics.maximumCallDepth));
        fnvMixScalar(jobDigest, result.statistics.gcWorkPerformed);
        result.determinismDigest = jobDigest;
        return result;
    }
};

ParallelJobScheduler::ParallelJobScheduler(std::size_t defaultWorkerCount)
    : impl_(std::make_unique<Impl>(defaultWorkerCount)) {}

ParallelJobScheduler::~ParallelJobScheduler() = default;
ParallelJobScheduler::ParallelJobScheduler(ParallelJobScheduler&&) noexcept = default;
ParallelJobScheduler& ParallelJobScheduler::operator=(ParallelJobScheduler&&) noexcept = default;

void ParallelJobScheduler::setDefaultWorkerCount(std::size_t workerCount) noexcept {
    if (impl_) impl_->defaultWorkers = workerCount;
}

std::size_t ParallelJobScheduler::defaultWorkerCount() const noexcept {
    return impl_ ? impl_->defaultWorkers : 0u;
}

ParallelJobResult ParallelJobScheduler::dispatchFor(
    const std::shared_ptr<const ProgramImage>& program,
    const std::shared_ptr<ManagedHeap>& heap,
    semantic::SymbolId symbolId,
    std::size_t totalCount,
    const std::vector<Value>& userArguments,
    const ParallelJobConfig& config,
    const std::shared_ptr<const BindingRegistry>& bindings) {
    return impl_->executeDispatch(
        ParallelKernelKind::Element,
        program,
        heap,
        symbolId,
        totalCount,
        userArguments,
        config,
        bindings);
}

ParallelJobResult ParallelJobScheduler::dispatchFor(
    const std::shared_ptr<const ProgramImage>& program,
    const std::shared_ptr<ManagedHeap>& heap,
    const std::string& qualifiedName,
    std::size_t totalCount,
    const std::vector<Value>& userArguments,
    const ParallelJobConfig& config,
    const std::shared_ptr<const BindingRegistry>& bindings) {
    if (!program) {
        ParallelJobResult res;
        res.error.code = ErrorCode::InvalidProgram;
        res.error.message = "ParallelJobScheduler requires a non-null ProgramImage";
        return res;
    }
    const auto symId = program->findFunction(qualifiedName);
    if (!symId) {
        ParallelJobResult res;
        res.error.code = ErrorCode::FunctionNotFound;
        res.error.message = "ParallelJob function '" + qualifiedName + "' was not found";
        return res;
    }
    return dispatchFor(program, heap, *symId, totalCount, userArguments, config, bindings);
}

ParallelJobResult ParallelJobScheduler::dispatchRange(
    const std::shared_ptr<const ProgramImage>& program,
    const std::shared_ptr<ManagedHeap>& heap,
    semantic::SymbolId symbolId,
    std::size_t totalCount,
    const std::vector<Value>& userArguments,
    const ParallelJobConfig& config,
    const std::shared_ptr<const BindingRegistry>& bindings) {
    return impl_->executeDispatch(
        ParallelKernelKind::Range,
        program,
        heap,
        symbolId,
        totalCount,
        userArguments,
        config,
        bindings);
}

ParallelJobResult ParallelJobScheduler::dispatchRange(
    const std::shared_ptr<const ProgramImage>& program,
    const std::shared_ptr<ManagedHeap>& heap,
    const std::string& qualifiedName,
    std::size_t totalCount,
    const std::vector<Value>& userArguments,
    const ParallelJobConfig& config,
    const std::shared_ptr<const BindingRegistry>& bindings) {
    if (!program) {
        ParallelJobResult res;
        res.error.code = ErrorCode::InvalidProgram;
        res.error.message = "ParallelJobScheduler requires a non-null ProgramImage";
        return res;
    }
    const auto symId = program->findFunction(qualifiedName);
    if (!symId) {
        ParallelJobResult res;
        res.error.code = ErrorCode::FunctionNotFound;
        res.error.message = "ParallelJob function '" + qualifiedName + "' was not found";
        return res;
    }
    return dispatchRange(program, heap, *symId, totalCount, userArguments, config, bindings);
}

} // namespace realscript::runtime
