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

// 4. Fixed.Sin / Fixed.Cos / Fixed.Sqrt wave & distance integration benchmark (10,000 iterations)
public int benchTrigSqrtMath(int steps) {
    Fixed angle = Fixed.Raw(-1800);
    Fixed stepAngle = Fixed.Raw(37);
    Fixed acc = Fixed.Zero();
    for (int i = 0; i < steps; i = i + 1) {
        Fixed s = Fixed.Sin(angle);
        Fixed c = Fixed.Cos(angle);
        Fixed radSq = s * s + c * c + Fixed.Raw((i % 16) * 64);
        Fixed dist = Fixed.Sqrt(radSq);
        acc = acc + (dist - s + c) / 4;
        angle = angle + stepAngle;
        if (angle.raw > 2000) {
            angle = angle - Fixed.Raw(4000);
        }
    }
    return acc.raw + angle.raw;
}

// 5. Quat (FromEuler + Multiply + Lerp + Rotate + op_Multiply(Vec3)) skeletal/camera rig benchmark (10,000 iterations)
public int benchQuatMath(int steps) {
    Quat orientation = Quat.Identity();
    Quat targetFlip = Quat.Raw(0, 0, 0, -256);
    Vec3 forward = Vec3.Of(0, 0, 1);
    Vec3 up = Vec3.Of(0, 1, 0);
    Fixed blend = Fixed.Raw(64); // 0.25
    Fixed dotAcc = Fixed.Zero();
    for (int i = 0; i < steps; i = i + 1) {
        Fixed pitch = Fixed.Raw((i % 17) * 12 - 96);
        Fixed yaw = Fixed.Raw((i % 23) * 16 - 176);
        Fixed roll = Fixed.Raw((i % 11) * 8 - 40);
        Quat delta = Quat.FromEuler(pitch, yaw, roll);
        Quat combined = orientation * delta;
        Quat goal = (i % 5 == 0) ? targetFlip : delta;
        orientation = Quat.Lerp(combined, goal, blend);
        forward = orientation.Rotate(forward);
        up = orientation * up;
        dotAcc = dotAcc + Quat.Dot(orientation, delta) + forward.x - up.z;
    }
    return orientation.x.raw + orientation.y.raw + orientation.z.raw + orientation.w.raw +
           forward.y.raw + up.x.raw + dotAcc.ToInt();
}

public int trapCosOverflow(int rawVal) {
    Fixed r = Fixed.Raw(rawVal);
    Fixed c = Fixed.Cos(r);
    return c.raw;
}

public int trapQuatMulOverflow(int big) {
    Quat a = Quat.Raw(big, big, big, big);
    Quat b = Quat.Raw(big, big, big, big);
    Quat c = a * b;
    return c.x.raw;
}

public int trapQuatRotateBoxed(int big) {
    Quat q = Quat.Raw(big, 128, -64, 256);
    Vec3 v = Vec3.Raw(big, 256, -128);
    Vec3 rotated = q.Rotate(v);
    return rotated.x.raw + q.w.raw;
}

public int edgeSqrtAndZeroQuatLerp() {
    Fixed z0 = Fixed.Sqrt(Fixed.Zero());
    Fixed zNeg = Fixed.Sqrt(Fixed.Raw(-512));
    Fixed zPos = Fixed.Sqrt(Fixed.Of(9));
    long sNeg = Math.Sqrt64((long)-100);
    long sZero = Math.Sqrt64((long)0);
    long sMax = Math.Sqrt64(9223372036854775807);
    Quat qZero = Quat.Raw(0, 0, 0, 0);
    Quat qId = Quat.Lerp(qZero, qZero, Fixed.Half());
    Quat qZeroNorm = qZero.Normalized();
    return z0.raw + zNeg.raw + zPos.raw + (int)sNeg + (int)sZero + (int)(sMax % (long)1000) + qId.w.raw + qZeroNorm.w.raw;
}

// 6. Chained Vec3 + Quat Rigid-Body & Bone Hierarchy Transform benchmark (10,000 iterations)
// Exercises SuperQuatMulNormalized, SuperVec3AddQuatRotate, SuperQuatMulRotateVec3, SuperQuatMulChain, SuperVec3MulAdd, SuperLoadVec3RawFast, SuperLoadQuatRawFast
public int benchTransformRigMath(int steps) {
    Quat rootRot = Quat.Identity();
    Quat deltaRot = Quat.Raw(4, 8, -3, 255);
    Quat childLocalRot = Quat.Raw(12, -6, 9, 254);
    Vec3 rootPos = Vec3.Of(0, 1, 0);
    Vec3 localVel = Vec3.Raw(64, 16, 128);
    Vec3 boneTipOffset = Vec3.Of(1, 2, -1);
    Fixed dt = Fixed.Raw(16);
    Vec3 tipAccum = Vec3.Zero();
    for (int i = 0; i < steps; i = i + 1) {
        rootRot = (rootRot * deltaRot).Normalized();
        rootPos = rootPos + rootRot * localVel;
        Vec3 worldTip = rootPos + (rootRot * childLocalRot) * boneTipOffset;
        tipAccum = tipAccum + worldTip * dt;
    }
    return rootRot.x.raw + rootRot.w.raw + rootPos.x.raw + tipAccum.x.raw + tipAccum.y.raw + tipAccum.z.raw;
}

public int trapSuperCompoundOverflow(int big) {
    Vec3 pos = Vec3.Raw(big, 10, 20);
    Vec3 vel = Vec3.Raw(big, 10, 20);
    Fixed one = Fixed.One();
    pos = pos + vel * one;
    return pos.x.raw;
}

public int probeSuperChainBudget(int scale) {
    Quat a = Quat.Raw(4, 8, -3, 255);
    Quat b = Quat.Raw(12, -6, 9, 254);
    Quat c = Quat.Raw(-5, 7, 11, 253);
    Quat ch = (a * b) * c;
    Quat nm = (a * b).Normalized();
    Vec3 p = Vec3.Of(1, 2, 3);
    Vec3 o = Vec3.Raw(scale, 64, -32);
    p = p + nm.Rotate(o);
    Vec3 r = (ch * b) * o;
    return p.x.raw + r.y.raw + nm.w.raw;
}

public int edgeSinSqrt64AndSelfNorm() {
    Fixed sMin = Fixed.Sin(Fixed.Raw(-2147483647 - 1));
    Fixed sNegWrap = Fixed.Sin(Fixed.Raw(-1500));
    Fixed sPosWrap = Fixed.Sin(Fixed.Raw(1500));
    long sqSmall = Math.Sqrt64((long)1000000);
    long sqLarge = Math.Sqrt64((long)4000000000000000000);
    Quat q = Quat.Raw(128, -256, 512, 1024);
    q = q.Normalized();
    Vec3 v = Vec3.Of(2, -3, 4);
    v = q.Rotate(v);
    return sMin.raw + sNegWrap.raw + sPosWrap.raw + (int)(sqSmall + (sqLarge % (long)10000)) + q.w.raw + v.x.raw;
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

    // 1. Verify 100% parity against non-flat slow path (Trace/Strict mode) on 200 steps, including zero spurious heap/GC divergence
    {
        realscript::runtime::ExecutionOptions slowOpts = fastOpts;
        slowOpts.determinism.mode = realscript::runtime::DeterminismMode::Strict;
        slowOpts.trace = [](const realscript::runtime::TraceEvent&) {};
        const auto heapBeforeFast = runtime.heap()->statistics();
        const auto fastCheck = runtime.invoke(entry, {std::int64_t{200}}, fastOpts);
        const auto heapAfterFast = runtime.heap()->statistics();
        const auto slowCheck = runtime.invoke(entry, {std::int64_t{200}}, slowOpts);
        const auto heapAfterSlow = runtime.heap()->statistics();
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
        requireCond(
            fastCheck.statistics.gcWorkPerformed == slowCheck.statistics.gcWorkPerformed,
            entry + " gcWorkPerformed mismatch");
        const auto fastObjs = heapAfterFast.objectsAllocated - heapBeforeFast.objectsAllocated;
        const auto slowObjs = heapAfterSlow.objectsAllocated - heapAfterFast.objectsAllocated;
        requireCond(fastObjs == slowObjs, entry + " heap objectsAllocated parity mismatch");
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

void testCompact16BValueSlotRegressions() {
    using namespace realscript;
    static_assert(sizeof(runtime::Value) == 16, "Value must be 16 bytes");
    static_assert(alignof(runtime::Value) == 16, "Value must be 16-byte aligned");
    static_assert(sizeof(runtime::ObjectRef) == 16, "ObjectRef must be 16 bytes");
    static_assert(sizeof(runtime::StructValue) == 16, "StructValue must be 16 bytes");
    static_assert(sizeof(runtime::StringValue) == 16, "StringValue must be 16 bytes");

    std::vector<runtime::Value> alignedFrame(64);
    requireCond(
        (reinterpret_cast<std::uintptr_t>(alignedFrame.data()) & 15u) == 0u,
        "std::vector<Value> buffer must be 16-byte aligned");

    // 1. Canonical index() 0..25 parity across inline & arena representations
    requireCond(runtime::Value{std::monostate{}}.index() == 0, "monostate index");
    requireCond(runtime::Value{runtime::NullString{}}.index() == 1, "NullString index");
    requireCond(runtime::Value{runtime::NullObject{}}.index() == 2, "NullObject index");
    requireCond(runtime::Value{runtime::NullArray{}}.index() == 3, "NullArray index");
    requireCond(runtime::Value{true}.index() == 4, "bool index");
    requireCond(runtime::Value{runtime::ByteValue{7}}.index() == 5, "Byte index");
    requireCond(runtime::Value{runtime::SByteValue{-7}}.index() == 6, "SByte index");
    requireCond(runtime::Value{runtime::ShortValue{-300}}.index() == 7, "Short index");
    requireCond(runtime::Value{runtime::UShortValue{300}}.index() == 8, "UShort index");
    requireCond(runtime::Value{std::int64_t{42}}.index() == 9, "int64 index");
    requireCond(runtime::Value{runtime::UIntValue{42}}.index() == 10, "UInt index");
    requireCond(runtime::Value{runtime::LongValue{42}}.index() == 11, "Long index");
    requireCond(runtime::Value{runtime::ULongValue{42}}.index() == 12, "ULong index");
    requireCond(runtime::Value{runtime::FloatValue{1.5f}}.index() == 13, "Float index");
    requireCond(runtime::Value{2.5}.index() == 14, "double index");
    requireCond(runtime::Value{runtime::CharValue{u'A'}}.index() == 15, "Char index");

    const auto enumTid = semantic::stableTypeId("Test.BigFlags");
    const runtime::Value enumInl{runtime::EnumValue{enumTid, -123456}};
    const runtime::Value enumBig{runtime::EnumValue{enumTid, (1LL << 44) + 77LL}};
    requireCond(enumInl.rawTag() == runtime::ValueTag::EnumInline && enumInl.index() == 16 && !enumInl.isTraceable(), "EnumInline tag/index");
    requireCond(enumBig.rawTag() == runtime::ValueTag::EnumArena && enumBig.index() == 16 && enumBig.isTraceable(), "EnumArena tag/index");

    requireCond(runtime::Value{runtime::FixedValue{256}}.index() == 17, "Fixed index");
    requireCond(runtime::Value{runtime::Fixed16Value{65536}}.index() == 18, "Fixed16 index");
    requireCond(runtime::Value{runtime::Vec2Value{1, 2}}.index() == 19, "Vec2 index");
    requireCond(runtime::Value{runtime::Vec3Value{1, 2, 3}}.index() == 20, "Vec3 index");

    const runtime::Value qMinInl{runtime::QuatValue{11, -22, 33, -8388608}};
    const runtime::Value qMaxInl{runtime::QuatValue{-44, 55, -66, 8388607}};
    const runtime::Value qBigPos{runtime::QuatValue{101, -202, 303, 1500000000}};
    const runtime::Value qBigNeg{runtime::QuatValue{-404, 505, -606, -1500000000}};
    requireCond(qMinInl.rawTag() == runtime::ValueTag::QuatInline && qMinInl.index() == 21 && !qMinInl.isTraceable(), "QuatInline min boundary");
    requireCond(qMaxInl.rawTag() == runtime::ValueTag::QuatInline && qMaxInl.index() == 21 && !qMaxInl.isTraceable(), "QuatInline max boundary");
    requireCond(qBigPos.rawTag() == runtime::ValueTag::QuatArena && qBigPos.index() == 21 && qBigPos.isTraceable(), "QuatArena pos overflow");
    requireCond(qBigNeg.rawTag() == runtime::ValueTag::QuatArena && qBigNeg.index() == 21 && qBigNeg.isTraceable(), "QuatArena neg overflow");
    requireCond(std::get<runtime::QuatValue>(qMinInl) == (runtime::QuatValue{11, -22, 33, -8388608}), "QuatInline min roundtrip");
    requireCond(std::get<runtime::QuatValue>(qMaxInl) == (runtime::QuatValue{-44, 55, -66, 8388607}), "QuatInline max roundtrip");
    requireCond(std::get<runtime::QuatValue>(qBigPos) == (runtime::QuatValue{101, -202, 303, 1500000000}), "QuatArena pos roundtrip");
    requireCond(std::get<runtime::QuatValue>(qBigNeg) == (runtime::QuatValue{-404, 505, -606, -1500000000}), "QuatArena neg roundtrip");

    // Simultaneous std::get_if<QuatValue> & std::get_if<EnumValue> ring-buffer safety
    const auto* qp1 = std::get_if<runtime::QuatValue>(&qBigPos);
    const auto* qp2 = std::get_if<runtime::QuatValue>(&qBigNeg);
    requireCond(qp1 && qp2 && qp1->w == 1500000000 && qp2->w == -1500000000, "simultaneous get_if<QuatValue> ring");

    // 2. Pool GC survival for QuatArena & EnumArena on ShadowStack and ManagedHeap
    {
        runtime::ManagedHeap heap;
        const auto rec = heap.allocateRecord(2);
        requireCond(rec.has_value(), "allocateRecord for arena values");
        requireCond(heap.fieldSet(*rec, 0, qBigPos), "fieldSet QuatArena");
        requireCond(heap.fieldSet(*rec, 1, enumBig), "fieldSet EnumArena");

        std::vector<runtime::Value> stackVals{*rec, qBigNeg};
        runtime::ShadowStack roots;
        roots.pushFrame(nullptr, nullptr, &stackVals);
        heap.collectFull(roots);
        (void)runtime::reclaimUnreferencedPoolValues(&roots);

        const auto f0 = heap.fieldGet(*rec, 0);
        const auto f1 = heap.fieldGet(*rec, 1);
        requireCond(f0 && std::get<runtime::QuatValue>(*f0) == (runtime::QuatValue{101, -202, 303, 1500000000}), "QuatArena heap survival");
        requireCond(f1 && std::get<runtime::EnumValue>(*f1) == (runtime::EnumValue{enumTid, (1LL << 44) + 77LL}), "EnumArena heap survival");
        requireCond(std::get<runtime::QuatValue>(stackVals[1]) == (runtime::QuatValue{-404, 505, -606, -1500000000}), "QuatArena stack survival");
        roots.popFrame();
    }

    // 3. NativeHandle multi-type round-trip & mutable ObjectRef / StructValue in-place get_if
    for (int i = 1; i <= 32; ++i) {
        const auto tid = semantic::stableTypeId("Engine.HandleType" + std::to_string(i));
        const runtime::NativeHandle nh(static_cast<std::uint32_t>(i * 7), static_cast<std::uint32_t>(i), tid, 3u);
        const runtime::Value v{nh};
        requireCond(v.index() == 25 && std::get<runtime::NativeHandle>(v) == nh, "NativeHandle round-trip");
    }
    {
        runtime::Value mv{runtime::ObjectRef(5u, 2u, runtime::ObjectKind::Record, 9u)};
        auto* mutObj = std::get_if<runtime::ObjectRef>(&mv);
        requireCond(mutObj != nullptr, "mutable get_if<ObjectRef>");
        *mutObj = runtime::ObjectRef(12u, 3u, runtime::ObjectKind::Array, 9u);
        requireCond(mv.rawTag() == runtime::ValueTag::ObjectRef && std::get<runtime::ObjectRef>(mv).slot == 12u, "in-place ObjectRef assignment preserved tag");
    }
}

void testReviewRegressions() {
    using namespace realscript;
    testCompact16BValueSlotRegressions();

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

        // Test math fast paths at -O0 (uninlined CallDirect -> Math* FlatOpcodes), -O1 (Basic), and -O2 (Aggressive)
        for (const auto optLevel : {
                 realscript::optimization::Level::None,
                 realscript::optimization::Level::Basic,
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

            // Edge-case & trap parity for Fixed.Sin/Cos/Sqrt, Math.Sqrt64, and Quat
            {
                realscript::runtime::ExecutionOptions traceSlowOpts = slowOpts;
                traceSlowOpts.trace = [](const realscript::runtime::TraceEvent&) {};

                const auto cosOvfFast = runtime.invoke(
                    "Bench.GameMath::trapCosOverflow",
                    {static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max())},
                    fastOpts);
                const auto cosOvfSlow = runtime.invoke(
                    "Bench.GameMath::trapCosOverflow",
                    {static_cast<std::int32_t>(std::numeric_limits<std::int32_t>::max())},
                    traceSlowOpts);
                requireCond(
                    !cosOvfFast.succeeded && !cosOvfSlow.succeeded &&
                        cosOvfFast.error.code == realscript::runtime::ErrorCode::IntegerOverflow &&
                        cosOvfFast.error.stackTrace == cosOvfSlow.error.stackTrace &&
                        cosOvfFast.instructionsExecuted == cosOvfSlow.instructionsExecuted,
                    "trapCosOverflow parity failed");

                const auto qMulOvfFast = runtime.invoke(
                    "Bench.GameMath::trapQuatMulOverflow", {std::int64_t{1000000}}, fastOpts);
                const auto qMulOvfSlow = runtime.invoke(
                    "Bench.GameMath::trapQuatMulOverflow", {std::int64_t{1000000}}, traceSlowOpts);
                requireCond(
                    !qMulOvfFast.succeeded && !qMulOvfSlow.succeeded &&
                        qMulOvfFast.error.code == realscript::runtime::ErrorCode::IntegerOverflow &&
                        qMulOvfFast.error.stackTrace == qMulOvfSlow.error.stackTrace &&
                        qMulOvfFast.instructionsExecuted == qMulOvfSlow.instructionsExecuted,
                    "trapQuatMulOverflow parity failed");

                const auto qRotOvfFast = runtime.invoke(
                    "Bench.GameMath::trapQuatRotateBoxed", {std::int64_t{1000000}}, fastOpts);
                const auto qRotOvfSlow = runtime.invoke(
                    "Bench.GameMath::trapQuatRotateBoxed", {std::int64_t{1000000}}, traceSlowOpts);
                requireCond(
                    !qRotOvfFast.succeeded && !qRotOvfSlow.succeeded &&
                        qRotOvfFast.error.code == realscript::runtime::ErrorCode::IntegerOverflow &&
                        qRotOvfFast.error.stackTrace == qRotOvfSlow.error.stackTrace &&
                        qRotOvfFast.instructionsExecuted == qRotOvfSlow.instructionsExecuted,
                    "trapQuatRotateBoxed overflow parity failed");

                const auto qRotOkFast = runtime.invoke(
                    "Bench.GameMath::trapQuatRotateBoxed", {std::int64_t{128}}, fastOpts);
                const auto qRotOkSlow = runtime.invoke(
                    "Bench.GameMath::trapQuatRotateBoxed", {std::int64_t{128}}, traceSlowOpts);
                requireCond(
                    qRotOkFast.succeeded && qRotOkSlow.succeeded &&
                        qRotOkFast.value == qRotOkSlow.value &&
                        qRotOkFast.instructionsExecuted == qRotOkSlow.instructionsExecuted &&
                        qRotOkFast.statistics.functionCalls == qRotOkSlow.statistics.functionCalls &&
                        qRotOkFast.statistics.maximumCallDepth == qRotOkSlow.statistics.maximumCallDepth,
                    "trapQuatRotateBoxed normal parity failed");

                // Sweep instruction budgets across SuperQuatRotateBoxed boundary
                for (std::uint64_t b : {std::uint64_t{5}, std::uint64_t{15}, qRotOkFast.instructionsExecuted - 1}) {
                    realscript::runtime::ExecutionOptions bFast = fastOpts;
                    bFast.limits.instructionBudget = b;
                    realscript::runtime::ExecutionOptions bSlow = traceSlowOpts;
                    bSlow.limits.instructionBudget = b;
                    const auto rF = runtime.invoke("Bench.GameMath::trapQuatRotateBoxed", {std::int64_t{128}}, bFast);
                    const auto rS = runtime.invoke("Bench.GameMath::trapQuatRotateBoxed", {std::int64_t{128}}, bSlow);
                    requireCond(
                        !rF.succeeded && !rS.succeeded &&
                            rF.error.code == realscript::runtime::ErrorCode::InstructionBudgetExceeded &&
                            rF.instructionsExecuted == rS.instructionsExecuted,
                        "SuperQuatRotateBoxed budget boundary parity failed");
                }

                const auto edgeFast = runtime.invoke("Bench.GameMath::edgeSqrtAndZeroQuatLerp", {}, fastOpts);
                const auto edgeSlow = runtime.invoke("Bench.GameMath::edgeSqrtAndZeroQuatLerp", {}, traceSlowOpts);
                requireCond(
                    edgeFast.succeeded && edgeSlow.succeeded &&
                        edgeFast.value == edgeSlow.value &&
                        edgeFast.instructionsExecuted == edgeSlow.instructionsExecuted &&
                        edgeFast.statistics.functionCalls == edgeSlow.statistics.functionCalls &&
                        edgeFast.statistics.branchesTaken == edgeSlow.statistics.branchesTaken &&
                        edgeFast.statistics.maximumCallDepth == edgeSlow.statistics.maximumCallDepth,
                    "edgeSqrtAndZeroQuatLerp parity failed");

                const auto edge2Fast = runtime.invoke("Bench.GameMath::edgeSinSqrt64AndSelfNorm", {}, fastOpts);
                const auto edge2Slow = runtime.invoke("Bench.GameMath::edgeSinSqrt64AndSelfNorm", {}, traceSlowOpts);
                requireCond(
                    edge2Fast.succeeded && edge2Slow.succeeded &&
                        edge2Fast.value == edge2Slow.value &&
                        edge2Fast.instructionsExecuted == edge2Slow.instructionsExecuted &&
                        edge2Fast.statistics.functionCalls == edge2Slow.statistics.functionCalls &&
                        edge2Fast.statistics.branchesTaken == edge2Slow.statistics.branchesTaken &&
                        edge2Fast.statistics.maximumCallDepth == edge2Slow.statistics.maximumCallDepth,
                    "edgeSinSqrt64AndSelfNorm parity failed");

                // Tight maximumHeapBytes = 256 bytes: builtin Quat/Vec3/Fixed methods must allocate 0 heap objects in BOTH fast & slow modes
                {
                    realscript::runtime::HeapConfig microHeapCfg;
                    microHeapCfg.maximumHeapBytes = 256;
                    auto microHeap = std::make_shared<realscript::runtime::ManagedHeap>(microHeapCfg);
                    realscript::runtime::Interpreter microInterp(program, microHeap);
                    const auto mFast = microInterp.invoke("Bench.GameMath::edgeSinSqrt64AndSelfNorm", {}, fastOpts);
                    const auto mSlow = microInterp.invoke("Bench.GameMath::edgeSinSqrt64AndSelfNorm", {}, traceSlowOpts);
                    requireCond(
                        mFast.succeeded && mSlow.succeeded &&
                            mFast.value == mSlow.value &&
                            microHeap->statistics().objectsAllocated == 0,
                        "tight heap zero-boxing parity failed");
                }
            }

            if (optLevel != realscript::optimization::Level::Aggressive) {
                (void)runBenchCase(runtime, "Bench.GameMath::benchFloatMath", 500, 1, 2);
                (void)runBenchCase(runtime, "Bench.GameMath::benchFixedMath", 500, 1, 2);
                (void)runBenchCase(runtime, "Bench.GameMath::benchVec3Math", 500, 1, 2);
                // Verify compound superinstruction overflow & single-instruction budget step fallback parity
                {
                    realscript::runtime::ExecutionOptions traceSlowOpts = slowOpts;
                    traceSlowOpts.trace = [](const realscript::runtime::TraceEvent&) {};
                    const auto cOvfF = runtime.invoke("Bench.GameMath::trapSuperCompoundOverflow", {std::int64_t{1500000000}}, fastOpts);
                    const auto cOvfS = runtime.invoke("Bench.GameMath::trapSuperCompoundOverflow", {std::int64_t{1500000000}}, traceSlowOpts);
                    requireCond(
                        !cOvfF.succeeded && !cOvfS.succeeded &&
                            cOvfF.error.code == realscript::runtime::ErrorCode::IntegerOverflow &&
                            cOvfF.error.stackTrace == cOvfS.error.stackTrace &&
                            cOvfF.instructionsExecuted == cOvfS.instructionsExecuted,
                        "trapSuperCompoundOverflow parity failed");

                    const auto chainOkF = runtime.invoke("Bench.GameMath::probeSuperChainBudget", {std::int64_t{128}}, fastOpts);
                    const auto chainOkS = runtime.invoke("Bench.GameMath::probeSuperChainBudget", {std::int64_t{128}}, traceSlowOpts);
                    requireCond(
                        chainOkF.succeeded && chainOkS.succeeded &&
                            chainOkF.value == chainOkS.value &&
                            chainOkF.instructionsExecuted == chainOkS.instructionsExecuted &&
                            chainOkF.statistics.functionCalls == chainOkS.statistics.functionCalls &&
                            chainOkF.statistics.branchesTaken == chainOkS.statistics.branchesTaken &&
                            chainOkF.statistics.maximumCallDepth == chainOkS.statistics.maximumCallDepth,
                        "probeSuperChainBudget normal parity failed");

                    for (std::uint64_t b : {
                             std::uint64_t{3},
                             std::uint64_t{25},
                             chainOkF.instructionsExecuted / 2,
                             chainOkF.instructionsExecuted - 1}) {
                        realscript::runtime::ExecutionOptions bFast = fastOpts;
                        bFast.limits.instructionBudget = b;
                        realscript::runtime::ExecutionOptions bSlow = traceSlowOpts;
                        bSlow.limits.instructionBudget = b;
                        const auto rF = runtime.invoke("Bench.GameMath::probeSuperChainBudget", {std::int64_t{128}}, bFast);
                        const auto rS = runtime.invoke("Bench.GameMath::probeSuperChainBudget", {std::int64_t{128}}, bSlow);
                        requireCond(
                            !rF.succeeded && !rS.succeeded &&
                                rF.error.code == realscript::runtime::ErrorCode::InstructionBudgetExceeded &&
                                rF.instructionsExecuted == rS.instructionsExecuted,
                            "probeSuperChainBudget budget boundary parity failed");
                    }
                }
                (void)runBenchCase(runtime, "Bench.GameMath::benchTrigSqrtMath", 200, 1, 2);
                (void)runBenchCase(runtime, "Bench.GameMath::benchQuatMath", 200, 1, 2);
                (void)runBenchCase(runtime, "Bench.GameMath::benchTransformRigMath", 200, 1, 2);
            } else {
                constexpr std::int64_t kSteps = 10000;
                const auto floatMetrics = runBenchCase(runtime, "Bench.GameMath::benchFloatMath", kSteps, 5, 15);
                const auto fixedMetrics = runBenchCase(runtime, "Bench.GameMath::benchFixedMath", kSteps, 5, 15);
                const auto vec3Metrics = runBenchCase(runtime, "Bench.GameMath::benchVec3Math", kSteps, 5, 15);
                const auto trigMetrics = runBenchCase(runtime, "Bench.GameMath::benchTrigSqrtMath", kSteps, 5, 15);
                const auto quatMetrics = runBenchCase(runtime, "Bench.GameMath::benchQuatMath", kSteps, 5, 15);
                const auto rigMetrics = runBenchCase(runtime, "Bench.GameMath::benchTransformRigMath", kSteps, 5, 15);

                std::cout << std::fixed << std::setprecision(2);
                std::cout << "=== RealScript Game Math FlatOpcode Benchmark (10,000 iterations, -O2) ===\n";
                for (const auto& m : {floatMetrics, fixedMetrics, vec3Metrics, trigMetrics, quatMetrics, rigMetrics}) {
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
