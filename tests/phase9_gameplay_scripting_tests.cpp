#include "realscript/game/GameplayScripting.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace realscript::game;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void testHostTimersEventsSequencesAndState() {
    auto host = std::make_shared<GameplayHost>(20, 42, 7);
    const auto timer = host->scheduleCall(
        10, "OnTimer", 2, {std::int32_t{5}});
    require(timer != 0, "script timer was not scheduled");
    const auto subscription = host->subscribe(
        "unit.died", 10, "OnUnitDied", -10);
    require(subscription != 0, "script event subscription failed");
    const auto event = host->publish(
        "unit.died", {std::int32_t{3}}, 1);
    require(event != 0, "script event publication failed");

    const std::vector<ScriptSequenceStep> steps{
        {1, "OnWindup", {}},
        {2, "OnImpact", {std::int32_t{9}}},
    };
    const auto sequence = host->startSequence(10, steps);
    require(sequence != 0, "script sequence was not scheduled");

    const auto state = host->snapshot();
    const auto hash = host->stableHash();
    GameplayHost restored;
    require(restored.restore(state), "gameplay host restore failed");
    require(restored.stableHash() == hash,
        "gameplay host hash changed after restore");

    auto tick = host->core().clock().step();
    auto batch = host->drainDue(tick);
    require(batch.eventsConsumed == 1, "published event was not consumed");
    require(batch.calls.size() == 2,
        "event and first sequence step were not both dispatched");
    require(batch.calls[0].callback == "OnWindup" ||
            batch.calls[1].callback == "OnWindup",
        "sequence windup callback was not dispatched");
    require(batch.calls[0].callback == "OnUnitDied" ||
            batch.calls[1].callback == "OnUnitDied",
        "event subscriber callback was not dispatched");

    tick = host->core().clock().step();
    batch = host->drainDue(tick);
    require(batch.calls.size() == 1 && batch.calls.front().callback == "OnTimer",
        "timer callback did not fire on the requested tick");

    tick = host->core().clock().step();
    batch = host->drainDue(tick);
    require(batch.calls.size() == 1 && batch.calls.front().callback == "OnImpact",
        "sequence impact callback did not fire on the cumulative tick");
    require(!host->cancelSequence(sequence),
        "completed sequence unexpectedly remained cancellable");
}

void testMetadataAndContractState() {
    ScriptMetadataRegistry metadata;
    require(metadata.addTypeAttribute(
        "Game.Unit::Marine",
        ScriptAttribute{"Serializable", {{"version", std::int32_t{2}}}}),
        "type attribute registration failed");
    require(metadata.addMemberAttribute(
        "Game.Unit::Marine",
        "Health",
        ScriptAttribute{"Replicated", {{"channel", std::string{"state"}}}}),
        "member attribute registration failed");
    require(metadata.findTypeAttribute(
        "Game.Unit::Marine", "Serializable") != nullptr,
        "type attribute lookup failed");
    require(metadata.findMemberAttribute(
        "Game.Unit::Marine", "Health", "Replicated") != nullptr,
        "member attribute lookup failed");

    const auto state = metadata.snapshot();
    const auto hash = metadata.stableHash();
    ScriptMetadataRegistry restored;
    require(restored.restore(state), "metadata restore failed");
    require(restored.stableHash() == hash,
        "metadata hash changed after restore");
}

void testGeneratedGameplayBindings() {
    auto host = std::make_shared<GameplayHost>();
    GameApi api;
    require(installGameplayBindings(api, host),
        "gameplay host binding registration failed");
#ifndef REALSCRIPT_GAMEPLAY_STUB
    require(api.valid(), "gameplay host bindings left GameApi invalid");
    require(!api.generatedSources().empty(),
        "gameplay host bindings generated no script API source");
#endif
}

#ifndef REALSCRIPT_GAMEPLAY_STUB

std::string diagnosticsText(
    const realscript::diagnostics::DiagnosticBag& diagnostics) {
    std::string result;
    for (const auto& diagnostic : diagnostics.items()) {
        if (!result.empty()) result.push_back('\n');
        result += diagnostic.code + ": " + diagnostic.message;
    }
    return result;
}

const char* behaviorSource = R"(
module Game.RuntimeTest;

class Behavior
{
    int total;

    void OnStart()
    {
        total = 1;
    }

    void OnFixedUpdate(double deltaTime)
    {
        if (deltaTime > 0.0)
        {
            total = total + 1;
        }
    }

    void OnTimer(int amount)
    {
        total = total + amount;
    }

    void OnEvent(int amount)
    {
        total = total + amount;
    }

    int Read()
    {
        return total;
    }
}
)";

void testSceneDriverAndContracts() {
    GameApi api;
    GameScriptCompiler compiler(api);
    auto compiled = compiler.compile({{"runtime_test.rs", behaviorSource}});
    require(compiled.succeeded(),
        "gameplay scene fixture failed to compile:\n" +
        diagnosticsText(compiled.diagnostics));

    ScriptRuntime runtime(compiled.program);
    const ScriptContract contract{
        "RuntimeBehavior",
        {{"OnTimer", 1, true}, {"OnEvent", 1, true}, {"Read", 0, true}}};
    const auto report = validateScriptContract(
        runtime, "Game.RuntimeTest::Behavior", contract);
    require(report.satisfied(), "script behavior did not satisfy its contract");

    SceneScriptRuntime scene(runtime);
    require(scene.attach(10, "Game.RuntimeTest::Behavior"),
        "scene behavior attachment failed");
    scene.start();

    auto host = std::make_shared<GameplayHost>(30, 8, 3);
    require(host->scheduleCall(
        10, "OnTimer", 1, {std::int32_t{5}}) != 0,
        "scene timer scheduling failed");
    require(host->subscribe("damage", 10, "OnEvent") != 0,
        "scene event subscription failed");
    require(host->publish("damage", {std::int32_t{3}}, 1) != 0,
        "scene event publication failed");

    SceneGameplayDriver driver(scene, host);
    const auto steps = driver.advanceTicks(1);
    require(steps.size() == 1 && steps.front().callbacksSucceeded == 2,
        "scene gameplay callbacks were not delivered");
    require(driver.errors().empty(), "scene gameplay driver recorded errors");

    const auto read = scene.invoke(10, "Read");
    require(read.succeeded && std::get<std::int64_t>(read.value) == 10,
        "scene gameplay order produced the wrong behavior state");
}

#endif

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

    run("host timers, events, sequences, and state",
        testHostTimersEventsSequencesAndState);
    run("metadata and contract state", testMetadataAndContractState);
    run("generated gameplay bindings", testGeneratedGameplayBindings);
#ifndef REALSCRIPT_GAMEPLAY_STUB
    run("scene driver and contracts", testSceneDriverAndContracts);
#endif
    return failures == 0 ? 0 : 1;
}
