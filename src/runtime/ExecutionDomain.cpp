#include "realscript/runtime/ExecutionDomain.h"

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
    if (callerDomain == DomainKind::UiPresentation && targetHeap == simHeap_.get()) {
        setDomainError(
            error,
            ErrorCode::CapabilityDenied,
            "CrossHeapWriteViolation: UI presentation domain cannot mutate SimDeterministic heap object directly; dispatch a SimCommandEnvelope instead");
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
    if (const auto* sv = std::get_if<StringValue>(&value)) {
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
