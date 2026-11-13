#pragma once

#include "realscript/jit/Jit.h"

#include "../runtime/ExecutionMirror.h"
#include "realscript/runtime/AsyncRuntime.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace realscript::jit::jit_detail {

template <typename FnPtr, typename Ptr>
[[nodiscard]] inline FnPtr bitCastFnPtr(Ptr ptr) noexcept {
    static_assert(sizeof(FnPtr) == sizeof(Ptr), "function pointer size mismatch");
    FnPtr out = nullptr;
    std::memcpy(&out, &ptr, sizeof(out));
    return out;
}

template <typename FnPtr>
[[nodiscard]] inline std::uint64_t fnPtrAddress(FnPtr fn) noexcept {
    static_assert(sizeof(FnPtr) <= sizeof(std::uint64_t), "64-bit pointer expected");
    std::uint64_t addr = 0;
    std::memcpy(&addr, &fn, sizeof(fn));
    return addr;
}

// P2-A: Value Layout Static Verification & Runtime Execution ABI
// ============================================================================


// Compile-time verification of Value 16-byte layout contract required by StencilJit
struct ValueLayoutProbe {
    static bool verify() noexcept {
        runtime::Value iv(std::int64_t{-0x12345678LL});
        runtime::Value dv(3.5);
        runtime::Value bv(true);
        std::int64_t iPayload = 0;
        double dPayload = 0.0;
        std::uint8_t bPayload = 0;
        std::uint8_t iTag = 0, dTag = 0, bTag = 0;
        std::memcpy(&iPayload, &iv, 8);
        std::memcpy(&iTag, reinterpret_cast<const std::uint8_t*>(&iv) + 15, 1);
        std::memcpy(&dPayload, &dv, 8);
        std::memcpy(&dTag, reinterpret_cast<const std::uint8_t*>(&dv) + 15, 1);
        std::memcpy(&bPayload, &bv, 1);
        std::memcpy(&bTag, reinterpret_cast<const std::uint8_t*>(&bv) + 15, 1);
        return iPayload == -0x12345678LL &&
               iTag == static_cast<std::uint8_t>(runtime::ValueTag::Int64) &&
               dPayload == 3.5 &&
               dTag == static_cast<std::uint8_t>(runtime::ValueTag::Double) &&
               bPayload == 1u &&
               bTag == static_cast<std::uint8_t>(runtime::ValueTag::Bool);
    }
};
static_assert(sizeof(runtime::Value) == 16, "StencilJit requires 16-byte Value");
static_assert(alignof(runtime::Value) == 16, "StencilJit requires 16-byte aligned Value");
static_assert(sizeof(runtime::ObjectRef) == 16, "StencilJit requires 16-byte ObjectRef");

inline constexpr std::uint8_t kTagBool = static_cast<std::uint8_t>(runtime::ValueTag::Bool);       // 4
inline constexpr std::uint8_t kTagInt64 = static_cast<std::uint8_t>(runtime::ValueTag::Int64);     // 9
inline constexpr std::uint8_t kTagLong = static_cast<std::uint8_t>(runtime::ValueTag::Long);       // 11
inline constexpr std::uint8_t kTagFloat = static_cast<std::uint8_t>(runtime::ValueTag::Float);     // 13
inline constexpr std::uint8_t kTagDouble = static_cast<std::uint8_t>(runtime::ValueTag::Double);   // 14
inline constexpr std::uint8_t kTagObjectRef = static_cast<std::uint8_t>(runtime::ValueTag::ObjectRef); // 27

inline constexpr std::uint8_t kPackedBool = static_cast<std::uint8_t>(runtime::PackedElemEncoding::Bool);       // 1
inline constexpr std::uint8_t kPackedInt32 = static_cast<std::uint8_t>(runtime::PackedElemEncoding::Int32);     // 7
inline constexpr std::uint8_t kPackedFloat64 = static_cast<std::uint8_t>(runtime::PackedElemEncoding::Float64); // 13

[[nodiscard]] constexpr bool fitsInt32(std::int64_t v) noexcept {
    return v >= static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()) &&
           v <= static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max());
}

[[nodiscard]] constexpr std::int64_t wrapInt32Val(std::int64_t v) noexcept {
    return static_cast<std::int64_t>(static_cast<std::int32_t>(static_cast<std::uint32_t>(v)));
}

struct alignas(16) JitArrayCacheEntry {
    std::uint64_t slotAndGen;        // offset 0  (0xffffffffffffffffULL when invalid)
    std::uint32_t heapId;            // offset 8
    std::uint8_t kind;               // offset 12
    std::uint8_t isPacked;           // offset 13 (1 = Packed, 2 = FlatStruct, 0 = Boxed)
    std::uint8_t encoding;           // offset 14 (PackedElemEncoding)
    std::uint8_t elementType;        // offset 15 (semantic::PrimitiveType)
    void* rawData;                   // offset 16 (packed rawData, FlatStruct Value*, or boxed Value*)
    std::uint32_t length;            // offset 24
    std::uint16_t flatStride;        // offset 28 (FlatStruct[] leaf stride)
    std::uint16_t flatSchemaId;      // offset 30 (FlatStruct[] schemaId)
    std::uint64_t elementTypeId;     // offset 32
    std::uint8_t* dirtyCards;        // offset 40 (FlatStruct[] GC card table or nullptr)
    std::uint32_t blittableSchemaId; // offset 48 (Packed BlittableStruct schemaId)
    std::uint8_t flatFlags;          // offset 52 (bit0=hasRefs, bit1=hasPool, bit2=needsRemBarrier)
    std::uint8_t padByte0;           // offset 53
    std::uint16_t padWord1;          // offset 54
    std::uint64_t pad3;              // offset 56
};
static_assert(std::is_standard_layout_v<JitArrayCacheEntry>, "JitArrayCacheEntry must be standard layout");
static_assert(sizeof(JitArrayCacheEntry) == 64, "JitArrayCacheEntry must be 64 bytes");
static_assert(offsetof(JitArrayCacheEntry, slotAndGen) == 0);
static_assert(offsetof(JitArrayCacheEntry, heapId) == 8);
static_assert(offsetof(JitArrayCacheEntry, kind) == 12);
static_assert(offsetof(JitArrayCacheEntry, isPacked) == 13);
static_assert(offsetof(JitArrayCacheEntry, encoding) == 14);
static_assert(offsetof(JitArrayCacheEntry, elementType) == 15);
static_assert(offsetof(JitArrayCacheEntry, rawData) == 16);
static_assert(offsetof(JitArrayCacheEntry, length) == 24);
static_assert(offsetof(JitArrayCacheEntry, flatStride) == 28);
static_assert(offsetof(JitArrayCacheEntry, flatSchemaId) == 30);
static_assert(offsetof(JitArrayCacheEntry, elementTypeId) == 32);
static_assert(offsetof(JitArrayCacheEntry, dirtyCards) == 40);
static_assert(offsetof(JitArrayCacheEntry, blittableSchemaId) == 48);
static_assert(offsetof(JitArrayCacheEntry, flatFlags) == 52);

struct JitRuntimeSession;
struct CompiledFunctionRecord;

struct alignas(16) JitAbiContext {
    runtime::Value* frameBase;                 // offset 0
    std::uint64_t remainingBudget;             // offset 8
    std::uint64_t branchesTaken;               // offset 16
    runtime::Value* returnSlot;                // offset 24
    std::uint32_t errorFlatIndex;              // offset 32
    std::uint32_t errorCodeRaw;                // offset 36
    JitRuntimeSession* session;                // offset 40
    const CompiledFunctionRecord* compiledFn;  // offset 48
    std::uint32_t branchCondOut;               // offset 56 (used by slow array-length branch helper)
    std::uint32_t nextFlatOffsetOut;           // offset 60 (used by InlineCallEnter / slow helpers)
    JitArrayCacheEntry arrayCache[4];          // offset 64 .. 319
};
static_assert(std::is_standard_layout_v<JitAbiContext>, "JitAbiContext must be standard layout");
static_assert(offsetof(JitAbiContext, frameBase) == 0);
static_assert(offsetof(JitAbiContext, remainingBudget) == 8);
static_assert(offsetof(JitAbiContext, branchesTaken) == 16);
static_assert(offsetof(JitAbiContext, returnSlot) == 24);
static_assert(offsetof(JitAbiContext, errorFlatIndex) == 32);
static_assert(offsetof(JitAbiContext, errorCodeRaw) == 36);
static_assert(offsetof(JitAbiContext, session) == 40);
static_assert(offsetof(JitAbiContext, compiledFn) == 48);
static_assert(offsetof(JitAbiContext, branchCondOut) == 56);
static_assert(offsetof(JitAbiContext, nextFlatOffsetOut) == 60);
static_assert(offsetof(JitAbiContext, arrayCache) == 64);

using NativeJitEntryFn = std::uint32_t (*)(JitAbiContext* ctx);

inline void invalidateJitArrayCaches(JitAbiContext& ctx) noexcept {
    for (auto& ce : ctx.arrayCache) {
        ce.slotAndGen = 0xffffffffffffffffULL;
        ce.heapId = 0;
        ce.kind = 0;
        ce.isPacked = 0;
        ce.encoding = 0;
        ce.elementType = 0;
        ce.rawData = nullptr;
        ce.length = 0;
        ce.flatStride = 0;
        ce.flatSchemaId = 0;
        ce.elementTypeId = 0;
        ce.dirtyCards = nullptr;
        ce.blittableSchemaId = 0;
        ce.flatFlags = 0;
    }
}

struct JitRuntimeSession {
    const runtime::ProgramImage* program = nullptr;
    runtime::ManagedHeap* heap = nullptr;
    runtime::Limits limits{};
    runtime::RuntimeStatistics stats{};
    runtime::RuntimeError error{};
    std::vector<runtime::Value>* values = nullptr;
    std::size_t frameOffset = 0;
    std::size_t valueTop = 0;
    std::size_t callDepth = 0;
    runtime::Value* valuesBase = nullptr;
    std::uint32_t frameOffset32 = 0;
    std::uint8_t flatBankId = 0;
    runtime::ShadowStack shadowStack{};
};

struct StencilTlsScratch {
    std::vector<runtime::Value> values;
    bool inUse = false;
};

// ============================================================================
// Direct-Threaded C++ Stencil Thunk Representation (Cross-Platform Fast Tier)
// ============================================================================

enum class StencilStepStatus : std::uint8_t {
    Continue = 0,
    ReturnOk = 1,
    Error = 2,
};

struct ThreadedStencilOp;
using ThreadedStencilFn = StencilStepStatus (*)(
    JitAbiContext* ctx,
    const ThreadedStencilOp*& pc) noexcept;

struct ThreadedStencilOp {
    ThreadedStencilFn handler = nullptr;
    const ThreadedStencilOp* trueTarget = nullptr;
    const ThreadedStencilOp* falseTarget = nullptr;
    std::int64_t imm64 = 0;
    std::uint32_t dstOff = 0;
    std::uint32_t src0Off = 0;
    std::uint32_t src1Off = 0;
    std::uint32_t src2Off = 0;
    std::uint32_t aux0Off = 0;
    std::uint32_t aux1Off = 0;
    std::uint32_t storeOff = 0;
    std::uint32_t flatIdx = 0;
    std::uint8_t span = 1;
    std::uint8_t unexecutedOnTrap = 0;
    std::uint8_t flags = 0;
    std::uint8_t auxByte = 0;
};

struct CompiledFunctionRecord {
    semantic::SymbolId symbolId = 0;
    const runtime::ProgramImage::FunctionLocation* location = nullptr;
    const runtime::detail::ExecutionFunction* execFunc = nullptr;
    std::atomic<StencilTier> tier{StencilTier::InterpreterFallback};
    std::atomic<NativeJitEntryFn> nativeEntry{nullptr};
    std::uint8_t* rawCodePtr = nullptr;
    std::size_t rawCodeSize = 0;
    bool eligibleForNative = false;
    bool eligibleForThreaded = false;
    mutable std::atomic<std::uint32_t> callCount{0};
    std::vector<std::uint8_t> unexecutedAfterOp;
    std::vector<ThreadedStencilOp> threadedStream;
};

// ============================================================================

// Shared Cold-Trap Metadata & Native Machine Code Emitters (x86_64 + ARM64)
// ============================================================================

struct ColdTrapStub {
    enum class Kind : std::uint8_t {
        BudgetSimple,
        BudgetPartialSuper,
        IntegerOverflow,
        DivisionByZero,
        TypeMismatch,
        ArrayLengthSlow,
        ArrayLengthBranchSlow,
        ArrayLoadSlow,
        ArrayStoreSlow,
        FlatStructSlow,
    };

    Kind kind = Kind::BudgetSimple;
    std::uint32_t stubLabel = 0;
    std::uint32_t resumeLabel = 0;
    std::uint32_t altResumeLabel = 0;
    std::uint32_t flatIdx = 0;
    std::int32_t restoreBudget = 0;
};

void populateJitErrorDiagnostics(
    JitRuntimeSession& session,
    const runtime::ProgramImage::FunctionLocation& location,
    const runtime::detail::ExecutionFunction& execFunc,
    std::uint32_t flatIdx,
    runtime::ErrorCode code,
    std::string message);

[[nodiscard]] std::uint32_t setHelperError(
    JitAbiContext* ctx,
    std::uint32_t flatIdx,
    runtime::ErrorCode code,
    std::string message) noexcept;

[[nodiscard]] bool bindJitArrayCacheSlot(
    runtime::ManagedHeap* heap,
    const runtime::Value& arrVal,
    JitArrayCacheEntry& ce) noexcept;

std::uint32_t jitNewArrayHelper(JitAbiContext* ctx, std::uint32_t flatIdx) noexcept;
std::uint32_t jitArrayLengthHelper(JitAbiContext* ctx, std::uint32_t flatIdx) noexcept;
std::uint32_t jitLoadElementHelper(JitAbiContext* ctx, std::uint32_t flatIdx) noexcept;
std::uint32_t jitStoreElementHelper(JitAbiContext* ctx, std::uint32_t flatIdx) noexcept;

bool jitValuesEqual(
    runtime::ManagedHeap* heap,
    const runtime::Value& left,
    const runtime::Value& right,
    bool& outEqual,
    runtime::ErrorCode& outErrCode,
    const char*& outErrMsg) noexcept;

std::uint32_t jitFlatStructAndStackRefHelper(JitAbiContext* ctx, std::uint32_t flatIdx) noexcept;
std::uint32_t jitInlineCallEnterHelper(JitAbiContext* ctx, std::uint32_t flatIdx) noexcept;
std::uint32_t jitPartialSuperBudgetHelper(JitAbiContext* ctx, std::uint32_t flatIdx) noexcept;

[[nodiscard]] std::uint8_t computeUnexecutedAfterOp(
    const runtime::detail::ExecutionFunction& execFunc,
    std::size_t flatIdx) noexcept;

[[nodiscard]] bool isStencilSupportedFlatOp(
    const runtime::detail::ExecutionFunction& execFunc,
    const runtime::detail::FlatInstruction& fi) noexcept;

[[nodiscard]] bool isFunctionEligibleForStencilJit(
    const runtime::detail::ExecutionFunction& execFunc) noexcept;

#if UINTPTR_MAX == 0xffffffffffffffffULL
[[nodiscard]] std::vector<std::uint8_t> compileFunctionX64(
    const CompiledFunctionRecord& record);

[[nodiscard]] std::vector<std::uint8_t> compileFunctionA64(
    const CompiledFunctionRecord& record);

[[nodiscard]] std::uint32_t executeSimulatedArm64Code(
    const std::vector<std::uint8_t>& code,
    JitAbiContext* abi,
    std::string& outSimDiagnostic) noexcept;
#endif

} // namespace realscript::jit::jit_detail
