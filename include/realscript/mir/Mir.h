#pragma once

#include "realscript/diagnostics/Diagnostic.h"
#include "realscript/semantic/Semantic.h"
#include "realscript/text/Text.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace realscript::mir {

using ValueId = std::int32_t;
using BlockId = std::uint32_t;

enum class Opcode {
    Parameter,
    ConstantInt,
    ConstantBool,
    ConstantString,
    ConstantNull,
    LoadLocal,
    StoreLocal,
    NegateInt,
    LogicalNot,
    AddInt,
    SubtractInt,
    MultiplyInt,
    DivideInt,
    RemainderInt,
    Equal,
    NotEqual,
    LessInt,
    LessOrEqualInt,
    GreaterInt,
    GreaterOrEqualInt,
};

struct Instruction {
    ValueId result = -1;
    semantic::PrimitiveType resultType = semantic::PrimitiveType::Void;
    Opcode opcode = Opcode::ConstantInt;
    std::vector<ValueId> operands;
    std::size_t localIndex = 0;
    std::int64_t integerImmediate = 0;
    bool boolImmediate = false;
    std::string stringImmediate;
    text::TextSpan sourceSpan;
};

struct BlockParameter {
    ValueId value = -1;
    semantic::PrimitiveType type = semantic::PrimitiveType::Error;
};

enum class TerminatorKind {
    None,
    Jump,
    Branch,
    ReturnValue,
    ReturnVoid,
};

struct Terminator {
    TerminatorKind kind = TerminatorKind::None;
    ValueId condition = -1;
    ValueId value = -1;
    BlockId target = 0;
    BlockId falseTarget = 0;
    std::vector<ValueId> arguments;
    std::vector<ValueId> falseArguments;
    text::TextSpan sourceSpan;
};

struct BasicBlock {
    BlockId id = 0;
    std::vector<BlockParameter> parameters;
    std::vector<Instruction> instructions;
    Terminator terminator;
};

struct Function {
    std::string name;
    semantic::PrimitiveType returnType = semantic::PrimitiveType::Error;
    std::vector<semantic::PrimitiveType> parameterTypes;
    std::vector<semantic::PrimitiveType> localTypes;
    std::vector<BasicBlock> blocks;
};

struct Module {
    std::string name;
    std::vector<Function> functions;
};

class Lowerer {
public:
    [[nodiscard]] Module lower(const semantic::SemanticModel& model);

private:
    [[nodiscard]] Function lowerFunction(const semantic::BoundFunction& function);
    void collectLocalTypes(const semantic::BoundStatement& statement);
    void lowerStatement(const semantic::BoundStatement& statement);
    [[nodiscard]] ValueId lowerExpression(const semantic::BoundExpression& expression);
    [[nodiscard]] ValueId lowerShortCircuit(const semantic::BoundBinaryExpression& expression);

    [[nodiscard]] BlockId createBlock();
    [[nodiscard]] ValueId addBlockParameter(BlockId block, semantic::PrimitiveType type);
    [[nodiscard]] BasicBlock& block(BlockId id);
    [[nodiscard]] const BasicBlock& block(BlockId id) const;
    [[nodiscard]] bool hasCurrentBlock() const noexcept;
    [[nodiscard]] bool currentBlockTerminated() const;
    void setCurrentBlock(BlockId id);
    void clearCurrentBlock() noexcept;

    [[nodiscard]] ValueId emitValue(
        Opcode opcode,
        semantic::PrimitiveType type,
        std::vector<ValueId> operands,
        text::TextSpan sourceSpan);
    void emitStoreLocal(std::size_t localIndex, ValueId value, text::TextSpan sourceSpan);
    void emitJump(BlockId target, std::vector<ValueId> arguments, text::TextSpan sourceSpan);
    void emitBranch(
        ValueId condition,
        BlockId trueTarget,
        BlockId falseTarget,
        std::vector<ValueId> trueArguments,
        std::vector<ValueId> falseArguments,
        text::TextSpan sourceSpan);
    void emitReturn(std::optional<ValueId> value, text::TextSpan sourceSpan);

    Function* currentFunction_ = nullptr;
    std::optional<BlockId> currentBlockId_;
    ValueId nextValueId_ = 0;
};

[[nodiscard]] const char* opcodeName(Opcode opcode) noexcept;
[[nodiscard]] const char* terminatorName(TerminatorKind kind) noexcept;
[[nodiscard]] bool verifyModule(const Module& module, diagnostics::DiagnosticBag& diagnostics);
[[nodiscard]] std::string printModule(const Module& module);

} // namespace realscript::mir
