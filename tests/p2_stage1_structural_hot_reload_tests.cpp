#include "realscript/compiler/Compilation.h"
#include "realscript/bytecode/Bytecode.h"
#include "realscript/hot_reload/HotReload.h"
#include "realscript/runtime/Runtime.h"

#include <cmath>
#include <iostream>
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

std::vector<bytecode::Module> compileModules(
    const std::vector<std::pair<std::string, std::string>>& files) {
    std::vector<compiler::SourceFile> sources;
    sources.reserve(files.size());
    for (const auto& [name, content] : files) {
        sources.push_back({name, content});
    }
    compiler::Compilation compilation(std::move(sources));
    auto build = compilation.build();
    if (build.diagnostics.hasErrors()) {
        std::string msg = "Compilation failed:";
        for (const auto& d : build.diagnostics.items()) {
            msg += "\n  " + d.code + ": " + d.message;
        }
        throw std::runtime_error(msg);
    }
    bytecode::Lowerer lowerer;
    std::vector<bytecode::Module> lowered;
    for (const auto& mirMod : build.modules) {
        lowered.push_back(lowerer.lower(mirMod));
    }
    return lowered;
}

std::shared_ptr<const runtime::ProgramImage> linkProgram(
    std::vector<bytecode::Module> modules) {
    runtime::RuntimeError error;
    auto linked = runtime::ProgramImage::link(std::move(modules), error);
    require(linked.has_value(), "Link failed: " + error.message);
    return std::make_shared<const runtime::ProgramImage>(std::move(*linked));
}

void testInPlaceSlotMigrationAndFormerlySerializedAs() {
    auto v1Modules = compileModules({{"game.rscript", R"(
module P2B.SlotTest;

public class Weapon {
    public int power;
}

public class Player {
    public int hp;
    public int mp;
}

public class Holder {
    public Player hero;
    public Player[] party;
}

public Holder Setup() {
    Player p = new Player();
    p.hp = 85;
    p.mp = 40;
    Holder h = new Holder();
    h.hero = p;
    h.party = new Player[1];
    h.party[0] = p;
    return h;
}
)"}});

    runtime::EngineRuntime rt(linkProgram(v1Modules));
    auto setupRes = rt.invoke("P2B.SlotTest::Setup");
    require(setupRes.succeeded, "Setup v1 failed: " + setupRes.error.message);
    const auto holderRef = std::get<runtime::ObjectRef>(setupRes.value);
    auto holderRoot = rt.heap()->retain(holderRef);

    const auto playerVal = rt.heap()->fieldGet(holderRef, 0);
    require(playerVal.has_value() && std::holds_alternative<runtime::ObjectRef>(*playerVal),
        "Holder.hero must be ObjectRef");
    const runtime::ObjectRef originalPlayerRef = std::get<runtime::ObjectRef>(*playerVal);

    require(rt.heap()->isInlineRecordObject(originalPlayerRef),
        "2-field Player in v1 must use SlabValues inline record storage");
    const std::size_t bytesBeforeExpand = rt.heap()->liveBytes();

    auto v2ModulesForReject = compileModules({{"game.rscript", R"(
module P2B.SlotTest;

public class Weapon {
    public int power;
}

public class Player {
    public int mp;
    [FormerlySerializedAs("hp")]
    public double health;
    public int flatBonus = 250;
    public string title = "ranger";
    public bool shielded;
    public Weapon weapon;
}

public class Holder {
    public Player hero;
    public Player[] party;
}
)"}});
    const auto rejected = hot_reload::prepare(*rt.programSnapshot(), std::move(v2ModulesForReject));
    require(!rejected.compatible, "allowStructuralChanges=false must reject structural layout changes");

    auto v2Modules = compileModules({{"game.rscript", R"(
module P2B.SlotTest;

public class Weapon {
    public int power;
}

public class Player {
    public int mp;
    [FormerlySerializedAs(oldName = "hp")]
    public double health;
    public int flatBonus = 250;
    public string title = "ranger";
    public bool shielded;
    public Weapon weapon;
}

public class Holder {
    public Player hero;
    public Player[] party;
}

public double CheckMigrated(Holder h) {
    if (h.hero != h.party[0]) return -1.0;
    if (h.hero.mp != 40) return -2.0;
    if (h.hero.flatBonus != 250) return -3.0;
    if (h.hero.title != "ranger") return -4.0;
    if (h.hero.shielded) return -5.0;
    if (h.hero.weapon != null) return -6.0;
    return h.hero.health + h.hero.flatBonus;
}
)"}});

    hot_reload::ReloadOptions structuralOpts{};
    structuralOpts.allowStructuralChanges = true;
    const auto planV2 = hot_reload::apply(rt, std::move(v2Modules), structuralOpts);
    require(planV2.compatible, "v2 structural reload failed");
    require(planV2.migrationStats.objectsMigrated == 1, "expected 1 Player migrated");
    require(planV2.migrationStats.fieldsRenamed == 1, "expected 1 field renamed via [FormerlySerializedAs]");
    require(planV2.migrationStats.fieldsConverted == 1, "expected 1 field converted int -> double");
    require(planV2.migrationStats.inlineToSlabOrHeapTransitions == 1,
        "expected transition from SlabValues (<=4) to HeapValues (>4)");

    const auto afterHeroVal = rt.heap()->fieldGet(holderRef, 0);
    require(afterHeroVal.has_value() &&
            std::get<runtime::ObjectRef>(*afterHeroVal) == originalPlayerRef,
        "ObjectRef (slot, generation, heapId) must remain 100% identical in-place!");
    require(!rt.heap()->isInlineRecordObject(originalPlayerRef),
        "6-field Player in v2 must use HeapValues storage");
    require(rt.heap()->liveBytes() == bytesBeforeExpand + 4 * sizeof(runtime::Value),
        "liveBytes must increase by exactly 4 * sizeof(Value)");

    const auto checkV2 = rt.invoke("P2B.SlotTest::CheckMigrated", {holderRef});
    require(checkV2.succeeded, "CheckMigrated v2 failed: " + checkV2.error.message);
    require(std::abs(std::get<double>(checkV2.value) - 335.0) < 1e-9,
        "CheckMigrated v2 expected 335.0");

    auto v3Modules = compileModules({{"game.rscript", R"(
module P2B.SlotTest;

public class Weapon {
    public int power;
}

public class Player {
    [FormerlySerializedAs("health")]
    public int vitalHp;
    public string title;
}

public class Holder {
    public Player hero;
    public Player[] party;
}

public string Summary(Holder h) {
    if (h.hero.vitalHp == 85) return h.hero.title;
    return "bad";
}
)"}});

    const std::size_t bytesBeforeShrink = rt.heap()->liveBytes();
    const auto planV3 = hot_reload::apply(rt, std::move(v3Modules), structuralOpts);
    require(planV3.compatible, "v3 shrink reload failed");
    require(planV3.migrationStats.heapToInlineTransitions == 1,
        "expected transition from HeapValues (>4) back to SlabValues (<=2)");
    require(rt.heap()->isInlineRecordObject(originalPlayerRef),
        "2-field Player in v3 must return to SlabValues inline storage");
    require(rt.heap()->liveBytes() == bytesBeforeShrink - 4 * sizeof(runtime::Value),
        "liveBytes after shrinking back to 2 fields must decrease by 4 * sizeof(Value)");

    const auto checkV3 = rt.invoke("P2B.SlotTest::Summary", {holderRef});
    require(checkV3.succeeded && checkV3.value == "ranger",
        "Summary v3 expected 'ranger'");
}

void testOldGenWriteBarrierAndReferenceFieldsUpdate() {
    auto v1Modules = compileModules({{"gc_barrier.rscript", R"(
module P2B.GcBarrier;

public class Child {
    public int tag;
}

public class Container {
    public Child kid;
    public int a;
}

public Container MakeOldContainer() {
    Container c = new Container();
    c.a = 7;
    return c;
}

public void AttachYoungChild(Container c, int tagVal) {
    Child k = new Child();
    k.tag = tagVal;
    c.kid = k;
}
)"}});

    runtime::EngineRuntime rt(linkProgram(v1Modules));
    auto cRes = rt.invoke("P2B.GcBarrier::MakeOldContainer");
    require(cRes.succeeded, "MakeOldContainer failed");
    const auto containerRef = std::get<runtime::ObjectRef>(cRes.value);
    auto containerRoot = rt.heap()->retain(containerRef);

    runtime::ShadowStack emptyStack;
    (void)rt.heap()->collectYoung(emptyStack);
    require(rt.heap()->nurserySize() == 0, "Container should be promoted out of nursery");

    auto attachRes = rt.invoke("P2B.GcBarrier::AttachYoungChild", {containerRef, std::int64_t{999}});
    require(attachRes.succeeded, "AttachYoungChild failed");
    require(rt.heap()->nurserySize() >= 1, "Young Child must reside in nursery");

    auto v2Modules = compileModules({{"gc_barrier.rscript", R"(
module P2B.GcBarrier;

public class Child {
    public int tag;
}

public class Container {
    public int a;
    public int b = 10;
    public int c = 20;
    public int d = 30;
    public int e = 40;
    [FormerlySerializedAs("kid")]
    public Child movedKid;
}

public int VerifyAndAllocNew(Container oldC) {
    Container fresh = new Container();
    Child freshKid = new Child();
    freshKid.tag = 111;
    fresh.movedKid = freshKid;
    return oldC.movedKid.tag + oldC.a + oldC.e + fresh.movedKid.tag;
}

public int ReadOldKid(Container oldC) {
    return oldC.movedKid.tag;
}
)"}});

    hot_reload::ReloadOptions opts{};
    opts.allowStructuralChanges = true;
    const auto plan = hot_reload::apply(rt, std::move(v2Modules), opts);
    require(plan.compatible, "GC barrier v2 reload failed");

    (void)rt.heap()->collectYoung(emptyStack);
    rt.heap()->collectFull(emptyStack);

    const auto readAfterGc = rt.invoke("P2B.GcBarrier::ReadOldKid", {containerRef});
    require(readAfterGc.succeeded && std::get<std::int64_t>(readAfterGc.value) == 999,
        "Young Child at shifted referenceFields index 5 must survive collectYoung + collectFull!");

    const auto verifyNew = rt.invoke("P2B.GcBarrier::VerifyAndAllocNew", {containerRef});
    require(verifyNew.succeeded && std::get<std::int64_t>(verifyNew.value) == (999 + 7 + 40 + 111),
        "Fresh NewObjectFast allocation after reload must use updated sharedTypeLayout referenceFields");
}

void testCheckedNumericConversionStaticFieldsAndDelegates() {
    auto v1Modules = compileModules({{"advanced.rscript", R"(
module P2B.Advanced;

public delegate int ActionCalc();

public class BaseUnit {
    public virtual int Compute() { return 10; }
}

public class EliteUnit : BaseUnit {
    public static int s_oldGlobalBonus = 300;
    public int okByteScore;
    public int overflowByteScore;
    public override int Compute() { return 50 + EliteUnit.s_oldGlobalBonus; }
}

public class LegacyRemoved {
    public static int s_deadStatic = 777;
    public int x;
}

public class SceneState {
    public EliteUnit unit;
    public ActionCalc cachedDelegate;
    public LegacyRemoved doomedRef;
}

public ActionCalc BindPolymorphic(BaseUnit b) { return b.Compute; }

public SceneState InitState() {
    EliteUnit u = new EliteUnit();
    u.okByteScore = 200;
    u.overflowByteScore = 99999;
    EliteUnit.s_oldGlobalBonus = 450;

    LegacyRemoved lr = new LegacyRemoved();
    lr.x = 12;

    SceneState st = new SceneState();
    st.unit = u;
    st.cachedDelegate = BindPolymorphic(u);
    st.doomedRef = lr;
    return st;
}
)"}});

    runtime::EngineRuntime rt(linkProgram(v1Modules));
    auto initRes = rt.invoke("P2B.Advanced::InitState");
    require(initRes.succeeded, "InitState failed: " + initRes.error.message);
    const auto stateRef = std::get<runtime::ObjectRef>(initRes.value);
    auto stateRoot = rt.heap()->retain(stateRef);

    const auto deadStaticId = semantic::stableTypeId("P2B.Advanced::LegacyRemoved::field:s_deadStatic");
    const auto oldBonusId = semantic::stableTypeId("P2B.Advanced::EliteUnit::field:s_oldGlobalBonus");
    rt.heap()->setStaticField(deadStaticId, runtime::Value{std::int64_t{777}});
    require(rt.heap()->hasStaticField(oldBonusId), "old static bonus must exist before reload");

    auto v2Modules = compileModules({{"advanced.rscript", R"(
module P2B.Advanced;

public delegate int ActionCalc();

public interface IScorer {
    long Score();
}

public class HelperNode {
    public int id = 55;
}

public class BaseUnit {
    public virtual int AlphaRank() { return 1; }
    public virtual int Compute() { return 10; }
}

public class EliteUnit : BaseUnit, IScorer {
    [FormerlySerializedAs("s_oldGlobalBonus")]
    public static long s_globalBonus = 0;

    public byte okByteScore = 9;
    public byte overflowByteScore = 9;

    public override int AlphaRank() { return 2; }
    public override int Compute() { return (int)EliteUnit.s_globalBonus + (int)okByteScore + (int)overflowByteScore; }
    public long Score() { return EliteUnit.s_globalBonus + 100L; }
}

public class SceneState {
    public EliteUnit unit;
    public ActionCalc cachedDelegate;
    public HelperNode doomedRef;
}

public int CallAction(ActionCalc fn) { return fn(); }

public long VerifyReloaded(SceneState st, ActionCalc fn) {
    if (st.doomedRef != null) return -1L;
    int viaDelegate = fn();
    BaseUnit asBase = st.unit;
    int viaVirtual = asBase.Compute() + asBase.AlphaRank();
    IScorer asIface = st.unit;
    long viaInterface = asIface.Score();
    HelperNode hn = new HelperNode();
    return (long)viaDelegate + (long)viaVirtual + viaInterface + (long)hn.id;
}
)"}});

    hot_reload::ReloadOptions opts{};
    opts.allowStructuralChanges = true;
    const auto plan = hot_reload::apply(rt, std::move(v2Modules), opts);
    require(plan.compatible, "Advanced v2 structural reload failed");
    require(!rt.heap()->hasStaticField(deadStaticId), "Removed type static field must be erased");
    require(!rt.heap()->hasStaticField(oldBonusId), "Renamed old static field ID must be erased");
    require(plan.migrationStats.delegatesRebound == 1, "Pre-existing virtual delegate must be rebound to new virtualSlot 1");

    const auto delVal = rt.heap()->fieldGet(stateRef, 1);
    require(delVal.has_value(), "cachedDelegate field missing");
    const auto verifyRes = rt.invoke("P2B.Advanced::VerifyReloaded", {stateRef, *delVal});
    require(verifyRes.succeeded, "VerifyReloaded failed: " + verifyRes.error.message);
    require(std::get<runtime::LongValue>(verifyRes.value).value == 1925,
        "VerifyReloaded expected 1925, got " +
            std::to_string(std::get<runtime::LongValue>(verifyRes.value).value));
}

void testStructAndMathDefaultsMigration() {
    auto v1Modules = compileModules({{"struct_math.rscript", R"(
module P2B.StructMath;

public struct Stats {
    public int atk;
    public int def;
}

public class Actor {
    public Stats stats;
}

public Actor Spawn() {
    Actor a = new Actor();
    Stats s = new Stats();
    s.atk = 35;
    s.def = 15;
    a.stats = s;
    return a;
}
)"}});

    runtime::EngineRuntime rt(linkProgram(v1Modules));
    auto res = rt.invoke("P2B.StructMath::Spawn");
    require(res.succeeded, "Spawn failed");
    const auto actorRef = std::get<runtime::ObjectRef>(res.value);
    auto root = rt.heap()->retain(actorRef);

    auto v2Modules = compileModules({{"struct_math.rscript", R"(
module P2B.StructMath;

public struct Stats {
    public int def;
    [FormerlySerializedAs("atk")]
    public double attackPower;
    public int critChance = 25;
}

public class Actor {
    public Stats stats;
    public int level = 3;
}

public double EvalActor(Actor a) {
    return a.stats.attackPower + (double)a.stats.def + (double)a.stats.critChance + (double)a.level;
}
)"}});

    hot_reload::ReloadOptions opts{};
    opts.allowStructuralChanges = true;
    const auto plan = hot_reload::apply(rt, std::move(v2Modules), opts);
    require(plan.compatible, "StructMath v2 reload failed");

    const auto vec3Zero = runtime::makeDefaultInlineStructValue(runtime::vec3TypeId());
    const auto quatZero = runtime::makeDefaultInlineStructValue(runtime::quatTypeId());
    const auto fixedZero = runtime::makeDefaultInlineStructValue(runtime::fixedTypeId());
    require(vec3Zero.has_value() && std::get<runtime::Vec3Value>(*vec3Zero) == runtime::Vec3Value{0, 0, 0},
        "Vec3 inline default must be zero");
    require(quatZero.has_value() && fixedZero.has_value(),
        "Quat and Fixed inline defaults must be materialized");

    const auto evalRes = rt.invoke("P2B.StructMath::EvalActor", {actorRef});
    require(evalRes.succeeded, "EvalActor failed: " + evalRes.error.message);
    require(std::abs(std::get<double>(evalRes.value) - 78.0) < 1e-9,
        "EvalActor expected 78.0");
}

void testOomRollbackSafety() {
    auto v1Modules = compileModules({{"oom.rscript", R"(
module P2B.Oom;
public class Tiny {
    public int x;
}
public Tiny Make() {
    Tiny t = new Tiny();
    t.x = 77;
    return t;
}
public int Read(Tiny t) { return t.x; }
)"}});

    auto progV1 = linkProgram(v1Modules);
    runtime::EngineRuntime rt(progV1);

    runtime::HeapConfig tightCfg{};
    tightCfg.initialCollectionThresholdBytes = 512;
    tightCfg.maximumHeapBytes = 1024;
    rt.setHeap(std::make_shared<runtime::ManagedHeap>(tightCfg));

    auto makeRes = rt.invoke("P2B.Oom::Make");
    require(makeRes.succeeded, "Oom Make failed");
    const auto tinyRef = std::get<runtime::ObjectRef>(makeRes.value);
    auto root = rt.heap()->retain(tinyRef);

    std::string manyFields;
    for (int i = 0; i < 60; ++i) {
        manyFields += "    public int f" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
    }
    auto v2Modules = compileModules({{"oom.rscript",
        "module P2B.Oom;\npublic class Tiny {\n    public int x;\n" +
        manyFields +
        "}\npublic int Read(Tiny t) { return t.x + 1000; }\n"}});

    hot_reload::ReloadOptions opts{};
    opts.allowStructuralChanges = true;
    const auto failedPlan = hot_reload::apply(rt, std::move(v2Modules), opts);
    require(!failedPlan.compatible, "OOM structural migration must fail cleanly");

    const auto stillV1 = rt.invoke("P2B.Oom::Read", {tinyRef});
    require(stillV1.succeeded && std::get<std::int64_t>(stillV1.value) == 77,
        "After OOM rollback, v1 program and heap slot must remain completely intact");
}

} // namespace

int main() {
    testInPlaceSlotMigrationAndFormerlySerializedAs();
    std::cout << "[PASS] In-place CompactSlot migration, [FormerlySerializedAs], Slab<->Heap & liveBytes\n";

    testOldGenWriteBarrierAndReferenceFieldsUpdate();
    std::cout << "[PASS] Old-Gen write barrier & sharedTypeLayouts referenceFields across Young/Full GC\n";

    testCheckedNumericConversionStaticFieldsAndDelegates();
    std::cout << "[PASS] Checked numeric conversion, static fields, vtable/interface shift & delegate rebinding\n";

    testStructAndMathDefaultsMigration();
    std::cout << "[PASS] Embedded Struct layout migration & inline math zero defaults\n";

    testOomRollbackSafety();
    std::cout << "[PASS] Atomic OOM rollback safety\n";

    return 0;
}
