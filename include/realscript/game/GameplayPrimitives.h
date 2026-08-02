#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace realscript::game {

using GameTick = std::uint64_t;
using TimerId = std::uint64_t;
using EventSequence = std::uint64_t;

struct EntityId {
    static constexpr std::uint32_t InvalidIndex =
        std::numeric_limits<std::uint32_t>::max();

    std::uint32_t index = InvalidIndex;
    std::uint32_t generation = 0;

    [[nodiscard]] constexpr bool valid() const noexcept {
        return index != InvalidIndex && generation != 0;
    }

    [[nodiscard]] constexpr std::uint64_t packed() const noexcept {
        return (static_cast<std::uint64_t>(generation) << 32u) |
            static_cast<std::uint64_t>(index);
    }

    [[nodiscard]] static constexpr EntityId fromPacked(
        std::uint64_t value) noexcept {
        return EntityId{
            static_cast<std::uint32_t>(value & 0xffffffffu),
            static_cast<std::uint32_t>(value >> 32u)};
    }

    friend constexpr bool operator==(EntityId left, EntityId right) noexcept {
        return left.index == right.index && left.generation == right.generation;
    }

    friend constexpr bool operator!=(EntityId left, EntityId right) noexcept {
        return !(left == right);
    }

    friend constexpr bool operator<(EntityId left, EntityId right) noexcept {
        return left.packed() < right.packed();
    }
};

class EntityRegistry {
public:
    struct SlotState {
        std::uint32_t generation = 1;
        bool alive = false;
    };

    struct State {
        std::vector<SlotState> slots;
        std::vector<std::uint32_t> freeIndices;
        std::size_t aliveCount = 0;
    };

    [[nodiscard]] EntityId create();
    bool destroy(EntityId entity);
    [[nodiscard]] bool alive(EntityId entity) const noexcept;
    [[nodiscard]] std::size_t aliveCount() const noexcept { return aliveCount_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }
    void clear();

    [[nodiscard]] State snapshot() const;
    bool restore(const State& state);
    [[nodiscard]] std::uint64_t stableHash() const noexcept;

private:
    std::vector<SlotState> slots_;
    std::vector<std::uint32_t> freeIndices_;
    std::size_t aliveCount_ = 0;
};

class FixedTickClock {
public:
    struct AdvanceResult {
        std::uint32_t steps = 0;
        double interpolationAlpha = 0.0;
        double droppedSeconds = 0.0;
    };

    struct State {
        std::uint32_t ticksPerSecond = 60;
        GameTick tick = 0;
        double accumulatorSeconds = 0.0;
        double droppedSeconds = 0.0;
    };

    explicit FixedTickClock(std::uint32_t ticksPerSecond = 60);

    [[nodiscard]] std::uint32_t ticksPerSecond() const noexcept {
        return ticksPerSecond_;
    }
    [[nodiscard]] double fixedDeltaSeconds() const noexcept;
    [[nodiscard]] GameTick tick() const noexcept { return tick_; }
    [[nodiscard]] double interpolationAlpha() const noexcept;
    [[nodiscard]] double droppedSeconds() const noexcept { return droppedSeconds_; }

    [[nodiscard]] AdvanceResult accumulate(
        double elapsedSeconds,
        std::uint32_t maximumSteps = 8);
    [[nodiscard]] GameTick step() noexcept;
    void reset(GameTick tick = 0) noexcept;

    [[nodiscard]] State snapshot() const noexcept;
    bool restore(const State& state) noexcept;
    [[nodiscard]] std::uint64_t stableHash() const noexcept;

private:
    std::uint32_t ticksPerSecond_ = 60;
    GameTick tick_ = 0;
    double accumulatorSeconds_ = 0.0;
    double droppedSeconds_ = 0.0;
};

class RandomStream {
public:
    struct State {
        std::uint64_t state = 0;
        std::uint64_t increment = 1;
    };

    explicit RandomStream(
        std::uint64_t seed = 0x853c49e6748fea9bULL,
        std::uint64_t sequence = 0xda3e39cb94b95bdbULL) noexcept;

    void reseed(std::uint64_t seed, std::uint64_t sequence) noexcept;
    [[nodiscard]] std::uint32_t nextUInt() noexcept;
    [[nodiscard]] std::int32_t nextInt(
        std::int32_t minimumInclusive,
        std::int32_t maximumExclusive) noexcept;
    [[nodiscard]] std::uint64_t nextUInt64() noexcept;
    [[nodiscard]] double nextUnitDouble() noexcept;

    [[nodiscard]] State snapshot() const noexcept {
        return State{state_, increment_};
    }
    bool restore(const State& state) noexcept;
    [[nodiscard]] std::uint64_t stableHash() const noexcept;

private:
    std::uint64_t state_ = 0;
    std::uint64_t increment_ = 1;
};

class TickScheduler {
public:
    static constexpr std::uint32_t RepeatForever = 0;

    struct TaskState {
        TimerId id = 0;
        GameTick dueTick = 0;
        GameTick intervalTicks = 0;
        std::uint32_t remainingFires = 1;
        std::uint64_t payload = 0;
    };

    struct FiredTask {
        TimerId id = 0;
        GameTick scheduledTick = 0;
        std::uint64_t payload = 0;
        bool repeats = false;
    };

    struct State {
        TimerId nextId = 1;
        std::vector<TaskState> tasks;
    };

    [[nodiscard]] TimerId scheduleAt(
        GameTick dueTick,
        std::uint64_t payload,
        GameTick intervalTicks = 0,
        std::uint32_t repeatCount = 1);
    [[nodiscard]] TimerId scheduleAfter(
        GameTick currentTick,
        GameTick delayTicks,
        std::uint64_t payload,
        GameTick intervalTicks = 0,
        std::uint32_t repeatCount = 1);
    bool cancel(TimerId id);
    [[nodiscard]] bool contains(TimerId id) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return tasks_.size(); }
    void clear() noexcept;

    [[nodiscard]] std::vector<FiredTask> runDue(
        GameTick currentTick,
        std::size_t budget = std::numeric_limits<std::size_t>::max());

    [[nodiscard]] State snapshot() const;
    bool restore(const State& state);
    [[nodiscard]] std::uint64_t stableHash() const noexcept;

private:
    struct QueueKey {
        GameTick dueTick = 0;
        TimerId id = 0;

        friend bool operator<(const QueueKey& left, const QueueKey& right) noexcept {
            return left.dueTick < right.dueTick ||
                (left.dueTick == right.dueTick && left.id < right.id);
        }
    };

    TimerId nextId_ = 1;
    std::map<TimerId, TaskState> tasks_;
    std::set<QueueKey> queue_;
};

using GameplayValue = std::variant<
    std::monostate,
    bool,
    std::int32_t,
    std::int64_t,
    double,
    std::string,
    EntityId>;

struct GameplayEvent {
    GameTick dueTick = 0;
    EventSequence sequence = 0;
    std::uint64_t target = 0;
    std::string topic;
    std::vector<GameplayValue> arguments;
};

class DeterministicEventQueue {
public:
    struct State {
        EventSequence nextSequence = 1;
        std::vector<GameplayEvent> events;
    };

    [[nodiscard]] EventSequence enqueue(GameplayEvent event);
    bool cancel(EventSequence sequence);
    [[nodiscard]] std::vector<GameplayEvent> popDue(
        GameTick currentTick,
        std::size_t budget = std::numeric_limits<std::size_t>::max());
    [[nodiscard]] std::size_t size() const noexcept { return events_.size(); }
    void clear() noexcept;

    [[nodiscard]] State snapshot() const;
    bool restore(const State& state);
    [[nodiscard]] std::uint64_t stableHash() const noexcept;

private:
    struct EventKey {
        GameTick dueTick = 0;
        EventSequence sequence = 0;

        friend bool operator<(const EventKey& left, const EventKey& right) noexcept {
            return left.dueTick < right.dueTick ||
                (left.dueTick == right.dueTick && left.sequence < right.sequence);
        }
    };

    EventSequence nextSequence_ = 1;
    std::map<EventKey, GameplayEvent> events_;
};

class DeterministicGameplayRuntime {
public:
    struct State {
        EntityRegistry::State entities;
        FixedTickClock::State clock;
        RandomStream::State random;
        TickScheduler::State scheduler;
        DeterministicEventQueue::State events;
    };

    explicit DeterministicGameplayRuntime(
        std::uint32_t ticksPerSecond = 60,
        std::uint64_t randomSeed = 0x853c49e6748fea9bULL,
        std::uint64_t randomSequence = 0xda3e39cb94b95bdbULL) noexcept;

    [[nodiscard]] EntityRegistry& entities() noexcept { return entities_; }
    [[nodiscard]] const EntityRegistry& entities() const noexcept { return entities_; }
    [[nodiscard]] FixedTickClock& clock() noexcept { return clock_; }
    [[nodiscard]] const FixedTickClock& clock() const noexcept { return clock_; }
    [[nodiscard]] RandomStream& random() noexcept { return random_; }
    [[nodiscard]] const RandomStream& random() const noexcept { return random_; }
    [[nodiscard]] TickScheduler& scheduler() noexcept { return scheduler_; }
    [[nodiscard]] const TickScheduler& scheduler() const noexcept { return scheduler_; }
    [[nodiscard]] DeterministicEventQueue& events() noexcept { return events_; }
    [[nodiscard]] const DeterministicEventQueue& events() const noexcept { return events_; }

    [[nodiscard]] State snapshot() const;
    bool restore(const State& state);
    [[nodiscard]] std::uint64_t stableHash() const noexcept;

private:
    EntityRegistry entities_;
    FixedTickClock clock_;
    RandomStream random_;
    TickScheduler scheduler_;
    DeterministicEventQueue events_;
};

[[nodiscard]] std::uint64_t stableGameplayValueHash(
    const GameplayValue& value) noexcept;

} // namespace realscript::game
