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

    // Verify 16-byte Value cross-DLL pool bridge (NativeHandle typeId, QuatArena, StringValue) across JIT shared module boundary
    {
        const char* bridgeSrc = R"(
module Phase6.JitValueBridge;
import RealScript.Math;

handle echoHandle(handle h)
{
    return h;
}

Quat echoQuat(Quat q)
{
    return q;
}

Quat makeJitArenaQuat(int wRaw)
{
    return Quat.Raw(404, -505, 606, wRaw);
}

string echoString(string s)
{
    return s;
}

string makeBridgeLabel(string prefix)
{
    return prefix + "_from_jit_dll";
}
)";
        realscript::compiler::Compilation comp;
        comp.addSource({"jit_bridge.rs", bridgeSrc});
        auto built = comp.build();
        require(!built.diagnostics.hasErrors(), "JIT value bridge fixture failed to compile");

        auto bridgeCfg = options();
        bridgeCfg.generation.programName = "Phase6JitValueBridge";
        auto bridgeJit = jit.compile(built.modules, bridgeCfg);
        require(bridgeJit.succeeded(), bridgeJit.error.empty() ? "bridge JIT failed" : bridgeJit.error);

        auto sharedHeap = std::make_shared<realscript::runtime::ManagedHeap>();
        bridgeJit.module->setHeap(sharedHeap);

        const auto handleTid = realscript::semantic::stableTypeId("Phase6.JitValueBridge::BridgeEntity");
        const realscript::runtime::NativeHandle hostHandle(17u, 5u, handleTid, 3u);
        const auto hRes = bridgeJit.module->invoke("Phase6.JitValueBridge::echoHandle", {hostHandle});
        require(
            hRes.succeeded && std::get<realscript::runtime::NativeHandle>(hRes.value) == hostHandle,
            "NativeHandle typeId corrupted across JIT DLL boundary");

        const realscript::runtime::QuatValue bigQuat{101, -202, 303, 1500000000};
        const auto qRes = bridgeJit.module->invoke("Phase6.JitValueBridge::echoQuat", {bigQuat});
        require(
            qRes.succeeded && std::get<realscript::runtime::QuatValue>(qRes.value) == bigQuat,
            "QuatArena (Host -> JIT -> Host) corrupted across JIT DLL boundary");

        const realscript::runtime::QuatValue jitQuatExpected{404, -505, 606, 1750000000};
        const auto qJitRes = bridgeJit.module->invoke(
            "Phase6.JitValueBridge::makeJitArenaQuat",
            {std::int64_t{1750000000}});
        require(
            qJitRes.succeeded &&
                std::get<realscript::runtime::QuatValue>(qJitRes.value) == jitQuatExpected,
            "QuatArena (JIT -> Host) corrupted across JIT DLL boundary");

        const std::string hostLongStr = "host_seed_long_non_sso_bridge_string";
        const auto echoStrRes = bridgeJit.module->invoke(
            "Phase6.JitValueBridge::echoString",
            {hostLongStr});
        require(
            echoStrRes.succeeded && std::get<std::string>(echoStrRes.value) == hostLongStr,
            "StringValue echo corrupted across JIT DLL boundary");

        const auto sRes = bridgeJit.module->invoke(
            "Phase6.JitValueBridge::makeBridgeLabel",
            {hostLongStr});
        const auto* strRef = std::get_if<realscript::runtime::ObjectRef>(&sRes.value);
        require(
            sRes.succeeded && strRef != nullptr &&
                sharedHeap->stringView(*strRef) == (hostLongStr + "_from_jit_dll"),
            "StringValue concat across JIT DLL boundary corrupted");
    }
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
