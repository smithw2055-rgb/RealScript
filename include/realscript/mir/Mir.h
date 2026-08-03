#pragma once

#include "realscript/compiler/LanguageMetadata.h"
#include "realscript/debug/DebugInfo.h"
#include "realscript/diagnostics/Diagnostic.h"
#include "realscript/semantic/Semantic.h"
#include "realscript/text/Text.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <limits>
#include <string>
#include <vector>

namespace realscript::mir {

using ValueId = std::int32_t;
using BlockId = std::uint32_t;

enum class Opcode {
    Parameter,
    ConstantInt,
    ConstantDouble,
    ConstantBool,
    ConstantString,
    ConstantNull,
    LoadLocal,
    StoreLocal,
    ConvertNullToString,
    ConvertNullToObject,
    ConvertNullToArray,
    ConvertIntToLong,
    ConvertIntToDouble,
    ConvertLongToDouble,
    NewObject,
    NewStruct,
    NewArray,
    CheckNotNull,
    ArrayLength,
    LoadElement,
    StoreElement,
    LoadField,
    StoreField,
    LoadStructField,
    StoreStructField,
    Call,
    NegateInt,
    NegateLong,
    NegateDouble,
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

struct Instruction {
    ValueId result = -1;
    semantic::PrimitiveType resultType = semantic::PrimitiveType::Void;
    semantic::SymbolId resultTypeId = 0;
    Opcode opcode = Opcode::ConstantInt;
    std::vector<ValueId> operands;
    std::size_t localIndex = 0;
    semantic::SymbolId typeId = 0;
    std::size_t fieldIndex = 0;
    semantic::PrimitiveType elementType = semantic::PrimitiveType::Error;
    semantic::SymbolId elementTypeId = 0;
    std::int64_t integerImmediate = 0;
    double doubleImmediate = 0.0;
    bool boolImmediate = false;
    std::string stringImmediate;
    semantic::SymbolId symbolId = 0;
    bool virtualDispatch = false;
    std::uint32_t virtualSlot = std::numeric_limits<std::uint32_t>::max();
    std::string symbolName;
    std::vector<semantic::PrimitiveType> parameterTypes;
    std::vector<semantic::SymbolId> parameterTypeIds;
    text::TextSpan sourceSpan;
};

struct BlockParameter {
    ValueId value = -1;
    semantic::PrimitiveType type = semantic::PrimitiveType::Error;
    semantic::SymbolId typeId = 0;
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
    semantic::SymbolId symbolId = 0;
    std::string moduleName;
    std::string name;
    semantic::PrimitiveType returnType = semantic::PrimitiveType::Error;
    semantic::SymbolId returnTypeId = 0;
    std::vector<semantic::PrimitiveType> parameterTypes;
    std::vector<semantic::SymbolId> parameterTypeIds;
    std::vector<semantic::PrimitiveType> localTypes;
    std::vector<semantic::SymbolId> localTypeIds;
    std::vector<BasicBlock> blocks;
    debug::FunctionDebugInfo debugInfo;
};

struct Module {
    std::string name;
    compiler::LanguageModuleMetadata languageMetadata;
    std::vector<debug::SourceFileInfo> sourceFiles;
    std::vector<semantic::TypeSymbol> types;
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
    [[nodiscard]] ValueId lowerShortCircuit(
        const semantic::BoundBinaryExpression& expression);

    [[nodiscard]] BlockId createBlock();
    [[nodiscard]] ValueId addBlockParameter(
        BlockId block,
        semantic::PrimitiveType type,
        semantic::SymbolId typeId = 0);
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
    void emitStoreLocal(
        std::size_t localIndex,
        ValueId value,
        text::TextSpan sourceSpan);
    void emitJump(
        BlockId target,
        std::vector<ValueId> arguments,
        text::TextSpan sourceSpan);
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
    std::vector<BlockId> breakTargets_;
    std::vector<BlockId> continueTargets_;
};

[[nodiscard]] const char* opcodeName(Opcode opcode) noexcept;
[[nodiscard]] const char* terminatorName(TerminatorKind kind) noexcept;
[[nodiscard]] bool verifyModule(
    const Module& module,
    diagnostics::DiagnosticBag& diagnostics);
[[nodiscard]] std::string printModule(const Module& module);

} // namespace realscript::mir
