#include "realscript/runtime/Runtime.h"

#include "realscript/diagnostics/Diagnostic.h"

#include <algorithm>
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


bool valuesEqual(State& state, const Value& left, const Value& right, bool& equal) {
    if (valueType(left) != valueType(right)) {
        equal = false;
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

const bytecode::BasicBlock* findBlock(
    const bytecode::Function& function,
    bytecode::BlockId id) {
    for (const auto& block : function.blocks) {
        if (block.id == id) return &block;
    }
    return nullptr;
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
    result = std::move(*externalResult);
    return true;
}

bool executeInstruction(
    State& state,
    const bytecode::Module& module,
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
    case bytecode::Opcode::ConstantInt:
        return storeResult(instruction.integerImmediate);
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
    if (!expectType(state, *left, semantic::PrimitiveType::Int, "binary operation") ||
        !expectType(state, *right, semantic::PrimitiveType::Int, "binary operation")) return false;
    const auto a = std::get<std::int64_t>(*left);
    const auto b = std::get<std::int64_t>(*right);
    Value output;
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
        if (!expectType(state, arguments[index], function.parameterTypes[index], "argument")) return false;
    }

    state.stack.push_back(location.module->name + "::" + function.name);
    ++state.statistics.functionCalls;
    state.statistics.maximumCallDepth = std::max(state.statistics.maximumCallDepth, state.stack.size());
    emitTrace(state, TraceEventKind::FunctionEnter);
    std::vector<Value> locals(function.localTypes.size());
    std::vector<Value> registers(function.registerTypes.size());
    ShadowFrameScope roots(state.shadowStack, &arguments, &locals, &registers);
    const bytecode::BasicBlock* block = findBlock(function, 0);
    if (!block) {
        state.stack.pop_back();
        return fail(state, ErrorCode::InvalidProgram, "entry block is missing");
    }

    while (true) {
        for (const auto& instruction : block->instructions) {
            if (!executeInstruction(state, *location.module, instruction,
                    arguments, locals, registers)) {
                state.stack.pop_back();
                return false;
            }
        }
        if (!consume(state)) {
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
    case ErrorCode::OutOfMemory: return "out-of-memory";
    case ErrorCode::InvalidProgram: return "invalid-program";
    }
    return "unknown";
}

semantic::PrimitiveType valueType(const Value& value) noexcept {
    if (std::holds_alternative<std::monostate>(value)) return semantic::PrimitiveType::Null;
    if (std::holds_alternative<NullString>(value)) return semantic::PrimitiveType::String;
    if (std::holds_alternative<bool>(value)) return semantic::PrimitiveType::Bool;
    if (std::holds_alternative<std::int64_t>(value)) return semantic::PrimitiveType::Int;
    if (std::holds_alternative<std::string>(value)) return semantic::PrimitiveType::String;
    const auto& reference = std::get<ObjectRef>(value);
    return reference.kind == ObjectKind::String
        ? semantic::PrimitiveType::String
        : semantic::PrimitiveType::Error;
}

std::string valueToString(const Value& value) {
    return valueToString(value, nullptr);
}

std::string valueToString(const Value& value, const ManagedHeap* heap) {
    if (std::holds_alternative<std::monostate>(value)) return "null";
    if (std::holds_alternative<NullString>(value)) return "null";
    if (std::holds_alternative<bool>(value)) return std::get<bool>(value) ? "true" : "false";
    if (std::holds_alternative<std::int64_t>(value)) return std::to_string(std::get<std::int64_t>(value));
    if (std::holds_alternative<std::string>(value)) return std::get<std::string>(value);
    const auto reference = std::get<ObjectRef>(value);
    if (heap) {
        const auto text = heap->stringView(reference);
        if (text) return std::string(*text);
    }
    return "<object:" + std::to_string(reference.slot) + ":" +
        std::to_string(reference.generation) + ">";
}

} // namespace realscript::runtime
