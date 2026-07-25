#include "realscript/runtime/Runtime.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace realscript::runtime {
namespace {

enum class MarkColor : std::uint8_t { White, Gray, Black };
enum class CollectionPhase : std::uint8_t { Idle, Mark, Sweep };

std::size_t valueBytes(const Value& value) {
    if (const auto* string = std::get_if<std::string>(&value)) return string->size();
    return sizeof(Value);
}

} // namespace

struct ManagedHeap::Impl {
    struct Object {
        ObjectKind kind = ObjectKind::Record;
        std::string string;
        std::vector<Value> values;
        std::size_t bytes = 0;
        MarkColor color = MarkColor::White;
    };

    struct Slot {
        std::uint32_t generation = 1;
        std::unique_ptr<Object> object;
    };

    struct RootEntry {
        std::size_t token = 0;
        const Value* value = nullptr;
        const std::vector<Value>* values = nullptr;
    };

    std::vector<Slot> slots;
    std::vector<std::uint32_t> freeSlots;
    std::vector<RootEntry> roots;
    std::vector<std::uint32_t> gray;
    std::size_t nextToken = 1;
    CollectionPhase phase = CollectionPhase::Idle;
    std::size_t sweepIndex = 0;
    HeapStatistics statistics;

    Object* find(ObjectRef reference) noexcept {
        if (reference.slot >= slots.size()) return nullptr;
        auto& slot = slots[reference.slot];
        if (!slot.object || slot.generation != reference.generation ||
            slot.object->kind != reference.kind) return nullptr;
        return slot.object.get();
    }

    const Object* find(ObjectRef reference) const noexcept {
        return const_cast<Impl*>(this)->find(reference);
    }

    void markReference(ObjectRef reference) {
        auto* object = find(reference);
        if (!object || object->color != MarkColor::White) return;
        object->color = MarkColor::Gray;
        gray.push_back(reference.slot);
    }

    void markValue(const Value& value) {
        if (const auto* reference = std::get_if<ObjectRef>(&value)) {
            markReference(*reference);
        }
    }

    void scanObject(std::uint32_t slotIndex) {
        if (slotIndex >= slots.size()) return;
        auto* object = slots[slotIndex].object.get();
        if (!object || object->color != MarkColor::Gray) return;
        for (const auto& value : object->values) markValue(value);
        object->color = MarkColor::Black;
    }

    void markRoots() {
        for (const auto& root : roots) {
            if (root.value) markValue(*root.value);
            if (root.values) {
                for (const auto& value : *root.values) markValue(value);
            }
        }
    }

    void beginCollection() {
        gray.clear();
        sweepIndex = 0;
        for (auto& slot : slots) {
            if (slot.object) slot.object->color = MarkColor::White;
        }
        markRoots();
        phase = gray.empty() ? CollectionPhase::Sweep : CollectionPhase::Mark;
        ++statistics.collections;
    }

    void reclaim(std::uint32_t index) {
        auto& slot = slots[index];
        if (!slot.object) return;
        statistics.reclaimedBytes += slot.object->bytes;
        ++statistics.reclaimedObjects;
        statistics.liveBytes -= slot.object->bytes;
        --statistics.liveObjects;
        slot.object.reset();
        if (slot.generation == std::numeric_limits<std::uint32_t>::max()) {
            slot.generation = 1;
        } else {
            ++slot.generation;
        }
        freeSlots.push_back(index);
    }

    void writeBarrier(Object& owner, const Value& value) {
        if (phase != CollectionPhase::Mark || owner.color != MarkColor::Black) return;
        if (const auto* reference = std::get_if<ObjectRef>(&value)) {
            auto* child = find(*reference);
            if (child && child->color == MarkColor::White) markReference(*reference);
        }
    }

    ObjectRef allocate(ObjectKind kind, std::unique_ptr<Object> object) {
        std::uint32_t index = 0;
        if (freeSlots.empty()) {
            index = static_cast<std::uint32_t>(slots.size());
            slots.push_back({});
        } else {
            index = freeSlots.back();
            freeSlots.pop_back();
        }
        object->kind = kind;
        if (phase != CollectionPhase::Idle) object->color = MarkColor::Black;
        auto& slot = slots[index];
        const auto bytes = object->bytes;
        slot.object = std::move(object);
        ++statistics.allocations;
        ++statistics.liveObjects;
        statistics.liveBytes += bytes;
        statistics.peakBytes = std::max(statistics.peakBytes, statistics.liveBytes);
        return {index, slot.generation, kind};
    }
};

ManagedHeap::RootScope::RootScope(ManagedHeap& heap) : heap_(&heap) {}
ManagedHeap::RootScope::~RootScope() {
    if (!heap_) return;
    for (const auto token : tokens_) heap_->removeRoot(token);
}
ManagedHeap::RootScope::RootScope(RootScope&& other) noexcept
    : heap_(other.heap_), tokens_(std::move(other.tokens_)) { other.heap_ = nullptr; }
ManagedHeap::RootScope& ManagedHeap::RootScope::operator=(RootScope&& other) noexcept {
    if (this == &other) return *this;
    if (heap_) for (const auto token : tokens_) heap_->removeRoot(token);
    heap_ = other.heap_;
    tokens_ = std::move(other.tokens_);
    other.heap_ = nullptr;
    return *this;
}
void ManagedHeap::RootScope::add(const Value& value) {
    if (heap_) tokens_.push_back(heap_->addRoot(&value));
}
void ManagedHeap::RootScope::add(const std::vector<Value>& values) {
    if (heap_) tokens_.push_back(heap_->addRoot(&values));
}

ManagedHeap::ManagedHeap() : impl_(std::make_unique<Impl>()) {}
ManagedHeap::~ManagedHeap() = default;

ObjectRef ManagedHeap::allocateString(std::string value) {
    auto object = std::make_unique<Impl::Object>();
    object->bytes = sizeof(Impl::Object) + value.size();
    object->string = std::move(value);
    return impl_->allocate(ObjectKind::String, std::move(object));
}

ObjectRef ManagedHeap::allocateArray(std::size_t size, Value initial) {
    auto object = std::make_unique<Impl::Object>();
    object->values.assign(size, std::move(initial));
    object->bytes = sizeof(Impl::Object) + size * sizeof(Value);
    for (const auto& value : object->values) object->bytes += valueBytes(value);
    return impl_->allocate(ObjectKind::Array, std::move(object));
}

ObjectRef ManagedHeap::allocateRecord(std::size_t fieldCount) {
    auto object = std::make_unique<Impl::Object>();
    object->values.resize(fieldCount);
    object->bytes = sizeof(Impl::Object) + fieldCount * sizeof(Value);
    return impl_->allocate(ObjectKind::Record, std::move(object));
}

bool ManagedHeap::alive(ObjectRef reference) const noexcept { return impl_->find(reference) != nullptr; }
std::optional<std::string> ManagedHeap::stringValue(ObjectRef reference) const {
    const auto* object = impl_->find(reference);
    if (!object || object->kind != ObjectKind::String) return std::nullopt;
    return object->string;
}
std::size_t ManagedHeap::arraySize(ObjectRef reference) const noexcept {
    const auto* object = impl_->find(reference);
    return object && object->kind == ObjectKind::Array ? object->values.size() : 0;
}
std::optional<Value> ManagedHeap::arrayGet(ObjectRef reference, std::size_t index) const {
    const auto* object = impl_->find(reference);
    if (!object || object->kind != ObjectKind::Array || index >= object->values.size()) return std::nullopt;
    return object->values[index];
}
bool ManagedHeap::arraySet(ObjectRef reference, std::size_t index, Value value) {
    auto* object = impl_->find(reference);
    if (!object || object->kind != ObjectKind::Array || index >= object->values.size()) return false;
    impl_->writeBarrier(*object, value);
    const auto oldBytes = valueBytes(object->values[index]);
    const auto newBytes = valueBytes(value);
    object->values[index] = std::move(value);
    if (newBytes >= oldBytes) {
        object->bytes += newBytes - oldBytes;
        impl_->statistics.liveBytes += newBytes - oldBytes;
    } else {
        object->bytes -= oldBytes - newBytes;
        impl_->statistics.liveBytes -= oldBytes - newBytes;
    }
    impl_->statistics.peakBytes = std::max(
        impl_->statistics.peakBytes, impl_->statistics.liveBytes);
    return true;
}
std::optional<Value> ManagedHeap::fieldGet(ObjectRef reference, std::size_t index) const {
    const auto* object = impl_->find(reference);
    if (!object || object->kind != ObjectKind::Record || index >= object->values.size()) return std::nullopt;
    return object->values[index];
}
bool ManagedHeap::fieldSet(ObjectRef reference, std::size_t index, Value value) {
    auto* object = impl_->find(reference);
    if (!object || object->kind != ObjectKind::Record || index >= object->values.size()) return false;
    impl_->writeBarrier(*object, value);
    const auto oldBytes = valueBytes(object->values[index]);
    const auto newBytes = valueBytes(value);
    object->values[index] = std::move(value);
    if (newBytes >= oldBytes) {
        object->bytes += newBytes - oldBytes;
        impl_->statistics.liveBytes += newBytes - oldBytes;
    } else {
        object->bytes -= oldBytes - newBytes;
        impl_->statistics.liveBytes -= oldBytes - newBytes;
    }
    impl_->statistics.peakBytes = std::max(
        impl_->statistics.peakBytes, impl_->statistics.liveBytes);
    return true;
}

void ManagedHeap::requestCollection() {
    if (impl_->phase == CollectionPhase::Idle) impl_->beginCollection();
}
bool ManagedHeap::collectionInProgress() const noexcept {
    return impl_->phase != CollectionPhase::Idle;
}
bool ManagedHeap::step(std::size_t workBudget) {
    if (workBudget == 0) {
        impl_->statistics.lastStepWork = 0;
        return collectionInProgress();
    }
    if (impl_->phase == CollectionPhase::Idle) impl_->beginCollection();
    ++impl_->statistics.incrementalSteps;
    std::size_t work = 0;
    while (work < workBudget && impl_->phase != CollectionPhase::Idle) {
        impl_->markRoots();
        if (!impl_->gray.empty() && impl_->phase == CollectionPhase::Sweep) {
            impl_->phase = CollectionPhase::Mark;
        }
        if (impl_->phase == CollectionPhase::Mark) {
            if (impl_->gray.empty()) {
                impl_->phase = CollectionPhase::Sweep;
                continue;
            }
            const auto index = impl_->gray.back();
            impl_->gray.pop_back();
            impl_->scanObject(index);
            ++work;
            continue;
        }
        while (work < workBudget && impl_->sweepIndex < impl_->slots.size()) {
            auto& slot = impl_->slots[impl_->sweepIndex];
            if (slot.object) {
                if (slot.object->color == MarkColor::White) {
                    impl_->reclaim(static_cast<std::uint32_t>(impl_->sweepIndex));
                } else {
                    slot.object->color = MarkColor::White;
                }
            }
            ++impl_->sweepIndex;
            ++work;
        }
        if (impl_->sweepIndex >= impl_->slots.size()) impl_->phase = CollectionPhase::Idle;
    }
    impl_->statistics.lastStepWork = work;
    return collectionInProgress();
}
void ManagedHeap::collect() {
    requestCollection();
    while (collectionInProgress()) (void)step(1024);
}
const HeapStatistics& ManagedHeap::statistics() const noexcept { return impl_->statistics; }

std::size_t ManagedHeap::addRoot(const Value* value) {
    const auto token = impl_->nextToken++;
    impl_->roots.push_back({token, value, nullptr});
    if (impl_->phase != CollectionPhase::Idle) impl_->markValue(*value);
    return token;
}
std::size_t ManagedHeap::addRoot(const std::vector<Value>* values) {
    const auto token = impl_->nextToken++;
    impl_->roots.push_back({token, nullptr, values});
    if (impl_->phase != CollectionPhase::Idle) {
        for (const auto& value : *values) impl_->markValue(value);
    }
    return token;
}
void ManagedHeap::removeRoot(std::size_t token) noexcept {
    const auto found = std::find_if(impl_->roots.begin(), impl_->roots.end(),
        [token](const Impl::RootEntry& entry) { return entry.token == token; });
    if (found != impl_->roots.end()) impl_->roots.erase(found);
}

} // namespace realscript::runtime
