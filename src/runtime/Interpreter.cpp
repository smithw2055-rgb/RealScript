#include "realscript/runtime/Runtime.h"

#include "realscript/debug/Debugger.h"
#include "realscript/diagnostics/Diagnostic.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace realscript::runtime {
namespace {

struct FunctionLocation {
    const bytecode::Module* module = nullptr;
    const bytecode::Function* function = nullptr;
};

struct State {
    Limits limits;
    std::uint64_t executed = 0;
    RuntimeStatistics statistics;
    RuntimeError error;
    std::vector<std::string> stack;
    const std::unordered_map<semantic::SymbolId, FunctionLocation>* functions = nullptr;
    const std::unordered_map<semantic::SymbolId, const semantic::TypeSymbol*>* types = nullptr;
    const ExternalFunction* externalResolver = nullptr;
    const BindingRegistry* bindings = nullptr;
    const TraceSink* trace = nullptr;
    ProfileCollector* profile = nullptr;
    DeterminismSession determinism;
    ManagedHeap* heap = nullptr;
    debug::DebugController* debugger = nullptr;
    std::vector<debug::DebugFrameView> debugFrames;
    ShadowStack shadowStack;
    std::optional<Value> pendingException;
};

class ShadowFrameScope {
public:
    ShadowFrameScope(
        ShadowStack& shadowStack,
        const std::vector<Value>* arguments,
        const std::vector<Value>* locals,
        const std::vector<Value>* registers)
        : shadowStack_(shadowStack) {
        shadowStack_.pushFrame(arguments, locals, registers);
    }
    ~ShadowFrameScope() { shadowStack_.popFrame(); }
    ShadowFrameScope(const ShadowFrameScope&) = delete;
    ShadowFrameScope& operator=(const ShadowFrameScope&) = delete;

private:
    ShadowStack& shadowStack_;
};

class DebugFrameScope {
public:
    DebugFrameScope(State& state, debug::DebugFrameView view)
        : state_(state) {
        state_.debugFrames.push_back(std::move(view));
    }
    ~DebugFrameScope() { state_.debugFrames.pop_back(); }
    DebugFrameScope(const DebugFrameScope&) = delete;
    DebugFrameScope& operator=(const DebugFrameScope&) = delete;

private:
    State& state_;
};

void emitTrace(State& state, TraceEventKind kind, std::string operation = {}) {
    TraceEvent event;
    event.kind = kind;
    event.function = state.stack.empty() ? std::string{} : state.stack.back();
    event.operation = std::move(operation);
    event.instructionIndex = state.executed;
    event.callDepth = state.stack.size();
    state.determinism.observe(event);
    if (state.profile) state.profile->record(event);
    if (state.trace && *state.trace) (*state.trace)(event);
}

bool fail(State& state, ErrorCode code, std::string message) {
    state.error.code = code;
    state.error.message = std::move(message);
    state.error.stackTrace = state.stack;
    std::reverse(state.error.stackTrace.begin(), state.error.stackTrace.end());
    emitTrace(state, TraceEventKind::RuntimeError, state.error.message);
    return false;
}

bool consume(State& state, std::string_view operation = {}) {
    if (state.executed >= state.limits.instructionBudget) {
        return fail(state, ErrorCode::InstructionBudgetExceeded,
            "instruction budget exceeded");
    }
    ++state.executed;
    state.statistics.instructionsExecuted = state.executed;
    if (!operation.empty()) {
        emitTrace(state, TraceEventKind::Instruction, std::string(operation));
    }
    if (state.heap && state.limits.gcWorkBudget != 0) {
        const auto work = state.heap->step(
            state.shadowStack,
            state.limits.gcWorkBudget);
        state.statistics.gcWorkPerformed += work;
        if (work != 0) {
            emitTrace(state, TraceEventKind::GcStep, std::to_string(work));
        }
    }
    return true;
}

bool expectType(
    State& state,
    const Value& value,
    semantic::PrimitiveType type,
    const std::string& context) {
    if (const auto* reference = std::get_if<ObjectRef>(&value)) {
        if (!state.heap || !state.heap->isAlive(*reference)) {
            return fail(state, ErrorCode::InvalidObjectReference,
                context + " contains an invalid managed object reference");
        }
    }
    if (valueType(value) != type) {
        return fail(state, ErrorCode::TypeMismatch,
            context + " expected '" + semantic::primitiveTypeName(type) +
            "', got '" + semantic::primitiveTypeName(valueType(value)) + "'");
    }
    return true;
}

const semantic::TypeSymbol* typeDescriptor(
    const bytecode::Module& module,
    std::uint32_t index) {
    return index < module.types.size() ? &module.types[index] : nullptr;
}

const semantic::TypeSymbol* typeDescriptorById(
    const bytecode::Module& module,
    semantic::SymbolId id) {
    for (const auto& type : module.types) {
        if (type.id == id) return &type;
    }
    return nullptr;
}

semantic::SymbolId exactTypeId(
    semantic::PrimitiveType type,
    const std::string& typeName) {
    return semantic::isExactType(type) && !typeName.empty()
        ? semantic::stableTypeId(typeName)
        : 0;
}

bool runtimeAssignable(
    const State& state,
    semantic::SymbolId actual,
    semantic::SymbolId expected) {
    if (actual == expected) return true;
    if (!state.types) return false;
    std::unordered_set<semantic::SymbolId> visited;
    auto current = actual;
    while (current != 0 && visited.insert(current).second) {
        const auto found = state.types->find(current);
        if (found == state.types->end()) return false;
        for (const auto& implementation :
             found->second->interfaceDispatchMaps) {
            if (implementation.interfaceTypeId == expected) return true;
        }
        if (found->second->baseTypeId == expected) return true;
        current = found->second->baseTypeId;
    }
    return false;
}

const semantic::InterfaceDispatchMap* runtimeInterfaceMap(
    const State& state,
    semantic::SymbolId actual,
    semantic::SymbolId interfaceTypeId) {
    if (!state.types) return nullptr;
    std::unordered_set<semantic::SymbolId> visited;
    auto current = actual;
    while (current != 0 && visited.insert(current).second) {
        const auto found = state.types->find(current);
        if (found == state.types->end()) return nullptr;
        for (const auto& implementation :
             found->second->interfaceDispatchMaps) {
            if (implementation.interfaceTypeId == interfaceTypeId) {
                return &implementation;
            }
        }
        current = found->second->baseTypeId;
    }
    return nullptr;
}

bool expectObject(
    State& state,
    const Value& value,
    const semantic::TypeSymbol& type,
    bool allowNull,
    const std::string& context) {
    if (std::holds_alternative<NullObject>(value)) {
        if (allowNull) return true;
        return fail(state, ErrorCode::NullReference,
            context + " attempted to dereference null");
    }
    const auto* reference = std::get_if<ObjectRef>(&value);
    if (!reference || reference->kind != ObjectKind::Record) {
        return fail(state, ErrorCode::TypeMismatch,
            context + " expected object '" + semantic::canonicalTypeName(type) + "'");
    }
    if (!state.heap || !state.heap->isAlive(*reference)) {
        return fail(state, ErrorCode::InvalidObjectReference,
            context + " contains an invalid managed object reference");
    }
    const auto actual = state.heap->objectTypeId(*reference);
    if (!actual || !runtimeAssignable(state, *actual, type.id)) {
        return fail(state, ErrorCode::TypeMismatch,
            context + " expected object '" + semantic::canonicalTypeName(type) + "'");
    }
    return true;
}

bool expectSignatureType(
    State& state,
    const Value& value,
    semantic::PrimitiveType type,
    semantic::SymbolId typeId,
    const std::string& context) {
    if (!expectType(state, value, type, context)) return false;
    if (type == semantic::PrimitiveType::Object) {
        if (std::holds_alternative<NullObject>(value) || typeId == 0) return true;
        const auto* reference = std::get_if<ObjectRef>(&value);
        const auto actual = reference && state.heap
            ? state.heap->objectTypeId(*reference)
            : std::optional<semantic::SymbolId>{};
        if (!actual || !runtimeAssignable(state, *actual, typeId)) {
            return fail(state, ErrorCode::TypeMismatch,
                context + " has the wrong runtime object type");
        }
    } else if (type == semantic::PrimitiveType::Array) {
        if (std::holds_alternative<NullArray>(value) || typeId == 0) return true;
        const auto* reference = std::get_if<ObjectRef>(&value);
        const auto actual = reference && state.heap
            ? state.heap->arrayTypeId(*reference)
            : std::optional<semantic::SymbolId>{};
        if (!actual || *actual != typeId) {
            return fail(state, ErrorCode::TypeMismatch,
                context + " has the wrong runtime array type");
        }
    } else if (type == semantic::PrimitiveType::Handle && typeId != 0) {
        const auto* handle = std::get_if<NativeHandle>(&value);
        if (!handle || (handle->valid() && handle->typeId != typeId) ||
            (!handle->valid() && handle->typeId != 0 && handle->typeId != typeId)) {
            return fail(state, ErrorCode::TypeMismatch,
                context + " has the wrong native handle type");
        }
    } else if (type == semantic::PrimitiveType::Struct) {
        const auto* structure = std::get_if<StructValue>(&value);
        if (!structure || typeId == 0 || structure->typeId != typeId ||
            !structure->storage) {
            return fail(state, ErrorCode::TypeMismatch,
                context + " has the wrong struct type");
        }
    } else if (type == semantic::PrimitiveType::Enum) {
        const auto* enumeration = std::get_if<EnumValue>(&value);
        if (!enumeration || typeId == 0 || enumeration->typeId != typeId) {
            return fail(state, ErrorCode::TypeMismatch,
                context + " has the wrong enum type");
        }
    }
    return true;
}

semantic::SymbolId typeIdAt(
    const std::vector<semantic::SymbolId>& typeIds,
    std::size_t index) noexcept {
    return index < typeIds.size() ? typeIds[index] : 0;
}

Value defaultValue(
    const bytecode::Module& module,
    semantic::PrimitiveType type,
    semantic::SymbolId typeId,
    std::size_t depth = 0) {
    switch (type) {
    case semantic::PrimitiveType::Bool: return false;
    case semantic::PrimitiveType::Byte: return ByteValue{};
    case semantic::PrimitiveType::SByte: return SByteValue{};
    case semantic::PrimitiveType::Short: return ShortValue{};
    case semantic::PrimitiveType::UShort: return UShortValue{};
    case semantic::PrimitiveType::Int: return std::int64_t{0};
    case semantic::PrimitiveType::UInt: return UIntValue{};
    case semantic::PrimitiveType::Long: return LongValue{};
    case semantic::PrimitiveType::ULong: return ULongValue{};
    case semantic::PrimitiveType::Float: return FloatValue{};
    case semantic::PrimitiveType::Double: return 0.0;
    case semantic::PrimitiveType::Char: return CharValue{};
    case semantic::PrimitiveType::String: return NullString{};
    case semantic::PrimitiveType::Object: return NullObject{};
    case semantic::PrimitiveType::Array: return NullArray{};
    case semantic::PrimitiveType::Handle: {
        NativeHandle handle;
        handle.typeId = typeId;
        return handle;
    }
    case semantic::PrimitiveType::Enum: return EnumValue{typeId, 0};
    case semantic::PrimitiveType::Struct: {
        if (depth > 64) return StructValue{};
        const auto* descriptor = typeDescriptorById(module, typeId);
        if (!descriptor || descriptor->kind != semantic::TypeKind::Struct) {
            return StructValue{};
        }
        auto storage = std::make_shared<StructStorage>();
        storage->fields.reserve(descriptor->fields.size());
        for (const auto& field : descriptor->fields) {
            storage->fields.push_back(defaultValue(
                module, field.type, exactTypeId(field.type, field.typeName), depth + 1));
        }
        return StructValue{typeId, std::move(storage)};
    }
    case semantic::PrimitiveType::Null: return std::monostate{};
    case semantic::PrimitiveType::Void:
    case semantic::PrimitiveType::Error:
        break;
    }
    return std::monostate{};
}


bool valuesEqual(State& state, const Value& left, const Value& right, bool& equal) {
    if (valueType(left) != valueType(right)) {
        equal = false;
        return true;
    }
    if (valueType(left) == semantic::PrimitiveType::Object ||
        valueType(left) == semantic::PrimitiveType::Array) {
        const auto validate = [&](const Value& value) {
            const auto* reference = std::get_if<ObjectRef>(&value);
            return !reference || (state.heap && state.heap->isAlive(*reference));
        };
        if (!validate(left) || !validate(right)) {
            return fail(state, ErrorCode::InvalidObjectReference,
                "reference equality encountered an invalid managed reference");
        }
        equal = left == right;
        return true;
    }
    if (valueType(left) != semantic::PrimitiveType::String) {
        equal = left == right;
        return true;
    }
    if (std::holds_alternative<NullString>(left) ||
        std::holds_alternative<NullString>(right)) {
        equal = std::holds_alternative<NullString>(left) &&
            std::holds_alternative<NullString>(right);
        return true;
    }
    const auto getText = [&](const Value& value) -> std::optional<std::string_view> {
        if (const auto* text = std::get_if<std::string>(&value)) return *text;
        if (const auto* reference = std::get_if<ObjectRef>(&value)) {
            if (!state.heap) return std::nullopt;
            return state.heap->stringView(*reference);
        }
        return std::nullopt;
    };
    const auto leftText = getText(left);
    const auto rightText = getText(right);
    if (!leftText || !rightText) {
        return fail(state, ErrorCode::InvalidObjectReference,
            "string equality encountered an invalid managed reference");
    }
    equal = *leftText == *rightText;
    return true;
}

bool checkedIntResult(State& state, std::int64_t value, Value& output) {
    constexpr auto minimum = static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min());
    constexpr auto maximum = static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max());
    if (value < minimum || value > maximum) {
        return fail(state, ErrorCode::IntegerOverflow, "integer arithmetic overflow");
    }
    output = value;
    return true;
}

std::int64_t wrapInt32(std::int64_t value) noexcept {
    const auto bits = static_cast<std::uint32_t>(value);
    return bits <= static_cast<std::uint32_t>(
                       std::numeric_limits<std::int32_t>::max())
        ? static_cast<std::int64_t>(bits)
        : static_cast<std::int64_t>(bits) - (std::int64_t{1} << 32);
}

std::int64_t signedFromBits(std::uint64_t bits) noexcept {
    return bits <= static_cast<std::uint64_t>(
                       std::numeric_limits<std::int64_t>::max())
        ? static_cast<std::int64_t>(bits)
        : -1 - static_cast<std::int64_t>(
            std::numeric_limits<std::uint64_t>::max() - bits);
}


bool checkedLongAdd(State& state, std::int64_t left, std::int64_t right, Value& output) {
    if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
        (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right)) {
        return fail(state, ErrorCode::IntegerOverflow, "long arithmetic overflow");
    }
    output = LongValue{left + right};
    return true;
}

bool checkedLongSubtract(State& state, std::int64_t left, std::int64_t right, Value& output) {
    if ((right < 0 && left > std::numeric_limits<std::int64_t>::max() + right) ||
        (right > 0 && left < std::numeric_limits<std::int64_t>::min() + right)) {
        return fail(state, ErrorCode::IntegerOverflow, "long arithmetic overflow");
    }
    output = LongValue{left - right};
    return true;
}

bool checkedLongMultiply(State& state, std::int64_t left, std::int64_t right, Value& output) {
    if (left == 0 || right == 0) {
        output = LongValue{};
        return true;
    }
    const auto minimum = std::numeric_limits<std::int64_t>::min();
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    bool overflow = false;
    if (left > 0) {
        overflow = right > 0 ? left > maximum / right : right < minimum / left;
    } else {
        overflow = right > 0 ? left < minimum / right :
            (left != 0 && right < maximum / left);
    }
    if (overflow) {
        return fail(state, ErrorCode::IntegerOverflow, "long arithmetic overflow");
    }
    output = LongValue{left * right};
    return true;
}

const bytecode::BasicBlock* findBlock(
    const bytecode::Function& function,
    bytecode::BlockId id) {
    for (const auto& block : function.blocks) {
        if (block.id == id) return &block;
    }
    return nullptr;
}

const debug::SequencePoint* findSequencePoint(
    const bytecode::Function& function,
    bytecode::BlockId blockId,
    std::uint32_t instructionIndex,
    bool terminator) {
    for (const auto& point : function.debugInfo.sequencePoints) {
        if (point.blockId == blockId &&
            point.instructionIndex == instructionIndex &&
            point.terminator == terminator) {
            return &point;
        }
    }
    return nullptr;
}

bool debugSequencePoint(
    State& state,
    const bytecode::Module& /*module*/,
    const bytecode::Function& function,
    bytecode::BlockId blockId,
    std::uint32_t instructionIndex,
    bool terminator) {
    if (!state.debugger) return true;
    const auto* point = findSequencePoint(
        function, blockId, instructionIndex, terminator);
    if (!point) return true;
    state.debugFrames.back().point = point;
    if (!state.debugger->onSequencePoint(state.debugFrames, state.heap)) {
        return fail(state, ErrorCode::ExecutionTerminated,
            "execution terminated by debugger");
    }
    return true;
}

bool executeFunction(
    State& state,
    const FunctionLocation& location,
    const std::vector<Value>& arguments,
    Value& result);

bool executeCall(
    State& state,
    const bytecode::Module& module,
    const bytecode::Instruction& instruction,
    const std::vector<Value>& registers,
    Value& result) {
    if (instruction.index >= module.functionReferences.size()) {
        return fail(state, ErrorCode::InvalidProgram, "call reference index is invalid");
    }
    const auto& reference = module.functionReferences[instruction.index];
    std::vector<Value> arguments;
    arguments.reserve(instruction.operands.size());
    for (const auto operand : instruction.operands) {
        if (operand >= registers.size()) {
            return fail(state, ErrorCode::InvalidProgram, "call operand register is invalid");
        }
        arguments.push_back(registers[operand]);
    }

    if (arguments.size() != reference.parameterTypes.size()) {
        return fail(state, ErrorCode::InvalidProgram,
            "call argument count does not match its reference");
    }
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (!expectSignatureType(
                state,
                arguments[index],
                reference.parameterTypes[index],
                typeIdAt(reference.parameterTypeIds, index),
                "call argument")) {
            return false;
        }
    }

    auto targetSymbolId = reference.symbolId;
    if (reference.virtualDispatch && reference.interfaceDispatch) {
        return fail(state, ErrorCode::InvalidProgram,
            "call cannot use virtual and interface dispatch together");
    }
    if (reference.virtualDispatch) {
        if (reference.virtualSlot ==
                std::numeric_limits<std::uint32_t>::max() ||
            arguments.empty() ||
            reference.parameterTypes.empty() ||
            reference.parameterTypes.front() !=
                semantic::PrimitiveType::Object) {
            return fail(state, ErrorCode::InvalidProgram,
                "virtual call has an invalid receiver contract");
        }
        if (std::holds_alternative<NullObject>(arguments.front())) {
            return fail(state, ErrorCode::NullReference,
                "virtual call attempted to dereference null");
        }
        const auto* receiver = std::get_if<ObjectRef>(&arguments.front());
        const auto actualTypeId = receiver && state.heap
            ? state.heap->objectTypeId(*receiver)
            : std::optional<semantic::SymbolId>{};
        if (!actualTypeId || !state.types) {
            return fail(state, ErrorCode::InvalidObjectReference,
                "virtual call receiver has no runtime type descriptor");
        }
        const auto actualType = state.types->find(*actualTypeId);
        if (actualType == state.types->end() ||
            reference.virtualSlot >=
                actualType->second->virtualDispatchTable.size()) {
            return fail(state, ErrorCode::InvalidProgram,
                "virtual call slot is outside the runtime dispatch table");
        }
        targetSymbolId =
            actualType->second->virtualDispatchTable[reference.virtualSlot];
        if (targetSymbolId == 0) {
            return fail(state, ErrorCode::InvalidProgram,
                "virtual call reached an abstract runtime slot");
        }
    } else if (reference.interfaceDispatch) {
        if (reference.interfaceTypeId == 0 ||
            reference.interfaceSlot ==
                std::numeric_limits<std::uint32_t>::max() ||
            arguments.empty() ||
            reference.parameterTypes.empty() ||
            reference.parameterTypes.front() !=
                semantic::PrimitiveType::Object ||
            typeIdAt(reference.parameterTypeIds, 0) !=
                reference.interfaceTypeId) {
            return fail(state, ErrorCode::InvalidProgram,
                "interface call has an invalid receiver contract");
        }
        if (std::holds_alternative<NullObject>(arguments.front())) {
            return fail(state, ErrorCode::NullReference,
                "interface call attempted to dereference null");
        }
        const auto* receiver = std::get_if<ObjectRef>(&arguments.front());
        const auto actualTypeId = receiver && state.heap
            ? state.heap->objectTypeId(*receiver)
            : std::optional<semantic::SymbolId>{};
        if (!actualTypeId) {
            return fail(state, ErrorCode::InvalidObjectReference,
                "interface call receiver has no runtime type descriptor");
        }
        const auto* implementation = runtimeInterfaceMap(
            state, *actualTypeId, reference.interfaceTypeId);
        if (!implementation ||
            reference.interfaceSlot >= implementation->slots.size()) {
            return fail(state, ErrorCode::InvalidProgram,
                "interface call slot is outside the runtime dispatch map");
        }
        targetSymbolId = implementation->slots[reference.interfaceSlot];
        if (targetSymbolId == 0) {
            return fail(state, ErrorCode::InvalidProgram,
                "interface call reached an unimplemented runtime slot");
        }
    }

    const auto found = state.functions->find(targetSymbolId);
    if (found != state.functions->end()) {
        return executeFunction(state, found->second, arguments, result);
    }
    if (reference.virtualDispatch || reference.interfaceDispatch) {
        return fail(state, ErrorCode::InvalidProgram,
            "runtime dispatch target function is unavailable");
    }
    RuntimeError externalError;
    ++state.statistics.externalCalls;
    emitTrace(state, TraceEventKind::ExternalCall, reference.name);
    std::optional<Value> externalResult;
    const ExternalFunction* fallback =
        state.externalResolver && *state.externalResolver
        ? state.externalResolver
        : nullptr;
    if (!state.determinism.invokeExternal(
            state.bindings,
            fallback,
            reference,
            arguments,
            externalResult,
            externalError)) {
        state.error = std::move(externalError);
        if (state.error.code == ErrorCode::None) {
            state.error.code = ErrorCode::ExternalFunctionUnresolved;
            state.error.message = "external function '" + reference.name + "' failed";
        }
        state.error.stackTrace = state.stack;
        std::reverse(state.error.stackTrace.begin(), state.error.stackTrace.end());
        return false;
    }
    if (!expectSignatureType(
            state,
            *externalResult,
            reference.returnType,
            reference.returnTypeId,
            "external return value")) {
        return false;
    }
    result = std::move(*externalResult);
    return true;
}

std::int64_t delegateWord(std::uint64_t value, bool high) noexcept {
    return static_cast<std::int64_t>(
        static_cast<std::uint32_t>(high ? value >> 32u : value));
}

bool readDelegateId(
    State& state,
    ObjectRef reference,
    std::size_t lowIndex,
    semantic::SymbolId& value,
    const char* context) {
    const auto low = state.heap->fieldGet(reference, lowIndex);
    const auto high = state.heap->fieldGet(reference, lowIndex + 1u);
    const auto* lowValue = low ? std::get_if<std::int64_t>(&*low) : nullptr;
    const auto* highValue = high ? std::get_if<std::int64_t>(&*high) : nullptr;
    if (!lowValue || !highValue || *lowValue < 0 || *highValue < 0 ||
        *lowValue > std::numeric_limits<std::uint32_t>::max() ||
        *highValue > std::numeric_limits<std::uint32_t>::max()) {
        return fail(state, ErrorCode::InvalidProgram, context);
    }
    value = static_cast<std::uint64_t>(
        static_cast<std::uint32_t>(*lowValue)) |
        (static_cast<std::uint64_t>(
            static_cast<std::uint32_t>(*highValue)) << 32u);
    return true;
}

bool flattenDelegate(
    State& state,
    semantic::SymbolId delegateTypeId,
    const Value& value,
    std::vector<Value>& leaves,
    std::size_t depth = 0) {
    if (std::holds_alternative<NullObject>(value)) return true;
    if (depth > 1024) {
        return fail(state, ErrorCode::InvalidProgram,
            "delegate invocation list is too deep");
    }
    const auto* reference = std::get_if<ObjectRef>(&value);
    const auto actualType = reference && state.heap
        ? state.heap->objectTypeId(*reference)
        : std::optional<semantic::SymbolId>{};
    if (!reference || !actualType || *actualType != delegateTypeId ||
        state.heap->fieldCount(*reference) != 8u) {
        return fail(state, ErrorCode::TypeMismatch,
            "delegate invocation list contains an incompatible value");
    }
    const auto flagsValue = state.heap->fieldGet(*reference, 3u);
    const auto* flags = flagsValue
        ? std::get_if<std::int64_t>(&*flagsValue)
        : nullptr;
    if (!flags || *flags < 0 || *flags > 4) {
        return fail(state, ErrorCode::InvalidProgram,
            "delegate invocation-list metadata is corrupt");
    }
    if (*flags != 4) {
        leaves.push_back(value);
        return true;
    }
    const auto left = state.heap->fieldGet(*reference, 0u);
    const auto right = state.heap->fieldGet(*reference, 7u);
    return left && right &&
        flattenDelegate(
            state, delegateTypeId, *left, leaves, depth + 1) &&
        flattenDelegate(
            state, delegateTypeId, *right, leaves, depth + 1);
}

bool sameDelegateLeaf(
    State& state,
    const Value& left,
    const Value& right) {
    const auto* leftRef = std::get_if<ObjectRef>(&left);
    const auto* rightRef = std::get_if<ObjectRef>(&right);
    if (!leftRef || !rightRef) return false;
    for (std::size_t index = 0; index < 7u; ++index) {
        const auto a = state.heap->fieldGet(*leftRef, index);
        const auto b = state.heap->fieldGet(*rightRef, index);
        if (!a || !b || !(*a == *b)) return false;
    }
    return true;
}

bool buildDelegateList(
    State& state,
    semantic::SymbolId delegateTypeId,
    const std::vector<Value>& leaves,
    Value& result) {
    if (leaves.empty()) {
        result = NullObject{};
        return true;
    }
    result = leaves.front();
    for (std::size_t index = 1; index < leaves.size(); ++index) {
        std::vector<Value> fields{
            result,
            std::int64_t{0},
            std::int64_t{0},
            std::int64_t{4},
            std::int64_t{0},
            std::int64_t{0},
            std::int64_t{0},
            leaves[index],
        };
        RuntimeError error;
        const auto allocated = state.heap->allocateObject(
            delegateTypeId, std::move(fields), {0u, 7u}, &error);
        if (!allocated) {
            return fail(state,
                error.code == ErrorCode::None
                    ? ErrorCode::OutOfMemory
                    : error.code,
                error.message.empty()
                    ? "delegate-list allocation failed"
                    : error.message);
        }
        result = *allocated;
    }
    return true;
}

bool executeInstruction(
    State& state,
    const bytecode::Module& module,
    const bytecode::Function& function,
    const bytecode::Instruction& instruction,
    const std::vector<Value>& arguments,
    std::vector<Value>& locals,
    std::vector<Value>& registers) {
    if (!consume(state, bytecode::opcodeName(instruction.opcode))) return false;
    auto operand = [&](std::size_t index) -> const Value* {
        if (index >= instruction.operands.size() || instruction.operands[index] >= registers.size()) {
            fail(state, ErrorCode::InvalidProgram, "instruction operand register is invalid");
            return nullptr;
        }
        return &registers[instruction.operands[index]];
    };
    auto storeResult = [&](Value value) {
        if (instruction.result == bytecode::InvalidRegister || instruction.result >= registers.size()) {
            return fail(state, ErrorCode::InvalidProgram, "instruction result register is invalid");
        }
        registers[instruction.result] = std::move(value);
        return true;
    };

    switch (instruction.opcode) {
    case bytecode::Opcode::LoadParameter:
        if (instruction.index >= arguments.size())
            return fail(state, ErrorCode::InvalidProgram, "parameter index is invalid");
        return storeResult(arguments[instruction.index]);
    case bytecode::Opcode::ConstantInt: {
        if (instruction.result >= function.registerTypes.size()) {
            return fail(state, ErrorCode::InvalidProgram,
                "constant result type metadata is missing");
        }
        const auto resultType = function.registerTypes[instruction.result];
        if (resultType == semantic::PrimitiveType::Long) {
            return storeResult(LongValue{instruction.integerImmediate});
        }
        if (resultType == semantic::PrimitiveType::Enum) {
            return storeResult(EnumValue{
                typeIdAt(function.registerTypeIds, instruction.result),
                instruction.integerImmediate});
        }
        return storeResult(instruction.integerImmediate);
    }
    case bytecode::Opcode::ConstantDouble:
        return storeResult(instruction.doubleImmediate);
    case bytecode::Opcode::ConstantBool:
        return storeResult(instruction.boolImmediate);
    case bytecode::Opcode::ConstantString: {
        if (!state.heap) {
            return fail(state, ErrorCode::InvalidProgram,
                "string allocation requires a managed heap");
        }
        RuntimeError allocationError;
        const auto reference = state.heap->allocateString(
            instruction.stringImmediate,
            &allocationError);
        if (!reference) {
            return fail(state,
                allocationError.code == ErrorCode::None
                    ? ErrorCode::OutOfMemory
                    : allocationError.code,
                allocationError.message.empty()
                    ? "managed string allocation failed"
                    : allocationError.message);
        }
        return storeResult(*reference);
    }
    case bytecode::Opcode::ConstantNull:
        return storeResult(std::monostate{});
    case bytecode::Opcode::LoadLocal:
        if (instruction.index >= locals.size())
            return fail(state, ErrorCode::InvalidProgram, "local index is invalid");
        return storeResult(locals[instruction.index]);
    case bytecode::Opcode::StoreLocal: {
        const auto* value = operand(0);
        if (!value) return false;
        if (instruction.index >= locals.size())
            return fail(state, ErrorCode::InvalidProgram, "local index is invalid");
        locals[instruction.index] = *value;
        return true;
    }
    case bytecode::Opcode::ConvertNullToString: {
        const auto* value = operand(0);
        if (!value || !expectType(state, *value, semantic::PrimitiveType::Null, "conversion")) return false;
        return storeResult(NullString{});
    }
    case bytecode::Opcode::ConvertNullToObject: {
        const auto* value = operand(0);
        if (!value || !expectType(state, *value, semantic::PrimitiveType::Null, "conversion")) return false;
        return storeResult(NullObject{});
    }
    case bytecode::Opcode::ConvertNullToArray: {
        const auto* value = operand(0);
        if (!value || !expectType(state, *value, semantic::PrimitiveType::Null, "conversion")) return false;
        return storeResult(NullArray{});
    }
    case bytecode::Opcode::ConvertIntToLong: {
        const auto* value = operand(0);
        if (!value || !expectType(state, *value, semantic::PrimitiveType::Int,
                "numeric conversion")) return false;
        return storeResult(LongValue{std::get<std::int64_t>(*value)});
    }
    case bytecode::Opcode::ConvertIntToDouble: {
        const auto* value = operand(0);
        if (!value || !expectType(state, *value, semantic::PrimitiveType::Int,
                "numeric conversion")) return false;
        return storeResult(static_cast<double>(std::get<std::int64_t>(*value)));
    }
    case bytecode::Opcode::ConvertLongToDouble: {
        const auto* value = operand(0);
        if (!value || !expectType(state, *value, semantic::PrimitiveType::Long,
                "numeric conversion")) return false;
        return storeResult(static_cast<double>(std::get<LongValue>(*value).value));
    }
    case bytecode::Opcode::ConvertNumeric: {
        const auto* value = operand(0);
        if (!value || instruction.result == bytecode::InvalidRegister ||
            instruction.result >= function.registerTypes.size()) {
            return fail(
                state, ErrorCode::InvalidProgram,
                "numeric conversion metadata is invalid");
        }
        Value converted;
        if (!tryConvertNumeric(
                *value, function.registerTypes[instruction.result], converted,
                instruction.checkedArithmetic)) {
            return fail(
                state, ErrorCode::IntegerOverflow,
                "numeric conversion overflow");
        }
        return storeResult(std::move(converted));
    }
    case bytecode::Opcode::ConstantTypeId:
        return storeResult(ULongValue{
            static_cast<std::uint64_t>(instruction.integerImmediate)});
    case bytecode::Opcode::IsType:
    case bytecode::Opcode::AsType: {
        const auto* value = operand(0);
        if (!value || instruction.operands.front() >=
                function.registerTypes.size()) {
            return fail(
                state, ErrorCode::InvalidProgram,
                "runtime type operation metadata is invalid");
        }
        const auto sourceType = function.registerTypes[
            instruction.operands.front()];
        const bool nullValue =
            std::holds_alternative<std::monostate>(*value) ||
            std::holds_alternative<NullString>(*value) ||
            std::holds_alternative<NullObject>(*value) ||
            std::holds_alternative<NullArray>(*value);
        bool matches = false;
        if (!nullValue) {
            if (instruction.elementType == semantic::PrimitiveType::Object) {
                const auto* reference = std::get_if<ObjectRef>(value);
                const auto actual = reference && state.heap
                    ? state.heap->objectTypeId(*reference)
                    : std::optional<semantic::SymbolId>{};
                matches = actual && runtimeAssignable(
                    state, *actual, instruction.elementTypeId);
            } else if (instruction.elementType ==
                       semantic::PrimitiveType::Array) {
                const auto* reference = std::get_if<ObjectRef>(value);
                const auto actual = reference && state.heap
                    ? state.heap->arrayTypeId(*reference)
                    : std::optional<semantic::SymbolId>{};
                matches = actual && *actual == instruction.elementTypeId;
            } else {
                matches = sourceType == instruction.elementType;
            }
        }
        if (instruction.opcode == bytecode::Opcode::IsType) {
            return storeResult(matches);
        }
        if (matches) return storeResult(*value);
        if (instruction.elementType == semantic::PrimitiveType::String) {
            return storeResult(NullString{});
        }
        if (instruction.elementType == semantic::PrimitiveType::Array) {
            return storeResult(NullArray{});
        }
        return storeResult(NullObject{});
    }
    case bytecode::Opcode::NewArray: {
        const auto* lengthValue = operand(0);
        if (!lengthValue ||
            !expectType(state, *lengthValue, semantic::PrimitiveType::Int,
                "array length") ||
            !state.heap) {
            return false;
        }
        const auto length = std::get<std::int64_t>(*lengthValue);
        if (length < 0) {
            return fail(state, ErrorCode::IndexOutOfRange,
                "array length cannot be negative");
        }
        if (static_cast<std::uint64_t>(length) >
            static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
            return fail(state, ErrorCode::OutOfMemory, "array length is too large");
        }
        if (instruction.result >= function.registerTypeIds.size()) {
            return fail(state, ErrorCode::InvalidProgram,
                "array allocation result type metadata is missing");
        }
        RuntimeError allocationError;
        const auto reference = state.heap->allocateTypedArray(
            function.registerTypeIds[instruction.result],
            instruction.elementType,
            instruction.elementTypeId,
            static_cast<std::size_t>(length),
            defaultValue(module, instruction.elementType, instruction.elementTypeId),
            &allocationError);
        if (!reference) {
            return fail(state,
                allocationError.code == ErrorCode::None
                    ? ErrorCode::OutOfMemory
                    : allocationError.code,
                allocationError.message.empty()
                    ? "managed array allocation failed"
                    : allocationError.message);
        }
        return storeResult(*reference);
    }
    case bytecode::Opcode::NewObject: {
        const auto* type = typeDescriptor(module, instruction.typeIndex);
        if (!type || type->interfaceType || type->delegateType ||
            !state.heap) {
            return fail(state, ErrorCode::InvalidProgram,
                "object allocation references an invalid class or heap");
        }
        std::vector<Value> fields;
        std::vector<std::size_t> referenceFields;
        fields.reserve(type->fields.size());
        for (std::size_t index = 0; index < type->fields.size(); ++index) {
            const auto fieldType = type->fields[index].type;
            fields.push_back(defaultValue(
                module, fieldType,
                exactTypeId(fieldType, type->fields[index].typeName)));
            if (fieldType == semantic::PrimitiveType::String ||
                fieldType == semantic::PrimitiveType::Object ||
                fieldType == semantic::PrimitiveType::Array ||
                fieldType == semantic::PrimitiveType::Struct) {
                referenceFields.push_back(index);
            }
        }
        RuntimeError allocationError;
        const auto reference = state.heap->allocateObject(
            type->id,
            std::move(fields),
            std::move(referenceFields),
            &allocationError);
        if (!reference) {
            return fail(state,
                allocationError.code == ErrorCode::None
                    ? ErrorCode::OutOfMemory
                    : allocationError.code,
                allocationError.message.empty()
                    ? "managed object allocation failed"
                    : allocationError.message);
        }
        return storeResult(*reference);
    }
    case bytecode::Opcode::NewStruct: {
        const auto* type = typeDescriptor(module, instruction.typeIndex);
        if (!type || type->kind != semantic::TypeKind::Struct) {
            return fail(state, ErrorCode::InvalidProgram,
                "struct allocation references an invalid type descriptor");
        }
        auto storage = std::make_shared<StructStorage>();
        storage->fields.reserve(type->fields.size());
        for (const auto& field : type->fields) {
            storage->fields.push_back(defaultValue(
                module, field.type, exactTypeId(field.type, field.typeName)));
        }
        return storeResult(StructValue{type->id, std::move(storage)});
    }
    case bytecode::Opcode::ArrayLength: {
        const auto* receiver = operand(0);
        if (!receiver) return false;
        if (std::holds_alternative<NullArray>(*receiver)) {
            return fail(state, ErrorCode::NullReference,
                "array length attempted to dereference null");
        }
        if (!expectType(state, *receiver, semantic::PrimitiveType::Array,
                "array length")) {
            return false;
        }
        const auto* reference = std::get_if<ObjectRef>(receiver);
        const auto length = reference && state.heap
            ? state.heap->arrayLength(*reference)
            : std::optional<std::size_t>{};
        if (!length || *length >
                static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
            return fail(state, ErrorCode::InvalidObjectReference,
                "array length references an invalid managed array");
        }
        return storeResult(static_cast<std::int64_t>(*length));
    }
    case bytecode::Opcode::LoadElement: {
        const auto* receiver = operand(0);
        const auto* indexValue = operand(1);
        if (!receiver || !indexValue) return false;
        if (std::holds_alternative<NullArray>(*receiver)) {
            return fail(state, ErrorCode::NullReference,
                "array element load attempted to dereference null");
        }
        if (!expectType(state, *receiver, semantic::PrimitiveType::Array,
                "array element load") ||
            !expectType(state, *indexValue, semantic::PrimitiveType::Int,
                "array index")) {
            return false;
        }
        const auto index = std::get<std::int64_t>(*indexValue);
        const auto reference = std::get<ObjectRef>(*receiver);
        const auto length = state.heap->arrayLength(reference);
        if (!length) {
            return fail(state, ErrorCode::InvalidObjectReference,
                "array element load references an invalid managed array");
        }
        if (index < 0 || static_cast<std::uint64_t>(index) >= *length) {
            return fail(state, ErrorCode::IndexOutOfRange,
                "array element index is out of range");
        }
        const auto elementType = state.heap->arrayElementType(reference);
        const auto elementTypeId = state.heap->arrayElementTypeId(reference);
        if (!elementType || !elementTypeId ||
            *elementType != instruction.elementType ||
            *elementTypeId != instruction.elementTypeId) {
            return fail(state, ErrorCode::TypeMismatch,
                "array element metadata does not match the bytecode operation");
        }
        const auto value = state.heap->arrayGet(
            reference, static_cast<std::size_t>(index));
        if (!value || !expectSignatureType(
                state, *value, instruction.elementType,
                instruction.elementTypeId, "array element value")) {
            return false;
        }
        return storeResult(*value);
    }
    case bytecode::Opcode::StoreElement: {
        const auto* receiver = operand(0);
        const auto* indexValue = operand(1);
        const auto* value = operand(2);
        if (!receiver || !indexValue || !value) return false;
        if (std::holds_alternative<NullArray>(*receiver)) {
            return fail(state, ErrorCode::NullReference,
                "array element store attempted to dereference null");
        }
        if (!expectType(state, *receiver, semantic::PrimitiveType::Array,
                "array element store") ||
            !expectType(state, *indexValue, semantic::PrimitiveType::Int,
                "array index") ||
            !expectSignatureType(state, *value, instruction.elementType,
                instruction.elementTypeId, "array element store value")) {
            return false;
        }
        const auto index = std::get<std::int64_t>(*indexValue);
        const auto reference = std::get<ObjectRef>(*receiver);
        const auto length = state.heap->arrayLength(reference);
        if (!length) {
            return fail(state, ErrorCode::InvalidObjectReference,
                "array element store references an invalid managed array");
        }
        if (index < 0 || static_cast<std::uint64_t>(index) >= *length) {
            return fail(state, ErrorCode::IndexOutOfRange,
                "array element index is out of range");
        }
        const auto elementType = state.heap->arrayElementType(reference);
        const auto elementTypeId = state.heap->arrayElementTypeId(reference);
        if (!elementType || !elementTypeId ||
            *elementType != instruction.elementType ||
            *elementTypeId != instruction.elementTypeId) {
            return fail(state, ErrorCode::TypeMismatch,
                "array element metadata does not match the bytecode operation");
        }
        RuntimeError arrayError;
        if (!state.heap->arraySet(
                reference,
                static_cast<std::size_t>(index),
                *value,
                &arrayError,
                instruction.elementType == semantic::PrimitiveType::Object
                    ? ArrayStoreTypePolicy::PrevalidatedAssignableObject
                    : ArrayStoreTypePolicy::Exact)) {
            return fail(state,
                arrayError.code == ErrorCode::None
                    ? ErrorCode::InvalidProgram
                    : arrayError.code,
                arrayError.message.empty()
                    ? "array element store failed"
                    : arrayError.message);
        }
        return true;
    }
    case bytecode::Opcode::CheckNotNull: {
        const auto* value = operand(0);
        const auto* type = typeDescriptor(module, instruction.typeIndex);
        if (!value || !type || !expectObject(state, *value, *type, false, "field access")) {
            return false;
        }
        return storeResult(*value);
    }
    case bytecode::Opcode::LoadField: {
        const auto* receiver = operand(0);
        const auto* type = typeDescriptor(module, instruction.typeIndex);
        if (!receiver || !type ||
            instruction.index >= type->fields.size() ||
            !expectObject(state, *receiver, *type, false, "field load")) {
            return false;
        }
        const auto reference = std::get<ObjectRef>(*receiver);
        const auto value = state.heap->fieldGet(reference, instruction.index);
        if (!value) {
            return fail(state, ErrorCode::InvalidProgram,
                "field load references an invalid field slot");
        }
        const auto& field = type->fields[instruction.index];
        if (!expectSignatureType(
                state,
                *value,
                field.type,
                exactTypeId(field.type, field.typeName),
                "field load value")) {
            return false;
        }
        return storeResult(*value);
    }
    case bytecode::Opcode::StoreField: {
        const auto* receiver = operand(0);
        const auto* value = operand(1);
        const auto* type = typeDescriptor(module, instruction.typeIndex);
        if (!receiver || !value || !type ||
            instruction.index >= type->fields.size() ||
            !expectObject(state, *receiver, *type, false, "field store") ||
            !expectSignatureType(
                state,
                *value,
                type->fields[instruction.index].type,
                exactTypeId(
                    type->fields[instruction.index].type,
                    type->fields[instruction.index].typeName),
                "field store value")) {
            return false;
        }
        RuntimeError fieldError;
        if (!state.heap->fieldSet(
                std::get<ObjectRef>(*receiver),
                instruction.index,
                *value,
                &fieldError)) {
            return fail(state,
                fieldError.code == ErrorCode::None
                    ? ErrorCode::InvalidProgram
                    : fieldError.code,
                fieldError.message.empty()
                    ? "field store failed"
                    : fieldError.message);
        }
        return true;
    }
    case bytecode::Opcode::LoadStructField: {
        const auto* receiver = operand(0);
        const auto* type = typeDescriptor(module, instruction.typeIndex);
        if (!receiver || !type || type->kind != semantic::TypeKind::Struct ||
            instruction.index >= type->fields.size() ||
            !expectSignatureType(state, *receiver, semantic::PrimitiveType::Struct,
                type->id, "struct field load")) {
            return false;
        }
        const auto& structure = std::get<StructValue>(*receiver);
        if (!structure.storage || instruction.index >= structure.storage->fields.size()) {
            return fail(state, ErrorCode::InvalidProgram,
                "struct field load references an invalid field slot");
        }
        const auto& field = type->fields[instruction.index];
        const auto& value = structure.storage->fields[instruction.index];
        if (!expectSignatureType(state, value, field.type,
                exactTypeId(field.type, field.typeName), "struct field load value")) {
            return false;
        }
        return storeResult(value);
    }
    case bytecode::Opcode::StoreStructField: {
        const auto* receiver = operand(0);
        const auto* value = operand(1);
        const auto* type = typeDescriptor(module, instruction.typeIndex);
        if (!receiver || !value || !type || type->kind != semantic::TypeKind::Struct ||
            instruction.index >= type->fields.size() ||
            !expectSignatureType(state, *receiver, semantic::PrimitiveType::Struct,
                type->id, "struct field store")) {
            return false;
        }
        const auto& field = type->fields[instruction.index];
        if (!expectSignatureType(state, *value, field.type,
                exactTypeId(field.type, field.typeName), "struct field store value")) {
            return false;
        }
        const auto& structure = std::get<StructValue>(*receiver);
        if (!structure.storage || instruction.index >= structure.storage->fields.size()) {
            return fail(state, ErrorCode::InvalidProgram,
                "struct field store references an invalid field slot");
        }
        auto storage = std::make_shared<StructStorage>(*structure.storage);
        storage->fields[instruction.index] = *value;
        return storeResult(StructValue{type->id, std::move(storage)});
    }
    case bytecode::Opcode::NewDelegate: {
        const auto* type = typeDescriptor(module, instruction.typeIndex);
        if (!type || !type->delegateType || !state.heap ||
            instruction.index >= module.functionReferences.size() ||
            instruction.operands.size() > 1) {
            return fail(state, ErrorCode::InvalidProgram,
                "delegate creation has invalid metadata");
        }
        const auto& reference = module.functionReferences[instruction.index];
        Value target = NullObject{};
        if (!instruction.operands.empty()) {
            const auto* value = operand(0);
            if (!value || reference.parameterTypes.empty() ||
                !expectSignatureType(
                    state, *value, reference.parameterTypes.front(),
                    typeIdAt(reference.parameterTypeIds, 0),
                    "delegate target")) {
                return false;
            }
            target = *value;
        } else if (!reference.parameterTypes.empty() &&
                   (reference.virtualDispatch ||
                    reference.interfaceDispatch)) {
            return fail(state, ErrorCode::InvalidProgram,
                "delegate dispatch target has no receiver");
        }
        std::uint64_t flags = 0;
        if (reference.virtualDispatch) flags |= 1u;
        if (reference.interfaceDispatch) flags |= 2u;
        std::vector<Value> fields{
            std::move(target),
            delegateWord(reference.symbolId, false),
            delegateWord(reference.symbolId, true),
            static_cast<std::int64_t>(flags),
            static_cast<std::int64_t>(reference.virtualDispatch
                ? reference.virtualSlot
                : reference.interfaceDispatch
                    ? reference.interfaceSlot
                    : 0u),
            delegateWord(reference.interfaceTypeId, false),
            delegateWord(reference.interfaceTypeId, true),
            NullObject{},
        };
        RuntimeError error;
        const auto allocated = state.heap->allocateObject(
            type->id, std::move(fields), {0u, 7u}, &error);
        if (!allocated) {
            return fail(state,
                error.code == ErrorCode::None
                    ? ErrorCode::OutOfMemory
                    : error.code,
                error.message.empty()
                    ? "delegate allocation failed"
                    : error.message);
        }
        return storeResult(*allocated);
    }
    case bytecode::Opcode::CombineDelegate:
    case bytecode::Opcode::RemoveDelegate: {
        const auto* type = typeDescriptor(module, instruction.typeIndex);
        const auto* left = operand(0);
        const auto* right = operand(1);
        if (!type || !type->delegateType || !left || !right ||
            !state.heap ||
            !expectSignatureType(
                state, *left, semantic::PrimitiveType::Object,
                type->id, "delegate combination left operand") ||
            !expectSignatureType(
                state, *right, semantic::PrimitiveType::Object,
                type->id, "delegate combination right operand")) {
            return false;
        }
        std::vector<Value> leftLeaves;
        std::vector<Value> rightLeaves;
        if (!flattenDelegate(
                state, type->id, *left, leftLeaves) ||
            !flattenDelegate(
                state, type->id, *right, rightLeaves)) {
            return false;
        }
        if (instruction.opcode ==
                bytecode::Opcode::CombineDelegate) {
            leftLeaves.insert(
                leftLeaves.end(),
                rightLeaves.begin(), rightLeaves.end());
        } else if (!rightLeaves.empty() &&
                   rightLeaves.size() <= leftLeaves.size()) {
            std::optional<std::size_t> removeAt;
            for (std::size_t start = 0;
                 start + rightLeaves.size() <= leftLeaves.size();
                 ++start) {
                bool matches = true;
                for (std::size_t index = 0;
                     index < rightLeaves.size(); ++index) {
                    if (!sameDelegateLeaf(
                            state,
                            leftLeaves[start + index],
                            rightLeaves[index])) {
                        matches = false;
                        break;
                    }
                }
                if (matches) removeAt = start;
            }
            if (removeAt) {
                leftLeaves.erase(
                    leftLeaves.begin() +
                        static_cast<std::ptrdiff_t>(*removeAt),
                    leftLeaves.begin() +
                        static_cast<std::ptrdiff_t>(
                            *removeAt + rightLeaves.size()));
            }
        }
        Value combined;
        if (!buildDelegateList(
                state, type->id, leftLeaves, combined)) {
            return false;
        }
        return storeResult(std::move(combined));
    }
    case bytecode::Opcode::InvokeDelegate: {
        const auto* type = typeDescriptor(module, instruction.typeIndex);
        const auto* delegateValue = operand(0);
        if (!type || !type->delegateType || !delegateValue ||
            !state.heap) {
            return fail(state, ErrorCode::InvalidProgram,
                "delegate invocation has invalid metadata");
        }
        if (std::holds_alternative<NullObject>(*delegateValue)) {
            return fail(state, ErrorCode::NullReference,
                "delegate invocation attempted to dereference null");
        }
        const auto* delegateRef = std::get_if<ObjectRef>(delegateValue);
        const auto actualType = delegateRef
            ? state.heap->objectTypeId(*delegateRef)
            : std::optional<semantic::SymbolId>{};
        if (!delegateRef || !actualType || *actualType != type->id ||
            state.heap->fieldCount(*delegateRef) != 8u) {
            return fail(state, ErrorCode::TypeMismatch,
                "delegate invocation received an incompatible value");
        }
        const auto rootFlagsValue = state.heap->fieldGet(
            *delegateRef, 3u);
        const auto* rootFlags = rootFlagsValue
            ? std::get_if<std::int64_t>(&*rootFlagsValue)
            : nullptr;
        if (rootFlags && *rootFlags == 4) {
            std::vector<Value> leaves;
            if (!flattenDelegate(
                    state, type->id, *delegateValue, leaves)) {
                return false;
            }
            Value lastResult;
            for (const auto& leaf : leaves) {
                auto nestedRegisters = registers;
                nestedRegisters[instruction.operands.front()] = leaf;
                if (!executeInstruction(
                        state, module, function, instruction,
                        arguments, locals, nestedRegisters)) {
                    return false;
                }
                if (instruction.result !=
                    bytecode::InvalidRegister) {
                    lastResult = nestedRegisters[instruction.result];
                }
            }
            if (instruction.result ==
                bytecode::InvalidRegister) {
                return true;
            }
            return storeResult(std::move(lastResult));
        }
        const semantic::FunctionSymbol* invoke = nullptr;
        for (const auto& method : type->methods) {
            if (method.name == "Invoke") {
                invoke = &method;
                break;
            }
        }
        if (!invoke || invoke->parameters.empty() ||
            instruction.operands.size() != invoke->parameters.size()) {
            return fail(state, ErrorCode::InvalidProgram,
                "delegate invocation signature is invalid");
        }
        std::vector<Value> delegateArguments;
        delegateArguments.reserve(instruction.operands.size());
        for (std::size_t index = 1;
             index < instruction.operands.size(); ++index) {
            const auto* value = operand(index);
            const auto& parameter = invoke->parameters[index];
            const auto parameterType =
                semantic::storageTypeOf(parameter);
            if (!value || !expectSignatureType(
                    state, *value, parameterType,
                    exactTypeId(
                        parameterType,
                        semantic::storageTypeNameOf(parameter)),
                    "delegate argument")) {
                return false;
            }
            delegateArguments.push_back(*value);
        }
        semantic::SymbolId targetSymbolId = 0;
        if (!readDelegateId(
                state, *delegateRef, 1u, targetSymbolId,
                "delegate callable identity is corrupt")) {
            return false;
        }
        const auto target = state.heap->fieldGet(*delegateRef, 0u);
        const auto flagsValue = state.heap->fieldGet(*delegateRef, 3u);
        const auto slotValue = state.heap->fieldGet(*delegateRef, 4u);
        const auto* flags = flagsValue
            ? std::get_if<std::int64_t>(&*flagsValue)
            : nullptr;
        const auto* slot = slotValue
            ? std::get_if<std::int64_t>(&*slotValue)
            : nullptr;
        if (!target || !flags || !slot || *flags < 0 || *flags > 3 ||
            *slot < 0 ||
            *slot > std::numeric_limits<std::uint32_t>::max()) {
            return fail(state, ErrorCode::InvalidProgram,
                "delegate dispatch metadata is corrupt");
        }
        const bool virtualDispatch = (*flags & 1) != 0;
        const bool interfaceDispatch = (*flags & 2) != 0;
        if (virtualDispatch && interfaceDispatch) {
            return fail(state, ErrorCode::InvalidProgram,
                "delegate dispatch modes conflict");
        }
        if (!std::holds_alternative<NullObject>(*target)) {
            delegateArguments.insert(delegateArguments.begin(), *target);
        }
        if (virtualDispatch || interfaceDispatch) {
            const auto* receiver = delegateArguments.empty()
                ? nullptr
                : std::get_if<ObjectRef>(&delegateArguments.front());
            const auto runtimeType = receiver
                ? state.heap->objectTypeId(*receiver)
                : std::optional<semantic::SymbolId>{};
            if (!runtimeType || !state.types) {
                return fail(state, ErrorCode::InvalidObjectReference,
                    "delegate receiver has no runtime type");
            }
            const auto foundType = state.types->find(*runtimeType);
            if (foundType == state.types->end()) {
                return fail(state, ErrorCode::InvalidProgram,
                    "delegate receiver descriptor is unavailable");
            }
            const auto dispatchSlot = static_cast<std::uint32_t>(*slot);
            if (virtualDispatch) {
                if (dispatchSlot >=
                    foundType->second->virtualDispatchTable.size()) {
                    return fail(state, ErrorCode::InvalidProgram,
                        "delegate virtual slot is invalid");
                }
                targetSymbolId = foundType->second
                    ->virtualDispatchTable[dispatchSlot];
            } else {
                semantic::SymbolId interfaceTypeId = 0;
                if (!readDelegateId(
                        state, *delegateRef, 5u, interfaceTypeId,
                        "delegate interface identity is corrupt")) {
                    return false;
                }
                const auto* map = runtimeInterfaceMap(
                    state, *runtimeType, interfaceTypeId);
                if (!map || dispatchSlot >= map->slots.size()) {
                    return fail(state, ErrorCode::InvalidProgram,
                        "delegate interface slot is invalid");
                }
                targetSymbolId = map->slots[dispatchSlot];
            }
        }
        const auto found = state.functions->find(targetSymbolId);
        if (found == state.functions->end()) {
            return fail(state, ErrorCode::InvalidProgram,
                "delegate target function is unavailable");
        }
        Value delegateResult;
        if (!executeFunction(
                state, found->second, delegateArguments, delegateResult)) {
            return false;
        }
        if (instruction.result == bytecode::InvalidRegister) return true;
        return storeResult(std::move(delegateResult));
    }
    case bytecode::Opcode::Call: {
        Value callResult;
        if (!executeCall(state, module, instruction, registers, callResult)) return false;
        if (instruction.result == bytecode::InvalidRegister) return true;
        return storeResult(std::move(callResult));
    }
    case bytecode::Opcode::NegateInt: {
        const auto* value = operand(0);
        if (!value || !expectType(state, *value, semantic::PrimitiveType::Int, "negation")) return false;
        Value output;
        const auto number = std::get<std::int64_t>(*value);
        if (!instruction.checkedArithmetic &&
            number == std::numeric_limits<std::int32_t>::min()) {
            return storeResult(number);
        }
        if (!checkedIntResult(state, -number, output)) return false;
        return storeResult(std::move(output));
    }
    case bytecode::Opcode::NegateLong: {
        const auto* value = operand(0);
        if (!value || !expectType(state, *value, semantic::PrimitiveType::Long,
                "negation")) return false;
        const auto number = std::get<LongValue>(*value).value;
        if (number == std::numeric_limits<std::int64_t>::min()) {
            if (!instruction.checkedArithmetic) return storeResult(LongValue{number});
            return fail(state, ErrorCode::IntegerOverflow, "long negation overflow");
        }
        return storeResult(LongValue{-number});
    }
    case bytecode::Opcode::NegateDouble: {
        const auto* value = operand(0);
        if (value && valueType(*value) == semantic::PrimitiveType::Float) {
            return storeResult(FloatValue{-std::get<FloatValue>(*value).value});
        }
        if (!value || !expectType(state, *value, semantic::PrimitiveType::Double,
                "negation")) return false;
        return storeResult(-std::get<double>(*value));
    }
    case bytecode::Opcode::LogicalNot: {
        const auto* value = operand(0);
        if (!value || !expectType(state, *value, semantic::PrimitiveType::Bool, "logical not")) return false;
        return storeResult(!std::get<bool>(*value));
    }
    default:
        break;
    }

    const auto* left = operand(0);
    const auto* right = operand(1);
    if (!left || !right) return false;

    if (instruction.opcode == bytecode::Opcode::Equal || instruction.opcode == bytecode::Opcode::NotEqual) {
        bool equal = false;
        if (!valuesEqual(state, *left, *right, equal)) return false;
        return storeResult(instruction.opcode == bytecode::Opcode::Equal ? equal : !equal);
    }
    const auto isLongOpcode = instruction.opcode >= bytecode::Opcode::AddLong &&
        instruction.opcode <= bytecode::Opcode::GreaterOrEqualLong;
    const auto isDoubleOpcode = instruction.opcode >= bytecode::Opcode::AddDouble &&
        instruction.opcode <= bytecode::Opcode::GreaterOrEqualDouble;
    Value output;
    if (isLongOpcode) {
        if (valueType(*left) == semantic::PrimitiveType::ULong) {
            if (!expectType(state, *right, semantic::PrimitiveType::ULong,
                    "binary operation")) return false;
            const auto a = std::get<ULongValue>(*left).value;
            const auto b = std::get<ULongValue>(*right).value;
            switch (instruction.opcode) {
            case bytecode::Opcode::AddLong:
                if (instruction.checkedArithmetic &&
                    b > std::numeric_limits<std::uint64_t>::max() - a)
                    return fail(state, ErrorCode::IntegerOverflow, "ulong addition overflow");
                return storeResult(ULongValue{a + b});
            case bytecode::Opcode::SubtractLong:
                if (instruction.checkedArithmetic && a < b) return fail(state, ErrorCode::IntegerOverflow, "ulong subtraction overflow");
                return storeResult(ULongValue{a - b});
            case bytecode::Opcode::MultiplyLong:
                if (instruction.checkedArithmetic && b != 0 &&
                    a > std::numeric_limits<std::uint64_t>::max() / b)
                    return fail(state, ErrorCode::IntegerOverflow, "ulong multiplication overflow");
                return storeResult(ULongValue{a * b});
            case bytecode::Opcode::DivideLong:
                if (b == 0) return fail(state, ErrorCode::DivisionByZero, "division by zero");
                return storeResult(ULongValue{a / b});
            case bytecode::Opcode::RemainderLong:
                if (b == 0) return fail(state, ErrorCode::DivisionByZero, "remainder by zero");
                return storeResult(ULongValue{a % b});
            case bytecode::Opcode::LessLong: return storeResult(a < b);
            case bytecode::Opcode::LessOrEqualLong: return storeResult(a <= b);
            case bytecode::Opcode::GreaterLong: return storeResult(a > b);
            case bytecode::Opcode::GreaterOrEqualLong: return storeResult(a >= b);
            default: break;
            }
        }
        if (!expectType(state, *left, semantic::PrimitiveType::Long, "binary operation") ||
            !expectType(state, *right, semantic::PrimitiveType::Long, "binary operation")) {
            return false;
        }
        const auto a = std::get<LongValue>(*left).value;
        const auto b = std::get<LongValue>(*right).value;
        switch (instruction.opcode) {
        case bytecode::Opcode::AddLong:
            if (!instruction.checkedArithmetic) {
                return storeResult(LongValue{signedFromBits(
                    static_cast<std::uint64_t>(a) +
                    static_cast<std::uint64_t>(b))});
            }
            return checkedLongAdd(state, a, b, output) && storeResult(output);
        case bytecode::Opcode::SubtractLong:
            if (!instruction.checkedArithmetic) {
                return storeResult(LongValue{signedFromBits(
                    static_cast<std::uint64_t>(a) -
                    static_cast<std::uint64_t>(b))});
            }
            return checkedLongSubtract(state, a, b, output) && storeResult(output);
        case bytecode::Opcode::MultiplyLong:
            if (!instruction.checkedArithmetic) {
                return storeResult(LongValue{signedFromBits(
                    static_cast<std::uint64_t>(a) *
                    static_cast<std::uint64_t>(b))});
            }
            return checkedLongMultiply(state, a, b, output) && storeResult(output);
        case bytecode::Opcode::DivideLong:
            if (b == 0) return fail(state, ErrorCode::DivisionByZero, "division by zero");
            if (a == std::numeric_limits<std::int64_t>::min() && b == -1) {
                return fail(state, ErrorCode::IntegerOverflow, "long division overflow");
            }
            return storeResult(LongValue{a / b});
        case bytecode::Opcode::RemainderLong:
            if (b == 0) return fail(state, ErrorCode::DivisionByZero, "remainder by zero");
            if (a == std::numeric_limits<std::int64_t>::min() && b == -1) {
                return storeResult(LongValue{});
            }
            return storeResult(LongValue{a % b});
        case bytecode::Opcode::LessLong: return storeResult(a < b);
        case bytecode::Opcode::LessOrEqualLong: return storeResult(a <= b);
        case bytecode::Opcode::GreaterLong: return storeResult(a > b);
        case bytecode::Opcode::GreaterOrEqualLong: return storeResult(a >= b);
        default: break;
        }
    }
    if (isDoubleOpcode) {
        if (valueType(*left) == semantic::PrimitiveType::Float) {
            if (!expectType(state, *right, semantic::PrimitiveType::Float,
                    "binary operation")) return false;
            const auto a = std::get<FloatValue>(*left).value;
            const auto b = std::get<FloatValue>(*right).value;
            switch (instruction.opcode) {
            case bytecode::Opcode::AddDouble: return storeResult(FloatValue{a + b});
            case bytecode::Opcode::SubtractDouble: return storeResult(FloatValue{a - b});
            case bytecode::Opcode::MultiplyDouble: return storeResult(FloatValue{a * b});
            case bytecode::Opcode::DivideDouble: return storeResult(FloatValue{a / b});
            case bytecode::Opcode::LessDouble: return storeResult(a < b);
            case bytecode::Opcode::LessOrEqualDouble: return storeResult(a <= b);
            case bytecode::Opcode::GreaterDouble: return storeResult(a > b);
            case bytecode::Opcode::GreaterOrEqualDouble: return storeResult(a >= b);
            default: break;
            }
        }
        if (!expectType(state, *left, semantic::PrimitiveType::Double, "binary operation") ||
            !expectType(state, *right, semantic::PrimitiveType::Double, "binary operation")) {
            return false;
        }
        const auto a = std::get<double>(*left);
        const auto b = std::get<double>(*right);
        switch (instruction.opcode) {
        case bytecode::Opcode::AddDouble: return storeResult(a + b);
        case bytecode::Opcode::SubtractDouble: return storeResult(a - b);
        case bytecode::Opcode::MultiplyDouble: return storeResult(a * b);
        case bytecode::Opcode::DivideDouble:
            // Floating-point division follows IEEE 754. Zero divisors produce
            // infinities or NaN rather than an integer-style runtime trap.
            return storeResult(a / b);
        case bytecode::Opcode::LessDouble: return storeResult(a < b);
        case bytecode::Opcode::LessOrEqualDouble: return storeResult(a <= b);
        case bytecode::Opcode::GreaterDouble: return storeResult(a > b);
        case bytecode::Opcode::GreaterOrEqualDouble: return storeResult(a >= b);
        default: break;
        }
    }
    if (valueType(*left) == semantic::PrimitiveType::UInt) {
        if (!expectType(state, *right, semantic::PrimitiveType::UInt,
                "binary operation")) return false;
        const auto a = std::get<UIntValue>(*left).value;
        const auto b = std::get<UIntValue>(*right).value;
        switch (instruction.opcode) {
        case bytecode::Opcode::AddInt:
            if (instruction.checkedArithmetic &&
                b > std::numeric_limits<std::uint32_t>::max() - a)
                return fail(state, ErrorCode::IntegerOverflow, "uint addition overflow");
            return storeResult(UIntValue{static_cast<std::uint32_t>(a + b)});
        case bytecode::Opcode::SubtractInt:
            if (instruction.checkedArithmetic && a < b) return fail(state, ErrorCode::IntegerOverflow, "uint subtraction overflow");
            return storeResult(UIntValue{static_cast<std::uint32_t>(a - b)});
        case bytecode::Opcode::MultiplyInt:
            if (instruction.checkedArithmetic && b != 0 &&
                a > std::numeric_limits<std::uint32_t>::max() / b)
                return fail(state, ErrorCode::IntegerOverflow, "uint multiplication overflow");
            return storeResult(UIntValue{static_cast<std::uint32_t>(a * b)});
        case bytecode::Opcode::DivideInt:
            if (b == 0) return fail(state, ErrorCode::DivisionByZero, "division by zero");
            return storeResult(UIntValue{static_cast<std::uint32_t>(a / b)});
        case bytecode::Opcode::RemainderInt:
            if (b == 0) return fail(state, ErrorCode::DivisionByZero, "remainder by zero");
            return storeResult(UIntValue{static_cast<std::uint32_t>(a % b)});
        case bytecode::Opcode::LessInt: return storeResult(a < b);
        case bytecode::Opcode::LessOrEqualInt: return storeResult(a <= b);
        case bytecode::Opcode::GreaterInt: return storeResult(a > b);
        case bytecode::Opcode::GreaterOrEqualInt: return storeResult(a >= b);
        default: break;
        }
    }
    if (!expectType(state, *left, semantic::PrimitiveType::Int, "binary operation") ||
        !expectType(state, *right, semantic::PrimitiveType::Int, "binary operation")) return false;
    const auto a = std::get<std::int64_t>(*left);
    const auto b = std::get<std::int64_t>(*right);
    switch (instruction.opcode) {
    case bytecode::Opcode::AddInt:
        if (!instruction.checkedArithmetic) return storeResult(wrapInt32(a + b));
        return checkedIntResult(state, a + b, output) && storeResult(output);
    case bytecode::Opcode::SubtractInt:
        if (!instruction.checkedArithmetic) return storeResult(wrapInt32(a - b));
        return checkedIntResult(state, a - b, output) && storeResult(output);
    case bytecode::Opcode::MultiplyInt:
        if (!instruction.checkedArithmetic) return storeResult(wrapInt32(a * b));
        return checkedIntResult(state, a * b, output) && storeResult(output);
    case bytecode::Opcode::DivideInt:
        if (b == 0) return fail(state, ErrorCode::DivisionByZero, "division by zero");
        if (a == std::numeric_limits<std::int32_t>::min() && b == -1)
            return fail(state, ErrorCode::IntegerOverflow, "integer division overflow");
        return storeResult(a / b);
    case bytecode::Opcode::RemainderInt:
        if (b == 0) return fail(state, ErrorCode::DivisionByZero, "remainder by zero");
        if (a == std::numeric_limits<std::int32_t>::min() && b == -1) return storeResult(std::int64_t{0});
        return storeResult(a % b);
    case bytecode::Opcode::LessInt: return storeResult(a < b);
    case bytecode::Opcode::LessOrEqualInt: return storeResult(a <= b);
    case bytecode::Opcode::GreaterInt: return storeResult(a > b);
    case bytecode::Opcode::GreaterOrEqualInt: return storeResult(a >= b);
    default:
        return fail(state, ErrorCode::InvalidProgram, "unsupported bytecode opcode");
    }
}

bool executeFunction(
    State& state,
    const FunctionLocation& location,
    const std::vector<Value>& arguments,
    Value& result) {
    const auto& function = *location.function;
    if (state.stack.size() >= state.limits.recursionLimit) {
        return fail(state, ErrorCode::RecursionLimitExceeded, "recursion limit exceeded");
    }
    if (arguments.size() != function.parameterTypes.size()) {
        return fail(state, ErrorCode::InvalidArguments, "argument count does not match function signature");
    }
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (!expectSignatureType(
                state,
                arguments[index],
                function.parameterTypes[index],
                typeIdAt(function.parameterTypeIds, index),
                "argument")) {
            return false;
        }
    }

    state.stack.push_back(location.module->name + "::" + function.name);
    ++state.statistics.functionCalls;
    state.statistics.maximumCallDepth = std::max(state.statistics.maximumCallDepth, state.stack.size());
    emitTrace(state, TraceEventKind::FunctionEnter);
    std::vector<Value> locals(function.localTypes.size());
    std::vector<Value> registers(function.registerTypes.size());
    ShadowFrameScope roots(state.shadowStack, &arguments, &locals, &registers);
    DebugFrameScope debugFrame(state, debug::DebugFrameView{
        location.module,
        &function,
        nullptr,
        &arguments,
        &locals,
        &registers,
    });
    const bytecode::BasicBlock* block = findBlock(function, 0);
    if (!block) {
        state.stack.pop_back();
        return fail(state, ErrorCode::InvalidProgram, "entry block is missing");
    }

    const auto routePendingException = [&](bytecode::BlockId protectedBlock) {
        if (!state.pendingException ||
            !std::holds_alternative<ObjectRef>(*state.pendingException)) {
            return static_cast<const bytecode::BasicBlock*>(nullptr);
        }
        const auto reference = std::get<ObjectRef>(*state.pendingException);
        const auto actual = state.heap->objectTypeId(reference);
        if (!actual) return static_cast<const bytecode::BasicBlock*>(nullptr);
        for (const auto& handler : function.exceptionHandlers) {
            if (std::find(handler.protectedBlocks.begin(),
                          handler.protectedBlocks.end(), protectedBlock) ==
                handler.protectedBlocks.end()) {
                continue;
            }
            if (handler.catchTypeId != 0 &&
                !runtimeAssignable(state, *actual, handler.catchTypeId)) {
                continue;
            }
            if (handler.exceptionLocal >= locals.size()) {
                return static_cast<const bytecode::BasicBlock*>(nullptr);
            }
            locals[handler.exceptionLocal] = *state.pendingException;
            state.pendingException.reset();
            return findBlock(function, handler.handlerBlock);
        }
        return static_cast<const bytecode::BasicBlock*>(nullptr);
    };

    while (true) {
        bool exceptionTransferred = false;
        for (std::size_t instructionIndex = 0;
             instructionIndex < block->instructions.size();
             ++instructionIndex) {
            const auto& instruction = block->instructions[instructionIndex];
            if (!debugSequencePoint(
                    state, *location.module, function, block->id,
                    static_cast<std::uint32_t>(instructionIndex), false)) {
                state.stack.pop_back();
                return false;
            }
            if (!executeInstruction(state, *location.module, function, instruction,
                    arguments, locals, registers)) {
                if (state.pendingException) {
                    if (const auto* handler = routePendingException(block->id)) {
                        block = handler;
                        exceptionTransferred = true;
                        break;
                    }
                }
                state.stack.pop_back();
                return false;
            }
        }
        if (exceptionTransferred) continue;
        if (!debugSequencePoint(
                state, *location.module, function, block->id,
                static_cast<std::uint32_t>(block->instructions.size()), true) ||
            !consume(state, bytecode::terminatorName(block->terminator.kind))) {
            state.stack.pop_back();
            return false;
        }
        const auto& terminator = block->terminator;
        if (terminator.kind == bytecode::TerminatorKind::Throw) {
            if (terminator.value >= registers.size() ||
                !std::holds_alternative<ObjectRef>(registers[terminator.value])) {
                state.stack.pop_back();
                return fail(state, ErrorCode::NullReference,
                    "throw requires a non-null script object");
            }
            state.pendingException = registers[terminator.value];
            if (const auto* handler = routePendingException(block->id)) {
                block = handler;
                continue;
            }
            state.stack.pop_back();
            return false;
        }
        if (terminator.kind == bytecode::TerminatorKind::ReturnVoid) {
            result = std::monostate{};
            emitTrace(state, TraceEventKind::FunctionExit,
                valueToString(result, state.heap));
            state.stack.pop_back();
            return true;
        }
        if (terminator.kind == bytecode::TerminatorKind::ReturnValue) {
            if (terminator.value >= registers.size()) {
                state.stack.pop_back();
                return fail(state, ErrorCode::InvalidProgram, "return register is invalid");
            }
            if (!expectSignatureType(
                    state,
                    registers[terminator.value],
                    function.returnType,
                    function.returnTypeId,
                    "return value")) {
                state.stack.pop_back();
                return false;
            }
            result = registers[terminator.value];
            emitTrace(state, TraceEventKind::FunctionExit,
                valueToString(result, state.heap));
            state.stack.pop_back();
            return true;
        }

        bytecode::BlockId target = terminator.target;
        const std::vector<bytecode::Register>* edgeArguments = &terminator.arguments;
        if (terminator.kind == bytecode::TerminatorKind::Branch) {
            if (terminator.condition >= registers.size() ||
                !expectType(state, registers[terminator.condition], semantic::PrimitiveType::Bool, "branch")) {
                state.stack.pop_back();
                return false;
            }
            if (!std::get<bool>(registers[terminator.condition])) {
                target = terminator.falseTarget;
                edgeArguments = &terminator.falseArguments;
            }
        } else if (terminator.kind != bytecode::TerminatorKind::Jump) {
            state.stack.pop_back();
            return fail(state, ErrorCode::InvalidProgram, "block has invalid terminator");
        }

        ++state.statistics.branchesTaken;
        emitTrace(state, TraceEventKind::Branch, "bb" + std::to_string(target));
        const auto* next = findBlock(function, target);
        if (!next || next->parameters.size() != edgeArguments->size()) {
            state.stack.pop_back();
            return fail(state, ErrorCode::InvalidProgram, "branch target or argument count is invalid");
        }
        std::vector<Value> transferred;
        transferred.reserve(edgeArguments->size());
        for (const auto source : *edgeArguments) {
            if (source >= registers.size()) {
                state.stack.pop_back();
                return fail(state, ErrorCode::InvalidProgram, "branch argument register is invalid");
            }
            transferred.push_back(registers[source]);
        }
        for (std::size_t index = 0; index < transferred.size(); ++index) {
            if (next->parameters[index].target >= registers.size()) {
                state.stack.pop_back();
                return fail(state, ErrorCode::InvalidProgram, "block parameter register is invalid");
            }
            registers[next->parameters[index].target] = std::move(transferred[index]);
        }
        block = next;
    }
}

} // namespace

Interpreter::Interpreter(std::vector<bytecode::Module> modules)
    : modules_(std::move(modules)),
      heap_(std::make_shared<ManagedHeap>()) {}

Interpreter::Interpreter(std::shared_ptr<const ProgramImage> program)
    : Interpreter(std::move(program), std::make_shared<ManagedHeap>()) {}

Interpreter::Interpreter(
    std::shared_ptr<const ProgramImage> program,
    std::shared_ptr<ManagedHeap> heap)
    : program_(std::move(program)),
      heap_(heap ? std::move(heap) : std::make_shared<ManagedHeap>()) {
    if (program_) modules_ = program_->modules();
}

void Interpreter::setExternalResolver(ExternalFunction resolver) {
    externalResolver_ = std::move(resolver);
}

void Interpreter::setHeap(std::shared_ptr<ManagedHeap> heap) {
    heap_ = heap ? std::move(heap) : std::make_shared<ManagedHeap>();
}

std::shared_ptr<ManagedHeap> Interpreter::heap() const noexcept { return heap_; }

void Interpreter::setBindingRegistry(std::shared_ptr<const BindingRegistry> bindings) {
    bindings_ = std::move(bindings);
}

ExecutionResult Interpreter::invoke(
    semantic::SymbolId symbolId,
    const std::vector<Value>& arguments,
    Limits limits) const {
    ExecutionOptions options;
    options.limits = limits;
    return invoke(symbolId, arguments, std::move(options));
}

ExecutionResult Interpreter::invoke(
    semantic::SymbolId symbolId,
    const std::vector<Value>& arguments,
    ExecutionOptions options) const {
    std::unordered_map<semantic::SymbolId, FunctionLocation> functions;
    std::unordered_map<semantic::SymbolId, const semantic::TypeSymbol*> types;
    for (const auto& module : modules_) {
        diagnostics::DiagnosticBag diagnostics;
        if (!bytecode::verifyModule(module, diagnostics)) {
            ExecutionResult invalid;
            invalid.error.code = ErrorCode::InvalidProgram;
            invalid.error.message = "bytecode verification failed before execution";
            return invalid;
        }
        for (const auto& type : module.types) {
            types.emplace(type.id, &type);
        }
        for (const auto& function : module.functions) {
            if (!functions.emplace(function.symbolId, FunctionLocation{&module, &function}).second) {
                ExecutionResult duplicate;
                duplicate.error.code = ErrorCode::InvalidProgram;
                duplicate.error.message = "duplicate function SymbolId across loaded modules";
                return duplicate;
            }
        }
    }
    const auto found = functions.find(symbolId);
    if (found == functions.end()) {
        ExecutionResult missing;
        missing.error.code = ErrorCode::FunctionNotFound;
        missing.error.message = "entry function was not found";
        return missing;
    }
    State state;
    state.limits = options.limits;
    state.functions = &functions;
    state.types = &types;
    state.externalResolver = &externalResolver_;
    state.bindings = bindings_.get();
    state.trace = &options.trace;
    state.profile = options.profile.get();
    state.determinism = DeterminismSession(options.determinism);
    state.heap = heap_.get();
    state.debugger = options.debugger.get();
    if (state.debugger) {
        if (program_) state.debugger->bindProgram(*program_);
        else state.debugger->bindModules(modules_);
    }
    Value value;
    ExecutionResult execution;
    execution.succeeded = executeFunction(state, found->second, arguments, value);
    if (!execution.succeeded && state.pendingException &&
        state.error.code == ErrorCode::None) {
        state.error.code = ErrorCode::ScriptException;
        state.error.message = "unhandled script exception";
        state.error.stackTrace = state.stack;
    }
    execution.succeeded = state.determinism.finish(
        execution.succeeded,
        value,
        state.error,
        state.statistics);
    execution.value = std::move(value);
    execution.error = std::move(state.error);
    execution.instructionsExecuted = state.executed;
    execution.statistics = state.statistics;
    execution.determinismDigest = state.determinism.digest();
    execution.replayEntriesConsumed = state.determinism.replayEntriesConsumed();
    return execution;
}

ExecutionResult Interpreter::invoke(
    const std::string& qualifiedName,
    const std::vector<Value>& arguments,
    Limits limits) const {
    ExecutionOptions options;
    options.limits = limits;
    return invoke(qualifiedName, arguments, std::move(options));
}

ExecutionResult Interpreter::invoke(
    const std::string& qualifiedName,
    const std::vector<Value>& arguments,
    ExecutionOptions options) const {
    for (const auto& module : modules_) {
        for (const auto& function : module.functions) {
            if (module.name + "::" + function.name == qualifiedName) {
                return invoke(function.symbolId, arguments, std::move(options));
            }
        }
    }
    ExecutionResult missing;
    missing.error.code = ErrorCode::FunctionNotFound;
    missing.error.message = "entry function '" + qualifiedName + "' was not found";
    return missing;
}

const char* errorCodeName(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::None: return "none";
    case ErrorCode::FunctionNotFound: return "function-not-found";
    case ErrorCode::InvalidArguments: return "invalid-arguments";
    case ErrorCode::TypeMismatch: return "type-mismatch";
    case ErrorCode::DivisionByZero: return "division-by-zero";
    case ErrorCode::IntegerOverflow: return "integer-overflow";
    case ErrorCode::InstructionBudgetExceeded: return "instruction-budget-exceeded";
    case ErrorCode::RecursionLimitExceeded: return "recursion-limit-exceeded";
    case ErrorCode::ExternalFunctionUnresolved: return "external-function-unresolved";
    case ErrorCode::DuplicateSymbol: return "duplicate-symbol";
    case ErrorCode::InvalidObjectReference: return "invalid-object-reference";
    case ErrorCode::InvalidNativeHandle: return "invalid-native-handle";
    case ErrorCode::NullReference: return "null-reference";
    case ErrorCode::IndexOutOfRange: return "index-out-of-range";
    case ErrorCode::OutOfMemory: return "out-of-memory";
    case ErrorCode::InvalidProgram: return "invalid-program";
    case ErrorCode::ExecutionTerminated: return "execution-terminated";
    case ErrorCode::DeterminismViolation: return "determinism-violation";
    case ErrorCode::ReplayMismatch: return "replay-mismatch";
    case ErrorCode::ScriptException: return "script-exception";
    }
    return "unknown";
}

bool tryConvertNumeric(
    const Value& value,
    semantic::PrimitiveType target,
    Value& result,
    bool checkedArithmetic) noexcept {
    long double number = 0.0L;
    std::uint64_t integerBits = 0;
    bool integerSource = true;
    if (const auto* byteValue = std::get_if<ByteValue>(&value)) {
        number = byteValue->value; integerBits = byteValue->value;
    } else if (const auto* sbyteValue = std::get_if<SByteValue>(&value)) {
        number = sbyteValue->value; integerBits = static_cast<std::uint64_t>(sbyteValue->value);
    } else if (const auto* shortValue = std::get_if<ShortValue>(&value)) {
        number = shortValue->value; integerBits = static_cast<std::uint64_t>(shortValue->value);
    } else if (const auto* ushortValue = std::get_if<UShortValue>(&value)) {
        number = ushortValue->value; integerBits = ushortValue->value;
    } else if (const auto* intValue = std::get_if<std::int64_t>(&value)) {
        number = static_cast<long double>(*intValue); integerBits = static_cast<std::uint64_t>(*intValue);
    } else if (const auto* uintValue = std::get_if<UIntValue>(&value)) {
        number = uintValue->value; integerBits = uintValue->value;
    } else if (const auto* longValue = std::get_if<LongValue>(&value)) {
        number = static_cast<long double>(longValue->value); integerBits = static_cast<std::uint64_t>(longValue->value);
    } else if (const auto* ulongValue = std::get_if<ULongValue>(&value)) {
        number = static_cast<long double>(ulongValue->value); integerBits = ulongValue->value;
    }
    else if (const auto* floatValue = std::get_if<FloatValue>(&value)) number = floatValue->value;
    else if (const auto* doubleValue = std::get_if<double>(&value)) number = *doubleValue;
    else if (const auto* charValue = std::get_if<CharValue>(&value)) {
        number = charValue->value; integerBits = charValue->value;
    }
    else return false;
    if (std::holds_alternative<FloatValue>(value) ||
        std::holds_alternative<double>(value)) integerSource = false;

    if (!checkedArithmetic && semantic::isIntegralType(target)) {
        if (!integerSource) {
            if (!std::isfinite(number)) integerBits = 0;
            else {
                const auto modulus = std::ldexp(1.0L,
                    target == semantic::PrimitiveType::Byte ||
                    target == semantic::PrimitiveType::SByte ? 8 :
                    target == semantic::PrimitiveType::Short ||
                    target == semantic::PrimitiveType::UShort ||
                    target == semantic::PrimitiveType::Char ? 16 :
                    target == semantic::PrimitiveType::Int ||
                    target == semantic::PrimitiveType::UInt ? 32 : 64);
                auto wrapped = std::fmod(std::trunc(number), modulus);
                if (wrapped < 0) wrapped += modulus;
                integerBits = static_cast<std::uint64_t>(wrapped);
            }
        }
        switch (target) {
        case semantic::PrimitiveType::Byte:
            result = ByteValue{static_cast<std::uint8_t>(integerBits)}; return true;
        case semantic::PrimitiveType::SByte: {
            const auto bits = static_cast<std::uint8_t>(integerBits);
            const auto signedValue = bits <= 0x7fU
                ? static_cast<std::int16_t>(bits)
                : static_cast<std::int16_t>(bits) - 0x100;
            result = SByteValue{static_cast<std::int8_t>(signedValue)}; return true;
        }
        case semantic::PrimitiveType::Short: {
            const auto bits = static_cast<std::uint16_t>(integerBits);
            const auto signedValue = bits <= 0x7fffU
                ? static_cast<std::int32_t>(bits)
                : static_cast<std::int32_t>(bits) - 0x10000;
            result = ShortValue{static_cast<std::int16_t>(signedValue)}; return true;
        }
        case semantic::PrimitiveType::UShort:
            result = UShortValue{static_cast<std::uint16_t>(integerBits)}; return true;
        case semantic::PrimitiveType::Char:
            result = CharValue{static_cast<char16_t>(integerBits)}; return true;
        case semantic::PrimitiveType::Int:
            result = wrapInt32(static_cast<std::int64_t>(integerBits)); return true;
        case semantic::PrimitiveType::UInt:
            result = UIntValue{static_cast<std::uint32_t>(integerBits)}; return true;
        case semantic::PrimitiveType::Long:
            result = LongValue{signedFromBits(integerBits)}; return true;
        case semantic::PrimitiveType::ULong:
            result = ULongValue{integerBits}; return true;
        default:
            break;
        }
    }

    if (checkedArithmetic) {
        const auto inRange = [&](long double minimum, long double maximum) {
            return std::isfinite(number) && number >= minimum && number <= maximum;
        };
        switch (target) {
        case semantic::PrimitiveType::Byte:
            if (!inRange(0, 255)) return false;
            break;
        case semantic::PrimitiveType::SByte:
            if (!inRange(-128, 127)) return false;
            break;
        case semantic::PrimitiveType::Short:
            if (!inRange(-32768, 32767)) return false;
            break;
        case semantic::PrimitiveType::UShort:
        case semantic::PrimitiveType::Char:
            if (!inRange(0, 65535)) return false;
            break;
        case semantic::PrimitiveType::Int:
            if (!inRange(
                    std::numeric_limits<std::int32_t>::min(),
                    std::numeric_limits<std::int32_t>::max())) return false;
            break;
        case semantic::PrimitiveType::UInt:
            if (!inRange(0, std::numeric_limits<std::uint32_t>::max())) return false;
            break;
        case semantic::PrimitiveType::Long:
            if (!inRange(
                    static_cast<long double>(std::numeric_limits<std::int64_t>::min()),
                    static_cast<long double>(std::numeric_limits<std::int64_t>::max()))) return false;
            break;
        case semantic::PrimitiveType::ULong:
            if (!inRange(0, static_cast<long double>(
                    std::numeric_limits<std::uint64_t>::max()))) return false;
            break;
        case semantic::PrimitiveType::Float:
            if (!inRange(
                    -std::numeric_limits<float>::max(),
                    std::numeric_limits<float>::max())) return false;
            break;
        default:
            break;
        }
    }

    switch (target) {
    case semantic::PrimitiveType::Byte:
        result = ByteValue{static_cast<std::uint8_t>(number)}; return true;
    case semantic::PrimitiveType::SByte:
        result = SByteValue{static_cast<std::int8_t>(number)}; return true;
    case semantic::PrimitiveType::Short:
        result = ShortValue{static_cast<std::int16_t>(number)}; return true;
    case semantic::PrimitiveType::UShort:
        result = UShortValue{static_cast<std::uint16_t>(number)}; return true;
    case semantic::PrimitiveType::Int:
        result = static_cast<std::int64_t>(number); return true;
    case semantic::PrimitiveType::UInt:
        result = UIntValue{static_cast<std::uint32_t>(number)}; return true;
    case semantic::PrimitiveType::Long:
        result = LongValue{static_cast<std::int64_t>(number)}; return true;
    case semantic::PrimitiveType::ULong:
        result = ULongValue{static_cast<std::uint64_t>(number)}; return true;
    case semantic::PrimitiveType::Float:
        result = FloatValue{static_cast<float>(number)}; return true;
    case semantic::PrimitiveType::Double:
        result = static_cast<double>(number); return true;
    case semantic::PrimitiveType::Char:
        result = CharValue{static_cast<char16_t>(number)}; return true;
    default:
        return false;
    }
}

semantic::PrimitiveType valueType(const Value& value) noexcept {
    if (std::holds_alternative<std::monostate>(value)) return semantic::PrimitiveType::Null;
    if (std::holds_alternative<NullString>(value)) return semantic::PrimitiveType::String;
    if (std::holds_alternative<NullObject>(value)) return semantic::PrimitiveType::Object;
    if (std::holds_alternative<NullArray>(value)) return semantic::PrimitiveType::Array;
    if (std::holds_alternative<bool>(value)) return semantic::PrimitiveType::Bool;
    if (std::holds_alternative<ByteValue>(value)) return semantic::PrimitiveType::Byte;
    if (std::holds_alternative<SByteValue>(value)) return semantic::PrimitiveType::SByte;
    if (std::holds_alternative<ShortValue>(value)) return semantic::PrimitiveType::Short;
    if (std::holds_alternative<UShortValue>(value)) return semantic::PrimitiveType::UShort;
    if (std::holds_alternative<std::int64_t>(value)) return semantic::PrimitiveType::Int;
    if (std::holds_alternative<UIntValue>(value)) return semantic::PrimitiveType::UInt;
    if (std::holds_alternative<LongValue>(value)) return semantic::PrimitiveType::Long;
    if (std::holds_alternative<ULongValue>(value)) return semantic::PrimitiveType::ULong;
    if (std::holds_alternative<FloatValue>(value)) return semantic::PrimitiveType::Float;
    if (std::holds_alternative<double>(value)) return semantic::PrimitiveType::Double;
    if (std::holds_alternative<CharValue>(value)) return semantic::PrimitiveType::Char;
    if (std::holds_alternative<EnumValue>(value)) return semantic::PrimitiveType::Enum;
    if (std::holds_alternative<StructValue>(value)) return semantic::PrimitiveType::Struct;
    if (std::holds_alternative<std::string>(value)) return semantic::PrimitiveType::String;
    if (std::holds_alternative<NativeHandle>(value)) return semantic::PrimitiveType::Handle;
    const auto& reference = std::get<ObjectRef>(value);
    if (reference.kind == ObjectKind::String) return semantic::PrimitiveType::String;
    if (reference.kind == ObjectKind::Array) return semantic::PrimitiveType::Array;
    return semantic::PrimitiveType::Object;
}

std::string valueToString(const Value& value) {
    return valueToString(value, nullptr);
}

std::string valueToString(const Value& value, const ManagedHeap* heap) {
    if (std::holds_alternative<std::monostate>(value)) return "null";
    if (std::holds_alternative<NullString>(value)) return "null";
    if (std::holds_alternative<NullObject>(value)) return "null";
    if (std::holds_alternative<NullArray>(value)) return "null";
    if (std::holds_alternative<bool>(value)) return std::get<bool>(value) ? "true" : "false";
    if (const auto* number = std::get_if<ByteValue>(&value)) return std::to_string(number->value);
    if (const auto* number = std::get_if<SByteValue>(&value)) return std::to_string(number->value);
    if (const auto* number = std::get_if<ShortValue>(&value)) return std::to_string(number->value);
    if (const auto* number = std::get_if<UShortValue>(&value)) return std::to_string(number->value);
    if (std::holds_alternative<std::int64_t>(value)) return std::to_string(std::get<std::int64_t>(value));
    if (const auto* number = std::get_if<UIntValue>(&value)) return std::to_string(number->value);
    if (const auto* number = std::get_if<LongValue>(&value)) return std::to_string(number->value);
    if (const auto* number = std::get_if<ULongValue>(&value)) return std::to_string(number->value);
    if (const auto* number = std::get_if<FloatValue>(&value)) return std::to_string(number->value);
    if (const auto* number = std::get_if<double>(&value)) return std::to_string(*number);
    if (const auto* character = std::get_if<CharValue>(&value)) return std::to_string(character->value);
    if (const auto* enumeration = std::get_if<EnumValue>(&value)) {
        return "<enum:type=0x" + std::to_string(enumeration->typeId) + ":" +
            std::to_string(enumeration->value) + ">";
    }
    if (const auto* structure = std::get_if<StructValue>(&value)) {
        return "<struct:type=0x" + std::to_string(structure->typeId) + ":fields=" +
            std::to_string(structure->storage ? structure->storage->fields.size() : 0) + ">";
    }
    if (std::holds_alternative<std::string>(value)) return std::get<std::string>(value);
    if (const auto* handle = std::get_if<NativeHandle>(&value)) {
        return handle->valid()
            ? "<handle:" + std::to_string(handle->slot) + ":" +
                std::to_string(handle->generation) + ":registry=" +
                std::to_string(handle->registryId) + ":type=0x" +
                std::to_string(handle->typeId) + ">"
            : "null";
    }
    const auto reference = std::get<ObjectRef>(value);
    if (heap) {
        const auto text = heap->stringView(reference);
        if (text) return std::string(*text);
    }
    const auto kind = reference.kind == ObjectKind::Array ? "array" : "object";
    return "<" + std::string(kind) + ":" + std::to_string(reference.slot) + ":" +
        std::to_string(reference.generation) + ":heap=" +
        std::to_string(reference.heapId) + ">";
}

} // namespace realscript::runtime
