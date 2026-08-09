#include "realscript_aot_generated.h"

#include "realscript/aot_cpp/AotCpp.h"
#include "realscript/aot_cpp/AotRuntime.h"
#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/optimization/Optimizer.h"
#include "realscript/runtime/Runtime.h"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
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

std::vector<realscript::mir::Module> compileFixture() {
    realscript::compiler::Compilation compilation;
    compilation.addSource({
        "tests/fixtures/phase5_model.rs",
        readFile(std::string(REALSCRIPT_SOURCE_DIR) +
            "/tests/fixtures/phase5_model.rs"),
    });
    compilation.addSource({
        "tests/fixtures/phase5_app.rs",
        readFile(std::string(REALSCRIPT_SOURCE_DIR) +
            "/tests/fixtures/phase5_app.rs"),
    });
    auto result = compilation.build();
    require(!result.diagnostics.hasErrors(), "Phase 6 AOT fixture failed to compile");
    return result.modules;
}

std::vector<realscript::bytecode::Module> lower(
    const std::vector<realscript::mir::Module>& modules) {
    realscript::bytecode::Lowerer lowerer;
    std::vector<realscript::bytecode::Module> result;
    for (const auto& module : modules) result.push_back(lowerer.lower(module));
    return result;
}

void testCrossBackendDeterminismAndProfiling() {
    const auto modules = compileFixture();
    realscript::runtime::Interpreter interpreter(lower(modules));
    realscript::aot::Program aot(realscript_generated::Phase5FixtureProgram());

    auto interpreterProfile = std::make_shared<realscript::runtime::ProfileCollector>();
    auto aotProfile = std::make_shared<realscript::runtime::ProfileCollector>();
    std::vector<realscript::runtime::TraceEvent> interpreterTrace;
    std::vector<realscript::runtime::TraceEvent> aotTrace;
    realscript::runtime::ExecutionOptions interpreterOptions;
    interpreterOptions.trace = [&](const auto& event) { interpreterTrace.push_back(event); };
    interpreterOptions.profile = interpreterProfile;
    interpreterOptions.determinism.mode =
        realscript::runtime::DeterminismMode::Strict;
    realscript::runtime::ExecutionOptions aotOptions;
    aotOptions.trace = [&](const auto& event) { aotTrace.push_back(event); };
    aotOptions.profile = aotProfile;
    aotOptions.determinism.mode = realscript::runtime::DeterminismMode::Strict;

    const auto interpreted = interpreter.invoke(
        "Phase5.App::main", {}, interpreterOptions);
    const auto compiled = aot.invoke("Phase5.App::main", {}, aotOptions);
    require(interpreted.succeeded && compiled.succeeded &&
            interpreted.value == compiled.value,
        "interpreter/AOT deterministic execution diverged");
    if (interpreted.determinismDigest != compiled.determinismDigest) {
        std::cerr << "interpreter digest=" << interpreted.determinismDigest
            << " aot=" << compiled.determinismDigest << '\n';
        const auto count = std::max(interpreterTrace.size(), aotTrace.size());
        for (std::size_t i = 0; i < count; ++i) {
            const auto left = i < interpreterTrace.size()
                ? realscript::runtime::traceEventKindName(interpreterTrace[i].kind) +
                    std::string(" ") + interpreterTrace[i].function + " " + interpreterTrace[i].operation
                : std::string("<missing>");
            const auto right = i < aotTrace.size()
                ? realscript::runtime::traceEventKindName(aotTrace[i].kind) +
                    std::string(" ") + aotTrace[i].function + " " + aotTrace[i].operation
                : std::string("<missing>");
            if (left != right) {
                std::cerr << "event " << i << "\n I: " << left
                    << "\n A: " << right << '\n';
                break;
            }
        }
        throw std::runtime_error("interpreter/AOT execution digests diverged");
    }

    const auto interpretedTyped = interpreter.invoke(
        "Phase5.App::typedBranch",
        {std::int64_t{4}},
        interpreterOptions);
    const auto compiledTyped = aot.invoke(
        "Phase5.App::typedBranch",
        {std::int64_t{4}},
        aotOptions);
    require(interpretedTyped.succeeded && compiledTyped.succeeded &&
            interpretedTyped.value == compiledTyped.value &&
            interpretedTyped.instructionsExecuted ==
                compiledTyped.instructionsExecuted &&
            interpretedTyped.determinismDigest ==
                compiledTyped.determinismDigest,
        "typed interpreter/AOT deterministic execution diverged");
    const auto interpretedBounded = interpreter.invoke(
        "Phase5.App::boundedIncrement",
        {std::int64_t{99}},
        interpreterOptions);
    const auto compiledBounded = aot.invoke(
        "Phase5.App::boundedIncrement",
        {std::int64_t{99}},
        aotOptions);
    require(interpretedBounded.succeeded && compiledBounded.succeeded &&
            interpretedBounded.value == compiledBounded.value &&
            interpretedBounded.instructionsExecuted ==
                compiledBounded.instructionsExecuted &&
            interpretedBounded.determinismDigest ==
                compiledBounded.determinismDigest,
        "range-proven typed interpreter/AOT deterministic execution diverged");
    require(
        realscript::runtime::profileToJson(interpreterProfile->snapshot()) ==
            realscript::runtime::profileToJson(aotProfile->snapshot()),
        "interpreter/AOT profile attribution diverged");
}

void testOptimizedAotGeneration() {
    realscript::diagnostics::DiagnosticBag diagnostics;
    realscript::optimization::Optimizer optimizer;
    realscript::optimization::Options options;
    options.level = realscript::optimization::Level::Aggressive;
    auto optimized = optimizer.optimize(compileFixture(), diagnostics, options);
    require(!diagnostics.hasErrors() && !optimized.modules.empty(),
        "Phase 6 optimizer did not produce an AOT input");

    realscript::aot::CppGenerator generator;
    realscript::aot::GenerationOptions generation;
    generation.programName = "Phase6Optimized";
    const auto generated = generator.generate(
        optimized.modules,
        diagnostics,
        generation);
    require(!diagnostics.hasErrors() &&
            generated.source.find("Phase6OptimizedProgram") != std::string::npos &&
            generated.contentHash != 0,
        "optimized MIR did not generate deterministic C++17 AOT source");
}

} // namespace

int main() {
    try {
        testCrossBackendDeterminismAndProfiling();
        testOptimizedAotGeneration();
        std::cout << "Phase 6 AOT determinism tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}
