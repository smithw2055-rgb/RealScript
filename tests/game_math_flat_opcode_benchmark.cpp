#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/optimization/Optimizer.h"
#include "realscript/runtime/Runtime.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

void requireCond(bool cond, const std::string& msg) {
    if (!cond) {
        throw std::runtime_error(msg);
    }
}

const char* kGameMathBenchSource = R"RS(
module Bench.GameMath;
import RealScript.Math;

// 1. Dense Float arithmetic & damped spring integration benchmark (10,000 iterations)
public int benchFloatMath(int steps) {
    float x = 0.0f;
    float v = 1.5f;
    float target = 100.0f;
    float dt = 0.016f;
    float stiffness = 0.125f;
    float damping = 0.98f;
    for (int i = 0; i < steps; i = i + 1) {
        float force = (target - x) * stiffness + (float)(i % 7) * 0.05f;
        v = v * damping + force * dt;
        x = x + v * dt;
        if (x > 200.0f) {
            x = x - 50.0f;
        } else if (x < -200.0f) {
            x = -x * 0.5f;
        }
    }
    return (int)(x * 1000.0f);
}

// 2. Fixed (Q24.8 fixed-point) dense arithmetic & damped integration benchmark (10,000 iterations)
public int benchFixedMath(int steps) {
    Fixed x = Fixed.Of(0);
    Fixed v = Fixed.Ratio(3, 2);
    Fixed target = Fixed.Of(100);
    Fixed dt = Fixed.Raw(4);          // ~0.015625 (4/256)
    Fixed stiffness = Fixed.Raw(32);  // 0.125 (32/256)
    Fixed damping = Fixed.Raw(250);   // ~0.9765625 (250/256)
    Fixed bias = Fixed.Raw(12);
    Fixed upper = Fixed.Of(200);
    Fixed lower = Fixed.Of(-200);
    Fixed stepBack = Fixed.Of(50);
    for (int i = 0; i < steps; i = i + 1) {
        Fixed force = (target - x) * stiffness + bias;
        v = v * damping + force * dt;
        x = x + v * dt;
        if (x > upper) {
            x = x - stepBack;
        } else if (x < lower) {
            x = -x / 2;
        }
    }
    return x.raw + v.ToInt();
}

// 3. Vec3 3D kinematics integration, dot & cross product benchmark (10,000 iterations)
public int benchVec3Math(int steps) {
    Vec3 pos = Vec3.Of(1, 2, 3);
    Vec3 vel = Vec3.Raw(384, -256, 512);
    Vec3 acc = Vec3.Raw(16, 32, -24);
    Vec3 up = Vec3.Of(0, 1, 0);
    Fixed dt = Fixed.Raw(16);         // 0.0625
    Fixed drag = Fixed.Raw(248);      // 0.96875
    Fixed dotAcc = Fixed.Zero();
    for (int i = 0; i < steps; i = i + 1) {
        Vec3 coriolis = Vec3.Cross(vel, up);
        vel = vel * drag + (acc + coriolis * dt) * dt;
        pos = pos + vel * dt;
        dotAcc = dotAcc + Vec3.Dot(pos, vel) * dt;
    }
    return pos.x.raw + pos.y.raw + pos.z.raw + dotAcc.ToInt();
}

public int trapFixedDivZero(int denom) {
    Fixed a = Fixed.Of(10);
    Fixed b = Fixed.Raw(denom);
    Fixed c = a / b;
    return c.raw;
}

public int trapVec3Overflow(int big) {
    Vec3 a = Vec3.Raw(big, 10, 20);
    Vec3 b = Vec3.Raw(big, 10, 20);
    Vec3 c = a + b;
    return c.x.raw;
}

public int trapFixedFromIntPayload(int whole) {
    Fixed a = Fixed.Of(whole);
    return a.raw;
}

public int trapFixedMulIntPayload(int scalar) {
    Fixed a = Fixed.Of(3);
    Fixed b = a * scalar;
    return b.raw;
}
)RS";

std::shared_ptr<realscript::runtime::ProgramImage> compileSources(
    const std::vector<std::pair<std::string, std::string>>& sources,
    realscript::optimization::Level optLevel) {
    realscript::compiler::Compilation compilation;
    for (const auto& [path, text] : sources) {
        compilation.addSource({path, text});
    }
    auto build = compilation.build();
    requireCond(!build.diagnostics.hasErrors(), "Failed to compile test sources");

    realscript::optimization::Optimizer optimizer;
    realscript::optimization::Options optOpts;
    optOpts.level = optLevel;
    auto optimized = optimizer.optimize(std::move(build.modules), build.diagnostics, optOpts);
    requireCond(!build.diagnostics.hasErrors(), "Failed to optimize test sources");

    realscript::bytecode::Lowerer lowerer;
    std::vector<realscript::bytecode::Module> bc;
    for (const auto& mod : optimized.modules) {
        auto lowered = lowerer.lower(mod);
        requireCond(realscript::bytecode::verifyModule(lowered, build.diagnostics), "Bytecode verify failed");
        bc.push_back(std::move(lowered));
    }
    realscript::runtime::RuntimeError linkErr;
    auto linked = realscript::runtime::ProgramImage::link(std::move(bc), linkErr);
    requireCond(linked.has_value(), "Link failed: " + linkErr.message);
    return std::make_shared<realscript::runtime::ProgramImage>(std::move(*linked));
}

std::shared_ptr<realscript::runtime::ProgramImage> buildBenchProgram(
    realscript::optimization::Level level = realscript::optimization::Level::Aggressive) {
    return compileSources({{"game_math_bench.rs", kGameMathBenchSource}}, level);
}

struct BenchMetrics {
    std::string entry;
    double medianMicroseconds = 0.0;
    double minMicroseconds = 0.0;
    std::uint64_t instructionsExecuted = 0;
    std::uint64_t functionCalls = 0;
    std::int64_t returnValue = 0;
};

BenchMetrics runBenchCase(
    realscript::runtime::EngineRuntime& runtime,
    const std::string& entry,
    std::int64_t steps,
    int warmupCount,
    int sampleCount) {

    realscript::runtime::ExecutionOptions fastOpts;
    fastOpts.determinism.mode = realscript::runtime::DeterminismMode::Off;
    fastOpts.limits.instructionBudget = 100'000'000ULL;
    fastOpts.limits.gcWorkBudget = 8;

    // 1. Verify 100% parity against non-flat slow path (Trace/Strict mode) on 200 steps
    {
        realscript::runtime::ExecutionOptions slowOpts = fastOpts;
        slowOpts.determinism.mode = realscript::runtime::DeterminismMode::Strict;
        slowOpts.trace = [](const realscript::runtime::TraceEvent&) {};
        const auto fastCheck = runtime.invoke(entry, {std::int64_t{200}}, fastOpts);
        const auto slowCheck = runtime.invoke(entry, {std::int64_t{200}}, slowOpts);
        requireCond(fastCheck.succeeded && slowCheck.succeeded, entry + " parity run failed");
        requireCond(fastCheck.value == slowCheck.value, entry + " value mismatch");
        requireCond(
            fastCheck.instructionsExecuted == slowCheck.instructionsExecuted,
            entry + " instructionsExecuted mismatch");
        requireCond(
            fastCheck.statistics.functionCalls == slowCheck.statistics.functionCalls,
            entry + " functionCalls mismatch");
        requireCond(
            fastCheck.statistics.branchesTaken == slowCheck.statistics.branchesTaken,
            entry + " branchesTaken mismatch");
        requireCond(
            fastCheck.statistics.maximumCallDepth == slowCheck.statistics.maximumCallDepth,
            entry + " maximumCallDepth mismatch");
    }

    // 2. Warmup
    realscript::runtime::ExecutionResult last;
    for (int w = 0; w < warmupCount; ++w) {
        last = runtime.invoke(entry, {steps}, fastOpts);
        requireCond(last.succeeded, entry + " warmup failed: " + last.error.message);
    }

    // 3. Timed samples
    std::vector<double> samplesUs;
    samplesUs.reserve(static_cast<std::size_t>(sampleCount));
    for (int s = 0; s < sampleCount; ++s) {
        const auto t0 = std::chrono::steady_clock::now();
        last = runtime.invoke(entry, {steps}, fastOpts);
        const auto t1 = std::chrono::steady_clock::now();
        requireCond(last.succeeded, entry + " sample failed");
        samplesUs.push_back(
            std::chrono::duration<double, std::micro>(t1 - t0).count());
    }
    std::sort(samplesUs.begin(), samplesUs.end());

    BenchMetrics m;
    m.entry = entry;
    m.minMicroseconds = samplesUs.front();
    m.medianMicroseconds = samplesUs[samplesUs.size() / 2];
    m.instructionsExecuted = last.instructionsExecuted;
    m.functionCalls = last.statistics.functionCalls;
    m.returnValue = std::get<std::int64_t>(last.value);
    return m;
}

void testReviewRegressions() {
    using namespace realscript;

    // Regression 1: Typed `string[]` elements (`ObjectRef{kind=String}`) survive full, incremental, and young GC
    {
        runtime::ManagedHeap heap;
        const auto s0 = heap.allocateString("alpha_element");
        const auto s1 = heap.allocateString("beta_element");
        requireCond(s0.has_value() && s1.has_value(), "string allocation failed");
        const auto arr = heap.allocateTypedArray(
            semantic::stableTypeId("string[]"),
            semantic::PrimitiveType::String,
            0,
            2,
            runtime::NullString{});
        requireCond(arr.has_value(), "typed string[] allocation failed");
        requireCond(heap.arraySet(*arr, 0, *s0), "arraySet[0] failed");
        requireCond(heap.arraySet(*arr, 1, *s1), "arraySet[1] failed");

        std::vector<runtime::Value> regs{*arr};
        runtime::ShadowStack roots;
        roots.pushFrame(nullptr, nullptr, &regs);

        requireCond(heap.collectYoung(roots) > 0 || heap.isAlive(*s0), "collectYoung failed");
        requireCond(heap.isAlive(*s0) && heap.isAlive(*s1), "string[] elements reclaimed during young GC");

        heap.collectFull(roots);
        requireCond(heap.isAlive(*arr), "root string[] reclaimed");
        requireCond(heap.isAlive(*s0) && heap.isAlive(*s1), "string[] elements reclaimed during full GC");
        requireCond(heap.stringView(*s0) == "alpha_element", "s0 payload corrupted");
        requireCond(heap.stringView(*s1) == "beta_element", "s1 payload corrupted");
        roots.popFrame();
    }

    // Regression 2: In-place ProgramImage replacement invalidates TLS inlineCallCache (`lastMirrorId`)
    {
        const char* srcV1 = R"RS(
            module Test.Polymorph;
            public class Base { public virtual int eval() { return 111; } }
            public class Sub : Base { public override int eval() { return 111; } }
            public int run() { Base b = new Sub(); return b.eval(); }
        )RS";
        const char* srcV2 = R"RS(
            module Test.Polymorph;
            public class Base { public virtual int eval() { return 222; } }
            public class Sub : Base { public override int eval() { return 222; } }
            public int run() { Base b = new Sub(); return b.eval(); }
        )RS";
        auto prog = compileSources({{"v1.rs", srcV1}}, optimization::Level::Aggressive);
        auto prog2 = compileSources({{"v2.rs", srcV2}}, optimization::Level::Aggressive);

        runtime::Interpreter interp(prog);
        const auto r1 = interp.invoke("Test.Polymorph::run", {});
        requireCond(r1.succeeded && std::get<std::int64_t>(r1.value) == 111, "v1 polymorph failed");

        // Overwrite *prog in-place so `const ProgramImage*` address stays identical!
        *prog = std::move(*prog2);
        const auto r2 = interp.invoke("Test.Polymorph::run", {});
        requireCond(r2.succeeded && std::get<std::int64_t>(r2.value) == 222, "in-place ProgramImage inlineCallCache invalidation failed");
    }

    // Regression 3: Incremental GC `Sweep -> Mark` reopen + fresh ShadowStack + addPersistentRoot write barrier
    {
        runtime::ManagedHeap heap;
        const auto parent = heap.allocateRecord(1);
        const auto hiddenChild = heap.allocateString("hidden_across_sweep_reopen");
        const auto latePinned = heap.allocateString("pinned_during_sweep");
        requireCond(parent && hiddenChild && latePinned, "allocations failed");
        requireCond(heap.fieldSet(*parent, 0, *hiddenChild), "parent.f0 = hiddenChild failed");

        for (int i = 0; i < 32; ++i) {
            (void)heap.allocateString("filler_" + std::to_string(i));
        }

        {
            std::vector<runtime::Value> regs1{*parent};
            runtime::ShadowStack stack1;
            stack1.pushFrame(nullptr, nullptr, &regs1);
            heap.requestCollection();
            // Step once: starts Mark, marks `parent` (puts on markStack), `hiddenChild` & `latePinned` still unmarked
            (void)heap.step(stack1, 1);
            // Mutator hides `hiddenChild` out of `parent` before `parent` is scanned:
            requireCond(heap.fieldSet(*parent, 0, runtime::NullObject{}), "clear parent.f0 failed");
            // Advance until Sweep begins (`parent` had null, so `hiddenChild` & `latePinned` are still unmarked!)
            while (heap.phase() == runtime::GcPhase::Mark) {
                (void)heap.step(stack1, 1);
            }
            requireCond(heap.phase() == runtime::GcPhase::Sweep, "expected Sweep phase");
            // Pin `latePinned` during Sweep, and allocate a new object to reopen Mark from Sweep!
            const auto pinTok = heap.addPersistentRoot(*latePinned);
            requireCond(pinTok != 0, "addPersistentRoot during Sweep failed");
            (void)heap.allocateString("reopen_mark_trigger");
            requireCond(heap.phase() == runtime::GcPhase::Mark, "expected Mark reopened from Sweep");
            stack1.popFrame();
        }
        // Construct a brand-new ShadowStack (often reusing stack1's C++ stack address!) holding `hiddenChild`
        {
            std::vector<runtime::Value> regs2{*hiddenChild};
            runtime::ShadowStack stack2;
            stack2.pushFrame(nullptr, nullptr, &regs2);
            while (heap.phase() != runtime::GcPhase::Idle || heap.collectionRequested()) {
                (void)heap.step(stack2, 2);
            }
            requireCond(heap.isAlive(*hiddenChild), "hiddenChild on new ShadowStack was erroneously swept!");
            requireCond(heap.isAlive(*latePinned), "latePinned added during Sweep was erroneously swept!");
            stack2.popFrame();
        }
    }

    // Regression 4: ShadowStack > InlineFrameCapacity overflow, atomic move transfer & transient thread shard pruning
    {
        std::vector<std::vector<runtime::Value>> frames(7);
        for (std::size_t i = 0; i < frames.size(); ++i) {
            frames[i].push_back(runtime::StringValue("pooled_frame_str_" + std::to_string(i)));
        }
        runtime::ShadowStack sA;
        for (const auto& f : frames) {
            sA.pushFrame(nullptr, nullptr, &f);
        }
        requireCond(sA.frameCount() == 7, "ShadowStack overflow frameCount mismatch");

        runtime::ShadowStack sB(std::move(sA));
        requireCond(sA.frameCount() == 0 && sB.frameCount() == 7, "ShadowStack move-ctor frameCount mismatch");
        runtime::ShadowStack sC;
        sC = std::move(sB);
        requireCond(sB.frameCount() == 0 && sC.frameCount() == 7, "ShadowStack move-assign frameCount mismatch");

        // Spawn transient threads that register/unregister ShadowStacks and exit
        for (int t = 0; t < 4; ++t) {
            std::thread worker([]() {
                std::vector<runtime::Value> localVals{
                    runtime::StringValue(std::string_view("transient_worker_str"))};
                runtime::ShadowStack ws;
                ws.pushFrame(nullptr, nullptr, &localVals);
                ws.popFrame();
            });
            worker.join();
        }

        (void)runtime::reclaimUnreferencedPoolValues(&sC);
        std::size_t visitedCount = 0;
        sC.visitRoots([&](const runtime::Value& v) {
            ++visitedCount;
            requireCond(std::get<std::string>(v).rfind("pooled_frame_str_", 0) == 0, "unexpected pooled string");
        });
        requireCond(visitedCount == 7, "ShadowStack visitRoots count mismatch");
        while (sC.frameCount() > 0) sC.popFrame();
    }

    // Regression 5: `fieldSetFast` rejects dead managed String ObjectRef & `StoreFieldFast` respects instruction budget before mutating heap
    {
        const char* boxSrc = R"RS(
            module Test.BoxSafety;
            public class Box {
                public int count;
                public string label;
                public Box() { count = 7; label = "init"; }
            }
            public Box makeBox() { return new Box(); }
            public void writeCount(Box b, int v) { b.count = v; }
            public void writeLabel(Box b, string s) { b.label = s; }
        )RS";
        auto prog = compileSources({{"box.rs", boxSrc}}, optimization::Level::None);
        runtime::Interpreter interp(prog);
        auto heap = interp.heap();

        const auto made = interp.invoke("Test.BoxSafety::makeBox", {});
        requireCond(made.succeeded && std::holds_alternative<runtime::ObjectRef>(made.value), "makeBox failed");
        const auto boxRef = std::get<runtime::ObjectRef>(made.value);
        auto boxPin = heap->retain(boxRef);

        // Create and reclaim a stale string ObjectRef
        const auto deadStr = heap->allocateString("ephemeral_dead_string");
        requireCond(deadStr.has_value(), "deadStr alloc failed");
        runtime::ShadowStack emptyRoots;
        heap->collectFull(emptyRoots);
        requireCond(!heap->isAlive(*deadStr), "deadStr should have been reclaimed");

        const auto boxTid = heap->objectTypeId(boxRef);
        requireCond(boxTid.has_value(), "boxTid missing");
        requireCond(
            !heap->fieldSetFast(boxRef, 1, *deadStr, *boxTid, semantic::PrimitiveType::String),
            "fieldSetFast must reject reclaimed string ObjectRef");

        const auto staleWrite = interp.invoke("Test.BoxSafety::writeLabel", {boxRef, *deadStr});
        requireCond(
            !staleWrite.succeeded && staleWrite.error.code == runtime::ErrorCode::InvalidObjectReference,
            "writeLabel with dead string ObjectRef must fail with InvalidObjectReference");

        // Verify exact heap-mutation parity against slow/Strict mode at every instruction budget boundary!
        const auto okWrite = interp.invoke("Test.BoxSafety::writeCount", {boxRef, std::int64_t{42}});
        requireCond(okWrite.succeeded && okWrite.instructionsExecuted >= 2, "baseline writeCount failed");
        requireCond(std::get<std::int64_t>(*heap->fieldGet(boxRef, 0)) == 42, "baseline fieldGet != 42");

        for (std::uint64_t b = 1; b < okWrite.instructionsExecuted; ++b) {
            requireCond(heap->fieldSet(boxRef, 0, std::int64_t{42}), "reset box.count fast failed");
            runtime::ExecutionOptions limFast;
            limFast.determinism.mode = runtime::DeterminismMode::Off;
            limFast.limits.instructionBudget = b;
            const auto trappedFast = interp.invoke("Test.BoxSafety::writeCount", {boxRef, std::int64_t{999}}, limFast);
            const auto fastAfter = std::get<std::int64_t>(*heap->fieldGet(boxRef, 0));

            requireCond(heap->fieldSet(boxRef, 0, std::int64_t{42}), "reset box.count slow failed");
            runtime::ExecutionOptions limSlow;
            limSlow.determinism.mode = runtime::DeterminismMode::Strict;
            limSlow.limits.instructionBudget = b;
            const auto trappedSlow = interp.invoke("Test.BoxSafety::writeCount", {boxRef, std::int64_t{999}}, limSlow);
            const auto slowAfter = std::get<std::int64_t>(*heap->fieldGet(boxRef, 0));

            requireCond(
                !trappedFast.succeeded && !trappedSlow.succeeded &&
                    trappedFast.error.code == runtime::ErrorCode::InstructionBudgetExceeded &&
                    trappedSlow.error.code == runtime::ErrorCode::InstructionBudgetExceeded &&
                    trappedFast.instructionsExecuted == trappedSlow.instructionsExecuted &&
                    fastAfter == slowAfter,
                "StoreFieldFast heap mutation on budget boundary diverged from slow path!");
        }
    }

    // Regression 6: Host-constructed Outer struct containing wrong nested StructB field must fail LoadStructFieldFast with TypeMismatch
    {
        const char* nestedSrc = R"RS(
            module Test.NestedStructSafety;
            public struct StructA { public int a; }
            public struct StructB { public int b; }
            public struct Outer { public StructA child; }
            public int readNested(Outer o) {
                StructA c = o.child;
                return c.a;
            }
        )RS";
        auto prog = compileSources({{"nested.rs", nestedSrc}}, optimization::Level::None);
        runtime::Interpreter interp(prog);

        const auto structBId = semantic::stableTypeId("Test.NestedStructSafety::StructB");
        const auto outerId = semantic::stableTypeId("Test.NestedStructSafety::Outer");
        const auto wrongChild = runtime::makeStructValueFromFields(
            structBId, std::vector<runtime::Value>{runtime::Value{std::int64_t{77}}});
        const auto malformedOuter = runtime::makeStructValueFromFields(
            outerId, std::vector<runtime::Value>{runtime::Value{wrongChild}});

        runtime::ExecutionOptions fastOpts;
        fastOpts.determinism.mode = runtime::DeterminismMode::Off;
        runtime::ExecutionOptions slowOpts;
        slowOpts.determinism.mode = runtime::DeterminismMode::Strict;

        const auto fastRes = interp.invoke("Test.NestedStructSafety::readNested", {malformedOuter}, fastOpts);
        const auto slowRes = interp.invoke("Test.NestedStructSafety::readNested", {malformedOuter}, slowOpts);
        requireCond(
            !fastRes.succeeded && !slowRes.succeeded &&
                fastRes.error.code == runtime::ErrorCode::TypeMismatch &&
                slowRes.error.code == runtime::ErrorCode::TypeMismatch,
            "LoadStructFieldFast failed to reject mismatched nested Struct SymbolId");
    }

    // Regression 7: User-defined `module RealScript.Math;` override must NOT be miscompiled into builtin Q24.8 intrinsics
    {
        const char* customMath = R"RS(
            module RealScript.Math;
            public struct Fixed {
                public int raw;
                public Fixed(int r) { raw = r; }
                public static Fixed One() { return new Fixed(1000); }
                public static Fixed operator *(Fixed a, Fixed b) {
                    return new Fixed((int)(((long)a.raw * (long)b.raw) / 1000));
                }
            }
        )RS";
        const char* callerSrc = R"RS(
            module Test.CustomMathCaller;
            import RealScript.Math;
            public int compute() {
                Fixed a = Fixed.One();
                Fixed b = new Fixed(2500);
                Fixed c = a * b;
                return c.raw;
            }
        )RS";
        auto prog = compileSources(
            {{"custom_math.rs", customMath}, {"caller.rs", callerSrc}},
            optimization::Level::None);
        runtime::Interpreter interp(prog);
        runtime::ExecutionOptions fastOpts;
        fastOpts.determinism.mode = runtime::DeterminismMode::Off;
        runtime::ExecutionOptions slowOpts;
        slowOpts.determinism.mode = runtime::DeterminismMode::Strict;

        const auto fastRes = interp.invoke("Test.CustomMathCaller::compute", {}, fastOpts);
        const auto slowRes = interp.invoke("Test.CustomMathCaller::compute", {}, slowOpts);
        requireCond(
            fastRes.succeeded && slowRes.succeeded &&
                std::get<std::int64_t>(fastRes.value) == 2500 &&
                fastRes.value == slowRes.value,
            "user-defined RealScript.Math module was miscompiled into builtin Q24.8 intrinsics");
    }
}

} // namespace

int main() {
    try {
        testReviewRegressions();

        // Test math fast paths at both -O0 (uninlined CallDirect -> Math* FlatOpcodes) and -O2 (inlined + uninlined)
        for (const auto optLevel : {
                 realscript::optimization::Level::None,
                 realscript::optimization::Level::Aggressive}) {
            auto program = buildBenchProgram(optLevel);
            realscript::runtime::EngineRuntime runtime(program);

            realscript::runtime::ExecutionOptions fastOpts;
            fastOpts.determinism.mode = realscript::runtime::DeterminismMode::Off;
            realscript::runtime::ExecutionOptions slowOpts;
            slowOpts.determinism.mode = realscript::runtime::DeterminismMode::Strict;

            const auto divFast = runtime.invoke("Bench.GameMath::trapFixedDivZero", {std::int64_t{0}}, fastOpts);
            const auto divSlow = runtime.invoke("Bench.GameMath::trapFixedDivZero", {std::int64_t{0}}, slowOpts);
            requireCond(
                !divFast.succeeded && !divSlow.succeeded &&
                    divFast.error.code == realscript::runtime::ErrorCode::DivisionByZero &&
                    divFast.error.stackTrace == divSlow.error.stackTrace &&
                    divFast.instructionsExecuted == divSlow.instructionsExecuted,
                "trapFixedDivZero parity failed");

            const auto ovfFast = runtime.invoke("Bench.GameMath::trapVec3Overflow", {std::int64_t{1500000000}}, fastOpts);
            const auto ovfSlow = runtime.invoke("Bench.GameMath::trapVec3Overflow", {std::int64_t{1500000000}}, slowOpts);
            requireCond(
                !ovfFast.succeeded && !ovfSlow.succeeded &&
                    ovfFast.error.code == realscript::runtime::ErrorCode::IntegerOverflow &&
                    ovfFast.error.stackTrace == ovfSlow.error.stackTrace &&
                    ovfFast.instructionsExecuted == ovfSlow.instructionsExecuted,
                "trapVec3Overflow parity failed");

            // Host out-of-int32 int64_t payload safety (no C++ signed int64 overflow UB)
            const std::int64_t hugePayload = (std::int64_t{1} << 40);
            const auto hugeOfFast = runtime.invoke("Bench.GameMath::trapFixedFromIntPayload", {hugePayload}, fastOpts);
            const auto hugeOfSlow = runtime.invoke("Bench.GameMath::trapFixedFromIntPayload", {hugePayload}, slowOpts);
            requireCond(
                !hugeOfFast.succeeded && !hugeOfSlow.succeeded &&
                    hugeOfFast.error.code == realscript::runtime::ErrorCode::IntegerOverflow &&
                    hugeOfSlow.error.code == realscript::runtime::ErrorCode::IntegerOverflow,
                "trapFixedFromIntPayload out-of-int32 guard failed");

            const auto hugeMulFast = runtime.invoke("Bench.GameMath::trapFixedMulIntPayload", {hugePayload}, fastOpts);
            const auto hugeMulSlow = runtime.invoke("Bench.GameMath::trapFixedMulIntPayload", {hugePayload}, slowOpts);
            requireCond(
                !hugeMulFast.succeeded && !hugeMulSlow.succeeded &&
                    hugeMulFast.error.code == realscript::runtime::ErrorCode::IntegerOverflow &&
                    hugeMulSlow.error.code == realscript::runtime::ErrorCode::IntegerOverflow,
                "trapFixedMulIntPayload out-of-int32 guard failed");

            if (optLevel == realscript::optimization::Level::None) {
                (void)runBenchCase(runtime, "Bench.GameMath::benchFloatMath", 500, 1, 2);
                (void)runBenchCase(runtime, "Bench.GameMath::benchFixedMath", 500, 1, 2);
                (void)runBenchCase(runtime, "Bench.GameMath::benchVec3Math", 500, 1, 2);
            } else {
                constexpr std::int64_t kSteps = 10000;
                const auto floatMetrics = runBenchCase(runtime, "Bench.GameMath::benchFloatMath", kSteps, 5, 15);
                const auto fixedMetrics = runBenchCase(runtime, "Bench.GameMath::benchFixedMath", kSteps, 5, 15);
                const auto vec3Metrics = runBenchCase(runtime, "Bench.GameMath::benchVec3Math", kSteps, 5, 15);

                std::cout << std::fixed << std::setprecision(2);
                std::cout << "=== RealScript Game Math FlatOpcode Benchmark (10,000 iterations, -O2) ===\n";
                for (const auto& m : {floatMetrics, fixedMetrics, vec3Metrics}) {
                    std::cout << "[PASS] " << m.entry
                              << " | median=" << m.medianMicroseconds << " us"
                              << " (min=" << m.minMicroseconds << " us)"
                              << " | instructions=" << m.instructionsExecuted
                              << " | calls=" << m.functionCalls
                              << " | result=" << m.returnValue << "\n";
                }
            }
        }
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[FAIL] " << ex.what() << "\n";
        return 1;
    }
}
