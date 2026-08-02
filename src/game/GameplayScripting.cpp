#include "realscript/game/GameplayScripting.h"

#include <algorithm>
#include <limits>
#include <set>
#include <utility>

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

GameTick normalizedDelay(GameTick delayTicks) noexcept {
    return delayTicks == 0 ? 1 : delayTicks;
}

GameTick saturatedAdd(GameTick left, GameTick right) noexcept {
    return right > std::numeric_limits<GameTick>::max() - left
        ? std::numeric_limits<GameTick>::max()
        : left + right;
}

bool validCall(const ScheduledScriptCall& call) {
    return call.payloadId != 0 && call.timerId != 0 && call.target != 0 &&
        !call.callback.empty();
}

} // namespace

GameplayHost::GameplayHost(
    std::uint32_t ticksPerSecond,
    std::uint64_t randomSeed,
    std::uint64_t randomSequence) noexcept
    : core_(ticksPerSecond, randomSeed, randomSequence) {}

TimerId GameplayHost::scheduleCall(
    SceneEntityId target,
    std::string callback,
    GameTick delayTicks,
    std::vector<GameplayValue> arguments,
    GameTick intervalTicks,
    std::uint32_t repeatCount) {
    if (target == 0 || callback.empty() || nextPayloadId_ == 0) return 0;
    if ((repeatCount == TickScheduler::RepeatForever || repeatCount > 1) &&
        intervalTicks == 0) {
        return 0;
    }

    const auto payloadId = nextPayloadId_++;
    const auto timerId = core_.scheduler().scheduleAfter(
        core_.clock().tick(),
        normalizedDelay(delayTicks),
        payloadId,
        intervalTicks,
        repeatCount);
    if (timerId == 0) return 0;

    ScheduledScriptCall call{
        payloadId,
        timerId,
        target,
        std::move(callback),
        std::move(arguments)};
    calls_.emplace(payloadId, std::move(call));
    timerToPayload_.emplace(timerId, payloadId);
    return timerId;
}

bool GameplayHost::eraseCall(TimerId timerId, bool cancelScheduler) {
    const auto timerFound = timerToPayload_.find(timerId);
    if (timerFound == timerToPayload_.end()) return false;
    const auto payloadId = timerFound->second;
    if (cancelScheduler) static_cast<void>(core_.scheduler().cancel(timerId));
    timerToPayload_.erase(timerFound);
    calls_.erase(payloadId);
    detachTimerFromSequences(timerId);
    return true;
}

bool GameplayHost::cancelCall(TimerId timerId) {
    return eraseCall(timerId, true);
}

void GameplayHost::detachTimerFromSequences(TimerId timerId) {
    const auto found = timerToSequence_.find(timerId);
    if (found == timerToSequence_.end()) return;
    const auto sequenceId = found->second;
    timerToSequence_.erase(found);
    const auto sequenceFound = sequences_.find(sequenceId);
    if (sequenceFound == sequences_.end()) return;
    auto& timers = sequenceFound->second.timers;
    timers.erase(std::remove(timers.begin(), timers.end(), timerId), timers.end());
    if (timers.empty()) sequences_.erase(sequenceFound);
}

SequenceId GameplayHost::startSequence(
    SceneEntityId target,
    const std::vector<ScriptSequenceStep>& steps,
    std::uint32_t repeatCount) {
    if (target == 0 || steps.empty() || nextSequenceId_ == 0) return 0;
    GameTick cycleTicks = 0;
    for (const auto& step : steps) {
        if (step.callback.empty()) return 0;
        const auto delay = normalizedDelay(step.delayTicks);
        if (cycleTicks == std::numeric_limits<GameTick>::max()) return 0;
        cycleTicks = saturatedAdd(cycleTicks, delay);
    }
    if (cycleTicks == std::numeric_limits<GameTick>::max()) return 0;

    const auto sequenceId = nextSequenceId_++;
    ScriptSequenceState sequence;
    sequence.id = sequenceId;
    GameTick dueOffset = 0;
    for (const auto& step : steps) {
        dueOffset = saturatedAdd(dueOffset, normalizedDelay(step.delayTicks));
        const auto timerId = scheduleCall(
            target,
            step.callback,
            dueOffset,
            step.arguments,
            repeatCount == 1 ? 0 : cycleTicks,
            repeatCount);
        if (timerId == 0) {
            for (const auto created : sequence.timers) {
                static_cast<void>(cancelCall(created));
            }
            return 0;
        }
        sequence.timers.push_back(timerId);
        timerToSequence_.emplace(timerId, sequenceId);
    }
    sequences_.emplace(sequenceId, std::move(sequence));
    return sequenceId;
}

bool GameplayHost::cancelSequence(SequenceId sequenceId) {
    const auto found = sequences_.find(sequenceId);
    if (found == sequences_.end()) return false;
    const auto timers = found->second.timers;
    for (const auto timerId : timers) {
        static_cast<void>(eraseCall(timerId, true));
    }
    sequences_.erase(sequenceId);
    return true;
}

SubscriptionId GameplayHost::subscribe(
    std::string topic,
    SceneEntityId target,
    std::string callback,
    std::int32_t priority) {
    if (topic.empty() || target == 0 || callback.empty() ||
        nextSubscriptionId_ == 0) {
        return 0;
    }
    const auto id = nextSubscriptionId_++;
    subscriptions_.emplace(id, ScriptEventSubscription{
        id, std::move(topic), target, std::move(callback), priority});
    return id;
}

bool GameplayHost::unsubscribe(SubscriptionId subscriptionId) {
    return subscriptions_.erase(subscriptionId) != 0;
}

EventSequence GameplayHost::publish(
    std::string topic,
    std::vector<GameplayValue> arguments,
    GameTick delayTicks) {
    return core_.events().enqueue(GameplayEvent{
        saturatedAdd(core_.clock().tick(), normalizedDelay(delayTicks)),
        0,
        0,
        std::move(topic),
        std::move(arguments)});
}

EventSequence GameplayHost::enqueueDirect(
    SceneEntityId target,
    std::string callback,
    std::vector<GameplayValue> arguments,
    GameTick delayTicks) {
    if (target == 0) return 0;
    return core_.events().enqueue(GameplayEvent{
        saturatedAdd(core_.clock().tick(), normalizedDelay(delayTicks)),
        0,
        target,
        std::move(callback),
        std::move(arguments)});
}

bool GameplayHost::cancelEvent(EventSequence sequence) {
    return core_.events().cancel(sequence);
}

ScriptDispatchBatch GameplayHost::drainDue(
    GameTick currentTick,
    std::size_t timerBudget,
    std::size_t eventBudget) {
    ScriptDispatchBatch batch;
    const auto fired = core_.scheduler().runDue(currentTick, timerBudget);
    batch.timersConsumed = fired.size();
    for (const auto& task : fired) {
        const auto callFound = calls_.find(task.payload);
        if (callFound == calls_.end()) continue;
        const auto& call = callFound->second;
        batch.calls.push_back(ScriptDispatchCall{
            call.target,
            call.callback,
            call.arguments,
            task.id,
            0});
        if (!task.repeats) static_cast<void>(eraseCall(task.id, false));
    }

    const auto events = core_.events().popDue(currentTick, eventBudget);
    batch.eventsConsumed = events.size();
    for (const auto& event : events) {
        if (event.target != 0) {
            batch.calls.push_back(ScriptDispatchCall{
                event.target,
                event.topic,
                event.arguments,
                0,
                event.sequence});
            continue;
        }

        std::vector<const ScriptEventSubscription*> subscribers;
        for (const auto& entry : subscriptions_) {
            if (entry.second.topic == event.topic) {
                subscribers.push_back(&entry.second);
            }
        }
        std::sort(
            subscribers.begin(), subscribers.end(),
            [](const auto* left, const auto* right) {
                return left->priority < right->priority ||
                    (left->priority == right->priority && left->id < right->id);
            });
        for (const auto* subscriber : subscribers) {
            batch.calls.push_back(ScriptDispatchCall{
                subscriber->target,
                subscriber->callback,
                event.arguments,
                0,
                event.sequence});
        }
    }
    return batch;
}

GameplayHost::State GameplayHost::snapshot() const {
    State state;
    state.core = core_.snapshot();
    state.nextPayloadId = nextPayloadId_;
    state.nextSubscriptionId = nextSubscriptionId_;
    state.nextSequenceId = nextSequenceId_;
    state.calls.reserve(calls_.size());
    for (const auto& entry : calls_) state.calls.push_back(entry.second);
    state.subscriptions.reserve(subscriptions_.size());
    for (const auto& entry : subscriptions_) {
        state.subscriptions.push_back(entry.second);
    }
    state.sequences.reserve(sequences_.size());
    for (const auto& entry : sequences_) state.sequences.push_back(entry.second);
    return state;
}

bool GameplayHost::restore(const State& state) {
    if (state.nextPayloadId == 0 || state.nextSubscriptionId == 0 ||
        state.nextSequenceId == 0) {
        return false;
    }

    DeterministicGameplayRuntime core;
    if (!core.restore(state.core)) return false;
    std::map<TimerId, std::uint64_t> schedulerPayloads;
    for (const auto& task : state.core.scheduler.tasks) {
        schedulerPayloads.emplace(task.id, task.payload);
    }

    std::map<ScriptPayloadId, ScheduledScriptCall> calls;
    std::map<TimerId, ScriptPayloadId> timerToPayload;
    ScriptPayloadId maximumPayload = 0;
    for (const auto& call : state.calls) {
        const auto timer = schedulerPayloads.find(call.timerId);
        if (!validCall(call) || timer == schedulerPayloads.end() ||
            timer->second != call.payloadId ||
            !calls.emplace(call.payloadId, call).second ||
            !timerToPayload.emplace(call.timerId, call.payloadId).second) {
            return false;
        }
        maximumPayload = std::max(maximumPayload, call.payloadId);
    }
    if (state.nextPayloadId <= maximumPayload) return false;

    std::map<SubscriptionId, ScriptEventSubscription> subscriptions;
    SubscriptionId maximumSubscription = 0;
    for (const auto& subscription : state.subscriptions) {
        if (subscription.id == 0 || subscription.topic.empty() ||
            subscription.target == 0 || subscription.callback.empty() ||
            !subscriptions.emplace(subscription.id, subscription).second) {
            return false;
        }
        maximumSubscription = std::max(maximumSubscription, subscription.id);
    }
    if (state.nextSubscriptionId <= maximumSubscription) return false;

    std::map<SequenceId, ScriptSequenceState> sequences;
    std::map<TimerId, SequenceId> timerToSequence;
    SequenceId maximumSequence = 0;
    for (const auto& sequence : state.sequences) {
        if (sequence.id == 0 || sequence.timers.empty() ||
            !sequences.emplace(sequence.id, sequence).second) {
            return false;
        }
        std::set<TimerId> uniqueTimers;
        for (const auto timerId : sequence.timers) {
            if (timerToPayload.find(timerId) == timerToPayload.end() ||
                !uniqueTimers.insert(timerId).second ||
                !timerToSequence.emplace(timerId, sequence.id).second) {
                return false;
            }
        }
        maximumSequence = std::max(maximumSequence, sequence.id);
    }
    if (state.nextSequenceId <= maximumSequence) return false;

    core_ = std::move(core);
    nextPayloadId_ = state.nextPayloadId;
    nextSubscriptionId_ = state.nextSubscriptionId;
    nextSequenceId_ = state.nextSequenceId;
    calls_ = std::move(calls);
    timerToPayload_ = std::move(timerToPayload);
    subscriptions_ = std::move(subscriptions);
    sequences_ = std::move(sequences);
    timerToSequence_ = std::move(timerToSequence);
    return true;
}

std::uint64_t GameplayHost::stableHash() const noexcept {
    std::uint64_t hash = FnvOffset;
    hashU64(hash, core_.stableHash());
    hashU64(hash, nextPayloadId_);
    hashU64(hash, nextSubscriptionId_);
    hashU64(hash, nextSequenceId_);
    hashU64(hash, calls_.size());
    for (const auto& entry : calls_) {
        const auto& call = entry.second;
        hashU64(hash, call.payloadId);
        hashU64(hash, call.timerId);
        hashU64(hash, call.target);
        hashString(hash, call.callback);
        hashU64(hash, call.arguments.size());
        for (const auto& argument : call.arguments) {
            hashU64(hash, stableGameplayValueHash(argument));
        }
    }
    hashU64(hash, subscriptions_.size());
    for (const auto& entry : subscriptions_) {
        const auto& subscription = entry.second;
        hashU64(hash, subscription.id);
        hashString(hash, subscription.topic);
        hashU64(hash, subscription.target);
        hashString(hash, subscription.callback);
        hashU64(hash, static_cast<std::uint32_t>(subscription.priority));
    }
    hashU64(hash, sequences_.size());
    for (const auto& entry : sequences_) {
        hashU64(hash, entry.second.id);
        hashU64(hash, entry.second.timers.size());
        for (const auto timerId : entry.second.timers) hashU64(hash, timerId);
    }
    return hash;
}

runtime::Value toRuntimeValue(const GameplayValue& value) {
    if (std::holds_alternative<std::monostate>(value)) {
        return runtime::Value{};
    }
    if (const auto* boolean = std::get_if<bool>(&value)) return *boolean;
    if (const auto* integer = std::get_if<std::int32_t>(&value)) {
        return static_cast<std::int64_t>(*integer);
    }
    if (const auto* integer = std::get_if<std::int64_t>(&value)) {
        return runtime::LongValue{*integer};
    }
    if (const auto* number = std::get_if<double>(&value)) return *number;
    if (const auto* text = std::get_if<std::string>(&value)) return *text;
    const auto entity = std::get<EntityId>(value);
    return runtime::LongValue{static_cast<std::int64_t>(entity.packed())};
}

std::optional<GameplayValue> toGameplayValue(const runtime::Value& value) {
    if (std::holds_alternative<std::monostate>(value)) {
        return GameplayValue{std::monostate{}};
    }
    if (const auto* boolean = std::get_if<bool>(&value)) {
        return GameplayValue{*boolean};
    }
    if (const auto* integer = std::get_if<std::int64_t>(&value)) {
        return GameplayValue{static_cast<std::int32_t>(*integer)};
    }
    if (const auto* integer = std::get_if<runtime::LongValue>(&value)) {
        return GameplayValue{integer->value};
    }
    if (const auto* number = std::get_if<double>(&value)) {
        return GameplayValue{*number};
    }
    if (const auto* text = std::get_if<std::string>(&value)) {
        return GameplayValue{*text};
    }
    return std::nullopt;
}

SceneGameplayDriver::SceneGameplayDriver(
    SceneScriptRuntime& scene,
    std::shared_ptr<GameplayHost> host)
    : scene_(scene), host_(std::move(host)) {}

SceneGameplayDriver::StepResult SceneGameplayDriver::step() {
    return step(Options{});
}

SceneGameplayDriver::StepResult SceneGameplayDriver::step(
    const Options& options) {
    StepResult result;
    if (!host_) return result;
    result.tick = host_->core().clock().step();
    const auto batch = host_->drainDue(
        result.tick,
        options.timerBudgetPerTick,
        options.eventBudgetPerTick);
    result.timersConsumed = batch.timersConsumed;
    result.eventsConsumed = batch.eventsConsumed;
    result.callbacksAttempted = batch.calls.size();

    for (const auto& call : batch.calls) {
        std::vector<runtime::Value> arguments;
        arguments.reserve(call.arguments.size());
        for (const auto& argument : call.arguments) {
            arguments.push_back(toRuntimeValue(argument));
        }
        const auto invoked = scene_.invoke(call.target, call.callback, arguments);
        if (invoked.succeeded) {
            ++result.callbacksSucceeded;
        } else {
            errors_.push_back(SceneScriptError{
                call.target, call.callback, invoked.error});
        }
    }

    scene_.fixedUpdate(host_->core().clock().fixedDeltaSeconds());
    if (options.evaluateTriggers) {
        result.triggersFired = scene_.evaluateTriggers();
    }
    if (options.flushSceneEvents) {
        result.sceneEventsFlushed = scene_.flushEvents();
    }
    return result;
}

SceneGameplayDriver::AdvanceResult SceneGameplayDriver::advance(
    double elapsedSeconds) {
    return advance(elapsedSeconds, Options{});
}

SceneGameplayDriver::AdvanceResult SceneGameplayDriver::advance(
    double elapsedSeconds,
    const Options& options) {
    AdvanceResult result;
    if (!host_) return result;
    result.clock = host_->core().clock().accumulate(
        elapsedSeconds, options.maximumCatchUpSteps);
    result.steps.reserve(result.clock.steps);
    for (std::uint32_t index = 0; index < result.clock.steps; ++index) {
        result.steps.push_back(step(options));
    }
    return result;
}

std::vector<SceneGameplayDriver::StepResult>
SceneGameplayDriver::advanceTicks(std::uint32_t ticks) {
    return advanceTicks(ticks, Options{});
}

std::vector<SceneGameplayDriver::StepResult>
SceneGameplayDriver::advanceTicks(
    std::uint32_t ticks,
    const Options& options) {
    std::vector<StepResult> results;
    results.reserve(ticks);
    for (std::uint32_t index = 0; index < ticks; ++index) {
        results.push_back(step(options));
    }
    return results;
}

bool installGameplayBindings(
    GameApi& api,
    std::shared_ptr<GameplayHost> host,
    std::string moduleName) {
    if (!host || moduleName.empty()) return false;
    bool valid = true;
    valid = api.function(moduleName, "CurrentTick", [host] {
        return static_cast<std::int64_t>(host->core().clock().tick());
    }) && valid;
    valid = api.function(moduleName, "CreateEntity", [host] {
        const auto entity = host->core().entities().create();
        return entity.valid()
            ? static_cast<std::int64_t>(entity.packed())
            : std::int64_t{0};
    }) && valid;
    valid = api.function(moduleName, "DestroyEntity", [host](std::int64_t value) {
        return host->core().entities().destroy(
            EntityId::fromPacked(static_cast<std::uint64_t>(value)));
    }) && valid;
    valid = api.function(moduleName, "IsEntityAlive", [host](std::int64_t value) {
        return host->core().entities().alive(
            EntityId::fromPacked(static_cast<std::uint64_t>(value)));
    }) && valid;
    valid = api.function(
        moduleName,
        "RandomInt",
        [host](int minimumInclusive, int maximumExclusive) {
            return host->core().random().nextInt(
                minimumInclusive, maximumExclusive);
        }) && valid;
    valid = api.function(
        moduleName,
        "Schedule",
        [host](std::int64_t target, std::string callback, int delayTicks) {
            if (delayTicks < 0) return std::int64_t{0};
            return static_cast<std::int64_t>(host->scheduleCall(
                static_cast<SceneEntityId>(target),
                std::move(callback),
                static_cast<GameTick>(delayTicks)));
        }) && valid;
    valid = api.function(
        moduleName,
        "ScheduleRepeating",
        [host](
            std::int64_t target,
            std::string callback,
            int delayTicks,
            int intervalTicks,
            int repeatCount) {
            if (delayTicks < 0 || intervalTicks <= 0 || repeatCount < 0) {
                return std::int64_t{0};
            }
            return static_cast<std::int64_t>(host->scheduleCall(
                static_cast<SceneEntityId>(target),
                std::move(callback),
                static_cast<GameTick>(delayTicks),
                {},
                static_cast<GameTick>(intervalTicks),
                static_cast<std::uint32_t>(repeatCount)));
        }) && valid;
    valid = api.function(moduleName, "CancelTimer", [host](std::int64_t timerId) {
        return timerId > 0 && host->cancelCall(static_cast<TimerId>(timerId));
    }) && valid;
    valid = api.function(
        moduleName,
        "Publish",
        [host](std::string topic, int delayTicks) {
            if (delayTicks < 0) return std::int64_t{0};
            return static_cast<std::int64_t>(host->publish(
                std::move(topic), {}, static_cast<GameTick>(delayTicks)));
        }) && valid;
    return valid;
}

} // namespace realscript::game
