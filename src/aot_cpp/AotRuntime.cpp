#include "realscript/aot_cpp/AotRuntime.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace realscript::aot {
namespace {

runtime::DeterminismOperationId operationIdFromText(
    std::string_view operation) noexcept {
    return runtime::determinismOperationId(operation);
}

semantic::SymbolId typeIdAt(
    const semantic::SymbolId* ids,
    std::size_t count,
    std::size_t index) noexcept {
    return ids && index < count ? ids[index] : 0;
}

bool checkedIntResult(
    ExecutionContext& context,
    std::int64_t value,
    runtime::Value& output) {
    constexpr auto minimum =
        static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min());
    constexpr auto maximum =
        static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max());
    if (value < minimum || value > maximum) {
        return context.fail(
            runtime::ErrorCode::IntegerOverflow,
            "integer arithmetic overflow");
    }
    output = value;
    return true;
}

bool checkedLongAdd(
    ExecutionContext& context,
    std::int64_t left,
    std::int64_t right,
    runtime::Value& output) {
    if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
        (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right)) {
        return context.fail(
            runtime::ErrorCode::IntegerOverflow,
            "long arithmetic overflow");
    }
    output = runtime::LongValue{left + right};
    return true;
}

bool checkedLongSubtract(
    ExecutionContext& context,
    std::int64_t left,
    std::int64_t right,
    runtime::Value& output) {
    if ((right < 0 && left > std::numeric_limits<std::int64_t>::max() + right) ||
        (right > 0 && left < std::numeric_limits<std::int64_t>::min() + right)) {
        return context.fail(
            runtime::ErrorCode::IntegerOverflow,
            "long arithmetic overflow");
    }
    output = runtime::LongValue{left - right};
    return true;
}

bool requiresDescriptor(semantic::PrimitiveType type) noexcept {
    return type == semantic::PrimitiveType::Object ||
        type == semantic::PrimitiveType::Struct ||
        type == semantic::PrimitiveType::Enum;
}

bool requiresExactTypeId(semantic::PrimitiveType type) noexcept {
    return type == semantic::PrimitiveType::Object ||
        type == semantic::PrimitiveType::Struct ||
        type == semantic::PrimitiveType::Enum ||
        type == semantic::PrimitiveType::Array;
}

bool validSignatureIdentity(
    semantic::PrimitiveType type,
    semantic::SymbolId typeId) noexcept {
    return requiresExactTypeId(type) ? typeId != 0 : typeId == 0;
}

bool validStorageType(semantic::PrimitiveType type) noexcept {
    return type == semantic::PrimitiveType::Bool ||
        semantic::isNumericType(type) ||
        type == semantic::PrimitiveType::String ||
        type == semantic::PrimitiveType::Object ||
        type == semantic::PrimitiveType::Struct ||
        type == semantic::PrimitiveType::Enum ||
        type == semantic::PrimitiveType::Array ||
        type == semantic::PrimitiveType::Handle;
}

bool validReturnType(semantic::PrimitiveType type) noexcept {
    return type == semantic::PrimitiveType::Void || validStorageType(type);
}

bool checkedLongMultiply(
    ExecutionContext& context,
    std::int64_t left,
    std::int64_t right,
    runtime::Value& output) {
    if (left == 0 || right == 0) {
        output = runtime::LongValue{};
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
        return context.fail(
            runtime::ErrorCode::IntegerOverflow,
            "long arithmetic overflow");
    }
    output = runtime::LongValue{left * right};
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

} // namespace

FrameScope::FrameScope(
    ExecutionContext& context,
    const std::vector<runtime::Value>& arguments,
    const std::vector<runtime::Value>& locals,
    const std::vector<runtime::Value>& registers)
    : context_(&context) {
    context_->shadowStack().pushFrame(&arguments, &locals, &registers);
}

FrameScope::~FrameScope() {
    if (context_) context_->shadowStack().popFrame();
}

ExecutionContext::ExecutionContext(
    const ProgramDescriptor& program,
    std::shared_ptr<runtime::ManagedHeap> heap,
    std::shared_ptr<const runtime::BindingRegistry> bindings,
    runtime::ExecutionOptions options)
    : program_(&program),
      heap_(heap ? std::move(heap) : std::make_shared<runtime::ManagedHeap>()),
      bindings_(std::move(bindings)),
      options_(std::move(options)),
      determinism_(options_.determinism) {
    traceEventsEnabled_ =
        options_.determinism.mode != runtime::DeterminismMode::Off ||
        options_.profile != nullptr || static_cast<bool>(options_.trace);
    determinismEventsEnabled_ =
        options_.determinism.mode != runtime::DeterminismMode::Off;
    diagnosticEventsEnabled_ =
        static_cast<bool>(options_.trace);
    determinismOnly_ = determinismEventsEnabled_ &&
        options_.profile == nullptr && !diagnosticEventsEnabled_;
    for (std::uint32_t index = 0; index < program.typeCount; ++index) {
        const auto& type = program.types[index];
        types_.emplace(type.id, &type);
    }
    for (std::uint32_t index = 0; index < program.functionCount; ++index) {
        const auto& function = program.functions[index];
        functions_.emplace(function.symbolId, &function);
    }
}

const TypeDescriptor* ExecutionContext::findType(
    semantic::SymbolId typeId) const {
    const auto found = types_.find(typeId);
    return found == types_.end() ? nullptr : found->second;
}

bool ExecutionContext::isAssignable(
    semantic::SymbolId actual,
    semantic::SymbolId expected) const noexcept {
    if (actual == expected) return true;
    std::unordered_set<semantic::SymbolId> visited;
    auto current = actual;
    while (current != 0 && visited.insert(current).second) {
        const auto* type = findType(current);
        if (!type) return false;
        for (std::uint32_t index = 0;
             index < type->interfaceDispatchMapCount;
             ++index) {
            if (type->interfaceDispatchMaps &&
                type->interfaceDispatchMaps[index].interfaceTypeId == expected) {
                return true;
            }
        }
        if (type->baseTypeId == expected) return true;
        current = type->baseTypeId;
    }
    return false;
}

const FunctionDescriptor* ExecutionContext::findFunction(
    semantic::SymbolId symbolId) const {
    const auto found = functions_.find(symbolId);
    return found == functions_.end() ? nullptr : found->second;
}

void ExecutionContext::emitTrace(
    runtime::TraceEventKind kind,
    runtime::DeterminismOperationId operationId,
    std::string_view operation) {
    if (!traceEventsEnabled_) return;
    if (determinismEventsEnabled_) {
        determinismEventDigest_ = runtime::accumulateDeterminismEvent(
            determinismEventDigest_,
            kind,
            currentFunctionId_,
            operationId,
            executed_,
            stack_.size());
        ++determinismEventCount_;
    }
    if (options_.profile) {
        ++profileEvents_;
        auto& profile = profileFunctions_[currentFunctionId_];
        if (profile.function.empty() && !stack_.empty()) {
            profile.function = stack_.back();
        }
        profile.maximumCallDepth = std::max(
            profile.maximumCallDepth, stack_.size());
        switch (kind) {
        case runtime::TraceEventKind::FunctionEnter: ++profile.calls; break;
        case runtime::TraceEventKind::FunctionExit: ++profile.returns; break;
        case runtime::TraceEventKind::Instruction: ++profile.instructions; break;
        case runtime::TraceEventKind::Branch: ++profile.branches; break;
        case runtime::TraceEventKind::ExternalCall: ++profile.externalCalls; break;
        case runtime::TraceEventKind::GcStep: ++profile.gcSteps; break;
        case runtime::TraceEventKind::RuntimeError: ++profile.runtimeErrors; break;
        }
    }
    if (!diagnosticEventsEnabled_) return;
    runtime::TraceEvent event;
    event.kind = kind;
    event.function = stack_.empty() ? std::string{} : stack_.back();
    event.operation.assign(operation.data(), operation.size());
    event.instructionIndex = executed_;
    event.callDepth = stack_.size();
    if (options_.trace) options_.trace(event);
}

bool ExecutionContext::fail(
    runtime::ErrorCode code,
    std::string message) {
    error_.code = code;
    error_.message = std::move(message);
    error_.stackTrace = stack_;
    std::reverse(error_.stackTrace.begin(), error_.stackTrace.end());
    emitTrace(runtime::TraceEventKind::RuntimeError, 0, error_.message);
    return false;
}

bool ExecutionContext::consume(std::string_view operation) {
    return consume(operationIdFromText(operation), operation);
}

bool ExecutionContext::consume(
    runtime::DeterminismOperationId operationId,
    std::string_view operation) {
    if (executed_ >= options_.limits.instructionBudget) {
        return fail(
            runtime::ErrorCode::InstructionBudgetExceeded,
            "instruction budget exceeded");
    }
    ++executed_;
    statistics_.instructionsExecuted = executed_;
    if (traceEventsEnabled_) {
        if (determinismOnly_) {
            determinismEventDigest_ = runtime::accumulateDeterminismEvent(
                determinismEventDigest_,
                runtime::TraceEventKind::Instruction,
                currentFunctionId_,
                operationId,
                executed_,
                stack_.size());
            ++determinismEventCount_;
        } else {
            emitTrace(runtime::TraceEventKind::Instruction,
                operationId, operation);
        }
    }
    if (heap_ && options_.limits.gcWorkBudget != 0) {
        const auto work = heap_->step(
            shadowStack_,
            options_.limits.gcWorkBudget);
        statistics_.gcWorkPerformed += work;
        if (work != 0) {
            if (diagnosticEventsEnabled_) {
                const auto workText = std::to_string(work);
                emitTrace(runtime::TraceEventKind::GcStep, work, workText);
            } else {
                emitTrace(runtime::TraceEventKind::GcStep, work);
            }
        }
    }
    return true;
}

bool ExecutionContext::branch(std::uint32_t blockId) {
    ++statistics_.branchesTaken;
    if (traceEventsEnabled_) {
        if (determinismOnly_) {
            determinismEventDigest_ = runtime::accumulateDeterminismEvent(
                determinismEventDigest_,
                runtime::TraceEventKind::Branch,
                currentFunctionId_,
                blockId,
                executed_,
                stack_.size());
            ++determinismEventCount_;
        } else if (diagnosticEventsEnabled_) {
            const auto operation = "bb" + std::to_string(blockId);
            emitTrace(runtime::TraceEventKind::Branch, blockId, operation);
        } else {
            emitTrace(runtime::TraceEventKind::Branch, blockId);
        }
    }
    return true;
}

bool ExecutionContext::throwScript(const runtime::Value& value) {
    const auto* reference = std::get_if<runtime::ObjectRef>(&value);
    if (!reference || !heap_ || !heap_->isAlive(*reference)) {
        return fail(
            runtime::ErrorCode::NullReference,
            "throw requires a non-null script object");
    }
    pendingException_ = value;
    return false;
}

bool ExecutionContext::hasPendingException() const noexcept {
    return pendingException_.has_value();
}

bool ExecutionContext::pendingExceptionMatches(
    semantic::SymbolId typeId) const {
    if (!pendingException_) return false;
    if (typeId == 0) return true;
    const auto* reference = std::get_if<runtime::ObjectRef>(
        &*pendingException_);
    if (!reference || !heap_) return false;
    const auto actual = heap_->objectTypeId(*reference);
    return actual && isAssignable(*actual, typeId);
}

bool ExecutionContext::takePendingException(runtime::Value& value) {
    if (!pendingException_) return false;
    value = *pendingException_;
    pendingException_.reset();
    return true;
}

void ExecutionContext::reportUnhandledScriptException() {
    if (!pendingException_ || error_.code != runtime::ErrorCode::None) return;
    error_.code = runtime::ErrorCode::ScriptException;
    error_.message = "unhandled script exception";
    error_.stackTrace = stack_;
}

bool ExecutionContext::expectType(
    const runtime::Value& value,
    semantic::PrimitiveType type,
    std::string_view context) {
    if (const auto* reference = std::get_if<runtime::ObjectRef>(&value)) {
        if (!heap_ || !heap_->isAlive(*reference)) {
            return fail(
                runtime::ErrorCode::InvalidObjectReference,
                std::string(context) +
                    " contains an invalid managed object reference");
        }
    }
    if (runtime::valueType(value) != type) {
        return fail(
            runtime::ErrorCode::TypeMismatch,
            std::string(context) + " expected '" +
                semantic::primitiveTypeName(type) + "', got '" +
                semantic::primitiveTypeName(runtime::valueType(value)) + "'");
    }
    return true;
}

bool ExecutionContext::expectSignatureType(
    const runtime::Value& value,
    semantic::PrimitiveType type,
    semantic::SymbolId typeId,
    std::string_view context) {
    if (type == semantic::PrimitiveType::Void ||
        type == semantic::PrimitiveType::Null) {
        if (std::holds_alternative<std::monostate>(value)) return true;
        return fail(
            runtime::ErrorCode::TypeMismatch,
            std::string(context) + " expected an empty value");
    }
    if (!expectType(value, type, context)) return false;
    if (type == semantic::PrimitiveType::Object) {
        if (std::holds_alternative<runtime::NullObject>(value) || typeId == 0) {
            return true;
        }
        const auto* reference = std::get_if<runtime::ObjectRef>(&value);
        const auto actual = reference && heap_
            ? heap_->objectTypeId(*reference)
            : std::optional<semantic::SymbolId>{};
        if (!actual || !isAssignable(*actual, typeId)) {
            return fail(
                runtime::ErrorCode::TypeMismatch,
                std::string(context) + " has the wrong runtime object type");
        }
    } else if (type == semantic::PrimitiveType::Array) {
        if (std::holds_alternative<runtime::NullArray>(value) || typeId == 0) {
            return true;
        }
        const auto* reference = std::get_if<runtime::ObjectRef>(&value);
        const auto actual = reference && heap_
            ? heap_->arrayTypeId(*reference)
            : std::optional<semantic::SymbolId>{};
        if (!actual || *actual != typeId) {
            return fail(
                runtime::ErrorCode::TypeMismatch,
                std::string(context) + " has the wrong runtime array type");
        }
    } else if (type == semantic::PrimitiveType::Handle && typeId != 0) {
        const auto* handle = std::get_if<runtime::NativeHandle>(&value);
        if (!handle || (handle->valid() && handle->typeId != typeId) ||
            (!handle->valid() && handle->typeId != 0 && handle->typeId != typeId)) {
            return fail(
                runtime::ErrorCode::TypeMismatch,
                std::string(context) + " has the wrong native handle type");
        }
    } else if (type == semantic::PrimitiveType::Struct) {
        const auto* structure = std::get_if<runtime::StructValue>(&value);
        if (!structure || typeId == 0 || structure->typeId != typeId ||
            !structure->storage) {
            return fail(
                runtime::ErrorCode::TypeMismatch,
                std::string(context) + " has the wrong struct type");
        }
    } else if (type == semantic::PrimitiveType::Enum) {
        const auto* enumeration = std::get_if<runtime::EnumValue>(&value);
        if (!enumeration || typeId == 0 || enumeration->typeId != typeId) {
            return fail(
                runtime::ErrorCode::TypeMismatch,
                std::string(context) + " has the wrong enum type");
        }
    }
    return true;
}

runtime::Value ExecutionContext::defaultValue(
    semantic::PrimitiveType type,
    semantic::SymbolId typeId,
    std::size_t depth) const {
    switch (type) {
    case semantic::PrimitiveType::Bool: return false;
    case semantic::PrimitiveType::Byte: return runtime::ByteValue{};
    case semantic::PrimitiveType::SByte: return runtime::SByteValue{};
    case semantic::PrimitiveType::Short: return runtime::ShortValue{};
    case semantic::PrimitiveType::UShort: return runtime::UShortValue{};
    case semantic::PrimitiveType::Int: return std::int64_t{0};
    case semantic::PrimitiveType::UInt: return runtime::UIntValue{};
    case semantic::PrimitiveType::Long: return runtime::LongValue{};
    case semantic::PrimitiveType::ULong: return runtime::ULongValue{};
    case semantic::PrimitiveType::Float: return runtime::FloatValue{};
    case semantic::PrimitiveType::Double: return 0.0;
    case semantic::PrimitiveType::Char: return runtime::CharValue{};
    case semantic::PrimitiveType::String: return runtime::NullString{};
    case semantic::PrimitiveType::Object: return runtime::NullObject{};
    case semantic::PrimitiveType::Array: return runtime::NullArray{};
    case semantic::PrimitiveType::Handle: {
        runtime::NativeHandle handle;
        handle.typeId = typeId;
        return handle;
    }
    case semantic::PrimitiveType::Enum:
        return runtime::EnumValue{typeId, 0};
    case semantic::PrimitiveType::Struct: {
        if (depth > 64) return runtime::StructValue{};
        const auto* descriptor = findType(typeId);
        if (!descriptor || descriptor->kind != semantic::TypeKind::Struct) {
            return runtime::StructValue{};
        }
        auto storage = std::make_shared<runtime::StructStorage>();
        storage->fields.reserve(descriptor->fieldCount);
        for (std::uint32_t index = 0; index < descriptor->fieldCount; ++index) {
            const auto& field = descriptor->fields[index];
            storage->fields.push_back(defaultValue(
                field.type,
                field.typeId,
                depth + 1));
        }
        return runtime::StructValue{typeId, std::move(storage)};
    }
    case semantic::PrimitiveType::Null: return std::monostate{};
    case semantic::PrimitiveType::Void:
    case semantic::PrimitiveType::Error:
        break;
    }
    return std::monostate{};
}

bool ExecutionContext::invoke(
    semantic::SymbolId symbolId,
    const runtime::Value* arguments,
    std::size_t argumentCount,
    runtime::Value& result) {
    const auto* function = findFunction(symbolId);
    if (!function || !function->implementation) {
        return fail(
            runtime::ErrorCode::FunctionNotFound,
            "AOT entry function was not found");
    }
    if (stack_.size() >= options_.limits.recursionLimit) {
        return fail(
            runtime::ErrorCode::RecursionLimitExceeded,
            "recursion limit exceeded");
    }
    if (argumentCount != function->parameterCount) {
        return fail(
            runtime::ErrorCode::InvalidArguments,
            "argument count does not match function signature");
    }
    for (std::size_t index = 0; index < argumentCount; ++index) {
        if (!expectSignatureType(
                arguments[index],
                function->parameterTypes[index],
                typeIdAt(
                    function->parameterTypeIds,
                    function->parameterCount,
                    index),
                "argument")) {
            return false;
        }
    }

    const auto previousFunctionId = currentFunctionId_;
    currentFunctionId_ = function->symbolId;
    stack_.push_back(function->name ? function->name : "<aot-function>");
    ++statistics_.functionCalls;
    statistics_.maximumCallDepth = std::max(
        statistics_.maximumCallDepth,
        stack_.size());
    emitTrace(runtime::TraceEventKind::FunctionEnter);

    bool succeeded = false;
    try {
        succeeded = function->implementation(
            *this,
            arguments,
            argumentCount,
            result);
    } catch (const std::bad_alloc&) {
        succeeded = fail(runtime::ErrorCode::OutOfMemory,
            "AOT execution exhausted native memory");
    } catch (const std::exception& exception) {
        succeeded = fail(runtime::ErrorCode::InvalidProgram,
            std::string("AOT execution threw: ") + exception.what());
    } catch (...) {
        succeeded = fail(runtime::ErrorCode::InvalidProgram,
            "AOT execution threw an unknown exception");
    }
    if (!succeeded) {
        stack_.pop_back();
        currentFunctionId_ = previousFunctionId;
        return false;
    }
    if (!expectSignatureType(
            result,
            function->returnType,
            function->returnTypeId,
            "return value")) {
        stack_.pop_back();
        currentFunctionId_ = previousFunctionId;
        return false;
    }
    if (traceEventsEnabled_) {
        if (diagnosticEventsEnabled_) {
            const auto operation = runtime::valueToString(result, heap_.get());
            emitTrace(runtime::TraceEventKind::FunctionExit, 0, operation);
        } else {
            emitTrace(runtime::TraceEventKind::FunctionExit);
        }
    }
    stack_.pop_back();
    currentFunctionId_ = previousFunctionId;
    return true;
}

bool ExecutionContext::call(
    const CallSignature& signature,
    const runtime::Value* arguments,
    std::size_t argumentCount,
    runtime::Value& result) {
    if (argumentCount != signature.parameterCount) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "AOT call argument count does not match its signature");
    }
    for (std::size_t index = 0; index < argumentCount; ++index) {
        if (!expectSignatureType(
                arguments[index],
                signature.parameterTypes[index],
                typeIdAt(
                    signature.parameterTypeIds,
                    signature.parameterCount,
                    index),
                "call argument")) {
            return false;
        }
    }
    auto targetSymbolId = signature.symbolId;
    if (signature.virtualDispatch && signature.interfaceDispatch) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "AOT call cannot combine virtual and interface dispatch");
    }
    if (signature.virtualDispatch) {
        if (signature.virtualSlot ==
                std::numeric_limits<std::uint32_t>::max() ||
            argumentCount == 0 ||
            !signature.parameterTypes ||
            signature.parameterTypes[0] !=
                semantic::PrimitiveType::Object) {
            return fail(
                runtime::ErrorCode::InvalidProgram,
                "AOT virtual call has an invalid receiver contract");
        }
        if (std::holds_alternative<runtime::NullObject>(arguments[0])) {
            return fail(
                runtime::ErrorCode::NullReference,
                "AOT virtual call attempted to dereference null");
        }
        const auto* receiver =
            std::get_if<runtime::ObjectRef>(&arguments[0]);
        const auto actualTypeId = receiver && heap_
            ? heap_->objectTypeId(*receiver)
            : std::optional<semantic::SymbolId>{};
        const auto* actualType = actualTypeId
            ? findType(*actualTypeId)
            : nullptr;
        if (!actualType ||
            signature.virtualSlot >= actualType->virtualSlotCount ||
            !actualType->virtualDispatchTable) {
            return fail(
                runtime::ErrorCode::InvalidProgram,
                "AOT virtual call slot is outside the runtime dispatch table");
        }
        targetSymbolId =
            actualType->virtualDispatchTable[signature.virtualSlot];
        if (targetSymbolId == 0) {
            return fail(
                runtime::ErrorCode::InvalidProgram,
                "AOT virtual call reached an abstract runtime slot");
        }
    } else if (signature.interfaceDispatch) {
        if (signature.interfaceTypeId == 0 ||
            signature.interfaceSlot ==
                std::numeric_limits<std::uint32_t>::max() ||
            argumentCount == 0 ||
            !signature.parameterTypes ||
            signature.parameterTypes[0] !=
                semantic::PrimitiveType::Object ||
            typeIdAt(
                signature.parameterTypeIds,
                signature.parameterCount,
                0) != signature.interfaceTypeId) {
            return fail(
                runtime::ErrorCode::InvalidProgram,
                "AOT interface call has an invalid receiver contract");
        }
        if (std::holds_alternative<runtime::NullObject>(arguments[0])) {
            return fail(
                runtime::ErrorCode::NullReference,
                "AOT interface call attempted to dereference null");
        }
        const auto* receiver =
            std::get_if<runtime::ObjectRef>(&arguments[0]);
        const auto actualTypeId = receiver && heap_
            ? heap_->objectTypeId(*receiver)
            : std::optional<semantic::SymbolId>{};
        const auto* actualType = actualTypeId
            ? findType(*actualTypeId)
            : nullptr;
        const InterfaceDispatchDescriptor* implementation = nullptr;
        if (actualType && actualType->interfaceDispatchMaps) {
            for (std::uint32_t index = 0;
                 index < actualType->interfaceDispatchMapCount;
                 ++index) {
                const auto& candidate =
                    actualType->interfaceDispatchMaps[index];
                if (candidate.interfaceTypeId ==
                    signature.interfaceTypeId) {
                    implementation = &candidate;
                    break;
                }
            }
        }
        if (!implementation || !implementation->slots ||
            signature.interfaceSlot >= implementation->slotCount) {
            return fail(
                runtime::ErrorCode::InvalidProgram,
                "AOT interface call slot is outside the runtime dispatch map");
        }
        targetSymbolId = implementation->slots[signature.interfaceSlot];
        if (targetSymbolId == 0) {
            return fail(
                runtime::ErrorCode::InvalidProgram,
                "AOT interface call reached an unimplemented runtime slot");
        }
    }
    if (findFunction(targetSymbolId)) {
        return invoke(targetSymbolId, arguments, argumentCount, result);
    }
    if (signature.virtualDispatch || signature.interfaceDispatch) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "AOT runtime dispatch target function is unavailable");
    }
    if (!bindings_) {
        return fail(
            runtime::ErrorCode::ExternalFunctionUnresolved,
            std::string("external function '") +
                (signature.name ? signature.name : "<unknown>") +
                "' is unresolved");
    }
    bytecode::FunctionReference reference;
    reference.symbolId = signature.symbolId;
    reference.name = signature.name ? signature.name : "";
    reference.returnType = signature.returnType;
    reference.returnTypeId = signature.returnTypeId;
    reference.virtualDispatch = signature.virtualDispatch;
    reference.virtualSlot = signature.virtualSlot;
    reference.interfaceDispatch = signature.interfaceDispatch;
    reference.interfaceTypeId = signature.interfaceTypeId;
    reference.interfaceSlot = signature.interfaceSlot;
    if (signature.parameterCount != 0) {
        reference.parameterTypes.assign(
            signature.parameterTypes,
            signature.parameterTypes + signature.parameterCount);
    }
    if (signature.parameterTypeIds && signature.parameterCount != 0) {
        reference.parameterTypeIds.assign(
            signature.parameterTypeIds,
            signature.parameterTypeIds + signature.parameterCount);
    }
    std::vector<runtime::Value> values;
    if (argumentCount != 0) {
        values.assign(arguments, arguments + argumentCount);
    }
    runtime::RuntimeError externalError;
    ++statistics_.externalCalls;
    emitTrace(runtime::TraceEventKind::ExternalCall,
        reference.symbolId, reference.name);
    std::optional<runtime::Value> externalResult;
    if (!determinism_.invokeExternal(
            bindings_.get(),
            nullptr,
            reference,
            values,
            externalResult,
            externalError)) {
        error_ = std::move(externalError);
        if (error_.code == runtime::ErrorCode::None) {
            error_.code = runtime::ErrorCode::ExternalFunctionUnresolved;
            error_.message = "external function '" + reference.name + "' failed";
        }
        error_.stackTrace = stack_;
        std::reverse(error_.stackTrace.begin(), error_.stackTrace.end());
        return false;
    }
    if (!expectSignatureType(
            *externalResult,
            signature.returnType,
            signature.returnTypeId,
            "external return value")) {
        return false;
    }
    result = std::move(*externalResult);
    return true;
}

bool ExecutionContext::newDelegate(
    semantic::SymbolId delegateTypeId,
    const CallSignature& signature,
    const runtime::Value* target,
    std::size_t targetCount,
    runtime::Value& result) {
    const auto* delegateType = findType(delegateTypeId);
    if (!delegateType || !delegateType->delegateType ||
        targetCount > 1 || (targetCount != 0 && !target)) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "AOT delegate creation has invalid metadata");
    }
    runtime::Value targetValue = runtime::NullObject{};
    if (targetCount == 1) {
        if (signature.parameterCount == 0 ||
            !expectSignatureType(
                target[0], signature.parameterTypes[0],
                typeIdAt(
                    signature.parameterTypeIds,
                    signature.parameterCount, 0),
                "delegate target")) {
            return false;
        }
        targetValue = target[0];
    } else if (signature.virtualDispatch ||
               signature.interfaceDispatch) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "AOT delegate dispatch target has no receiver");
    }
    const auto word = [](std::uint64_t value, bool high) {
        return static_cast<std::int64_t>(
            static_cast<std::uint32_t>(high ? value >> 32u : value));
    };
    std::uint64_t flags = 0;
    if (signature.virtualDispatch) flags |= 1u;
    if (signature.interfaceDispatch) flags |= 2u;
    std::vector<runtime::Value> fields{
        std::move(targetValue),
        word(signature.symbolId, false),
        word(signature.symbolId, true),
        static_cast<std::int64_t>(flags),
        static_cast<std::int64_t>(signature.virtualDispatch
            ? signature.virtualSlot
            : signature.interfaceDispatch
                ? signature.interfaceSlot
                : 0u),
        word(signature.interfaceTypeId, false),
        word(signature.interfaceTypeId, true),
        runtime::NullObject{},
    };
    runtime::RuntimeError allocationError;
    const auto reference = heap_->allocateObject(
        delegateTypeId, std::move(fields), {0u, 7u},
        &allocationError);
    if (!reference) {
        return fail(
            allocationError.code == runtime::ErrorCode::None
                ? runtime::ErrorCode::OutOfMemory
                : allocationError.code,
            allocationError.message.empty()
                ? "AOT delegate allocation failed"
                : allocationError.message);
    }
    result = *reference;
    return true;
}

bool ExecutionContext::combineDelegates(
    semantic::SymbolId delegateTypeId,
    const runtime::Value& left,
    const runtime::Value& right,
    bool remove,
    runtime::Value& result) {
    const auto* delegateType = findType(delegateTypeId);
    if (!delegateType || !delegateType->delegateType ||
        !expectSignatureType(
            left, semantic::PrimitiveType::Object,
            delegateTypeId, "delegate combination left operand") ||
        !expectSignatureType(
            right, semantic::PrimitiveType::Object,
            delegateTypeId, "delegate combination right operand")) {
        return false;
    }
    const auto flatten = [&](const auto& self,
                             const runtime::Value& value,
                             std::vector<runtime::Value>& leaves,
                             std::size_t depth) -> bool {
        if (std::holds_alternative<runtime::NullObject>(value)) {
            return true;
        }
        if (depth > 1024) {
            return fail(
                runtime::ErrorCode::InvalidProgram,
                "AOT delegate invocation list is too deep");
        }
        const auto* reference =
            std::get_if<runtime::ObjectRef>(&value);
        const auto type = reference
            ? heap_->objectTypeId(*reference)
            : std::optional<semantic::SymbolId>{};
        const auto flagsValue = reference
            ? heap_->fieldGet(*reference, 3u)
            : std::optional<runtime::Value>{};
        const auto* flags = flagsValue
            ? std::get_if<std::int64_t>(&*flagsValue)
            : nullptr;
        if (!reference || !type || *type != delegateTypeId ||
            heap_->fieldCount(*reference) != 8u || !flags ||
            *flags < 0 || *flags > 4) {
            return fail(
                runtime::ErrorCode::InvalidProgram,
                "AOT delegate invocation-list metadata is corrupt");
        }
        if (*flags != 4) {
            leaves.push_back(value);
            return true;
        }
        const auto first = heap_->fieldGet(*reference, 0u);
        const auto second = heap_->fieldGet(*reference, 7u);
        return first && second &&
            self(self, *first, leaves, depth + 1) &&
            self(self, *second, leaves, depth + 1);
    };
    std::vector<runtime::Value> leftLeaves;
    std::vector<runtime::Value> rightLeaves;
    if (!flatten(flatten, left, leftLeaves, 0) ||
        !flatten(flatten, right, rightLeaves, 0)) {
        return false;
    }
    const auto sameLeaf = [&](const runtime::Value& a,
                              const runtime::Value& b) {
        const auto* aRef = std::get_if<runtime::ObjectRef>(&a);
        const auto* bRef = std::get_if<runtime::ObjectRef>(&b);
        if (!aRef || !bRef) return false;
        for (std::size_t index = 0; index < 7u; ++index) {
            const auto av = heap_->fieldGet(*aRef, index);
            const auto bv = heap_->fieldGet(*bRef, index);
            if (!av || !bv || !(*av == *bv)) return false;
        }
        return true;
    };
    if (!remove) {
        leftLeaves.insert(
            leftLeaves.end(), rightLeaves.begin(), rightLeaves.end());
    } else if (!rightLeaves.empty() &&
               rightLeaves.size() <= leftLeaves.size()) {
        std::optional<std::size_t> removeAt;
        for (std::size_t start = 0;
             start + rightLeaves.size() <= leftLeaves.size(); ++start) {
            bool matches = true;
            for (std::size_t index = 0;
                 index < rightLeaves.size(); ++index) {
                if (!sameLeaf(
                        leftLeaves[start + index], rightLeaves[index])) {
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
    if (leftLeaves.empty()) {
        result = runtime::NullObject{};
        return true;
    }
    result = leftLeaves.front();
    for (std::size_t index = 1; index < leftLeaves.size(); ++index) {
        std::vector<runtime::Value> fields{
            result,
            std::int64_t{0}, std::int64_t{0},
            std::int64_t{4}, std::int64_t{0},
            std::int64_t{0}, std::int64_t{0},
            leftLeaves[index],
        };
        runtime::RuntimeError allocationError;
        const auto allocated = heap_->allocateObject(
            delegateTypeId, std::move(fields), {0u, 7u},
            &allocationError);
        if (!allocated) {
            return fail(
                allocationError.code == runtime::ErrorCode::None
                    ? runtime::ErrorCode::OutOfMemory
                    : allocationError.code,
                allocationError.message.empty()
                    ? "AOT delegate-list allocation failed"
                    : allocationError.message);
        }
        result = *allocated;
    }
    return true;
}

bool ExecutionContext::invokeDelegate(
    semantic::SymbolId delegateTypeId,
    const runtime::Value* arguments,
    std::size_t argumentCount,
    runtime::Value& result) {
    const auto* delegateType = findType(delegateTypeId);
    if (!delegateType || !delegateType->delegateType ||
        !arguments || argumentCount == 0) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "AOT delegate invocation has invalid metadata");
    }
    if (std::holds_alternative<runtime::NullObject>(arguments[0])) {
        return fail(
            runtime::ErrorCode::NullReference,
            "AOT delegate invocation attempted to dereference null");
    }
    const auto* delegateRef =
        std::get_if<runtime::ObjectRef>(&arguments[0]);
    const auto actualType = delegateRef
        ? heap_->objectTypeId(*delegateRef)
        : std::optional<semantic::SymbolId>{};
    if (!delegateRef || !actualType || *actualType != delegateTypeId ||
        heap_->fieldCount(*delegateRef) != 8u) {
        return fail(
            runtime::ErrorCode::TypeMismatch,
            "AOT delegate invocation received an incompatible value");
    }
    const auto rootFlagsValue = heap_->fieldGet(*delegateRef, 3u);
    const auto* rootFlags = rootFlagsValue
        ? std::get_if<std::int64_t>(&*rootFlagsValue)
        : nullptr;
    if (rootFlags && *rootFlags == 4) {
        std::vector<runtime::Value> leaves;
        const auto flatten = [&](const auto& self,
                                 const runtime::Value& value,
                                 std::size_t depth) -> bool {
            if (depth > 1024) return false;
            const auto* reference =
                std::get_if<runtime::ObjectRef>(&value);
            const auto flagsValue = reference
                ? heap_->fieldGet(*reference, 3u)
                : std::optional<runtime::Value>{};
            const auto* flags = flagsValue
                ? std::get_if<std::int64_t>(&*flagsValue)
                : nullptr;
            if (!reference || !flags) return false;
            if (*flags != 4) {
                leaves.push_back(value);
                return true;
            }
            const auto first = heap_->fieldGet(*reference, 0u);
            const auto second = heap_->fieldGet(*reference, 7u);
            return first && second &&
                self(self, *first, depth + 1) &&
                self(self, *second, depth + 1);
        };
        if (!flatten(flatten, arguments[0], 0)) {
            return fail(
                runtime::ErrorCode::InvalidProgram,
                "AOT delegate invocation list is corrupt");
        }
        std::vector<runtime::Value> nested(
            arguments, arguments + argumentCount);
        for (const auto& leaf : leaves) {
            nested[0] = leaf;
            if (!consume("invoke.delegate")) return false;
            if (!invokeDelegate(
                    delegateTypeId, nested.data(), nested.size(), result)) {
                return false;
            }
        }
        return true;
    }
    const auto readId = [&](std::size_t lowIndex,
                            semantic::SymbolId& value) {
        const auto low = heap_->fieldGet(*delegateRef, lowIndex);
        const auto high = heap_->fieldGet(*delegateRef, lowIndex + 1u);
        const auto* lowValue = low
            ? std::get_if<std::int64_t>(&*low)
            : nullptr;
        const auto* highValue = high
            ? std::get_if<std::int64_t>(&*high)
            : nullptr;
        if (!lowValue || !highValue || *lowValue < 0 ||
            *highValue < 0 ||
            *lowValue > std::numeric_limits<std::uint32_t>::max() ||
            *highValue > std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }
        value = static_cast<std::uint64_t>(
            static_cast<std::uint32_t>(*lowValue)) |
            (static_cast<std::uint64_t>(
                static_cast<std::uint32_t>(*highValue)) << 32u);
        return true;
    };
    semantic::SymbolId targetSymbolId = 0;
    if (!readId(1u, targetSymbolId)) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "AOT delegate callable identity is corrupt");
    }
    const auto target = heap_->fieldGet(*delegateRef, 0u);
    const auto flagsValue = heap_->fieldGet(*delegateRef, 3u);
    const auto slotValue = heap_->fieldGet(*delegateRef, 4u);
    const auto* flags = flagsValue
        ? std::get_if<std::int64_t>(&*flagsValue)
        : nullptr;
    const auto* slot = slotValue
        ? std::get_if<std::int64_t>(&*slotValue)
        : nullptr;
    if (!target || !flags || !slot || *flags < 0 || *flags > 3 ||
        *slot < 0 ||
        *slot > std::numeric_limits<std::uint32_t>::max()) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "AOT delegate dispatch metadata is corrupt");
    }
    std::vector<runtime::Value> values;
    if (argumentCount > 1) {
        values.assign(arguments + 1, arguments + argumentCount);
    }
    if (!std::holds_alternative<runtime::NullObject>(*target)) {
        values.insert(values.begin(), *target);
    }
    const bool virtualDispatch = (*flags & 1) != 0;
    const bool interfaceDispatch = (*flags & 2) != 0;
    if (virtualDispatch && interfaceDispatch) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "AOT delegate dispatch modes conflict");
    }
    if (virtualDispatch || interfaceDispatch) {
        const auto* receiver = values.empty()
            ? nullptr
            : std::get_if<runtime::ObjectRef>(&values.front());
        const auto runtimeTypeId = receiver
            ? heap_->objectTypeId(*receiver)
            : std::optional<semantic::SymbolId>{};
        const auto* runtimeType = runtimeTypeId
            ? findType(*runtimeTypeId)
            : nullptr;
        if (!runtimeType) {
            return fail(
                runtime::ErrorCode::InvalidObjectReference,
                "AOT delegate receiver has no runtime type");
        }
        const auto dispatchSlot = static_cast<std::uint32_t>(*slot);
        if (virtualDispatch) {
            if (!runtimeType->virtualDispatchTable ||
                dispatchSlot >= runtimeType->virtualSlotCount) {
                return fail(
                    runtime::ErrorCode::InvalidProgram,
                    "AOT delegate virtual slot is invalid");
            }
            targetSymbolId =
                runtimeType->virtualDispatchTable[dispatchSlot];
        } else {
            semantic::SymbolId interfaceTypeId = 0;
            if (!readId(5u, interfaceTypeId)) {
                return fail(
                    runtime::ErrorCode::InvalidProgram,
                    "AOT delegate interface identity is corrupt");
            }
            const InterfaceDispatchDescriptor* map = nullptr;
            if (runtimeType->interfaceDispatchMaps) {
                for (std::uint32_t index = 0;
                     index < runtimeType->interfaceDispatchMapCount;
                     ++index) {
                    const auto& candidate =
                        runtimeType->interfaceDispatchMaps[index];
                    if (candidate.interfaceTypeId == interfaceTypeId) {
                        map = &candidate;
                        break;
                    }
                }
            }
            if (!map || !map->slots || dispatchSlot >= map->slotCount) {
                return fail(
                    runtime::ErrorCode::InvalidProgram,
                    "AOT delegate interface slot is invalid");
            }
            targetSymbolId = map->slots[dispatchSlot];
        }
    }
    if (!findFunction(targetSymbolId)) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "AOT delegate target function is unavailable");
    }
    return invoke(
        targetSymbolId,
        values.empty() ? nullptr : values.data(),
        values.size(),
        result);
}

bool ExecutionContext::constantString(
    std::string_view value,
    runtime::Value& result) {
    runtime::RuntimeError allocationError;
    const auto reference = heap_->allocateString(std::string(value), &allocationError);
    if (!reference) {
        return fail(
            allocationError.code == runtime::ErrorCode::None
                ? runtime::ErrorCode::OutOfMemory
                : allocationError.code,
            allocationError.message.empty()
                ? "managed string allocation failed"
                : allocationError.message);
    }
    result = *reference;
    return true;
}

bool ExecutionContext::newObject(
    semantic::SymbolId typeId,
    runtime::Value& result) {
    const auto* type = findType(typeId);
    if (!type || type->kind != semantic::TypeKind::Class ||
        type->interfaceType || type->delegateType) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "AOT object allocation references an invalid class descriptor");
    }
    std::vector<runtime::Value> fields;
    std::vector<std::size_t> references;
    fields.reserve(type->fieldCount);
    for (std::uint32_t index = 0; index < type->fieldCount; ++index) {
        const auto& field = type->fields[index];
        fields.push_back(defaultValue(field.type, field.typeId));
        if (semantic::isReferenceType(field.type) ||
            field.type == semantic::PrimitiveType::Struct) {
            references.push_back(index);
        }
    }
    runtime::RuntimeError allocationError;
    const auto reference = heap_->allocateObject(
        typeId,
        std::move(fields),
        std::move(references),
        &allocationError);
    if (!reference) {
        return fail(
            allocationError.code == runtime::ErrorCode::None
                ? runtime::ErrorCode::OutOfMemory
                : allocationError.code,
            allocationError.message.empty()
                ? "managed object allocation failed"
                : allocationError.message);
    }
    result = *reference;
    return true;
}

bool ExecutionContext::newStruct(
    semantic::SymbolId typeId,
    runtime::Value& result) {
    result = defaultValue(semantic::PrimitiveType::Struct, typeId);
    const auto* structure = std::get_if<runtime::StructValue>(&result);
    if (!structure || !structure->storage) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "AOT struct allocation references an invalid struct descriptor");
    }
    return true;
}

bool ExecutionContext::newArray(
    semantic::SymbolId arrayTypeId,
    semantic::PrimitiveType elementType,
    semantic::SymbolId elementTypeId,
    const runtime::Value& length,
    runtime::Value& result) {
    if (!expectType(length, semantic::PrimitiveType::Int, "array length")) {
        return false;
    }
    const auto size = std::get<std::int64_t>(length);
    if (size < 0) {
        return fail(
            runtime::ErrorCode::IndexOutOfRange,
            "array length cannot be negative");
    }
    if (static_cast<std::uint64_t>(size) >
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return fail(runtime::ErrorCode::OutOfMemory, "array length is too large");
    }
    runtime::RuntimeError allocationError;
    const auto reference = heap_->allocateTypedArray(
        arrayTypeId,
        elementType,
        elementTypeId,
        static_cast<std::size_t>(size),
        defaultValue(elementType, elementTypeId),
        &allocationError);
    if (!reference) {
        return fail(
            allocationError.code == runtime::ErrorCode::None
                ? runtime::ErrorCode::OutOfMemory
                : allocationError.code,
            allocationError.message.empty()
                ? "managed array allocation failed"
                : allocationError.message);
    }
    result = *reference;
    return true;
}

bool ExecutionContext::checkNotNull(
    semantic::SymbolId typeId,
    const runtime::Value& receiver,
    runtime::Value& result) {
    if (std::holds_alternative<runtime::NullObject>(receiver)) {
        return fail(
            runtime::ErrorCode::NullReference,
            "field access attempted to dereference null");
    }
    if (!expectSignatureType(
            receiver,
            semantic::PrimitiveType::Object,
            typeId,
            "field access")) {
        return false;
    }
    result = receiver;
    return true;
}

bool ExecutionContext::typeOperation(
    semantic::PrimitiveType sourceType,
    semantic::PrimitiveType targetType,
    semantic::SymbolId targetTypeId,
    const runtime::Value& value,
    bool safeCast,
    runtime::Value& result) {
    const bool nullValue =
        std::holds_alternative<std::monostate>(value) ||
        std::holds_alternative<runtime::NullString>(value) ||
        std::holds_alternative<runtime::NullObject>(value) ||
        std::holds_alternative<runtime::NullArray>(value);
    bool matches = false;
    if (!nullValue) {
        if (targetType == semantic::PrimitiveType::Object) {
            const auto* reference = std::get_if<runtime::ObjectRef>(&value);
            const auto actual = reference && heap_
                ? heap_->objectTypeId(*reference)
                : std::optional<semantic::SymbolId>{};
            matches = actual && isAssignable(*actual, targetTypeId);
        } else if (targetType == semantic::PrimitiveType::Array) {
            const auto* reference = std::get_if<runtime::ObjectRef>(&value);
            const auto actual = reference && heap_
                ? heap_->arrayTypeId(*reference)
                : std::optional<semantic::SymbolId>{};
            matches = actual && *actual == targetTypeId;
        } else {
            matches = sourceType == targetType;
        }
    }
    if (!safeCast) {
        result = matches;
        return true;
    }
    if (matches) {
        result = value;
    } else if (targetType == semantic::PrimitiveType::String) {
        result = runtime::NullString{};
    } else if (targetType == semantic::PrimitiveType::Array) {
        result = runtime::NullArray{};
    } else {
        result = runtime::NullObject{};
    }
    return true;
}

bool ExecutionContext::arrayLength(
    const runtime::Value& receiver,
    runtime::Value& result) {
    if (std::holds_alternative<runtime::NullArray>(receiver)) {
        return fail(
            runtime::ErrorCode::NullReference,
            "array length attempted to dereference null");
    }
    if (!expectType(receiver, semantic::PrimitiveType::Array, "array length")) {
        return false;
    }
    const auto reference = std::get<runtime::ObjectRef>(receiver);
    const auto length = heap_->arrayLength(reference);
    if (!length || *length > static_cast<std::size_t>(INT32_MAX)) {
        return fail(
            runtime::ErrorCode::InvalidObjectReference,
            "array length references an invalid managed array");
    }
    result = static_cast<std::int64_t>(*length);
    return true;
}

bool ExecutionContext::loadElement(
    semantic::PrimitiveType elementType,
    semantic::SymbolId elementTypeId,
    const runtime::Value& receiver,
    const runtime::Value& index,
    runtime::Value& result) {
    if (std::holds_alternative<runtime::NullArray>(receiver)) {
        return fail(
            runtime::ErrorCode::NullReference,
            "array element load attempted to dereference null");
    }
    if (!expectType(receiver, semantic::PrimitiveType::Array, "array element load") ||
        !expectType(index, semantic::PrimitiveType::Int, "array index")) {
        return false;
    }
    const auto numericIndex = std::get<std::int64_t>(index);
    const auto reference = std::get<runtime::ObjectRef>(receiver);
    const auto length = heap_->arrayLength(reference);
    if (!length) {
        return fail(
            runtime::ErrorCode::InvalidObjectReference,
            "array element load references an invalid managed array");
    }
    if (numericIndex < 0 ||
        static_cast<std::uint64_t>(numericIndex) >= *length) {
        return fail(
            runtime::ErrorCode::IndexOutOfRange,
            "array element index is out of range");
    }
    const auto actualType = heap_->arrayElementType(reference);
    const auto actualTypeId = heap_->arrayElementTypeId(reference);
    if (!actualType || !actualTypeId ||
        *actualType != elementType || *actualTypeId != elementTypeId) {
        return fail(
            runtime::ErrorCode::TypeMismatch,
            "array element metadata does not match the AOT operation");
    }
    const auto value = heap_->arrayGet(
        reference,
        static_cast<std::size_t>(numericIndex));
    if (!value || !expectSignatureType(
            *value,
            elementType,
            elementTypeId,
            "array element load value")) {
        return false;
    }
    result = *value;
    return true;
}

bool ExecutionContext::storeElement(
    semantic::PrimitiveType elementType,
    semantic::SymbolId elementTypeId,
    const runtime::Value& receiver,
    const runtime::Value& index,
    const runtime::Value& value) {
    runtime::Value ignored;
    if (std::holds_alternative<runtime::NullArray>(receiver)) {
        return fail(
            runtime::ErrorCode::NullReference,
            "array element store attempted to dereference null");
    }
    if (!expectType(receiver, semantic::PrimitiveType::Array, "array element store") ||
        !expectType(index, semantic::PrimitiveType::Int, "array index") ||
        !expectSignatureType(value, elementType, elementTypeId,
            "array element store value")) {
        return false;
    }
    const auto numericIndex = std::get<std::int64_t>(index);
    const auto reference = std::get<runtime::ObjectRef>(receiver);
    const auto length = heap_->arrayLength(reference);
    if (!length) {
        return fail(
            runtime::ErrorCode::InvalidObjectReference,
            "array element store references an invalid managed array");
    }
    if (numericIndex < 0 ||
        static_cast<std::uint64_t>(numericIndex) >= *length) {
        return fail(
            runtime::ErrorCode::IndexOutOfRange,
            "array element index is out of range");
    }
    const auto actualType = heap_->arrayElementType(reference);
    const auto actualTypeId = heap_->arrayElementTypeId(reference);
    if (!actualType || !actualTypeId ||
        *actualType != elementType || *actualTypeId != elementTypeId) {
        return fail(
            runtime::ErrorCode::TypeMismatch,
            "array element metadata does not match the AOT operation");
    }
    runtime::RuntimeError arrayError;
    if (!heap_->arraySet(
            reference,
            static_cast<std::size_t>(numericIndex),
            value,
            &arrayError,
            elementType == semantic::PrimitiveType::Object
                ? runtime::ArrayStoreTypePolicy::PrevalidatedAssignableObject
                : runtime::ArrayStoreTypePolicy::Exact)) {
        return fail(
            arrayError.code == runtime::ErrorCode::None
                ? runtime::ErrorCode::InvalidProgram
                : arrayError.code,
            arrayError.message.empty()
                ? "array element store failed"
                : arrayError.message);
    }
    return true;
}

bool ExecutionContext::loadField(
    semantic::SymbolId typeId,
    std::size_t fieldIndex,
    const runtime::Value& receiver,
    runtime::Value& result) {
    const auto* type = findType(typeId);
    if (!type || type->kind != semantic::TypeKind::Class ||
        fieldIndex >= type->fieldCount) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "AOT field load references invalid type metadata");
    }
    runtime::Value checked;
    if (!checkNotNull(typeId, receiver, checked)) return false;
    const auto value = heap_->fieldGet(
        std::get<runtime::ObjectRef>(checked),
        fieldIndex);
    if (!value) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "field load references an invalid field slot");
    }
    const auto& field = type->fields[fieldIndex];
    if (!expectSignatureType(
            *value,
            field.type,
            field.typeId,
            "field load value")) {
        return false;
    }
    result = *value;
    return true;
}

bool ExecutionContext::storeField(
    semantic::SymbolId typeId,
    std::size_t fieldIndex,
    const runtime::Value& receiver,
    const runtime::Value& value) {
    const auto* type = findType(typeId);
    if (!type || type->kind != semantic::TypeKind::Class ||
        fieldIndex >= type->fieldCount) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "AOT field store references invalid type metadata");
    }
    runtime::Value checked;
    if (!checkNotNull(typeId, receiver, checked)) return false;
    const auto& field = type->fields[fieldIndex];
    if (!expectSignatureType(
            value,
            field.type,
            field.typeId,
            "field store value")) {
        return false;
    }
    runtime::RuntimeError fieldError;
    if (!heap_->fieldSet(
            std::get<runtime::ObjectRef>(checked),
            fieldIndex,
            value,
            &fieldError)) {
        return fail(
            fieldError.code == runtime::ErrorCode::None
                ? runtime::ErrorCode::InvalidProgram
                : fieldError.code,
            fieldError.message.empty()
                ? "field store failed"
                : fieldError.message);
    }
    return true;
}

bool ExecutionContext::loadStructField(
    semantic::SymbolId typeId,
    std::size_t fieldIndex,
    const runtime::Value& receiver,
    runtime::Value& result) {
    const auto* type = findType(typeId);
    if (!type || type->kind != semantic::TypeKind::Struct ||
        fieldIndex >= type->fieldCount ||
        !expectSignatureType(
            receiver,
            semantic::PrimitiveType::Struct,
            typeId,
            "struct field load")) {
        return false;
    }
    const auto& structure = std::get<runtime::StructValue>(receiver);
    if (!structure.storage || fieldIndex >= structure.storage->fields.size()) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "struct field load references an invalid field slot");
    }
    const auto& field = type->fields[fieldIndex];
    const auto& value = structure.storage->fields[fieldIndex];
    if (!expectSignatureType(
            value,
            field.type,
            field.typeId,
            "struct field load value")) {
        return false;
    }
    result = value;
    return true;
}

bool ExecutionContext::storeStructField(
    semantic::SymbolId typeId,
    std::size_t fieldIndex,
    const runtime::Value& receiver,
    const runtime::Value& value,
    runtime::Value& result) {
    const auto* type = findType(typeId);
    if (!type || type->kind != semantic::TypeKind::Struct ||
        fieldIndex >= type->fieldCount ||
        !expectSignatureType(
            receiver,
            semantic::PrimitiveType::Struct,
            typeId,
            "struct field store")) {
        return false;
    }
    const auto& field = type->fields[fieldIndex];
    if (!expectSignatureType(
            value,
            field.type,
            field.typeId,
            "struct field store value")) {
        return false;
    }
    const auto& structure = std::get<runtime::StructValue>(receiver);
    if (!structure.storage || fieldIndex >= structure.storage->fields.size()) {
        return fail(
            runtime::ErrorCode::InvalidProgram,
            "struct field store references an invalid field slot");
    }
    auto storage = std::make_shared<runtime::StructStorage>(*structure.storage);
    storage->fields[fieldIndex] = value;
    result = runtime::StructValue{typeId, std::move(storage)};
    return true;
}

bool ExecutionContext::convert(
    semantic::ConversionKind conversion,
    semantic::PrimitiveType targetType,
    bool checkedArithmetic,
    const runtime::Value& value,
    runtime::Value& result) {
    switch (conversion) {
    case semantic::ConversionKind::Identity:
        result = value;
        return true;
    case semantic::ConversionKind::NullToString:
        if (!expectType(value, semantic::PrimitiveType::Null, "conversion")) {
            return false;
        }
        result = runtime::NullString{};
        return true;
    case semantic::ConversionKind::NullToObject:
        if (!expectType(value, semantic::PrimitiveType::Null, "conversion")) {
            return false;
        }
        result = runtime::NullObject{};
        return true;
    case semantic::ConversionKind::NullToArray:
        if (!expectType(value, semantic::PrimitiveType::Null, "conversion")) {
            return false;
        }
        result = runtime::NullArray{};
        return true;
    case semantic::ConversionKind::IntToLong:
        if (!expectType(value, semantic::PrimitiveType::Int, "numeric conversion")) {
            return false;
        }
        result = runtime::LongValue{std::get<std::int64_t>(value)};
        return true;
    case semantic::ConversionKind::IntToDouble:
        if (!expectType(value, semantic::PrimitiveType::Int, "numeric conversion")) {
            return false;
        }
        result = static_cast<double>(std::get<std::int64_t>(value));
        return true;
    case semantic::ConversionKind::LongToDouble:
        if (!expectType(value, semantic::PrimitiveType::Long, "numeric conversion")) {
            return false;
        }
        result = static_cast<double>(std::get<runtime::LongValue>(value).value);
        return true;
    case semantic::ConversionKind::Numeric:
        if (runtime::tryConvertNumeric(
                value, targetType, result, checkedArithmetic)) return true;
        return fail(
            runtime::ErrorCode::IntegerOverflow,
            "numeric conversion overflow");
    case semantic::ConversionKind::None:
        break;
    }
    return fail(
        runtime::ErrorCode::InvalidProgram,
        "unsupported AOT conversion");
}

bool ExecutionContext::unary(
    UnaryOperation operation,
    bool checkedArithmetic,
    const runtime::Value& value,
    runtime::Value& result) {
    switch (operation) {
    case UnaryOperation::NegateInt:
        if (!expectType(value, semantic::PrimitiveType::Int, "negation")) {
            return false;
        }
        if (!checkedArithmetic &&
            std::get<std::int64_t>(value) ==
                std::numeric_limits<std::int32_t>::min()) {
            result = std::get<std::int64_t>(value);
            return true;
        }
        return checkedIntResult(*this, -std::get<std::int64_t>(value), result);
    case UnaryOperation::NegateLong: {
        if (!expectType(value, semantic::PrimitiveType::Long, "negation")) {
            return false;
        }
        const auto number = std::get<runtime::LongValue>(value).value;
        if (number == std::numeric_limits<std::int64_t>::min()) {
            if (!checkedArithmetic) {
                result = runtime::LongValue{number};
                return true;
            }
            return fail(
                runtime::ErrorCode::IntegerOverflow,
                "long negation overflow");
        }
        result = runtime::LongValue{-number};
        return true;
    }
    case UnaryOperation::NegateDouble:
        if (runtime::valueType(value) == semantic::PrimitiveType::Float) {
            result = runtime::FloatValue{
                -std::get<runtime::FloatValue>(value).value};
            return true;
        }
        if (!expectType(value, semantic::PrimitiveType::Double, "negation")) {
            return false;
        }
        result = -std::get<double>(value);
        return true;
    case UnaryOperation::LogicalNot:
        if (!expectType(value, semantic::PrimitiveType::Bool, "logical not")) {
            return false;
        }
        result = !std::get<bool>(value);
        return true;
    }
    return fail(runtime::ErrorCode::InvalidProgram, "unsupported AOT unary operation");
}

bool ExecutionContext::binary(
    BinaryOperation operation,
    bool checkedArithmetic,
    const runtime::Value& left,
    const runtime::Value& right,
    runtime::Value& result) {
    if (operation == BinaryOperation::Equal || operation == BinaryOperation::NotEqual) {
        bool equal = false;
        if (runtime::valueType(left) != runtime::valueType(right)) {
            equal = false;
        } else if (runtime::valueType(left) == semantic::PrimitiveType::String) {
            if (std::holds_alternative<runtime::NullString>(left) ||
                std::holds_alternative<runtime::NullString>(right)) {
                equal = std::holds_alternative<runtime::NullString>(left) &&
                    std::holds_alternative<runtime::NullString>(right);
            } else {
                const auto text = [&](const runtime::Value& value)
                    -> std::optional<std::string_view> {
                    if (const auto* direct = std::get_if<std::string>(&value)) {
                        return *direct;
                    }
                    if (const auto* reference =
                            std::get_if<runtime::ObjectRef>(&value)) {
                        return heap_->stringView(*reference);
                    }
                    return std::nullopt;
                };
                const auto a = text(left);
                const auto b = text(right);
                if (!a || !b) {
                    return fail(
                        runtime::ErrorCode::InvalidObjectReference,
                        "string equality encountered an invalid managed reference");
                }
                equal = *a == *b;
            }
        } else {
            if ((runtime::valueType(left) == semantic::PrimitiveType::Object ||
                 runtime::valueType(left) == semantic::PrimitiveType::Array) &&
                ((!std::get_if<runtime::ObjectRef>(&left) ||
                  heap_->isAlive(std::get<runtime::ObjectRef>(left))) == false ||
                 (!std::get_if<runtime::ObjectRef>(&right) ||
                  heap_->isAlive(std::get<runtime::ObjectRef>(right))) == false)) {
                return fail(
                    runtime::ErrorCode::InvalidObjectReference,
                    "reference equality encountered an invalid managed reference");
            }
            equal = left == right;
        }
        result = operation == BinaryOperation::Equal ? equal : !equal;
        return true;
    }

    const auto longOperation =
        operation >= BinaryOperation::AddLong &&
        operation <= BinaryOperation::GreaterOrEqualLong;
    const auto doubleOperation =
        operation >= BinaryOperation::AddDouble &&
        operation <= BinaryOperation::GreaterOrEqualDouble;
    if (longOperation) {
        if (runtime::valueType(left) == semantic::PrimitiveType::ULong) {
            if (!expectType(right, semantic::PrimitiveType::ULong,
                    "binary operation")) return false;
            const auto a = std::get<runtime::ULongValue>(left).value;
            const auto b = std::get<runtime::ULongValue>(right).value;
            switch (operation) {
            case BinaryOperation::AddLong:
                if (checkedArithmetic &&
                    b > std::numeric_limits<std::uint64_t>::max() - a)
                    return fail(runtime::ErrorCode::IntegerOverflow, "ulong addition overflow");
                result = runtime::ULongValue{a + b}; return true;
            case BinaryOperation::SubtractLong:
                if (checkedArithmetic && a < b) return fail(runtime::ErrorCode::IntegerOverflow, "ulong subtraction overflow");
                result = runtime::ULongValue{a - b}; return true;
            case BinaryOperation::MultiplyLong:
                if (checkedArithmetic && b != 0 &&
                    a > std::numeric_limits<std::uint64_t>::max() / b)
                    return fail(runtime::ErrorCode::IntegerOverflow, "ulong multiplication overflow");
                result = runtime::ULongValue{a * b}; return true;
            case BinaryOperation::DivideLong:
                if (b == 0) return fail(runtime::ErrorCode::DivisionByZero, "division by zero");
                result = runtime::ULongValue{a / b}; return true;
            case BinaryOperation::RemainderLong:
                if (b == 0) return fail(runtime::ErrorCode::DivisionByZero, "remainder by zero");
                result = runtime::ULongValue{a % b}; return true;
            case BinaryOperation::LessLong: result = a < b; return true;
            case BinaryOperation::LessOrEqualLong: result = a <= b; return true;
            case BinaryOperation::GreaterLong: result = a > b; return true;
            case BinaryOperation::GreaterOrEqualLong: result = a >= b; return true;
            default: break;
            }
        }
        if (!expectType(left, semantic::PrimitiveType::Long, "binary operation") ||
            !expectType(right, semantic::PrimitiveType::Long, "binary operation")) {
            return false;
        }
        const auto a = std::get<runtime::LongValue>(left).value;
        const auto b = std::get<runtime::LongValue>(right).value;
        switch (operation) {
        case BinaryOperation::AddLong:
            if (!checkedArithmetic) {
                result = runtime::LongValue{signedFromBits(
                    static_cast<std::uint64_t>(a) +
                    static_cast<std::uint64_t>(b))};
                return true;
            }
            return checkedLongAdd(*this, a, b, result);
        case BinaryOperation::SubtractLong:
            if (!checkedArithmetic) {
                result = runtime::LongValue{signedFromBits(
                    static_cast<std::uint64_t>(a) -
                    static_cast<std::uint64_t>(b))};
                return true;
            }
            return checkedLongSubtract(*this, a, b, result);
        case BinaryOperation::MultiplyLong:
            if (!checkedArithmetic) {
                result = runtime::LongValue{signedFromBits(
                    static_cast<std::uint64_t>(a) *
                    static_cast<std::uint64_t>(b))};
                return true;
            }
            return checkedLongMultiply(*this, a, b, result);
        case BinaryOperation::DivideLong:
            if (b == 0) {
                return fail(runtime::ErrorCode::DivisionByZero, "division by zero");
            }
            if (a == std::numeric_limits<std::int64_t>::min() && b == -1) {
                return fail(
                    runtime::ErrorCode::IntegerOverflow,
                    "long division overflow");
            }
            result = runtime::LongValue{a / b};
            return true;
        case BinaryOperation::RemainderLong:
            if (b == 0) {
                return fail(runtime::ErrorCode::DivisionByZero, "remainder by zero");
            }
            result = runtime::LongValue{
                a == std::numeric_limits<std::int64_t>::min() && b == -1
                    ? 0
                    : a % b};
            return true;
        case BinaryOperation::LessLong: result = a < b; return true;
        case BinaryOperation::LessOrEqualLong: result = a <= b; return true;
        case BinaryOperation::GreaterLong: result = a > b; return true;
        case BinaryOperation::GreaterOrEqualLong: result = a >= b; return true;
        default: break;
        }
    }
    if (doubleOperation) {
        if (runtime::valueType(left) == semantic::PrimitiveType::Float) {
            if (!expectType(right, semantic::PrimitiveType::Float,
                    "binary operation")) return false;
            const auto a = std::get<runtime::FloatValue>(left).value;
            const auto b = std::get<runtime::FloatValue>(right).value;
            switch (operation) {
            case BinaryOperation::AddDouble: result = runtime::FloatValue{a + b}; return true;
            case BinaryOperation::SubtractDouble: result = runtime::FloatValue{a - b}; return true;
            case BinaryOperation::MultiplyDouble: result = runtime::FloatValue{a * b}; return true;
            case BinaryOperation::DivideDouble: result = runtime::FloatValue{a / b}; return true;
            case BinaryOperation::LessDouble: result = a < b; return true;
            case BinaryOperation::LessOrEqualDouble: result = a <= b; return true;
            case BinaryOperation::GreaterDouble: result = a > b; return true;
            case BinaryOperation::GreaterOrEqualDouble: result = a >= b; return true;
            default: break;
            }
        }
        if (!expectType(left, semantic::PrimitiveType::Double, "binary operation") ||
            !expectType(right, semantic::PrimitiveType::Double, "binary operation")) {
            return false;
        }
        const auto a = std::get<double>(left);
        const auto b = std::get<double>(right);
        switch (operation) {
        case BinaryOperation::AddDouble: result = a + b; return true;
        case BinaryOperation::SubtractDouble: result = a - b; return true;
        case BinaryOperation::MultiplyDouble: result = a * b; return true;
        case BinaryOperation::DivideDouble: result = a / b; return true;
        case BinaryOperation::LessDouble: result = a < b; return true;
        case BinaryOperation::LessOrEqualDouble: result = a <= b; return true;
        case BinaryOperation::GreaterDouble: result = a > b; return true;
        case BinaryOperation::GreaterOrEqualDouble: result = a >= b; return true;
        default: break;
        }
    }
    if (runtime::valueType(left) == semantic::PrimitiveType::UInt) {
        if (!expectType(right, semantic::PrimitiveType::UInt,
                "binary operation")) return false;
        const auto a = std::get<runtime::UIntValue>(left).value;
        const auto b = std::get<runtime::UIntValue>(right).value;
        switch (operation) {
        case BinaryOperation::AddInt:
            if (checkedArithmetic &&
                b > std::numeric_limits<std::uint32_t>::max() - a)
                return fail(runtime::ErrorCode::IntegerOverflow, "uint addition overflow");
            result = runtime::UIntValue{static_cast<std::uint32_t>(a + b)}; return true;
        case BinaryOperation::SubtractInt:
            if (checkedArithmetic && a < b) return fail(runtime::ErrorCode::IntegerOverflow, "uint subtraction overflow");
            result = runtime::UIntValue{static_cast<std::uint32_t>(a - b)}; return true;
        case BinaryOperation::MultiplyInt:
            if (checkedArithmetic && b != 0 &&
                a > std::numeric_limits<std::uint32_t>::max() / b)
                return fail(runtime::ErrorCode::IntegerOverflow, "uint multiplication overflow");
            result = runtime::UIntValue{static_cast<std::uint32_t>(a * b)}; return true;
        case BinaryOperation::DivideInt:
            if (b == 0) return fail(runtime::ErrorCode::DivisionByZero, "division by zero");
            result = runtime::UIntValue{static_cast<std::uint32_t>(a / b)}; return true;
        case BinaryOperation::RemainderInt:
            if (b == 0) return fail(runtime::ErrorCode::DivisionByZero, "remainder by zero");
            result = runtime::UIntValue{static_cast<std::uint32_t>(a % b)}; return true;
        case BinaryOperation::LessInt: result = a < b; return true;
        case BinaryOperation::LessOrEqualInt: result = a <= b; return true;
        case BinaryOperation::GreaterInt: result = a > b; return true;
        case BinaryOperation::GreaterOrEqualInt: result = a >= b; return true;
        default: break;
        }
    }
    if (!expectType(left, semantic::PrimitiveType::Int, "binary operation") ||
        !expectType(right, semantic::PrimitiveType::Int, "binary operation")) {
        return false;
    }
    const auto a = std::get<std::int64_t>(left);
    const auto b = std::get<std::int64_t>(right);
    switch (operation) {
    case BinaryOperation::AddInt:
        if (!checkedArithmetic) { result = wrapInt32(a + b); return true; }
        return checkedIntResult(*this, a + b, result);
    case BinaryOperation::SubtractInt:
        if (!checkedArithmetic) { result = wrapInt32(a - b); return true; }
        return checkedIntResult(*this, a - b, result);
    case BinaryOperation::MultiplyInt:
        if (!checkedArithmetic) { result = wrapInt32(a * b); return true; }
        return checkedIntResult(*this, a * b, result);
    case BinaryOperation::DivideInt:
        if (b == 0) {
            return fail(runtime::ErrorCode::DivisionByZero, "division by zero");
        }
        if (a == std::numeric_limits<std::int32_t>::min() && b == -1) {
            return fail(
                runtime::ErrorCode::IntegerOverflow,
                "integer division overflow");
        }
        result = a / b;
        return true;
    case BinaryOperation::RemainderInt:
        if (b == 0) {
            return fail(runtime::ErrorCode::DivisionByZero, "remainder by zero");
        }
        result = a == std::numeric_limits<std::int32_t>::min() && b == -1
            ? std::int64_t{0}
            : a % b;
        return true;
    case BinaryOperation::LessInt: result = a < b; return true;
    case BinaryOperation::LessOrEqualInt: result = a <= b; return true;
    case BinaryOperation::GreaterInt: result = a > b; return true;
    case BinaryOperation::GreaterOrEqualInt: result = a >= b; return true;
    default: break;
    }
    return fail(runtime::ErrorCode::InvalidProgram, "unsupported AOT binary operation");
}

const ProgramDescriptor& ExecutionContext::program() const noexcept {
    return *program_;
}

std::shared_ptr<runtime::ManagedHeap> ExecutionContext::heap() const noexcept {
    return heap_;
}

runtime::ShadowStack& ExecutionContext::shadowStack() noexcept {
    return shadowStack_;
}

const runtime::RuntimeError& ExecutionContext::error() const noexcept {
    return error_;
}

const runtime::RuntimeStatistics& ExecutionContext::statistics() const noexcept {
    return statistics_;
}

std::uint64_t ExecutionContext::instructionsExecuted() const noexcept {
    return executed_;
}

bool ExecutionContext::finalizeDeterminism(
    bool succeeded,
    const runtime::Value& value) {
    if (determinismEventsEnabled_) {
        determinism_.mergeEventDigest(
            determinismEventDigest_, determinismEventCount_);
    }
    succeeded = determinism_.finish(succeeded, value, error_, statistics_);
    if (options_.profile) {
        runtime::ExecutionProfile profile;
        profile.totalEvents = profileEvents_;
        profile.functions.reserve(profileFunctions_.size());
        for (auto& [symbolId, function] : profileFunctions_) {
            (void)symbolId;
            profile.functions.push_back(std::move(function));
        }
        options_.profile->merge(profile);
    }
    return succeeded;
}

std::uint64_t ExecutionContext::determinismDigest() const noexcept {
    return determinism_.digest();
}

std::size_t ExecutionContext::replayEntriesConsumed() const noexcept {
    return determinism_.replayEntriesConsumed();
}

Program::Program(const ProgramDescriptor& descriptor)
    : descriptor_(&descriptor),
      heap_(std::make_shared<runtime::ManagedHeap>()) {
    if (descriptor.abiMajor != RuntimeAbiMajor ||
        descriptor.abiMinor > RuntimeAbiMinor) {
        throw std::invalid_argument("AOT program requires an incompatible runtime ABI");
    }
    if (!descriptor.name || descriptor.name[0] == '\0' ||
        descriptor.moduleCount == 0 || !descriptor.moduleNames ||
        descriptor.functionCount == 0 || !descriptor.functions ||
        (descriptor.typeCount != 0 && !descriptor.types) ||
        (descriptor.sourceMapCount != 0 && !descriptor.sourceMap)) {
        throw std::invalid_argument("AOT program descriptor is incomplete");
    }

    std::unordered_set<std::string> moduleNames;
    for (std::uint32_t index = 0; index < descriptor.moduleCount; ++index) {
        const auto* name = descriptor.moduleNames[index];
        if (!name || name[0] == '\0' || !moduleNames.emplace(name).second) {
            throw std::invalid_argument("AOT program contains an invalid module table");
        }
    }

    std::unordered_set<semantic::SymbolId> typeIds;
    for (std::uint32_t index = 0; index < descriptor.typeCount; ++index) {
        const auto& type = descriptor.types[index];
        if (type.id == 0 || !type.moduleName || type.moduleName[0] == '\0' ||
            !type.name || type.name[0] == '\0' ||
            !typeIds.insert(type.id).second ||
            (type.fieldCount != 0 && !type.fields) ||
            (type.enumMemberCount != 0 && !type.enumMembers)) {
            throw std::invalid_argument("AOT program contains an invalid type table");
        }
        if (moduleNames.count(type.moduleName) == 0 ||
            (type.kind != semantic::TypeKind::Class &&
             type.kind != semantic::TypeKind::Struct &&
             type.kind != semantic::TypeKind::Enum) ||
            (type.kind == semantic::TypeKind::Enum && type.fieldCount != 0) ||
            (type.kind != semantic::TypeKind::Enum &&
             type.enumMemberCount != 0)) {
            throw std::invalid_argument("AOT type descriptor kind is inconsistent");
        }
        std::unordered_set<std::string> fieldNames;
        for (std::uint32_t fieldIndex = 0;
             fieldIndex < type.fieldCount;
             ++fieldIndex) {
            const auto& field = type.fields[fieldIndex];
            if (!field.name || field.name[0] == '\0' ||
                field.index != fieldIndex ||
                !validStorageType(field.type) ||
                !validSignatureIdentity(field.type, field.typeId) ||
                !fieldNames.emplace(field.name).second) {
                throw std::invalid_argument("AOT program contains an invalid field table");
            }
        }
        std::unordered_set<std::string> enumNames;
        for (std::uint32_t memberIndex = 0;
             memberIndex < type.enumMemberCount;
             ++memberIndex) {
            const auto& member = type.enumMembers[memberIndex];
            if (!member.name || member.name[0] == '\0' ||
                !enumNames.emplace(member.name).second) {
                throw std::invalid_argument("AOT program contains an invalid enum table");
            }
        }
    }

    for (std::uint32_t index = 0; index < descriptor.typeCount; ++index) {
        const auto& type = descriptor.types[index];
        for (std::uint32_t fieldIndex = 0;
             fieldIndex < type.fieldCount;
             ++fieldIndex) {
            const auto& field = type.fields[fieldIndex];
            if (requiresDescriptor(field.type) &&
                typeIds.count(field.typeId) == 0) {
                throw std::invalid_argument(
                    "AOT field references an unknown exact type");
            }
        }
    }

    std::unordered_set<semantic::SymbolId> functionIds;
    for (std::uint32_t index = 0; index < descriptor.functionCount; ++index) {
        const auto& function = descriptor.functions[index];
        const std::string_view qualifiedName =
            function.name ? std::string_view(function.name) : std::string_view{};
        const auto separator = qualifiedName.find("::");
        const auto moduleName = separator == std::string_view::npos
            ? std::string_view{}
            : qualifiedName.substr(0, separator);
        if (function.symbolId == 0 || !function.name ||
            function.name[0] == '\0' || !function.implementation ||
            moduleName.empty() ||
            moduleNames.count(std::string(moduleName)) == 0 ||
            !functionIds.insert(function.symbolId).second ||
            !validReturnType(function.returnType) ||
            !validSignatureIdentity(function.returnType, function.returnTypeId) ||
            (requiresDescriptor(function.returnType) &&
             typeIds.count(function.returnTypeId) == 0) ||
            (function.parameterCount != 0 &&
             (!function.parameterTypes || !function.parameterTypeIds))) {
            throw std::invalid_argument("AOT program contains an invalid function table");
        }
        // Overloads intentionally share a qualified source name. Calls emitted
        // by the compiler use SymbolId; the name-only embedding convenience API
        // retains the first deterministic descriptor.
        names_.try_emplace(function.name, function.symbolId);
        for (std::uint32_t parameter = 0;
             parameter < function.parameterCount;
             ++parameter) {
            if (!validStorageType(function.parameterTypes[parameter]) ||
                !validSignatureIdentity(
                    function.parameterTypes[parameter],
                    function.parameterTypeIds[parameter]) ||
                (requiresDescriptor(function.parameterTypes[parameter]) &&
                 typeIds.count(function.parameterTypeIds[parameter]) == 0)) {
                throw std::invalid_argument(
                    "AOT program contains an invalid function signature");
            }
        }
    }

    for (std::uint32_t index = 0; index < descriptor.sourceMapCount; ++index) {
        const auto& entry = descriptor.sourceMap[index];
        if (functionIds.count(entry.symbolId) == 0 ||
            entry.generatedLine == 0 || !entry.sourcePath ||
            entry.sourcePath[0] == '\0' || entry.sourceLine == 0 ||
            entry.sourceColumn == 0) {
            throw std::invalid_argument("AOT program contains an invalid source map");
        }
    }
}

void Program::setBindings(
    std::shared_ptr<const runtime::BindingRegistry> bindings) {
    bindings_ = std::move(bindings);
}

void Program::setHeap(std::shared_ptr<runtime::ManagedHeap> heap) {
    heap_ = heap ? std::move(heap) : std::make_shared<runtime::ManagedHeap>();
}

std::shared_ptr<runtime::ManagedHeap> Program::heap() const noexcept {
    return heap_;
}

const ProgramDescriptor& Program::descriptor() const noexcept {
    return *descriptor_;
}

runtime::ExecutionResult Program::invoke(
    semantic::SymbolId symbolId,
    const std::vector<runtime::Value>& arguments,
    runtime::ExecutionOptions options) const {
    ExecutionContext context(*descriptor_, heap_, bindings_, std::move(options));
    runtime::Value value;
    runtime::ExecutionResult execution;
    execution.succeeded = context.invoke(
        symbolId,
        arguments.data(),
        arguments.size(),
        value);
    if (!execution.succeeded && context.hasPendingException()) {
        context.reportUnhandledScriptException();
    }
    execution.succeeded = context.finalizeDeterminism(
        execution.succeeded, value);
    execution.value = std::move(value);
    execution.error = context.error();
    execution.instructionsExecuted = context.instructionsExecuted();
    execution.statistics = context.statistics();
    execution.determinismDigest = context.determinismDigest();
    execution.replayEntriesConsumed = context.replayEntriesConsumed();
    return execution;
}

runtime::ExecutionResult Program::invoke(
    const std::string& qualifiedName,
    const std::vector<runtime::Value>& arguments,
    runtime::ExecutionOptions options) const {
    const auto found = names_.find(qualifiedName);
    if (found == names_.end()) {
        runtime::ExecutionResult missing;
        missing.error.code = runtime::ErrorCode::FunctionNotFound;
        missing.error.message = "AOT entry function was not found";
        return missing;
    }
    return invoke(found->second, arguments, std::move(options));
}

} // namespace realscript::aot
