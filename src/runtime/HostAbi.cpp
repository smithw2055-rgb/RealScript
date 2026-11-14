#include "realscript/HostAbi.h"

#include "realscript/bytecode/Bytecode.h"
#include "realscript/runtime/AsyncRuntime.h"
#include "realscript/runtime/Runtime.h"
#include "realscript/semantic/Semantic.h"

#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

struct RsDomainOpaque {
    std::mutex mutex;
    std::shared_ptr<realscript::runtime::ValuePoolDomain> valuePoolDomain;
    std::vector<std::weak_ptr<realscript::runtime::ManagedHeap>> heaps;
};

struct RsHeapOpaque {
    struct CachedBlittableSchema {
        std::uint32_t strideBytes = 0;
        std::uint32_t blittableSchemaId = 0;
    };

    explicit RsHeapOpaque(std::shared_ptr<realscript::runtime::ManagedHeap> h)
        : heap(std::move(h)) {}

    std::shared_ptr<realscript::runtime::ManagedHeap> heap;
    std::mutex v2Mutex;
    std::unordered_map<realscript::semantic::SymbolId, CachedBlittableSchema> blittableCache;
};

struct RsProgramOpaque {
    struct CachedFieldMeta {
        std::string name;
        std::string formerNamesCsv;
        std::vector<std::string> formerNames;
        std::uint64_t typeId = 0;
        RsPrimitiveKindV2 primitiveKind = RS_PRIMITIVE_V2_UNKNOWN;
        realscript::semantic::PrimitiveType semanticPrimitive =
            realscript::semantic::PrimitiveType::Error;
        bool hasFieldTag = false;
        std::int32_t fieldTag = 0;
        bool isSynthetic = false;
        std::uint32_t byteOffset = 0;
        std::uint32_t fieldIndex = 0;
    };
    struct CachedTypeMeta {
        std::uint64_t typeId = 0;
        std::uint64_t baseTypeId = 0;
        std::string name;
        std::string canonicalName;
        std::string moduleName;
        RsTypeKindV2 kind = RS_TYPE_KIND_V2_CLASS;
        bool isBlittable = false;
        std::uint32_t nativeByteSize = 0;
        std::vector<CachedFieldMeta> fields;
    };

    explicit RsProgramOpaque(std::shared_ptr<const realscript::runtime::ProgramImage> p)
        : program(std::move(p)) {}

    std::shared_ptr<const realscript::runtime::ProgramImage> program;
    std::once_flag introspectOnce;
    std::vector<CachedTypeMeta> cachedTypes;
    std::unordered_map<std::uint64_t, std::size_t> typeIdToIndex;
    std::unordered_map<std::string, std::uint64_t> typeNameToId;
    std::once_flag jsonOnce;
    std::string cachedAbiJson;
};

namespace {
void revokeAllPendingCallbacksForVm(std::uint64_t vmInstanceId) noexcept;
}

struct RsVmOpaque {
    struct CachedErrorFrame {
        std::string function;
        std::string moduleName;
        std::string sourceFile;
        std::uint32_t line = 0;
        std::uint32_t column = 0;
    };

    RsVmOpaque(
        std::shared_ptr<const realscript::runtime::ProgramImage> p,
        std::shared_ptr<realscript::runtime::ManagedHeap> h,
        std::shared_ptr<realscript::runtime::BindingRegistry> b,
        std::unique_ptr<realscript::runtime::Interpreter> i)
        : program(std::move(p)),
          heap(std::move(h)),
          bindings(std::move(b)),
          interpreter(std::move(i)) {
        static std::atomic<std::uint64_t> nextVmId{1};
        instanceId = nextVmId.fetch_add(1, std::memory_order_relaxed);
    }

    ~RsVmOpaque() {
        revokeAllPendingCallbacksForVm(instanceId);
        options.awaitScheduler = nullptr;
        awaitSchedulerAdapter.reset();
    }

    std::uint64_t instanceId = 0;
    std::shared_ptr<const realscript::runtime::ProgramImage> program;
    std::shared_ptr<realscript::runtime::ManagedHeap> heap;
    std::shared_ptr<realscript::runtime::BindingRegistry> bindings;
    std::unique_ptr<realscript::runtime::Interpreter> interpreter;
    realscript::runtime::ExecutionOptions options{};
    std::unique_ptr<realscript::runtime::IAwaitScheduler> awaitSchedulerAdapter;
    std::uint64_t lastExecutedInstructions = 0;
    RsStatusV1 lastStatus = RS_STATUS_V1_OK;
    RsErrorCodeV2 lastErrorCode = RS_ERROR_CODE_V2_NONE;
    std::string lastErrorMessage;
    std::string lastExceptionType;
    std::vector<CachedErrorFrame> lastFrames;
};

namespace {

using namespace realscript;
using namespace realscript::runtime;

RsObjectRefV1 toAbiObjectRef(ObjectRef ref) noexcept {
    RsObjectRefV1 out{};
    out.slot = ref.slot;
    out.generation = ref.generation;
    out.heap_id = ref.heapId;
    out.object_kind = static_cast<uint8_t>(ref.kind);
    return out;
}

ObjectRef fromAbiObjectRef(RsObjectRefV1 ref) noexcept {
    ObjectRef out{};
    out.kind = static_cast<ObjectKind>(ref.object_kind);
    out.slot = ref.slot;
    out.generation = ref.generation;
    out.heapId = ref.heap_id;
    return out;
}

RsNativeHandleV1 toAbiNativeHandle(NativeHandle h) noexcept {
    RsNativeHandleV1 out{};
    out.slot = h.slot;
    out.generation = h.generation;
    out.type_id = h.typeId;
    out.registry_id = h.registryId;
    return out;
}

NativeHandle fromAbiNativeHandle(RsNativeHandleV1 h) noexcept {
    return NativeHandle{h.slot, h.generation, h.type_id, h.registry_id};
}

std::optional<Value> readStructFieldForAbi(
    const Value& v,
    std::size_t fieldIndex) noexcept {
    if (const auto* sv = std::get_if<StructValue>(&v)) {
        return structValueFieldAt(*sv, fieldIndex);
    }
    return loadInlineStructField(v, fieldIndex);
}

RsStatusV1 marshalValueToAbi(
    ManagedHeap* targetHeapForTempStrings,
    const Value& v,
    RsValueV1* out) noexcept {
    if (!out) return RS_STATUS_V1_INVALID_ARGUMENT;
    std::memset(out, 0, sizeof(*out));
    ScopedValuePoolDomain domainScope(
        targetHeapForTempStrings ? targetHeapForTempStrings->valuePoolDomain() : nullptr);
    if (std::holds_alternative<std::monostate>(v)) {
        out->kind = RS_VALUE_KIND_V1_VOID;
        return RS_STATUS_V1_OK;
    }
    if (const auto* b = std::get_if<bool>(&v)) {
        out->kind = RS_VALUE_KIND_V1_BOOL;
        out->payload.bool_val = *b ? 1u : 0u;
        return RS_STATUS_V1_OK;
    }
    if (const auto* bv = std::get_if<ByteValue>(&v)) {
        out->kind = RS_VALUE_KIND_V1_INT64;
        out->payload.i64_val = static_cast<int64_t>(bv->value);
        return RS_STATUS_V1_OK;
    }
    if (const auto* sbv = std::get_if<SByteValue>(&v)) {
        out->kind = RS_VALUE_KIND_V1_INT64;
        out->payload.i64_val = static_cast<int64_t>(sbv->value);
        return RS_STATUS_V1_OK;
    }
    if (const auto* sv = std::get_if<ShortValue>(&v)) {
        out->kind = RS_VALUE_KIND_V1_INT64;
        out->payload.i64_val = static_cast<int64_t>(sv->value);
        return RS_STATUS_V1_OK;
    }
    if (const auto* usv = std::get_if<UShortValue>(&v)) {
        out->kind = RS_VALUE_KIND_V1_INT64;
        out->payload.i64_val = static_cast<int64_t>(usv->value);
        return RS_STATUS_V1_OK;
    }
    if (const auto* i = std::get_if<std::int64_t>(&v)) {
        out->kind = RS_VALUE_KIND_V1_INT64;
        out->payload.i64_val = *i;
        return RS_STATUS_V1_OK;
    }
    if (const auto* lv = std::get_if<LongValue>(&v)) {
        out->kind = RS_VALUE_KIND_V1_INT64;
        out->payload.i64_val = lv->value;
        return RS_STATUS_V1_OK;
    }
    if (const auto* u32 = std::get_if<UIntValue>(&v)) {
        out->kind = RS_VALUE_KIND_V1_UINT64;
        out->payload.u64_val = static_cast<uint64_t>(u32->value);
        return RS_STATUS_V1_OK;
    }
    if (const auto* u = std::get_if<ULongValue>(&v)) {
        out->kind = RS_VALUE_KIND_V1_UINT64;
        out->payload.u64_val = u->value;
        return RS_STATUS_V1_OK;
    }
    if (const auto* fv = std::get_if<FloatValue>(&v)) {
        out->kind = RS_VALUE_KIND_V1_DOUBLE;
        out->payload.f64_val = static_cast<double>(fv->value);
        return RS_STATUS_V1_OK;
    }
    if (const auto* d = std::get_if<double>(&v)) {
        out->kind = RS_VALUE_KIND_V1_DOUBLE;
        out->payload.f64_val = *d;
        return RS_STATUS_V1_OK;
    }
    if (const auto* cv = std::get_if<CharValue>(&v)) {
        out->kind = RS_VALUE_KIND_V2_CHAR;
        out->payload.u64_val = static_cast<uint64_t>(cv->value);
        return RS_STATUS_V1_OK;
    }
    if (const auto* ev = std::get_if<EnumValue>(&v)) {
        out->kind = RS_VALUE_KIND_V1_INT64;
        out->payload.i64_val = ev->value;
        return RS_STATUS_V1_OK;
    }
    if (const auto* f16 = std::get_if<Fixed16Value>(&v)) {
        out->kind = RS_VALUE_KIND_V2_FIXED16;
        out->payload.i64_val = f16->raw;
        return RS_STATUS_V1_OK;
    }
    if (const auto* fx = std::get_if<FixedValue>(&v)) {
        out->kind = RS_VALUE_KIND_V1_INT64;
        out->payload.i64_val = fx->raw;
        return RS_STATUS_V1_OK;
    }
    if (const auto* fx64 = std::get_if<Fixed64Value>(&v)) {
        out->kind = RS_VALUE_KIND_V1_INT64;
        out->payload.i64_val = fx64->raw;
        return RS_STATUS_V1_OK;
    }
    if (const auto* r = std::get_if<ObjectRef>(&v)) {
        out->kind = RS_VALUE_KIND_V1_OBJECT_REF;
        out->payload.obj_ref = toAbiObjectRef(*r);
        return RS_STATUS_V1_OK;
    }
    if (v.rawTag() == ValueTag::String) {
        if (!targetHeapForTempStrings) {
            return RS_STATUS_V1_INVALID_ARGUMENT;
        }
        RuntimeError err;
        const auto allocated = targetHeapForTempStrings->allocateString(
            v.asStringUnchecked().str(), &err);
        if (!allocated) return RS_STATUS_V1_OUT_OF_MEMORY;
        out->kind = RS_VALUE_KIND_V1_OBJECT_REF;
        out->payload.obj_ref = toAbiObjectRef(*allocated);
        return RS_STATUS_V1_OK;
    }
    if (const auto* nh = std::get_if<NativeHandle>(&v)) {
        out->kind = RS_VALUE_KIND_V1_NATIVE_HANDLE;
        out->payload.native_handle = toAbiNativeHandle(*nh);
        return RS_STATUS_V1_OK;
    }
    if (std::holds_alternative<NullObject>(v)) {
        out->kind = RS_VALUE_KIND_V1_NULL_OBJECT;
        return RS_STATUS_V1_OK;
    }
    if (std::holds_alternative<NullString>(v)) {
        out->kind = RS_VALUE_KIND_V1_NULL_STRING;
        return RS_STATUS_V1_OK;
    }
    if (v.rawTag() == ValueTag::NullArray) {
        out->kind = RS_VALUE_KIND_V2_NULL_ARRAY;
        return RS_STATUS_V1_OK;
    }
    if (v.rawTag() == ValueTag::Struct) {
        const auto f0 = readStructFieldForAbi(v, 0);
        const auto f1 = readStructFieldForAbi(v, 1);
        const auto f2 = readStructFieldForAbi(v, 2);
        const auto f3 = readStructFieldForAbi(v, 3);
        if (f0 && f1 && f2 && !f3 &&
            (f0->rawTag() == ValueTag::ObjectRef || f0->rawTag() == ValueTag::NullArray) &&
            std::get_if<std::int64_t>(&*f1) &&
            std::get_if<std::int64_t>(&*f2)) {
            out->kind = RS_VALUE_KIND_V2_SPAN;
            if (const auto* r = std::get_if<ObjectRef>(&*f0)) {
                out->payload.obj_ref = toAbiObjectRef(*r);
            } else {
                out->payload.obj_ref = RsObjectRefV1{};
            }
            return RS_STATUS_V1_OK;
        }
    }
    return RS_STATUS_V1_RUNTIME_ERROR;
}

bool marshalValueFromAbi(const RsValueV1& in, Value& out) noexcept {
    switch (in.kind) {
    case RS_VALUE_KIND_V1_VOID:
        out = Value{};
        return true;
    case RS_VALUE_KIND_V1_BOOL:
        out = Value{in.payload.bool_val != 0};
        return true;
    case RS_VALUE_KIND_V1_INT64:
        out = Value{in.payload.i64_val};
        return true;
    case RS_VALUE_KIND_V1_UINT64:
        out = Value{ULongValue{in.payload.u64_val}};
        return true;
    case RS_VALUE_KIND_V1_DOUBLE:
        out = Value{in.payload.f64_val};
        return true;
    case RS_VALUE_KIND_V1_OBJECT_REF:
        out = Value{fromAbiObjectRef(in.payload.obj_ref)};
        return true;
    case RS_VALUE_KIND_V1_NATIVE_HANDLE:
        out = Value{fromAbiNativeHandle(in.payload.native_handle)};
        return true;
    case RS_VALUE_KIND_V1_NULL_OBJECT:
        out = Value{NullObject{}};
        return true;
    case RS_VALUE_KIND_V1_NULL_STRING:
        out = Value{NullString{}};
        return true;
    case RS_VALUE_KIND_V2_NULL_ARRAY:
        out = Value{NullArray{}};
        return true;
    case RS_VALUE_KIND_V2_FIXED16:
        out = Value{Fixed16Value{in.payload.i64_val}};
        return true;
    case RS_VALUE_KIND_V2_CHAR:
        out = Value{CharValue{static_cast<char16_t>(in.payload.u64_val)}};
        return true;
    case RS_VALUE_KIND_V2_SPAN:
        out = Value{fromAbiObjectRef(in.payload.obj_ref)};
        return true;
    default:
        return false;
    }
}

bool coerceAbiValueForType(
    const RsValueV1& in,
    semantic::PrimitiveType expectedType,
    semantic::SymbolId expectedTypeId,
    ManagedHeap* heap,
    Value& out) noexcept {
    Value raw{};
    if (!marshalValueFromAbi(in, raw)) return false;

    const auto tryExtractSigned = [&](int64_t& outSigned) -> bool {
        if (in.kind == RS_VALUE_KIND_V1_INT64) {
            outSigned = in.payload.i64_val;
            return true;
        }
        if (in.kind == RS_VALUE_KIND_V1_UINT64 &&
            in.payload.u64_val <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            outSigned = static_cast<int64_t>(in.payload.u64_val);
            return true;
        }
        return false;
    };
    const auto tryExtractUnsigned = [&](uint64_t& outUnsigned) -> bool {
        if (in.kind == RS_VALUE_KIND_V1_UINT64) {
            outUnsigned = in.payload.u64_val;
            return true;
        }
        if (in.kind == RS_VALUE_KIND_V1_INT64 && in.payload.i64_val >= 0) {
            outUnsigned = static_cast<uint64_t>(in.payload.i64_val);
            return true;
        }
        return false;
    };

    switch (expectedType) {
    case semantic::PrimitiveType::Bool:
        if (in.kind == RS_VALUE_KIND_V1_BOOL) {
            out = Value{in.payload.bool_val != 0};
            return true;
        }
        return false;
    case semantic::PrimitiveType::Byte: {
        uint64_t uv = 0;
        if (tryExtractUnsigned(uv) && uv <= 0xFFu) {
            out = Value{ByteValue{static_cast<uint8_t>(uv)}};
            return true;
        }
        return false;
    }
    case semantic::PrimitiveType::SByte: {
        int64_t sv = 0;
        if (tryExtractSigned(sv) && sv >= -128 && sv <= 127) {
            out = Value{SByteValue{static_cast<int8_t>(sv)}};
            return true;
        }
        return false;
    }
    case semantic::PrimitiveType::Short: {
        int64_t sv = 0;
        if (tryExtractSigned(sv) && sv >= -32768 && sv <= 32767) {
            out = Value{ShortValue{static_cast<int16_t>(sv)}};
            return true;
        }
        return false;
    }
    case semantic::PrimitiveType::UShort: {
        uint64_t uv = 0;
        if (tryExtractUnsigned(uv) && uv <= 0xFFFFu) {
            out = Value{UShortValue{static_cast<uint16_t>(uv)}};
            return true;
        }
        return false;
    }
    case semantic::PrimitiveType::Int: {
        int64_t sv = 0;
        if (tryExtractSigned(sv)) {
            out = Value{sv};
            return true;
        }
        return false;
    }
    case semantic::PrimitiveType::UInt: {
        uint64_t uv = 0;
        if (tryExtractUnsigned(uv) && uv <= 0xFFFFFFFFu) {
            out = Value{UIntValue{static_cast<uint32_t>(uv)}};
            return true;
        }
        return false;
    }
    case semantic::PrimitiveType::Long: {
        int64_t sv = 0;
        if (tryExtractSigned(sv)) {
            out = Value{LongValue{sv}};
            return true;
        }
        return false;
    }
    case semantic::PrimitiveType::ULong: {
        uint64_t uv = 0;
        if (tryExtractUnsigned(uv)) {
            out = Value{ULongValue{uv}};
            return true;
        }
        return false;
    }
    case semantic::PrimitiveType::Float:
        if (in.kind == RS_VALUE_KIND_V1_DOUBLE) {
            out = Value{FloatValue{static_cast<float>(in.payload.f64_val)}};
            return true;
        }
        if (in.kind == RS_VALUE_KIND_V1_INT64) {
            out = Value{FloatValue{static_cast<float>(in.payload.i64_val)}};
            return true;
        }
        return false;
    case semantic::PrimitiveType::Double:
        if (in.kind == RS_VALUE_KIND_V1_DOUBLE) {
            out = Value{in.payload.f64_val};
            return true;
        }
        if (in.kind == RS_VALUE_KIND_V1_INT64) {
            out = Value{static_cast<double>(in.payload.i64_val)};
            return true;
        }
        return false;
    case semantic::PrimitiveType::Char:
        if (in.kind == RS_VALUE_KIND_V2_CHAR) {
            out = Value{CharValue{static_cast<char16_t>(in.payload.u64_val)}};
            return true;
        }
        if (in.kind == RS_VALUE_KIND_V1_INT64 && in.payload.i64_val >= 0 &&
            in.payload.i64_val <= 0xFFFF) {
            out = Value{CharValue{static_cast<char16_t>(in.payload.i64_val)}};
            return true;
        }
        return false;
    case semantic::PrimitiveType::Enum: {
        int64_t sv = 0;
        if (expectedTypeId != 0 && tryExtractSigned(sv)) {
            out = Value{EnumValue{expectedTypeId, sv}};
            return true;
        }
        return false;
    }
    case semantic::PrimitiveType::Array:
        if (in.kind == RS_VALUE_KIND_V1_NULL_OBJECT || in.kind == RS_VALUE_KIND_V2_NULL_ARRAY) {
            out = Value{NullArray{}};
            return true;
        }
        out = std::move(raw);
        return true;
    case semantic::PrimitiveType::String:
        if (in.kind == RS_VALUE_KIND_V1_NULL_OBJECT || in.kind == RS_VALUE_KIND_V1_NULL_STRING) {
            out = Value{NullString{}};
            return true;
        }
        out = std::move(raw);
        return true;
    case semantic::PrimitiveType::Struct:
        if (in.kind == RS_VALUE_KIND_V1_INT64 && expectedTypeId == fixedTypeId()) {
            out = Value{FixedValue{static_cast<int32_t>(in.payload.i64_val)}};
            return true;
        }
        if (in.kind == RS_VALUE_KIND_V2_FIXED16 && expectedTypeId == fixed16TypeId()) {
            out = Value{Fixed16Value{in.payload.i64_val}};
            return true;
        }
        if (in.kind == RS_VALUE_KIND_V1_INT64 && expectedTypeId == fixed64TypeId()) {
            out = Value{Fixed64Value{in.payload.i64_val}};
            return true;
        }
        if (in.kind == RS_VALUE_KIND_V2_SPAN) {
            if (!heap || expectedTypeId == 0) return false;
            const ObjectRef bufRef = fromAbiObjectRef(in.payload.obj_ref);
            if (!bufRef.valid()) {
                out = heap->makeNativeSpanValue(expectedTypeId, ObjectRef{}, 0, 0);
                return true;
            }
            const auto lenOpt = heap->arrayLength(bufRef);
            if (!lenOpt ||
                *lenOpt > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
                return false;
            }
            out = heap->makeNativeSpanValue(
                expectedTypeId, bufRef, 0, static_cast<std::int32_t>(*lenOpt));
            return true;
        }
        out = std::move(raw);
        return true;
    default:
        out = std::move(raw);
        return true;
    }
}

semantic::SymbolId exactFieldTypeId(
    semantic::PrimitiveType type,
    const std::string& typeName) noexcept {
    return (semantic::isExactType(type) ||
            type == semantic::PrimitiveType::Handle) &&
            !typeName.empty()
        ? semantic::stableTypeId(typeName)
        : 0;
}

Value makeDefaultFieldValue(
    semantic::PrimitiveType ptype,
    semantic::SymbolId typeId,
    bool* outIsRef = nullptr) noexcept {
    if (outIsRef) *outIsRef = false;
    switch (ptype) {
    case semantic::PrimitiveType::Bool: return Value{false};
    case semantic::PrimitiveType::Byte: return Value{ByteValue{0}};
    case semantic::PrimitiveType::SByte: return Value{SByteValue{0}};
    case semantic::PrimitiveType::Short: return Value{ShortValue{0}};
    case semantic::PrimitiveType::UShort: return Value{UShortValue{0}};
    case semantic::PrimitiveType::Int: return Value{int64_t{0}};
    case semantic::PrimitiveType::UInt: return Value{UIntValue{0}};
    case semantic::PrimitiveType::Long: return Value{LongValue{0}};
    case semantic::PrimitiveType::ULong: return Value{ULongValue{0}};
    case semantic::PrimitiveType::Float: return Value{FloatValue{0.0f}};
    case semantic::PrimitiveType::Double: return Value{0.0};
    case semantic::PrimitiveType::Char: return Value{CharValue{0}};
    case semantic::PrimitiveType::String:
        if (outIsRef) *outIsRef = true;
        return Value{NullString{}};
    case semantic::PrimitiveType::Object:
        if (outIsRef) *outIsRef = true;
        return Value{NullObject{}};
    case semantic::PrimitiveType::Array:
        if (outIsRef) *outIsRef = true;
        return Value{NullArray{}};
    case semantic::PrimitiveType::Enum:
        return Value{EnumValue{typeId, 0}};
    case semantic::PrimitiveType::Struct:
        if (auto defStruct = makeDefaultBlittableStructValue(typeId)) {
            return *defStruct;
        }
        if (auto defInline = makeDefaultInlineStructValue(typeId)) {
            return *defInline;
        }
        return Value{};
    default:
        return Value{};
    }
}

RsStatusV1 mapRuntimeError(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::None:
        return RS_STATUS_V1_OK;
    case ErrorCode::OutOfMemory:
        return RS_STATUS_V1_OUT_OF_MEMORY;
    case ErrorCode::CrossHeapAccessViolation:
        return RS_STATUS_V1_CROSS_HEAP_VIOLATION;
    case ErrorCode::InvalidObjectReference:
    case ErrorCode::InvalidNativeHandle:
        return RS_STATUS_V1_STALE_REFERENCE;
    case ErrorCode::InvalidArguments:
        return RS_STATUS_V1_INVALID_ARGUMENT;
    default:
        return RS_STATUS_V1_RUNTIME_ERROR;
    }
}

RsErrorCodeV2 mapErrorCodeToV2(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::None: return RS_ERROR_CODE_V2_NONE;
    case ErrorCode::FunctionNotFound: return RS_ERROR_CODE_V2_FUNCTION_NOT_FOUND;
    case ErrorCode::InvalidArguments: return RS_ERROR_CODE_V2_INVALID_ARGUMENTS;
    case ErrorCode::TypeMismatch: return RS_ERROR_CODE_V2_TYPE_MISMATCH;
    case ErrorCode::DivisionByZero: return RS_ERROR_CODE_V2_DIVISION_BY_ZERO;
    case ErrorCode::IntegerOverflow: return RS_ERROR_CODE_V2_INTEGER_OVERFLOW;
    case ErrorCode::InstructionBudgetExceeded: return RS_ERROR_CODE_V2_INSTRUCTION_BUDGET_EXCEEDED;
    case ErrorCode::RecursionLimitExceeded: return RS_ERROR_CODE_V2_RECURSION_LIMIT_EXCEEDED;
    case ErrorCode::ExternalFunctionUnresolved: return RS_ERROR_CODE_V2_EXTERNAL_FUNCTION_UNRESOLVED;
    case ErrorCode::DuplicateSymbol: return RS_ERROR_CODE_V2_DUPLICATE_SYMBOL;
    case ErrorCode::InvalidObjectReference: return RS_ERROR_CODE_V2_INVALID_OBJECT_REFERENCE;
    case ErrorCode::InvalidNativeHandle: return RS_ERROR_CODE_V2_INVALID_NATIVE_HANDLE;
    case ErrorCode::NullReference: return RS_ERROR_CODE_V2_NULL_REFERENCE;
    case ErrorCode::IndexOutOfRange: return RS_ERROR_CODE_V2_INDEX_OUT_OF_RANGE;
    case ErrorCode::OutOfMemory: return RS_ERROR_CODE_V2_OUT_OF_MEMORY;
    case ErrorCode::InvalidProgram: return RS_ERROR_CODE_V2_INVALID_PROGRAM;
    case ErrorCode::ExecutionTerminated: return RS_ERROR_CODE_V2_EXECUTION_TERMINATED;
    case ErrorCode::DeterminismViolation: return RS_ERROR_CODE_V2_DETERMINISM_VIOLATION;
    case ErrorCode::ReplayMismatch: return RS_ERROR_CODE_V2_REPLAY_MISMATCH;
    case ErrorCode::ScriptException: return RS_ERROR_CODE_V2_SCRIPT_EXCEPTION;
    case ErrorCode::CapabilityDenied: return RS_ERROR_CODE_V2_CAPABILITY_DENIED;
    case ErrorCode::CrossHeapAccessViolation: return RS_ERROR_CODE_V2_CROSS_HEAP_VIOLATION;
    default: return RS_ERROR_CODE_V2_INVALID_PROGRAM;
    }
}

void clearVmLastErrorInternal(RsVmOpaque* vm) noexcept {
    if (!vm) return;
    vm->lastStatus = RS_STATUS_V1_OK;
    vm->lastErrorCode = RS_ERROR_CODE_V2_NONE;
    vm->lastErrorMessage.clear();
    vm->lastExceptionType.clear();
    vm->lastFrames.clear();
}

void recordVmExecutionError(
    RsVmOpaque* vm,
    const ExecutionResult& res,
    RsStatusV1 status) {
    if (!vm) return;
    vm->lastExecutedInstructions = res.instructionsExecuted;
    if (res.succeeded && status == RS_STATUS_V1_OK) {
        clearVmLastErrorInternal(vm);
        return;
    }
    vm->lastStatus = status;
    vm->lastErrorCode = mapErrorCodeToV2(res.error.code);
    vm->lastErrorMessage = res.error.message;
    if (res.error.code == ErrorCode::ScriptException) {
        vm->lastExceptionType = "ScriptException";
    } else if (res.error.code != ErrorCode::None) {
        const char* errName = errorCodeName(res.error.code);
        vm->lastExceptionType = errName ? std::string(errName) : std::string();
    } else {
        vm->lastExceptionType.clear();
    }
    vm->lastFrames.clear();
    if (!res.error.sourceLocations.empty()) {
        vm->lastFrames.reserve(res.error.sourceLocations.size());
        for (const auto& loc : res.error.sourceLocations) {
            RsVmOpaque::CachedErrorFrame cf{};
            cf.function = loc.function;
            if (vm->program && loc.symbolId != 0) {
                if (const auto* fl = vm->program->resolveFunction(loc.symbolId);
                    fl && fl->module) {
                    cf.moduleName = fl->module->name;
                }
            }
            if (cf.moduleName.empty()) {
                const auto sep = loc.function.find("::");
                if (sep != std::string::npos) {
                    cf.moduleName = loc.function.substr(0, sep);
                }
            }
            cf.sourceFile = loc.sourcePath;
            cf.line = loc.sourceLine;
            cf.column = loc.sourceColumn;
            vm->lastFrames.push_back(std::move(cf));
        }
    } else if (!res.error.stackTrace.empty()) {
        vm->lastFrames.reserve(res.error.stackTrace.size());
        for (const auto& frameName : res.error.stackTrace) {
            RsVmOpaque::CachedErrorFrame cf{};
            cf.function = frameName;
            if (vm->program) {
                if (const auto fid = vm->program->findFunction(frameName)) {
                    if (const auto* fl = vm->program->resolveFunction(*fid);
                        fl && fl->module) {
                        cf.moduleName = fl->module->name;
                    }
                }
            }
            if (cf.moduleName.empty()) {
                const auto sep = frameName.find("::");
                if (sep != std::string::npos) {
                    cf.moduleName = frameName.substr(0, sep);
                }
            }
            vm->lastFrames.push_back(std::move(cf));
        }
    }
}

void recordVmHostError(
    RsVmOpaque* vm,
    RsStatusV1 status,
    RsErrorCodeV2 errCode,
    const char* msg) {
    if (!vm) return;
    vm->lastStatus = status;
    vm->lastErrorCode = errCode;
    vm->lastErrorMessage = msg ? msg : "";
    vm->lastExceptionType.clear();
    vm->lastFrames.clear();
}

struct ResumeTokenRegistry {
    std::mutex mutex;
    std::atomic<std::uintptr_t> nextId{1};
    struct Entry {
        std::uint64_t vmInstanceId = 0;
        std::function<void()> callback;
    };
    std::unordered_map<std::uintptr_t, Entry> entries;
};

ResumeTokenRegistry& globalResumeRegistry() noexcept {
    static ResumeTokenRegistry reg;
    return reg;
}

void revokeAllPendingCallbacksForVm(std::uint64_t vmInstanceId) noexcept {
    auto& reg = globalResumeRegistry();
    std::lock_guard<std::mutex> lock(reg.mutex);
    for (auto it = reg.entries.begin(); it != reg.entries.end();) {
        if (it->second.vmInstanceId == vmInstanceId) {
            it = reg.entries.erase(it);
        } else {
            ++it;
        }
    }
}

void cAsyncResumeThunk(void* resumeCtx) noexcept {
    if (!resumeCtx) return;
    const std::uintptr_t id = reinterpret_cast<std::uintptr_t>(resumeCtx);
    std::function<void()> cb;
    {
        auto& reg = globalResumeRegistry();
        std::lock_guard<std::mutex> lock(reg.mutex);
        const auto it = reg.entries.find(id);
        if (it == reg.entries.end()) return;
        cb = std::move(it->second.callback);
        reg.entries.erase(it);
    }
    if (cb) {
        try {
            cb();
        } catch (...) {
        }
    }
}

RsStatusV1 marshalAwaitedValueToAbi(
    ManagedHeap* heap,
    const Value& awaited,
    RsValueV1* out) noexcept {
    const RsStatusV1 direct = marshalValueToAbi(heap, awaited, out);
    if (direct == RS_STATUS_V1_OK) {
        return RS_STATUS_V1_OK;
    }
    if (awaited.rawTag() == ValueTag::Struct) {
        if (const auto f0 = readStructFieldForAbi(awaited, 0)) {
            if (marshalValueToAbi(heap, *f0, out) == RS_STATUS_V1_OK) {
                return RS_STATUS_V1_OK;
            }
        }
        std::memset(out, 0, sizeof(*out));
        out->kind = RS_VALUE_KIND_V1_UINT64;
        out->payload.u64_val = static_cast<uint64_t>(awaited.asStructUnchecked().typeId);
        return RS_STATUS_V1_OK;
    }
    return direct;
}

class CAwaitSchedulerAdapter final : public IAwaitScheduler {
public:
    CAwaitSchedulerAdapter(
        std::uint64_t vmInstanceId,
        ManagedHeap* heap,
        RsAwaitSchedulerCallbacksV2 callbacks) noexcept
        : vmInstanceId_(vmInstanceId),
          heap_(heap),
          callbacks_(callbacks) {}

    [[nodiscard]] bool handlesValue(const Value& awaited) const noexcept override {
        if (!callbacks_.handles_value) return false;
        RsValueV1 abiVal{};
        if (marshalAwaitedValueToAbi(heap_, awaited, &abiVal) != RS_STATUS_V1_OK) {
            return false;
        }
        return callbacks_.handles_value(callbacks_.user_data, &abiVal) != 0;
    }

    [[nodiscard]] bool isCompleted(const Value& awaited) const noexcept override {
        if (!callbacks_.is_completed) return false;
        RsValueV1 abiVal{};
        if (marshalAwaitedValueToAbi(heap_, awaited, &abiVal) != RS_STATUS_V1_OK) {
            return false;
        }
        return callbacks_.is_completed(callbacks_.user_data, &abiVal) != 0;
    }

    bool tryGetResult(
        const Value& awaited,
        bool hasResult,
        Value& outResult,
        RuntimeError& outError) override {
        if (!callbacks_.try_get_result) {
            outError.code = ErrorCode::InvalidProgram;
            outError.message = "C await scheduler missing try_get_result callback";
            return false;
        }
        RsValueV1 abiAwaited{};
        if (marshalAwaitedValueToAbi(heap_, awaited, &abiAwaited) != RS_STATUS_V1_OK) {
            outError.code = ErrorCode::InvalidArguments;
            outError.message = "failed to marshal custom awaitable to C11 ABI";
            return false;
        }
        RsValueV1 abiOut{};
        char errBuf[512]{};
        const RsStatusV1 st = callbacks_.try_get_result(
            callbacks_.user_data,
            &abiAwaited,
            hasResult ? 1u : 0u,
            &abiOut,
            errBuf,
            sizeof(errBuf));
        if (st != RS_STATUS_V1_OK) {
            outError.code = (st == RS_STATUS_V1_INVALID_ARGUMENT)
                ? ErrorCode::InvalidArguments
                : ErrorCode::ScriptException;
            errBuf[sizeof(errBuf) - 1] = '\0';
            outError.message = errBuf[0] != '\0'
                ? std::string(errBuf)
                : std::string("custom C await scheduler try_get_result failed");
            return false;
        }
        if (hasResult && !marshalValueFromAbi(abiOut, outResult)) {
            outError.code = ErrorCode::TypeMismatch;
            outError.message = "custom C await scheduler returned unmarshalable result";
            return false;
        }
        return true;
    }

    bool onSuspend(
        const Value& awaited,
        std::function<void()> resumeCallback,
        RuntimeError& outError) override {
        if (!callbacks_.on_suspend) {
            outError.code = ErrorCode::InvalidProgram;
            outError.message = "C await scheduler missing on_suspend callback";
            return false;
        }
        RsValueV1 abiAwaited{};
        if (marshalAwaitedValueToAbi(heap_, awaited, &abiAwaited) != RS_STATUS_V1_OK) {
            outError.code = ErrorCode::InvalidArguments;
            outError.message = "failed to marshal custom awaitable to C11 ABI";
            return false;
        }
        std::uintptr_t tokenId = 0;
        {
            auto& reg = globalResumeRegistry();
            std::lock_guard<std::mutex> lock(reg.mutex);
            tokenId = reg.nextId.fetch_add(1, std::memory_order_relaxed);
            reg.entries.emplace(
                tokenId,
                ResumeTokenRegistry::Entry{vmInstanceId_, std::move(resumeCallback)});
        }
        char errBuf[512]{};
        const RsStatusV1 st = callbacks_.on_suspend(
            callbacks_.user_data,
            &abiAwaited,
            &cAsyncResumeThunk,
            reinterpret_cast<void*>(tokenId),
            errBuf,
            sizeof(errBuf));
        if (st != RS_STATUS_V1_OK) {
            auto& reg = globalResumeRegistry();
            std::lock_guard<std::mutex> lock(reg.mutex);
            reg.entries.erase(tokenId);
            outError.code = (st == RS_STATUS_V1_INVALID_ARGUMENT)
                ? ErrorCode::InvalidArguments
                : ErrorCode::InvalidProgram;
            errBuf[sizeof(errBuf) - 1] = '\0';
            outError.message = errBuf[0] != '\0'
                ? std::string(errBuf)
                : std::string("custom C await scheduler on_suspend rejected suspension");
            return false;
        }
        return true;
    }

    bool tryResolveAsyncReturn(
        semantic::SymbolId returnTypeId,
        const Value* payload,
        Value& outWrapped,
        RuntimeError& outError) override {
        if (!callbacks_.try_resolve_async_return) return false;
        RsValueV1 abiPayload{};
        const RsValueV1* payloadPtr = nullptr;
        if (payload) {
            if (marshalValueToAbi(heap_, *payload, &abiPayload) != RS_STATUS_V1_OK) {
                outError.code = ErrorCode::InvalidArguments;
                outError.message = "failed to marshal async return payload to C11 ABI";
                return false;
            }
            payloadPtr = &abiPayload;
        }
        RsValueV1 abiWrapped{};
        uint8_t handled = 0;
        char errBuf[512]{};
        const RsStatusV1 st = callbacks_.try_resolve_async_return(
            callbacks_.user_data,
            static_cast<uint64_t>(returnTypeId),
            payloadPtr,
            &abiWrapped,
            &handled,
            errBuf,
            sizeof(errBuf));
        if (st != RS_STATUS_V1_OK) {
            outError.code = ErrorCode::InvalidProgram;
            errBuf[sizeof(errBuf) - 1] = '\0';
            outError.message = errBuf[0] != '\0'
                ? std::string(errBuf)
                : std::string("custom C await scheduler try_resolve_async_return failed");
            return false;
        }
        if (handled == 0) return false;
        if (!marshalValueFromAbi(abiWrapped, outWrapped)) {
            outError.code = ErrorCode::TypeMismatch;
            outError.message = "custom C await scheduler returned unmarshalable carrier";
            return false;
        }
        if (returnTypeId != 0 && outWrapped.rawTag() != ValueTag::Struct) {
            std::vector<Value> fields;
            fields.push_back(std::move(outWrapped));
            outWrapped = Value{makeStructValueFromFields(returnTypeId, std::move(fields))};
        }
        return true;
    }

private:
    std::uint64_t vmInstanceId_ = 0;
    ManagedHeap* heap_ = nullptr;
    RsAwaitSchedulerCallbacksV2 callbacks_{};
};

RsPrimitiveKindV2 mapPrimitiveKindToV2(semantic::PrimitiveType p) noexcept {
    switch (p) {
    case semantic::PrimitiveType::Void: return RS_PRIMITIVE_V2_VOID;
    case semantic::PrimitiveType::Bool: return RS_PRIMITIVE_V2_BOOL;
    case semantic::PrimitiveType::Byte: return RS_PRIMITIVE_V2_BYTE;
    case semantic::PrimitiveType::SByte: return RS_PRIMITIVE_V2_SBYTE;
    case semantic::PrimitiveType::Short: return RS_PRIMITIVE_V2_SHORT;
    case semantic::PrimitiveType::UShort: return RS_PRIMITIVE_V2_USHORT;
    case semantic::PrimitiveType::Int: return RS_PRIMITIVE_V2_INT;
    case semantic::PrimitiveType::UInt: return RS_PRIMITIVE_V2_UINT;
    case semantic::PrimitiveType::Long: return RS_PRIMITIVE_V2_LONG;
    case semantic::PrimitiveType::ULong: return RS_PRIMITIVE_V2_ULONG;
    case semantic::PrimitiveType::Float: return RS_PRIMITIVE_V2_FLOAT;
    case semantic::PrimitiveType::Double: return RS_PRIMITIVE_V2_DOUBLE;
    case semantic::PrimitiveType::Char: return RS_PRIMITIVE_V2_CHAR;
    case semantic::PrimitiveType::String: return RS_PRIMITIVE_V2_STRING;
    case semantic::PrimitiveType::Object: return RS_PRIMITIVE_V2_OBJECT;
    case semantic::PrimitiveType::Struct: return RS_PRIMITIVE_V2_STRUCT;
    case semantic::PrimitiveType::Array: return RS_PRIMITIVE_V2_ARRAY;
    case semantic::PrimitiveType::Enum: return RS_PRIMITIVE_V2_ENUM;
    case semantic::PrimitiveType::Handle: return RS_PRIMITIVE_V2_HANDLE;
    default: return RS_PRIMITIVE_V2_UNKNOWN;
    }
}

semantic::PrimitiveType mapPrimitiveKindFromV2(RsPrimitiveKindV2 k) noexcept {
    switch (k) {
    case RS_PRIMITIVE_V2_VOID: return semantic::PrimitiveType::Void;
    case RS_PRIMITIVE_V2_BOOL: return semantic::PrimitiveType::Bool;
    case RS_PRIMITIVE_V2_BYTE: return semantic::PrimitiveType::Byte;
    case RS_PRIMITIVE_V2_SBYTE: return semantic::PrimitiveType::SByte;
    case RS_PRIMITIVE_V2_SHORT: return semantic::PrimitiveType::Short;
    case RS_PRIMITIVE_V2_USHORT: return semantic::PrimitiveType::UShort;
    case RS_PRIMITIVE_V2_INT: return semantic::PrimitiveType::Int;
    case RS_PRIMITIVE_V2_UINT: return semantic::PrimitiveType::UInt;
    case RS_PRIMITIVE_V2_LONG: return semantic::PrimitiveType::Long;
    case RS_PRIMITIVE_V2_ULONG: return semantic::PrimitiveType::ULong;
    case RS_PRIMITIVE_V2_FLOAT: return semantic::PrimitiveType::Float;
    case RS_PRIMITIVE_V2_DOUBLE: return semantic::PrimitiveType::Double;
    case RS_PRIMITIVE_V2_CHAR: return semantic::PrimitiveType::Char;
    case RS_PRIMITIVE_V2_STRING: return semantic::PrimitiveType::String;
    case RS_PRIMITIVE_V2_OBJECT: return semantic::PrimitiveType::Object;
    case RS_PRIMITIVE_V2_STRUCT: return semantic::PrimitiveType::Struct;
    case RS_PRIMITIVE_V2_ARRAY: return semantic::PrimitiveType::Array;
    case RS_PRIMITIVE_V2_ENUM: return semantic::PrimitiveType::Enum;
    case RS_PRIMITIVE_V2_HANDLE: return semantic::PrimitiveType::Handle;
    default: return semantic::PrimitiveType::Error;
    }
}

bool matchAttrTail(std::string_view attrName, std::string_view shortName, std::string_view fullName) noexcept {
    if (attrName == shortName || attrName == fullName) return true;
    const auto sep = attrName.rfind("::");
    if (sep != std::string_view::npos) {
        const auto tail = attrName.substr(sep + 2);
        return tail == shortName || tail == fullName;
    }
    return false;
}

std::string unquoteAttrText(std::string_view raw) {
    while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.front()))) raw.remove_prefix(1);
    while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.back()))) raw.remove_suffix(1);
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
        std::string out;
        out.reserve(raw.size() - 2);
        for (std::size_t i = 1; i + 1 < raw.size(); ++i) {
            if (raw[i] == '\\' && i + 2 < raw.size()) {
                ++i;
                switch (raw[i]) {
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                default: out.push_back(raw[i]); break;
                }
            } else {
                out.push_back(raw[i]);
            }
        }
        return out;
    }
    return std::string(raw);
}

std::string extractAttrStringArg(
    const compiler::LanguageAttributeRecord& rec,
    const char* const* preferredKeys,
    std::size_t keyCount) {
    for (std::size_t k = 0; k < keyCount; ++k) {
        for (const auto& arg : rec.arguments) {
            if (arg.name == preferredKeys[k]) {
                auto val = unquoteAttrText(arg.value);
                if (!val.empty()) return val;
            }
        }
    }
    for (const auto& arg : rec.arguments) {
        auto val = unquoteAttrText(arg.value);
        if (!val.empty()) return val;
    }
    return {};
}

std::optional<std::int32_t> extractFieldTagAttrValue(
    const compiler::LanguageAttributeRecord& rec) {
    static const char* const kTagKeys[] = {"tag", "id", "value", "arg0"};
    const auto parseI32 = [](std::string_view text) -> std::optional<std::int32_t> {
        auto trimmed = unquoteAttrText(text);
        if (trimmed.empty()) return std::nullopt;
        std::int32_t val = 0;
        const char* first = trimmed.data();
        const char* last = first + trimmed.size();
        const auto res = std::from_chars(first, last, val);
        if (res.ec == std::errc() && res.ptr == last) {
            return val;
        }
        return std::nullopt;
    };
    for (const char* key : kTagKeys) {
        for (const auto& arg : rec.arguments) {
            if (arg.name == key) {
                if (const auto parsed = parseI32(arg.value)) return parsed;
            }
        }
    }
    for (const auto& arg : rec.arguments) {
        if (const auto parsed = parseI32(arg.value)) return parsed;
    }
    return std::nullopt;
}

void ensureProgramIntrospection(RsProgramOpaque* progHandle) {
    std::call_once(progHandle->introspectOnce, [progHandle]() {
        const ProgramImage& prog = *progHandle->program;
        static const char* const kFormerKeys[] = {"oldName", "name", "arg0"};
        std::unordered_map<std::string, std::vector<std::string>> formerByTarget;
        std::unordered_map<std::string, std::int32_t> tagByTarget;
        semantic::TypeSymbolMap visibleTypes;

        for (const auto& mod : prog.modules()) {
            for (const auto& ts : mod.types) {
                visibleTypes.try_emplace(semantic::canonicalTypeName(ts), ts);
            }
            for (const auto& attr : mod.languageMetadata.attributes) {
                if (matchAttrTail(attr.name, "FormerlySerializedAs", "FormerlySerializedAsAttribute")) {
                    auto oldName = extractAttrStringArg(attr, kFormerKeys, 3);
                    if (!oldName.empty()) {
                        auto& list = formerByTarget[attr.target];
                        if (std::find(list.begin(), list.end(), oldName) == list.end()) {
                            list.push_back(std::move(oldName));
                        }
                    }
                } else if (matchAttrTail(attr.name, "FieldTag", "FieldTagAttribute")) {
                    if (const auto tag = extractFieldTagAttrValue(attr)) {
                        tagByTarget[attr.target] = *tag;
                    }
                }
            }
        }

        for (const auto& mod : prog.modules()) {
            for (const auto& ts : mod.types) {
                RsProgramOpaque::CachedTypeMeta tm{};
                tm.typeId = static_cast<std::uint64_t>(ts.id);
                tm.baseTypeId = static_cast<std::uint64_t>(ts.baseTypeId);
                tm.name = ts.name;
                tm.canonicalName = semantic::canonicalTypeName(ts);
                tm.moduleName = ts.moduleName.empty() ? mod.name : ts.moduleName;
                if (ts.interfaceType) {
                    tm.kind = RS_TYPE_KIND_V2_INTERFACE;
                } else if (ts.delegateType) {
                    tm.kind = RS_TYPE_KIND_V2_DELEGATE;
                } else if (ts.kind == semantic::TypeKind::Struct) {
                    tm.kind = RS_TYPE_KIND_V2_STRUCT;
                } else if (ts.kind == semantic::TypeKind::Enum) {
                    tm.kind = RS_TYPE_KIND_V2_ENUM;
                } else {
                    tm.kind = RS_TYPE_KIND_V2_CLASS;
                }
                const auto layout = (ts.kind == semantic::TypeKind::Struct)
                    ? semantic::computeBlittableStructLayout(ts, visibleTypes)
                    : std::nullopt;
                tm.isBlittable = (ts.kind == semantic::TypeKind::Struct) &&
                    !ts.hasValueLifecycle &&
                    layout.has_value() &&
                    (ts.blittable || isBlittableStructType(ts.id));

                std::unordered_map<std::string, std::uint32_t> offsetByField;
                if (layout) {
                    tm.nativeByteSize = layout->strideBytes;
                    for (const auto& blf : layout->fields) {
                        offsetByField.emplace(blf.name, blf.offsetBytes);
                    }
                }

                tm.fields.reserve(ts.fields.size());
                for (std::size_t fi = 0; fi < ts.fields.size(); ++fi) {
                    const auto& f = ts.fields[fi];
                    RsProgramOpaque::CachedFieldMeta fm{};
                    fm.name = f.name;
                    fm.typeId = static_cast<std::uint64_t>(exactFieldTypeId(f.type, f.typeName));
                    fm.semanticPrimitive = f.type;
                    fm.primitiveKind = mapPrimitiveKindToV2(f.type);
                    fm.isSynthetic = f.synthetic;
                    fm.fieldIndex = static_cast<std::uint32_t>(f.index);
                    if (const auto offIt = offsetByField.find(f.name);
                        offIt != offsetByField.end()) {
                        fm.byteOffset = offIt->second;
                    }

                    const std::string ownTarget = tm.canonicalName + "::field:" + f.name;
                    const std::string declTarget = f.declaringTypeName.empty()
                        ? std::string()
                        : (f.declaringTypeName + "::field:" + f.name);
                    std::string propTarget;
                    std::string declPropTarget;
                    if (f.synthetic && f.name.size() > 1u && f.name.front() == '$') {
                        const std::string propName = f.name.substr(1u);
                        propTarget = tm.canonicalName + "::property:" + propName;
                        if (!f.declaringTypeName.empty()) {
                            declPropTarget =
                                f.declaringTypeName + "::property:" + propName;
                        }
                    }

                    if (f.fieldTag.has_value()) {
                        fm.hasFieldTag = true;
                        fm.fieldTag = *f.fieldTag;
                    } else if (const auto tagIt = tagByTarget.find(ownTarget);
                        tagIt != tagByTarget.end()) {
                        fm.hasFieldTag = true;
                        fm.fieldTag = tagIt->second;
                    } else if (!declTarget.empty()) {
                        if (const auto dTagIt = tagByTarget.find(declTarget);
                            dTagIt != tagByTarget.end()) {
                            fm.hasFieldTag = true;
                            fm.fieldTag = dTagIt->second;
                        }
                    }
                    if (!fm.hasFieldTag && !propTarget.empty()) {
                        if (const auto pTagIt = tagByTarget.find(propTarget);
                            pTagIt != tagByTarget.end()) {
                            fm.hasFieldTag = true;
                            fm.fieldTag = pTagIt->second;
                        } else if (!declPropTarget.empty()) {
                            if (const auto dpIt = tagByTarget.find(declPropTarget);
                                dpIt != tagByTarget.end()) {
                                fm.hasFieldTag = true;
                                fm.fieldTag = dpIt->second;
                            }
                        }
                    }

                    const auto pushFormer = [&](const std::string& alias) {
                        if (!alias.empty() &&
                            std::find(fm.formerNames.begin(), fm.formerNames.end(), alias) ==
                                fm.formerNames.end()) {
                            fm.formerNames.push_back(alias);
                        }
                    };
                    for (const auto& alias : f.formerlySerializedAs) {
                        pushFormer(alias);
                    }
                    const auto appendFormers = [&](const std::string& target) {
                        if (target.empty()) return;
                        if (const auto it = formerByTarget.find(target);
                            it != formerByTarget.end()) {
                            for (const auto& alias : it->second) {
                                pushFormer(alias);
                            }
                        }
                    };
                    if (!declTarget.empty() && declTarget != ownTarget) {
                        appendFormers(declTarget);
                    }
                    appendFormers(ownTarget);
                    if (!declPropTarget.empty() && declPropTarget != propTarget) {
                        appendFormers(declPropTarget);
                    }
                    appendFormers(propTarget);
                    for (std::size_t ai = 0; ai < fm.formerNames.size(); ++ai) {
                        if (ai > 0) fm.formerNamesCsv.push_back(',');
                        fm.formerNamesCsv += fm.formerNames[ai];
                    }
                    tm.fields.push_back(std::move(fm));
                }

                const std::size_t typeIdx = progHandle->cachedTypes.size();
                progHandle->typeIdToIndex[tm.typeId] = typeIdx;
                progHandle->typeNameToId[tm.canonicalName] = tm.typeId;
                if (!tm.moduleName.empty()) {
                    progHandle->typeNameToId.try_emplace(tm.moduleName + "." + tm.name, tm.typeId);
                }
                progHandle->typeNameToId.try_emplace(tm.name, tm.typeId);
                progHandle->cachedTypes.push_back(std::move(tm));
            }
        }
    });
}

void appendJsonString(std::string& out, std::string_view text) {
    out.push_back('"');
    for (const unsigned char ch : text) {
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (ch < 0x20u) {
                static constexpr char kHex[] = "0123456789abcdef";
                out += "\\u00";
                out.push_back(kHex[(ch >> 4u) & 0xFu]);
                out.push_back(kHex[ch & 0xFu]);
            } else {
                out.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    out.push_back('"');
}

const std::string& ensureProgramAbiJson(RsProgramOpaque* progHandle) {
    ensureProgramIntrospection(progHandle);
    std::call_once(progHandle->jsonOnce, [progHandle]() {
        std::string j;
        j.reserve(4096);
        j += "{\"abiMajor\":";
        j += std::to_string(RS_HOST_ABI_MAJOR_V2);
        j += ",\"abiMinor\":";
        j += std::to_string(RS_HOST_ABI_MINOR_V2);
        j += ",\"modules\":[";
        const auto& mods = progHandle->program->modules();
        for (std::size_t mi = 0; mi < mods.size(); ++mi) {
            if (mi > 0) j.push_back(',');
            appendJsonString(j, mods[mi].name);
        }
        j += "],\"types\":[";
        for (std::size_t ti = 0; ti < progHandle->cachedTypes.size(); ++ti) {
            const auto& tm = progHandle->cachedTypes[ti];
            if (ti > 0) j.push_back(',');
            j += "{\"typeId\":";
            j += std::to_string(tm.typeId);
            j += ",\"baseTypeId\":";
            j += std::to_string(tm.baseTypeId);
            j += ",\"name\":";
            appendJsonString(j, tm.name);
            j += ",\"canonicalName\":";
            appendJsonString(j, tm.canonicalName);
            j += ",\"module\":";
            appendJsonString(j, tm.moduleName);
            j += ",\"kind\":";
            j += std::to_string(static_cast<unsigned>(tm.kind));
            j += ",\"isBlittable\":";
            j += tm.isBlittable ? "true" : "false";
            j += ",\"nativeByteSize\":";
            j += std::to_string(tm.nativeByteSize);
            j += ",\"fields\":[";
            for (std::size_t fi = 0; fi < tm.fields.size(); ++fi) {
                const auto& fm = tm.fields[fi];
                if (fi > 0) j.push_back(',');
                j += "{\"name\":";
                appendJsonString(j, fm.name);
                j += ",\"index\":";
                j += std::to_string(fm.fieldIndex);
                j += ",\"primitive\":";
                j += std::to_string(static_cast<unsigned>(fm.primitiveKind));
                j += ",\"typeId\":";
                j += std::to_string(fm.typeId);
                j += ",\"byteOffset\":";
                j += std::to_string(fm.byteOffset);
                j += ",\"isSynthetic\":";
                j += fm.isSynthetic ? "true" : "false";
                j += ",\"hasFieldTag\":";
                j += fm.hasFieldTag ? "true" : "false";
                j += ",\"fieldTag\":";
                j += std::to_string(fm.fieldTag);
                j += ",\"formerNames\":[";
                for (std::size_t ai = 0; ai < fm.formerNames.size(); ++ai) {
                    if (ai > 0) j.push_back(',');
                    appendJsonString(j, fm.formerNames[ai]);
                }
                j += "]}";
            }
            j += "]}";
        }
        j += "],\"functions\":[";
        bool firstFn = true;
        for (const auto& mod : mods) {
            for (const auto& fn : mod.functions) {
                if (!firstFn) j.push_back(',');
                firstFn = false;
                j += "{\"symbolId\":";
                j += std::to_string(fn.symbolId);
                j += ",\"name\":";
                appendJsonString(j, fn.name);
                j += ",\"returnPrimitive\":";
                j += std::to_string(static_cast<unsigned>(mapPrimitiveKindToV2(fn.returnType)));
                j += ",\"returnTypeId\":";
                j += std::to_string(fn.returnTypeId);
                j += ",\"parameters\":[";
                for (std::size_t pi = 0; pi < fn.parameterTypes.size(); ++pi) {
                    if (pi > 0) j.push_back(',');
                    const semantic::SymbolId ptid =
                        pi < fn.parameterTypeIds.size() ? fn.parameterTypeIds[pi] : 0;
                    j += "{\"primitive\":";
                    j += std::to_string(
                        static_cast<unsigned>(mapPrimitiveKindToV2(fn.parameterTypes[pi])));
                    j += ",\"typeId\":";
                    j += std::to_string(ptid);
                    j += "}";
                }
                j += "]}";
            }
        }
        j += "]}";
        progHandle->cachedAbiJson = std::move(j);
    });
    return progHandle->cachedAbiJson;
}

RsStatusV1 apiHeapCreate(size_t maxBytes, RsHeapHandleV1* outHeap) noexcept {
    if (!outHeap) return RS_STATUS_V1_INVALID_ARGUMENT;
    try {
        HeapConfig opts{};
        if (maxBytes > 0) opts.maximumHeapBytes = maxBytes;
        auto* wrapper = new (std::nothrow) RsHeapOpaque{
            std::make_shared<ManagedHeap>(opts)};
        if (!wrapper) return RS_STATUS_V1_OUT_OF_MEMORY;
        *outHeap = wrapper;
        return RS_STATUS_V1_OK;
    } catch (...) {
        return RS_STATUS_V1_OUT_OF_MEMORY;
    }
}

void apiHeapDestroy(RsHeapHandleV1 heap) noexcept {
    delete heap;
}

uint64_t apiHeapId(RsHeapHandleV1 heap) noexcept {
    return (heap && heap->heap) ? heap->heap->heapId() : 0u;
}

RsStatusV1 apiHeapCollect(RsHeapHandleV1 heap) noexcept {
    if (!heap || !heap->heap) return RS_STATUS_V1_INVALID_ARGUMENT;
    try {
        ShadowStack emptyRoots;
        heap->heap->collect(emptyRoots);
        return RS_STATUS_V1_OK;
    } catch (...) {
        return RS_STATUS_V1_RUNTIME_ERROR;
    }
}

RsStatusV1 apiHeapCollectBudgetNs(
    RsHeapHandleV1 heap,
    uint64_t budgetNs,
    uint8_t* outCompleted) noexcept {
    if (!heap || !heap->heap) return RS_STATUS_V1_INVALID_ARGUMENT;
    try {
        ShadowStack emptyRoots;
        const auto stats = heap->heap->collectIncrementalWithBudget(
            emptyRoots,
            std::chrono::nanoseconds(budgetNs),
            32,
            true);
        if (outCompleted) *outCompleted = stats.cycleCompleted ? 1u : 0u;
        return RS_STATUS_V1_OK;
    } catch (...) {
        return RS_STATUS_V1_RUNTIME_ERROR;
    }
}

RsStatusV1 apiHeapPinObject(
    RsHeapHandleV1 heap,
    RsObjectRefV1 objectRef,
    uint64_t* outRootToken) noexcept {
    if (!heap || !heap->heap || !outRootToken) return RS_STATUS_V1_INVALID_ARGUMENT;
    try {
        const auto tok = heap->heap->addPersistentRoot(fromAbiObjectRef(objectRef));
        if (tok == 0) return RS_STATUS_V1_STALE_REFERENCE;
        *outRootToken = tok;
        return RS_STATUS_V1_OK;
    } catch (...) {
        return RS_STATUS_V1_RUNTIME_ERROR;
    }
}

RsStatusV1 apiHeapUnpinRoot(RsHeapHandleV1 heap, uint64_t rootToken) noexcept {
    if (!heap || !heap->heap) return RS_STATUS_V1_INVALID_ARGUMENT;
    return heap->heap->removePersistentRoot(rootToken)
        ? RS_STATUS_V1_OK
        : RS_STATUS_V1_INVALID_ARGUMENT;
}

uint8_t apiHeapIsAlive(RsHeapHandleV1 heap, RsObjectRefV1 objectRef) noexcept {
    if (!heap || !heap->heap) return 0u;
    return heap->heap->isAlive(fromAbiObjectRef(objectRef)) ? 1u : 0u;
}

RsStatusV1 apiHeapSetSweepCallback(
    RsHeapHandleV1 heap,
    RsNativeSweepCallbackV1 callback,
    void* userData) noexcept {
    if (!heap || !heap->heap) return RS_STATUS_V1_INVALID_ARGUMENT;
    try {
        if (!callback) {
            heap->heap->setNativeWrapperSweepCallback(nullptr);
            return RS_STATUS_V1_OK;
        }
        heap->heap->setNativeWrapperSweepCallback(
            [callback, userData](NativeHandle nh, std::uint64_t hid, ObjectRef swept) {
                callback(userData, toAbiNativeHandle(nh), hid, toAbiObjectRef(swept));
            });
        return RS_STATUS_V1_OK;
    } catch (...) {
        return RS_STATUS_V1_OUT_OF_MEMORY;
    }
}

RsWeakRefV1 apiWeakFromObject(RsObjectRefV1 objectRef) noexcept {
    RsWeakRefV1 w{};
    w.slot = objectRef.slot;
    w.generation = objectRef.generation;
    w.heap_id = objectRef.heap_id;
    w.object_kind = objectRef.object_kind;
    return w;
}

RsStatusV1 apiWeakTryResolve(
    RsHeapHandleV1 heap,
    RsWeakRefV1 weakRef,
    RsObjectRefV1* outObjectRef) noexcept {
    if (!heap || !heap->heap || !outObjectRef) return RS_STATUS_V1_INVALID_ARGUMENT;
    ObjectRef candidate{};
    candidate.kind = static_cast<ObjectKind>(weakRef.object_kind);
    candidate.slot = weakRef.slot;
    candidate.generation = weakRef.generation;
    candidate.heapId = weakRef.heap_id;
    if (!heap->heap->isAlive(candidate)) {
        return RS_STATUS_V1_STALE_REFERENCE;
    }
    *outObjectRef = toAbiObjectRef(candidate);
    return RS_STATUS_V1_OK;
}

RsStatusV1 apiHeapEnableDirtyTracking(
    RsHeapHandleV1 heap,
    uint64_t typeId,
    const uint8_t* fieldToBitMap,
    uint32_t fieldCount) noexcept {
    if (!heap || !heap->heap || (fieldCount > 0 && !fieldToBitMap)) {
        return RS_STATUS_V1_INVALID_ARGUMENT;
    }
    try {
        std::vector<std::uint8_t> map(fieldToBitMap, fieldToBitMap + fieldCount);
        heap->heap->enableTypeDirtyTracking(typeId, std::move(map));
        return RS_STATUS_V1_OK;
    } catch (...) {
        return RS_STATUS_V1_OUT_OF_MEMORY;
    }
}

RsStatusV1 apiHeapMarkDirtyBit(
    RsHeapHandleV1 heap,
    RsObjectRefV1 objectRef,
    uint8_t bitIndex) noexcept {
    if (!heap || !heap->heap) return RS_STATUS_V1_INVALID_ARGUMENT;
    if (!heap->heap->markDirtyBit(fromAbiObjectRef(objectRef), bitIndex)) {
        return RS_STATUS_V1_STALE_REFERENCE;
    }
    return RS_STATUS_V1_OK;
}

uint64_t apiHeapPeekDirtyMask(RsHeapHandleV1 heap, RsObjectRefV1 objectRef) noexcept {
    if (!heap || !heap->heap) return 0u;
    return heap->heap->peekDirtyMask(fromAbiObjectRef(objectRef));
}

uint64_t apiHeapConsumeDirtyMask(RsHeapHandleV1 heap, RsObjectRefV1 objectRef) noexcept {
    if (!heap || !heap->heap) return 0u;
    return heap->heap->consumeDirtyMask(fromAbiObjectRef(objectRef));
}

RsStatusV1 apiHeapReadFieldWithPolicy(
    RsHeapHandleV1 callerHeap,
    RsHeapHandleV1 targetHeap,
    RsObjectRefV1 targetObject,
    uint32_t fieldIndex,
    RsCrossHeapPolicyV1 policy,
    RsValueV1* outValue) noexcept {
    if (!callerHeap || !callerHeap->heap || !targetHeap || !targetHeap->heap || !outValue) {
        return RS_STATUS_V1_INVALID_ARGUMENT;
    }
    RuntimeError err;
    Value readVal{};
    const auto cppPolicy = static_cast<CrossHeapAccessPolicy>(policy);
    if (!callerHeap->heap->readFieldWithPolicy(
            *targetHeap->heap,
            fromAbiObjectRef(targetObject),
            fieldIndex,
            cppPolicy,
            readVal,
            &err)) {
        return mapRuntimeError(err.code);
    }
    return marshalValueToAbi(callerHeap->heap.get(), readVal, outValue);
}

RsStatusV1 apiHeapWriteFieldWithPolicy(
    RsHeapHandleV1 callerHeap,
    RsHeapHandleV1 targetHeap,
    RsObjectRefV1 targetObject,
    uint32_t fieldIndex,
    RsCrossHeapPolicyV1 policy,
    const RsValueV1* newValue) noexcept {
    if (!callerHeap || !callerHeap->heap || !targetHeap || !targetHeap->heap || !newValue) {
        return RS_STATUS_V1_INVALID_ARGUMENT;
    }
    Value cppVal{};
    if (!marshalValueFromAbi(*newValue, cppVal)) {
        return RS_STATUS_V1_INVALID_ARGUMENT;
    }
    RuntimeError err;
    const auto cppPolicy = static_cast<CrossHeapAccessPolicy>(policy);
    if (!callerHeap->heap->writeFieldWithPolicy(
            *targetHeap->heap,
            fromAbiObjectRef(targetObject),
            fieldIndex,
            std::move(cppVal),
            cppPolicy,
            &err)) {
        return mapRuntimeError(err.code);
    }
    return RS_STATUS_V1_OK;
}

RsStatusV1 apiStringAllocUtf8(
    RsHeapHandleV1 heap,
    const char* utf8Data,
    size_t utf8ByteLen,
    RsObjectRefV1* outStringRef) noexcept {
    if (!heap || !heap->heap || !outStringRef || (utf8ByteLen > 0 && !utf8Data)) {
        return RS_STATUS_V1_INVALID_ARGUMENT;
    }
    try {
        RuntimeError err;
        const std::string_view sv(utf8Data ? utf8Data : "", utf8ByteLen);
        const auto ref = heap->heap->allocateString(std::string(sv), &err);
        if (!ref) return mapRuntimeError(err.code);
        *outStringRef = toAbiObjectRef(*ref);
        return RS_STATUS_V1_OK;
    } catch (...) {
        return RS_STATUS_V1_OUT_OF_MEMORY;
    }
}

RsStatusV1 apiStringAllocUtf16(
    RsHeapHandleV1 heap,
    const uint16_t* utf16Data,
    size_t utf16UnitLen,
    RsObjectRefV1* outStringRef) noexcept {
    if (!heap || !heap->heap || !outStringRef || (utf16UnitLen > 0 && !utf16Data)) {
        return RS_STATUS_V1_INVALID_ARGUMENT;
    }
    try {
        std::string utf8;
        const std::u16string_view u16(
            reinterpret_cast<const char16_t*>(utf16Data), utf16UnitLen);
        utf16ToUtf8(u16, utf8);
        RuntimeError err;
        const auto ref = heap->heap->allocateString(utf8, &err);
        if (!ref) return mapRuntimeError(err.code);
        *outStringRef = toAbiObjectRef(*ref);
        return RS_STATUS_V1_OK;
    } catch (...) {
        return RS_STATUS_V1_OUT_OF_MEMORY;
    }
}

RsStatusV1 apiStringReadUtf8(
    RsHeapHandleV1 heap,
    RsObjectRefV1 stringRef,
    const char** outUtf8Ptr,
    size_t* outUtf8ByteLen) noexcept {
    if (!heap || !heap->heap || !outUtf8Ptr || !outUtf8ByteLen) {
        return RS_STATUS_V1_INVALID_ARGUMENT;
    }
    const auto sv = heap->heap->stringView(fromAbiObjectRef(stringRef));
    if (!sv) return RS_STATUS_V1_STALE_REFERENCE;
    *outUtf8Ptr = sv->data();
    *outUtf8ByteLen = sv->size();
    return RS_STATUS_V1_OK;
}

RsStatusV1 apiStringReadUtf16(
    RsHeapHandleV1 heap,
    RsObjectRefV1 stringRef,
    uint16_t* outUtf16Buf,
    size_t bufCapacityUnits,
    size_t* outRequiredUnits) noexcept {
    if (!heap || !heap->heap || !outRequiredUnits) {
        return RS_STATUS_V1_INVALID_ARGUMENT;
    }
    const auto sv = heap->heap->stringView(fromAbiObjectRef(stringRef));
    if (!sv) return RS_STATUS_V1_STALE_REFERENCE;
    try {
        std::u16string u16;
        utf8ToUtf16(*sv, u16);
        *outRequiredUnits = u16.size();
        if (!outUtf16Buf) return RS_STATUS_V1_OK;
        if (bufCapacityUnits < u16.size()) return RS_STATUS_V1_BUFFER_TOO_SMALL;
        if (!u16.empty()) {
            std::memcpy(outUtf16Buf, u16.data(), u16.size() * sizeof(uint16_t));
        }
        return RS_STATUS_V1_OK;
    } catch (...) {
        return RS_STATUS_V1_OUT_OF_MEMORY;
    }
}

RsStatusV1 apiProgramLoadRsbc(
    const uint8_t* rsbcBytes,
    size_t rsbcSize,
    RsProgramHandleV1* outProgram) noexcept {
    if (!rsbcBytes || rsbcSize == 0 || !outProgram) {
        return RS_STATUS_V1_INVALID_ARGUMENT;
    }
    try {
        std::vector<std::uint8_t> buf(rsbcBytes, rsbcBytes + rsbcSize);
        diagnostics::DiagnosticBag diags;
        bytecode::Module mod;
        if (!bytecode::decodeModule(buf, mod, diags) || diags.hasErrors()) {
            return RS_STATUS_V1_RUNTIME_ERROR;
        }
        std::vector<bytecode::Module> mods;
        mods.push_back(std::move(mod));
        RuntimeError linkErr;
        auto linked = ProgramImage::link(std::move(mods), linkErr);
        if (!linked) return mapRuntimeError(linkErr.code);
        auto* handle = new (std::nothrow) RsProgramOpaque{
            std::make_shared<const ProgramImage>(std::move(*linked))};
        if (!handle) return RS_STATUS_V1_OUT_OF_MEMORY;
        *outProgram = handle;
        return RS_STATUS_V1_OK;
    } catch (...) {
        return RS_STATUS_V1_RUNTIME_ERROR;
    }
}

void apiProgramDestroy(RsProgramHandleV1 program) noexcept {
    delete program;
}

RsStatusV1 apiVmCreate(
    RsProgramHandleV1 program,
    RsHeapHandleV1 heap,
    RsVmHandleV1* outVm) noexcept {
    if (!program || !program->program || !outVm) {
        return RS_STATUS_V1_INVALID_ARGUMENT;
    }
    try {
        auto targetHeap = (heap && heap->heap)
            ? heap->heap
            : std::make_shared<ManagedHeap>();
        auto bindings = std::make_shared<BindingRegistry>();
        auto interp = std::make_unique<Interpreter>(
            program->program, targetHeap);
        interp->setBindingRegistry(bindings);
        interp->setAsyncRuntime(std::make_shared<AsyncRuntimeContext>(targetHeap.get()));
        auto* vm = new (std::nothrow) RsVmOpaque{
            program->program,
            std::move(targetHeap),
            std::move(bindings),
            std::move(interp)};
        if (!vm) return RS_STATUS_V1_OUT_OF_MEMORY;
        *outVm = vm;
        return RS_STATUS_V1_OK;
    } catch (...) {
        return RS_STATUS_V1_OUT_OF_MEMORY;
    }
}

void apiVmDestroy(RsVmHandleV1 vm) noexcept {
    delete vm;
}

RsStatusV1 apiVmBindHostCallback(
    RsVmHandleV1 vm,
    const char* qualifiedName,
    RsHostCallbackV1 callback,
    void* userData) noexcept {
    if (!vm || !vm->bindings || !qualifiedName || !callback) {
        return RS_STATUS_V1_INVALID_ARGUMENT;
    }
    try {
        ManagedHeap* vmHeap = vm->heap.get();
        const bool ok = vm->bindings->bind(
            std::string(qualifiedName),
            [callback, userData, vmHeap](
                const bytecode::FunctionReference&,
                ValueSpan args,
                RuntimeError& error) -> std::optional<Value> {
                std::vector<RsValueV1> abiArgs(args.size());
                for (std::size_t i = 0; i < args.size(); ++i) {
                    if (marshalValueToAbi(vmHeap, args[i], &abiArgs[i]) != RS_STATUS_V1_OK) {
                        error.code = ErrorCode::InvalidArguments;
                        error.message = "failed to marshal argument to C11 host callback";
                        return std::nullopt;
                    }
                }
                RsValueV1 abiOut{};
                const RsStatusV1 st = callback(
                    userData,
                    abiArgs.empty() ? nullptr : abiArgs.data(),
                    static_cast<uint32_t>(abiArgs.size()),
                    &abiOut);
                if (st != RS_STATUS_V1_OK) {
                    error.code = ErrorCode::ExecutionTerminated;
                    error.message = "C11 host callback returned error status";
                    return std::nullopt;
                }
                Value retVal{};
                if (!marshalValueFromAbi(abiOut, retVal)) {
                    error.code = ErrorCode::TypeMismatch;
                    error.message = "invalid return value from C11 host callback";
                    return std::nullopt;
                }
                return retVal;
            });
        return ok ? RS_STATUS_V1_OK : RS_STATUS_V1_INVALID_ARGUMENT;
    } catch (...) {
        return RS_STATUS_V1_OUT_OF_MEMORY;
    }
}

RsStatusV1 invokeWithOptionsAndRecord(
    RsVmHandleV1 vm,
    const ExecutionResult& res,
    RsValueV1* outResult) noexcept {
    if (!vm) return RS_STATUS_V1_INVALID_ARGUMENT;
    try {
        if (!res.succeeded) {
            const RsStatusV1 st = mapRuntimeError(res.error.code);
            recordVmExecutionError(vm, res, st);
            return st;
        }
        recordVmExecutionError(vm, res, RS_STATUS_V1_OK);
        if (outResult) {
            const RsStatusV1 marshalSt =
                marshalValueToAbi(vm->heap.get(), res.value, outResult);
            if (marshalSt != RS_STATUS_V1_OK) {
                recordVmHostError(
                    vm,
                    marshalSt,
                    RS_ERROR_CODE_V2_TYPE_MISMATCH,
                    "failed to marshal invocation return value to C11 ABI");
            }
            return marshalSt;
        }
        return RS_STATUS_V1_OK;
    } catch (...) {
        recordVmHostError(
            vm,
            RS_STATUS_V1_RUNTIME_ERROR,
            RS_ERROR_CODE_V2_INVALID_PROGRAM,
            "unexpected internal error recording VM invocation");
        return RS_STATUS_V1_RUNTIME_ERROR;
    }
}

RsStatusV1 coerceInvocationArgs(
    ManagedHeap* heap,
    const bytecode::Function* fn,
    const RsValueV1* arguments,
    uint32_t argumentCount,
    uint32_t argOffset,
    std::vector<Value>& outCppArgs) noexcept {
    for (uint32_t i = 0; i < argumentCount; ++i) {
        const std::size_t dstIdx = static_cast<std::size_t>(argOffset) + i;
        if (fn && dstIdx < fn->parameterTypes.size()) {
            const semantic::SymbolId pTid =
                dstIdx < fn->parameterTypeIds.size() ? fn->parameterTypeIds[dstIdx] : 0;
            if (!coerceAbiValueForType(
                    arguments[i], fn->parameterTypes[dstIdx], pTid, heap, outCppArgs[dstIdx])) {
                return RS_STATUS_V1_INVALID_ARGUMENT;
            }
        } else {
            if (!marshalValueFromAbi(arguments[i], outCppArgs[dstIdx])) {
                return RS_STATUS_V1_INVALID_ARGUMENT;
            }
        }
    }
    return RS_STATUS_V1_OK;
}

RsStatusV1 apiVmInvoke(
    RsVmHandleV1 vm,
    const char* qualifiedName,
    const RsValueV1* arguments,
    uint32_t argumentCount,
    RsValueV1* outResult) noexcept {
    if (!vm || !vm->interpreter || !qualifiedName ||
        (argumentCount > 0 && !arguments)) {
        if (vm) {
            recordVmHostError(
                vm,
                RS_STATUS_V1_INVALID_ARGUMENT,
                RS_ERROR_CODE_V2_INVALID_ARGUMENTS,
                "invalid arguments to vm_invoke");
        }
        return RS_STATUS_V1_INVALID_ARGUMENT;
    }
    try {
        ScopedValuePoolDomain domainScope(
            vm->heap ? vm->heap->valuePoolDomain() : nullptr);
        std::vector<Value> cppArgs(argumentCount);
        const bytecode::Function* fnMeta = nullptr;
        if (vm->program) {
            if (const auto fid = vm->program->findFunction(qualifiedName)) {
                if (const auto* loc = vm->program->resolveFunction(*fid)) {
                    fnMeta = loc->function;
                }
            }
        }
        if (fnMeta && fnMeta->parameterTypes.size() == argumentCount) {
            if (coerceInvocationArgs(
                    vm->heap.get(),
                    fnMeta,
                    arguments,
                    argumentCount,
                    0u,
                    cppArgs) !=
                RS_STATUS_V1_OK) {
                recordVmHostError(
                    vm,
                    RS_STATUS_V1_INVALID_ARGUMENT,
                    RS_ERROR_CODE_V2_INVALID_ARGUMENTS,
                    "failed to coerce argument to parameter type");
                return RS_STATUS_V1_INVALID_ARGUMENT;
            }
        } else {
            for (uint32_t i = 0; i < argumentCount; ++i) {
                if (!marshalValueFromAbi(arguments[i], cppArgs[i])) {
                    recordVmHostError(
                        vm,
                        RS_STATUS_V1_INVALID_ARGUMENT,
                        RS_ERROR_CODE_V2_INVALID_ARGUMENTS,
                        "unsupported argument RsValueKindV1");
                    return RS_STATUS_V1_INVALID_ARGUMENT;
                }
            }
        }
        const auto res = vm->interpreter->invoke(
            std::string(qualifiedName), cppArgs, vm->options);
        return invokeWithOptionsAndRecord(vm, res, outResult);
    } catch (...) {
        recordVmHostError(
            vm,
            RS_STATUS_V1_RUNTIME_ERROR,
            RS_ERROR_CODE_V2_INVALID_PROGRAM,
            "unexpected C++ exception in vm_invoke");
        return RS_STATUS_V1_RUNTIME_ERROR;
    }
}

/* ---------------- V2 Implementations ---------------- */

RsStatusV1 apiDomainCreate(RsDomainHandleV2* outDomain) noexcept {
    if (!outDomain) return RS_STATUS_V1_INVALID_ARGUMENT;
    try {
        auto poolDomain = ValuePoolDomain::create();
        if (!poolDomain) return RS_STATUS_V1_OUT_OF_MEMORY;
        auto* dom = new (std::nothrow) RsDomainOpaque{};
        if (!dom) return RS_STATUS_V1_OUT_OF_MEMORY;
        dom->valuePoolDomain = std::move(poolDomain);
        *outDomain = dom;
        return RS_STATUS_V1_OK;
    } catch (...) {
        return RS_STATUS_V1_OUT_OF_MEMORY;
    }
}

void apiDomainDestroy(RsDomainHandleV2 domain) noexcept {
    if (!domain) return;
    {
        std::lock_guard<std::mutex> lock(domain->mutex);
        domain->heaps.clear();
    }
    domain->valuePoolDomain.reset();
    delete domain;
}

RsStatusV1 apiDomainCollect(
    RsDomainHandleV2 domain,
    uint64_t* outReclaimedBytes) noexcept {
    if (!domain) return RS_STATUS_V1_INVALID_ARGUMENT;
    try {
        std::vector<std::shared_ptr<ManagedHeap>> liveHeaps;
        {
            std::lock_guard<std::mutex> lock(domain->mutex);
            for (auto it = domain->heaps.begin(); it != domain->heaps.end();) {
                if (auto locked = it->lock()) {
                    liveHeaps.push_back(std::move(locked));
                    ++it;
                } else {
                    it = domain->heaps.erase(it);
                }
            }
        }
        std::uint64_t totalReclaimed = 0;
        ShadowStack emptyRoots;
        ScopedValuePoolDomain domainScope(domain->valuePoolDomain.get());
        for (const auto& h : liveHeaps) {
            const std::uint64_t before = h->statistics().bytesReclaimed;
            h->collectFull(emptyRoots);
            const std::uint64_t after = h->statistics().bytesReclaimed;
            if (after >= before) {
                totalReclaimed += (after - before);
            }
        }
        if (domain->valuePoolDomain) {
            domain->valuePoolDomain->collectGarbage();
        }
        if (outReclaimedBytes) *outReclaimedBytes = totalReclaimed;
        return RS_STATUS_V1_OK;
    } catch (...) {
        return RS_STATUS_V1_RUNTIME_ERROR;
    }
}

RsStatusV1 apiHeapCreateInDomain(
    RsDomainHandleV2 domain,
    size_t initialThresholdBytes,
    size_t maxHeapBytes,
    RsHeapHandleV1* outHeap) noexcept {
    if (!domain || !outHeap) return RS_STATUS_V1_INVALID_ARGUMENT;
    try {
        HeapConfig cfg{};
        cfg.valuePoolDomain = domain->valuePoolDomain;
        if (initialThresholdBytes > 0) cfg.initialCollectionThresholdBytes = initialThresholdBytes;
        if (maxHeapBytes > 0) cfg.maximumHeapBytes = maxHeapBytes;
        auto managed = std::make_shared<ManagedHeap>(cfg);
        auto* wrapper = new (std::nothrow) RsHeapOpaque{managed};
        if (!wrapper) return RS_STATUS_V1_OUT_OF_MEMORY;
        {
            std::lock_guard<std::mutex> lock(domain->mutex);
            domain->heaps.push_back(managed);
        }
        *outHeap = wrapper;
        return RS_STATUS_V1_OK;
    } catch (...) {
        return RS_STATUS_V1_OUT_OF_MEMORY;
    }
}

RsStatusV1 apiHeapTrimExcess(
    RsHeapHandleV1 heap,
    uint64_t* outTrimmedBytes) noexcept {
    if (!heap || !heap->heap) return RS_STATUS_V1_INVALID_ARGUMENT;
    try {
        const std::size_t trimmed = heap->heap->trimExcessCapacity();
        if (outTrimmedBytes) {
            *outTrimmedBytes = static_cast<uint64_t>(trimmed);
        }
        return RS_STATUS_V1_OK;
    } catch (...) {
        return RS_STATUS_V1_RUNTIME_ERROR;
    }
}

RsStatusV1 apiVmGetLastError(
    RsVmHandleV1 vm,
    RsErrorInfoV2* outErrorInfo) noexcept {
    if (!vm || !outErrorInfo) return RS_STATUS_V1_INVALID_ARGUMENT;
    outErrorInfo->status = vm->lastStatus;
    outErrorInfo->error_code = vm->lastErrorCode;
    outErrorInfo->frame_count = static_cast<uint32_t>(vm->lastFrames.size());
    outErrorInfo->reserved = 0u;
    outErrorInfo->utf8_message = vm->lastErrorMessage.c_str();
    outErrorInfo->exception_type_utf8 = vm->lastExceptionType.c_str();
    return RS_STATUS_V1_OK;
}

RsStatusV1 apiVmGetLastErrorFrame(
    RsVmHandleV1 vm,
    uint32_t frameIndex,
    RsErrorFrameV2* outFrame) noexcept {
    if (!vm || !outFrame || frameIndex >= vm->lastFrames.size()) {
        return RS_STATUS_V1_INVALID_ARGUMENT;
    }
    const auto& cf = vm->lastFrames[frameIndex];
    outFrame->function_utf8 = cf.function.c_str();
    outFrame->module_utf8 = cf.moduleName.c_str();
    outFrame->source_file_utf8 = cf.sourceFile.c_str();
    outFrame->line = cf.line;
    outFrame->column = cf.column;
    return RS_STATUS_V1_OK;
}

void apiVmClearLastError(RsVmHandleV1 vm) noexcept {
    clearVmLastErrorInternal(vm);
}

RsStatusV1 apiVmSetInstructionBudget(
    RsVmHandleV1 vm,
    uint64_t instructionBudget) noexcept {
    if (!vm) return RS_STATUS_V1_INVALID_ARGUMENT;
    vm->options.limits.instructionBudget =
        instructionBudget == 0
            ? Limits{}.instructionBudget
            : instructionBudget;
    return RS_STATUS_V1_OK;
}

RsStatusV1 apiVmGetLastExecutedInstructions(
    RsVmHandleV1 vm,
    uint64_t* outCount) noexcept {
    if (!vm || !outCount) return RS_STATUS_V1_INVALID_ARGUMENT;
    *outCount = vm->lastExecutedInstructions;
    return RS_STATUS_V1_OK;
}

RsStatusV1 apiVmSetAwaitScheduler(
    RsVmHandleV1 vm,
    const RsAwaitSchedulerCallbacksV2* callbacks) noexcept {
    if (!vm) return RS_STATUS_V1_INVALID_ARGUMENT;
    try {
        revokeAllPendingCallbacksForVm(vm->instanceId);
        vm->options.awaitScheduler = nullptr;
        vm->awaitSchedulerAdapter.reset();
        if (!callbacks) {
            return RS_STATUS_V1_OK;
        }
        if (callbacks->struct_size != sizeof(RsAwaitSchedulerCallbacksV2) ||
            !callbacks->handles_value ||
            !callbacks->is_completed ||
            !callbacks->try_get_result ||
            !callbacks->on_suspend) {
            return RS_STATUS_V1_INVALID_ARGUMENT;
        }
        if (vm->interpreter && vm->interpreter->asyncRuntime() == nullptr) {
            vm->interpreter->setAsyncRuntime(
                std::make_shared<AsyncRuntimeContext>(vm->heap.get()));
        }
        vm->awaitSchedulerAdapter = std::make_unique<CAwaitSchedulerAdapter>(
            vm->instanceId, vm->heap.get(), *callbacks);
        vm->options.awaitScheduler = vm->awaitSchedulerAdapter.get();
        return RS_STATUS_V1_OK;
    } catch (...) {
        return RS_STATUS_V1_OUT_OF_MEMORY;
    }
}

RsStatusV1 apiProgramTypeCount(
    RsProgramHandleV1 program,
    uint32_t* outCount) noexcept {
    if (!program || !program->program || !outCount) {
        return RS_STATUS_V1_INVALID_ARGUMENT;
    }
    try {
        ensureProgramIntrospection(program);
        *outCount = static_cast<uint32_t>(program->cachedTypes.size());
        return RS_STATUS_V1_OK;
    } catch (...) {
        return RS_STATUS_V1_OUT_OF_MEMORY;
    }
}

RsStatusV1 apiProgramGetTypeInfo(
    RsProgramHandleV1 program,
    uint32_t typeIndex,
    RsTypeInfoV2* outInfo) noexcept {
    if (!program || !program->program || !outInfo) {
        return RS_STATUS_V1_INVALID_ARGUMENT;
    }
    try {
        ensureProgramIntrospection(program);
        if (typeIndex >= program->cachedTypes.size()) {
            return RS_STATUS_V1_INVALID_ARGUMENT;
        }
        const auto& tm = program->cachedTypes[typeIndex];
        outInfo->type_id = tm.typeId;
        outInfo->base_type_id = tm.baseTypeId;
        outInfo->name_utf8 = tm.name.c_str();
        outInfo->module_utf8 = tm.moduleName.c_str();
        outInfo->kind = tm.kind;
        outInfo->is_blittable = tm.isBlittable ? 1u : 0u;
        outInfo->reserved = 0u;
        outInfo->field_count = static_cast<uint32_t>(tm.fields.size());
        outInfo->native_byte_size = tm.nativeByteSize;
        outInfo->reserved2 = 0u;
        return RS_STATUS_V1_OK;
    } catch (...) {
        return RS_STATUS_V1_OUT_OF_MEMORY;
    }
}

RsStatusV1 apiProgramFindType(
    RsProgramHandleV1 program,
    const char* qualifiedName,
    uint64_t* outTypeId) noexcept {
    if (!program || !program->program || !qualifiedName || !outTypeId) {
        return RS_STATUS_V1_INVALID_ARGUMENT;
    }
    try {
        ensureProgramIntrospection(program);
        const auto it = program->typeNameToId.find(std::string(qualifiedName));
        if (it == program->typeNameToId.end()) {
            return RS_STATUS_V1_INVALID_ARGUMENT;
        }
        *outTypeId = it->second;
        return RS_STATUS_V1_OK;
    } catch (...) {
        return RS_STATUS_V1_OUT_OF_MEMORY;
    }
}

RsStatusV1 apiProgramGetFieldInfo(
    RsProgramHandleV1 program,
    uint64_t typeId,
    uint32_t fieldIndex,
    RsFieldInfoV2* outInfo) noexcept {
    if (!program || !program->program || !outInfo) {
        return RS_STATUS_V1_INVALID_ARGUMENT;
    }
    try {
        ensureProgramIntrospection(program);
        const auto it = program->typeIdToIndex.find(typeId);
        if (it == program->typeIdToIndex.end()) {
            return RS_STATUS_V1_INVALID_ARGUMENT;
        }
        const auto& tm = program->cachedTypes[it->second];
        if (fieldIndex >= tm.fields.size()) {
            return RS_STATUS_V1_INVALID_ARGUMENT;
        }
        const auto& fm = tm.fields[fieldIndex];
        outInfo->name_utf8 = fm.name.c_str();
        outInfo->former_names_utf8 = fm.formerNamesCsv.c_str();
        outInfo->type_id = fm.typeId;
        outInfo->primitive_kind = fm.primitiveKind;
        outInfo->has_field_tag = fm.hasFieldTag ? 1u : 0u;
        outInfo->is_synthetic = fm.isSynthetic ? 1u : 0u;
        outInfo->reserved = 0u;
        outInfo->field_tag = fm.fieldTag;
        outInfo->alias_count = static_cast<uint32_t>(fm.formerNames.size());
        outInfo->byte_offset = fm.byteOffset;
        outInfo->field_index = fm.fieldIndex;
        outInfo->reserved2 = 0u;
        return RS_STATUS_V1_OK;
    } catch (...) {
        return RS_STATUS_V1_OUT_OF_MEMORY;
    }
}

RsStatusV1 apiProgramFindFieldIndex(
    RsProgramHandleV1 program,
    uint64_t typeId,
    const char* fieldName,
    uint32_t* outFieldIndex) noexcept {
    if (!program || !program->program || !fieldName || !outFieldIndex) {
        return RS_STATUS_V1_INVALID_ARGUMENT;
    }
    try {
        ensureProgramIntrospection(program);
        const auto it = program->typeIdToIndex.find(typeId);
        if (it == program->typeIdToIndex.end()) {
            return RS_STATUS_V1_INVALID_ARGUMENT;
        }
        const auto& tm = program->cachedTypes[it->second];
        for (const auto& fm : tm.fields) {
            if (fm.name == fieldName) {
                *outFieldIndex = fm.fieldIndex;
                return RS_STATUS_V1_OK;
            }
        }
        return RS_STATUS_V1_INVALID_ARGUMENT;
    } catch (...) {
        return RS_STATUS_V1_OUT_OF_MEMORY;
    }
}

RsStatusV1 apiProgramFindFunctionId(
    RsProgramHandleV1 program,
    const char* qualifiedFunctionName,
    uint64_t* outFunctionId) noexcept {
    if (!program || !program->program || !qualifiedFunctionName || !outFunctionId) {
        return RS_STATUS_V1_INVALID_ARGUMENT;
    }
    try {
        const auto found = program->program->findFunction(std::string(qualifiedFunctionName));
        if (!found) return RS_STATUS_V1_INVALID_ARGUMENT;
        *outFunctionId = static_cast<uint64_t>(*found);
        return RS_STATUS_V1_OK;
    } catch (...) {
        return RS_STATUS_V1_RUNTIME_ERROR;
    }
}

RsStatusV1 apiProgramExportAbiJson(
    RsProgramHandleV1 program,
    char* outJsonBuf,
    size_t bufCapacityBytes,
    size_t* outRequiredBytes) noexcept {
    if (!program || !program->program || !outRequiredBytes) {
        return RS_STATUS_V1_INVALID_ARGUMENT;
    }
    try {
        const std::string& json = ensureProgramAbiJson(program);
        const size_t required = json.size() + 1u;
        *outRequiredBytes = required;
        if (!outJsonBuf) return RS_STATUS_V1_OK;
        if (bufCapacityBytes < required) return RS_STATUS_V1_BUFFER_TOO_SMALL;
        std::memcpy(outJsonBuf, json.data(), required);
        return RS_STATUS_V1_OK;
    } catch (...) {
        return RS_STATUS_V1_OUT_OF_MEMORY;
    }
}

RsStatusV1 apiVmInvokeIdV2(
    RsVmHandleV1 vm,
    uint64_t functionId,
    const RsValueV1* arguments,
    uint32_t argumentCount,
    RsValueV1* outResult) noexcept {
    if (!vm || !vm->interpreter || !vm->program ||
        (argumentCount > 0 && !arguments)) {
        if (vm) {
            recordVmHostError(
                vm,
                RS_STATUS_V1_INVALID_ARGUMENT,
                RS_ERROR_CODE_V2_INVALID_ARGUMENTS,
                "invalid arguments to vm_invoke_id_v2");
        }
        return RS_STATUS_V1_INVALID_ARGUMENT;
    }
    try {
        ScopedValuePoolDomain domainScope(
            vm->heap ? vm->heap->valuePoolDomain() : nullptr);
        const auto sid = static_cast<semantic::SymbolId>(functionId);
        const auto* loc = vm->program->resolveFunction(sid);
        if (!loc || !loc->function) {
            recordVmHostError(
                vm,
                RS_STATUS_V1_INVALID_ARGUMENT,
                RS_ERROR_CODE_V2_FUNCTION_NOT_FOUND,
                "target function id was not found in program");
            return RS_STATUS_V1_INVALID_ARGUMENT;
        }
        std::vector<Value> cppArgs(argumentCount);
        if (coerceInvocationArgs(
                vm->heap.get(),
                loc->function,
                arguments,
                argumentCount,
                0u,
                cppArgs) !=
            RS_STATUS_V1_OK) {
            recordVmHostError(
                vm,
                RS_STATUS_V1_INVALID_ARGUMENT,
                RS_ERROR_CODE_V2_INVALID_ARGUMENTS,
                "failed to coerce argument to function parameter type");
            return RS_STATUS_V1_INVALID_ARGUMENT;
        }
        const auto res = vm->interpreter->invoke(sid, cppArgs, vm->options);
        return invokeWithOptionsAndRecord(vm, res, outResult);
    } catch (...) {
        recordVmHostError(
            vm,
            RS_STATUS_V1_RUNTIME_ERROR,
            RS_ERROR_CODE_V2_INVALID_PROGRAM,
            "unexpected C++ exception in vm_invoke_id_v2");
        return RS_STATUS_V1_RUNTIME_ERROR;
    }
}

RsStatusV1 apiVmInvokeMethodV2(
    RsVmHandleV1 vm,
    RsObjectRefV1 receiver,
    const char* methodNameUtf8,
    const RsValueV1* arguments,
    uint32_t argumentCount,
    RsValueV1* outResult) noexcept {
    if (!vm || !vm->interpreter || !vm->program || !vm->heap ||
        !methodNameUtf8 || (argumentCount > 0 && !arguments)) {
        if (vm) {
            recordVmHostError(
                vm,
                RS_STATUS_V1_INVALID_ARGUMENT,
                RS_ERROR_CODE_V2_INVALID_ARGUMENTS,
                "invalid arguments to vm_invoke_method_v2");
        }
        return RS_STATUS_V1_INVALID_ARGUMENT;
    }
    try {
        ScopedValuePoolDomain domainScope(
            vm->heap ? vm->heap->valuePoolDomain() : nullptr);
        const ObjectRef recvRef = fromAbiObjectRef(receiver);
        const auto dynTid = vm->heap->objectTypeId(recvRef);
        if (!dynTid) {
            recordVmHostError(
                vm,
                RS_STATUS_V1_STALE_REFERENCE,
                RS_ERROR_CODE_V2_INVALID_OBJECT_REFERENCE,
                "receiver object reference is stale or invalid");
            return RS_STATUS_V1_STALE_REFERENCE;
        }
        const semantic::TypeSymbol* actualType = vm->program->resolveType(*dynTid);
        semantic::SymbolId targetFnId = 0;
        semantic::SymbolId cursorTid = *dynTid;
        std::size_t depthGuard = 0;
        while (cursorTid != 0 && depthGuard < 64 && targetFnId == 0) {
            ++depthGuard;
            const auto* ts = vm->program->resolveType(cursorTid);
            if (!ts) break;
            for (const auto& m : ts->methods) {
                const std::size_t visibleOffset =
                    (m.method && !m.staticMethod) ? 1u : 0u;
                const std::size_t visibleArity =
                    m.parameters.size() >= visibleOffset
                        ? m.parameters.size() - visibleOffset
                        : m.parameters.size();
                if (!m.staticMethod &&
                    m.name == methodNameUtf8 &&
                    visibleArity == argumentCount) {
                    if (m.virtualSlot != std::numeric_limits<std::uint32_t>::max() &&
                        actualType &&
                        m.virtualSlot < actualType->virtualDispatchTable.size() &&
                        actualType->virtualDispatchTable[m.virtualSlot] != 0) {
                        targetFnId = actualType->virtualDispatchTable[m.virtualSlot];
                    } else {
                        targetFnId = m.id;
                    }
                    break;
                }
            }
            if (targetFnId == 0) {
                const std::string encodedName = ts->name + "." + std::string(methodNameUtf8);
                for (const auto& mod : vm->program->modules()) {
                    for (const auto& fn : mod.functions) {
                        if (fn.name == encodedName &&
                            fn.parameterTypes.size() == static_cast<std::size_t>(argumentCount) + 1u &&
                            !fn.parameterTypeIds.empty() &&
                            fn.parameterTypeIds.front() == ts->id) {
                            targetFnId = fn.symbolId;
                            break;
                        }
                    }
                    if (targetFnId != 0) break;
                }
            }
            if (targetFnId == 0) {
                const std::string qualifiedMethod =
                    semantic::canonicalTypeName(*ts) + "." + std::string(methodNameUtf8);
                if (const auto found = vm->program->findFunction(qualifiedMethod)) {
                    const auto* candidate = vm->program->resolveFunction(*found);
                    if (candidate && candidate->function &&
                        candidate->function->parameterTypes.size() ==
                            static_cast<std::size_t>(argumentCount) + 1u) {
                        targetFnId = *found;
                        break;
                    }
                }
            }
            cursorTid = ts->baseTypeId != 0
                ? ts->baseTypeId
                : (!ts->baseTypeName.empty()
                       ? semantic::stableTypeId(ts->baseTypeName)
                       : 0);
        }
        if (targetFnId == 0) {
            recordVmHostError(
                vm,
                RS_STATUS_V1_INVALID_ARGUMENT,
                RS_ERROR_CODE_V2_FUNCTION_NOT_FOUND,
                "method not found on receiver type");
            return RS_STATUS_V1_INVALID_ARGUMENT;
        }
        const auto* loc = vm->program->resolveFunction(targetFnId);
        std::vector<Value> cppArgs(static_cast<std::size_t>(argumentCount) + 1u);
        cppArgs[0] = Value{recvRef};
        if (coerceInvocationArgs(
                vm->heap.get(),
                loc ? loc->function : nullptr,
                arguments,
                argumentCount,
                1u,
                cppArgs) !=
            RS_STATUS_V1_OK) {
            recordVmHostError(
                vm,
                RS_STATUS_V1_INVALID_ARGUMENT,
                RS_ERROR_CODE_V2_INVALID_ARGUMENTS,
                "failed to coerce argument to method parameter type");
            return RS_STATUS_V1_INVALID_ARGUMENT;
        }
        const auto res = vm->interpreter->invoke(targetFnId, cppArgs, vm->options);
        return invokeWithOptionsAndRecord(vm, res, outResult);
    } catch (...) {
        recordVmHostError(
            vm,
            RS_STATUS_V1_RUNTIME_ERROR,
            RS_ERROR_CODE_V2_INVALID_PROGRAM,
            "unexpected C++ exception in vm_invoke_method_v2");
        return RS_STATUS_V1_RUNTIME_ERROR;
    }
}

RsStatusV1 apiHeapAllocRecord(
    RsHeapHandleV1 heap,
    RsProgramHandleV1 program,
    uint64_t typeId,
    uint32_t untypedFieldCount,
    RsObjectRefV1* outRef) noexcept {
    if (!heap || !heap->heap || !outRef) return RS_STATUS_V1_INVALID_ARGUMENT;
    try {
        RuntimeError err;
        std::optional<ObjectRef> allocated;
        if (typeId == 0) {
            allocated = heap->heap->allocateRecord(untypedFieldCount, &err);
        } else {
            if (!program || !program->program) return RS_STATUS_V1_INVALID_ARGUMENT;
            const auto* ts = program->program->resolveType(
                static_cast<semantic::SymbolId>(typeId));
            if (!ts || ts->kind != semantic::TypeKind::Class) {
                return RS_STATUS_V1_INVALID_ARGUMENT;
            }
            std::vector<Value> fields;
            std::vector<std::size_t> refFields;
            fields.reserve(ts->fields.size());
            for (std::size_t fi = 0; fi < ts->fields.size(); ++fi) {
                bool isRef = false;
                fields.push_back(makeDefaultFieldValue(
                    ts->fields[fi].type,
                    exactFieldTypeId(ts->fields[fi].type, ts->fields[fi].typeName),
                    &isRef));
                if (isRef) refFields.push_back(fi);
            }
            allocated = heap->heap->allocateObject(
                ts->id, std::move(fields), std::move(refFields), &err);
        }
        if (!allocated) return mapRuntimeError(err.code);
        *outRef = toAbiObjectRef(*allocated);
        return RS_STATUS_V1_OK;
    } catch (...) {
        return RS_STATUS_V1_OUT_OF_MEMORY;
    }
}

RsStatusV1 apiHeapAllocArray(
    RsHeapHandleV1 heap,
    RsPrimitiveKindV2 elementPrimitive,
    uint64_t elementTypeId,
    uint32_t length,
    RsObjectRefV1* outArrayRef) noexcept {
    if (!heap || !heap->heap || !outArrayRef) return RS_STATUS_V1_INVALID_ARGUMENT;
    try {
        RuntimeError err;
        std::optional<ObjectRef> allocated;
        const semantic::PrimitiveType elemPrim = mapPrimitiveKindFromV2(elementPrimitive);
        if (elemPrim == semantic::PrimitiveType::Error ||
            elemPrim == semantic::PrimitiveType::Void) {
            allocated = heap->heap->allocateArray(length, Value{}, &err);
        } else {
            const bool needsExact = semantic::isExactType(elemPrim);
            if (needsExact && elementTypeId == 0) {
                allocated = heap->heap->allocateArray(length, Value{}, &err);
            } else {
                const semantic::SymbolId tid = needsExact
                    ? static_cast<semantic::SymbolId>(elementTypeId)
                    : 0u;
                const Value initVal = makeDefaultFieldValue(elemPrim, tid, nullptr);
                allocated = heap->heap->allocateTypedArray(
                    0, elemPrim, tid, length, initVal, &err);
            }
        }
        if (!allocated) return mapRuntimeError(err.code);
        *outArrayRef = toAbiObjectRef(*allocated);
        return RS_STATUS_V1_OK;
    } catch (...) {
        return RS_STATUS_V1_OUT_OF_MEMORY;
    }
}

RsStatusV1 apiHeapArrayLength(
    RsHeapHandleV1 heap,
    RsObjectRefV1 arrayRef,
    uint32_t* outLength) noexcept {
    if (!heap || !heap->heap || !outLength) return RS_STATUS_V1_INVALID_ARGUMENT;
    const auto ref = fromAbiObjectRef(arrayRef);
    const auto len = heap->heap->arrayLength(ref);
    if (!len) {
        return heap->heap->isAlive(ref)
            ? RS_STATUS_V1_INVALID_ARGUMENT
            : RS_STATUS_V1_STALE_REFERENCE;
    }
    *outLength = static_cast<uint32_t>(*len);
    return RS_STATUS_V1_OK;
}

RsStatusV1 apiHeapArrayGetV2(
    RsHeapHandleV1 heap,
    RsObjectRefV1 arrayRef,
    uint32_t index,
    RsValueV1* outElement) noexcept {
    if (!heap || !heap->heap || !outElement) return RS_STATUS_V1_INVALID_ARGUMENT;
    try {
        ScopedValuePoolDomain domainScope(heap->heap->valuePoolDomain());
        const auto ref = fromAbiObjectRef(arrayRef);
        const auto len = heap->heap->arrayLength(ref);
        if (!len) {
            return heap->heap->isAlive(ref)
                ? RS_STATUS_V1_INVALID_ARGUMENT
                : RS_STATUS_V1_STALE_REFERENCE;
        }
        if (index >= *len) return RS_STATUS_V1_INVALID_ARGUMENT;
        const auto elem = heap->heap->arrayGet(ref, index);
        if (!elem) return RS_STATUS_V1_INVALID_ARGUMENT;
        return marshalValueToAbi(heap->heap.get(), *elem, outElement);
    } catch (...) {
        return RS_STATUS_V1_RUNTIME_ERROR;
    }
}

RsStatusV1 apiHeapArraySetV2(
    RsHeapHandleV1 heap,
    RsObjectRefV1 arrayRef,
    uint32_t index,
    const RsValueV1* value) noexcept {
    if (!heap || !heap->heap || !value) return RS_STATUS_V1_INVALID_ARGUMENT;
    try {
        ScopedValuePoolDomain domainScope(heap->heap->valuePoolDomain());
        const auto ref = fromAbiObjectRef(arrayRef);
        const auto len = heap->heap->arrayLength(ref);
        if (!len) {
            return heap->heap->isAlive(ref)
                ? RS_STATUS_V1_INVALID_ARGUMENT
                : RS_STATUS_V1_STALE_REFERENCE;
        }
        if (index >= *len) return RS_STATUS_V1_INVALID_ARGUMENT;

        Value cppVal{};
        const auto elemPrim = heap->heap->arrayElementType(ref);
        const auto elemTid = heap->heap->arrayElementTypeId(ref);
        if (elemPrim && *elemPrim != semantic::PrimitiveType::Error) {
            if (!coerceAbiValueForType(
                    *value, *elemPrim, elemTid.value_or(0), heap->heap.get(), cppVal)) {
                return RS_STATUS_V1_INVALID_ARGUMENT;
            }
        } else if (!marshalValueFromAbi(*value, cppVal)) {
            return RS_STATUS_V1_INVALID_ARGUMENT;
        }
        RuntimeError err;
        if (!heap->heap->arraySet(ref, index, std::move(cppVal), &err)) {
            return mapRuntimeError(err.code);
        }
        return RS_STATUS_V1_OK;
    } catch (...) {
        return RS_STATUS_V1_RUNTIME_ERROR;
    }
}

RsStatusV1 resolveBlittableSchemaOnHeap(
    RsHeapOpaque* heapWrapper,
    semantic::SymbolId structTypeId,
    RsHeapOpaque::CachedBlittableSchema& outSchema) {
    if (!heapWrapper || !heapWrapper->heap || structTypeId == 0 ||
        !isBlittableStructType(structTypeId)) {
        return RS_STATUS_V1_INVALID_ARGUMENT;
    }
    {
        std::lock_guard<std::mutex> lock(heapWrapper->v2Mutex);
        const auto it = heapWrapper->blittableCache.find(structTypeId);
        if (it != heapWrapper->blittableCache.end()) {
            outSchema = it->second;
            return RS_STATUS_V1_OK;
        }
    }
    const auto defVal = makeDefaultBlittableStructValue(structTypeId);
    if (!defVal) return RS_STATUS_V1_INVALID_ARGUMENT;
    RuntimeError err;
    const auto tmpArr = heapWrapper->heap->allocateTypedArray(
        0, semantic::PrimitiveType::Struct, structTypeId, 1u, *defVal, &err);
    if (!tmpArr) return mapRuntimeError(err.code);
    PackedArrayFastView view{};
    if (!heapWrapper->heap->tryBindPackedArrayFastView(*tmpArr, view) ||
        view.encoding != PackedElemEncoding::BlittableStruct ||
        view.strideBytes == 0 ||
        view.blittableSchemaId == 0) {
        return RS_STATUS_V1_INVALID_ARGUMENT;
    }
    outSchema = {view.strideBytes, view.blittableSchemaId};
    std::lock_guard<std::mutex> lock(heapWrapper->v2Mutex);
    heapWrapper->blittableCache.emplace(structTypeId, outSchema);
    return RS_STATUS_V1_OK;
}

RsStatusV1 apiHeapPackBlittableStruct(
    RsHeapHandleV1 heap,
    RsProgramHandleV1 program,
    uint64_t structTypeId,
    const RsValueV1* fieldValues,
    uint32_t fieldCount,
    void* outBuf,
    size_t bufCapacityBytes,
    size_t* outRequiredBytes) noexcept {
    if (!heap || !heap->heap || !outRequiredBytes) {
        return RS_STATUS_V1_INVALID_ARGUMENT;
    }
    try {
        ScopedValuePoolDomain domainScope(heap->heap->valuePoolDomain());
        const auto sid = static_cast<semantic::SymbolId>(structTypeId);
        RsHeapOpaque::CachedBlittableSchema schema{};
        const RsStatusV1 schemaSt = resolveBlittableSchemaOnHeap(heap, sid, schema);
        if (schemaSt != RS_STATUS_V1_OK) return schemaSt;

        *outRequiredBytes = static_cast<size_t>(schema.strideBytes);
        if (!outBuf) return RS_STATUS_V1_OK;
        if (bufCapacityBytes < schema.strideBytes) return RS_STATUS_V1_BUFFER_TOO_SMALL;
        if (fieldCount > 0 && !fieldValues) return RS_STATUS_V1_INVALID_ARGUMENT;

        auto structValOpt = makeDefaultBlittableStructValue(sid);
        if (!structValOpt) return RS_STATUS_V1_INVALID_ARGUMENT;
        Value structVal = *structValOpt;

        const semantic::TypeSymbol* ts =
            (program && program->program) ? program->program->resolveType(sid) : nullptr;
        const FlatStructSchema* flatSchema = findFlatStructSchemaByTypeId(sid);
        std::vector<const semantic::FieldSymbol*> instFields;
        if (ts) {
            instFields.reserve(ts->fields.size());
            for (const auto& f : ts->fields) {
                if (!f.staticField && !f.constField) instFields.push_back(&f);
            }
        }
        const std::size_t expectedFieldCount = ts
            ? instFields.size()
            : (flatSchema ? flatSchema->directFields.size() : 0);
        if (expectedFieldCount != fieldCount) {
            return RS_STATUS_V1_INVALID_ARGUMENT;
        }
        for (uint32_t fi = 0; fi < fieldCount; ++fi) {
            const semantic::PrimitiveType fType = ts
                ? instFields[fi]->type
                : flatSchema->directFields[fi].type;
            const semantic::SymbolId fTypeId = ts
                ? exactFieldTypeId(instFields[fi]->type, instFields[fi]->typeName)
                : flatSchema->directFields[fi].typeId;
            Value coerced{};
            if (!coerceAbiValueForType(
                    fieldValues[fi], fType, fTypeId, heap->heap.get(), coerced)) {
                return RS_STATUS_V1_INVALID_ARGUMENT;
            }
            const auto updated = storeInlineStructField(structVal, fi, coerced);
            if (!updated) return RS_STATUS_V1_INVALID_ARGUMENT;
            structVal = *updated;
        }

        bool needsDemote = false;
        if (!writePackedStructElement(
                outBuf,
                0,
                PackedElemEncoding::BlittableStruct,
                sid,
                schema.blittableSchemaId,
                structVal,
                needsDemote)) {
            return RS_STATUS_V1_INVALID_ARGUMENT;
        }
        return RS_STATUS_V1_OK;
    } catch (...) {
        return RS_STATUS_V1_RUNTIME_ERROR;
    }
}

RsStatusV1 apiHeapUnpackBlittableStruct(
    RsHeapHandleV1 heap,
    RsProgramHandleV1 program,
    uint64_t structTypeId,
    const void* inBuf,
    size_t bufSizeBytes,
    RsValueV1* outFieldValues,
    uint32_t maxFields,
    uint32_t* outFieldCount) noexcept {
    if (!heap || !heap->heap || !inBuf || !outFieldCount) {
        return RS_STATUS_V1_INVALID_ARGUMENT;
    }
    try {
        ScopedValuePoolDomain domainScope(heap->heap->valuePoolDomain());
        const auto sid = static_cast<semantic::SymbolId>(structTypeId);
        RsHeapOpaque::CachedBlittableSchema schema{};
        const RsStatusV1 schemaSt = resolveBlittableSchemaOnHeap(heap, sid, schema);
        if (schemaSt != RS_STATUS_V1_OK) return schemaSt;
        if (bufSizeBytes < schema.strideBytes) return RS_STATUS_V1_BUFFER_TOO_SMALL;

        const semantic::TypeSymbol* ts =
            (program && program->program) ? program->program->resolveType(sid) : nullptr;
        const FlatStructSchema* flatSchema = findFlatStructSchemaByTypeId(sid);
        std::size_t instFieldCount = 0;
        if (ts) {
            for (const auto& f : ts->fields) {
                if (!f.staticField && !f.constField) ++instFieldCount;
            }
        }
        const uint32_t nFields = ts
            ? static_cast<uint32_t>(instFieldCount)
            : (flatSchema ? static_cast<uint32_t>(flatSchema->directFields.size()) : 0u);
        *outFieldCount = nFields;
        if (!outFieldValues) return RS_STATUS_V1_OK;
        if (maxFields < nFields) return RS_STATUS_V1_BUFFER_TOO_SMALL;

        const Value unpacked = readPackedStructElement(
            inBuf,
            0,
            PackedElemEncoding::BlittableStruct,
            sid,
            schema.blittableSchemaId);
        if (unpacked.rawTag() != ValueTag::Struct) {
            return RS_STATUS_V1_RUNTIME_ERROR;
        }
        for (uint32_t fi = 0; fi < nFields; ++fi) {
            const auto fOpt = readStructFieldForAbi(unpacked, fi);
            if (!fOpt) return RS_STATUS_V1_RUNTIME_ERROR;
            const RsStatusV1 mst = marshalValueToAbi(
                heap->heap.get(), *fOpt, &outFieldValues[fi]);
            if (mst != RS_STATUS_V1_OK) return mst;
        }
        return RS_STATUS_V1_OK;
    } catch (...) {
        return RS_STATUS_V1_RUNTIME_ERROR;
    }
}

RsStatusV1 apiHeapWrapNativeBuffer(
    RsHeapHandleV1 heap,
    void* dataPtr,
    size_t byteLength,
    uint8_t readOnly,
    RsObjectRefV1* outBufferRef) noexcept {
    if (!heap || !heap->heap || !outBufferRef || (byteLength > 0 && !dataPtr)) {
        return RS_STATUS_V1_INVALID_ARGUMENT;
    }
    try {
        NativeBufferViewDesc desc{};
        desc.data = dataPtr;
        desc.elementCount = byteLength;
        desc.elementType = semantic::PrimitiveType::Byte;
        desc.elementTypeId = 0;
        desc.strideBytes = 1u;
        desc.readOnly = (readOnly != 0);
        RuntimeError err;
        const auto allocated = heap->heap->allocateNativeBufferView(
            semantic::stableTypeId("byte[]"), desc, &err);
        if (!allocated) return mapRuntimeError(err.code);
        *outBufferRef = toAbiObjectRef(*allocated);
        return RS_STATUS_V1_OK;
    } catch (...) {
        return RS_STATUS_V1_OUT_OF_MEMORY;
    }
}

RsStatusV1 apiHeapRevokeNativeBuffer(
    RsHeapHandleV1 heap,
    RsObjectRefV1 bufferRef) noexcept {
    if (!heap || !heap->heap) return RS_STATUS_V1_INVALID_ARGUMENT;
    try {
        return heap->heap->revokeNativeBufferView(fromAbiObjectRef(bufferRef))
            ? RS_STATUS_V1_OK
            : RS_STATUS_V1_STALE_REFERENCE;
    } catch (...) {
        return RS_STATUS_V1_RUNTIME_ERROR;
    }
}

const RsHostApiV1 kHostApiV1 = {
    sizeof(RsHostApiV1),
    RS_HOST_ABI_MAJOR_V1,
    RS_HOST_ABI_MINOR_V1,
    &apiHeapCreate,
    &apiHeapDestroy,
    &apiHeapId,
    &apiHeapCollect,
    &apiHeapCollectBudgetNs,
    &apiHeapPinObject,
    &apiHeapUnpinRoot,
    &apiHeapIsAlive,
    &apiHeapSetSweepCallback,
    &apiWeakFromObject,
    &apiWeakTryResolve,
    &apiHeapEnableDirtyTracking,
    &apiHeapMarkDirtyBit,
    &apiHeapPeekDirtyMask,
    &apiHeapConsumeDirtyMask,
    &apiHeapReadFieldWithPolicy,
    &apiHeapWriteFieldWithPolicy,
    &apiStringAllocUtf8,
    &apiStringAllocUtf16,
    &apiStringReadUtf8,
    &apiStringReadUtf16,
    &apiProgramLoadRsbc,
    &apiProgramDestroy,
    &apiVmCreate,
    &apiVmDestroy,
    &apiVmBindHostCallback,
    &apiVmInvoke,
};

const RsHostApiV2 kHostApiV2 = {
    kHostApiV1,
    sizeof(RsHostApiV2),
    RS_HOST_ABI_MAJOR_V2,
    RS_HOST_ABI_MINOR_V2,
    0u,
    &apiDomainCreate,
    &apiDomainDestroy,
    &apiDomainCollect,
    &apiHeapCreateInDomain,
    &apiHeapTrimExcess,
    &apiVmGetLastError,
    &apiVmGetLastErrorFrame,
    &apiVmClearLastError,
    &apiVmSetInstructionBudget,
    &apiVmGetLastExecutedInstructions,
    &apiVmSetAwaitScheduler,
    &apiProgramTypeCount,
    &apiProgramGetTypeInfo,
    &apiProgramFindType,
    &apiProgramGetFieldInfo,
    &apiProgramFindFieldIndex,
    &apiProgramFindFunctionId,
    &apiProgramExportAbiJson,
    &apiVmInvokeIdV2,
    &apiVmInvokeMethodV2,
    &apiHeapAllocRecord,
    &apiHeapAllocArray,
    &apiHeapArrayLength,
    &apiHeapArrayGetV2,
    &apiHeapArraySetV2,
    &apiHeapPackBlittableStruct,
    &apiHeapUnpackBlittableStruct,
    &apiHeapWrapNativeBuffer,
    &apiHeapRevokeNativeBuffer,
};

} // namespace

extern "C" const RsHostApiV1* rs_get_host_api_v1(void) {
    return &kHostApiV1;
}

extern "C" const RsHostApiV2* rs_get_host_api_v2(void) {
    return &kHostApiV2;
}
