#include "realscript/runtime/Runtime.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace realscript::runtime {
namespace {

void setHeapError(RuntimeError* error, ErrorCode code, std::string message) {
    if (!error) return;
    error->code = code;
    error->message = std::move(message);
}

std::size_t estimateValueStorage(std::size_t count) {
    if (count > std::numeric_limits<std::size_t>::max() / sizeof(Value)) {
        return std::numeric_limits<std::size_t>::max();
    }
    return count * sizeof(Value);
}

std::uint64_t nextIdentity(std::atomic<std::uint64_t>& next) noexcept {
    auto value = next.fetch_add(1, std::memory_order_relaxed);
    if (value == 0) value = next.fetch_add(1, std::memory_order_relaxed);
    return value;
}

std::uint64_t nextHeapId() noexcept {
    static std::atomic<std::uint64_t> next{1};
    return nextIdentity(next);
}

std::uint64_t nextRegistryId() noexcept {
    static std::atomic<std::uint64_t> next{1};
    return nextIdentity(next);
}

const char* objectKindName(ObjectKind kind) noexcept {
    switch (kind) {
    case ObjectKind::String: return "string";
    case ObjectKind::Array: return "array";
    case ObjectKind::Record: return "record";
    }
    return "unknown";
}

std::mutex& liveHeapsPoolMutex() noexcept {
    static std::mutex m;
    return m;
}

std::unordered_map<const void*, std::function<void()>>& liveHeapPoolMarkers() noexcept {
    static std::unordered_map<const void*, std::function<void()>> markers;
    return markers;
}

std::mutex& poolGcMutex() noexcept {
    static std::mutex m;
    return m;
}

std::mutex& activeShadowStacksMutex() noexcept {
    static std::mutex m;
    return m;
}

std::unordered_set<const ShadowStack*>& activeShadowStacks() noexcept {
    static std::unordered_set<const ShadowStack*> stacks;
    return stacks;
}

void registerActiveShadowStack(const ShadowStack* stack) noexcept {
    std::lock_guard<std::mutex> lock(activeShadowStacksMutex());
    activeShadowStacks().insert(stack);
}

void unregisterActiveShadowStack(const ShadowStack* stack) noexcept {
    std::lock_guard<std::mutex> lock(activeShadowStacksMutex());
    activeShadowStacks().erase(stack);
}

} // namespace

ShadowStack::~ShadowStack() noexcept {
    if (!frames_.empty()) {
        unregisterActiveShadowStack(this);
    }
}

ShadowStack::ShadowStack(const ShadowStack& other)
    : frames_(other.frames_) {
    if (!frames_.empty()) {
        registerActiveShadowStack(this);
    }
}

ShadowStack& ShadowStack::operator=(const ShadowStack& other) {
    if (this == &other) return *this;
    const bool wasEmpty = frames_.empty();
    frames_ = other.frames_;
    if (wasEmpty && !frames_.empty()) {
        registerActiveShadowStack(this);
    } else if (!wasEmpty && frames_.empty()) {
        unregisterActiveShadowStack(this);
    }
    return *this;
}

ShadowStack::ShadowStack(ShadowStack&& other) noexcept
    : frames_(std::move(other.frames_)) {
    if (!frames_.empty()) {
        unregisterActiveShadowStack(&other);
        registerActiveShadowStack(this);
    }
}

ShadowStack& ShadowStack::operator=(ShadowStack&& other) noexcept {
    if (this == &other) return *this;
    if (!frames_.empty()) {
        unregisterActiveShadowStack(this);
    }
    frames_ = std::move(other.frames_);
    if (!frames_.empty()) {
        unregisterActiveShadowStack(&other);
        registerActiveShadowStack(this);
    }
    return *this;
}

void ShadowStack::pushFrame(
    const std::vector<Value>* arguments,
    const std::vector<Value>* locals,
    const std::vector<Value>* registers,
    const std::size_t* activeRegisterLimit) {
    const bool wasEmpty = frames_.empty();
    frames_.push_back({arguments, locals, registers, activeRegisterLimit});
    if (wasEmpty) {
        registerActiveShadowStack(this);
    }
}

void ShadowStack::popFrame() noexcept {
    if (!frames_.empty()) {
        frames_.pop_back();
        if (frames_.empty()) {
            unregisterActiveShadowStack(this);
        }
    }
}

void ShadowStack::visitRoots(const std::function<void(const Value&)>& visitor) const {
    const auto visitValues = [&](const std::vector<Value>* values, const std::size_t* limit) {
        if (!values) return;
        const std::size_t count = limit
            ? std::min(*limit, values->size())
            : values->size();
        for (std::size_t index = 0; index < count; ++index) {
            visitor((*values)[index]);
        }
    };
    for (const auto& frame : frames_) {
        visitValues(frame.arguments, nullptr);
        visitValues(frame.locals, nullptr);
        visitValues(frame.registers, frame.activeRegisterLimit);
    }
}

std::size_t ShadowStack::frameCount() const noexcept { return frames_.size(); }

struct ManagedHeap::Impl {
    struct Header {
        ObjectKind kind = ObjectKind::Record;
        semantic::SymbolId typeId = 0;
        semantic::PrimitiveType elementType = semantic::PrimitiveType::Error;
        semantic::SymbolId elementTypeId = 0;
        bool marked = false;
        bool inNursery = true;
        bool inRememberedSet = false;
        bool inDirtyQueue = false;
        std::uint16_t uiObserverCount = 0;
        std::uint64_t dirtyFieldMask = 0;
        std::size_t sizeBytes = 0;
    };

    struct HeapObject {
        static constexpr std::size_t InlineFieldCapacity = 4;
        Header header;
        std::array<Value, InlineFieldCapacity> inlineValues{};
        std::uint8_t inlineValueCount = 0;
        bool usesInlineValues = false;
        std::vector<Value> values;
        std::string stringPayload;
        std::shared_ptr<const std::vector<std::size_t>> referenceFields;
        std::optional<NativeRawColumnView> rawColumnView;

        [[nodiscard]] std::size_t valueCount() const noexcept {
            return usesInlineValues
                ? static_cast<std::size_t>(inlineValueCount)
                : values.size();
        }
        [[nodiscard]] const Value& valueAt(std::size_t index) const noexcept {
            return usesInlineValues ? inlineValues[index] : values[index];
        }
        [[nodiscard]] Value& valueAt(std::size_t index) noexcept {
            return usesInlineValues ? inlineValues[index] : values[index];
        }
        void clearValues() noexcept {
            if (usesInlineValues) {
                for (std::uint8_t i = 0; i < inlineValueCount; ++i) {
                    inlineValues[i] = Value{};
                }
                inlineValueCount = 0;
                usesInlineValues = false;
            }
            values.clear();
        }

        void reset() noexcept {
            header = Header{};
            clearValues(); // Retain capacity in slot pool to avoid malloc/free churn!
            stringPayload.clear();
            referenceFields.reset();
            rawColumnView.reset();
        }
    };

    struct LegacyHeapObjectLayout {
        Header header;
        std::vector<Value> values;
        std::string stringPayload;
        std::shared_ptr<const std::vector<std::size_t>> referenceFields;
        std::optional<NativeRawColumnView> rawColumnView;
    };
    static constexpr std::size_t BaseObjectSizeBytes = sizeof(LegacyHeapObjectLayout);

    struct InlineSlotObjectPtr {
        HeapObject storage{};
        bool occupied = false;

        [[nodiscard]] constexpr explicit operator bool() const noexcept { return occupied; }
        [[nodiscard]] constexpr HeapObject* get() noexcept { return occupied ? &storage : nullptr; }
        [[nodiscard]] constexpr const HeapObject* get() const noexcept { return occupied ? &storage : nullptr; }
        [[nodiscard]] constexpr HeapObject* operator->() noexcept { return &storage; }
        [[nodiscard]] constexpr const HeapObject* operator->() const noexcept { return &storage; }
        [[nodiscard]] constexpr HeapObject& operator*() noexcept { return storage; }
        [[nodiscard]] constexpr const HeapObject& operator*() const noexcept { return storage; }

        void emplace(HeapObject&& obj) noexcept {
            storage = std::move(obj);
            occupied = true;
        }
        void reset() noexcept {
            if (occupied) {
                storage.reset();
                occupied = false;
            }
        }
    };

    struct Slot {
        InlineSlotObjectPtr object;
        std::uint32_t generation = 1;
    };

    struct NativeEcsColumnTable {
        std::vector<std::int64_t> entityIds;
        std::vector<Value> values;
        bool mayContainReferences = false;
    };

    [[nodiscard]] static bool valueMayContainReferences(const Value& value) noexcept {
        if (std::holds_alternative<ObjectRef>(value)) return true;
        if (const auto* s = std::get_if<StructValue>(&value)) {
            return !s->isInline && s->storage != nullptr;
        }
        return false;
    }

    explicit Impl(HeapConfig value)
        : config(value), heapId(nextHeapId()),
          nextCollectionThreshold(value.initialCollectionThresholdBytes) {
        {
            std::lock_guard<std::mutex> lock(liveHeapsPoolMutex());
            liveHeapPoolMarkers()[this] = [this]() { this->markAllReachablePoolValues(); };
        }
        if (config.initialCollectionThresholdBytes == 0) {
            config.initialCollectionThresholdBytes = 1;
            nextCollectionThreshold = 1;
        }
        if (config.maximumHeapBytes < config.initialCollectionThresholdBytes) {
            config.maximumHeapBytes = config.initialCollectionThresholdBytes;
        }
    }

    ~Impl() {
        std::lock_guard<std::mutex> lock(liveHeapsPoolMutex());
        liveHeapPoolMarkers().erase(this);
    }

    void markAllReachablePoolValues() const {
        for (const auto& entry : persistentRoots) markValueInPools(entry.second);
        for (const auto& entry : staticFields) markValueInPools(entry.second);
        for (const auto& [typeName, table] : nativeEcsTables) {
            (void)typeName;
            for (const auto& v : table.values) markValueInPools(v);
        }
        for (const auto& slot : slots) {
            if (!slot.object) continue;
            const auto count = slot.object->valueCount();
            for (std::size_t i = 0; i < count; ++i) {
                markValueInPools(slot.object->valueAt(i));
            }
        }
    }

    HeapConfig config;
    std::uint64_t heapId = 0;
    std::vector<Slot> slots;
    std::vector<std::uint32_t> freeSlots;
    std::vector<ObjectRef> markStack;
    std::vector<std::uint32_t> nurserySlots;
    std::vector<ObjectRef> rememberedSet;
    std::unordered_map<RootToken, Value> persistentRoots;
    std::unordered_map<semantic::SymbolId, Value> staticFields;
    std::unordered_map<std::string, NativeEcsColumnTable> nativeEcsTables;
    std::unordered_map<std::string, ObjectRef> internedStrings;
    std::unordered_map<semantic::SymbolId, std::shared_ptr<const std::vector<std::size_t>>> sharedTypeReferenceFields;
    std::unordered_map<semantic::SymbolId, std::vector<std::uint8_t>> typeObservableBits;
    std::vector<ObjectRef> dirtyObjects;
    RootToken nextRootToken = 1;
    GcPhase phase = GcPhase::Idle;
    bool requested = false;
    bool resumeSweep = false;
    bool hasSparseEcsStaticFields = false;
    bool rootsRescannedForSweep = false;
    std::size_t sweepIndex = 0;
    std::size_t sweepLimit = 0;
    std::size_t nextCollectionThreshold = 0;
    GcStatistics statistics;

    [[nodiscard]] HeapObject* get(ObjectRef reference) noexcept {
        if (!reference.valid() || reference.heapId != heapId ||
            reference.slot >= slots.size()) return nullptr;
        auto& slot = slots[reference.slot];
        if (!slot.object || slot.generation != reference.generation ||
            slot.object->header.kind != reference.kind) return nullptr;
        return slot.object.get();
    }

    [[nodiscard]] const HeapObject* get(ObjectRef reference) const noexcept {
        if (!reference.valid() || reference.heapId != heapId ||
            reference.slot >= slots.size()) return nullptr;
        const auto& slot = slots[reference.slot];
        if (!slot.object || slot.generation != reference.generation ||
            slot.object->header.kind != reference.kind) return nullptr;
        return slot.object.get();
    }

    [[nodiscard]] std::optional<std::uint32_t> acquireSlot(
        std::size_t sizeBytes,
        RuntimeError* error) {
        if (sizeBytes == std::numeric_limits<std::size_t>::max() ||
            sizeBytes > config.maximumHeapBytes ||
            statistics.liveBytes > config.maximumHeapBytes - sizeBytes) {
            setHeapError(error, ErrorCode::OutOfMemory,
                "managed heap allocation exceeds the configured heap limit");
            requested = true;
            return std::nullopt;
        }
        if (!freeSlots.empty()) {
            const auto slotIndex = freeSlots.back();
            freeSlots.pop_back();
            return slotIndex;
        }
        if (slots.size() >= static_cast<std::size_t>(ObjectRef::InvalidSlot)) {
            setHeapError(error, ErrorCode::OutOfMemory,
                "managed heap exhausted the object handle space");
            return std::nullopt;
        }
        const auto slotIndex = static_cast<std::uint32_t>(slots.size());
        slots.emplace_back();
        return slotIndex;
    }

    [[nodiscard]] ObjectRef finalizeSlotAllocation(
        std::uint32_t slotIndex,
        ObjectKind kind,
        semantic::SymbolId typeId,
        semantic::PrimitiveType elementType,
        semantic::SymbolId elementTypeId,
        std::shared_ptr<const std::vector<std::size_t>> referenceFields,
        std::size_t sizeBytes) {
        auto& slot = slots[slotIndex];
        auto& object = slot.object.storage;
        object.header.kind = kind;
        object.header.typeId = typeId;
        object.header.elementType = elementType;
        object.header.elementTypeId = elementTypeId;
        object.header.marked = phase != GcPhase::Idle;
        object.header.inNursery = true;
        object.header.inRememberedSet = false;
        object.header.inDirtyQueue = false;
        object.header.uiObserverCount = 0;
        object.header.dirtyFieldMask = 0;
        object.header.sizeBytes = sizeBytes;
        object.referenceFields = std::move(referenceFields);
        slot.object.occupied = true;
        nurserySlots.push_back(slotIndex);
        const ObjectRef reference{slotIndex, slot.generation, kind, heapId};

        ++statistics.objectsAllocated;
        statistics.bytesAllocated += sizeBytes;
        ++statistics.liveObjects;
        statistics.liveBytes += sizeBytes;
        statistics.peakLiveBytes = std::max(
            statistics.peakLiveBytes,
            statistics.liveBytes);
        if (statistics.liveBytes >= nextCollectionThreshold) requested = true;
        if (phase != GcPhase::Idle) {
            markStack.push_back(reference);
            if (phase == GcPhase::Sweep) {
                phase = GcPhase::Mark;
                resumeSweep = true;
            }
        }
        return reference;
    }

    [[nodiscard]] std::optional<ObjectRef> allocateStringSlot(
        std::string value,
        std::size_t sizeBytes,
        RuntimeError* error) {
        const auto slotIndex = acquireSlot(sizeBytes, error);
        if (!slotIndex) return std::nullopt;
        auto& object = slots[*slotIndex].object.storage;
        object.clearValues();
        object.stringPayload = std::move(value);
        return finalizeSlotAllocation(
            *slotIndex, ObjectKind::String, 0,
            semantic::PrimitiveType::Error, 0, nullptr, sizeBytes);
    }

    [[nodiscard]] std::optional<ObjectRef> allocateArraySlot(
        semantic::SymbolId arrayTypeId,
        semantic::PrimitiveType elementType,
        semantic::SymbolId elementTypeId,
        std::size_t length,
        const Value& initialValue,
        std::size_t sizeBytes,
        RuntimeError* error) {
        const auto slotIndex = acquireSlot(sizeBytes, error);
        if (!slotIndex) return std::nullopt;
        auto& object = slots[*slotIndex].object.storage;
        object.clearValues();
        object.stringPayload.clear();
        // Reuse existing vector capacity in-place without malloc/free!
        pinValueInPools(initialValue);
        object.values.assign(length, initialValue);
        return finalizeSlotAllocation(
            *slotIndex, ObjectKind::Array, arrayTypeId,
            elementType, elementTypeId, nullptr, sizeBytes);
    }

    [[nodiscard]] std::optional<ObjectRef> allocateValuesSlot(
        ObjectKind kind,
        semantic::SymbolId typeId,
        semantic::PrimitiveType elementType,
        semantic::SymbolId elementTypeId,
        std::vector<Value>&& vals,
        std::shared_ptr<const std::vector<std::size_t>> referenceFields,
        std::size_t sizeBytes,
        RuntimeError* error) {
        const auto slotIndex = acquireSlot(sizeBytes, error);
        if (!slotIndex) return std::nullopt;
        auto& object = slots[*slotIndex].object.storage;
        object.stringPayload.clear();
        for (const auto& v : vals) pinValueInPools(v);
        if (kind == ObjectKind::Record &&
            vals.size() <= HeapObject::InlineFieldCapacity) {
            object.values.clear();
            object.usesInlineValues = true;
            object.inlineValueCount = static_cast<std::uint8_t>(vals.size());
            for (std::size_t i = 0; i < vals.size(); ++i) {
                object.inlineValues[i] = std::move(vals[i]);
            }
            for (std::size_t i = vals.size(); i < HeapObject::InlineFieldCapacity; ++i) {
                object.inlineValues[i] = Value{};
            }
        } else {
            if (object.usesInlineValues) {
                for (std::uint8_t i = 0; i < object.inlineValueCount; ++i) {
                    object.inlineValues[i] = Value{};
                }
                object.inlineValueCount = 0;
                object.usesInlineValues = false;
            }
            if (object.values.capacity() >= vals.size()) {
                object.values.assign(vals.begin(), vals.end());
            } else {
                object.values = std::move(vals);
            }
        }
        return finalizeSlotAllocation(
            *slotIndex, kind, typeId, elementType, elementTypeId,
            std::move(referenceFields), sizeBytes);
    }

    void mark(ObjectRef reference) {
        auto* object = get(reference);
        if (!object || object->header.marked) return;
        object->header.marked = true;
        markStack.push_back(reference);
    }

    void markValue(
        const Value& value,
        std::unordered_set<const StructStorage*>& visitedStructs) {
        if (const auto* reference = std::get_if<ObjectRef>(&value)) {
            mark(*reference);
            return;
        }
        if (const auto* structure = std::get_if<StructValue>(&value);
            structure && structure->storage &&
            visitedStructs.insert(structure->storage.get()).second) {
            for (const auto& field : structure->storage->fields) {
                markValue(field, visitedStructs);
            }
        }
    }

    void markValue(const Value& value) {
        std::unordered_set<const StructStorage*> visitedStructs;
        markValue(value, visitedStructs);
    }

    [[nodiscard]] bool validRootValue(
        const Value& value,
        std::unordered_set<const StructStorage*>& visitedStructs) const {
        if (const auto* reference = std::get_if<ObjectRef>(&value)) {
            return get(*reference) != nullptr;
        }
        if (const auto* structure = std::get_if<StructValue>(&value)) {
            if (!structure->valid()) return false;
            if (!structure->storage) return true;
            if (!visitedStructs.insert(structure->storage.get()).second) return true;
            for (const auto& field : structure->storage->fields) {
                if (!validRootValue(field, visitedStructs)) return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool validRootValue(const Value& value) const {
        std::unordered_set<const StructStorage*> visitedStructs;
        return validRootValue(value, visitedStructs);
    }

    void markRoots(const ShadowStack& shadowStack) {
        shadowStack.visitRoots([&](const Value& value) { markValue(value); });
        for (const auto& entry : persistentRoots) markValue(entry.second);
        for (const auto& entry : staticFields) markValue(entry.second);
        for (const auto& [typeName, table] : nativeEcsTables) {
            (void)typeName;
            if (!table.mayContainReferences) continue;
            for (const auto& v : table.values) markValue(v);
        }
    }

    void startCollection(const ShadowStack& shadowStack) {
        for (auto& slot : slots) {
            if (slot.object) slot.object->header.marked = false;
        }
        markStack.clear();
        sweepIndex = 0;
        sweepLimit = slots.size();
        resumeSweep = false;
        rootsRescannedForSweep = false;
        phase = GcPhase::Mark;
        ++statistics.collectionsStarted;
        markRoots(shadowStack);
    }

    void scan(ObjectRef reference) {
        const auto* object = get(reference);
        if (!object || object->header.kind == ObjectKind::String) return;
        const auto count = object->valueCount();
        if (object->header.kind == ObjectKind::Array) {
            for (std::size_t i = 0; i < count; ++i) markValue(object->valueAt(i));
            return;
        }
        if (object->referenceFields) {
            for (const auto index : *object->referenceFields) {
                if (index < count) markValue(object->valueAt(index));
            }
        }
    }

    void writeBarrier(ObjectRef owner, const Value& value) {
        if (auto* ownerObj = get(owner);
            ownerObj && !ownerObj->header.inNursery && !ownerObj->header.inRememberedSet) {
            ownerObj->header.inRememberedSet = true;
            rememberedSet.push_back(owner);
        }
        if (phase == GcPhase::Idle) return;
        const auto* object = get(owner);
        if (!object || !object->header.marked) return;
        markValue(value);
        if (phase == GcPhase::Sweep && !markStack.empty()) {
            phase = GcPhase::Mark;
            resumeSweep = true;
        }
    }

    void finishCollection(const ShadowStack& shadowStack) {
        phase = GcPhase::Idle;
        requested = false;
        resumeSweep = false;
        sweepIndex = 0;
        sweepLimit = 0;
        for (const auto slotIdx : nurserySlots) {
            if (slotIdx < slots.size() && slots[slotIdx].object &&
                slots[slotIdx].object->header.inNursery) {
                slots[slotIdx].object->header.inNursery = false;
                ++statistics.objectsPromoted;
            }
        }
        nurserySlots.clear();
        for (const auto& ref : rememberedSet) {
            if (auto* obj = get(ref)) {
                obj->header.inRememberedSet = false;
            }
        }
        rememberedSet.clear();
        ++statistics.collectionsCompleted;
        const auto doubled = statistics.liveBytes >
                std::numeric_limits<std::size_t>::max() / 2
            ? config.maximumHeapBytes
            : statistics.liveBytes * 2;
        nextCollectionThreshold = std::max(
            config.initialCollectionThresholdBytes,
            std::min(config.maximumHeapBytes, doubled));
        (void)reclaimUnreferencedPoolValues(&shadowStack);
    }
};

ManagedHeap::ManagedHeap(HeapConfig config)
    : impl_(std::make_unique<Impl>(config)) {}

ManagedHeap::~ManagedHeap() = default;
ManagedHeap::ManagedHeap(ManagedHeap&&) noexcept = default;
ManagedHeap& ManagedHeap::operator=(ManagedHeap&&) noexcept = default;

std::optional<ObjectRef> ManagedHeap::allocateString(
    std::string value,
    RuntimeError* error) {
    const auto size = Impl::BaseObjectSizeBytes + value.size();
    return impl_->allocateStringSlot(std::move(value), size, error);
}

std::optional<ObjectRef> ManagedHeap::internString(
    std::string_view value,
    RuntimeError* error) {
    const std::string key(value);
    const auto found = impl_->internedStrings.find(key);
    if (found != impl_->internedStrings.end()) {
        if (impl_->get(found->second) != nullptr) {
            if (impl_->phase != GcPhase::Idle) {
                impl_->mark(found->second);
                if (impl_->phase == GcPhase::Sweep) {
                    impl_->phase = GcPhase::Mark;
                    impl_->resumeSweep = true;
                }
            }
            return found->second;
        }
        impl_->internedStrings.erase(found);
    }
    auto allocated = allocateString(key, error);
    if (allocated) {
        impl_->internedStrings[key] = *allocated;
    }
    return allocated;
}

std::size_t ManagedHeap::internedStringCount() const noexcept {
    std::size_t aliveCount = 0;
    for (const auto& [k, ref] : impl_->internedStrings) {
        (void)k;
        if (impl_->get(ref) != nullptr) ++aliveCount;
    }
    return aliveCount;
}

std::size_t ManagedHeap::sharedTypeLayoutCount() const noexcept {
    return impl_->sharedTypeReferenceFields.size();
}

std::optional<ObjectRef> ManagedHeap::allocateArray(
    std::size_t length,
    Value initialValue,
    RuntimeError* error) {
    auto elementType = valueType(initialValue);
    if (elementType == semantic::PrimitiveType::Null) {
        elementType = semantic::PrimitiveType::Error;
    }
    return allocateTypedArray(
        0, elementType, 0, length, std::move(initialValue), error);
}

std::optional<ObjectRef> ManagedHeap::allocateTypedArray(
    semantic::SymbolId arrayTypeId,
    semantic::PrimitiveType elementType,
    semantic::SymbolId elementTypeId,
    std::size_t length,
    Value initialValue,
    RuntimeError* error) {
    const bool validElement = elementType == semantic::PrimitiveType::Error ||
        elementType == semantic::PrimitiveType::Bool ||
        semantic::isNumericType(elementType) ||
        elementType == semantic::PrimitiveType::String ||
        elementType == semantic::PrimitiveType::Object ||
        elementType == semantic::PrimitiveType::Struct ||
        elementType == semantic::PrimitiveType::Enum ||
        elementType == semantic::PrimitiveType::Array ||
        elementType == semantic::PrimitiveType::Handle;
    if (!validElement ||
        (semantic::isExactType(elementType)
            ? elementTypeId == 0
            : elementTypeId != 0)) {
        setHeapError(error, ErrorCode::InvalidArguments,
            "managed array element type metadata is invalid");
        return std::nullopt;
    }
    if (elementType != semantic::PrimitiveType::Error &&
        valueType(initialValue) != elementType) {
        setHeapError(error, ErrorCode::TypeMismatch,
            "managed array initial value does not match its element type");
        return std::nullopt;
    }
    const auto storage = estimateValueStorage(length);
    if (storage == std::numeric_limits<std::size_t>::max()) {
        setHeapError(error, ErrorCode::OutOfMemory, "managed array is too large");
        return std::nullopt;
    }
    return impl_->allocateArraySlot(
        arrayTypeId,
        elementType,
        elementTypeId,
        length,
        initialValue,
        Impl::BaseObjectSizeBytes + storage,
        error);
}

std::optional<ObjectRef> ManagedHeap::allocateRecord(
    std::size_t fieldCount,
    RuntimeError* error) {
    const auto storage = estimateValueStorage(fieldCount);
    if (storage == std::numeric_limits<std::size_t>::max()) {
        setHeapError(error, ErrorCode::OutOfMemory, "managed record is too large");
        return std::nullopt;
    }
    std::vector<Value> fields(fieldCount);
    std::vector<std::size_t> referenceFields(fieldCount);
    for (std::size_t index = 0; index < fieldCount; ++index) {
        referenceFields[index] = index;
    }
    return impl_->allocateValuesSlot(
        ObjectKind::Record,
        0,
        semantic::PrimitiveType::Error,
        0,
        std::move(fields),
        std::make_shared<const std::vector<std::size_t>>(std::move(referenceFields)),
        Impl::BaseObjectSizeBytes + storage,
        error);
}

std::optional<ObjectRef> ManagedHeap::allocateObject(
    semantic::SymbolId typeId,
    std::vector<Value> fields,
    std::vector<std::size_t> referenceFields,
    RuntimeError* error) {
    const auto storage = estimateValueStorage(fields.size());
    if (typeId == 0) {
        setHeapError(error, ErrorCode::InvalidArguments,
            "managed object type ID must be non-zero");
        return std::nullopt;
    }
    if (storage == std::numeric_limits<std::size_t>::max()) {
        setHeapError(error, ErrorCode::OutOfMemory, "managed object is too large");
        return std::nullopt;
    }
    for (const auto index : referenceFields) {
        if (index >= fields.size()) {
            setHeapError(error, ErrorCode::InvalidArguments,
                "managed object reference field is out of range");
            return std::nullopt;
        }
    }
    std::sort(referenceFields.begin(), referenceFields.end());
    referenceFields.erase(
        std::unique(referenceFields.begin(), referenceFields.end()),
        referenceFields.end());
    std::shared_ptr<const std::vector<std::size_t>> sharedRefs;
    const auto foundLayout = impl_->sharedTypeReferenceFields.find(typeId);
    if (foundLayout != impl_->sharedTypeReferenceFields.end() &&
        foundLayout->second && *foundLayout->second == referenceFields) {
        sharedRefs = foundLayout->second;
    } else {
        sharedRefs = std::make_shared<const std::vector<std::size_t>>(std::move(referenceFields));
        impl_->sharedTypeReferenceFields[typeId] = sharedRefs;
    }
    return impl_->allocateValuesSlot(
        ObjectKind::Record,
        typeId,
        semantic::PrimitiveType::Error,
        0,
        std::move(fields),
        std::move(sharedRefs),
        Impl::BaseObjectSizeBytes + storage,
        error);
}

bool ManagedHeap::isAlive(ObjectRef reference) const noexcept {
    return impl_->get(reference) != nullptr;
}

std::optional<std::string_view> ManagedHeap::stringView(ObjectRef reference) const {
    const auto* object = impl_->get(reference);
    if (!object || object->header.kind != ObjectKind::String) return std::nullopt;
    return std::string_view(object->stringPayload);
}

namespace {

[[nodiscard]] constexpr std::size_t nativeFieldKindSize(NativeFieldKind kind) noexcept {
    switch (kind) {
    case NativeFieldKind::Int32: return 4;
    case NativeFieldKind::Int64: return 8;
    case NativeFieldKind::Float32: return 4;
    case NativeFieldKind::Float64: return 8;
    case NativeFieldKind::Bool: return 1;
    case NativeFieldKind::Fixed: return 4;
    case NativeFieldKind::Fixed16: return 2;
    case NativeFieldKind::Vec2: return 8;
    case NativeFieldKind::Vec3: return 12;
    case NativeFieldKind::Quat: return 16;
    }
    return 0;
}

[[nodiscard]] bool validateRawColumnView(
    const NativeRawColumnView& view,
    semantic::SymbolId elementTypeId) noexcept {
    if (elementTypeId == 0 || view.structTypeId != elementTypeId ||
        view.fields.empty() || view.strideBytes == 0) {
        return false;
    }
    if (view.count > 0 && !view.basePtr) {
        return false;
    }
    for (const auto& f : view.fields) {
        const auto fSize = nativeFieldKindSize(f.kind);
        if (fSize == 0 || f.byteOffset > view.strideBytes ||
            fSize > view.strideBytes - f.byteOffset) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] Value readNativeFieldFromBytes(
    const std::uint8_t* elemBase,
    const NativeFieldLayout& layout) noexcept {
    const std::uint8_t* ptr = elemBase + layout.byteOffset;
    switch (layout.kind) {
    case NativeFieldKind::Int32: {
        std::int32_t v = 0;
        std::memcpy(&v, ptr, sizeof(v));
        if (layout.scriptType == semantic::PrimitiveType::Long) {
            return LongValue{static_cast<std::int64_t>(v)};
        }
        return static_cast<std::int64_t>(v);
    }
    case NativeFieldKind::Int64: {
        std::int64_t v = 0;
        std::memcpy(&v, ptr, sizeof(v));
        if (layout.scriptType == semantic::PrimitiveType::Long) {
            return LongValue{v};
        }
        return v;
    }
    case NativeFieldKind::Float32: {
        float v = 0.0f;
        std::memcpy(&v, ptr, sizeof(v));
        if (layout.scriptType == semantic::PrimitiveType::Double) {
            return static_cast<double>(v);
        }
        return FloatValue{v};
    }
    case NativeFieldKind::Float64: {
        double v = 0.0;
        std::memcpy(&v, ptr, sizeof(v));
        if (layout.scriptType == semantic::PrimitiveType::Float) {
            return FloatValue{static_cast<float>(v)};
        }
        return v;
    }
    case NativeFieldKind::Bool: {
        bool v = false;
        std::memcpy(&v, ptr, sizeof(v));
        return v;
    }
    case NativeFieldKind::Fixed: {
        std::int32_t raw = 0;
        std::memcpy(&raw, ptr, sizeof(raw));
        return FixedValue{raw};
    }
    case NativeFieldKind::Fixed16: {
        std::int16_t raw = 0;
        std::memcpy(&raw, ptr, sizeof(raw));
        return Fixed16Value{raw};
    }
    case NativeFieldKind::Vec2: {
        Vec2Value v{};
        std::memcpy(&v, ptr, sizeof(v));
        return v;
    }
    case NativeFieldKind::Vec3: {
        Vec3Value v{};
        std::memcpy(&v, ptr, sizeof(v));
        return v;
    }
    case NativeFieldKind::Quat: {
        QuatValue v{};
        std::memcpy(&v, ptr, sizeof(v));
        return v;
    }
    }
    return std::int64_t{0};
}

bool writeNativeFieldToBytes(
    std::uint8_t* elemBase,
    const NativeFieldLayout& layout,
    const Value& val) noexcept {
    std::uint8_t* ptr = elemBase + layout.byteOffset;
    switch (layout.kind) {
    case NativeFieldKind::Int32: {
        std::int32_t out = 0;
        if (const auto* i = std::get_if<std::int64_t>(&val)) {
            out = static_cast<std::int32_t>(*i);
        } else if (const auto* l = std::get_if<LongValue>(&val)) {
            out = static_cast<std::int32_t>(l->value);
        } else {
            return false;
        }
        std::memcpy(ptr, &out, sizeof(out));
        return true;
    }
    case NativeFieldKind::Int64: {
        std::int64_t out = 0;
        if (const auto* i = std::get_if<std::int64_t>(&val)) {
            out = *i;
        } else if (const auto* l = std::get_if<LongValue>(&val)) {
            out = l->value;
        } else {
            return false;
        }
        std::memcpy(ptr, &out, sizeof(out));
        return true;
    }
    case NativeFieldKind::Float32: {
        float out = 0.0f;
        if (const auto* f = std::get_if<FloatValue>(&val)) {
            out = f->value;
        } else if (const auto* d = std::get_if<double>(&val)) {
            out = static_cast<float>(*d);
        } else {
            return false;
        }
        std::memcpy(ptr, &out, sizeof(out));
        return true;
    }
    case NativeFieldKind::Float64: {
        double out = 0.0;
        if (const auto* d = std::get_if<double>(&val)) {
            out = *d;
        } else if (const auto* f = std::get_if<FloatValue>(&val)) {
            out = static_cast<double>(f->value);
        } else {
            return false;
        }
        std::memcpy(ptr, &out, sizeof(out));
        return true;
    }
    case NativeFieldKind::Bool: {
        const auto* b = std::get_if<bool>(&val);
        if (!b) return false;
        const bool out = *b;
        std::memcpy(ptr, &out, sizeof(out));
        return true;
    }
    case NativeFieldKind::Fixed: {
        if (const auto* fx = std::get_if<FixedValue>(&val)) {
            std::memcpy(ptr, &fx->raw, sizeof(fx->raw));
            return true;
        }
        if (const auto f0 = loadInlineStructField(val, 0)) {
            if (const auto* i = std::get_if<std::int64_t>(&*f0)) {
                const auto raw = static_cast<std::int32_t>(*i);
                std::memcpy(ptr, &raw, sizeof(raw));
                return true;
            }
        }
        return false;
    }
    case NativeFieldKind::Fixed16: {
        if (const auto* fx = std::get_if<Fixed16Value>(&val)) {
            std::memcpy(ptr, &fx->raw, sizeof(fx->raw));
            return true;
        }
        return false;
    }
    case NativeFieldKind::Vec2: {
        if (const auto* v = std::get_if<Vec2Value>(&val)) {
            std::memcpy(ptr, v, sizeof(*v));
            return true;
        }
        return false;
    }
    case NativeFieldKind::Vec3: {
        if (const auto* v = std::get_if<Vec3Value>(&val)) {
            std::memcpy(ptr, v, sizeof(*v));
            return true;
        }
        return false;
    }
    case NativeFieldKind::Quat: {
        if (const auto* v = std::get_if<QuatValue>(&val)) {
            std::memcpy(ptr, v, sizeof(*v));
            return true;
        }
        return false;
    }
    }
    return false;
}

} // namespace

std::optional<std::size_t> ManagedHeap::arrayLength(ObjectRef reference) const {
    const auto* object = impl_->get(reference);
    if (!object || object->header.kind != ObjectKind::Array) return std::nullopt;
    if (object->rawColumnView.has_value()) {
        return object->rawColumnView->count;
    }
    return object->values.size();
}

std::optional<semantic::SymbolId> ManagedHeap::arrayTypeId(
    ObjectRef reference) const {
    const auto* object = impl_->get(reference);
    if (!object || object->header.kind != ObjectKind::Array) return std::nullopt;
    return object->header.typeId;
}

std::optional<semantic::PrimitiveType> ManagedHeap::arrayElementType(
    ObjectRef reference) const {
    const auto* object = impl_->get(reference);
    if (!object || object->header.kind != ObjectKind::Array) return std::nullopt;
    return object->header.elementType;
}

std::optional<semantic::SymbolId> ManagedHeap::arrayElementTypeId(
    ObjectRef reference) const {
    const auto* object = impl_->get(reference);
    if (!object || object->header.kind != ObjectKind::Array) return std::nullopt;
    return object->header.elementTypeId;
}

std::optional<Value> ManagedHeap::arrayGet(ObjectRef reference, std::size_t index) const {
    const auto* object = impl_->get(reference);
    if (!object || object->header.kind != ObjectKind::Array) return std::nullopt;
    if (object->rawColumnView.has_value()) {
        const auto& view = *object->rawColumnView;
        if (index >= view.count || (!view.basePtr && view.count > 0)) return std::nullopt;
        const auto* elemBase = static_cast<const std::uint8_t*>(view.basePtr) + index * view.strideBytes;
        const auto fieldCount = view.fields.size();
        if (fieldCount <= 16) {
            std::array<Value, 16> stackFields{};
            for (std::size_t i = 0; i < fieldCount; ++i) {
                stackFields[i] = readNativeFieldFromBytes(elemBase, view.fields[i]);
            }
            return Value{makeStructValueFromFields(view.structTypeId, ValueSpan(stackFields.data(), fieldCount))};
        }
        std::vector<Value> heapFields(fieldCount);
        for (std::size_t i = 0; i < fieldCount; ++i) {
            heapFields[i] = readNativeFieldFromBytes(elemBase, view.fields[i]);
        }
        return Value{makeStructValueFromFields(view.structTypeId, std::move(heapFields))};
    }
    const auto& values = object->values;
    if (index >= values.size()) return std::nullopt;
    return values[index];
}

const Value* ManagedHeap::arrayGetFast(ObjectRef reference, std::size_t index) const noexcept {
    const auto* object = impl_->get(reference);
    if (!object || object->header.kind != ObjectKind::Array ||
        object->rawColumnView.has_value() || index >= object->values.size()) {
        return nullptr;
    }
    return &object->values[index];
}

bool ManagedHeap::arraySetPrevalidated(
    ObjectRef reference,
    std::size_t index,
    const Value& value,
    bool mayBeReference) noexcept {
    auto* object = impl_->get(reference);
    if (!object || object->header.kind != ObjectKind::Array) {
        return false;
    }
    if (object->rawColumnView.has_value()) {
        const auto& view = *object->rawColumnView;
        if (view.readOnly || index >= view.count || !view.basePtr) return false;
        const auto* s = std::get_if<StructValue>(&value);
        if (!s || !s->valid() || s->typeId != view.structTypeId ||
            s->fieldCount() != view.fields.size()) {
            return false;
        }
        auto* elemBase = static_cast<std::uint8_t*>(view.basePtr) + index * view.strideBytes;
        for (std::size_t i = 0; i < view.fields.size(); ++i) {
            const auto fieldVal = structValueFieldAt(*s, i);
            if (!fieldVal || !writeNativeFieldToBytes(elemBase, view.fields[i], *fieldVal)) {
                return false;
            }
        }
        return true;
    }
    if (index >= object->values.size()) {
        return false;
    }
    if (mayBeReference) {
        impl_->writeBarrier(reference, value);
    }
    pinValueInPools(value);
    object->values[index] = value;
    return true;
}

bool ManagedHeap::arraySet(
    ObjectRef reference,
    std::size_t index,
    Value value,
    RuntimeError* error,
    ArrayStoreTypePolicy typePolicy) {
    auto* object = impl_->get(reference);
    if (!object || object->header.kind != ObjectKind::Array) {
        setHeapError(error, ErrorCode::InvalidObjectReference,
            "managed array reference is invalid");
        return false;
    }
    if (object->rawColumnView.has_value()) {
        const auto& view = *object->rawColumnView;
        if (index >= view.count) {
            setHeapError(error, ErrorCode::IndexOutOfRange,
                "managed array index is out of range");
            return false;
        }
        if (view.readOnly || !view.basePtr) {
            setHeapError(error, ErrorCode::CapabilityDenied,
                "cannot write to a read-only native ECS column view");
            return false;
        }
        const auto* s = std::get_if<StructValue>(&value);
        if (!s || !s->valid() || s->typeId != view.structTypeId ||
            s->fieldCount() != view.fields.size()) {
            setHeapError(error, ErrorCode::TypeMismatch,
                "managed array struct element type mismatch for raw ECS column");
            return false;
        }
        auto* elemBase = static_cast<std::uint8_t*>(view.basePtr) + index * view.strideBytes;
        for (std::size_t i = 0; i < view.fields.size(); ++i) {
            const auto fieldVal = structValueFieldAt(*s, i);
            if (!fieldVal || !writeNativeFieldToBytes(elemBase, view.fields[i], *fieldVal)) {
                setHeapError(error, ErrorCode::TypeMismatch,
                    "raw ECS column field type mismatch during in-place write");
                return false;
            }
        }
        return true;
    }
    auto& values = object->values;
    if (index >= values.size()) {
        setHeapError(error, ErrorCode::IndexOutOfRange,
            "managed array index is out of range");
        return false;
    }
    const auto expected = object->header.elementType;
    if (expected != semantic::PrimitiveType::Error && valueType(value) != expected) {
        setHeapError(error, ErrorCode::TypeMismatch,
            "managed array element type mismatch");
        return false;
    }
    if (expected == semantic::PrimitiveType::Object &&
        !std::holds_alternative<NullObject>(value)) {
        const auto* child = std::get_if<ObjectRef>(&value);
        const auto actual = child ? objectTypeId(*child) : std::nullopt;
        if (!actual ||
            (typePolicy == ArrayStoreTypePolicy::Exact &&
             *actual != object->header.elementTypeId)) {
            setHeapError(error, ErrorCode::TypeMismatch,
                "managed array object element type mismatch");
            return false;
        }
    } else if (expected == semantic::PrimitiveType::Array &&
               !std::holds_alternative<NullArray>(value)) {
        const auto* child = std::get_if<ObjectRef>(&value);
        const auto actual = child ? arrayTypeId(*child) : std::nullopt;
        if (!actual || *actual != object->header.elementTypeId) {
            setHeapError(error, ErrorCode::TypeMismatch,
                "managed array nested array type mismatch");
            return false;
        }
    } else if (expected == semantic::PrimitiveType::Struct) {
        const auto actualTypeId = structValueTypeId(value);
        const auto* structure = std::get_if<StructValue>(&value);
        if (!actualTypeId || *actualTypeId != object->header.elementTypeId ||
            (structure && !structure->valid())) {
            setHeapError(error, ErrorCode::TypeMismatch,
                "managed array struct element type mismatch");
            return false;
        }
    } else if (expected == semantic::PrimitiveType::Enum) {
        const auto* enumeration = std::get_if<EnumValue>(&value);
        if (!enumeration || enumeration->typeId != object->header.elementTypeId) {
            setHeapError(error, ErrorCode::TypeMismatch,
                "managed array enum element type mismatch");
            return false;
        }
    } else if (expected == semantic::PrimitiveType::Handle) {
        const auto* handle = std::get_if<NativeHandle>(&value);
        if (!handle || (object->header.elementTypeId != 0 &&
                        handle->typeId != object->header.elementTypeId)) {
            setHeapError(error, ErrorCode::TypeMismatch,
                "managed array native handle type mismatch");
            return false;
        }
    }
    pinValueInPools(value);
    impl_->writeBarrier(reference, value);
    values[index] = std::move(value);
    return true;
}

std::optional<semantic::SymbolId> ManagedHeap::objectTypeId(
    ObjectRef reference) const {
    const auto* object = impl_->get(reference);
    if (!object || object->header.kind != ObjectKind::Record ||
        object->header.typeId == 0) {
        return std::nullopt;
    }
    return object->header.typeId;
}

std::optional<std::size_t> ManagedHeap::fieldCount(ObjectRef reference) const {
    const auto* object = impl_->get(reference);
    if (!object || object->header.kind != ObjectKind::Record) return std::nullopt;
    return object->valueCount();
}

bool ManagedHeap::isInlineRecordObject(ObjectRef reference) const noexcept {
    const auto* object = impl_->get(reference);
    return object && object->header.kind == ObjectKind::Record &&
        object->usesInlineValues;
}

std::optional<Value> ManagedHeap::fieldGet(ObjectRef reference, std::size_t index) const {
    const auto* object = impl_->get(reference);
    if (!object || object->header.kind != ObjectKind::Record) return std::nullopt;
    if (index >= object->valueCount()) return std::nullopt;
    return object->valueAt(index);
}

const Value* ManagedHeap::fieldGetFast(ObjectRef reference, std::size_t index) const noexcept {
    const auto* object = impl_->get(reference);
    if (!object || object->header.kind != ObjectKind::Record ||
        index >= object->valueCount()) {
        return nullptr;
    }
    return &object->valueAt(index);
}

bool ManagedHeap::fieldSetFast(
    ObjectRef reference,
    std::size_t index,
    const Value& value) noexcept {
    return fieldSet(reference, index, value, nullptr);
}

bool ManagedHeap::fieldSet(
    ObjectRef reference,
    std::size_t index,
    Value value,
    RuntimeError* error) {
    auto* object = impl_->get(reference);
    if (!object || object->header.kind != ObjectKind::Record) {
        setHeapError(error, ErrorCode::InvalidObjectReference,
            "managed record reference is invalid");
        return false;
    }
    if (index >= object->valueCount()) {
        setHeapError(error, ErrorCode::InvalidArguments,
            "managed field index is out of range");
        return false;
    }
    auto& slotVal = object->valueAt(index);
    if (slotVal == value) {
        return true;
    }
    if (object->referenceFields &&
        std::binary_search(
            object->referenceFields->begin(),
            object->referenceFields->end(),
            index)) {
        impl_->writeBarrier(reference, value);
    }
    pinValueInPools(value);
    slotVal = std::move(value);

    std::uint8_t observableBit = 0xFFu;
    if (object->header.typeId != 0) {
        const auto foundBits = impl_->typeObservableBits.find(object->header.typeId);
        if (foundBits != impl_->typeObservableBits.end() &&
            index < foundBits->second.size()) {
            observableBit = foundBits->second[index];
        } else if (object->header.uiObserverCount > 0) {
            observableBit = static_cast<std::uint8_t>(index < 63u ? index : 63u);
        }
    } else if (object->header.uiObserverCount > 0) {
        observableBit = static_cast<std::uint8_t>(index < 63u ? index : 63u);
    }
    if (observableBit != 0xFFu) {
        const std::uint8_t safeBit = observableBit < 63u ? observableBit : 63u;
        object->header.dirtyFieldMask |= (1ULL << safeBit);
        if (!object->header.inDirtyQueue) {
            object->header.inDirtyQueue = true;
            impl_->dirtyObjects.push_back(reference);
        }
    }
    return true;
}

void ManagedHeap::registerTypeObservableBits(
    semantic::SymbolId typeId,
    std::vector<std::uint8_t> fieldObservableBits) {
    if (typeId == 0) return;
    impl_->typeObservableBits[typeId] = std::move(fieldObservableBits);
}

void ManagedHeap::registerProgramObservableTypes(const ProgramImage& program) {
    for (const auto& mod : program.modules()) {
        for (const auto& type : mod.types) {
            if (type.kind != semantic::TypeKind::Class ||
                (!type.observable && type.nextObservableBitIndex == 0)) {
                continue;
            }
            if (impl_->typeObservableBits.find(type.id) != impl_->typeObservableBits.end()) {
                continue;
            }
            std::vector<std::uint8_t> bits(type.fields.size(), 0xFFu);
            bool anyObservable = false;
            for (const auto& f : type.fields) {
                if (f.observable && f.index < bits.size()) {
                    bits[f.index] = static_cast<std::uint8_t>(
                        f.observableBitIndex < 63u ? f.observableBitIndex : 63u);
                    anyObservable = true;
                }
            }
            if (anyObservable) {
                impl_->typeObservableBits.emplace(type.id, std::move(bits));
            }
        }
    }
}

std::uint64_t ManagedHeap::dirtyFieldMask(ObjectRef reference) const noexcept {
    const auto* object = impl_->get(reference);
    if (!object || object->header.kind != ObjectKind::Record) return 0;
    return object->header.dirtyFieldMask;
}

void ManagedHeap::clearDirtyFieldMask(ObjectRef reference) noexcept {
    if (auto* object = impl_->get(reference)) {
        object->header.dirtyFieldMask = 0;
        object->header.inDirtyQueue = false;
    }
}

void ManagedHeap::addUiObserver(ObjectRef reference) noexcept {
    if (auto* object = impl_->get(reference)) {
        if (object->header.uiObserverCount < 0xFFFFu) {
            ++object->header.uiObserverCount;
        }
    }
}

void ManagedHeap::removeUiObserver(ObjectRef reference) noexcept {
    if (auto* object = impl_->get(reference)) {
        if (object->header.uiObserverCount > 0) {
            --object->header.uiObserverCount;
        }
    }
}

std::uint16_t ManagedHeap::uiObserverCount(ObjectRef reference) const noexcept {
    const auto* object = impl_->get(reference);
    return object ? object->header.uiObserverCount : 0;
}

std::vector<ObjectRef> ManagedHeap::consumeDirtyObjects() {
    std::vector<ObjectRef> result;
    result.reserve(impl_->dirtyObjects.size());
    for (const auto& ref : impl_->dirtyObjects) {
        if (auto* obj = impl_->get(ref)) {
            obj->header.inDirtyQueue = false;
            if (obj->header.dirtyFieldMask != 0) {
                result.push_back(ref);
            }
        }
    }
    impl_->dirtyObjects.clear();
    return result;
}

std::optional<ObjectRef> ManagedHeap::allocateWeakDelegate(
    semantic::SymbolId delegateTypeId,
    ObjectRef target,
    semantic::SymbolId functionId,
    std::int64_t baseFlags,
    RuntimeError* error) {
    if (delegateTypeId == 0) {
        setHeapError(error, ErrorCode::InvalidArguments,
            "delegate type ID must be non-zero");
        return std::nullopt;
    }
    const auto loWord = static_cast<std::int64_t>(static_cast<std::uint32_t>(functionId));
    const auto hiWord = static_cast<std::int64_t>(static_cast<std::uint32_t>(functionId >> 32u));
    std::vector<Value> fields{
        Value{target},
        Value{loWord},
        Value{hiWord},
        Value{(baseFlags & 3) | DelegateWeakTargetFlag},
        Value{std::int64_t{0}},
        Value{std::int64_t{0}},
        Value{std::int64_t{0}},
        Value{NullObject{}},
    };
    const auto storage = estimateValueStorage(fields.size());
    return impl_->allocateValuesSlot(
        ObjectKind::Record,
        delegateTypeId,
        semantic::PrimitiveType::Error,
        0,
        std::move(fields),
        std::make_shared<const std::vector<std::size_t>>(std::vector<std::size_t>{7u}),
        Impl::BaseObjectSizeBytes + storage,
        error);
}

Value ManagedHeap::pruneDelegateChain(const Value& delegateVal) {
    if (std::holds_alternative<NullObject>(delegateVal)) return delegateVal;
    const auto* ref = std::get_if<ObjectRef>(&delegateVal);
    if (!ref || !isAlive(*ref) || fieldCount(*ref) != 8u) return Value{NullObject{}};
    const auto* flagsVal = fieldGetFast(*ref, 3u);
    const auto* flags = flagsVal ? std::get_if<std::int64_t>(flagsVal) : nullptr;
    if (!flags) return Value{NullObject{}};
    const std::int64_t mode = (*flags & 7);
    const bool isWeak = ((*flags & DelegateWeakTargetFlag) != 0);
    if (mode != 4) {
        if (isWeak) {
            const auto* targetVal = fieldGetFast(*ref, 0u);
            if (const auto* targetRef = targetVal ? std::get_if<ObjectRef>(targetVal) : nullptr) {
                if (!isAlive(*targetRef)) {
                    return Value{NullObject{}};
                }
            }
        }
        return delegateVal;
    }
    const Value leftVal = fieldGet(*ref, 0u).value_or(Value{NullObject{}});
    const Value rightVal = fieldGet(*ref, 7u).value_or(Value{NullObject{}});
    const Value prunedLeft = pruneDelegateChain(leftVal);
    const Value prunedRight = pruneDelegateChain(rightVal);
    const bool leftNull = std::holds_alternative<NullObject>(prunedLeft);
    const bool rightNull = std::holds_alternative<NullObject>(prunedRight);
    if (leftNull && rightNull) return Value{NullObject{}};
    if (leftNull) return prunedRight;
    if (rightNull) return prunedLeft;
    if (!(prunedLeft == leftVal)) (void)fieldSet(*ref, 0u, prunedLeft);
    if (!(prunedRight == rightVal)) (void)fieldSet(*ref, 7u, prunedRight);
    return delegateVal;
}

ManagedHeap::RootToken ManagedHeap::addPersistentRoot(ObjectRef reference) {
    return addPersistentRoot(Value{reference});
}

ManagedHeap::RootToken ManagedHeap::addPersistentRoot(Value value) {
    if (!impl_->validRootValue(value)) return 0;
    auto token = impl_->nextRootToken++;
    if (token == 0) token = impl_->nextRootToken++;
    pinValueInPools(value);
    impl_->persistentRoots.emplace(token, std::move(value));
    return token;
}

bool ManagedHeap::updatePersistentRoot(
    RootToken token,
    ObjectRef reference) {
    return updatePersistentRoot(token, Value{reference});
}

bool ManagedHeap::updatePersistentRoot(RootToken token, Value value) {
    if (token == 0 || !impl_->validRootValue(value)) return false;
    const auto found = impl_->persistentRoots.find(token);
    if (found == impl_->persistentRoots.end()) return false;
    pinValueInPools(value);
    found->second = std::move(value);
    return true;
}

bool ManagedHeap::removePersistentRoot(RootToken token) noexcept {
    return impl_->persistentRoots.erase(token) != 0;
}

PersistentRoot ManagedHeap::retain(ObjectRef reference) {
    return retain(Value{reference});
}

PersistentRoot ManagedHeap::retain(Value value) {
    const auto token = addPersistentRoot(value);
    return PersistentRoot(token == 0 ? nullptr : this, token, std::move(value));
}

bool ManagedHeap::hasStaticField(semantic::SymbolId id) const noexcept {
    return impl_->staticFields.find(id) != impl_->staticFields.end();
}

std::optional<Value> ManagedHeap::getStaticField(semantic::SymbolId id) const {
    const auto found = impl_->staticFields.find(id);
    if (found == impl_->staticFields.end()) return std::nullopt;
    return found->second;
}

void ManagedHeap::setStaticField(semantic::SymbolId id, Value value) {
    if (impl_->phase != GcPhase::Idle) {
        impl_->markValue(value);
        if (impl_->phase == GcPhase::Sweep && !impl_->markStack.empty()) {
            impl_->phase = GcPhase::Mark;
            impl_->resumeSweep = true;
        }
    }
    pinValueInPools(value);
    impl_->staticFields[id] = std::move(value);
}

bool ManagedHeap::removeStaticField(semantic::SymbolId id) noexcept {
    return impl_->staticFields.erase(id) != 0;
}

std::vector<semantic::SymbolId> ManagedHeap::staticFieldIds() const {
    std::vector<semantic::SymbolId> ids;
    ids.reserve(impl_->staticFields.size());
    for (const auto& [id, val] : impl_->staticFields) {
        (void)val;
        ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

bool ManagedHeap::setNativeEcsComponent(
    std::string_view canonicalTypeName,
    std::int64_t entityId,
    Value component) {
    if (impl_->phase != GcPhase::Idle) {
        impl_->markValue(component);
        if (impl_->phase == GcPhase::Sweep && !impl_->markStack.empty()) {
            impl_->phase = GcPhase::Mark;
            impl_->resumeSweep = true;
        }
    }
    pinValueInPools(component);
    auto& table = impl_->nativeEcsTables[std::string(canonicalTypeName)];
    if (Impl::valueMayContainReferences(component)) {
        table.mayContainReferences = true;
    }
    const auto it = std::lower_bound(table.entityIds.begin(), table.entityIds.end(), entityId);
    const auto idx = static_cast<std::size_t>(std::distance(table.entityIds.begin(), it));
    if (it != table.entityIds.end() && *it == entityId) {
        table.values[idx] = std::move(component);
        return true;
    }
    table.entityIds.insert(it, entityId);
    table.values.insert(table.values.begin() + static_cast<std::ptrdiff_t>(idx), std::move(component));
    return true;
}

std::optional<Value> ManagedHeap::getNativeEcsComponent(
    std::string_view canonicalTypeName,
    std::int64_t entityId) const {
    const auto found = impl_->nativeEcsTables.find(std::string(canonicalTypeName));
    if (found == impl_->nativeEcsTables.end()) return std::nullopt;
    const auto& table = found->second;
    const auto it = std::lower_bound(table.entityIds.begin(), table.entityIds.end(), entityId);
    if (it == table.entityIds.end() || *it != entityId) return std::nullopt;
    const auto idx = static_cast<std::size_t>(std::distance(table.entityIds.begin(), it));
    return table.values[idx];
}

bool ManagedHeap::hasNativeEcsComponent(
    std::string_view canonicalTypeName,
    std::int64_t entityId) const noexcept {
    const auto found = impl_->nativeEcsTables.find(std::string(canonicalTypeName));
    if (found == impl_->nativeEcsTables.end()) return false;
    const auto& table = found->second;
    return std::binary_search(table.entityIds.begin(), table.entityIds.end(), entityId);
}

bool ManagedHeap::removeNativeEcsComponent(
    std::string_view canonicalTypeName,
    std::int64_t entityId) {
    const auto found = impl_->nativeEcsTables.find(std::string(canonicalTypeName));
    if (found == impl_->nativeEcsTables.end()) return false;
    auto& table = found->second;
    const auto it = std::lower_bound(table.entityIds.begin(), table.entityIds.end(), entityId);
    if (it == table.entityIds.end() || *it != entityId) return false;
    const auto idx = static_cast<std::size_t>(std::distance(table.entityIds.begin(), it));
    table.entityIds.erase(it);
    table.values.erase(table.values.begin() + static_cast<std::ptrdiff_t>(idx));
    return true;
}

bool ManagedHeap::destroyNativeEcsEntity(std::int64_t entityId) {
    bool removedAny = false;
    for (auto& [typeName, table] : impl_->nativeEcsTables) {
        (void)typeName;
        const auto it = std::lower_bound(table.entityIds.begin(), table.entityIds.end(), entityId);
        if (it != table.entityIds.end() && *it == entityId) {
            const auto idx = static_cast<std::size_t>(std::distance(table.entityIds.begin(), it));
            table.entityIds.erase(it);
            table.values.erase(table.values.begin() + static_cast<std::ptrdiff_t>(idx));
            removedAny = true;
        }
    }
    return removedAny;
}

bool ManagedHeap::hasNativeEcsTable(std::string_view canonicalTypeName) const noexcept {
    const auto found = impl_->nativeEcsTables.find(std::string(canonicalTypeName));
    return found != impl_->nativeEcsTables.end();
}

std::vector<std::string> ManagedHeap::nativeEcsTableNames() const {
    std::vector<std::string> names;
    names.reserve(impl_->nativeEcsTables.size());
    for (const auto& [name, table] : impl_->nativeEcsTables) {
        (void)table;
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

void ManagedHeap::clearNativeEcsTables() noexcept {
    impl_->nativeEcsTables.clear();
}

const std::vector<std::int64_t>* ManagedHeap::nativeEcsEntityIds(
    std::string_view canonicalTypeName) const noexcept {
    const auto found = impl_->nativeEcsTables.find(std::string(canonicalTypeName));
    return found != impl_->nativeEcsTables.end() ? &found->second.entityIds : nullptr;
}

const std::vector<Value>* ManagedHeap::nativeEcsComponentValues(
    std::string_view canonicalTypeName) const noexcept {
    const auto found = impl_->nativeEcsTables.find(std::string(canonicalTypeName));
    return found != impl_->nativeEcsTables.end() ? &found->second.values : nullptr;
}

bool ManagedHeap::setNativeEcsComponentByDenseIndex(
    std::string_view canonicalTypeName,
    std::size_t denseIndex,
    Value component) {
    const auto found = impl_->nativeEcsTables.find(std::string(canonicalTypeName));
    if (found == impl_->nativeEcsTables.end() || denseIndex >= found->second.values.size()) {
        return false;
    }
    if (impl_->phase != GcPhase::Idle) {
        impl_->markValue(component);
        if (impl_->phase == GcPhase::Sweep && !impl_->markStack.empty()) {
            impl_->phase = GcPhase::Mark;
            impl_->resumeSweep = true;
        }
    }
    if (Impl::valueMayContainReferences(component)) {
        found->second.mayContainReferences = true;
    }
    pinValueInPools(component);
    found->second.values[denseIndex] = std::move(component);
    return true;
}

std::optional<ObjectRef> ManagedHeap::allocateRawColumnArray(
    semantic::SymbolId arrayTypeId,
    semantic::SymbolId elementTypeId,
    NativeRawColumnView view,
    RuntimeError* error) {
    if (!validateRawColumnView(view, elementTypeId)) {
        setHeapError(error, ErrorCode::InvalidArguments,
            "raw ECS column view metadata is invalid");
        return std::nullopt;
    }
    const auto slotIndex = impl_->acquireSlot(Impl::BaseObjectSizeBytes, error);
    if (!slotIndex) return std::nullopt;
    auto& object = impl_->slots[*slotIndex].object.storage;
    object.clearValues();
    object.stringPayload.clear();
    object.rawColumnView = std::move(view);
    return impl_->finalizeSlotAllocation(
        *slotIndex,
        ObjectKind::Array,
        arrayTypeId,
        semantic::PrimitiveType::Struct,
        elementTypeId,
        nullptr,
        Impl::BaseObjectSizeBytes);
}

bool ManagedHeap::updateRawColumnArray(
    ObjectRef reference,
    NativeRawColumnView view) noexcept {
    auto* object = impl_->get(reference);
    if (!object || object->header.kind != ObjectKind::Array ||
        !object->rawColumnView.has_value() ||
        !validateRawColumnView(view, object->header.elementTypeId)) {
        return false;
    }
    object->rawColumnView = std::move(view);
    return true;
}

bool ManagedHeap::isRawColumnArray(ObjectRef reference) const noexcept {
    const auto* object = impl_->get(reference);
    return object && object->header.kind == ObjectKind::Array &&
        object->rawColumnView.has_value();
}

bool ManagedHeap::hasSparseEcsState() const noexcept {
    return !impl_->nativeEcsTables.empty() || impl_->hasSparseEcsStaticFields;
}

void ManagedHeap::notifySparseEcsState() noexcept {
    impl_->hasSparseEcsStaticFields = true;
}

const NativeRawColumnView* ManagedHeap::rawColumnView(ObjectRef reference) const noexcept {
    const auto* object = impl_->get(reference);
    if (!object || object->header.kind != ObjectKind::Array ||
        !object->rawColumnView.has_value()) {
        return nullptr;
    }
    return &*object->rawColumnView;
}

bool ManagedHeap::readRawColumnFields(
    ObjectRef reference,
    std::size_t index,
    Value* outFields,
    std::size_t maxFields,
    std::uint8_t& outFieldCount) const noexcept {
    const auto* object = impl_->get(reference);
    if (!object || object->header.kind != ObjectKind::Array ||
        !object->rawColumnView.has_value()) {
        return false;
    }
    const auto& view = *object->rawColumnView;
    if (index >= view.count || !view.basePtr || view.fields.size() > maxFields) {
        return false;
    }
    const auto* elemBase = static_cast<const std::uint8_t*>(view.basePtr) + index * view.strideBytes;
    const auto count = view.fields.size();
    for (std::size_t i = 0; i < count; ++i) {
        outFields[i] = readNativeFieldFromBytes(elemBase, view.fields[i]);
    }
    outFieldCount = static_cast<std::uint8_t>(count);
    return true;
}

bool ManagedHeap::writeRawColumnFields(
    ObjectRef reference,
    std::size_t index,
    const Value* fields,
    std::size_t fieldCount,
    RuntimeError* error) noexcept {
    auto* object = impl_->get(reference);
    if (!object || object->header.kind != ObjectKind::Array ||
        !object->rawColumnView.has_value()) {
        return false;
    }
    const auto& view = *object->rawColumnView;
    if (index >= view.count) {
        setHeapError(error, ErrorCode::IndexOutOfRange,
            "array element index is out of range");
        return false;
    }
    if (view.readOnly || !view.basePtr) {
        setHeapError(error, ErrorCode::CapabilityDenied,
            "cannot write to a read-only native ECS column view");
        return false;
    }
    if (fieldCount != view.fields.size()) {
        setHeapError(error, ErrorCode::TypeMismatch,
            "raw ECS column field count mismatch");
        return false;
    }
    auto* elemBase = static_cast<std::uint8_t*>(view.basePtr) + index * view.strideBytes;
    for (std::size_t i = 0; i < fieldCount; ++i) {
        if (!writeNativeFieldToBytes(elemBase, view.fields[i], fields[i])) {
            setHeapError(error, ErrorCode::TypeMismatch,
                "raw ECS column field type mismatch");
            return false;
        }
    }
    return true;
}

void ManagedHeap::requestCollection() noexcept { impl_->requested = true; }
bool ManagedHeap::collectionRequested() const noexcept { return impl_->requested; }
GcPhase ManagedHeap::phase() const noexcept { return impl_->phase; }

std::size_t ManagedHeap::step(
    const ShadowStack& shadowStack,
    std::size_t workBudget) {
    if (workBudget == 0) return 0;
    if (impl_->phase == GcPhase::Idle) {
        if (!impl_->requested) return 0;
        impl_->startCollection(shadowStack);
    } else if (impl_->phase == GcPhase::Mark) {
        impl_->markRoots(shadowStack);
    }

    std::size_t performed = 0;
    while (performed < workBudget) {
        if (impl_->phase == GcPhase::Sweep && !impl_->markStack.empty()) {
            impl_->phase = GcPhase::Mark;
            impl_->resumeSweep = true;
            impl_->rootsRescannedForSweep = false;
        }
        if (impl_->phase == GcPhase::Mark) {
            if (impl_->markStack.empty()) {
                if (!impl_->rootsRescannedForSweep) {
                    impl_->markRoots(shadowStack);
                    impl_->rootsRescannedForSweep = true;
                    if (!impl_->markStack.empty()) continue;
                }
                impl_->phase = GcPhase::Sweep;
                if (!impl_->resumeSweep) impl_->sweepIndex = 0;
                impl_->resumeSweep = false;
                continue;
            }
            const auto reference = impl_->markStack.back();
            impl_->markStack.pop_back();
            impl_->scan(reference);
            ++impl_->statistics.markWork;
            ++performed;
            continue;
        }

        if (impl_->phase == GcPhase::Sweep) {
            if (impl_->sweepIndex >= impl_->sweepLimit) {
                impl_->finishCollection(shadowStack);
                break;
            }
            auto& slot = impl_->slots[impl_->sweepIndex++];
            if (slot.object && !slot.object->header.marked) {
                if (slot.object->header.kind == ObjectKind::String) {
                    const auto& s = slot.object->stringPayload;
                    const auto it = impl_->internedStrings.find(s);
                    if (it != impl_->internedStrings.end() &&
                        it->second.slot == static_cast<std::uint32_t>(impl_->sweepIndex - 1) &&
                        it->second.generation == slot.generation) {
                        impl_->internedStrings.erase(it);
                    }
                }
                const auto bytes = slot.object->header.sizeBytes;
                slot.object.reset();
                ++slot.generation;
                if (slot.generation == 0) slot.generation = 1;
                impl_->freeSlots.push_back(
                    static_cast<std::uint32_t>(impl_->sweepIndex - 1));
                --impl_->statistics.liveObjects;
                impl_->statistics.liveBytes -= bytes;
                ++impl_->statistics.objectsReclaimed;
                impl_->statistics.bytesReclaimed += bytes;
            }
            ++impl_->statistics.sweepWork;
            ++performed;
            continue;
        }
        break;
    }
    return performed;
}

std::size_t ManagedHeap::stepIdleForDuration(
    const ShadowStack& shadowStack,
    double maxMilliseconds) {
    if (maxMilliseconds <= 0.0) return 0;
    const auto start = std::chrono::steady_clock::now();
    std::size_t totalWork = 0;
    if (impl_->phase == GcPhase::Idle && !impl_->requested && !impl_->nurserySlots.empty()) {
        totalWork += collectYoung(shadowStack, 64);
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double, std::milli>(now - start).count() >= maxMilliseconds) {
            return totalWork;
        }
    }
    while (impl_->phase != GcPhase::Idle || impl_->requested) {
        totalWork += step(shadowStack, 32);
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double, std::milli>(now - start).count() >= maxMilliseconds) {
            break;
        }
    }
    return totalWork;
}

void ManagedHeap::collectFull(const ShadowStack& shadowStack) {
    const auto drainCollection = [&]() {
        while (impl_->phase != GcPhase::Idle || impl_->requested) {
            const auto budget = std::max<std::size_t>(64, impl_->slots.size() + 1);
            (void)step(shadowStack, budget);
        }
    };

    // Objects allocated during an incremental cycle are kept alive until that
    // cycle finishes. Drain it first, then start a fresh cycle so collectFull()
    // observes the complete heap as it exists at the call boundary.
    if (impl_->phase != GcPhase::Idle) drainCollection();
    requestCollection();
    drainCollection();
}

std::size_t ManagedHeap::nurserySize() const noexcept {
    return impl_->nurserySlots.size();
}

std::size_t ManagedHeap::collectYoung(
    const ShadowStack& shadowStack,
    std::size_t workBudget) {
    if (impl_->phase != GcPhase::Idle) {
        if (workBudget != 0) {
            return step(shadowStack, workBudget);
        }
        std::size_t totalWork = 0;
        while (impl_->phase != GcPhase::Idle) {
            const auto budget = std::max<std::size_t>(64, impl_->slots.size() + 1);
            totalWork += step(shadowStack, budget);
        }
        return totalWork;
    }
    if (impl_->nurserySlots.empty()) return 0;

    for (const auto slotIdx : impl_->nurserySlots) {
        if (slotIdx < impl_->slots.size() && impl_->slots[slotIdx].object) {
            impl_->slots[slotIdx].object->header.marked = false;
        }
    }

    std::vector<ObjectRef> youngQueue;
    std::unordered_set<const StructStorage*> visitedStructs;
    const auto markYoungValue = [&](const auto& self, const Value& val) -> void {
        if (const auto* ref = std::get_if<ObjectRef>(&val)) {
            auto* obj = impl_->get(*ref);
            if (obj && obj->header.inNursery && !obj->header.marked) {
                obj->header.marked = true;
                youngQueue.push_back(*ref);
            }
            return;
        }
        if (const auto* structure = std::get_if<StructValue>(&val);
            structure && structure->storage &&
            visitedStructs.insert(structure->storage.get()).second) {
            for (const auto& field : structure->storage->fields) {
                self(self, field);
            }
        }
    };

    shadowStack.visitRoots([&](const Value& val) { markYoungValue(markYoungValue, val); });
    for (const auto& entry : impl_->persistentRoots) markYoungValue(markYoungValue, entry.second);
    for (const auto& entry : impl_->staticFields) markYoungValue(markYoungValue, entry.second);
    for (const auto& [typeName, table] : impl_->nativeEcsTables) {
        (void)typeName;
        if (!table.mayContainReferences) continue;
        for (const auto& v : table.values) markYoungValue(markYoungValue, v);
    }
    for (const auto& oldRef : impl_->rememberedSet) {
        const auto* oldObj = impl_->get(oldRef);
        if (!oldObj || oldObj->header.kind == ObjectKind::String) continue;
        const auto count = oldObj->valueCount();
        if (oldObj->header.kind == ObjectKind::Array) {
            for (std::size_t i = 0; i < count; ++i) markYoungValue(markYoungValue, oldObj->valueAt(i));
        } else if (oldObj->referenceFields) {
            for (const auto idx : *oldObj->referenceFields) {
                if (idx < count) markYoungValue(markYoungValue, oldObj->valueAt(idx));
            }
        }
    }

    while (!youngQueue.empty()) {
        const auto ref = youngQueue.back();
        youngQueue.pop_back();
        const auto* obj = impl_->get(ref);
        if (!obj || obj->header.kind == ObjectKind::String) continue;
        const auto count = obj->valueCount();
        if (obj->header.kind == ObjectKind::Array) {
            for (std::size_t i = 0; i < count; ++i) markYoungValue(markYoungValue, obj->valueAt(i));
        } else if (obj->referenceFields) {
            for (const auto idx : *obj->referenceFields) {
                if (idx < count) markYoungValue(markYoungValue, obj->valueAt(idx));
            }
        }
        ++impl_->statistics.markWork;
    }

    std::size_t processed = 0;
    for (const auto slotIdx : impl_->nurserySlots) {
        if (slotIdx >= impl_->slots.size()) continue;
        auto& slot = impl_->slots[slotIdx];
        if (!slot.object || !slot.object->header.inNursery) continue;
        ++processed;
        if (!slot.object->header.marked) {
            if (slot.object->header.kind == ObjectKind::String) {
                const auto& s = slot.object->stringPayload;
                const auto it = impl_->internedStrings.find(s);
                if (it != impl_->internedStrings.end() &&
                    it->second.slot == slotIdx &&
                    it->second.generation == slot.generation) {
                    impl_->internedStrings.erase(it);
                }
            }
            const auto bytes = slot.object->header.sizeBytes;
            slot.object.reset();
            ++slot.generation;
            if (slot.generation == 0) slot.generation = 1;
            impl_->freeSlots.push_back(slotIdx);
            --impl_->statistics.liveObjects;
            impl_->statistics.liveBytes -= bytes;
            ++impl_->statistics.objectsReclaimed;
            ++impl_->statistics.youngObjectsReclaimed;
            impl_->statistics.bytesReclaimed += bytes;
        } else {
            slot.object->header.inNursery = false;
            slot.object->header.marked = false;
            ++impl_->statistics.objectsPromoted;
        }
        ++impl_->statistics.sweepWork;
    }

    impl_->nurserySlots.clear();
    for (const auto& ref : impl_->rememberedSet) {
        if (auto* obj = impl_->get(ref)) {
            obj->header.inRememberedSet = false;
        }
    }
    impl_->rememberedSet.clear();
    ++impl_->statistics.youngCollections;
    if (impl_->statistics.liveBytes < impl_->nextCollectionThreshold) {
        impl_->requested = false;
    }
    (void)reclaimUnreferencedPoolValues(&shadowStack);
    return processed;
}

bool ManagedHeap::readPackedIntArray(
    ObjectRef reference,
    std::vector<std::int64_t>& out,
    RuntimeError* error) const {
    const auto* object = impl_->get(reference);
    if (!object || object->header.kind != ObjectKind::Array) {
        setHeapError(error, ErrorCode::InvalidObjectReference, "managed array reference is invalid");
        return false;
    }
    const auto& values = object->values;
    out.resize(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (const auto* iv = std::get_if<std::int64_t>(&values[i])) {
            out[i] = *iv;
        } else if (const auto* lv = std::get_if<LongValue>(&values[i])) {
            out[i] = lv->value;
        } else {
            setHeapError(error, ErrorCode::TypeMismatch, "array element is not an integer");
            return false;
        }
    }
    return true;
}

bool ManagedHeap::writePackedIntArray(
    ObjectRef reference,
    const std::int64_t* data,
    std::size_t count,
    RuntimeError* error) {
    if (count > 0 && !data) {
        setHeapError(error, ErrorCode::InvalidArguments, "bulk write source buffer is null");
        return false;
    }
    auto* object = impl_->get(reference);
    if (!object || object->header.kind != ObjectKind::Array) {
        setHeapError(error, ErrorCode::InvalidObjectReference, "managed array reference is invalid");
        return false;
    }
    if (object->header.elementType != semantic::PrimitiveType::Int &&
        object->header.elementType != semantic::PrimitiveType::Long &&
        object->header.elementType != semantic::PrimitiveType::Error) {
        setHeapError(error, ErrorCode::TypeMismatch, "array element is not an integer");
        return false;
    }
    auto& values = object->values;
    if (count > values.size()) {
        setHeapError(error, ErrorCode::IndexOutOfRange, "bulk write exceeds array length");
        return false;
    }
    const bool asLong = object->header.elementType == semantic::PrimitiveType::Long;
    for (std::size_t i = 0; i < count; ++i) {
        values[i] = asLong ? Value{LongValue{data[i]}} : Value{data[i]};
    }
    return true;
}

bool ManagedHeap::readPackedDoubleArray(
    ObjectRef reference,
    std::vector<double>& out,
    RuntimeError* error) const {
    const auto* object = impl_->get(reference);
    if (!object || object->header.kind != ObjectKind::Array) {
        setHeapError(error, ErrorCode::InvalidObjectReference, "managed array reference is invalid");
        return false;
    }
    const auto& values = object->values;
    out.resize(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (const auto* dv = std::get_if<double>(&values[i])) {
            out[i] = *dv;
        } else if (const auto* fv = std::get_if<FloatValue>(&values[i])) {
            out[i] = static_cast<double>(fv->value);
        } else {
            setHeapError(error, ErrorCode::TypeMismatch, "array element is not a floating-point number");
            return false;
        }
    }
    return true;
}

bool ManagedHeap::writePackedDoubleArray(
    ObjectRef reference,
    const double* data,
    std::size_t count,
    RuntimeError* error) {
    if (count > 0 && !data) {
        setHeapError(error, ErrorCode::InvalidArguments, "bulk write source buffer is null");
        return false;
    }
    auto* object = impl_->get(reference);
    if (!object || object->header.kind != ObjectKind::Array) {
        setHeapError(error, ErrorCode::InvalidObjectReference, "managed array reference is invalid");
        return false;
    }
    if (object->header.elementType != semantic::PrimitiveType::Double &&
        object->header.elementType != semantic::PrimitiveType::Float &&
        object->header.elementType != semantic::PrimitiveType::Error) {
        setHeapError(error, ErrorCode::TypeMismatch, "array element is not a floating-point number");
        return false;
    }
    auto& values = object->values;
    if (count > values.size()) {
        setHeapError(error, ErrorCode::IndexOutOfRange, "bulk write exceeds array length");
        return false;
    }
    const bool asFloat = object->header.elementType == semantic::PrimitiveType::Float;
    for (std::size_t i = 0; i < count; ++i) {
        values[i] = asFloat ? Value{FloatValue{static_cast<float>(data[i])}} : Value{data[i]};
    }
    return true;
}

bool ManagedHeap::readPackedVec2Array(
    ObjectRef reference,
    std::vector<Vec2Value>& out,
    RuntimeError* error) const {
    const auto* object = impl_->get(reference);
    if (!object || object->header.kind != ObjectKind::Array) {
        setHeapError(error, ErrorCode::InvalidObjectReference, "managed array reference is invalid");
        return false;
    }
    const auto& values = object->values;
    out.resize(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (const auto* v2 = std::get_if<Vec2Value>(&values[i])) {
            out[i] = *v2;
        } else {
            const auto fx = loadInlineStructField(values[i], 0);
            const auto fy = loadInlineStructField(values[i], 1);
            if (fx && fy && std::holds_alternative<FixedValue>(*fx) && std::holds_alternative<FixedValue>(*fy)) {
                out[i] = Vec2Value{std::get<FixedValue>(*fx).raw, std::get<FixedValue>(*fy).raw};
            } else {
                setHeapError(error, ErrorCode::TypeMismatch, "array element is not a Vec2");
                return false;
            }
        }
    }
    return true;
}

bool ManagedHeap::writePackedVec2Array(
    ObjectRef reference,
    const Vec2Value* data,
    std::size_t count,
    RuntimeError* error) {
    if (count > 0 && !data) {
        setHeapError(error, ErrorCode::InvalidArguments, "bulk write source buffer is null");
        return false;
    }
    auto* object = impl_->get(reference);
    if (!object || object->header.kind != ObjectKind::Array) {
        setHeapError(error, ErrorCode::InvalidObjectReference, "managed array reference is invalid");
        return false;
    }
    if (object->header.elementType != semantic::PrimitiveType::Error &&
        !(object->header.elementType == semantic::PrimitiveType::Struct &&
          object->header.elementTypeId == vec2TypeId())) {
        setHeapError(error, ErrorCode::TypeMismatch, "array element is not a Vec2");
        return false;
    }
    auto& values = object->values;
    if (count > values.size()) {
        setHeapError(error, ErrorCode::IndexOutOfRange, "bulk write exceeds array length");
        return false;
    }
    for (std::size_t i = 0; i < count; ++i) {
        values[i] = Value{data[i]};
    }
    return true;
}

const GcStatistics& ManagedHeap::statistics() const noexcept {
    return impl_->statistics;
}
std::size_t ManagedHeap::liveObjects() const noexcept {
    return impl_->statistics.liveObjects;
}
std::size_t ManagedHeap::liveBytes() const noexcept {
    return impl_->statistics.liveBytes;
}

std::uint64_t ManagedHeap::heapId() const noexcept { return impl_->heapId; }

HeapSnapshot ManagedHeap::snapshot(const ShadowStack* shadowStack) const {
    HeapSnapshot result;
    result.heapId = impl_->heapId;
    result.statistics = impl_->statistics;
    result.objects.reserve(impl_->statistics.liveObjects);
    for (std::size_t slotIndex = 0; slotIndex < impl_->slots.size(); ++slotIndex) {
        const auto& slot = impl_->slots[slotIndex];
        if (!slot.object) continue;
        const auto& object = *slot.object;
        HeapObjectInfo info;
        info.reference = {
            static_cast<std::uint32_t>(slotIndex),
            slot.generation,
            object.header.kind,
            impl_->heapId,
        };
        info.kind = object.header.kind;
        info.typeId = object.header.typeId;
        info.elementType = object.header.elementType;
        info.elementTypeId = object.header.elementTypeId;
        info.sizeBytes = object.header.sizeBytes;
        if (object.header.kind == ObjectKind::String) {
            info.stringValue = object.stringPayload;
        } else {
            const auto count = object.valueCount();
            info.valueCount = count;
            info.values.resize(count);
            for (std::size_t i = 0; i < count; ++i) {
                info.values[i] = object.valueAt(i);
            }
            const auto& values = info.values;
            if (object.referenceFields) {
                info.referenceFields = *object.referenceFields;
            }
            const auto addEdge = [&](std::size_t index) {
                if (index >= values.size()) return;
                std::unordered_set<const StructStorage*> visitedStructs;
                const auto addValueEdges = [&](const auto& self,
                                               const Value& value,
                                               const std::string& label) -> void {
                    if (const auto* target = std::get_if<ObjectRef>(&value);
                        target && impl_->get(*target)) {
                        info.edges.push_back({*target, label});
                        return;
                    }
                    if (const auto* structure = std::get_if<StructValue>(&value);
                        structure && structure->storage &&
                        visitedStructs.insert(structure->storage.get()).second) {
                        for (std::size_t field = 0;
                             field < structure->storage->fields.size(); ++field) {
                            self(self, structure->storage->fields[field],
                                label + ".struct[" + std::to_string(field) + "]");
                        }
                    }
                };
                addValueEdges(addValueEdges, values[index],
                    (object.header.kind == ObjectKind::Array ? "element[" : "field[") +
                        std::to_string(index) + "]");
            };
            if (object.header.kind == ObjectKind::Array) {
                for (std::size_t index = 0; index < values.size(); ++index) {
                    addEdge(index);
                }
            } else if (object.referenceFields) {
                for (const auto index : *object.referenceFields) addEdge(index);
            }
        }
        result.objects.push_back(std::move(info));
    }
    const auto appendRoots = [&](const Value& value,
                                 std::uint64_t token,
                                 const std::string& kind) {
        std::unordered_set<const StructStorage*> visitedStructs;
        const auto addRoots = [&](const auto& self, const Value& current) -> void {
            if (const auto* reference = std::get_if<ObjectRef>(&current);
                reference && impl_->get(*reference)) {
                result.roots.push_back({token, *reference, kind});
                return;
            }
            if (const auto* structure = std::get_if<StructValue>(&current);
                structure && structure->storage &&
                visitedStructs.insert(structure->storage.get()).second) {
                for (const auto& field : structure->storage->fields) {
                    self(self, field);
                }
            }
        };
        addRoots(addRoots, value);
    };
    for (const auto& [token, value] : impl_->persistentRoots) {
        appendRoots(value, token, "persistent");
    }
    for (const auto& [symId, value] : impl_->staticFields) {
        appendRoots(value, symId, "static");
    }
    {
        std::uint64_t ecsRootIndex = 0;
        for (const auto& [typeName, table] : impl_->nativeEcsTables) {
            (void)typeName;
            if (!table.mayContainReferences) continue;
            for (const auto& v : table.values) {
                appendRoots(v, ++ecsRootIndex, "ecs");
            }
        }
    }
    if (shadowStack) {
        std::uint64_t index = 0;
        shadowStack->visitRoots([&](const Value& value) {
            appendRoots(value, index++, "shadow");
        });
    }
    std::sort(result.objects.begin(), result.objects.end(),
        [](const HeapObjectInfo& left, const HeapObjectInfo& right) {
            return left.reference.slot < right.reference.slot;
        });
    std::sort(result.roots.begin(), result.roots.end(),
        [](const HeapRootInfo& left, const HeapRootInfo& right) {
            if (left.kind != right.kind) return left.kind < right.kind;
            if (left.token != right.token) return left.token < right.token;
            return left.reference.slot < right.reference.slot;
        });
    return result;
}

bool ManagedHeap::restore(
    const HeapSnapshot& snapshot,
    RuntimeError* error) {
    if (snapshot.heapId != impl_->heapId) {
        setHeapError(error, ErrorCode::InvalidObjectReference,
            "heap snapshot belongs to a different managed heap");
        return false;
    }
    if (impl_->phase != GcPhase::Idle) {
        setHeapError(error, ErrorCode::InvalidProgram,
            "heap snapshot cannot be restored during collection");
        return false;
    }

    std::size_t slotCount = 0;
    std::size_t liveBytes = 0;
    for (const auto& info : snapshot.objects) {
        if (!info.reference.valid() ||
            info.reference.heapId != impl_->heapId ||
            info.reference.kind != info.kind ||
            info.reference.slot == ObjectRef::InvalidSlot ||
            info.sizeBytes > impl_->config.maximumHeapBytes ||
            liveBytes > impl_->config.maximumHeapBytes - info.sizeBytes) {
            setHeapError(error, ErrorCode::InvalidProgram,
                "heap snapshot contains invalid object metadata");
            return false;
        }
        liveBytes += info.sizeBytes;
        slotCount = std::max(
            slotCount,
            static_cast<std::size_t>(info.reference.slot) + 1u);
    }

    std::vector<Impl::Slot> slots(slotCount);
    std::unordered_map<std::uint32_t, const HeapObjectInfo*> infos;
    for (const auto& info : snapshot.objects) {
        if (!infos.emplace(info.reference.slot, &info).second) {
            setHeapError(error, ErrorCode::InvalidProgram,
                "heap snapshot contains duplicate object slots");
            return false;
        }
        auto& slot = slots[info.reference.slot];
        slot.generation = info.reference.generation;
        Impl::HeapObject object;
        object.header.kind = info.kind;
        object.header.typeId = info.typeId;
        object.header.elementType = info.elementType;
        object.header.elementTypeId = info.elementTypeId;
        object.header.sizeBytes = info.sizeBytes;
        if (info.kind == ObjectKind::String) {
            if (!info.values.empty() || info.valueCount != 0) {
                setHeapError(error, ErrorCode::InvalidProgram,
                    "heap snapshot string payload is invalid");
                return false;
            }
            object.stringPayload = info.stringValue;
        } else {
            if (info.values.size() != info.valueCount) {
                setHeapError(error, ErrorCode::InvalidProgram,
                    "heap snapshot value count is invalid");
                return false;
            }
            for (const auto field : info.referenceFields) {
                if (field >= info.values.size()) {
                    setHeapError(error, ErrorCode::InvalidProgram,
                        "heap snapshot reference-field map is invalid");
                    return false;
                }
            }
            if (info.kind == ObjectKind::Record &&
                info.values.size() <= Impl::HeapObject::InlineFieldCapacity) {
                object.usesInlineValues = true;
                object.inlineValueCount = static_cast<std::uint8_t>(info.values.size());
                for (std::size_t fi = 0; fi < info.values.size(); ++fi) {
                    object.inlineValues[fi] = info.values[fi];
                }
            } else {
                object.values = info.values;
            }
            if (info.typeId != 0) {
                auto& shared = impl_->sharedTypeReferenceFields[info.typeId];
                if (!shared || *shared != info.referenceFields) {
                    shared = std::make_shared<const std::vector<std::size_t>>(info.referenceFields);
                }
                object.referenceFields = shared;
            } else {
                object.referenceFields = std::make_shared<const std::vector<std::size_t>>(info.referenceFields);
            }
        }
        slot.object.emplace(std::move(object));
    }

    const auto validValue = [&](const auto& self,
                                const Value& value,
                                std::unordered_set<const StructStorage*>&
                                    visited) -> bool {
        if (const auto* reference = std::get_if<ObjectRef>(&value)) {
            const auto found = infos.find(reference->slot);
            return reference->heapId == impl_->heapId &&
                found != infos.end() &&
                found->second->reference == *reference;
        }
        if (const auto* structure = std::get_if<StructValue>(&value);
            structure && structure->storage &&
            visited.insert(structure->storage.get()).second) {
            for (const auto& field : structure->storage->fields) {
                if (!self(self, field, visited)) return false;
            }
        }
        return true;
    };
    for (const auto& info : snapshot.objects) {
        for (const auto& value : info.values) {
            std::unordered_set<const StructStorage*> visited;
            if (!validValue(validValue, value, visited)) {
                setHeapError(error, ErrorCode::InvalidProgram,
                    "heap snapshot contains a dangling object reference");
                return false;
            }
        }
    }

    std::unordered_map<RootToken, Value> roots;
    RootToken nextRootToken = 1;
    for (const auto& root : snapshot.roots) {
        if (root.kind != "persistent") continue;
        const auto found = infos.find(root.reference.slot);
        if (root.token == 0 || found == infos.end() ||
            found->second->reference != root.reference ||
            !roots.emplace(root.token, root.reference).second) {
            setHeapError(error, ErrorCode::InvalidProgram,
                "heap snapshot contains invalid persistent roots");
            return false;
        }
        nextRootToken = std::max(nextRootToken, root.token + 1u);
    }

    std::vector<std::uint32_t> freeSlots;
    for (std::size_t index = 0; index < slots.size(); ++index) {
        if (!slots[index].object) {
            freeSlots.push_back(static_cast<std::uint32_t>(index));
        } else {
            const auto count = slots[index].object->valueCount();
            for (std::size_t fi = 0; fi < count; ++fi) {
                pinValueInPools(slots[index].object->valueAt(fi));
            }
        }
    }
    for (const auto& [token, val] : roots) {
        (void)token;
        pinValueInPools(val);
    }
    impl_->slots = std::move(slots);
    impl_->freeSlots = std::move(freeSlots);
    impl_->persistentRoots = std::move(roots);
    impl_->nextRootToken = nextRootToken;
    impl_->internedStrings.clear();
    impl_->markStack.clear();
    impl_->requested = false;
    impl_->sweepIndex = 0;
    impl_->resumeSweep = false;
    impl_->sweepLimit = 0;
    impl_->statistics = snapshot.statistics;
    impl_->statistics.liveObjects = snapshot.objects.size();
    impl_->statistics.liveBytes = liveBytes;
    impl_->statistics.peakLiveBytes = std::max(
        impl_->statistics.peakLiveBytes, liveBytes);
    impl_->nextCollectionThreshold = std::max(
        impl_->config.initialCollectionThresholdBytes,
        std::min(
            impl_->config.maximumHeapBytes,
            liveBytes > impl_->config.maximumHeapBytes / 2u
                ? impl_->config.maximumHeapBytes
                : std::max<std::size_t>(1u, liveBytes * 2u)));
    if (error) *error = {};
    return true;
}

std::vector<ObjectRef> ManagedHeap::retainingPath(
    ObjectRef target,
    const ShadowStack* shadowStack) const {
    if (!isAlive(target)) return {};
    const auto image = snapshot(shadowStack);
    std::unordered_map<std::uint32_t, const HeapObjectInfo*> objects;
    for (const auto& object : image.objects) {
        objects.emplace(object.reference.slot, &object);
    }
    std::deque<ObjectRef> queue;
    std::unordered_map<std::uint32_t, ObjectRef> parent;
    std::unordered_set<std::uint32_t> visited;
    for (const auto& root : image.roots) {
        if (visited.insert(root.reference.slot).second) {
            queue.push_back(root.reference);
            parent.emplace(root.reference.slot, ObjectRef{});
        }
    }
    while (!queue.empty()) {
        const auto current = queue.front();
        queue.pop_front();
        if (current == target) {
            std::vector<ObjectRef> path;
            auto cursor = current;
            while (cursor.valid()) {
                path.push_back(cursor);
                const auto found = parent.find(cursor.slot);
                if (found == parent.end()) break;
                cursor = found->second;
            }
            std::reverse(path.begin(), path.end());
            return path;
        }
        const auto found = objects.find(current.slot);
        if (found == objects.end()) continue;
        for (const auto& edge : found->second->edges) {
            if (visited.insert(edge.target.slot).second) {
                parent.emplace(edge.target.slot, current);
                queue.push_back(edge.target);
            }
        }
    }
    return {};
}

std::string ManagedHeap::leakSummary() const {
    const auto image = snapshot();
    std::size_t strings = 0;
    std::size_t arrays = 0;
    std::size_t records = 0;
    for (const auto& object : image.objects) {
        switch (object.kind) {
        case ObjectKind::String: ++strings; break;
        case ObjectKind::Array: ++arrays; break;
        case ObjectKind::Record: ++records; break;
        }
    }
    std::ostringstream out;
    out << "heap " << image.heapId << ": " << image.objects.size()
        << " live objects, " << image.statistics.liveBytes << " bytes, "
        << image.roots.size() << " persistent roots"
        << " [string=" << strings << ", array=" << arrays
        << ", record=" << records << ']';
    if (!image.roots.empty()) {
        out << "\nroots:";
        for (const auto& root : image.roots) {
            out << "\n  #" << root.token << " -> " << root.reference.slot
                << ':' << root.reference.generation;
        }
    }
    return out.str();
}

std::string HeapSnapshot::toText() const {
    std::ostringstream out;
    out << "heap " << heapId << " objects=" << objects.size()
        << " bytes=" << statistics.liveBytes << " roots=" << roots.size() << '\n';
    for (const auto& root : roots) {
        out << "root " << root.kind << '#' << root.token << " -> o"
            << root.reference.slot << ':' << root.reference.generation << '\n';
    }
    for (const auto& object : objects) {
        out << 'o' << object.reference.slot << ':' << object.reference.generation
            << ' ' << objectKindName(object.kind) << " bytes=" << object.sizeBytes;
        if (object.typeId != 0) out << " type=0x" << std::hex << object.typeId << std::dec;
        if (object.kind == ObjectKind::Array) {
            out << " element=" << semantic::primitiveTypeName(object.elementType)
                << " count=" << object.valueCount;
            if (object.elementTypeId != 0) {
                out << " elementType=0x" << std::hex << object.elementTypeId << std::dec;
            }
        }
        out << '\n';
        for (const auto& edge : object.edges) {
            out << "  " << edge.label << " -> o" << edge.target.slot << ':'
                << edge.target.generation << '\n';
        }
    }
    return out.str();
}

PersistentRoot::~PersistentRoot() { reset(); }

PersistentRoot::PersistentRoot(PersistentRoot&& other) noexcept
    : heap_(other.heap_), token_(other.token_), value_(std::move(other.value_)) {
    other.heap_ = nullptr;
    other.token_ = 0;
    other.value_ = {};
}

PersistentRoot& PersistentRoot::operator=(PersistentRoot&& other) noexcept {
    if (this == &other) return *this;
    reset();
    heap_ = other.heap_;
    token_ = other.token_;
    value_ = std::move(other.value_);
    other.heap_ = nullptr;
    other.token_ = 0;
    other.value_ = {};
    return *this;
}

bool PersistentRoot::valid() const noexcept {
    return heap_ && token_ != 0;
}

ObjectRef PersistentRoot::reference() const noexcept {
    const auto* reference = std::get_if<ObjectRef>(&value_);
    return reference ? *reference : ObjectRef{};
}

const Value& PersistentRoot::value() const noexcept { return value_; }

bool PersistentRoot::update(ObjectRef reference) {
    return update(Value{reference});
}

bool PersistentRoot::update(Value value) {
    if (!heap_ || token_ == 0 ||
        !heap_->updatePersistentRoot(token_, value)) {
        return false;
    }
    value_ = std::move(value);
    return true;
}

void PersistentRoot::reset() noexcept {
    if (heap_ && token_ != 0) (void)heap_->removePersistentRoot(token_);
    heap_ = nullptr;
    token_ = 0;
    value_ = {};
}

PersistentRoot::PersistentRoot(
    ManagedHeap* heap,
    ManagedHeap::RootToken token,
    Value value)
    : heap_(heap), token_(token), value_(std::move(value)) {}

NativeHandleRegistry::NativeHandleRegistry()
    : registryId_(nextRegistryId()) {
    slots_.reserve(256);
    freeSlots_.reserve(256);
}

NativeHandle NativeHandleRegistry::create(
    semantic::SymbolId typeId,
    std::shared_ptr<void> resource,
    std::string debugNameValue) {
    if (typeId == 0 || !resource) return {};
    const void* rawPtr = resource.get();
    std::uint32_t slotIndex = 0;
    if (!freeSlots_.empty()) {
        slotIndex = freeSlots_.back();
        freeSlots_.pop_back();
    } else {
        if (slots_.size() >= static_cast<std::size_t>(NativeHandle::InvalidSlot)) {
            return {};
        }
        slotIndex = static_cast<std::uint32_t>(slots_.size());
        slots_.push_back({});
    }
    auto& slot = slots_[slotIndex];
    if (slot.generation == 0 || slot.generation > 0xFFFFu) slot.generation = 1;
    slot.resource = std::move(resource);
    slot.rawPointer = rawPtr;
    slot.typeId = typeId;
    slot.debugName = std::move(debugNameValue);
    slot.cachedWrapper = {};
    slot.cachedHeapId = 0;
    if (rawPtr) {
        activeSlotByPointer_[PointerTypeKey{rawPtr, typeId}] = slotIndex;
    }
    return {slotIndex, slot.generation, typeId, registryId_};
}

NativeHandle NativeHandleRegistry::createBorrowed(
    semantic::SymbolId typeId,
    void* rawPointer,
    bool indexPointer) {
    if (typeId == 0 || !rawPointer) return {};
    std::uint32_t slotIndex = 0;
    if (!freeSlots_.empty()) {
        slotIndex = freeSlots_.back();
        freeSlots_.pop_back();
    } else {
        if (slots_.size() >= static_cast<std::size_t>(NativeHandle::InvalidSlot)) {
            return {};
        }
        slotIndex = static_cast<std::uint32_t>(slots_.size());
        slots_.push_back({});
    }
    auto& slot = slots_[slotIndex];
    if (slot.generation == 0 || slot.generation > 0xFFFFu) slot.generation = 1;
    slot.resource = std::shared_ptr<void>(std::shared_ptr<void>{}, rawPointer);
    slot.rawPointer = rawPointer;
    slot.typeId = typeId;
    slot.debugName.clear();
    slot.cachedWrapper = {};
    slot.cachedHeapId = 0;
    if (indexPointer) {
        activeSlotByPointer_[PointerTypeKey{rawPointer, typeId}] = slotIndex;
    }
    return {slotIndex, slot.generation, typeId, registryId_};
}

std::optional<NativeHandle> NativeHandleRegistry::findHandle(
    semantic::SymbolId typeId,
    const void* rawPointer) const noexcept {
    if (typeId == 0 || !rawPointer) return std::nullopt;
    const auto it = activeSlotByPointer_.find(PointerTypeKey{rawPointer, typeId});
    if (it == activeSlotByPointer_.end() || it->second >= slots_.size()) {
        return std::nullopt;
    }
    const auto& slot = slots_[it->second];
    if (!slot.resource || slot.rawPointer != rawPointer || slot.typeId != typeId) {
        return std::nullopt;
    }
    return NativeHandle{it->second, slot.generation, typeId, registryId_};
}

std::optional<ObjectRef> NativeHandleRegistry::cachedWrapper(
    NativeHandle handle,
    std::uint64_t heapId) const noexcept {
    if (!isAlive(handle)) return std::nullopt;
    const auto& slot = slots_[handle.slot];
    if (!slot.cachedWrapper.valid() || slot.cachedHeapId != heapId) {
        return std::nullopt;
    }
    return slot.cachedWrapper;
}

bool NativeHandleRegistry::setCachedWrapper(
    NativeHandle handle,
    ObjectRef wrapper,
    std::uint64_t heapId) noexcept {
    if (!isAlive(handle)) return false;
    auto& slot = slots_[handle.slot];
    slot.cachedWrapper = wrapper;
    slot.cachedHeapId = heapId;
    return true;
}

std::shared_ptr<void> NativeHandleRegistry::resolve(
    NativeHandle handle,
    semantic::SymbolId expectedTypeId,
    RuntimeError* error) const {
    if (!handle.valid() || handle.registryId != ((registryId_ == 0) ? 0u : (((registryId_ - 1u) % 65535u) + 1u)) ||
        handle.slot >= slots_.size()) {
        setHeapError(error, ErrorCode::InvalidNativeHandle,
            "native handle is invalid");
        return {};
    }
    const auto& slot = slots_[handle.slot];
    if (!slot.resource || slot.generation != handle.generation ||
        slot.typeId != handle.typeId) {
        setHeapError(error, ErrorCode::InvalidNativeHandle,
            "native handle is stale");
        return {};
    }
    if (expectedTypeId != 0 && slot.typeId != expectedTypeId) {
        setHeapError(error, ErrorCode::TypeMismatch,
            "native handle type does not match the expected host type");
        return {};
    }
    return slot.resource;
}

void* NativeHandleRegistry::resolveRawFast(
    NativeHandle handle,
    semantic::SymbolId expectedTypeId) const noexcept {
    if (!handle.valid() ||
        handle.registryId != ((registryId_ == 0) ? 0u : (((registryId_ - 1u) % 65535u) + 1u)) ||
        handle.slot >= slots_.size()) {
        return nullptr;
    }
    const auto& slot = slots_[handle.slot];
    if (!slot.rawPointer || slot.generation != handle.generation) {
        return nullptr;
    }
    if (expectedTypeId != 0 && slot.typeId != expectedTypeId) {
        return nullptr;
    }
    return const_cast<void*>(slot.rawPointer);
}

void NativeHandleRegistry::visitCachedWrappers(
    std::uint64_t heapId,
    const std::function<void(NativeHandle, ObjectRef, const void*)>& visitor) const {
    if (!visitor) return;
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        const auto& slot = slots_[i];
        if (slot.rawPointer && slot.cachedWrapper.valid() &&
            (heapId == 0 || slot.cachedHeapId == heapId)) {
            NativeHandle h{
                static_cast<std::uint32_t>(i),
                slot.generation,
                slot.typeId,
                registryId_};
            visitor(h, slot.cachedWrapper, slot.rawPointer);
        }
    }
}

bool NativeHandleRegistry::isAlive(NativeHandle handle) const noexcept {
    return handle.valid() && handle.registryId == ((registryId_ == 0) ? 0u : (((registryId_ - 1u) % 65535u) + 1u)) &&
        handle.slot < slots_.size() &&
        slots_[handle.slot].resource &&
        slots_[handle.slot].generation == handle.generation &&
        slots_[handle.slot].typeId == handle.typeId;
}

bool NativeHandleRegistry::release(NativeHandle handle) noexcept {
    if (!isAlive(handle)) return false;
    auto& slot = slots_[handle.slot];
    if (slot.rawPointer) {
        const auto it = activeSlotByPointer_.find(
            PointerTypeKey{slot.rawPointer, slot.typeId});
        if (it != activeSlotByPointer_.end() && it->second == handle.slot) {
            activeSlotByPointer_.erase(it);
        }
    }
    slot.resource.reset();
    slot.rawPointer = nullptr;
    slot.typeId = 0;
    slot.debugName.clear();
    slot.cachedWrapper = {};
    slot.cachedHeapId = 0;
    ++slot.generation;
    if (slot.generation == 0 || slot.generation > 0xFFFFu) slot.generation = 1;
    freeSlots_.push_back(handle.slot);
    return true;
}

void NativeHandleRegistry::clear() noexcept {
    activeSlotByPointer_.clear();
    freeSlots_.clear();
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        auto& slot = slots_[index];
        if (slot.resource) {
            slot.resource.reset();
            slot.rawPointer = nullptr;
            slot.typeId = 0;
            slot.debugName.clear();
            slot.cachedWrapper = {};
            slot.cachedHeapId = 0;
            ++slot.generation;
            if (slot.generation == 0 || slot.generation > 0xFFFFu) slot.generation = 1;
        }
        freeSlots_.push_back(static_cast<std::uint32_t>(index));
    }
}

std::size_t NativeHandleRegistry::liveCount() const noexcept {
    return static_cast<std::size_t>(std::count_if(
        slots_.begin(), slots_.end(),
        [](const Slot& slot) { return static_cast<bool>(slot.resource); }));
}

std::uint64_t NativeHandleRegistry::registryId() const noexcept {
    return registryId_;
}

std::string NativeHandleRegistry::debugName(NativeHandle handle) const {
    return isAlive(handle) ? slots_[handle.slot].debugName : std::string{};
}


std::size_t reclaimUnreferencedPoolValues(
    const ShadowStack* activeShadowStack) noexcept {
    if (!hasAnyPinnedPoolValues()) return 0u;
    std::lock_guard<std::mutex> gcLock(poolGcMutex());
    if (!hasAnyPinnedPoolValues()) return 0u;
    beginValuePoolGcMark();
    if (activeShadowStack) {
        activeShadowStack->visitRoots([](const Value& value) {
            markStackRootValueInPools(value);
        });
    }
    {
        std::lock_guard<std::mutex> stackLock(activeShadowStacksMutex());
        for (const ShadowStack* stack : activeShadowStacks()) {
            if (stack && stack != activeShadowStack) {
                stack->visitRoots([](const Value& value) {
                    markStackRootValueInPools(value);
                });
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(liveHeapsPoolMutex());
        for (const auto& [ptr, marker] : liveHeapPoolMarkers()) {
            (void)ptr;
            if (marker) marker();
        }
    }
    return finishValuePoolGcSweep();
}

} // namespace realscript::runtime
