#include "realscript/aot_cpp/AotCpp.h"
#include "realscript/aot_cpp/AotRuntime.h"
#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/compiler/ModuleBuildCache.h"
#include "realscript/debug/Debugger.h"
#include "realscript/jit/Jit.h"
#include "realscript/optimization/Optimizer.h"
#include "realscript/runtime/DebugHook.h"
#include "realscript/runtime/ParallelJob.h"
#include "realscript/runtime/Runtime.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <sys/statvfs.h>
#endif

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

struct CompiledVmBundle {
    std::shared_ptr<const runtime::ProgramImage> program;
    std::vector<mir::Module> mirModules;
};

CompiledVmBundle compileProgramSource(
    const std::string& sourceText,
    const std::string& fileName = "p2_audit_test.rscript",
    optimization::Level optLevel = optimization::Level::Basic) {
    compiler::Compilation comp({compiler::SourceFile{fileName, sourceText}});
    auto build = comp.build();
    if (build.diagnostics.hasErrors()) {
        std::ostringstream diagStream;
        for (const auto& d : build.diagnostics.items()) {
            diagStream << d.code << ": " << d.message << "\n";
        }
        throw std::runtime_error("Compilation failed:\n" + diagStream.str());
    }

    std::vector<mir::Module> mirMods = std::move(build.modules);
    if (optLevel != optimization::Level::None) {
        diagnostics::DiagnosticBag optDiags;
        optimization::Options optOpts;
        optOpts.level = optLevel;
        optimization::Optimizer optimizer;
        auto optRes = optimizer.optimize(std::move(mirMods), optDiags, optOpts);
        require(!optDiags.hasErrors(), "MIR optimization failed");
        mirMods = std::move(optRes.modules);
    }

    bytecode::Lowerer lowerer;
    std::vector<bytecode::Module> bcMods;
    bcMods.reserve(mirMods.size());
    for (const auto& m : mirMods) {
        bcMods.push_back(lowerer.lower(m));
    }

    runtime::RuntimeError linkErr;
    auto linked = runtime::ProgramImage::link(std::move(bcMods), linkErr);
    require(linked.has_value(), "ProgramImage link failed: " + linkErr.message);
    return CompiledVmBundle{
        std::make_shared<const runtime::ProgramImage>(std::move(*linked)),
        std::move(mirMods),
    };
}

semantic::SymbolId findFunctionId(
    const runtime::ProgramImage& program,
    std::string_view shortOrQualifiedName) {
    for (const auto& mod : program.modules()) {
        for (const auto& fn : mod.functions) {
            if (fn.name == shortOrQualifiedName ||
                (mod.name + "::" + fn.name) == shortOrQualifiedName) {
                return fn.symbolId;
            }
        }
    }
    throw std::runtime_error("function not found: " + std::string(shortOrQualifiedName));
}

// ============================================================================
// AUD-05: ParallelJob Flat-Frame >4-Field & Recursive Nested Pure Value Structs
// ============================================================================
void testAud05ParallelJobLargeAndNestedFlatStructs() {
    const std::string src = R"RS(
module P2ParallelFlat;

import RealScript.Math;

public struct ParticleState6 {
    public double px;
    public double py;
    public double vx;
    public double vy;
    public double ax;
    public double mass;
}

public struct LeafSpan {
    public int lo;
    public int hi;
}

public struct MidTransform {
    public LeafSpan span;
    public Vec2 bias;
    public int scale;
}

public struct DeepConfig {
    public MidTransform primary;
    public MidTransform secondary;
    public int salt;
}

ParticleState6 stepParticle(ParticleState6 s, double dt) {
    ParticleState6 next = new ParticleState6();
    next.vx = s.vx + s.ax * dt;
    next.vy = s.vy + (s.ax * 0.5) * dt;
    next.px = s.px + next.vx * dt;
    next.py = s.py + next.vy * dt;
    next.ax = s.ax * 0.95;
    next.mass = s.mass;
    return next;
}

public void kernelIntegrate6Fields(
    int index,
    double[] inPos,
    double[] outEnergy,
    double dt) {
    ParticleState6 p = new ParticleState6();
    p.px = inPos[index];
    p.py = inPos[index] * 0.5;
    p.vx = 1.25;
    p.vy = -0.75;
    p.ax = 2.0;
    p.mass = 3.0;
    ParticleState6 p1 = stepParticle(p, dt);
    ParticleState6 p2 = stepParticle(p1, dt);
    outEnergy[index] = p2.mass * (p2.px + p2.py + p2.vx - p2.vy + p2.ax);
}

int evalDeep(DeepConfig cfg, int idx) {
    DeepConfig localCfg = cfg;
    MidTransform inner = localCfg.primary;
    inner.scale = inner.scale + 2;
    localCfg.primary = inner;
    return (idx + localCfg.primary.span.lo) * localCfg.primary.scale
         + (localCfg.secondary.span.hi - localCfg.primary.span.hi)
         + localCfg.salt;
}

public int kernelNestedReduce(int index, DeepConfig cfg) {
    return evalDeep(cfg, index);
}

public DeepConfig makeDefaultConfig() {
    LeafSpan s1 = new LeafSpan();
    s1.lo = 3;
    s1.hi = 11;
    MidTransform m1 = new MidTransform();
    m1.span = s1;
    m1.bias = new Vec2(Fixed.Of(1), Fixed.Of(2));
    m1.scale = 5;

    LeafSpan s2 = new LeafSpan();
    s2.lo = 7;
    s2.hi = 29;
    MidTransform m2 = new MidTransform();
    m2.span = s2;
    m2.bias = new Vec2(Fixed.Of(3), Fixed.Of(4));
    m2.scale = 9;

    DeepConfig d = new DeepConfig();
    d.primary = m1;
    d.secondary = m2;
    d.salt = 17;
    return d;
}

public LeafSpan makeSampleSpan() {
    LeafSpan s = new LeafSpan();
    s.lo = 1;
    s.hi = 2;
    return s;
}

public ParticleState6 badKernelReturn6(int index, double dt) {
    ParticleState6 p = new ParticleState6();
    p.px = dt;
    return stepParticle(p, dt);
}
)RS";

    auto bundle = compileProgramSource(src, "aud05_flat_structs.rscript");
    const auto kernel6Id = findFunctionId(*bundle.program, "kernelIntegrate6Fields");
    const auto kernelNestedId = findFunctionId(*bundle.program, "kernelNestedReduce");
    const auto makeCfgId = findFunctionId(*bundle.program, "makeDefaultConfig");
    const auto makeSpanId = findFunctionId(*bundle.program, "makeSampleSpan");
    const auto badReturn6Id = findFunctionId(*bundle.program, "badKernelReturn6");

    const auto badReturn6Report = runtime::verifyParallelJobFunction(*bundle.program, badReturn6Id);
    require(
        !badReturn6Report.valid,
        "AUD-05 safety regression: top-level kernel returning >4-field struct must be rejected");

    // 1. Static verifier accepts both >4-field and 3-level nested custom pure value structs
    const auto report6 = runtime::verifyParallelJobFunction(*bundle.program, kernel6Id);
    require(
        report6.valid,
        "AUD-05 regression: 6-field pure value struct kernel rejected: " + report6.summaryMessage());

    const auto reportNested = runtime::verifyParallelJobFunction(*bundle.program, kernelNestedId);
    require(
        reportNested.valid,
        "AUD-05 regression: 3-level nested pure value struct kernel rejected: " +
            reportNested.summaryMessage());

    // 2. Execute 6-field struct kernel on 1 vs 4 workers: verify zero heap struct allocations & parity
    auto heap = std::make_shared<runtime::ManagedHeap>();
    constexpr std::size_t kCount = 256;
    const auto dblArrTid = semantic::stableTypeId("double[]");
    const auto inArrOpt = heap->allocateTypedArray(dblArrTid, semantic::PrimitiveType::Double, 0, kCount, 0.0);
    const auto outArr1Opt = heap->allocateTypedArray(dblArrTid, semantic::PrimitiveType::Double, 0, kCount, 0.0);
    const auto outArr4Opt = heap->allocateTypedArray(dblArrTid, semantic::PrimitiveType::Double, 0, kCount, 0.0);
    require(inArrOpt && outArr1Opt && outArr4Opt, "failed to allocate packed double[]");
    const auto inArr = *inArrOpt;
    const auto outArr1 = *outArr1Opt;
    const auto outArr4 = *outArr4Opt;
    auto r0 = heap->retain(inArr);
    auto r1 = heap->retain(outArr1);
    auto r4 = heap->retain(outArr4);
    std::vector<double> initIn(kCount);
    for (std::size_t i = 0; i < kCount; ++i) {
        initIn[i] = static_cast<double>(i) * 0.25 + 1.0;
    }
    require(heap->writePackedDoubleArray(inArr, initIn.data(), kCount), "writePackedDoubleArray failed");

    runtime::ParallelJobScheduler scheduler(4);

    // 2a. Production fast-accounting mode (DeterminismMode::Off): verify ZERO StructStorage & ManagedHeap allocations
    runtime::ParallelJobConfig fastCfg4;
    fastCfg4.workerCount = 4;
    fastCfg4.batchSize = 16;
    fastCfg4.executionOptions.determinism.mode = runtime::DeterminismMode::Off;
    (void)scheduler.dispatchFor(
        bundle.program, heap, kernel6Id, 8,
        {runtime::Value{inArr}, runtime::Value{outArr1}, runtime::Value{0.1}}, fastCfg4);

    const std::size_t liveBefore6 = heap->liveObjects();
    const std::uint64_t allocBefore6 = runtime::heapStructStorageAllocations();
    const auto fastRes4 = scheduler.dispatchFor(
        bundle.program, heap, kernel6Id, kCount,
        {runtime::Value{inArr}, runtime::Value{outArr4}, runtime::Value{0.125}}, fastCfg4);
    const std::uint64_t allocAfter6 = runtime::heapStructStorageAllocations();
    require(fastRes4.succeeded, "4-worker fast-path 6-field kernel failed: " + fastRes4.error.message);
    require(
        allocAfter6 == allocBefore6,
        "AUD-05 regression: expected 0 heap StructStorage allocations on flat-frame fast path, got " +
            std::to_string(allocAfter6 - allocBefore6));
    require(
        heap->liveObjects() == liveBefore6,
        "AUD-05 regression: ManagedHeap liveObjects changed during 6-field struct parallel dispatch");

    // 2b. Strict determinism mode: verify bit-identical digest across 1 vs 4 workers & zero ManagedHeap growth
    runtime::ParallelJobConfig cfg1;
    cfg1.workerCount = 1;
    cfg1.batchSize = 16;
    cfg1.executionOptions.determinism.mode = runtime::DeterminismMode::Strict;

    runtime::ParallelJobConfig cfg4 = cfg1;
    cfg4.workerCount = 4;

    const auto res1 = scheduler.dispatchFor(
        bundle.program, heap, kernel6Id, kCount,
        {runtime::Value{inArr}, runtime::Value{outArr1}, runtime::Value{0.125}}, cfg1);
    const auto res4 = scheduler.dispatchFor(
        bundle.program, heap, kernel6Id, kCount,
        {runtime::Value{inArr}, runtime::Value{outArr4}, runtime::Value{0.125}}, cfg4);
    require(res1.succeeded, "1-worker 6-field kernel failed: " + res1.error.message);
    require(res4.succeeded, "4-worker 6-field kernel failed: " + res4.error.message);
    require(
        heap->liveObjects() == liveBefore6,
        "AUD-05 regression: ManagedHeap liveObjects changed during Strict 6-field struct dispatch");
    require(
        res1.determinismDigest == res4.determinismDigest &&
            res1.batchDigests == res4.batchDigests,
        "1-worker vs 4-worker determinismDigest mismatch on 6-field struct kernel");

    std::vector<double> outVals1(kCount, 0.0), outVals4(kCount, 0.0);
    require(heap->readPackedDoubleArray(outArr1, outVals1), "readPackedDoubleArray 1 failed");
    require(heap->readPackedDoubleArray(outArr4, outVals4), "readPackedDoubleArray 4 failed");
    for (std::size_t i = 0; i < kCount; ++i) {
        require(
            outVals1[i] == outVals4[i] && outVals1[i] != 0.0,
            "6-field struct kernel element output mismatch at index " + std::to_string(i));
    }

    // 3. Execute 3-level nested custom struct parameter + helper mutation + integer Sum reduction
    runtime::Interpreter hostInterp(bundle.program, heap);
    const auto cfgValRes = hostInterp.invoke(makeCfgId, {});
    require(cfgValRes.succeeded, "makeDefaultConfig failed: " + cfgValRes.error.message);

    fastCfg4.reduceOp = runtime::ParallelReduceOp::Sum;
    (void)scheduler.dispatchFor(
        bundle.program, heap, kernelNestedId, 8, {cfgValRes.value}, fastCfg4);
    const std::uint64_t allocBeforeNested = runtime::heapStructStorageAllocations();
    const auto fastNested4 = scheduler.dispatchFor(
        bundle.program, heap, kernelNestedId, 128, {cfgValRes.value}, fastCfg4);
    const std::uint64_t allocAfterNested = runtime::heapStructStorageAllocations();
    require(fastNested4.succeeded, "fast 4-worker nested struct kernel failed: " + fastNested4.error.message);
    require(
        allocAfterNested == allocBeforeNested,
        "AUD-05 regression: expected 0 heap StructStorage allocations on nested flat-frame kernel, got " +
            std::to_string(allocAfterNested - allocBeforeNested));

    cfg1.reduceOp = runtime::ParallelReduceOp::Sum;
    cfg4.reduceOp = runtime::ParallelReduceOp::Sum;
    const auto nested1 = scheduler.dispatchFor(
        bundle.program, heap, kernelNestedId, 128, {cfgValRes.value}, cfg1);
    const auto nested4 = scheduler.dispatchFor(
        bundle.program, heap, kernelNestedId, 128, {cfgValRes.value}, cfg4);

    require(nested1.succeeded, "1-worker nested struct kernel failed: " + nested1.error.message);
    require(nested4.succeeded, "4-worker nested struct kernel failed: " + nested4.error.message);
    require(
        nested1.determinismDigest == nested4.determinismDigest,
        "1-worker vs 4-worker determinismDigest mismatch on nested struct kernel");

    require(
        std::get<std::int64_t>(nested4.value) == 64064,
        "unexpected nested struct reduced sum: " +
            std::to_string(std::get<std::int64_t>(nested4.value)));

    // 3b. Runtime typeId verification rejects mismatched StructValue arguments
    const auto wrongSpanRes = hostInterp.invoke(makeSpanId, {});
    require(wrongSpanRes.succeeded, "makeSampleSpan failed");
    const auto mismatchedStructRes = scheduler.dispatchFor(
        bundle.program, heap, kernelNestedId, 4, {wrongSpanRes.value}, fastCfg4);
    require(
        !mismatchedStructRes.succeeded &&
            mismatchedStructRes.error.code == runtime::ErrorCode::TypeMismatch,
        "AUD-05 regression: dispatchFor must reject mismatched StructValue typeId");

    // 4. Negative check: nested struct containing a managed reference (string / object / array) MUST fail
    const std::string badSrc = R"RS(
module P2ParallelBadNested;

public struct InnerWithString {
    public int id;
    public string label;
}

public struct OuterWrapper {
    public int tag;
    public InnerWithString payload;
}

public int badKernelConstruct(int index) {
    OuterWrapper w = new OuterWrapper();
    w.tag = index;
    return w.tag;
}

public int badKernelParam(int index, OuterWrapper w) {
    return index + w.tag;
}

public int badKernelLoadNestedStruct(int index, OuterWrapper w) {
    InnerWithString inner = w.payload;
    return inner.id + index;
}
)RS";
    auto badBundle = compileProgramSource(badSrc, "aud05_bad_nested.rscript");
    const auto badConstructReport = runtime::verifyParallelJobFunction(
        *badBundle.program, findFunctionId(*badBundle.program, "badKernelConstruct"));
    require(
        !badConstructReport.valid,
        "AUD-05 safety regression: nested struct containing string must be rejected");
    bool sawStructMutationViolation = false;
    for (const auto& v : badConstructReport.violations) {
        if (v.kind == runtime::ParallelVerifyViolationKind::NonInlineStructMutation) {
            sawStructMutationViolation = true;
        }
    }
    require(sawStructMutationViolation, "expected NonInlineStructMutation on nested string struct");

    const auto badParamReport = runtime::verifyParallelJobFunction(
        *badBundle.program, findFunctionId(*badBundle.program, "badKernelParam"));
    require(
        !badParamReport.valid,
        "AUD-05 safety regression: parameter with nested string struct must be rejected");

    const auto badLoadReport = runtime::verifyParallelJobFunction(
        *badBundle.program, findFunctionId(*badBundle.program, "badKernelLoadNestedStruct"));
    require(
        !badLoadReport.valid,
        "AUD-05 safety regression: LoadStructField / local on nested string struct must be rejected");
}

// ============================================================================
// AUD-06: RuntimeDebugHook Decoupling, ModuleBuildCache::buildAot & 8-Layer DAG
// ============================================================================
class LightweightCountingDebugHook final : public debug::RuntimeDebugHook {
public:
    std::size_t boundCount = 0;
    std::size_t beginCount = 0;
    std::size_t endCount = 0;
    std::size_t sequencePoints = 0;

    void bindProgram(const runtime::ProgramImage&) override { ++boundCount; }
    void beginExecution() override { ++beginCount; }
    void endExecution() noexcept override { ++endCount; }
    [[nodiscard]] bool hasPendingInterruption() const noexcept override { return false; }
    [[nodiscard]] bool onSequencePoint(
        const std::vector<debug::DebugFrameView>&,
        runtime::ManagedHeap*) override {
        ++sequencePoints;
        return true;
    }
    [[nodiscard]] bool onException(
        const std::vector<debug::DebugFrameView>&,
        runtime::ManagedHeap*,
        runtime::ErrorCode,
        std::string,
        std::string,
        bool,
        bool) override {
        return true;
    }
};

void testAud06LayerDecouplingAndDebugHook() {
    // 1. Custom RuntimeDebugHook works directly with Interpreter without DebugController
    const std::string dbgSrc = R"RS(
module P2DebugHookMod;

public int computeSum(int n) {
    int acc = 0;
    int i = 1;
    while (i <= n) {
        acc = acc + i;
        i = i + 1;
    }
    return acc;
}
)RS";
    auto unoptBundle = compileProgramSource(
        dbgSrc, "p2_debug_hook.rscript", optimization::Level::None);
    auto program = unoptBundle.program;
    auto heap = std::make_shared<runtime::ManagedHeap>();
    runtime::Interpreter interp(program, heap);

    auto customHook = std::make_shared<LightweightCountingDebugHook>();
    runtime::ExecutionOptions opts;
    opts.debugger = customHook;
    const auto sumRes = interp.invoke(
        findFunctionId(*program, "computeSum"), {runtime::Value{std::int64_t{5}}}, opts);
    require(sumRes.succeeded && std::get<std::int64_t>(sumRes.value) == 15, "computeSum failed");
    require(
        customHook->boundCount == 1 && customHook->beginCount == 1 &&
            customHook->endCount == 1 && customHook->sequencePoints > 0,
        "LightweightCountingDebugHook lifecycle callbacks did not fire as expected");

    // 2. Full DebugController implicitly binds to ExecutionOptions::debugger and hits breakpoints
    auto controller = std::make_shared<debug::DebugController>();
    controller->bindProgram(*program);
    const auto bps = controller->setBreakpoints(
        "p2_debug_hook.rscript", {text::LinePosition{7, 8}});
    require(bps.size() == 1 && bps[0].verified, "breakpoint verification failed");

    std::size_t hits = 0;
    bool inspectedFrame = false;
    controller->setStopHandler([&](const debug::DebugStop& stop) {
        if (stop.reason == debug::StopReason::Breakpoint) {
            ++hits;
            if (!stop.frames.empty() && stop.frames.front().function.find("computeSum") != std::string::npos) {
                inspectedFrame = true;
            }
        }
        return debug::ResumeMode::Continue;
    });

    runtime::ExecutionOptions ctrlOpts;
    ctrlOpts.debugger = controller;
    const auto bpRes = interp.invoke(
        findFunctionId(*program, "computeSum"), {runtime::Value{std::int64_t{4}}}, ctrlOpts);
    require(bpRes.succeeded && std::get<std::int64_t>(bpRes.value) == 10, "computeSum with DebugController failed");
    require(hits >= 4 && inspectedFrame, "expected loop breakpoint hits with computeSum stack frame");

    // 3. ModuleBuildCache::buildAot works across cold miss and warm cache hit
#ifdef REALSCRIPT_JIT_CACHE_DIR
    const std::filesystem::path baseTmp = std::filesystem::path(REALSCRIPT_JIT_CACHE_DIR).parent_path();
#else
    const std::filesystem::path baseTmp = std::filesystem::current_path();
#endif
    const auto cacheDir = baseTmp / "p2_audit_aot_build_cache";
    std::error_code ec;
    std::filesystem::remove_all(cacheDir, ec);

    compiler::ModuleBuildCache buildCache(cacheDir);
    compiler::ModuleBuildCacheOptions cacheOpts;
    cacheOpts.optimizationOptions.level = optimization::Level::Basic;
    aot::GenerationOptions aotOpts;
    aotOpts.programName = "P2CacheCheck";

    const std::vector<compiler::SourceFile> cacheSources{
        compiler::SourceFile{"p2_cache_mod.rscript", dbgSrc},
    };
    const auto coldAot = buildCache.buildAot(cacheSources, cacheOpts, aotOpts);
    require(coldAot.succeeded(), "cold ModuleBuildCache::buildAot failed");
    require(!coldAot.aotCacheHit, "cold buildAot must be a cache miss");
    require(!coldAot.program.source.empty(), "cold buildAot returned empty C++ source");

    const auto warmAot = buildCache.buildAot(cacheSources, cacheOpts, aotOpts);
    require(warmAot.succeeded(), "warm ModuleBuildCache::buildAot failed");
    require(warmAot.aotCacheHit, "second buildAot call must hit disk .aotcache");
    require(warmAot.program.source == coldAot.program.source, "warm AOT source mismatch");
    const auto nullCbWarm =
        buildCache.buildAotWithGenerator(cacheSources, cacheOpts, aotOpts, nullptr);
    require(
        !nullCbWarm.succeeded(),
        "buildAotWithGenerator with null callback must fail even on warm .aotcache");
    std::filesystem::remove_all(cacheDir, ec);

    // 4. Verify CMakeLists.txt topological layer placement invariants
#ifdef REALSCRIPT_SOURCE_DIR
    const std::filesystem::path rootDir(REALSCRIPT_SOURCE_DIR);
    const std::string cmake = readTextFile(rootDir / "CMakeLists.txt");
    const auto posFoundation = cmake.find("realscript_add_layer_library(realscript_layer_foundation");
    const auto posCompiler = cmake.find("realscript_add_layer_library(realscript_layer_compiler_core");
    const auto posRuntime = cmake.find("realscript_add_layer_library(realscript_layer_runtime_core");
    const auto posNative = cmake.find("realscript_add_layer_library(realscript_layer_native_backends");
    const auto posDebug = cmake.find("realscript_add_layer_library(realscript_layer_debug");
    const auto posGame = cmake.find("realscript_add_layer_library(realscript_layer_game");
    const auto posTooling = cmake.find("realscript_add_layer_library(realscript_layer_tooling");
    const auto posExt = cmake.find("realscript_add_layer_library(realscript_layer_extensibility");

    require(
        posFoundation < posCompiler && posCompiler < posRuntime &&
            posRuntime < posNative && posNative < posDebug &&
            posDebug < posGame && posGame < posTooling && posTooling < posExt,
        "CMakeLists.txt 8-layer topological declaration order violated");

    const auto posJson = cmake.find("src/tooling/Json.cpp");
    const auto posDebugInfo = cmake.find("src/debug/DebugInfo.cpp");
    require(
        posJson > posFoundation && posJson < posCompiler,
        "src/tooling/Json.cpp must belong to realscript_layer_foundation");
    require(
        posDebugInfo > posCompiler && posDebugInfo < posRuntime,
        "src/debug/DebugInfo.cpp must belong to realscript_layer_compiler_core");
#endif
}

// ============================================================================
// AUD-07: ToolchainJit Shell-Free Subprocess, noexec Diagnostics & AOT Diet
// ============================================================================
void testAud07ToolchainJitHardeningAndAotHeaderDiet() {
    // 1. Non-async AOT generation omits AsyncRuntime inline pull-in; async keeps it
    {
        const auto syncBundle = compileProgramSource(R"RS(
module P2SyncAot;
public int addMul(int a, int b) {
    return (a + b) * 3;
}
)RS", "p2_sync_aot.rscript");
        diagnostics::DiagnosticBag diags;
        aot::CppGenerator gen;
        const auto generatedSync = gen.generate(syncBundle.mirModules, diags);
        require(!diags.hasErrors(), "sync AOT generation failed");
        require(
            generatedSync.source.find("#define REALSCRIPT_AOT_NO_ASYNC_INLINES 1") != std::string::npos,
            "non-async AOT unit should define REALSCRIPT_AOT_NO_ASYNC_INLINES");

        const auto asyncBundle = compileProgramSource(R"RS(
module P2AsyncAot;
import RealScript.Async;
public async Awaitable<int> tickOnce() {
    return 42;
}
)RS", "p2_async_aot.rscript");
        const auto generatedAsync = gen.generate(asyncBundle.mirModules, diags);
        require(!diags.hasErrors(), "async AOT generation failed");
        require(
            generatedAsync.source.find("REALSCRIPT_AOT_NO_ASYNC_INLINES") == std::string::npos,
            "async AOT unit must retain inline AsyncRuntime fast-paths");
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
        auto jitBundle = compileProgramSource(R"RS(
module P2ShellFreeJit;
public int compute(int x) {
    return x * x + 7;
}
)RS", "p2_shell_free_jit.rscript");

        // 2. Output directory containing spaces, quotes, ';', '&', and '$()' must NOT invoke /bin/sh
        const auto canaryFile = baseOpts.outputDirectory.parent_path() / "p2_shell_injection_canary.txt";
        std::error_code ec;
        std::filesystem::remove(canaryFile, ec);

        const auto trickyDir = baseOpts.outputDirectory.parent_path() /
            "jit cache (spaces & 'quotes'); $(touch p2_shell_injection_canary.txt)";
        std::filesystem::remove_all(trickyDir, ec);
        const auto trickyIncludeDir = trickyDir / "inc (a & %TEMP% 'q')";
        std::filesystem::create_directories(trickyIncludeDir, ec);

        jit::ToolchainOptions trickyOpts = baseOpts;
        trickyOpts.outputDirectory = trickyDir;
        trickyOpts.reuseCachedLibrary = false;
#if defined(_MSC_VER)
        trickyOpts.compilerArguments.push_back("/I" + trickyIncludeDir.string());
        trickyOpts.compilerArguments.push_back("/DRS_P2_TRICKY_FLAG=1");
#else
        trickyOpts.compilerArguments.push_back("-I" + trickyIncludeDir.string());
        trickyOpts.compilerArguments.push_back("-DRS_P2_TRICKY_FLAG=1");
#endif

        jit::ToolchainJit toolchain;
        const auto compiled = toolchain.compile(jitBundle.mirModules, trickyOpts);
        require(
            compiled.succeeded(),
            "AUD-07 regression: ToolchainJit failed on path with spaces/shell metacharacters: " +
                compiled.error);
        require(
            !std::filesystem::exists(canaryFile, ec),
            "AUD-07 security regression: shell metacharacter canary was executed!");

        auto heap = std::make_shared<runtime::ManagedHeap>();
        compiled.module->setHeap(heap);
        const auto execRes = compiled.module->invoke(
            "P2ShellFreeJit::compute", {runtime::Value{std::int64_t{6}}});
        require(
            execRes.succeeded && std::get<std::int64_t>(execRes.value) == 43,
            "ToolchainJit module invocation returned unexpected value");

        std::filesystem::remove_all(trickyDir, ec);

#if !defined(_WIN32)
        // 3. If /tmp is mounted with ST_NOEXEC in this environment, verify explicit noexec diagnostic
        struct statvfs tmpVfs{};
        if (statvfs("/tmp", &tmpVfs) == 0 && (tmpVfs.f_flag & ST_NOEXEC) != 0UL) {
            const auto noexecDir = std::filesystem::path("/tmp") / "realscript_p2_noexec_diag_test";
            std::filesystem::remove_all(noexecDir, ec);
            jit::ToolchainOptions noexecOpts = baseOpts;
            noexecOpts.outputDirectory = noexecDir;
            noexecOpts.reuseCachedLibrary = false;
            const auto noexecRes = toolchain.compile(jitBundle.mirModules, noexecOpts);
            require(!noexecRes.succeeded(), "dlopen on ST_NOEXEC /tmp unexpectedly succeeded");
            require(
                noexecRes.error.find("noexec") != std::string::npos &&
                    noexecRes.error.find("outputDirectory") != std::string::npos,
                "AUD-07 regression: missing actionable 'noexec' mount hint in error: " +
                    noexecRes.error);
            std::filesystem::remove_all(noexecDir, ec);
        }
#endif
    }
#endif
}

} // namespace

int main() {
    try {
        testAud05ParallelJobLargeAndNestedFlatStructs();
        testAud06LayerDecouplingAndDebugHook();
        testAud07ToolchainJitHardeningAndAotHeaderDiet();
        std::cout << "All P2 audit architecture & performance regression tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "P2 audit regression test failure: " << ex.what() << "\n";
        return 1;
    }
}
