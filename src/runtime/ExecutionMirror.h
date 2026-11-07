#pragma once

#include "realscript/runtime/Runtime.h"

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
    const bytecode::Function* bytecode = nullptr;
    std::size_t entry = InvalidBlockIndex;
    std::size_t edgeScratchCount = 0;
    bool hasDebugSequencePoints = false;
    std::vector<ExecutionBlock> blocks;
    std::unordered_map<bytecode::BlockId, std::size_t> blockIndices;
};

struct ExecutionMirror {
    std::unordered_map<const bytecode::Function*, ExecutionFunction> functions;
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
