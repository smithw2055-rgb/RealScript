#pragma once

#include "realscript/runtime/Runtime.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace realscript::runtime {

enum class DomainKind : std::uint8_t {
    SimDeterministic = 0,
    UiPresentation = 1,
    Sim = SimDeterministic,
    Ui = UiPresentation,
};

struct PropertySlotHandle {
    static constexpr std::uint32_t InvalidIndex = 0xffffffffu;

    semantic::SymbolId typeId = 0;
    std::uint32_t backingFieldIndex = InvalidIndex;
    semantic::SymbolId getterSymbolId = 0;
    semantic::SymbolId setterSymbolId = 0;
    std::uint8_t observableBitIndex = 0xffu;
    bool isAutoProperty = false;
    bool isObservable = false;
    bool isReadOnly = false;
    bool isSimState = false;
    bool isSimCommand = false;

    [[nodiscard]] constexpr bool valid() const noexcept {
        return typeId != 0 &&
            (backingFieldIndex != InvalidIndex || getterSymbolId != 0 || setterSymbolId != 0);
    }
    [[nodiscard]] constexpr bool isAutoBacked() const noexcept {
        return isAutoProperty && backingFieldIndex != InvalidIndex;
    }
};

struct SimCommandEnvelope {
    semantic::SymbolId commandTypeId = 0;
    ObjectRef targetObject{};
    std::vector<Value> payload;
    std::uint64_t clientFrameTick = 0;
};

using UiTaskId = std::uint64_t;

enum class UiTaskStatus : std::uint8_t {
    Pending = 0,
    Suspended = 1,
    Completed = 2,
    Cancelled = 3,
    Faulted = 4,
};

using UiAsyncStepCallback = std::function<bool(std::uint32_t stepIndex, Value& inOutResult)>;

class ExecutionDomainContext {
public:
    ExecutionDomainContext();
    ExecutionDomainContext(
        std::shared_ptr<ManagedHeap> simHeap,
        std::shared_ptr<ManagedHeap> uiHeap,
        std::shared_ptr<NativeHandleRegistry> nativeHandles = nullptr);

    [[nodiscard]] ManagedHeap& simHeap() noexcept { return *simHeap_; }
    [[nodiscard]] const ManagedHeap& simHeap() const noexcept { return *simHeap_; }
    [[nodiscard]] std::shared_ptr<ManagedHeap> simHeapPtr() const noexcept { return simHeap_; }

    [[nodiscard]] ManagedHeap& uiHeap() noexcept { return *uiHeap_; }
    [[nodiscard]] const ManagedHeap& uiHeap() const noexcept { return *uiHeap_; }
    [[nodiscard]] std::shared_ptr<ManagedHeap> uiHeapPtr() const noexcept { return uiHeap_; }

    [[nodiscard]] NativeHandleRegistry& nativeHandles() noexcept { return *nativeHandles_; }
    [[nodiscard]] const NativeHandleRegistry& nativeHandles() const noexcept { return *nativeHandles_; }
    [[nodiscard]] std::shared_ptr<NativeHandleRegistry> nativeHandlesPtr() const noexcept { return nativeHandles_; }

    [[nodiscard]] DomainKind activeDomain() const noexcept { return activeDomain_; }
    void setActiveDomain(DomainKind domain) noexcept { activeDomain_ = domain; }

    [[nodiscard]] bool isSimObject(ObjectRef ref) const noexcept {
        return ref.valid() && simHeap_ &&
            ref.heapId == static_cast<std::uint32_t>(simHeap_->heapId());
    }
    [[nodiscard]] bool isUiObject(ObjectRef ref) const noexcept {
        return ref.valid() && uiHeap_ &&
            ref.heapId == static_cast<std::uint32_t>(uiHeap_->heapId());
    }

    [[nodiscard]] ManagedHeap* heapForObject(ObjectRef ref) const noexcept;
    [[nodiscard]] std::optional<std::string_view> stringViewAcrossDomains(ObjectRef ref) const;

    void registerProgramObservableMetadata(const ProgramImage& program);

    // Cross-heap / same-heap field access with Cross-Heap Read-Only enforcement
    [[nodiscard]] std::optional<Value> readCrossHeapFieldFast(
        ObjectRef simObject,
        std::size_t fieldIndex,
        RuntimeError* error = nullptr) const noexcept;

    bool readObjectField(
        DomainKind callerDomain,
        ObjectRef target,
        std::size_t fieldIndex,
        Value& outValue,
        RuntimeError* error = nullptr) const noexcept;

    bool writeObjectField(
        DomainKind callerDomain,
        ObjectRef target,
        std::size_t fieldIndex,
        Value newValue,
        RuntimeError* error = nullptr);

    // Resolve compile-time/link-time property or field into an O(1) PropertySlotHandle.
    [[nodiscard]] std::optional<PropertySlotHandle> resolvePropertySlot(
        const ProgramImage& program,
        semantic::SymbolId typeId,
        std::string_view memberName) const noexcept;

    [[nodiscard]] std::optional<PropertySlotHandle> resolvePropertySlot(
        const ProgramImage& program,
        std::string_view canonicalTypeName,
        std::string_view memberName) const noexcept;

    // O(1) property/field read across UiHeap and SimHeap (with cross-heap StringValue conversion).
    [[nodiscard]] std::optional<Value> readPropertySlotFast(
        DomainKind callerDomain,
        ObjectRef target,
        const PropertySlotHandle& slot,
        const Interpreter* interpreter = nullptr,
        RuntimeError* error = nullptr) const;

    bool readPropertySlotFast(
        DomainKind callerDomain,
        ObjectRef target,
        const PropertySlotHandle& slot,
        Value& outValue,
        RuntimeError* error = nullptr,
        const Interpreter* interpreter = nullptr) const;

    // O(1) property/field write with strict Cross-Heap Read-Only enforcement (UiPresentation -> SimHeap rejected).
    bool writePropertySlotFast(
        DomainKind callerDomain,
        ObjectRef target,
        const PropertySlotHandle& slot,
        const Value& value,
        const Interpreter* interpreter = nullptr,
        RuntimeError* error = nullptr);

    bool writePropertySlotFast(
        DomainKind callerDomain,
        ObjectRef target,
        const PropertySlotHandle& slot,
        const Value& value,
        RuntimeError* error);

    void enqueueSimCommand(SimCommandEnvelope command);
    [[nodiscard]] std::vector<SimCommandEnvelope> consumeSimCommands();
    [[nodiscard]] std::size_t pendingSimCommandCount() const noexcept {
        return pendingSimCommands_.size();
    }
    std::size_t flushSimCommands(
        const Interpreter& simInterpreter,
        RuntimeError* error = nullptr);

    // Heap-allocated reentrant UI async tasks with control-unload cancellation safety
    UiTaskId spawnReentrantSequenceTask(
        const ProgramImage& program,
        const Interpreter& uiInterpreter,
        ObjectRef targetObject,
        std::string_view sequenceName,
        std::vector<Value> arguments = {},
        NativeHandle boundControl = {},
        UiTaskId awaitPrerequisite = 0);

    UiTaskId spawnCallbackTask(
        UiAsyncStepCallback stepCallback,
        NativeHandle boundControl = {},
        UiTaskId awaitPrerequisite = 0);

    bool stepUiTask(UiTaskId taskId, const Interpreter* uiInterpreter = nullptr);
    std::size_t stepAllUiTasks(const Interpreter* uiInterpreter = nullptr);
    bool cancelUiTask(UiTaskId taskId) noexcept;
    std::size_t cancelUiTasksForControl(NativeHandle boundControl) noexcept;
    [[nodiscard]] UiTaskStatus uiTaskStatus(UiTaskId taskId) const noexcept;
    [[nodiscard]] std::optional<Value> uiTaskResult(UiTaskId taskId) const;
    [[nodiscard]] std::size_t activeUiTaskCount() const noexcept;

    std::size_t stepUiIdleGc(const ShadowStack& shadowStack, double maxMilliseconds);
    std::size_t stepUiIdleGc(double maxMilliseconds);

private:
    struct UiAsyncTaskFrame {
        UiTaskId taskId = 0;
        UiTaskStatus status = UiTaskStatus::Pending;
        UiTaskId prerequisiteTaskId = 0;
        WeakRef targetWeakRef{};
        NativeHandle boundControl{};
        std::uint32_t stepIndex = 0;
        Value result{};
        RuntimeError error{};
        // For sequence-backed tasks: isolated per-instance synthetic fields on the heap
        bool isSequenceTask = false;
        semantic::SymbolId sequenceEntrySymbolId = 0;
        semantic::SymbolId sequenceStepSymbolId = 0;
        std::int64_t sequenceTargetId = 0;
        std::vector<Value> sequenceEntryArgs;
        std::size_t stateFieldIndex = PropertySlotHandle::InvalidIndex;
        std::size_t completedFieldIndex = PropertySlotHandle::InvalidIndex;
        std::size_t resultFieldIndex = PropertySlotHandle::InvalidIndex;
        std::vector<std::pair<std::size_t, Value>> isolatedSequenceFields;
        // For callback-backed tasks
        UiAsyncStepCallback stepCallback;
    };

    std::shared_ptr<ManagedHeap> simHeap_;
    std::shared_ptr<ManagedHeap> uiHeap_;
    std::shared_ptr<NativeHandleRegistry> nativeHandles_;
    DomainKind activeDomain_ = DomainKind::UiPresentation;
    std::vector<SimCommandEnvelope> pendingSimCommands_;
    UiTaskId nextUiTaskId_ = 1;
    std::vector<std::unique_ptr<UiAsyncTaskFrame>> uiTasks_;
    std::unordered_set<std::uint32_t> cancelledControlSlots_;
};

} // namespace realscript::runtime
