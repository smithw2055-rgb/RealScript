#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/compiler/ModuleBuildCache.h"
#include "realscript/hot_reload/HotReload.h"
#include "realscript/jit/Jit.h"
#include "realscript/optimization/Optimizer.h"
#include "realscript/runtime/AsyncRuntime.h"
#include "realscript/runtime/Runtime.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace realscript;

namespace {

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error("flat_struct_and_inline_hotreload_tests FAILED: " + message);
    }
}

struct CompiledArtifact {
    compiler::BuildSnapshot snapshot;
    std::vector<compiler::ModuleBuildInfo> buildInfo;
    std::vector<bytecode::Module> bcModules;
    std::shared_ptr<const runtime::ProgramImage> program;
};

CompiledArtifact compileModules(
    const std::vector<compiler::SourceFile>& sources,
    optimization::Level optLevel = optimization::Level::Aggressive,
    const compiler::BuildSnapshot* previous = nullptr) {
    compiler::Compilation comp(sources);
    auto build = comp.build(previous);
    if (build.diagnostics.hasErrors()) {
        std::string msg = "compile error:\n";
        for (const auto& d : build.diagnostics.items()) {
            msg += "  " + d.code + ": " + d.message + "\n";
        }
        throw std::runtime_error(msg);
    }
    optimization::Options opts;
    opts.level = optLevel;
    auto optRes = optimization::Optimizer{}.optimize(
        std::move(build.modules), build.diagnostics, opts);
    check(!build.diagnostics.hasErrors(), "optimizer produced diagnostics");

    bytecode::Lowerer lowerer;
    std::vector<bytecode::Module> bc;
    bc.reserve(optRes.modules.size());
    for (const auto& m : optRes.modules) {
        bc.push_back(lowerer.lower(m));
    }
    std::vector<bytecode::Module> linkCopy = bc;
    runtime::RuntimeError linkErr;
    auto linked = runtime::ProgramImage::link(std::move(linkCopy), linkErr);
    check(linked.has_value(), "ProgramImage::link failed: " + linkErr.message);
    return CompiledArtifact{
        std::move(build.snapshot),
        std::move(build.buildInfo),
        std::move(bc),
        std::make_shared<const runtime::ProgramImage>(std::move(*linked)),
    };
}

// ---------------------------------------------------------------------------
// 1. Arbitrary-Size Flat-Frame Structs (>16 fields, 3-level nested, refs)
//    with Strictly ZERO Heap StructStorage Allocations
// ---------------------------------------------------------------------------
void testZeroHeapFlatFrameLargeAndNestedStructs() {
    std::cout << "[1/4] Testing >16-field & 3-level nested flat-frame structs with 0 StructStorage allocations...\n";
    constexpr const char* kSrc = R"RS(
module Perf.FlatStructs;
import RealScript.Math;

public class TagNode {
    public int id;
    public TagNode(int v) {
        this.id = v;
    }
}

public struct LeafTransform {
    public int px;
    public int py;
    public int pz;
    public int vx;
    public int vy;
    public int vz;
    public Fixed scale;
}

public struct MidSubState {
    public LeafTransform localTf;
    public LeafTransform worldTf;
    public int flags;
    public int layer;
}

public struct MegaHeroState {
    public int hp;
    public int mp;
    public int stamina;
    public int armor;
    public int shield;
    public int combo;
    public int frameTick;
    public int cooldownA;
    public int cooldownB;
    public int cooldownC;
    public MidSubState body;
    public TagNode ownerTag;
    public string heroName;
}

public MegaHeroState StepHero(MegaHeroState s, int delta) {
    s.hp = s.hp + delta;
    s.mp = s.mp + 2;
    s.stamina = s.stamina + 3;
    s.armor = s.armor + 1;
    s.shield = s.shield + (delta * 2);
    s.combo = s.combo + 1;
    s.frameTick = s.frameTick + 1;
    s.cooldownA = s.cooldownA + delta;
    s.cooldownB = s.cooldownB + delta + 1;
    s.cooldownC = s.cooldownC + delta + 2;

    MidSubState mid = s.body;
    LeafTransform tf = mid.localTf;
    tf.px = tf.px + delta;
    tf.py = tf.py + (delta * 2);
    tf.pz = tf.pz + (delta * 3);
    tf.vx = tf.vx + 1;
    tf.vy = tf.vy + 2;
    tf.vz = tf.vz + 3;
    mid.localTf = tf;
    mid.flags = mid.flags + 1;
    s.body = mid;
    return s;
}

public int RunSimulation(int iterations) {
    TagNode node = new TagNode(77);
    MegaHeroState hero = new MegaHeroState();
    hero.hp = 100;
    hero.ownerTag = node;
    hero.heroName = "Paladin";

    MidSubState initMid = new MidSubState();
    LeafTransform initTf = new LeafTransform();
    initTf.scale = Fixed.One();
    initMid.localTf = initTf;
    initMid.layer = 5;
    hero.body = initMid;

    for (int i = 0; i < iterations; i = i + 1) {
        hero = StepHero(hero, 1);
    }

    if (hero.heroName != "Paladin") {
        return -1;
    }
    return hero.hp + hero.shield + hero.cooldownC +
           hero.body.localTf.px + hero.body.localTf.py + hero.body.localTf.pz +
           hero.body.flags + hero.body.layer + hero.ownerTag.id;
}
)RS";

    auto bundle = compileModules({{"flat_structs.rscript", kSrc}}, optimization::Level::None);
    auto heap = std::make_shared<runtime::ManagedHeap>();
    runtime::Interpreter vm(bundle.program, heap);

    runtime::resetHeapStructStorageAllocations();
    const auto res = vm.invoke("Perf.FlatStructs::RunSimulation", {std::int64_t{200}});
    check(res.succeeded, "RunSimulation failed: " + res.error.message);
    const std::uint64_t heapStructAllocs = runtime::heapStructStorageAllocations();
    check(heapStructAllocs == 0u,
        "Expected 0 heap StructStorage allocations during 200-iter 28-leaf struct loop, got " +
            std::to_string(heapStructAllocs));

    // Verify exact mathematical result:
    // hp = 100 + 200 = 300
    // shield = 400
    // cooldownC = 200 * 3 = 600
    // px = 200, py = 400, pz = 600
    // flags = 200, layer = 5, ownerTag.id = 77
    // Total = 300 + 400 + 600 + 200 + 400 + 600 + 200 + 5 + 77 = 2782
    check(std::get<std::int64_t>(res.value) == 2782LL,
        "Unexpected RunSimulation result: " + std::to_string(std::get<std::int64_t>(res.value)));
}

// ---------------------------------------------------------------------------
// 2. SSA Temporary Struct Slot Liveness-Interval Linear-Scan Reuse Verification
// ---------------------------------------------------------------------------
void testSsaStructPayloadSlotReuse() {
    std::cout << "[2/4] Testing SSA temporary struct payload liveness-interval linear-scan slot reuse...\n";
    constexpr const char* kSrc = R"RS(
module Perf.SlotReuse;

public struct Packet8 {
    public int a0;
    public int a1;
    public int a2;
    public int a3;
    public int a4;
    public int a5;
    public int a6;
    public int a7;
}

public Packet8 MakePacket(int seed) {
    Packet8 p = new Packet8();
    p.a0 = seed;
    p.a7 = seed * 2;
    return p;
}

public Packet8 TransformPacket(Packet8 p, int k) {
    p.a1 = p.a0 + k;
    p.a6 = p.a7 + k;
    return p;
}

public int ChainManyTemps(int x) {
    int acc = 0;
    Packet8 r1 = TransformPacket(MakePacket(x + 1), 10);
    acc = acc + r1.a1 + r1.a6;
    Packet8 r2 = TransformPacket(MakePacket(x + 2), 20);
    acc = acc + r2.a1 + r2.a6;
    Packet8 r3 = TransformPacket(MakePacket(x + 3), 30);
    acc = acc + r3.a1 + r3.a6;
    Packet8 r4 = TransformPacket(MakePacket(x + 4), 40);
    acc = acc + r4.a1 + r4.a6;
    return acc;
}
)RS";

    auto bundle = compileModules({{"slot_reuse.rscript", kSrc}}, optimization::Level::None);
    const auto stats = bundle.program->functionStructFrameStats("Perf.SlotReuse::ChainManyTemps");
    check(stats.has_value(), "functionStructFrameStats missing for ChainManyTemps");
    check(stats->uncompactedRegPayloadSlots >= 64u,
        "Expected >= 64 uncompacted SSA struct payload slots, got " +
            std::to_string(stats->uncompactedRegPayloadSlots));
    check(stats->reusedRegPayloadSlots > 0u &&
              stats->compactedRegPayloadSlots < stats->uncompactedRegPayloadSlots,
        "Expected linear-scan liveness allocator to reuse disjoint temporary struct slots (" +
            std::to_string(stats->compactedRegPayloadSlots) + " vs " +
            std::to_string(stats->uncompactedRegPayloadSlots) + ")");

    runtime::Interpreter vm(bundle.program);
    runtime::resetHeapStructStorageAllocations();
    const auto res = vm.invoke("Perf.SlotReuse::ChainManyTemps", {std::int64_t{5}});
    check(res.succeeded, "ChainManyTemps failed: " + res.error.message);
    check(runtime::heapStructStorageAllocations() == 0u,
        "ChainManyTemps must allocate 0 StructStorage objects");
    // x=5:
    // i=1 (seed=6, k=10): a1=16, a6=22 => 38
    // i=2 (seed=7, k=20): a1=27, a6=34 => 61
    // i=3 (seed=8, k=30): a1=38, a6=46 => 84
    // i=4 (seed=9, k=40): a1=49, a6=58 => 107
    // sum = 290
    check(std::get<std::int64_t>(res.value) == 290LL,
        "ChainManyTemps expected 290, got " + std::to_string(std::get<std::int64_t>(res.value)));
}

// ---------------------------------------------------------------------------
// 3. Managed FlatStruct[] Arrays: Contiguous Multi-Slot Storage, Zero Heap
//    StructStorage Allocations, Card-Table Young GC, Incremental GC, Snapshot,
//    Async Suspension, and [FormerlySerializedAs] Structural Hot Reload
// ---------------------------------------------------------------------------
void testManagedFlatStructArraysGcSnapshotAndHotReload() {
    std::cout << "[3/4] Testing S[] FlatStruct contiguous storage, Card-Table Young GC, Snapshot & Hot Reload...\n";
    constexpr const char* kV1 = R"RS(
module Game.Inventory;

public class ItemPayload {
    public int power;
    public ItemPayload(int p) {
        this.power = p;
    }
}

public struct InventorySlot {
    public int slotId;
    public int stackCount;
    public int durability;
    public int rarity;
    public int enchantLevel;
    public string tag;
    public ItemPayload item;
}

public class BagStore {
    public static InventorySlot[] slots = new InventorySlot[160];
}

public void InitOldGenSlots() {
    for (int i = 0; i < 160; i = i + 1) {
        InventorySlot s = new InventorySlot();
        s.slotId = i;
        s.stackCount = 1;
        s.durability = 100;
        s.rarity = 2;
        s.enchantLevel = 3;
        s.tag = "base";
        s.item = new ItemPayload(i + 1);
        BagStore.slots[i] = s;
    }
}

public void MutateWithFreshNurseryObjects() {
    for (int i = 100; i < 160; i = i + 1) {
        InventorySlot s = BagStore.slots[i];
        s.stackCount = s.stackCount + 5;
        s.enchantLevel = s.enchantLevel + 2;
        s.tag = "upgraded";
        s.item = new ItemPayload((i + 1) * 10);
        BagStore.slots[i] = s;
    }
}

public int VerifySlots() {
    int total = 0;
    for (int i = 0; i < 160; i = i + 1) {
        InventorySlot s = BagStore.slots[i];
        total = total + s.stackCount + s.enchantLevel + s.item.power;
    }
    return total;
}

public InventorySlot[] GetRawSlots() {
    return BagStore.slots;
}
)RS";

    auto v1 = compileModules({{"inventory_v1.rscript", kV1}});
    auto heap = std::make_shared<runtime::ManagedHeap>();
    runtime::EngineRuntime engine(v1.program);
    engine.setHeap(heap);

    runtime::resetHeapStructStorageAllocations();
    check(engine.invoke("Game.Inventory::InitOldGenSlots", {}).succeeded, "InitOldGenSlots failed");
    check(runtime::heapStructStorageAllocations() == 0u,
        "InitOldGenSlots on 7-field FlatStruct[160] must perform 0 StructStorage allocations, got " +
            std::to_string(runtime::heapStructStorageAllocations()));

    const auto arrRes = engine.invoke("Game.Inventory::GetRawSlots", {});
    check(arrRes.succeeded, "GetRawSlots failed");
    const auto arrRef = std::get<runtime::ObjectRef>(arrRes.value);
    check(heap->isFlatStructArray(arrRef), "InventorySlot[160] must use SlotStorage::FlatStruct");
    check(heap->flatStructArrayStride(arrRef) == 7u, "InventorySlot stride must be 7 Value cells");

    // Promote BagStore.slots[160] into Old Gen via collectYoung, then mutate indices [100..159]
    // with newly allocated Nursery ItemPayload objects and verify Card-Table write barriers!
    runtime::ShadowStack emptyRoots;
    (void)heap->collectYoung(emptyRoots);
    (void)heap->collectYoung(emptyRoots);

    runtime::resetHeapStructStorageAllocations();
    check(engine.invoke("Game.Inventory::MutateWithFreshNurseryObjects", {}).succeeded,
        "MutateWithFreshNurseryObjects failed");
    check(runtime::heapStructStorageAllocations() == 0u,
        "MutateWithFreshNurseryObjects must perform 0 StructStorage allocations");

    // Trigger Young GC (must scan dirty cards of Old-Gen FlatStruct[160]!) AND incremental full GC slices
    (void)heap->collectYoung(emptyRoots);
    (void)heap->collectIncrementalWithBudget(emptyRoots, std::chrono::microseconds(200), 8, true);
    heap->collectFull(emptyRoots);

    const auto verify1 = engine.invoke("Game.Inventory::VerifySlots", {});
    check(verify1.succeeded, "VerifySlots post-GC failed: " + verify1.error.message);
    const std::int64_t expectedSum = std::get<std::int64_t>(verify1.value);
    // Compute exact expected sum:
    // i in [0..99]: stackCount=1, enchantLevel=3, power=i+1 => 4*100 + sum(1..100=5050) = 5450
    // i in [100..159]: stackCount=6, enchantLevel=5, power=(i+1)*10 => 11*60 + 10*sum(101..160=7830) = 660 + 78300 = 78960
    // Total = 84410
    check(expectedSum == 84410LL, "Expected 84410 after Young+Incremental GC, got " + std::to_string(expectedSum));

    // Snapshot & rollback restore round-trip on FlatStruct[160]
    const auto snap = heap->snapshot();
    check(engine.invoke("Game.Inventory::MutateWithFreshNurseryObjects", {}).succeeded,
        "Second mutation before rollback failed");
    runtime::RuntimeError restoreErr;
    check(heap->restore(snap, &restoreErr), "Snapshot restore failed: " + restoreErr.message);
    check(heap->isFlatStructArray(arrRef), "Restored array must remain FlatStruct");
    const auto verifySnap = engine.invoke("Game.Inventory::VerifySlots", {});
    check(verifySnap.succeeded && std::get<std::int64_t>(verifySnap.value) == 84410LL,
        "VerifySlots after snapshot restore mismatch");

    // Structural Hot Reload with [FormerlySerializedAs("enchantLevel")] + newly added field!
    constexpr const char* kV2 = R"RS(
module Game.Inventory;

public class ItemPayload {
    public int power;
    public ItemPayload(int p) {
        this.power = p;
    }
}

public struct InventorySlot {
    public int slotId;
    public int stackCount;
    public int durability;
    public int rarity;
    [FormerlySerializedAs("enchantLevel")]
    public int upgradeTier;
    public int bonusSocket;
    public string tag;
    public ItemPayload item;
}

public class BagStore {
    public static InventorySlot[] slots = new InventorySlot[160];
}

public int VerifyMigratedSlots() {
    int total = 0;
    for (int i = 0; i < 160; i = i + 1) {
        InventorySlot s = BagStore.slots[i];
        total = total + s.stackCount + s.upgradeTier + s.bonusSocket + s.item.power;
    }
    return total;
}
)RS";

    auto v2 = compileModules({{"inventory_v2.rscript", kV2}});
    hot_reload::ReloadOptions reloadOpts;
    reloadOpts.allowStructuralChanges = true;
    const auto plan = hot_reload::apply(engine, std::move(v2.bcModules), reloadOpts);
    check(plan.compatible, "HotReload V1->V2 on FlatStruct[160] failed");
    check(heap->isFlatStructArray(arrRef) && heap->flatStructArrayStride(arrRef) == 8u,
        "Migrated InventorySlot[160] must re-layout to 8-slot FlatStruct array");
    const auto verifyV2 = engine.invoke("Game.Inventory::VerifyMigratedSlots", {});
    check(verifyV2.succeeded && std::get<std::int64_t>(verifyV2.value) == 84410LL,
        "VerifyMigratedSlots preserved [FormerlySerializedAs] field mismatch");
}

// ---------------------------------------------------------------------------
// 4. Cross-Module -O2 Inlining Reverse-Dependency Tracking, .rsbc Codec,
//    CompilationSnapshot & ModuleBuildCache Invalidation, and HotReload Guard
// ---------------------------------------------------------------------------
void testCrossModuleInliningAndHotReloadConsistency() {
    std::cout << "[4/4] Testing cross-module -O2 inlining reverse dependency tracking & hot-reload consistency...\n";
    constexpr const char* kMathUtilV1 = R"RS(
module Core.Formula;

public int ComputeDamage(int atk, int armor) {
    return (atk * 2) - armor;
}
)RS";

    constexpr const char* kMathUtilV2BodyOnly = R"RS(
module Core.Formula;

public int ComputeDamage(int atk, int armor) {
    return (atk * 3) - armor + 50;
}
)RS";

    constexpr const char* kCombatApp = R"RS(
module Game.Combat;
import Core.Formula;

public int ExecuteStrike(int atk, int armor) {
    return ComputeDamage(atk, armor) + 1;
}
)RS";

    // (a) Compile at -O2 and verify Game.Combat records inlined dependency on Core.Formula::ComputeDamage
    auto v1 = compileModules({
        {"formula.rscript", kMathUtilV1},
        {"combat.rscript", kCombatApp},
    }, optimization::Level::Aggressive);

    const bytecode::Module* formulaModV1 = nullptr;
    const bytecode::Module* combatModV1 = nullptr;
    for (const auto& m : v1.bcModules) {
        if (m.name == "Core.Formula") formulaModV1 = &m;
        if (m.name == "Game.Combat") combatModV1 = &m;
    }
    check(formulaModV1 != nullptr && combatModV1 != nullptr, "Expected both bytecode modules");
    check(combatModV1->languageMetadata.inlinedDependencies.size() == 1u,
        "Expected Game.Combat to record 1 cross-module inlined dependency");
    const auto& dep = combatModV1->languageMetadata.inlinedDependencies[0];
    check(dep.callerFunction == "ExecuteStrike" &&
              dep.calleeModule == "Core.Formula" &&
              dep.calleeFunction == "ComputeDamage" &&
              dep.calleeBodyFingerprint != 0u,
        "Recorded LanguageInlinedDependency fields mismatch");

    // (b) Verify .rsbc binary encode/decode round-trip preserves inlinedDependencies & functionInlineStamps
    const auto encodedCombat = bytecode::encodeModule(*combatModV1);
    diagnostics::DiagnosticBag decodeDiag;
    bytecode::Module decodedCombat;
    check(bytecode::decodeModule(encodedCombat, decodedCombat, decodeDiag) && !decodeDiag.hasErrors(),
        "decodeModule v0.10 failed");
    check(decodedCombat.languageMetadata.inlinedDependencies.size() == 1u &&
              decodedCombat.languageMetadata.inlinedDependencies[0].calleeBodyFingerprint ==
                  dep.calleeBodyFingerprint,
        ".rsbc round-trip lost LanguageInlinedDependency");

    // Seed v1.snapshot with the post-O2 inlined dependency metadata and verify Compilation::build(previous)
    // invalidates Game.Combat even though Core.Formula's publicFingerprint did NOT change!
    v1.snapshot.modules.at("Game.Combat").module.languageMetadata = combatModV1->languageMetadata;
    compiler::Compilation incrComp({
        {"formula.rscript", kMathUtilV2BodyOnly},
        {"combat.rscript", kCombatApp},
    });
    auto incrRes = incrComp.build(&v1.snapshot);
    check(!incrRes.diagnostics.hasErrors(), "Incremental build failed");
    bool combatReused = true;
    for (const auto& bi : incrRes.buildInfo) {
        if (bi.name == "Game.Combat") combatReused = bi.reused;
    }
    check(!combatReused,
        "Compilation::build must NOT reuse cached Game.Combat when inlined Core.Formula source changed!");

    // (c) Verify HotReload::prepare REJECTS stale un-recompiled Game.Combat_v1.rsbc + Core.Formula_v2.rsbc!
    auto v2Full = compileModules({
        {"formula.rscript", kMathUtilV2BodyOnly},
        {"combat.rscript", kCombatApp},
    }, optimization::Level::Aggressive);

    const bytecode::Module* formulaModV2 = nullptr;
    for (const auto& m : v2Full.bcModules) {
        if (m.name == "Core.Formula") formulaModV2 = &m;
    }
    check(formulaModV2 != nullptr, "Missing Core.Formula v2");

    runtime::EngineRuntime engine(v1.program);
    const auto r1 = engine.invoke("Game.Combat::ExecuteStrike", {std::int64_t{20}, std::int64_t{5}});
    check(r1.succeeded && std::get<std::int64_t>(r1.value) == 36LL, "V1 ExecuteStrike expected 36");

    // Attempt partial reload with updated Core.Formula_v2 + STALE Game.Combat_v1!
    std::vector<bytecode::Module> staleMix = {*formulaModV2, decodedCombat};
    const auto badPlan = hot_reload::prepare(*v1.program, std::move(staleMix));
    check(!badPlan.compatible, "HotReload::prepare must reject stale cross-module -O2 inlined caller");
    bool sawStaleIssue = false;
    for (const auto& iss : badPlan.issues) {
        if (iss.kind == hot_reload::ReloadIssueKind::StaleInlinedDependency) {
            sawStaleIssue = true;
        }
    }
    check(sawStaleIssue, "Expected ReloadIssueKind::StaleInlinedDependency");

    // (d) Now apply properly recompiled v2Full modules: must succeed, populate inlineInvalidatedFunctions,
    // flush EngineRuntime/StencilJit native backend, and return the new formula result (20*3 - 5 + 50 + 1 = 106)!
    const auto goodPlan = hot_reload::apply(engine, v2Full.bcModules);
    check(goodPlan.compatible, "Properly recompiled V2 HotReload failed");
    check(!goodPlan.inlineInvalidatedFunctions.empty(),
        "Expected ExecuteStrike in ReloadPlan::inlineInvalidatedFunctions");

    const auto r2 = engine.invoke("Game.Combat::ExecuteStrike", {std::int64_t{20}, std::int64_t{5}});
    check(r2.succeeded && std::get<std::int64_t>(r2.value) == 106LL,
        "V2 ExecuteStrike expected 106 after hot reload, got " + std::to_string(std::get<std::int64_t>(r2.value)));

    // (e) Verify ModuleBuildCache::build & buildAot at -O2 update downstream .rsbc and .aotcache
    // on body-only edit of inlined upstream (where publicFingerprint stays identical)!
    const auto tempCacheDir = std::filesystem::temp_directory_path() / "rs_inline_aot_cache_test";
    std::error_code ec;
    std::filesystem::remove_all(tempCacheDir, ec);
    compiler::ModuleBuildCache diskCache(tempCacheDir / "cache");
    compiler::ModuleBuildCacheOptions cacheOpts;
    cacheOpts.optimizationOptions.level = optimization::Level::Aggressive;
    aot::GenerationOptions aotOpts;
    aotOpts.programName = "CombatPkg";

    const std::vector<compiler::SourceFile> srcsV1 = {
        {"formula.rscript", kMathUtilV1},
        {"combat.rscript", kCombatApp},
    };
    const std::vector<compiler::SourceFile> srcsV2 = {
        {"formula.rscript", kMathUtilV2BodyOnly},
        {"combat.rscript", kCombatApp},
    };

    auto cachedBc1 = diskCache.build(srcsV1, cacheOpts);
    check(cachedBc1.succeeded(), "ModuleBuildCache::build V1 failed");
    auto cachedBc1Hit = diskCache.build(srcsV1, cacheOpts);
    check(cachedBc1Hit.succeeded() && cachedBc1Hit.statistics.cacheHits == 2u,
        "ModuleBuildCache::build V1 second run must hit graph cache");

    // Mutate ONLY Core.Formula body (publicFingerprint unchanged): downstream Game.Combat .rsbc MUST re-inline V2!
    auto cachedBc2 = diskCache.build(srcsV2, cacheOpts);
    check(cachedBc2.succeeded(), "ModuleBuildCache::build V2 failed");
    runtime::RuntimeError cacheLinkErr;
    auto cacheLinkedV2 = runtime::ProgramImage::link(cachedBc2.bytecodeModules, cacheLinkErr);
    check(cacheLinkedV2.has_value(), "Cached V2 link failed");
    runtime::Interpreter cacheVm(std::make_shared<const runtime::ProgramImage>(std::move(*cacheLinkedV2)));
    const auto cacheRunV2 = cacheVm.invoke("Game.Combat::ExecuteStrike", {std::int64_t{20}, std::int64_t{5}});
    check(cacheRunV2.succeeded && std::get<std::int64_t>(cacheRunV2.value) == 106LL,
        "ModuleBuildCache V2 ExecuteStrike expected 106, got " +
            std::to_string(std::get<std::int64_t>(cacheRunV2.value)));

    auto aot1 = diskCache.buildAot(srcsV1, cacheOpts, aotOpts);
    check(aot1.succeeded(), "ModuleBuildCache::buildAot V1 failed");
    auto aot1Hit = diskCache.buildAot(srcsV1, cacheOpts, aotOpts);
    check(aot1Hit.succeeded() && aot1Hit.aotCacheHit, "ModuleBuildCache::buildAot V1 repeat must hit aotCache");

    auto aot2 = diskCache.buildAot(srcsV2, cacheOpts, aotOpts);
    check(aot2.succeeded(), "ModuleBuildCache::buildAot V2 failed");
    check(!aot2.aotCacheHit && aot1.program.source != aot2.program.source,
        "ModuleBuildCache::buildAot at -O2 must regenerate AOT code when upstream inlined body changes");
    std::filesystem::remove_all(tempCacheDir, ec);
}

} // namespace

int main() {
    try {
        testZeroHeapFlatFrameLargeAndNestedStructs();
        testSsaStructPayloadSlotReuse();
        testManagedFlatStructArraysGcSnapshotAndHotReload();
        testCrossModuleInliningAndHotReloadConsistency();
        std::cout << "ALL flat_struct_and_inline_hotreload_tests PASSED!\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << "\n";
        return 1;
    }
}
