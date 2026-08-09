#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/jit/Jit.h"
#include "realscript/runtime/Runtime.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot read fixture: " + path.string());
    std::ostringstream content;
    content << stream.rdbuf();
    return content.str();
}

std::vector<realscript::mir::Module> compileFixture() {
    realscript::compiler::Compilation compilation;
    const auto root = std::filesystem::path(REALSCRIPT_SOURCE_DIR);
    compilation.addSource({
        "tests/fixtures/phase5_model.rs",
        readFile(root / "tests/fixtures/phase5_model.rs"),
    });
    compilation.addSource({
        "tests/fixtures/phase5_app.rs",
        readFile(root / "tests/fixtures/phase5_app.rs"),
    });
    auto result = compilation.build();
    require(!result.diagnostics.hasErrors(), "JIT fixture failed to compile");
    return result.modules;
}

std::vector<realscript::bytecode::Module> lower(
    const std::vector<realscript::mir::Module>& modules) {
    realscript::bytecode::Lowerer lowerer;
    std::vector<realscript::bytecode::Module> result;
    for (const auto& module : modules) result.push_back(lowerer.lower(module));
    return result;
}

realscript::jit::ToolchainOptions options() {
    realscript::jit::ToolchainOptions result;
    result.compiler = REALSCRIPT_JIT_COMPILER;
    result.includeDirectory = REALSCRIPT_JIT_INCLUDE_DIR;
    result.supportLibrary = REALSCRIPT_JIT_SUPPORT_LIBRARY;
    result.outputDirectory = REALSCRIPT_JIT_CACHE_DIR;
    result.generation.programName = "Phase6JitFixture";
    result.optimization.level = realscript::optimization::Level::Aggressive;
#if defined(_WIN32)
#if defined(_DEBUG)
    result.compilerArguments.push_back("/MDd");
#else
    result.compilerArguments.push_back("/MD");
#endif
#else
    result.compilerArguments.push_back("-O2");
#endif
    return result;
}

void testToolchainJit() {
    const auto modules = compileFixture();
    const auto configuration = options();
    std::error_code cleanupError;
    std::filesystem::remove_all(configuration.outputDirectory, cleanupError);
    require(realscript::jit::ToolchainJit::available(configuration),
        "configured C++17 JIT toolchain is unavailable");

    realscript::jit::ToolchainJit jit;
    auto first = jit.compile(modules, configuration);
    require(first.succeeded(), first.error.empty() ?
        "first JIT compilation failed" : first.error);
    require(!first.cacheHit, "first JIT compilation unexpectedly used the cache");

    realscript::diagnostics::DiagnosticBag optimizationDiagnostics;
    realscript::optimization::Optimizer optimizer;
    auto optimized = optimizer.optimize(
        modules, optimizationDiagnostics, configuration.optimization);
    require(!optimizationDiagnostics.hasErrors(),
        "JIT comparison optimization failed");
    realscript::runtime::Interpreter interpreter(lower(optimized.modules));

    auto jitProfile = std::make_shared<realscript::runtime::ProfileCollector>();
    auto interpreterProfile =
        std::make_shared<realscript::runtime::ProfileCollector>();
    realscript::runtime::ExecutionOptions execution;
    execution.determinism.mode = realscript::runtime::DeterminismMode::Strict;
    execution.profile = jitProfile;
    const auto result = first.module->invoke("Phase5.App::main", {}, execution);
    execution.profile = interpreterProfile;
    const auto interpreted = interpreter.invoke("Phase5.App::main", {}, execution);
    require(result.succeeded && interpreted.succeeded &&
            std::get<std::int64_t>(result.value) == 60 &&
            result.value == interpreted.value,
        "JIT compiled program returned an unexpected result");
    require(result.determinismDigest == interpreted.determinismDigest &&
            result.determinismDigest != 0,
        "JIT/interpreter deterministic digests diverged");
    require(
        realscript::runtime::profileToJson(jitProfile->snapshot()) ==
            realscript::runtime::profileToJson(interpreterProfile->snapshot()),
        "JIT/interpreter profiles diverged");

    auto second = jit.compile(modules, configuration);
    require(second.succeeded(), second.error.empty() ?
        "cached JIT compilation failed" : second.error);
    require(second.cacheHit, "second JIT compilation did not reuse the cache");
    require(second.module->contentHash() == first.module->contentHash(),
        "cached JIT module content hash changed");
}

} // namespace

int main() {
    try {
        testToolchainJit();
        std::cout << "Phase 6 toolchain JIT tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}
