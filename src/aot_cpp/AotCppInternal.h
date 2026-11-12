#pragma once

#include "realscript/aot_cpp/AotCpp.h"
#include "realscript/aot_cpp/AotRuntime.h"
#include "realscript/compiler/Compilation.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace realscript::aot::detail {

class Emitter {
public:
    void line(const std::string& value = {}) {
        text_ += value;
        text_ += '\n';
        ++line_;
    }

    void raw(const std::string& value) {
        text_ += value;
        line_ += static_cast<std::uint32_t>(
            std::count(value.begin(), value.end(), '\n'));
    }

    [[nodiscard]] std::uint32_t nextLine() const noexcept { return line_ + 1; }
    [[nodiscard]] const std::string& str() const noexcept { return text_; }

private:
    std::string text_;
    std::uint32_t line_ = 0;
};

struct FunctionView {
    const mir::Module* module = nullptr;
    const mir::Function* function = nullptr;
    std::string qualifiedName;
    std::string cppName;
};

struct TypeView {
    const semantic::TypeSymbol* type = nullptr;
    std::string cppName;
};

struct CallSite {
    std::size_t id = 0;
    const mir::Instruction* instruction = nullptr;
};

struct SourceMapRecord {
    semantic::SymbolId symbolId = 0;
    std::uint32_t generatedLine = 0;
    std::string sourcePath;
    std::uint32_t sourceLine = 0;
    std::uint32_t sourceColumn = 0;
};

enum class InlineMathKind : std::uint8_t {
    None = 0,
    Fixed,
    Fixed16,
    Fixed64,
    Vec2,
    Vec3,
    Quat,
    Vec2f,
    Vec3f,
    Quatf,
};

struct TypedSlotType {
    semantic::PrimitiveType primitive = semantic::PrimitiveType::Error;
    semantic::SymbolId typeId = 0;

    friend constexpr bool operator==(TypedSlotType left, TypedSlotType right) noexcept {
        return left.primitive == right.primitive && left.typeId == right.typeId;
    }
    friend constexpr bool operator!=(TypedSlotType left, TypedSlotType right) noexcept {
        return !(left == right);
    }
};

struct TypedRangeAnalysis {
    std::set<mir::ValueId> safeCheckedArithmetic;
};

struct VectorizedEcsColumnBinding {
    std::size_t colLocalIndex = 0;
    std::size_t iterLocalIndex = 0;
    semantic::SymbolId elemTypeId = 0;
    bool isWrite = false;
};

struct VectorizedEcsLoopPlan {
    mir::BlockId headerBlockId = 0;
    mir::BlockId bodyBlockId = 0;
    mir::BlockId incrementBlockId = 0;
    mir::BlockId exitBlockId = 0;
    std::size_t indexLocalIndex = 0;
    std::size_t bodyPreambleCount = 0;
    bool hasSparseWritebackCall = false;
    bool usesIndexInsideBody = false;
    std::vector<VectorizedEcsColumnBinding> columns;
};

struct VectorizedPackedArrayBinding {
    std::size_t arrayLocalIndex = 0;
    semantic::PrimitiveType elemPrim = semantic::PrimitiveType::Error;
    bool isRead = false;
    bool isWrite = false;
};

struct VectorizedPackedArrayLoopPlan {
    mir::BlockId headerBlockId = 0;
    mir::BlockId bodyBlockId = 0;
    mir::BlockId incrementBlockId = 0;
    mir::BlockId exitBlockId = 0;
    std::size_t indexLocalIndex = 0;
    std::string boundExpr;
    std::vector<VectorizedPackedArrayBinding> arrays;
    std::unordered_map<mir::ValueId, std::size_t> bodyArrayLoadRegs;
    std::unordered_set<mir::ValueId> bodyIndexLoadRegs;
    std::uint64_t headerInstsWithTerminator = 0;
    std::uint64_t iterInstsWithTerminators = 0;
    std::uint64_t iterBranches = 3;
};

// Shared formatting & MIR query utilities (defined in AotTypedAnalysis.cpp)
[[nodiscard]] std::string hexId(semantic::SymbolId id);
[[nodiscard]] std::string primitiveExpression(semantic::PrimitiveType type);
[[nodiscard]] std::string typeKindExpression(semantic::TypeKind kind);
[[nodiscard]] semantic::SymbolId exactTypeId(semantic::PrimitiveType type, const std::string& name);
[[nodiscard]] std::string conversionExpression(mir::Opcode opcode);
[[nodiscard]] std::string unaryExpression(mir::Opcode opcode);
[[nodiscard]] std::string binaryExpression(mir::Opcode opcode);
[[nodiscard]] bool isConversion(mir::Opcode opcode);
[[nodiscard]] bool isUnary(mir::Opcode opcode);
[[nodiscard]] bool isBinary(mir::Opcode opcode);
[[nodiscard]] std::size_t registerCount(const mir::Function& function);
[[nodiscard]] const mir::BasicBlock* findBlock(
    const mir::Function& function,
    mir::BlockId id);
[[nodiscard]] const debug::SequencePoint* findPoint(
    const mir::Function& function,
    mir::BlockId blockId,
    std::uint32_t instructionIndex,
    bool terminator);
[[nodiscard]] const debug::SourceFileInfo* findSource(
    const mir::Module& module,
    debug::SourceFileId id);
[[nodiscard]] const debug::SourceFileInfo* findFunctionSource(
    const mir::Module& module,
    const mir::Function& function);
[[nodiscard]] std::optional<debug::SequencePoint> resolveSequencePoint(
    const mir::Module& module,
    const mir::Function& function,
    mir::BlockId blockId,
    std::uint32_t instructionIndex,
    bool terminator,
    text::TextSpan fallbackSpan,
    bool* outExplicitSequencePoint = nullptr);
bool emitLineDirective(
    Emitter& output,
    const mir::Module& module,
    const std::optional<debug::SequencePoint>& point,
    bool enabled,
    std::vector<SourceMapRecord>* sourceMap,
    semantic::SymbolId symbolId,
    bool recordInMap = true);
void emitGeneratedLineReset(Emitter& output, bool enabled);
[[nodiscard]] std::string valueExpression(mir::ValueId id);

// Typed slot & accounting analysis (defined in AotTypedAnalysis.cpp)
[[nodiscard]] InlineMathKind classifyInlineMath(
    semantic::PrimitiveType type,
    semantic::SymbolId typeId) noexcept;
[[nodiscard]] const char* inlineMathFieldMember(
    InlineMathKind kind,
    std::size_t fieldIndex) noexcept;
[[nodiscard]] bool isTypedAotSlot(
    TypedSlotType slot,
    const std::unordered_set<semantic::SymbolId>* customStructs = nullptr) noexcept;
[[nodiscard]] std::vector<TypedSlotType> valueSlotTypes(
    const mir::Function& function);
[[nodiscard]] bool supportsTypedPrimitiveAot(
    const mir::Function& function,
    const std::unordered_set<semantic::SymbolId>& customStructs);
[[nodiscard]] std::string typedCppType(TypedSlotType slot);
[[nodiscard]] std::string typedDefaultValue(TypedSlotType slot);
[[nodiscard]] std::string typedValueExpression(mir::ValueId id);
[[nodiscard]] std::string typedLocalExpression(std::size_t index);
[[nodiscard]] std::string int64Literal(std::int64_t value);
[[nodiscard]] std::string typedScalarFunctionSignature(
    const FunctionView& view,
    bool declarationOnly);
void emitTypedTransfer(
    Emitter& output,
    const mir::Function& function,
    mir::BlockId target,
    const std::vector<mir::ValueId>& arguments,
    const std::string& prefix,
    const std::string& indent,
    const std::vector<std::size_t>* regGuardSlot = nullptr);
void emitTypedRawConsume(
    Emitter& output,
    std::uint64_t count,
    const std::string& indent);
void emitTypedConsume(
    Emitter& output,
    mir::Opcode opcode,
    const std::string& indent);
void emitTypedConsume(
    Emitter& output,
    mir::TerminatorKind terminator,
    const std::string& indent);
[[nodiscard]] bool typedCheckedArithmeticProvenSafe(
    const TypedRangeAnalysis& analysis,
    const mir::Instruction& instruction) noexcept;
[[nodiscard]] bool typedInstructionCanFail(
    const TypedRangeAnalysis& analysis,
    const mir::Instruction& instruction) noexcept;
[[nodiscard]] std::vector<std::uint64_t> typedRawConsumeBatches(
    const mir::BasicBlock& block,
    const TypedRangeAnalysis& analysis);

// Integer range abstract interpretation (defined in AotRangeAnalysis.cpp)
[[nodiscard]] TypedRangeAnalysis analyzeTypedIntegerRanges(
    const mir::Function& function);

// SIMD & zero-copy ECS / packed array vectorization (defined in AotSimdVectorizer.cpp)
[[nodiscard]] std::string packedArrayCacheName(
    std::size_t localIndex,
    semantic::PrimitiveType elemPrim);
[[nodiscard]] std::unordered_map<mir::BlockId, VectorizedEcsLoopPlan>
analyzeVectorizedEcsLoops(
    const mir::Function& function,
    const std::unordered_map<std::size_t, semantic::SymbolId>& arrayLocalStructElemType);
void emitVectorizedEcsLoop(
    Emitter& source,
    const mir::Module& module,
    const mir::Function& function,
    const std::vector<TypedSlotType>& types,
    const VectorizedEcsLoopPlan& plan,
    bool emitLineDirectives);
[[nodiscard]] std::unordered_map<mir::BlockId, VectorizedPackedArrayLoopPlan>
analyzeVectorizedPackedArrayLoops(
    const mir::Function& function,
    const std::vector<TypedSlotType>& types,
    const TypedRangeAnalysis& rangeAnalysis,
    const std::unordered_map<std::size_t, semantic::PrimitiveType>& singleStorePackedArrayLocals);
void emitVectorizedPackedArrayLoop(
    Emitter& source,
    const mir::Function& function,
    const std::vector<TypedSlotType>& types,
    const VectorizedPackedArrayLoopPlan& plan);

// Typed scalar & unboxed struct function emission (defined in AotTypedEmitter.cpp)
[[nodiscard]] bool aotTypeAssignable(
    semantic::SymbolId actual,
    semantic::SymbolId expected,
    const std::map<semantic::SymbolId, TypeView>& allTypes) noexcept;
void emitTypedPrimitiveFunction(
    Emitter& source,
    const FunctionView& view,
    const GenerationOptions& options,
    std::vector<SourceMapRecord>& sourceMap,
    const std::unordered_map<semantic::SymbolId, const FunctionView*>& typedFunctions,
    const std::map<semantic::SymbolId, TypeView>& allTypes,
    const std::unordered_map<const mir::Instruction*, std::size_t>& callIds);

} // namespace realscript::aot::detail
