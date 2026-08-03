#include "realscript/game/Gameplay.h"
#include "realscript/hot_reload/HotReload.h"
#include "realscript/syntax/Syntax.h"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

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

void testPersistedLocalsYieldBreakAndRollback() {
    realscript::game::GameApi api;
    auto host = std::make_shared<realscript::game::GameplayHost>(30, 11, 17);
    require(realscript::game::installGameplayBindings(api, host),
        "gameplay bindings failed");
    realscript::game::GameScriptCompiler compiler(api);
    const auto compiled = compiler.compile({{"phase22.rs", R"(
module Phase22.Coroutines;
import RealScript.Game;

class CounterEnumerator
{
    int index;
    int count;
    CounterEnumerator(int value) { index = -1; count = value; }
    bool MoveNext() { index = index + 1; return index < count; }
    int Current() { return index + 1; }
}

class CounterRange
{
    int count;
    CounterRange(int value) { count = value; }
    CounterEnumerator GetEnumerator()
    {
        return new CounterEnumerator(count);
    }
}

class Behavior
{
    int result;
    int selector;

    sequence Run(long target)
    {
        int total = 2;
        yield wait_ticks(1);
        total = total + 3;
        int doubled = total * 2;
        yield wait_ticks(1);
        result = total + doubled;
        yield break;
        result = 999;
    }

    sequence Loop(long target)
    {
        int index = 0;
        int sum = 0;
        while (index < 3)
        {
            sum = sum + index;
            index = index + 1;
            yield wait_ticks(1);
        }
        result = sum;
    }

    sequence Cancelable(long target)
    {
        result = 0;
        yield wait_ticks(4);
        result = 77;
    }

    sequence Branch(long target)
    {
        int value = selector;
        if (value == 1)
        {
            value = value + 4;
            yield wait_ticks(1);
            value = value + 5;
        }
        else
        {
            value = 99;
        }
        result = value;
    }

    sequence Nested(long target, int bonus)
    {
        int index = 0;
        int sum = 0;
        while (index < 3)
        {
            if (index == 1)
            {
                yield wait_ticks(1);
                sum = sum + 10;
            }
            else
            {
                yield wait_ticks(1);
                sum = sum + index;
            }
            index = index + 1;
        }
        result = sum + bonus;
    }

    sequence ForLoop(long target)
    {
        int sum = 0;
        for (int index = 0; index < 3; index = index + 1)
        {
            yield wait_ticks(1);
            sum = sum + index;
        }
        result = sum;
    }

    sequence DoLoop(long target)
    {
        int index = 0;
        int sum = 0;
        do
        {
            yield wait_ticks(1);
            sum = sum + index;
            index = index + 1;
        }
        while (index < 3);
        result = sum;
    }

    sequence Switcher(long target, int choice)
    {
        int value = 0;
        switch (choice)
        {
            case 1:
                yield wait_ticks(1);
                value = 10;
                break;
            default:
                yield wait_ticks(1);
                value = 20;
                break;
        }
        result = value;
    }

    sequence ForeachArray(long target)
    {
        int[] values = new int[3];
        values[0] = 4;
        values[1] = 5;
        values[2] = 6;
        int sum = 0;
        foreach (int value in values)
        {
            yield wait_ticks(1);
            sum = sum + value;
        }
        result = sum;
    }

    sequence ForeachList(long target)
    {
        List<int> values = new List<int>(1);
        values.Add(7);
        values.Add(8);
        values.Add(9);
        int sum = 0;
        foreach (int value in values)
        {
            yield wait_ticks(1);
            sum = sum + value;
        }
        result = sum;
    }

    sequence ForeachEnumerator(long target)
    {
        CounterRange values = new CounterRange(3);
        int sum = 0;
        foreach (int value in values)
        {
            yield wait_ticks(1);
            sum = sum + value;
        }
        result = sum;
    }

    sequence<int> Child(long target, int value)
    {
        int childValue = value;
        yield wait_ticks(2);
        return childValue + 1;
    }

    sequence Parent(long target)
    {
        result = 0;
        Child(target, 40);
        result = GetChildResult() + 1;
    }

    void Choose(int value) { selector = value; return; }
    int Read() { return result; }
}
)"}});
    require(compiled.succeeded(),
        "Phase 22 sequence compilation failed:\n" +
            diagnosticsText(compiled.diagnostics));

    const realscript::semantic::TypeSymbol* behavior = nullptr;
    for (const auto& module : compiled.modules) {
        for (const auto& type : module.types) {
            if (type.name == "Behavior") behavior = &type;
        }
    }
    require(behavior != nullptr, "Phase 22 behavior descriptor is missing");
    bool sawNestedContinuationDebugInfo = false;
    bool sawResultMetadata = false;
    for (const auto& module : compiled.modules) {
        for (const auto& sequence :
             module.languageMetadata.sequences) {
            if (sequence.typeName ==
                    "Phase22.Coroutines::Behavior" &&
                sequence.name == "Child" &&
                sequence.resultTypeName == "int") {
                sawResultMetadata = module.version.minor == 8;
            }
        }
        const auto encoded =
            realscript::bytecode::encodeModule(module);
        realscript::bytecode::Module decoded;
        realscript::diagnostics::DiagnosticBag decodeDiagnostics;
        require(realscript::bytecode::decodeModule(
                encoded, decoded, decodeDiagnostics) &&
                realscript::bytecode::encodeModule(decoded) ==
                    encoded,
            "Phase 22 result metadata bytecode round trip failed");
        for (const auto& function : module.functions) {
            if (function.name !=
                "Behavior.$sequence_Nested_1") continue;
            sawNestedContinuationDebugInfo =
                function.debugInfo.sourceName == "phase22.rs" &&
                !function.debugInfo.sequencePoints.empty();
            for (const auto& local : function.debugInfo.locals) {
                require(local.name.find("$sequence_") != 0,
                    "debugger exposed a compiler-owned sequence state local");
            }
        }
    }
    require(sawNestedContinuationDebugInfo,
        "sequence continuation lost its original source-map sequence points");
    require(sawResultMetadata,
        "declared sequence result identity was not retained in RSBC 0.8 metadata");
    std::size_t persistedLocals = 0;
    for (const auto& field : behavior->fields) {
        if (field.synthetic &&
            field.name.find("$sequence_local_Run_") == 0) ++persistedLocals;
    }
    require(persistedLocals == 2,
        "sequence locals were not promoted to precise managed fields");

    realscript::game::ScriptRuntime scripts(compiled.program);
    realscript::game::SceneScriptRuntime scene(scripts);
    require(scene.attach(21, "Phase22.Coroutines::Behavior"),
        "Phase 22 behavior attachment failed");
    scene.start();
    const auto started = scene.invoke(
        21, "Run", {realscript::runtime::LongValue{21}});
    require(started.succeeded, "Phase 22 sequence start failed");

    realscript::game::SceneGameplayDriver driver(scene, host);
    require(driver.advanceTicks(1).size() == 1,
        "first Phase 22 continuation did not run");
    const auto heapSnapshot = scripts.heap()->snapshot();
    const auto hostSnapshot = host->snapshot();

    require(driver.advanceTicks(1).size() == 1,
        "second Phase 22 continuation did not run");
    auto read = scene.invoke(21, "Read");
    require(read.succeeded && std::get<std::int64_t>(read.value) == 15,
        "persisted sequence locals produced the wrong result");

    realscript::runtime::RuntimeError restoreError;
    require(scripts.heap()->restore(heapSnapshot, &restoreError),
        "sequence heap rollback failed: " + restoreError.message);
    require(host->restore(hostSnapshot), "sequence scheduler rollback failed");
    require(driver.advanceTicks(1).size() == 1,
        "restored Phase 22 continuation did not replay");
    read = scene.invoke(21, "Read");
    require(read.succeeded && std::get<std::int64_t>(read.value) == 15,
        "sequence rollback/replay changed the result");

    const auto loop = scene.invoke(
        21, "Loop", {realscript::runtime::LongValue{21}});
    require(loop.succeeded, "loop sequence start failed");
    require(driver.advanceTicks(3).size() == 3,
        "loop sequence did not resume for each iteration");
    read = scene.invoke(21, "Read");
    require(read.succeeded && std::get<std::int64_t>(read.value) == 3,
        "loop suspension state produced the wrong result");

    const auto cancelable = scene.invoke(
        21, "Cancelable", {realscript::runtime::LongValue{21}});
    require(cancelable.succeeded, "cancelable sequence start failed");
    const auto cancelled = scene.invoke(21, "CancelCancelable");
    require(cancelled.succeeded && std::get<bool>(cancelled.value),
        "sequence cancellation did not cancel its pending timer");
    static_cast<void>(driver.advanceTicks(4));
    read = scene.invoke(21, "Read");
    require(read.succeeded && std::get<std::int64_t>(read.value) == 0,
        "a cancelled sequence executed its continuation");

    require(scene.invoke(21, "Choose", {std::int64_t{1}}).succeeded,
        "branch selector update failed");
    require(scene.invoke(
        21, "Branch", {realscript::runtime::LongValue{21}}).succeeded,
        "suspending branch sequence start failed");
    static_cast<void>(driver.advanceTicks(1));
    read = scene.invoke(21, "Read");
    require(read.succeeded && std::get<std::int64_t>(read.value) == 10,
        "suspending branch did not preserve its local state");

    require(scene.invoke(21, "Choose", {std::int64_t{0}}).succeeded,
        "false branch selector update failed");
    require(scene.invoke(
        21, "Branch", {realscript::runtime::LongValue{21}}).succeeded,
        "immediate branch sequence start failed");
    read = scene.invoke(21, "Read");
    require(read.succeeded && std::get<std::int64_t>(read.value) == 99,
        "non-suspending branch did not complete immediately");

    require(scene.invoke(
        21, "Nested", {realscript::runtime::LongValue{21}, std::int64_t{5}}).succeeded,
        "nested state-machine sequence start failed");
    require(driver.advanceTicks(3).size() == 3,
        "nested state-machine sequence did not resume three times");
    read = scene.invoke(21, "Read");
    require(read.succeeded && std::get<std::int64_t>(read.value) == 17,
        "nested branch/loop state-machine sequence produced the wrong result: " +
            (read.succeeded
                ? std::to_string(std::get<std::int64_t>(read.value))
                : read.error.message));

    require(scene.invoke(
        21, "Nested",
        {realscript::runtime::LongValue{21}, std::int64_t{100}}).succeeded,
        "first restartable state-machine sequence start failed");
    require(scene.invoke(
        21, "Nested",
        {realscript::runtime::LongValue{21}, std::int64_t{7}}).succeeded,
        "replacement state-machine sequence start failed");
    require(driver.advanceTicks(3).size() == 3,
        "restarting a sequence left an obsolete continuation scheduled");
    read = scene.invoke(21, "Read");
    require(read.succeeded && std::get<std::int64_t>(read.value) == 19,
        "restarted sequence did not use the replacement parameter state");

    require(scene.invoke(
        21, "ForLoop", {realscript::runtime::LongValue{21}}).succeeded,
        "for-loop state-machine sequence start failed");
    require(driver.advanceTicks(3).size() == 3,
        "for-loop state-machine sequence did not resume three times");
    read = scene.invoke(21, "Read");
    require(read.succeeded && std::get<std::int64_t>(read.value) == 3,
        "for-loop state-machine sequence produced the wrong result");

    require(scene.invoke(
        21, "DoLoop", {realscript::runtime::LongValue{21}}).succeeded,
        "do/while state-machine sequence start failed");
    require(driver.advanceTicks(3).size() == 3,
        "do/while state-machine sequence did not resume three times");
    read = scene.invoke(21, "Read");
    require(read.succeeded && std::get<std::int64_t>(read.value) == 3,
        "do/while state-machine sequence produced the wrong result");

    require(scene.invoke(
        21, "Switcher",
        {realscript::runtime::LongValue{21}, std::int64_t{1}}).succeeded,
        "switch state-machine sequence start failed");
    require(driver.advanceTicks(1).size() == 1,
        "switch state-machine sequence did not resume");
    read = scene.invoke(21, "Read");
    require(read.succeeded && std::get<std::int64_t>(read.value) == 10,
        "switch state-machine resumed the wrong case");

    require(scene.invoke(
        21, "ForeachArray",
        {realscript::runtime::LongValue{21}}).succeeded,
        "foreach state-machine sequence start failed");
    require(driver.advanceTicks(3).size() == 3,
        "foreach state-machine sequence did not resume three times");
    read = scene.invoke(21, "Read");
    require(read.succeeded && std::get<std::int64_t>(read.value) == 15,
        "foreach state-machine lost its collection/index/element state");

    require(scene.invoke(
        21, "ForeachList",
        {realscript::runtime::LongValue{21}}).succeeded,
        "collection foreach state-machine sequence start failed");
    require(driver.advanceTicks(3).size() == 3,
        "collection foreach state-machine did not resume three times");
    read = scene.invoke(21, "Read");
    require(read.succeeded && std::get<std::int64_t>(read.value) == 24,
        "collection foreach state-machine lost its indexed collection state");

    require(scene.invoke(
        21, "ForeachEnumerator",
        {realscript::runtime::LongValue{21}}).succeeded,
        "enumerator foreach state-machine sequence start failed");
    require(driver.advanceTicks(3).size() == 3,
        "enumerator foreach state-machine did not resume three times");
    read = scene.invoke(21, "Read");
    require(read.succeeded && std::get<std::int64_t>(read.value) == 6,
        "enumerator foreach advanced twice or lost Current across suspension");

    require(scene.invoke(
        21, "Parent",
        {realscript::runtime::LongValue{21}}).succeeded,
        "nested sequence composition start failed");
    const auto firstCompositionTick = driver.advanceTicks(1);
    const auto compositionHeapSnapshot = scripts.heap()->snapshot();
    const auto compositionHostSnapshot = host->snapshot();
    const auto secondCompositionTick = driver.advanceTicks(1);
    auto compositionEvents = firstCompositionTick;
    compositionEvents.insert(
        compositionEvents.end(),
        secondCompositionTick.begin(), secondCompositionTick.end());
    std::size_t compositionCallbacks = 0;
    for (const auto& step : compositionEvents) {
        compositionCallbacks += step.callbacksAttempted;
    }
    require(compositionEvents.size() == 2 &&
            compositionCallbacks == 3,
        "nested sequence composition used an invalid polling schedule: " +
            std::to_string(compositionCallbacks));
    read = scene.invoke(21, "Read");
    require(read.succeeded && std::get<std::int64_t>(read.value) == 42,
        "parent sequence continued before its child completed");
    const auto childCompleted = scene.invoke(
        21, "IsChildCompleted");
    const auto childResult = scene.invoke(
        21, "GetChildResult");
    require(childCompleted.succeeded &&
            std::get<bool>(childCompleted.value) &&
            childResult.succeeded &&
            std::get<std::int64_t>(childResult.value) == 41,
        "declared sequence result/completion accessors lost their state");

    require(scripts.heap()->restore(
                compositionHeapSnapshot, &restoreError),
        "nested sequence heap rollback failed: " + restoreError.message);
    require(host->restore(compositionHostSnapshot),
        "nested sequence scheduler rollback failed");
    const auto replayedCompositionTick = driver.advanceTicks(1);
    require(replayedCompositionTick.size() == 1 &&
            replayedCompositionTick.front().callbacksAttempted == 2,
        "nested sequence rollback did not replay child-before-parent ordering");
    read = scene.invoke(21, "Read");
    require(read.succeeded && std::get<std::int64_t>(read.value) == 42,
        "nested sequence rollback changed the composed result");
}

void testActiveSequenceHotReloadMigration() {
    const auto compile = [](const std::string& source) {
        realscript::compiler::Compilation compilation({{
            "phase22-reload.rs", source}});
        const auto build = compilation.build();
        require(!build.diagnostics.hasErrors(),
            "Phase 22 hot-reload fixture compilation failed:\n" +
                diagnosticsText(build.diagnostics));
        realscript::bytecode::Lowerer lowerer;
        std::vector<realscript::bytecode::Module> modules;
        for (const auto& module : build.modules) {
            modules.push_back(lowerer.lower(module));
        }
        return modules;
    };
    const auto source = [](int increment, bool addLayout) {
        return std::string(R"(
module Phase22.Reload;
long Schedule(long target, string callback, int delay)
{
    return target + delay;
}

bool CancelTimer(long timer) { return timer != 0; }
class Behavior
{
    int result;
    sequence Run(long target)
    {
        int persisted = 1;
)") +
            (addLayout ? "        int extra = 0;\n" : "") +
            R"(        yield wait_ticks(1);
        result = persisted + )" + std::to_string(increment) + R"(;
    }
    int Read() { return result; }
}
Behavior Create() { return new Behavior(); }
)";
    };

    realscript::runtime::RuntimeError error;
    auto linked = realscript::runtime::ProgramImage::link(
        compile(source(2, false)), error);
    require(linked.has_value(),
        "Phase 22 hot-reload fixture link failed: " + error.message);
    realscript::runtime::EngineRuntime runtime(
        std::make_shared<realscript::runtime::ProgramImage>(
            std::move(*linked)));
    const auto created = runtime.invoke("Phase22.Reload::Create");
    require(created.succeeded &&
            std::holds_alternative<realscript::runtime::ObjectRef>(
                created.value),
        "Phase 22 hot-reload object allocation failed");
    const auto object =
        std::get<realscript::runtime::ObjectRef>(created.value);
    auto root = runtime.heap()->retain(object);
    require(runtime.invoke(
        "Phase22.Reload::Behavior.Run",
        {object, realscript::runtime::LongValue{9}}).succeeded,
        "Phase 22 hot-reload sequence entry failed");

    auto accepted = realscript::hot_reload::apply(
        runtime, compile(source(3, false)));
    require(accepted.compatible && !accepted.changedFunctions.empty(),
        "body-only active sequence hot reload was rejected");
    require(runtime.invoke(
        "Phase22.Reload::Behavior.$sequence_Run_1", {object}).succeeded,
        "reloaded active sequence continuation failed");
    const auto read = runtime.invoke(
        "Phase22.Reload::Behavior.Read", {object});
    require(read.succeeded &&
            std::get<std::int64_t>(read.value) == 4,
        "active sequence did not preserve fields while adopting reloaded code");

    const auto rejected = realscript::hot_reload::prepare(
        *runtime.programSnapshot(), compile(source(3, true)));
    bool layoutIssue = false;
    for (const auto& issue : rejected.issues) {
        layoutIssue = layoutIssue ||
            issue.kind == realscript::hot_reload::ReloadIssueKind::
                TypeLayoutChanged;
    }
    require(!rejected.compatible && layoutIssue,
        "sequence state-layout change was accepted by hot reload");
}

void testSequenceResultDiagnostics() {
    const auto hasDiagnostic = [](const std::string& source,
                                  const std::string& code) {
        realscript::compiler::Compilation compilation({{
            "phase22-invalid-result.rs", source}});
        const auto build = compilation.build();
        for (const auto& diagnostic : build.diagnostics.items()) {
            if (diagnostic.code == code) return true;
        }
        return false;
    };
    require(hasDiagnostic(R"(
module Phase22.InvalidResult;
class Invalid
{
    sequence<int> Missing(long target)
    {
        yield break;
    }
}
)", "RS8815"),
        "missing result-sequence return did not report RS8815");

    require(hasDiagnostic(R"(
module Phase22.InvalidResult;
class Invalid
{
    sequence VoidResult(long target)
    {
        return 1;
    }
}
)", "RS8813"),
        "void sequence value return did not report RS8813");

    realscript::diagnostics::DiagnosticBag parseDiagnostics;
    realscript::text::SourceText malformed(
        "class Invalid { sequence<int Broken(long target) { } }",
        "phase22-malformed-result.rs");
    realscript::syntax::Parser parser(malformed, parseDiagnostics);
    static_cast<void>(parser.parseCompilationUnit());
    require(parseDiagnostics.hasErrors(),
        "malformed sequence result type recovered without a parser diagnostic");
}

} // namespace

int main() {
    try {
        testPersistedLocalsYieldBreakAndRollback();
        testActiveSequenceHotReloadMigration();
        testSequenceResultDiagnostics();
        std::cout << "Phase 22 coroutine tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}
