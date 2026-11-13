#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/game/GameBindings.h"
#include "realscript/game/RsBindParser.h"
#include "realscript/jit/Jit.h"
#include "realscript/optimization/Optimizer.h"
#include "realscript/runtime/AsyncRuntime.h"
#include "realscript/runtime/Runtime.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace realscript;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    require(static_cast<bool>(in), "failed to open file: " + path.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::size_t countLines(const std::string& content) {
    std::size_t lines = 0;
    for (const char ch : content) {
        if (ch == '\n') ++lines;
    }
    return lines;
}

std::shared_ptr<const runtime::ProgramImage> compileSourceToProgram(
    const std::string& sourceText,
    const std::string& fileName = "p3_audit_test.rscript",
    optimization::Level optLevel = optimization::Level::Basic) {
    compiler::Compilation comp({compiler::SourceFile{fileName, sourceText}});
    auto build = comp.build();
    if (build.diagnostics.hasErrors()) {
        std::ostringstream oss;
        for (const auto& d : build.diagnostics.items()) {
            oss << d.code << ": " << d.message << "\n";
        }
        throw std::runtime_error("Compilation failed:\n" + oss.str());
    }

    std::vector<mir::Module> mirMods = std::move(build.modules);
    if (optLevel != optimization::Level::None) {
        diagnostics::DiagnosticBag optDiags;
        optimization::Options optOpts;
        optOpts.level = optLevel;
        mirMods = optimization::Optimizer{}.optimize(std::move(mirMods), optDiags, optOpts).modules;
        require(!optDiags.hasErrors(), "MIR optimization failed");
    }

    bytecode::Lowerer lowerer;
    std::vector<bytecode::Module> bcMods;
    bcMods.reserve(mirMods.size());
    for (const auto& m : mirMods) {
        bcMods.push_back(lowerer.lower(m));
    }

    runtime::RuntimeError linkErr;
    auto linked = runtime::ProgramImage::link(std::move(bcMods), linkErr);
    require(linked.has_value(), "Link failed: " + linkErr.message);
    return std::make_shared<const runtime::ProgramImage>(std::move(*linked));
}

// ============================================================================
// 1. AUD-08a: Public SDK RsBindParser.h De-exposure & C++ Parser Fidelity
// ============================================================================
void testRsBindParserSdkHeaderHygieneAndParity() {
#ifdef REALSCRIPT_SOURCE_DIR
    const std::filesystem::path root(REALSCRIPT_SOURCE_DIR);
    const std::string publicHeader = readTextFile(root / "include/realscript/game/RsBindParser.h");
    const std::size_t headerLines = countLines(publicHeader);
    require(
        headerLines < 50,
        "AUD-08 regression: public RsBindParser.h still exposes implementation (" +
            std::to_string(headerLines) + " lines)");
    require(
        publicHeader.find("class CppDeclarationParser") == std::string::npos &&
            publicHeader.find("<sstream>") == std::string::npos &&
            publicHeader.find("<unordered_set>") == std::string::npos,
        "AUD-08 regression: internal parser classes or heavy STL headers leaked in RsBindParser.h");
    require(
        std::filesystem::exists(root / "src/game/RsBindParser.cpp"),
        "expected src/game/RsBindParser.cpp to exist");
#endif

    const char* kCppHeader = R"CPP(
#pragma once
#define ENABLE_PHYSICS_BINDINGS 1

namespace engine::phys {
#if ENABLE_PHYSICS_BINDINGS
RS_EXPORT_ENUM()
enum class BodyLayer : int {
    Static = 0,
    Dynamic = 1 << 1,
};

RS_EXPORT_ECS_COMPONENT()
struct RigidState {
    Fixed64 tick;
    Vec3f velocity;
    Quatf orientation;
};

RS_EXPORT_FUNCTION()
int StepSimulation(int substeps, double dt);
#else
RS_EXPORT_FUNCTION()
int DisabledGhostSymbol(int x);
#endif
}
)CPP";

    const auto viaPublicApi =
        game::GameApi::generateBindingsFromCppHeader(kCppHeader, "Engine.Physics", {});
    const auto viaDetailEntry =
        game::rsbind_detail::generateFromParsedHeader(kCppHeader, "Engine.Physics", {});
    const auto viaDetailDefaultMacros =
        game::rsbind_detail::generateFromParsedHeader(kCppHeader, "rsbind_p3_default_macros");

    require(viaPublicApi.enumsExported == 1, "expected 1 exported enum");
    require(viaPublicApi.structsExported == 1, "expected 1 exported ECS struct");
    require(viaPublicApi.functionsExported == 1, "expected 1 exported function");
    require(
        viaPublicApi.scriptDeclarationStub.find("DisabledGhostSymbol") == std::string::npos,
        "preprocessor dead branch should not export DisabledGhostSymbol");
    require(
        viaPublicApi.scriptDeclarationStub == viaDetailEntry.scriptDeclarationStub &&
            viaPublicApi.cppRegistrationSource == viaDetailEntry.cppRegistrationSource,
        "GameApi vs rsbind_detail output mismatch after moving implementation to RsBindParser.cpp");
    require(
        viaDetailDefaultMacros.enumsExported == 1 &&
            viaDetailDefaultMacros.structsExported == 1 &&
            viaDetailDefaultMacros.functionsExported == 1,
        "two-arg generateFromParsedHeader default predefinedMacros failed");
}

// ============================================================================
// 2. AUD-08b: Physical JIT Module Split & Cross-Tier Execution Parity
// ============================================================================
void testJitModularSplitAndCrossTierParity() {
#ifdef REALSCRIPT_SOURCE_DIR
    const std::filesystem::path root(REALSCRIPT_SOURCE_DIR);
    require(
        !std::filesystem::exists(root / "src/jit/Jit.cpp"),
        "monolithic src/jit/Jit.cpp should be removed after modular split");

    const std::vector<std::string> expectedJitUnits = {
        "src/jit/ToolchainJit.cpp",
        "src/jit/StencilJitRuntime.cpp",
        "src/jit/StencilJitX64.cpp",
        "src/jit/StencilJitArm64.cpp",
        "src/jit/StencilJit.cpp",
    };
    for (const auto& rel : expectedJitUnits) {
        const auto p = root / rel;
        require(std::filesystem::exists(p), "missing split JIT module: " + rel);
        const std::size_t lines = countLines(readTextFile(p));
        require(
            lines < 3200,
            "split JIT module exceeds 3,200-line maintainability budget: " + rel +
                " (" + std::to_string(lines) + " lines)");
    }
    require(
        std::filesystem::exists(root / "src/jit/JitInternal.h") &&
            !std::filesystem::exists(root / "include/realscript/jit/JitInternal.h"),
        "AUD-08b regression: JitInternal.h must live under src/jit/ and stay out of public include/");
    const std::string toolchainJitSrc = readTextFile(root / "src/jit/ToolchainJit.cpp");
    require(
        toolchainJitSrc.find("ensureFileContent") != std::string::npos &&
            toolchainJitSrc.find("PROC_THREAD_ATTRIBUTE_HANDLE_LIST") != std::string::npos &&
            toolchainJitSrc.find("writeFileIfChanged") == std::string::npos &&
            toolchainJitSrc.find("std::system(") == std::string::npos,
        "AUD-07 regression: ToolchainJit.cpp lost Phase 2 subprocess/file hardening after split");
#endif

    const std::string jitScript = R"RS(
module P3JitSplit;

public struct PairAcc {
    public int sum;
    public int xorAcc;
    public int prod;
    public int count;
    public int minVal;
    public int maxVal;
}

public int runKernel(int n) {
    int[] data = new int[n];
    for (int i = 0; i < n; i = i + 1) {
        data[i] = unchecked((i * 17 + 5) % 97);
    }
    PairAcc acc = new PairAcc();
    acc.minVal = 1000000;
    acc.maxVal = -1000000;
    acc.prod = 1;
    for (int i = 0; i < n; i = i + 1) {
        int v = data[i];
        acc.sum = acc.sum + v;
        acc.xorAcc = unchecked(acc.xorAcc * 31 + v);
        acc.prod = unchecked((acc.prod * (v + 3)) % 10007);
        acc.count = acc.count + 1;
        if (v < acc.minVal) { acc.minVal = v; }
        if (v > acc.maxVal) { acc.maxVal = v; }
    }
    return acc.sum * 10 + acc.prod + acc.maxVal - acc.minVal + acc.count;
}
)RS";

    auto program = compileSourceToProgram(jitScript, "p3_jit_split.rscript", optimization::Level::Aggressive);

    // Reference VM execution
    auto vmHeap = std::make_shared<runtime::ManagedHeap>();
    runtime::Interpreter vm(program, vmHeap);
    const auto fnSym = program->findFunction("P3JitSplit::runKernel");
    require(fnSym.has_value(), "P3JitSplit::runKernel not found");
    const auto vmRes = vm.invoke(*fnSym, {runtime::Value{std::int64_t{64}}});
    require(vmRes.succeeded, "VM reference failed: " + vmRes.error.message);
    const std::int64_t expectedVal = std::get<std::int64_t>(vmRes.value);

    // Tier-2 Native (x86_64 or ARM64 host) + Simulated ARM64
    jit::StencilJit stencilCompiler;
    jit::StencilJitOptions nativeOpts;
    nativeOpts.enableNativeCodegen = true;
    auto nativeCompiled = stencilCompiler.compile(program, nativeOpts);
    require(nativeCompiled.succeeded(), "StencilJit native compile failed: " + nativeCompiled.error);

    const auto nativeRes = nativeCompiled.module->invoke(
        "P3JitSplit::runKernel", {runtime::Value{std::int64_t{64}}});
    require(nativeRes.succeeded, "StencilJit native invoke failed: " + nativeRes.error.message);
    require(
        std::get<std::int64_t>(nativeRes.value) == expectedVal &&
            nativeRes.instructionsExecuted == vmRes.instructionsExecuted,
        "StencilJit native parity mismatch against VM");

#if UINTPTR_MAX == 0xffffffffffffffffULL
    const auto arm64SimRes = nativeCompiled.module->invokeSimulatedArm64(
        "P3JitSplit::runKernel", {runtime::Value{std::int64_t{64}}});
    require(arm64SimRes.succeeded, "Simulated ARM64 invoke failed: " + arm64SimRes.error.message);
    require(
        std::get<std::int64_t>(arm64SimRes.value) == expectedVal &&
            arm64SimRes.instructionsExecuted == vmRes.instructionsExecuted,
        "Simulated ARM64 parity mismatch against VM");

    const auto x64Bytes = nativeCompiled.module->inspectMachineCode(
        "P3JitSplit::runKernel", jit::StencilTargetArch::X86_64);
    const auto a64Bytes = nativeCompiled.module->inspectMachineCode(
        "P3JitSplit::runKernel", jit::StencilTargetArch::Arm64);
    require(!x64Bytes.empty() && !a64Bytes.empty(), "expected non-empty x64 and ARM64 machine code");
#endif

    // Tier-1 Portable Direct-Threaded Stencil Thunks
    jit::StencilJitOptions threadedOpts;
    threadedOpts.enableNativeCodegen = false;
    auto threadedCompiled = stencilCompiler.compile(program, threadedOpts);
    require(threadedCompiled.succeeded(), "StencilJit threaded compile failed: " + threadedCompiled.error);
    const auto threadedRes = threadedCompiled.module->invoke(
        "P3JitSplit::runKernel", {runtime::Value{std::int64_t{64}}});
    require(threadedRes.succeeded, "StencilJit threaded invoke failed: " + threadedRes.error.message);
    require(
        std::get<std::int64_t>(threadedRes.value) == expectedVal &&
            threadedRes.instructionsExecuted == vmRes.instructionsExecuted,
        "StencilJit Tier-1 threaded parity mismatch against VM");
}

// ============================================================================
// 3. AUD-08c: Physical Interpreter Split (Entry vs SlowOps/BlockVM vs FlatLoop)
// ============================================================================
void testInterpreterModularSplitFlatVsBlockVmParity() {
#ifdef REALSCRIPT_SOURCE_DIR
    const std::filesystem::path root(REALSCRIPT_SOURCE_DIR);
    const std::size_t entryLines = countLines(readTextFile(root / "src/runtime/Interpreter.cpp"));
    require(
        entryLines < 1200,
        "AUD-08 regression: src/runtime/Interpreter.cpp was not modularized (" +
            std::to_string(entryLines) + " lines)");
    require(
        std::filesystem::exists(root / "src/runtime/InterpreterSlowOps.cpp") &&
            std::filesystem::exists(root / "src/runtime/InterpreterFlatLoop.cpp"),
        "expected InterpreterSlowOps.cpp and InterpreterFlatLoop.cpp to exist");
    require(
        std::filesystem::exists(root / "src/runtime/InterpreterInternal.h") &&
            !std::filesystem::exists(root / "include/realscript/runtime/InterpreterInternal.h"),
        "AUD-08c regression: InterpreterInternal.h must live under src/runtime/ and stay out of public include/");
    const std::size_t slowOpsLines = countLines(readTextFile(root / "src/runtime/InterpreterSlowOps.cpp"));
    const std::size_t flatLoopLines = countLines(readTextFile(root / "src/runtime/InterpreterFlatLoop.cpp"));
    require(
        slowOpsLines < 4500 && flatLoopLines < 7500,
        "AUD-08c regression: split interpreter units exceed modular line budgets (SlowOps=" +
            std::to_string(slowOpsLines) + ", FlatLoop=" + std::to_string(flatLoopLines) + ")");
#endif

    const std::string vmSrc = R"RS(
module P3InterpSplit;
import RealScript.Async;

delegate void AccumulatorFn(int delta);

class NegativeInputError {
    public int code = 1;
}

class Bucket {
    event AccumulatorFn Updated;
    int total = 0;

    public Bucket() {
        Updated += Add;
        Updated += d => total = total + d * 2;
    }

    void Add(int d) { total = total + d; }

    public int Fire(int seed) {
        Updated(seed);
        return total;
    }
}

int mayThrowOnNegative(int v) {
    if (v < 0) {
        throw new NegativeInputError();
    }
    return v * 3;
}

public int exerciseDelegatesAndExceptions(int seed) {
    Bucket b = new Bucket();
    int subTotal = b.Fire(seed);

    int recovered = 0;
    try {
        recovered = mayThrowOnNegative(-seed);
    } catch (NegativeInputError ex) {
        recovered = 19 + ex.code;
    }
    return subTotal + recovered + mayThrowOnNegative(seed);
}

async Awaitable addAfterSignal(Awaitable<int> sig, int bonus, int[] outBox) {
    int v = await sig;
    await Awaitable.Yield();
    outBox[0] = v + bonus;
}

public int exerciseAsyncRoundtrip(int x) {
    Awaitable<int> sig = Async.NewIntSignal();
    int[] outBox = new int[1];
    outBox[0] = 0;
    Awaitable task = addAfterSignal(sig, 100, outBox);
    if (task.IsCompleted()) { return -1; }
    Async.ResolveIntSignal(sig, x);
    Async.StepUpdate();
    if (task.IsCompleted()) { return -2; }
    Async.StepUpdate();
    if (!task.IsCompleted()) { return -3; }
    task.Release();
    return outBox[0];
}
)RS";

    auto program = compileSourceToProgram(vmSrc, "p3_interp_split.rscript", optimization::Level::Basic);
    const auto syncSym = program->findFunction("P3InterpSplit::exerciseDelegatesAndExceptions");
    const auto asyncSym = program->findFunction("P3InterpSplit::exerciseAsyncRoundtrip");
    require(syncSym.has_value() && asyncSym.has_value(), "functions missing in P3InterpSplit");

    auto heap = std::make_shared<runtime::ManagedHeap>();
    runtime::Interpreter interp(program, heap);

    // 1. FlatLoop fast path (DeterminismMode::Off)
    runtime::ExecutionOptions flatOpts;
    flatOpts.determinism.mode = runtime::DeterminismMode::Off;
    const auto flatRes = interp.invoke(*syncSym, {runtime::Value{std::int64_t{7}}}, flatOpts);
    require(flatRes.succeeded, "FlatLoop invoke failed: " + flatRes.error.message);

    // 2. BlockVM + SlowOps path (DeterminismMode::Strict)
    runtime::ExecutionOptions strictOpts;
    strictOpts.determinism.mode = runtime::DeterminismMode::Strict;
    const auto strictRes = interp.invoke(*syncSym, {runtime::Value{std::int64_t{7}}}, strictOpts);
    require(strictRes.succeeded, "Strict BlockVM invoke failed: " + strictRes.error.message);

    // Expected: subTotal = 7 + 14 = 21; recovered = 20; mayThrowOnNegative(7) = 21 -> 62
    require(
        std::get<std::int64_t>(flatRes.value) == 62 &&
            std::get<std::int64_t>(strictRes.value) == 62 &&
            flatRes.instructionsExecuted == strictRes.instructionsExecuted,
        "FlatLoop vs BlockVM parity mismatch after Interpreter split");

    // 3. Coroutine suspend & resume across BOTH FlatLoop (Off) and BlockVM (Strict)
    auto asyncCtx = std::make_shared<runtime::AsyncRuntimeContext>(heap.get());
    interp.setAsyncRuntime(asyncCtx);
    const auto asyncFlat = interp.invoke(*asyncSym, {runtime::Value{std::int64_t{23}}}, flatOpts);
    require(asyncFlat.succeeded && std::get<std::int64_t>(asyncFlat.value) == 123,
        "FlatLoop async roundtrip failed: " + asyncFlat.error.message);

    const auto asyncStrict = interp.invoke(*asyncSym, {runtime::Value{std::int64_t{23}}}, strictOpts);
    require(asyncStrict.succeeded && std::get<std::int64_t>(asyncStrict.value) == 123,
        "BlockVM async roundtrip failed: " + asyncStrict.error.message);
}

} // namespace

int main() {
    try {
        testRsBindParserSdkHeaderHygieneAndParity();
        testJitModularSplitAndCrossTierParity();
        testInterpreterModularSplitFlatVsBlockVmParity();
        std::cout << "All P3 audit maintainability & modular split regression tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "P3 audit regression test failure: " << ex.what() << "\n";
        return 1;
    }
}
