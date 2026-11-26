#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
#pragma once

#include "realscript/runtime/Runtime.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace realscript::runtime::detail {

inline constexpr std::size_t InvalidBlockIndex =
    std::numeric_limits<std::size_t>::max();
inline constexpr std::uint16_t InvalidCompactReg = 0xffffu;
inline constexpr std::uint32_t InvalidStreamOffset = 0xffffffffu;

struct AtomicBoolFlag {
    mutable std::atomic<bool> flag{false};
    AtomicBoolFlag() noexcept = default;
    explicit AtomicBoolFlag(bool initial) noexcept : flag(initial) {}
    AtomicBoolFlag(const AtomicBoolFlag& other) noexcept
        : flag(other.flag.load(std::memory_order_acquire)) {}
    AtomicBoolFlag& operator=(const AtomicBoolFlag& other) noexcept {
        flag.store(other.flag.load(std::memory_order_acquire), std::memory_order_release);
        return *this;
    }
    AtomicBoolFlag(AtomicBoolFlag&& other) noexcept
        : flag(other.flag.load(std::memory_order_acquire)) {}
    AtomicBoolFlag& operator=(AtomicBoolFlag&& other) noexcept {
        flag.store(other.flag.load(std::memory_order_acquire), std::memory_order_release);
        return *this;
    }
    [[nodiscard]] bool load(std::memory_order order = std::memory_order_acquire) const noexcept {
        return flag.load(order);
    }
    void store(bool v, std::memory_order order = std::memory_order_release) const noexcept {
        flag.store(v, order);
    }
    operator bool() const noexcept { return flag.load(std::memory_order_acquire); }
    AtomicBoolFlag& operator=(bool v) noexcept {
        flag.store(v, std::memory_order_release);
        return *this;
    }
};

template <typename T, std::size_t PageSize = 256, std::size_t MaxPages = 1024>
class StablePagedVector {
public:
    StablePagedVector() noexcept = default;
    ~StablePagedVector() {
        const std::size_t n = count_.load(std::memory_order_relaxed);
        const std::size_t pageCount = (n + PageSize - 1u) / PageSize;
        for (std::size_t p = 0; p < pageCount && p < MaxPages; ++p) {
            delete pages_[p].load(std::memory_order_relaxed);
        }
    }
    StablePagedVector(const StablePagedVector&) = delete;
    StablePagedVector& operator=(const StablePagedVector&) = delete;
    StablePagedVector(StablePagedVector&&) = delete;
    StablePagedVector& operator=(StablePagedVector&&) = delete;

    void push_back(const T& value) {
        const std::size_t idx = count_.load(std::memory_order_relaxed);
        const std::size_t pIdx = idx / PageSize;
        if (pIdx >= MaxPages) std::abort();
        auto* page = ensurePage(pIdx);
        (*page)[idx % PageSize] = value;
        count_.store(idx + 1u, std::memory_order_release);
    }
    void push_back(T&& value) {
        const std::size_t idx = count_.load(std::memory_order_relaxed);
        const std::size_t pIdx = idx / PageSize;
        if (pIdx >= MaxPages) std::abort();
        auto* page = ensurePage(pIdx);
        (*page)[idx % PageSize] = std::move(value);
        count_.store(idx + 1u, std::memory_order_release);
    }
    template <typename... Args>
    void emplace_back(Args&&... args) {
        const std::size_t idx = count_.load(std::memory_order_relaxed);
        const std::size_t pIdx = idx / PageSize;
        if (pIdx >= MaxPages) std::abort();
        auto* page = ensurePage(pIdx);
        (*page)[idx % PageSize] = T(std::forward<Args>(args)...);
        count_.store(idx + 1u, std::memory_order_release);
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return count_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool empty() const noexcept { return size() == 0u; }
    void shrink_to_fit() noexcept {}

    [[nodiscard]] const T& operator[](std::size_t idx) const noexcept {
        const auto* page = pages_[idx / PageSize].load(std::memory_order_acquire);
        return (*page)[idx % PageSize];
    }
    [[nodiscard]] T& operator[](std::size_t idx) noexcept {
        auto* page = pages_[idx / PageSize].load(std::memory_order_acquire);
        return (*page)[idx % PageSize];
    }

private:
    using PageArray = std::array<T, PageSize>;
    PageArray* ensurePage(std::size_t pIdx) {
        auto* page = pages_[pIdx].load(std::memory_order_relaxed);
        if (!page) {
            page = new PageArray();
            pages_[pIdx].store(page, std::memory_order_release);
        }
        return page;
    }

    std::array<std::atomic<PageArray*>, MaxPages> pages_{};
    std::atomic<std::size_t> count_{0};
};

struct ExecutionEdge {
    std::size_t target = InvalidBlockIndex;
    bytecode::BlockId targetBlockId = 0;
    const bytecode::SmallRegisterVector* sources = nullptr;
    const std::vector<bytecode::BlockParameter>* destinations = nullptr;
    bool hasArguments = false;
};

struct ExecutionFunction;

enum class QuickOpcode : std::uint8_t {
    Generic,
    LoadParameter,
    ConstantInt,
    LoadLocal,
    StoreLocal,
    CallDirect,
    CallPolled,
    AddInt,
    SubtractInt,
    MultiplyInt,
    DivideInt,
    RemainderInt,
    LessInt,
    LessOrEqualInt,
    GreaterInt,
    GreaterOrEqualInt,
    EqualInt,
    NotEqualInt,
    CheckNotNullFast,
    LoadElementIntFast,
    StoreElementIntFast,
    ConstantDouble,
    AddDouble,
    SubtractDouble,
    MultiplyDouble,
    DivideDouble,
    LessDouble,
    LessOrEqualDouble,
    GreaterDouble,
    GreaterOrEqualDouble,
    LoadFieldFast,
    StoreFieldFast,
    LoadStructFieldFast,
    StoreStructFieldFast,
    ConstantBool,
    ConstantLong,
    ArrayLengthFast,
    ConvertIntToLongFast,
    ConvertIntToDoubleFast,
    ConvertLongToDoubleFast,
    NegateIntUnchecked,
    NegateDouble,
    LogicalNot,
    AddFloat,
    SubtractFloat,
    MultiplyFloat,
    DivideFloat,
    LessFloat,
    LessOrEqualFloat,
    GreaterFloat,
    GreaterOrEqualFloat,
    EqualFloat,
    NotEqualFloat,
    NegateFloat,
    ConvertIntToFloatFast,
    ConvertDoubleToFloatFast,
    ConvertFloatToIntFast,
    ConvertFloatToDoubleFast,
    ConvertLongToIntFast,
    NewStructFast,
};

enum class SuperOpcode : std::uint8_t {
    None,
    LoadParameterStoreLocal,
    BinaryLocalIntConstant,
    BinaryLocalIntLocal,
    BinaryLocalIntConstantStoreLocal,
    BinaryLocalIntLocalStoreLocal,
    BinaryLocalIntReg,
    BinaryLocalIntRegStoreLocal,
    BinaryLocalIntRegChainStoreLocal,
};

enum class FlatOpcode : std::uint8_t {
    // 1. Basic moves & constants on flat frameBase
    MoveSlot,
    ConstantInt,
    // 2. Quickened binary int operations on flat frameBase
    AddIntChecked,
    AddIntUnchecked,
    SubIntChecked,
    SubIntUnchecked,
    MulIntChecked,
    MulIntUnchecked,
    DivInt,
    RemInt,
    LessInt,
    LessOrEqualInt,
    GreaterInt,
    GreaterOrEqualInt,
    EqualInt,
    NotEqualInt,
    // 3. Superinstructions with dead temporary register elision
    SuperMoveParamStoreLocal,
    SuperBinaryLocalIntConst,
    SuperBinaryLocalIntConstStore,
    SuperBinaryLocalIntConstStoreElided,
    SuperBinaryLocalIntLocal,
    SuperBinaryLocalIntLocalStore,
    SuperBinaryLocalIntLocalStoreElided,
    SuperBinaryLocalIntReg,
    SuperBinaryLocalIntRegStore,
    SuperBinaryLocalIntRegStoreElided,
    SuperBinaryLocalIntRegChainStore,
    SuperBinaryLocalIntRegChainStoreElided,
    // 4. Branch-fused loop header superinstructions
    SuperLessLocalRegBranch,
    SuperLessLocalConstBranch,
    // 5. Object, Array & Call fast paths
    CheckNotNullFast,
    LoadElementIntFast,
    StoreElementIntFast,
    CallDirect0,
    CallDirect1,
    CallDirect2,
    CallDirectN,
    CallPolledFast,
    ConstantDouble,
    AddDouble,
    SubDouble,
    MulDouble,
    DivDouble,
    LessDouble,
    LessOrEqualDouble,
    GreaterDouble,
    GreaterOrEqualDouble,
    LoadFieldFast,
    StoreFieldFast,
    LoadStructFieldFast,
    StoreStructFieldFast,
    ConstantBool,
    ConstantLong,
    ArrayLengthFast,
    ConvertIntToLongFast,
    ConvertIntToDoubleFast,
    ConvertLongToDoubleFast,
    SuperBinaryLocalDoubleConstStore,
    SuperBinaryLocalDoubleConstStoreElided,
    SuperBinaryLocalDoubleLocalStore,
    SuperBinaryLocalDoubleLocalStoreElided,
    NegateIntUnchecked,
    NegateDouble,
    LogicalNot,
    NewArrayFast,
    // 6. Float scalar operations, conversions & superinstructions
    AddFloat,
    SubFloat,
    MulFloat,
    DivFloat,
    LessFloat,
    LessOrEqualFloat,
    GreaterFloat,
    GreaterOrEqualFloat,
    EqualFloat,
    NotEqualFloat,
    NegateFloat,
    ConvertIntToFloatFast,
    ConvertDoubleToFloatFast,
    ConvertFloatToIntFast,
    ConvertFloatToDoubleFast,
    ConvertLongToIntFast,
    SuperConstFloat,
    SuperConstFloatStoreLocal,
    SuperBinaryLocalFloatLocalStore,
    SuperBinaryLocalFloatLocalStoreElided,
    SuperBinaryLocalFloatReg,
    SuperBinaryLocalFloatRegElided,
    SuperBinaryLocalFloatRegStore,
    SuperBinaryLocalFloatRegStoreElided,
    SuperBinaryRegFloatStoreElided,
    // 7. Inline struct & Fixed / Vec3 / Vec2 / Quat / Trig / Sqrt fast paths
    NewStructFast,
    LoadFixedRawFast,
    StoreFixedRawFast,
    LoadVec3FieldFast,
    StoreVec3FieldFast,
    LoadQuatFieldFast,
    StoreQuatFieldFast,
    MathFixedConst,
    MathFixedFromRaw,
    MathFixedFromInt,
    MathFixedRatio,
    MathFixedNeg,
    MathFixedAdd,
    MathFixedSub,
    MathFixedMul,
    MathFixedMulInt,
    MathFixedDiv,
    MathFixedDivInt,
    MathFixedSin,
    MathFixedCos,
    MathFixedSqrt,
    MathSqrt64,
    MathVec3Const,
    MathVec3Raw,
    MathVec3Of,
    MathVec3Neg,
    MathVec3Add,
    MathVec3Sub,
    MathVec3MulFixed,
    MathVec3MulInt,
    MathVec3DivFixed,
    MathVec3DivInt,
    MathVec3Dot,
    MathVec3Cross,
    MathVec2Add,
    MathVec2Sub,
    MathVec2MulFixed,
    MathVec2Dot,
    MathQuatIdentity,
    MathQuatDot,
    MathQuatMul,
    MathQuatRotate,
    MathQuatRotateVec3,
    SuperQuatRotateBoxed,
    MathQuatNormalize,
    SuperQuatNormalizedBoxed,
    MathQuatFromEuler,
    MathQuatLerp,
    // 7b. Dead-temporary-eliding compound math & field superinstructions
    SuperLoadVec3RawFast,
    SuperLoadQuatRawFast,
    SuperFixedMulAdd,
    SuperVec3MulAdd,
    SuperVec3AddQuatRotate,
    SuperQuatMulNormalized,
    SuperQuatMulChain,
    SuperQuatMulRotateVec3,
    // 7c. Cache-friendly prologue/back-edge fusion & small-frame function unfolding opcodes
    SuperMoveParamStoreLocalElided,
    SuperBinaryLocalIntConstStoreJump,
    SuperBinaryLocalIntLocalStoreJump,
    SuperBinaryLocalIntRegStoreJump,
    SuperCheckNotNullCallPolledFast,
    InlineCallEnter,
    InlineReturnVoid,
    InlineReturnVoidJump,
    InlineReturnScalar,
    InlineReturnScalarJump,
    // 7d. Strongly-typed Array & Object fast paths
    LoadElementTypedFast,
    StoreElementPrimFast,
    StoreElementStructFast,
    StoreElementObjFast,
    SuperLessLocalArrayLengthBranch,
    NewObjectFast,
    LoadFieldExactFast,
    StoreFieldExactFast,
    IsTypeObjFast,
    AsTypeObjFast,
    NewDelegateFast,
    CombineDelegateFast,
    RemoveDelegateFast,
    InvokeDelegateFast,
    AsyncEnter,
    AwaitSuspend,
    AwaitResume,
    AsyncResolve,
    // 7e. P0-B Static-Type-Proven Trusted Fast-Path Opcodes (Zero ValueTag Checks)
    AddIntTrustedChecked,
    AddIntTrustedUnchecked,
    SubIntTrustedChecked,
    SubIntTrustedUnchecked,
    MulIntTrustedChecked,
    MulIntTrustedUnchecked,
    LessIntTrusted,
    AddDoubleTrusted,
    SubDoubleTrusted,
    MulDoubleTrusted,
    DivDoubleTrusted,
    AddFloatTrusted,
    SubFloatTrusted,
    MulFloatTrusted,
    // 7f. Arbitrary-size Flat-Frame Struct Slice Opcodes (Zero Heap Allocation)
    InitStructSlice,
    AliasStructSlice,
    CopyStructSlice,
    LoadStructSliceField,
    StoreStructSliceField,
    StoreLocalStructFieldInPlace,
    StoreElementStructFieldInPlace,
    // 7g. Zero-Heap-Allocation StackSlotRef Opcodes (ref/out & struct methods)
    InitStackRefCell,
    BindLocalStackRef,
    WritebackLocalStackRef,
    LoadStackRefCell,
    StoreStackRefCell,
    StoreStackRefStructFieldInPlace,
    // 7h. High-Frequency Literal / Conversion / Value Equality / Intrinsic Fast Opcodes
    ConstantStringFast,
    ConstantEnumFast,
    ConstantNullTagFast,
    EqualValueFast,
    ConvertNumericFast,
    NegateIntChecked,
    BinaryUIntFast,
    CallIntrinsicFast,
    GenericBytecode,
    // 8. Flattened block terminators
    TermJumpNoArgs,
    TermBranchBoolNoArgs,
    TermBranchGeneric,
    TermReturnVoid,
    TermReturnScalarFast,
    TermReturnGeneric,
    TermThrow,
};

struct alignas(8) ExecutionInstruction {
    const bytecode::Instruction* bytecode = nullptr;
    const ProgramImage::FunctionLocation* directTarget = nullptr;
    const ExecutionFunction* directExecution = nullptr;
    std::int64_t integerImmediate = 0;
    DeterminismOperationId operationId = 0;
    const char* opcodeName = nullptr;

    QuickOpcode opcode = QuickOpcode::Generic;
    SuperOpcode superOpcode = SuperOpcode::None;
    QuickOpcode binaryOpcode1 = QuickOpcode::Generic;
    QuickOpcode binaryOpcode2 = QuickOpcode::Generic;
    std::uint8_t span = 1;
    bool checkedArithmetic1 = true;
    bool checkedArithmetic2 = true;

    std::uint16_t result = InvalidCompactReg;
    std::uint16_t op0 = InvalidCompactReg;
    std::uint16_t op1 = InvalidCompactReg;
    std::uint16_t op2 = InvalidCompactReg;
    std::uint16_t auxReg1 = InvalidCompactReg;
    std::uint16_t auxReg2 = InvalidCompactReg;
    std::uint16_t superResult = InvalidCompactReg;
    std::uint16_t storeSlot = InvalidCompactReg;
};

struct FlatColdCallMeta {
    const ProgramImage::FunctionLocation* directTarget = nullptr;
    const ExecutionFunction* directExecution = nullptr;
    const ProgramImage::FunctionLocation* inlinedCallee = nullptr;
    std::uint32_t callerBlockIndex = 0;
    std::uint32_t callerInstIndex = 0;
    std::uint16_t inlineFrameBias = 0;
};

struct ResolvedFlatColdMeta {
    const bytecode::Instruction* bytecode = nullptr;
    const ProgramImage::FunctionLocation* directTarget = nullptr;
    const ExecutionFunction* directExecution = nullptr;
    const ProgramImage::FunctionLocation* inlinedCallee = nullptr;
    std::uint32_t blockIndex = 0;
    std::uint32_t instructionIndexInBlock = 0;
    std::uint32_t callerBlockIndex = 0;
    std::uint32_t callerInstIndex = 0;
    std::uint16_t inlineFrameBias = 0;
};

// Cold side-metadata split out of FlatInstruction so hot dispatch streams pack
// cleanly at 32 bytes/instruction (exactly 2 instructions per 64-byte L1 cache line),
// while per-instruction cold records compact to 16 bytes with sparse call/inline tables.
struct FlatColdMeta {
    std::uint32_t blockIndex = 0;
    std::uint32_t instructionIndexInBlock = 0;
    std::uint32_t bytecodeIndexInBlock = InvalidStreamOffset;
    std::uint32_t callMetaIdx = InvalidStreamOffset;
};
static_assert(sizeof(FlatColdMeta) == 16);

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4201)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
struct alignas(32) FlatInstruction {
    static constexpr std::uint8_t FlagChecked1 = 0x01u;
    static constexpr std::uint8_t FlagChecked2 = 0x02u;
    static constexpr std::uint8_t FlagFastParamPrologue = 0x04u;
    static constexpr std::uint8_t FlagArrayNoNullCheck = 0x08u;
    static constexpr std::uint8_t FlagArrayNoBoundsCheck = 0x10u;
    static constexpr std::uint8_t FlagPreJump1 = 0x20u;
    static constexpr std::uint8_t FlagTrustedSrc0 = 0x40u;
    static constexpr std::uint8_t FlagTrustedSrc1 = 0x80u;

    std::int64_t integerImmediate = 0; // bytes 0..7
    union {                            // bytes 8..15
        struct {
            std::uint16_t src2Slot;
            std::uint16_t aux0Slot;
            std::uint16_t aux1Slot;
            std::uint16_t storeSlot;
        };
        struct {
            std::uint32_t trueTargetOffset;
            std::uint32_t falseTargetOffset;
        };
    };

    // All slot fields below are pre-biased relative to `Value* frameBase`:
    //   parameters: [0 .. paramCount - 1]
    //   locals:     [localOffset .. registerOffset - 1]
    //   registers:  [registerOffset .. scratchOffset - 1]
    std::uint16_t dstSlot = InvalidCompactReg;           // bytes 16..17
    std::uint16_t src0Slot = InvalidCompactReg;          // bytes 18..19
    std::uint16_t src1Slot = InvalidCompactReg;          // bytes 20..21
    FlatOpcode flatOpcode = FlatOpcode::GenericBytecode; // byte 22
    std::uint8_t span = 1;                               // byte 23
    QuickOpcode binaryOpcode1 = QuickOpcode::Generic;    // byte 24
    QuickOpcode binaryOpcode2 = QuickOpcode::Generic;    // byte 25
    std::uint8_t arithFlags = FlagChecked1 | FlagChecked2; // byte 26
    std::uint8_t inlineArgCount = 0;                     // byte 27
    std::uint32_t auxImm32 = 0;                          // bytes 28..31

    constexpr FlatInstruction() noexcept
        : integerImmediate(0),
          src2Slot(InvalidCompactReg),
          aux0Slot(InvalidCompactReg),
          aux1Slot(InvalidCompactReg),
          storeSlot(InvalidCompactReg),
          dstSlot(InvalidCompactReg),
          src0Slot(InvalidCompactReg),
          src1Slot(InvalidCompactReg),
          flatOpcode(FlatOpcode::GenericBytecode),
          span(1),
          binaryOpcode1(QuickOpcode::Generic),
          binaryOpcode2(QuickOpcode::Generic),
          arithFlags(FlagChecked1 | FlagChecked2),
          inlineArgCount(0),
          auxImm32(0) {}

    [[nodiscard]] constexpr bool checked1() const noexcept {
        return (arithFlags & FlagChecked1) != 0u;
    }
    [[nodiscard]] constexpr bool checked2() const noexcept {
        return (arithFlags & FlagChecked2) != 0u;
    }
    constexpr void setChecked1(bool v) noexcept {
        arithFlags = v
            ? static_cast<std::uint8_t>(arithFlags | FlagChecked1)
            : static_cast<std::uint8_t>(arithFlags & ~FlagChecked1);
    }
    constexpr void setChecked2(bool v) noexcept {
        arithFlags = v
            ? static_cast<std::uint8_t>(arithFlags | FlagChecked2)
            : static_cast<std::uint8_t>(arithFlags & ~FlagChecked2);
    }
};
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
static_assert(sizeof(FlatInstruction) == 32, "FlatInstruction must be 32 bytes (2 per 64B cache line)");
static_assert(alignof(FlatInstruction) == 32, "FlatInstruction must be 32-byte aligned");

enum class FastIntrinsicKind : std::uint8_t {
    None = 0,
    StaticHas = 1,
    StaticGet = 2,
    StaticSet = 3,
    StringLength = 4,
    StringIsEmpty = 5,
    StringContains = 6,
    StringStartsWith = 7,
    StringEndsWith = 8,
    StringSubstring1 = 9,
    StringSubstring2 = 10,
    StringTrim = 11,
    StringToLower = 12,
    StringToUpper = 13,
    StringConcat = 14,
    StableHash = 15,
    ArrayPush = 16,
    ArrayRemoveAt = 17,
    ArrayPop = 18,
    ArrayClear = 19,
};

enum class FastFieldKind : std::uint8_t {
    None = 0,
    ExactValueTag = 1,
    Quat = 2,
    String = 3,
    Object = 4,
    Array = 5,
    UserStruct = 6,
    Enum = 7,
    Quatf = 8,
    Handle = 9,
};

struct FieldAccessMeta {
    semantic::SymbolId ownerTypeId = 0;
    semantic::SymbolId fieldTypeId = 0;
};

struct ObjectAllocationTemplate {
    semantic::SymbolId typeId = 0;
    std::vector<Value> defaultFields;
    std::shared_ptr<const std::vector<std::size_t>> referenceFields;
};

enum class TrivialMethodKind : std::uint8_t {
    None = 0,
    ReturnVoid,
    ReturnConst,
    ReturnParam,
    ReturnThisFieldExact,
    StoreThisFieldExact,
    ReturnThisFieldAddField,
    ReturnThisFieldSubField,
    ReturnThisFieldAddConst,
    ReturnThisFieldSubConst,
    ReturnThisFieldMulConst,
};

struct PrelinkedDispatchTarget {
    semantic::SymbolId symbolId = 0;
    const ProgramImage::FunctionLocation* target = nullptr;
    const ExecutionFunction* execution = nullptr;
};

struct PrelinkedInterfaceMap {
    semantic::SymbolId interfaceTypeId = 0;
    std::vector<PrelinkedDispatchTarget> slots;
};

struct TypeDispatchMetadata {
    semantic::SymbolId typeId = 0;
    std::vector<semantic::SymbolId> assignableTargetIds;
    std::vector<PrelinkedDispatchTarget> virtualTargets;
    std::vector<PrelinkedInterfaceMap> interfaceMaps;
};

struct PolymorphicCallSiteDesc {
    semantic::SymbolId interfaceTypeId = 0;
    std::uint32_t slot = 0;
    bool isInterface = false;
    bool valid = false;
};

struct NewDelegateSiteDesc {
    semantic::SymbolId delegateTypeId = 0;
    semantic::PrimitiveType expectedReceiverType = semantic::PrimitiveType::Error;
    semantic::SymbolId expectedReceiverTypeId = 0;
    bool hasReceiverOperand = false;
    bool valid = false;
    std::array<Value, 8> templateFields{};
};

struct InvokeDelegateSiteDesc {
    semantic::SymbolId delegateTypeId = 0;
    std::vector<semantic::PrimitiveType> userParamTypes;
    std::vector<semantic::SymbolId> userParamTypeIds;
    std::vector<std::uint8_t> userParamExactTags;
    bool valid = false;
};

struct ExecutionBlock {
    const bytecode::BasicBlock* bytecode = nullptr;
    std::vector<ExecutionInstruction> instructions;
    ExecutionEdge branch;
    ExecutionEdge falseBranch;
    bytecode::TerminatorKind terminatorKind = bytecode::TerminatorKind::None;
    DeterminismOperationId terminatorOpId = 0;
    const char* terminatorOpName = nullptr;
    bytecode::Register terminatorReg = bytecode::InvalidRegister;
    bool conditionPrevalidatedBool = false;
    bool returnPrevalidatedScalar = false;
};

struct ExecutionFunction {
    Value trivialConstValue{};
    const ExecutionMirror* ownerMirror = nullptr;
    const bytecode::Function* bytecode = nullptr;
    const bytecode::Module* module = nullptr;
    const bytecode::FunctionReference* directNativeReference = nullptr;
    std::uint16_t nativeForwarderInstructionCost = 0;
    std::size_t entry = InvalidBlockIndex;
    std::size_t edgeScratchCount = 0;
    std::uint16_t paramOffset = 0;
    std::uint16_t localOffset = 0;
    std::uint16_t registerOffset = 0;
    std::uint16_t scratchOffset = 0;
    std::uint32_t totalFrameSlots = 0;
    std::uint32_t entryStreamOffset = InvalidStreamOffset;
    std::uint16_t trivialInstCost = 0;
    std::uint16_t trivialParamIndex = 0;
    std::uint16_t trivialFieldIndex0 = 0;
    std::uint16_t trivialFieldIndex1 = 0;
    semantic::SymbolId trivialOwnerTypeId = 0;
    std::uint8_t trivialFieldTag0 = 0;
    std::uint8_t trivialFieldTag1 = 0;
    bool trivialCheckedArith = true;
    TrivialMethodKind trivialKind = TrivialMethodKind::None;
    bool hasDebugSequencePoints = false;
    bool hasExceptionHandlers = false;
    bool canUseFlatFastPath = false;
    bool isSmallLeafInlineCandidate = false;
    bool hasFastParamToLocalPrologue = false;
    bool isAsyncFunction = false;
    bool hasStructPayloads = false;
    AtomicBoolFlag compiled{false};
    std::uint32_t lazyLocIndex = InvalidStreamOffset;
    std::uint16_t singleZoneSlots = 0;
    std::uint16_t payloadOffset = 0;
    std::uint16_t edgeStructScratchOffset = InvalidCompactReg;
    std::uint32_t uncompactedRegPayloadSlots = 0;
    std::uint32_t compactedRegPayloadSlots = 0;
    std::vector<std::uint16_t> slotPayloadOffsets;
    std::vector<std::uint16_t> slotStructSchemaIds;
    std::vector<ExecutionBlock> blocks;
    std::vector<FlatInstruction> flatStream;
    std::vector<FlatColdMeta> flatCold;
    std::vector<FlatColdCallMeta> flatColdCalls;
    mutable std::unique_ptr<std::once_flag> slowInstructionsOnce;
    std::vector<std::uint32_t> blockStreamOffsets;
    std::vector<std::uint32_t> denseBlockIndices;
    std::unique_ptr<std::unordered_map<bytecode::BlockId, std::size_t>> sparseBlockIndices;

    [[nodiscard]] ResolvedFlatColdMeta resolveCold(std::size_t idx) const noexcept {
        ResolvedFlatColdMeta out;
        if (idx >= flatCold.size()) {
            return out;
        }
        const auto& compact = flatCold[idx];
        out.blockIndex = compact.blockIndex;
        out.instructionIndexInBlock = compact.instructionIndexInBlock;
        if (compact.callMetaIdx < flatColdCalls.size()) {
            const auto& call = flatColdCalls[compact.callMetaIdx];
            out.directTarget = call.directTarget;
            out.directExecution = call.directExecution;
            out.inlinedCallee = call.inlinedCallee;
            out.callerBlockIndex = call.callerBlockIndex;
            out.callerInstIndex = call.callerInstIndex;
            out.inlineFrameBias = call.inlineFrameBias;
        }
        if (compact.bytecodeIndexInBlock != InvalidStreamOffset) {
            const ExecutionFunction* owner =
                (out.inlinedCallee != nullptr && out.directExecution != nullptr)
                    ? out.directExecution
                    : this;
            if (compact.blockIndex < owner->blocks.size()) {
                const auto* bb = owner->blocks[compact.blockIndex].bytecode;
                if (bb != nullptr &&
                    compact.bytecodeIndexInBlock < bb->instructions.size()) {
                    out.bytecode = &bb->instructions[compact.bytecodeIndexInBlock];
                }
            }
        }
        return out;
    }

    [[nodiscard]] const ProgramImage::FunctionLocation* coldInlinedCallee(
        std::size_t idx) const noexcept {
        if (idx >= flatCold.size()) {
            return nullptr;
        }
        const std::uint32_t callIdx = flatCold[idx].callMetaIdx;
        return callIdx < flatColdCalls.size()
            ? flatColdCalls[callIdx].inlinedCallee
            : nullptr;
    }

    [[nodiscard]] std::size_t findBlockIndex(bytecode::BlockId id) const noexcept {
        if (id < denseBlockIndices.size()) {
            const std::uint32_t idx = denseBlockIndices[id];
            return idx == InvalidStreamOffset
                ? InvalidBlockIndex
                : static_cast<std::size_t>(idx);
        }
        if (sparseBlockIndices) {
            const auto found = sparseBlockIndices->find(id);
            return found == sparseBlockIndices->end()
                ? InvalidBlockIndex
                : found->second;
        }
        return InvalidBlockIndex;
    }
};

struct StackRefTemplate {
    semantic::SymbolId wrapperTypeId = 0;
    semantic::PrimitiveType innerType = semantic::PrimitiveType::Error;
    semantic::SymbolId innerTypeId = 0;
    std::uint16_t flatSchemaId = 0;
    std::uint16_t slotCount = 1;
    Value scalarDefaultValue{};
};

struct FastFunctionDispatchEntry {
    semantic::SymbolId symbolId = 0;
    const ProgramImage::FunctionLocation* location = nullptr;
    const ExecutionFunction* execution = nullptr;
};

struct FieldMetaPairHash {
    std::size_t operator()(
        const std::pair<semantic::SymbolId, semantic::SymbolId>& key) const noexcept {
        const std::uint64_t a = key.first;
        const std::uint64_t b = key.second;
        const std::uint64_t h =
            a ^ (b * 0x9e3779b97f4a7c15ULL + 0x517cc1b727220a95ULL + (a << 6) + (a >> 2));
        return static_cast<std::size_t>(h);
    }
};

struct LazyCompileState {
    static constexpr std::size_t kMaxDecodedChunks = 2u;
    struct DecodedChunkEntry {
        std::uint32_t chunkIndex = std::numeric_limits<std::uint32_t>::max();
        std::vector<std::uint8_t> bytes;
    };

    mutable std::mutex mutex;
    std::shared_ptr<const bytecode::LazyBytecodeBundle> baseBundle;
    std::vector<DecodedChunkEntry> decodedChunkLru;
    std::atomic<std::size_t> evictedChunkCount{0};
    const std::vector<bytecode::Module>* modules = nullptr;
    const std::unordered_map<semantic::SymbolId, ProgramImage::FunctionLocation>* functions = nullptr;
    const std::unordered_map<semantic::SymbolId, const semantic::TypeSymbol*>* types = nullptr;
    const ProgramImage::LazyFunctionLocTable* lazyFuncLocTable = nullptr;
    mutable std::mutex lazyExecMutex; // Leaf lock: never acquires mutex or typeDispatchMutex
    StablePagedVector<ExecutionFunction, 64> lazyExecStorage;
    std::unordered_map<const bytecode::Function*, const ExecutionFunction*> lazyExecMap;
    std::unordered_map<std::string, std::uint32_t> stringLiteralMap;
    std::unordered_map<semantic::SymbolId, std::uint16_t> objectTemplateByTypeId;
    std::unordered_map<
        std::pair<semantic::SymbolId, semantic::SymbolId>,
        std::uint32_t,
        FieldMetaPairHash> fieldMetaMap;
    std::atomic<std::size_t> compiledLazyCount{0};
    std::size_t totalLazyCount = 0;
    mutable std::mutex typeDispatchMutex;
    std::deque<TypeDispatchMetadata> lazyTypeDispatchStorage;
    std::unordered_map<semantic::SymbolId, const TypeDispatchMetadata*> lazyTypeDispatchMap;

    std::uint32_t sinCalleeBaseCost = 0;
    std::uint16_t sinCalleeCalls = 0;
    std::uint32_t sinWrapExtraCost = 0;
    std::uint32_t cosCalleeBaseCost = 0;
    std::uint16_t cosCalleeCalls = 0;
    std::uint32_t sqrt64NegCalleeCost = 0;
    std::uint32_t sqrt64PosBaseCalleeCost = 0;
    std::uint32_t sqrt64LoopIterCost = 0;
    std::uint32_t fixedSqrtNegCalleeCost = 0;
    std::uint32_t fixedSqrtPosBaseCalleeCost = 0;
    std::uint32_t quatLenPosBaseCalleeCost = 0;
    std::uint16_t quatLenPosCalls = 0;
    std::uint32_t quatNormPosBaseCalleeCost = 0;
    std::uint16_t quatNormPosCalls = 0;
    std::uint32_t quatFromEulerBaseCalleeCost = 0;
    std::uint16_t quatFromEulerBaseCalls = 0;
    std::uint32_t quatLerpPosBaseCalleeCost = 0;
    std::uint16_t quatLerpPosBaseCalls = 0;
    std::uint32_t quatLerpDotNegExtraCost = 0;
    std::uint16_t quatLerpDotNegExtraCalls = 0;
};

struct ExecutionMirror {
    std::uint64_t instanceId = 0;
    AtomicBoolFlag hasAsyncOperations{false};
    std::unordered_map<const bytecode::Function*, ExecutionFunction> functions;
    std::vector<FastFunctionDispatchEntry> fastFunctionTable;
    StablePagedVector<FieldAccessMeta, 256> fieldMetas;
    StablePagedVector<ObjectAllocationTemplate, 256> objectTemplates;
    StablePagedVector<std::string, 256> stringLiterals;
    StablePagedVector<Value, 256> constPoolValues;
    StablePagedVector<StackRefTemplate, 256> stackRefTemplates;
    std::unordered_map<semantic::SymbolId, std::uint16_t> stackRefTemplateByTypeId;
    std::unordered_map<semantic::SymbolId, TypeDispatchMetadata> typeDispatch;
    StablePagedVector<PolymorphicCallSiteDesc, 256> polyCallSites;
    StablePagedVector<NewDelegateSiteDesc, 256> newDelegateSites;
    StablePagedVector<InvokeDelegateSiteDesc, 256> invokeDelegateSites;
    mutable std::unique_ptr<LazyCompileState> lazyState;
    struct ParallelBatchFactorySlot {
        std::mutex mutex;
        ParallelBatchKernelFactory factory;
    };
    mutable std::shared_ptr<ParallelBatchFactorySlot> parallelBatchFactorySlot =
        std::make_shared<ParallelBatchFactorySlot>();

    [[nodiscard]] const FastFunctionDispatchEntry* lookupFunctionFast(
        semantic::SymbolId symId) const noexcept {
        if (symId == 0 || fastFunctionTable.empty()) {
            return nullptr;
        }
        const std::size_t mask = fastFunctionTable.size() - 1u;
        std::size_t slot = static_cast<std::size_t>(
            (symId ^ (symId >> 32u)) * 11400714819323198485ULL) & mask;
        while (true) {
            const auto& entry = fastFunctionTable[slot];
            if (entry.symbolId == symId) {
                return &entry;
            }
            if (entry.symbolId == 0u) {
                return nullptr;
            }
            slot = (slot + 1u) & mask;
        }
    }
};

[[nodiscard]] bool ensureFunctionCompiledSlow(
    const ExecutionMirror& mirror,
    const ExecutionFunction& execFn);

[[nodiscard]] inline bool ensureFunctionCompiled(
    const ExecutionMirror& mirror,
    const ExecutionFunction& execFn) {
    if (execFn.compiled.load(std::memory_order_acquire)) {
        return true;
    }
    return ensureFunctionCompiledSlow(mirror, execFn);
}

[[nodiscard]] std::shared_ptr<const ExecutionMirror> buildExecutionMirror(
    const std::vector<bytecode::Module>& modules,
    const std::unordered_map<
        semantic::SymbolId, ProgramImage::FunctionLocation>& functions,
    const std::unordered_map<
        semantic::SymbolId, const semantic::TypeSymbol*>& types,
    const std::shared_ptr<const bytecode::LazyBytecodeBundle>& lazyBaseBundle = nullptr,
    const ProgramImage::LazyFunctionLocTable* lazyFuncLocTable = nullptr);

[[nodiscard]] const ExecutionFunction* findOrCreateLazyExecutionFunction(
    const ExecutionMirror& mirror,
    const bytecode::Function& function) noexcept;

[[nodiscard]] const ExecutionFunction* findExistingExecutionFunction(
    const ExecutionMirror& mirror,
    const bytecode::Function& function) noexcept;

[[nodiscard]] inline const ExecutionFunction* findExecutionFunction(
    const ExecutionMirror& mirror,
    const bytecode::Function& function) noexcept {
    const auto found = mirror.functions.find(&function);
    if (found != mirror.functions.end()) {
        return &found->second;
    }
    if (mirror.lazyState != nullptr) {
        return findOrCreateLazyExecutionFunction(mirror, function);
    }
    return nullptr;
}

[[nodiscard]] inline const ExecutionBlock* findExecutionBlock(
    const ExecutionFunction& function,
    bytecode::BlockId id) noexcept {
    const std::size_t idx = function.findBlockIndex(id);
    return idx == InvalidBlockIndex ? nullptr : &function.blocks[idx];
}

void ensureSlowInstructions(const ExecutionFunction& execFn);

[[nodiscard]] const TypeDispatchMetadata* resolveTypeDispatch(
    const ExecutionMirror& mirror,
    semantic::SymbolId typeId) noexcept;

} // namespace realscript::runtime::detail

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
