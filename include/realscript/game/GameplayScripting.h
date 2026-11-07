#pragma once

#include "realscript/game/GameplayMetadata.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace realscript::game {

using SubscriptionId = std::uint64_t;
using SequenceId = std::uint64_t;
using ScriptPayloadId = std::uint64_t;
using AsyncOperationId = std::uint64_t;

struct ScriptAsyncOperationState {
    AsyncOperationId id = 0;
    std::string name;
    bool completed = false;
    GameplayValue result{};
};

struct ScheduledScriptCall {
    ScriptPayloadId payloadId = 0;
    TimerId timerId = 0;
    SceneEntityId target = 0;
    std::string callback;
    std::vector<GameplayValue> arguments;
};

struct ScriptEventSubscription {
    SubscriptionId id = 0;
    std::string topic;
    SceneEntityId target = 0;
    std::string callback;
    std::int32_t priority = 0;
    bool once = false;
};

struct ScriptSequenceStep {
    GameTick delayTicks = 1;
    std::string callback;
    std::vector<GameplayValue> arguments;
};

struct ScriptSequenceState {
    SequenceId id = 0;
    std::vector<TimerId> timers;
};

struct ScriptDispatchCall {
    SceneEntityId target = 0;
    std::string callback;
    std::vector<GameplayValue> arguments;
    TimerId sourceTimer = 0;
    EventSequence sourceEvent = 0;
};

struct ScriptDispatchBatch {
    std::vector<ScriptDispatchCall> calls;
    std::size_t timersConsumed = 0;
    std::size_t eventsConsumed = 0;
};

class GameplayHost {
public:
    struct State {
        DeterministicGameplayRuntime::State core;
        ScriptPayloadId nextPayloadId = 1;
        SubscriptionId nextSubscriptionId = 1;
        SequenceId nextSequenceId = 1;
        AsyncOperationId nextAsyncOpId = 1;
        std::vector<ScheduledScriptCall> calls;
        std::vector<ScriptEventSubscription> subscriptions;
        std::vector<ScriptSequenceState> sequences;
        std::vector<ScriptAsyncOperationState> asyncOps;
    };

    explicit GameplayHost(
        std::uint32_t ticksPerSecond = 60,
        std::uint64_t randomSeed = 0x853c49e6748fea9bULL,
        std::uint64_t randomSequence = 0xda3e39cb94b95bdbULL) noexcept;

    [[nodiscard]] DeterministicGameplayRuntime& core() noexcept { return core_; }
    [[nodiscard]] const DeterministicGameplayRuntime& core() const noexcept {
        return core_;
    }

    [[nodiscard]] TimerId scheduleCall(
        SceneEntityId target,
        std::string callback,
        GameTick delayTicks,
        std::vector<GameplayValue> arguments = {},
        GameTick intervalTicks = 0,
        std::uint32_t repeatCount = 1);
    bool cancelCall(TimerId timerId);

    [[nodiscard]] SequenceId startSequence(
        SceneEntityId target,
        const std::vector<ScriptSequenceStep>& steps,
        std::uint32_t repeatCount = 1);
    bool cancelSequence(SequenceId sequenceId);

    [[nodiscard]] SubscriptionId subscribe(
        std::string topic,
        SceneEntityId target,
        std::string callback,
        std::int32_t priority = 0);
    [[nodiscard]] SubscriptionId subscribeOnce(
        std::string topic,
        SceneEntityId target,
        std::string callback,
        std::int32_t priority = 0);
    bool unsubscribe(SubscriptionId subscriptionId);
    [[nodiscard]] std::size_t subscriberCount(std::string_view topic) const noexcept;
    std::size_t cancelEntityCalls(SceneEntityId target);

    [[nodiscard]] EventSequence publish(
        std::string topic,
        std::vector<GameplayValue> arguments = {},
        GameTick delayTicks = 1);

    AsyncOperationId createAsyncOperation(std::string name = {});
    bool completeAsyncOperation(
        AsyncOperationId operationId,
        GameplayValue result = {},
        GameTick delayTicks = 1);
    [[nodiscard]] bool isAsyncOperationCompleted(AsyncOperationId operationId) const noexcept;
    [[nodiscard]] std::optional<GameplayValue> getAsyncOperationResult(
        AsyncOperationId operationId) const;
    SubscriptionId waitAsyncOperation(
        AsyncOperationId operationId,
        SceneEntityId target,
        std::string callback);
    [[nodiscard]] EventSequence enqueueDirect(
        SceneEntityId target,
        std::string callback,
        std::vector<GameplayValue> arguments = {},
        GameTick delayTicks = 1);
    bool cancelEvent(EventSequence sequence);

    [[nodiscard]] ScriptDispatchBatch drainDue(
        GameTick currentTick,
        std::size_t timerBudget = std::numeric_limits<std::size_t>::max(),
        std::size_t eventBudget = std::numeric_limits<std::size_t>::max());

    [[nodiscard]] State snapshot() const;
    bool restore(const State& state);
    [[nodiscard]] std::uint64_t stableHash() const noexcept;

private:
    bool eraseCall(TimerId timerId, bool cancelScheduler);
    void detachTimerFromSequences(TimerId timerId);

    DeterministicGameplayRuntime core_;
    ScriptPayloadId nextPayloadId_ = 1;
    SubscriptionId nextSubscriptionId_ = 1;
    SequenceId nextSequenceId_ = 1;
    AsyncOperationId nextAsyncOpId_ = 1;
    std::map<ScriptPayloadId, ScheduledScriptCall> calls_;
    std::map<TimerId, ScriptPayloadId> timerToPayload_;
    std::map<SubscriptionId, ScriptEventSubscription> subscriptions_;
    std::map<SequenceId, ScriptSequenceState> sequences_;
    std::map<TimerId, SequenceId> timerToSequence_;
    std::map<AsyncOperationId, ScriptAsyncOperationState> asyncOps_;
};

[[nodiscard]] runtime::Value toRuntimeValue(const GameplayValue& value);
[[nodiscard]] std::optional<GameplayValue> toGameplayValue(
    const runtime::Value& value);

class SceneGameplayDriver {
public:
    struct Options {
        std::uint32_t maximumCatchUpSteps = 8;
        std::size_t timerBudgetPerTick = 4096;
        std::size_t eventBudgetPerTick = 4096;
        bool evaluateTriggers = true;
        bool flushSceneEvents = true;
    };

    struct StepResult {
        GameTick tick = 0;
        std::size_t callbacksAttempted = 0;
        std::size_t callbacksSucceeded = 0;
        std::size_t timersConsumed = 0;
        std::size_t eventsConsumed = 0;
        std::size_t triggersFired = 0;
        std::size_t sceneEventsFlushed = 0;
    };

    struct AdvanceResult {
        FixedTickClock::AdvanceResult clock;
        std::vector<StepResult> steps;
    };

    SceneGameplayDriver(
        SceneScriptRuntime& scene,
        std::shared_ptr<GameplayHost> host);

    [[nodiscard]] StepResult step();
    [[nodiscard]] StepResult step(const Options& options);
    [[nodiscard]] AdvanceResult advance(double elapsedSeconds);
    [[nodiscard]] AdvanceResult advance(
        double elapsedSeconds,
        const Options& options);
    [[nodiscard]] std::vector<StepResult> advanceTicks(std::uint32_t ticks);
    [[nodiscard]] std::vector<StepResult> advanceTicks(
        std::uint32_t ticks,
        const Options& options);

    [[nodiscard]] std::shared_ptr<GameplayHost> host() const noexcept {
        return host_;
    }
    [[nodiscard]] const std::vector<SceneScriptError>& errors() const noexcept {
        return errors_;
    }
    void clearErrors() { errors_.clear(); }

private:
    SceneScriptRuntime& scene_;
    std::shared_ptr<GameplayHost> host_;
    std::vector<SceneScriptError> errors_;
};

bool installGameplayBindings(
    GameApi& api,
    std::shared_ptr<GameplayHost> host,
    std::string moduleName = "RealScript.Game");

} // namespace realscript::game
