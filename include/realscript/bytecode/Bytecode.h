#pragma once

#include "realscript/compiler/LanguageMetadata.h"
#include "realscript/debug/DebugInfo.h"
#include "realscript/diagnostics/Diagnostic.h"
#include "realscript/semantic/Semantic.h"

#include <cstdint>
#include <cstdlib>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace realscript::mir { struct Module; }

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
    AsyncEnter,
    AwaitSuspend,
    AwaitResume,
    AsyncResolve,
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
        assign(values.begin(), values.size());
    }
    SmallRegisterVector(const std::vector<Register>& values) {
        assign(values.data(), values.size());
    }
    SmallRegisterVector(const Register* values, std::size_t count) {
        assign(values, count);
    }
    operator std::vector<Register>() const {
        return std::vector<Register>(begin(), end());
    }

    void assign(const Register* values, std::size_t count) {
        if (count == 0u || !values) {
            clear();
            return;
        }
        if (count > 0xFFFu) {
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
            throw std::overflow_error(
                "SmallRegisterVector exceeded 12-bit operand count limit (4095)");
#else
            std::abort();
#endif
        }
        if (count == 1u) {
            inline0_ = values[0];
            sizeAndSlice_ = 1u;
            return;
        }
        const auto newSlice = internSlice(values, static_cast<std::uint16_t>(count));
        inline0_ = 0;
        sizeAndSlice_ = (newSlice << 12u) | static_cast<std::uint32_t>(count);
    }

    [[nodiscard]] static std::size_t internedSliceCount() noexcept;
    // Test/teardown helper only: invalidates all existing slice IDs in the global pool.
    // Must not be called while any bytecode::Module, ProgramImage, or SmallRegisterVector is live.
    static void clearSlicePool() noexcept;

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
        const std::uint32_t id = sliceId();
        return id == 0 ? inline0_ : sliceData(id)[index];
    }
    [[nodiscard]] const Register& front() const noexcept { return (*this)[0]; }
    [[nodiscard]] const Register& back() const noexcept { return (*this)[size() - 1]; }

    void push_back(Register value) {
        const auto curSize = static_cast<std::uint16_t>(size());
        if (curSize >= 0xFFFu) {
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
            throw std::overflow_error(
                "SmallRegisterVector exceeded 12-bit operand count limit (4095)");
#else
            std::abort();
#endif
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
    static std::uint32_t internSlice(
        const Register* values,
        std::uint16_t count);
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
    [[nodiscard]] static std::size_t internedStringCount() noexcept;
    // Test/teardown helper only: invalidates all existing string pointers/IDs in the global pool.
    // Must not be called while any bytecode::Module, ProgramImage, or BytecodeString is live.
    static void clearStringPool() noexcept;
    [[nodiscard]] constexpr bool empty() const noexcept { return poolId_ == 0u; }
    [[nodiscard]] constexpr std::uint32_t poolId() const noexcept { return poolId_; }
    [[nodiscard]] auto begin() const noexcept { return str().begin(); }
    [[nodiscard]] auto end() const noexcept { return str().end(); }

    friend constexpr bool operator==(const BytecodeString& l, const BytecodeString& r) noexcept {
        return l.poolId_ == r.poolId_;
    }
    friend constexpr bool operator!=(const BytecodeString& l, const BytecodeString& r) noexcept {
        return l.poolId_ != r.poolId_;
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
    friend bool operator!=(const BytecodeString& l, const char* r) noexcept {
        return !(l == r);
    }
    friend bool operator!=(const char* l, const BytecodeString& r) noexcept {
        return !(l == r);
    }
    friend std::ostream& operator<<(std::ostream& os, const BytecodeString& s) {
        return os << s.str();
    }

private:
    std::uint32_t poolId_ = 0;
};
static_assert(sizeof(BytecodeString) == 4, "BytecodeString must be 4 bytes");
static_assert(std::is_trivially_copyable_v<BytecodeString>);

struct Instruction {
    constexpr Instruction() noexcept
        : elementTypeId(0) {}

    Opcode opcode = Opcode::ConstantInt;
    semantic::PrimitiveType elementType = semantic::PrimitiveType::Error;
    bool boolImmediate = false;
    bool checkedArithmetic = true;
    Register result = InvalidRegister;
    SmallRegisterVector operands{};
    std::uint32_t index = 0;
    std::uint16_t typeIndex = 0;
    std::uint16_t reserved0_ = 0;
    union {
        semantic::SymbolId elementTypeId;
        std::int64_t integerImmediate;
        double doubleImmediate;
        BytecodeString stringImmediate;
    };

    [[nodiscard]] constexpr bool usesIntegerImmediate() const noexcept {
        return opcode == Opcode::ConstantInt || opcode == Opcode::ConstantTypeId || opcode == Opcode::LoadParameter || opcode == Opcode::AwaitSuspend;
    }
    [[nodiscard]] constexpr bool usesDoubleImmediate() const noexcept {
        return opcode == Opcode::ConstantDouble;
    }
    [[nodiscard]] constexpr bool usesStringImmediate() const noexcept {
        return opcode == Opcode::ConstantString;
    }
    [[nodiscard]] constexpr bool usesElementTypeId() const noexcept {
        return !usesIntegerImmediate() && !usesDoubleImmediate() && !usesStringImmediate();
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
    [[nodiscard]] constexpr BytecodeString activeStringImmediate() const noexcept {
        return usesStringImmediate() ? stringImmediate : BytecodeString{};
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
    semantic::Accessibility accessibility = semantic::Accessibility::Public;
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
    const std::uint8_t* data,
    std::size_t size,
    Module& module,
    diagnostics::DiagnosticBag& diagnostics);
[[nodiscard]] bool decodeModule(
    const std::vector<std::uint8_t>& bytes,
    Module& module,
    diagnostics::DiagnosticBag& diagnostics);
[[nodiscard]] std::string bytesToHex(const std::vector<std::uint8_t>& bytes);

struct LazyFunctionBodyLoc {
    std::uint32_t moduleIndex = 0;
    std::uint32_t functionIndex = 0;
    std::uint32_t chunkIndex = 0;
    std::uint32_t offsetInChunk = 0;
    std::uint32_t uncompressedSize = 0;
};
static_assert(
    sizeof(LazyFunctionBodyLoc) == 20 &&
    alignof(LazyFunctionBodyLoc) == 4 &&
    std::is_trivially_copyable_v<LazyFunctionBodyLoc>,
    "LazyFunctionBodyLoc must match packed v4 .rodata layout");

struct LazyBytecodeChunk {
    std::uint32_t uncompressedSize = 0;
    std::uint32_t compressedOffset = 0;
    std::uint32_t compressedSize = 0;
    std::uint64_t checksum = 0;
};

struct LazyStringSpan {
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
};
static_assert(
    sizeof(LazyStringSpan) == 8 &&
    alignof(LazyStringSpan) == 4 &&
    std::is_trivially_copyable_v<LazyStringSpan>,
    "LazyStringSpan must match packed v4 .rodata layout");

struct LazyBytecodeBundle {
    std::vector<Module> headerModules;
    std::vector<std::string> sharedStrings;
    std::vector<char> flatStringData;
    std::vector<LazyStringSpan> flatStringSpans;
    std::vector<LazyFunctionBodyLoc> functionLocs;
    std::vector<LazyBytecodeChunk> chunks;
    const std::uint8_t* rawBlobData = nullptr;
    std::size_t rawBlobSize = 0;
    std::vector<std::uint8_t> ownedBlob;
    std::uint32_t v4StringCount = 0;
    std::uint32_t v4StringDataSize = 0;
    std::uint32_t v4StringSpansOffset = 0;
    std::uint32_t v4StringDataOffset = 0;
    std::uint32_t v4FunctionLocCount = 0;
    std::uint32_t v4FunctionLocsOffset = 0;
    bool v4ZeroCopyStrings = false;
    bool v4ZeroCopyFunctionLocs = false;

    [[nodiscard]] const std::uint8_t* blobBase() const noexcept {
        return !ownedBlob.empty() ? ownedBlob.data() : rawBlobData;
    }

    [[nodiscard]] std::size_t blobSize() const noexcept {
        return !ownedBlob.empty() ? ownedBlob.size() : rawBlobSize;
    }

    [[nodiscard]] bool hasZeroCopyStrings() const noexcept {
        return v4ZeroCopyStrings && blobBase() != nullptr;
    }

    [[nodiscard]] bool hasZeroCopyFunctionLocs() const noexcept {
        return v4ZeroCopyFunctionLocs && blobBase() != nullptr;
    }

    [[nodiscard]] const LazyStringSpan* flatStringSpansPtr() const noexcept {
        if (hasZeroCopyStrings()) {
            return reinterpret_cast<const LazyStringSpan*>(blobBase() + v4StringSpansOffset);
        }
        return flatStringSpans.data();
    }

    [[nodiscard]] const char* flatStringDataPtr() const noexcept {
        if (hasZeroCopyStrings()) {
            return reinterpret_cast<const char*>(blobBase() + v4StringDataOffset);
        }
        return flatStringData.data();
    }

    [[nodiscard]] std::size_t flatStringBytes() const noexcept {
        return hasZeroCopyStrings()
            ? static_cast<std::size_t>(v4StringDataSize)
            : flatStringData.size();
    }

    [[nodiscard]] std::size_t functionLocCount() const noexcept {
        return hasZeroCopyFunctionLocs()
            ? static_cast<std::size_t>(v4FunctionLocCount)
            : functionLocs.size();
    }

    [[nodiscard]] const LazyFunctionBodyLoc* functionLocsPtr() const noexcept {
        if (hasZeroCopyFunctionLocs()) {
            return reinterpret_cast<const LazyFunctionBodyLoc*>(blobBase() + v4FunctionLocsOffset);
        }
        return functionLocs.data();
    }

    [[nodiscard]] const LazyFunctionBodyLoc& functionLocAt(std::uint32_t index) const noexcept {
        return functionLocsPtr()[index];
    }

    [[nodiscard]] std::size_t stringCount() const noexcept {
        if (hasZeroCopyStrings()) {
            return static_cast<std::size_t>(v4StringCount);
        }
        return !flatStringSpans.empty() ? flatStringSpans.size() : sharedStrings.size();
    }

    [[nodiscard]] std::string_view stringViewAt(std::uint32_t index) const noexcept {
        if (hasZeroCopyStrings()) {
            if (index >= v4StringCount) return {};
            const auto& sp = flatStringSpansPtr()[index];
            if (sp.length == 0u ||
                sp.offset > v4StringDataSize ||
                sp.length > v4StringDataSize - sp.offset) {
                return {};
            }
            return std::string_view(flatStringDataPtr() + sp.offset, sp.length);
        }
        if (!flatStringSpans.empty()) {
            if (index >= flatStringSpans.size()) return {};
            const auto& sp = flatStringSpans[index];
            if (sp.length == 0u ||
                static_cast<std::size_t>(sp.offset) + sp.length > flatStringData.size()) {
                return {};
            }
            return std::string_view(flatStringData.data() + sp.offset, sp.length);
        }
        if (index >= sharedStrings.size()) return {};
        return sharedStrings[index];
    }

    [[nodiscard]] std::string stringCopyAt(std::uint32_t index) const {
        const auto sv = stringViewAt(index);
        return std::string(sv.data(), sv.size());
    }
};

[[nodiscard]] std::vector<std::uint8_t> encodeLazyBytecodeBundle(
    const std::vector<Module>& modules,
    std::size_t targetChunkBytes = 24576);

[[nodiscard]] bool decodeLazyBytecodeBundleHeader(
    const std::uint8_t* data,
    std::size_t size,
    LazyBytecodeBundle& outBundle,
    bool copyBlob,
    diagnostics::DiagnosticBag& diagnostics,
    bool skipChecksum = false);

[[nodiscard]] bool decompressLazyChunk(
    const LazyBytecodeBundle& bundle,
    std::uint32_t chunkIndex,
    std::vector<std::uint8_t>& outChunk,
    diagnostics::DiagnosticBag& diagnostics);

[[nodiscard]] bool decodeLazyFunctionBody(
    const LazyBytecodeBundle& bundle,
    std::uint32_t locIndex,
    Function& outFunction,
    diagnostics::DiagnosticBag& diagnostics,
    const std::vector<std::uint8_t>* preDecompressedChunk = nullptr,
    const Module* moduleOverride = nullptr);

} // namespace realscript::bytecode
