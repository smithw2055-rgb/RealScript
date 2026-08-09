#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/optimization/Optimizer.h"
#include "realscript/runtime/Runtime.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

enum class BenchmarkMode {
    Raw,
    Deterministic,
    Profiled,
};

enum class HostPath {
    Interpreter,
    EngineRuntime,
};

std::string readFile(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot open input file: " + path);
    std::ostringstream content;
    content << stream.rdbuf();
    return content.str();
}

std::uint64_t parseCount(
    const std::string& value,
    const char* option,
    bool allowZero = false) {
    std::size_t consumed = 0;
    if (value.empty() || value.front() == '-') {
        throw std::runtime_error(
            std::string(option) +
            (allowZero ? " must be a non-negative integer" :
                         " must be a positive integer"));
    }
    const auto result = std::stoull(value, &consumed);
    if (consumed != value.size() || (!allowZero && result == 0)) {
        throw std::runtime_error(
            std::string(option) +
            (allowZero ? " must be a non-negative integer" :
                         " must be a positive integer"));
    }
    return result;
}

std::string escapeJson(std::string_view value) {
    std::ostringstream escaped;
    escaped << std::hex << std::setfill('0');
    for (const unsigned char character : value) {
        switch (character) {
        case '"': escaped << "\\\""; break;
        case '\\': escaped << "\\\\"; break;
        case '\b': escaped << "\\b"; break;
        case '\f': escaped << "\\f"; break;
        case '\n': escaped << "\\n"; break;
        case '\r': escaped << "\\r"; break;
        case '\t': escaped << "\\t"; break;
        default:
            if (character < 0x20U) {
                escaped << "\\u" << std::setw(4)
                    << static_cast<unsigned int>(character);
            } else {
                escaped << static_cast<char>(character);
            }
            break;
        }
    }
    return escaped.str();
}

realscript::optimization::Level parseLevel(const std::string& value) {
    if (value == "0") return realscript::optimization::Level::None;
    if (value == "1") return realscript::optimization::Level::Basic;
    if (value == "2") return realscript::optimization::Level::Aggressive;
    throw std::runtime_error("--opt-level must be 0, 1, or 2");
}

BenchmarkMode parseMode(const std::string& value) {
    if (value == "raw") return BenchmarkMode::Raw;
    if (value == "deterministic") return BenchmarkMode::Deterministic;
    if (value == "profiled") return BenchmarkMode::Profiled;
    throw std::runtime_error(
        "--mode must be raw, deterministic, or profiled");
}

const char* modeName(BenchmarkMode mode) noexcept {
    switch (mode) {
    case BenchmarkMode::Raw: return "raw";
    case BenchmarkMode::Deterministic: return "deterministic";
    case BenchmarkMode::Profiled: return "profiled";
    }
    return "raw";
}

HostPath parseHostPath(const std::string& value) {
    if (value == "interpreter") return HostPath::Interpreter;
    if (value == "engine") return HostPath::EngineRuntime;
    throw std::runtime_error(
        "--host-path must be interpreter or engine");
}

const char* backendName(HostPath path) noexcept {
    return path == HostPath::Interpreter
        ? "interpreter"
        : "engine-runtime";
}

double percentile(const std::vector<double>& sorted, double fraction) {
    if (sorted.empty()) return 0.0;
    const auto rank = static_cast<std::size_t>(
        fraction * static_cast<double>(sorted.size() - 1));
    return sorted[rank];
}

void usage() {
    std::cerr
        << "usage: rsbench [options] <file.rs>...\n"
        << "  --entry <module::function>\n"
        << "  --iterations <count>\n"
        << "  --warmup <count>\n"
        << "  --samples <count>\n"
        << "  --mode <raw|deterministic|profiled>\n"
        << "  --host-path <interpreter|engine>\n"
        << "  --gc-work <count>\n"
        << "  --opt-level <0|1|2>\n"
        << "  --json\n";
}

std::string inferEntry(const std::vector<realscript::mir::Module>& modules) {
    std::string candidate;
    for (const auto& module : modules) {
        for (const auto& function : module.functions) {
            if (function.name != "main" || !function.parameterTypes.empty()) continue;
            const auto current = module.name + "::" + function.name;
            if (!candidate.empty()) {
                throw std::runtime_error(
                    "multiple parameterless main functions; use --entry");
            }
            candidate = current;
        }
    }
    if (candidate.empty()) {
        throw std::runtime_error(
            "cannot infer a parameterless main function; use --entry");
    }
    return candidate;
}

realscript::semantic::SymbolId resolveEntry(
    const std::vector<realscript::mir::Module>& modules,
    const std::string& qualifiedName) {
    for (const auto& module : modules) {
        for (const auto& function : module.functions) {
            if (module.name + "::" + function.name == qualifiedName) {
                return function.symbolId;
            }
        }
    }
    throw std::runtime_error("entry function was not found: " + qualifiedName);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }

    try {
        std::string entry;
        std::uint64_t iterations = 100;
        std::uint64_t warmup = 10;
        std::uint64_t samples = 1;
        std::size_t gcWorkBudget = 8;
        bool json = false;
        BenchmarkMode mode = BenchmarkMode::Deterministic;
        HostPath hostPath = HostPath::Interpreter;
        realscript::optimization::Options optimizerOptions;
        optimizerOptions.level = realscript::optimization::Level::Aggressive;
        std::vector<std::string> paths;

        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];
            const auto requireValue = [&](const char* option) -> std::string {
                if (index + 1 >= argc) {
                    throw std::runtime_error(std::string(option) + " requires a value");
                }
                return argv[++index];
            };
            if (argument == "--entry") entry = requireValue("--entry");
            else if (argument == "--iterations") {
                iterations = parseCount(requireValue("--iterations"), "--iterations");
            } else if (argument == "--warmup") {
                warmup = parseCount(
                    requireValue("--warmup"), "--warmup", true);
            } else if (argument == "--samples") {
                samples = parseCount(requireValue("--samples"), "--samples");
            } else if (argument == "--mode") {
                mode = parseMode(requireValue("--mode"));
            } else if (argument == "--host-path") {
                hostPath = parseHostPath(requireValue("--host-path"));
            } else if (argument == "--gc-work") {
                gcWorkBudget = static_cast<std::size_t>(parseCount(
                    requireValue("--gc-work"), "--gc-work", true));
            } else if (argument == "--opt-level") {
                optimizerOptions.level = parseLevel(requireValue("--opt-level"));
            } else if (argument == "--json") json = true;
            else if (argument == "--help") {
                usage();
                return 0;
            } else if (argument.rfind("--", 0) == 0) {
                throw std::runtime_error("unknown option: " + argument);
            } else {
                paths.push_back(argument);
            }
        }
        if (paths.empty()) {
            usage();
            return 2;
        }

        realscript::compiler::Compilation compilation;
        for (const auto& path : paths) compilation.addSource({path, readFile(path)});
        const auto frontendStarted = std::chrono::steady_clock::now();
        auto build = compilation.build();
        const auto frontendStopped = std::chrono::steady_clock::now();
        if (build.diagnostics.hasErrors()) {
            for (const auto& diagnostic : build.diagnostics.items()) {
                std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
            }
            return 1;
        }

        realscript::optimization::Optimizer optimizer;
        const auto optimizationStarted = std::chrono::steady_clock::now();
        auto optimized = optimizer.optimize(
            std::move(build.modules), build.diagnostics, optimizerOptions);
        const auto optimizationStopped = std::chrono::steady_clock::now();
        if (build.diagnostics.hasErrors()) {
            for (const auto& diagnostic : build.diagnostics.items()) {
                std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
            }
            return 1;
        }
        if (entry.empty()) entry = inferEntry(optimized.modules);
        const auto entrySymbol = resolveEntry(optimized.modules, entry);

        realscript::bytecode::Lowerer lowerer;
        std::vector<realscript::bytecode::Module> bytecode;
        const auto bytecodeStarted = std::chrono::steady_clock::now();
        for (const auto& module : optimized.modules) {
            auto lowered = lowerer.lower(module);
            (void)realscript::bytecode::verifyModule(lowered, build.diagnostics);
            bytecode.push_back(std::move(lowered));
        }
        if (build.diagnostics.hasErrors()) {
            throw std::runtime_error("optimized bytecode verification failed");
        }
        const auto bytecodeStopped = std::chrono::steady_clock::now();
        const auto frontendMilliseconds =
            std::chrono::duration<double, std::milli>(
                frontendStopped - frontendStarted).count();
        const auto optimizationMilliseconds =
            std::chrono::duration<double, std::milli>(
                optimizationStopped - optimizationStarted).count();
        const auto bytecodeMilliseconds =
            std::chrono::duration<double, std::milli>(
                bytecodeStopped - bytecodeStarted).count();

        std::unique_ptr<realscript::runtime::Interpreter> interpreter;
        std::unique_ptr<realscript::runtime::EngineRuntime> engineRuntime;
        if (hostPath == HostPath::Interpreter) {
            interpreter = std::make_unique<realscript::runtime::Interpreter>(
                std::move(bytecode));
        } else {
            realscript::runtime::RuntimeError linkError;
            auto linked = realscript::runtime::ProgramImage::link(
                std::move(bytecode), linkError);
            if (!linked) {
                throw std::runtime_error(
                    "program link failed: " + linkError.message);
            }
            auto program = std::make_shared<realscript::runtime::ProgramImage>(
                std::move(*linked));
            engineRuntime = std::make_unique<realscript::runtime::EngineRuntime>(
                std::move(program));
        }
        realscript::runtime::ExecutionOptions execution;
        execution.limits.gcWorkBudget = gcWorkBudget;
        execution.determinism.mode = mode == BenchmarkMode::Raw
            ? realscript::runtime::DeterminismMode::Off
            : realscript::runtime::DeterminismMode::Strict;
        const auto invoke = [&](realscript::runtime::ExecutionOptions options) {
            return interpreter
                ? interpreter->invoke(entrySymbol, {}, std::move(options))
                : engineRuntime->invoke(entrySymbol, {}, std::move(options));
        };
        for (std::uint64_t index = 0; index < warmup; ++index) {
            const auto result = invoke(execution);
            if (!result.succeeded) {
                throw std::runtime_error("warmup failed: " + result.error.message);
            }
        }

        auto profile = std::make_shared<realscript::runtime::ProfileCollector>();
        if (mode == BenchmarkMode::Profiled) execution.profile = profile;
        std::uint64_t digest = 0;
        realscript::runtime::ExecutionResult last;
        double elapsed = 0.0;
        std::vector<double> sampleNanoseconds;
        sampleNanoseconds.reserve(static_cast<std::size_t>(samples));
        for (std::uint64_t sample = 0; sample < samples; ++sample) {
            const auto started = std::chrono::steady_clock::now();
            for (std::uint64_t index = 0; index < iterations; ++index) {
                last = invoke(execution);
                if (!last.succeeded) {
                    throw std::runtime_error(
                        "benchmark failed: " + last.error.message);
                }
                digest ^= last.determinismDigest + 0x9e3779b97f4a7c15ULL +
                    (digest << 6U) + (digest >> 2U);
            }
            const auto stopped = std::chrono::steady_clock::now();
            const auto sampleElapsed =
                std::chrono::duration<double>(stopped - started).count();
            elapsed += sampleElapsed;
            sampleNanoseconds.push_back(
                sampleElapsed * 1'000'000'000.0 /
                static_cast<double>(iterations));
        }
        std::sort(sampleNanoseconds.begin(), sampleNanoseconds.end());
        const auto minimumNanoseconds = sampleNanoseconds.front();
        const auto medianNanoseconds = percentile(sampleNanoseconds, 0.50);
        const auto p95Nanoseconds = percentile(sampleNanoseconds, 0.95);
        const auto p99Nanoseconds = percentile(sampleNanoseconds, 0.99);
        const auto maximumNanoseconds = sampleNanoseconds.back();
        const auto nanosecondsPerInstruction = last.instructionsExecuted == 0
            ? 0.0
            : medianNanoseconds /
                static_cast<double>(last.instructionsExecuted);
        const auto snapshot = profile->snapshot();
        const auto heap = interpreter
            ? interpreter->heap()
            : engineRuntime->heap();
        const auto& gc = heap->statistics();

        if (json) {
            std::cout << "{\"backend\":\"" << backendName(hostPath)
                << "\",\"entry\":\""
                << escapeJson(entry) << "\",\"optimization\":\""
                << realscript::optimization::levelName(optimizerOptions.level)
                << "\",\"mode\":\"" << modeName(mode)
                << "\",\"iterations\":" << iterations
                << ",\"warmup\":" << warmup
                << ",\"samples\":" << samples
                << ",\"gcWorkBudget\":" << gcWorkBudget
                << ",\"compilation\":{\"frontendMilliseconds\":"
                << frontendMilliseconds
                << ",\"optimizationMilliseconds\":"
                << optimizationMilliseconds
                << ",\"bytecodeMilliseconds\":"
                << bytecodeMilliseconds << "}"
                << ",\"seconds\":" << std::setprecision(12) << elapsed
                << ",\"nanosecondsPerInvocation\":" << medianNanoseconds
                << ",\"medianNanoseconds\":" << medianNanoseconds
                << ",\"p95Nanoseconds\":" << p95Nanoseconds
                << ",\"p99Nanoseconds\":" << p99Nanoseconds
                << ",\"minNanoseconds\":" << minimumNanoseconds
                << ",\"maxNanoseconds\":" << maximumNanoseconds
                << ",\"instructionsPerInvocation\":"
                << last.instructionsExecuted
                << ",\"nanosecondsPerInstruction\":"
                << nanosecondsPerInstruction
                << ",\"digest\":" << digest
                << ",\"result\":\""
                << escapeJson(realscript::runtime::valueToString(
                    last.value, heap.get()))
                << "\",\"gc\":{\"objectsAllocated\":"
                << gc.objectsAllocated
                << ",\"bytesAllocated\":" << gc.bytesAllocated
                << ",\"collectionsCompleted\":" << gc.collectionsCompleted
                << ",\"liveBytes\":" << gc.liveBytes
                << ",\"peakLiveBytes\":" << gc.peakLiveBytes << "}"
                << ",\"profile\":"
                << realscript::runtime::profileToJson(snapshot) << "}\n";
        } else {
            std::cout << "backend=" << backendName(hostPath)
                << " entry=" << entry
                << " optimization="
                << realscript::optimization::levelName(optimizerOptions.level)
                << " mode=" << modeName(mode)
                << " iterations=" << iterations
                << " warmup=" << warmup
                << " samples=" << samples << '\n'
                << "gc-work-budget=" << gcWorkBudget << '\n'
                << "frontend-ms=" << frontendMilliseconds
                << " optimization-ms=" << optimizationMilliseconds
                << " bytecode-ms=" << bytecodeMilliseconds << '\n'
                << "elapsed-seconds=" << std::setprecision(9) << elapsed
                << " ns/invocation=" << std::fixed << std::setprecision(2)
                << medianNanoseconds
                << " p95=" << p95Nanoseconds
                << " p99=" << p99Nanoseconds
                << " ns/instruction=" << nanosecondsPerInstruction
                << " digest=" << digest << '\n'
                << "result=" << realscript::runtime::valueToString(
                    last.value, heap.get()) << '\n'
                << realscript::optimization::formatStatistics(
                    optimized.statistics) << '\n'
                << realscript::runtime::formatProfile(snapshot);
        }
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "rsbench: " << exception.what() << '\n';
        return 2;
    }
}
