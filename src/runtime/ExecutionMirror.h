#pragma once

#include "realscript/runtime/Runtime.h"

#include <cstddef>
#include <limits>
#include <memory>
#include <unordered_map>
#include <vector>

namespace realscript::runtime::detail {

inline constexpr std::size_t InvalidBlockIndex =
    std::numeric_limits<std::size_t>::max();

struct ExecutionEdge {
    std::size_t target = InvalidBlockIndex;
    const std::vector<bytecode::Register>* sources = nullptr;
    const std::vector<bytecode::BlockParameter>* destinations = nullptr;
};

struct ExecutionFunction;

enum class QuickOpcode : std::uint8_t {
    Generic,
    LoadParameter,
    ConstantInt,
    LoadLocal,
    StoreLocal,
    CallDirect,
    AddInt,
    SubtractInt,
    MultiplyInt,
    DivideInt,
    RemainderInt,
    LessInt,
    LessOrEqualInt,
    GreaterInt,
    GreaterOrEqualInt,
};

enum class SuperOpcode : std::uint8_t {
    None,
    LoadParameterStoreLocal,
    BinaryLocalIntConstant,
    BinaryLocalIntLocal,
    BinaryLocalIntConstantStoreLocal,
    BinaryLocalIntLocalStoreLocal,
};

struct ExecutionInstruction {
    const bytecode::Instruction* bytecode = nullptr;
    QuickOpcode opcode = QuickOpcode::Generic;
    SuperOpcode superOpcode = SuperOpcode::None;
    std::uint8_t span = 1;
    const ProgramImage::FunctionLocation* directTarget = nullptr;
    const ExecutionFunction* directExecution = nullptr;
};

struct ExecutionBlock {
    const bytecode::BasicBlock* bytecode = nullptr;
    std::vector<ExecutionInstruction> instructions;
    ExecutionEdge branch;
    ExecutionEdge falseBranch;
};

struct ExecutionFunction {
    const bytecode::Function* bytecode = nullptr;
    std::size_t entry = InvalidBlockIndex;
    std::size_t edgeScratchCount = 0;
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
