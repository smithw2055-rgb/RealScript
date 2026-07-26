#include "realscript/runtime/Runtime.h"

#include "realscript/debug/Debugger.h"
#include "realscript/diagnostics/Diagnostic.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
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
    const ExternalFunction* externalResolver = nullptr;
    const TraceSink* trace = nullptr;
    ManagedHeap* heap = nullptr;
    debug::DebugController* debugger = nullptr;
    std::vector<debug::DebugFrameView> debugFrames;
    ShadowStack shadowStack;
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
    if (!state.trace || !*state.trace) return;
    TraceEvent event;
    event.kind = kind;
    event.function = state.stack.empty() ? std::string{} : state.stack.back();
    event.operation = std::move(operation);
    event.instructionIndex = state.executed;
    event.callDepth = state.stack.size();
    (*state.trace)(event);
}

bool fail(State& state, ErrorCode code, std::string message) {
    state.error.code = code;
    state.error.message = std::move(message);
    state.error.stackTrace = state.stack;
    std::reverse(state.error.stackTrace.begin(), state.error.stackTrace.end());
    emitTrace(state, TraceEventKind::RuntimeError, state.error.message);
    return false;
}

bool consume(State& state) {
    if (state.executed >= state.limits.instructionBudget) {
        return fail(state, ErrorCode::InstructionBudgetExceeded,
            "instruction budget exceeded");
    }
    ++state.executed;
    state.statistics.instructionsExecuted = state.executed;
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
    if (!actual || *actual != type.id) {
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
        if (!actual || *actual != typeId) {
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
    case semantic::PrimitiveType::Int: return std::int64_t{0};
    case semantic::PrimitiveType::Long: return LongValue{};
    case semantic::PrimitiveType::Double: return 0.0;
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

    const auto found = state.functions->find(reference.symbolId);
    if (found != state.functions->end()) {
        return executeFunction(state, found->second, arguments, result);
    }
    if (!*state.externalResolver) {
        return fail(state, ErrorCode::ExternalFunctionUnresolved,
            "external function '" + reference.name + "' is unresolved");
    }
    RuntimeError externalError;
    ++state.statistics.externalCalls;
    emitTrace(state, TraceEventKind::ExternalCall, reference.name);
    auto externalResult = (*state.externalResolver)(reference, arguments, externalError);
    if (!externalResult) {
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

bool executeInstruction(
    State& state,
    const bytecode::Module& module,
    const bytecode::Function& function,
    const bytecode::Instruction& instruction,
    const std::vector<Value>& arguments,
    std::vector<Value>& locals,
    std::vector<Value>& registers) {
    if (!consume(state)) return false;
    emitTrace(state, TraceEventKind::Instruction, bytecode::opcodeName(instruction.opcode));
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
        if (!type || !state.heap) {
            return fail(state, ErrorCode::InvalidProgram,
                "object allocation references an invalid type or heap");
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
                reference, static_cast<std::size_t>(index), *value, &arrayError)) {
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
        if (!checkedIntResult(state, -std::get<std::int64_t>(*value), output)) return false;
        return storeResult(std::move(output));
    }
    case bytecode::Opcode::NegateLong: {
        const auto* value = operand(0);
        if (!value || !expectType(state, *value, semantic::PrimitiveType::Long,
                "negation")) return false;
        const auto number = std::get<LongValue>(*value).value;
        if (number == std::numeric_limits<std::int64_t>::min()) {
            return fail(state, ErrorCode::IntegerOverflow, "long negation overflow");
        }
        return storeResult(LongValue{-number});
    }
    case bytecode::Opcode::NegateDouble: {
        const auto* value = operand(0);
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
        if (!expectType(state, *left, semantic::PrimitiveType::Long, "binary operation") ||
            !expectType(state, *right, semantic::PrimitiveType::Long, "binary operation")) {
            return false;
        }
        const auto a = std::get<LongValue>(*left).value;
        const auto b = std::get<LongValue>(*right).value;
        switch (instruction.opcode) {
        case bytecode::Opcode::AddLong:
            return checkedLongAdd(state, a, b, output) && storeResult(output);
        case bytecode::Opcode::SubtractLong:
            return checkedLongSubtract(state, a, b, output) && storeResult(output);
        case bytecode::Opcode::MultiplyLong:
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
    if (!expectType(state, *left, semantic::PrimitiveType::Int, "binary operation") ||
        !expectType(state, *right, semantic::PrimitiveType::Int, "binary operation")) return false;
    const auto a = std::get<std::int64_t>(*left);
    const auto b = std::get<std::int64_t>(*right);
    switch (instruction.opcode) {
    case bytecode::Opcode::AddInt: return checkedIntResult(state, a + b, output) && storeResult(output);
    case bytecode::Opcode::SubtractInt: return checkedIntResult(state, a - b, output) && storeResult(output);
    case bytecode::Opcode::MultiplyInt: return checkedIntResult(state, a * b, output) && storeResult(output);
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

    while (true) {
        for (std::size_t instructionIndex = 0;
             instructionIndex < block->instructions.size();
             ++instructionIndex) {
            const auto& instruction = block->instructions[instructionIndex];
            if (!debugSequencePoint(
                    state, *location.module, function, block->id,
                    static_cast<std::uint32_t>(instructionIndex), false) ||
                !executeInstruction(state, *location.module, function, instruction,
                    arguments, locals, registers)) {
                state.stack.pop_back();
                return false;
            }
        }
        if (!debugSequencePoint(
                state, *location.module, function, block->id,
                static_cast<std::uint32_t>(block->instructions.size()), true) ||
            !consume(state)) {
            state.stack.pop_back();
            return false;
        }
        const auto& terminator = block->terminator;
        if (terminator.kind == bytecode::TerminatorKind::ReturnVoid) {
            result = std::monostate{};
            emitTrace(state, TraceEventKind::FunctionExit);
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
    if (!bindings_) return;
    externalResolver_ = [bindings = bindings_](
        const bytecode::FunctionReference& reference,
        const std::vector<Value>& arguments,
        RuntimeError& error) {
        return bindings->invoke(reference, arguments, error);
    };
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
    for (const auto& module : modules_) {
        diagnostics::DiagnosticBag diagnostics;
        if (!bytecode::verifyModule(module, diagnostics)) {
            ExecutionResult invalid;
            invalid.error.code = ErrorCode::InvalidProgram;
            invalid.error.message = "bytecode verification failed before execution";
            return invalid;
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
    state.externalResolver = &externalResolver_;
    state.trace = &options.trace;
    state.heap = heap_.get();
    state.debugger = options.debugger.get();
    if (state.debugger) {
        if (program_) state.debugger->bindProgram(*program_);
        else state.debugger->bindModules(modules_);
    }
    Value value;
    ExecutionResult execution;
    execution.succeeded = executeFunction(state, found->second, arguments, value);
    execution.value = std::move(value);
    execution.error = std::move(state.error);
    execution.instructionsExecuted = state.executed;
    execution.statistics = state.statistics;
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
    }
    return "unknown";
}

semantic::PrimitiveType valueType(const Value& value) noexcept {
    if (std::holds_alternative<std::monostate>(value)) return semantic::PrimitiveType::Null;
    if (std::holds_alternative<NullString>(value)) return semantic::PrimitiveType::String;
    if (std::holds_alternative<NullObject>(value)) return semantic::PrimitiveType::Object;
    if (std::holds_alternative<NullArray>(value)) return semantic::PrimitiveType::Array;
    if (std::holds_alternative<bool>(value)) return semantic::PrimitiveType::Bool;
    if (std::holds_alternative<std::int64_t>(value)) return semantic::PrimitiveType::Int;
    if (std::holds_alternative<LongValue>(value)) return semantic::PrimitiveType::Long;
    if (std::holds_alternative<double>(value)) return semantic::PrimitiveType::Double;
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
    if (std::holds_alternative<std::int64_t>(value)) return std::to_string(std::get<std::int64_t>(value));
    if (const auto* number = std::get_if<LongValue>(&value)) return std::to_string(number->value);
    if (const auto* number = std::get_if<double>(&value)) return std::to_string(*number);
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
