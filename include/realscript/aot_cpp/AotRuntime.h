#pragma once

#include "realscript/aot_cpp/RuntimeAbi.h"
#include "realscript/runtime/Runtime.h"
#include "realscript/runtime/AsyncRuntime.h"
#include "realscript/semantic/Semantic.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <climits>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <unordered_map>
#include <vector>

#if defined(__GNUC__) || defined(__clang__)
#define RS_AOT_LIKELY(x) __builtin_expect(!!(x), 1)
#define RS_AOT_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define RS_AOT_LIKELY(x) (x)
#define RS_AOT_UNLIKELY(x) (x)
#endif

#if defined(_MSC_VER)
#define RS_AOT_RESTRICT __restrict
#elif defined(__GNUC__) || defined(__clang__)
#define RS_AOT_RESTRICT __restrict__
#else
#define RS_AOT_RESTRICT
#endif

#if defined(__clang__)
#define RS_AOT_PRAGMA_SIMD_LOOP _Pragma("clang loop vectorize(enable) interleave(enable)")
#elif defined(__GNUC__)
#define RS_AOT_PRAGMA_SIMD_LOOP _Pragma("GCC ivdep")
#elif defined(_MSC_VER)
#define RS_AOT_PRAGMA_SIMD_LOOP __pragma(loop(ivdep))
#else
#define RS_AOT_PRAGMA_SIMD_LOOP
#endif

namespace realscript::aot {

constexpr std::uint32_t RuntimeAbiMajor = 2;
constexpr std::uint32_t RuntimeAbiMinor = 0;

struct GeneratedFieldLayout {
    std::size_t byteOffset = 0;
    std::size_t byteSize = 0;
    runtime::NativeFieldKind expectedKind = runtime::NativeFieldKind::Int64;
};

struct TypedRawFieldCache {
    std::size_t byteOffset = 0;
    runtime::NativeFieldKind kind = runtime::NativeFieldKind::Int64;
};

struct TypedRawColumnCache {
    std::uint8_t* basePtr = nullptr;
    std::size_t strideBytes = 0;
    std::int64_t count = -1;
    bool readOnly = false;
    bool exactNativeLayout = false;
    std::uint8_t fieldCount = 0;
    std::array<TypedRawFieldCache, 16> fields{};
};

// P2-C: Zero-boxing P0 PackedArray fast-path caches & __restrict SIMD batch operators
struct TypedPackedInt32ArrayCache {
    std::int32_t* data = nullptr;
    std::int64_t count = -1;
};

struct TypedPackedDoubleArrayCache {
    double* data = nullptr;
    std::int64_t count = -1;
};

struct TypedStructArrayCache {
    runtime::Value* flatData = nullptr;
    std::uint8_t* packedData = nullptr;
    std::uint8_t* dirtyCards = nullptr;
    std::int64_t count = -1;
    std::uint64_t gcEpoch = 0;
    std::uint32_t blittableSchemaId = 0;
    std::uint16_t flatStride = 0;
    std::uint16_t flatSchemaId = 0;
    runtime::ObjectRef arrayRef{};
    bool containsManagedReferences = false;
    bool containsPoolValues = false;
    bool needsRememberedBarrier = false;
};

[[nodiscard]] inline bool packedBuffersDisjoint(
    const void* aBase,
    std::size_t aByteLength,
    const void* bBase,
    std::size_t bByteLength) noexcept {
    if (!aBase || !bBase) return false;
    const auto aStart = reinterpret_cast<std::uintptr_t>(aBase);
    const auto bStart = reinterpret_cast<std::uintptr_t>(bBase);
    const auto aEnd = aStart + aByteLength;
    const auto bEnd = bStart + bByteLength;
    return aEnd <= bStart || bEnd <= aStart;
}

template <typename UnaryFn>
inline void simdTransformPackedInt32(
    std::int32_t* RS_AOT_RESTRICT dst,
    const std::int32_t* RS_AOT_RESTRICT src,
    std::size_t count,
    UnaryFn&& op) noexcept {
    RS_AOT_PRAGMA_SIMD_LOOP
    for (std::size_t i = 0; i < count; ++i) {
        dst[i] = op(src[i]);
    }
}

template <typename BinaryFn>
inline void simdZipPackedInt32(
    std::int32_t* RS_AOT_RESTRICT dst,
    const std::int32_t* RS_AOT_RESTRICT lhs,
    const std::int32_t* RS_AOT_RESTRICT rhs,
    std::size_t count,
    BinaryFn&& op) noexcept {
    RS_AOT_PRAGMA_SIMD_LOOP
    for (std::size_t i = 0; i < count; ++i) {
        dst[i] = op(lhs[i], rhs[i]);
    }
}

inline void simdFmaPackedInt32(
    std::int32_t* RS_AOT_RESTRICT dst,
    const std::int32_t* RS_AOT_RESTRICT x,
    const std::int32_t* RS_AOT_RESTRICT y,
    std::size_t count,
    std::int32_t alpha) noexcept {
    const auto uAlpha = static_cast<std::uint32_t>(alpha);
    RS_AOT_PRAGMA_SIMD_LOOP
    for (std::size_t i = 0; i < count; ++i) {
        const auto prod = uAlpha * static_cast<std::uint32_t>(x[i]);
        dst[i] = static_cast<std::int32_t>(prod + static_cast<std::uint32_t>(y[i]));
    }
}

inline void simdScaleBiasPackedInt32(
    std::int32_t* RS_AOT_RESTRICT dst,
    const std::int32_t* RS_AOT_RESTRICT src,
    std::size_t count,
    std::int32_t scale,
    std::int32_t bias) noexcept {
    const auto uScale = static_cast<std::uint32_t>(scale);
    const auto uBias = static_cast<std::uint32_t>(bias);
    RS_AOT_PRAGMA_SIMD_LOOP
    for (std::size_t i = 0; i < count; ++i) {
        dst[i] = static_cast<std::int32_t>(uScale * static_cast<std::uint32_t>(src[i]) + uBias);
    }
}

template <typename UnaryFn>
inline void simdTransformPackedDouble(
    double* RS_AOT_RESTRICT dst,
    const double* RS_AOT_RESTRICT src,
    std::size_t count,
    UnaryFn&& op) noexcept {
    RS_AOT_PRAGMA_SIMD_LOOP
    for (std::size_t i = 0; i < count; ++i) {
        dst[i] = op(src[i]);
    }
}

template <typename BinaryFn>
inline void simdZipPackedDouble(
    double* RS_AOT_RESTRICT dst,
    const double* RS_AOT_RESTRICT lhs,
    const double* RS_AOT_RESTRICT rhs,
    std::size_t count,
    BinaryFn&& op) noexcept {
    RS_AOT_PRAGMA_SIMD_LOOP
    for (std::size_t i = 0; i < count; ++i) {
        dst[i] = op(lhs[i], rhs[i]);
    }
}

inline void simdFmaPackedDouble(
    double* RS_AOT_RESTRICT dst,
    const double* RS_AOT_RESTRICT x,
    const double* RS_AOT_RESTRICT y,
    std::size_t count,
    double alpha) noexcept {
    RS_AOT_PRAGMA_SIMD_LOOP
    for (std::size_t i = 0; i < count; ++i) {
        dst[i] = alpha * x[i] + y[i];
    }
}

inline void simdScaleBiasPackedDouble(
    double* RS_AOT_RESTRICT dst,
    const double* RS_AOT_RESTRICT src,
    std::size_t count,
    double scale,
    double bias) noexcept {
    RS_AOT_PRAGMA_SIMD_LOOP
    for (std::size_t i = 0; i < count; ++i) {
        dst[i] = src[i] * scale + bias;
    }
}

template <typename FieldT>
[[nodiscard]] inline FieldT readRawColumnField(
    const std::uint8_t* elemBase,
    const TypedRawFieldCache& field) noexcept {
    const std::uint8_t* ptr = elemBase + field.byteOffset;
    if constexpr (std::is_same_v<FieldT, double>) {
        if (RS_AOT_LIKELY(field.kind == runtime::NativeFieldKind::Float64)) {
            double v = 0.0;
            std::memcpy(&v, ptr, sizeof(v));
            return v;
        }
        float f = 0.0f;
        std::memcpy(&f, ptr, sizeof(f));
        return static_cast<double>(f);
    } else if constexpr (std::is_same_v<FieldT, float>) {
        if (RS_AOT_LIKELY(field.kind == runtime::NativeFieldKind::Float32)) {
            float v = 0.0f;
            std::memcpy(&v, ptr, sizeof(v));
            return v;
        }
        double d = 0.0;
        std::memcpy(&d, ptr, sizeof(d));
        return static_cast<float>(d);
    } else if constexpr (std::is_same_v<FieldT, std::int64_t>) {
        if (RS_AOT_LIKELY(field.kind == runtime::NativeFieldKind::Int64)) {
            std::int64_t v = 0;
            std::memcpy(&v, ptr, sizeof(v));
            return v;
        }
        switch (field.kind) {
        case runtime::NativeFieldKind::Int8: {
            std::int8_t v = 0;
            std::memcpy(&v, ptr, sizeof(v));
            return static_cast<std::int64_t>(v);
        }
        case runtime::NativeFieldKind::UInt8: {
            std::uint8_t v = 0;
            std::memcpy(&v, ptr, sizeof(v));
            return static_cast<std::int64_t>(v);
        }
        case runtime::NativeFieldKind::Int16: {
            std::int16_t v = 0;
            std::memcpy(&v, ptr, sizeof(v));
            return static_cast<std::int64_t>(v);
        }
        case runtime::NativeFieldKind::UInt16: {
            std::uint16_t v = 0;
            std::memcpy(&v, ptr, sizeof(v));
            return static_cast<std::int64_t>(v);
        }
        case runtime::NativeFieldKind::UInt32: {
            std::uint32_t v = 0;
            std::memcpy(&v, ptr, sizeof(v));
            return static_cast<std::int64_t>(v);
        }
        case runtime::NativeFieldKind::UInt64: {
            std::uint64_t v = 0;
            std::memcpy(&v, ptr, sizeof(v));
            return static_cast<std::int64_t>(v);
        }
        default: {
            std::int32_t i32 = 0;
            std::memcpy(&i32, ptr, sizeof(i32));
            return static_cast<std::int64_t>(i32);
        }
        }
    } else if constexpr (std::is_same_v<FieldT, runtime::Fixed16Value>) {
        std::int16_t raw = 0;
        std::memcpy(&raw, ptr, sizeof(raw));
        return runtime::Fixed16Value{raw};
    } else {
        FieldT v{};
        std::memcpy(&v, ptr, sizeof(FieldT));
        return v;
    }
}

template <typename FieldT>
inline void writeRawColumnField(
    std::uint8_t* elemBase,
    const TypedRawFieldCache& field,
    FieldT value) noexcept {
    std::uint8_t* ptr = elemBase + field.byteOffset;
    if constexpr (std::is_same_v<FieldT, double>) {
        if (RS_AOT_LIKELY(field.kind == runtime::NativeFieldKind::Float64)) {
            std::memcpy(ptr, &value, sizeof(value));
            return;
        }
        const float f = static_cast<float>(value);
        std::memcpy(ptr, &f, sizeof(f));
    } else if constexpr (std::is_same_v<FieldT, float>) {
        if (RS_AOT_LIKELY(field.kind == runtime::NativeFieldKind::Float32)) {
            std::memcpy(ptr, &value, sizeof(value));
            return;
        }
        const double d = static_cast<double>(value);
        std::memcpy(ptr, &d, sizeof(d));
    } else if constexpr (std::is_same_v<FieldT, std::int64_t>) {
        if (RS_AOT_LIKELY(field.kind == runtime::NativeFieldKind::Int64)) {
            std::memcpy(ptr, &value, sizeof(value));
            return;
        }
        switch (field.kind) {
        case runtime::NativeFieldKind::Int8: {
            const auto v = static_cast<std::int8_t>(value);
            std::memcpy(ptr, &v, sizeof(v));
            return;
        }
        case runtime::NativeFieldKind::UInt8: {
            const auto v = static_cast<std::uint8_t>(value);
            std::memcpy(ptr, &v, sizeof(v));
            return;
        }
        case runtime::NativeFieldKind::Int16: {
            const auto v = static_cast<std::int16_t>(value);
            std::memcpy(ptr, &v, sizeof(v));
            return;
        }
        case runtime::NativeFieldKind::UInt16: {
            const auto v = static_cast<std::uint16_t>(value);
            std::memcpy(ptr, &v, sizeof(v));
            return;
        }
        case runtime::NativeFieldKind::UInt32: {
            const auto v = static_cast<std::uint32_t>(value);
            std::memcpy(ptr, &v, sizeof(v));
            return;
        }
        case runtime::NativeFieldKind::UInt64: {
            const auto v = static_cast<std::uint64_t>(value);
            std::memcpy(ptr, &v, sizeof(v));
            return;
        }
        default: {
            const std::int32_t i32 = static_cast<std::int32_t>(value);
            std::memcpy(ptr, &i32, sizeof(i32));
            return;
        }
        }
    } else if constexpr (std::is_same_v<FieldT, runtime::Fixed16Value>) {
        const auto raw = static_cast<std::int16_t>(value.raw);
        std::memcpy(ptr, &raw, sizeof(raw));
    } else {
        std::memcpy(ptr, &value, sizeof(FieldT));
    }
}

constexpr std::uint32_t GeneratedModuleVersion = 1;

struct FieldDescriptor {
    const char* name = nullptr;
    semantic::PrimitiveType type = semantic::PrimitiveType::Error;
    semantic::SymbolId typeId = 0;
    std::uint32_t index = 0;
    bool synthetic = false;
};

struct EnumMemberDescriptor {
    const char* name = nullptr;
    std::int64_t value = 0;
};

struct InterfaceDispatchDescriptor {
    semantic::SymbolId interfaceTypeId = 0;
    const semantic::SymbolId* slots = nullptr;
    std::uint32_t slotCount = 0;
};

struct TypeDescriptor {
    semantic::SymbolId id = 0;
    semantic::TypeKind kind = semantic::TypeKind::Class;
    semantic::SymbolId baseTypeId = 0;
    const char* moduleName = nullptr;
    const char* name = nullptr;
    const FieldDescriptor* fields = nullptr;
    std::uint32_t fieldCount = 0;
    const EnumMemberDescriptor* enumMembers = nullptr;
    std::uint32_t enumMemberCount = 0;
    const semantic::SymbolId* virtualDispatchTable = nullptr;
    std::uint32_t virtualSlotCount = 0;
    bool interfaceType = false;
    const InterfaceDispatchDescriptor* interfaceDispatchMaps = nullptr;
    std::uint32_t interfaceDispatchMapCount = 0;
    bool delegateType = false;
};

class ExecutionContext;

using FunctionImplementation = bool (*)(
    ExecutionContext&,
    const runtime::Value*,
    std::size_t,
    runtime::Value&);

struct FunctionDescriptor {
    semantic::SymbolId symbolId = 0;
    const char* name = nullptr;
    semantic::PrimitiveType returnType = semantic::PrimitiveType::Error;
    semantic::SymbolId returnTypeId = 0;
    const semantic::PrimitiveType* parameterTypes = nullptr;
    const semantic::SymbolId* parameterTypeIds = nullptr;
    std::uint32_t parameterCount = 0;
    FunctionImplementation implementation = nullptr;
    const debug::FunctionDebugInfo* debugInfo = nullptr;
};

struct SourceMapEntry {
    semantic::SymbolId symbolId = 0;
    std::uint32_t generatedLine = 0;
    const char* sourcePath = nullptr;
    std::uint32_t sourceLine = 0;
    std::uint32_t sourceColumn = 0;
};

struct ProgramDescriptor {
    std::uint32_t abiMajor = RuntimeAbiMajor;
    std::uint32_t abiMinor = RuntimeAbiMinor;
    const char* name = nullptr;
    std::uint64_t contentHash = 0;
    const char* const* moduleNames = nullptr;
    std::uint32_t moduleCount = 0;
    const TypeDescriptor* types = nullptr;
    std::uint32_t typeCount = 0;
    const FunctionDescriptor* functions = nullptr;
    std::uint32_t functionCount = 0;
    const SourceMapEntry* sourceMap = nullptr;
    std::uint32_t sourceMapCount = 0;
};

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
struct ResolvedAotDelegateLeaf {
    runtime::Value receiver{runtime::NullObject{}};
    semantic::SymbolId targetSymbolId = 0;
    bool hasReceiver = false;
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

struct CallSignature {
    semantic::SymbolId symbolId = 0;
    const char* name = nullptr;
    semantic::PrimitiveType returnType = semantic::PrimitiveType::Error;
    semantic::SymbolId returnTypeId = 0;
    const semantic::PrimitiveType* parameterTypes = nullptr;
    const semantic::SymbolId* parameterTypeIds = nullptr;
    std::uint32_t parameterCount = 0;
    bool virtualDispatch = false;
    std::uint32_t virtualSlot = std::numeric_limits<std::uint32_t>::max();
    bool interfaceDispatch = false;
    semantic::SymbolId interfaceTypeId = 0;
    std::uint32_t interfaceSlot = std::numeric_limits<std::uint32_t>::max();
};

enum class UnaryOperation : std::uint8_t {
    NegateInt,
    NegateLong,
    NegateDouble,
    LogicalNot,
};

enum class BinaryOperation : std::uint8_t {
    Equal,
    NotEqual,
    AddInt,
    SubtractInt,
    MultiplyInt,
    DivideInt,
    RemainderInt,
    LessInt,
    LessOrEqualInt,
    GreaterInt,
    GreaterOrEqualInt,
    AddLong,
    SubtractLong,
    MultiplyLong,
    DivideLong,
    RemainderLong,
    LessLong,
    LessOrEqualLong,
    GreaterLong,
    GreaterOrEqualLong,
    AddDouble,
    SubtractDouble,
    MultiplyDouble,
    DivideDouble,
    LessDouble,
    LessOrEqualDouble,
    GreaterDouble,
    GreaterOrEqualDouble,
};

class FrameScope {
public:
    FrameScope(
        ExecutionContext& context,
        const std::vector<runtime::Value>& arguments,
        const std::vector<runtime::Value>& locals,
        const std::vector<runtime::Value>& registers,
        bool allowTransientStackSlices = true);
    ~FrameScope();
    FrameScope(const FrameScope&) = delete;
    FrameScope& operator=(const FrameScope&) = delete;

private:
    ExecutionContext* context_ = nullptr;
    std::uint32_t savedStackRefTop_ = 0;
    bool allowTransientStackSlices_ = true;
};

class StackRefScope {
public:
    explicit StackRefScope(ExecutionContext& context) noexcept;
    ~StackRefScope() noexcept;
    StackRefScope(const StackRefScope&) = delete;
    StackRefScope& operator=(const StackRefScope&) = delete;

private:
    ExecutionContext* context_ = nullptr;
    std::uint32_t savedTop_ = 0;
};

class ExecutionContext {
public:
    template <bool Enabled>
    class TypedRawAccountingScope {
    public:
        explicit TypedRawAccountingScope(ExecutionContext& context) noexcept {
            if constexpr (Enabled) {
                state_.context = &context;
                const auto budget = context.options_.limits.instructionBudget;
                state_.remaining = context.executed_ < budget
                    ? budget - context.executed_
                    : 0;
            } else {
                (void)context;
            }
        }
        ~TypedRawAccountingScope() {
            if constexpr (Enabled) {
                state_.context->executed_ += state_.consumed;
                state_.context->statistics_.instructionsExecuted =
                    state_.context->executed_;
                state_.context->statistics_.branchesTaken += state_.branches;
            }
        }
        TypedRawAccountingScope(const TypedRawAccountingScope&) = delete;
        TypedRawAccountingScope& operator=(
            const TypedRawAccountingScope&) = delete;

        [[nodiscard]] bool consume(std::uint64_t count) {
            if constexpr (!Enabled) {
                (void)count;
                return true;
            } else {
                if (count == 0) return true;
                if (count > state_.remaining - state_.consumed) {
                    state_.consumed = state_.remaining;
                    return state_.context->fail(
                        runtime::ErrorCode::InstructionBudgetExceeded,
                        "instruction budget exceeded");
                }
                state_.consumed += count;
                return true;
            }
        }
        [[nodiscard]] bool canConsume(std::uint64_t count) const noexcept {
            if constexpr (!Enabled) {
                (void)count;
                return true;
            } else {
                return count <= state_.remaining - state_.consumed;
            }
        }
        void branch() noexcept {
            if constexpr (Enabled) ++state_.branches;
        }
        void addBranches(std::uint64_t count) noexcept {
            if constexpr (Enabled) {
                state_.branches += count;
            } else {
                (void)count;
            }
        }
        void suspendBeforeCall() noexcept {
            if constexpr (Enabled) {
                state_.context->executed_ += state_.consumed;
                state_.context->statistics_.instructionsExecuted =
                    state_.context->executed_;
                state_.context->statistics_.branchesTaken += state_.branches;
                state_.consumed = 0;
                state_.branches = 0;
            }
        }
        void resumeAfterCall() noexcept {
            if constexpr (Enabled) {
                const auto budget =
                    state_.context->options_.limits.instructionBudget;
                state_.remaining = state_.context->executed_ < budget
                    ? budget - state_.context->executed_
                    : 0;
                state_.consumed = 0;
                state_.branches = 0;
            }
        }

    private:
        struct EnabledState {
            ExecutionContext* context = nullptr;
            std::uint64_t remaining = 0;
            std::uint64_t consumed = 0;
            std::uint64_t branches = 0;
        };
        struct DisabledState {};
        std::conditional_t<Enabled, EnabledState, DisabledState> state_;
    };

    template <bool Enabled>
    class TypedDeterminismAccountingScope {
    public:
        explicit TypedDeterminismAccountingScope(
            ExecutionContext& context) noexcept {
            if constexpr (Enabled) {
                state_.context = &context;
                state_.budget = context.options_.limits.instructionBudget;
                state_.executed = context.executed_;
                state_.digest = context.determinismEventDigest_;
                state_.eventCount = context.determinismEventCount_;
                state_.branches = context.statistics_.branchesTaken;
                state_.functionId = context.currentFunctionId_;
                state_.callDepth = context.stack_.size();
            } else {
                (void)context;
            }
        }
        ~TypedDeterminismAccountingScope() {
            if constexpr (Enabled) flush();
        }
        TypedDeterminismAccountingScope(
            const TypedDeterminismAccountingScope&) = delete;
        TypedDeterminismAccountingScope& operator=(
            const TypedDeterminismAccountingScope&) = delete;

        [[nodiscard]] bool consume(
            runtime::DeterminismOperationId operationId) {
            if constexpr (!Enabled) {
                (void)operationId;
                return true;
            } else {
                if (state_.executed >= state_.budget) {
                    flush();
                    return state_.context->fail(
                        runtime::ErrorCode::InstructionBudgetExceeded,
                        "instruction budget exceeded");
                }
                ++state_.executed;
                state_.digest = runtime::accumulateDeterminismEvent(
                    state_.digest,
                    runtime::TraceEventKind::Instruction,
                    state_.functionId,
                    operationId,
                    state_.executed,
                    state_.callDepth);
                ++state_.eventCount;
                return true;
            }
        }
        void branch(std::uint32_t blockId) noexcept {
            if constexpr (Enabled) {
                ++state_.branches;
                state_.digest = runtime::accumulateDeterminismEvent(
                    state_.digest,
                    runtime::TraceEventKind::Branch,
                    state_.functionId,
                    blockId,
                    state_.executed,
                    state_.callDepth);
                ++state_.eventCount;
            }
        }
        void flush() noexcept {
            if constexpr (Enabled) {
                if (state_.flushed) return;
                state_.context->executed_ = state_.executed;
                state_.context->statistics_.instructionsExecuted =
                    state_.executed;
                state_.context->statistics_.branchesTaken = state_.branches;
                state_.context->determinismEventDigest_ = state_.digest;
                state_.context->determinismEventCount_ = state_.eventCount;
                state_.flushed = true;
            }
        }
        void suspendBeforeCall() noexcept {
            if constexpr (Enabled) {
                flush();
            }
        }
        void resumeAfterCall() noexcept {
            if constexpr (Enabled) {
                state_.executed = state_.context->executed_;
                state_.digest = state_.context->determinismEventDigest_;
                state_.eventCount = state_.context->determinismEventCount_;
                state_.branches = state_.context->statistics_.branchesTaken;
                state_.functionId = state_.context->currentFunctionId_;
                state_.callDepth = state_.context->stack_.size();
                state_.flushed = false;
            }
        }

    private:
        struct EnabledState {
            ExecutionContext* context = nullptr;
            std::uint64_t budget = 0;
            std::uint64_t executed = 0;
            std::uint64_t digest = runtime::DeterminismEventSeed;
            std::uint64_t eventCount = 0;
            std::uint64_t branches = 0;
            semantic::SymbolId functionId = 0;
            std::size_t callDepth = 0;
            bool flushed = false;
        };
        struct DisabledState {};
        std::conditional_t<Enabled, EnabledState, DisabledState> state_;
    };

    ExecutionContext(
        const ProgramDescriptor& program,
        std::shared_ptr<runtime::ManagedHeap> heap,
        std::shared_ptr<const runtime::BindingRegistry> bindings,
        runtime::ExecutionOptions options = {},
        const std::unordered_map<semantic::SymbolId, const TypeDescriptor*>* prebuiltTypes = nullptr,
        const std::unordered_map<semantic::SymbolId, const FunctionDescriptor*>* prebuiltFunctions = nullptr);
    ~ExecutionContext();

    [[nodiscard]] bool invoke(
        semantic::SymbolId symbolId,
        const runtime::Value* arguments,
        std::size_t argumentCount,
        runtime::Value& result);
    [[nodiscard]] bool call(
        const CallSignature& signature,
        const runtime::Value* arguments,
        std::size_t argumentCount,
        runtime::Value& result);
    [[nodiscard]] bool newDelegate(
        semantic::SymbolId delegateTypeId,
        const CallSignature& signature,
        const runtime::Value* target,
        std::size_t targetCount,
        runtime::Value& result);
    [[nodiscard]] bool invokeDelegate(
        semantic::SymbolId delegateTypeId,
        const runtime::Value* arguments,
        std::size_t argumentCount,
        runtime::Value& result);
    [[nodiscard]] bool combineDelegates(
        semantic::SymbolId delegateTypeId,
        const runtime::Value& left,
        const runtime::Value& right,
        bool remove,
        runtime::Value& result);
    [[nodiscard]] int inspectDelegateSlow(
        semantic::SymbolId delegateTypeId,
        const runtime::Value& delValue,
        ResolvedAotDelegateLeaf& outSingle);
    [[nodiscard]] inline int inspectSingleOrMulticastDelegate(
        semantic::SymbolId delegateTypeId,
        const runtime::Value& delValue,
        ResolvedAotDelegateLeaf& outSingle) {
        if (RS_AOT_LIKELY(delValue.rawTag() == runtime::ValueTag::ObjectRef && heap_ != nullptr)) {
            const auto ref = delValue.asObjectRefUnchecked();
            const auto& ce = delLeafCache_[ref.slot & 15u];
            if (RS_AOT_LIKELY(
                    ce.valid &&
                    !ce.isMulticast &&
                    ce.delRef == ref &&
                    ce.delegateTypeId == delegateTypeId &&
                    heap_->objectTypeIdFast(ref) == delegateTypeId &&
                    (!ce.hasObjectReceiver || heap_->objectTypeIdFast(ce.recvRef) != 0))) {
                outSingle = ce.resolved;
                return 1;
            }
            if (ce.valid && ce.isMulticast && ce.delRef == ref &&
                ce.delegateTypeId == delegateTypeId &&
                heap_->objectTypeIdFast(ref) == delegateTypeId) {
                return 2;
            }
        }
        return inspectDelegateSlow(delegateTypeId, delValue, outSingle);
    }
    [[nodiscard]] const ResolvedAotDelegateLeaf* acquireMulticastLeavesFast(
        semantic::SymbolId delegateTypeId,
        const runtime::Value& rootValue,
        std::size_t& outCount,
        std::vector<ResolvedAotDelegateLeaf>& fallbackBuf);
    void releaseMulticastLeavesFast(const ResolvedAotDelegateLeaf* ptr) noexcept {
        if (ptr == cachedMulticastLeaves_.data() && cachedMulticastPinCount_ > 0) {
            --cachedMulticastPinCount_;
        }
    }
    [[nodiscard]] bool hasFunction(semantic::SymbolId symbolId) const noexcept;
    [[nodiscard]] std::uint32_t stackRefTop() const noexcept { return stackRefTop_; }
    [[nodiscard]] std::size_t stackRefSlotCapacity() const noexcept { return stackRefSlots_.size(); }

    [[nodiscard]] bool consume(std::string_view operation);
    [[nodiscard]] bool consume(
        runtime::DeterminismOperationId operationId,
        std::string_view operation);
    [[nodiscard]] bool fastAccountingEnabled() const noexcept {
        return fastAccounting_;
    }
    [[nodiscard]] bool trustedReleaseEnabled() const noexcept {
        return options_.trustedRelease && fastAccounting_;
    }
    [[nodiscard]] bool typedDeterminismAccountingEnabled() const noexcept {
        return determinismOnly_ && options_.limits.gcWorkBudget == 0;
    }
    void setActiveGeneratedLine(std::uint32_t generatedLine) noexcept {
        if (!stackSites_.empty()) {
            stackSites_.back().generatedLine = generatedLine;
        }
    }

    template <bool FastAccounting, bool DeterminismAccounting, bool TrustedRelease = false>
    [[nodiscard]] bool enterTypedDirectCall(
        semantic::SymbolId calleeSymbolId,
        const char* calleeName,
        semantic::SymbolId& outPreviousFunctionId,
        std::uint32_t callerGeneratedLine = 0) {
        if constexpr (TrustedRelease) {
            (void)calleeSymbolId;
            (void)calleeName;
            (void)outPreviousFunctionId;
            (void)callerGeneratedLine;
            return true;
        } else {
            if (callerGeneratedLine != 0 && !stackSites_.empty()) {
                stackSites_.back().generatedLine = callerGeneratedLine;
            }
            if (stack_.size() >= options_.limits.recursionLimit) {
                return fail(
                    runtime::ErrorCode::RecursionLimitExceeded,
                    "recursion limit exceeded",
                    callerGeneratedLine);
            }
            outPreviousFunctionId = currentFunctionId_;
            currentFunctionId_ = calleeSymbolId;
            stack_.push_back(calleeName ? calleeName : "<aot-function>");
            stackSites_.push_back({calleeSymbolId, 0});
            ++statistics_.functionCalls;
            if (stack_.size() > statistics_.maximumCallDepth) {
                statistics_.maximumCallDepth = stack_.size();
            }
            if constexpr (DeterminismAccounting) {
                determinismEventDigest_ = runtime::accumulateDeterminismEvent(
                    determinismEventDigest_,
                    runtime::TraceEventKind::FunctionEnter,
                    currentFunctionId_,
                    0,
                    executed_,
                    stack_.size());
                ++determinismEventCount_;
            } else if constexpr (!FastAccounting) {
                emitTrace(runtime::TraceEventKind::FunctionEnter);
            }
            return true;
        }
    }

    template <bool FastAccounting, bool DeterminismAccounting, bool TrustedRelease = false, typename BoxFn>
    void exitTypedDirectCall(
        semantic::SymbolId previousFunctionId,
        bool succeeded,
        BoxFn&& boxReturnValue) {
        if constexpr (TrustedRelease) {
            (void)previousFunctionId;
            (void)succeeded;
            (void)boxReturnValue;
            return;
        } else {
            if (succeeded) {
                if constexpr (DeterminismAccounting) {
                    determinismEventDigest_ = runtime::accumulateDeterminismEvent(
                        determinismEventDigest_,
                        runtime::TraceEventKind::FunctionExit,
                        currentFunctionId_,
                        0,
                        executed_,
                        stack_.size());
                    ++determinismEventCount_;
                } else if constexpr (!FastAccounting) {
                    if (traceEventsEnabled_) {
                        if (diagnosticEventsEnabled_) {
                            const runtime::Value boxed = boxReturnValue();
                            const auto operation =
                                runtime::valueToString(boxed, heap_.get());
                            emitTrace(
                                runtime::TraceEventKind::FunctionExit,
                                0,
                                operation);
                        } else {
                            emitTrace(runtime::TraceEventKind::FunctionExit);
                        }
                    }
                }
            }
            if (!stack_.empty()) stack_.pop_back();
            if (!stackSites_.empty()) stackSites_.pop_back();
            currentFunctionId_ = previousFunctionId;
        }
    }
    [[nodiscard]] bool consumeRawTyped(std::uint64_t count = 1) {
        if (count == 0) return true;
        const auto budget = options_.limits.instructionBudget;
        if (executed_ >= budget || count > budget - executed_) {
            executed_ = budget;
            statistics_.instructionsExecuted = executed_;
            return fail(
                runtime::ErrorCode::InstructionBudgetExceeded,
                "instruction budget exceeded");
        }
        executed_ += count;
        statistics_.instructionsExecuted = executed_;
        return true;
    }
    [[nodiscard]] bool branch(std::uint32_t blockId);
    void branchRawTyped() noexcept {
        ++statistics_.branchesTaken;
    }
    [[nodiscard]] bool throwScript(const runtime::Value& value);
    [[nodiscard]] bool hasPendingException() const noexcept;
    [[nodiscard]] bool pendingExceptionMatches(
        semantic::SymbolId typeId) const;
    [[nodiscard]] bool takePendingException(runtime::Value& value);
    void reportUnhandledScriptException();
    [[nodiscard]] bool fail(
        runtime::ErrorCode code,
        std::string message,
        std::uint32_t generatedLine = 0);
    [[nodiscard]] bool expectType(
        const runtime::Value& value,
        semantic::PrimitiveType type,
        std::string_view context);
    [[nodiscard]] bool expectSignatureType(
        const runtime::Value& value,
        semantic::PrimitiveType type,
        semantic::SymbolId typeId,
        std::string_view context);

    [[nodiscard]] runtime::Value defaultValue(
        semantic::PrimitiveType type,
        semantic::SymbolId typeId,
        std::size_t depth = 0) const;

    [[nodiscard]] bool constantString(
        std::string_view value,
        runtime::Value& result);
    [[nodiscard]] bool newObject(
        semantic::SymbolId typeId,
        runtime::Value& result);
    [[nodiscard]] bool newStackRefObject(
        semantic::SymbolId typeId,
        runtime::Value& result);
    [[nodiscard]] bool newStruct(
        semantic::SymbolId typeId,
        runtime::Value& result);
    [[nodiscard]] bool newArray(
        semantic::SymbolId arrayTypeId,
        semantic::PrimitiveType elementType,
        semantic::SymbolId elementTypeId,
        const runtime::Value& length,
        runtime::Value& result);
    [[nodiscard]] bool checkNotNull(
        semantic::SymbolId typeId,
        const runtime::Value& receiver,
        runtime::Value& result);
    [[nodiscard]] bool typeOperation(
        semantic::PrimitiveType sourceType,
        semantic::PrimitiveType targetType,
        semantic::SymbolId targetTypeId,
        const runtime::Value& value,
        bool safeCast,
        runtime::Value& result);
    [[nodiscard]] bool arrayLength(
        const runtime::Value& receiver,
        runtime::Value& result);
    [[nodiscard]] bool arrayLengthFast(
        const runtime::Value& receiver,
        std::int64_t& result);
    [[nodiscard]] bool loadElement(
        semantic::PrimitiveType elementType,
        semantic::SymbolId elementTypeId,
        const runtime::Value& receiver,
        const runtime::Value& index,
        runtime::Value& result);
    [[nodiscard]] bool storeElement(
        semantic::PrimitiveType elementType,
        semantic::SymbolId elementTypeId,
        const runtime::Value& receiver,
        const runtime::Value& index,
        const runtime::Value& value);
    [[nodiscard]] bool loadField(
        semantic::SymbolId typeId,
        std::size_t fieldIndex,
        const runtime::Value& receiver,
        runtime::Value& result);
    [[nodiscard]] bool storeField(
        semantic::SymbolId typeId,
        std::size_t fieldIndex,
        const runtime::Value& receiver,
        const runtime::Value& value);
    [[nodiscard]] bool loadStructField(
        semantic::SymbolId typeId,
        std::size_t fieldIndex,
        const runtime::Value& receiver,
        runtime::Value& result);
    [[nodiscard]] bool storeStructField(
        semantic::SymbolId typeId,
        std::size_t fieldIndex,
        const runtime::Value& receiver,
        const runtime::Value& value,
        runtime::Value& result);
    [[nodiscard]] bool initUnboxedStruct(
        semantic::SymbolId typeId,
        struct UnboxedCustomStruct& result);
    template <typename T, typename = std::enable_if_t<T::kIsGeneratedAotStruct>>
    [[nodiscard]] constexpr bool initUnboxedStruct(
        semantic::SymbolId,
        T& result) const noexcept {
        result = T{};
        return true;
    }
    [[nodiscard]] bool loadUnboxedStructField(
        semantic::SymbolId typeId,
        std::size_t fieldIndex,
        const struct UnboxedCustomStruct& receiver,
        runtime::Value& result);
    [[nodiscard]] bool storeUnboxedStructField(
        semantic::SymbolId typeId,
        std::size_t fieldIndex,
        const struct UnboxedCustomStruct& receiver,
        const runtime::Value& value,
        struct UnboxedCustomStruct& result);
    [[nodiscard]] bool ecsWritebackBoxed(
        const CallSignature& signature,
        std::int64_t index,
        const runtime::Value& boxedStruct);
    [[nodiscard]] bool resolveEcsColumnFast(
        semantic::SymbolId denseColSymbolId,
        const CallSignature& fallbackSignature,
        runtime::Value& outResult);
    [[nodiscard]] struct TypedRawColumnCache acquireRawColumnCache(
        const runtime::Value& arrayValue,
        semantic::SymbolId expectedStructTypeId = 0,
        std::size_t generatedStructSize = 0,
        const struct GeneratedFieldLayout* generatedFields = nullptr,
        std::size_t generatedFieldCount = 0) const noexcept;
    [[nodiscard]] bool hasSparseEcsState() const noexcept {
        return heap_ != nullptr && heap_->hasSparseEcsState();
    }
    [[nodiscard]] bool convert(
        semantic::ConversionKind conversion,
        semantic::PrimitiveType targetType,
        bool checkedArithmetic,
        const runtime::Value& value,
        runtime::Value& result);
    [[nodiscard]] bool unary(
        UnaryOperation operation,
        bool checkedArithmetic,
        const runtime::Value& value,
        runtime::Value& result);
    [[nodiscard]] bool binary(
        BinaryOperation operation,
        bool checkedArithmetic,
        const runtime::Value& left,
        const runtime::Value& right,
        runtime::Value& result);

    [[nodiscard]] runtime::ManagedHeap* rawHeap() const noexcept {
        return heap_.get();
    }
    [[nodiscard]] semantic::SymbolId objectTypeIdFast(
        const runtime::Value& receiver) const noexcept {
        if (receiver.rawTag() == runtime::ValueTag::ObjectRef && heap_) {
            return heap_->objectTypeIdFast(receiver.asObjectRefUnchecked());
        }
        return 0;
    }
    [[nodiscard]] bool checkNotNullFast(
        semantic::SymbolId typeId,
        const runtime::Value& receiver,
        runtime::Value& result) {
        if (receiver.rawTag() == runtime::ValueTag::ObjectRef && heap_) {
            const auto actualTid = heap_->objectTypeIdFast(receiver.asObjectRefUnchecked());
            if (actualTid != 0 && (actualTid == typeId || isAssignable(actualTid, typeId))) {
                result = receiver;
                return true;
            }
        }
        return checkNotNull(typeId, receiver, result);
    }
    [[nodiscard]] bool loadFieldObjectFast(
        semantic::SymbolId ownerTypeId,
        std::size_t fieldIndex,
        semantic::SymbolId expectedFieldTypeId,
        const runtime::Value& receiver,
        runtime::Value& result) {
        if (RS_AOT_LIKELY(receiver.rawTag() == runtime::ValueTag::ObjectRef && heap_ != nullptr)) {
            runtime::ObjectFastView view{};
            if (RS_AOT_LIKELY(heap_->tryBindObjectFastView(receiver.asObjectRefUnchecked(), view) &&
                              fieldIndex < view.fieldCount &&
                              (view.typeId == ownerTypeId || isAssignable(view.typeId, ownerTypeId)))) {
                const runtime::Value& fv = view.fields[fieldIndex];
                const auto ftag = fv.rawTag();
                if (ftag == runtime::ValueTag::NullObject) {
                    result = fv;
                    return true;
                }
                if (RS_AOT_LIKELY(ftag == runtime::ValueTag::ObjectRef)) {
                    const auto fTid = heap_->objectTypeIdFast(fv.asObjectRefUnchecked());
                    if (RS_AOT_LIKELY(fTid != 0 &&
                                      (expectedFieldTypeId == 0 ||
                                       fTid == expectedFieldTypeId ||
                                       isAssignable(fTid, expectedFieldTypeId)))) {
                        result = fv;
                        return true;
                    }
                }
            }
        }
        return loadField(ownerTypeId, fieldIndex, receiver, result);
    }
    [[nodiscard]] bool loadFieldInt64Fast(
        semantic::SymbolId typeId,
        std::size_t fieldIndex,
        const runtime::Value& receiver,
        std::int64_t& result) {
        if (receiver.rawTag() == runtime::ValueTag::ObjectRef) {
            const auto ref = receiver.asObjectRefUnchecked();
            if (ref.isStackSlotRef()) {
                if (RS_AOT_LIKELY(fieldIndex == 0 &&
                                  ref.generation == stackRefBankId_ &&
                                  ref.stackRefSchemaId() == 0u &&
                                  ref.slot < stackRefTop_ &&
                                  stackRefSlots_[ref.slot].tryGetInt64(result))) {
                    return true;
                }
            } else if (heap_) {
                runtime::ObjectFastView view{};
                if (heap_->tryBindObjectFastView(ref, view) &&
                    fieldIndex < view.fieldCount &&
                    (view.typeId == typeId || isAssignable(view.typeId, typeId)) &&
                    view.fields[fieldIndex].tryGetInt64(result)) {
                    return true;
                }
            }
        }
        runtime::Value boxed;
        if (!loadField(typeId, fieldIndex, receiver, boxed)) return false;
        return boxed.tryGetInt64(result);
    }
    template <typename T, typename = std::enable_if_t<T::kIsGeneratedAotStruct>>
    [[nodiscard]] bool storeFieldCustomStructFast(
        semantic::SymbolId typeId,
        std::size_t fieldIndex,
        const runtime::Value& receiver,
        const T& value) {
        if (RS_AOT_LIKELY(receiver.rawTag() == runtime::ValueTag::ObjectRef)) {
            const auto ref = receiver.asObjectRefUnchecked();
            if (ref.isStackSlotRef() &&
                fieldIndex == 0 &&
                ref.generation == stackRefBankId_ &&
                ref.stackRefSchemaId() != 0u) {
                if (const auto* sc = runtime::getFlatStructSchema(ref.stackRefSchemaId());
                    sc != nullptr && sc->structTypeId == T::kTypeId &&
                    ref.slot + sc->flatSlotCount <= stackRefTop_) {
                    runtime::Value* cur = stackRefSlots_.data() + ref.slot;
                    T::storeToFlatLeaves(cur, value);
                    return true;
                }
            }
        }
        return storeField(typeId, fieldIndex, receiver, value.boxToValue());
    }
    template <typename T, typename = std::enable_if_t<T::kIsGeneratedAotStruct>>
    [[nodiscard]] bool loadElementStructFast(
        semantic::SymbolId elementTypeId,
        const runtime::Value& receiver,
        std::int64_t index,
        T& out) {
        if (RS_AOT_LIKELY(receiver.rawTag() == runtime::ValueTag::ObjectRef && heap_ != nullptr)) {
            const auto ref = receiver.asObjectRefUnchecked();
            runtime::FlatStructArrayFastView fView{};
            if (heap_->tryBindFlatStructArrayFastView(ref, fView) && fView.elementTypeId == elementTypeId) {
                if (RS_AOT_UNLIKELY(static_cast<std::uint64_t>(index) >= fView.length)) {
                    return fail(runtime::ErrorCode::IndexOutOfRange, "array element index is out of range");
                }
                const runtime::Value* cur =
                    fView.data + static_cast<std::size_t>(index) * fView.flatStride;
                if (RS_AOT_LIKELY(T::loadFromFlatLeaves(cur, out))) {
                    return true;
                }
            }
            runtime::PackedArrayFastView pView{};
            if (heap_->tryBindPackedArrayFastView(ref, pView) &&
                pView.encoding == runtime::PackedElemEncoding::BlittableStruct &&
                pView.elementTypeId == elementTypeId) {
                if (RS_AOT_UNLIKELY(static_cast<std::uint64_t>(index) >= pView.length)) {
                    return fail(runtime::ErrorCode::IndexOutOfRange, "array element index is out of range");
                }
                if (const auto* sc = runtime::findFlatStructSchemaByTypeId(elementTypeId)) {
                    std::array<runtime::Value, 64> stackLeaves{};
                    std::vector<runtime::Value> heapLeaves;
                    runtime::Value* buf = sc->flatSlotCount <= stackLeaves.size()
                        ? stackLeaves.data()
                        : (heapLeaves.resize(sc->flatSlotCount), heapLeaves.data());
                    if (runtime::decodePackedStructToLeaves(
                            pView.rawData, static_cast<std::size_t>(index), pView.blittableSchemaId, *sc, buf)) {
                        const runtime::Value* cur = buf;
                        if (RS_AOT_LIKELY(T::loadFromFlatLeaves(cur, out))) {
                            return true;
                        }
                    }
                }
            }
        }
        runtime::Value boxed;
        if (!loadElement(semantic::PrimitiveType::Struct, elementTypeId, receiver, runtime::Value{index}, boxed)) {
            return false;
        }
        if (!T::unboxFromValue(boxed, out)) {
            return fail(runtime::ErrorCode::TypeMismatch, "array struct element unbox failed");
        }
        return true;
    }
    template <typename T, typename = std::enable_if_t<T::kIsGeneratedAotStruct>>
    [[nodiscard]] bool storeElementStructFast(
        semantic::SymbolId elementTypeId,
        const runtime::Value& receiver,
        std::int64_t index,
        const T& value) {
        if (RS_AOT_LIKELY(receiver.rawTag() == runtime::ValueTag::ObjectRef && heap_ != nullptr)) {
            const auto ref = receiver.asObjectRefUnchecked();
            runtime::FlatStructArrayFastView fView{};
            if (heap_->tryBindFlatStructArrayFastView(ref, fView) && fView.elementTypeId == elementTypeId) {
                if (RS_AOT_UNLIKELY(static_cast<std::uint64_t>(index) >= fView.length)) {
                    return fail(runtime::ErrorCode::IndexOutOfRange, "array element index is out of range");
                }
                const auto uIdx = static_cast<std::size_t>(index);
                runtime::Value* cur = fView.data + uIdx * fView.flatStride;
                if (fView.containsPoolValues) {
                    T::storeToFlatLeavesPinned(cur, value);
                } else {
                    T::storeToFlatLeaves(cur, value);
                }
                if (fView.containsManagedReferences) {
                    if (fView.dirtyCards != nullptr) {
                        fView.dirtyCards[uIdx >> runtime::ManagedHeap::kCardElementShift] = 1u;
                    }
                    if (fView.needsRememberedBarrier || heap_->phase() != runtime::GcPhase::Idle) {
                        heap_->recordFlatStructArrayWriteBarrier(ref, uIdx);
                    }
                }
                return true;
            }
            if (const auto* sc = runtime::findFlatStructSchemaByTypeId(elementTypeId);
                sc != nullptr && stackRefBankId_ != 0u) {
                if (RS_AOT_UNLIKELY(index < 0)) {
                    return fail(runtime::ErrorCode::IndexOutOfRange, "array element index is out of range");
                }
                const std::uint32_t baseOff = stackRefTop_;
                const std::size_t need = static_cast<std::size_t>(baseOff) + sc->flatSlotCount;
                if (need <= runtime::StructStorageHandle::FrameOffsetMask) {
                    if (need > stackRefSlots_.size()) {
                        stackRefSlots_.resize(std::max<std::size_t>(need, stackRefSlots_.capacity() * 2u + 64u));
                    }
                    runtime::Value* cur = stackRefSlots_.data() + baseOff;
                    T::storeToFlatLeaves(cur, value);
                    const runtime::Value sliceVal{runtime::StructValue::makeFrameSlice(
                        elementTypeId, stackRefBankId_, baseOff, sc->schemaId)};
                    const bool ok = storeElement(
                        semantic::PrimitiveType::Struct, elementTypeId, receiver, runtime::Value{index}, sliceVal);
                    for (std::uint16_t k = 0; k < sc->flatSlotCount; ++k) {
                        if (cur[k].isTraceable()) {
                            cur[k] = runtime::Value{};
                        }
                    }
                    return ok;
                }
            }
        }
        return storeElement(
            semantic::PrimitiveType::Struct, elementTypeId, receiver, runtime::Value{index}, value.boxToValue());
    }
    [[nodiscard]] bool storeFieldInt64Fast(
        semantic::SymbolId typeId,
        std::size_t fieldIndex,
        const runtime::Value& receiver,
        std::int64_t value) {
        if (receiver.rawTag() == runtime::ValueTag::ObjectRef) {
            const auto ref = receiver.asObjectRefUnchecked();
            if (ref.isStackSlotRef()) {
                if (RS_AOT_LIKELY(fieldIndex == 0 &&
                                  ref.generation == stackRefBankId_ &&
                                  ref.stackRefSchemaId() == 0u &&
                                  ref.slot < stackRefTop_)) {
                    stackRefSlots_[ref.slot].storeInt64(value);
                    return true;
                }
            } else if (heap_) {
                runtime::ObjectFastView view{};
                if (heap_->tryBindObjectFastView(ref, view) &&
                    fieldIndex < view.fieldCount &&
                    (view.typeId == typeId || isAssignable(view.typeId, typeId)) &&
                    view.fields[fieldIndex].rawTag() == runtime::ValueTag::Int64) {
                    view.fields[fieldIndex].storeInt64(value);
                    return true;
                }
            }
        }
        return storeField(typeId, fieldIndex, receiver, runtime::Value{value});
    }
    [[nodiscard]] bool tryBindPackedInt32Array(
        const runtime::Value& arrayValue,
        std::int32_t*& outData,
        std::uint32_t& outLength) const noexcept {
        if (RS_AOT_LIKELY(arrayValue.rawTag() == runtime::ValueTag::ObjectRef && heap_ != nullptr)) {
            const auto ref = arrayValue.asObjectRefUnchecked();
            if (RS_AOT_LIKELY(
                    heap_->tryBindPackedInt32Array(ref, outData, outLength) &&
                    outLength <= static_cast<std::uint32_t>(INT32_MAX))) {
                static const semantic::SymbolId kIntArrayTypeId =
                    semantic::stableTypeId("int[]");
                if (RS_AOT_LIKELY(heap_->arrayTypeId(ref) == kIntArrayTypeId)) {
                    return true;
                }
            }
        }
        outData = nullptr;
        outLength = 0;
        return false;
    }

    [[nodiscard]] bool tryBindPackedDoubleArray(
        const runtime::Value& arrayValue,
        double*& outData,
        std::uint32_t& outLength) const noexcept {
        if (RS_AOT_LIKELY(arrayValue.rawTag() == runtime::ValueTag::ObjectRef && heap_ != nullptr)) {
            const auto ref = arrayValue.asObjectRefUnchecked();
            if (RS_AOT_LIKELY(
                    heap_->tryBindPackedDoubleArray(ref, outData, outLength) &&
                    outLength <= static_cast<std::uint32_t>(INT32_MAX))) {
                return true;
            }
        }
        outData = nullptr;
        outLength = 0;
        return false;
    }

    [[nodiscard]] TypedPackedInt32ArrayCache acquirePackedInt32ArrayCache(
        const runtime::Value& arrayValue) const noexcept {
        TypedPackedInt32ArrayCache cache{};
        std::int32_t* ptr = nullptr;
        std::uint32_t len = 0;
        if (tryBindPackedInt32Array(arrayValue, ptr, len)) {
            cache.data = ptr;
            cache.count = static_cast<std::int64_t>(len);
        }
        return cache;
    }

    [[nodiscard]] TypedPackedDoubleArrayCache acquirePackedDoubleArrayCache(
        const runtime::Value& arrayValue) const noexcept {
        TypedPackedDoubleArrayCache cache{};
        double* ptr = nullptr;
        std::uint32_t len = 0;
        if (tryBindPackedDoubleArray(arrayValue, ptr, len)) {
            cache.data = ptr;
            cache.count = static_cast<std::int64_t>(len);
        }
        return cache;
    }

    [[nodiscard]] TypedStructArrayCache acquireStructArrayCache(
        semantic::SymbolId elementTypeId,
        const runtime::Value& arrayValue) const noexcept {
        TypedStructArrayCache cache{};
        if (arrayValue.rawTag() != runtime::ValueTag::ObjectRef || heap_ == nullptr) {
            return cache;
        }
        const auto ref = arrayValue.asObjectRefUnchecked();
        runtime::FlatStructArrayFastView fView{};
        if (heap_->tryBindFlatStructArrayFastView(ref, fView) &&
            fView.elementTypeId == elementTypeId) {
            cache.flatData = fView.data;
            cache.dirtyCards = fView.dirtyCards;
            cache.count = static_cast<std::int64_t>(fView.length);
            cache.gcEpoch = heap_->gcEpoch();
            cache.flatStride = fView.flatStride;
            cache.flatSchemaId = fView.flatSchemaId;
            cache.arrayRef = ref;
            cache.containsManagedReferences = fView.containsManagedReferences;
            cache.containsPoolValues = fView.containsPoolValues;
            cache.needsRememberedBarrier = fView.needsRememberedBarrier;
            return cache;
        }
        runtime::PackedArrayFastView pView{};
        if (heap_->tryBindPackedArrayFastView(ref, pView) &&
            pView.encoding == runtime::PackedElemEncoding::BlittableStruct &&
            pView.elementTypeId == elementTypeId) {
            cache.packedData = static_cast<std::uint8_t*>(pView.rawData);
            cache.count = static_cast<std::int64_t>(pView.length);
            cache.gcEpoch = heap_->gcEpoch();
            cache.blittableSchemaId = pView.blittableSchemaId;
            cache.arrayRef = ref;
            return cache;
        }
        return cache;
    }

    template <typename T, typename = std::enable_if_t<T::kIsGeneratedAotStruct>>
    [[nodiscard]] bool loadElementStructCached(
        semantic::SymbolId elementTypeId,
        TypedStructArrayCache& cache,
        const runtime::Value& receiver,
        std::int64_t index,
        T& out) {
        if (RS_AOT_UNLIKELY((cache.flatData != nullptr || cache.packedData != nullptr) &&
                            heap_ != nullptr && cache.gcEpoch != heap_->gcEpoch())) {
            cache = acquireStructArrayCache(elementTypeId, receiver);
        }
        if (RS_AOT_LIKELY(cache.flatData != nullptr)) {
            if (RS_AOT_UNLIKELY(static_cast<std::uint64_t>(index) >= static_cast<std::uint64_t>(cache.count))) {
                return fail(runtime::ErrorCode::IndexOutOfRange, "array element index is out of range");
            }
            const runtime::Value* cur =
                cache.flatData + static_cast<std::size_t>(index) * cache.flatStride;
            if (RS_AOT_LIKELY(T::loadFromFlatLeaves(cur, out))) {
                return true;
            }
        } else if (RS_AOT_LIKELY(cache.packedData != nullptr)) {
            if (RS_AOT_UNLIKELY(static_cast<std::uint64_t>(index) >= static_cast<std::uint64_t>(cache.count))) {
                return fail(runtime::ErrorCode::IndexOutOfRange, "array element index is out of range");
            }
            if (const auto* sc = runtime::findFlatStructSchemaByTypeId(elementTypeId)) {
                std::array<runtime::Value, 64> stackLeaves{};
                std::vector<runtime::Value> heapLeaves;
                runtime::Value* buf = sc->flatSlotCount <= stackLeaves.size()
                    ? stackLeaves.data()
                    : (heapLeaves.resize(sc->flatSlotCount), heapLeaves.data());
                if (runtime::decodePackedStructToLeaves(
                        cache.packedData, static_cast<std::size_t>(index), cache.blittableSchemaId, *sc, buf)) {
                    const runtime::Value* cur = buf;
                    if (RS_AOT_LIKELY(T::loadFromFlatLeaves(cur, out))) {
                        return true;
                    }
                }
            }
        }
        return loadElementStructFast(elementTypeId, receiver, index, out);
    }

    template <typename T, typename = std::enable_if_t<T::kIsGeneratedAotStruct>>
    [[nodiscard]] bool storeElementStructCached(
        semantic::SymbolId elementTypeId,
        TypedStructArrayCache& cache,
        const runtime::Value& receiver,
        std::int64_t index,
        const T& value) {
        if (RS_AOT_UNLIKELY((cache.flatData != nullptr || cache.packedData != nullptr) &&
                            heap_ != nullptr && cache.gcEpoch != heap_->gcEpoch())) {
            cache = acquireStructArrayCache(elementTypeId, receiver);
        }
        if (RS_AOT_LIKELY(cache.flatData != nullptr)) {
            if (RS_AOT_UNLIKELY(static_cast<std::uint64_t>(index) >= static_cast<std::uint64_t>(cache.count))) {
                return fail(runtime::ErrorCode::IndexOutOfRange, "array element index is out of range");
            }
            const auto uIdx = static_cast<std::size_t>(index);
            runtime::Value* cur = cache.flatData + uIdx * cache.flatStride;
            if (cache.containsPoolValues) {
                T::storeToFlatLeavesPinned(cur, value);
            } else {
                T::storeToFlatLeaves(cur, value);
            }
            if (cache.containsManagedReferences && heap_ != nullptr) {
                const std::size_t cardIdx = uIdx >> runtime::ManagedHeap::kCardElementShift;
                const bool cardWasClean = (cache.dirtyCards != nullptr) && (cache.dirtyCards[cardIdx] == 0u);
                if (cache.dirtyCards != nullptr) {
                    cache.dirtyCards[cardIdx] = 1u;
                }
                if (cardWasClean || cache.needsRememberedBarrier || heap_->phase() != runtime::GcPhase::Idle) {
                    heap_->recordFlatStructArrayWriteBarrier(cache.arrayRef, uIdx);
                    cache.needsRememberedBarrier = false;
                }
            }
            return true;
        }
        return storeElementStructFast(elementTypeId, receiver, index, value);
    }

    template <bool TrustedRelease = false, bool Negate = false>
    [[nodiscard]] bool compareStringsFast(
        const runtime::Value& left,
        const runtime::Value& right,
        bool& outResult) {
        if (std::holds_alternative<runtime::NullString>(left) ||
            std::holds_alternative<runtime::NullString>(right)) {
            const bool eq = std::holds_alternative<runtime::NullString>(left) &&
                std::holds_alternative<runtime::NullString>(right);
            outResult = Negate ? !eq : eq;
            return true;
        }
        const auto extractText = [&](const runtime::Value& value)
            -> std::optional<std::string_view> {
            const auto tag = value.rawTag();
            if (tag == runtime::ValueTag::String) {
                return std::string_view(value.asStringUnchecked().str());
            }
            if (tag == runtime::ValueTag::ObjectRef) {
                return heap_
                    ? heap_->stringView(value.asObjectRefUnchecked())
                    : std::nullopt;
            }
            return std::nullopt;
        };
        const auto a = extractText(left);
        const auto b = extractText(right);
        if (RS_AOT_UNLIKELY(!a || !b)) {
            if constexpr (TrustedRelease) {
                outResult = Negate;
                return true;
            } else {
                return fail(
                    runtime::ErrorCode::InvalidObjectReference,
                    "string equality encountered an invalid managed reference");
            }
        }
        const bool eq = (*a == *b);
        outResult = Negate ? !eq : eq;
        return true;
    }

    template <bool TrustedRelease = false, bool Negate = false>
    [[nodiscard]] bool compareReferencesFast(
        const runtime::Value& left,
        const runtime::Value& right,
        bool& outResult) {
        if constexpr (!TrustedRelease) {
            const auto isValidRefOperand = [&](const runtime::Value& value) noexcept {
                if (value.rawTag() != runtime::ValueTag::ObjectRef) return true;
                const auto& ref = value.asObjectRefUnchecked();
                if (ref.isStackSlotRef()) return true;
                return heap_ != nullptr && heap_->isAlive(ref);
            };
            if (RS_AOT_UNLIKELY(!isValidRefOperand(left) || !isValidRefOperand(right))) {
                return fail(
                    runtime::ErrorCode::InvalidObjectReference,
                    "reference equality encountered an invalid managed reference");
            }
        }
        const bool eq = (left == right);
        outResult = Negate ? !eq : eq;
        return true;
    }

    [[nodiscard]] bool loadElementInt32Fast(
        const runtime::Value& receiver,
        std::int64_t index,
        std::int64_t& result) {
        std::int32_t* data = nullptr;
        std::uint32_t len = 0;
        if (RS_AOT_LIKELY(tryBindPackedInt32Array(receiver, data, len))) {
            if (RS_AOT_UNLIKELY(static_cast<std::uint64_t>(index) >= len)) {
                return fail(
                    runtime::ErrorCode::IndexOutOfRange,
                    "array element index is out of range");
            }
            result = static_cast<std::int64_t>(data[static_cast<std::size_t>(index)]);
            return true;
        }
        runtime::Value boxed;
        if (!loadElement(semantic::PrimitiveType::Int, 0, receiver, runtime::Value{index}, boxed)) {
            return false;
        }
        return boxed.tryGetInt64(result);
    }

    [[nodiscard]] bool storeElementInt32Fast(
        const runtime::Value& receiver,
        std::int64_t index,
        std::int64_t value) {
        std::int32_t* data = nullptr;
        std::uint32_t len = 0;
        if (RS_AOT_LIKELY(
                tryBindPackedInt32Array(receiver, data, len) &&
                value >= static_cast<std::int64_t>(INT32_MIN) &&
                value <= static_cast<std::int64_t>(INT32_MAX))) {
            if (RS_AOT_UNLIKELY(static_cast<std::uint64_t>(index) >= len)) {
                return fail(
                    runtime::ErrorCode::IndexOutOfRange,
                    "array element index is out of range");
            }
            data[static_cast<std::size_t>(index)] = static_cast<std::int32_t>(value);
            return true;
        }
        return storeElement(
            semantic::PrimitiveType::Int,
            0,
            receiver,
            runtime::Value{index},
            runtime::Value{value});
    }

    [[nodiscard]] bool loadElementDoubleFast(
        const runtime::Value& receiver,
        std::int64_t index,
        double& result) {
        double* data = nullptr;
        std::uint32_t len = 0;
        if (RS_AOT_LIKELY(tryBindPackedDoubleArray(receiver, data, len))) {
            if (RS_AOT_UNLIKELY(static_cast<std::uint64_t>(index) >= len)) {
                return fail(
                    runtime::ErrorCode::IndexOutOfRange,
                    "array element index is out of range");
            }
            result = data[static_cast<std::size_t>(index)];
            return true;
        }
        runtime::Value boxed;
        if (!loadElement(semantic::PrimitiveType::Double, 0, receiver, runtime::Value{index}, boxed)) {
            return false;
        }
        return boxed.tryGetDouble(result);
    }

    [[nodiscard]] bool storeElementDoubleFast(
        const runtime::Value& receiver,
        std::int64_t index,
        double value) {
        double* data = nullptr;
        std::uint32_t len = 0;
        if (RS_AOT_LIKELY(tryBindPackedDoubleArray(receiver, data, len))) {
            if (RS_AOT_UNLIKELY(static_cast<std::uint64_t>(index) >= len)) {
                return fail(
                    runtime::ErrorCode::IndexOutOfRange,
                    "array element index is out of range");
            }
            data[static_cast<std::size_t>(index)] = value;
            return true;
        }
        return storeElement(
            semantic::PrimitiveType::Double,
            0,
            receiver,
            runtime::Value{index},
            runtime::Value{value});
    }

    [[nodiscard]] bool loadElementObjectFast(
        semantic::SymbolId elementTypeId,
        const runtime::Value& receiver,
        std::int64_t index,
        runtime::Value& result) {
        if (receiver.rawTag() == runtime::ValueTag::ObjectRef && heap_) {
            runtime::ArrayFastView view{};
            if (heap_->tryBindArrayFastView(receiver.asObjectRefUnchecked(), view) &&
                view.elementType == semantic::PrimitiveType::Object &&
                view.elementTypeId == elementTypeId &&
                static_cast<std::uint64_t>(index) < view.length) {
                result = view.data[static_cast<std::size_t>(index)];
                return true;
            }
        }
        return loadElement(
            semantic::PrimitiveType::Object,
            elementTypeId,
            receiver,
            runtime::Value{index},
            result);
    }
    [[nodiscard]] bool isTypeObjectFast(
        const runtime::Value& value,
        semantic::SymbolId targetTypeId,
        bool& result) {
        const auto tag = value.rawTag();
        if (tag == runtime::ValueTag::NullObject || tag == runtime::ValueTag::Monostate) {
            result = false;
            return true;
        }
        if (tag == runtime::ValueTag::ObjectRef && heap_) {
            const auto actualTid = heap_->objectTypeIdFast(value.asObjectRefUnchecked());
            if (actualTid != 0) {
                result = (actualTid == targetTypeId) || isAssignable(actualTid, targetTypeId);
                return true;
            }
        }
        runtime::Value boxed;
        if (!typeOperation(
                semantic::PrimitiveType::Object,
                semantic::PrimitiveType::Object,
                targetTypeId,
                value,
                false,
                boxed)) {
            return false;
        }
        result = std::holds_alternative<bool>(boxed) && std::get<bool>(boxed);
        return true;
    }
    [[nodiscard]] bool asTypeObjectFast(
        const runtime::Value& value,
        semantic::SymbolId targetTypeId,
        runtime::Value& result) {
        const auto tag = value.rawTag();
        if (tag == runtime::ValueTag::NullObject || tag == runtime::ValueTag::Monostate) {
            result = runtime::NullObject{};
            return true;
        }
        if (tag == runtime::ValueTag::ObjectRef && heap_) {
            const auto actualTid = heap_->objectTypeIdFast(value.asObjectRefUnchecked());
            if (actualTid != 0) {
                if (actualTid == targetTypeId || isAssignable(actualTid, targetTypeId)) {
                    result = value;
                } else {
                    result = runtime::NullObject{};
                }
                return true;
            }
        }
        return typeOperation(
            semantic::PrimitiveType::Object,
            semantic::PrimitiveType::Object,
            targetTypeId,
            value,
            true,
            result);
    }
    [[nodiscard]] const ProgramDescriptor& program() const noexcept;
    [[nodiscard]] std::shared_ptr<runtime::ManagedHeap> heap() const noexcept;
    void setAsyncRuntime(
        std::shared_ptr<runtime::AsyncRuntimeContext> asyncRuntime,
        std::shared_ptr<void> resumeEnv = nullptr);
    [[nodiscard]] runtime::AsyncRuntimeContext* rawAsyncRuntime() const noexcept {
        return asyncRuntime_.get();
    }
    [[nodiscard]] bool tryEnterResumingCoroutine(
        semantic::SymbolId functionSymbolId,
        std::vector<runtime::Value>& argumentValues,
        std::vector<runtime::Value>& locals,
        std::vector<runtime::Value>& registers,
        std::uint32_t& outResumeBlockId,
        runtime::CoroutineFrameHandle& outActiveCoroutine);
    [[nodiscard]] bool awaitSuspend(
        semantic::SymbolId functionSymbolId,
        semantic::SymbolId returnStructTypeId,
        const runtime::Value& awaitableValue,
        runtime::Value& awaitLocalSlot,
        std::uint32_t resumeBlockId,
        const std::vector<runtime::Value>& argumentValues,
        const std::vector<runtime::Value>& locals,
        const std::vector<runtime::Value>& registers,
        runtime::CoroutineFrameHandle& ioActiveCoroutine,
        bool& outSuspendedNow,
        runtime::Value& outResult);
    [[nodiscard]] bool awaitResume(
        runtime::Value& awaitLocalSlot,
        semantic::PrimitiveType expectedType,
        semantic::SymbolId expectedTypeId,
        runtime::Value* optionalOutResult);
    [[nodiscard]] bool asyncResolve(
        runtime::CoroutineFrameHandle activeCoroutine,
        semantic::SymbolId returnStructTypeId,
        const runtime::Value* optionalPayload,
        runtime::Value& outWrappedResult);
    [[nodiscard]] bool finishUncaughtAsyncException(
        runtime::CoroutineFrameHandle activeCoroutine,
        semantic::SymbolId returnStructTypeId,
        runtime::Value& outResult);

    [[nodiscard]] inline bool isResumingCoroutine(
        semantic::SymbolId functionSymbolId) const noexcept {
        if (RS_AOT_LIKELY(resumingFramePtr_ == nullptr)) return false;
        return resumingCoroutine_.valid() &&
               resumingFramePtr_->functionSymbolId == functionSymbolId;
    }
    [[nodiscard]] inline bool enterResumingScalarCoroutine(
        semantic::SymbolId functionSymbolId,
        void* outScalarBuffer,
        std::size_t scalarByteSize,
        runtime::Value* outRefSlots,
        std::size_t refCount,
        std::uint32_t& outResumeBlockId,
        runtime::CoroutineFrameHandle& outActiveCoroutine) {
        if (RS_AOT_LIKELY(resumingFramePtr_ == nullptr)) return false;
        return enterResumingScalarCoroutineSlow(
            functionSymbolId,
            outScalarBuffer,
            scalarByteSize,
            outRefSlots,
            refCount,
            outResumeBlockId,
            outActiveCoroutine);
    }
    [[nodiscard]] bool enterResumingScalarCoroutineSlow(
        semantic::SymbolId functionSymbolId,
        void* outScalarBuffer,
        std::size_t scalarByteSize,
        runtime::Value* outRefSlots,
        std::size_t refCount,
        std::uint32_t& outResumeBlockId,
        runtime::CoroutineFrameHandle& outActiveCoroutine);
    [[nodiscard]] inline bool canSkipAwaitSuspendFast(
        std::int64_t packedAwaitable) const noexcept {
        const auto h = runtime::AwaitableHandle::unpack64(
            static_cast<std::uint64_t>(packedAwaitable));
        if (h.isCompletedVoidSentinel()) return asyncRuntime_ != nullptr;
        return asyncRuntime_ != nullptr &&
               asyncRuntime_->awaitables().isValid(h) &&
               asyncRuntime_->awaitables().isCompleted(h);
    }
    [[nodiscard]] bool awaitSuspendScalar(
        semantic::SymbolId functionSymbolId,
        semantic::SymbolId returnStructTypeId,
        std::int64_t packedAwaitable,
        std::uint32_t resumeBlockId,
        const void* scalarData,
        std::size_t scalarByteSize,
        const runtime::Value* refSlots,
        std::size_t refCount,
        runtime::CoroutineFrameHandle& ioActiveCoroutine,
        bool& outSuspendedNow,
        std::int64_t& outPackedResult);
    [[nodiscard]] inline bool awaitResumeVoidScalar(
        std::int64_t& ioPackedAwaitable) {
        const auto h = runtime::AwaitableHandle::unpack64(
            static_cast<std::uint64_t>(ioPackedAwaitable));
        if (RS_AOT_LIKELY(h.isCompletedVoidSentinel() && asyncRuntime_ != nullptr)) {
            ioPackedAwaitable = 0;
            return true;
        }
        return awaitResumeVoidScalarSlow(ioPackedAwaitable);
    }
    [[nodiscard]] bool awaitResumeVoidScalarSlow(std::int64_t& ioPackedAwaitable);
    [[nodiscard]] bool awaitResumeInt64Scalar(
        std::int64_t& ioPackedAwaitable,
        semantic::PrimitiveType expectedType,
        std::int64_t& outValue);
    [[nodiscard]] bool awaitResumeDoubleScalar(
        std::int64_t& ioPackedAwaitable,
        double& outValue);
    [[nodiscard]] bool awaitResumeBoolScalar(
        std::int64_t& ioPackedAwaitable,
        bool& outValue);
    [[nodiscard]] bool awaitResumeBoxedScalar(
        std::int64_t& ioPackedAwaitable,
        semantic::PrimitiveType expectedType,
        semantic::SymbolId expectedTypeId,
        runtime::Value& outValue);
    [[nodiscard]] inline bool asyncResolveVoidScalar(
        runtime::CoroutineFrameHandle activeCoroutine,
        std::int64_t& outPackedResult) {
        if (RS_AOT_LIKELY(!activeCoroutine.valid() && asyncRuntime_ != nullptr)) {
            outPackedResult = static_cast<std::int64_t>(
                runtime::AwaitableHandle::completedVoid().pack64());
            return true;
        }
        return asyncResolveVoidScalarSlow(activeCoroutine, outPackedResult);
    }
    [[nodiscard]] bool asyncResolveVoidScalarSlow(
        runtime::CoroutineFrameHandle activeCoroutine,
        std::int64_t& outPackedResult);
    [[nodiscard]] bool asyncResolveIntScalar(
        runtime::CoroutineFrameHandle activeCoroutine,
        std::int64_t payload,
        bool isLong,
        std::int64_t& outPackedResult);
    [[nodiscard]] bool asyncResolveDoubleScalar(
        runtime::CoroutineFrameHandle activeCoroutine,
        double payload,
        std::int64_t& outPackedResult);
    [[nodiscard]] bool asyncResolveBoolScalar(
        runtime::CoroutineFrameHandle activeCoroutine,
        bool payload,
        std::int64_t& outPackedResult);
    [[nodiscard]] bool asyncResolveBoxedScalar(
        runtime::CoroutineFrameHandle activeCoroutine,
        const runtime::Value& payload,
        std::int64_t& outPackedResult);
    [[nodiscard]] bool finishUncaughtAsyncExceptionScalar(
        runtime::CoroutineFrameHandle activeCoroutine,
        semantic::SymbolId returnStructTypeId,
        std::int64_t& outPackedResult);

    // Zero-boxing TrustedRelease inline intrinsics for RealScript.Async::Async.__*
    [[nodiscard]] inline bool asyncIntrinsicYieldFast(
        std::int64_t tokenPacked,
        std::int64_t& outHandle) {
        if (RS_AOT_UNLIKELY(!asyncRuntime_)) {
            return fail(runtime::ErrorCode::InvalidProgram, "Async intrinsic requires AsyncRuntimeContext");
        }
        const auto h = asyncRuntime_->enqueueYield(
            runtime::CancellationToken::unpack64(static_cast<std::uint64_t>(tokenPacked)));
        outHandle = static_cast<std::int64_t>(h.pack64());
        return true;
    }
    [[nodiscard]] inline bool asyncIntrinsicNewPendingFast(
        std::int64_t tokenPacked,
        std::int64_t& outHandle) {
        if (RS_AOT_UNLIKELY(!asyncRuntime_)) {
            return fail(runtime::ErrorCode::InvalidProgram, "Async intrinsic requires AsyncRuntimeContext");
        }
        const auto h = asyncRuntime_->awaitables().allocatePending(
            runtime::ResumeQueue::Update,
            runtime::CancellationToken::unpack64(static_cast<std::uint64_t>(tokenPacked)));
        outHandle = static_cast<std::int64_t>(h.pack64());
        return true;
    }
    [[nodiscard]] inline bool asyncIntrinsicFromCanceledFast(std::int64_t& outHandle) {
        if (RS_AOT_UNLIKELY(!asyncRuntime_)) {
            return fail(runtime::ErrorCode::InvalidProgram, "Async intrinsic requires AsyncRuntimeContext");
        }
        outHandle = static_cast<std::int64_t>(asyncRuntime_->awaitables().fromCanceled().pack64());
        return true;
    }
    [[nodiscard]] inline bool asyncIntrinsicCompleteVoidFast(
        std::int64_t handlePacked,
        bool& outOk) {
        if (RS_AOT_UNLIKELY(!asyncRuntime_)) {
            return fail(runtime::ErrorCode::InvalidProgram, "Async intrinsic requires AsyncRuntimeContext");
        }
        outOk = asyncRuntime_->awaitables().trySetResult(
            runtime::AwaitableHandle::unpack64(static_cast<std::uint64_t>(handlePacked)),
            runtime::Value{});
        return true;
    }
    [[nodiscard]] inline bool asyncIntrinsicCompleteIntFast(
        std::int64_t handlePacked,
        std::int64_t value,
        bool& outOk) {
        if (RS_AOT_UNLIKELY(!asyncRuntime_)) {
            return fail(runtime::ErrorCode::InvalidProgram, "Async intrinsic requires AsyncRuntimeContext");
        }
        outOk = asyncRuntime_->awaitables().trySetResult(
            runtime::AwaitableHandle::unpack64(static_cast<std::uint64_t>(handlePacked)),
            runtime::Value{value});
        return true;
    }
    [[nodiscard]] inline bool asyncIntrinsicCompleteCancelFast(
        std::int64_t handlePacked,
        bool& outOk) {
        if (RS_AOT_UNLIKELY(!asyncRuntime_)) {
            return fail(runtime::ErrorCode::InvalidProgram, "Async intrinsic requires AsyncRuntimeContext");
        }
        outOk = asyncRuntime_->awaitables().trySetCanceled(
            runtime::AwaitableHandle::unpack64(static_cast<std::uint64_t>(handlePacked)));
        return true;
    }
    [[nodiscard]] inline bool asyncIntrinsicIsCompletedFast(
        std::int64_t handlePacked,
        bool& outDone) {
        if (RS_AOT_UNLIKELY(!asyncRuntime_)) {
            return fail(runtime::ErrorCode::InvalidProgram, "Async intrinsic requires AsyncRuntimeContext");
        }
        outDone = asyncRuntime_->awaitables().isCompleted(
            runtime::AwaitableHandle::unpack64(static_cast<std::uint64_t>(handlePacked)));
        return true;
    }
    [[nodiscard]] inline bool asyncIntrinsicCtsCreateFast(std::int64_t& outHandle) {
        if (RS_AOT_UNLIKELY(!asyncRuntime_)) {
            return fail(runtime::ErrorCode::InvalidProgram, "Async intrinsic requires AsyncRuntimeContext");
        }
        outHandle = static_cast<std::int64_t>(asyncRuntime_->cancellations().createSource().pack64());
        return true;
    }
    [[nodiscard]] inline bool asyncIntrinsicCtsCreateLinkedFast(
        std::int64_t parentPacked,
        std::int64_t& outHandle) {
        if (RS_AOT_UNLIKELY(!asyncRuntime_)) {
            return fail(runtime::ErrorCode::InvalidProgram, "Async intrinsic requires AsyncRuntimeContext");
        }
        const auto p = runtime::CancellationToken::unpack64(static_cast<std::uint64_t>(parentPacked));
        outHandle = static_cast<std::int64_t>(
            asyncRuntime_->cancellations().createLinkedSource(&p, 1u).pack64());
        return true;
    }
    [[nodiscard]] inline bool asyncIntrinsicCtsCreateLinked2Fast(
        std::int64_t parentAPacked,
        std::int64_t parentBPacked,
        std::int64_t& outHandle) {
        if (RS_AOT_UNLIKELY(!asyncRuntime_)) {
            return fail(runtime::ErrorCode::InvalidProgram, "Async intrinsic requires AsyncRuntimeContext");
        }
        const std::array<runtime::CancellationToken, 2> parents{{
            runtime::CancellationToken::unpack64(static_cast<std::uint64_t>(parentAPacked)),
            runtime::CancellationToken::unpack64(static_cast<std::uint64_t>(parentBPacked)),
        }};
        outHandle = static_cast<std::int64_t>(
            asyncRuntime_->cancellations().createLinkedSource(parents.data(), parents.size()).pack64());
        return true;
    }
    [[nodiscard]] inline bool asyncIntrinsicCtsCreateTimeoutFast(
        double timeoutSeconds,
        std::int64_t& outHandle) {
        if (RS_AOT_UNLIKELY(!asyncRuntime_)) {
            return fail(runtime::ErrorCode::InvalidProgram, "Async intrinsic requires AsyncRuntimeContext");
        }
        outHandle = static_cast<std::int64_t>(
            asyncRuntime_->createTimeoutSource(timeoutSeconds, false).pack64());
        return true;
    }
    [[nodiscard]] inline bool asyncIntrinsicCtsCreateLinkedTimeoutFast(
        std::int64_t parentPacked,
        double timeoutSeconds,
        std::int64_t& outHandle) {
        if (RS_AOT_UNLIKELY(!asyncRuntime_)) {
            return fail(runtime::ErrorCode::InvalidProgram, "Async intrinsic requires AsyncRuntimeContext");
        }
        const auto p = runtime::CancellationToken::unpack64(static_cast<std::uint64_t>(parentPacked));
        outHandle = static_cast<std::int64_t>(
            asyncRuntime_->createLinkedTimeoutSource(&p, 1u, timeoutSeconds, false).pack64());
        return true;
    }
    [[nodiscard]] inline bool asyncIntrinsicCtsTokenFast(
        std::int64_t ctsPacked,
        std::int64_t& outTokPacked) {
        if (RS_AOT_UNLIKELY(!asyncRuntime_)) {
            return fail(runtime::ErrorCode::InvalidProgram, "Async intrinsic requires AsyncRuntimeContext");
        }
        const auto tok = asyncRuntime_->cancellations().token(
            runtime::CancellationSourceHandle::unpack64(static_cast<std::uint64_t>(ctsPacked)));
        outTokPacked = static_cast<std::int64_t>(tok.pack64());
        return true;
    }
    [[nodiscard]] inline bool asyncIntrinsicCtsCancelFast(
        std::int64_t ctsPacked,
        std::int64_t& outCount) {
        if (RS_AOT_UNLIKELY(!asyncRuntime_)) {
            return fail(runtime::ErrorCode::InvalidProgram, "Async intrinsic requires AsyncRuntimeContext");
        }
        outCount = static_cast<std::int64_t>(asyncRuntime_->cancellations().cancel(
            runtime::CancellationSourceHandle::unpack64(static_cast<std::uint64_t>(ctsPacked))));
        return true;
    }
    [[nodiscard]] inline bool asyncIntrinsicCtsReleaseFast(
        std::int64_t ctsPacked,
        bool& outOk) {
        if (RS_AOT_UNLIKELY(!asyncRuntime_)) {
            return fail(runtime::ErrorCode::InvalidProgram, "Async intrinsic requires AsyncRuntimeContext");
        }
        outOk = asyncRuntime_->cancellations().releaseSource(
            runtime::CancellationSourceHandle::unpack64(static_cast<std::uint64_t>(ctsPacked)));
        return true;
    }
    [[nodiscard]] inline bool asyncIntrinsicTokenIsCanceledFast(
        std::int64_t tokPacked,
        bool& outCanceled) {
        if (RS_AOT_UNLIKELY(!asyncRuntime_)) {
            return fail(runtime::ErrorCode::InvalidProgram, "Async intrinsic requires AsyncRuntimeContext");
        }
        outCanceled = asyncRuntime_->cancellations().isCancellationRequested(
            runtime::CancellationToken::unpack64(static_cast<std::uint64_t>(tokPacked)));
        return true;
    }
    [[nodiscard]] inline bool asyncIntrinsicTokenIsTimedOutFast(
        std::int64_t tokPacked,
        bool& outTimedOut) {
        if (RS_AOT_UNLIKELY(!asyncRuntime_)) {
            return fail(runtime::ErrorCode::InvalidProgram, "Async intrinsic requires AsyncRuntimeContext");
        }
        outTimedOut = asyncRuntime_->cancellations().isTimedOut(
            runtime::CancellationToken::unpack64(static_cast<std::uint64_t>(tokPacked)));
        return true;
    }
    [[nodiscard]] inline bool asyncIntrinsicAdvanceTimeoutsFast(
        double deltaSeconds,
        std::int64_t& outFired) {
        if (RS_AOT_UNLIKELY(!asyncRuntime_)) {
            return fail(runtime::ErrorCode::InvalidProgram, "Async intrinsic requires AsyncRuntimeContext");
        }
        outFired = static_cast<std::int64_t>(asyncRuntime_->advanceTimeouts(deltaSeconds));
        return true;
    }
    [[nodiscard]] inline bool asyncIntrinsicStepUpdateFast(
        std::int64_t maxCount,
        std::int64_t& outDrained) {
        if (RS_AOT_UNLIKELY(!asyncRuntime_)) {
            return fail(runtime::ErrorCode::InvalidProgram, "Async intrinsic requires AsyncRuntimeContext");
        }
        outDrained = static_cast<std::int64_t>(asyncRuntime_->stepResumeQueue(
            runtime::ResumeQueue::Update,
            maxCount > 0 ? static_cast<std::size_t>(maxCount) : 0u));
        return true;
    }
    [[nodiscard]] inline bool asyncIntrinsicReadyUpdateCountFast(std::int64_t& outReady) {
        if (RS_AOT_UNLIKELY(!asyncRuntime_)) {
            return fail(runtime::ErrorCode::InvalidProgram, "Async intrinsic requires AsyncRuntimeContext");
        }
        outReady = static_cast<std::int64_t>(
            asyncRuntime_->awaitables().readyContinuationCount(runtime::ResumeQueue::Update));
        return true;
    }
    [[nodiscard]] runtime::ShadowStack& shadowStack() noexcept;
    [[nodiscard]] const runtime::RuntimeError& error() const noexcept;
    [[nodiscard]] const runtime::RuntimeStatistics& statistics() const noexcept;
    [[nodiscard]] std::uint64_t instructionsExecuted() const noexcept;
    [[nodiscard]] bool finalizeDeterminism(
        bool succeeded,
        const runtime::Value& value);
    [[nodiscard]] std::uint64_t determinismDigest() const noexcept;
    [[nodiscard]] std::size_t replayEntriesConsumed() const noexcept;
    [[nodiscard]] bool isAssignable(
        semantic::SymbolId actual,
        semantic::SymbolId expected) const noexcept;

private:
    friend class FrameScope;
    friend class StackRefScope;
    [[nodiscard]] const TypeDescriptor* findType(semantic::SymbolId typeId) const;
    [[nodiscard]] const FunctionDescriptor* findFunction(
        semantic::SymbolId symbolId) const;
    void emitTrace(
        runtime::TraceEventKind kind,
        runtime::DeterminismOperationId operationId = 0,
        std::string_view operation = {});

    const ProgramDescriptor* program_ = nullptr;
    std::shared_ptr<runtime::ManagedHeap> heap_;
    std::shared_ptr<const runtime::BindingRegistry> bindings_;
    runtime::ExecutionOptions options_;
    bool traceEventsEnabled_ = false;
    bool determinismEventsEnabled_ = false;
    bool determinismOnly_ = false;
    bool diagnosticEventsEnabled_ = false;
    bool fastAccounting_ = false;
    std::uint64_t determinismEventDigest_ = runtime::DeterminismEventSeed;
    std::uint64_t determinismEventCount_ = 0;
    std::unordered_map<semantic::SymbolId, runtime::FunctionProfile>
        profileFunctions_;
    std::uint64_t profileEvents_ = 0;
    runtime::RuntimeError error_;
    runtime::RuntimeStatistics statistics_;
    runtime::DeterminismSession determinism_;
    std::uint64_t executed_ = 0;
    struct FrameSite {
        semantic::SymbolId symbolId = 0;
        std::uint32_t generatedLine = 0;
    };
    [[nodiscard]] std::vector<std::string> materializeStackTrace() const;
    [[nodiscard]] std::vector<runtime::StackTraceLocation> materializeSourceLocations() const;
    [[nodiscard]] std::vector<std::string> materializeSymbolizedStackTrace() const;
    void populateErrorStackTrace(std::uint32_t activeGeneratedLine = 0);
    std::vector<const char*> stack_;
    std::vector<FrameSite> stackSites_;
    semantic::SymbolId currentFunctionId_ = 0;
    runtime::ShadowStack shadowStack_;
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
    struct AotDelegateCacheEntry {
        ResolvedAotDelegateLeaf resolved{};
        runtime::ObjectRef delRef{};
        runtime::ObjectRef recvRef{};
        semantic::SymbolId delegateTypeId = 0;
        bool hasObjectReceiver = false;
        bool isMulticast = false;
        bool valid = false;
    };
    std::array<AotDelegateCacheEntry, 16> delLeafCache_{};
    struct CachedMulticastItem {
        ResolvedAotDelegateLeaf leaf{};
        runtime::ObjectRef leafRef{};
        runtime::ObjectRef recvRef{};
        bool hasObjRecv = false;
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    semantic::SymbolId lastVerifiedDelegateTypeId_ = 0;
    semantic::SymbolId cachedMulticastTypeId_ = 0;
    runtime::ObjectRef cachedMulticastRef_{};
    std::vector<CachedMulticastItem> cachedMulticastMeta_;
    std::vector<ResolvedAotDelegateLeaf> cachedMulticastLeaves_;
    std::uint32_t cachedMulticastPinCount_ = 0;
    std::unordered_map<semantic::SymbolId, const TypeDescriptor*> ownedTypes_;
    std::unordered_map<semantic::SymbolId, const FunctionDescriptor*> ownedFunctions_;
    const std::unordered_map<semantic::SymbolId, const TypeDescriptor*>* typesPtr_ = nullptr;
    const std::unordered_map<semantic::SymbolId, const FunctionDescriptor*>* functionsPtr_ = nullptr;
    struct AssignableCacheEntry {
        semantic::SymbolId actual = 0;
        semantic::SymbolId expected = 0;
        std::uint8_t result = 0; // 0 = empty, 1 = false, 2 = true
    };
    mutable std::array<AssignableCacheEntry, 32> assignableCache_{};
    std::optional<runtime::Value> pendingException_;
    std::shared_ptr<runtime::AsyncRuntimeContext> asyncRuntime_;
    std::shared_ptr<void> resumeEnv_;
    runtime::CoroutineFrameHandle resumingCoroutine_{};
    runtime::CoroutineFrame* resumingFramePtr_ = nullptr;
    std::vector<runtime::Value> stackRefEmptyRoot_;
    std::vector<runtime::Value> stackRefSlots_;
    std::uint32_t stackRefTop_ = 0;
    std::uint32_t activeStackRefScopes_ = 0;
    std::uint8_t stackRefBankId_ = 0;
    bool stackRefRootPushed_ = false;
};

class Program {
public:
    explicit Program(const ProgramDescriptor& descriptor);

    void setBindings(std::shared_ptr<const runtime::BindingRegistry> bindings);
    void setHeap(std::shared_ptr<runtime::ManagedHeap> heap);
    [[nodiscard]] std::shared_ptr<runtime::ManagedHeap> heap() const noexcept;
    void setAsyncRuntime(std::shared_ptr<runtime::AsyncRuntimeContext> asyncRuntime);
    [[nodiscard]] std::shared_ptr<runtime::AsyncRuntimeContext> asyncRuntime() const noexcept;
    [[nodiscard]] const ProgramDescriptor& descriptor() const noexcept;
    [[nodiscard]] bool hasFunction(semantic::SymbolId symbolId) const noexcept {
        return functions_.find(symbolId) != functions_.end();
    }
    [[nodiscard]] std::optional<runtime::StackTraceLocation> resolveSourceLocation(
        semantic::SymbolId symbolId,
        std::uint32_t generatedLine = 0) const;
    [[nodiscard]] std::optional<runtime::StackTraceLocation> resolveSourceLocation(
        const std::string& qualifiedName,
        std::uint32_t generatedLine = 0) const;
    [[nodiscard]] std::vector<std::string> symbolizeStackTrace(
        const runtime::RuntimeError& error) const;

    [[nodiscard]] runtime::ExecutionResult invoke(
        semantic::SymbolId symbolId,
        const std::vector<runtime::Value>& arguments = {},
        runtime::ExecutionOptions options = {}) const;
    [[nodiscard]] runtime::ExecutionResult invoke(
        const std::string& qualifiedName,
        const std::vector<runtime::Value>& arguments = {},
        runtime::ExecutionOptions options = {}) const;

private:
    const ProgramDescriptor* descriptor_ = nullptr;
    std::shared_ptr<const runtime::BindingRegistry> bindings_;
    std::shared_ptr<runtime::ManagedHeap> heap_;
    std::shared_ptr<runtime::AsyncRuntimeContext> asyncRuntime_;
    mutable std::shared_ptr<void> resumeEnv_;
    std::unordered_map<std::string, semantic::SymbolId> names_;
    std::unordered_map<semantic::SymbolId, const TypeDescriptor*> types_;
    std::unordered_map<semantic::SymbolId, const FunctionDescriptor*> functions_;
};

// Typed host binding adapters. They keep generated/native call sites on stable
// SymbolIds while avoiding a Variant[]-style reflection layer in user code.
template <typename T>
struct NativeValue;

template <>
struct NativeValue<bool> {
    static bool read(const runtime::Value& value, bool& output) {
        const auto* typed = std::get_if<bool>(&value);
        if (!typed) return false;
        output = *typed;
        return true;
    }
    static runtime::Value write(bool value) { return value; }
};

template <>
struct NativeValue<std::int32_t> {
    static bool read(const runtime::Value& value, std::int32_t& output) {
        const auto* typed = std::get_if<std::int64_t>(&value);
        if (!typed || *typed < INT32_MIN || *typed > INT32_MAX) return false;
        output = static_cast<std::int32_t>(*typed);
        return true;
    }
    static runtime::Value write(std::int32_t value) {
        return static_cast<std::int64_t>(value);
    }
};

template <>
struct NativeValue<std::int64_t> {
    static bool read(const runtime::Value& value, std::int64_t& output) {
        const auto* typed = std::get_if<runtime::LongValue>(&value);
        if (!typed) return false;
        output = typed->value;
        return true;
    }
    static runtime::Value write(std::int64_t value) {
        return runtime::LongValue{value};
    }
};

template <>
struct NativeValue<double> {
    static bool read(const runtime::Value& value, double& output) {
        const auto* typed = std::get_if<double>(&value);
        if (!typed) return false;
        output = *typed;
        return true;
    }
    static runtime::Value write(double value) { return value; }
};


template <>
struct NativeValue<std::string> {
    static bool read(const runtime::Value& value, std::string& output) {
        const auto* typed = std::get_if<std::string>(&value);
        if (!typed) return false;
        output = *typed;
        return true;
    }
    static runtime::Value write(std::string value) { return value; }
};

template <>
struct NativeValue<runtime::EnumValue> {
    static bool read(const runtime::Value& value, runtime::EnumValue& output) {
        return value.tryGetEnum(output);
    }
    static runtime::Value write(runtime::EnumValue value) { return value; }
};

template <>
struct NativeValue<runtime::ObjectRef> {
    static bool read(const runtime::Value& value, runtime::ObjectRef& output) {
        const auto* typed = std::get_if<runtime::ObjectRef>(&value);
        if (!typed) return false;
        output = *typed;
        return true;
    }
    static runtime::Value write(runtime::ObjectRef value) { return value; }
};

template <>
struct NativeValue<runtime::NativeHandle> {
    static bool read(const runtime::Value& value, runtime::NativeHandle& output) {
        return value.tryGetNativeHandle(output);
    }
    static runtime::Value write(runtime::NativeHandle value) { return value; }
};

template <>
struct NativeValue<runtime::StructValue> {
    static bool read(const runtime::Value& value, runtime::StructValue& output) {
        const auto* typed = std::get_if<runtime::StructValue>(&value);
        if (!typed) return false;
        output = *typed;
        return true;
    }
    static runtime::Value write(runtime::StructValue value) {
        return value;
    }
};

template <typename Result, typename... Args, std::size_t... I>
std::optional<runtime::Value> invokeNativeTypedImpl(
    Result (*function)(Args...),
    runtime::ValueSpan arguments,
    runtime::RuntimeError& error,
    std::index_sequence<I...>) {
    if (arguments.size() != sizeof...(Args)) {
        error.code = runtime::ErrorCode::InvalidArguments;
        error.message = "native thunk argument count mismatch";
        return std::nullopt;
    }
    std::tuple<std::decay_t<Args>...> converted;
    const bool valid = (NativeValue<std::decay_t<Args>>::read(
        arguments[I], std::get<I>(converted)) && ...);
    if (!valid) {
        error.code = runtime::ErrorCode::TypeMismatch;
        error.message = "native thunk argument type mismatch";
        return std::nullopt;
    }
    if constexpr (std::is_void_v<Result>) {
        std::apply(function, converted);
        return runtime::Value{};
    } else {
        return NativeValue<std::decay_t<Result>>::write(
            std::apply(function, converted));
    }
}

template <typename Result, typename... Args>
runtime::ExternalFunction makeNativeThunk(Result (*function)(Args...)) {
    return [function](
        const bytecode::FunctionReference&,
        runtime::ValueSpan arguments,
        runtime::RuntimeError& error) -> std::optional<runtime::Value> {
        try {
            return invokeNativeTypedImpl(
                function,
                arguments,
                error,
                std::index_sequence_for<Args...>{});
        } catch (const std::exception& exception) {
            error.code = runtime::ErrorCode::ExternalFunctionUnresolved;
            error.message = std::string("native thunk threw: ") + exception.what();
            return std::nullopt;
        } catch (...) {
            error.code = runtime::ErrorCode::ExternalFunctionUnresolved;
            error.message = "native thunk threw an unknown exception";
            return std::nullopt;
        }
    };
}


inline std::int64_t typedWrapInt32(std::int64_t value) noexcept {
    const auto bits = static_cast<std::uint32_t>(value);
    return bits <= static_cast<std::uint32_t>(
                       std::numeric_limits<std::int32_t>::max())
        ? static_cast<std::int64_t>(bits)
        : static_cast<std::int64_t>(bits) - (std::int64_t{1} << 32);
}

inline std::int64_t typedSignedFromBits(std::uint64_t bits) noexcept {
    return bits <= static_cast<std::uint64_t>(
                       std::numeric_limits<std::int64_t>::max())
        ? static_cast<std::int64_t>(bits)
        : -1 - static_cast<std::int64_t>(
            std::numeric_limits<std::uint64_t>::max() - bits);
}

template <bool Checked>
[[nodiscard]] inline bool typedAddLong(
    std::int64_t left,
    std::int64_t right,
    std::int64_t& out) noexcept {
    if constexpr (Checked) {
        if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
            (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right)) {
            return false;
        }
        out = left + right;
        return true;
    } else {
        out = typedSignedFromBits(
            static_cast<std::uint64_t>(left) +
            static_cast<std::uint64_t>(right));
        return true;
    }
}

template <bool Checked>
[[nodiscard]] inline bool typedSubLong(
    std::int64_t left,
    std::int64_t right,
    std::int64_t& out) noexcept {
    if constexpr (Checked) {
        if ((right < 0 && left > std::numeric_limits<std::int64_t>::max() + right) ||
            (right > 0 && left < std::numeric_limits<std::int64_t>::min() + right)) {
            return false;
        }
        out = left - right;
        return true;
    } else {
        out = typedSignedFromBits(
            static_cast<std::uint64_t>(left) -
            static_cast<std::uint64_t>(right));
        return true;
    }
}

template <bool Checked>
[[nodiscard]] inline bool typedMulLong(
    std::int64_t left,
    std::int64_t right,
    std::int64_t& out) noexcept {
    if constexpr (Checked) {
        if (left == 0 || right == 0) {
            out = 0;
            return true;
        }
        const auto minimum = std::numeric_limits<std::int64_t>::min();
        const auto maximum = std::numeric_limits<std::int64_t>::max();
        bool overflow = false;
        if (left > 0) {
            overflow = right > 0 ? left > maximum / right : right < minimum / left;
        } else {
            overflow = right > 0 ? left < minimum / right
                                 : (left != 0 && right < maximum / left);
        }
        if (overflow) return false;
        out = left * right;
        return true;
    } else {
        out = typedSignedFromBits(
            static_cast<std::uint64_t>(left) *
            static_cast<std::uint64_t>(right));
        return true;
    }
}

[[nodiscard]] inline runtime::Value typedBoxScalar(
    semantic::PrimitiveType type,
    bool value) noexcept {
    (void)type;
    return runtime::Value{value};
}

[[nodiscard]] inline runtime::Value typedBoxScalar(
    semantic::PrimitiveType type,
    std::int64_t value) noexcept {
    if (type == semantic::PrimitiveType::Long) {
        return runtime::Value{runtime::LongValue{value}};
    }
    return runtime::Value{value};
}

[[nodiscard]] inline runtime::Value typedBoxScalar(
    semantic::PrimitiveType type,
    float value) noexcept {
    (void)type;
    return runtime::Value{runtime::FloatValue{value}};
}

[[nodiscard]] inline runtime::Value typedBoxScalar(
    semantic::PrimitiveType type,
    double value) noexcept {
    (void)type;
    return runtime::Value{value};
}

[[nodiscard]] inline runtime::Value typedBoxScalar(
    semantic::PrimitiveType type,
    runtime::FixedValue value) noexcept {
    (void)type;
    return runtime::Value{value};
}

[[nodiscard]] inline runtime::Value typedBoxScalar(
    semantic::PrimitiveType type,
    runtime::Fixed16Value value) noexcept {
    (void)type;
    return runtime::Value{value};
}

[[nodiscard]] inline runtime::Value typedBoxScalar(
    semantic::PrimitiveType type,
    runtime::Vec2Value value) noexcept {
    (void)type;
    return runtime::Value{value};
}

[[nodiscard]] inline runtime::Value typedBoxScalar(
    semantic::PrimitiveType type,
    runtime::Vec3Value value) noexcept {
    (void)type;
    return runtime::Value{value};
}

[[nodiscard]] inline runtime::Value typedBoxScalar(
    semantic::PrimitiveType type,
    runtime::QuatValue value) noexcept {
    (void)type;
    return runtime::Value{value};
}

[[nodiscard]] inline runtime::Value typedBoxScalar(
    semantic::PrimitiveType type,
    runtime::Fixed64Value value) noexcept {
    (void)type;
    return runtime::Value{value};
}

[[nodiscard]] inline runtime::Value typedBoxScalar(
    semantic::PrimitiveType type,
    runtime::Vec2fValue value) noexcept {
    (void)type;
    return runtime::Value{value};
}

[[nodiscard]] inline runtime::Value typedBoxScalar(
    semantic::PrimitiveType type,
    runtime::Vec3fValue value) noexcept {
    (void)type;
    return runtime::Value{value};
}

[[nodiscard]] inline runtime::Value typedBoxScalar(
    semantic::PrimitiveType type,
    runtime::QuatfValue value) noexcept {
    (void)type;
    return runtime::Value{value};
}

[[nodiscard]] inline runtime::Value typedBoxScalar(
    semantic::PrimitiveType type,
    runtime::EnumValue value) noexcept {
    (void)type;
    return runtime::Value{value};
}

[[nodiscard]] inline runtime::Value typedBoxScalar(
    semantic::PrimitiveType type,
    const runtime::Value& value) {
    if (std::holds_alternative<std::monostate>(value)) {
        if (type == semantic::PrimitiveType::Object) {
            return runtime::Value{runtime::NullObject{}};
        }
        if (type == semantic::PrimitiveType::Array) {
            return runtime::Value{runtime::NullArray{}};
        }
        if (type == semantic::PrimitiveType::String) {
            return runtime::Value{runtime::NullString{}};
        }
    }
    return value;
}

struct EmptyGcGuards {};

inline void retainTypedGcSlot(
    runtime::ManagedHeap* rawH,
    runtime::PersistentRoot& slot,
    const runtime::Value& value) {
    if (rawH != nullptr &&
        (std::holds_alternative<runtime::ObjectRef>(value) ||
         value.rawTag() == runtime::ValueTag::String)) {
        if (std::holds_alternative<runtime::ObjectRef>(value) &&
            std::get<runtime::ObjectRef>(value).isStackSlotRef()) {
            if (slot.valid()) slot.reset();
            return;
        }
        if (!slot.valid() || !slot.update(value)) {
            slot = rawH->retain(value);
        }
    } else if (slot.valid()) {
        slot.reset();
    }
}

inline void retainTypedGcSlot(
    runtime::ManagedHeap* rawH,
    runtime::PersistentRoot* firstSlot,
    const runtime::Value& value) {
    if (firstSlot != nullptr) {
        retainTypedGcSlot(rawH, *firstSlot, value);
    }
}

template <typename T, typename = std::enable_if_t<T::kIsGeneratedAotStruct>>
inline void retainTypedGcSlot(
    runtime::ManagedHeap* rawH,
    runtime::PersistentRoot* firstSlot,
    const T& value) {
    if constexpr (T::kGcSlotCount > 0) {
        if (rawH != nullptr && firstSlot != nullptr) {
            T::retainGcSlots(rawH, firstSlot, value);
        }
    } else {
        (void)rawH;
        (void)firstSlot;
        (void)value;
    }
}

[[nodiscard]] inline bool equalStringValuesNoThrow(
    const runtime::ManagedHeap* rawH,
    const runtime::Value& left,
    const runtime::Value& right) noexcept {
    if (std::holds_alternative<runtime::NullString>(left) ||
        std::holds_alternative<runtime::NullString>(right)) {
        return std::holds_alternative<runtime::NullString>(left) &&
            std::holds_alternative<runtime::NullString>(right);
    }
    try {
        const auto extractText = [&](const runtime::Value& value)
            -> std::optional<std::string_view> {
            const auto tag = value.rawTag();
            if (tag == runtime::ValueTag::String) {
                return std::string_view(value.asStringUnchecked().str());
            }
            if (tag == runtime::ValueTag::ObjectRef) {
                return rawH
                    ? rawH->stringView(value.asObjectRefUnchecked())
                    : std::nullopt;
            }
            return std::nullopt;
        };
        const auto a = extractText(left);
        const auto b = extractText(right);
        return a && b && (*a == *b);
    } catch (...) {
        return false;
    }
}

struct UnboxedCustomStruct {
    semantic::SymbolId typeId = 0;
    std::array<runtime::Value, 16> fields{};
    std::uint8_t fieldCount = 0;

    UnboxedCustomStruct() = default;
    explicit UnboxedCustomStruct(semantic::SymbolId id) noexcept : typeId(id) {}

    UnboxedCustomStruct(const UnboxedCustomStruct& other)
        : typeId(other.typeId), fieldCount(other.fieldCount) {
        for (std::uint8_t i = 0; i < fieldCount; ++i) {
            fields[i] = other.fields[i];
        }
    }

    UnboxedCustomStruct& operator=(const UnboxedCustomStruct& other) {
        if (this != &other) {
            typeId = other.typeId;
            fieldCount = other.fieldCount;
            for (std::uint8_t i = 0; i < fieldCount; ++i) {
                fields[i] = other.fields[i];
            }
        }
        return *this;
    }

    UnboxedCustomStruct(UnboxedCustomStruct&& other) noexcept
        : typeId(other.typeId), fieldCount(other.fieldCount) {
        for (std::uint8_t i = 0; i < fieldCount; ++i) {
            fields[i] = std::move(other.fields[i]);
        }
    }

    UnboxedCustomStruct& operator=(UnboxedCustomStruct&& other) noexcept {
        if (this != &other) {
            typeId = other.typeId;
            fieldCount = other.fieldCount;
            for (std::uint8_t i = 0; i < fieldCount; ++i) {
                fields[i] = std::move(other.fields[i]);
            }
        }
        return *this;
    }

    friend bool operator==(const UnboxedCustomStruct& left, const UnboxedCustomStruct& right) {
        if (left.typeId != right.typeId || left.fieldCount != right.fieldCount) return false;
        for (std::uint8_t i = 0; i < left.fieldCount; ++i) {
            if (!(left.fields[i] == right.fields[i])) return false;
        }
        return true;
    }
    friend bool operator!=(const UnboxedCustomStruct& left, const UnboxedCustomStruct& right) {
        return !(left == right);
    }
};

[[nodiscard]] inline runtime::Value typedBoxScalar(
    semantic::PrimitiveType type,
    const UnboxedCustomStruct& value) {
    (void)type;
    return runtime::Value{runtime::makeStructValueFromFields(
        value.typeId,
        runtime::ValueSpan(value.fields.data(), value.fieldCount))};
}

template <typename T, typename = std::enable_if_t<T::kIsGeneratedAotStruct>>
[[nodiscard]] inline runtime::Value typedBoxScalar(
    semantic::PrimitiveType,
    const T& value) {
    return value.boxToValue();
}

[[nodiscard]] inline bool typedUnboxScalar(
    semantic::PrimitiveType type,
    const runtime::Value& value,
    bool& out) noexcept {
    (void)type;
    if (const auto* p = std::get_if<bool>(&value)) {
        out = *p;
        return true;
    }
    return false;
}

[[nodiscard]] inline bool typedUnboxScalar(
    semantic::PrimitiveType type,
    const runtime::Value& value,
    std::int64_t& out) noexcept {
    if (type == semantic::PrimitiveType::Long) {
        if (const auto* p = std::get_if<runtime::LongValue>(&value)) {
            out = p->value;
            return true;
        }
        return false;
    }
    if (const auto* p = std::get_if<std::int64_t>(&value)) {
        out = *p;
        return true;
    }
    return false;
}

[[nodiscard]] inline bool typedUnboxScalar(
    semantic::PrimitiveType type,
    const runtime::Value& value,
    float& out) noexcept {
    (void)type;
    if (const auto* p = std::get_if<runtime::FloatValue>(&value)) {
        out = p->value;
        return true;
    }
    return false;
}

[[nodiscard]] inline bool typedUnboxScalar(
    semantic::PrimitiveType type,
    const runtime::Value& value,
    double& out) noexcept {
    (void)type;
    if (const auto* p = std::get_if<double>(&value)) {
        out = *p;
        return true;
    }
    return false;
}

[[nodiscard]] inline bool typedUnboxScalar(
    semantic::PrimitiveType type,
    const runtime::Value& value,
    runtime::FixedValue& out) noexcept {
    (void)type;
    if (const auto* p = std::get_if<runtime::FixedValue>(&value)) {
        out = *p;
        return true;
    }
    if (const auto f0 = runtime::loadInlineStructField(value, 0)) {
        if (const auto* i = std::get_if<std::int64_t>(&*f0)) {
            out = runtime::FixedValue{static_cast<std::int32_t>(*i)};
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline bool typedUnboxScalar(
    semantic::PrimitiveType type,
    const runtime::Value& value,
    runtime::Fixed16Value& out) noexcept {
    (void)type;
    if (const auto* p = std::get_if<runtime::Fixed16Value>(&value)) {
        out = *p;
        return true;
    }
    return false;
}

[[nodiscard]] inline bool typedUnboxScalar(
    semantic::PrimitiveType type,
    const runtime::Value& value,
    runtime::Vec2Value& out) noexcept {
    (void)type;
    if (const auto* p = std::get_if<runtime::Vec2Value>(&value)) {
        out = *p;
        return true;
    }
    return false;
}

[[nodiscard]] inline bool typedUnboxScalar(
    semantic::PrimitiveType type,
    const runtime::Value& value,
    runtime::Vec3Value& out) noexcept {
    (void)type;
    if (const auto* p = std::get_if<runtime::Vec3Value>(&value)) {
        out = *p;
        return true;
    }
    return false;
}

[[nodiscard]] inline bool typedUnboxScalar(
    semantic::PrimitiveType type,
    const runtime::Value& value,
    runtime::QuatValue& out) noexcept {
    (void)type;
    return value.tryGetQuat(out);
}

[[nodiscard]] inline bool typedUnboxScalar(
    semantic::PrimitiveType type,
    const runtime::Value& value,
    runtime::Fixed64Value& out) noexcept {
    (void)type;
    if (const auto* p = std::get_if<runtime::Fixed64Value>(&value)) {
        out = *p;
        return true;
    }
    return false;
}

[[nodiscard]] inline bool typedUnboxScalar(
    semantic::PrimitiveType type,
    const runtime::Value& value,
    runtime::Vec2fValue& out) noexcept {
    (void)type;
    if (const auto* p = std::get_if<runtime::Vec2fValue>(&value)) {
        out = *p;
        return true;
    }
    return false;
}

[[nodiscard]] inline bool typedUnboxScalar(
    semantic::PrimitiveType type,
    const runtime::Value& value,
    runtime::Vec3fValue& out) noexcept {
    (void)type;
    if (const auto* p = std::get_if<runtime::Vec3fValue>(&value)) {
        out = *p;
        return true;
    }
    return false;
}

[[nodiscard]] inline bool typedUnboxScalar(
    semantic::PrimitiveType type,
    const runtime::Value& value,
    runtime::QuatfValue& out) noexcept {
    (void)type;
    return value.tryGetQuatf(out);
}

[[nodiscard]] inline bool typedUnboxScalar(
    semantic::PrimitiveType type,
    const runtime::Value& value,
    runtime::EnumValue& out) noexcept {
    (void)type;
    return value.tryGetEnum(out);
}

[[nodiscard]] inline bool typedUnboxScalar(
    semantic::PrimitiveType type,
    const runtime::Value& value,
    runtime::Value& out) {
    if (type == semantic::PrimitiveType::Object) {
        if (std::holds_alternative<runtime::NullObject>(value)) {
            out = value;
            return true;
        }
        if (const auto* ref = std::get_if<runtime::ObjectRef>(&value);
            ref && ref->kind == runtime::ObjectKind::Record) {
            out = value;
            return true;
        }
        return false;
    }
    if (type == semantic::PrimitiveType::Array) {
        if (std::holds_alternative<runtime::NullArray>(value)) {
            out = value;
            return true;
        }
        if (const auto* ref = std::get_if<runtime::ObjectRef>(&value);
            ref && ref->kind == runtime::ObjectKind::Array) {
            out = value;
            return true;
        }
        return false;
    }
    if (type == semantic::PrimitiveType::String) {
        if (std::holds_alternative<runtime::NullString>(value) ||
            runtime::valueType(value) == semantic::PrimitiveType::String) {
            out = value;
            return true;
        }
        return false;
    }
    out = value;
    return true;
}

[[nodiscard]] inline bool typedUnboxScalar(
    semantic::PrimitiveType type,
    const runtime::Value& value,
    UnboxedCustomStruct& out) {
    (void)type;
    if (const auto* s = std::get_if<runtime::StructValue>(&value);
        s && s->valid() && s->fieldCount() <= 16) {
        out.typeId = s->typeId;
        out.fieldCount = static_cast<std::uint8_t>(s->fieldCount());
        for (std::uint8_t i = 0; i < out.fieldCount; ++i) {
            if (auto f = runtime::structValueFieldAt(*s, i)) {
                out.fields[i] = std::move(*f);
            } else {
                return false;
            }
        }
        return true;
    }
    return false;
}

template <typename T, typename = std::enable_if_t<T::kIsGeneratedAotStruct>>
[[nodiscard]] inline bool typedUnboxScalar(
    semantic::PrimitiveType,
    const runtime::Value& value,
    T& out) {
    return T::unboxFromValue(value, out);
}

template <semantic::PrimitiveType SrcType, semantic::PrimitiveType DstType, bool Checked, typename SrcT, typename DstT>
[[nodiscard]] inline bool typedConvertNumeric(SrcT src, DstT& dst) noexcept {
    if constexpr (SrcType == DstType) {
        dst = static_cast<DstT>(src);
        return true;
    } else if constexpr (SrcType == semantic::PrimitiveType::Int &&
                         DstType == semantic::PrimitiveType::Long) {
        dst = static_cast<std::int64_t>(src);
        return true;
    } else if constexpr (SrcType == semantic::PrimitiveType::Long &&
                         DstType == semantic::PrimitiveType::Int) {
        if constexpr (Checked) {
            if (src < std::numeric_limits<std::int32_t>::min() ||
                src > std::numeric_limits<std::int32_t>::max()) {
                return false;
            }
            dst = static_cast<std::int64_t>(src);
            return true;
        } else {
            dst = typedWrapInt32(src);
            return true;
        }
    } else if constexpr ((SrcType == semantic::PrimitiveType::Int ||
                          SrcType == semantic::PrimitiveType::Long ||
                          SrcType == semantic::PrimitiveType::Float) &&
                         DstType == semantic::PrimitiveType::Double) {
        dst = static_cast<double>(src);
        return true;
    } else if constexpr (SrcType == semantic::PrimitiveType::Int &&
                         DstType == semantic::PrimitiveType::Float) {
        dst = static_cast<float>(src);
        return true;
    } else {
        runtime::Value converted;
        if (!runtime::tryConvertNumeric(
                typedBoxScalar(SrcType, src),
                DstType,
                converted,
                Checked)) {
            return false;
        }
        return typedUnboxScalar(DstType, converted, dst);
    }
}

} // namespace realscript::aot

