#pragma once

#include "realscript/diagnostics/Diagnostic.h"
#include "realscript/mir/Mir.h"
#include "realscript/semantic/Semantic.h"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace realscript::bytecode {

using Register = std::uint32_t;
using BlockId = std::uint32_t;

constexpr Register InvalidRegister = std::numeric_limits<Register>::max();

struct Version {
    std::uint16_t major = 0;
    std::uint16_t minor = 1;
};

enum class Opcode : std::uint8_t {
    LoadParameter,
    ConstantInt,
    ConstantBool,
    ConstantString,
    ConstantNull,
    LoadLocal,
    StoreLocal,
    ConvertNullToString,
    Call,
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

struct FunctionReference {
    semantic::SymbolId symbolId = 0;
    std::string name;
    semantic::PrimitiveType returnType = semantic::PrimitiveType::Error;
    std::vector<semantic::PrimitiveType> parameterTypes;
};

struct Instruction {
    Opcode opcode = Opcode::ConstantInt;
    Register result = InvalidRegister;
    std::vector<Register> operands;
    std::uint32_t index = 0;
    std::int64_t integerImmediate = 0;
    bool boolImmediate = false;
    std::string stringImmediate;
};

struct BlockParameter {
    Register target = InvalidRegister;
    semantic::PrimitiveType type = semantic::PrimitiveType::Error;
};

enum class TerminatorKind : std::uint8_t {
    None,
    Jump,
    Branch,
    ReturnValue,
    ReturnVoid,
};

struct Terminator {
    TerminatorKind kind = TerminatorKind::None;
    Register condition = InvalidRegister;
    Register value = InvalidRegister;
    BlockId target = 0;
    BlockId falseTarget = 0;
    std::vector<Register> arguments;
    std::vector<Register> falseArguments;
};

struct BasicBlock {
    BlockId id = 0;
    std::vector<BlockParameter> parameters;
    std::vector<Instruction> instructions;
    Terminator terminator;
};

struct Function {
    semantic::SymbolId symbolId = 0;
    std::string name;
    semantic::PrimitiveType returnType = semantic::PrimitiveType::Error;
    std::vector<semantic::PrimitiveType> parameterTypes;
    std::vector<semantic::PrimitiveType> localTypes;
    std::vector<semantic::PrimitiveType> registerTypes;
    std::vector<BasicBlock> blocks;
};

struct Module {
    Version version;
    std::string name;
    std::vector<FunctionReference> functionReferences;
    std::vector<Function> functions;
};

class Lowerer {
public:
    [[nodiscard]] Module lower(const mir::Module& module) const;
};

[[nodiscard]] const char* opcodeName(Opcode opcode) noexcept;
[[nodiscard]] const char* terminatorName(TerminatorKind kind) noexcept;
[[nodiscard]] bool verifyModule(
    const Module& module,
    diagnostics::DiagnosticBag& diagnostics);
[[nodiscard]] std::string disassembleModule(const Module& module);

[[nodiscard]] std::vector<std::uint8_t> encodeModule(const Module& module);
[[nodiscard]] bool decodeModule(
    const std::vector<std::uint8_t>& bytes,
    Module& module,
    diagnostics::DiagnosticBag& diagnostics);
[[nodiscard]] std::string bytesToHex(const std::vector<std::uint8_t>& bytes);

} // namespace realscript::bytecode
