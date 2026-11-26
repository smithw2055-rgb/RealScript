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

// ---------------------------------------------------------------------------
// 10. L0 Dirty Callbacks, Dynamic Array Ops, UTF-8/Borrowed, Weak Delegates & Policy
// ---------------------------------------------------------------------------
struct L0CallbackLog {
    std::size_t fieldCallbacks = 0;
    runtime::ObjectRef lastFieldObj{};
    std::size_t lastFieldIndex = 0;
    std::uint8_t lastDirtyBit = 0xFFu;
    std::vector<runtime::ArrayMutationOp> arrayOps;
    std::vector<runtime::ObjectRef> sweptRefs;
};

void onL0FieldMutated(
    void* userData,
    runtime::ManagedHeap&,
    runtime::ObjectRef object,
    std::size_t fieldIndex,
    std::uint8_t dirtyBit) {
    auto* log = static_cast<L0CallbackLog*>(userData);
    ++log->fieldCallbacks;
    log->lastFieldObj = object;
    log->lastFieldIndex = fieldIndex;
    log->lastDirtyBit = dirtyBit;
}

void onL0ArrayMutated(
    void* userData,
    runtime::ManagedHeap&,
    runtime::ObjectRef,
    runtime::ArrayMutationOp op,
    std::size_t,
    const runtime::Value&,
    const runtime::Value&) {
    auto* log = static_cast<L0CallbackLog*>(userData);
    log->arrayOps.push_back(op);
}

void onL0SlotSwept(
    void* userData,
    runtime::ManagedHeap&,
    runtime::ObjectRef sweptRef) {
    auto* log = static_cast<L0CallbackLog*>(userData);
    log->sweptRefs.push_back(sweptRef);
}

class InterceptCallPolicy final : public runtime::IExecutionPolicy {
public:
    semantic::SymbolId targetFn = 0;
    bool interceptEnabled = true;
    std::size_t seenCalls = 0;

    bool interceptCall(
        semantic::SymbolId functionId,
        runtime::ValueSpan arguments,
        bool& outIntercepted,
        runtime::Value& outResult,
        runtime::RuntimeError&) override {
        if (functionId == targetFn && interceptEnabled) {
            ++seenCalls;
            outIntercepted = true;
            const std::int64_t arg0 =
                (!arguments.empty() && std::holds_alternative<std::int64_t>(arguments[0]))
                    ? std::get<std::int64_t>(arguments[0])
                    : 0;
            outResult = runtime::Value{arg0 * 10 + 7};
            return true;
        }
        outIntercepted = false;
        return true;
    }
};

void testL0DirtyCallbacksArraysUtf8WeakDelegatesAndPolicy() {
    // A. Dirty tracking, observation flags, dynamic array ops & slot sweep callback
    {
        runtime::ManagedHeap heap;
        L0CallbackLog log{};
        heap.setFieldMutationCallback(&onL0FieldMutated, &log);
        heap.setArrayMutationCallback(&onL0ArrayMutated, &log);
        heap.setSlotSweepCallback(&onL0SlotSwept, &log);

        const auto baseType = semantic::stableTypeId("L0Test::BaseObj");
        const auto derivedType = semantic::stableTypeId("L0Test::DerivedObj");
        heap.registerTypeParent(derivedType, baseType);
        heap.enableTypeDirtyTracking(derivedType, {2u, 5u});

        auto objOpt = heap.allocateObject(
            derivedType,
            {runtime::Value{std::int64_t{10}}, runtime::Value{std::int64_t{20}}},
            {});
        require(objOpt.has_value(), "allocateObject failed");
        const auto objRef = *objOpt;
        require(heap.setObjectObserved(objRef, true) && heap.isObjectObserved(objRef),
            "setObjectObserved/isObjectObserved failed");

        require(heap.fieldSet(objRef, 1u, runtime::Value{std::int64_t{99}}), "fieldSet failed");
        require(log.fieldCallbacks >= 1u && log.lastFieldObj == objRef &&
                log.lastFieldIndex == 1u && log.lastDirtyBit == 5u,
            "FieldMutationCallback did not report expected dirtyBit");
        require((heap.peekDirtyMask(objRef) & (1ULL << 5u)) != 0ULL,
            "peekDirtyMask missing bit 5 after fieldSet");

        require(heap.markDirtyBit(objRef, 11u), "markDirtyBit failed");
        const auto mask = heap.consumeDirtyMask(objRef);
        require((mask & ((1ULL << 5u) | (1ULL << 11u))) == ((1ULL << 5u) | (1ULL << 11u)) &&
                heap.peekDirtyMask(objRef) == 0ULL,
            "consumeDirtyMask must return combined mask and reset to 0");
        const auto dirtyObjs = heap.consumeDirtyObjects();
        require(!dirtyObjs.empty(), "Expected non-empty consumeDirtyObjects");

        auto arrOpt = heap.allocateArray(0);
        require(arrOpt.has_value(), "allocateArray failed");
        const auto arrRef = *arrOpt;
        require(heap.setArrayObserved(arrRef, true) && heap.isArrayObserved(arrRef),
            "setArrayObserved/isArrayObserved failed");
        auto arrPin = heap.retain(runtime::Value{arrRef});
        require(heap.arrayPush(arrRef, runtime::Value{std::int64_t{10}}), "arrayPush failed");
        require(heap.arrayInsert(arrRef, 0u, runtime::Value{std::int64_t{5}}), "arrayInsert failed");
        require(heap.arraySet(arrRef, 1u, runtime::Value{std::int64_t{15}}), "arraySet failed");
        runtime::Value removedVal;
        require(heap.arrayRemoveAt(arrRef, 0u, &removedVal) &&
                std::get<std::int64_t>(removedVal) == 5,
            "arrayRemoveAt failed");
        require(heap.arrayClear(arrRef) && heap.arrayLength(arrRef) == std::optional<std::size_t>(0),
            "arrayClear failed");
        require(log.arrayOps.size() == 5u &&
                log.arrayOps[0] == runtime::ArrayMutationOp::Push &&
                log.arrayOps[1] == runtime::ArrayMutationOp::Insert &&
                log.arrayOps[2] == runtime::ArrayMutationOp::Set &&
                log.arrayOps[3] == runtime::ArrayMutationOp::RemoveAt &&
                log.arrayOps[4] == runtime::ArrayMutationOp::Clear,
            "ArrayMutationCallback ops mismatch");

        // Trigger GC sweep on unpinned objRef and verify SlotSweepCallback fires.
        heap.collect(runtime::RootSet{});
        bool sawObjSwept = false;
        for (const auto swept : log.sweptRefs) {
            if (swept == objRef) {
                sawObjSwept = true;
                break;
            }
        }
        require(sawObjSwept, "SlotSweepCallback did not fire for collected ObjectRef");
    }

    // B. UTF-8 helpers + NativeHandleRegistry::createBorrowed
    {
        require(runtime::utf8IsAsciiFast("Hello_L0"), "ASCII check failed");
        require(!runtime::utf8IsAsciiFast("Hi\xE4\xB8\xAD"), "Non-ASCII check failed");
        const std::string utf8 = "A\xE4\xB8\xAD\xF0\x9F\x9A\x80Z"; // A + 中 + rocket + Z
        require(runtime::utf8CodePointLength(utf8) == 4u, "utf8CodePointLength expected 4");
        require(runtime::utf8CodePointSlice(utf8, 1u, 2u) == "\xE4\xB8\xAD\xF0\x9F\x9A\x80",
            "utf8CodePointSlice failed");
        require(runtime::utf8CodePointIndexOf(utf8, "\xF0\x9F\x9A\x80") == 2,
            "utf8CodePointIndexOf failed");
        require(runtime::utf8CodePointLastIndexOf("ab\xE4\xB8\xAD" "ab", "ab") == 3,
            "utf8CodePointLastIndexOf failed");
        std::u16string utf16;
        std::string roundTrip;
        runtime::utf8ToUtf16(utf8, utf16);
        runtime::utf16ToUtf8(utf16, roundTrip);
        require(roundTrip == utf8, "utf8 <-> utf16 roundtrip mismatch");

        runtime::NativeHandleRegistry handles;
        int dummyNative = 123;
        const auto handleType = semantic::stableTypeId("L0Test::NativeObj");
        const auto h = handles.createBorrowed(handleType, &dummyNative, true);
        require(h.valid() && handles.findHandle(handleType, &dummyNative) == std::optional<runtime::NativeHandle>(h),
            "createBorrowed / findHandle failed");
        runtime::ManagedHeap heap;
        auto wOpt = heap.allocateRecord(1u);
        require(wOpt.has_value(), "allocateRecord for wrapper failed");
        require(handles.setCachedWrapper(h, *wOpt, heap.heapId()), "setCachedWrapper failed");
        require(handles.cachedWrapper(h, heap.heapId()) == wOpt, "cachedWrapper lookup failed");
        require(handles.removeCachedWrapper(h, heap.heapId()), "removeCachedWrapper failed");
        require(!handles.cachedWrapper(h, heap.heapId()).has_value(), "Expected cleared wrapper");
    }

    // C. Weak delegates (live, GC-expired void & non-void, multicast prune) + IExecutionPolicy::interceptCall
    {
        const std::string src = R"RS(
module Kernel.WeakAndPolicy;
public delegate int IntAction();
public delegate void VoidAction();
public class Target {
    public int id = 0;
    public int GetVal() { return id + 70; }
    public void Ping() { id = id + 10; }
}
public int RunInt(IntAction d) { return d(); }
public void RunVoid(VoidAction d) { d(); }
public VoidAction CombineVoid(VoidAction a, VoidAction b) { return a + b; }
public int PolicyTarget(int x) { return x + 100; }
)RS";
        auto prog = compileAndLink({{"weak_policy.rscript", src}});
        auto heap = std::make_shared<runtime::ManagedHeap>();
        runtime::Interpreter interp(prog, heap);

        semantic::SymbolId getValId = 0;
        semantic::SymbolId pingId = 0;
        for (const auto& mod : prog->modules()) {
            for (const auto& fn : mod.functions) {
                if (fn.name.find("Target.GetVal") != std::string::npos) getValId = fn.symbolId;
                if (fn.name.find("Target.Ping") != std::string::npos) pingId = fn.symbolId;
            }
        }
        require(getValId != 0 && pingId != 0, "Could not locate Target method SymbolIds");

        const auto targetTypeId = semantic::stableTypeId("Kernel.WeakAndPolicy::Target");
        const auto intDelTypeId = semantic::stableTypeId("Kernel.WeakAndPolicy::IntAction");
        const auto voidDelTypeId = semantic::stableTypeId("Kernel.WeakAndPolicy::VoidAction");

        auto t1Opt = heap->allocateObject(targetTypeId, {runtime::Value{std::int64_t{7}}}, {});
        auto t2Opt = heap->allocateObject(targetTypeId, {runtime::Value{std::int64_t{1}}}, {});
        require(t1Opt.has_value() && t2Opt.has_value(), "Target allocation failed");
        const auto t1 = *t1Opt;
        const auto t2 = *t2Opt;

        auto pinT1 = heap->retain(runtime::Value{t1});
        auto pinT2 = heap->retain(runtime::Value{t2});

        auto weakIntOpt = heap->allocateWeakDelegate(intDelTypeId, t2, getValId);
        auto weakVoidLiveOpt = heap->allocateWeakDelegate(voidDelTypeId, t1, pingId);
        auto weakVoidDeadOpt = heap->allocateWeakDelegate(voidDelTypeId, t2, pingId);
        require(weakIntOpt.has_value() && weakVoidLiveOpt.has_value() && weakVoidDeadOpt.has_value(),
            "allocateWeakDelegate failed");

        auto pinWeakInt = heap->retain(runtime::Value{*weakIntOpt});
        const auto combRes = interp.invoke(
            "Kernel.WeakAndPolicy::CombineVoid",
            {runtime::Value{*weakVoidLiveOpt}, runtime::Value{*weakVoidDeadOpt}});
        require(combRes.succeeded && std::holds_alternative<runtime::ObjectRef>(combRes.value),
            "CombineVoid failed");
        auto pinComb = heap->retain(combRes.value);

        // 1. Live invocation succeeds
        const auto liveInt = interp.invoke(
            "Kernel.WeakAndPolicy::RunInt", {runtime::Value{*weakIntOpt}});
        require(liveInt.succeeded && std::get<std::int64_t>(liveInt.value) == 71,
            "Live weak IntAction expected 71");

        // 2. Drop pinT2 and collect GC -> t2 dies, t1 stays alive!
        pinT2.reset();
        heap->collect(runtime::RootSet{});
        require(!heap->isAlive(t2) && heap->isAlive(t1), "GC state for weak targets mismatch");

        // Expired non-void weak delegate returns default int 0 without crashing
        const auto deadInt = interp.invoke(
            "Kernel.WeakAndPolicy::RunInt", {runtime::Value{*weakIntOpt}});
        require(deadInt.succeeded && std::holds_alternative<std::int64_t>(deadInt.value) &&
                std::get<std::int64_t>(deadInt.value) == 0,
            "Expired non-void weak delegate must return default int 0");

        // Multicast weak invocation runs live leaf (t1.id += 10) and skips dead t2 cleanly
        const auto multiRun = interp.invoke("Kernel.WeakAndPolicy::RunVoid", {combRes.value});
        require(multiRun.succeeded, "Multicast weak RunVoid failed");
        require(heap->fieldGet(t1, 0u) == std::optional<runtime::Value>(runtime::Value{std::int64_t{17}}),
            "Expected live leaf t1.id to become 17");

        // pruneDelegateChain prunes dead leaf and leaves live leaf
        const auto pruned = heap->pruneDelegateChain(combRes.value);
        require(std::holds_alternative<runtime::ObjectRef>(pruned) &&
                std::get<runtime::ObjectRef>(pruned) != std::get<runtime::ObjectRef>(combRes.value),
            "pruneDelegateChain should collapse multicast to remaining live leaf");
        const auto deadPrune = heap->pruneDelegateChain(runtime::Value{*weakIntOpt});
        require(std::holds_alternative<runtime::NullObject>(deadPrune),
            "pruneDelegateChain on expired single weak delegate must return NullObject");

        // 3. IExecutionPolicy::interceptCall
        const auto policyFnOpt = prog->findFunction("Kernel.WeakAndPolicy::PolicyTarget");
        require(policyFnOpt.has_value(), "PolicyTarget function not found");
        InterceptCallPolicy policy;
        policy.targetFn = *policyFnOpt;
        interp.setExecutionPolicy(&policy);

        const auto interceptedRes = interp.invoke(
            *policyFnOpt, {runtime::Value{std::int64_t{5}}});
        require(interceptedRes.succeeded && std::get<std::int64_t>(interceptedRes.value) == 57 &&
                policy.seenCalls == 1u,
            "IExecutionPolicy::interceptCall expected 57");

        policy.interceptEnabled = false;
        const auto passthroughRes = interp.invoke(
            *policyFnOpt, {runtime::Value{std::int64_t{5}}});
        require(passthroughRes.succeeded && std::get<std::int64_t>(passthroughRes.value) == 105,
            "IExecutionPolicy bypass expected 105");
        interp.setExecutionPolicy(nullptr);
    }
}

void testZeroLockProfileRingBufferAndChromeTrace() {
    const std::string src = R"RS(
module Kernel.ProfTrace;

public int LeafWork(int n) {
    int acc = 0;
    for (int i = 0; i < n; i = i + 1) {
        acc = acc + (i * 3) + 1;
    }
    return acc;
}

public int InnerCall(int n) {
    int s = 0;
    for (int j = 0; j < 4; j = j + 1) {
        s = s + LeafWork(n);
    }
    return s;
}

public int OuterRoot(int n) {
    int total = 0;
    for (int k = 0; k < 3; k = k + 1) {
        total = total + InnerCall(n);
    }
    return total;
}
)RS";

    auto prog = compileAndLink({{"prof_trace.rscript", src}}, optimization::Level::None);
    auto heap = std::make_shared<runtime::ManagedHeap>();
    runtime::Interpreter interp(prog, heap);

    auto collector = std::make_shared<runtime::ProfileCollector>();
    runtime::ExecutionOptions opts;
    opts.profile = collector;

    const auto res = interp.invoke("Kernel.ProfTrace::OuterRoot", {runtime::Value{std::int64_t{40}}}, opts);
    require(res.succeeded && std::holds_alternative<std::int64_t>(res.value),
        "OuterRoot execution failed: " + res.error.message);

    const runtime::ExecutionProfile prof = collector->snapshot();
    std::uint64_t sumCalls = 0;
    std::uint64_t sumReturns = 0;
    for (const auto& fnProf : prof.functions) {
        sumCalls += fnProf.calls;
        sumReturns += fnProf.returns;
    }
    require(sumCalls == 16u && sumReturns == 16u,
        "Expected 1 OuterRoot + 3 InnerCall + 12 LeafWork = 16 calls/returns, got calls=" +
            std::to_string(sumCalls) + " returns=" + std::to_string(sumReturns));
    require(prof.traceEvents.size() == 16u,
        "Expected 16 ring-buffered steady_clock trace spans, got " +
            std::to_string(prof.traceEvents.size()));

    bool sawOuter = false;
    bool sawInner = false;
    bool sawLeaf = false;
    runtime::ProfileTraceEvent outerSpan{};
    runtime::ProfileTraceEvent innerSpan{};
    for (const auto& ev : prof.traceEvents) {
        const std::uint64_t evEndNs = ev.timestampNs + ev.durationNs;
        require(evEndNs >= ev.timestampNs,
            "Trace event must have monotonic steady_clock timestamps");
        if (ev.function == "Kernel.ProfTrace::OuterRoot") {
            sawOuter = true;
            outerSpan = ev;
        } else if (ev.function == "Kernel.ProfTrace::InnerCall") {
            if (!sawInner) {
                sawInner = true;
                innerSpan = ev;
            }
        } else if (ev.function == "Kernel.ProfTrace::LeafWork") {
            sawLeaf = true;
        }
    }
    require(sawOuter && sawInner && sawLeaf,
        "Expected traceEvents for OuterRoot, InnerCall, and LeafWork");
    require(outerSpan.callDepth < innerSpan.callDepth &&
            outerSpan.timestampNs <= innerSpan.timestampNs &&
            (outerSpan.timestampNs + outerSpan.durationNs) >=
                (innerSpan.timestampNs + innerSpan.durationNs),
        "Parent OuterRoot steady_clock span must hierarchically enclose child InnerCall span");

    const std::string chromeJson = runtime::exportChromeTraceJson(prof);
    require(chromeJson.find("\"ph\":\"X\"") != std::string::npos &&
            chromeJson.find("\"dur\":") != std::string::npos &&
            chromeJson.find("\"ts\":") != std::string::npos &&
            chromeJson.find("\"functionId\":") != std::string::npos &&
            chromeJson.find("\"callDepth\":") != std::string::npos &&
            chromeJson.find("Kernel.ProfTrace::OuterRoot") != std::string::npos &&
            chromeJson.find("Kernel.ProfTrace::InnerCall") != std::string::npos &&
            chromeJson.find("Kernel.ProfTrace::LeafWork") != std::string::npos,
        "exportChromeTraceJson must emit real steady_clock 'X' flamegraph events with ts/dur");

    // Verify fallback path when no ring-buffer trace spans exist
    runtime::ExecutionProfile legacyOnly;
    runtime::FunctionProfile fp;
    fp.function = "Legacy::Synthetic";
    fp.calls = 2;
    fp.returns = 2;
    fp.instructions = 25;
    legacyOnly.functions.push_back(fp);
    const std::string legacyJson = runtime::exportChromeTraceJson(legacyOnly);
    require(legacyJson.find("\"name\":\"Legacy::Synthetic\"") != std::string::npos &&
            legacyJson.find("\"ph\":\"X\"") != std::string::npos &&
            legacyJson.find("\"dur\":25") != std::string::npos &&
            legacyJson.find("\"functionId\":") == std::string::npos,
        "exportChromeTraceJson must fall back cleanly to legacy instruction counters when traceEvents is empty");

    // Verify ring-buffer wraparound (>8192 events) stays crash-free and preserves exact total summary counts
    for (std::size_t i = 0; i < 9000u; ++i) {
        collector->record(runtime::TraceEvent{
            runtime::TraceEventKind::FunctionEnter, "Stress::Leaf", {}, 0u, 2u});
        collector->record(runtime::TraceEvent{
            runtime::TraceEventKind::FunctionExit, "Stress::Leaf", {}, 0u, 2u});
        collector->recordTraceSpan(77u, "Stress::Leaf", 1000u + i, 200u, 2u);
    }
    const auto afterWrap = collector->snapshot();
    std::uint64_t wrapCalls = 0;
    for (const auto& fnProf : afterWrap.functions) {
        wrapCalls += fnProf.calls;
    }
    require(afterWrap.totalEvents >= 16u &&
            wrapCalls <= 16u + 9000u &&
            afterWrap.traceEvents.size() <= 16u + runtime::ProfileCollector::TraceRingCapacity,
        "Ring-buffer wraparound must overwrite unread slots safely without crashing");
    collector->reset();
    const auto cleared = collector->snapshot();
    require(cleared.functions.empty() && cleared.totalEvents == 0u && cleared.traceEvents.empty(),
        "ProfileCollector::reset() must clear both counters and traceEvents");
}

void testFlatLoopExecutionPolicyAndFastOpcodes() {
    const std::string src = R"RS(
module Kernel.FlatPolicyAndFastOps;

public enum Kind { Zero = 0, Alpha = 5, Beta = 12 }
public delegate int IntOp(int x);

public class Box {
    public int value = 1;
    public string label = "init";
    public int Add(int x) { return value + x; }
    public virtual int Score() { return value + 10; }
    public virtual void SetVal(int v) { value = v; }
}

public class SubBox : Box {
    public override int Score() { return value + 100; }
    public override void SetVal(int v) { value = v; }
}

public class PlainBox {
    public int value = 0;
}

public Kind PickKind(int x) {
    return x > 0 ? Kind.Beta : Kind.Alpha;
}

public int UsePickedKind(int x) {
    Kind k = PickKind(x);
    return k == Kind.Beta ? 12 : 5;
}

public string SliceCharAt(string s, int i) {
    return s.Substring(i, 1);
}

public int RunPolySetVal(Box b, int v) {
    b.SetVal(v);
    return b.Score();
}

public int MutateBothBoxes(Box b, PlainBox p, int v) {
    b.SetVal(v);
    p.value = v + 1;
    return b.Score() + p.value;
}

public int LeafMul(int x) {
    return x * 3 + 1;
}

public int InvokeViaDelegates(IntOp a, IntOp b, int x) {
    return a(x) + b(x);
}

public int RunDelegatesWithPolicy(int x) {
    Box b = new Box();
    b.value = 20;
    return InvokeViaDelegates(LeafMul, b.Add, x);
}

public int RunInlinedAndPoly(int n) {
    int sum = 0;
    for (int i = 0; i < n; i = i + 1) {
        sum = sum + LeafMul(i);
    }
    Box b = new SubBox();
    b.value = 7;
    return sum + b.Score();
}

public int MutateField(Box b, int v, string lbl) {
    b.value = v;
    b.label = lbl;
    return b.value;
}

public int MutateArray(int[] arr, int idx, int val) {
    arr[idx] = val;
    return arr[idx];
}

public int ExerciseFastOpcodes(string raw, int signedIn) {
    string trimmed = raw.Trim();
    string lower = trimmed.ToLower();
    string upper = trimmed.ToUpper();
    string rebuilt = "ab" + "cd";
    string nullStr = null;
    Box nullBox = null;
    int[] nullArr = null;
    int acc = 0;
    if (trimmed == "AbCd") acc = acc + 10;
    if (lower == "abcd") acc = acc + 20;
    if (upper == "ABCD") acc = acc + 40;
    if (rebuilt == trimmed.ToLower()) acc = acc + 80;
    if (trimmed != "nope") acc = acc + 100;
    if (nullStr == null && nullBox == null && nullArr == null) acc = acc + 200;
    if (trimmed.Contains("bC") && trimmed.StartsWith("Ab") && trimmed.EndsWith("Cd")) {
        acc = acc + 400;
    }
    int singleMatches = 0;
    for (int i = 0; i < upper.Length; i = i + 1) {
        string ch = upper.Substring(i, 1);
        if (ch == "A" || ch == "B" || ch == "C" || ch == "D") {
            singleMatches = singleMatches + 1;
        }
    }
    Kind k = Kind.Beta;
    int kInt = (k == Kind.Beta && k != Kind.Alpha) ? 12 : 0;
    int neg = -signedIn;
    uint u1 = 10u;
    uint u2 = 25u;
    uint uSum = u1 + u2;
    uint uDiff = u2 - u1;
    uint uProd = u1 * 3u;
    int uConv = (int)u1;
    if (u1 < u2 && uDiff == 15u && uSum == 35u && uProd == 30u) {
        acc = acc + 800;
    }
    int[] list = new int[0];
    list.Push(11);
    list.Push(22);
    list.RemoveAt(0);
    return acc + singleMatches * 1000 + kInt + uConv + neg + list[0];
}
)RS";

    auto prog = compileAndLink({{"flat_policy_fast_ops.rscript", src}}, optimization::Level::Basic);
    auto heap = std::make_shared<runtime::ManagedHeap>();
    runtime::Interpreter interp(prog, heap);

    struct DetailedPolicy final : public runtime::IExecutionPolicy {
        semantic::SymbolId blockedLeafFn = 0;
        semantic::SymbolId interceptPolyFn = 0;
        semantic::SymbolId interceptPolyVoidFn = 0;
        bool rejectCall = false;
        bool interceptCallActive = false;
        bool rejectFieldWrite = false;
        bool rejectArrayWrite = false;
        std::size_t fieldWritesSeen = 0;
        std::size_t arrayWritesSeen = 0;
        std::size_t callsIntercepted = 0;
        semantic::SymbolId lastDeclTypeId = 0;
        std::string lastFieldName;

        bool checkFieldWrite(
            const runtime::ManagedHeap*,
            const runtime::ManagedHeap*,
            runtime::ObjectRef,
            semantic::SymbolId declTypeId,
            std::size_t,
            std::string_view fieldName,
            const runtime::Value&,
            runtime::RuntimeError& outError) const override {
            const_cast<DetailedPolicy*>(this)->fieldWritesSeen += 1u;
            const_cast<DetailedPolicy*>(this)->lastDeclTypeId = declTypeId;
            const_cast<DetailedPolicy*>(this)->lastFieldName = std::string(fieldName);
            if (rejectFieldWrite) {
                outError.code = runtime::ErrorCode::CapabilityDenied;
                outError.message = "policy denied field write in FlatLoop";
                return false;
            }
            return true;
        }

        bool checkArrayWrite(
            const runtime::ManagedHeap*,
            const runtime::ManagedHeap*,
            runtime::ObjectRef,
            const runtime::Value&,
            runtime::RuntimeError& outError) const override {
            const_cast<DetailedPolicy*>(this)->arrayWritesSeen += 1u;
            if (rejectArrayWrite) {
                outError.code = runtime::ErrorCode::CapabilityDenied;
                outError.message = "policy denied array write in FlatLoop";
                return false;
            }
            return true;
        }

        bool interceptCall(
            semantic::SymbolId functionId,
            runtime::ValueSpan arguments,
            bool& outIntercepted,
            runtime::Value& outResult,
            runtime::RuntimeError& outError) override {
            if (functionId == blockedLeafFn) {
                if (rejectCall) {
                    outError.code = runtime::ErrorCode::CapabilityDenied;
                    outError.message = "policy denied inlined leaf call";
                    return false;
                }
                if (interceptCallActive) {
                    ++callsIntercepted;
                    const std::int64_t x = (!arguments.empty() && std::holds_alternative<std::int64_t>(arguments[0]))
                        ? std::get<std::int64_t>(arguments[0])
                        : 0;
                    outIntercepted = true;
                    outResult = runtime::Value{x + 50};
                    return true;
                }
            }
            if (functionId == interceptPolyFn && interceptCallActive) {
                ++callsIntercepted;
                outIntercepted = true;
                outResult = runtime::Value{std::int64_t{999}};
                return true;
            }
            if (functionId == interceptPolyVoidFn && interceptCallActive) {
                ++callsIntercepted;
                outIntercepted = true;
                outResult = std::monostate{};
                return true;
            }
            outIntercepted = false;
            return true;
        }
    };

    const auto leafFn = prog->findFunction("Kernel.FlatPolicyAndFastOps::LeafMul");
    const auto polyFn = prog->findFunction("Kernel.FlatPolicyAndFastOps::SubBox.Score");
    const auto polySetFn = prog->findFunction("Kernel.FlatPolicyAndFastOps::SubBox.SetVal");
    require(leafFn.has_value() && polyFn.has_value() && polySetFn.has_value(),
        "Expected LeafMul, SubBox.Score and SubBox.SetVal functions");

    DetailedPolicy policy;
    policy.blockedLeafFn = *leafFn;
    policy.interceptPolyFn = *polyFn;
    policy.interceptPolyVoidFn = *polySetFn;
    interp.setExecutionPolicy(&policy);

    // 1. Passthrough matches baseline (LeafMul(0)+LeafMul(1)+LeafMul(2) = 1+4+7 = 12; SubBox.Score() = 107 -> 119)
    const auto passRes = interp.invoke(
        "Kernel.FlatPolicyAndFastOps::RunInlinedAndPoly", {runtime::Value{std::int64_t{3}}});
    require(passRes.succeeded && std::get<std::int64_t>(passRes.value) == 119 &&
            policy.fieldWritesSeen >= 1u,
        "FlatLoop policy passthrough result or checkFieldWrite tracking mismatch");

    // 2. Intercept both inlined leaf (InlineCallEnter) and polymorphic call (CallPolledFast)
    policy.interceptCallActive = true;
    const auto intRes = interp.invoke(
        "Kernel.FlatPolicyAndFastOps::RunInlinedAndPoly", {runtime::Value{std::int64_t{3}}});
    // Intercepted LeafMul(0..2) = 50+51+52 = 153; SubBox.Score() = 999 -> 1152
    require(intRes.succeeded && std::get<std::int64_t>(intRes.value) == 1152 &&
            policy.callsIntercepted == 4u,
        "FlatLoop InlineCallEnter / CallPolledFast interceptCall mismatch");

    // 3. Reject call in InlineCallEnter
    policy.interceptCallActive = false;
    policy.rejectCall = true;
    const auto rejectCallRes = interp.invoke(
        "Kernel.FlatPolicyAndFastOps::RunInlinedAndPoly", {runtime::Value{std::int64_t{3}}});
    require(!rejectCallRes.succeeded &&
            rejectCallRes.error.code == runtime::ErrorCode::CapabilityDenied,
        "FlatLoop InlineCallEnter reject should return CapabilityDenied");
    policy.rejectCall = false;

    // 3b. Delegate invocation forwarding directValues ArgumentSource under active executionPolicy
    policy.callsIntercepted = 0;
    policy.interceptCallActive = false;
    const auto delPassRes = interp.invoke(
        "Kernel.FlatPolicyAndFastOps::RunDelegatesWithPolicy", {runtime::Value{std::int64_t{4}}});
    require(delPassRes.succeeded && std::get<std::int64_t>(delPassRes.value) == 37,
        "FlatLoop RunDelegatesWithPolicy passthrough mismatch");
    policy.interceptCallActive = true;
    const auto delIntRes = interp.invoke(
        "Kernel.FlatPolicyAndFastOps::RunDelegatesWithPolicy", {runtime::Value{std::int64_t{4}}});
    require(delIntRes.succeeded && std::get<std::int64_t>(delIntRes.value) == 78 &&
            policy.callsIntercepted == 1u,
        "FlatLoop RunDelegatesWithPolicy interceptCall mismatch");
    policy.callsIntercepted = 0;
    policy.interceptCallActive = false;

    // 4. Field and array write enforcement under FlatLoop
    const auto boxTypeId = semantic::stableTypeId("Kernel.FlatPolicyAndFastOps::Box");
    const auto intArrTypeId = semantic::stableTypeId(
        semantic::arrayTypeName(semantic::PrimitiveType::Int, ""));
    auto boxOpt = heap->allocateObject(boxTypeId, {runtime::Value{std::int64_t{5}}, runtime::Value{runtime::NullString{}}}, {1u});
    auto arrOpt = heap->allocateTypedArray(intArrTypeId, semantic::PrimitiveType::Int, 0, 2, runtime::Value{std::int64_t{0}});
    require(boxOpt.has_value() && arrOpt.has_value(), "Allocation failed for policy test");
    auto pinBox = heap->retain(runtime::Value{*boxOpt});
    auto pinArr = heap->retain(runtime::Value{*arrOpt});

    policy.fieldWritesSeen = 0;
    policy.rejectFieldWrite = false;
    const auto mutFieldOk = interp.invoke(
        "Kernel.FlatPolicyAndFastOps::MutateField",
        {runtime::Value{*boxOpt}, runtime::Value{std::int64_t{42}}, runtime::Value{runtime::StringValue(std::string("ok"))}});
    require(mutFieldOk.succeeded && std::get<std::int64_t>(mutFieldOk.value) == 42 &&
            policy.fieldWritesSeen == 2u,
        "FlatLoop MutateField allow path mismatch");

    policy.rejectFieldWrite = true;
    const auto mutFieldDeny = interp.invoke(
        "Kernel.FlatPolicyAndFastOps::MutateField",
        {runtime::Value{*boxOpt}, runtime::Value{std::int64_t{99}}, runtime::Value{runtime::StringValue(std::string("no"))}});
    require(!mutFieldDeny.succeeded &&
            mutFieldDeny.error.code == runtime::ErrorCode::CapabilityDenied &&
            heap->fieldGet(*boxOpt, 0u) == std::optional<runtime::Value>(runtime::Value{std::int64_t{42}}),
        "FlatLoop checkFieldWrite rejection must block write");
    policy.rejectFieldWrite = false;

    policy.arrayWritesSeen = 0;
    policy.rejectArrayWrite = false;
    const auto mutArrOk = interp.invoke(
        "Kernel.FlatPolicyAndFastOps::MutateArray",
        {runtime::Value{*arrOpt}, runtime::Value{std::int64_t{1}}, runtime::Value{std::int64_t{77}}});
    require(mutArrOk.succeeded && std::get<std::int64_t>(mutArrOk.value) == 77 &&
            policy.arrayWritesSeen == 1u,
        "FlatLoop MutateArray allow path mismatch");

    policy.rejectArrayWrite = true;
    const auto mutArrDeny = interp.invoke(
        "Kernel.FlatPolicyAndFastOps::MutateArray",
        {runtime::Value{*arrOpt}, runtime::Value{std::int64_t{1}}, runtime::Value{std::int64_t{88}}});
    require(!mutArrDeny.succeeded &&
            mutArrDeny.error.code == runtime::ErrorCode::CapabilityDenied &&
            heap->arrayGet(*arrOpt, 1u) == std::optional<runtime::Value>(runtime::Value{std::int64_t{77}}),
        "FlatLoop checkArrayWrite rejection must block array write");
    policy.rejectArrayWrite = false;
    interp.setExecutionPolicy(nullptr);

    // 5. Parity of all new fast opcodes vs BlockVM (trace enabled forces BlockVM) + GC epoch invalidation
    auto rawStr = heap->allocateString("  AbCd  ");
    require(rawStr.has_value(), "allocateString failed");
    const std::int64_t expectedFast = 10 + 20 + 40 + 80 + 100 + 200 + 400 + 800 + 4000 + 12 + 10 - (-7) + 22;
    const auto flatRes = interp.invoke(
        "Kernel.FlatPolicyAndFastOps::ExerciseFastOpcodes",
        {runtime::Value{*rawStr}, runtime::Value{std::int64_t{-7}}});
    require(flatRes.succeeded && std::get<std::int64_t>(flatRes.value) == expectedFast,
        "FlatLoop fast opcodes result mismatch");

    // Collect full heap so cached literal ObjectRefs in TLS stringLiteralCache are swept, then re-invoke!
    heap->collect(runtime::RootSet{});
    auto rawStr2 = heap->allocateString("  AbCd  ");
    require(rawStr2.has_value(), "allocateString2 failed");
    const auto afterGcRes = interp.invoke(
        "Kernel.FlatPolicyAndFastOps::ExerciseFastOpcodes",
        {runtime::Value{*rawStr2}, runtime::Value{std::int64_t{-7}}});
    require(afterGcRes.succeeded && std::get<std::int64_t>(afterGcRes.value) == expectedFast,
        "FlatLoop ConstantStringFast after GC epoch bump must re-intern cleanly");

    runtime::ExecutionOptions blockOpts;
    blockOpts.trace = [](const runtime::TraceEvent&) {};
    const auto blockRes = interp.invoke(
        "Kernel.FlatPolicyAndFastOps::ExerciseFastOpcodes",
        {runtime::Value{*rawStr2}, runtime::Value{std::int64_t{-7}}},
        blockOpts);
    require(blockRes.succeeded && blockRes.value == flatRes.value,
        "FlatLoop vs BlockVM parity mismatch on new fast opcodes");

    // 6. CallPolledFast trivial virtual setter under active IExecutionPolicy
    {
        const auto subBoxTypeId = semantic::stableTypeId("Kernel.FlatPolicyAndFastOps::SubBox");
        auto subOpt = heap->allocateObject(
            subBoxTypeId, {runtime::Value{std::int64_t{5}}, runtime::Value{runtime::NullString{}}}, {1u});
        require(subOpt.has_value(), "allocateObject SubBox failed");
        auto pinSub = heap->retain(runtime::Value{*subOpt});

        policy.fieldWritesSeen = 0;
        policy.rejectFieldWrite = false;
        policy.interceptCallActive = false;
        policy.callsIntercepted = 0;
        policy.lastDeclTypeId = 0;
        policy.lastFieldName.clear();
        interp.setExecutionPolicy(&policy);

        const auto polySetAllow = interp.invoke(
            "Kernel.FlatPolicyAndFastOps::RunPolySetVal",
            {runtime::Value{*subOpt}, runtime::Value{std::int64_t{31}}});
        require(polySetAllow.succeeded && std::get<std::int64_t>(polySetAllow.value) == 131 &&
                policy.fieldWritesSeen == 1u &&
                policy.lastDeclTypeId != 0 &&
                policy.lastFieldName == "value",
            "CallPolledFast StoreThisFieldExact under IExecutionPolicy allow mismatch");

        policy.rejectFieldWrite = true;
        const auto polySetDeny = interp.invoke(
            "Kernel.FlatPolicyAndFastOps::RunPolySetVal",
            {runtime::Value{*subOpt}, runtime::Value{std::int64_t{99}}});
        require(!polySetDeny.succeeded &&
                polySetDeny.error.code == runtime::ErrorCode::CapabilityDenied &&
                heap->fieldGet(*subOpt, 0u) == std::optional<runtime::Value>(runtime::Value{std::int64_t{31}}),
            "CallPolledFast StoreThisFieldExact checkFieldWrite rejection must block write");
        policy.rejectFieldWrite = false;

        policy.interceptCallActive = true;
        const auto polySetIntercept = interp.invoke(
            "Kernel.FlatPolicyAndFastOps::RunPolySetVal",
            {runtime::Value{*subOpt}, runtime::Value{std::int64_t{55}}});
        require(polySetIntercept.succeeded &&
                policy.callsIntercepted == 2u &&
                std::get<std::int64_t>(polySetIntercept.value) == 999 &&
                heap->fieldGet(*subOpt, 0u) == std::optional<runtime::Value>(runtime::Value{std::int64_t{31}}),
            "CallPolledFast StoreThisFieldExact interceptCall mismatch");
        policy.interceptCallActive = false;
        interp.setExecutionPolicy(nullptr);
    }

    // 7. Wave 2: Selective per-layout dirty tracking + setObjectObserved
    {
        const auto plainTypeId = semantic::stableTypeId("Kernel.FlatPolicyAndFastOps::PlainBox");
        heap->enableTypeDirtyTracking(boxTypeId, {3u, 4u});
        auto trackedBoxOpt = heap->allocateObject(
            boxTypeId, {runtime::Value{std::int64_t{1}}, runtime::Value{runtime::NullString{}}}, {1u});
        auto plainBoxOpt = heap->allocateObject(
            plainTypeId, {runtime::Value{std::int64_t{1}}}, {});
        require(trackedBoxOpt.has_value() && plainBoxOpt.has_value(),
            "Allocation failed for selective dirty tracking test");
        auto pinTracked = heap->retain(runtime::Value{*trackedBoxOpt});
        auto pinPlain = heap->retain(runtime::Value{*plainBoxOpt});
        (void)heap->consumeDirtyMask(*trackedBoxOpt);
        (void)heap->consumeDirtyMask(*plainBoxOpt);

        L0CallbackLog cbLog{};
        heap->setFieldMutationCallback(&onL0FieldMutated, &cbLog);

        const auto m1 = interp.invoke(
            "Kernel.FlatPolicyAndFastOps::MutateBothBoxes",
            {runtime::Value{*trackedBoxOpt}, runtime::Value{*plainBoxOpt}, runtime::Value{std::int64_t{10}}});
        require(m1.succeeded && std::get<std::int64_t>(m1.value) == 31,
            "MutateBothBoxes result mismatch");
        require(heap->consumeDirtyMask(*trackedBoxOpt) == (1ULL << 3u) &&
                heap->consumeDirtyMask(*plainBoxOpt) == 0ULL &&
                cbLog.fieldCallbacks == 1u &&
                cbLog.lastFieldObj == *trackedBoxOpt &&
                cbLog.lastDirtyBit == 3u,
            "Selective per-layout dirty tracking must only notify tracked or observed objects");

        require(heap->setObjectObserved(*plainBoxOpt, true), "setObjectObserved(plain, true) failed");
        const auto m2 = interp.invoke(
            "Kernel.FlatPolicyAndFastOps::MutateBothBoxes",
            {runtime::Value{*trackedBoxOpt}, runtime::Value{*plainBoxOpt}, runtime::Value{std::int64_t{20}}});
        require(m2.succeeded && std::get<std::int64_t>(m2.value) == 51 &&
                cbLog.fieldCallbacks == 3u &&
                cbLog.lastFieldObj == *plainBoxOpt &&
                cbLog.lastDirtyBit == 0xFFu,
            "Observed plain object after setObjectObserved(true) must fire fieldMutationCallback");
        heap->setFieldMutationCallback(nullptr, nullptr);
    }

    // 8. Wave 2: Enum leaf return unfolding & ASCII + multi-byte UTF-8 Substring(i, 1)
    {
        const auto enumFlat1 = interp.invoke(
            "Kernel.FlatPolicyAndFastOps::UsePickedKind", {runtime::Value{std::int64_t{1}}});
        const auto enumFlat0 = interp.invoke(
            "Kernel.FlatPolicyAndFastOps::UsePickedKind", {runtime::Value{std::int64_t{0}}});
        const auto enumBlk1 = interp.invoke(
            "Kernel.FlatPolicyAndFastOps::UsePickedKind", {runtime::Value{std::int64_t{1}}}, blockOpts);
        require(enumFlat1.succeeded && std::get<std::int64_t>(enumFlat1.value) == 12 &&
                enumFlat0.succeeded && std::get<std::int64_t>(enumFlat0.value) == 5 &&
                enumBlk1.succeeded && enumBlk1.value == enumFlat1.value,
            "Enum leaf return unfolding mismatch");

        auto asciiStr = heap->allocateString("Az");
        auto utf8Str = heap->allocateString("A\xE4\xB8\xAD" "B");
        require(asciiStr.has_value() && utf8Str.has_value(), "allocateString failed for SliceCharAt");
        const auto a0 = interp.invoke(
            "Kernel.FlatPolicyAndFastOps::SliceCharAt",
            {runtime::Value{*asciiStr}, runtime::Value{std::int64_t{0}}});
        const auto a1 = interp.invoke(
            "Kernel.FlatPolicyAndFastOps::SliceCharAt",
            {runtime::Value{*asciiStr}, runtime::Value{std::int64_t{1}}});
        const auto u0 = interp.invoke(
            "Kernel.FlatPolicyAndFastOps::SliceCharAt",
            {runtime::Value{*utf8Str}, runtime::Value{std::int64_t{0}}});
        const auto u1 = interp.invoke(
            "Kernel.FlatPolicyAndFastOps::SliceCharAt",
            {runtime::Value{*utf8Str}, runtime::Value{std::int64_t{1}}});
        const auto u2 = interp.invoke(
            "Kernel.FlatPolicyAndFastOps::SliceCharAt",
            {runtime::Value{*utf8Str}, runtime::Value{std::int64_t{2}}});
        const auto svOf = [&](const runtime::ExecutionResult& r) {
            require(r.succeeded && std::holds_alternative<runtime::ObjectRef>(r.value),
                "Expected managed string slice result");
            return heap->stringView(std::get<runtime::ObjectRef>(r.value)).value_or(std::string_view{});
        };
        require(svOf(a0) == "A" && svOf(a1) == "z" &&
                svOf(u0) == "A" && svOf(u1) == "\xE4\xB8\xAD" && svOf(u2) == "B",
            "Substring(i, 1) ASCII or multi-byte UTF-8 slice mismatch");
    }
}

void testWaveRegressionEnumMirrorSlabsAndStrings() {
    const std::string src = R"RS(
module Kernel.WaveReg;

public enum Flags32 {
    Low = 3,
    High = 1073741824
}

public class SixSlot {
    public int a = 1;
    public int b = 2;
    public int c = 3;
    public int d = 4;
    public int e = 5;
    public int f = 6;
}

public Flags32 PickFlag(int high) {
    return high != 0 ? Flags32.High : Flags32.Low;
}

public int SumSix(SixSlot s) {
    return s.a + s.b + s.c + s.d + s.e + s.f;
}
)RS";

    auto prog = compileAndLink({{"wave_reg.rscript", src}}, optimization::Level::Aggressive);
    auto heap = std::make_shared<runtime::ManagedHeap>();

    // 1. ConstantEnumFast 32-bit sign/mask parity (FlatLoop vs BlockVM slow fallback).
    {
        runtime::Interpreter interp(prog);
        interp.setHeap(heap);

        const auto enumInt = [](const runtime::ExecutionResult& r) -> std::int64_t {
            require(r.succeeded && std::holds_alternative<runtime::EnumValue>(r.value),
                "Expected EnumValue result from PickFlag");
            return std::get<runtime::EnumValue>(r.value).value;
        };

        const auto flatHi = interp.invoke("Kernel.WaveReg::PickFlag", {runtime::Value{std::int64_t{1}}});
        const auto flatLo = interp.invoke("Kernel.WaveReg::PickFlag", {runtime::Value{std::int64_t{0}}});
        require(enumInt(flatHi) == 1073741824 && enumInt(flatLo) == 3,
            "ConstantEnumFast FlatLoop result mismatch");

        runtime::ExecutionOptions blockOpts;
        blockOpts.trace = [](const runtime::TraceEvent&) {};
        const auto blkHi = interp.invoke(
            "Kernel.WaveReg::PickFlag", {runtime::Value{std::int64_t{1}}}, blockOpts);
        const auto blkLo = interp.invoke(
            "Kernel.WaveReg::PickFlag", {runtime::Value{std::int64_t{0}}}, blockOpts);
        require(enumInt(blkHi) == enumInt(flatHi) && enumInt(blkLo) == enumInt(flatLo),
            "ConstantEnumFast BlockVM ensureSlowInstructions parity mismatch");
    }

    // 2. Concurrent ProgramImage::executionMirrorFast_ initialization & reads across threads.
    {
        auto freshProg = compileAndLink({{"wave_reg_conc.rscript", src}}, optimization::Level::Basic);
        constexpr std::size_t kThreads = 4u;
        std::vector<std::uint8_t> okFlags(kThreads, 0u);
        std::vector<std::thread> workers;
        workers.reserve(kThreads);
        for (std::size_t t = 0u; t < kThreads; ++t) {
            workers.emplace_back([&, t]() {
                runtime::Interpreter threadInterp(freshProg);
                bool allOk = true;
                for (int iter = 0; iter < 50; ++iter) {
                    const auto res = threadInterp.invoke(
                        "Kernel.WaveReg::PickFlag",
                        {runtime::Value{std::int64_t{(iter & 1) ? 1 : 0}}});
                    const std::int64_t expected = (iter & 1) ? 1073741824 : 3;
                    if (!res.succeeded ||
                        !std::holds_alternative<runtime::EnumValue>(res.value) ||
                        std::get<runtime::EnumValue>(res.value).value != expected) {
                        allOk = false;
                        break;
                    }
                }
                okFlags[t] = allOk ? 1u : 0u;
            });
        }
        for (auto& w : workers) w.join();
        for (std::size_t t = 0u; t < kThreads; ++t) {
            require(okFlags[t] == 1u, "Concurrent ProgramImage::executionMirror() call failed");
        }
    }

    // 3. >4-field object & array slab class (<= 8 values) + FNV-1a zero-alloc internedStrings & sweep.
    {
        const auto sixTid = semantic::stableTypeId("Kernel.WaveReg::SixSlot");
        auto sixObj = heap->allocateObject(
            sixTid,
            {runtime::Value{std::int64_t{10}},
             runtime::Value{std::int64_t{20}},
             runtime::Value{std::int64_t{30}},
             runtime::Value{std::int64_t{40}},
             runtime::Value{std::int64_t{50}},
             runtime::Value{std::int64_t{60}}},
            {});
        require(sixObj.has_value(), "allocateObject SixSlot (6 fields) failed");
        auto pinSix = heap->retain(runtime::Value{*sixObj});

        const auto arrTid = semantic::stableTypeId(
            semantic::arrayTypeName(semantic::PrimitiveType::Int, ""));
        auto arr6 = heap->allocateTypedArray(arrTid, semantic::PrimitiveType::Int, 0, 6u, runtime::Value{std::int64_t{7}});
        require(arr6.has_value(), "allocateTypedArray (6 elems) failed");
        auto pinArr = heap->retain(runtime::Value{*arr6});

        auto keep1 = heap->internString("wave_intern_keep_string_long");
        auto keep2 = heap->internString("wave_intern_keep_string_long");
        auto drop1 = heap->internString("wave_intern_drop_string_long");
        require(keep1.has_value() && keep2.has_value() && drop1.has_value(),
            "internString failed");
        require(*keep1 == *keep2, "internedStrings lookup did not deduplicate identical strings");
        auto pinKeep = heap->retain(runtime::Value{*keep1});
        const std::size_t beforeSweepCount = heap->internedStringCount();

        runtime::Interpreter interp(prog);
        interp.setHeap(heap);
        const auto sumRes = interp.invoke("Kernel.WaveReg::SumSix", {runtime::Value{*sixObj}});
        require(sumRes.succeeded && std::get<std::int64_t>(sumRes.value) == 210,
            "SumSix failed on 6-field slab object");

        heap->collect(runtime::RootSet{});
        require(heap->isAlive(*sixObj) && heap->isAlive(*keep1) && !heap->isAlive(*drop1),
            "GC did not preserve pinned slab/interned string or reclaim unpinned interned string");
        require(heap->internedStringCount() < beforeSweepCount,
            "internedStringCount did not drop after sweeping unpinned interned string");
        auto keep3 = heap->internString("wave_intern_keep_string_long");
        require(keep3.has_value() && *keep3 == *keep1,
            "Pinned interned string lost identity across GC");
    }
}

void testAuditRegressionGcCachesAndQuatf() {
    const std::string src = R"RS(
module Kernel.AuditRt;

public class Box {
    public int v;
    public Box(int x) { v = x; }
}

public interface ITag {
    int Id();
}

public class Alpha : ITag {
    public int Id() { return 11; }
}

public class Beta : ITag {
    public int Id() { return 22; }
}

public string GetAuditLit() {
    return "incremental_gc_literal_alive_check";
}

public string GetAuditLitSweep() {
    return "incremental_gc_sweep_literal_alive";
}

public int[] MakeDynArr() {
    int[] a = new int[2];
    a[0] = 10;
    a[1] = 20;
    return a;
}

public int MutateDynamicArr(int[] a) {
    a.Push(99);
    return a.Length;
}

public int CallTag(ITag t) {
    return t.Id();
}
)RS";

    auto prog = compileAndLink({{"audit_rt.rscript", src}}, optimization::Level::Basic);

    // A. ConstantStringFast while heap->phase() != GcPhase::Idle (Mark and Sweep phases).
    {
        auto heap = std::make_shared<runtime::ManagedHeap>();
        const auto boxTid = semantic::stableTypeId("Kernel.AuditRt::Box");
        std::vector<runtime::PersistentRoot> keepBoxes;
        for (int i = 0; i < 128; ++i) {
            auto obj = heap->allocateObject(boxTid, {runtime::Value{std::int64_t{i}}}, {});
            require(obj.has_value(), "Audit seed allocateObject failed");
            if ((i % 4) == 0) {
                keepBoxes.push_back(heap->retain(runtime::Value{*obj}));
            }
        }
        heap->requestCollection();
        runtime::ShadowStack emptyRoots;
        (void)heap->step(emptyRoots, 1);
        require(heap->phase() != runtime::GcPhase::Idle,
            "Expected incremental GC to remain non-Idle after 1 work step");

        runtime::Interpreter interp(prog, heap);
        runtime::ExecutionOptions noStepOpts;
        noStepOpts.limits.gcWorkBudget = 0;
        const auto markRes = interp.invoke("Kernel.AuditRt::GetAuditLit", {}, noStepOpts);
        require(markRes.succeeded && std::holds_alternative<runtime::ObjectRef>(markRes.value),
            "ConstantStringFast during incremental GC failed: " + markRes.error.message);
        const auto strRef = std::get<runtime::ObjectRef>(markRes.value);
        auto pinStr = heap->retain(markRes.value);
        while (heap->phase() != runtime::GcPhase::Idle) {
            (void)heap->step(emptyRoots, 16);
        }
        require(heap->isAlive(strRef) &&
                heap->stringView(strRef) == std::optional<std::string_view>{"incremental_gc_literal_alive_check"},
            "String literal allocated during incremental GC did not survive cycle finish");
        pinStr.reset();

        heap->requestCollection();
        for (int s = 0; s < 512 && heap->phase() != runtime::GcPhase::Sweep && heap->phase() != runtime::GcPhase::Idle; ++s) {
            (void)heap->step(emptyRoots, 1);
        }
        if (heap->phase() == runtime::GcPhase::Sweep) {
            const auto sweepRes = interp.invoke("Kernel.AuditRt::GetAuditLitSweep", {}, noStepOpts);
            require(sweepRes.succeeded && std::holds_alternative<runtime::ObjectRef>(sweepRes.value),
                "ConstantStringFast during Sweep failed: " + sweepRes.error.message);
            const auto sweepRef = std::get<runtime::ObjectRef>(sweepRes.value);
            auto pinSweep = heap->retain(sweepRes.value);
            while (heap->phase() != runtime::GcPhase::Idle) {
                (void)heap->step(emptyRoots, 16);
            }
            require(heap->isAlive(sweepRef) &&
                    heap->stringView(sweepRef) == std::optional<std::string_view>{"incremental_gc_sweep_literal_alive"},
                "String literal allocated during GcPhase::Sweep was collected or corrupted");
        }
    }

    // B. CallIntrinsicFast a.Push(...) under IExecutionPolicy::checkArrayWrite denial.
    {
        struct AuditPolicy final : public runtime::IExecutionPolicy {
            bool rejectArrayWrite = false;
            std::size_t seen = 0;
            bool checkArrayWrite(
                const runtime::ManagedHeap*,
                const runtime::ManagedHeap*,
                runtime::ObjectRef,
                const runtime::Value&,
                runtime::RuntimeError& outError) const override {
                const_cast<AuditPolicy*>(this)->seen += 1u;
                if (rejectArrayWrite) {
                    outError.code = runtime::ErrorCode::CapabilityDenied;
                    outError.message = "audit denied array write";
                    return false;
                }
                return true;
            }
        };
        auto heap = std::make_shared<runtime::ManagedHeap>();
        runtime::Interpreter interp(prog, heap);
        const auto arrRes = interp.invoke("Kernel.AuditRt::MakeDynArr");
        require(arrRes.succeeded && std::holds_alternative<runtime::ObjectRef>(arrRes.value),
            "MakeDynArr failed");
        const auto arrRef = std::get<runtime::ObjectRef>(arrRes.value);
        auto pinArr = heap->retain(arrRes.value);

        AuditPolicy policy;
        policy.rejectArrayWrite = true;
        interp.setExecutionPolicy(&policy);
        const auto pushRes = interp.invoke("Kernel.AuditRt::MutateDynamicArr", {runtime::Value{arrRef}});
        require(!pushRes.succeeded &&
                pushRes.error.code == runtime::ErrorCode::CapabilityDenied &&
                policy.seen >= 1u &&
                heap->arrayLength(arrRef) == std::optional<std::size_t>{2u},
            "CallIntrinsicFast array.Push must respect IExecutionPolicy::checkArrayWrite");
        interp.setExecutionPolicy(nullptr);
    }

    // C. ManagedHeap::snapshot() + restore() invalidates Interpreter PIC via restoreEpoch().
    {
        auto hPic = std::make_shared<runtime::ManagedHeap>();
        const auto snap = hPic->snapshot();
        const std::uint64_t initialEpoch = hPic->restoreEpoch();
        runtime::Interpreter interp(prog, hPic);

        const auto alphaTid = semantic::stableTypeId("Kernel.AuditRt::Alpha");
        const auto betaTid = semantic::stableTypeId("Kernel.AuditRt::Beta");
        auto alphaObj = hPic->allocateObject(alphaTid, {}, {});
        require(alphaObj.has_value(), "allocate Alpha failed");

        const auto r1 = interp.invoke("Kernel.AuditRt::CallTag", {runtime::Value{*alphaObj}});
        const auto r2 = interp.invoke("Kernel.AuditRt::CallTag", {runtime::Value{*alphaObj}});
        require(r1.succeeded && std::get<std::int64_t>(r1.value) == 11 &&
                r2.succeeded && std::get<std::int64_t>(r2.value) == 11,
            "Warming Alpha PIC failed");

        runtime::RuntimeError restErr;
        require(hPic->restore(snap, &restErr), "ManagedHeap::restore failed: " + restErr.message);
        require(hPic->restoreEpoch() > initialEpoch, "ManagedHeap::restore did not bump restoreEpoch");

        auto betaObj = hPic->allocateObject(betaTid, {}, {});
        require(betaObj.has_value(), "allocate Beta after restore failed");
        const auto rPost = interp.invoke("Kernel.AuditRt::CallTag", {runtime::Value{*betaObj}});
        require(rPost.succeeded && std::get<std::int64_t>(rPost.value) == 22,
            "Post-restore Interpreter PIC used stale pre-restore dispatch entry");
    }

    // D. internArenaQuatf > 2,048 distinct non-inline QuatfArena entries without TLS cache corruption.
    {
        std::vector<runtime::Value> quats;
        quats.reserve(2400);
        for (int i = 0; i < 2400; ++i) {
            const float x = static_cast<float>(i);
            std::uint32_t wBits = 0x3F000001u + (static_cast<std::uint32_t>(i) * 3u);
            if ((wBits & 0xFFu) == 0u) {
                wBits |= 1u;
            }
            float w = 0.0f;
            std::memcpy(&w, &wBits, sizeof(w));
            runtime::Value q{runtime::QuatfValue{x, -1.5f, 2.25f, w}};
            require(q.rawTag() == runtime::ValueTag::QuatfArena,
                "Expected non-inline QuatfArena tag");
            quats.push_back(q);
        }
        for (int i = 0; i < 2400; ++i) {
            const auto* got = std::get_if<runtime::QuatfValue>(&quats[static_cast<std::size_t>(i)]);
            require(got != nullptr && got->x == static_cast<float>(i) && got->y == -1.5f && got->z == 2.25f,
                "QuatfArena value corrupted after >2048 distinct allocations");
        }
    }
}

void testStaticAndConstFieldsExcludedFromInstanceLayout() {
    const std::string src = R"RS(
module Kernel.InstanceLayout;

public delegate void PingHandler(int code);

public class DepBase {
    private static string _s0 = "a";
    private static string _s1 = "b";
    private static DepBase _s2 = null;
    private static DepBase _s3 = null;
    private static int[] _s4 = null;
    public static int StaticCounter { get; set; } = 7;

    public static string ReadStaticTag() {
        return _s0 + _s1 + ":" + StaticCounter;
    }
}

public class BoxedFloatLike : DepBase {
    public float Value;
    public static BoxedFloatLike Zero = null;
    public BoxedFloatLike(float v) { Value = v; }
}

public class WidgetBase : DepBase {
    public int BaseId = 10;
    public static int BaseStatic = 3;
}

public class DerivedWidget : WidgetBase {
    public static string DerivedTag = "dw";
    public int OwnProp { get; set; }
    public event PingHandler Pinged;
    public const int ConstMagic = 42;
    public int OwnField = 5;

    public void EmitPing(int x) {
        Pinged(x);
    }
}

public struct CompactStruct {
    public const int K = 9;
    public static int S = 11;
    public int X;
    public int Y;
}

public static int RunChecks() {
    var bf = new BoxedFloatLike(2.5f);
    BoxedFloatLike.Zero = bf;
    DepBase.StaticCounter = 12;

    var w = new DerivedWidget();
    w.OwnProp = 100;
    int captured = 0;
    w.Pinged += (int c) => { captured = captured + c; };
    w.EmitPing(8);

    CompactStruct cs = new CompactStruct();
    cs.X = 3;
    cs.Y = 4;
    if (DepBase.ReadStaticTag() != "ab:12") { return -1; }
    if (DerivedWidget.DerivedTag != "dw" || WidgetBase.BaseStatic != 3) { return -2; }
    if (w.BaseId != 10 || w.OwnField != 5 || w.OwnProp != 100 || captured != 8) { return -3; }
    if (cs.X + cs.Y + CompactStruct.K + CompactStruct.S != 27) { return -4; }
    return (int)(bf.Value * 2.0f);
}

public static BoxedFloatLike MakeBoxed() {
    return new BoxedFloatLike(3.5f);
}

public static DerivedWidget MakeWidget() {
    return new DerivedWidget();
}
)RS";

    auto img = compileAndLink({{"instance_layout.rscript", src}});

    // Verify lazy RSLB round-trip preserves instance-first layout
    const auto lazyBlob = bytecode::encodeLazyBytecodeBundle(img->modules());
    auto lazyBundle = std::make_shared<bytecode::LazyBytecodeBundle>();
    diagnostics::DiagnosticBag lazyDiag;
    require(bytecode::decodeLazyBytecodeBundleHeader(
                lazyBlob.data(), lazyBlob.size(), *lazyBundle, true, lazyDiag),
        "Lazy bundle decode failed");
    runtime::RuntimeError linkErr;
    auto lazyImgOpt = runtime::ProgramImage::linkLayered(lazyBundle, {}, linkErr, true);
    require(lazyImgOpt.has_value(), "linkLayered failed: " + linkErr.message);
    std::shared_ptr<const runtime::ProgramImage> lazyImg =
        std::make_shared<const runtime::ProgramImage>(std::move(*lazyImgOpt));

    const std::array<std::shared_ptr<const runtime::ProgramImage>, 2> imgs{img, lazyImg};
    for (const auto& checkImg : imgs) {
        auto heap = std::make_shared<runtime::ManagedHeap>();
        runtime::Interpreter interp(checkImg, heap);
        auto res = interp.invoke("Kernel.InstanceLayout::RunChecks");
        std::int64_t out = 0;
        require(res.succeeded && res.value.tryGetInt64(out) && out == 5,
            "InstanceLayout RunChecks failed: " + res.error.message);

        auto bfRes = interp.invoke("Kernel.InstanceLayout::MakeBoxed");
        require(bfRes.succeeded && bfRes.value.rawTag() == runtime::ValueTag::ObjectRef,
            "MakeBoxed failed");
        const auto bfRef = std::get<runtime::ObjectRef>(bfRes.value);
        const std::size_t bfBytes = heap->objectSizeBytes(bfRef).value_or(0u);
        require(bfBytes == 48u,
            "Expected BoxedFloatLike size == 48 B (32B slot + 16B float), got " + std::to_string(bfBytes));

        auto wRes = interp.invoke("Kernel.InstanceLayout::MakeWidget");
        require(wRes.succeeded && wRes.value.rawTag() == runtime::ValueTag::ObjectRef,
            "MakeWidget failed");
        const auto wRef = std::get<runtime::ObjectRef>(wRes.value);
        // DerivedWidget has 4 instance fields: BaseId, OwnProp($OwnProp), OwnField, $event_Pinged_delegate
        const std::size_t wBytes = heap->objectSizeBytes(wRef).value_or(0u);
        require(wBytes == 32u + 4u * 16u,
            "Expected DerivedWidget size == 96 B (32B + 4*16B), got " + std::to_string(wBytes));

        const auto snap = heap->snapshot();
        bool foundBf = false;
        for (const auto& obj : snap.objects) {
            if (obj.reference == bfRef) {
                foundBf = true;
                require(obj.valueCount == 1u,
                    "Expected BoxedFloatLike valueCount == 1, got " + std::to_string(obj.valueCount));
                require(obj.referenceFields.empty(),
                    "Expected BoxedFloatLike referenceFields empty (static refs must be excluded)");
            }
        }
        require(foundBf, "BoxedFloatLike missing from heap snapshot");
    }
}

void testHybridRecordCompressionAccessDemotionGcAndDirty() {
    const std::string src = R"RS(
module Kernel.HybridRecordSuite;

public enum DockMode {
    None = 0,
    Left = 1,
    Fill = 2,
}

public class Tag {
    public int Value;
}

public class WidgetLike {
    public string Name;
    public object Data;
    public Tag Linked;
    public bool Visible;
    public byte Alpha;
    public sbyte Layer;
    public short OffsetX;
    public ushort Flags;
    public char Glyph;
    public int Count;
    public uint Mask;
    public long Stamp;
    public ulong Hash;
    public float Opacity;
    public double Width;
    public DockMode Dock;
    public int S0;
    public int S1;
    public int S2;
    public int S3;
    public float F0;
    public float F1;
    public bool B0;
    public bool B1;

    public int GetCount() { return Count; }
    public void SetCount(int c) { Count = c; }
}

public WidgetLike MakeWidget(string label) {
    WidgetLike w = new WidgetLike();
    w.Name = label;
    Tag t = new Tag();
    t.Value = 77;
    w.Data = t;
    w.Linked = t;
    w.Visible = true;
    w.Alpha = 250;
    w.Layer = (sbyte)(-12);
    w.OffsetX = (short)(-320);
    w.Flags = 51966;
    w.Glyph = 'A';
    w.Count = 42;
    w.Mask = 3735928559u;
    w.Stamp = 1234567890123l;
    w.Hash = 9876543210987ul;
    w.Opacity = 0.75f;
    w.Width = 320.5;
    w.Dock = DockMode.Fill;
    w.S0 = 10;
    w.S1 = 20;
    w.S2 = 30;
    w.S3 = 40;
    w.F0 = 1.5f;
    w.F1 = 2.5f;
    w.B0 = true;
    w.B1 = false;
    return w;
}

public int ExerciseTrivialAndFields(WidgetLike w) {
    w.SetCount(w.GetCount() + 8);
    int sum = w.Count + w.S0 + w.S1 + w.S2 + w.S3;
    if (w.Visible && w.B0 && !w.B1 && w.Dock == DockMode.Fill &&
        w.Alpha == 250 && w.Layer == (sbyte)(-12) && w.OffsetX == (short)(-320) &&
        w.Flags == 51966 && w.Glyph == 'A' && w.Mask == 3735928559u &&
        w.Stamp == 1234567890123l && w.Hash == 9876543210987ul &&
        w.Width > 320.0 && w.Opacity >= 0.5f && w.Name == "panel") {
        Tag t = w.Data as Tag;
        if (t != null) {
            return sum + t.Value;
        }
    }
    return -1;
}

public void RebindObjectField(WidgetLike w) {
    Tag fresh = new Tag();
    fresh.Value = 321;
    w.Data = fresh;
}

public int ReadObjectFieldTag(WidgetLike w) {
    Tag t = w.Data as Tag;
    if (t != null) {
        return t.Value;
    }
    return -1;
}
)RS";

    auto img = compileAndLink({{"hybrid_record.rscript", src}}, optimization::Level::Aggressive);
    auto heap = std::make_shared<runtime::ManagedHeap>();
    runtime::Interpreter interp(img, heap);

    // 1. Dirty callback setup
    struct DirtyLog {
        std::size_t count = 0;
        std::size_t lastField = 0;
        std::uint8_t lastBit = 0xFFu;
    } dirtyLog;

    const auto widgetTid = semantic::stableTypeId("Kernel.HybridRecordSuite::WidgetLike");
    const auto* widgetType = img->resolveType(widgetTid);
    require(widgetType != nullptr, "Missing WidgetLike type");

    std::size_t countIdx = 0, nameIdx = 0, opacityIdx = 0;
    for (const auto& f : widgetType->fields) {
        if (f.name == "Count") countIdx = f.index;
        if (f.name == "Name") nameIdx = f.index;
        if (f.name == "Opacity") opacityIdx = f.index;
    }
    std::vector<std::uint8_t> dirtyMap(widgetType->fields.size(), 0xFFu);
    dirtyMap[countIdx] = 3u;
    dirtyMap[opacityIdx] = 7u;
    heap->enableTypeDirtyTracking(widgetTid, std::move(dirtyMap));
    heap->setFieldMutationCallback(
        [](void* ud, runtime::ManagedHeap&, runtime::ObjectRef, std::size_t fIdx, std::uint8_t dBit) {
            auto* log = static_cast<DirtyLog*>(ud);
            ++log->count;
            log->lastField = fIdx;
            log->lastBit = dBit;
        },
        &dirtyLog);

    // 2. Allocate & verify HybridRecord compression
    const auto mk1 = interp.invoke("Kernel.HybridRecordSuite::MakeWidget", {runtime::Value{"panel"}});
    require(mk1.succeeded && mk1.value.rawTag() == runtime::ValueTag::ObjectRef, "MakeWidget #1 failed");
    const auto wRef = mk1.value.asObjectRefUnchecked();
    const auto rootW1 = heap->retain(wRef);

    const auto mk2 = interp.invoke("Kernel.HybridRecordSuite::MakeWidget", {runtime::Value{"other"}});
    require(mk2.succeeded && mk2.value.rawTag() == runtime::ValueTag::ObjectRef, "MakeWidget #2 failed");
    const auto wOther = mk2.value.asObjectRefUnchecked();
    const auto rootW2 = heap->retain(wOther);

    require(heap->isHybridRecordObject(wRef), "Expected WidgetLike to use SlotStorage::HybridRecord");
    require(!heap->isInlineRecordObject(wRef), "Expected WidgetLike not to report SlabValues");
    const std::size_t flatExpectedBytes = 32u + widgetType->fields.size() * sizeof(runtime::Value);
    const std::size_t hybridBytes = heap->objectSizeBytes(wRef).value_or(0u);
    require(hybridBytes > 32u && hybridBytes * 2u < flatExpectedBytes,
        "Expected HybridRecord objectSizeBytes < 50% of flat layout (got " +
            std::to_string(hybridBytes) + " vs flat " + std::to_string(flatExpectedBytes) + ")");

    // 3. fieldGetFast vs fieldGetInto
    require(heap->fieldGetFast(wRef, nameIdx) != nullptr,
        "Expected fieldGetFast to return non-null for ValueSlot string field");
    require(heap->fieldGetFast(wRef, countIdx) == nullptr,
        "Expected fieldGetFast to return nullptr for packed scalar Count field");

    runtime::Value intoVal{};
    semantic::SymbolId outTid = 0;
    require(heap->fieldGetInto(wRef, countIdx, intoVal, &outTid) && outTid == widgetTid,
        "fieldGetInto failed on packed scalar Count");
    std::int64_t iv = 0;
    require(intoVal.tryGetInt64(iv) && iv == 42, "Unexpected Count from fieldGetInto");
    require(heap->fieldGetInto(wRef, opacityIdx, intoVal) &&
            intoVal.rawTag() == runtime::ValueTag::Float &&
            intoVal.asFloatUnchecked() == 0.75f,
        "Unexpected Opacity from fieldGetInto");

    // 4. Dirty notification: unchanged scalar write does NOT fire; changed write DOES fire
    dirtyLog = {};
    require(heap->fieldSetFast(wRef, opacityIdx, runtime::Value{runtime::FloatValue{0.75f}}, widgetTid, semantic::PrimitiveType::Float),
        "fieldSetFast unchanged failed");
    require(dirtyLog.count == 0u, "Unchanged scalar write should not trigger dirty callback");
    require(heap->fieldSetFast(wRef, opacityIdx, runtime::Value{runtime::FloatValue{0.5f}}, widgetTid, semantic::PrimitiveType::Float),
        "fieldSetFast changed failed");
    require(dirtyLog.count == 1u && dirtyLog.lastField == opacityIdx && dirtyLog.lastBit == 7u,
        "Changed scalar write did not report expected dirty callback");

    // 5. VM execution + trivial methods + field checks
    dirtyLog = {};
    const auto exRes = interp.invoke("Kernel.HybridRecordSuite::ExerciseTrivialAndFields", {runtime::Value{wRef}});
    std::int64_t exInt = 0;
    require(exRes.succeeded && exRes.value.tryGetInt64(exInt) && exInt == (50 + 10 + 20 + 30 + 40 + 77),
        "ExerciseTrivialAndFields parity failed: " + exRes.error.message);
    require(dirtyLog.count == 1u && dirtyLog.lastField == countIdx && dirtyLog.lastBit == 3u,
        "VM trivial StoreThisFieldExact on HybridRecord missed dirty notification");

    // 6. GC promotion to old-gen + write barrier + collectYoung + collectFull + snapshot/restore
    heap->collectYoung(runtime::ShadowStack{});
    require(interp.invoke("Kernel.HybridRecordSuite::RebindObjectField", {runtime::Value{wRef}}).succeeded,
        "RebindObjectField failed");
    heap->collectYoung(runtime::ShadowStack{});
    heap->collectFull(runtime::ShadowStack{});
    const auto tagRes = interp.invoke("Kernel.HybridRecordSuite::ReadObjectFieldTag", {runtime::Value{wRef}});
    std::int64_t tagVal = 0;
    require(tagRes.succeeded && tagRes.value.tryGetInt64(tagVal) && tagVal == 321,
        "GC lost young object written into old-gen HybridRecord object field");

    // 7. Lossless demotion on >32-bit int write while leaving sibling instances Hybrid
    const std::int64_t wideVal = (std::int64_t{1} << 42) + 99;
    require(heap->fieldSet(wRef, countIdx, runtime::Value{wideVal}),
        "fieldSet >32-bit int on HybridRecord failed");
    require(!heap->isHybridRecordObject(wRef),
        "Expected single demoted instance to leave SlotStorage::HybridRecord");
    require(heap->isHybridRecordObject(wOther),
        "Expected sibling instance to remain SlotStorage::HybridRecord");
    require(heap->fieldGetInto(wRef, countIdx, intoVal) &&
            intoVal.tryGetInt64(iv) && iv == wideVal,
        "Wide int value lost after Hybrid->HeapValues demotion");
    require(heap->fieldGetInto(wRef, opacityIdx, intoVal) &&
            intoVal.rawTag() == runtime::ValueTag::Float &&
            intoVal.asFloatUnchecked() == 0.5f,
        "Neighboring packed scalar corrupted during Hybrid->HeapValues demotion");

    // 8. Snapshot + mutate + restore on the same heap
    const auto snap = heap->snapshot();
    require(heap->fieldSetFast(wOther, opacityIdx, runtime::Value{runtime::FloatValue{0.25f}}, widgetTid, semantic::PrimitiveType::Float),
        "Pre-restore mutation failed");
    runtime::RuntimeError restoreErr;
    require(heap->restore(snap, &restoreErr),
        "ManagedHeap::restore from HybridRecord snapshot failed: " + restoreErr.message);
    runtime::Value restoredOpacity{};
    require(heap->fieldGetInto(wOther, opacityIdx, restoredOpacity) &&
            restoredOpacity.rawTag() == runtime::ValueTag::Float &&
            restoredOpacity.asFloatUnchecked() == 0.75f,
        "Restored HybridRecord Opacity mismatch");
    (void)rootW1;
    (void)rootW2;
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
            {"L0 dirty callbacks, dynamic array ops, UTF-8/borrowed, weak delegates & IExecutionPolicy", testL0DirtyCallbacksArraysUtf8WeakDelegatesAndPolicy},
            {"Zero-lock ProfileCollector RingBuffer & steady_clock ChromeTrace flamegraph", testZeroLockProfileRingBufferAndChromeTrace},
            {"FlatLoop IExecutionPolicy enforcement & new fast opcodes", testFlatLoopExecutionPolicyAndFastOpcodes},
            {"Waves 1-4 regression: Enum32, concurrent ExecutionMirror, 8-slot slabs & FNV-1a internedStrings", testWaveRegressionEnumMirrorSlabsAndStrings},
            {"Audit regression: incremental ConstantStringFast, intrinsic policy, restoreEpoch PIC & >2k QuatfArena", testAuditRegressionGcCachesAndQuatf},
            {"Static and const fields excluded from instance object layout", testStaticAndConstFieldsExcludedFromInstanceLayout},
            {"Phase 2.2: HybridRecord compression, scalar fast paths, GC, dirty tracking & demotion", testHybridRecordCompressionAccessDemotionGcAndDirty},
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
