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

[[nodiscard]] bool isInlineSafeCustomStruct(
    const ProgramImage& program,
    semantic::SymbolId typeId,
    std::size_t depth = 0) noexcept {
    if (isInlineBuiltInMathStruct(typeId)) return true;
    if (typeId == 0 || depth > 4u) return false;
    const auto* type = program.resolveType(typeId);
    if (!type || type->kind != semantic::TypeKind::Struct) return false;
    std::size_t instanceFieldCount = 0;
    for (const auto& field : type->fields) {
        if (field.staticField) continue;
        ++instanceFieldCount;
        if (instanceFieldCount > StructValue::MaxInlineFields) {
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
        case semantic::PrimitiveType::Enum:
            break;
        case semantic::PrimitiveType::Struct: {
            const semantic::SymbolId nestedTid =
                field.typeName.empty() ? 0 : semantic::stableTypeId(field.typeName);
            if (!isInlineBuiltInMathStruct(nestedTid)) {
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
        return isInlineSafeCustomStruct(program, typeId);
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
            if (!isInlineSafeCustomStruct(program, pTid)) {
                addViolation(
                    *entryLoc, 0, 0, "param",
                    ParallelVerifyViolationKind::UnsupportedParameterType,
                    "Struct parameter " + std::to_string(p) +
                        " exceeds inline zero-heap layout limits");
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
            if (pTid == 0 || allowedPackedArrayIds.find(pTid) == allowedPackedArrayIds.end()) {
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
                case bytecode::Opcode::NewStruct:
                case bytecode::Opcode::StoreStructField: {
                    const auto* structType =
                        inst.typeIndex < mod.types.size() ? &mod.types[inst.typeIndex] : nullptr;
                    if (!structType || !isInlineSafeCustomStruct(program, structType->id)) {
                        addViolation(
                            *loc, block.id, uInstIdx, opName,
                            ParallelVerifyViolationKind::NonInlineStructMutation,
                            std::string(opName) + " on non-inline struct allocates heap storage");
                    }
                    break;
                }
                case bytecode::Opcode::LoadElement:
                case bytecode::Opcode::StoreElement:
                    if (!isPackedParallelElementType(inst.elementType)) {
                        addViolation(
                            *loc, block.id, uInstIdx, opName,
                            ParallelVerifyViolationKind::NonPackedArrayElementAccess,
                            std::string(opName) + " on non-packed element type '" +
                                std::string(semantic::primitiveTypeName(inst.elementType)) +
                                "' is forbidden in ParallelJob");
                    }
                    break;
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
                            if (subOp.rfind("ecs_col::", 0) == 0 ||
                                subOp.rfind("ecs_ent::", 0) == 0 ||
                                !tryParseNumericStaticFieldId(subOp, staticId)) {
                                addViolation(
                                    *loc, block.id, uInstIdx, opName,
                                    ParallelVerifyViolationKind::ImpureRuntimeIntrinsic,
                                    "ECS/dynamic intrinsic '" + ref.name +
                                        "' may allocate or mutate ManagedHeap");
                            } else {
                                staticFieldIdSet.insert(staticId);
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

        bool operator==(const CachedVerifyKey& other) const noexcept {
            return program == other.program &&
                   mirrorInstanceId == other.mirrorInstanceId &&
                   symbolId == other.symbolId &&
                   kind == other.kind &&
                   bindings == other.bindings &&
                   bindingsSize == other.bindingsSize;
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

    void prepareWorkerInterpreters(
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

        while (workerInterpreters.size() < numWorkers) {
            auto interp = std::make_unique<Interpreter>(program, heap);
            interp->setBindingRegistry(augmentedBindings);
            workerInterpreters.push_back(std::move(interp));
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
            bindings ? bindings->size() : 0u};
        if (canUseVerifyCache) {
            const auto foundIt = verifyCache.find(cacheKey);
            if (foundIt != verifyCache.end()) {
                report = foundIt->second;
            } else {
                ParallelJobVerifyOptions vOpts;
                vOpts.expectedKind = kernelKind;
                vOpts.bindings = bindings.get();
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

        // Verify referenced static fields are already materialized so workers never lazily write
        for (const semantic::SymbolId staticId : report.referencedStaticFieldIds) {
            if (!heap->hasStaticField(staticId)) {
                result.error.code = ErrorCode::DeterminismViolation;
                result.error.message =
                    "ParallelJob referenced uninitialized static field (lazy init forbidden in parallel region)";
                return result;
            }
        }

        // 3. Pre-validate all user arguments (PackedArray liveness, encoding & Object liveness)
        //    and check output vs input ObjectRef non-aliasing on the main thread!
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
            PackedArrayFastView view{};
            bool isRead = false;
            bool isWrite = false;
        };
        std::vector<ValidatedArrayArg> arrayArgs;
        arrayArgs.reserve(userArguments.size());

        for (std::size_t u = 0; u < userArguments.size(); ++u) {
            const std::size_t pIdx = prefixParamCount + u;
            const auto expectedPrim = report.parameterTypes[pIdx];
            const auto expectedTid = pIdx < report.parameterTypeIds.size()
                ? report.parameterTypeIds[pIdx]
                : 0;
            const Value& argVal = userArguments[u];

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
                PackedArrayFastView view{};
                if (!heap->tryBindPackedArrayFastView(*ref, view)) {
                    result.error.code = ErrorCode::TypeMismatch;
                    result.error.message = "ParallelJob array argument " + std::to_string(u) +
                        " is not backed by P0 PackedArray storage";
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
                arrayArgs.push_back(ValidatedArrayArg{pIdx, u, *ref, view, isRead, isWrite});
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
            } else if (valueType(argVal) != expectedPrim) {
                result.error.code = ErrorCode::TypeMismatch;
                result.error.message = "ParallelJob scalar argument " + std::to_string(u) +
                    " type mismatch";
                return result;
            }
        }

        // Cross-parameter ObjectRef non-aliasing check:
        // Any written output array must NOT share an ObjectRef with any other written
        // or read-only array parameter!
        for (std::size_t i = 0; i < arrayArgs.size(); ++i) {
            for (std::size_t j = i + 1u; j < arrayArgs.size(); ++j) {
                if (arrayArgs[i].ref == arrayArgs[j].ref &&
                    (arrayArgs[i].isWrite || arrayArgs[j].isWrite)) {
                    result.error.code = ErrorCode::InvalidArguments;
                    result.error.message =
                        "ParallelJob aliasing violation: array arguments " +
                        std::to_string(arrayArgs[i].userArgIndex) + " and " +
                        std::to_string(arrayArgs[j].userArgIndex) +
                        " reference the same ObjectRef while at least one is written";
                    return result;
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

        prepareWorkerInterpreters(
            effectiveWorkers, program, heap, bindings, report.usesParallelPrng);

        std::vector<ParallelBatchOutcome> batchOutcomes(numBatches);
        std::atomic<std::size_t> nextChunkIndex{0u};

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
                callArgs.resize(prefixParamCount + userArguments.size());
                for (std::size_t u = 0; u < userArguments.size(); ++u) {
                    callArgs[prefixParamCount + u] = userArguments[u];
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
