#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/compiler/ModuleBuildCache.h"
#include "realscript/debug/DebugInfo.h"
#include "realscript/diagnostics/Diagnostic.h"
#include "realscript/mir/Mir.h"
#include "realscript/optimization/Optimizer.h"
#include "realscript/runtime/Runtime.h"

#include <chrono>
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

bool hasDiagCode(const diagnostics::DiagnosticBag& bag, const std::string& code) {
    for (const auto& d : bag.items()) {
        if (d.code == code) {
            return true;
        }
    }
    return false;
}

std::string formatDiags(const diagnostics::DiagnosticBag& bag) {
    std::ostringstream oss;
    for (const auto& d : bag.items()) {
        oss << d.sourceName << " [" << d.code << "] " << d.message << '\n';
    }
    return oss.str();
}

struct CompiledProgramBundle {
    std::vector<mir::Module> mirModules;
    std::vector<bytecode::Module> bytecodeModules;
    std::shared_ptr<const runtime::ProgramImage> program;
};

CompiledProgramBundle compileAndLink(
    std::vector<compiler::SourceFile> sources,
    optimization::Level optLevel = optimization::Level::Basic,
    compiler::CompilationOptions compOpts = {}) {
    compiler::Compilation comp(std::move(sources), std::move(compOpts));
    auto build = comp.build();
    require(!build.diagnostics.hasErrors(), "Compilation failed:\n" + formatDiags(build.diagnostics));

    diagnostics::DiagnosticBag optDiag;
    optimization::Options optOpts;
    optOpts.level = optLevel;
    optOpts.preserveDebugInfo = true;
    auto optResult = optimization::Optimizer{}.optimize(std::move(build.modules), optDiag, optOpts);
    require(!optDiag.hasErrors(), "Optimization failed:\n" + formatDiags(optDiag));

    bytecode::Lowerer lowerer;
    std::vector<bytecode::Module> bcMods;
    bcMods.reserve(optResult.modules.size());
    for (const auto& m : optResult.modules) {
        require(mir::verifyModule(m, optDiag), "MIR verification failed");
        auto bc = lowerer.lower(m);
        require(bytecode::verifyModule(bc, optDiag), "Bytecode verification failed");
        bcMods.push_back(std::move(bc));
    }

    runtime::RuntimeError linkErr;
    auto linked = runtime::ProgramImage::link(bcMods, linkErr);
    require(linked.has_value(), "Program link failed: " + linkErr.message);

    CompiledProgramBundle bundle;
    bundle.mirModules = std::move(optResult.modules);
    bundle.bytecodeModules = std::move(bcMods);
    bundle.program = std::make_shared<const runtime::ProgramImage>(std::move(*linked));
    return bundle;
}

void writeTextFile(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    require(out.good(), "Failed to open file for writing: " + path.string());
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    require(out.good(), "Failed to write file: " + path.string());
}

// ---------------------------------------------------------------------------
// 1. MIR Lowering, MirVerifier Rejections, Optimizer Parity (-O0 vs -O2)
// ---------------------------------------------------------------------------
void testMirVerifierAndOptimizerParity() {
    const std::string src = R"RS(
module Kernel.MirOpt;

public struct VecPair {
    public int x;
    public int y;
}

public class BaseNode {
    public virtual int Bonus(int v) { return v + 1; }
}

public sealed class LeafNode : BaseNode {
    public override int Bonus(int v) { return v * 3 + 5; }
}

int AddSmall(int a, int b) {
    return a + b + 2;
}

public int Compute(int n) {
    int c = 6 * 7 - 2;
    VecPair p = new VecPair();
    p.x = 10;
    p.y = 20;
    LeafNode node = new LeafNode();
    int acc = 0;
    for (int i = 0; i < n; i = i + 1) {
        acc = acc + c + p.x + AddSmall(i, 1) + node.Bonus(2);
    }
    if (true) {
        acc = acc + p.y;
    } else {
        acc = acc - 9999;
    }
    return acc;
}
)RS";

    auto o0 = compileAndLink({{"mir_opt.rscript", src}}, optimization::Level::None);
    auto o2 = compileAndLink({{"mir_opt.rscript", src}}, optimization::Level::Aggressive);

    require(!o2.mirModules.empty(), "Expected MIR module in -O2 bundle");
    const std::string printedMir = mir::printModule(o2.mirModules.front());
    require(printedMir.find("Compute") != std::string::npos, "MIR print missing Compute");

    runtime::EngineRuntime rt0(o0.program);
    runtime::EngineRuntime rt2(o2.program);
    const auto r0 = rt0.invoke("Kernel.MirOpt::Compute", {runtime::Value{std::int64_t{8}}});
    const auto r2 = rt2.invoke("Kernel.MirOpt::Compute", {runtime::Value{std::int64_t{8}}});
    require(r0.succeeded && r2.succeeded, "Compute failed under -O0 or -O2");
    require(
        std::get<std::int64_t>(r0.value) == std::get<std::int64_t>(r2.value),
        "-O0 and -O2 produced divergent results");

    // Verify MIR verifier catches broken jump target (RS3009) and broken operand (RS3012).
    {
        mir::Module broken = o0.mirModules.front();
        diagnostics::DiagnosticBag bag;
        broken.functions.front().blocks.front().terminator.kind = mir::TerminatorKind::Jump;
        broken.functions.front().blocks.front().terminator.target = 9999;
        require(!mir::verifyModule(broken, bag) && hasDiagCode(bag, "RS3009"),
            "MirVerifier did not report RS3009 on out-of-range block target");
    }
    {
        mir::Module broken = o0.mirModules.front();
        diagnostics::DiagnosticBag bag;
        mir::Instruction badInst;
        badInst.result = 999;
        badInst.resultType = semantic::PrimitiveType::Int;
        badInst.opcode = mir::Opcode::AddInt;
        badInst.operands = {777, 888};
        broken.functions.front().blocks.front().instructions.push_back(badInst);
        require(!mir::verifyModule(broken, bag) && hasDiagCode(bag, "RS3006"),
            "MirVerifier did not report RS3006 on undefined MIR value operand");

        mir::Module badArity = o0.mirModules.front();
        diagnostics::DiagnosticBag arityBag;
        badInst.operands = {777};
        badArity.functions.front().blocks.front().instructions.push_back(badInst);
        require(!mir::verifyModule(badArity, arityBag) && hasDiagCode(arityBag, "RS3012"),
            "MirVerifier did not report RS3012 on invalid MIR operand count");
    }
}

// ---------------------------------------------------------------------------
// 2. Deterministic Record/Replay & Replay Digest Parity
// ---------------------------------------------------------------------------
void testDeterministicRecordReplayAndDigest() {
    const std::string src = R"RS(
module Kernel.DetReplay;

int NextRand(int seed);
double CurrentStep();

public double RunTick(int seed, int count) {
    double sum = CurrentStep();
    int s = seed;
    for (int i = 0; i < count; i = i + 1) {
        s = NextRand(s + i);
        sum = sum + (double)s;
    }
    return sum;
}
)RS";

    auto bundle = compileAndLink({{"det_replay.rscript", src}}, optimization::Level::Basic);

    auto bindings = std::make_shared<runtime::BindingRegistry>();
    int hostCounter = 1;
    bindings->bind(
        "Kernel.DetReplay::NextRand",
        [&hostCounter](
            const bytecode::FunctionReference&,
            runtime::ValueSpan args,
            runtime::RuntimeError&) -> std::optional<runtime::Value> {
            const std::int64_t seed = std::get<std::int64_t>(args[0]);
            return runtime::Value{seed * 3 + (hostCounter++)};
        },
        runtime::BindingDeterminism::Recordable);
    bindings->bind(
        "Kernel.DetReplay::CurrentStep",
        [](const bytecode::FunctionReference&,
           runtime::ValueSpan,
           runtime::RuntimeError&) -> std::optional<runtime::Value> {
            return runtime::Value{0.25};
        },
        runtime::BindingDeterminism::Recordable);

    auto replayLog = std::make_shared<runtime::ReplayLog>();
    runtime::ExecutionOptions recOpts;
    recOpts.determinism.mode = runtime::DeterminismMode::Record;
    recOpts.determinism.replayLog = replayLog;

    runtime::EngineRuntime rtRec(bundle.program);
    rtRec.setBindings(bindings);
    const auto recRes = rtRec.invoke(
        "Kernel.DetReplay::RunTick",
        {runtime::Value{std::int64_t{5}}, runtime::Value{std::int64_t{4}}},
        recOpts);
    require(recRes.succeeded, "Record execution failed: " + recRes.error.message);
    require(recRes.determinismDigest != 0 && replayLog->size() == 5,
        "Expected non-zero determinism digest and 5 recorded events");

    // Mutate host side-state to confirm Replay consumes the recorded log rather than calling host bindings.
    hostCounter = 999999;
    runtime::ExecutionOptions repOpts;
    repOpts.determinism.mode = runtime::DeterminismMode::Replay;
    repOpts.determinism.replayLog = replayLog;

    runtime::EngineRuntime rtRep(bundle.program);
    rtRep.setBindings(bindings);
    const auto repRes = rtRep.invoke(
        "Kernel.DetReplay::RunTick",
        {runtime::Value{std::int64_t{5}}, runtime::Value{std::int64_t{4}}},
        repOpts);
    require(repRes.succeeded, "Replay execution failed: " + repRes.error.message);
    require(
        std::get<double>(repRes.value) == std::get<double>(recRes.value) &&
        repRes.determinismDigest == recRes.determinismDigest &&
        repRes.replayEntriesConsumed == 5,
        "Replay output, digest, or consumed entries differed from Record");
}

// ---------------------------------------------------------------------------
// 3. .rsbc Codec Canonical Round-Trip, RSLM #line Trailer & BytecodeVerifier
// ---------------------------------------------------------------------------
void testBytecodeCodecLineTrailerAndVerifier() {
    const std::string lineMapSrc = R"RS(
module Kernel.RsbcLines;

public int Transform(int x) {
    int a = x + 1;
#line hidden
    int glue = a * 2;
#line 77 "views/PanelView.rscript"
    int shown = glue + 9;
#line default
    return shown;
}
)RS";

    auto bundle = compileAndLink({{"$generated/PanelView.g.rscript", lineMapSrc}}, optimization::Level::Basic);
    require(bundle.bytecodeModules.size() == 1, "Expected 1 compiled bytecode module");
    const auto& origMod = bundle.bytecodeModules.front();

    const std::string disasm = bytecode::disassembleModule(origMod);
    require(disasm.find("Transform") != std::string::npos, "Disassembly missing Transform");

    const auto rsbcBytes = bytecode::encodeModule(origMod);
    require(rsbcBytes.size() > 16, "Encoded RSBC module unexpectedly small");

    // Confirm canonical RSLM trailer is appended in the encoded bytes and survives round-trip.
    const std::string bytesStr(reinterpret_cast<const char*>(rsbcBytes.data()), rsbcBytes.size());
    require(bytesStr.find("MLSR") != std::string::npos || bytesStr.find("RSLM") != std::string::npos,
        "Encoded RSBC missing expected RSLM #line debug trailer");

    bytecode::Module decodedMod;
    diagnostics::DiagnosticBag diag;
    require(bytecode::decodeModule(rsbcBytes, decodedMod, diag) && !diag.hasErrors(),
        "RSBC decode failed: " + formatDiags(diag));
    require(bytecode::verifyModule(decodedMod, diag) && !diag.hasErrors(),
        "Decoded RSBC failed verification: " + formatDiags(diag));

    bool sawHidden = false;
    bool sawMappedLine77 = false;
    for (const auto& fn : decodedMod.functions) {
        if (fn.name.find("Transform") == std::string::npos) {
            continue;
        }
        for (const auto& sp : fn.debugInfo.sequencePoints) {
            if (sp.hidden) {
                sawHidden = true;
            }
            if (!sp.hidden && sp.mappedPath == "views/PanelView.rscript" && sp.range.start.line == 76) {
                sawMappedLine77 = true;
            }
        }
    }
    require(sawHidden && sawMappedLine77,
        "RSBC RSLM trailer round-trip did not preserve hidden or mappedPath/line 77 sequence points");

    // Re-encoding decoded module must be byte-for-byte deterministic.
    const auto reEncoded = bytecode::encodeModule(decodedMod);
    require(reEncoded == rsbcBytes, "RSBC canonical re-encoding was not byte-identical");

    // Bytecode codec malformed rejections: bad magic (RS5000), bad version (RS5001), section/count bounds.
    {
        auto badMagic = rsbcBytes;
        badMagic[0] = 'X';
        bytecode::Module outMod;
        diagnostics::DiagnosticBag errBag;
        require(!bytecode::decodeModule(badMagic, outMod, errBag) && hasDiagCode(errBag, "RS5000"),
            "Expected RS5000 for invalid RSBC magic");
    }
    {
        auto badVer = rsbcBytes;
        badVer[4] = 99;
        badVer[5] = 0;
        bytecode::Module outMod;
        diagnostics::DiagnosticBag errBag;
        require(!bytecode::decodeModule(badVer, outMod, errBag) && hasDiagCode(errBag, "RS5001"),
            "Expected RS5001 for unsupported RSBC version");
    }
    {
        auto truncated = rsbcBytes;
        truncated.resize(12);
        bytecode::Module outMod;
        diagnostics::DiagnosticBag errBag;
        require(!bytecode::decodeModule(truncated, outMod, errBag) && errBag.hasErrors(),
            "Expected error when decoding truncated RSBC buffer");
    }
    {
        bytecode::Module badDbg = decodedMod;
        diagnostics::DiagnosticBag errBag;
        badDbg.functions.front().debugInfo.sequencePoints.front().blockId = 9999;
        require(!bytecode::verifyModule(badDbg, errBag) && hasDiagCode(errBag, "RS5162"),
            "Expected RS5162 for out-of-range DebugInfo sequence point blockId");
    }
}

// ---------------------------------------------------------------------------
// 4. ModuleBuildCache Incremental Hit/Miss, Corruption Self-Healing,
//    RealScript.Memory/Weak, Partial Stub Extraction & RSMR #line Round-Trip
// ---------------------------------------------------------------------------
void testModuleBuildCacheAndRsmrLineRoundTrip() {
    const auto workRoot =
        std::filesystem::temp_directory_path() / "rs_kernel_mir_bytecode_cache_test";
    std::error_code ec;
    std::filesystem::remove_all(workRoot, ec);
    const auto cacheDir = workRoot / ".rscache";
    std::filesystem::create_directories(cacheDir);

    const std::string coreV1 = R"RS(
module Cache.Core;

public partial class CounterBox {
    public int Value = 10;
    public partial void Bump();
}

public partial class CounterBox {
    public partial void Bump() {
        Value = Value + 5;
    }
}

public int ComputeBase(int x) {
    return x * 2;
}
)RS";

    const std::string appSrc = R"RS(
module Cache.App;
import Cache.Core;
import RealScript.Memory;
import RealScript.Weak;

#line 500 "authored/AppMain.rscript"
public int RunAll(int x) {
    CounterBox box = new CounterBox();
    box.Bump();
    WeakRef<CounterBox> weakBox = WeakRef<CounterBox>.Empty();
    int[] arr = new int[2];
    arr[0] = 3;
    arr[1] = 4;
    ReadOnlySpan<int> sp = ReadOnlySpan<int>.FromArray(arr);
    if (weakBox.HasHandle()) { return -1; }
    return ComputeBase(x) + box.Value + sp.Get(0) + sp.Get(1);
}
#line default
)RS";

    const std::vector<compiler::SourceFile> v1Sources = {
        {"CacheCore.rscript", coreV1},
        {"$generated/AppMain.g.rscript", appSrc},
    };

    // Graph extraction + declaration stub check for partial module.
    const auto graph = compiler::ModuleBuildCache::extractModuleGraph(v1Sources);
    require(graph.succeeded(), "extractModuleGraph failed:\n" + formatDiags(graph.diagnostics));
    const auto coreIt = graph.modules.find("Cache.Core");
    require(coreIt != graph.modules.end(), "Missing Cache.Core in extracted graph");
    const std::string stub = compiler::ModuleBuildCache::generateDeclarationStub(coreIt->second);
    require(stub.find("class CounterBox") != std::string::npos,
        "Declaration stub missing partial class CounterBox");

    compiler::ModuleBuildCache cache(cacheDir);
    compiler::ModuleBuildCacheOptions opts;
    opts.optimizationOptions.level = optimization::Level::Basic;
    opts.flags = compiler::kBuildCacheFlagPreserveDebugInfo;
    opts.retainMirModules = true;
    opts.emitDeclarationStubs = true;

    // Build #1: cold cache misses both modules.
    const auto build1 = cache.build(v1Sources, opts);
    require(build1.succeeded(), "Initial cached build failed:\n" + formatDiags(build1.diagnostics));
    require(build1.statistics.cacheMisses == 2 && build1.statistics.cacheHits == 0,
        "Expected 2 cold misses on first build");

    // Build #2: warm cache hits both modules AND preserves RSMR #line directives in MIR + BC!
    const auto build2 = cache.build(v1Sources, opts);
    require(build2.succeeded(), "Warm cached build failed:\n" + formatDiags(build2.diagnostics));
    require(build2.statistics.cacheHits == 2 && build2.statistics.cacheMisses == 0,
        "Expected 2 warm hits on second build");
    bool hitHasLineDirectives = false;
    for (const auto& mirMod : build2.mirModules) {
        if (mirMod.name != "Cache.App") {
            continue;
        }
        for (const auto& sf : mirMod.sourceFiles) {
            if (!sf.lineDirectives.empty()) {
                hitHasLineDirectives = true;
            }
        }
    }
    require(hitHasLineDirectives,
        "ModuleBuildCache MIR overlay (RSMR) did not preserve lineDirectives across cache hit");

    // Verify execution of cached program (including partial class + WeakRef + ReadOnlySpan).
    runtime::RuntimeError linkErr;
    auto linked = runtime::ProgramImage::link(build2.bytecodeModules, linkErr);
    require(linked.has_value(), "Link from cached bytecode failed: " + linkErr.message);
    runtime::EngineRuntime rt(std::make_shared<const runtime::ProgramImage>(std::move(*linked)));
    const auto execRes = rt.invoke("Cache.App::RunAll", {runtime::Value{std::int64_t{6}}});
    require(execRes.succeeded && std::get<std::int64_t>(execRes.value) == (12 + 15 + 3 + 4),
        "Cached Cache.App::RunAll returned unexpected value: " + execRes.error.message);

    // Body-only change to Cache.Core keeps public API identical -> Cache.App stays cacheHit, Cache.Core misses!
    const std::string coreV2BodyOnly = R"RS(
module Cache.Core;

public partial class CounterBox {
    public int Value = 10;
    public partial void Bump();
}

public partial class CounterBox {
    public partial void Bump() {
        Value = Value + 5;
    }
}

public int ComputeBase(int x) {
    return x * 3;
}
)RS";
    const auto build3 = cache.build(
        {{"CacheCore.rscript", coreV2BodyOnly}, {"$generated/AppMain.g.rscript", appSrc}},
        opts);
    require(build3.succeeded(), "Body-only edit cached build failed");
    require(build3.statistics.cacheMisses == 1 && build3.statistics.cacheHits == 1,
        "Body-only edit should rebuild Cache.Core and hit Cache.App");

    // Corrupt a cached bytecode file and verify automatic self-healing recompilation.
    bool corruptedSomething = false;
    for (const auto& st : build3.moduleStatuses) {
        if (st.moduleName == "Cache.Core" && std::filesystem::exists(st.bytecodeCachePath)) {
            writeTextFile(st.bytecodeCachePath, "BAD_CORRUPT_CACHE_DATA");
            corruptedSomething = true;
        }
    }
    require(corruptedSomething, "Failed to locate cached .rsbc file to corrupt");

    const auto healed = cache.build(
        {{"CacheCore.rscript", coreV2BodyOnly}, {"$generated/AppMain.g.rscript", appSrc}},
        opts);
    require(healed.succeeded(), "Corrupted cache recovery build failed");
    require(healed.statistics.corruptedEntriesHealed >= 1,
        "Expected corruptedEntriesHealed >= 1 after corrupting cached file");

    std::filesystem::remove_all(workRoot, ec);
}

} // namespace

int main() {
    try {
        std::vector<std::pair<const char*, void (*)()>> tests = {
            {"MirVerifier and Optimizer -O0/-O2 parity", testMirVerifierAndOptimizerParity},
            {"Deterministic Record/Replay and determinismDigest", testDeterministicRecordReplayAndDigest},
            {"RSBC codec round-trip, RSLM #line trailer & verifier", testBytecodeCodecLineTrailerAndVerifier},
            {"ModuleBuildCache incremental hit/miss, healing, stubs & RSMR", testModuleBuildCacheAndRsmrLineRoundTrip},
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
