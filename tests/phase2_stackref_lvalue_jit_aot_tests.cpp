#include "realscript/aot_cpp/AotCpp.h"
#include "realscript/aot_cpp/AotRuntime.h"
#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/jit/Jit.h"
#include "realscript/optimization/Optimizer.h"
#include "realscript/runtime/Runtime.h"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace realscript;

namespace {

void requireCond(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error("phase2_stackref_lvalue_jit_aot_tests FAILED: " + message);
    }
}

std::string formatDiagnostics(const diagnostics::DiagnosticBag& bag) {
    std::string out;
    for (const auto& item : bag.items()) {
        out += "  " + item.code + ": " + item.message + "\n";
    }
    return out;
}

bool hasDiagnosticCode(const diagnostics::DiagnosticBag& bag, const std::string& code) {
    for (const auto& item : bag.items()) {
        if (item.code == code) {
            return true;
        }
    }
    return false;
}

struct BuiltArtifact {
    std::vector<mir::Module> mirModules;
    std::vector<bytecode::Module> bcModules;
    std::shared_ptr<const runtime::ProgramImage> program;
};

BuiltArtifact compileScript(
    const std::string& fileName,
    const std::string& source,
    optimization::Level optLevel = optimization::Level::Aggressive) {
    compiler::Compilation comp({{fileName, source}});
    auto build = comp.build();
    if (build.diagnostics.hasErrors()) {
        throw std::runtime_error(
            "Compilation failed for " + fileName + ":\n" + formatDiagnostics(build.diagnostics));
    }

    optimization::Options opts;
    opts.level = optLevel;
    auto optRes = optimization::Optimizer{}.optimize(
        std::move(build.modules), build.diagnostics, opts);
    requireCond(
        !build.diagnostics.hasErrors(),
        "Optimizer produced errors:\n" + formatDiagnostics(build.diagnostics));

    BuiltArtifact out;
    out.mirModules = optRes.modules;

    bytecode::Lowerer lowerer;
    out.bcModules.reserve(optRes.modules.size());
    for (const auto& m : optRes.modules) {
        out.bcModules.push_back(lowerer.lower(m));
    }

    std::vector<bytecode::Module> linkCopy = out.bcModules;
    runtime::RuntimeError linkErr;
    auto linked = runtime::ProgramImage::link(std::move(linkCopy), linkErr);
    requireCond(linked.has_value(), "ProgramImage::link failed: " + linkErr.message);
    out.program = std::make_shared<const runtime::ProgramImage>(std::move(*linked));
    return out;
}

// ---------------------------------------------------------------------------
// 1. 16B Value / ObjectRef ABI & StackSlotRef Encoding Invariants
// ---------------------------------------------------------------------------
void testStackSlotRefAbiInvariants() {
    std::cout << "[1/4] Verifying 16B Value/ObjectRef ABI and StackSlotRef encoding invariants...\n";
    static_assert(sizeof(runtime::Value) == 16, "Value must remain 16 bytes");
    static_assert(alignof(runtime::Value) == 16, "Value alignment must remain 16 bytes");
    static_assert(sizeof(runtime::ObjectRef) == 16, "ObjectRef must remain 16 bytes");

    requireCond(
        runtime::ObjectRef::StackSlotRefHeapId == 0xfffffffeu,
        "Unexpected StackSlotRefHeapId sentinel");

    const auto scalarRef = runtime::ObjectRef::makeStackSlotRef(19u, 3u, 0u);
    requireCond(scalarRef.isStackSlotRef(), "scalarRef must be recognized as StackSlotRef");
    requireCond(scalarRef.slot == 19u && scalarRef.generation == 3u, "scalarRef slot/bank mismatch");
    requireCond(scalarRef.stackRefSchemaId() == 0u, "scalarRef schemaId must be 0");

    const auto structRef = runtime::ObjectRef::makeStackSlotRef(256u, 7u, 513u);
    requireCond(structRef.isStackSlotRef(), "structRef must be recognized as StackSlotRef");
    requireCond(structRef.slot == 256u && structRef.generation == 7u, "structRef slot/bank mismatch");
    requireCond(structRef.stackRefSchemaId() == 513u, "structRef 16-bit schemaId round-trip failed");
}

// ---------------------------------------------------------------------------
// 2. Zero-Heap StackSlotRef Across Struct Methods, ref/out, Swap & External Bridge
// ---------------------------------------------------------------------------
void testZeroHeapStackSlotRef() {
    std::cout << "[2/4] Testing zero-heap StackSlotRef on struct methods, ref/out, Swap & external bridge...\n";
    constexpr const char* kSrc = R"RS(
module Phase2.StackRefSuite;

public delegate void IntMutator(ref int target, int delta);

public struct Transform18 {
    public int px; public int py; public int pz;
    public int rx; public int ry; public int rz;
    public int sx; public int sy; public int sz;
    public int m0; public int m1; public int m2;
    public int m3; public int m4; public int m5;
    public int m6; public int m7; public int m8;

    public void Translate(int dx, int dy, int dz) {
        this.px += dx;
        this.py += dy;
        this.pz += dz;
        this.m8 += dx + dy + dz;
    }
}

public struct BodyRig {
    public Transform18 localTf;
    public int armor;
    public int energy;

    public void Advance(int stepVal) {
        this.localTf.Translate(stepVal, 2, 3);
        this.energy += stepVal;
    }
}

public struct MegaUnit {
    public BodyRig body;
    public int hp;
    public int shield;
    public int ticks;

    public void TickUnit(int dmg) {
        this.body.Advance(1);
        this.hp -= dmg;
        this.ticks += 1;
    }
}

public void SwapInts(ref int a, ref int b) {
    int tmp = a;
    a = b;
    b = tmp;
}

public void WriteOutSummary(in MegaUnit u, out int totalPos, ref Transform18 tf, int bonus) {
    tf.px += bonus;
    tf.m7 += bonus * 2;
    totalPos = u.hp + tf.px + tf.py + tf.pz + tf.m7 + tf.m8 + u.body.energy + u.ticks;
}

public void AddDelta(ref int target, int delta) {
    target += delta;
}

public int RunTightStackRefLoop(int iterations) {
    MegaUnit unit = new MegaUnit();
    unit.hp = 1000;
    unit.body.localTf.px = 10;
    unit.body.localTf.py = 20;
    unit.body.localTf.pz = 30;

    int left = 4;
    int right = 9;
    int summary = 0;

    for (int i = 0; i < iterations; i = i + 1) {
        unit.TickUnit(2);
        SwapInts(ref left, ref right);
        WriteOutSummary(in unit, out summary, ref unit.body.localTf, 1);
    }
    return summary + left * 10 + right;
}

public int RunDelegateRefCheck(int start) {
    IntMutator mut = AddDelta;
    int v = start;
    mut(ref v, 15);
    mut(ref v, 27);
    return v;
}
)RS";

    for (const auto optLevel : {optimization::Level::None, optimization::Level::Aggressive}) {
        auto built = compileScript("phase2_stackref.rscript", kSrc, optLevel);
        auto heap = std::make_shared<runtime::ManagedHeap>();
        runtime::Interpreter vm(built.program, heap);

        const std::uint64_t objBefore = heap->statistics().objectsAllocated;
        runtime::resetHeapStructStorageAllocations();

        const auto res = vm.invoke("Phase2.StackRefSuite::RunTightStackRefLoop", {std::int64_t{200}});
        requireCond(res.succeeded, "RunTightStackRefLoop failed: " + res.error.message);

        const std::uint64_t objAfter = heap->statistics().objectsAllocated;
        const std::uint64_t structBoxes = runtime::heapStructStorageAllocations();

        requireCond(
            objAfter == objBefore,
            "Expected 0 ManagedHeap object allocations in RunTightStackRefLoop, got " +
                std::to_string(objAfter - objBefore));
        requireCond(
            structBoxes == 0u,
            "Expected 0 StructStorage heap allocations in RunTightStackRefLoop, got " +
                std::to_string(structBoxes));

        // Verify exact mathematical result for 200 iterations:
        // hp = 1000 - 400 = 600
        // px = 10 + 200 (Translate) + 200 (WriteOutSummary) = 410
        // py = 20 + 400 = 420
        // pz = 30 + 600 = 630
        // m7 = 400
        // m8 = 200 * (1 + 2 + 3) = 1200
        // energy = 200, ticks = 200
        // summary = 600 + 410 + 420 + 630 + 400 + 1200 + 200 + 200 = 4060
        // 200 swaps leaves left=4, right=9 => + 49 => 4109
        requireCond(
            std::get<std::int64_t>(res.value) == 4109LL,
            "Unexpected RunTightStackRefLoop value: " +
                std::to_string(std::get<std::int64_t>(res.value)));

        const auto delRes = vm.invoke("Phase2.StackRefSuite::RunDelegateRefCheck", {std::int64_t{8}});
        requireCond(
            delRes.succeeded && std::get<std::int64_t>(delRes.value) == 50LL,
            "Delegate ref writeback check failed");
    }
}

// ---------------------------------------------------------------------------
// 3. Lifted RS2482 Nested Lvalues (Single-Eval Side Effects + Zero-Alloc Array Writes)
//    & Preserved Negative Diagnostics (RS2482, RS2483, RS8702, RS2651, RS2486)
// ---------------------------------------------------------------------------
void testLiftedRs2482NestedWritesAndDiagnostics() {
    std::cout << "[3/4] Testing lifted RS2482 nested lvalues, side-effect single-eval & preserved diagnostics...\n";
    constexpr const char* kSrc = R"RS(
module Phase2.NestedLvalues;

public struct Vec6 {
    public int x; public int y; public int z;
    public int u; public int v; public int w;
}

public struct PackedParticle20 {
    public Vec6 pos;
    public Vec6 vel;
    public int c0; public int c1; public int c2; public int c3;
    public int c4; public int c5; public int c6; public int c7;
}

public struct FlatHero22 {
    public string name;
    public PackedParticle20 body;
    public int level;
}

public class HeroHolder {
    public FlatHero22 hero;
}

public class WorldConfig {
    public static PackedParticle20 globalOrigin;
    public static int holderEvalCount;
    public static int packedArrEvalCount;
    public static int packedIdxEvalCount;
    public static int flatArrEvalCount;
    public static int flatIdxEvalCount;
}

public HeroHolder EvalHolderOnce(HeroHolder h) {
    WorldConfig.holderEvalCount += 1;
    return h;
}

public PackedParticle20[] EvalPackedArrOnce(PackedParticle20[] a) {
    WorldConfig.packedArrEvalCount += 1;
    return a;
}

public int EvalPackedIdxOnce() {
    WorldConfig.packedIdxEvalCount += 1;
    return 2;
}

public FlatHero22[] EvalFlatArrOnce(FlatHero22[] a) {
    WorldConfig.flatArrEvalCount += 1;
    return a;
}

public int EvalFlatIdxOnce() {
    WorldConfig.flatIdxEvalCount += 1;
    return 1;
}

public void MutateViaRef(ref FlatHero22 h, int delta) {
    h.body.pos.z += delta;
    h.body.c7 += delta * 2;
}

public int VerifyNestedLvalueSemantics() {
    WorldConfig.holderEvalCount = 0;
    WorldConfig.packedArrEvalCount = 0;
    WorldConfig.packedIdxEvalCount = 0;
    WorldConfig.flatArrEvalCount = 0;
    WorldConfig.flatIdxEvalCount = 0;

    FlatHero22 localHero = new FlatHero22();
    localHero.name = "paladin";
    localHero.body.pos.x = 10;
    localHero.body.pos.x += 5;
    localHero.body.pos.z = 3;
    MutateViaRef(ref localHero, 4);

    HeroHolder holder = new HeroHolder();
    holder.hero = localHero;
    EvalHolderOnce(holder).hero.body.pos.x += 7;

    WorldConfig.globalOrigin.pos.y = 40;
    WorldConfig.globalOrigin.pos.y += 6;

    PackedParticle20[] packed = new PackedParticle20[4];
    packed[2].pos.x = 100;
    EvalPackedArrOnce(packed)[EvalPackedIdxOnce()].pos.x += 23;
    packed[2].c6 += 9;

    FlatHero22[] heroes = new FlatHero22[3];
    heroes[1].name = "ranger";
    heroes[1].body.pos.y = 30;
    EvalFlatArrOnce(heroes)[EvalFlatIdxOnce()].body.pos.y += 12;

    int sum = holder.hero.body.pos.x
            + holder.hero.body.pos.z
            + holder.hero.body.c7
            + WorldConfig.globalOrigin.pos.y
            + packed[2].pos.x
            + packed[2].c6
            + heroes[1].body.pos.y;

    int evalCounts = WorldConfig.holderEvalCount * 1000
                   + WorldConfig.packedArrEvalCount * 10000
                   + WorldConfig.packedIdxEvalCount * 100000
                   + WorldConfig.flatArrEvalCount * 1000000
                   + WorldConfig.flatIdxEvalCount * 10000000;
    return sum + evalCounts;
}

public class ArrayArena {
    public PackedParticle20[] packed;
    public FlatHero22[] flat;
}

public ArrayArena CreateArena() {
    ArrayArena a = new ArrayArena();
    a.packed = new PackedParticle20[8];
    a.flat = new FlatHero22[8];
    for (int i = 0; i < 8; i = i + 1) {
        a.flat[i].name = "unit";
    }
    return a;
}

public int MutateArenaHotLoop(ArrayArena a, int rounds) {
    for (int r = 0; r < rounds; r = r + 1) {
        for (int i = 0; i < 8; i = i + 1) {
            a.packed[i].pos.x += 2;
            a.packed[i].c7 += 1;
            a.flat[i].body.pos.y += 3;
            a.flat[i].level += 1;
        }
    }
    return a.packed[0].pos.x + a.packed[7].c7 + a.flat[0].body.pos.y + a.flat[7].level;
}
)RS";

    for (const auto optLevel : {optimization::Level::None, optimization::Level::Aggressive}) {
        auto built = compileScript("phase2_lvalues.rscript", kSrc, optLevel);
        auto heap = std::make_shared<runtime::ManagedHeap>();
        runtime::Interpreter vm(built.program, heap);

        const auto semRes = vm.invoke("Phase2.NestedLvalues::VerifyNestedLvalueSemantics");
        requireCond(semRes.succeeded, "VerifyNestedLvalueSemantics failed: " + semRes.error.message);
        // holder.hero.body.pos.x = 22, pos.z = 7, c7 = 8, globalOrigin.pos.y = 46,
        // packed[2].pos.x = 123, c6 = 9, heroes[1].body.pos.y = 42 => sum = 257
        // All 5 side-effectful receiver/index helpers ran EXACTLY ONCE => 11111000 => 11111257
        requireCond(
            std::get<std::int64_t>(semRes.value) == 11111257LL,
            "Unexpected VerifyNestedLvalueSemantics result: " +
                std::to_string(std::get<std::int64_t>(semRes.value)));

        const auto arenaRes = vm.invoke("Phase2.NestedLvalues::CreateArena");
        requireCond(arenaRes.succeeded, "CreateArena failed: " + arenaRes.error.message);
        auto arenaRoot = heap->retain(std::get<runtime::ObjectRef>(arenaRes.value));
        (void)arenaRoot;

        const std::uint64_t objBefore = heap->statistics().objectsAllocated;
        runtime::resetHeapStructStorageAllocations();

        const auto hotRes = vm.invoke(
            "Phase2.NestedLvalues::MutateArenaHotLoop",
            {arenaRes.value, std::int64_t{100}});
        requireCond(hotRes.succeeded, "MutateArenaHotLoop failed: " + hotRes.error.message);

        const std::uint64_t objAfter = heap->statistics().objectsAllocated;
        const std::uint64_t structBoxes = runtime::heapStructStorageAllocations();
        requireCond(
            objAfter == objBefore,
            "Expected 0 ManagedHeap allocations during in-place array hot loop, got " +
                std::to_string(objAfter - objBefore));
        requireCond(
            structBoxes == 0u,
            "Expected 0 StructStorage allocations during in-place array hot loop, got " +
                std::to_string(structBoxes));
        // 200 + 100 + 300 + 100 = 700
        requireCond(
            std::get<std::int64_t>(hotRes.value) == 700LL,
            "Unexpected MutateArenaHotLoop result");
    }

    // Verify negative diagnostics remain strictly enforced
    struct NegativeCase {
        const char* name;
        const char* code;
        const char* source;
    };
    const std::vector<NegativeCase> negativeCases = {
        {
            "rvalue_struct_receiver",
            "RS2482",
            R"RS(
module Neg.RvalueReceiver;
struct Inner { public int x; }
struct Outer { public Inner inner; }
Outer MakeOuter() { return new Outer(); }
void Bad() { MakeOuter().inner.x = 5; }
)RS",
        },
        {
            "readonly_field_nested_write",
            "RS2483",
            R"RS(
module Neg.ReadonlyField;
struct Inner { public int x; }
class Box {
    public readonly Inner item;
    public void Bad() { this.item.x = 10; }
}
)RS",
        },
        {
            "in_param_nested_write",
            "RS8702",
            R"RS(
module Neg.InParamWrite;
struct Inner { public int x; }
struct Outer { public Inner inner; }
void Bad(in Outer o) { o.inner.x = 3; }
)RS",
        },
        {
            "struct_property_setter",
            "RS2486",
            R"RS(
module Neg.StructSetter;
struct BadProp {
    public int Value { get; set; }
}
)RS",
        },
    };

    for (const auto& nc : negativeCases) {
        compiler::Compilation comp({{std::string(nc.name) + ".rscript", nc.source}});
        const auto build = comp.build();
        requireCond(
            build.diagnostics.hasErrors() && hasDiagnosticCode(build.diagnostics, nc.code),
            std::string("Expected diagnostic ") + nc.code + " for " + nc.name + ", got:\n" +
                formatDiagnostics(build.diagnostics));
    }
}

// ---------------------------------------------------------------------------
// 4. StencilJit (x86_64 Native + Threaded) & C++17 AOT Flat-Struct Fast Paths
// ---------------------------------------------------------------------------
void testStencilJitAndCppAotFastPaths() {
    std::cout << "[4/4] Testing StencilJit (x86_64 + Threaded) and C++17 AOT flat-struct & StackSlotRef fast paths...\n";
    constexpr const char* kSrc = R"RS(
module Phase2.JitAotSuite;

public struct SubFrame10 {
    public int a0; public int a1; public int a2; public int a3; public int a4;
    public int a5; public int a6; public int a7; public int a8; public int a9;

    public void Bump(int d) {
        this.a0 += d;
        this.a9 += d * 2;
    }
}

public struct SimBody22 {
    public SubFrame10 primary;
    public SubFrame10 secondary;
    public int hp;
    public int energy;
}

public struct NamedActor {
    public string tag;
    public SimBody22 sim;
    public int score;
}

public void SwapPair(ref int x, ref int y) {
    int t = x;
    x = y;
    y = t;
}

public SimBody22 MakeDefaultBody() {
    SimBody22 b = new SimBody22();
    b.hp = 500;
    b.primary.a0 = 7;
    return b;
}

public int StepPassedStruct(SimBody22 body, int rounds) {
    int p = 2;
    int q = 5;
    for (int i = 0; i < rounds; i = i + 1) {
        body.primary.a0 += 2;
        body.primary.a9 += 4;
        body.secondary.a4 += 2;
        body.hp -= 1;
        body.energy += 6;
        SwapPair(ref p, ref q);
    }
    return body.primary.a0 + body.primary.a9 + body.secondary.a4 + body.hp + body.energy + p * 10 + q;
}

public int RunScalarAndStructLoop(int rounds) {
    SimBody22 body = new SimBody22();
    body.hp = 500;
    body.primary.a0 = 7;
    int p = 2;
    int q = 5;
    for (int i = 0; i < rounds; i = i + 1) {
        body.primary.a0 += 2;
        body.primary.a9 += 4;
        body.secondary.a4 += 2;
        body.hp -= 1;
        body.energy += 6;
        SwapPair(ref p, ref q);
    }
    return body.primary.a0 + body.primary.a9 + body.secondary.a4 + body.hp + body.energy + p * 10 + q;
}

public SimBody22[] AllocPackedBodies(int count) {
    return new SimBody22[count];
}

public NamedActor[] AllocFlatActors(int count) {
    return new NamedActor[count];
}

public int StepPackedBodiesTyped(SimBody22[] packed, int rounds) {
    int n = packed.Length;
    for (int r = 0; r < rounds; r = r + 1) {
        for (int i = 0; i < n; i = i + 1) {
            packed[i].primary.a0 += 2;
            packed[i].secondary.a9 += 3;
        }
    }
    return packed[0].primary.a0 + packed[n - 1].secondary.a9;
}

public int StepArraysInPlace(SimBody22[] packed, NamedActor[] flat, int rounds) {
    int n = packed.Length;
    for (int r = 0; r < rounds; r = r + 1) {
        for (int i = 0; i < n; i = i + 1) {
            packed[i].primary.a0 += 2;
            packed[i].secondary.a9 += 3;
            flat[i].sim.primary.a1 += 4;
            flat[i].score += 1;
        }
    }
    return packed[0].primary.a0 + packed[n - 1].secondary.a9 + flat[0].sim.primary.a1 + flat[n - 1].score;
}
)RS";

    auto built = compileScript("phase2_jit_aot.rscript", kSrc, optimization::Level::Aggressive);

    // --- 4A. StencilJit Native x86_64 & ThreadedStencils Verification ---
    jit::StencilJit sjit;
    auto nativeCompiled = sjit.compile(built.program);
    requireCond(nativeCompiled.succeeded(), "StencilJit native compile failed: " + nativeCompiled.error);
    if (jit::ExecutableCodeArena::nativeCodeExecutionSupported()) {
        requireCond(
            nativeCompiled.fallbackFunctionCount == 0,
            "Expected 0 InterpreterFallback functions in StencilJit, got " +
                std::to_string(nativeCompiled.fallbackFunctionCount));
        requireCond(
            nativeCompiled.module->functionTier("Phase2.JitAotSuite::RunScalarAndStructLoop") ==
                jit::StencilTier::NativeMachineCode,
            "RunScalarAndStructLoop must compile to StencilTier::NativeMachineCode");
        requireCond(
            nativeCompiled.module->functionTier("Phase2.JitAotSuite::StepArraysInPlace") ==
                jit::StencilTier::NativeMachineCode,
            "StepArraysInPlace must compile to StencilTier::NativeMachineCode");
        requireCond(
            nativeCompiled.module->functionTier("Phase2.JitAotSuite::StepPassedStruct") ==
                jit::StencilTier::NativeMachineCode,
            "StepPassedStruct must compile to StencilTier::NativeMachineCode");
        requireCond(
            nativeCompiled.module->functionTier("Phase2.JitAotSuite::SubFrame10.Bump") ==
                jit::StencilTier::NativeMachineCode,
            "SubFrame10.Bump must compile to StencilTier::NativeMachineCode");
    }

    jit::StencilJitOptions threadedOpts;
    threadedOpts.enableNativeCodegen = false;
    auto threadedCompiled = sjit.compile(built.program, threadedOpts);
    requireCond(threadedCompiled.succeeded(), "StencilJit threaded compile failed: " + threadedCompiled.error);

    auto vmHeap = std::make_shared<runtime::ManagedHeap>();
    runtime::Interpreter vm(built.program, vmHeap);

    auto jitHeap = std::make_shared<runtime::ManagedHeap>();
    nativeCompiled.module->setHeap(jitHeap);

    auto thHeap = std::make_shared<runtime::ManagedHeap>();
    threadedCompiled.module->setHeap(thHeap);

    // Hot loop 1: >16-field flat struct parameter + struct method + ref Swap with 0 allocations
    const auto vmLoop = vm.invoke("Phase2.JitAotSuite::RunScalarAndStructLoop", {std::int64_t{100}});
    requireCond(vmLoop.succeeded, "VM RunScalarAndStructLoop failed: " + vmLoop.error.message);

    const std::uint64_t jitObjBefore = jitHeap->statistics().objectsAllocated;
    runtime::resetHeapStructStorageAllocations();
    const auto jitLoop = nativeCompiled.module->invoke(
        "Phase2.JitAotSuite::RunScalarAndStructLoop", {std::int64_t{100}});
    const std::uint64_t jitObjAfter = jitHeap->statistics().objectsAllocated;
    const std::uint64_t jitBoxes = runtime::heapStructStorageAllocations();

    requireCond(jitLoop.succeeded, "StencilJit RunScalarAndStructLoop failed: " + jitLoop.error.message);
    requireCond(jitLoop.value == vmLoop.value, "StencilJit vs VM value mismatch on RunScalarAndStructLoop");
    requireCond(
        jitLoop.instructionsExecuted == vmLoop.instructionsExecuted &&
            jitLoop.determinismDigest == vmLoop.determinismDigest,
        "StencilJit vs VM determinism/accounting mismatch on RunScalarAndStructLoop");
    requireCond(
        jitObjAfter == jitObjBefore && jitBoxes == 0u,
        "Expected 0 heap/StructStorage allocations in StencilJit RunScalarAndStructLoop, got obj=" +
            std::to_string(jitObjAfter - jitObjBefore) + ", structBoxes=" + std::to_string(jitBoxes));

    const auto thLoop = threadedCompiled.module->invoke(
        "Phase2.JitAotSuite::RunScalarAndStructLoop", {std::int64_t{100}});
    requireCond(
        thLoop.succeeded && thLoop.value == vmLoop.value,
        "Threaded StencilJit parity mismatch on RunScalarAndStructLoop");

    // Verify custom-struct entry parameter & return value across StencilJit NativeMachineCode
    const auto jitBodyVal = nativeCompiled.module->invoke("Phase2.JitAotSuite::MakeDefaultBody");
    requireCond(jitBodyVal.succeeded, "StencilJit MakeDefaultBody failed: " + jitBodyVal.error.message);
    const std::uint64_t passObjBefore = jitHeap->statistics().objectsAllocated;
    runtime::resetHeapStructStorageAllocations();
    const auto jitPassedRes = nativeCompiled.module->invoke(
        "Phase2.JitAotSuite::StepPassedStruct", {jitBodyVal.value, std::int64_t{100}});
    requireCond(
        jitPassedRes.succeeded && jitPassedRes.value == vmLoop.value,
        "StencilJit StepPassedStruct custom-struct entry arg mismatch");
    requireCond(
        jitHeap->statistics().objectsAllocated == passObjBefore &&
            runtime::heapStructStorageAllocations() == 0u,
        "Expected 0 allocations in StencilJit StepPassedStruct");

    // Hot loop 2: PackedStruct[] & FlatStruct[] in-place mutations in StencilJit with 0 allocations
    const auto packedArr = nativeCompiled.module->invoke("Phase2.JitAotSuite::AllocPackedBodies", {std::int64_t{6}});
    const auto flatArr = nativeCompiled.module->invoke("Phase2.JitAotSuite::AllocFlatActors", {std::int64_t{6}});
    requireCond(packedArr.succeeded && flatArr.succeeded, "StencilJit array setup failed");
    auto r1 = jitHeap->retain(std::get<runtime::ObjectRef>(packedArr.value));
    auto r2 = jitHeap->retain(std::get<runtime::ObjectRef>(flatArr.value));
    (void)r1;
    (void)r2;

    const std::uint64_t jitArrObjBefore = jitHeap->statistics().objectsAllocated;
    runtime::resetHeapStructStorageAllocations();
    const auto jitArrRes = nativeCompiled.module->invoke(
        "Phase2.JitAotSuite::StepArraysInPlace",
        {packedArr.value, flatArr.value, std::int64_t{80}});
    const std::uint64_t jitArrObjAfter = jitHeap->statistics().objectsAllocated;
    const std::uint64_t jitArrBoxes = runtime::heapStructStorageAllocations();

    requireCond(jitArrRes.succeeded, "StencilJit StepArraysInPlace failed: " + jitArrRes.error.message);
    // 80 * (2 + 3 + 4 + 1) = 800
    requireCond(std::get<std::int64_t>(jitArrRes.value) == 800LL, "Unexpected StencilJit StepArraysInPlace result");
    requireCond(
        jitArrObjAfter == jitArrObjBefore && jitArrBoxes == 0u,
        "Expected 0 heap/StructStorage allocations in StencilJit StepArraysInPlace, got obj=" +
            std::to_string(jitArrObjAfter - jitArrObjBefore) + ", structBoxes=" + std::to_string(jitArrBoxes));

    // --- 4B. C++17 AOT Codegen & Native Shared-Library Execution ---
    {
        diagnostics::DiagnosticBag aotDiags;
        aot::GenerationOptions genOpts;
        genOpts.programName = "Phase2AotProg";
        genOpts.cppNamespace = "phase2_aot_ns";
        genOpts.trustedRelease = false;
        aot::CppGenerator generator;
        const auto genOut = generator.generate(built.mirModules, aotDiags, genOpts);
        requireCond(
            !aotDiags.hasErrors() && !genOut.source.empty(),
            "C++17 AOT codegen failed:\n" + formatDiagnostics(aotDiags));
        requireCond(
            genOut.source.find("loadFromFlatLeaves") != std::string::npos &&
                genOut.source.find("storeToFlatLeaves") != std::string::npos,
            "Generated C++17 AOT must emit topological flat-leaf unbox/box helpers for >16-field nested structs");
        requireCond(
            genOut.source.find("StackRefScope") != std::string::npos,
            "Generated C++17 AOT must emit StackRefScope for zero-heap ref parameters");
        requireCond(
            genOut.source.find("loadElementStructCached") != std::string::npos ||
                genOut.source.find("storeElementStructCached") != std::string::npos ||
                genOut.source.find("loadElementStructFast") != std::string::npos ||
                genOut.source.find("storeElementStructFast") != std::string::npos,
            "Generated C++17 AOT must emit cached or fast struct array helpers");
    }

#if defined(REALSCRIPT_JIT_COMPILER) && defined(REALSCRIPT_JIT_INCLUDE_DIR) && \
    defined(REALSCRIPT_JIT_SUPPORT_LIBRARY) && defined(REALSCRIPT_JIT_CACHE_DIR)
    {
        jit::ToolchainOptions tcOpts;
        tcOpts.compiler = REALSCRIPT_JIT_COMPILER;
        tcOpts.includeDirectory = REALSCRIPT_JIT_INCLUDE_DIR;
        tcOpts.supportLibrary = REALSCRIPT_JIT_SUPPORT_LIBRARY;
        tcOpts.outputDirectory = std::filesystem::path(REALSCRIPT_JIT_CACHE_DIR) / "phase2_aot";
        tcOpts.reuseCachedLibrary = false;
#if defined(_WIN32)
#if defined(_DEBUG)
        tcOpts.compilerArguments = {"/MDd"};
#else
        tcOpts.compilerArguments = {"/MD", "/O2"};
#endif
#endif

        if (jit::ToolchainJit::available(tcOpts)) {
            jit::ToolchainJit tcJit;
            auto aotCompiled = tcJit.compile(built.mirModules, tcOpts);
            requireCond(
                aotCompiled.succeeded(),
                "ToolchainJit C++17 AOT compile failed: " + aotCompiled.error + "\n" +
                    aotCompiled.compilerOutput);

            auto aotHeap = std::make_shared<runtime::ManagedHeap>();
            aotCompiled.module->setHeap(aotHeap);

            const std::uint64_t aotObjBefore = aotHeap->statistics().objectsAllocated;
            runtime::resetHeapStructStorageAllocations();
            const auto aotLoop = aotCompiled.module->invoke(
                "Phase2.JitAotSuite::RunScalarAndStructLoop", {std::int64_t{100}});
            const std::uint64_t aotObjAfter = aotHeap->statistics().objectsAllocated;
            const std::uint64_t aotBoxes = runtime::heapStructStorageAllocations();

            requireCond(aotLoop.succeeded, "AOT RunScalarAndStructLoop failed: " + aotLoop.error.message);
            requireCond(aotLoop.value == vmLoop.value, "AOT vs VM value mismatch on RunScalarAndStructLoop");
            requireCond(
                aotObjAfter == aotObjBefore && aotBoxes == 0u,
                "Expected 0 heap/StructStorage allocations in C++17 AOT RunScalarAndStructLoop, got obj=" +
                    std::to_string(aotObjAfter - aotObjBefore) + ", structBoxes=" + std::to_string(aotBoxes));

            const auto aotPacked = aotCompiled.module->invoke(
                "Phase2.JitAotSuite::AllocPackedBodies", {std::int64_t{6}});
            const auto aotFlat = aotCompiled.module->invoke(
                "Phase2.JitAotSuite::AllocFlatActors", {std::int64_t{6}});
            requireCond(aotPacked.succeeded && aotFlat.succeeded, "AOT array allocation failed");
            auto ar1 = aotHeap->retain(std::get<runtime::ObjectRef>(aotPacked.value));
            auto ar2 = aotHeap->retain(std::get<runtime::ObjectRef>(aotFlat.value));
            (void)ar1;
            (void)ar2;

            const std::uint64_t aotArrObjBefore = aotHeap->statistics().objectsAllocated;
            runtime::resetHeapStructStorageAllocations();
            const auto aotArrRes = aotCompiled.module->invoke(
                "Phase2.JitAotSuite::StepArraysInPlace",
                {aotPacked.value, aotFlat.value, std::int64_t{80}});
            const std::uint64_t aotArrObjAfter = aotHeap->statistics().objectsAllocated;
            const std::uint64_t aotArrBoxes = runtime::heapStructStorageAllocations();

            requireCond(aotArrRes.succeeded, "AOT StepArraysInPlace failed: " + aotArrRes.error.message);
            requireCond(
                std::get<std::int64_t>(aotArrRes.value) == 800LL,
                "Unexpected AOT StepArraysInPlace result");
            requireCond(
                aotArrObjAfter == aotArrObjBefore && aotArrBoxes == 0u,
                "Expected 0 heap/StructStorage allocations in C++17 AOT StepArraysInPlace, got obj=" +
                    std::to_string(aotArrObjAfter - aotArrObjBefore) +
                    ", structBoxes=" + std::to_string(aotArrBoxes));
        }
    }
#endif
}

void testUntypedAotTransientSliceCap() {
    std::cout << "[5/6] Verifying ExecutionContext transient FrameSlice cap & StructStorage fallback...\n";
    const char* kCapSource = R"RS(
module Phase2.SliceCapSuite;

public struct BigStruct {
    public int F0; public int F1; public int F2; public int F3;
    public int F4; public int F5; public int F6; public int F7;
}

public int Touch(BigStruct s) {
    return s.F0 + s.F7;
}
)RS";
    auto built = compileScript("phase2_slice_cap.rscript", kCapSource);
    semantic::SymbolId structTypeId = 0;
    for (const auto& t : built.mirModules.front().types) {
        if (t.name == "BigStruct") {
            structTypeId = t.id;
            break;
        }
    }
    requireCond(structTypeId != 0, "BigStruct symbolId not found");

    std::vector<aot::FieldDescriptor> fields(8);
    const char* fNames[8] = {"F0", "F1", "F2", "F3", "F4", "F5", "F6", "F7"};
    for (std::uint32_t i = 0; i < 8; ++i) {
        fields[i] = {fNames[i], semantic::PrimitiveType::Int, 0, i, false};
    }
    aot::TypeDescriptor td{};
    td.id = structTypeId;
    td.kind = semantic::TypeKind::Struct;
    td.moduleName = "Phase2.SliceCapSuite";
    td.name = "BigStruct";
    td.fields = fields.data();
    td.fieldCount = static_cast<std::uint32_t>(fields.size());

    aot::ProgramDescriptor pd{};
    pd.name = "SliceCapProg";
    pd.types = &td;
    pd.typeCount = 1;

    auto heap = std::make_shared<runtime::ManagedHeap>();
    aot::ExecutionContext ctx(pd, heap, nullptr);
    aot::StackRefScope scope(ctx);

    runtime::Value sVal;
    runtime::Value updated;
    bool sawDetachedFallback = false;
    for (int i = 0; i < 12000; ++i) {
        requireCond(ctx.newStruct(structTypeId, sVal), "ExecutionContext::newStruct failed");
        requireCond(
            ctx.storeStructField(structTypeId, 7, sVal, runtime::Value{static_cast<std::int64_t>(i)}, updated),
            "ExecutionContext::storeStructField failed");
        if (const auto* sv = std::get_if<runtime::StructValue>(&updated); sv != nullptr && !sv->isFrameSlice()) {
            sawDetachedFallback = true;
        }
        requireCond(ctx.stackRefTop() <= 65536u, "stackRefTop exceeded transient FrameSlice cap");
    }
    runtime::Value readBack;
    requireCond(ctx.loadStructField(structTypeId, 7, updated, readBack), "loadStructField failed after fallback");
    requireCond(sawDetachedFallback, "Expected transient FrameSlice allocation to fall back to detached StructStorage after cap");
    requireCond(std::get<std::int64_t>(readBack) == 11999LL, "Detached fallback struct value mismatch");
}

void testDeepStructPropertyCompoundAssignSingleEval() {
    std::cout << "[6/6] Verifying deep struct auto-property compound assignment single-evaluation across VM, JIT, and AOT...\n";
    const char* kPropSource = R"RS(
module Phase2.StructPropCompoundSuite;

public class PropBox {
    public int Score { get; set; }
}

public struct InnerProp {
    public int Hp { get; }
    public int Bonus;

    public InnerProp(int baseHp, int addHp, int bonus) {
        this.Hp = baseHp;
        this.Hp += addHp;
        this.Bonus = bonus;
    }
}

public struct OuterProp {
    public InnerProp Inner;
    public PropBox Box;
}

public class EvalState {
    public static int ArrEvalCount = 0;
    public static int IdxEvalCount = 0;
    public static OuterProp[] SharedArr;

    public static OuterProp[] GetArr() {
        ArrEvalCount = ArrEvalCount + 1;
        return SharedArr;
    }

    public static int NextIdx() {
        IdxEvalCount = IdxEvalCount + 1;
        return 1;
    }
}

public int RunCompoundPropertyTest() {
    EvalState.ArrEvalCount = 0;
    EvalState.IdxEvalCount = 0;
    EvalState.SharedArr = new OuterProp[3];
    PropBox b = new PropBox();
    b.Score = 20;
    EvalState.SharedArr[1].Inner = new InnerProp(10, 5, 4);
    EvalState.SharedArr[1].Box = b;

    EvalState.GetArr()[EvalState.NextIdx()].Inner.Bonus += 6;
    EvalState.GetArr()[EvalState.NextIdx()].Box.Score += 7;

    if (EvalState.ArrEvalCount != 2 || EvalState.IdxEvalCount != 2) {
        return -1000 - EvalState.ArrEvalCount * 10 - EvalState.IdxEvalCount;
    }
    return EvalState.SharedArr[1].Inner.Hp * 10000 +
           EvalState.SharedArr[1].Inner.Bonus * 100 +
           EvalState.SharedArr[1].Box.Score;
}
)RS";
    auto built = compileScript("phase2_struct_prop_compound.rscript", kPropSource);

    auto vmHeap = std::make_shared<runtime::ManagedHeap>();
    runtime::Interpreter vm(built.program, vmHeap);
    const auto vmRes = vm.invoke("Phase2.StructPropCompoundSuite::RunCompoundPropertyTest");
    requireCond(vmRes.succeeded, "VM RunCompoundPropertyTest failed: " + vmRes.error.message);
    requireCond(
        std::get<std::int64_t>(vmRes.value) == 151027LL,
        "Expected Hp=15 Bonus=10 Score=27 -> 151027 (and single receiver/index evaluation), got " +
            std::to_string(std::get<std::int64_t>(vmRes.value)));

    jit::StencilJit jitEngine;
    auto jitCompiled = jitEngine.compile(built.program);
    requireCond(jitCompiled.succeeded(), "StencilJit compile failed: " + jitCompiled.error);
    auto jitHeap = std::make_shared<runtime::ManagedHeap>();
    jitCompiled.module->setHeap(jitHeap);
    const auto jitRes = jitCompiled.module->invoke("Phase2.StructPropCompoundSuite::RunCompoundPropertyTest");
    requireCond(jitRes.succeeded && jitRes.value == vmRes.value, "StencilJit compound property mismatch");
    const auto a64Res = jitCompiled.module->invokeSimulatedArm64("Phase2.StructPropCompoundSuite::RunCompoundPropertyTest");
    requireCond(a64Res.succeeded && a64Res.value == vmRes.value, "SimARM64 compound property mismatch");

#if defined(REALSCRIPT_JIT_COMPILER) && defined(REALSCRIPT_JIT_INCLUDE_DIR) && \
    defined(REALSCRIPT_JIT_SUPPORT_LIBRARY) && defined(REALSCRIPT_JIT_CACHE_DIR)
    jit::ToolchainOptions tcOpts;
    tcOpts.compiler = REALSCRIPT_JIT_COMPILER;
    tcOpts.includeDirectory = REALSCRIPT_JIT_INCLUDE_DIR;
    tcOpts.supportLibrary = REALSCRIPT_JIT_SUPPORT_LIBRARY;
    tcOpts.outputDirectory = std::filesystem::path(REALSCRIPT_JIT_CACHE_DIR) / "phase2_prop_compound";
    tcOpts.reuseCachedLibrary = false;
#if defined(_WIN32)
#if defined(_DEBUG)
    tcOpts.compilerArguments = {"/MDd"};
#else
    tcOpts.compilerArguments = {"/MD", "/O2"};
#endif
#endif
    if (jit::ToolchainJit::available(tcOpts)) {
        jit::ToolchainJit tcJit;
        auto aotMod = tcJit.compile(built.mirModules, tcOpts);
        requireCond(aotMod.succeeded(), "AOT compound property compile failed: " + aotMod.error);
        auto aotHeap = std::make_shared<runtime::ManagedHeap>();
        aotMod.module->setHeap(aotHeap);
        const auto aotRes = aotMod.module->invoke("Phase2.StructPropCompoundSuite::RunCompoundPropertyTest");
        requireCond(aotRes.succeeded && aotRes.value == vmRes.value, "AOT compound property mismatch");
    }
#endif
}

} // namespace

int main() {
    try {
        testStackSlotRefAbiInvariants();
        testZeroHeapStackSlotRef();
        testLiftedRs2482NestedWritesAndDiagnostics();
        testStencilJitAndCppAotFastPaths();
        testUntypedAotTransientSliceCap();
        testDeepStructPropertyCompoundAssignSingleEval();
        std::cout << "All Phase 2 StackSlotRef, RS2482 nested lvalues, StencilJit & C++17 AOT tests passed!\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[FAIL] " << ex.what() << "\n";
        return 1;
    }
}
