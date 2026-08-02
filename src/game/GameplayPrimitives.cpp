#include "realscript/game/GameplayPrimitives.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_set>

namespace realscript::game {
namespace {

constexpr std::uint64_t FnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t FnvPrime = 1099511628211ULL;

void hashByte(std::uint64_t& hash, std::uint8_t value) noexcept {
    hash ^= value;
    hash *= FnvPrime;
}

void hashU64(std::uint64_t& hash, std::uint64_t value) noexcept {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        hashByte(hash, static_cast<std::uint8_t>(value >> shift));
    }
}

void hashString(std::uint64_t& hash, const std::string& value) noexcept {
    hashU64(hash, value.size());
    for (const auto character : value) {
        hashByte(hash, static_cast<std::uint8_t>(character));
    }
}

std::uint64_t canonicalDoubleBits(double value) noexcept {
    if (value == 0.0) return 0;
    if (std::isnan(value)) return 0x7ff8000000000000ULL;
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "unexpected double width");
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

std::uint32_t nextGeneration(std::uint32_t current) noexcept {
    ++current;
    return current == 0 ? 1 : current;
}

} // namespace

EntityId EntityRegistry::create() {
    std::uint32_t index = 0;
    if (!freeIndices_.empty()) {
        index = freeIndices_.front();
        freeIndices_.erase(freeIndices_.begin());
        slots_[index].alive = true;
    } else {
        if (slots_.size() >= EntityId::InvalidIndex) return {};
        index = static_cast<std::uint32_t>(slots_.size());
        slots_.push_back(SlotState{1, true});
    }
    ++aliveCount_;
    return EntityId{index, slots_[index].generation};
}

bool EntityRegistry::destroy(EntityId entity) {
    if (!alive(entity)) return false;
    auto& slot = slots_[entity.index];
    slot.alive = false;
    slot.generation = nextGeneration(slot.generation);
    const auto position = std::lower_bound(
        freeIndices_.begin(), freeIndices_.end(), entity.index);
    freeIndices_.insert(position, entity.index);
    --aliveCount_;
    return true;
}

bool EntityRegistry::alive(EntityId entity) const noexcept {
    return entity.valid() && entity.index < slots_.size() &&
        slots_[entity.index].alive &&
        slots_[entity.index].generation == entity.generation;
}

void EntityRegistry::clear() {
    slots_.clear();
    freeIndices_.clear();
    aliveCount_ = 0;
}

EntityRegistry::State EntityRegistry::snapshot() const {
    return State{slots_, freeIndices_, aliveCount_};
}

bool EntityRegistry::restore(const State& state) {
    if (!std::is_sorted(state.freeIndices.begin(), state.freeIndices.end()) ||
        std::adjacent_find(
            state.freeIndices.begin(), state.freeIndices.end()) !=
            state.freeIndices.end()) {
        return false;
    }

    std::vector<bool> freeFlags(state.slots.size(), false);
    for (const auto index : state.freeIndices) {
        if (index >= state.slots.size()) return false;
        freeFlags[index] = true;
    }

    std::size_t aliveCount = 0;
    for (std::size_t index = 0; index < state.slots.size(); ++index) {
        const auto& slot = state.slots[index];
        if (slot.generation == 0 || slot.alive == freeFlags[index]) return false;
        if (slot.alive) ++aliveCount;
    }
    if (aliveCount != state.aliveCount) return false;

    slots_ = state.slots;
    freeIndices_ = state.freeIndices;
    aliveCount_ = state.aliveCount;
    return true;
}

std::uint64_t EntityRegistry::stableHash() const noexcept {
    std::uint64_t hash = FnvOffset;
    hashU64(hash, slots_.size());
    for (const auto& slot : slots_) {
        hashU64(hash, slot.generation);
        hashByte(hash, slot.alive ? 1u : 0u);
    }
    hashU64(hash, freeIndices_.size());
    for (const auto index : freeIndices_) hashU64(hash, index);
    hashU64(hash, aliveCount_);
    return hash;
}

FixedTickClock::FixedTickClock(std::uint32_t ticksPerSecond)
    : ticksPerSecond_(ticksPerSecond == 0 ? 60 : ticksPerSecond) {}

double FixedTickClock::fixedDeltaSeconds() const noexcept {
    return 1.0 / static_cast<double>(ticksPerSecond_);
}

double FixedTickClock::interpolationAlpha() const noexcept {
    return accumulatorSeconds_ / fixedDeltaSeconds();
}

FixedTickClock::AdvanceResult FixedTickClock::accumulate(
    double elapsedSeconds,
    std::uint32_t maximumSteps) {
    AdvanceResult result;
    if (!std::isfinite(elapsedSeconds) || elapsedSeconds <= 0.0 ||
        maximumSteps == 0) {
        result.interpolationAlpha = interpolationAlpha();
        return result;
    }

    accumulatorSeconds_ += elapsedSeconds;
    const auto delta = fixedDeltaSeconds();
    auto available = static_cast<std::uint64_t>(
        std::floor(accumulatorSeconds_ / delta));
    if (available > maximumSteps) {
        const auto droppedSteps = available - maximumSteps;
        result.droppedSeconds = static_cast<double>(droppedSteps) * delta;
        droppedSeconds_ += result.droppedSeconds;
        accumulatorSeconds_ -= result.droppedSeconds;
        available = maximumSteps;
    }

    result.steps = static_cast<std::uint32_t>(available);
    accumulatorSeconds_ -= static_cast<double>(result.steps) * delta;
    if (accumulatorSeconds_ < 0.0) accumulatorSeconds_ = 0.0;
    if (accumulatorSeconds_ >= delta) {
        accumulatorSeconds_ = std::fmod(accumulatorSeconds_, delta);
    }
    result.interpolationAlpha = interpolationAlpha();
    return result;
}

GameTick FixedTickClock::step() noexcept {
    if (tick_ != std::numeric_limits<GameTick>::max()) ++tick_;
    return tick_;
}

void FixedTickClock::reset(GameTick tick) noexcept {
    tick_ = tick;
    accumulatorSeconds_ = 0.0;
    droppedSeconds_ = 0.0;
}

FixedTickClock::State FixedTickClock::snapshot() const noexcept {
    return State{ticksPerSecond_, tick_, accumulatorSeconds_, droppedSeconds_};
}

bool FixedTickClock::restore(const State& state) noexcept {
    if (state.ticksPerSecond == 0 ||
        !std::isfinite(state.accumulatorSeconds) ||
        !std::isfinite(state.droppedSeconds) ||
        state.accumulatorSeconds < 0.0 || state.droppedSeconds < 0.0) {
        return false;
    }
    const auto delta = 1.0 / static_cast<double>(state.ticksPerSecond);
    if (state.accumulatorSeconds >= delta) return false;
    ticksPerSecond_ = state.ticksPerSecond;
    tick_ = state.tick;
    accumulatorSeconds_ = state.accumulatorSeconds;
    droppedSeconds_ = state.droppedSeconds;
    return true;
}

std::uint64_t FixedTickClock::stableHash() const noexcept {
    std::uint64_t hash = FnvOffset;
    hashU64(hash, ticksPerSecond_);
    hashU64(hash, tick_);
    hashU64(hash, canonicalDoubleBits(accumulatorSeconds_));
    hashU64(hash, canonicalDoubleBits(droppedSeconds_));
    return hash;
}

RandomStream::RandomStream(
    std::uint64_t seed,
    std::uint64_t sequence) noexcept {
    reseed(seed, sequence);
}

void RandomStream::reseed(
    std::uint64_t seed,
    std::uint64_t sequence) noexcept {
    state_ = 0;
    increment_ = (sequence << 1u) | 1u;
    static_cast<void>(nextUInt());
    state_ += seed;
    static_cast<void>(nextUInt());
}

std::uint32_t RandomStream::nextUInt() noexcept {
    const auto oldState = state_;
    state_ = oldState * 6364136223846793005ULL + increment_;
    const auto xorShifted = static_cast<std::uint32_t>(
        ((oldState >> 18u) ^ oldState) >> 27u);
    const auto rotation = static_cast<std::uint32_t>(oldState >> 59u);
    return (xorShifted >> rotation) |
        (xorShifted << ((0u - rotation) & 31u));
}

std::int32_t RandomStream::nextInt(
    std::int32_t minimumInclusive,
    std::int32_t maximumExclusive) noexcept {
    if (maximumExclusive <= minimumInclusive) return minimumInclusive;
    const auto range = static_cast<std::uint32_t>(
        static_cast<std::int64_t>(maximumExclusive) - minimumInclusive);
    const auto threshold = static_cast<std::uint32_t>(0u - range) % range;
    std::uint32_t value = 0;
    do {
        value = nextUInt();
    } while (value < threshold);
    return static_cast<std::int32_t>(
        static_cast<std::int64_t>(minimumInclusive) + value % range);
}

std::uint64_t RandomStream::nextUInt64() noexcept {
    return (static_cast<std::uint64_t>(nextUInt()) << 32u) | nextUInt();
}

double RandomStream::nextUnitDouble() noexcept {
    const auto value = nextUInt64() >> 11u;
    return static_cast<double>(value) * (1.0 / 9007199254740992.0);
}

bool RandomStream::restore(const State& state) noexcept {
    if ((state.increment & 1u) == 0) return false;
    state_ = state.state;
    increment_ = state.increment;
    return true;
}

std::uint64_t RandomStream::stableHash() const noexcept {
    std::uint64_t hash = FnvOffset;
    hashU64(hash, state_);
    hashU64(hash, increment_);
    return hash;
}

TimerId TickScheduler::scheduleAt(
    GameTick dueTick,
    std::uint64_t payload,
    GameTick intervalTicks,
    std::uint32_t repeatCount) {
    if ((repeatCount == RepeatForever || repeatCount > 1) && intervalTicks == 0) {
        return 0;
    }
    if (nextId_ == 0) return 0;
    const auto id = nextId_++;
    TaskState task{id, dueTick, intervalTicks, repeatCount, payload};
    tasks_.emplace(id, task);
    queue_.insert(QueueKey{dueTick, id});
    return id;
}

TimerId TickScheduler::scheduleAfter(
    GameTick currentTick,
    GameTick delayTicks,
    std::uint64_t payload,
    GameTick intervalTicks,
    std::uint32_t repeatCount) {
    const auto maximum = std::numeric_limits<GameTick>::max();
    const auto due = delayTicks > maximum - currentTick
        ? maximum
        : currentTick + delayTicks;
    return scheduleAt(due, payload, intervalTicks, repeatCount);
}

bool TickScheduler::cancel(TimerId id) {
    const auto found = tasks_.find(id);
    if (found == tasks_.end()) return false;
    queue_.erase(QueueKey{found->second.dueTick, id});
    tasks_.erase(found);
    return true;
}

bool TickScheduler::contains(TimerId id) const noexcept {
    return tasks_.find(id) != tasks_.end();
}

void TickScheduler::clear() noexcept {
    tasks_.clear();
    queue_.clear();
    nextId_ = 1;
}

std::vector<TickScheduler::FiredTask> TickScheduler::runDue(
    GameTick currentTick,
    std::size_t budget) {
    std::vector<FiredTask> fired;
    fired.reserve(std::min(budget, queue_.size()));
    while (!queue_.empty() && fired.size() < budget) {
        const auto key = *queue_.begin();
        if (key.dueTick > currentTick) break;
        queue_.erase(queue_.begin());
        const auto found = tasks_.find(key.id);
        if (found == tasks_.end()) continue;

        auto& task = found->second;
        const auto repeats = task.remainingFires == RepeatForever ||
            task.remainingFires > 1;
        fired.push_back(FiredTask{
            task.id, task.dueTick, task.payload, repeats});

        if (!repeats) {
            tasks_.erase(found);
            continue;
        }
        if (task.remainingFires != RepeatForever) --task.remainingFires;
        if (task.intervalTicks >
            std::numeric_limits<GameTick>::max() - task.dueTick) {
            tasks_.erase(found);
            continue;
        }
        task.dueTick += task.intervalTicks;
        queue_.insert(QueueKey{task.dueTick, task.id});
    }
    return fired;
}

TickScheduler::State TickScheduler::snapshot() const {
    State state;
    state.nextId = nextId_;
    state.tasks.reserve(tasks_.size());
    for (const auto& entry : tasks_) state.tasks.push_back(entry.second);
    return state;
}

bool TickScheduler::restore(const State& state) {
    if (state.nextId == 0) return false;
    std::map<TimerId, TaskState> tasks;
    std::set<QueueKey> queue;
    TimerId maximumId = 0;
    for (const auto& task : state.tasks) {
        if (task.id == 0 ||
            ((task.remainingFires == RepeatForever || task.remainingFires > 1) &&
             task.intervalTicks == 0) ||
            !tasks.emplace(task.id, task).second) {
            return false;
        }
        queue.insert(QueueKey{task.dueTick, task.id});
        maximumId = std::max(maximumId, task.id);
    }
    if (state.nextId <= maximumId) return false;
    nextId_ = state.nextId;
    tasks_ = std::move(tasks);
    queue_ = std::move(queue);
    return true;
}

std::uint64_t TickScheduler::stableHash() const noexcept {
    std::uint64_t hash = FnvOffset;
    hashU64(hash, nextId_);
    hashU64(hash, tasks_.size());
    for (const auto& entry : tasks_) {
        const auto& task = entry.second;
        hashU64(hash, task.id);
        hashU64(hash, task.dueTick);
        hashU64(hash, task.intervalTicks);
        hashU64(hash, task.remainingFires);
        hashU64(hash, task.payload);
    }
    return hash;
}

std::uint64_t stableGameplayValueHash(const GameplayValue& value) noexcept {
    std::uint64_t hash = FnvOffset;
    hashU64(hash, value.index());
    if (const auto* boolean = std::get_if<bool>(&value)) {
        hashByte(hash, *boolean ? 1u : 0u);
    } else if (const auto* integer = std::get_if<std::int32_t>(&value)) {
        hashU64(hash, static_cast<std::uint32_t>(*integer));
    } else if (const auto* longInteger = std::get_if<std::int64_t>(&value)) {
        hashU64(hash, static_cast<std::uint64_t>(*longInteger));
    } else if (const auto* number = std::get_if<double>(&value)) {
        hashU64(hash, canonicalDoubleBits(*number));
    } else if (const auto* text = std::get_if<std::string>(&value)) {
        hashString(hash, *text);
    } else if (const auto* entity = std::get_if<EntityId>(&value)) {
        hashU64(hash, entity->packed());
    }
    return hash;
}

EventSequence DeterministicEventQueue::enqueue(GameplayEvent event) {
    if (event.topic.empty() || nextSequence_ == 0) return 0;

    if (event.sequence == 0) {
        if (nextSequence_ == std::numeric_limits<EventSequence>::max()) return 0;
        event.sequence = nextSequence_++;
    } else {
        const auto duplicate = std::find_if(
            events_.begin(), events_.end(),
            [&](const auto& entry) {
                return entry.second.sequence == event.sequence;
            });
        if (duplicate != events_.end()) return 0;
        if (event.sequence >= nextSequence_) {
            if (event.sequence == std::numeric_limits<EventSequence>::max()) {
                return 0;
            }
            nextSequence_ = event.sequence + 1;
        }
    }

    const EventKey key{event.dueTick, event.sequence};
    if (!events_.emplace(key, std::move(event)).second) return 0;
    return key.sequence;
}

bool DeterministicEventQueue::cancel(EventSequence sequence) {
    const auto found = std::find_if(
        events_.begin(), events_.end(),
        [sequence](const auto& entry) {
            return entry.second.sequence == sequence;
        });
    if (found == events_.end()) return false;
    events_.erase(found);
    return true;
}

std::vector<GameplayEvent> DeterministicEventQueue::popDue(
    GameTick currentTick,
    std::size_t budget) {
    std::vector<GameplayEvent> result;
    result.reserve(std::min(budget, events_.size()));
    while (!events_.empty() && result.size() < budget) {
        const auto found = events_.begin();
        if (found->first.dueTick > currentTick) break;
        result.push_back(std::move(found->second));
        events_.erase(found);
    }
    return result;
}

void DeterministicEventQueue::clear() noexcept {
    events_.clear();
    nextSequence_ = 1;
}

DeterministicEventQueue::State DeterministicEventQueue::snapshot() const {
    State state;
    state.nextSequence = nextSequence_;
    state.events.reserve(events_.size());
    for (const auto& entry : events_) state.events.push_back(entry.second);
    return state;
}

bool DeterministicEventQueue::restore(const State& state) {
    if (state.nextSequence == 0) return false;
    std::map<EventKey, GameplayEvent> events;
    std::unordered_set<EventSequence> sequences;
    sequences.reserve(state.events.size());
    EventSequence maximumSequence = 0;
    for (const auto& event : state.events) {
        if (event.sequence == 0 || event.topic.empty() ||
            !sequences.insert(event.sequence).second ||
            !events.emplace(
                EventKey{event.dueTick, event.sequence}, event).second) {
            return false;
        }
        maximumSequence = std::max(maximumSequence, event.sequence);
    }
    if (state.nextSequence <= maximumSequence) return false;
    nextSequence_ = state.nextSequence;
    events_ = std::move(events);
    return true;
}

std::uint64_t DeterministicEventQueue::stableHash() const noexcept {
    std::uint64_t hash = FnvOffset;
    hashU64(hash, nextSequence_);
    hashU64(hash, events_.size());
    for (const auto& entry : events_) {
        const auto& event = entry.second;
        hashU64(hash, event.dueTick);
        hashU64(hash, event.sequence);
        hashU64(hash, event.target);
        hashString(hash, event.topic);
        hashU64(hash, event.arguments.size());
        for (const auto& argument : event.arguments) {
            hashU64(hash, stableGameplayValueHash(argument));
        }
    }
    return hash;
}

DeterministicGameplayRuntime::DeterministicGameplayRuntime(
    std::uint32_t ticksPerSecond,
    std::uint64_t randomSeed,
    std::uint64_t randomSequence) noexcept
    : clock_(ticksPerSecond), random_(randomSeed, randomSequence) {}

DeterministicGameplayRuntime::State
DeterministicGameplayRuntime::snapshot() const {
    return State{
        entities_.snapshot(),
        clock_.snapshot(),
        random_.snapshot(),
        scheduler_.snapshot(),
        events_.snapshot()};
}

bool DeterministicGameplayRuntime::restore(const State& state) {
    EntityRegistry entities;
    FixedTickClock clock;
    RandomStream random;
    TickScheduler scheduler;
    DeterministicEventQueue events;
    if (!entities.restore(state.entities) ||
        !clock.restore(state.clock) ||
        !random.restore(state.random) ||
        !scheduler.restore(state.scheduler) ||
        !events.restore(state.events)) {
        return false;
    }
    entities_ = std::move(entities);
    clock_ = std::move(clock);
    random_ = std::move(random);
    scheduler_ = std::move(scheduler);
    events_ = std::move(events);
    return true;
}

std::uint64_t DeterministicGameplayRuntime::stableHash() const noexcept {
    std::uint64_t hash = FnvOffset;
    hashU64(hash, entities_.stableHash());
    hashU64(hash, clock_.stableHash());
    hashU64(hash, random_.stableHash());
    hashU64(hash, scheduler_.stableHash());
    hashU64(hash, events_.stableHash());
    return hash;
}

} // namespace realscript::game
