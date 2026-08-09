#include "realscript_aot_generated.h"

#include "realscript/aot_cpp/AotRuntime.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Samples {
    double medianNanoseconds = 0.0;
    double p95Nanoseconds = 0.0;
    double minimumNanoseconds = 0.0;
    double maximumNanoseconds = 0.0;
};

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
        const auto elapsed =
            std::chrono::duration<double>(stopped - started).count();
        values.push_back(
            elapsed * 1'000'000'000.0 / static_cast<double>(iterations));
    }
    std::sort(values.begin(), values.end());
    return Samples{
        values[(values.size() - 1) / 2],
        values[static_cast<std::size_t>(
            0.95 * static_cast<double>(values.size() - 1))],
        values.front(),
        values.back(),
    };
}

#if defined(_MSC_VER)
#define REALSCRIPT_NOINLINE __declspec(noinline)
#else
#define REALSCRIPT_NOINLINE __attribute__((noinline))
#endif

REALSCRIPT_NOINLINE std::int64_t nativeIntegerLoop(std::int64_t limit) {
    std::int64_t total = 0;
    for (std::int64_t index = 0; index < limit; ++index) total += index;
    return total;
}

void printSamples(const char* name, const Samples& samples) {
    std::cout << "\"" << name << "\":{"
        << "\"medianNanoseconds\":" << samples.medianNanoseconds
        << ",\"p95Nanoseconds\":" << samples.p95Nanoseconds
        << ",\"minNanoseconds\":" << samples.minimumNanoseconds
        << ",\"maxNanoseconds\":" << samples.maximumNanoseconds
        << "}";
}

} // namespace

int main() {
    try {
        constexpr std::uint64_t sampleCount = 11;
        constexpr std::uint64_t iterations = 5;
        constexpr std::int64_t expected = 49'995'000;

        realscript::aot::Program program(
            realscript_generated::IntegerLoopProgram());
        const auto& descriptor = program.descriptor();
        realscript::semantic::SymbolId entry = 0;
        for (std::uint32_t index = 0; index < descriptor.functionCount; ++index) {
            const auto& function = descriptor.functions[index];
            const std::string name = function.name ? function.name : "";
            if (name == "Bench.IntegerLoop::main") {
                entry = function.symbolId;
                break;
            }
        }
        if (entry == 0) throw std::runtime_error("AOT main entry was not found");

        realscript::runtime::ExecutionOptions rawOptions;
        rawOptions.determinism.mode =
            realscript::runtime::DeterminismMode::Off;
        rawOptions.limits.gcWorkBudget = 0;
        realscript::runtime::ExecutionOptions deterministicOptions;
        deterministicOptions.determinism.mode =
            realscript::runtime::DeterminismMode::Strict;
        deterministicOptions.limits.gcWorkBudget = 0;

        volatile std::int64_t resultSink = 0;
        const auto invokeAot = [&](const auto& options) {
            const auto result = program.invoke(entry, {}, options);
            if (!result.succeeded) {
                throw std::runtime_error(
                    "AOT invocation failed: " + result.error.message);
            }
            resultSink = std::get<std::int64_t>(result.value);
        };
        invokeAot(rawOptions);
        if (resultSink != expected) {
            throw std::runtime_error("AOT integer benchmark result diverged");
        }

        const auto aotRaw = measure(sampleCount, iterations, [&] {
            invokeAot(rawOptions);
        });
        const auto aotDeterministic = measure(sampleCount, iterations, [&] {
            invokeAot(deterministicOptions);
        });
        const auto native = measure(sampleCount, iterations, [&] {
            resultSink = nativeIntegerLoop(10'000);
        });
        if (resultSink != expected) {
            throw std::runtime_error("native integer benchmark result diverged");
        }

        std::cout << std::setprecision(12) << "{";
        printSamples("aotRaw", aotRaw);
        std::cout << ",";
        printSamples("aotDeterministic", aotDeterministic);
        std::cout << ",";
        printSamples("native", native);
        std::cout << ",\"samples\":" << sampleCount
            << ",\"iterations\":" << iterations
            << ",\"result\":" << resultSink << "}\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "rsbench_aot: " << exception.what() << '\n';
        return 1;
    }
}
