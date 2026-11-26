#include "realscript/aot_cpp/AotCpp.h"
#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/hot_reload/HotReload.h"
#include "realscript/jit/Jit.h"
#include "realscript/optimization/Optimizer.h"
#include "realscript/runtime/ParallelJob.h"
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

void testHotReloadFieldTagAndMultiHopFormerlySerializedAs() {
    // 1. Field rename matched solely via [FieldTag(N)] (no [FormerlySerializedAs]).
    const std::string tagV1 = R"RS(
module Kernel.HrTag;

public class Hero {
    [FieldTag(10)]
    public int hp = 100;

    [FieldTag(20)]
    public string heroName = "Init";

    public int ReadHp() { return hp; }
    public string ReadHeroName() { return heroName; }
}

public Hero Create() {
    var h = new Hero();
    h.hp = 432;
    h.heroName = "Knight";
    return h;
}
)RS";
    const std::string tagV2 = R"RS(
module Kernel.HrTag;

public class Hero {
    [FieldTag(20)]
    public string callsign = "Default";

    [FieldTag(10)]
    public int currentHealth = 1;

    public int ReadCurrentHealth() { return currentHealth; }
    public string ReadCallsign() { return callsign; }
}

public Hero Create() { return new Hero(); }
)RS";

    {
        auto b1 = compileAndLink({{"hr_tag_v1.rscript", tagV1}}, optimization::Level::None);
        auto b2 = compileAndLink({{"hr_tag_v2.rscript", tagV2}}, optimization::Level::None);
        runtime::EngineRuntime rt(b1.program);
        const auto createRes = rt.invoke("Kernel.HrTag::Create");
        require(createRes.succeeded, "FieldTag Create failed");
        const auto heroRef = std::get<runtime::ObjectRef>(createRes.value);
        auto root = rt.heap()->retain(runtime::Value{heroRef});

        hot_reload::ReloadOptions opts;
        opts.allowStructuralChanges = true;
        const auto plan = hot_reload::apply(rt, b2.bytecodeModules, opts);
        require(plan.compatible, "FieldTag-only structural hot reload failed");

        const auto hpRes = rt.invoke("Kernel.HrTag::Hero.ReadCurrentHealth", {runtime::Value{heroRef}});
        require(hpRes.succeeded && std::get<std::int64_t>(hpRes.value) == 432,
            "FieldTag(10) must preserve value 432 across reordered rename hp -> currentHealth");
        const auto nameRes = rt.invoke("Kernel.HrTag::Hero.ReadCallsign", {runtime::Value{heroRef}});
        require(nameRes.succeeded, "ReadCallsign failed");
        const auto nameStr = rt.heap()->stringView(std::get<runtime::ObjectRef>(nameRes.value));
        require(nameStr.has_value() && *nameStr == "Knight",
            "FieldTag(20) must preserve string 'Knight' across heroName -> callsign");
    }

    // 2. Multi-hop [FormerlySerializedAs("v1")] [FormerlySerializedAs("v2")] matching a v1 instance directly to v3.
    const std::string hopV1 = R"RS(
module Kernel.HrMultiHop;

public class Stats {
    public int v1 = 10;
}

public Stats Create() {
    var s = new Stats();
    s.v1 = 777;
    return s;
}
)RS";
    const std::string hopV2 = R"RS(
module Kernel.HrMultiHop;

public class Stats {
    [FormerlySerializedAs("v1")]
    public int v2 = 20;
}

public Stats Create() { return new Stats(); }
)RS";
    const std::string hopV3 = R"RS(
module Kernel.HrMultiHop;

public class Stats {
    [FormerlySerializedAs("v1")]
    [FormerlySerializedAs("v2")]
    public int v3 = 30;

    public int ReadV3() { return v3; }
}

public Stats Create() { return new Stats(); }
)RS";

    // Path A: v1 -> v3 direct (matches first [FormerlySerializedAs("v1")]).
    {
        auto b1 = compileAndLink({{"hop_v1.rscript", hopV1}}, optimization::Level::None);
        auto b3 = compileAndLink({{"hop_v3.rscript", hopV3}}, optimization::Level::None);
        runtime::EngineRuntime rt(b1.program);
        const auto sRef = std::get<runtime::ObjectRef>(rt.invoke("Kernel.HrMultiHop::Create").value);
        auto root = rt.heap()->retain(runtime::Value{sRef});
        hot_reload::ReloadOptions opts;
        opts.allowStructuralChanges = true;
        const auto plan = hot_reload::apply(rt, b3.bytecodeModules, opts);
        require(plan.compatible, "Multi-hop direct v1 -> v3 hot reload failed");
        const auto v3Res = rt.invoke("Kernel.HrMultiHop::Stats.ReadV3", {runtime::Value{sRef}});
        require(v3Res.succeeded && std::get<std::int64_t>(v3Res.value) == 777,
            "v1 -> v3 migration must preserve value 777 via first [FormerlySerializedAs]");
    }

    // Path B: v1 -> v2 (set v2 value = 888) -> v3 (matches second [FormerlySerializedAs("v2")]).
    {
        auto b1 = compileAndLink({{"hop_v1.rscript", hopV1}}, optimization::Level::None);
        auto b2 = compileAndLink({{"hop_v2.rscript", hopV2}}, optimization::Level::None);
        auto b3 = compileAndLink({{"hop_v3.rscript", hopV3}}, optimization::Level::None);
        runtime::EngineRuntime rt(b1.program);
        const auto sRef = std::get<runtime::ObjectRef>(rt.invoke("Kernel.HrMultiHop::Create").value);
        auto root = rt.heap()->retain(runtime::Value{sRef});

        hot_reload::ReloadOptions opts;
        opts.allowStructuralChanges = true;
        require(hot_reload::apply(rt, b2.bytecodeModules, opts).compatible, "v1 -> v2 reload failed");
        require(hot_reload::apply(rt, b3.bytecodeModules, opts).compatible, "v2 -> v3 reload failed");
        const auto v3Res = rt.invoke("Kernel.HrMultiHop::Stats.ReadV3", {runtime::Value{sRef}});
        require(v3Res.succeeded && std::get<std::int64_t>(v3Res.value) == 777,
            "v1 -> v2 -> v3 migration must preserve 777 across multi-hop [FormerlySerializedAs]");
    }
}

void testStencilJitValueLifecycleAndSharedHeapResilience() {
    auto bundle = compileAndLink({{"StencilLifecycle.rscript", R"RS(
module Kernel.StencilLifecycle;

public class ValueLifecycleAttribute { public ValueLifecycleAttribute() {} }

public static class Stats {
    public static int DisposedCount = 0;
    public static int LastToken = 0;
}

[ValueLifecycle]
public struct NativeToken {
    public int Token;

    public static NativeToken Make(int t) {
        NativeToken n = new NativeToken();
        n.Token = t;
        return n;
    }

    public NativeToken Retain() {
        return this;
    }

    public void Dispose() {
        if (Token != 0) {
            Stats.DisposedCount = Stats.DisposedCount + 1;
            Stats.LastToken = Token;
            Token = 0;
        }
    }
}

public class BoxHolder {
    public NativeToken Handle;
}

public int AllocateTransient(int token) {
    BoxHolder holder = new BoxHolder();
    holder.Handle = NativeToken.Make(token);
    return holder.Handle.Token;
}

public int ReadDisposedCount() {
    return Stats.DisposedCount;
}

public int ReadLastToken() {
    return Stats.LastToken;
}
)RS"}}, optimization::Level::Basic);

    // 1. Standalone StencilModule with default heap + explicit setHeap(customHeap).
    {
        jit::StencilJit jitEngine;
        auto compiled = jitEngine.compile(bundle.program);
        require(compiled.succeeded() && compiled.module && compiled.module->valid(),
            "StencilJit compile failed for ValueLifecycle test: " + compiled.error);

        auto customHeap = std::make_shared<runtime::ManagedHeap>();
        compiled.module->setHeap(customHeap);
        require(customHeap->hasRegisteredValueLifecycleTypes(),
            "StencilModule::setHeap must register [ValueLifecycle] types");
        require(customHeap->hasValueLifecycleSweepCallback(),
            "StencilModule::setHeap must register a sweep handler");

        auto allocRes = compiled.module->invoke(
            "Kernel.StencilLifecycle::AllocateTransient", {runtime::Value{42LL}});
        require(allocRes.succeeded && std::get<std::int64_t>(allocRes.value) == 42,
            "StencilModule AllocateTransient failed: " + allocRes.error.message);

        customHeap->collect();

        auto countRes = compiled.module->invoke("Kernel.StencilLifecycle::ReadDisposedCount");
        auto tokenRes = compiled.module->invoke("Kernel.StencilLifecycle::ReadLastToken");
        require(countRes.succeeded && std::get<std::int64_t>(countRes.value) == 1,
            "StencilModule GC sweep must invoke Dispose() once (got " +
            std::to_string(std::get<std::int64_t>(countRes.value)) + ")");
        require(tokenRes.succeeded && std::get<std::int64_t>(tokenRes.value) == 42,
            "StencilModule GC sweep must observe Token == 42");
    }

    // 2. Shared ManagedHeap between Interpreter and StencilModule: no double-dispose and
    // surviving Interpreter continues firing Dispose() after StencilModule destruction.
    {
        auto sharedHeap = std::make_shared<runtime::ManagedHeap>();
        runtime::Interpreter interp(bundle.program, sharedHeap);

        jit::StencilJit jitEngine;
        auto compiled = jitEngine.compile(bundle.program);
        require(compiled.succeeded() && compiled.module, "StencilJit compile failed");
        compiled.module->setHeap(sharedHeap);

        auto r1 = compiled.module->invoke(
            "Kernel.StencilLifecycle::AllocateTransient", {runtime::Value{71LL}});
        require(r1.succeeded, "shared heap StencilModule invoke failed");
        sharedHeap->collect();

        auto c1 = interp.invoke("Kernel.StencilLifecycle::ReadDisposedCount");
        require(c1.succeeded && std::get<std::int64_t>(c1.value) == 1,
            "shared heap GC must fire Dispose() exactly once (not double-disposed)");

        compiled.module.reset();
        require(sharedHeap->hasValueLifecycleSweepCallback(),
            "destroying StencilModule must leave outer Interpreter sweep handler intact");

        auto r2 = interp.invoke(
            "Kernel.StencilLifecycle::AllocateTransient", {runtime::Value{88LL}});
        require(r2.succeeded, "Interpreter AllocateTransient failed after StencilModule reset");
        sharedHeap->collect();

        auto c2 = interp.invoke("Kernel.StencilLifecycle::ReadDisposedCount");
        auto t2 = interp.invoke("Kernel.StencilLifecycle::ReadLastToken");
        require(c2.succeeded && std::get<std::int64_t>(c2.value) == 2,
            "Interpreter sweep handler must still fire Dispose() after StencilModule destruction");
        require(t2.succeeded && std::get<std::int64_t>(t2.value) == 88,
            "Interpreter sweep handler must see Token == 88 after StencilModule destruction");
    }
}

void testStencilJitActiveIncrementalGcProfileAndTelemetry() {
    const std::string src = R"RS(
module Kernel.StencilGcProfile;

public class Box {
    public int v;
}

public int RunGcAndAllocLoop(int n) {
    int[] arr = new int[8];
    int acc = 0;
    for (int i = 0; i < n; i = i + 1) {
        Box b = new Box();
        b.v = i * 3 + 1;
        arr[i % 8] = b.v;
        int[] scratch = new int[4];
        scratch[0] = arr[i % 8];
        acc = acc + scratch[0] + b.v;
    }
    return acc;
}
)RS";

    auto bundle = compileAndLink({{"stencil_gc_prof.rscript", src}}, optimization::Level::Basic);
    auto vmHeap = std::make_shared<runtime::ManagedHeap>();
    runtime::Interpreter interp(bundle.program, vmHeap);
    const auto vmRes = interp.invoke("Kernel.StencilGcProfile::RunGcAndAllocLoop", {std::int64_t{48}});
    require(vmRes.succeeded, "VM baseline failed: " + vmRes.error.message);

    jit::StencilJit sjit;
    jit::StencilJitOptions nativeOpts;
    nativeOpts.enableNativeCodegen = true;
    auto compiled = sjit.compile(bundle.program, nativeOpts);
    require(compiled.succeeded(), "StencilJit compile failed: " + compiled.error);
    auto heap = std::make_shared<runtime::ManagedHeap>();
    compiled.module->setHeap(heap);
#if UINTPTR_MAX == 0xffffffffffffffffULL
    require(compiled.module->functionTier("Kernel.StencilGcProfile::RunGcAndAllocLoop") ==
            jit::StencilTier::NativeMachineCode,
        "Expected RunGcAndAllocLoop to be promoted to NativeMachineCode");
#endif

    // Seed live + garbage objects and initiate an active incremental GC cycle before invoking JIT.
    std::vector<runtime::PersistentRoot> roots;
    const auto boxTypeId = semantic::stableTypeId("Kernel.StencilGcProfile::Box");
    for (int i = 0; i < 64; ++i) {
        auto obj = heap->allocateObject(boxTypeId, {runtime::Value{std::int64_t{i}}}, {});
        require(obj.has_value(), "Seed allocateObject failed");
        if (i % 4 == 0) {
            roots.push_back(heap->retain(runtime::Value{*obj}));
        }
    }
    heap->requestCollection();
    runtime::ShadowStack emptyRoots;
    (void)heap->step(emptyRoots, 1);

    auto profile = std::make_shared<runtime::ProfileCollector>();
    runtime::FrameTelemetryCollector telemetry;
    runtime::ActiveFrameTelemetryScope telemetryScope(&telemetry);
    telemetry.beginFrame(1u);

    runtime::ExecutionOptions opts;
    opts.profile = profile;
    opts.limits.gcWorkBudget = 8;

    compiled.module->resetInterpreterFallbackCount();
    const auto jitRes = compiled.module->invoke(
        "Kernel.StencilGcProfile::RunGcAndAllocLoop", {std::int64_t{48}}, opts);
    assertParity("StencilJit active incremental GC + Profile", jitRes, vmRes);
    require(compiled.module->interpreterFallbackCount() == 0u,
        "StencilJit fell back to Interpreter under active incremental GC or ProfileCollector");
    require(jitRes.statistics.gcWorkPerformed > 0u,
        "Expected StencilJit to step incremental GC safepoints under active GC");

    const auto snap = profile->snapshot();
    require(!snap.traceEvents.empty(), "Expected ProfileCollector traceEvents from StencilJit");
    require(!snap.functions.empty(), "Expected ProfileCollector function summary from StencilJit");
    bool sawFn = false;
    for (const auto& fp : snap.functions) {
        if (fp.function == "Kernel.StencilGcProfile::RunGcAndAllocLoop") {
            require(fp.calls >= 1u && fp.instructions > 0u,
                "Expected ProfileCollector calls >= 1 and instructions > 0 for StencilJit");
            sawFn = true;
        }
    }
    require(sawFn, "Missing StencilJit function entry in ProfileCollector snapshot");

    const auto frameSample = telemetry.endFrame(heap.get());
    require(frameSample.totalInstructions > 0u && frameSample.hotFunctionCount >= 1u,
        "Expected FrameTelemetryCollector to record StencilJit instructions");
    require(frameSample.boxedHeapLiveBytes > 0u,
        "Expected live heap bytes in FrameTelemetryCollector after StencilJit run");
}

void testStencilJitParallelBatchExecution() {
    auto bundle = compileAndLink({{"StencilParallel.rscript", R"RS(
module Kernel.StencilParallel;

public int StepAndScore(int index, int[] dst, int[] src, int scale) {
    int v = src[index] * scale + 3;
    dst[index] = v;
    return v;
}

public void RangeTransform(int start, int count, int[] dst, int[] src, int bias) {
    for (int off = 0; off < count; off = off + 1) {
        int idx = start + off;
        dst[idx] = src[idx] * 2 + bias;
    }
}
)RS"}});

    constexpr std::size_t kCount = 128;
    auto makeInputs = [&](runtime::ManagedHeap& h) {
        const auto arrTypeId = semantic::stableTypeId("int[]");
        auto src = h.allocateTypedArray(
            arrTypeId, semantic::PrimitiveType::Int, 0, kCount, runtime::Value{std::int64_t{0}});
        auto dst = h.allocateTypedArray(
            arrTypeId, semantic::PrimitiveType::Int, 0, kCount, runtime::Value{std::int64_t{0}});
        require(src.has_value() && dst.has_value(), "Packed int array allocation failed");
        for (std::size_t i = 0; i < kCount; ++i) {
            require(h.arraySet(*src, i, runtime::Value{static_cast<std::int64_t>(i + 1)}),
                "arraySet failed");
        }
        return std::pair<runtime::ObjectRef, runtime::ObjectRef>{*src, *dst};
    };

    // Baseline VM execution without StencilJit attached
    auto vmHeap = std::make_shared<runtime::ManagedHeap>();
    auto [vmSrc, vmDst] = makeInputs(*vmHeap);
    runtime::ParallelJobScheduler scheduler(4);
    runtime::ParallelJobConfig elemCfg;
    elemCfg.workerCount = 4;
    elemCfg.batchSize = 16;
    elemCfg.reduceOp = runtime::ParallelReduceOp::Sum;

    const auto vmElemRes = scheduler.dispatchFor(
        bundle.program,
        vmHeap,
        "Kernel.StencilParallel::StepAndScore",
        kCount,
        {runtime::Value{vmDst}, runtime::Value{vmSrc}, runtime::Value{std::int64_t{5}}},
        elemCfg);
    require(vmElemRes.succeeded && vmElemRes.totalItemsProcessed == kCount,
        "Baseline VM parallel dispatchFor failed: " + vmElemRes.error.message);

    auto [vmRangeSrc, vmRangeDst] = makeInputs(*vmHeap);
    runtime::ParallelJobConfig rangeCfg;
    rangeCfg.workerCount = 4;
    rangeCfg.batchSize = 32;
    const auto vmRangeRes = scheduler.dispatchRange(
        bundle.program,
        vmHeap,
        "Kernel.StencilParallel::RangeTransform",
        kCount,
        {runtime::Value{vmRangeDst}, runtime::Value{vmRangeSrc}, runtime::Value{std::int64_t{9}}},
        rangeCfg);
    require(vmRangeRes.succeeded && vmRangeRes.totalItemsProcessed == kCount,
        "Baseline VM parallel dispatchRange failed: " + vmRangeRes.error.message);

    // StencilJit native batch execution attached to ProgramImage
    jit::StencilJit sjit;
    jit::StencilJitOptions opts;
    opts.enableNativeCodegen = true;
    opts.hotnessThreshold = 0;
    auto compiled = sjit.compile(bundle.program, opts);
    require(compiled.succeeded() && compiled.module && compiled.module->valid(),
        "StencilJit compile failed for parallel test: " + compiled.error);
    compiled.module->resetInterpreterFallbackCount();
    compiled.module->resetParallelBatchExecutionCount();

    auto jitHeap = std::make_shared<runtime::ManagedHeap>();
    auto [jitSrc, jitDst] = makeInputs(*jitHeap);
    const auto jitElemRes = scheduler.dispatchFor(
        bundle.program,
        jitHeap,
        "Kernel.StencilParallel::StepAndScore",
        kCount,
        {runtime::Value{jitDst}, runtime::Value{jitSrc}, runtime::Value{std::int64_t{5}}},
        elemCfg);
    require(jitElemRes.succeeded && jitElemRes.totalItemsProcessed == kCount,
        "StencilJit parallel dispatchFor failed: " + jitElemRes.error.message);
    require(jitElemRes.usedNativeBatchKernel,
        "Expected StencilJit parallel dispatchFor to use native batch kernel");
    require(compiled.module->parallelBatchExecutionCount() > 0,
        "Expected non-zero StencilJit parallelBatchExecutionCount after dispatchFor");
    require(compiled.module->interpreterFallbackCount() == 0,
        "Expected zero interpreter fallbacks during StencilJit parallel dispatchFor");
    require(std::get<std::int64_t>(jitElemRes.value) == std::get<std::int64_t>(vmElemRes.value),
        "StencilJit parallel reduction mismatch against VM");

    compiled.module->resetParallelBatchExecutionCount();
    auto [jitRangeSrc, jitRangeDst] = makeInputs(*jitHeap);
    const auto jitRangeRes = scheduler.dispatchRange(
        bundle.program,
        jitHeap,
        "Kernel.StencilParallel::RangeTransform",
        kCount,
        {runtime::Value{jitRangeDst}, runtime::Value{jitRangeSrc}, runtime::Value{std::int64_t{9}}},
        rangeCfg);
    require(jitRangeRes.succeeded && jitRangeRes.totalItemsProcessed == kCount,
        "StencilJit parallel dispatchRange failed: " + jitRangeRes.error.message);
    require(jitRangeRes.usedNativeBatchKernel,
        "Expected StencilJit parallel dispatchRange to use native batch kernel");
    require(compiled.module->parallelBatchExecutionCount() > 0,
        "Expected non-zero StencilJit parallelBatchExecutionCount after dispatchRange");
    require(compiled.module->interpreterFallbackCount() == 0,
        "Expected zero interpreter fallbacks during StencilJit parallel dispatchRange");

    for (std::size_t i = 0; i < kCount; ++i) {
        const auto a = jitHeap->arrayGet(jitDst, i);
        const auto b = vmHeap->arrayGet(vmDst, i);
        require(a && b && std::get<std::int64_t>(*a) == std::get<std::int64_t>(*b),
            "Element kernel output parity mismatch against VM");
        const auto ra = jitHeap->arrayGet(jitRangeDst, i);
        const auto rb = vmHeap->arrayGet(vmRangeDst, i);
        require(ra && rb && std::get<std::int64_t>(*ra) == std::get<std::int64_t>(*rb),
            "Range kernel output parity mismatch against VM");
    }
}

void testStencilJitAllocAttributionAndFunctionDiagnostics() {
    const std::string src = R"RS(
module Kernel.StencilDiag;

public class DiagFault {
    public string msg;
    public DiagFault(string m) { msg = m; }
}

public class Pair {
    public int a = 0;
    public int b = 0;
}

public int FastAdd(int x, int y) {
    return x * 3 + y;
}

public int AllocWork(int n) {
    Pair p = new Pair();
    p.a = n;
    int[] arr = new int[4];
    arr[0] = p.a + 1;
    return arr[0];
}

public int TryCatchFallback(int x) {
    int acc = 0;
    try {
        if (x > 0) {
            throw new DiagFault("err");
        }
    } catch (DiagFault ex) {
        acc = ex.msg.Length;
    }
    return acc + x;
}
)RS";

    auto bundle = compileAndLink({{"stencil_diag.rscript", src}}, optimization::Level::Basic);
    jit::StencilJit sjit;
    jit::StencilJitOptions opts;
    opts.enableNativeCodegen = true;
    auto compiled = sjit.compile(bundle.program, opts);
    require(compiled.succeeded(), "StencilJit compile failed: " + compiled.error);

    auto heap = std::make_shared<runtime::ManagedHeap>();
    compiled.module->setHeap(heap);

    runtime::FrameTelemetryCollector telemetry;
    runtime::ActiveFrameTelemetryScope telemetryScope(&telemetry);
    telemetry.beginFrame(1u);

    const auto allocRes = compiled.module->invoke(
        "Kernel.StencilDiag::AllocWork", {runtime::Value{std::int64_t{5}}});
    require(allocRes.succeeded && std::get<std::int64_t>(allocRes.value) == 6,
        "StencilJit AllocWork execution failed: " + allocRes.error.message);

    const auto sample = telemetry.endFrame(heap.get());
    require(sample.frameAllocCount >= 2u, "Expected StencilJit frameAllocCount >= 2");
    require(sample.frameAllocBytes > 0u, "Expected StencilJit frameAllocBytes > 0");
    require(sample.hotAllocationCount >= 1u, "Expected StencilJit hotAllocationCount >= 1");

    const auto allocSymId = bundle.program->findFunction("Kernel.StencilDiag::AllocWork");
    require(allocSymId.has_value(), "Missing AllocWork symbolId");
    bool attributedToAllocWork = false;
    for (std::uint8_t i = 0; i < sample.hotAllocationCount; ++i) {
        if (sample.topAllocations[i].symbolId == *allocSymId &&
            sample.topAllocations[i].allocationCount > 0u) {
            attributedToAllocWork = true;
            break;
        }
    }
    require(attributedToAllocWork, "Expected StencilJit allocation attributed to AllocWork");

    const auto fastRes = compiled.module->invoke(
        "Kernel.StencilDiag::FastAdd",
        {runtime::Value{std::int64_t{4}}, runtime::Value{std::int64_t{5}}});
    require(fastRes.succeeded && std::get<std::int64_t>(fastRes.value) == 17,
        "FastAdd execution failed");

    const auto tryRes = compiled.module->invoke(
        "Kernel.StencilDiag::TryCatchFallback", {runtime::Value{std::int64_t{2}}});
    require(tryRes.succeeded && std::get<std::int64_t>(tryRes.value) == 5,
        "TryCatchFallback execution failed: " + tryRes.error.message);

    const auto diags = compiled.module->functionDiagnostics();
    bool sawFast = false;
    bool sawTryFallback = false;
    for (const auto& d : diags) {
        if (d.qualifiedName == "Kernel.StencilDiag::FastAdd") {
            sawFast = true;
            require(d.eligibleForThreaded, "FastAdd must be eligible for threaded execution");
#if UINTPTR_MAX == 0xffffffffffffffffULL
            require(d.eligibleForNative, "FastAdd must be eligible for native StencilJit");
            require(d.tier == jit::StencilTier::NativeMachineCode,
                "FastAdd expected NativeMachineCode tier");
            require(d.fallbackReason.empty(), "FastAdd should have empty fallbackReason");
#endif
        } else if (d.qualifiedName == "Kernel.StencilDiag::TryCatchFallback") {
            sawTryFallback = true;
            require(!d.eligibleForNative, "TryCatchFallback must not be native-eligible");
            require(!d.fallbackReason.empty(), "TryCatchFallback must report fallbackReason");
            require(d.tier == jit::StencilTier::InterpreterFallback,
                "TryCatchFallback expected InterpreterFallback tier");
        }
    }
    require(sawFast && sawTryFallback, "Missing expected functions in StencilModule::functionDiagnostics");
}

void testWaveRegressionStencilJitPolicyAndFastOps() {
    const std::string src = R"RS(
module Kernel.StencilWave;

public class Box {
    public int v = 5;
}

public int NegCheck(int x) {
    return -x;
}

public int ConvAndUInt(double d, int a, int b) {
    uint ua = (uint)a;
    uint ub = (uint)b;
    uint diff = (ua + ub) - (uint)2;
    return (int)d + (int)diff;
}

public int LeafBump(int x) {
    return x + 4;
}

public int PolicyTarget(Box b, int delta) {
    int bump = LeafBump(delta);
    b.v = b.v + bump;
    return b.v;
}
)RS";

    auto bundle = compileAndLink({{"stencil_wave.rscript", src}}, optimization::Level::Basic);
    auto heap = std::make_shared<runtime::ManagedHeap>();

    jit::StencilJit sjit;
    jit::StencilJitOptions nativeOpts;
    nativeOpts.enableNativeCodegen = true;
    auto nativeCompiled = sjit.compile(bundle.program, nativeOpts);
    require(nativeCompiled.succeeded(), "StencilJit native compile failed: " + nativeCompiled.error);
    nativeCompiled.module->setHeap(heap);

    jit::StencilJitOptions threadedOpts;
    threadedOpts.enableNativeCodegen = false;
    auto threadedCompiled = sjit.compile(bundle.program, threadedOpts);
    require(threadedCompiled.succeeded(), "StencilJit threaded compile failed");
    threadedCompiled.module->setHeap(heap);

    runtime::Interpreter interp(bundle.program);
    interp.setHeap(heap);

    const std::int64_t iMin = std::numeric_limits<std::int32_t>::min();
    const auto expNegOk = interp.invoke("Kernel.StencilWave::NegCheck", {runtime::Value{std::int64_t{-19}}});
    const auto expNegOv = interp.invoke("Kernel.StencilWave::NegCheck", {runtime::Value{iMin}});
    const auto expConv = interp.invoke("Kernel.StencilWave::ConvAndUInt",
        {runtime::Value{12.75}, runtime::Value{std::int64_t{10}}, runtime::Value{std::int64_t{5}}});

    require(expNegOk.succeeded && std::get<std::int64_t>(expNegOk.value) == 19,
        "Interpreter NegCheck(-19) mismatch");
    require(!expNegOv.succeeded, "Interpreter NegCheck(INT32_MIN) should fail overflow");
    require(expConv.succeeded && std::get<std::int64_t>(expConv.value) == 25,
        "Interpreter ConvAndUInt mismatch");

    assertParity("StencilNative NegCheck ok",
        nativeCompiled.module->invoke("Kernel.StencilWave::NegCheck", {runtime::Value{std::int64_t{-19}}}),
        expNegOk);
    assertParity("StencilNative NegCheck overflow",
        nativeCompiled.module->invoke("Kernel.StencilWave::NegCheck", {runtime::Value{iMin}}),
        expNegOv);
    assertParity("StencilNative ConvAndUInt",
        nativeCompiled.module->invoke("Kernel.StencilWave::ConvAndUInt",
            {runtime::Value{12.75}, runtime::Value{std::int64_t{10}}, runtime::Value{std::int64_t{5}}}),
        expConv);
    assertParity("StencilThreaded ConvAndUInt",
        threadedCompiled.module->invoke("Kernel.StencilWave::ConvAndUInt",
            {runtime::Value{12.75}, runtime::Value{std::int64_t{10}}, runtime::Value{std::int64_t{5}}}),
        expConv);
    assertParity("StencilSimArm64 ConvAndUInt",
        nativeCompiled.module->invokeSimulatedArm64("Kernel.StencilWave::ConvAndUInt",
            {runtime::Value{12.75}, runtime::Value{std::int64_t{10}}, runtime::Value{std::int64_t{5}}}),
        expConv);

    struct JPolicy final : public runtime::IExecutionPolicy {
        semantic::SymbolId bumpSymId = 0;
        bool denyWrite = false;
        bool intercept = false;
        std::size_t writeChecks = 0;
        std::size_t intercepts = 0;

        bool checkFieldWrite(
            const runtime::ManagedHeap*,
            const runtime::ManagedHeap*,
            runtime::ObjectRef,
            semantic::SymbolId,
            std::size_t,
            std::string_view,
            const runtime::Value&,
            runtime::RuntimeError& err) const override {
            const_cast<JPolicy*>(this)->writeChecks += 1u;
            if (denyWrite) {
                err.code = runtime::ErrorCode::CapabilityDenied;
                err.message = "stencil policy denied field write";
                return false;
            }
            return true;
        }

        bool interceptCall(
            semantic::SymbolId fnId,
            runtime::ValueSpan arguments,
            bool& outIntercepted,
            runtime::Value& outResult,
            runtime::RuntimeError&) override {
            if (intercept && fnId == bumpSymId) {
                ++intercepts;
                outIntercepted = true;
                const std::int64_t x = (!arguments.empty() && std::holds_alternative<std::int64_t>(arguments[0]))
                    ? std::get<std::int64_t>(arguments[0])
                    : 0;
                outResult = runtime::Value{x + 100};
                return true;
            }
            outIntercepted = false;
            return true;
        }
    };

    const auto boxTid = semantic::stableTypeId("Kernel.StencilWave::Box");
    auto boxObj = heap->allocateObject(boxTid, {runtime::Value{std::int64_t{5}}}, {});
    require(boxObj.has_value(), "Box allocation failed");
    auto pinBox = heap->retain(runtime::Value{*boxObj});

    JPolicy policy;
    const auto bumpSym = bundle.program->findFunction("Kernel.StencilWave::LeafBump");
    require(bumpSym.has_value(), "Missing LeafBump symbol");
    policy.bumpSymId = *bumpSym;
    runtime::ExecutionOptions polOpts;
    polOpts.executionPolicy = &policy;

    policy.denyWrite = true;
    const auto denyRes = nativeCompiled.module->invoke(
        "Kernel.StencilWave::PolicyTarget",
        {runtime::Value{*boxObj}, runtime::Value{std::int64_t{3}}},
        polOpts);
    require(!denyRes.succeeded &&
            denyRes.error.code == runtime::ErrorCode::CapabilityDenied &&
            policy.writeChecks >= 1u &&
            heap->fieldGet(*boxObj, 0u) == std::optional<runtime::Value>{runtime::Value{std::int64_t{5}}},
        "StencilJit must enforce IExecutionPolicy::checkFieldWrite on fast path");

    policy.denyWrite = false;
    policy.intercept = true;
    const auto intRes = nativeCompiled.module->invoke(
        "Kernel.StencilWave::PolicyTarget",
        {runtime::Value{*boxObj}, runtime::Value{std::int64_t{3}}},
        polOpts);
    require(intRes.succeeded &&
            std::get<std::int64_t>(intRes.value) == 108 &&
            policy.intercepts >= 1u,
        "StencilJit must enforce IExecutionPolicy::interceptCall when policy attached");
}

void testAuditRegressionStencilStructsPoolDomainEnumAndCompactHr() {
    // A. Struct-returning CallDirect, StaticGet struct, Enum -> int, and ValuePoolDomain GC survival.
    {
        const std::string src = R"RS(
module Kernel.StencilAudit;
import RealScript.Math;

public enum Mode {
    Idle = 0,
    Active = 7,
    Turbo = 42
}

public struct Pair {
    public int a;
    public int b;
    public Pair(int x, int y) {
        a = x;
        b = y;
    }
}

public class Config {
    public static Pair BasePair = new Pair(100, 25);
}

public Pair MakeOffset(int x, int y) {
    return new Pair(x + 1, y + 2);
}

public int EvalStructAndEnum(int x, Mode m) {
    Pair p = MakeOffset(x, x * 2);
    Pair b = Config.BasePair;
    return p.a + p.b + b.a + b.b + (m == Mode.Turbo ? 42 : 0);
}

public int EvalPoolDomainQuat(int seed) {
    Quatf q = new Quatf((float)seed, -1.5f, 2.25f, 0.1234567f);
    Quatf[] arr = new Quatf[4];
    arr[0] = q;
    return (int)arr[0].x + 100;
}
)RS";

        auto bundle = compileAndLink({{"stencil_audit.rscript", src}}, optimization::Level::Basic);
        auto domain = runtime::ValuePoolDomain::create();
        require(domain != nullptr, "ValuePoolDomain::create failed");
        runtime::HeapConfig hcfg;
        hcfg.valuePoolDomain = domain;
        auto heap = std::make_shared<runtime::ManagedHeap>(hcfg);

        jit::StencilJit sjit;
        jit::StencilJitOptions nativeOpts;
        nativeOpts.enableNativeCodegen = true;
        auto nativeCompiled = sjit.compile(bundle.program, nativeOpts);
        require(nativeCompiled.succeeded(), "Audit StencilJit native compile failed: " + nativeCompiled.error);
        nativeCompiled.module->setHeap(heap);

        jit::StencilJitOptions threadedOpts;
        threadedOpts.enableNativeCodegen = false;
        auto threadedCompiled = sjit.compile(bundle.program, threadedOpts);
        require(threadedCompiled.succeeded(), "Audit StencilJit threaded compile failed");
        threadedCompiled.module->setHeap(heap);

#if UINTPTR_MAX == 0xffffffffffffffffULL
        require(nativeCompiled.module->functionTier("Kernel.StencilAudit::EvalStructAndEnum") ==
                jit::StencilTier::NativeMachineCode,
            "Expected EvalStructAndEnum to run as NativeMachineCode (no struct-call fallback)");
#endif

        runtime::Interpreter interp(bundle.program, heap);
        const std::vector<runtime::Value> evalArgs{
            runtime::Value{std::int64_t{10}},
            runtime::Value{runtime::EnumValue{semantic::stableTypeId("Kernel.StencilAudit::Mode"), 42}},
        };
        const auto expEval = interp.invoke("Kernel.StencilAudit::EvalStructAndEnum", evalArgs);
        require(expEval.succeeded && std::get<std::int64_t>(expEval.value) == 200,
            "Interpreter EvalStructAndEnum baseline mismatch: " + expEval.error.message);

        assertParity("StencilAudit Native EvalStructAndEnum",
            nativeCompiled.module->invoke("Kernel.StencilAudit::EvalStructAndEnum", evalArgs),
            expEval);
        assertParity("StencilAudit Threaded EvalStructAndEnum",
            threadedCompiled.module->invoke("Kernel.StencilAudit::EvalStructAndEnum", evalArgs),
            expEval);
        assertParity("StencilAudit SimArm64 EvalStructAndEnum",
            nativeCompiled.module->invokeSimulatedArm64("Kernel.StencilAudit::EvalStructAndEnum", evalArgs),
            expEval);

        const auto expQuat = interp.invoke("Kernel.StencilAudit::EvalPoolDomainQuat", {runtime::Value{std::int64_t{17}}});
        require(expQuat.succeeded && std::get<std::int64_t>(expQuat.value) == 117,
            "Interpreter EvalPoolDomainQuat mismatch");
        assertParity("StencilAudit Native EvalPoolDomainQuat",
            nativeCompiled.module->invoke("Kernel.StencilAudit::EvalPoolDomainQuat", {runtime::Value{std::int64_t{17}}}),
            expQuat);
        heap->collect(runtime::RootSet{});
        assertParity("StencilAudit Native EvalPoolDomainQuat post-GC",
            nativeCompiled.module->invoke("Kernel.StencilAudit::EvalPoolDomainQuat", {runtime::Value{std::int64_t{17}}}),
            expQuat);
    }

    // A2. Direct bytecode ConvertNumeric(Enum -> Int) in StencilJit (Native, Threaded, SimArm64).
    {
        const semantic::SymbolId enumTid = semantic::stableTypeId("Kernel.BcEnum::AuditBcEnum");
        bytecode::Module mod;
        mod.name = "Kernel.BcEnum";
        mod.version = bytecode::Version{};

        semantic::TypeSymbol enumType;
        enumType.id = enumTid;
        enumType.kind = semantic::TypeKind::Enum;
        enumType.moduleName = "Kernel.BcEnum";
        enumType.name = "AuditBcEnum";
        mod.types.push_back(enumType);

        bytecode::Function fn;
        fn.symbolId = semantic::stableTypeId("Kernel.BcEnum::EnumToInt");
        fn.accessibility = semantic::Accessibility::Public;
        fn.name = "EnumToInt";
        fn.returnType = semantic::PrimitiveType::Int;
        fn.parameterTypes = {semantic::PrimitiveType::Enum};
        fn.parameterTypeIds = {enumTid};
        fn.registerTypes = {semantic::PrimitiveType::Enum, semantic::PrimitiveType::Int};
        fn.registerTypeIds = {enumTid, 0u};

        bytecode::BasicBlock bb;
        bb.id = 0;

        bytecode::Instruction p0;
        p0.opcode = bytecode::Opcode::LoadParameter;
        p0.result = 0;
        p0.integerImmediate = 0;

        bytecode::Instruction c1;
        c1.opcode = bytecode::Opcode::ConvertNumeric;
        c1.result = 1;
        c1.operands.push_back(0);
        c1.checkedArithmetic = true;

        bb.instructions.push_back(p0);
        bb.instructions.push_back(c1);
        bb.terminator.kind = bytecode::TerminatorKind::ReturnValue;
        bb.terminator.value = 1;
        fn.blocks.push_back(std::move(bb));
        mod.functions.push_back(std::move(fn));

        runtime::RuntimeError linkErr;
        auto linked = runtime::ProgramImage::link({std::move(mod)}, linkErr, false);
        require(linked.has_value(), "Link EnumToInt failed: " + linkErr.message);
        auto enumProg = std::make_shared<const runtime::ProgramImage>(std::move(*linked));

        jit::StencilJit sjit;
        jit::StencilJitOptions nativeOpts;
        nativeOpts.enableNativeCodegen = true;
        auto nativeEnum = sjit.compile(enumProg, nativeOpts);
        require(nativeEnum.succeeded(), "EnumToInt native compile failed: " + nativeEnum.error);

        jit::StencilJitOptions threadedOpts;
        threadedOpts.enableNativeCodegen = false;
        auto threadedEnum = sjit.compile(enumProg, threadedOpts);
        require(threadedEnum.succeeded(), "EnumToInt threaded compile failed");

        runtime::Interpreter interp(enumProg);
        const std::vector<runtime::Value> enumArgs{runtime::Value{runtime::EnumValue{enumTid, 42}}};
        const auto exp = interp.invoke("Kernel.BcEnum::EnumToInt", enumArgs);
        require(exp.succeeded && std::get<std::int64_t>(exp.value) == 42,
            "Interpreter EnumToInt baseline mismatch: " + exp.error.message);

        assertParity("StencilAudit Native EnumToInt",
            nativeEnum.module->invoke("Kernel.BcEnum::EnumToInt", enumArgs),
            exp);
        assertParity("StencilAudit Threaded EnumToInt",
            threadedEnum.module->invoke("Kernel.BcEnum::EnumToInt", enumArgs),
            exp);
        assertParity("StencilAudit SimArm64 EnumToInt",
            nativeEnum.module->invokeSimulatedArm64("Kernel.BcEnum::EnumToInt", enumArgs),
            exp);
    }

    // B. ProgramImage::compactForRuntime() + hot_reload::prepare with imported cross-module types.
    {
        const std::string modA = R"RS(
module Kernel.HrCompactA;
public class SharedItem {
    public int x = 1;
}
)RS";
        const std::string modBV1 = R"RS(
module Kernel.HrCompactB;
import Kernel.HrCompactA;
public class Worker {
    public int Run(SharedItem s) { return s.x + 10; }
}
)RS";
        const std::string modBV2 = R"RS(
module Kernel.HrCompactB;
import Kernel.HrCompactA;
public class Worker {
    public int Run(SharedItem s) { return s.x + 20; }
}
)RS";
        auto v1 = compileAndLink({{"a.rscript", modA}, {"b.rscript", modBV1}}, optimization::Level::Basic);
        auto v2 = compileAndLink({{"a.rscript", modA}, {"b.rscript", modBV2}}, optimization::Level::Basic);

        runtime::ProgramImage compacted(*v1.program);
        compacted.compactForRuntime();

        const auto plan = hot_reload::prepare(compacted, v2.bytecodeModules);
        require(plan.compatible && !plan.changedFunctions.empty(),
            "hot_reload::prepare failed after compactForRuntime() on imported module types");
    }
}

void testHybridRecordStencilJitAndHotReloadMigration() {
    const std::string srcV1 = R"RS(
module Kernel.HybridJitHr;

public class Payload {
    public int Code;
}

public class RichNode {
    public string Title;
    public object Tag;
    public int Width;
    public int Height;
    public float Scale;
    public double Opacity;
    public bool Enabled;
    public byte Mode;
    public long Revision;
    public short ZIndex;
}

public RichNode CreateNode(string title, int w, int h) {
    RichNode n = new RichNode();
    n.Title = title;
    Payload p = new Payload();
    p.Code = 55;
    n.Tag = p;
    n.Width = w;
    n.Height = h;
    n.Scale = 1.25f;
    n.Opacity = 0.875;
    n.Enabled = true;
    n.Mode = 3;
    n.Revision = 9001l;
    n.ZIndex = (short)(-4);
    return n;
}

public int StepNode(RichNode n, int delta) {
    n.Width = n.Width + delta;
    int bonus = 0;
    Payload p = n.Tag as Payload;
    if (p != null) {
        bonus = p.Code;
    }
    if (n.Enabled && n.Opacity > 0.5 && n.Scale > 1.0f && n.Mode == 3 && n.ZIndex == (short)(-4)) {
        return n.Width + n.Height + bonus;
    }
    return -1;
}
)RS";

    const std::string srcV2 = R"RS(
module Kernel.HybridJitHr;

public class Payload {
    public int Code;
}

public class RichNode {
    public string Title;
    public object Tag;
    public int Width;
    public int Height;
    public double Scale;
    public double Opacity;
    public bool Enabled;
    public byte Mode;
    public long Revision;
    public short ZIndex;
    public int AddedField = 7;
}

public RichNode CreateNode(string title, int w, int h) {
    RichNode n = new RichNode();
    n.Title = title;
    return n;
}

public int StepNode(RichNode n, int delta) {
    Payload p = n.Tag as Payload;
    if (p != null) {
        return n.Width + n.Height + p.Code + n.AddedField;
    }
    return -1;
}
)RS";

    auto v1 = compileAndLink({{"hybrid_jit_hr.rscript", srcV1}}, optimization::Level::Basic);
    auto v2 = compileAndLink({{"hybrid_jit_hr.rscript", srcV2}}, optimization::Level::Basic);

    // 1. StencilJit Tier-2 parity on HybridRecord allocation + field read/write
    jit::StencilJit sjit;
    jit::StencilJitOptions jitOpts;
    jitOpts.enableNativeCodegen = true;
    auto jitCompiled = sjit.compile(v1.program, jitOpts);
    require(jitCompiled.succeeded(), "StencilJit compile failed: " + jitCompiled.error);

    auto sharedHeap = std::make_shared<runtime::ManagedHeap>();
    jitCompiled.module->setHeap(sharedHeap);
    runtime::Interpreter interpV1(v1.program, sharedHeap);

    const auto nRes = jitCompiled.module->invoke(
        "Kernel.HybridJitHr::CreateNode",
        {runtime::Value{"card"}, std::int64_t{100}, std::int64_t{40}});
    require(nRes.succeeded && nRes.value.rawTag() == runtime::ValueTag::ObjectRef,
        "JIT CreateNode failed: " + nRes.error.message);
    const auto nodeRef = nRes.value.asObjectRefUnchecked();
    const auto nodeRoot = sharedHeap->retain(nodeRef);
    require(sharedHeap->isHybridRecordObject(nodeRef),
        "Expected JIT-allocated RichNode to use SlotStorage::HybridRecord");

    const auto jitStep = jitCompiled.module->invoke(
        "Kernel.HybridJitHr::StepNode",
        {runtime::Value{nodeRef}, std::int64_t{10}});
    std::int64_t jitStepOut = 0;
    require(jitStep.succeeded && jitStep.value.tryGetInt64(jitStepOut) && jitStepOut == (110 + 40 + 55),
        "JIT StepNode on HybridRecord failed");

    const auto vmStep = interpV1.invoke(
        "Kernel.HybridJitHr::StepNode",
        {runtime::Value{nodeRef}, std::int64_t{5}});
    std::int64_t vmStepOut = 0;
    require(vmStep.succeeded && vmStep.value.tryGetInt64(vmStepOut) && vmStepOut == (115 + 40 + 55),
        "VM StepNode parity after JIT on HybridRecord failed");

    // 2. HotReload migration of live HybridRecord instance
    hot_reload::ReloadOptions hrOpts;
    hrOpts.allowStructuralChanges = true;
    const auto plan = hot_reload::prepare(*v1.program, v2.bytecodeModules, hrOpts);
    require(plan.compatible && !plan.typeMigrations.empty(),
        "HotReload plan failed for HybridRecord class update: " +
            (plan.issues.empty() ? std::string("none") : plan.issues.front().message));
    runtime::LayoutMigrationStats hrStats{};
    runtime::RuntimeError hrErr;
    require(sharedHeap->migrateTypeLayouts(plan.typeMigrations, &hrStats, &hrErr),
        "migrateTypeLayouts failed on HybridRecord: " + hrErr.message);
    require(hrStats.objectsMigrated >= 1u, "Expected HybridRecord instance to be migrated");

    runtime::Interpreter interpV2(v2.program, sharedHeap);
    const auto afterHr = interpV2.invoke(
        "Kernel.HybridJitHr::StepNode",
        {runtime::Value{nodeRef}, std::int64_t{0}});
    std::int64_t afterHrOut = 0;
    require(afterHr.succeeded && afterHr.value.tryGetInt64(afterHrOut) && afterHrOut == (115 + 40 + 55 + 7),
        "Post-HotReload StepNode on migrated HybridRecord failed");
    (void)nodeRoot;
}

} // namespace

int main() {
    try {
        std::vector<std::pair<const char*, void (*)()>> tests = {
            {"StencilJit Tier-1/Tier-2/Simulated ARM64 & budget parity", testStencilJitTiersAndSimulatedArm64Parity},
            {"ToolchainJit shell-free hardening & AOT sync header diet", testToolchainJitShellFreeAndHeaderDiet},
            {"HotReload -O2 inline invalidation, structural migration & active sequence", testHotReloadInlineStructuralAndSequences},
            {"HotReload Pillar 6: [FieldTag(N)] rename & multi-hop [FormerlySerializedAs]", testHotReloadFieldTagAndMultiHopFormerlySerializedAs},
            {"StencilJit [ValueLifecycle] sweep registration & shared-heap resilience", testStencilJitValueLifecycleAndSharedHeapResilience},
            {"StencilJit active incremental GC, ProfileCollector & FrameTelemetryCollector (zero fallback)", testStencilJitActiveIncrementalGcProfileAndTelemetry},
            {"StencilJit native ParallelJobScheduler element reduction & range execution", testStencilJitParallelBatchExecution},
            {"StencilJit GC.Alloc attribution & per-function diagnostics", testStencilJitAllocAttributionAndFunctionDiagnostics},
            {"Wave 2 regression: StencilJit IExecutionPolicy & NegateIntChecked/ConvertNumeric/BinaryUInt", testWaveRegressionStencilJitPolicyAndFastOps},
            {"Audit regression: StencilJit struct calls, ValuePoolDomain, Enum->Int & compactForRuntime HotReload", testAuditRegressionStencilStructsPoolDomainEnumAndCompactHr},
            {"Phase 2.2: HybridRecord StencilJit parity & HotReload migration", testHybridRecordStencilJitAndHotReloadMigration},
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
