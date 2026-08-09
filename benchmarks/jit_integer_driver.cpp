#include "realscript/compiler/Compilation.h"
#include "realscript/jit/Jit.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Samples {
    double medianNanoseconds = 0.0;
    double p95Nanoseconds = 0.0;
};

std::string readFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot read benchmark source");
    std::ostringstream content;
    content << stream.rdbuf();
    return content.str();
}

template <typename Function>
Samples measure(
    std::uint64_t sampleCount,
    std::uint64_t iterations,
    Function&& function) {
    std::vector<double> values;
    values.reserve(static_cast<std::size_t>(sampleCount));
    for (std::uint64_t sample = 0; sample < sampleCount; ++sample) {
        const auto started = std::chrono::steady_clock::now();
        for (std::uint64_t index = 0; index < iterations; ++index) function();
        const auto stopped = std::chrono::steady_clock::now();
        values.push_back(
            std::chrono::duration<double>(stopped - started).count() *
            1'000'000'000.0 / static_cast<double>(iterations));
    }
    std::sort(values.begin(), values.end());
    return Samples{
        values[(values.size() - 1) / 2],
        values[static_cast<std::size_t>(
            0.95 * static_cast<double>(values.size() - 1))],
    };
}

void printSamples(const char* name, const Samples& samples) {
    std::cout << "\"" << name << "\":{"
        << "\"medianNanoseconds\":" << samples.medianNanoseconds
        << ",\"p95Nanoseconds\":" << samples.p95Nanoseconds << "}";
}

} // namespace

int main() {
    try {
        const auto sourcePath =
            std::filesystem::path(REALSCRIPT_BENCH_SOURCE_DIR) /
            "benchmarks/micro/integer_loop.rs";
        realscript::compiler::Compilation compilation({{
            "benchmarks/micro/integer_loop.rs",
            readFile(sourcePath),
        }});
        auto build = compilation.build();
        if (build.diagnostics.hasErrors()) {
            throw std::runtime_error("JIT benchmark source compilation failed");
        }

        realscript::jit::ToolchainOptions options;
        options.compiler = REALSCRIPT_BENCH_JIT_COMPILER;
        options.includeDirectory = REALSCRIPT_BENCH_JIT_INCLUDE_DIR;
        options.supportLibrary = REALSCRIPT_BENCH_JIT_SUPPORT_LIBRARY;
        const auto runId = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        options.outputDirectory =
            std::filesystem::path(REALSCRIPT_BENCH_JIT_CACHE_BASE) / runId;
        options.generation.programName = "IntegerLoopJit";
        options.optimization.level =
            realscript::optimization::Level::Aggressive;
#if defined(_WIN32)
        options.compilerArguments = {"/MD", "/O2"};
#else
        options.compilerArguments = {"-O2"};
#endif
        if (!realscript::jit::ToolchainJit::available(options)) {
            throw std::runtime_error("configured JIT toolchain is unavailable");
        }

        realscript::jit::ToolchainJit jit;
        const auto coldStarted = std::chrono::steady_clock::now();
        auto cold = jit.compile(build.modules, options);
        const auto coldStopped = std::chrono::steady_clock::now();
        if (!cold.succeeded()) {
            throw std::runtime_error(
                cold.error.empty() ? "cold JIT compile failed" : cold.error);
        }
        const auto cachedStarted = std::chrono::steady_clock::now();
        auto cached = jit.compile(build.modules, options);
        const auto cachedStopped = std::chrono::steady_clock::now();
        if (!cached.succeeded()) {
            throw std::runtime_error(
                cached.error.empty() ? "cached JIT compile failed" : cached.error);
        }

        realscript::runtime::ExecutionOptions rawOptions;
        rawOptions.determinism.mode =
            realscript::runtime::DeterminismMode::Off;
        rawOptions.limits.gcWorkBudget = 0;
        realscript::runtime::ExecutionOptions deterministicOptions;
        deterministicOptions.determinism.mode =
            realscript::runtime::DeterminismMode::Strict;
        deterministicOptions.limits.gcWorkBudget = 0;
        volatile std::int64_t resultSink = 0;
        const auto invoke = [&](const auto& execution) {
            const auto result = cached.module->invoke(
                "Bench.IntegerLoop::main", {}, execution);
            if (!result.succeeded) {
                throw std::runtime_error(
                    "JIT invocation failed: " + result.error.message);
            }
            resultSink = std::get<std::int64_t>(result.value);
        };
        invoke(rawOptions);
        if (resultSink != 49'995'000) {
            throw std::runtime_error("JIT integer benchmark result diverged");
        }
        const auto jitRaw = measure(11, 5, [&] { invoke(rawOptions); });
        const auto jitDeterministic = measure(
            11, 5, [&] { invoke(deterministicOptions); });

        const auto coldMilliseconds =
            std::chrono::duration<double, std::milli>(
                coldStopped - coldStarted).count();
        const auto cachedMilliseconds =
            std::chrono::duration<double, std::milli>(
                cachedStopped - cachedStarted).count();
        std::cout << std::setprecision(12) << "{"
            << "\"coldCompileMilliseconds\":" << coldMilliseconds
            << ",\"cachedCompileMilliseconds\":" << cachedMilliseconds
            << ",\"coldCacheHit\":"
            << (cold.cacheHit ? "true" : "false")
            << ",\"cachedCacheHit\":"
            << (cached.cacheHit ? "true" : "false") << ",";
        printSamples("jitRaw", jitRaw);
        std::cout << ",";
        printSamples("jitDeterministic", jitDeterministic);
        std::cout << ",\"result\":" << resultSink << "}\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "rsbench_jit: " << exception.what() << '\n';
        return 1;
    }
}
