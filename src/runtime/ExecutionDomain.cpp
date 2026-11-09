#include "realscript/runtime/ExecutionDomain.h"
#include "realscript/aot_cpp/AotRuntime.h"

#include <limits>
#include <string>
#include <utility>

namespace realscript::runtime {
namespace {

void setDomainError(RuntimeError* error, ErrorCode code, std::string message) {
    if (!error) return;
    error->code = code;
    error->message = std::move(message);
}

Value sanitizeCrossHeapValue(const ManagedHeap& sourceHeap, const Value& val) noexcept {
    if (const auto* objRef = std::get_if<ObjectRef>(&val)) {
        if (objRef->kind == ObjectKind::String) {
            if (const auto sv = sourceHeap.stringView(*objRef)) {
                return Value{StringValue(std::string(*sv))};
            }
            return Value{NullString{}};
        }
    }
    return val;
}

} // namespace

ExecutionDomainContext::ExecutionDomainContext()
    : simHeap_(std::make_shared<ManagedHeap>()),
      uiHeap_(std::make_shared<ManagedHeap>()),
      nativeHandles_(std::make_shared<NativeHandleRegistry>()) {}

ExecutionDomainContext::ExecutionDomainContext(
    std::shared_ptr<ManagedHeap> simHeap,
    std::shared_ptr<ManagedHeap> uiHeap,
    std::shared_ptr<NativeHandleRegistry> nativeHandles)
    : simHeap_(simHeap ? std::move(simHeap) : std::make_shared<ManagedHeap>()),
      uiHeap_(uiHeap ? std::move(uiHeap) : std::make_shared<ManagedHeap>()),
      nativeHandles_(nativeHandles ? std::move(nativeHandles) : std::make_shared<NativeHandleRegistry>()) {}

ManagedHeap* ExecutionDomainContext::heapForObject(ObjectRef ref) const noexcept {
    if (!ref.valid()) return nullptr;
    if (uiHeap_ && ref.heapId == static_cast<std::uint32_t>(uiHeap_->heapId())) {
        return uiHeap_.get();
    }
    if (simHeap_ && ref.heapId == static_cast<std::uint32_t>(simHeap_->heapId())) {
        return simHeap_.get();
    }
    return nullptr;
}

std::optional<std::string_view> ExecutionDomainContext::stringViewAcrossDomains(
    ObjectRef ref) const {
    if (const auto* heap = heapForObject(ref)) {
        return heap->stringView(ref);
    }
    return std::nullopt;
}

void ExecutionDomainContext::registerProgramObservableMetadata(const ProgramImage& program) {
    if (simHeap_) simHeap_->registerProgramObservableTypes(program);
    if (uiHeap_) uiHeap_->registerProgramObservableTypes(program);
}

std::optional<Value> ExecutionDomainContext::readCrossHeapFieldFast(
    ObjectRef simObject,
    std::size_t fieldIndex,
    RuntimeError* error) const noexcept {
    if (!isSimObject(simObject)) {
        setDomainError(
            error,
            ErrorCode::InvalidObjectReference,
            "cross-heap read requires a valid SimHeap ObjectRef");
        return std::nullopt;
    }
    const Value* rawVal = simHeap_->fieldGetFast(simObject, fieldIndex);
    if (!rawVal) {
        setDomainError(
            error,
            ErrorCode::InvalidObjectReference,
            "SimHeap object is dead or fieldIndex is out of range");
        return std::nullopt;
    }
    return sanitizeCrossHeapValue(*simHeap_, *rawVal);
}

bool ExecutionDomainContext::readObjectField(
    DomainKind callerDomain,
    ObjectRef target,
    std::size_t fieldIndex,
    Value& outValue,
    RuntimeError* error) const noexcept {
    ManagedHeap* targetHeap = heapForObject(target);
    if (!targetHeap) {
        setDomainError(
            error,
            ErrorCode::InvalidObjectReference,
            "target object does not belong to SimHeap or UiHeap");
        return false;
    }
    const Value* rawVal = targetHeap->fieldGetFast(target, fieldIndex);
    if (!rawVal) {
        setDomainError(
            error,
            ErrorCode::InvalidObjectReference,
            "target object is dead or fieldIndex is out of range");
        return false;
    }
    const bool crossHeapRead =
        (callerDomain == DomainKind::UiPresentation && targetHeap == simHeap_.get()) ||
        (callerDomain == DomainKind::SimDeterministic && targetHeap == uiHeap_.get());
    outValue = crossHeapRead ? sanitizeCrossHeapValue(*targetHeap, *rawVal) : *rawVal;
    return true;
}

bool ExecutionDomainContext::writeObjectField(
    DomainKind callerDomain,
    ObjectRef target,
    std::size_t fieldIndex,
    Value newValue,
    RuntimeError* error) {
    ManagedHeap* targetHeap = heapForObject(target);
    if (!targetHeap) {
        setDomainError(
            error,
            ErrorCode::InvalidObjectReference,
            "target object does not belong to SimHeap or UiHeap");
        return false;
    }
    if (callerDomain == DomainKind::UiPresentation && targetHeap == simHeap_.get()) {
        setDomainError(
            error,
            ErrorCode::CapabilityDenied,
            "CrossHeapWriteViolation: UI presentation domain cannot mutate SimDeterministic heap object directly");
        return false;
    }
    if (callerDomain == DomainKind::SimDeterministic && targetHeap == uiHeap_.get()) {
        setDomainError(
            error,
            ErrorCode::CapabilityDenied,
            "CrossHeapWriteViolation: SimDeterministic domain cannot mutate UiHeap object directly");
        return false;
    }
    if (const auto* valRef = std::get_if<ObjectRef>(&newValue)) {
        if (targetHeap == simHeap_.get() && isUiObject(*valRef)) {
            setDomainError(
                error,
                ErrorCode::CapabilityDenied,
                "CrossHeapReferenceViolation: SimDeterministic heap object cannot hold a reference to a UiHeap object");
            return false;
        }
        if (valRef->kind == ObjectKind::String &&
            valRef->heapId != static_cast<std::uint32_t>(targetHeap->heapId())) {
            if (const auto sv = stringViewAcrossDomains(*valRef)) {
                const auto interned = targetHeap->internString(*sv, error);
                if (!interned) return false;
                newValue = Value{*interned};
            } else {
                setDomainError(
                    error,
                    ErrorCode::InvalidObjectReference,
                    "cross-heap string reference is invalid or collected");
                return false;
            }
        }
    }
    if (const auto* sv = std::get_if<StringValue>(&newValue)) {
        const auto interned = targetHeap->internString(sv->str(), error);
        if (!interned) return false;
        newValue = Value{*interned};
    }
    return targetHeap->fieldSet(target, fieldIndex, std::move(newValue), error);
}

std::optional<PropertySlotHandle> ExecutionDomainContext::resolvePropertySlot(
    const ProgramImage& program,
    semantic::SymbolId typeId,
    std::string_view memberName) const noexcept {
    const auto* type = program.resolveType(typeId);
    if (!type) return std::nullopt;

    for (const auto& prop : type->properties) {
        if (prop.name == memberName) {
            PropertySlotHandle handle;
            handle.typeId = type->id;
            handle.isObservable = prop.observable;
            handle.observableBitIndex = static_cast<std::uint8_t>(
                prop.observableBitIndex < 64u ? prop.observableBitIndex : 0xffu);
            handle.isReadOnly = !prop.setter.has_value();
            handle.isSimState = prop.simState || type->simState;
            handle.isSimCommand = prop.setter.has_value() && prop.setter->simCommand;
            if (prop.getter) handle.getterSymbolId = prop.getter->id;
            if (prop.setter) handle.setterSymbolId = prop.setter->id;
            if (prop.backingFieldIndex != std::numeric_limits<std::size_t>::max() &&
                prop.backingFieldIndex < type->fields.size()) {
                handle.backingFieldIndex = static_cast<std::uint32_t>(prop.backingFieldIndex);
                handle.isAutoProperty = true;
                const auto& backing = type->fields[prop.backingFieldIndex];
                if (backing.observable && backing.observableBitIndex < 64u) {
                    handle.isObservable = true;
                    handle.observableBitIndex = static_cast<std::uint8_t>(backing.observableBitIndex);
                }
            }
            return handle;
        }
    }

    for (const auto& field : type->fields) {
        if (field.name == memberName) {
            PropertySlotHandle handle;
            handle.typeId = type->id;
            handle.backingFieldIndex = static_cast<std::uint32_t>(field.index);
            handle.isAutoProperty = true;
            handle.isObservable = field.observable;
            handle.observableBitIndex = static_cast<std::uint8_t>(
                field.observableBitIndex < 64u ? field.observableBitIndex : 0xffu);
            handle.isReadOnly = field.readonlyField || field.constField;
            handle.isSimState = field.simState || type->simState;
            return handle;
        }
    }

    return std::nullopt;
}

std::optional<PropertySlotHandle> ExecutionDomainContext::resolvePropertySlot(
    const ProgramImage& program,
    std::string_view canonicalTypeName,
    std::string_view memberName) const noexcept {
    return resolvePropertySlot(
        program,
        semantic::stableTypeId(std::string(canonicalTypeName)),
        memberName);
}

std::optional<Value> ExecutionDomainContext::readPropertySlotFast(
    DomainKind callerDomain,
    ObjectRef target,
    const PropertySlotHandle& slot,
    const Interpreter* interpreter,
    RuntimeError* error) const {
    if (!slot.valid()) {
        setDomainError(error, ErrorCode::InvalidArguments, "PropertySlotHandle is invalid");
        return std::nullopt;
    }
    ManagedHeap* targetHeap = heapForObject(target);
    if (!targetHeap) {
        setDomainError(error, ErrorCode::InvalidObjectReference, "target object does not belong to SimHeap or UiHeap");
        return std::nullopt;
    }
    const bool crossHeapRead =
        (callerDomain == DomainKind::UiPresentation && targetHeap == simHeap_.get()) ||
        (callerDomain == DomainKind::SimDeterministic && targetHeap == uiHeap_.get());

    if (slot.isAutoProperty && slot.backingFieldIndex != PropertySlotHandle::InvalidIndex) {
        const Value* val = targetHeap->fieldGetFast(target, slot.backingFieldIndex);
        if (!val) {
            setDomainError(error, ErrorCode::InvalidObjectReference, "target object is dead or slot index is out of range");
            return std::nullopt;
        }
        return crossHeapRead ? sanitizeCrossHeapValue(*targetHeap, *val) : *val;
    }

    if (slot.getterSymbolId != 0 && interpreter) {
        const auto exec = interpreter->invoke(slot.getterSymbolId, {Value{target}});
        if (!exec.succeeded) {
            if (error) *error = exec.error;
            return std::nullopt;
        }
        return crossHeapRead ? sanitizeCrossHeapValue(*targetHeap, exec.value) : exec.value;
    }

    setDomainError(error, ErrorCode::FunctionNotFound, "property slot has no accessible backing field or getter");
    return std::nullopt;
}

bool ExecutionDomainContext::readPropertySlotFast(
    DomainKind callerDomain,
    ObjectRef target,
    const PropertySlotHandle& slot,
    Value& outValue,
    RuntimeError* error,
    const Interpreter* interpreter) const {
    auto res = readPropertySlotFast(callerDomain, target, slot, interpreter, error);
    if (!res) return false;
    outValue = std::move(*res);
    return true;
}

bool ExecutionDomainContext::writePropertySlotFast(
    DomainKind callerDomain,
    ObjectRef target,
    const PropertySlotHandle& slot,
    const Value& value,
    const Interpreter* interpreter,
    RuntimeError* error) {
    if (!slot.valid()) {
        setDomainError(error, ErrorCode::InvalidArguments, "PropertySlotHandle is invalid");
        return false;
    }
    ManagedHeap* targetHeap = heapForObject(target);
    if (!targetHeap) {
        setDomainError(error, ErrorCode::InvalidObjectReference, "target object does not belong to SimHeap or UiHeap");
        return false;
    }
    if (callerDomain == DomainKind::UiPresentation && slot.isSimCommand && slot.setterSymbolId != 0) {
        SimCommandEnvelope cmd;
        cmd.commandTypeId = slot.setterSymbolId;
        cmd.targetObject = target;
        cmd.payload.push_back(value);
        enqueueSimCommand(std::move(cmd));
        return true;
    }
    if (callerDomain == DomainKind::UiPresentation &&
        (targetHeap == simHeap_.get() || slot.isSimState)) {
        setDomainError(
            error,
            ErrorCode::CapabilityDenied,
            "CrossHeapWriteViolation: UI presentation domain cannot mutate SimDeterministic heap or [SimState] property directly; dispatch a SimCommandEnvelope instead");
        return false;
    }
    if (callerDomain == DomainKind::SimDeterministic && targetHeap == uiHeap_.get()) {
        setDomainError(
            error,
            ErrorCode::CapabilityDenied,
            "CrossHeapWriteViolation: SimDeterministic domain cannot mutate UiHeap object directly");
        return false;
    }
    if (slot.isReadOnly) {
        setDomainError(error, ErrorCode::CapabilityDenied, "cannot write to a read-only property or field slot");
        return false;
    }

    Value writeVal = value;
    if (const auto* valRef = std::get_if<ObjectRef>(&writeVal)) {
        if (targetHeap == simHeap_.get() && isUiObject(*valRef)) {
            setDomainError(
                error,
                ErrorCode::CapabilityDenied,
                "CrossHeapReferenceViolation: SimDeterministic heap object cannot hold a reference to a UiHeap object");
            return false;
        }
        if (valRef->kind == ObjectKind::String &&
            valRef->heapId != static_cast<std::uint32_t>(targetHeap->heapId())) {
            if (const auto sv = stringViewAcrossDomains(*valRef)) {
                const auto interned = targetHeap->internString(*sv, error);
                if (!interned) return false;
                writeVal = Value{*interned};
            } else {
                setDomainError(
                    error,
                    ErrorCode::InvalidObjectReference,
                    "cross-heap string reference is invalid or collected");
                return false;
            }
        }
    }
    if (const auto* sv = std::get_if<StringValue>(&writeVal)) {
        const auto interned = targetHeap->internString(sv->str(), error);
        if (!interned) return false;
        writeVal = Value{*interned};
    }

    if (slot.isAutoProperty && slot.backingFieldIndex != PropertySlotHandle::InvalidIndex) {
        return targetHeap->fieldSet(target, slot.backingFieldIndex, std::move(writeVal), error);
    }

    if (slot.setterSymbolId != 0 && interpreter) {
        const auto exec = interpreter->invoke(slot.setterSymbolId, {Value{target}, std::move(writeVal)});
        if (!exec.succeeded) {
            if (error) *error = exec.error;
            return false;
        }
        return true;
    }

    setDomainError(error, ErrorCode::FunctionNotFound, "property slot has no accessible backing field or setter");
    return false;
}

bool ExecutionDomainContext::writePropertySlotFast(
    DomainKind callerDomain,
    ObjectRef target,
    const PropertySlotHandle& slot,
    const Value& value,
    RuntimeError* error) {
    return writePropertySlotFast(callerDomain, target, slot, value, nullptr, error);
}

void ExecutionDomainContext::enqueueSimCommand(SimCommandEnvelope command) {
    pendingSimCommands_.push_back(std::move(command));
}

std::vector<SimCommandEnvelope> ExecutionDomainContext::consumeSimCommands() {
    std::vector<SimCommandEnvelope> out;
    out.swap(pendingSimCommands_);
    return out;
}

std::size_t ExecutionDomainContext::flushSimCommands(
    const Interpreter& simInterpreter,
    RuntimeError* error) {
    auto commands = consumeSimCommands();
    std::size_t executedCount = 0;
    for (auto& cmd : commands) {
        std::vector<Value> args;
        args.reserve(cmd.payload.size() + (cmd.targetObject.valid() ? 1u : 0u));
        if (cmd.targetObject.valid()) {
            args.push_back(Value{cmd.targetObject});
        }
        for (auto& val : cmd.payload) {
            if (simHeap_) {
                if (const auto* strRef = std::get_if<ObjectRef>(&val)) {
                    if (strRef->kind == ObjectKind::String &&
                        strRef->heapId != static_cast<std::uint32_t>(simHeap_->heapId())) {
                        if (const auto sv = stringViewAcrossDomains(*strRef)) {
                            RuntimeError strErr;
                            if (const auto interned = simHeap_->internString(*sv, &strErr)) {
                                args.push_back(Value{*interned});
                                continue;
                            }
                        }
                    }
                } else if (const auto* sv = std::get_if<StringValue>(&val)) {
                    RuntimeError strErr;
                    if (const auto interned = simHeap_->internString(sv->str(), &strErr)) {
                        args.push_back(Value{*interned});
                        continue;
                    }
                }
            }
            args.push_back(std::move(val));
        }
        const auto exec = simInterpreter.invoke(cmd.commandTypeId, args);
        if (!exec.succeeded) {
            if (error && error->code == ErrorCode::None) {
                *error = exec.error;
            }
            continue;
        }
        ++executedCount;
    }
    return executedCount;
}

std::size_t ExecutionDomainContext::flushSimCommands(
    aot::ExecutionContext& simAotContext,
    RuntimeError* error) {
    auto commands = consumeSimCommands();
    std::size_t executedCount = 0;
    for (auto& cmd : commands) {
        std::vector<Value> args;
        args.reserve(cmd.payload.size() + (cmd.targetObject.valid() ? 1u : 0u));
        if (cmd.targetObject.valid()) {
            args.push_back(Value{cmd.targetObject});
        }
        for (auto& val : cmd.payload) {
            if (simHeap_) {
                if (const auto* strRef = std::get_if<ObjectRef>(&val)) {
                    if (strRef->kind == ObjectKind::String &&
                        strRef->heapId != static_cast<std::uint32_t>(simHeap_->heapId())) {
                        if (const auto sv = stringViewAcrossDomains(*strRef)) {
                            RuntimeError strErr;
                            if (const auto interned = simHeap_->internString(*sv, &strErr)) {
                                args.push_back(Value{*interned});
                                continue;
                            }
                        }
                    }
                } else if (const auto* sv = std::get_if<StringValue>(&val)) {
                    RuntimeError strErr;
                    if (const auto interned = simHeap_->internString(sv->str(), &strErr)) {
                        args.push_back(Value{*interned});
                        continue;
                    }
                }
            }
            args.push_back(std::move(val));
        }
        Value outResult{};
        if (!simAotContext.invoke(cmd.commandTypeId, args.data(), args.size(), outResult)) {
            if (error && error->code == ErrorCode::None) {
                *error = simAotContext.error();
            }
            continue;
        }
        ++executedCount;
    }
    return executedCount;
}

UiTaskId ExecutionDomainContext::spawnReentrantSequenceTask(
    const ProgramImage& program,
    const Interpreter& uiInterpreter,
    ObjectRef targetObject,
    std::string_view sequenceName,
    std::vector<Value> arguments,
    NativeHandle boundControl,
    UiTaskId awaitPrerequisite) {
    auto frame = std::make_unique<UiAsyncTaskFrame>();
    frame->taskId = nextUiTaskId_++;
    frame->prerequisiteTaskId = awaitPrerequisite;
    frame->targetWeakRef = WeakRef(targetObject);
    frame->boundControl = boundControl;
    frame->isSequenceTask = true;

    ManagedHeap* targetHeap = heapForObject(targetObject);
    if (!targetHeap || !targetHeap->isAlive(targetObject)) {
        frame->status = UiTaskStatus::Faulted;
        const UiTaskId id = frame->taskId;
        uiTasks_.push_back(std::move(frame));
        return id;
    }
    if (boundControl.valid() &&
        (cancelledControlSlots_.count(boundControl.slot) != 0 ||
         (nativeHandles_ && nativeHandles_->resolveRawFast(boundControl) == nullptr))) {
        frame->status = UiTaskStatus::Cancelled;
        const UiTaskId id = frame->taskId;
        uiTasks_.push_back(std::move(frame));
        return id;
    }

    const auto typeIdOpt = targetHeap->objectTypeId(targetObject);
    const auto* typeDesc = typeIdOpt ? program.resolveType(*typeIdOpt) : nullptr;
    if (!typeDesc) {
        frame->status = UiTaskStatus::Faulted;
        const UiTaskId id = frame->taskId;
        uiTasks_.push_back(std::move(frame));
        return id;
    }

    const std::string seqStr(sequenceName);
    const std::string stateName = "$sequence_state_" + seqStr;
    const std::string completedName = "$sequence_completed_" + seqStr;
    const std::string resultName = "$sequence_result_" + seqStr;
    const std::string targetName = "$sequence_target_" + seqStr;
    const std::string timerName = "$sequence_timer_" + seqStr;
    const std::string paramPrefix = "$sequence_parameter_" + seqStr + "_";
    const std::string localPrefix = "$sequence_local_" + seqStr + "_";
    const std::string foreachPrefix = "$sequence_foreach_" + seqStr + "_";

    std::vector<std::pair<std::size_t, Value>> backupFields;
    for (const auto& field : typeDesc->fields) {
        if (!field.synthetic) continue;
        const bool belongsToSequence =
            field.name == stateName ||
            field.name == completedName ||
            field.name == resultName ||
            field.name == targetName ||
            field.name == timerName ||
            field.name.rfind(paramPrefix, 0) == 0 ||
            field.name.rfind(localPrefix, 0) == 0 ||
            field.name.rfind(foreachPrefix, 0) == 0;
        if (!belongsToSequence) continue;

        if (field.name == stateName) frame->stateFieldIndex = field.index;
        if (field.name == completedName) frame->completedFieldIndex = field.index;
        if (field.name == resultName) frame->resultFieldIndex = field.index;

        if (const Value* cur = targetHeap->fieldGetFast(targetObject, field.index)) {
            backupFields.emplace_back(field.index, *cur);
        }
    }

    (void)uiInterpreter;
    semantic::SymbolId entrySymbolId = 0;
    std::size_t expectedArgsAfterThis = 1;
    const std::string stepMethodName = "$sequence_" + seqStr + "_1";
    for (const auto& method : typeDesc->methods) {
        if (method.name == seqStr) {
            entrySymbolId = method.id;
            if (!method.parameters.empty()) {
                expectedArgsAfterThis = method.parameters.size() - 1u;
            }
        } else if (method.name == stepMethodName) {
            frame->sequenceStepSymbolId = method.id;
        }
    }
    if (frame->sequenceStepSymbolId == 0) {
        const std::string qualStep = semantic::canonicalTypeName(*typeDesc) + "." + stepMethodName;
        if (const auto foundStep = program.findFunction(qualStep)) {
            frame->sequenceStepSymbolId = *foundStep;
        }
    }
    if (entrySymbolId == 0) {
        const std::string qualEntry = semantic::canonicalTypeName(*typeDesc) + "." + seqStr;
        if (const auto foundEntry = program.findFunction(qualEntry)) {
            entrySymbolId = *foundEntry;
        }
    }
    if (entrySymbolId == 0) {
        frame->status = UiTaskStatus::Faulted;
        const UiTaskId id = frame->taskId;
        uiTasks_.push_back(std::move(frame));
        return id;
    }

    std::vector<Value> callArgs;
    callArgs.reserve(arguments.size() + 2u);
    callArgs.push_back(Value{targetObject});
    frame->sequenceTargetId = static_cast<std::int64_t>(frame->taskId);
    const bool omittedTargetParam = (arguments.size() + 1u == expectedArgsAfterThis) ||
        arguments.empty() ||
        (!std::holds_alternative<LongValue>(arguments[0]) &&
         !std::holds_alternative<std::int64_t>(arguments[0]));
    if (omittedTargetParam) {
        callArgs.push_back(Value{LongValue{frame->sequenceTargetId}});
        for (auto& arg : arguments) {
            callArgs.push_back(std::move(arg));
        }
    } else {
        if (const auto* lv = std::get_if<LongValue>(&arguments[0])) {
            frame->sequenceTargetId = lv->value;
        } else if (const auto* iv = std::get_if<std::int64_t>(&arguments[0])) {
            frame->sequenceTargetId = *iv;
            arguments[0] = Value{LongValue{*iv}};
        }
        for (auto& arg : arguments) {
            callArgs.push_back(std::move(arg));
        }
    }

    frame->sequenceEntrySymbolId = entrySymbolId;
    frame->sequenceEntryArgs = std::move(callArgs);
    frame->isolatedSequenceFields.reserve(backupFields.size());
    for (const auto& [idx, oldVal] : backupFields) {
        if (idx == frame->stateFieldIndex) {
            frame->isolatedSequenceFields.emplace_back(idx, Value{std::int64_t{0}});
        } else if (idx == frame->completedFieldIndex) {
            frame->isolatedSequenceFields.emplace_back(idx, Value{false});
        } else {
            frame->isolatedSequenceFields.emplace_back(idx, oldVal);
        }
    }
    frame->status = UiTaskStatus::Suspended;

    const UiTaskId id = frame->taskId;
    uiTasks_.push_back(std::move(frame));
    return id;
}

UiTaskId ExecutionDomainContext::spawnCallbackTask(
    UiAsyncStepCallback stepCallback,
    NativeHandle boundControl,
    UiTaskId awaitPrerequisite) {
    auto frame = std::make_unique<UiAsyncTaskFrame>();
    frame->taskId = nextUiTaskId_++;
    frame->prerequisiteTaskId = awaitPrerequisite;
    frame->boundControl = boundControl;
    frame->isSequenceTask = false;
    frame->stepCallback = std::move(stepCallback);
    if (boundControl.valid() &&
        (cancelledControlSlots_.count(boundControl.slot) != 0 ||
         (nativeHandles_ && nativeHandles_->resolveRawFast(boundControl) == nullptr))) {
        frame->status = UiTaskStatus::Cancelled;
    } else {
        frame->status = UiTaskStatus::Suspended;
    }
    const UiTaskId id = frame->taskId;
    uiTasks_.push_back(std::move(frame));
    return id;
}

bool ExecutionDomainContext::stepUiTask(UiTaskId taskId, const Interpreter* uiInterpreter) {
    UiAsyncTaskFrame* frame = nullptr;
    for (const auto& item : uiTasks_) {
        if (item->taskId == taskId) {
            frame = item.get();
            break;
        }
    }
    if (!frame) return false;
    if (frame->status == UiTaskStatus::Completed ||
        frame->status == UiTaskStatus::Cancelled ||
        frame->status == UiTaskStatus::Faulted) {
        return false;
    }

    if (frame->boundControl.valid()) {
        if (cancelledControlSlots_.count(frame->boundControl.slot) != 0 ||
            (nativeHandles_ && nativeHandles_->resolveRawFast(frame->boundControl) == nullptr)) {
            frame->status = UiTaskStatus::Cancelled;
            return false;
        }
    }

    if (frame->prerequisiteTaskId != 0) {
        const auto prereqStatus = uiTaskStatus(frame->prerequisiteTaskId);
        if (prereqStatus == UiTaskStatus::Cancelled || prereqStatus == UiTaskStatus::Faulted) {
            frame->status = UiTaskStatus::Cancelled;
            return false;
        }
        if (prereqStatus != UiTaskStatus::Completed) {
            return true; // Still waiting on prerequisite
        }
    }

    if (!frame->isSequenceTask) {
        if (!frame->stepCallback) {
            frame->status = UiTaskStatus::Completed;
            return false;
        }
        const bool finished = frame->stepCallback(frame->stepIndex++, frame->result);
        if (frame->boundControl.valid() &&
            (cancelledControlSlots_.count(frame->boundControl.slot) != 0 ||
             (nativeHandles_ && nativeHandles_->resolveRawFast(frame->boundControl) == nullptr))) {
            frame->status = UiTaskStatus::Cancelled;
            return false;
        }
        if (finished) {
            frame->status = UiTaskStatus::Completed;
            return false;
        }
        frame->status = UiTaskStatus::Suspended;
        return true;
    }

    if (!uiInterpreter) return false;
    const ObjectRef targetObject = frame->targetWeakRef.ref;
    ManagedHeap* targetHeap = heapForObject(targetObject);
    if (!targetHeap || !targetHeap->isAlive(targetObject)) {
        frame->status = UiTaskStatus::Cancelled;
        return false;
    }

    // Swap isolated sequence fields into targetObject
    std::vector<std::pair<std::size_t, Value>> backupFields;
    backupFields.reserve(frame->isolatedSequenceFields.size());
    for (const auto& [idx, taskVal] : frame->isolatedSequenceFields) {
        if (const Value* cur = targetHeap->fieldGetFast(targetObject, idx)) {
            backupFields.emplace_back(idx, *cur);
        }
        if (idx == frame->stateFieldIndex &&
            std::holds_alternative<std::int64_t>(taskVal) &&
            std::get<std::int64_t>(taskVal) < 0) {
            // Negative state means yielded for N frames/steps; advance to positive state on resume
            (void)targetHeap->fieldSetFast(
                targetObject, idx, Value{-std::get<std::int64_t>(taskVal)});
        } else {
            (void)targetHeap->fieldSetFast(targetObject, idx, taskVal);
        }
    }

    ExecutionResult exec{};
    if (frame->stepIndex == 0 && frame->sequenceEntrySymbolId != 0) {
        if (!frame->sequenceEntryArgs.empty()) {
            frame->sequenceEntryArgs[0] = Value{targetObject};
        }
        exec = uiInterpreter->invoke(frame->sequenceEntrySymbolId, frame->sequenceEntryArgs);
    } else {
        exec = uiInterpreter->invoke(frame->sequenceStepSymbolId, {Value{targetObject}});
    }
    ++frame->stepIndex;

    for (auto& [idx, taskVal] : frame->isolatedSequenceFields) {
        if (const Value* updated = targetHeap->fieldGetFast(targetObject, idx)) {
            taskVal = *updated;
        }
    }
    for (const auto& [idx, oldVal] : backupFields) {
        (void)targetHeap->fieldSetFast(targetObject, idx, oldVal);
    }

    if (!exec.succeeded) {
        frame->status = UiTaskStatus::Faulted;
        frame->error = exec.error;
        return false;
    }

    bool completed = false;
    std::int64_t stateVal = 0;
    for (const auto& [idx, val] : frame->isolatedSequenceFields) {
        if (idx == frame->completedFieldIndex && std::holds_alternative<bool>(val)) {
            completed = std::get<bool>(val);
        } else if (idx == frame->stateFieldIndex && std::holds_alternative<std::int64_t>(val)) {
            stateVal = std::get<std::int64_t>(val);
        } else if (idx == frame->resultFieldIndex) {
            frame->result = val;
        }
    }
    if (completed || stateVal == 0) {
        frame->status = UiTaskStatus::Completed;
        return false;
    }
    frame->status = UiTaskStatus::Suspended;
    return true;
}

std::size_t ExecutionDomainContext::stepAllUiTasks(const Interpreter* uiInterpreter) {
    std::size_t steppedCount = 0;
    const std::size_t count = uiTasks_.size();
    for (std::size_t i = 0; i < count; ++i) {
        const UiTaskId id = uiTasks_[i]->taskId;
        const auto beforeStatus = uiTasks_[i]->status;
        if (beforeStatus == UiTaskStatus::Suspended || beforeStatus == UiTaskStatus::Pending) {
            (void)stepUiTask(id, uiInterpreter);
            const auto afterStatus = uiTasks_[i]->status;
            if (afterStatus != UiTaskStatus::Cancelled && afterStatus != UiTaskStatus::Faulted) {
                ++steppedCount;
            }
        }
    }
    return steppedCount;
}

bool ExecutionDomainContext::cancelUiTask(UiTaskId taskId) noexcept {
    for (const auto& item : uiTasks_) {
        if (item->taskId == taskId) {
            if (item->status != UiTaskStatus::Completed) {
                item->status = UiTaskStatus::Cancelled;
                return true;
            }
            return false;
        }
    }
    return false;
}

std::size_t ExecutionDomainContext::cancelUiTasksForControl(NativeHandle boundControl) noexcept {
    if (!boundControl.valid()) return 0;
    cancelledControlSlots_.insert(boundControl.slot);
    std::size_t cancelledCount = 0;
    for (const auto& item : uiTasks_) {
        if (item->boundControl.valid() &&
            item->boundControl.slot == boundControl.slot &&
            item->boundControl.generation == boundControl.generation &&
            item->status != UiTaskStatus::Completed &&
            item->status != UiTaskStatus::Cancelled) {
            item->status = UiTaskStatus::Cancelled;
            ++cancelledCount;
        }
    }
    return cancelledCount;
}

UiTaskStatus ExecutionDomainContext::uiTaskStatus(UiTaskId taskId) const noexcept {
    for (const auto& item : uiTasks_) {
        if (item->taskId == taskId) {
            return item->status;
        }
    }
    return UiTaskStatus::Faulted;
}

std::optional<Value> ExecutionDomainContext::uiTaskResult(UiTaskId taskId) const {
    for (const auto& item : uiTasks_) {
        if (item->taskId == taskId && item->status == UiTaskStatus::Completed) {
            return item->result;
        }
    }
    return std::nullopt;
}

std::size_t ExecutionDomainContext::activeUiTaskCount() const noexcept {
    std::size_t active = 0;
    for (const auto& item : uiTasks_) {
        if (item->status == UiTaskStatus::Pending || item->status == UiTaskStatus::Suspended) {
            ++active;
        }
    }
    return active;
}

std::size_t ExecutionDomainContext::stepUiIdleGc(
    const ShadowStack& shadowStack,
    double maxMilliseconds) {
    if (!uiHeap_) return 0;
    return uiHeap_->stepIdleForDuration(shadowStack, maxMilliseconds);
}

std::size_t ExecutionDomainContext::stepUiIdleGc(double maxMilliseconds) {
    ShadowStack emptyStack;
    return stepUiIdleGc(emptyStack, maxMilliseconds);
}

} // namespace realscript::runtime
