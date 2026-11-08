#pragma once

#include "realscript/aot_cpp/RuntimeAbi.h"
#include "realscript/runtime/Runtime.h"
#include "realscript/semantic/Semantic.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
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

#if defined(__GNUC__) || defined(__clang__)
#define RS_AOT_LIKELY(x) __builtin_expect(!!(x), 1)
#define RS_AOT_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define RS_AOT_LIKELY(x) (x)
#define RS_AOT_UNLIKELY(x) (x)
#endif

namespace realscript::aot {

constexpr std::uint32_t RuntimeAbiMajor = 1;
constexpr std::uint32_t RuntimeAbiMinor = 2;

struct GeneratedFieldLayout {
    std::size_t byteOffset = 0;
    std::size_t byteSize = 0;
    runtime::NativeFieldKind expectedKind = runtime::NativeFieldKind::Int64;
};

struct TypedRawFieldCache {
    std::size_t byteOffset = 0;
    runtime::NativeFieldKind kind = runtime::NativeFieldKind::Int64;
};

struct TypedRawColumnCache {
    std::uint8_t* basePtr = nullptr;
    std::size_t strideBytes = 0;
    std::int64_t count = -1;
    bool readOnly = false;
    bool exactNativeLayout = false;
    std::uint8_t fieldCount = 0;
    std::array<TypedRawFieldCache, 16> fields{};
};

template <typename FieldT>
[[nodiscard]] inline FieldT readRawColumnField(
    const std::uint8_t* elemBase,
    const TypedRawFieldCache& field) noexcept {
    const std::uint8_t* ptr = elemBase + field.byteOffset;
    if constexpr (std::is_same_v<FieldT, double>) {
        if (RS_AOT_LIKELY(field.kind == runtime::NativeFieldKind::Float64)) {
            double v = 0.0;
            std::memcpy(&v, ptr, sizeof(v));
            return v;
        }
        float f = 0.0f;
        std::memcpy(&f, ptr, sizeof(f));
        return static_cast<double>(f);
    } else if constexpr (std::is_same_v<FieldT, float>) {
        if (RS_AOT_LIKELY(field.kind == runtime::NativeFieldKind::Float32)) {
            float v = 0.0f;
            std::memcpy(&v, ptr, sizeof(v));
            return v;
        }
        double d = 0.0;
        std::memcpy(&d, ptr, sizeof(d));
        return static_cast<float>(d);
    } else if constexpr (std::is_same_v<FieldT, std::int64_t>) {
        if (RS_AOT_LIKELY(field.kind == runtime::NativeFieldKind::Int64)) {
            std::int64_t v = 0;
            std::memcpy(&v, ptr, sizeof(v));
            return v;
        }
        std::int32_t i32 = 0;
        std::memcpy(&i32, ptr, sizeof(i32));
        return static_cast<std::int64_t>(i32);
    } else {
        FieldT v{};
        std::memcpy(&v, ptr, sizeof(FieldT));
        return v;
    }
}

template <typename FieldT>
inline void writeRawColumnField(
    std::uint8_t* elemBase,
    const TypedRawFieldCache& field,
    FieldT value) noexcept {
    std::uint8_t* ptr = elemBase + field.byteOffset;
    if constexpr (std::is_same_v<FieldT, double>) {
        if (RS_AOT_LIKELY(field.kind == runtime::NativeFieldKind::Float64)) {
            std::memcpy(ptr, &value, sizeof(value));
            return;
        }
        const float f = static_cast<float>(value);
        std::memcpy(ptr, &f, sizeof(f));
    } else if constexpr (std::is_same_v<FieldT, float>) {
        if (RS_AOT_LIKELY(field.kind == runtime::NativeFieldKind::Float32)) {
            std::memcpy(ptr, &value, sizeof(value));
            return;
        }
        const double d = static_cast<double>(value);
        std::memcpy(ptr, &d, sizeof(d));
    } else if constexpr (std::is_same_v<FieldT, std::int64_t>) {
        if (RS_AOT_LIKELY(field.kind == runtime::NativeFieldKind::Int64)) {
            std::memcpy(ptr, &value, sizeof(value));
            return;
        }
        const std::int32_t i32 = static_cast<std::int32_t>(value);
        std::memcpy(ptr, &i32, sizeof(i32));
    } else {
        std::memcpy(ptr, &value, sizeof(FieldT));
    }
}

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
    template <bool Enabled>
    class TypedRawAccountingScope {
    public:
        explicit TypedRawAccountingScope(ExecutionContext& context) noexcept {
            if constexpr (Enabled) {
                state_.context = &context;
                const auto budget = context.options_.limits.instructionBudget;
                state_.remaining = context.executed_ < budget
                    ? budget - context.executed_
                    : 0;
            } else {
                (void)context;
            }
        }
        ~TypedRawAccountingScope() {
            if constexpr (Enabled) {
                state_.context->executed_ += state_.consumed;
                state_.context->statistics_.instructionsExecuted =
                    state_.context->executed_;
                state_.context->statistics_.branchesTaken += state_.branches;
            }
        }
        TypedRawAccountingScope(const TypedRawAccountingScope&) = delete;
        TypedRawAccountingScope& operator=(
            const TypedRawAccountingScope&) = delete;

        [[nodiscard]] bool consume(std::uint64_t count) {
            if constexpr (!Enabled) {
                (void)count;
                return true;
            } else {
                if (count == 0) return true;
                if (count > state_.remaining - state_.consumed) {
                    state_.consumed = state_.remaining;
                    return state_.context->fail(
                        runtime::ErrorCode::InstructionBudgetExceeded,
                        "instruction budget exceeded");
                }
                state_.consumed += count;
                return true;
            }
        }
        void branch() noexcept {
            if constexpr (Enabled) ++state_.branches;
        }
        void suspendBeforeCall() noexcept {
            if constexpr (Enabled) {
                state_.context->executed_ += state_.consumed;
                state_.context->statistics_.instructionsExecuted =
                    state_.context->executed_;
                state_.context->statistics_.branchesTaken += state_.branches;
                state_.consumed = 0;
                state_.branches = 0;
            }
        }
        void resumeAfterCall() noexcept {
            if constexpr (Enabled) {
                const auto budget =
                    state_.context->options_.limits.instructionBudget;
                state_.remaining = state_.context->executed_ < budget
                    ? budget - state_.context->executed_
                    : 0;
                state_.consumed = 0;
                state_.branches = 0;
            }
        }

    private:
        struct EnabledState {
            ExecutionContext* context = nullptr;
            std::uint64_t remaining = 0;
            std::uint64_t consumed = 0;
            std::uint64_t branches = 0;
        };
        struct DisabledState {};
        std::conditional_t<Enabled, EnabledState, DisabledState> state_;
    };

    template <bool Enabled>
    class TypedDeterminismAccountingScope {
    public:
        explicit TypedDeterminismAccountingScope(
            ExecutionContext& context) noexcept {
            if constexpr (Enabled) {
                state_.context = &context;
                state_.budget = context.options_.limits.instructionBudget;
                state_.executed = context.executed_;
                state_.digest = context.determinismEventDigest_;
                state_.eventCount = context.determinismEventCount_;
                state_.branches = context.statistics_.branchesTaken;
                state_.functionId = context.currentFunctionId_;
                state_.callDepth = context.stack_.size();
            } else {
                (void)context;
            }
        }
        ~TypedDeterminismAccountingScope() {
            if constexpr (Enabled) flush();
        }
        TypedDeterminismAccountingScope(
            const TypedDeterminismAccountingScope&) = delete;
        TypedDeterminismAccountingScope& operator=(
            const TypedDeterminismAccountingScope&) = delete;

        [[nodiscard]] bool consume(
            runtime::DeterminismOperationId operationId) {
            if constexpr (!Enabled) {
                (void)operationId;
                return true;
            } else {
                if (state_.executed >= state_.budget) {
                    flush();
                    return state_.context->fail(
                        runtime::ErrorCode::InstructionBudgetExceeded,
                        "instruction budget exceeded");
                }
                ++state_.executed;
                state_.digest = runtime::accumulateDeterminismEvent(
                    state_.digest,
                    runtime::TraceEventKind::Instruction,
                    state_.functionId,
                    operationId,
                    state_.executed,
                    state_.callDepth);
                ++state_.eventCount;
                return true;
            }
        }
        void branch(std::uint32_t blockId) noexcept {
            if constexpr (Enabled) {
                ++state_.branches;
                state_.digest = runtime::accumulateDeterminismEvent(
                    state_.digest,
                    runtime::TraceEventKind::Branch,
                    state_.functionId,
                    blockId,
                    state_.executed,
                    state_.callDepth);
                ++state_.eventCount;
            }
        }
        void flush() noexcept {
            if constexpr (Enabled) {
                if (state_.flushed) return;
                state_.context->executed_ = state_.executed;
                state_.context->statistics_.instructionsExecuted =
                    state_.executed;
                state_.context->statistics_.branchesTaken = state_.branches;
                state_.context->determinismEventDigest_ = state_.digest;
                state_.context->determinismEventCount_ = state_.eventCount;
                state_.flushed = true;
            }
        }
        void suspendBeforeCall() noexcept {
            if constexpr (Enabled) {
                flush();
            }
        }
        void resumeAfterCall() noexcept {
            if constexpr (Enabled) {
                state_.executed = state_.context->executed_;
                state_.digest = state_.context->determinismEventDigest_;
                state_.eventCount = state_.context->determinismEventCount_;
                state_.branches = state_.context->statistics_.branchesTaken;
                state_.functionId = state_.context->currentFunctionId_;
                state_.callDepth = state_.context->stack_.size();
                state_.flushed = false;
            }
        }

    private:
        struct EnabledState {
            ExecutionContext* context = nullptr;
            std::uint64_t budget = 0;
            std::uint64_t executed = 0;
            std::uint64_t digest = runtime::DeterminismEventSeed;
            std::uint64_t eventCount = 0;
            std::uint64_t branches = 0;
            semantic::SymbolId functionId = 0;
            std::size_t callDepth = 0;
            bool flushed = false;
        };
        struct DisabledState {};
        std::conditional_t<Enabled, EnabledState, DisabledState> state_;
    };

    ExecutionContext(
        const ProgramDescriptor& program,
        std::shared_ptr<runtime::ManagedHeap> heap,
        std::shared_ptr<const runtime::BindingRegistry> bindings,
        runtime::ExecutionOptions options = {},
        const std::unordered_map<semantic::SymbolId, const TypeDescriptor*>* prebuiltTypes = nullptr,
        const std::unordered_map<semantic::SymbolId, const FunctionDescriptor*>* prebuiltFunctions = nullptr);

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
    [[nodiscard]] bool trustedReleaseEnabled() const noexcept {
        return options_.trustedRelease && fastAccounting_;
    }
    [[nodiscard]] bool typedDeterminismAccountingEnabled() const noexcept {
        return determinismOnly_ && options_.limits.gcWorkBudget == 0;
    }
    template <bool FastAccounting, bool DeterminismAccounting, bool TrustedRelease = false>
    [[nodiscard]] bool enterTypedDirectCall(
        semantic::SymbolId calleeSymbolId,
        const char* calleeName,
        semantic::SymbolId& outPreviousFunctionId) {
        if constexpr (TrustedRelease) {
            (void)calleeSymbolId;
            (void)calleeName;
            (void)outPreviousFunctionId;
            return true;
        } else {
            if (stack_.size() >= options_.limits.recursionLimit) {
                return fail(
                    runtime::ErrorCode::RecursionLimitExceeded,
                    "recursion limit exceeded");
            }
            outPreviousFunctionId = currentFunctionId_;
            currentFunctionId_ = calleeSymbolId;
            stack_.push_back(calleeName ? calleeName : "<aot-function>");
            ++statistics_.functionCalls;
            if (stack_.size() > statistics_.maximumCallDepth) {
                statistics_.maximumCallDepth = stack_.size();
            }
            if constexpr (DeterminismAccounting) {
                determinismEventDigest_ = runtime::accumulateDeterminismEvent(
                    determinismEventDigest_,
                    runtime::TraceEventKind::FunctionEnter,
                    currentFunctionId_,
                    0,
                    executed_,
                    stack_.size());
                ++determinismEventCount_;
            } else if constexpr (!FastAccounting) {
                emitTrace(runtime::TraceEventKind::FunctionEnter);
            }
            return true;
        }
    }

    template <bool FastAccounting, bool DeterminismAccounting, bool TrustedRelease = false, typename BoxFn>
    void exitTypedDirectCall(
        semantic::SymbolId previousFunctionId,
        bool succeeded,
        BoxFn&& boxReturnValue) {
        if constexpr (TrustedRelease) {
            (void)previousFunctionId;
            (void)succeeded;
            (void)boxReturnValue;
            return;
        } else {
            if (succeeded) {
                if constexpr (DeterminismAccounting) {
                    determinismEventDigest_ = runtime::accumulateDeterminismEvent(
                        determinismEventDigest_,
                        runtime::TraceEventKind::FunctionExit,
                        currentFunctionId_,
                        0,
                        executed_,
                        stack_.size());
                    ++determinismEventCount_;
                } else if constexpr (!FastAccounting) {
                    if (traceEventsEnabled_) {
                        if (diagnosticEventsEnabled_) {
                            const runtime::Value boxed = boxReturnValue();
                            const auto operation =
                                runtime::valueToString(boxed, heap_.get());
                            emitTrace(
                                runtime::TraceEventKind::FunctionExit,
                                0,
                                operation);
                        } else {
                            emitTrace(runtime::TraceEventKind::FunctionExit);
                        }
                    }
                }
            }
            stack_.pop_back();
            currentFunctionId_ = previousFunctionId;
        }
    }
    [[nodiscard]] bool consumeRawTyped(std::uint64_t count = 1) {
        if (count == 0) return true;
        const auto budget = options_.limits.instructionBudget;
        if (executed_ >= budget || count > budget - executed_) {
            executed_ = budget;
            statistics_.instructionsExecuted = executed_;
            return fail(
                runtime::ErrorCode::InstructionBudgetExceeded,
                "instruction budget exceeded");
        }
        executed_ += count;
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
    [[nodiscard]] bool arrayLengthFast(
        const runtime::Value& receiver,
        std::int64_t& result);
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
    [[nodiscard]] bool initUnboxedStruct(
        semantic::SymbolId typeId,
        struct UnboxedCustomStruct& result);
    template <typename T, typename = std::enable_if_t<T::kIsGeneratedAotStruct>>
    [[nodiscard]] constexpr bool initUnboxedStruct(
        semantic::SymbolId,
        T& result) const noexcept {
        result = T{};
        return true;
    }
    [[nodiscard]] bool loadUnboxedStructField(
        semantic::SymbolId typeId,
        std::size_t fieldIndex,
        const struct UnboxedCustomStruct& receiver,
        runtime::Value& result);
    [[nodiscard]] bool storeUnboxedStructField(
        semantic::SymbolId typeId,
        std::size_t fieldIndex,
        const struct UnboxedCustomStruct& receiver,
        const runtime::Value& value,
        struct UnboxedCustomStruct& result);
    [[nodiscard]] bool ecsWritebackBoxed(
        const CallSignature& signature,
        std::int64_t index,
        const runtime::Value& boxedStruct);
    [[nodiscard]] bool resolveEcsColumnFast(
        semantic::SymbolId denseColSymbolId,
        const CallSignature& fallbackSignature,
        runtime::Value& outResult);
    [[nodiscard]] struct TypedRawColumnCache acquireRawColumnCache(
        const runtime::Value& arrayValue,
        semantic::SymbolId expectedStructTypeId = 0,
        std::size_t generatedStructSize = 0,
        const struct GeneratedFieldLayout* generatedFields = nullptr,
        std::size_t generatedFieldCount = 0) const noexcept;
    [[nodiscard]] bool hasSparseEcsState() const noexcept {
        return heap_ != nullptr && heap_->hasSparseEcsState();
    }
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
    [[nodiscard]] std::vector<std::string> materializeStackTrace() const;
    std::vector<const char*> stack_;
    semantic::SymbolId currentFunctionId_ = 0;
    runtime::ShadowStack shadowStack_;
    std::unordered_map<semantic::SymbolId, const TypeDescriptor*> ownedTypes_;
    std::unordered_map<semantic::SymbolId, const FunctionDescriptor*> ownedFunctions_;
    const std::unordered_map<semantic::SymbolId, const TypeDescriptor*>* typesPtr_ = nullptr;
    const std::unordered_map<semantic::SymbolId, const FunctionDescriptor*>* functionsPtr_ = nullptr;
    std::optional<runtime::Value> pendingException_;
};

class Program {
public:
    explicit Program(const ProgramDescriptor& descriptor);

    void setBindings(std::shared_ptr<const runtime::BindingRegistry> bindings);
    void setHeap(std::shared_ptr<runtime::ManagedHeap> heap);
    [[nodiscard]] std::shared_ptr<runtime::ManagedHeap> heap() const noexcept;
    [[nodiscard]] const ProgramDescriptor& descriptor() const noexcept;
    [[nodiscard]] bool hasFunction(semantic::SymbolId symbolId) const noexcept {
        return functions_.find(symbolId) != functions_.end();
    }

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
    std::unordered_map<semantic::SymbolId, const TypeDescriptor*> types_;
    std::unordered_map<semantic::SymbolId, const FunctionDescriptor*> functions_;
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
    runtime::ValueSpan arguments,
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
        runtime::ValueSpan arguments,
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


inline std::int64_t typedWrapInt32(std::int64_t value) noexcept {
    const auto bits = static_cast<std::uint32_t>(value);
    return bits <= static_cast<std::uint32_t>(
                       std::numeric_limits<std::int32_t>::max())
        ? static_cast<std::int64_t>(bits)
        : static_cast<std::int64_t>(bits) - (std::int64_t{1} << 32);
}

inline std::int64_t typedSignedFromBits(std::uint64_t bits) noexcept {
    return bits <= static_cast<std::uint64_t>(
                       std::numeric_limits<std::int64_t>::max())
        ? static_cast<std::int64_t>(bits)
        : -1 - static_cast<std::int64_t>(
            std::numeric_limits<std::uint64_t>::max() - bits);
}

template <bool Checked>
[[nodiscard]] inline bool typedAddLong(
    std::int64_t left,
    std::int64_t right,
    std::int64_t& out) noexcept {
    if constexpr (Checked) {
        if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
            (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right)) {
            return false;
        }
        out = left + right;
        return true;
    } else {
        out = typedSignedFromBits(
            static_cast<std::uint64_t>(left) +
            static_cast<std::uint64_t>(right));
        return true;
    }
}

template <bool Checked>
[[nodiscard]] inline bool typedSubLong(
    std::int64_t left,
    std::int64_t right,
    std::int64_t& out) noexcept {
    if constexpr (Checked) {
        if ((right < 0 && left > std::numeric_limits<std::int64_t>::max() + right) ||
            (right > 0 && left < std::numeric_limits<std::int64_t>::min() + right)) {
            return false;
        }
        out = left - right;
        return true;
    } else {
        out = typedSignedFromBits(
            static_cast<std::uint64_t>(left) -
            static_cast<std::uint64_t>(right));
        return true;
    }
}

template <bool Checked>
[[nodiscard]] inline bool typedMulLong(
    std::int64_t left,
    std::int64_t right,
    std::int64_t& out) noexcept {
    if constexpr (Checked) {
        if (left == 0 || right == 0) {
            out = 0;
            return true;
        }
        const auto minimum = std::numeric_limits<std::int64_t>::min();
        const auto maximum = std::numeric_limits<std::int64_t>::max();
        bool overflow = false;
        if (left > 0) {
            overflow = right > 0 ? left > maximum / right : right < minimum / left;
        } else {
            overflow = right > 0 ? left < minimum / right
                                 : (left != 0 && right < maximum / left);
        }
        if (overflow) return false;
        out = left * right;
        return true;
    } else {
        out = typedSignedFromBits(
            static_cast<std::uint64_t>(left) *
            static_cast<std::uint64_t>(right));
        return true;
    }
}

[[nodiscard]] inline runtime::Value typedBoxScalar(
    semantic::PrimitiveType type,
    bool value) noexcept {
    (void)type;
    return runtime::Value{value};
}

[[nodiscard]] inline runtime::Value typedBoxScalar(
    semantic::PrimitiveType type,
    std::int64_t value) noexcept {
    if (type == semantic::PrimitiveType::Long) {
        return runtime::Value{runtime::LongValue{value}};
    }
    return runtime::Value{value};
}

[[nodiscard]] inline runtime::Value typedBoxScalar(
    semantic::PrimitiveType type,
    float value) noexcept {
    (void)type;
    return runtime::Value{runtime::FloatValue{value}};
}

[[nodiscard]] inline runtime::Value typedBoxScalar(
    semantic::PrimitiveType type,
    double value) noexcept {
    (void)type;
    return runtime::Value{value};
}

[[nodiscard]] inline runtime::Value typedBoxScalar(
    semantic::PrimitiveType type,
    runtime::FixedValue value) noexcept {
    (void)type;
    return runtime::Value{value};
}

[[nodiscard]] inline runtime::Value typedBoxScalar(
    semantic::PrimitiveType type,
    runtime::Fixed16Value value) noexcept {
    (void)type;
    return runtime::Value{value};
}

[[nodiscard]] inline runtime::Value typedBoxScalar(
    semantic::PrimitiveType type,
    runtime::Vec2Value value) noexcept {
    (void)type;
    return runtime::Value{value};
}

[[nodiscard]] inline runtime::Value typedBoxScalar(
    semantic::PrimitiveType type,
    runtime::Vec3Value value) noexcept {
    (void)type;
    return runtime::Value{value};
}

[[nodiscard]] inline runtime::Value typedBoxScalar(
    semantic::PrimitiveType type,
    runtime::QuatValue value) noexcept {
    (void)type;
    return runtime::Value{value};
}

[[nodiscard]] inline runtime::Value typedBoxScalar(
    semantic::PrimitiveType type,
    runtime::EnumValue value) noexcept {
    (void)type;
    return runtime::Value{value};
}

[[nodiscard]] inline runtime::Value typedBoxScalar(
    semantic::PrimitiveType type,
    const runtime::Value& value) {
    (void)type;
    return value;
}

struct EmptyGcGuards {};

struct UnboxedCustomStruct {
    semantic::SymbolId typeId = 0;
    std::array<runtime::Value, 16> fields{};
    std::uint8_t fieldCount = 0;

    UnboxedCustomStruct() = default;
    explicit UnboxedCustomStruct(semantic::SymbolId id) noexcept : typeId(id) {}

    UnboxedCustomStruct(const UnboxedCustomStruct& other)
        : typeId(other.typeId), fieldCount(other.fieldCount) {
        for (std::uint8_t i = 0; i < fieldCount; ++i) {
            fields[i] = other.fields[i];
        }
    }

    UnboxedCustomStruct& operator=(const UnboxedCustomStruct& other) {
        if (this != &other) {
            typeId = other.typeId;
            fieldCount = other.fieldCount;
            for (std::uint8_t i = 0; i < fieldCount; ++i) {
                fields[i] = other.fields[i];
            }
        }
        return *this;
    }

    UnboxedCustomStruct(UnboxedCustomStruct&& other) noexcept
        : typeId(other.typeId), fieldCount(other.fieldCount) {
        for (std::uint8_t i = 0; i < fieldCount; ++i) {
            fields[i] = std::move(other.fields[i]);
        }
    }

    UnboxedCustomStruct& operator=(UnboxedCustomStruct&& other) noexcept {
        if (this != &other) {
            typeId = other.typeId;
            fieldCount = other.fieldCount;
            for (std::uint8_t i = 0; i < fieldCount; ++i) {
                fields[i] = std::move(other.fields[i]);
            }
        }
        return *this;
    }

    friend bool operator==(const UnboxedCustomStruct& left, const UnboxedCustomStruct& right) {
        if (left.typeId != right.typeId || left.fieldCount != right.fieldCount) return false;
        for (std::uint8_t i = 0; i < left.fieldCount; ++i) {
            if (!(left.fields[i] == right.fields[i])) return false;
        }
        return true;
    }
    friend bool operator!=(const UnboxedCustomStruct& left, const UnboxedCustomStruct& right) {
        return !(left == right);
    }
};

[[nodiscard]] inline runtime::Value typedBoxScalar(
    semantic::PrimitiveType type,
    const UnboxedCustomStruct& value) {
    (void)type;
    return runtime::Value{runtime::makeStructValueFromFields(
        value.typeId,
        runtime::ValueSpan(value.fields.data(), value.fieldCount))};
}

template <typename T, typename = std::enable_if_t<T::kIsGeneratedAotStruct>>
[[nodiscard]] inline runtime::Value typedBoxScalar(
    semantic::PrimitiveType,
    const T& value) {
    return value.boxToValue();
}

[[nodiscard]] inline bool typedUnboxScalar(
    semantic::PrimitiveType type,
    const runtime::Value& value,
    bool& out) noexcept {
    (void)type;
    if (const auto* p = std::get_if<bool>(&value)) {
        out = *p;
        return true;
    }
    return false;
}

[[nodiscard]] inline bool typedUnboxScalar(
    semantic::PrimitiveType type,
    const runtime::Value& value,
    std::int64_t& out) noexcept {
    if (type == semantic::PrimitiveType::Long) {
        if (const auto* p = std::get_if<runtime::LongValue>(&value)) {
            out = p->value;
            return true;
        }
        return false;
    }
    if (const auto* p = std::get_if<std::int64_t>(&value)) {
        out = *p;
        return true;
    }
    return false;
}

[[nodiscard]] inline bool typedUnboxScalar(
    semantic::PrimitiveType type,
    const runtime::Value& value,
    float& out) noexcept {
    (void)type;
    if (const auto* p = std::get_if<runtime::FloatValue>(&value)) {
        out = p->value;
        return true;
    }
    return false;
}

[[nodiscard]] inline bool typedUnboxScalar(
    semantic::PrimitiveType type,
    const runtime::Value& value,
    double& out) noexcept {
    (void)type;
    if (const auto* p = std::get_if<double>(&value)) {
        out = *p;
        return true;
    }
    return false;
}

[[nodiscard]] inline bool typedUnboxScalar(
    semantic::PrimitiveType type,
    const runtime::Value& value,
    runtime::FixedValue& out) noexcept {
    (void)type;
    if (const auto* p = std::get_if<runtime::FixedValue>(&value)) {
        out = *p;
        return true;
    }
    if (const auto f0 = runtime::loadInlineStructField(value, 0)) {
        if (const auto* i = std::get_if<std::int64_t>(&*f0)) {
            out = runtime::FixedValue{static_cast<std::int32_t>(*i)};
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline bool typedUnboxScalar(
    semantic::PrimitiveType type,
    const runtime::Value& value,
    runtime::Fixed16Value& out) noexcept {
    (void)type;
    if (const auto* p = std::get_if<runtime::Fixed16Value>(&value)) {
        out = *p;
        return true;
    }
    return false;
}

[[nodiscard]] inline bool typedUnboxScalar(
    semantic::PrimitiveType type,
    const runtime::Value& value,
    runtime::Vec2Value& out) noexcept {
    (void)type;
    if (const auto* p = std::get_if<runtime::Vec2Value>(&value)) {
        out = *p;
        return true;
    }
    return false;
}

[[nodiscard]] inline bool typedUnboxScalar(
    semantic::PrimitiveType type,
    const runtime::Value& value,
    runtime::Vec3Value& out) noexcept {
    (void)type;
    if (const auto* p = std::get_if<runtime::Vec3Value>(&value)) {
        out = *p;
        return true;
    }
    return false;
}

[[nodiscard]] inline bool typedUnboxScalar(
    semantic::PrimitiveType type,
    const runtime::Value& value,
    runtime::QuatValue& out) noexcept {
    (void)type;
    if (const auto* p = std::get_if<runtime::QuatValue>(&value)) {
        out = *p;
        return true;
    }
    return false;
}

[[nodiscard]] inline bool typedUnboxScalar(
    semantic::PrimitiveType type,
    const runtime::Value& value,
    runtime::EnumValue& out) noexcept {
    (void)type;
    if (const auto* p = std::get_if<runtime::EnumValue>(&value)) {
        out = *p;
        return true;
    }
    return false;
}

[[nodiscard]] inline bool typedUnboxScalar(
    semantic::PrimitiveType type,
    const runtime::Value& value,
    runtime::Value& out) {
    if (type == semantic::PrimitiveType::Object) {
        if (std::holds_alternative<runtime::NullObject>(value)) {
            out = value;
            return true;
        }
        if (const auto* ref = std::get_if<runtime::ObjectRef>(&value);
            ref && ref->kind == runtime::ObjectKind::Record) {
            out = value;
            return true;
        }
        return false;
    }
    if (type == semantic::PrimitiveType::Array) {
        if (std::holds_alternative<runtime::NullArray>(value)) {
            out = value;
            return true;
        }
        if (const auto* ref = std::get_if<runtime::ObjectRef>(&value);
            ref && ref->kind == runtime::ObjectKind::Array) {
            out = value;
            return true;
        }
        return false;
    }
    out = value;
    return true;
}

[[nodiscard]] inline bool typedUnboxScalar(
    semantic::PrimitiveType type,
    const runtime::Value& value,
    UnboxedCustomStruct& out) {
    (void)type;
    if (const auto* s = std::get_if<runtime::StructValue>(&value);
        s && s->valid() && s->fieldCount() <= 16) {
        out.typeId = s->typeId;
        out.fieldCount = static_cast<std::uint8_t>(s->fieldCount());
        for (std::uint8_t i = 0; i < out.fieldCount; ++i) {
            if (auto f = runtime::structValueFieldAt(*s, i)) {
                out.fields[i] = std::move(*f);
            } else {
                return false;
            }
        }
        return true;
    }
    return false;
}

template <typename T, typename = std::enable_if_t<T::kIsGeneratedAotStruct>>
[[nodiscard]] inline bool typedUnboxScalar(
    semantic::PrimitiveType,
    const runtime::Value& value,
    T& out) {
    return T::unboxFromValue(value, out);
}

template <semantic::PrimitiveType SrcType, semantic::PrimitiveType DstType, bool Checked, typename SrcT, typename DstT>
[[nodiscard]] inline bool typedConvertNumeric(SrcT src, DstT& dst) noexcept {
    if constexpr (SrcType == DstType) {
        dst = static_cast<DstT>(src);
        return true;
    } else if constexpr (SrcType == semantic::PrimitiveType::Int &&
                         DstType == semantic::PrimitiveType::Long) {
        dst = static_cast<std::int64_t>(src);
        return true;
    } else if constexpr (SrcType == semantic::PrimitiveType::Long &&
                         DstType == semantic::PrimitiveType::Int) {
        if constexpr (Checked) {
            if (src < std::numeric_limits<std::int32_t>::min() ||
                src > std::numeric_limits<std::int32_t>::max()) {
                return false;
            }
            dst = static_cast<std::int64_t>(src);
            return true;
        } else {
            dst = typedWrapInt32(src);
            return true;
        }
    } else if constexpr ((SrcType == semantic::PrimitiveType::Int ||
                          SrcType == semantic::PrimitiveType::Long ||
                          SrcType == semantic::PrimitiveType::Float) &&
                         DstType == semantic::PrimitiveType::Double) {
        dst = static_cast<double>(src);
        return true;
    } else if constexpr (SrcType == semantic::PrimitiveType::Int &&
                         DstType == semantic::PrimitiveType::Float) {
        dst = static_cast<float>(src);
        return true;
    } else {
        runtime::Value converted;
        if (!runtime::tryConvertNumeric(
                typedBoxScalar(SrcType, src),
                DstType,
                converted,
                Checked)) {
            return false;
        }
        return typedUnboxScalar(DstType, converted, dst);
    }
}

} // namespace realscript::aot

