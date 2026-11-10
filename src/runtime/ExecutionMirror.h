#pragma once

#include "realscript/runtime/Runtime.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <unordered_map>
#include <vector>

namespace realscript::runtime::detail {

inline constexpr std::size_t InvalidBlockIndex =
    std::numeric_limits<std::size_t>::max();
inline constexpr std::uint16_t InvalidCompactReg = 0xffffu;
inline constexpr std::uint32_t InvalidStreamOffset = 0xffffffffu;

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

// Cold side-metadata split out of FlatInstruction so hot dispatch streams pack
// cleanly at 32 bytes/instruction (exactly 2 instructions per 64-byte L1 cache line).
struct FlatColdMeta {
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

enum class FastFieldKind : std::uint8_t {
    None = 0,
    ExactValueTag = 1,
    Quat = 2,
    String = 3,
    Object = 4,
    Array = 5,
    UserStruct = 6,
    Enum = 7,
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
    const bytecode::Function* bytecode = nullptr;
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
    std::vector<ExecutionBlock> blocks;
    std::vector<FlatInstruction> flatStream;
    std::vector<FlatColdMeta> flatCold;
    std::vector<std::uint32_t> blockStreamOffsets;
    std::unordered_map<bytecode::BlockId, std::size_t> blockIndices;
};

struct ExecutionMirror {
    std::uint64_t instanceId = 0;
    std::unordered_map<const bytecode::Function*, ExecutionFunction> functions;
    std::vector<FieldAccessMeta> fieldMetas;
    std::vector<ObjectAllocationTemplate> objectTemplates;
    std::unordered_map<semantic::SymbolId, TypeDispatchMetadata> typeDispatch;
    std::vector<PolymorphicCallSiteDesc> polyCallSites;
    std::vector<NewDelegateSiteDesc> newDelegateSites;
    std::vector<InvokeDelegateSiteDesc> invokeDelegateSites;
};

[[nodiscard]] std::shared_ptr<const ExecutionMirror> buildExecutionMirror(
    const std::vector<bytecode::Module>& modules,
    const std::unordered_map<
        semantic::SymbolId, ProgramImage::FunctionLocation>& functions);

[[nodiscard]] inline const ExecutionFunction* findExecutionFunction(
    const ExecutionMirror& mirror,
    const bytecode::Function& function) noexcept {
    const auto found = mirror.functions.find(&function);
    return found == mirror.functions.end() ? nullptr : &found->second;
}

[[nodiscard]] inline const ExecutionBlock* findExecutionBlock(
    const ExecutionFunction& function,
    bytecode::BlockId id) noexcept {
    const auto found = function.blockIndices.find(id);
    return found == function.blockIndices.end()
        ? nullptr
        : &function.blocks[found->second];
}

} // namespace realscript::runtime::detail
