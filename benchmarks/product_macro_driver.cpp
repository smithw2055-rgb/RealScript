#include "realscript/game/GameScripting.h"
#include "realscript/game/GameplayScripting.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

using Clock = std::chrono::steady_clock;

double milliseconds(Clock::time_point started, Clock::time_point stopped) {
    return std::chrono::duration<double, std::milli>(stopped - started).count();
}

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string diagnosticsText(
    const realscript::diagnostics::DiagnosticBag& diagnostics) {
    std::string result;
    for (const auto& diagnostic : diagnostics.items()) {
        result += diagnostic.code + ": " + diagnostic.message + "\n";
    }
    return result;
}

} // namespace

int main() {
    try {
        constexpr std::uint64_t entityCount = 10'000;
        realscript::game::GameApi api;
        auto host = std::make_shared<realscript::game::GameplayHost>(30, 11, 17);
        require(realscript::game::installGameplayBindings(api, host),
            "failed to install gameplay bindings");
        realscript::game::GameScriptCompiler compiler(api);
        const auto compiled = compiler.compile({{"product_macro.rs", R"(
module Bench.ProductMacro;
import RealScript.Game;

class Behavior
{
    int result;

    sequence Run(long target)
    {
        yield wait_ticks(1);
        result = result + 1;
    }

    int Read() { return result; }
}
)"}});
        require(compiled.succeeded(),
            "product macro source failed to compile:\n" +
                diagnosticsText(compiled.diagnostics));

        realscript::game::ScriptRuntime scripts(compiled.program);
        realscript::game::SceneScriptRuntime scene(scripts);
        realscript::runtime::ExecutionOptions raw;
        raw.limits.gcWorkBudget = 0;
        scene.setExecutionOptions(raw);
        for (std::uint64_t entity = 1; entity <= entityCount; ++entity) {
            require(scene.attach(entity, "Bench.ProductMacro::Behavior"),
                "failed to attach product macro behavior");
        }
        scene.start();
        for (std::uint64_t entity = 1; entity <= entityCount; ++entity) {
            const auto started = scene.invoke(
                entity,
                "Run",
                {realscript::runtime::LongValue{
                    static_cast<std::int64_t>(entity)}});
            require(started.succeeded,
                "failed to start product macro sequence");
        }

        realscript::game::SceneGameplayDriver driver(scene, host);
        realscript::game::SceneGameplayDriver::Options options;
        options.timerBudgetPerTick = entityCount + 1;
        options.eventBudgetPerTick = entityCount + 1;
        options.evaluateTriggers = false;
        options.flushSceneEvents = false;

        const auto snapshotStarted = Clock::now();
        const auto heapSnapshot = scripts.heap()->snapshot();
        const auto hostSnapshot = host->snapshot();
        const auto snapshotStopped = Clock::now();

        const auto resumeStarted = Clock::now();
        const auto resumed = driver.advanceTicks(1, options);
        const auto resumeStopped = Clock::now();
        require(resumed.size() == 1 &&
                resumed.front().callbacksSucceeded == entityCount,
            "product macro did not resume every sequence");

        realscript::runtime::RuntimeError restoreError;
        const auto restoreStarted = Clock::now();
        const auto heapRestored = scripts.heap()->restore(
            heapSnapshot, &restoreError);
        require(heapRestored,
            "heap restore failed: " + restoreError.message);
        require(host->restore(hostSnapshot), "gameplay host restore failed");
        const auto restoreStopped = Clock::now();

        const auto replayStarted = Clock::now();
        const auto replayed = driver.advanceTicks(1, options);
        const auto replayStopped = Clock::now();
        require(replayed.size() == 1 &&
                replayed.front().callbacksSucceeded == entityCount,
            "product macro replay did not resume every sequence");

        const auto resumeMs = milliseconds(resumeStarted, resumeStopped);
        const auto replayMs = milliseconds(replayStarted, replayStopped);
        std::cout << "{\"entities\":" << entityCount
            << ",\"resumeMilliseconds\":" << resumeMs
            << ",\"resumeNanosecondsPerCallback\":"
            << resumeMs * 1'000'000.0 / static_cast<double>(entityCount)
            << ",\"snapshotMilliseconds\":"
            << milliseconds(snapshotStarted, snapshotStopped)
            << ",\"snapshotObjects\":" << heapSnapshot.objects.size()
            << ",\"snapshotRoots\":" << heapSnapshot.roots.size()
            << ",\"restoreMilliseconds\":"
            << milliseconds(restoreStarted, restoreStopped)
            << ",\"replayMilliseconds\":" << replayMs
            << ",\"replayNanosecondsPerCallback\":"
            << replayMs * 1'000'000.0 / static_cast<double>(entityCount)
            << "}\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "rsbench_product: " << exception.what() << '\n';
        return 1;
    }
}
