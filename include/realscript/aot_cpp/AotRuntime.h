#pragma once

#include "realscript/aot_cpp/RuntimeAbi.h"
#include "realscript/runtime/Runtime.h"
#include "realscript/semantic/Semantic.h"

#include <cstddef>
#include <cstdint>
#include <climits>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <unordered_map>
#include <vector>

namespace realscript::aot {

constexpr std::uint32_t RuntimeAbiMajor = 1;
constexpr std::uint32_t RuntimeAbiMinor = 2;
constexpr std::uint32_t GeneratedModuleVersion = 1;

struct FieldDescriptor {
    const char* name = nullptr;
    semantic::PrimitiveType type = semantic::PrimitiveType::Error;
    semantic::SymbolId typeId = 0;
    std::uint32_t index = 0;
    bool synthetic = false;
};

struct EnumMemberDescriptor {
    const char* name = nullptr;
    std::int64_t value = 0;
};

struct InterfaceDispatchDescriptor {
    semantic::SymbolId interfaceTypeId = 0;
    const semantic::SymbolId* slots = nullptr;
    std::uint32_t slotCount = 0;
};

struct TypeDescriptor {
    semantic::SymbolId id = 0;
    semantic::TypeKind kind = semantic::TypeKind::Class;
    semantic::SymbolId baseTypeId = 0;
    const char* moduleName = nullptr;
    const char* name = nullptr;
    const FieldDescriptor* fields = nullptr;
    std::uint32_t fieldCount = 0;
    const EnumMemberDescriptor* enumMembers = nullptr;
    std::uint32_t enumMemberCount = 0;
    const semantic::SymbolId* virtualDispatchTable = nullptr;
    std::uint32_t virtualSlotCount = 0;
    bool interfaceType = false;
    const InterfaceDispatchDescriptor* interfaceDispatchMaps = nullptr;
    std::uint32_t interfaceDispatchMapCount = 0;
    bool delegateType = false;
};

class ExecutionContext;

using FunctionImplementation = bool (*)(
    ExecutionContext&,
    const runtime::Value*,
    std::size_t,
    runtime::Value&);

struct FunctionDescriptor {
    semantic::SymbolId symbolId = 0;
    const char* name = nullptr;
    semantic::PrimitiveType returnType = semantic::PrimitiveType::Error;
    semantic::SymbolId returnTypeId = 0;
    const semantic::PrimitiveType* parameterTypes = nullptr;
    const semantic::SymbolId* parameterTypeIds = nullptr;
    std::uint32_t parameterCount = 0;
    FunctionImplementation implementation = nullptr;
    const debug::FunctionDebugInfo* debugInfo = nullptr;
};

struct SourceMapEntry {
    semantic::SymbolId symbolId = 0;
    std::uint32_t generatedLine = 0;
    const char* sourcePath = nullptr;
    std::uint32_t sourceLine = 0;
    std::uint32_t sourceColumn = 0;
};

struct ProgramDescriptor {
    std::uint32_t abiMajor = RuntimeAbiMajor;
    std::uint32_t abiMinor = RuntimeAbiMinor;
    const char* name = nullptr;
    std::uint64_t contentHash = 0;
    const char* const* moduleNames = nullptr;
    std::uint32_t moduleCount = 0;
    const TypeDescriptor* types = nullptr;
    std::uint32_t typeCount = 0;
    const FunctionDescriptor* functions = nullptr;
    std::uint32_t functionCount = 0;
    const SourceMapEntry* sourceMap = nullptr;
    std::uint32_t sourceMapCount = 0;
};

struct CallSignature {
    semantic::SymbolId symbolId = 0;
    const char* name = nullptr;
    semantic::PrimitiveType returnType = semantic::PrimitiveType::Error;
    semantic::SymbolId returnTypeId = 0;
    const semantic::PrimitiveType* parameterTypes = nullptr;
    const semantic::SymbolId* parameterTypeIds = nullptr;
    std::uint32_t parameterCount = 0;
    bool virtualDispatch = false;
    std::uint32_t virtualSlot = std::numeric_limits<std::uint32_t>::max();
    bool interfaceDispatch = false;
    semantic::SymbolId interfaceTypeId = 0;
    std::uint32_t interfaceSlot = std::numeric_limits<std::uint32_t>::max();
};

enum class UnaryOperation : std::uint8_t {
    NegateInt,
    NegateLong,
    NegateDouble,
    LogicalNot,
};

enum class BinaryOperation : std::uint8_t {
    Equal,
    NotEqual,
    AddInt,
    SubtractInt,
    MultiplyInt,
    DivideInt,
    RemainderInt,
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

class FrameScope {
public:
    FrameScope(
        ExecutionContext& context,
        const std::vector<runtime::Value>& arguments,
        const std::vector<runtime::Value>& locals,
        const std::vector<runtime::Value>& registers);
    ~FrameScope();
    FrameScope(const FrameScope&) = delete;
    FrameScope& operator=(const FrameScope&) = delete;

private:
    ExecutionContext* context_ = nullptr;
};

class ExecutionContext {
public:
    ExecutionContext(
        const ProgramDescriptor& program,
        std::shared_ptr<runtime::ManagedHeap> heap,
        std::shared_ptr<const runtime::BindingRegistry> bindings,
        runtime::ExecutionOptions options = {});

    [[nodiscard]] bool invoke(
        semantic::SymbolId symbolId,
        const runtime::Value* arguments,
        std::size_t argumentCount,
        runtime::Value& result);
    [[nodiscard]] bool call(
        const CallSignature& signature,
        const runtime::Value* arguments,
        std::size_t argumentCount,
        runtime::Value& result);
    [[nodiscard]] bool newDelegate(
        semantic::SymbolId delegateTypeId,
        const CallSignature& signature,
        const runtime::Value* target,
        std::size_t targetCount,
        runtime::Value& result);
    [[nodiscard]] bool invokeDelegate(
        semantic::SymbolId delegateTypeId,
        const runtime::Value* arguments,
        std::size_t argumentCount,
        runtime::Value& result);
    [[nodiscard]] bool combineDelegates(
        semantic::SymbolId delegateTypeId,
        const runtime::Value& left,
        const runtime::Value& right,
        bool remove,
        runtime::Value& result);

    [[nodiscard]] bool consume(std::string_view operation);
    [[nodiscard]] bool consume(
        runtime::DeterminismOperationId operationId,
        std::string_view operation);
    [[nodiscard]] bool fastAccountingEnabled() const noexcept {
        return fastAccounting_;
    }
    [[nodiscard]] bool consumeRawTyped() {
        if (executed_ >= options_.limits.instructionBudget) {
            return fail(
                runtime::ErrorCode::InstructionBudgetExceeded,
                "instruction budget exceeded");
        }
        ++executed_;
        statistics_.instructionsExecuted = executed_;
        return true;
    }
    [[nodiscard]] bool branch(std::uint32_t blockId);
    void branchRawTyped() noexcept {
        ++statistics_.branchesTaken;
    }
    [[nodiscard]] bool throwScript(const runtime::Value& value);
    [[nodiscard]] bool hasPendingException() const noexcept;
    [[nodiscard]] bool pendingExceptionMatches(
        semantic::SymbolId typeId) const;
    [[nodiscard]] bool takePendingException(runtime::Value& value);
    void reportUnhandledScriptException();
    [[nodiscard]] bool fail(runtime::ErrorCode code, std::string message);
    [[nodiscard]] bool expectType(
        const runtime::Value& value,
        semantic::PrimitiveType type,
        std::string_view context);
    [[nodiscard]] bool expectSignatureType(
        const runtime::Value& value,
        semantic::PrimitiveType type,
        semantic::SymbolId typeId,
        std::string_view context);

    [[nodiscard]] runtime::Value defaultValue(
        semantic::PrimitiveType type,
        semantic::SymbolId typeId,
        std::size_t depth = 0) const;

    [[nodiscard]] bool constantString(
        std::string_view value,
        runtime::Value& result);
    [[nodiscard]] bool newObject(
        semantic::SymbolId typeId,
        runtime::Value& result);
    [[nodiscard]] bool newStruct(
        semantic::SymbolId typeId,
        runtime::Value& result);
    [[nodiscard]] bool newArray(
        semantic::SymbolId arrayTypeId,
        semantic::PrimitiveType elementType,
        semantic::SymbolId elementTypeId,
        const runtime::Value& length,
        runtime::Value& result);
    [[nodiscard]] bool checkNotNull(
        semantic::SymbolId typeId,
        const runtime::Value& receiver,
        runtime::Value& result);
    [[nodiscard]] bool typeOperation(
        semantic::PrimitiveType sourceType,
        semantic::PrimitiveType targetType,
        semantic::SymbolId targetTypeId,
        const runtime::Value& value,
        bool safeCast,
        runtime::Value& result);
    [[nodiscard]] bool arrayLength(
        const runtime::Value& receiver,
        runtime::Value& result);
    [[nodiscard]] bool loadElement(
        semantic::PrimitiveType elementType,
        semantic::SymbolId elementTypeId,
        const runtime::Value& receiver,
        const runtime::Value& index,
        runtime::Value& result);
    [[nodiscard]] bool storeElement(
        semantic::PrimitiveType elementType,
        semantic::SymbolId elementTypeId,
        const runtime::Value& receiver,
        const runtime::Value& index,
        const runtime::Value& value);
    [[nodiscard]] bool loadField(
        semantic::SymbolId typeId,
        std::size_t fieldIndex,
        const runtime::Value& receiver,
        runtime::Value& result);
    [[nodiscard]] bool storeField(
        semantic::SymbolId typeId,
        std::size_t fieldIndex,
        const runtime::Value& receiver,
        const runtime::Value& value);
    [[nodiscard]] bool loadStructField(
        semantic::SymbolId typeId,
        std::size_t fieldIndex,
        const runtime::Value& receiver,
        runtime::Value& result);
    [[nodiscard]] bool storeStructField(
        semantic::SymbolId typeId,
        std::size_t fieldIndex,
        const runtime::Value& receiver,
        const runtime::Value& value,
        runtime::Value& result);
    [[nodiscard]] bool convert(
        semantic::ConversionKind conversion,
        semantic::PrimitiveType targetType,
        bool checkedArithmetic,
        const runtime::Value& value,
        runtime::Value& result);
    [[nodiscard]] bool unary(
        UnaryOperation operation,
        bool checkedArithmetic,
        const runtime::Value& value,
        runtime::Value& result);
    [[nodiscard]] bool binary(
        BinaryOperation operation,
        bool checkedArithmetic,
        const runtime::Value& left,
        const runtime::Value& right,
        runtime::Value& result);

    [[nodiscard]] const ProgramDescriptor& program() const noexcept;
    [[nodiscard]] std::shared_ptr<runtime::ManagedHeap> heap() const noexcept;
    [[nodiscard]] runtime::ShadowStack& shadowStack() noexcept;
    [[nodiscard]] const runtime::RuntimeError& error() const noexcept;
    [[nodiscard]] const runtime::RuntimeStatistics& statistics() const noexcept;
    [[nodiscard]] std::uint64_t instructionsExecuted() const noexcept;
    [[nodiscard]] bool finalizeDeterminism(
        bool succeeded,
        const runtime::Value& value);
    [[nodiscard]] std::uint64_t determinismDigest() const noexcept;
    [[nodiscard]] std::size_t replayEntriesConsumed() const noexcept;

private:
    friend class FrameScope;
    [[nodiscard]] const TypeDescriptor* findType(semantic::SymbolId typeId) const;
    [[nodiscard]] const FunctionDescriptor* findFunction(
        semantic::SymbolId symbolId) const;
    [[nodiscard]] bool isAssignable(
        semantic::SymbolId actual,
        semantic::SymbolId expected) const noexcept;
    void emitTrace(
        runtime::TraceEventKind kind,
        runtime::DeterminismOperationId operationId = 0,
        std::string_view operation = {});

    const ProgramDescriptor* program_ = nullptr;
    std::shared_ptr<runtime::ManagedHeap> heap_;
    std::shared_ptr<const runtime::BindingRegistry> bindings_;
    runtime::ExecutionOptions options_;
    bool traceEventsEnabled_ = false;
    bool determinismEventsEnabled_ = false;
    bool determinismOnly_ = false;
    bool diagnosticEventsEnabled_ = false;
    bool fastAccounting_ = false;
    std::uint64_t determinismEventDigest_ = runtime::DeterminismEventSeed;
    std::uint64_t determinismEventCount_ = 0;
    std::unordered_map<semantic::SymbolId, runtime::FunctionProfile>
        profileFunctions_;
    std::uint64_t profileEvents_ = 0;
    runtime::RuntimeError error_;
    runtime::RuntimeStatistics statistics_;
    runtime::DeterminismSession determinism_;
    std::uint64_t executed_ = 0;
    std::vector<std::string> stack_;
    semantic::SymbolId currentFunctionId_ = 0;
    runtime::ShadowStack shadowStack_;
    std::unordered_map<semantic::SymbolId, const TypeDescriptor*> types_;
    std::unordered_map<semantic::SymbolId, const FunctionDescriptor*> functions_;
    std::optional<runtime::Value> pendingException_;
};

class Program {
public:
    explicit Program(const ProgramDescriptor& descriptor);

    void setBindings(std::shared_ptr<const runtime::BindingRegistry> bindings);
    void setHeap(std::shared_ptr<runtime::ManagedHeap> heap);
    [[nodiscard]] std::shared_ptr<runtime::ManagedHeap> heap() const noexcept;
    [[nodiscard]] const ProgramDescriptor& descriptor() const noexcept;

    [[nodiscard]] runtime::ExecutionResult invoke(
        semantic::SymbolId symbolId,
        const std::vector<runtime::Value>& arguments = {},
        runtime::ExecutionOptions options = {}) const;
    [[nodiscard]] runtime::ExecutionResult invoke(
        const std::string& qualifiedName,
        const std::vector<runtime::Value>& arguments = {},
        runtime::ExecutionOptions options = {}) const;

private:
    const ProgramDescriptor* descriptor_ = nullptr;
    std::shared_ptr<const runtime::BindingRegistry> bindings_;
    std::shared_ptr<runtime::ManagedHeap> heap_;
    std::unordered_map<std::string, semantic::SymbolId> names_;
};

// Typed host binding adapters. They keep generated/native call sites on stable
// SymbolIds while avoiding a Variant[]-style reflection layer in user code.
template <typename T>
struct NativeValue;

template <>
struct NativeValue<bool> {
    static bool read(const runtime::Value& value, bool& output) {
        const auto* typed = std::get_if<bool>(&value);
        if (!typed) return false;
        output = *typed;
        return true;
    }
    static runtime::Value write(bool value) { return value; }
};

template <>
struct NativeValue<std::int32_t> {
    static bool read(const runtime::Value& value, std::int32_t& output) {
        const auto* typed = std::get_if<std::int64_t>(&value);
        if (!typed || *typed < INT32_MIN || *typed > INT32_MAX) return false;
        output = static_cast<std::int32_t>(*typed);
        return true;
    }
    static runtime::Value write(std::int32_t value) {
        return static_cast<std::int64_t>(value);
    }
};

template <>
struct NativeValue<std::int64_t> {
    static bool read(const runtime::Value& value, std::int64_t& output) {
        const auto* typed = std::get_if<runtime::LongValue>(&value);
        if (!typed) return false;
        output = typed->value;
        return true;
    }
    static runtime::Value write(std::int64_t value) {
        return runtime::LongValue{value};
    }
};

template <>
struct NativeValue<double> {
    static bool read(const runtime::Value& value, double& output) {
        const auto* typed = std::get_if<double>(&value);
        if (!typed) return false;
        output = *typed;
        return true;
    }
    static runtime::Value write(double value) { return value; }
};


template <>
struct NativeValue<std::string> {
    static bool read(const runtime::Value& value, std::string& output) {
        const auto* typed = std::get_if<std::string>(&value);
        if (!typed) return false;
        output = *typed;
        return true;
    }
    static runtime::Value write(std::string value) { return value; }
};

template <>
struct NativeValue<runtime::EnumValue> {
    static bool read(const runtime::Value& value, runtime::EnumValue& output) {
        const auto* typed = std::get_if<runtime::EnumValue>(&value);
        if (!typed) return false;
        output = *typed;
        return true;
    }
    static runtime::Value write(runtime::EnumValue value) { return value; }
};

template <>
struct NativeValue<runtime::ObjectRef> {
    static bool read(const runtime::Value& value, runtime::ObjectRef& output) {
        const auto* typed = std::get_if<runtime::ObjectRef>(&value);
        if (!typed) return false;
        output = *typed;
        return true;
    }
    static runtime::Value write(runtime::ObjectRef value) { return value; }
};

template <>
struct NativeValue<runtime::NativeHandle> {
    static bool read(const runtime::Value& value, runtime::NativeHandle& output) {
        const auto* typed = std::get_if<runtime::NativeHandle>(&value);
        if (!typed) return false;
        output = *typed;
        return true;
    }
    static runtime::Value write(runtime::NativeHandle value) { return value; }
};

template <>
struct NativeValue<runtime::StructValue> {
    static bool read(const runtime::Value& value, runtime::StructValue& output) {
        const auto* typed = std::get_if<runtime::StructValue>(&value);
        if (!typed) return false;
        output = *typed;
        return true;
    }
    static runtime::Value write(runtime::StructValue value) {
        return value;
    }
};

template <typename Result, typename... Args, std::size_t... I>
std::optional<runtime::Value> invokeNativeTypedImpl(
    Result (*function)(Args...),
    const std::vector<runtime::Value>& arguments,
    runtime::RuntimeError& error,
    std::index_sequence<I...>) {
    if (arguments.size() != sizeof...(Args)) {
        error.code = runtime::ErrorCode::InvalidArguments;
        error.message = "native thunk argument count mismatch";
        return std::nullopt;
    }
    std::tuple<std::decay_t<Args>...> converted;
    const bool valid = (NativeValue<std::decay_t<Args>>::read(
        arguments[I], std::get<I>(converted)) && ...);
    if (!valid) {
        error.code = runtime::ErrorCode::TypeMismatch;
        error.message = "native thunk argument type mismatch";
        return std::nullopt;
    }
    if constexpr (std::is_void_v<Result>) {
        std::apply(function, converted);
        return runtime::Value{};
    } else {
        return NativeValue<std::decay_t<Result>>::write(
            std::apply(function, converted));
    }
}

template <typename Result, typename... Args>
runtime::ExternalFunction makeNativeThunk(Result (*function)(Args...)) {
    return [function](
        const bytecode::FunctionReference&,
        const std::vector<runtime::Value>& arguments,
        runtime::RuntimeError& error) -> std::optional<runtime::Value> {
        try {
            return invokeNativeTypedImpl(
                function,
                arguments,
                error,
                std::index_sequence_for<Args...>{});
        } catch (const std::exception& exception) {
            error.code = runtime::ErrorCode::ExternalFunctionUnresolved;
            error.message = std::string("native thunk threw: ") + exception.what();
            return std::nullopt;
        } catch (...) {
            error.code = runtime::ErrorCode::ExternalFunctionUnresolved;
            error.message = "native thunk threw an unknown exception";
            return std::nullopt;
        }
    };
}

} // namespace realscript::aot
