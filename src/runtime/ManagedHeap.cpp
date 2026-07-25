#include "realscript/runtime/Runtime.h"

#include <algorithm>
#include <limits>
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
        : config(value), nextCollectionThreshold(value.initialCollectionThresholdBytes) {
        if (config.initialCollectionThresholdBytes == 0) {
            config.initialCollectionThresholdBytes = 1;
            nextCollectionThreshold = 1;
        }
        if (config.maximumHeapBytes < config.initialCollectionThresholdBytes) {
            config.maximumHeapBytes = config.initialCollectionThresholdBytes;
        }
    }

    HeapConfig config;
    std::vector<Slot> slots;
    std::vector<std::uint32_t> freeSlots;
    std::vector<ObjectRef> markStack;
    std::unordered_map<RootToken, ObjectRef> persistentRoots;
    RootToken nextRootToken = 1;
    GcPhase phase = GcPhase::Idle;
    bool requested = false;
    std::size_t sweepIndex = 0;
    std::size_t nextCollectionThreshold = 0;
    GcStatistics statistics;

    [[nodiscard]] HeapObject* get(ObjectRef reference) noexcept {
        if (!reference.valid() || reference.slot >= slots.size()) return nullptr;
        auto& slot = slots[reference.slot];
        if (!slot.object || slot.generation != reference.generation ||
            slot.object->header.kind != reference.kind) return nullptr;
        return slot.object.get();
    }

    [[nodiscard]] const HeapObject* get(ObjectRef reference) const noexcept {
        if (!reference.valid() || reference.slot >= slots.size()) return nullptr;
        const auto& slot = slots[reference.slot];
        if (!slot.object || slot.generation != reference.generation ||
            slot.object->header.kind != reference.kind) return nullptr;
        return slot.object.get();
    }

    [[nodiscard]] std::optional<ObjectRef> allocate(
        ObjectKind kind,
        semantic::SymbolId typeId,
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
        object->header.marked = phase != GcPhase::Idle;
        object->header.sizeBytes = sizeBytes;
        object->payload = std::move(payload);
        object->referenceFields = std::move(referenceFields);
        auto& slot = slots[slotIndex];
        slot.object = std::move(object);
        const ObjectRef reference{slotIndex, slot.generation, kind};

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

    void markValue(const Value& value) {
        if (const auto* reference = std::get_if<ObjectRef>(&value)) mark(*reference);
    }

    void markRoots(const ShadowStack& shadowStack) {
        shadowStack.visitRoots([&](const Value& value) { markValue(value); });
        for (const auto& entry : persistentRoots) mark(entry.second);
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
    return impl_->allocate(ObjectKind::String, 0, std::move(value), {}, size, error);
}

std::optional<ObjectRef> ManagedHeap::allocateArray(
    std::size_t length,
    Value initialValue,
    RuntimeError* error) {
    const auto storage = estimateValueStorage(length);
    if (storage == std::numeric_limits<std::size_t>::max()) {
        setHeapError(error, ErrorCode::OutOfMemory, "managed array is too large");
        return std::nullopt;
    }
    std::vector<Value> values(length, std::move(initialValue));
    return impl_->allocate(
        ObjectKind::Array,
        0,
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
    RuntimeError* error) {
    auto* object = impl_->get(reference);
    if (!object || object->header.kind != ObjectKind::Array) {
        setHeapError(error, ErrorCode::InvalidObjectReference,
            "managed array reference is invalid");
        return false;
    }
    auto& values = std::get<std::vector<Value>>(object->payload);
    if (index >= values.size()) {
        setHeapError(error, ErrorCode::InvalidArguments,
            "managed array index is out of range");
        return false;
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
    if (!isAlive(reference)) return 0;
    auto token = impl_->nextRootToken++;
    if (token == 0) token = impl_->nextRootToken++;
    impl_->persistentRoots.emplace(token, reference);
    return token;
}

bool ManagedHeap::updatePersistentRoot(RootToken token, ObjectRef reference) {
    if (token == 0 || !isAlive(reference)) return false;
    const auto found = impl_->persistentRoots.find(token);
    if (found == impl_->persistentRoots.end()) return false;
    found->second = reference;
    return true;
}

bool ManagedHeap::removePersistentRoot(RootToken token) noexcept {
    return impl_->persistentRoots.erase(token) != 0;
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
    requestCollection();
    while (impl_->phase != GcPhase::Idle || impl_->requested) {
        const auto budget = std::max<std::size_t>(64, impl_->slots.size() + 1);
        (void)step(shadowStack, budget);
    }
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

} // namespace realscript::runtime
