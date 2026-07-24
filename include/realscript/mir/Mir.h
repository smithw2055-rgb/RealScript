#pragma once

#include "realscript/semantic/Semantic.h"
#include "realscript/text/Text.h"

#include <cstdint>
#include <string>
#include <vector>

namespace realscript::mir {

using ValueId = std::int32_t;

enum class Opcode {
    Parameter,
    ConstantInt,
    ConstantBool,
    ConstantString,
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
    LogicalAnd,
    LogicalOr,
    ReturnValue,
    ReturnVoid,
};

struct Instruction {
    ValueId result = -1;
    semantic::PrimitiveType resultType = semantic::PrimitiveType::Void;
    Opcode opcode = Opcode::ReturnVoid;
    std::vector<ValueId> operands;
    std::int64_t integerImmediate = 0;
    bool boolImmediate = false;
    std::string stringImmediate;
    text::TextSpan sourceSpan;
};

struct BasicBlock {
    std::uint32_t id = 0;
    std::vector<Instruction> instructions;
};

struct Function {
    std::string name;
    semantic::PrimitiveType returnType = semantic::PrimitiveType::Error;
    std::vector<semantic::PrimitiveType> parameterTypes;
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
    void lowerStatement(const semantic::BoundStatement& statement);
    [[nodiscard]] ValueId lowerExpression(const semantic::BoundExpression& expression);
    [[nodiscard]] ValueId emitValue(
        Opcode opcode,
        semantic::PrimitiveType type,
        std::vector<ValueId> operands,
        text::TextSpan sourceSpan);
    void emitTerminator(Opcode opcode, std::vector<ValueId> operands, text::TextSpan sourceSpan);

    Function* currentFunction_ = nullptr;
    BasicBlock* currentBlock_ = nullptr;
    std::vector<ValueId> variableValues_;
    ValueId nextValueId_ = 0;
};

[[nodiscard]] const char* opcodeName(Opcode opcode) noexcept;
[[nodiscard]] std::string printModule(const Module& module);

} // namespace realscript::mir
