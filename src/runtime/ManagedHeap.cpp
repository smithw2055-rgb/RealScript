#include "realscript/runtime/Runtime.h"

#include <algorithm>
#include <atomic>
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

} // namespace

void ShadowStack::pushFrame(
    const std::vector<Value>* arguments,
    const std::vector<Value>* locals,
    const std::vector<Value>* registers) {
    frames_.push_back({arguments, locals, registers});
}

void ShadowStack::popFrame() noexcept {
    if (!frames_.empty()) frames_.pop_back();
}

void ShadowStack::visitRoots(const std::function<void(const Value&)>& visitor) const {
    const auto visitValues = [&](const std::vector<Value>* values) {
        if (!values) return;
        for (const auto& value : *values) visitor(value);
    };
    for (const auto& frame : frames_) {
        visitValues(frame.arguments);
        visitValues(frame.locals);
        visitValues(frame.registers);
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
        std::size_t sizeBytes = 0;
    };

    struct HeapObject {
        Header header;
        std::variant<std::string, std::vector<Value>> payload;
        std::vector<std::size_t> referenceFields;
    };

    struct Slot {
        std::unique_ptr<HeapObject> object;
        std::uint32_t generation = 1;
    };

    explicit Impl(HeapConfig value)
        : config(value), heapId(nextHeapId()),
          nextCollectionThreshold(value.initialCollectionThresholdBytes) {
        if (config.initialCollectionThresholdBytes == 0) {
            config.initialCollectionThresholdBytes = 1;
            nextCollectionThreshold = 1;
        }
        if (config.maximumHeapBytes < config.initialCollectionThresholdBytes) {
            config.maximumHeapBytes = config.initialCollectionThresholdBytes;
        }
    }

    HeapConfig config;
    std::uint64_t heapId = 0;
    std::vector<Slot> slots;
    std::vector<std::uint32_t> freeSlots;
    std::vector<ObjectRef> markStack;
    std::unordered_map<RootToken, Value> persistentRoots;
    RootToken nextRootToken = 1;
    GcPhase phase = GcPhase::Idle;
    bool requested = false;
    std::size_t sweepIndex = 0;
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

    [[nodiscard]] std::optional<ObjectRef> allocate(
        ObjectKind kind,
        semantic::SymbolId typeId,
        semantic::PrimitiveType elementType,
        semantic::SymbolId elementTypeId,
        std::variant<std::string, std::vector<Value>> payload,
        std::vector<std::size_t> referenceFields,
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

        std::uint32_t slotIndex = 0;
        if (!freeSlots.empty()) {
            slotIndex = freeSlots.back();
            freeSlots.pop_back();
        } else {
            if (slots.size() >= static_cast<std::size_t>(ObjectRef::InvalidSlot)) {
                setHeapError(error, ErrorCode::OutOfMemory,
                    "managed heap exhausted the object handle space");
                return std::nullopt;
            }
            slotIndex = static_cast<std::uint32_t>(slots.size());
            slots.emplace_back();
        }

        auto object = std::make_unique<HeapObject>();
        object->header.kind = kind;
        object->header.typeId = typeId;
        object->header.elementType = elementType;
        object->header.elementTypeId = elementTypeId;
        object->header.marked = phase != GcPhase::Idle;
        object->header.sizeBytes = sizeBytes;
        object->payload = std::move(payload);
        object->referenceFields = std::move(referenceFields);
        auto& slot = slots[slotIndex];
        slot.object = std::move(object);
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
                sweepIndex = 0;
            }
        }
        return reference;
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
            if (structure->typeId == 0 || !structure->storage) return false;
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
    }

    void startCollection(const ShadowStack& shadowStack) {
        for (auto& slot : slots) {
            if (slot.object) slot.object->header.marked = false;
        }
        markStack.clear();
        sweepIndex = 0;
        phase = GcPhase::Mark;
        ++statistics.collectionsStarted;
        markRoots(shadowStack);
    }

    void scan(ObjectRef reference) {
        const auto* object = get(reference);
        if (!object || object->header.kind == ObjectKind::String) return;
        const auto& values = std::get<std::vector<Value>>(object->payload);
        if (object->header.kind == ObjectKind::Array) {
            for (const auto& value : values) markValue(value);
            return;
        }
        for (const auto index : object->referenceFields) {
            if (index < values.size()) markValue(values[index]);
        }
    }

    void writeBarrier(ObjectRef owner, const Value& value) {
        if (phase == GcPhase::Idle) return;
        const auto* object = get(owner);
        if (!object || !object->header.marked) return;
        markValue(value);
        if (phase == GcPhase::Sweep && !markStack.empty()) {
            phase = GcPhase::Mark;
            sweepIndex = 0;
        }
    }

    void finishCollection() {
        phase = GcPhase::Idle;
        requested = false;
        sweepIndex = 0;
        ++statistics.collectionsCompleted;
        const auto doubled = statistics.liveBytes >
                std::numeric_limits<std::size_t>::max() / 2
            ? config.maximumHeapBytes
            : statistics.liveBytes * 2;
        nextCollectionThreshold = std::max(
            config.initialCollectionThresholdBytes,
            std::min(config.maximumHeapBytes, doubled));
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
    const auto size = sizeof(Impl::HeapObject) + value.size();
    return impl_->allocate(
        ObjectKind::String, 0, semantic::PrimitiveType::Error, 0,
        std::move(value), {}, size, error);
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
        elementType == semantic::PrimitiveType::Int ||
        elementType == semantic::PrimitiveType::Long ||
        elementType == semantic::PrimitiveType::Double ||
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
    std::vector<Value> values(length, std::move(initialValue));
    return impl_->allocate(
        ObjectKind::Array,
        arrayTypeId,
        elementType,
        elementTypeId,
        std::move(values),
        {},
        sizeof(Impl::HeapObject) + storage,
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
    return impl_->allocate(
        ObjectKind::Record,
        0,
        semantic::PrimitiveType::Error,
        0,
        std::move(fields),
        std::move(referenceFields),
        sizeof(Impl::HeapObject) + storage,
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
    return impl_->allocate(
        ObjectKind::Record,
        typeId,
        semantic::PrimitiveType::Error,
        0,
        std::move(fields),
        std::move(referenceFields),
        sizeof(Impl::HeapObject) + storage,
        error);
}

bool ManagedHeap::isAlive(ObjectRef reference) const noexcept {
    return impl_->get(reference) != nullptr;
}

std::optional<std::string_view> ManagedHeap::stringView(ObjectRef reference) const {
    const auto* object = impl_->get(reference);
    if (!object || object->header.kind != ObjectKind::String) return std::nullopt;
    return std::string_view(std::get<std::string>(object->payload));
}

std::optional<std::size_t> ManagedHeap::arrayLength(ObjectRef reference) const {
    const auto* object = impl_->get(reference);
    if (!object || object->header.kind != ObjectKind::Array) return std::nullopt;
    return std::get<std::vector<Value>>(object->payload).size();
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
    const auto& values = std::get<std::vector<Value>>(object->payload);
    if (index >= values.size()) return std::nullopt;
    return values[index];
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
    auto& values = std::get<std::vector<Value>>(object->payload);
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
        const auto* structure = std::get_if<StructValue>(&value);
        if (!structure || structure->typeId != object->header.elementTypeId ||
            !structure->storage) {
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
    return std::get<std::vector<Value>>(object->payload).size();
}

std::optional<Value> ManagedHeap::fieldGet(ObjectRef reference, std::size_t index) const {
    const auto* object = impl_->get(reference);
    if (!object || object->header.kind != ObjectKind::Record) return std::nullopt;
    const auto& fields = std::get<std::vector<Value>>(object->payload);
    if (index >= fields.size()) return std::nullopt;
    return fields[index];
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
    auto& fields = std::get<std::vector<Value>>(object->payload);
    if (index >= fields.size()) {
        setHeapError(error, ErrorCode::InvalidArguments,
            "managed field index is out of range");
        return false;
    }
    if (std::binary_search(
            object->referenceFields.begin(),
            object->referenceFields.end(),
            index)) {
        impl_->writeBarrier(reference, value);
    }
    fields[index] = std::move(value);
    return true;
}

ManagedHeap::RootToken ManagedHeap::addPersistentRoot(ObjectRef reference) {
    return addPersistentRoot(Value{reference});
}

ManagedHeap::RootToken ManagedHeap::addPersistentRoot(Value value) {
    if (!impl_->validRootValue(value)) return 0;
    auto token = impl_->nextRootToken++;
    if (token == 0) token = impl_->nextRootToken++;
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
    }

    std::size_t performed = 0;
    while (performed < workBudget) {
        impl_->markRoots(shadowStack);
        if (impl_->phase == GcPhase::Sweep && !impl_->markStack.empty()) {
            impl_->phase = GcPhase::Mark;
            impl_->sweepIndex = 0;
        }
        if (impl_->phase == GcPhase::Mark) {
            impl_->markRoots(shadowStack);
            if (impl_->markStack.empty()) {
                impl_->phase = GcPhase::Sweep;
                impl_->sweepIndex = 0;
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
            if (impl_->sweepIndex >= impl_->slots.size()) {
                impl_->finishCollection();
                break;
            }
            auto& slot = impl_->slots[impl_->sweepIndex++];
            if (slot.object && !slot.object->header.marked) {
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
        if (object.header.kind != ObjectKind::String) {
            const auto& values = std::get<std::vector<Value>>(object.payload);
            info.valueCount = values.size();
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
            } else {
                for (const auto index : object.referenceFields) addEdge(index);
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
            return left.token < right.token;
        });
    return result;
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
    : registryId_(nextRegistryId()) {}

NativeHandle NativeHandleRegistry::create(
    semantic::SymbolId typeId,
    std::shared_ptr<void> resource,
    std::string debugNameValue) {
    if (typeId == 0 || !resource) return {};
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
    slot.resource = std::move(resource);
    slot.typeId = typeId;
    slot.debugName = std::move(debugNameValue);
    return {slotIndex, slot.generation, typeId, registryId_};
}

std::shared_ptr<void> NativeHandleRegistry::resolve(
    NativeHandle handle,
    semantic::SymbolId expectedTypeId,
    RuntimeError* error) const {
    if (!handle.valid() || handle.registryId != registryId_ ||
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

bool NativeHandleRegistry::isAlive(NativeHandle handle) const noexcept {
    return handle.valid() && handle.registryId == registryId_ &&
        handle.slot < slots_.size() &&
        slots_[handle.slot].resource &&
        slots_[handle.slot].generation == handle.generation &&
        slots_[handle.slot].typeId == handle.typeId;
}

bool NativeHandleRegistry::release(NativeHandle handle) noexcept {
    if (!isAlive(handle)) return false;
    auto& slot = slots_[handle.slot];
    slot.resource.reset();
    slot.typeId = 0;
    slot.debugName.clear();
    ++slot.generation;
    if (slot.generation == 0) slot.generation = 1;
    freeSlots_.push_back(handle.slot);
    return true;
}

void NativeHandleRegistry::clear() noexcept {
    freeSlots_.clear();
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        auto& slot = slots_[index];
        if (slot.resource) {
            slot.resource.reset();
            slot.typeId = 0;
            slot.debugName.clear();
            ++slot.generation;
            if (slot.generation == 0) slot.generation = 1;
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

} // namespace realscript::runtime
