#pragma once

#include "realscript/compiler/LanguageMetadata.h"
#include "realscript/debug/DebugInfo.h"
#include "realscript/diagnostics/Diagnostic.h"
#include "realscript/mir/Mir.h"
#include "realscript/semantic/Semantic.h"

#include <cstdint>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace realscript::bytecode {

using Register = std::uint32_t;
using BlockId = std::uint32_t;

constexpr Register InvalidRegister = std::numeric_limits<Register>::max();

struct Version {
    std::uint16_t major = 0;
    std::uint16_t minor = 7;
};

enum class Opcode : std::uint8_t {
    LoadParameter,
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
    NewDelegate,
    InvokeDelegate,
    CombineDelegate,
    RemoveDelegate,
    ConvertNumeric,
    IsType,
    AsType,
    ConstantTypeId,
};

struct FunctionReference {
    semantic::SymbolId symbolId = 0;
    std::string name;
    semantic::PrimitiveType returnType = semantic::PrimitiveType::Error;
    semantic::SymbolId returnTypeId = 0;
    std::vector<semantic::PrimitiveType> parameterTypes;
    std::vector<semantic::SymbolId> parameterTypeIds;
    bool virtualDispatch = false;
    std::uint32_t virtualSlot = std::numeric_limits<std::uint32_t>::max();
    bool interfaceDispatch = false;
    semantic::SymbolId interfaceTypeId = 0;
    std::uint32_t interfaceSlot = std::numeric_limits<std::uint32_t>::max();
};

class SmallRegisterVector {
public:
    constexpr SmallRegisterVector() noexcept = default;
    SmallRegisterVector(std::initializer_list<Register> values) {
        for (const auto v : values) push_back(v);
    }
    SmallRegisterVector(const std::vector<Register>& values) {
        for (const auto v : values) push_back(v);
    }
    operator std::vector<Register>() const {
        return std::vector<Register>(begin(), end());
    }

    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return static_cast<std::size_t>(sizeAndSlice_ & 0xFFFu);
    }
    [[nodiscard]] constexpr bool empty() const noexcept {
        return size() == 0;
    }
    void reserve(std::size_t) noexcept {}
    void clear() noexcept {
        sizeAndSlice_ = 0;
        inline0_ = 0;
    }

    [[nodiscard]] const Register* data() const noexcept {
        const std::uint32_t id = sliceId();
        return id == 0 ? &inline0_ : sliceData(id);
    }
    [[nodiscard]] const Register* begin() const noexcept { return data(); }
    [[nodiscard]] const Register* end() const noexcept { return data() + size(); }

    [[nodiscard]] const Register& operator[](std::size_t index) const noexcept {
        return data()[index];
    }
    [[nodiscard]] const Register& front() const noexcept { return (*this)[0]; }
    [[nodiscard]] const Register& back() const noexcept { return (*this)[size() - 1]; }

    void push_back(Register value) {
        const auto curSize = static_cast<std::uint16_t>(size());
        if (curSize >= 0xFFFu) {
            throw std::overflow_error(
                "SmallRegisterVector exceeded 12-bit operand count limit (4095)");
        }
        const auto curSlice = sliceId();
        if (curSlice == 0 && curSize == 0) {
            inline0_ = value;
            sizeAndSlice_ = 1u;
            return;
        }
        const auto newSlice = appendToSlice(curSlice, inline0_, curSize, value);
        const auto nextSize = static_cast<std::uint32_t>((curSize + 1u) & 0xFFFu);
        sizeAndSlice_ = (newSlice << 12u) | nextSize;
    }

    friend bool operator==(
        const SmallRegisterVector& left,
        const SmallRegisterVector& right) noexcept {
        if (left.sizeAndSlice_ == right.sizeAndSlice_ &&
            (left.sliceId() != 0 || left.inline0_ == right.inline0_)) {
            return true;
        }
        const auto sz = left.size();
        if (sz != right.size()) return false;
        const auto* ld = left.data();
        const auto* rd = right.data();
        for (std::size_t i = 0; i < sz; ++i) {
            if (ld[i] != rd[i]) return false;
        }
        return true;
    }
    friend bool operator!=(
        const SmallRegisterVector& left,
        const SmallRegisterVector& right) noexcept {
        return !(left == right);
    }

private:
    [[nodiscard]] constexpr std::uint32_t sliceId() const noexcept {
        return sizeAndSlice_ >> 12u;
    }
    static const Register* sliceData(std::uint32_t id) noexcept;
    static std::uint32_t appendToSlice(
        std::uint32_t id,
        Register firstInline,
        std::uint16_t currentSize,
        Register value);

    std::uint32_t sizeAndSlice_ = 0;
    Register inline0_ = 0;
};
static_assert(sizeof(SmallRegisterVector) == 8, "SmallRegisterVector must be 8 bytes");
static_assert(std::is_trivially_copyable_v<SmallRegisterVector>);

class BytecodeString {
public:
    constexpr BytecodeString() noexcept = default;
    BytecodeString(const std::string& value);
    BytecodeString(std::string&& value);
    BytecodeString(std::string_view value);
    BytecodeString(const char* value);

    [[nodiscard]] const std::string& str() const noexcept;
    operator const std::string&() const noexcept { return str(); }
    operator std::string_view() const noexcept { return str(); }
    [[nodiscard]] const char* c_str() const noexcept { return str().c_str(); }
    [[nodiscard]] const char* data() const noexcept { return str().data(); }
    [[nodiscard]] std::size_t size() const noexcept { return str().size(); }
    [[nodiscard]] bool empty() const noexcept { return poolId_ == 0 || str().empty(); }
    [[nodiscard]] auto begin() const noexcept { return str().begin(); }
    [[nodiscard]] auto end() const noexcept { return str().end(); }

    friend bool operator==(const BytecodeString& l, const BytecodeString& r) noexcept {
        return l.poolId_ == r.poolId_ || l.str() == r.str();
    }
    friend bool operator!=(const BytecodeString& l, const BytecodeString& r) noexcept {
        return !(l == r);
    }
    friend bool operator==(const BytecodeString& l, const std::string& r) noexcept {
        return l.str() == r;
    }
    friend bool operator==(const std::string& l, const BytecodeString& r) noexcept {
        return l == r.str();
    }
    friend bool operator!=(const BytecodeString& l, const std::string& r) noexcept {
        return l.str() != r;
    }
    friend bool operator!=(const std::string& l, const BytecodeString& r) noexcept {
        return l != r.str();
    }
    friend bool operator==(const BytecodeString& l, const char* r) noexcept {
        return l.str() == (r ? r : "");
    }
    friend bool operator==(const char* l, const BytecodeString& r) noexcept {
        return (l ? l : "") == r.str();
    }
    friend std::ostream& operator<<(std::ostream& os, const BytecodeString& s) {
        return os << s.str();
    }

private:
    std::uint16_t poolId_ = 0;
};
static_assert(sizeof(BytecodeString) == 2, "BytecodeString must be 2 bytes");
static_assert(std::is_trivially_copyable_v<BytecodeString>);

struct Instruction {
    Opcode opcode = Opcode::ConstantInt;
    semantic::PrimitiveType elementType = semantic::PrimitiveType::Error;
    bool boolImmediate = false;
    bool checkedArithmetic = true;
    Register result = InvalidRegister;
    SmallRegisterVector operands{};
    std::uint32_t index = 0;
    std::uint16_t typeIndex = 0;
    BytecodeString stringImmediate{};
    union {
        semantic::SymbolId elementTypeId = 0;
        std::int64_t integerImmediate;
        double doubleImmediate;
    };

    [[nodiscard]] constexpr bool usesIntegerImmediate() const noexcept {
        return opcode == Opcode::ConstantInt || opcode == Opcode::ConstantTypeId || opcode == Opcode::LoadParameter;
    }
    [[nodiscard]] constexpr bool usesDoubleImmediate() const noexcept {
        return opcode == Opcode::ConstantDouble;
    }
    [[nodiscard]] constexpr bool usesElementTypeId() const noexcept {
        return !usesIntegerImmediate() && !usesDoubleImmediate();
    }
    [[nodiscard]] constexpr semantic::SymbolId activeElementTypeId() const noexcept {
        return usesElementTypeId() ? elementTypeId : 0u;
    }
    [[nodiscard]] constexpr std::int64_t activeIntegerImmediate() const noexcept {
        return usesIntegerImmediate() ? integerImmediate : 0;
    }
    [[nodiscard]] constexpr double activeDoubleImmediate() const noexcept {
        return usesDoubleImmediate() ? doubleImmediate : 0.0;
    }
};
static_assert(sizeof(Instruction) == 32, "Instruction must be 32 bytes");
static_assert(std::is_trivially_copyable_v<Instruction>);

struct BlockParameter {
    Register target = InvalidRegister;
    semantic::PrimitiveType type = semantic::PrimitiveType::Error;
    semantic::SymbolId typeId = 0;
};

enum class TerminatorKind : std::uint8_t {
    None,
    Jump,
    Branch,
    ReturnValue,
    ReturnVoid,
    Throw,
};

struct Terminator {
    TerminatorKind kind = TerminatorKind::None;
    std::uint8_t reserved0_ = 0;
    std::uint16_t target = 0;
    Register condition = InvalidRegister;
    Register value = InvalidRegister;
    std::uint16_t falseTarget = 0;
    std::uint16_t reserved1_ = 0;
    SmallRegisterVector arguments{};
    SmallRegisterVector falseArguments{};
};
static_assert(sizeof(Terminator) == 32, "Terminator must be 32 bytes");
static_assert(std::is_trivially_copyable_v<Terminator>);

struct BasicBlock {
    BlockId id = 0;
    std::vector<BlockParameter> parameters;
    std::vector<Instruction> instructions;
    Terminator terminator;
};

struct ExceptionHandler {
    std::vector<BlockId> protectedBlocks;
    BlockId handlerBlock = 0;
    semantic::SymbolId catchTypeId = 0;
    std::size_t exceptionLocal = 0;
};

struct Function {
    semantic::SymbolId symbolId = 0;
    std::string name;
    semantic::PrimitiveType returnType = semantic::PrimitiveType::Error;
    semantic::SymbolId returnTypeId = 0;
    std::vector<semantic::PrimitiveType> parameterTypes;
    std::vector<semantic::SymbolId> parameterTypeIds;
    std::vector<semantic::PrimitiveType> localTypes;
    std::vector<semantic::SymbolId> localTypeIds;
    std::vector<semantic::PrimitiveType> registerTypes;
    std::vector<semantic::SymbolId> registerTypeIds;
    std::vector<BasicBlock> blocks;
    std::vector<ExceptionHandler> exceptionHandlers;
    debug::FunctionDebugInfo debugInfo;
};

struct Module {
    Version version;
    std::string name;
    compiler::LanguageModuleMetadata languageMetadata;
    std::vector<debug::SourceFileInfo> sourceFiles;
    std::vector<semantic::TypeSymbol> types;
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
