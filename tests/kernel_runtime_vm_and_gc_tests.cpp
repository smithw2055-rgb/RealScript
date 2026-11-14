#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/optimization/Optimizer.h"
#include "realscript/runtime/Runtime.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
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

// ---------------------------------------------------------------------------
// 6. Pillar 1: ValuePoolDomain Concurrency, >20k Single-Shard Scaling,
//    Large String Sweep Shrink & >70 Domain Cycle Epoch Isolation
// ---------------------------------------------------------------------------
std::uint64_t testFnv1a64(std::string_view text) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char ch : text) {
        hash ^= static_cast<std::uint64_t>(ch);
        hash *= 1099511628211ULL;
    }
    return hash;
}

void testValuePoolDomainsConcurrencyScaleAndEpochIsolation() {
    static_assert(sizeof(runtime::Value) == 16, "Value must remain 16 bytes");

    auto domainA = runtime::ValuePoolDomain::create();
    auto domainB = runtime::ValuePoolDomain::create();
    require(domainA && domainB && domainA->domainId() != domainB->domainId(),
        "Expected distinct non-default ValuePoolDomains");

    runtime::HeapConfig cfgA;
    cfgA.valuePoolDomain = domainA;
    runtime::HeapConfig cfgB;
    cfgB.valuePoolDomain = domainB;
    runtime::ManagedHeap heapA(cfgA);
    runtime::ManagedHeap heapB(cfgB);

    // Concurrent GC on domainA while domainB allocates strings & arrays.
    std::string threadErr;
    std::thread tA([&]() {
        try {
            runtime::ScopedValuePoolDomain scope(domainA);
            auto keepArr = heapA.allocateArray(16, runtime::Value{});
            if (!keepArr) {
                threadErr = "domainA keepArr alloc failed";
                return;
            }
            auto keepPin = heapA.retain(runtime::Value{*keepArr});
            for (int round = 0; round < 35; ++round) {
                runtime::StringValue sv("domA_keep_" + std::to_string(round % 16));
                heapA.arraySet(*keepArr, static_cast<std::size_t>(round % 16), runtime::Value{sv});
                {
                    auto tmpArr = heapA.allocateArray(8, runtime::Value{});
                    if (tmpArr) {
                        heapA.arraySet(
                            *tmpArr,
                            0,
                            runtime::Value{runtime::StringValue(
                                "domA_trash_" + std::to_string(round) + "_" + std::string(400, 'a'))});
                    }
                }
                if ((round % 5) == 0) {
                    runtime::RootSet emptyRoots;
                    heapA.collect(emptyRoots);
                    domainA->collectGarbage();
                }
            }
            const auto checkVal = heapA.arrayGet(*keepArr, 3);
            if (!checkVal || !std::holds_alternative<runtime::StringValue>(*checkVal) ||
                std::get<runtime::StringValue>(*checkVal).str() != "domA_keep_3") {
                threadErr = "domainA keepArr content corrupted during concurrent GC";
            }
        } catch (const std::exception& ex) {
            threadErr = ex.what();
        }
    });

    std::string threadBErr;
    std::thread tB([&]() {
        try {
            runtime::ScopedValuePoolDomain scope(domainB);
            auto arrB = heapB.allocateArray(1500, runtime::Value{});
            if (!arrB) {
                threadBErr = "domainB arr alloc failed";
                return;
            }
            auto pinB = heapB.retain(runtime::Value{*arrB});
            for (int i = 0; i < 1500; ++i) {
                const std::string txt = "domB_item_" + std::to_string(i);
                heapB.arraySet(*arrB, static_cast<std::size_t>(i),
                    runtime::Value{runtime::StringValue(txt)});
            }
            for (int i = 0; i < 1500; ++i) {
                const auto v = heapB.arrayGet(*arrB, static_cast<std::size_t>(i));
                if (!v || !std::holds_alternative<runtime::StringValue>(*v) ||
                    std::get<runtime::StringValue>(*v).str() != ("domB_item_" + std::to_string(i))) {
                    threadBErr = "domainB string mismatch at index " + std::to_string(i);
                    return;
                }
            }
        } catch (const std::exception& ex) {
            threadBErr = ex.what();
        }
    });
    tA.join();
    tB.join();
    require(threadErr.empty(), "Concurrent domainA failure: " + threadErr);
    require(threadBErr.empty(), "Concurrent domainB failure: " + threadBErr);

    // >20,000 live strings in a SINGLE shard (>16,384 legacy cap).
    {
        runtime::ScopedValuePoolDomain scope(domainA);
        constexpr std::size_t kTargetCount = 20500;
        std::vector<std::string> shard0Keys;
        shard0Keys.reserve(kTargetCount);
        for (std::uint64_t cand = 0; shard0Keys.size() < kTargetCount; ++cand) {
            std::string s = "sh0_" + std::to_string(cand);
            if (((testFnv1a64(s) >> 60u) & 0xFu) == 0u) {
                shard0Keys.push_back(std::move(s));
            }
        }
        auto shardArr = heapA.allocateArray(kTargetCount, runtime::Value{});
        require(shardArr.has_value(), "Failed allocating root array for 20,500 shard strings");
        auto shardPin = heapA.retain(runtime::Value{*shardArr});
        for (std::size_t i = 0; i < kTargetCount; ++i) {
            heapA.arraySet(*shardArr, i, runtime::Value{runtime::StringValue(shard0Keys[i])});
        }
        runtime::RootSet roots;
        heapA.collect(roots);
        domainA->collectGarbage();
        require(domainA->stats().liveStrings >= kTargetCount,
            "Expected >= 20,500 live strings in single shard after GC mark");
        for (std::size_t probe : {std::size_t{0}, std::size_t{8191}, std::size_t{16385}, std::size_t{20499}}) {
            const auto readBack = heapA.arrayGet(*shardArr, probe);
            require(readBack && std::holds_alternative<runtime::StringValue>(*readBack) &&
                std::get<runtime::StringValue>(*readBack).str() == shard0Keys[probe],
                "Single-shard >20k string lookup mismatch");
        }
    }

    // Large string (>256B capacity) shrink & sweep inside an isolated domain.
    {
        auto shrinkDomain = runtime::ValuePoolDomain::create();
        runtime::HeapConfig shrinkCfg;
        shrinkCfg.valuePoolDomain = shrinkDomain;
        runtime::ManagedHeap shrinkHeap(shrinkCfg);
        runtime::ScopedValuePoolDomain scope(shrinkDomain);
        {
            auto bigArr = shrinkHeap.allocateArray(64, runtime::Value{});
            require(bigArr.has_value(), "shrinkHeap array alloc failed");
            auto pinBig = shrinkHeap.retain(runtime::Value{*bigArr});
            for (int i = 0; i < 64; ++i) {
                shrinkHeap.arraySet(
                    *bigArr,
                    static_cast<std::size_t>(i),
                    runtime::Value{runtime::StringValue("big_payload_" + std::to_string(i) + "_" + std::string(2048, 'X'))});
            }
            require(shrinkDomain->stats().liveStrings >= 64u, "Expected large strings registered in domain");
        }
        runtime::RootSet emptyRoots;
        shrinkHeap.collect(emptyRoots);
        const auto beforeCount = shrinkDomain->stats().collectionCount;
        shrinkDomain->collectGarbage();
        const auto statsAfter = shrinkDomain->stats();
        require(statsAfter.collectionCount > beforeCount && statsAfter.liveStrings == 0u,
            "Expected large strings swept cleanly when orphaned");
    }

    // >70 sequential domain create/destroy cycles: verify 6-bit ID recycling + StringInternCache epoch isolation.
    {
        std::vector<std::uint64_t> lastEpochById(64u, 0u);
        bool sawRecycledDomainId = false;
        const std::string sharedKey = "tls_intern_cache_epoch_probe";
        for (int cycle = 0; cycle < 76; ++cycle) {
            auto dom = runtime::ValuePoolDomain::create();
            const std::uint8_t id = dom->domainId();
            const std::uint64_t epoch = dom->domainEpoch();
            require(id > 0u && id < 64u && epoch > 0u, "Invalid domainId/domainEpoch");
            if (lastEpochById[id] != 0u) {
                require(epoch > lastEpochById[id], "Recycled domainId must advance domainEpoch");
                sawRecycledDomainId = true;
            }
            lastEpochById[id] = epoch;

            {
                runtime::ScopedValuePoolDomain scope(dom);
                runtime::StringValue sv(sharedKey);
                require(sv.str() == sharedKey, "TLS intern cache returned corrupt value");
                require(dom->stats().liveStrings >= 1u,
                    "Recycled domainId must miss stale TLS StringInternCache entry via domainEpoch");
            }
        }
        require(sawRecycledDomainId, "Expected >70 cycles to recycle at least one 6-bit domainId");
    }
}

// ---------------------------------------------------------------------------
// 7. Pillar 2: [ValueLifecycle] ManagedHeap Sweep (Holder Class & Arrays)
// ---------------------------------------------------------------------------
void testValueLifecycleHeapSweepResourceServer() {
    const std::string src = R"RS(
module Kernel.ValueLifecycleGc;
import RealScript.Resource;

public class ResHolder {
    public ResourceHandle Handle;
    public ResHolder(ResourceHandle h) {
        Handle = h;
    }
}

public ResHolder CreateHolder(string path) {
    return new ResHolder(ResourceHandle.Load(path));
}

public long SpawnOrphanArray(string path) {
    ResourceHandle[] arr = new ResourceHandle[2];
    arr[0] = ResourceHandle.Load(path);
    arr[1] = arr[0].Retain();
    return arr[0].ResourceId();
}

public int QueryUseCount(long resId) {
    return ResourceServer.GetUseCount(resId);
}

public bool QueryDisposed(long resId) {
    return ResourceServer.IsResourceDisposed(resId);
}
)RS";

    auto program = compileAndLink({{"ValueLifecycleGc.rscript", src}});
    runtime::EngineRuntime rt(program);
    auto& heap = *rt.heap();

    // 1. Orphaned ResourceHolder instance collected via GC -> Dispose() drops useCount to 0.
    std::int64_t holderResId = 0;
    {
        auto pathRef = heap.allocateString("vl_holder_asset.png");
        require(pathRef.has_value(), "allocateString failed");
        const auto holderRes = rt.invoke("Kernel.ValueLifecycleGc::CreateHolder", {runtime::Value{*pathRef}});
        require(holderRes.succeeded && std::holds_alternative<runtime::ObjectRef>(holderRes.value),
            "CreateHolder failed: " + holderRes.error.message);
        const auto holderObj = std::get<runtime::ObjectRef>(holderRes.value);
        auto pin = heap.retain(runtime::Value{holderObj});

        const auto hVal = heap.fieldGet(holderObj, 0);
        require(hVal.has_value() && std::holds_alternative<runtime::StructValue>(*hVal),
            "Missing ResourceHandle field");
        const auto ridField = runtime::structValueFieldAt(std::get<runtime::StructValue>(*hVal), 1);
        require(ridField.has_value() && std::holds_alternative<runtime::LongValue>(*ridField),
            "Missing __resourceId field");
        holderResId = std::get<runtime::LongValue>(*ridField).value;
        require(holderResId > 0, "Expected positive resourceId");

        const auto liveUse = rt.invoke("Kernel.ValueLifecycleGc::QueryUseCount",
            {runtime::Value{runtime::LongValue{holderResId}}});
        require(liveUse.succeeded && std::get<std::int64_t>(liveUse.value) == 1,
            "Expected live useCount == 1 before GC");
    }

    {
        runtime::RootSet emptyRoots;
        heap.collect(emptyRoots);
    }

    const auto afterUse = rt.invoke("Kernel.ValueLifecycleGc::QueryUseCount",
        {runtime::Value{runtime::LongValue{holderResId}}});
    const auto afterDisposed = rt.invoke("Kernel.ValueLifecycleGc::QueryDisposed",
        {runtime::Value{runtime::LongValue{holderResId}}});
    require(afterUse.succeeded && std::get<std::int64_t>(afterUse.value) == 0,
        "Expected GC sweep Dispose() to drop ResourceServer.GetUseCount to 0");
    require(afterDisposed.succeeded && std::get<bool>(afterDisposed.value),
        "Expected swept ResourceHandle resource to report IsResourceDisposed == true");

    // 2. Orphaned ResourceHandle[] array collected via GC -> all elements Dispose().
    auto arrPathRef = heap.allocateString("vl_array_asset.png");
    require(arrPathRef.has_value(), "allocateString failed");
    const auto spawnArrRes = rt.invoke("Kernel.ValueLifecycleGc::SpawnOrphanArray", {runtime::Value{*arrPathRef}});
    require(spawnArrRes.succeeded && std::holds_alternative<runtime::LongValue>(spawnArrRes.value),
        "SpawnOrphanArray failed: " + spawnArrRes.error.message);
    const std::int64_t arrResId = std::get<runtime::LongValue>(spawnArrRes.value).value;

    {
        runtime::RootSet emptyRoots;
        heap.collect(emptyRoots);
    }

    const auto arrUse = rt.invoke("Kernel.ValueLifecycleGc::QueryUseCount",
        {runtime::Value{runtime::LongValue{arrResId}}});
    require(arrUse.succeeded && std::get<std::int64_t>(arrUse.value) == 0,
        "Expected GC sweep of ResourceHandle[] to dispose every element lease");
}

// ---------------------------------------------------------------------------
// 8. Pillar 4: Zero-Copy NativeBufferView + Span<T> + ReadOnly + Revocation
// ---------------------------------------------------------------------------
void testZeroCopyNativeBufferViewsAndRevocation() {
    const std::string src = R"RS(
module Kernel.NativeSpanTest;
import RealScript.Memory;

[Blittable]
public struct Particle {
    public float X;
    public float Y;
    public int Id;
}

public float MutateFloatSpan(Span<float> s) {
    float sum = 0.0f;
    for (int i = 0; i < s.Length; i = i + 1) {
        float v = s.Get(i);
        s.Set(i, v * 2.0f + 1.0f);
        sum = sum + s.Get(i);
    }
    return sum;
}

public float ReadFloatSpan(ReadOnlySpan<float> s) {
    return s.Get(0) + s.Get(s.Length - 1);
}

public int MutateParticles(Span<Particle> s) {
    int totalId = 0;
    for (int i = 0; i < s.Length; i = i + 1) {
        Particle p = s.Get(i);
        p.X = p.X + 10.0f;
        p.Y = p.Y * 3.0f;
        p.Id = p.Id + i;
        s.Set(i, p);
        totalId = totalId + p.Id;
    }
    return totalId;
}

public void WriteFloatAt(Span<float> s, int idx, float v) {
    s.Set(idx, v);
}

public float ReadFloatAt(Span<float> s, int idx) {
    return s.Get(idx);
}
)RS";

    auto program = compileAndLink({{"ValueLifecycleGc.rscript", src}});
    runtime::EngineRuntime rt(program);
    auto& heap = *rt.heap();

    const semantic::SymbolId particleTid = semantic::stableTypeId("Kernel.NativeSpanTest::Particle");
    const auto resolveParamTypeId = [&](const std::string& fnName) -> semantic::SymbolId {
        if (const auto sid = program->findFunction(fnName)) {
            if (const auto* loc = program->resolveFunction(*sid);
                loc && loc->function && !loc->function->parameterTypeIds.empty()) {
                return loc->function->parameterTypeIds[0];
            }
        }
        return 0;
    };
    const semantic::SymbolId effSpanFloatTid =
        resolveParamTypeId("Kernel.NativeSpanTest::MutateFloatSpan");
    const semantic::SymbolId effRoSpanFloatTid =
        resolveParamTypeId("Kernel.NativeSpanTest::ReadFloatSpan");
    const semantic::SymbolId resolvedSpanParticleTid =
        resolveParamTypeId("Kernel.NativeSpanTest::MutateParticles");
    require(effSpanFloatTid != 0 && effRoSpanFloatTid != 0 && resolvedSpanParticleTid != 0,
        "Expected resolved Span parameter type IDs");

    // 1. Float[256] zero-copy NativeBufferView mutated in place via Span<float>.
    std::vector<float> hostFloats(256u, 1.5f);
    runtime::NativeBufferViewDesc floatDesc{};
    floatDesc.data = hostFloats.data();
    floatDesc.elementCount = hostFloats.size();
    floatDesc.elementType = semantic::PrimitiveType::Float;
    floatDesc.elementTypeId = 0;
    floatDesc.strideBytes = sizeof(float);
    floatDesc.readOnly = false;

    runtime::RuntimeError nbErr;
    const auto floatBufRef = heap.allocateNativeBufferView(
        semantic::stableTypeId("float[]"), floatDesc, &nbErr);
    require(floatBufRef.has_value(), "allocateNativeBufferView(float[]) failed: " + nbErr.message);
    auto floatPin = heap.retain(runtime::Value{*floatBufRef});

    const auto floatSpanVal = heap.makeNativeSpanValue(
        effSpanFloatTid, *floatBufRef, 0, static_cast<std::int32_t>(hostFloats.size()));
    const auto mutateRes = rt.invoke("Kernel.NativeSpanTest::MutateFloatSpan", {floatSpanVal});
    require(mutateRes.succeeded, "MutateFloatSpan failed: " + mutateRes.error.message);
    require(hostFloats[0] == 4.0f && hostFloats[255] == 4.0f,
        "Expected in-place C++ float[256] mutation via Span<float>");

    // 2. [Blittable] struct Particle[] zero-copy NativeBufferView mutated in place via Span<Particle>.
    struct HostParticle {
        float x;
        float y;
        std::int32_t id;
    };
    static_assert(sizeof(HostParticle) == 12, "Particle natural packed size mismatch");
    std::vector<HostParticle> hostParticles(8u, HostParticle{1.0f, 2.0f, 10});
    runtime::NativeBufferViewDesc partDesc{};
    partDesc.data = hostParticles.data();
    partDesc.elementCount = hostParticles.size();
    partDesc.elementType = semantic::PrimitiveType::Struct;
    partDesc.elementTypeId = particleTid;
    partDesc.strideBytes = sizeof(HostParticle);
    partDesc.readOnly = false;

    const auto partBufRef = heap.allocateNativeBufferView(
        semantic::stableTypeId("Kernel.NativeSpanTest::Particle[]"), partDesc, &nbErr);
    require(partBufRef.has_value(), "allocateNativeBufferView(Particle[]) failed: " + nbErr.message);
    auto partPin = heap.retain(runtime::Value{*partBufRef});

    const auto partSpanVal = heap.makeNativeSpanValue(
        resolvedSpanParticleTid, *partBufRef, 0, static_cast<std::int32_t>(hostParticles.size()));
    const auto partMutRes = rt.invoke("Kernel.NativeSpanTest::MutateParticles", {partSpanVal});
    require(partMutRes.succeeded, "MutateParticles failed: " + partMutRes.error.message);
    require(hostParticles[0].x == 11.0f && hostParticles[0].y == 6.0f && hostParticles[0].id == 10 &&
            hostParticles[7].x == 11.0f && hostParticles[7].y == 6.0f && hostParticles[7].id == 17,
        "Expected in-place C++ Particle[] mutation via Span<Particle>");

    // 3. ReadOnly NativeBufferView & ReadOnlySpan<float>: read succeeds, write rejected cleanly.
    std::vector<float> roFloats(4u, 7.5f);
    runtime::NativeBufferViewDesc roDesc{};
    roDesc.data = roFloats.data();
    roDesc.elementCount = roFloats.size();
    roDesc.elementType = semantic::PrimitiveType::Float;
    roDesc.strideBytes = sizeof(float);
    roDesc.readOnly = true;

    const auto roBufRef = heap.allocateNativeBufferView(
        semantic::stableTypeId("float[]"), roDesc, &nbErr);
    require(roBufRef.has_value(), "allocateNativeBufferView(ro float[]) failed: " + nbErr.message);
    auto roPin = heap.retain(runtime::Value{*roBufRef});

    const auto roSpanVal = heap.makeNativeSpanValue(
        effRoSpanFloatTid, *roBufRef, 0, static_cast<std::int32_t>(roFloats.size()));
    const auto roReadRes = rt.invoke("Kernel.NativeSpanTest::ReadFloatSpan", {roSpanVal});
    require(roReadRes.succeeded, "ReadFloatSpan on readOnly native buffer failed");

    const auto rwViewOfRoBuf = heap.makeNativeSpanValue(
        effSpanFloatTid, *roBufRef, 0, static_cast<std::int32_t>(roFloats.size()));
    const auto roWriteTrap = rt.invoke("Kernel.NativeSpanTest::WriteFloatAt",
        {rwViewOfRoBuf, runtime::Value{std::int64_t{0}}, runtime::Value{runtime::FloatValue{99.0f}}});
    require(!roWriteTrap.succeeded && roFloats[0] == 7.5f,
        "Write to readOnly NativeBufferView must trap without mutating C++ memory");

    // 4. Revocation traps deterministically with ErrorCode::InvalidObjectReference.
    require(heap.revokeNativeBufferView(*floatBufRef), "revokeNativeBufferView failed");
    require(heap.isNativeBufferViewRevoked(*floatBufRef), "Expected revoked flag on NativeBufferView");
    const auto postRevokeRead = rt.invoke("Kernel.NativeSpanTest::ReadFloatAt",
        {floatSpanVal, runtime::Value{std::int64_t{0}}});
    require(!postRevokeRead.succeeded &&
            postRevokeRead.error.code == runtime::ErrorCode::InvalidObjectReference,
        "Post-revoke NativeBufferView access must trap with InvalidObjectReference");
}

// ---------------------------------------------------------------------------
// 9. Pillar 6: ManagedHeap::trimExcessCapacity Reclamation & ABA Safety
// ---------------------------------------------------------------------------
void testManagedHeapTrimExcessCapacity() {
    runtime::ManagedHeap heap;
    {
        std::vector<runtime::PersistentRoot> pins;
        for (int i = 0; i < 1200; ++i) {
            auto arr = heap.allocateArray(256, runtime::Value{std::int64_t{i}});
            require(arr.has_value(), "burst array allocation failed");
            if ((i % 4) == 0) {
                pins.push_back(heap.retain(runtime::Value{*arr}));
            }
        }
    }
    runtime::RootSet emptyRoots;
    heap.collect(emptyRoots);
    const std::size_t trimmedBytes = heap.trimExcessCapacity();
    require(trimmedBytes > 0u, "Expected trimExcessCapacity to reclaim > 0 bytes after burst peak");

    // Verify subsequent allocations, weak references, and GC cycles remain ABA-safe.
    auto obj1 = heap.allocateRecord(4u);
    require(obj1.has_value(), "post-trim record alloc failed");
    heap.fieldSet(*obj1, 0u, runtime::Value{std::int64_t{4242}});
    const runtime::ObjectRef weak1 = *obj1;
    {
        auto pin1 = heap.retain(runtime::Value{*obj1});
        heap.collect(emptyRoots);
        require(heap.isAlive(weak1), "Pinned object ref must stay alive after GC");
    }
    heap.collect(emptyRoots);
    require(!heap.isAlive(weak1), "Unpinned object ref must become stale after GC");
    auto obj2 = heap.allocateRecord(4u);
    require(obj2.has_value() && !heap.isAlive(weak1),
        "Slot reuse after trimExcessCapacity must preserve generation ABA safety");
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
            {"Pillar 1: ValuePoolDomain concurrency, >20k single shard, shrink & >70 cycles", testValuePoolDomainsConcurrencyScaleAndEpochIsolation},
            {"Pillar 2: [ValueLifecycle] ManagedHeap sweep & ResourceServer useCount", testValueLifecycleHeapSweepResourceServer},
            {"Pillar 4: Zero-copy NativeBufferView, Span<T>, readOnly & revocation", testZeroCopyNativeBufferViewsAndRevocation},
            {"Pillar 6: ManagedHeap::trimExcessCapacity & ABA safety", testManagedHeapTrimExcessCapacity},
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
