#include "realscript_aot_generated.h"

#include "realscript/aot_cpp/AotRuntime.h"
#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/optimization/Optimizer.h"
#include "realscript/runtime/AsyncRuntime.h"
#include "realscript/runtime/Runtime.h"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string readFile(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot read fixture: " + path);
    std::ostringstream content;
    content << stream.rdbuf();
    return content.str();
}

std::vector<realscript::bytecode::Module> compileFixture() {
    const auto path = std::string(REALSCRIPT_SOURCE_DIR) +
        "/tests/fixtures/phase26_aot.rscript";
    realscript::compiler::Compilation compilation({{
        "tests/fixtures/phase26_aot.rscript", readFile(path)}});
    auto build = compilation.build();
    require(!build.diagnostics.hasErrors(),
        "Phase 26 AOT fixture compilation failed");
    realscript::optimization::Options options;
    options.level = realscript::optimization::Level::Aggressive;
    auto optimized = realscript::optimization::Optimizer{}.optimize(
        build.modules, build.diagnostics, options);
    require(!build.diagnostics.hasErrors(),
        "Phase 26 interpreter fixture optimization failed");
    realscript::bytecode::Lowerer lowerer;
    std::vector<realscript::bytecode::Module> modules;
    for (const auto& module : optimized.modules) {
        modules.push_back(lowerer.lower(module));
    }
    return modules;
}

void requireParity(
    realscript::runtime::Interpreter& interpreter,
    realscript::aot::Program& aot,
    const std::string& name,
    std::vector<realscript::runtime::Value> arguments,
    std::int64_t expected) {
    const auto interpreted = interpreter.invoke(name, arguments);
    const auto compiled = aot.invoke(name, std::move(arguments));
    require(interpreted.succeeded,
        name + " failed in interpreter: " + interpreted.error.message);
    require(compiled.succeeded,
        name + " failed in generated AOT: " + compiled.error.message);
    require(std::get<std::int64_t>(interpreted.value) == expected,
        name + " interpreter returned " +
            std::to_string(std::get<std::int64_t>(interpreted.value)) +
            ", expected " + std::to_string(expected));
    require(std::get<std::int64_t>(compiled.value) == expected,
        name + " AOT returned " +
            std::to_string(std::get<std::int64_t>(compiled.value)) +
            ", expected " + std::to_string(expected));
    require(interpreted.instructionsExecuted == compiled.instructionsExecuted,
        name + " instruction accounting diverged between interpreter (" +
            std::to_string(interpreted.instructionsExecuted) + ") and AOT (" +
            std::to_string(compiled.instructionsExecuted) + ")");
}

void testScriptDrivenAsyncParity() {
    auto modules = compileFixture();
    realscript::runtime::Interpreter interpreter(std::move(modules));
    realscript::aot::Program aot(realscript_generated::Phase26AotProgram());

    requireParity(interpreter, aot, "Phase26.Aot::RunSyncFastPath",
        {std::int64_t{15}, std::int64_t{12}}, 42);
    requireParity(interpreter, aot, "Phase26.Aot::RunDeepChainAndPolymorphism",
        {std::int64_t{10}}, 74);
    requireParity(interpreter, aot, "Phase26.Aot::RunCombinatorsAndRace",
        {}, 66);
    requireParity(interpreter, aot, "Phase26.Aot::RunCrossAwaitExceptionHandling",
        {false}, 1043);
    requireParity(interpreter, aot, "Phase26.Aot::RunCrossAwaitExceptionHandling",
        {true}, 973);
    requireParity(interpreter, aot, "Phase26.Aot::RunAsyncFinallyRethrowPropagation",
        {}, 1296);

    require(interpreter.asyncRuntime() != nullptr && aot.asyncRuntime() != nullptr,
        "missing AsyncRuntimeContext on interpreter or AOT program");
    require(interpreter.asyncRuntime()->coroutines().statistics().activeCount == 0,
        "interpreter leaked active coroutine frames");
    require(interpreter.asyncRuntime()->coroutines().statistics().activeGcRoots == 0,
        "interpreter leaked coroutine GC roots");
    require(interpreter.asyncRuntime()->awaitables().statistics().activeCount == 0,
        "interpreter leaked active awaitable handles");
    require(aot.asyncRuntime()->coroutines().statistics().activeCount == 0,
        "AOT leaked active coroutine frames");
    require(aot.asyncRuntime()->coroutines().statistics().activeGcRoots == 0,
        "AOT leaked coroutine GC roots");
    require(aot.asyncRuntime()->awaitables().statistics().activeCount == 0,
        "AOT leaked active awaitable handles");
}

void testHostDrivenBetweenInvokesParity() {
    auto modules = compileFixture();
    realscript::runtime::Interpreter interpreter(std::move(modules));
    realscript::aot::Program aot(realscript_generated::Phase26AotProgram());

    // 1. Drive interpreter coroutine from C++ host across ticks
    const auto interpGate = interpreter.asyncRuntime()->awaitables().allocatePending();
    const auto interpGateStruct = realscript::runtime::makeAwaitableStructValue(
        realscript::semantic::stableTypeId("RealScript.Async::Awaitable__int"), interpGate);
    const auto interpBoxOpt = interpreter.heap()->allocateTypedArray(
        realscript::semantic::stableTypeId("int[]"),
        realscript::semantic::PrimitiveType::Int,
        0,
        1,
        realscript::runtime::Value(std::int64_t{0}));
    require(interpBoxOpt.has_value(), "failed to allocate interpreter box array");
    const auto interpRootPin = interpreter.heap()->retain(
        realscript::runtime::Value(*interpBoxOpt));

    const std::vector<realscript::runtime::Value> interpArgs = {
        interpGateStruct, realscript::runtime::Value(*interpBoxOpt)};
    const auto rInterp = interpreter.invoke("Phase26.Aot::HostDrivenWorker", interpArgs);
    require(rInterp.succeeded, "HostDrivenWorker launch failed in interpreter");
    realscript::runtime::AwaitableHandle interpWorkerHandle{};
    require(realscript::runtime::tryExtractAwaitableHandle(rInterp.value, interpWorkerHandle),
        "HostDrivenWorker did not return Awaitable handle in interpreter");
    require(!interpreter.asyncRuntime()->awaitables().isCompleted(interpWorkerHandle),
        "HostDrivenWorker completed prematurely in interpreter");

    // Force full GC while suspended between C++ invoke() calls!
    {
        realscript::runtime::ShadowStack emptyStack;
        interpreter.heap()->collectFull(emptyStack);
    }

    require(interpreter.asyncRuntime()->awaitables().trySetResult(
        interpGate, realscript::runtime::Value(std::int64_t{9})),
        "failed to resolve interpGate");
    interpreter.asyncRuntime()->stepResumeQueue(realscript::runtime::ResumeQueue::Update);
    require(!interpreter.asyncRuntime()->awaitables().isCompleted(interpWorkerHandle),
        "HostDrivenWorker should still be suspended on Yield() after tick 1");
    interpreter.asyncRuntime()->stepResumeQueue(realscript::runtime::ResumeQueue::Update);
    require(interpreter.asyncRuntime()->awaitables().isCompleted(interpWorkerHandle),
        "HostDrivenWorker did not complete on tick 2 in interpreter");

    // 2. Drive C++17 AOT coroutine from C++ host across identical ticks
    const auto aotGate = aot.asyncRuntime()->awaitables().allocatePending();
    const auto aotGateStruct = realscript::runtime::makeAwaitableStructValue(
        realscript::semantic::stableTypeId("RealScript.Async::Awaitable__int"), aotGate);
    const auto aotBoxOpt = aot.heap()->allocateTypedArray(
        realscript::semantic::stableTypeId("int[]"),
        realscript::semantic::PrimitiveType::Int,
        0,
        1,
        realscript::runtime::Value(std::int64_t{0}));
    require(aotBoxOpt.has_value(), "failed to allocate AOT box array");
    const auto aotRootPin = aot.heap()->retain(realscript::runtime::Value(*aotBoxOpt));

    const std::vector<realscript::runtime::Value> aotArgs = {
        aotGateStruct, realscript::runtime::Value(*aotBoxOpt)};
    const auto rAot = aot.invoke("Phase26.Aot::HostDrivenWorker", aotArgs);
    require(rAot.succeeded, "HostDrivenWorker launch failed in AOT: " + rAot.error.message);
    realscript::runtime::AwaitableHandle aotWorkerHandle{};
    require(realscript::runtime::tryExtractAwaitableHandle(rAot.value, aotWorkerHandle),
        "HostDrivenWorker did not return Awaitable handle in AOT");

    {
        realscript::runtime::ShadowStack emptyStack;
        aot.heap()->collectFull(emptyStack);
    }

    require(aot.asyncRuntime()->awaitables().trySetResult(
        aotGate, realscript::runtime::Value(std::int64_t{9})),
        "failed to resolve aotGate");
    aot.asyncRuntime()->stepResumeQueue(realscript::runtime::ResumeQueue::Update);
    require(!aot.asyncRuntime()->awaitables().isCompleted(aotWorkerHandle),
        "AOT HostDrivenWorker should still be suspended on Yield() after tick 1");
    aot.asyncRuntime()->stepResumeQueue(realscript::runtime::ResumeQueue::Update);
    require(aot.asyncRuntime()->awaitables().isCompleted(aotWorkerHandle),
        "AOT HostDrivenWorker did not complete on tick 2");

    const auto interpVal = interpreter.heap()->arrayGet(*interpBoxOpt, 0);
    const auto aotVal = aot.heap()->arrayGet(*aotBoxOpt, 0);
    require(interpVal && aotVal &&
            std::get<std::int64_t>(*interpVal) == 93 &&
            std::get<std::int64_t>(*aotVal) == 93,
        "host-driven resumption result mismatch between interpreter and AOT");
    require(interpreter.asyncRuntime()->awaitables().discard(interpWorkerHandle),
        "failed to discard interpWorkerHandle");
    require(aot.asyncRuntime()->awaitables().discard(aotWorkerHandle),
        "failed to discard aotWorkerHandle");
    require(interpreter.asyncRuntime()->awaitables().statistics().activeCount == 0 &&
            aot.asyncRuntime()->awaitables().statistics().activeCount == 0,
        "host-driven parity test leaked active awaitable handles");
}


void testAotVsInterpreterAsyncBenchmarks() {
    auto modules = compileFixture();
    realscript::runtime::Interpreter interpreter(std::move(modules));
    realscript::aot::Program aot(realscript_generated::Phase26AotProgram());

    realscript::runtime::ExecutionOptions opts{};
    opts.limits.instructionBudget = 500'000'000ULL;

    constexpr std::int64_t kWarmup = 16;
    constexpr std::int64_t kIters = 50000;
    const std::vector<realscript::runtime::Value> warmArgs = {kWarmup};
    const std::vector<realscript::runtime::Value> benchArgs = {kIters};

    for (const char* fn : {
             "Phase26.Aot::BenchSyncFastPath",
             "Phase26.Aot::BenchPooledSuspend",
             "Phase26.Aot::BenchProtectedTryFinallySuspend"}) {
        const auto wi = interpreter.invoke(fn, warmArgs, opts);
        const auto wa = aot.invoke(fn, warmArgs, opts);
        require(wi.succeeded && wa.succeeded &&
                wi.instructionsExecuted == wa.instructionsExecuted,
            std::string("warmup parity failed for ") + fn);

        const auto t0 = std::chrono::steady_clock::now();
        const auto ri = interpreter.invoke(fn, benchArgs, opts);
        const auto t1 = std::chrono::steady_clock::now();
        const auto ra = aot.invoke(fn, benchArgs, opts);
        const auto t2 = std::chrono::steady_clock::now();

        require(ri.succeeded && ra.succeeded &&
                std::get<std::int64_t>(ri.value) == kIters &&
                std::get<std::int64_t>(ra.value) == kIters &&
                ri.instructionsExecuted == ra.instructionsExecuted,
            std::string("50k benchmark parity failed for ") + fn);

        const double vmNs = std::chrono::duration<double, std::nano>(t1 - t0).count() /
            static_cast<double>(kIters);
        const double aotNs = std::chrono::duration<double, std::nano>(t2 - t1).count() /
            static_cast<double>(kIters);
        std::cout << std::fixed << std::setprecision(1)
                  << "[aot-bench] " << fn << " | FlatVM=" << vmNs
                  << " ns/op | C++17 AOT=" << aotNs << " ns/op\n";
    }
}

} // namespace

int main() {
    try {
        testScriptDrivenAsyncParity();
        testHostDrivenBetweenInvokesParity();
        testAotVsInterpreterAsyncBenchmarks();
        std::cout << "All Phase 26 AOT async/await & exception parity tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Phase 26 AOT test failure: " << ex.what() << "\n";
        return 1;
    }
}
