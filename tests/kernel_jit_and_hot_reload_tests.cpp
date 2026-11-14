#include "realscript/aot_cpp/AotCpp.h"
#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/hot_reload/HotReload.h"
#include "realscript/jit/Jit.h"
#include "realscript/optimization/Optimizer.h"
#include "realscript/runtime/Runtime.h"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
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

std::string formatDiags(const diagnostics::DiagnosticBag& bag) {
    std::ostringstream oss;
    for (const auto& d : bag.items()) {
        oss << d.sourceName << " [" << d.code << "] " << d.message << '\n';
    }
    return oss.str();
}

struct CompiledBundle {
    std::vector<mir::Module> mirModules;
    std::vector<bytecode::Module> bytecodeModules;
    std::shared_ptr<const runtime::ProgramImage> program;
};

CompiledBundle compileAndLink(
    std::vector<compiler::SourceFile> sources,
    optimization::Level optLevel = optimization::Level::Basic) {
    compiler::Compilation comp(std::move(sources));
    auto build = comp.build();
    require(!build.diagnostics.hasErrors(), "Compile failed:\n" + formatDiags(build.diagnostics));

    diagnostics::DiagnosticBag optDiag;
    optimization::Options optOpts;
    optOpts.level = optLevel;
    auto optimized = optimization::Optimizer{}.optimize(std::move(build.modules), optDiag, optOpts);
    require(!optDiag.hasErrors(), "Optimization failed");

    bytecode::Lowerer lowerer;
    std::vector<bytecode::Module> bcMods;
    bcMods.reserve(optimized.modules.size());
    for (const auto& m : optimized.modules) {
        bcMods.push_back(lowerer.lower(m));
    }

    runtime::RuntimeError linkErr;
    auto linked = runtime::ProgramImage::link(bcMods, linkErr);
    require(linked.has_value(), "Link failed: " + linkErr.message);
    return CompiledBundle{
        std::move(optimized.modules),
        std::move(bcMods),
        std::make_shared<const runtime::ProgramImage>(std::move(*linked)),
    };
}

void assertParity(
    const std::string& label,
    const runtime::ExecutionResult& actual,
    const runtime::ExecutionResult& expected) {
    require(actual.succeeded == expected.succeeded,
        label + ": succeeded mismatch (" + actual.error.message + " vs " + expected.error.message + ")");
    if (!expected.succeeded) {
        require(actual.error.code == expected.error.code, label + ": error code mismatch");
        return;
    }
    if (std::holds_alternative<std::int64_t>(expected.value)) {
        require(std::get<std::int64_t>(actual.value) == std::get<std::int64_t>(expected.value),
            label + ": int mismatch");
    } else if (std::holds_alternative<double>(expected.value)) {
        require(std::get<double>(actual.value) == std::get<double>(expected.value),
            label + ": double mismatch");
    }
}

// ---------------------------------------------------------------------------
// 1. StencilJit W^X Arena, Tier-1 Threaded, Tier-2 x86_64 & Simulated ARM64
// ---------------------------------------------------------------------------
void testStencilJitTiersAndSimulatedArm64Parity() {
    const std::string src = R"RS(
module Kernel.StencilSuite;

int LeafScale(int x) {
    return x * 3 + 7;
}

public int CheckedAdd(int a, int b) {
    return a + b;
}

public int DivOp(int a, int b) {
    return a / b;
}

public int RemOp(int a, int b) {
    return a % b;
}

public int SumLoop(int n) {
    int acc = 0;
    for (int i = 0; i < n; i = i + 1) {
        acc = acc + LeafScale(i);
    }
    return acc;
}
)RS";

    auto bundle = compileAndLink({{"stencil_suite.rscript", src}}, optimization::Level::Basic);

    jit::StencilJit sjit;
    jit::StencilJitOptions nativeOpts;
    nativeOpts.enableNativeCodegen = true;
    auto nativeCompiled = sjit.compile(bundle.program, nativeOpts);
    require(nativeCompiled.succeeded(), "StencilJit native compile failed: " + nativeCompiled.error);

    jit::StencilJitOptions threadedOpts;
    threadedOpts.enableNativeCodegen = false;
    auto threadedCompiled = sjit.compile(bundle.program, threadedOpts);
    require(threadedCompiled.succeeded() &&
            threadedCompiled.threadedFunctionCount == threadedCompiled.totalFunctionCount,
        "StencilJit Tier-1 threaded compile failed");

#if UINTPTR_MAX == 0xffffffffffffffffULL
    require(nativeCompiled.nativeBackendActive && nativeCompiled.nativeFunctionCount == nativeCompiled.totalFunctionCount,
        "Expected active native Tier-2 x86_64/ARM64 codegen on 64-bit host");
    const auto nativeBytes = nativeCompiled.module->inspectMachineCode("Kernel.StencilSuite::SumLoop");
    require(nativeBytes.size() > 16, "Native machine code inspection returned too few bytes");
#endif

    const auto armBytes =
        nativeCompiled.module->inspectMachineCode("Kernel.StencilSuite::SumLoop", jit::StencilTargetArch::Arm64);
    require(armBytes.size() > 16, "Simulated ARM64 codegen produced empty buffer");

    runtime::Interpreter interp(bundle.program);
    const std::int64_t iMax = std::numeric_limits<std::int32_t>::max();
    const std::int64_t iMin = std::numeric_limits<std::int32_t>::min();

    struct Case {
        const char* fn;
        std::vector<runtime::Value> args;
    };
    const std::vector<Case> cases = {
        {"Kernel.StencilSuite::CheckedAdd", {std::int64_t{42}, std::int64_t{58}}},
        {"Kernel.StencilSuite::CheckedAdd", {iMax, std::int64_t{1}}},
        {"Kernel.StencilSuite::DivOp", {std::int64_t{-100}, std::int64_t{7}}},
        {"Kernel.StencilSuite::DivOp", {std::int64_t{10}, std::int64_t{0}}},
        {"Kernel.StencilSuite::DivOp", {iMin, std::int64_t{-1}}},
        {"Kernel.StencilSuite::RemOp", {std::int64_t{-100}, std::int64_t{7}}},
        {"Kernel.StencilSuite::SumLoop", {std::int64_t{40}}},
    };

    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& c = cases[i];
        const auto exp = interp.invoke(c.fn, c.args);
        assertParity(std::string("Native#") + std::to_string(i), nativeCompiled.module->invoke(c.fn, c.args), exp);
        assertParity(std::string("Threaded#") + std::to_string(i), threadedCompiled.module->invoke(c.fn, c.args), exp);
        assertParity(std::string("SimArm64#") + std::to_string(i),
            nativeCompiled.module->invokeSimulatedArm64(c.fn, c.args), exp);
    }

    // Exact instruction budget boundary parity.
    const auto fullRun = interp.invoke("Kernel.StencilSuite::SumLoop", {std::int64_t{30}});
    require(fullRun.succeeded && fullRun.instructionsExecuted > 10, "SumLoop baseline failed");
    runtime::Limits lim;
    lim.instructionBudget = fullRun.instructionsExecuted - 3;
    const auto expLim = interp.invoke("Kernel.StencilSuite::SumLoop", {std::int64_t{30}}, lim);
    assertParity("BudgetNative", nativeCompiled.module->invoke("Kernel.StencilSuite::SumLoop", {std::int64_t{30}}, lim), expLim);
    assertParity("BudgetThreaded", threadedCompiled.module->invoke("Kernel.StencilSuite::SumLoop", {std::int64_t{30}}, lim), expLim);
}

// ---------------------------------------------------------------------------
// 2. ToolchainJit Shell-Free Subprocess Hardening & AOT Sync Header Diet
// ---------------------------------------------------------------------------
void testToolchainJitShellFreeAndHeaderDiet() {
    {
        const auto syncBundle = compileAndLink({{"sync_diet.rscript", R"RS(
module Kernel.SyncDiet;
public int Mul3(int a, int b) { return (a + b) * 3; }
)RS"}});
        diagnostics::DiagnosticBag diags;
        aot::CppGenerator gen;
        const auto genSync = gen.generate(syncBundle.mirModules, diags);
        require(!diags.hasErrors(), "Sync AOT generate failed");
        require(genSync.source.find("#define REALSCRIPT_AOT_NO_ASYNC_INLINES 1") != std::string::npos,
            "Sync AOT source missing REALSCRIPT_AOT_NO_ASYNC_INLINES");

        const auto asyncBundle = compileAndLink({{"async_diet.rscript", R"RS(
module Kernel.AsyncDiet;
import RealScript.Async;
public async Awaitable<int> Ping() { return 7; }
)RS"}});
        const auto genAsync = gen.generate(asyncBundle.mirModules, diags);
        require(!diags.hasErrors(), "Async AOT generate failed");
        require(genAsync.source.find("REALSCRIPT_AOT_NO_ASYNC_INLINES") == std::string::npos,
            "Async AOT source must retain inline AsyncRuntime fast paths");
    }

#if defined(REALSCRIPT_JIT_COMPILER) && defined(REALSCRIPT_JIT_INCLUDE_DIR) && \
    defined(REALSCRIPT_JIT_SUPPORT_LIBRARY) && defined(REALSCRIPT_JIT_CACHE_DIR)
    jit::ToolchainOptions baseOpts;
    baseOpts.compiler = REALSCRIPT_JIT_COMPILER;
    baseOpts.includeDirectory = REALSCRIPT_JIT_INCLUDE_DIR;
    baseOpts.supportLibrary = REALSCRIPT_JIT_SUPPORT_LIBRARY;
    baseOpts.outputDirectory = REALSCRIPT_JIT_CACHE_DIR;
#if defined(_MSC_VER)
#ifdef _DEBUG
    baseOpts.compilerArguments = {"/MDd"};
#else
    baseOpts.compilerArguments = {"/MD", "/O1"};
#endif
#else
    baseOpts.compilerArguments = {"-O1"};
#endif

    if (jit::ToolchainJit::available(baseOpts)) {
        auto jitBundle = compileAndLink({{"tc_shellfree.rscript", R"RS(
module Kernel.TcShellFree;
public int SquarePlus(int x) { return x * x + 9; }
)RS"}});
        const auto parentDir = baseOpts.outputDirectory.parent_path();
        const auto canary = parentDir / "kernel_shell_injection_canary.txt";
        std::error_code ec;
        std::filesystem::remove(canary, ec);

        const auto trickyDir = parentDir /
            "jit cache (spaces & 'quotes'); $(touch kernel_shell_injection_canary.txt)";
        std::filesystem::remove_all(trickyDir, ec);
        const auto trickyInc = trickyDir / "inc (a & %TEMP% 'q')";
        std::filesystem::create_directories(trickyInc, ec);

        jit::ToolchainOptions tricky = baseOpts;
        tricky.outputDirectory = trickyDir;
        tricky.reuseCachedLibrary = false;
#if defined(_MSC_VER)
        tricky.compilerArguments.push_back("/I" + trickyInc.string());
#else
        tricky.compilerArguments.push_back("-I" + trickyInc.string());
#endif

        jit::ToolchainJit tc;
        const auto compiled = tc.compile(jitBundle.mirModules, tricky);
        require(compiled.succeeded(), "ToolchainJit failed with special chars path: " + compiled.error);
        require(!std::filesystem::exists(canary, ec),
            "Security regression: shell metacharacters executed during ToolchainJit compile!");

        compiled.module->setHeap(std::make_shared<runtime::ManagedHeap>());
        const auto inv = compiled.module->invoke(
            "Kernel.TcShellFree::SquarePlus", {runtime::Value{std::int64_t{6}}});
        require(inv.succeeded && std::get<std::int64_t>(inv.value) == 45,
            "ToolchainJit compiled module returned wrong result");
        std::filesystem::remove_all(trickyDir, ec);
    }
#endif
}

// ---------------------------------------------------------------------------
// 3. HotReload: -O2 Inline Cache Invalidation, Structural [FormerlySerializedAs],
//    & Active `sequence` Coroutine Migration
// ---------------------------------------------------------------------------
void testHotReloadInlineStructuralAndSequences() {
    // A. Cross-module -O2 inlining stale mix rejection + inlineInvalidatedFunctions flush
    {
        const std::string formulaV1 = R"RS(
module Kernel.HrFormula;
public int Calc(int atk, int def) { return atk * 2 - def; }
)RS";
        const std::string formulaV2 = R"RS(
module Kernel.HrFormula;
public int Calc(int atk, int def) { return atk * 3 - def + 10; }
)RS";
        const std::string combatSrc = R"RS(
module Kernel.HrCombat;
import Kernel.HrFormula;
public int Strike(int atk, int def) { return Calc(atk, def) + 1; }
)RS";
        auto v1 = compileAndLink({{"f.rscript", formulaV1}, {"c.rscript", combatSrc}}, optimization::Level::Aggressive);
        auto v2 = compileAndLink({{"f.rscript", formulaV2}, {"c.rscript", combatSrc}}, optimization::Level::Aggressive);

        const bytecode::Module* staleCombat = nullptr;
        const bytecode::Module* freshFormula = nullptr;
        for (const auto& m : v1.bytecodeModules) {
            if (m.name == "Kernel.HrCombat") staleCombat = &m;
        }
        for (const auto& m : v2.bytecodeModules) {
            if (m.name == "Kernel.HrFormula") freshFormula = &m;
        }
        require(staleCombat && freshFormula, "Missing modules for inline invalidation check");

        runtime::EngineRuntime rt(v1.program);
        const auto r1 = rt.invoke("Kernel.HrCombat::Strike", {runtime::Value{std::int64_t{10}}, runtime::Value{std::int64_t{4}}});
        require(r1.succeeded && std::get<std::int64_t>(r1.value) == 17, "V1 Strike result mismatch");

        const auto stalePlan = hot_reload::prepare(*v1.program, {*freshFormula, *staleCombat});
        bool sawStale = false;
        for (const auto& iss : stalePlan.issues) {
            if (iss.kind == hot_reload::ReloadIssueKind::StaleInlinedDependency) {
                sawStale = true;
            }
        }
        require(!stalePlan.compatible && sawStale, "Expected StaleInlinedDependency rejection");

        const auto okPlan = hot_reload::apply(rt, v2.bytecodeModules);
        require(okPlan.compatible && !okPlan.inlineInvalidatedFunctions.empty(),
            "Expected clean inline invalidation reload");
        const auto r2 = rt.invoke("Kernel.HrCombat::Strike", {runtime::Value{std::int64_t{10}}, runtime::Value{std::int64_t{4}}});
        require(r2.succeeded && std::get<std::int64_t>(r2.value) == 37, "V2 Strike result mismatch");
    }

    // B. Structural Hot Reload with [FormerlySerializedAs], new virtual slot & delegate rebind
    {
        const std::string structV1 = R"RS(
module Kernel.HrStruct;
public delegate int CalcFn();
public class BaseUnit {
    public virtual int Compute() { return 10; }
}
public class EliteUnit : BaseUnit {
    public static int s_oldBonus = 200;
    public int legacyScore;
    public override int Compute() { return 50 + EliteUnit.s_oldBonus; }
}
public class SceneState {
    public EliteUnit unit;
    public CalcFn fn;
    public int InvokeFn() { return fn(); }
}
public CalcFn BindCompute(BaseUnit b) { return b.Compute; }
public SceneState Init() {
    EliteUnit u = new EliteUnit();
    u.legacyScore = 123;
    EliteUnit.s_oldBonus = 400;
    SceneState s = new SceneState();
    s.unit = u;
    s.fn = BindCompute(u);
    return s;
}
)RS";
        const std::string structV2 = R"RS(
module Kernel.HrStruct;
public delegate int CalcFn();
public class BaseUnit {
    public virtual int AlphaRank() { return 1; }
    public virtual int Compute() { return 10; }
}
public class EliteUnit : BaseUnit {
    [FormerlySerializedAs("s_oldBonus")]
    public static long s_bonus = 0L;
    [FormerlySerializedAs("legacyScore")]
    public int currentScore;
    public int newField = 7;
    public override int AlphaRank() { return 2; }
    public override int Compute() { return (int)EliteUnit.s_bonus + currentScore + newField; }
}
public class SceneState {
    public EliteUnit unit;
    public CalcFn fn;
    public int InvokeFn() { return fn(); }
}
public int Verify(SceneState s) {
    BaseUnit b = s.unit;
    return s.InvokeFn() + b.Compute() + b.AlphaRank();
}
)RS";

        auto v1 = compileAndLink({{"hr_struct_v1.rscript", structV1}});
        auto v2 = compileAndLink({{"hr_struct_v2.rscript", structV2}});
        runtime::EngineRuntime rt(v1.program);
        const auto init = rt.invoke("Kernel.HrStruct::Init");
        require(init.succeeded, "Structural Init failed");
        const auto stateRef = std::get<runtime::ObjectRef>(init.value);
        auto root = rt.heap()->retain(runtime::Value{stateRef});

        hot_reload::ReloadOptions opts;
        opts.allowStructuralChanges = true;
        const auto plan = hot_reload::apply(rt, v2.bytecodeModules, opts);
        std::ostringstream hrDiag;
        for (const auto& iss : plan.issues) {
            hrDiag << iss.subject << ": " << iss.message << "; ";
        }
        require(plan.compatible && plan.migrationStats.delegatesRebound == 1,
            "Structural hot reload failed or did not rebind polymorphic delegate (compatible=" +
                std::to_string(plan.compatible ? 1 : 0) +
                ", rebound=" + std::to_string(plan.migrationStats.delegatesRebound) +
                ", issues=" + hrDiag.str() + ")");

        const auto ver = rt.invoke("Kernel.HrStruct::Verify", {runtime::Value{stateRef}});
        // s_bonus(400) + currentScore(123) + newField(7) = 530; 530 + 530 + 2 = 1062
        require(ver.succeeded && std::get<std::int64_t>(ver.value) == 1062,
            "Structural [FormerlySerializedAs] field/vtable migration returned unexpected value");
    }

    // C. Active `sequence` coroutine body-only reload + state-layout rejection
    {
        const auto seqSrc = [](int delta, bool extraLocal) {
            return std::string(R"(
module Kernel.HrSeq;
long Schedule(long target, string cb, int delay) { return target + delay; }
bool CancelTimer(long timer) { return timer != 0; }
class Actor {
    int val;
    sequence Run(long target) {
        int persisted = 5;
)") + (extraLocal ? "        int extra = 9;\n" : "") + R"(        yield wait_ticks(1);
        val = persisted + )" + std::to_string(delta) + R"(;
    }
    int Read() { return val; }
}
Actor Create() { return new Actor(); }
)";
        };

        auto v1 = compileAndLink({{"hr_seq.rscript", seqSrc(2, false)}});
        auto v2 = compileAndLink({{"hr_seq.rscript", seqSrc(8, false)}});
        auto v3Bad = compileAndLink({{"hr_seq.rscript", seqSrc(8, true)}});

        runtime::EngineRuntime rt(v1.program);
        const auto obj = std::get<runtime::ObjectRef>(rt.invoke("Kernel.HrSeq::Create").value);
        auto pin = rt.heap()->retain(runtime::Value{obj});
        require(rt.invoke("Kernel.HrSeq::Actor.Run", {obj, runtime::Value{runtime::LongValue{1}}}).succeeded,
            "Sequence Run start failed");

        const auto okPlan = hot_reload::apply(rt, v2.bytecodeModules);
        require(okPlan.compatible && !okPlan.changedFunctions.empty(),
            "Body-only active sequence hot reload rejected");
        require(rt.invoke("Kernel.HrSeq::Actor.$sequence_Run_1", {obj}).succeeded,
            "Resuming reloaded active sequence failed");
        const auto readRes = rt.invoke("Kernel.HrSeq::Actor.Read", {obj});
        require(readRes.succeeded && std::get<std::int64_t>(readRes.value) == (5 + 8),
            "Reloaded active sequence did not apply updated body with preserved locals");

        const auto badPlan = hot_reload::prepare(*rt.programSnapshot(), v3Bad.bytecodeModules);
        bool layoutChanged = false;
        for (const auto& iss : badPlan.issues) {
            if (iss.kind == hot_reload::ReloadIssueKind::TypeLayoutChanged) {
                layoutChanged = true;
            }
        }
        require(!badPlan.compatible && layoutChanged,
            "Expected TypeLayoutChanged rejection when changing sequence local layout");
    }
}

} // namespace

int main() {
    try {
        std::vector<std::pair<const char*, void (*)()>> tests = {
            {"StencilJit Tier-1/Tier-2/Simulated ARM64 & budget parity", testStencilJitTiersAndSimulatedArm64Parity},
            {"ToolchainJit shell-free hardening & AOT sync header diet", testToolchainJitShellFreeAndHeaderDiet},
            {"HotReload -O2 inline invalidation, structural migration & active sequence", testHotReloadInlineStructuralAndSequences},
        };
        for (const auto& [name, fn] : tests) {
            std::cout << "[RUN ] " << name << '\n';
            fn();
            std::cout << "[PASS] " << name << '\n';
        }
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[FAIL] " << ex.what() << '\n';
        return 1;
    }
}
