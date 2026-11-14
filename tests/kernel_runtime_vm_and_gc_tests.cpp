#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/optimization/Optimizer.h"
#include "realscript/runtime/Runtime.h"

#include <chrono>
#include <cstdint>
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

std::string formatDiags(const diagnostics::DiagnosticBag& bag) {
    std::ostringstream oss;
    for (const auto& d : bag.items()) {
        oss << d.sourceName << " [" << d.code << "] " << d.message << '\n';
    }
    return oss.str();
}

std::shared_ptr<const runtime::ProgramImage> compileAndLink(
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
    auto linked = runtime::ProgramImage::link(std::move(bcMods), linkErr);
    require(linked.has_value(), "Link failed: " + linkErr.message);
    return std::make_shared<const runtime::ProgramImage>(std::move(*linked));
}

// ---------------------------------------------------------------------------
// 1. BlockVM vs FlatLoop Parity + Stack-Slot ref/out Zero-Heap Lvalues
// ---------------------------------------------------------------------------
void testBlockVsFlatLoopParityAndRefOutLvalues() {
    const std::string src = R"RS(
module Kernel.VmParity;

public struct Counter {
    public int value;
}

public class Holder {
    public int number;
}

public class VmFault {
    public string msg;
    public VmFault(string m) { msg = m; }
}

void SwapInt(ref int a, ref int b) {
    int tmp = a;
    a = b;
    b = tmp;
}

void SplitValue(int input, out int quotient, out int remainder) {
    quotient = input / 7;
    remainder = input % 7;
}

void BumpHolder(ref int fieldRef, int delta) {
    fieldRef = fieldRef + delta;
}

public int RunAll(int n) {
    int a = 11;
    int b = 29;
    SwapInt(ref a, ref b);
    int q = 0;
    int r = 0;
    SplitValue(a, out q, out r);

    Counter c = new Counter();
    c.value = 5;
    BumpHolder(ref c.value, 4);

    int[] arr = new int[3];
    arr[0] = 1;
    arr[1] = 2;
    arr[2] = 3;
    SwapInt(ref arr[0], ref arr[2]);

    Holder h = new Holder();
    h.number = 100;
    BumpHolder(ref h.number, n);

    int thrownAcc = 0;
    try {
        if (n > 0) {
            throw new VmFault("boom");
        }
    } catch (VmFault ex) {
        thrownAcc = ex.msg.Length;
    }

    return a * 10 + b + q + r + c.value + arr[0] + arr[2] + h.number + thrownAcc;
}

public int PureRefLoop(int iterations) {
    int x = 1;
    int y = 2;
    for (int i = 0; i < iterations; i = i + 1) {
        SwapInt(ref x, ref y);
    }
    return x + y;
}
)RS";

    auto prog = compileAndLink({{"vm_parity.rscript", src}});

    runtime::EngineRuntime rtFlat(prog);
    runtime::ExecutionOptions flatOpts;
    flatOpts.determinism.mode = runtime::DeterminismMode::Off;

    runtime::EngineRuntime rtBlock(prog);
    runtime::ExecutionOptions blockOpts;
    blockOpts.determinism.mode = runtime::DeterminismMode::Strict;

    const auto flatRes = rtFlat.invoke(
        "Kernel.VmParity::RunAll", {runtime::Value{std::int64_t{7}}}, flatOpts);
    const auto blockRes = rtBlock.invoke(
        "Kernel.VmParity::RunAll", {runtime::Value{std::int64_t{7}}}, blockOpts);
    require(flatRes.succeeded && blockRes.succeeded, "VM execution failed");
    require(
        std::get<std::int64_t>(flatRes.value) == std::get<std::int64_t>(blockRes.value),
        "FlatLoop and BlockVM diverged on ref/out + exception test");

    // Zero-heap allocation check on stack-slot primitive `ref` loop.
    const auto baseSnap = rtFlat.heap()->statistics();
    const auto pureRes = rtFlat.invoke(
        "Kernel.VmParity::PureRefLoop", {runtime::Value{std::int64_t{200}}}, flatOpts);
    require(pureRes.succeeded && std::get<std::int64_t>(pureRes.value) == 3,
        "PureRefLoop failed");
    const auto afterSnap = rtFlat.heap()->statistics();
    require(afterSnap.objectsAllocated == baseSnap.objectsAllocated,
        "Primitive local ref parameters must not heap-allocate boxed slots");
}

// ---------------------------------------------------------------------------
// 2. Flat Structs, Packed Arrays, Dual-Track Exact Math (Fixed/Fixed16/Vec/Quat)
// ---------------------------------------------------------------------------
void testFlatStructsPackedArraysAndDualMath() {
    const std::string src = R"RS(
module Kernel.StructMath;
import RealScript.Math;

public struct Inner {
    public int a;
    public int b;
}

public struct LargeItem {
    public int f0;
    public int f1;
    public int f2;
    public int f3;
    public int f4;
    public int f5;
    public string label;
}

public int ExerciseStructsAndMath() {
    LargeItem[] items = new LargeItem[8];
    items[3].f0 = 12;
    items[3].f5 = 30;
    items[3].label = "item3";

    LargeItem copy = items[3];
    copy.f0 = 999;
    if (items[3].f0 != 12) { return -1; }
    if (items[3].label != "item3") { return -2; }

    Fixed fx = Fixed.Of(3) + Fixed.Ratio(1, 2);
    Fixed16 fx16 = Fixed16.Of(4);
    Vec2 v = new Vec2(Fixed.Of(3), Fixed.Of(4));
    Vec3 v3 = new Vec3(Fixed.Of(1), Fixed.Of(2), Fixed.Of(2));
    Quat q = Quat.Identity();

    Fixed[] fxArr = new Fixed[2];
    fxArr[0] = fx;
    fxArr[1] = fx + Fixed.Of(1);

    int fxSum = fxArr[0].raw + fxArr[1].raw + fx16.raw;
    int vecCheck = v.LengthSq().raw + Vec3.Dot(v3, v3).raw + q.w.raw;
    if (fxSum <= 0 || vecCheck <= 0) { return -3; }
    return items[3].f0 + items[3].f5 + items[3].label.Length;
}
)RS";

    auto prog = compileAndLink({{"struct_math.rscript", src}});
    runtime::EngineRuntime rt(prog);
    const auto res = rt.invoke("Kernel.StructMath::ExerciseStructsAndMath");
    require(res.succeeded && std::get<std::int64_t>(res.value) == (12 + 30 + 5),
        "Flat structs / dual math check failed: " + res.error.message);

    // Confirm packed array allocation on ManagedHeap.
    auto heap = rt.heap();
    const auto rawFixed = semantic::stableTypeId("RealScript.Math::Fixed");
    const runtime::Value defaultFixed{
        runtime::makeStructValueFromFields(rawFixed, std::vector<runtime::Value>{runtime::Value{std::int64_t{0}}})};
    const auto packedArr = heap->allocateTypedArray(
        semantic::stableTypeId("RealScript.Math::Fixed[]"),
        semantic::PrimitiveType::Struct,
        rawFixed,
        16,
        defaultFixed);
    require(packedArr.has_value() && heap->isPackedArray(*packedArr),
        "allocateTypedArray(Fixed) failed");
}

// ---------------------------------------------------------------------------
// 3. Generational GC, Write Barriers, Idle Incremental Budget No-Op
// ---------------------------------------------------------------------------
void testGcWriteBarriersAndIncrementalBudget() {
    const std::string src = R"RS(
module Kernel.GcTest;

public class Child {
    public int payload;
}

public class Parent {
    public Child child;
}

public Parent MakeParent(int v) {
    Parent p = new Parent();
    Child c = new Child();
    c.payload = v;
    p.child = c;
    return p;
}

public Parent RebindChild(Parent p, int nextV) {
    Child c = new Child();
    c.payload = nextV;
    p.child = c;
    return p;
}

public int ReadParent(Parent p) {
    return p.child.payload;
}

public void AllocGarbageBurst() {
    for (int i = 0; i < 64; i = i + 1) {
        Parent g = MakeParent(i);
    }
}
)RS";

    auto prog = compileAndLink({{"gc_test.rscript", src}});
    runtime::EngineRuntime rt(prog);
    auto heap = rt.heap();

    // Test idle no-op when Idle and below threshold.
    const auto strRef = heap->allocateString("steady");
    require(strRef.has_value(), "allocateString failed");
    auto pinStr = heap->retain(runtime::Value{*strRef});
    runtime::ShadowStack roots;
    const auto idleStats = heap->collectIncrementalWithBudget(
        roots, std::chrono::microseconds(200), 32, false);
    require(!idleStats.cycleCompleted && !idleStats.budgetExhausted &&
        heap->phase() == runtime::GcPhase::Idle && idleStats.totalWork() == 0,
        "collectIncrementalWithBudget should no-op when Idle and below threshold");
    pinStr.reset();

    // Promote Parent to old generation, rebind field to new nursery Child, trigger young GC!
    const auto pRes = rt.invoke("Kernel.GcTest::MakeParent", {runtime::Value{std::int64_t{42}}});
    require(pRes.succeeded, "MakeParent failed");
    const auto parentRef = std::get<runtime::ObjectRef>(pRes.value);
    auto parentRoot = heap->retain(runtime::Value{parentRef});

    // Promote via young + full GC.
    heap->collectFull(roots);
    require(rt.invoke("Kernel.GcTest::AllocGarbageBurst").succeeded, "AllocGarbageBurst failed");
    const auto rebindRes = rt.invoke(
        "Kernel.GcTest::RebindChild", {runtime::Value{parentRef}, runtime::Value{std::int64_t{777}}});
    require(rebindRes.succeeded, "RebindChild failed");

    // Young GC only -> remembered write-barrier must keep newly assigned nursery Child alive!
    std::vector<runtime::Value> youngRoots{runtime::Value{parentRef}};
    roots.pushFrame(nullptr, &youngRoots, nullptr);
    heap->collectYoung(roots, 0);
    roots.popFrame();

    const auto readRes = rt.invoke("Kernel.GcTest::ReadParent", {runtime::Value{parentRef}});
    require(readRes.succeeded && std::get<std::int64_t>(readRes.value) == 777,
        "Write barrier failed to keep nursery child referenced from old parent");

    // Forced incremental GC with ns budget sweeps unreachable garbage across slices.
    std::size_t slices = 0;
    runtime::IncrementalGcBudgetStats budgetStats;
    do {
        budgetStats = heap->collectIncrementalWithBudget(
            roots, std::chrono::nanoseconds(50000), 4, slices == 0);
        ++slices;
    } while (!budgetStats.cycleCompleted && slices < 100);
    require(budgetStats.cycleCompleted, "Incremental ns-budget GC did not complete within 100 slices");
    parentRoot.reset();
}

// ---------------------------------------------------------------------------
// 4. Multi-Heap NativeHandleRegistry, Re-Entrant Sweep Callback &
//    Generation-Safe removeCachedWrapper
// ---------------------------------------------------------------------------
void testNativeHandleReentrantSweepAndGenerationSafety() {
    auto reg = std::make_shared<runtime::NativeHandleRegistry>();
    int hostObj1 = 11;
    int hostObj2 = 22;
    auto res1 = std::shared_ptr<void>(&hostObj1, [](void*) {});
    auto res2 = std::shared_ptr<void>(&hostObj2, [](void*) {});
    const auto nh1 = reg->create(9001, res1, "WidgetA");
    const auto nh2 = reg->create(9001, res2, "WidgetB");

    auto heapA = std::make_shared<runtime::ManagedHeap>();
    auto heapB = std::make_shared<runtime::ManagedHeap>();

    // Allocate two garbage-eligible objects in heapA and one retained object in heapB.
    const auto wA1 = heapA->allocateObject(9001, {runtime::Value{nh1}}, {});
    const auto wA2 = heapA->allocateObject(9001, {runtime::Value{nh2}}, {});
    const auto wB1 = heapB->allocateObject(9001, {runtime::Value{nh1}}, {});
    require(wA1.has_value() && wA2.has_value() && wB1.has_value(), "allocateObject failed");

    require(reg->setCachedWrapper(nh1, *wA1, heapA->heapId()), "setCachedWrapper A1 failed");
    require(reg->setCachedWrapper(nh1, *wB1, heapB->heapId()), "setCachedWrapper B1 failed");
    require(reg->setCachedWrapper(nh2, *wA2, heapA->heapId()), "setCachedWrapper A2 failed");

    // Generation-safe removeCachedWrapper: stale expectedWrapper must return false and preserve current!
    require(!reg->removeCachedWrapper(nh1, heapA->heapId(), *wA2),
        "removeCachedWrapper must return false when expectedWrapper does not match");
    require(reg->cachedWrapper(nh1, heapA->heapId()) == *wA1,
        "Mismatched expectedWrapper unexpectedly removed live wrapper");

    // Re-entrant NativeWrapperSweepCallback: call heapA methods directly inside the sweep callback!
    int sweepCount = 0;
    std::string callbackString;
    bool callbackAllocatedInsideSweep = false;
    bool callbackIsAliveChecked = false;
    heapA->setNativeWrapperSweepCallback(
        [&](runtime::NativeHandle sweptHandle,
            std::uint64_t callbackHeapId,
            runtime::ObjectRef sweptWrapper) {
            ++sweepCount;
            require(callbackHeapId == heapA->heapId(), "Callback heapId mismatch");
            // Re-entrant heap operations inside callback must NOT deadlock!
            callbackIsAliveChecked = !heapA->isAlive(sweptWrapper);
            const auto reAlloc = heapA->allocateString("reentrant-sweep-ok");
            if (reAlloc.has_value()) {
                if (const auto sv = heapA->stringView(*reAlloc)) {
                    callbackString = std::string(*sv);
                    callbackAllocatedInsideSweep = true;
                }
            }
            (void)reg->removeCachedWrapper(sweptHandle, callbackHeapId, sweptWrapper);
        });

    auto pinB = heapB->retain(runtime::Value{*wB1});
    runtime::ShadowStack roots;
    heapA->collectFull(roots);

    require(sweepCount >= 2, "Expected sweep callback to fire for both unpinned wrappers in heapA");
    require(callbackAllocatedInsideSweep && callbackString == "reentrant-sweep-ok",
        "Re-entrant heap allocation inside NativeWrapperSweepCallback failed");
    require(callbackIsAliveChecked, "Swept wrapper should already be unmarked/dead inside callback");
    require(!reg->cachedWrapper(nh1, heapA->heapId()).has_value(),
        "Cleared heapA wrapper should no longer be cached");
    require(reg->cachedWrapper(nh1, heapB->heapId()) == *wB1,
        "Sweeping heapA must leave heapB cached wrapper intact");
    pinB.reset();
}

// ---------------------------------------------------------------------------
// 5. CrossHeapAccessPolicy (ManagedHeap + Nested Structs + VM SlowOps/FlatLoop)
//    & FlatLoop 64-Bit Dirty Bitmask Tracking Across Inheritance
// ---------------------------------------------------------------------------
void testCrossHeapPolicyAndFlatLoopDirtyBitmask() {
    const std::string src = R"RS(
module Kernel.CrossDirty;

public struct DeepBox {
    public string text;
}

public class BaseEntity {
    public int hp = 100;
    public int shield = 50;
}

public class BossEntity : BaseEntity {
    public int rage = 10;
    public string title = "Boss";
    public DeepBox box;
}

public BossEntity SpawnBoss() {
    BossEntity b = new BossEntity();
    b.box.text = "cross-heap-secret";
    return b;
}

public string ReadBossText(BossEntity b) {
    return b.title;
}

public void MutateBossFlat(BossEntity b) {
    b.hp = 999;
    b.rage = 42;
}
)RS";

    auto prog = compileAndLink({{"cross_dirty.rscript", src}});

    // Setup targetHeap with a live BossEntity object.
    auto targetRt = std::make_shared<runtime::EngineRuntime>(prog);
    const auto spawned = targetRt->invoke("Kernel.CrossDirty::SpawnBoss");
    require(spawned.succeeded, "SpawnBoss failed");
    const auto bossRef = std::get<runtime::ObjectRef>(spawned.value);
    auto bossPin = targetRt->heap()->retain(runtime::Value{bossRef});

    // A. ManagedHeap::readFieldWithPolicy / writeFieldWithPolicy matrix across distinct callerHeap.
    runtime::ManagedHeap callerHeap;
    const auto targetHeap = targetRt->heap();
    require(callerHeap.heapId() != targetHeap->heapId(), "Expected distinct heap IDs");

    runtime::RuntimeError policyErr;
    runtime::Value outVal;

    // Nested StructValue (`box` at field index 4) with `ReadOnlySanitizeStrings` deep-clones string to callerHeap!
    require(
        callerHeap.readFieldWithPolicy(
            *targetHeap, bossRef, 4, runtime::CrossHeapAccessPolicy::ReadOnlySanitizeStrings, outVal, &policyErr),
        "Nested struct read under ReadOnlySanitizeStrings failed: " + policyErr.message);
    require(std::holds_alternative<runtime::StructValue>(outVal), "Expected StructValue");
    const auto clonedTextVal = runtime::structValueFieldAt(std::get<runtime::StructValue>(outVal), 0);
    require(clonedTextVal.has_value() && std::holds_alternative<runtime::ObjectRef>(*clonedTextVal),
        "Expected cloned string ObjectRef inside nested StructValue");
    const auto clonedStrRef = std::get<runtime::ObjectRef>(*clonedTextVal);
    require(
        clonedStrRef.heapId == static_cast<std::uint32_t>(callerHeap.heapId()) &&
        callerHeap.stringView(clonedStrRef) == std::optional<std::string_view>("cross-heap-secret"),
        "Sanitized nested struct string must live on callerHeap with preserved content");

    // Non-string ObjectRef read is blocked under ReadOnlySanitizeStrings (via VM) & Deny.
    require(
        !callerHeap.readFieldWithPolicy(
            *targetHeap, bossRef, 3, runtime::CrossHeapAccessPolicy::Deny, outVal, &policyErr) &&
        policyErr.code == runtime::ErrorCode::CrossHeapAccessViolation,
        "Cross-heap read under Deny must report CrossHeapAccessViolation");

    // Write is blocked under both ReadOnlySanitizeStrings and Deny.
    policyErr = {};
    require(
        !callerHeap.writeFieldWithPolicy(
            *targetHeap, bossRef, 0, runtime::Value{std::int64_t{5}},
            runtime::CrossHeapAccessPolicy::ReadOnlySanitizeStrings, &policyErr) &&
        policyErr.code == runtime::ErrorCode::CrossHeapAccessViolation,
        "Cross-heap write under ReadOnlySanitizeStrings must fail with CrossHeapAccessViolation");

    // Direct ManagedHeap string copy across heaps.
    const auto targetStrVal = targetHeap->fieldGet(bossRef, 3);
    require(targetStrVal.has_value() && std::holds_alternative<runtime::ObjectRef>(*targetStrVal),
        "Boss.title fieldGet failed");
    require(
        runtime::ManagedHeap::tryCopyStringGlobal(std::get<runtime::ObjectRef>(*targetStrVal)) ==
            std::optional<std::string>("Boss"),
        "tryCopyStringGlobal failed on live foreign heap string");

    // B. VM SlowOps (Strict) and FlatLoop (Off) reject cross-heap access under Deny/ReadOnlySanitizeStrings.
    for (auto mode : {runtime::DeterminismMode::Off, runtime::DeterminismMode::Strict}) {
        runtime::ExecutionOptions vmOpts;
        vmOpts.determinism.mode = mode;
        vmOpts.crossHeapPolicy = runtime::CrossHeapAccessPolicy::Deny;
        runtime::EngineRuntime vmCaller(prog);
        const auto readRes = vmCaller.invoke("Kernel.CrossDirty::ReadBossText", {runtime::Value{bossRef}}, vmOpts);
        require(!readRes.succeeded && readRes.error.code == runtime::ErrorCode::CrossHeapAccessViolation,
            "VM read did not reject cross-heap object under Deny policy");

        const auto writeRes = vmCaller.invoke("Kernel.CrossDirty::MutateBossFlat", {runtime::Value{bossRef}}, vmOpts);
        require(!writeRes.succeeded && writeRes.error.code == runtime::ErrorCode::CrossHeapAccessViolation,
            "VM write did not reject cross-heap object under Deny policy");
    }

    // C. FlatLoop VM script field assignment marks 64-bit dirty bitmask across base & derived classes!
    targetHeap->registerTypeHierarchy(*prog);
    const auto baseTypeId = semantic::stableTypeId("Kernel.CrossDirty::BaseEntity");
    const auto bossTypeId = semantic::stableTypeId("Kernel.CrossDirty::BossEntity");
    targetHeap->enableTypeDirtyTracking(baseTypeId, {1, 2});
    targetHeap->enableTypeDirtyTracking(bossTypeId, {1, 2, 7, 8, 9});
    require(targetHeap->hasDirtyTracking(), "Expected dirty tracking to become active");
    (void)targetHeap->consumeDirtyMask(bossRef);

    runtime::ExecutionOptions flatDirtyOpts;
    flatDirtyOpts.determinism.mode = runtime::DeterminismMode::Off;
    const auto mutRes = targetRt->invoke(
        "Kernel.CrossDirty::MutateBossFlat", {runtime::Value{bossRef}}, flatDirtyOpts);
    require(mutRes.succeeded, "MutateBossFlat failed: " + mutRes.error.message);

    const std::uint64_t expectedBits = (1ull << 1) | (1ull << 7);
    const std::uint64_t peeked = targetHeap->peekDirtyMask(bossRef);
    require((peeked & expectedBits) == expectedBits,
        "FlatLoop field writes did not set inherited base (bit 1) + derived (bit 7) dirty bits");
    const std::uint64_t consumed = targetHeap->consumeDirtyMask(bossRef);
    require(consumed == peeked && targetHeap->peekDirtyMask(bossRef) == 0,
        "consumeDirtyMask must return prior mask and reset it to 0");

    bossPin.reset();
}

} // namespace

int main() {
    try {
        std::vector<std::pair<const char*, void (*)()>> tests = {
            {"BlockVM vs FlatLoop parity & stack-slot ref/out lvalues", testBlockVsFlatLoopParityAndRefOutLvalues},
            {"Flat structs, packed arrays & dual-track Fixed/Vec/Quat math", testFlatStructsPackedArraysAndDualMath},
            {"Generational GC, write barriers & idle incremental budget", testGcWriteBarriersAndIncrementalBudget},
            {"Multi-heap NativeHandleRegistry, re-entrant sweep & generation safety", testNativeHandleReentrantSweepAndGenerationSafety},
            {"CrossHeapAccessPolicy (Host+VM) & FlatLoop dirty bitmask", testCrossHeapPolicyAndFlatLoopDirtyBitmask},
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
