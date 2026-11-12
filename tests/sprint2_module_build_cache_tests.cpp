#include "realscript/aot_cpp/AotCpp.h"
#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/compiler/ModuleBuildCache.h"
#include "realscript/optimization/Optimizer.h"
#include "realscript/runtime/Runtime.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

using namespace realscript;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string diagnosticsText(const diagnostics::DiagnosticBag& bag) {
    std::string out;
    for (const auto& d : bag.items()) {
        out += "[" + d.code + "] " + d.message + "\n";
    }
    return out;
}

void writeTextFile(const std::filesystem::path& path, const std::string& content) {
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(out), "failed to open file for write: " + path.string());
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    require(static_cast<bool>(out), "failed to write file: " + path.string());
}

std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    require(static_cast<bool>(in), "failed to open file for read: " + path.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

runtime::ExecutionResult invokeDeterministic(
    std::vector<bytecode::Module> modules,
    const std::string& qualifiedEntry) {
    runtime::RuntimeError linkErr;
    auto linked = runtime::ProgramImage::link(std::move(modules), linkErr);
    require(
        linked.has_value(),
        "ProgramImage::link failed for '" + qualifiedEntry + "': " + linkErr.message);
    auto image = std::make_shared<const runtime::ProgramImage>(std::move(*linked));
    auto heap = std::make_shared<runtime::ManagedHeap>();
    runtime::Interpreter interp(image, heap);
    runtime::ExecutionOptions execOpts;
    execOpts.determinism.mode = runtime::DeterminismMode::Strict;
    const auto exec = interp.invoke(qualifiedEntry, {}, execOpts);
    require(
        exec.succeeded,
        "Interpreter::invoke failed for '" + qualifiedEntry + "': " + exec.error.message);
    return exec;
}

std::int64_t invokeIntEntry(
    std::vector<bytecode::Module> modules,
    const std::string& qualifiedEntry) {
    const auto exec = invokeDeterministic(std::move(modules), qualifiedEntry);
    require(
        std::holds_alternative<std::int64_t>(exec.value),
        "Expected int64 return value from '" + qualifiedEntry + "'");
    return std::get<std::int64_t>(exec.value);
}

std::vector<bytecode::Module> buildUncachedBytecode(
    const std::vector<compiler::SourceFile>& sources,
    const optimization::Options& optOptions) {
    compiler::Compilation comp(sources);
    auto built = comp.build();
    require(
        !built.diagnostics.hasErrors(),
        "Uncached reference build failed:\n" + diagnosticsText(built.diagnostics));
    if (optOptions.level != optimization::Level::None) {
        optimization::Optimizer optimizer;
        auto optRes = optimizer.optimize(std::move(built.modules), built.diagnostics, optOptions);
        require(
            !built.diagnostics.hasErrors(),
            "Uncached reference optimization failed:\n" + diagnosticsText(built.diagnostics));
        built.modules = std::move(optRes.modules);
    }
    bytecode::Lowerer lowerer;
    std::vector<bytecode::Module> out;
    out.reserve(built.modules.size());
    for (const auto& mirMod : built.modules) {
        auto bc = lowerer.lower(mirMod);
        require(
            bytecode::verifyModule(bc, built.diagnostics),
            "Uncached bytecode verify failed for " + mirMod.name + ":\n" +
                diagnosticsText(built.diagnostics));
        out.push_back(std::move(bc));
    }
    return out;
}

const compiler::ModuleCacheEntryStatus& findStatus(
    const std::vector<compiler::ModuleCacheEntryStatus>& statuses,
    const std::string& moduleName) {
    for (const auto& s : statuses) {
        if (s.moduleName == moduleName) return s;
    }
    throw std::runtime_error("missing ModuleCacheEntryStatus for " + moduleName);
}

std::string quoteShellArg(const std::filesystem::path& p) {
    const std::string s = p.string();
#if defined(_WIN32)
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"') out += "\\\"";
        else out += c;
    }
    out += "\"";
    return out;
#else
    std::string out = "'";
    for (const char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
#endif
}

struct CmdOutput {
    int exitCode = -1;
    std::string stdoutText;
    std::string stderrText;
};

CmdOutput runCommandCapture(
    const std::string& cmdLine,
    const std::filesystem::path& tempBase) {
    static std::size_t seq = 0;
    ++seq;
    const auto outFile = tempBase / ("cmd_out_" + std::to_string(seq) + ".txt");
    const auto errFile = tempBase / ("cmd_err_" + std::to_string(seq) + ".txt");
    const std::string innerCmd =
        cmdLine + " > " + quoteShellArg(outFile) + " 2> " + quoteShellArg(errFile);
#if defined(_WIN32)
    const std::string fullCmd = "\"" + innerCmd + "\"";
#else
    const std::string& fullCmd = innerCmd;
#endif
    const int rawStatus = std::system(fullCmd.c_str());
    CmdOutput res;
    res.exitCode = rawStatus == 0 ? 0 : 1;
    if (std::filesystem::exists(outFile)) res.stdoutText = readTextFile(outFile);
    if (std::filesystem::exists(errFile)) res.stderrText = readTextFile(errFile);
    return res;
}

// ============================================================================
// 1. Verify DAG Extraction, Topological Order & Fingerprint Granularity
// ============================================================================
void testModuleGraphAndFingerprintGranularity() {
    const char* hostDecl = R"RS(
module Engine.Host;
public int GetBaseTick() { return 10; }
)RS";

    const char* coreTypes = R"RS(
module Core.Types;
import Engine.Host;

public enum DamageKind {
    Physical = 1,
    Magic = 2,
}

public struct HitPacket {
    public int baseAmount;
    private int internalNonce;
    public HitPacket(int amount) {
        baseAmount = amount;
        internalNonce = GetBaseTick();
    }
    public int Effective() { return baseAmount; }
}
)RS";

    const char* coreCombatV1 = R"RS(
module Core.Combat;
import Core.Types;

internal class SecretTable {
    public static int Scale() { return 2; }
}

internal int applyPrivateArmor(int raw) {
    return raw * SecretTable.Scale();
}

public class Combatant {
    private int power;
    public int bonus;

    public Combatant(int p, int b) {
        power = p;
        bonus = b;
    }

    private int internalCalc(int factor) {
        return applyPrivateArmor(power) + factor;
    }

    public int EvaluateStrike(HitPacket pkt, DamageKind kind) {
        int kindBoost = kind == DamageKind.Magic ? 5 : 1;
        return internalCalc(pkt.Effective()) + bonus + kindBoost;
    }
}

public int ResolveAttack(int basePower, int pktAmount) {
    Combatant c = new Combatant(basePower, 3);
    HitPacket pkt = new HitPacket(pktAmount);
    return c.EvaluateStrike(pkt, DamageKind.Magic);
}
)RS";

    const char* gameUi = R"RS(
module Game.UI;
import Core.Types;
import Core.Combat;

public int RenderDamagePreview() {
    return ResolveAttack(10, 4);
}
)RS";

    // Intentionally pass sources in reverse dependency order to test topological sorting
    const std::vector<compiler::SourceFile> sourcesV1 = {
        {"Game.UI.rscript", gameUi},
        {"Core.Combat.rscript", coreCombatV1},
        {"Core.Types.rscript", coreTypes},
        {"Engine.Host.d.rscript", hostDecl},
    };

    optimization::Options optO2;
    optO2.level = optimization::Level::Aggressive;
    const auto graphV1 = compiler::ModuleBuildCache::extractModuleGraph(sourcesV1, optO2);
    require(graphV1.succeeded(), "Graph extraction failed:\n" + diagnosticsText(graphV1.diagnostics));
    require(graphV1.topologicalOrder.size() == 4, "Expected 4 modules in topological order");

    const auto indexOf = [&](const std::string& name) -> std::size_t {
        const auto it = std::find(
            graphV1.topologicalOrder.begin(), graphV1.topologicalOrder.end(), name);
        require(it != graphV1.topologicalOrder.end(), "Module missing from topologicalOrder: " + name);
        return static_cast<std::size_t>(std::distance(graphV1.topologicalOrder.begin(), it));
    };
    require(indexOf("Engine.Host") < indexOf("Core.Types"), "Engine.Host must precede Core.Types");
    require(indexOf("Core.Types") < indexOf("Core.Combat"), "Core.Types must precede Core.Combat");
    require(indexOf("Core.Combat") < indexOf("Game.UI"), "Core.Combat must precede Game.UI");

    // Modify private helper signature, private class, private method signature, and public body
    const char* coreCombatV2PrivateChange = R"RS(
module Core.Combat;
import Core.Types;

internal class RenamedSecretFormula {
    public static int Multiplier(int extra) { return 3 + extra; }
}

internal int applyPrivateArmor(int raw, int flatBonus) {
    return raw * RenamedSecretFormula.Multiplier(1) + flatBonus;
}

public class Combatant {
    private int power;
    public int bonus;

    public Combatant(int p, int b) {
        power = p;
        bonus = b;
    }

    private int internalCalcChanged(int factor, int extra) {
        return applyPrivateArmor(power, extra) + factor;
    }

    public int EvaluateStrike(HitPacket pkt, DamageKind kind) {
        int kindBoost = kind == DamageKind.Magic ? 10 : 2;
        return internalCalcChanged(pkt.Effective(), 7) + bonus + kindBoost;
    }
}

public int ResolveAttack(int basePower, int pktAmount) {
    Combatant c = new Combatant(basePower, 3);
    HitPacket pkt = new HitPacket(pktAmount);
    return c.EvaluateStrike(pkt, DamageKind.Magic);
}
)RS";

    const std::vector<compiler::SourceFile> sourcesV2 = {
        {"Game.UI.rscript", gameUi},
        {"Core.Combat.rscript", coreCombatV2PrivateChange},
        {"Core.Types.rscript", coreTypes},
        {"Engine.Host.d.rscript", hostDecl},
    };
    const auto graphV2 = compiler::ModuleBuildCache::extractModuleGraph(sourcesV2, optO2);
    require(graphV2.succeeded(), "Graph V2 extraction failed");

    const auto& combatV1 = graphV1.modules.at("Core.Combat");
    const auto& combatV2 = graphV2.modules.at("Core.Combat");
    require(
        combatV1.implementationFingerprint != combatV2.implementationFingerprint,
        "implementationFingerprint must change when private implementation or method body changes");
    require(
        combatV1.publicFingerprint == combatV2.publicFingerprint,
        "publicFingerprint MUST remain identical when only private functions/classes/methods or public bodies change");

    // Changing optimization level must change implementationFingerprint while preserving publicFingerprint
    optimization::Options optO0;
    optO0.level = optimization::Level::None;
    const auto graphO0 = compiler::ModuleBuildCache::extractModuleGraph(sourcesV1, optO0);
    require(
        graphO0.modules.at("Core.Combat").implementationFingerprint != combatV1.implementationFingerprint,
        "implementationFingerprint must incorporate optimization level");
    require(
        graphO0.modules.at("Core.Combat").publicFingerprint == combatV1.publicFingerprint,
        "publicFingerprint must not depend on optimization level");

    // P0-2 / P2-2: Interface method declaration order MUST affect publicFingerprint (interfaceSlot order!)
    const auto ifaceGraph1 = compiler::ModuleBuildCache::extractModuleGraph({
        {"iface.rscript", "module Core.Iface;\npublic interface IOrdered {\n    int Alpha(int x);\n    int Beta(int x);\n}\n"},
    });
    const auto ifaceGraph2 = compiler::ModuleBuildCache::extractModuleGraph({
        {"iface.rscript", "module Core.Iface;\npublic interface IOrdered {\n    int Beta(int x);\n    int Alpha(int x);\n}\n"},
    });
    require(ifaceGraph1.succeeded() && ifaceGraph2.succeeded(), "Interface graph extraction failed");
    require(
        ifaceGraph1.modules.at("Core.Iface").publicFingerprint !=
            ifaceGraph2.modules.at("Core.Iface").publicFingerprint,
        "Interface method declaration order MUST change publicFingerprint because it governs interfaceSlot indices");

    // P0-2 / P2-2: Operator overloads MUST appear in .d.rscript stub and publicFingerprint
    const auto opGraph1 = compiler::ModuleBuildCache::extractModuleGraph({
        {"op.rscript", R"RS(
module Core.Ops;
public struct Meter {
    public int v;
    public Meter(int x) { v = x; }
    public static Meter operator +(Meter a, Meter b) { return new Meter(a.v + b.v); }
    public static implicit operator Meter(int x) { return new Meter(x); }
}
)RS"},
    });
    const auto opGraph2 = compiler::ModuleBuildCache::extractModuleGraph({
        {"op.rscript", R"RS(
module Core.Ops;
public struct Meter {
    public int v;
    public Meter(int x) { v = x; }
    public static Meter operator -(Meter a, Meter b) { return new Meter(a.v - b.v); }
    public static explicit operator Meter(int x) { return new Meter(x); }
}
)RS"},
    });
    require(opGraph1.succeeded() && opGraph2.succeeded(), "Operator graph extraction failed");
    const auto& opInfo1 = opGraph1.modules.at("Core.Ops");
    require(
        opInfo1.declarationStub.find("operator +") != std::string::npos &&
            opInfo1.declarationStub.find("implicit operator") != std::string::npos,
        "Generated .d.rscript stub must include public binary and conversion operators:\n" +
            opInfo1.declarationStub);
    require(
        opInfo1.publicFingerprint != opGraph2.modules.at("Core.Ops").publicFingerprint,
        "Operator overload changes must alter publicFingerprint");

    // Verify circular import cycle detection
    const auto cycleGraph = compiler::ModuleBuildCache::extractModuleGraph({
        {"a.rscript", "module Cycle.A;\nimport Cycle.B;\npublic int f() { return 1; }\n"},
        {"b.rscript", "module Cycle.B;\nimport Cycle.A;\npublic int g() { return 2; }\n"},
    });
    require(!cycleGraph.succeeded(), "Circular module import should fail DAG validation");
}

// ============================================================================
// 2. Cold Build, Warm Full Hit, Upstream Private vs Public Edits & .d.rscript Stubs
// ============================================================================
void testIncrementalModuleBuildCacheLifecycle(const std::filesystem::path& workRoot) {
    const auto cacheDir = workRoot / "lifecycle_cache";
    std::error_code ec;
    std::filesystem::remove_all(cacheDir, ec);

    const std::string coreTypesV1 = R"RS(
module Core.Types;

public enum Element {
    Neutral = 0,
    Fire = 3,
}

public struct DamageSpec {
    public int flat;
    private int flags;
    public DamageSpec(int f) {
        flat = f;
        flags = 1;
    }
    public int Value() { return flat; }
}
)RS";

    const std::string coreCombatV1 = R"RS(
module Core.Combat;
import Core.Types;

internal int computeCritBonus(int baseVal) {
    return baseVal * 2;
}

public class Fighter {
    private int strength;
    public int rank;

    public Fighter(int str, int r) {
        strength = str;
        rank = r;
    }

    private int internalPower() {
        return computeCritBonus(strength) + rank;
    }

    public int DealDamage(DamageSpec spec, Element elem) {
        int elemBonus = elem == Element.Fire ? 5 : 0;
        return internalPower() + spec.Value() + elemBonus;
    }
}

public int SimulateHit(int str, int specFlat) {
    Fighter f = new Fighter(str, 4);
    DamageSpec spec = new DamageSpec(specFlat);
    return f.DealDamage(spec, Element.Fire);
}
)RS";

    const std::string gameUiV1 = R"RS(
module Game.UI;
import Core.Types;
import Core.Combat;

public int QueryHudValue() {
    return SimulateHit(10, 6) + 100;
}
)RS";

    const std::string gameAiV1 = R"RS(
module Game.AI;
import Core.Types;
import Core.Combat;

public int ScoreThreat() {
    Fighter boss = new Fighter(15, 2);
    DamageSpec spec = new DamageSpec(8);
    return boss.DealDamage(spec, Element.Fire);
}
)RS";

    std::vector<compiler::SourceFile> sources = {
        {"Core.Types.rscript", coreTypesV1},
        {"Core.Combat.rscript", coreCombatV1},
        {"Game.UI.rscript", gameUiV1},
        {"Game.AI.rscript", gameAiV1},
    };

    compiler::ModuleBuildCache cache(cacheDir);
    compiler::ModuleBuildCacheOptions opts;
    opts.optimizationOptions.level = optimization::Level::Aggressive;

    // Step 1: Cold build -> 0 hits, 4 misses
    auto cold = cache.build(sources, opts);
    require(cold.succeeded(), "Cold build failed:\n" + diagnosticsText(cold.diagnostics));
    require(cold.statistics.totalModules == 4, "Expected 4 modules");
    require(cold.statistics.cacheHits == 0, "Cold build must have 0 hits");
    require(cold.statistics.cacheMisses == 4, "Cold build must have 4 misses");
    for (const auto& st : cold.moduleStatuses) {
        require(!st.cacheHit, "Cold module should not report cacheHit");
        require(
            std::filesystem::exists(st.bytecodeCachePath),
            "Expected persisted .rsbc file on disk: " + st.bytecodeCachePath.string());
    }
    require(invokeIntEntry(cold.bytecodeModules, "Game.UI::QueryHudValue") == 135, "Unexpected cold UI result");
    require(invokeIntEntry(cold.bytecodeModules, "Game.AI::ScoreThreat") == 45, "Unexpected cold AI result");

    // Step 2: Second build -> full cache hit (4 hits, 0 misses, 0 optimizer work)
    auto warm = cache.build(sources, opts);
    require(warm.succeeded(), "Warm build failed:\n" + diagnosticsText(warm.diagnostics));
    require(warm.statistics.cacheHits == 4, "Second build must hit all 4 modules");
    require(warm.statistics.cacheMisses == 0, "Second build must have 0 misses");
    require(
        warm.optimizationStatistics.functionsVisited == 0,
        "Full cache hit must bypass optimizer completely");
    require(invokeIntEntry(warm.bytecodeModules, "Game.UI::QueryHudValue") == 135, "Unexpected warm UI result");
    require(invokeIntEntry(warm.bytecodeModules, "Game.AI::ScoreThreat") == 45, "Unexpected warm AI result");

    const auto uiCacheKeyBefore = findStatus(warm.moduleStatuses, "Game.UI").cacheKey;
    const auto aiCacheKeyBefore = findStatus(warm.moduleStatuses, "Game.AI").cacheKey;
    const auto combatPubFpBefore = findStatus(warm.moduleStatuses, "Core.Combat").publicFingerprint;

    // Step 3: Modify Core.Combat private helper + private method + public method BODY (same public signature!)
    const std::string coreCombatV2PrivateEdit = R"RS(
module Core.Combat;
import Core.Types;

internal int computeCritBonusChanged(int baseVal, int extraScale) {
    return baseVal * extraScale + 10;
}

public class Fighter {
    private int strength;
    public int rank;

    public Fighter(int str, int r) {
        strength = str;
        rank = r;
    }

    private int internalPowerV2(int mult) {
        return computeCritBonusChanged(strength, mult) + rank;
    }

    public int DealDamage(DamageSpec spec, Element elem) {
        int elemBonus = elem == Element.Fire ? 20 : 0;
        return internalPowerV2(3) + spec.Value() + elemBonus;
    }
}

public int SimulateHit(int str, int specFlat) {
    Fighter f = new Fighter(str, 4);
    DamageSpec spec = new DamageSpec(specFlat);
    return f.DealDamage(spec, Element.Fire);
}
)RS";
    sources[1].content = coreCombatV2PrivateEdit;

    auto privateEditBuild = cache.build(sources, opts);
    require(
        privateEditBuild.succeeded(),
        "Private-edit incremental build failed:\n" + diagnosticsText(privateEditBuild.diagnostics));
    require(
        privateEditBuild.statistics.cacheHits == 3 &&
            privateEditBuild.statistics.cacheMisses == 1,
        "Expected 3 cache hits (Core.Types, Game.UI, Game.AI) and 1 miss (Core.Combat), got hits=" +
            std::to_string(privateEditBuild.statistics.cacheHits) + " misses=" +
            std::to_string(privateEditBuild.statistics.cacheMisses));

    const auto& typesAfterPriv = findStatus(privateEditBuild.moduleStatuses, "Core.Types");
    const auto& combatAfterPriv = findStatus(privateEditBuild.moduleStatuses, "Core.Combat");
    const auto& uiAfterPriv = findStatus(privateEditBuild.moduleStatuses, "Game.UI");
    const auto& aiAfterPriv = findStatus(privateEditBuild.moduleStatuses, "Game.AI");

    require(typesAfterPriv.cacheHit, "Core.Types must hit cache");
    require(!combatAfterPriv.cacheHit, "Core.Combat must recompile on private implementation edit");
    require(
        combatAfterPriv.usedDeclarationStubs,
        "Core.Combat recompilation must use on-demand .d.rscript stub for clean upstream Core.Types");
    require(
        combatAfterPriv.publicFingerprint == combatPubFpBefore,
        "Core.Combat publicFingerprint must be unchanged");
    require(uiAfterPriv.cacheHit && uiAfterPriv.cacheKey == uiCacheKeyBefore, "Game.UI must hit existing cache");
    require(aiAfterPriv.cacheHit && aiAfterPriv.cacheKey == aiCacheKeyBefore, "Game.AI must hit existing cache");

    require(
        invokeIntEntry(privateEditBuild.bytecodeModules, "Game.UI::QueryHudValue") == 170,
        "Cached Game.UI must link with recompiled Core.Combat and reflect updated implementation");
    require(
        invokeIntEntry(privateEditBuild.bytecodeModules, "Game.AI::ScoreThreat") == 85,
        "Cached Game.AI must link with recompiled Core.Combat and reflect updated implementation");

    // Step 4: Modify ONLY leaf Game.UI -> verify on-demand .d.rscript stub compilation for Core.Combat & Core.Types
    sources[2].content = R"RS(
module Game.UI;
import Core.Types;
import Core.Combat;

public int QueryHudValue() {
    return SimulateHit(10, 6) + 250;
}
)RS";
    auto leafEditBuild = cache.build(sources, opts);
    require(leafEditBuild.succeeded(), "Leaf edit build failed:\n" + diagnosticsText(leafEditBuild.diagnostics));
    require(
        leafEditBuild.statistics.cacheHits == 3 && leafEditBuild.statistics.cacheMisses == 1,
        "Modifying only Game.UI must hit Core.Types, Core.Combat, Game.AI (3 hits, 1 miss)");
    const auto& uiAfterLeaf = findStatus(leafEditBuild.moduleStatuses, "Game.UI");
    require(!uiAfterLeaf.cacheHit && uiAfterLeaf.usedDeclarationStubs, "Game.UI must recompile using upstream .d.rscript stubs");
    require(
        std::filesystem::exists(findStatus(leafEditBuild.moduleStatuses, "Core.Combat").stubCachePath),
        "Core.Combat .d.rscript stub should be materialized on demand");
    require(invokeIntEntry(leafEditBuild.bytecodeModules, "Game.UI::QueryHudValue") == 320, "Unexpected leaf UI value");

    // Also verify standalone compilation of Game.UI using ONLY materialized .d.rscript stubs
    const auto extractedForStubs = compiler::ModuleBuildCache::extractModuleGraph(sources, opts.optimizationOptions);
    const auto typesStubPath = cache.materializeDeclarationStub(extractedForStubs.modules.at("Core.Types"));
    const auto combatStubPath = cache.materializeDeclarationStub(extractedForStubs.modules.at("Core.Combat"));
    require(std::filesystem::exists(typesStubPath) && std::filesystem::exists(combatStubPath), "Stub files must exist");
    compiler::Compilation standaloneComp({
        {typesStubPath.string(), readTextFile(typesStubPath)},
        {combatStubPath.string(), readTextFile(combatStubPath)},
        {"Game.UI.rscript", sources[2].content},
    });
    const auto standaloneRes = standaloneComp.build();
    require(
        !standaloneRes.diagnostics.hasErrors(),
        "Standalone single-module compilation against .d.rscript stubs failed:\n" +
            diagnosticsText(standaloneRes.diagnostics));

    // Step 5: Modify Core.Combat PUBLIC signature -> downstream Game.UI & Game.AI MUST invalidate and recompile!
    const std::string coreCombatV3PublicSigChange = R"RS(
module Core.Combat;
import Core.Types;

public class Fighter {
    private int strength;
    public int rank;

    public Fighter(int str, int r) {
        strength = str;
        rank = r;
    }

    public int DealDamage(DamageSpec spec, Element elem, int armorPen = 5) {
        int elemBonus = elem == Element.Fire ? 20 : 0;
        return strength * 3 + 10 + rank + spec.Value() + elemBonus + armorPen;
    }
}

public int SimulateHit(int str, int specFlat, int extraFlat = 1) {
    Fighter f = new Fighter(str, 4);
    DamageSpec spec = new DamageSpec(specFlat);
    return f.DealDamage(spec, Element.Fire) + extraFlat;
}
)RS";
    sources[1].content = coreCombatV3PublicSigChange;

    auto pubChangeBuild = cache.build(sources, opts);
    require(
        pubChangeBuild.succeeded(),
        "Public signature change build failed:\n" + diagnosticsText(pubChangeBuild.diagnostics));
    require(
        pubChangeBuild.statistics.cacheHits == 1 &&
            pubChangeBuild.statistics.cacheMisses == 3,
        "Public signature change on Core.Combat must keep Core.Types cached (1 hit) and invalidate Core.Combat, Game.UI, Game.AI (3 misses)");
    require(findStatus(pubChangeBuild.moduleStatuses, "Core.Types").cacheHit, "Core.Types must remain cached");
    require(!findStatus(pubChangeBuild.moduleStatuses, "Core.Combat").cacheHit, "Core.Combat must recompile");
    require(!findStatus(pubChangeBuild.moduleStatuses, "Game.UI").cacheHit, "Game.UI must invalidate & recompile");
    require(!findStatus(pubChangeBuild.moduleStatuses, "Game.AI").cacheHit, "Game.AI must invalidate & recompile");
    require(
        findStatus(pubChangeBuild.moduleStatuses, "Core.Combat").publicFingerprint != combatPubFpBefore,
        "Core.Combat publicFingerprint must change when public method signature changes");

    require(invokeIntEntry(pubChangeBuild.bytecodeModules, "Game.UI::QueryHudValue") == 326, "Updated public signature UI value mismatch");
    require(invokeIntEntry(pubChangeBuild.bytecodeModules, "Game.AI::ScoreThreat") == 90, "Updated public signature AI value mismatch");

    // Step 6: Modify Core.Types enum constant & struct layout -> all 4 modules invalidate!
    sources[0].content = R"RS(
module Core.Types;

public enum Element {
    Neutral = 0,
    Fire = 99,
}

public struct DamageSpec {
    public int flat;
    public int bonusFlat;
    private int flags;
    public DamageSpec(int f) {
        flat = f;
        bonusFlat = 2;
        flags = 1;
    }
    public int Value() { return flat + bonusFlat; }
}
)RS";
    auto rootLayoutChange = cache.build(sources, opts);
    require(rootLayoutChange.succeeded(), "Root struct layout change failed");
    require(
        rootLayoutChange.statistics.cacheHits == 0 && rootLayoutChange.statistics.cacheMisses == 4,
        "Root struct layout / enum constant change must transitively invalidate all 4 modules");
    require(invokeIntEntry(rootLayoutChange.bytecodeModules, "Game.UI::QueryHudValue") == 328, "Struct layout update UI value mismatch");
}

// ============================================================================
// 3. Corrupted Cache File Detection & Automatic Self-Healing Recompilation
// ============================================================================
void testCorruptedCacheSelfHealing(const std::filesystem::path& workRoot) {
    const auto cacheDir = workRoot / "heal_cache";
    std::error_code ec;
    std::filesystem::remove_all(cacheDir, ec);

    const std::vector<compiler::SourceFile> sources = {
        {"ModA.rscript", "module Heal.A;\npublic int BaseVal() { return 21; }\n"},
        {"ModB.rscript", "module Heal.B;\nimport Heal.A;\npublic int Main() { return BaseVal() * 2; }\n"},
    };

    compiler::ModuleBuildCache cache(cacheDir);
    compiler::ModuleBuildCacheOptions opts;
    opts.optimizationOptions.level = optimization::Level::Basic;

    auto initial = cache.build(sources, opts);
    require(initial.succeeded() && initial.statistics.cacheMisses == 2, "Initial heal setup failed");
    require(invokeIntEntry(initial.bytecodeModules, "Heal.B::Main") == 42, "Initial heal run mismatch");

    const auto modBRsbcPath = findStatus(initial.moduleStatuses, "Heal.B").bytecodeCachePath;
    require(std::filesystem::exists(modBRsbcPath), "Heal.B .rsbc must exist");

    // Remove opt graph bundle so warm build validates standalone per-module .rsbc checksum!
    std::filesystem::remove_all(cacheDir / "graphs", ec);

    // Case A: Flip a single bit inside Heal.B's .rsbc payload (checksum failure -> auto heal!)
    {
        std::fstream fs(modBRsbcPath, std::ios::in | std::ios::out | std::ios::binary);
        require(static_cast<bool>(fs), "Failed to open .rsbc for corruption test");
        fs.seekg(0, std::ios::end);
        const auto len = static_cast<std::size_t>(fs.tellg());
        require(len > 20, "Unexpectedly tiny .rsbc");
        fs.seekg(static_cast<std::streamoff>(len - 5));
        char byte = 0;
        fs.read(&byte, 1);
        byte ^= static_cast<char>(0x5Au);
        fs.seekp(static_cast<std::streamoff>(len - 5));
        fs.write(&byte, 1);
    }

    auto healedBitFlip = cache.build(sources, opts);
    require(healedBitFlip.succeeded(), "Bit-flipped .rsbc must auto-heal without failing build");
    require(
        healedBitFlip.statistics.cacheHits == 1 &&
            healedBitFlip.statistics.cacheMisses == 1 &&
            healedBitFlip.statistics.corruptedEntriesHealed == 1,
        "Bit-flipped Heal.B .rsbc must hit Heal.A (1 hit) and heal+recompile Heal.B (1 miss, 1 healed)");
    require(
        findStatus(healedBitFlip.moduleStatuses, "Heal.B").healedCorruptedCache,
        "Heal.B status must report healedCorruptedCache=true");
    require(invokeIntEntry(healedBitFlip.bytecodeModules, "Heal.B::Main") == 42, "Healed bytecode produced wrong result");

    // Subsequent build must be 100% clean cache hit!
    auto postHeal = cache.build(sources, opts);
    require(
        postHeal.succeeded() &&
            postHeal.statistics.cacheHits == 2 &&
            postHeal.statistics.cacheMisses == 0 &&
            postHeal.statistics.corruptedEntriesHealed == 0,
        "Post-heal build must hit all repaired cache entries cleanly");

    // Case B: Truncate Heal.A's .rsbc to 4 bytes (and clear graphs bundle so per-module .rsbc is verified)
    std::filesystem::remove_all(cacheDir / "graphs", ec);
    const auto modARsbcPath = findStatus(postHeal.moduleStatuses, "Heal.A").bytecodeCachePath;
    writeTextFile(modARsbcPath, "RSBC");
    auto healedTruncate = cache.build(sources, opts);
    require(
        healedTruncate.succeeded() &&
            healedTruncate.statistics.cacheHits == 1 &&
            healedTruncate.statistics.cacheMisses == 1 &&
            healedTruncate.statistics.corruptedEntriesHealed == 1,
        "Truncated .rsbc must auto-heal and recompile cleanly");
    require(invokeIntEntry(healedTruncate.bytecodeModules, "Heal.B::Main") == 42, "Post-truncate invoke failed");

    // Case C: Corrupt AOT cache bundle -> buildAot must auto-heal!
    aot::GenerationOptions aotOpts;
    aotOpts.programName = "HealAotProg";
    auto aotCold = cache.buildAot(sources, opts, aotOpts);
    require(aotCold.succeeded() && !aotCold.aotCacheHit, "Initial buildAot failed");
    auto aotWarm = cache.buildAot(sources, opts, aotOpts);
    require(aotWarm.succeeded() && aotWarm.aotCacheHit && aotWarm.statistics.aotOutputSkipped, "Warm buildAot must hit AOT cache");

    for (const auto& entry : std::filesystem::recursive_directory_iterator(cacheDir / "aot")) {
        if (entry.is_regular_file()) {
            writeTextFile(entry.path(), "CORRUPTED_AOT_BUNDLE");
        }
    }
    auto aotHealed = cache.buildAot(sources, opts, aotOpts);
    require(
        aotHealed.succeeded() &&
            aotHealed.statistics.corruptedEntriesHealed >= 1 &&
            aotHealed.program.contentHash == aotCold.program.contentHash,
        "Corrupted AOT bundle must auto-heal and regenerate identical AOT output");
}

// ============================================================================
// 4. End-to-End CLI Tests: `rsc --cache-dir --cache-stats` & `rsaot --cache-dir --cache-stats`
// ============================================================================
void testCliEndToEnd(
    const std::filesystem::path& workRoot,
    const std::filesystem::path& rscBin,
    const std::filesystem::path& rsaotBin) {
    require(std::filesystem::exists(rscBin), "rsc binary not found at: " + rscBin.string());
    require(std::filesystem::exists(rsaotBin), "rsaot binary not found at: " + rsaotBin.string());

    const auto cliDir = workRoot / "cli_e2e";
    const auto srcDir = cliDir / "src";
    const auto cacheDir = cliDir / "cache";
    const auto bcOutDir = cliDir / "bc_out";
    const auto aotOutDir = cliDir / "aot_out";
    std::error_code ec;
    std::filesystem::remove_all(cliDir, ec);
    std::filesystem::create_directories(srcDir, ec);

    const auto corePath = srcDir / "Core.Combat.rscript";
    const auto uiPath = srcDir / "Game.UI.rscript";
    const auto aiPath = srcDir / "Game.AI.rscript";

    writeTextFile(corePath, R"RS(
module Core.Combat;

internal int internalBonus(int x) {
    return x + 5;
}

public int CalcPower(int baseVal) {
    return internalBonus(baseVal) * 2;
}
)RS");

    writeTextFile(uiPath, R"RS(
module Game.UI;
import Core.Combat;

public int HudScore() {
    return CalcPower(10) + 1;
}
)RS");

    writeTextFile(aiPath, R"RS(
module Game.AI;
import Core.Combat;

public int AiDecision() {
    return CalcPower(7) + HudFallback();
}

internal int HudFallback() {
    return 3;
}
)RS");

    const std::string srcArgs =
        quoteShellArg(corePath) + " " + quoteShellArg(uiPath) + " " + quoteShellArg(aiPath);

    // 1) rsc cold build + --run
    const auto rscCold = runCommandCapture(
        quoteShellArg(rscBin) + " " + srcArgs +
            " --run Game.UI::HudScore --opt-level 2 --cache-dir " + quoteShellArg(cacheDir) +
            " --cache-stats",
        cliDir);
    require(rscCold.exitCode == 0, "rsc cold failed:\n" + rscCold.stderrText);
    require(rscCold.stdoutText.find("31") != std::string::npos, "Expected stdout 31, got: " + rscCold.stdoutText);
    require(
        rscCold.stderrText.find("cacheHits=0") != std::string::npos &&
            rscCold.stderrText.find("cacheMisses=3") != std::string::npos,
        "Expected cacheHits=0 cacheMisses=3 in stderr, got: " + rscCold.stderrText);

    // 2) rsc warm build + --run -> full hit
    const auto rscWarm = runCommandCapture(
        quoteShellArg(rscBin) + " " + srcArgs +
            " --run Game.UI::HudScore --opt-level 2 --cache-dir " + quoteShellArg(cacheDir) +
            " --cache-stats",
        cliDir);
    require(rscWarm.exitCode == 0, "rsc warm failed:\n" + rscWarm.stderrText);
    require(rscWarm.stdoutText.find("31") != std::string::npos, "Expected stdout 31 on warm run");
    require(
        rscWarm.stderrText.find("cacheHits=3") != std::string::npos &&
            rscWarm.stderrText.find("cacheMisses=0") != std::string::npos,
        "Expected cacheHits=3 cacheMisses=0 on warm run, got: " + rscWarm.stderrText);

    // 3) Modify Core.Combat private implementation -> downstream Game.UI & Game.AI stay cache hits!
    writeTextFile(corePath, R"RS(
module Core.Combat;

internal int internalBonus(int x, int flat) {
    return x + flat;
}

public int CalcPower(int baseVal) {
    return internalBonus(baseVal, 20) * 2;
}
)RS");

    const auto rscPrivEdit = runCommandCapture(
        quoteShellArg(rscBin) + " " + srcArgs +
            " --run Game.UI::HudScore --opt-level 2 --cache-dir " + quoteShellArg(cacheDir) +
            " --cache-stats",
        cliDir);
    require(rscPrivEdit.exitCode == 0, "rsc private-edit run failed:\n" + rscPrivEdit.stderrText);
    require(rscPrivEdit.stdoutText.find("61") != std::string::npos, "Expected updated stdout 61, got: " + rscPrivEdit.stdoutText);
    require(
        rscPrivEdit.stderrText.find("cacheHits=2") != std::string::npos &&
            rscPrivEdit.stderrText.find("cacheMisses=1") != std::string::npos,
        "Expected cacheHits=2 cacheMisses=1 after private edit, got: " + rscPrivEdit.stderrText);

    // Emit bytecode directory from warm cache and verify disassemble works
    const auto rscEmitDir = runCommandCapture(
        quoteShellArg(rscBin) + " " + srcArgs +
            " --emit-bytecode-dir " + quoteShellArg(bcOutDir) +
            " --opt-level 2 --cache-dir " + quoteShellArg(cacheDir) + " --cache-stats",
        cliDir);
    require(rscEmitDir.exitCode == 0, "rsc --emit-bytecode-dir failed:\n" + rscEmitDir.stderrText);
    require(
        rscEmitDir.stderrText.find("cacheHits=3") != std::string::npos &&
            rscEmitDir.stderrText.find("cacheMisses=0") != std::string::npos,
        "Expected 3 cache hits on --emit-bytecode-dir");
    const auto disasmRes = runCommandCapture(
        quoteShellArg(rscBin) + " " + quoteShellArg(bcOutDir / "Game.UI.rsbc") + " --disassemble",
        cliDir);
    require(disasmRes.exitCode == 0 && disasmRes.stdoutText.find("HudScore") != std::string::npos, "Disassembly failed");

    // 4) rsaot incremental caching & output skip verification
    const auto aotCacheDir = cliDir / "aot_only_cache";
    const auto aotCold = runCommandCapture(
        quoteShellArg(rsaotBin) +
            " --output-dir " + quoteShellArg(aotOutDir) +
            " --program-name CacheDemo" +
            " --opt-level 2" +
            " --cache-dir " + quoteShellArg(aotCacheDir) +
            " --cache-stats " + srcArgs,
        cliDir);
    require(aotCold.exitCode == 0, "rsaot cold failed:\n" + aotCold.stderrText);
    require(
        aotCold.stderrText.find("cacheHits=0") != std::string::npos &&
            aotCold.stderrText.find("cacheMisses=3") != std::string::npos,
        "Expected rsaot cold cacheHits=0 cacheMisses=3, got: " + aotCold.stderrText);
    require(
        aotCold.stdoutText.find("(updated)") != std::string::npos,
        "Expected rsaot cold stdout to report (updated), got: " + aotCold.stdoutText);

    const auto genCppPath = aotOutDir / "realscript_aot_generated.cpp";
    require(std::filesystem::exists(genCppPath), "Generated AOT .cpp must exist");
    const auto mtimeBefore = std::filesystem::last_write_time(genCppPath);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Warm rsaot -> must hit all 3 modules AND skip rewriting unchanged AOT files!
    const auto aotWarm = runCommandCapture(
        quoteShellArg(rsaotBin) +
            " --output-dir " + quoteShellArg(aotOutDir) +
            " --program-name CacheDemo" +
            " --opt-level 2" +
            " --cache-dir " + quoteShellArg(aotCacheDir) +
            " --cache-stats " + srcArgs,
        cliDir);
    require(aotWarm.exitCode == 0, "rsaot warm failed:\n" + aotWarm.stderrText);
    require(
        aotWarm.stderrText.find("cacheHits=3") != std::string::npos &&
            aotWarm.stderrText.find("cacheMisses=0") != std::string::npos,
        "Expected rsaot warm cacheHits=3 cacheMisses=0, got: " + aotWarm.stderrText);
    require(
        aotWarm.stdoutText.find("(unchanged)") != std::string::npos,
        "Expected rsaot warm stdout to report (unchanged), got: " + aotWarm.stdoutText);
    const auto mtimeAfter = std::filesystem::last_write_time(genCppPath);
    require(mtimeBefore == mtimeAfter, "Incremental AOT skip must preserve output file mtime");

    // Modify Core.Combat private body -> rsaot must reuse Game.UI & Game.AI MIR cache (2 hits, 1 miss) and update C++ output!
    writeTextFile(corePath, R"RS(
module Core.Combat;

internal int internalBonus(int x, int flat) {
    return x + flat + 100;
}

public int CalcPower(int baseVal) {
    return internalBonus(baseVal, 20) * 2;
}
)RS");
    const auto aotPartial = runCommandCapture(
        quoteShellArg(rsaotBin) +
            " --output-dir " + quoteShellArg(aotOutDir) +
            " --program-name CacheDemo" +
            " --opt-level 2" +
            " --cache-dir " + quoteShellArg(aotCacheDir) +
            " --cache-stats " + srcArgs,
        cliDir);
    require(aotPartial.exitCode == 0, "rsaot partial incremental failed:\n" + aotPartial.stderrText);
    require(
        aotPartial.stderrText.find("cacheHits=2") != std::string::npos &&
            aotPartial.stderrText.find("cacheMisses=1") != std::string::npos,
        "Expected rsaot partial cacheHits=2 cacheMisses=1, got: " + aotPartial.stderrText);
    require(
        aotPartial.stdoutText.find("(updated)") != std::string::npos,
        "Expected rsaot partial stdout to report (updated), got: " + aotPartial.stdoutText);

    // Modify Core.Combat PUBLIC signature -> rsaot must invalidate all 3 modules (0 hits, 3 misses)
    writeTextFile(corePath, R"RS(
module Core.Combat;

public int CalcPower(int baseVal, int extra = 0) {
    return (baseVal + 120 + extra) * 2;
}
)RS");
    const auto aotPubInvalidation = runCommandCapture(
        quoteShellArg(rsaotBin) +
            " --output-dir " + quoteShellArg(aotOutDir) +
            " --program-name CacheDemo" +
            " --opt-level 2" +
            " --cache-dir " + quoteShellArg(aotCacheDir) +
            " --cache-stats " + srcArgs,
        cliDir);
    require(aotPubInvalidation.exitCode == 0, "rsaot public invalidation failed:\n" + aotPubInvalidation.stderrText);
    require(
        aotPubInvalidation.stderrText.find("cacheHits=0") != std::string::npos &&
            aotPubInvalidation.stderrText.find("cacheMisses=3") != std::string::npos,
        "Expected rsaot public invalidation cacheHits=0 cacheMisses=3, got: " + aotPubInvalidation.stderrText);
}

// ============================================================================
// 5. Inheritance, Interfaces, Built-in Math & AOT C++ Native Execution Parity
// ============================================================================
void testRichObjectModelAndBuiltinsCache(const std::filesystem::path& workRoot) {
    const auto cacheDir = workRoot / "rich_cache";
    std::error_code ec;
    std::filesystem::remove_all(cacheDir, ec);

    std::vector<compiler::SourceFile> sources = {
        {"Core.Contracts.rscript", R"RS(
module Core.Contracts;
import RealScript.Math;

internal struct InternalTag {
    int code;
}

public interface IScorer {
    int Score(int baseVal);
}

public class BaseUnit : IScorer {
    private InternalTag tag;
    public int BaseScore { get; set; } = 10;

    public BaseUnit(int initial) {
        BaseScore = initial;
    }

    private int secretOffset() {
        return Math.Abs(-1) + Math.Clamp(9, 0, 5);
    }

    public virtual int Score(int baseVal) {
        return BaseScore + baseVal + secretOffset();
    }
}
)RS"},
        {"Game.Units.rscript", R"RS(
module Game.Units;
import Core.Contracts;

public class EliteUnit : BaseUnit {
    public EliteUnit(int initial) : base(initial) {}

    public override int Score(int baseVal) {
        return base.Score(baseVal) * 2;
    }
}

public int RunElite() {
    IScorer s = new EliteUnit(5);
    return s.Score(4);
}
)RS"},
    };

    compiler::ModuleBuildCache cache(cacheDir);
    compiler::ModuleBuildCacheOptions opts;
    opts.optimizationOptions.level = optimization::Level::Aggressive;

    auto cold = cache.build(sources, opts);
    require(cold.succeeded(), "Rich cold build failed:\n" + diagnosticsText(cold.diagnostics));
    require(cold.statistics.cacheHits == 0 && cold.statistics.cacheMisses == 2, "Rich cold stats mismatch");
    require(invokeIntEntry(cold.bytecodeModules, "Game.Units::RunElite") == 30, "Rich cold execution mismatch");

    // Warm build (also verifies built-in RealScript.Math silent cache reload!)
    auto warm = cache.build(sources, opts);
    require(warm.succeeded(), "Rich warm build failed");
    require(warm.statistics.cacheHits == 2 && warm.statistics.cacheMisses == 0, "Rich warm stats mismatch");
    require(invokeIntEntry(warm.bytecodeModules, "Game.Units::RunElite") == 30, "Rich warm execution mismatch");

    // Modify BaseUnit private method body -> Game.Units MUST hit cache and link cleanly with inherited vtable & interface!
    sources[0].content = R"RS(
module Core.Contracts;
import RealScript.Math;

internal struct InternalTag {
    int code;
}

public interface IScorer {
    int Score(int baseVal);
}

public class BaseUnit : IScorer {
    private InternalTag tag;
    public int BaseScore { get; set; } = 10;

    public BaseUnit(int initial) {
        BaseScore = initial;
    }

    private int secretOffsetChanged(int k) {
        return Math.Abs(-4) + Math.Max(2, 6) + k;
    }

    public virtual int Score(int baseVal) {
        return BaseScore + baseVal + secretOffsetChanged(1);
    }
}
)RS";
    auto privUpdate = cache.build(sources, opts);
    require(privUpdate.succeeded(), "Rich private update failed:\n" + diagnosticsText(privUpdate.diagnostics));
    require(
        privUpdate.statistics.cacheHits == 1 && privUpdate.statistics.cacheMisses == 1,
        "Game.Units must stay cached when BaseUnit changes private helper");
    require(findStatus(privUpdate.moduleStatuses, "Game.Units").cacheHit, "Game.Units must be cache hit");
    require(invokeIntEntry(privUpdate.bytecodeModules, "Game.Units::RunElite") == 40, "Rich updated execution mismatch");
}

// ============================================================================
// 6. P0-2 & P2-2: RealScript.Resource, Cross-Module Generics / Awaitable<T> /
//    ResourceHandle<T>, Unified -O2 Gas/Digest Parity & AOT Shared Generics Key
// ============================================================================
void testBuiltinResourceCrossModuleGenericsAndO2Parity(const std::filesystem::path& workRoot) {
    // 6A. Explicit `import RealScript.Resource;` + implicit `using` / `IDisposable`
    {
        const auto resCacheDir = workRoot / "resource_builtin_cache";
        std::error_code ec;
        std::filesystem::remove_all(resCacheDir, ec);

        std::vector<compiler::SourceFile> resSources = {
            {"Res.Explicit.rscript", R"RS(
module Res.Explicit;
import RealScript.Resource;

public class MeshAsset : Resource {
    public int vertexCount;
    public MeshAsset(int v) { vertexCount = v; }
}

public int TestExplicitResource() {
    MeshAsset m = new MeshAsset(128);
    using (ResourceHandle<MeshAsset> h = ResourceHandle<MeshAsset>.Adopt(m)) {
        return h.Get().vertexCount + (h.IsValid() ? 2 : 0);
    }
}
)RS"},
            {"Res.Implicit.rscript", R"RS(
module Res.Implicit;
import Res.Explicit;

class LeaseScope : IDisposable {
    public static int disposedTotal = 0;
    private int delta;
    public LeaseScope(int d) { delta = d; }
    public void Dispose() { disposedTotal = disposedTotal + delta; }
}

public int TestAllResources() {
    LeaseScope.disposedTotal = 0;
    using (LeaseScope s = new LeaseScope(7)) {
        int tmp = s == null ? 0 : 1;
        LeaseScope.disposedTotal = LeaseScope.disposedTotal + tmp;
    }
    return TestExplicitResource() + LeaseScope.disposedTotal;
}
)RS"},
        };

        compiler::ModuleBuildCache cache(resCacheDir);
        compiler::ModuleBuildCacheOptions opts;
        opts.optimizationOptions.level = optimization::Level::Aggressive;

        auto cold = cache.build(resSources, opts);
        require(
            cold.succeeded(),
            "RealScript.Resource explicit/implicit cold build failed (must never report RS4001):\n" +
                diagnosticsText(cold.diagnostics));
        require(
            invokeIntEntry(cold.bytecodeModules, "Res.Implicit::TestAllResources") == 138,
            "Unexpected RealScript.Resource cold execution value");

        auto warm = cache.build(resSources, opts);
        require(warm.succeeded() && warm.statistics.cacheHits == 2, "RealScript.Resource warm hit failed");
        require(
            invokeIntEntry(warm.bytecodeModules, "Res.Implicit::TestAllResources") == 138,
            "Unexpected RealScript.Resource warm execution value");

        // Incremental edit on Res.Implicit while Res.Explicit (and RealScript.Resource) hits cache!
        resSources[1].content = R"RS(
module Res.Implicit;
import Res.Explicit;

class LeaseScope : IDisposable {
    public static int disposedTotal = 0;
    private int delta;
    public LeaseScope(int d) { delta = d; }
    public void Dispose() { disposedTotal = disposedTotal + delta; }
}

public int TestAllResources() {
    LeaseScope.disposedTotal = 0;
    using (LeaseScope s = new LeaseScope(20)) {
    }
    return TestExplicitResource() + LeaseScope.disposedTotal;
}
)RS";
        auto inc = cache.build(resSources, opts);
        require(
            inc.succeeded() && inc.statistics.cacheHits == 1 && inc.statistics.cacheMisses == 1,
            "RealScript.Resource incremental build failed:\n" + diagnosticsText(inc.diagnostics));
        require(
            invokeIntEntry(inc.bytecodeModules, "Res.Implicit::TestAllResources") == 150,
            "Unexpected RealScript.Resource incremental execution value");
    }

    // 6B. Cross-module generic specializations + Awaitable<T> + ResourceHandle<T> + unified -O2 Gas/Digest parity
    {
        const auto genCacheDir = workRoot / "cross_mod_generics_cache";
        std::error_code ec;
        std::filesystem::remove_all(genCacheDir, ec);

        const std::string genLibV1 = R"RS(
module Gen.Lib;
import RealScript.Async;
import RealScript.Resource;

internal int internalBias(int x) {
    return x + 5;
}

public class GenericVault<T> {
    public T item;
    private int weight;

    public GenericVault(T v, int w) {
        item = v;
        weight = w;
    }

    public T Unwrap() {
        return item;
    }

    public int ComputeScore(int mult) {
        return internalBias(weight * mult);
    }
}

public T ChooseGeneric<T>(T first, T second, bool pickFirst) {
    return pickFirst ? first : second;
}

public class ShaderAsset : Resource {
    public int passCount;
    public ShaderAsset(int p) { passCount = p; }
}

public ResourceHandle<ShaderAsset> AcquireShader(int passes) {
    ShaderAsset s = new ShaderAsset(passes);
    return ResourceHandle<ShaderAsset>.Adopt(s);
}

public Awaitable<int> ReadyBonus(int v) {
    return Awaitable<int>.FromResult(internalBias(v));
}

public int HotInlineMath(int x) {
    return x * 3 + 1;
}
)RS";

        const std::string genAppAV1 = R"RS(
module Gen.AppA;
import Gen.Lib;
import RealScript.Async;
import RealScript.Resource;

public int RunAppA() {
    GenericVault<int> vault = new GenericVault<int>(40, 6);
    int chosen = ChooseGeneric<int>(vault.Unwrap(), 9, true);
    Awaitable<int> aw = ReadyBonus(10);
    int bonusFlag = aw.IsCompleted() ? 4 : 0;
    aw.Release();
    int shaderPasses = 0;
    using (ResourceHandle<ShaderAsset> sh = AcquireShader(12)) {
        shaderPasses = sh.Get().passCount;
    }
    int acc = 0;
    for (int i = 0; i < 6; i = i + 1) {
        acc = acc + HotInlineMath(i);
    }
    return chosen + vault.ComputeScore(2) + bonusFlag + shaderPasses + acc;
}
)RS";

        const std::string genAppBV1 = R"RS(
module Gen.AppB;
import Gen.Lib;

public int RunAppB() {
    GenericVault<int> v = new GenericVault<int>(7, 3);
    return v.Unwrap() + v.ComputeScore(4) + HotInlineMath(5);
}
)RS";

        std::vector<compiler::SourceFile> sources = {
            {"Gen.Lib.rscript", genLibV1},
            {"Gen.AppA.rscript", genAppAV1},
            {"Gen.AppB.rscript", genAppBV1},
        };

        compiler::ModuleBuildCache cache(genCacheDir);
        compiler::ModuleBuildCacheOptions opts;
        opts.optimizationOptions.level = optimization::Level::Aggressive;

        const auto assertParityWithUncached = [&](
                                                  const std::vector<bytecode::Module>& cachedMods,
                                                  const std::string& entry,
                                                  const std::string& label) {
            const auto uncachedMods = buildUncachedBytecode(sources, opts.optimizationOptions);
            const auto cachedExec = invokeDeterministic(cachedMods, entry);
            const auto uncachedExec = invokeDeterministic(uncachedMods, entry);
            const auto cachedVal = std::get<std::int64_t>(cachedExec.value);
            const auto uncachedVal = std::get<std::int64_t>(uncachedExec.value);
            require(
                cachedVal == uncachedVal,
                label + " value mismatch for " + entry + ": cached=" + std::to_string(cachedVal) +
                    " uncached=" + std::to_string(uncachedVal));
            require(
                cachedExec.instructionsExecuted == uncachedExec.instructionsExecuted,
                label + " Gas (instructionsExecuted) mismatch for " + entry + ": cached=" +
                    std::to_string(cachedExec.instructionsExecuted) + " uncached=" +
                    std::to_string(uncachedExec.instructionsExecuted));
            require(
                cachedExec.determinismDigest == uncachedExec.determinismDigest,
                label + " determinismDigest mismatch for " + entry + ": cached=" +
                    std::to_string(cachedExec.determinismDigest) + " uncached=" +
                    std::to_string(uncachedExec.determinismDigest));
        };

        // Step 1: Cold build
        auto cold = cache.build(sources, opts);
        require(cold.succeeded(), "Cross-module generic cold build failed:\n" + diagnosticsText(cold.diagnostics));
        assertParityWithUncached(cold.bytecodeModules, "Gen.AppA::RunAppA", "Cold");
        assertParityWithUncached(cold.bytecodeModules, "Gen.AppB::RunAppB", "Cold");

        // Step 2: Warm build -> full cache hit with zero optimizer work and 100% parity
        auto warm = cache.build(sources, opts);
        require(
            warm.succeeded() && warm.statistics.cacheHits == 3 &&
                warm.optimizationStatistics.functionsVisited == 0,
            "Cross-module generic warm build failed");
        assertParityWithUncached(warm.bytecodeModules, "Gen.AppA::RunAppA", "Warm");
        assertParityWithUncached(warm.bytecodeModules, "Gen.AppB::RunAppB", "Warm");

        // Step 3: Downstream Gen.AppB incrementally introduces NEW specializations
        // (`GenericVault<string>`, `ChooseGeneric<string>`, `Awaitable<string>.FromResult`)
        // onto cached upstream `Gen.Lib` and built-in `RealScript.Async`!
        sources[2].content = R"RS(
module Gen.AppB;
import Gen.Lib;
import RealScript.Async;

public int RunAppB() {
    GenericVault<string> sv = new GenericVault<string>("blade", 4);
    string label = ChooseGeneric<string>(sv.Unwrap(), "none", true);
    Awaitable<string> awStr = Awaitable<string>.FromResult(label);
    int extra = awStr.IsCompleted() ? 10 : 0;
    awStr.Release();
    int labelBonus = label == "blade" ? 50 : 0;
    return labelBonus + sv.ComputeScore(3) + extra + HotInlineMath(5);
}
)RS";
        auto downstreamSpecBuild = cache.build(sources, opts);
        require(
            downstreamSpecBuild.succeeded(),
            "Incremental downstream new generic specialization build failed:\n" +
                diagnosticsText(downstreamSpecBuild.diagnostics));
        assertParityWithUncached(
            downstreamSpecBuild.bytecodeModules, "Gen.AppA::RunAppA", "DownstreamNewSpecialization");
        assertParityWithUncached(
            downstreamSpecBuild.bytecodeModules, "Gen.AppB::RunAppB", "DownstreamNewSpecialization");

        // Step 4: Upstream `Gen.Lib` modifies `internalBias` helper AND `HotInlineMath` body!
        // Both cross-module generic specialization bodies and `-O2` cross-module inlining MUST match uncached build!
        sources[0].content = R"RS(
module Gen.Lib;
import RealScript.Async;
import RealScript.Resource;

internal int internalBias(int x) {
    return x + 25;
}

public class GenericVault<T> {
    public T item;
    private int weight;

    public GenericVault(T v, int w) {
        item = v;
        weight = w;
    }

    public T Unwrap() {
        return item;
    }

    public int ComputeScore(int mult) {
        return internalBias(weight * mult);
    }
}

public T ChooseGeneric<T>(T first, T second, bool pickFirst) {
    return pickFirst ? first : second;
}

public class ShaderAsset : Resource {
    public int passCount;
    public ShaderAsset(int p) { passCount = p; }
}

public ResourceHandle<ShaderAsset> AcquireShader(int passes) {
    ShaderAsset s = new ShaderAsset(passes);
    return ResourceHandle<ShaderAsset>.Adopt(s);
}

public Awaitable<int> ReadyBonus(int v) {
    return Awaitable<int>.FromResult(internalBias(v));
}

public int HotInlineMath(int x) {
    return x * 9 + 4;
}
)RS";
        auto upstreamHelperAndInlineEdit = cache.build(sources, opts);
        require(
            upstreamHelperAndInlineEdit.succeeded(),
            "Upstream internal helper + inline target edit failed:\n" +
                diagnosticsText(upstreamHelperAndInlineEdit.diagnostics));
        assertParityWithUncached(
            upstreamHelperAndInlineEdit.bytecodeModules, "Gen.AppA::RunAppA", "UpstreamHelperAndInlineEdit");
        assertParityWithUncached(
            upstreamHelperAndInlineEdit.bytecodeModules, "Gen.AppB::RunAppB", "UpstreamHelperAndInlineEdit");
    }

    // 6C. AOT `enableSharedReferenceGenerics` cache key isolation & metadata persistence
    {
        const auto aotGenCacheDir = workRoot / "aot_shared_generics_cache";
        std::error_code ec;
        std::filesystem::remove_all(aotGenCacheDir, ec);

        const std::vector<compiler::SourceFile> aotSources = {
            {"SharedGen.rscript", R"RS(
module SharedGen.Demo;

public class AlphaItem {
    public int a;
    public AlphaItem(int v) { a = v; }
}

public class BetaItem {
    public int b;
    public BetaItem(int v) { b = v; }
}

public class RefHolder<T> {
    public T item;
    public RefHolder(T v) { item = v; }
    public T Fetch() { try { return item; } finally {} }
    public bool HasItem() { return item != null; }
}

public int RunShared() {
    RefHolder<AlphaItem> ha = new RefHolder<AlphaItem>(new AlphaItem(10));
    RefHolder<BetaItem> hb = new RefHolder<BetaItem>(new BetaItem(20));
    return (ha.HasItem() ? ha.Fetch().a : 0) + (hb.HasItem() ? hb.Fetch().b : 0);
}
)RS"},
        };

        compiler::ModuleBuildCache cache(aotGenCacheDir);
        compiler::ModuleBuildCacheOptions opts;
        opts.optimizationOptions.level = optimization::Level::Aggressive;

        aot::GenerationOptions sharedOn;
        sharedOn.programName = "SharedGenProg";
        sharedOn.enableSharedReferenceGenerics = true;

        auto coldOn = cache.buildAot(aotSources, opts, sharedOn);
        require(coldOn.succeeded() && !coldOn.aotCacheHit, "coldOn buildAot failed");
        require(
            coldOn.program.sharedGenericBodyCount > 0,
            "Expected sharedGenericBodyCount > 0 when enableSharedReferenceGenerics=true");

        auto warmOn = cache.buildAot(aotSources, opts, sharedOn);
        require(warmOn.succeeded() && warmOn.aotCacheHit, "warmOn buildAot must hit cache");
        require(
            warmOn.program.sharedGenericBodyCount == coldOn.program.sharedGenericBodyCount &&
                warmOn.program.foldedGenericFunctionCount == coldOn.program.foldedGenericFunctionCount,
            "Warm AOT cache hit must restore sharedGenericBodyCount and foldedGenericFunctionCount");

        aot::GenerationOptions sharedOff = sharedOn;
        sharedOff.enableSharedReferenceGenerics = false;
        auto coldOff = cache.buildAot(aotSources, opts, sharedOff);
        require(
            coldOff.succeeded() && !coldOff.aotCacheHit,
            "Toggling enableSharedReferenceGenerics must produce a distinct AOT cache key (cache miss)");
        require(
            coldOff.program.sharedGenericBodyCount == 0 &&
                coldOff.program.source != coldOn.program.source,
            "enableSharedReferenceGenerics=false must emit unshared bodies with sharedGenericBodyCount=0");
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto exePath = (argc > 0 && argv && argv[0])
            ? std::filesystem::absolute(argv[0])
            : std::filesystem::current_path() / "sprint2_module_build_cache_tests";
        const auto binDir = exePath.parent_path();

#if defined(_WIN32)
        const std::string ext = ".exe";
#else
        const std::string ext = "";
#endif

#ifdef REALSCRIPT_RSC_BIN
        const std::filesystem::path rscBin = REALSCRIPT_RSC_BIN;
#else
        const std::filesystem::path rscBin = binDir / ("rsc" + ext);
#endif

#ifdef REALSCRIPT_RSAOT_BIN
        const std::filesystem::path rsaotBin = REALSCRIPT_RSAOT_BIN;
#else
        const std::filesystem::path rsaotBin = binDir / ("rsaot" + ext);
#endif

        const auto nonce = static_cast<unsigned long long>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        const auto workRoot =
            std::filesystem::temp_directory_path() /
            ("sprint2_module_cache_test_tmp_" + std::to_string(nonce));
        std::error_code ec;
        std::filesystem::remove_all(workRoot, ec);
        std::filesystem::create_directories(workRoot, ec);

        std::cout << "[1/6] Testing DAG extraction, topological sort & fingerprint granularity...\n";
        testModuleGraphAndFingerprintGranularity();

        std::cout << "[2/6] Testing cold build, warm full hit, private vs public invalidation & .d.rscript stubs...\n";
        testIncrementalModuleBuildCacheLifecycle(workRoot);

        std::cout << "[3/6] Testing corrupted cache file checksum verification & automatic self-healing...\n";
        testCorruptedCacheSelfHealing(workRoot);

        std::cout << "[4/6] Testing end-to-end rsc & rsaot --cache-dir / --cache-stats CLI workflows...\n";
        testCliEndToEnd(workRoot, rscBin, rsaotBin);

        std::cout << "[5/6] Testing inheritance, interfaces, built-in RealScript.Math & internal layout types...\n";
        testRichObjectModelAndBuiltinsCache(workRoot);

        std::cout << "[6/6] Testing RealScript.Resource, cross-module generics, -O2 parity & AOT shared generics key...\n";
        testBuiltinResourceCrossModuleGenericsAndO2Parity(workRoot);

        std::filesystem::remove_all(workRoot, ec);
        std::cout << "All sprint2_module_build_cache_tests passed!\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "sprint2_module_build_cache_tests FAILED: " << ex.what() << '\n';
        return 1;
    }
}
