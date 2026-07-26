#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/optimization/Optimizer.h"
#include "realscript/runtime/Runtime.h"

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

void usage() {
    std::cerr
        << "usage: rsbench [options] <file.rs>...\n"
        << "  --entry <module::function>\n"
        << "  --iterations <count>\n"
        << "  --warmup <count>\n"
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
        bool json = false;
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
        auto build = compilation.build();
        if (build.diagnostics.hasErrors()) {
            for (const auto& diagnostic : build.diagnostics.items()) {
                std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
            }
            return 1;
        }

        realscript::optimization::Optimizer optimizer;
        auto optimized = optimizer.optimize(
            std::move(build.modules), build.diagnostics, optimizerOptions);
        if (build.diagnostics.hasErrors()) {
            for (const auto& diagnostic : build.diagnostics.items()) {
                std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
            }
            return 1;
        }
        if (entry.empty()) entry = inferEntry(optimized.modules);

        realscript::bytecode::Lowerer lowerer;
        std::vector<realscript::bytecode::Module> bytecode;
        for (const auto& module : optimized.modules) {
            auto lowered = lowerer.lower(module);
            (void)realscript::bytecode::verifyModule(lowered, build.diagnostics);
            bytecode.push_back(std::move(lowered));
        }
        if (build.diagnostics.hasErrors()) {
            throw std::runtime_error("optimized bytecode verification failed");
        }

        realscript::runtime::Interpreter interpreter(std::move(bytecode));
        realscript::runtime::ExecutionOptions execution;
        execution.determinism.mode = realscript::runtime::DeterminismMode::Strict;
        for (std::uint64_t index = 0; index < warmup; ++index) {
            const auto result = interpreter.invoke(entry, {}, execution);
            if (!result.succeeded) {
                throw std::runtime_error("warmup failed: " + result.error.message);
            }
        }

        auto profile = std::make_shared<realscript::runtime::ProfileCollector>();
        execution.profile = profile;
        std::uint64_t digest = 0;
        realscript::runtime::ExecutionResult last;
        const auto started = std::chrono::steady_clock::now();
        for (std::uint64_t index = 0; index < iterations; ++index) {
            last = interpreter.invoke(entry, {}, execution);
            if (!last.succeeded) {
                throw std::runtime_error("benchmark failed: " + last.error.message);
            }
            digest ^= last.determinismDigest + 0x9e3779b97f4a7c15ULL +
                (digest << 6U) + (digest >> 2U);
        }
        const auto stopped = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration<double>(stopped - started).count();
        const auto nanoseconds = elapsed * 1'000'000'000.0 /
            static_cast<double>(iterations);
        const auto snapshot = profile->snapshot();

        if (json) {
            std::cout << "{\"backend\":\"interpreter\",\"entry\":\""
                << escapeJson(entry) << "\",\"optimization\":\""
                << realscript::optimization::levelName(optimizerOptions.level)
                << "\",\"iterations\":" << iterations
                << ",\"warmup\":" << warmup
                << ",\"seconds\":" << std::setprecision(12) << elapsed
                << ",\"nanosecondsPerInvocation\":" << nanoseconds
                << ",\"digest\":" << digest
                << ",\"result\":\""
                << escapeJson(realscript::runtime::valueToString(
                    last.value, interpreter.heap().get()))
                << "\",\"profile\":"
                << realscript::runtime::profileToJson(snapshot) << "}\n";
        } else {
            std::cout << "backend=interpreter entry=" << entry
                << " optimization="
                << realscript::optimization::levelName(optimizerOptions.level)
                << " iterations=" << iterations
                << " warmup=" << warmup << '\n'
                << "elapsed-seconds=" << std::setprecision(9) << elapsed
                << " ns/invocation=" << std::fixed << std::setprecision(2)
                << nanoseconds << " digest=" << digest << '\n'
                << "result=" << realscript::runtime::valueToString(
                    last.value, interpreter.heap().get()) << '\n'
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
