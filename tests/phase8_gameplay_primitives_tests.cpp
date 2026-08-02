#include "realscript/game/GameplayPrimitives.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using namespace realscript::game;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void testGenerationCheckedEntities() {
    EntityRegistry registry;
    const auto first = registry.create();
    const auto second = registry.create();
    require(first.valid() && second.valid(), "entity allocation failed");
    require(registry.alive(first), "first entity was not alive");
    require(registry.destroy(first), "entity destruction failed");
    require(!registry.alive(first), "stale entity remained alive");

    const auto replacement = registry.create();
    require(replacement.index == first.index, "lowest free slot was not reused");
    require(replacement.generation != first.generation,
        "generation was not advanced on slot reuse");
    require(registry.alive(replacement), "replacement entity was not alive");

    const auto state = registry.snapshot();
    EntityRegistry restored;
    require(restored.restore(state), "entity registry restore failed");
    require(restored.stableHash() == registry.stableHash(),
        "entity registry hash changed after restore");
}

void testFixedTickClockAndDropBudget() {
    FixedTickClock clock(10);
    const auto advance = clock.accumulate(0.55, 3);
    require(advance.steps == 3, "fixed-tick step budget was not enforced");
    require(advance.droppedSeconds > 0.19 && advance.droppedSeconds < 0.21,
        "fixed-tick dropped time was incorrect");
    for (std::uint32_t index = 0; index < advance.steps; ++index) {
        static_cast<void>(clock.step());
    }
    require(clock.tick() == 3, "fixed-tick clock did not advance");

    const auto state = clock.snapshot();
    FixedTickClock restored;
    require(restored.restore(state), "fixed-tick clock restore failed");
    require(restored.stableHash() == clock.stableHash(),
        "fixed-tick clock hash changed after restore");
}

void testDeterministicRandomStream() {
    RandomStream first(42, 7);
    RandomStream second(42, 7);
    for (int index = 0; index < 128; ++index) {
        require(first.nextUInt() == second.nextUInt(),
            "equal random streams diverged");
    }
    for (int index = 0; index < 128; ++index) {
        const auto value = first.nextInt(-5, 12);
        require(value >= -5 && value < 12, "bounded random value was out of range");
    }
}

void testSchedulerOrderingAndSnapshot() {
    TickScheduler scheduler;
    const auto repeating = scheduler.scheduleAt(2, 20, 2, 3);
    const auto first = scheduler.scheduleAt(1, 10);
    const auto second = scheduler.scheduleAt(1, 11);
    require(repeating != 0 && first != 0 && second != 0,
        "scheduler rejected valid tasks");

    auto due = scheduler.runDue(1);
    require(due.size() == 2, "wrong number of tasks fired at tick one");
    require(due[0].id == first && due[1].id == second,
        "same-tick tasks did not use stable id order");

    due = scheduler.runDue(6);
    require(due.size() == 3, "repeating task did not fire three times");
    require(!scheduler.contains(repeating), "finite repeating task remained scheduled");

    const auto timer = scheduler.scheduleAt(9, 99, 3, TickScheduler::RepeatForever);
    require(timer != 0, "infinite timer creation failed");
    const auto state = scheduler.snapshot();
    TickScheduler restored;
    require(restored.restore(state), "scheduler restore failed");
    require(restored.stableHash() == scheduler.stableHash(),
        "scheduler hash changed after restore");
}

void testEventOrderingAndAggregateState() {
    DeterministicGameplayRuntime runtime(30, 123, 5);
    const auto entity = runtime.entities().create();
    require(entity.valid(), "aggregate entity creation failed");
    const auto damageEvent = runtime.events().enqueue(
        GameplayEvent{2, 0, entity.packed(), "damage", {std::int64_t{4}}});
    const auto spawnEvent = runtime.events().enqueue(
        GameplayEvent{1, 0, entity.packed(), "spawn", {std::string{"marine"}}});
    require(damageEvent != 0 && spawnEvent != 0, "aggregate event creation failed");
    const auto timer = runtime.scheduler().scheduleAt(3, 77);
    require(timer != 0, "aggregate timer creation failed");

    auto due = runtime.events().popDue(1);
    require(due.size() == 1 && due.front().topic == "spawn",
        "event queue did not use tick order");

    const auto state = runtime.snapshot();
    const auto hash = runtime.stableHash();
    DeterministicGameplayRuntime restored;
    require(restored.restore(state), "aggregate runtime restore failed");
    require(restored.stableHash() == hash,
        "aggregate runtime hash changed after restore");
}

} // namespace

int main() {
    int failures = 0;
    const auto run = [&](const char* name, auto test) {
        try {
            test();
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
        }
    };

    run("generation-checked entities", testGenerationCheckedEntities);
    run("fixed-tick clock", testFixedTickClockAndDropBudget);
    run("deterministic random stream", testDeterministicRandomStream);
    run("scheduler ordering and snapshot", testSchedulerOrderingAndSnapshot);
    run("event ordering and aggregate state", testEventOrderingAndAggregateState);
    return failures == 0 ? 0 : 1;
}
