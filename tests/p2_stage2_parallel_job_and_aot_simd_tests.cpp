#include "realscript/aot_cpp/AotCpp.h"
#include "realscript/aot_cpp/AotRuntime.h"
#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/mir/Mir.h"
#include "realscript/optimization/Optimizer.h"
#include "realscript/runtime/ParallelJob.h"
#include "realscript/runtime/Runtime.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#undef assert
#define assert(expr)                                                                         \
    do {                                                                                     \
        if (!(expr)) {                                                                       \
            std::cerr << "Assertion failed: " << #expr << " (" << __FILE__ << ":"            \
                      << __LINE__ << ")\n";                                                  \
            std::abort();                                                                    \
        }                                                                                    \
    } while (false)

using namespace realscript;

namespace {

constexpr const char* kExternalStubSource = R"RS(
module Host.Parallel;

public int ImpureHostLog(int x) { return x; }
public int PureHostClamp(int x, int lo, int hi) { return x; }
public int ParallelRandomUInt32() { return 0; }
public int ParallelRandomInt(int minInclusive, int maxExclusive) { return minInclusive; }
public int ParallelRandomRange(int minInclusive, int maxInclusive) { return minInclusive; }
public int ParallelRandomByte255() { return 0; }
public double ParallelRandomDouble() { return 0.0; }
)RS";

std::shared_ptr<const runtime::ProgramImage> compileSourceToProgram(
    const std::string& name,
    const std::string& sourceText,
    optimization::Level optLevel = optimization::Level::Aggressive,
    bool includeExternalStub = true) {
    compiler::Compilation comp;
    std::set<std::string> stubModuleNames;
    if (includeExternalStub) {
        comp.addSource(compiler::SourceFile{"host_parallel_stub.rscript", kExternalStubSource});
        stubModuleNames.insert("Host.Parallel");
    }
    comp.addSource(compiler::SourceFile{name, sourceText});
    auto build = comp.build();
    if (build.diagnostics.hasErrors()) {
        for (const auto& d : build.diagnostics.items()) {
            std::cerr << "[CompileError] " << d.code << ": " << d.message << "\n";
        }
        assert(false && "Expected RealScript compilation to succeed");
    }

    // Strip stub modules BEFORE running Optimizer so inlining never replaces external calls!
    std::vector<mir::Module> userMir;
    for (auto& m : build.modules) {
        if (stubModuleNames.find(m.name) == stubModuleNames.end()) {
            userMir.push_back(std::move(m));
        }
    }

    optimization::Options opts;
    opts.level = optLevel;
    auto opt = optimization::Optimizer{}.optimize(std::move(userMir), build.diagnostics, opts);

    bytecode::Lowerer lowerer;
    std::vector<bytecode::Module> bc;
    for (const auto& m : opt.modules) {
        bc.push_back(lowerer.lower(m));
    }
    runtime::RuntimeError linkErr;
    auto linked = runtime::ProgramImage::link(std::move(bc), linkErr);
    if (!linked) {
        std::cerr << "[LinkError] " << linkErr.message << "\n";
    }
    assert(linked.has_value());
    return std::make_shared<const runtime::ProgramImage>(std::move(*linked));
}

std::vector<mir::Module> compileMirModules(
    const std::string& name,
    const std::string& sourceText,
    optimization::Level optLevel) {
    compiler::Compilation comp({{name, sourceText}});
    auto build = comp.build();
    if (build.diagnostics.hasErrors()) {
        for (const auto& d : build.diagnostics.items()) {
            std::cerr << "[CompileError] " << d.code << ": " << d.message << "\n";
        }
        assert(false && "Expected RealScript MIR compilation to succeed");
    }
    optimization::Options opts;
    opts.level = optLevel;
    auto opt = optimization::Optimizer{}.optimize(std::move(build.modules), build.diagnostics, opts);
    return std::move(opt.modules);
}

bool hasViolationKind(
    const runtime::ParallelJobVerificationReport& report,
    runtime::ParallelVerifyViolationKind expected) {
    for (const auto& v : report.violations) {
        if (v.kind == expected) return true;
    }
    return false;
}

void testStaticVerifierRejectionsAndAllowances() {
    std::cout << "[1/4] Testing verifyParallelJobFunction static bytecode verification...\n";

    const std::string src = R"RS(
module VerifySuite;
import RealScript.Math;
import RealScript.Async;
import Host.Parallel;

public class ConfigObj {
    public int Scale = 3;
    public int[] EmbeddedArr = new int[4];
}

public class StaticHolder {
    public static int GlobalFactor = 7;
}

public delegate int OpDel(int x);

void BadAllocHelper(int x) {
    ConfigObj tmp = new ConfigObj();
}

public void BadNewObjectTransitive(int index, int[] dst) {
    BadAllocHelper(index);
    dst[index] = index;
}

public void BadNewArray(int index, int[] dst) {
    int[] scratch = new int[2];
    dst[index] = scratch[0];
}

int SampleTarget(int x) { return x + 1; }

public void BadDelegate(int index, int[] dst) {
    OpDel d = SampleTarget;
    dst[index] = d(index);
}

public void BadStoreInstanceField(int index, ConfigObj cfg, int[] dst) {
    cfg.Scale = index;
    dst[index] = index;
}

void MutateStaticHelper(int v) {
    StaticHolder.GlobalFactor = v;
}

public void BadStoreStaticFieldTransitive(int index, int[] dst) {
    MutateStaticHelper(index);
    dst[index] = index;
}

public async Awaitable<int> BadAsyncKernel(int index) {
    return index;
}

public void BadStringConst(int index, int[] dst) {
    string s = "alloc";
    dst[index] = index;
}

public void BadNonPackedArrayParam(int index, string[] names, int[] dst) {
    dst[index] = names.length;
}

public void BadUntrackedFieldArrayWrite(int index, ConfigObj cfg) {
    cfg.EmbeddedArr[index] = 42;
}

public void BadImpureHost(int index, int[] dst) {
    dst[index] = ImpureHostLog(index);
}

int PureMathHelper(int x, int scale) {
    Fixed fx = Fixed.Of(x);
    Fixed fs = Fixed.Of(scale);
    Vec3 v = new Vec3(fx, fs, fx + fs);
    return (v.x + v.z).ToInt();
}

public void ValidElementKernel(
    int index,
    ConfigObj cfg,
    int[] srcX,
    double[] srcY,
    bool[] flags,
    int[] dstOut) {
    if (flags[index]) {
        int h = PureMathHelper(srcX[index], cfg.Scale + StaticHolder.GlobalFactor);
        int rnd = ParallelRandomInt(0, 100);
        int clamped = PureHostClamp(unchecked(h + rnd), -100000, 100000);
        dstOut[index] = unchecked(clamped + (int)srcY[index]);
    } else {
        dstOut[index] = 0;
    }
}

public int ValidRangeKernel(int start, int count, int[] src, int[] dst) {
    int end = start + count;
    int acc = 0;
    for (int i = start; i < end; i = i + 1) {
        int r = ParallelRandomInt(1, 10);
        int val = unchecked(src[i] * 3 + r);
        dst[i] = val;
        acc = unchecked(acc + val);
    }
    return acc;
}
)RS";

    auto program = compileSourceToProgram("verify_suite.rscript", src, optimization::Level::None);

    {
        auto r = runtime::verifyParallelJobFunction(*program, "VerifySuite::BadNewObjectTransitive");
        assert(!r.valid && hasViolationKind(r, runtime::ParallelVerifyViolationKind::HeapObjectAllocation));
    }
    {
        auto r = runtime::verifyParallelJobFunction(*program, "VerifySuite::BadNewArray");
        assert(!r.valid && hasViolationKind(r, runtime::ParallelVerifyViolationKind::HeapArrayAllocation));
    }
    {
        auto r = runtime::verifyParallelJobFunction(*program, "VerifySuite::BadDelegate");
        assert(!r.valid &&
               hasViolationKind(r, runtime::ParallelVerifyViolationKind::DelegateAllocationOrCombine) &&
               hasViolationKind(r, runtime::ParallelVerifyViolationKind::DelegateInvocation));
    }
    {
        auto r = runtime::verifyParallelJobFunction(*program, "VerifySuite::BadStoreInstanceField");
        assert(!r.valid && hasViolationKind(r, runtime::ParallelVerifyViolationKind::InstanceFieldMutation));
    }
    {
        auto r = runtime::verifyParallelJobFunction(*program, "VerifySuite::BadStoreStaticFieldTransitive");
        assert(!r.valid && hasViolationKind(r, runtime::ParallelVerifyViolationKind::StaticFieldMutation));
    }
    {
        auto r = runtime::verifyParallelJobFunction(*program, "VerifySuite::BadAsyncKernel");
        assert(!r.valid && hasViolationKind(r, runtime::ParallelVerifyViolationKind::AsyncOrAwaitOperation));
    }
    {
        auto r = runtime::verifyParallelJobFunction(*program, "VerifySuite::BadStringConst");
        assert(!r.valid && hasViolationKind(r, runtime::ParallelVerifyViolationKind::StringAllocationOrMutation));
    }
    {
        auto r = runtime::verifyParallelJobFunction(*program, "VerifySuite::BadNonPackedArrayParam");
        assert(!r.valid && hasViolationKind(r, runtime::ParallelVerifyViolationKind::NonPackedArrayParameter));
    }
    {
        auto r = runtime::verifyParallelJobFunction(*program, "VerifySuite::BadUntrackedFieldArrayWrite");
        assert(!r.valid && hasViolationKind(r, runtime::ParallelVerifyViolationKind::UntrackedArrayMutation));
    }
    {
        auto r = runtime::verifyParallelJobFunction(*program, "VerifySuite::BadImpureHost");
        assert(!r.valid && hasViolationKind(r, runtime::ParallelVerifyViolationKind::ImpureExternalHostCall));
    }

    // Valid element kernel with whitelisted Deterministic external host function
    runtime::BindingRegistry bindings;
    bindings.bind(
        "Host.Parallel::PureHostClamp",
        [](const bytecode::FunctionReference&, runtime::ValueSpan args, runtime::RuntimeError&)
            -> std::optional<runtime::Value> {
            const auto v = std::get<std::int64_t>(args[0]);
            const auto lo = std::get<std::int64_t>(args[1]);
            const auto hi = std::get<std::int64_t>(args[2]);
            return runtime::Value{std::max(lo, std::min(hi, v))};
        },
        runtime::BindingDeterminism::Deterministic);

    runtime::ParallelJobVerifyOptions vOpts;
    vOpts.expectedKind = runtime::ParallelKernelKind::Element;
    vOpts.bindings = &bindings;
    vOpts.pureExternalNames.insert("PureHostClamp");

    auto validElem = runtime::verifyParallelJobFunction(
        *program, "VerifySuite::ValidElementKernel", vOpts);
    if (!validElem.valid) {
        std::cerr << validElem.summaryMessage() << "\n";
    }
    assert(validElem.valid);
    assert(validElem.supportsElementKernel);
    assert(validElem.usesParallelPrng);
    assert(validElem.referencedStaticFieldIds.size() == 1u);
    assert((validElem.readArrayParamIndices == std::vector<std::size_t>{2u, 3u, 4u}));
    assert((validElem.writtenArrayParamIndices == std::vector<std::size_t>{5u}));

    runtime::ParallelJobVerifyOptions rangeOpts;
    rangeOpts.expectedKind = runtime::ParallelKernelKind::Range;
    auto validRange = runtime::verifyParallelJobFunction(
        *program, "VerifySuite::ValidRangeKernel", rangeOpts);
    assert(validRange.valid);
    assert(validRange.supportsRangeKernel);
    assert((validRange.readArrayParamIndices == std::vector<std::size_t>{2u}));
    assert((validRange.writtenArrayParamIndices == std::vector<std::size_t>{3u}));
}

void testParallelSchedulerAliasingAndLivenessChecks() {
    std::cout << "[2/4] Testing ParallelJobScheduler pre-dispatch liveness & aliasing checks...\n";

    const std::string src = R"RS(
module AliasSuite;

public void CopyScale(int index, int[] src, int[] dst) {
    dst[index] = unchecked(src[index] * 2 + 1);
}

public void InPlaceAddOne(int start, int count, int[] data) {
    int end = start + count;
    for (int i = start; i < end; i = i + 1) {
        data[i] = unchecked(data[i] + 1);
    }
}
)RS";

    auto program = compileSourceToProgram("alias_suite.rscript", src);
    auto heap = std::make_shared<runtime::ManagedHeap>();

    const auto intArrTid = semantic::stableTypeId("int[]");
    auto arrA = heap->allocateTypedArray(intArrTid, semantic::PrimitiveType::Int, 0, 128, std::int64_t{5});
    auto arrB = heap->allocateTypedArray(intArrTid, semantic::PrimitiveType::Int, 0, 128, std::int64_t{0});
    assert(arrA.has_value() && arrB.has_value());
    auto rootA = heap->retain(*arrA);
    auto rootB = heap->retain(*arrB);

    runtime::ParallelJobScheduler scheduler(4);
    runtime::ParallelJobConfig cfg;
    cfg.batchSize = 16;
    cfg.executionOptions.determinism.mode = runtime::DeterminismMode::Strict;

    // 1. Rejects aliased src == dst ObjectRef
    {
        auto aliased = scheduler.dispatchFor(
            program,
            heap,
            "AliasSuite::CopyScale",
            128,
            {runtime::Value{*arrA}, runtime::Value{*arrA}},
            cfg);
        assert(!aliased.succeeded);
        assert(aliased.error.code == runtime::ErrorCode::InvalidArguments);
    }

    // 2. Rejects NullArray
    {
        auto nullRes = scheduler.dispatchFor(
            program,
            heap,
            "AliasSuite::CopyScale",
            128,
            {runtime::Value{runtime::NullArray{}}, runtime::Value{*arrB}},
            cfg);
        assert(!nullRes.succeeded);
        assert(nullRes.error.code == runtime::ErrorCode::NullReference);
    }

    // 3. Rejects dead/collected ObjectRef
    {
        auto tempArr = heap->allocateTypedArray(intArrTid, semantic::PrimitiveType::Int, 0, 16, std::int64_t{1});
        assert(tempArr.has_value());
        heap->collect();
        assert(!heap->isAlive(*tempArr));
        auto deadRes = scheduler.dispatchFor(
            program,
            heap,
            "AliasSuite::CopyScale",
            16,
            {runtime::Value{*tempArr}, runtime::Value{*arrB}},
            cfg);
        assert(!deadRes.succeeded);
        assert(deadRes.error.code == runtime::ErrorCode::InvalidObjectReference);
    }

    // 4. Single-array disjoint shard in-place update allowed by default, rejected when disabled
    {
        auto okInPlace = scheduler.dispatchRange(
            program,
            heap,
            "AliasSuite::InPlaceAddOne",
            128,
            {runtime::Value{*arrA}},
            cfg);
        assert(okInPlace.succeeded);
        std::vector<std::int64_t> vals;
        assert(heap->readPackedIntArray(*arrA, vals));
        for (const auto v : vals) assert(v == 6);

        runtime::ParallelJobConfig disallowInPlaceCfg = cfg;
        disallowInPlaceCfg.allowInPlaceSingleArrayReadWrite = false;
        auto rejectedInPlace = scheduler.dispatchRange(
            program,
            heap,
            "AliasSuite::InPlaceAddOne",
            128,
            {runtime::Value{*arrA}},
            disallowInPlaceCfg);
        assert(!rejectedInPlace.succeeded);
        assert(rejectedInPlace.error.code == runtime::ErrorCode::InvalidArguments);
    }
}

void testMultiCoreStrictDeterminismParity() {
    std::cout << "[3/4] Testing 1 / 4 / 8 worker threads Strict Determinism bit-identical parity...\n";

    const std::string src = R"RS(
module DetSuite;
import RealScript.Math;
import Host.Parallel;

public class SimParams {
    public int BaseGain = 5;
    public double Damping = 0.875;
}

int MixStep(int v, int gain, int noise) {
    Fixed fv = Fixed.Of(v);
    Fixed fg = Fixed.Of(gain);
    return unchecked((fv * fg).ToInt() + noise);
}

public int KernelElementStochastic(
    int index,
    SimParams simCfg,
    int[] inputA,
    double[] inputB,
    int[] outInts,
    double[] outDoubles) {
    int noise = ParallelRandomInt(-50, 51);
    double jitter = ParallelRandomDouble();
    int mixed = MixStep(inputA[index], simCfg.BaseGain, noise);
    outInts[index] = unchecked(mixed + index);
    double dval = inputB[index] * simCfg.Damping + jitter * 0.25 + (double)(index % 7);
    outDoubles[index] = dval;
    return outInts[index];
}

public double KernelRangeEnergy(
    int start,
    int count,
    SimParams simCfg,
    int[] inputA,
    double[] inputB,
    double[] outEnergy) {
    int end = start + count;
    double chunkSum = 0.0;
    for (int i = start; i < end; i = i + 1) {
        double r = ParallelRandomDouble();
        double e = (double)inputA[i] * simCfg.Damping + inputB[i] * (1.0 + r * 0.125);
        outEnergy[i] = e;
        chunkSum = chunkSum + e;
    }
    return chunkSum;
}

public Fixed KernelRangeFixedMax(
    int start,
    int count,
    int[] inputA) {
    int end = start + count;
    Fixed best = Fixed.Of(-100000);
    for (int i = start; i < end; i = i + 1) {
        int r = ParallelRandomInt(0, 25);
        Fixed cur = Fixed.Of(inputA[i] + r);
        if (cur > best) {
            best = cur;
        }
    }
    return best;
}
)RS";

    auto program = compileSourceToProgram("det_suite.rscript", src, optimization::Level::Aggressive);

    struct RunSnapshot {
        std::vector<std::int64_t> outInts;
        std::vector<double> outDoubles;
        std::vector<double> outEnergy;
        runtime::Value elemSumValue{};
        runtime::RuntimeStatistics elemStats{};
        std::uint64_t elemDigest = 0;
        std::vector<std::uint64_t> elemBatchDigests;

        runtime::Value rangeSumValue{};
        runtime::RuntimeStatistics rangeStats{};
        std::uint64_t rangeDigest = 0;
        std::vector<std::uint64_t> rangeBatchDigests;

        runtime::Value fixedMaxValue{};
        std::uint64_t fixedMaxDigest = 0;
    };

    constexpr std::size_t kCount = 1003; // Intentionally non-multiple of batchSize (37)
    constexpr std::size_t kBatchSize = 37;
    constexpr std::uint32_t kSeed = 0xC0FFEE42u;

    auto executeWithWorkers = [&](std::size_t workers) -> RunSnapshot {
        auto heap = std::make_shared<runtime::ManagedHeap>();

        const auto paramsTid = semantic::stableTypeId("DetSuite::SimParams");
        auto paramsObj = heap->allocateObject(
            paramsTid,
            {runtime::Value{std::int64_t{5}}, runtime::Value{0.875}},
            {});
        assert(paramsObj.has_value());

        const auto intArrTid = semantic::stableTypeId("int[]");
        const auto dblArrTid = semantic::stableTypeId("double[]");

        auto inA = heap->allocateTypedArray(intArrTid, semantic::PrimitiveType::Int, 0, kCount, std::int64_t{0});
        auto inB = heap->allocateTypedArray(dblArrTid, semantic::PrimitiveType::Double, 0, kCount, 0.0);
        auto outInts = heap->allocateTypedArray(intArrTid, semantic::PrimitiveType::Int, 0, kCount, std::int64_t{0});
        auto outDoubles = heap->allocateTypedArray(dblArrTid, semantic::PrimitiveType::Double, 0, kCount, 0.0);
        auto outEnergy = heap->allocateTypedArray(dblArrTid, semantic::PrimitiveType::Double, 0, kCount, 0.0);
        assert(inA && inB && outInts && outDoubles && outEnergy);

        std::vector<std::int64_t> initA(kCount);
        std::vector<double> initB(kCount);
        for (std::size_t i = 0; i < kCount; ++i) {
            initA[i] = static_cast<std::int64_t>((static_cast<int>(i) * 13) % 97 - 48);
            initB[i] = static_cast<double>(i) * 0.375 - 12.5;
        }
        assert(heap->writePackedIntArray(*inA, initA.data(), kCount));
        assert(heap->writePackedDoubleArray(*inB, initB.data(), kCount));

        const std::size_t objectsBefore = heap->liveObjects();
        const std::size_t collectionsBefore = heap->statistics().collectionsCompleted;

        runtime::ParallelJobScheduler scheduler(workers);

        runtime::ParallelJobConfig elemCfg;
        elemCfg.workerCount = workers;
        elemCfg.batchSize = kBatchSize;
        elemCfg.baseSeed = kSeed;
        elemCfg.reduceOp = runtime::ParallelReduceOp::Sum;
        elemCfg.executionOptions.determinism.mode = runtime::DeterminismMode::Strict;

        auto elemRes = scheduler.dispatchFor(
            program,
            heap,
            "DetSuite::KernelElementStochastic",
            kCount,
            {
                runtime::Value{*paramsObj},
                runtime::Value{*inA},
                runtime::Value{*inB},
                runtime::Value{*outInts},
                runtime::Value{*outDoubles},
            },
            elemCfg);
        if (!elemRes.succeeded) {
            std::cerr << "Element job failed: " << elemRes.error.message << "\n";
        }
        assert(elemRes.succeeded);
        assert(elemRes.statistics.gcWorkPerformed == 0u);

        runtime::ParallelJobConfig rangeCfg = elemCfg;
        rangeCfg.reduceOp = runtime::ParallelReduceOp::Sum;
        auto rangeRes = scheduler.dispatchRange(
            program,
            heap,
            "DetSuite::KernelRangeEnergy",
            kCount,
            {
                runtime::Value{*paramsObj},
                runtime::Value{*inA},
                runtime::Value{*inB},
                runtime::Value{*outEnergy},
            },
            rangeCfg);
        if (!rangeRes.succeeded) {
            std::cerr << "Range job failed: " << rangeRes.error.message << "\n";
        }
        assert(rangeRes.succeeded);
        assert(rangeRes.statistics.gcWorkPerformed == 0u);

        runtime::ParallelJobConfig maxCfg = elemCfg;
        maxCfg.reduceOp = runtime::ParallelReduceOp::Max;
        auto maxRes = scheduler.dispatchRange(
            program,
            heap,
            "DetSuite::KernelRangeFixedMax",
            kCount,
            {runtime::Value{*inA}},
            maxCfg);
        assert(maxRes.succeeded);

        // Zero heap allocations and zero GC collections occurred!
        assert(heap->liveObjects() == objectsBefore);
        assert(heap->statistics().collectionsCompleted == collectionsBefore);

        RunSnapshot snap;
        assert(heap->readPackedIntArray(*outInts, snap.outInts));
        assert(heap->readPackedDoubleArray(*outDoubles, snap.outDoubles));
        assert(heap->readPackedDoubleArray(*outEnergy, snap.outEnergy));
        snap.elemSumValue = elemRes.value;
        snap.elemStats = elemRes.statistics;
        snap.elemDigest = elemRes.determinismDigest;
        snap.elemBatchDigests = elemRes.batchDigests;

        snap.rangeSumValue = rangeRes.value;
        snap.rangeStats = rangeRes.statistics;
        snap.rangeDigest = rangeRes.determinismDigest;
        snap.rangeBatchDigests = rangeRes.batchDigests;

        snap.fixedMaxValue = maxRes.value;
        snap.fixedMaxDigest = maxRes.determinismDigest;
        return snap;
    };

    const RunSnapshot ref1 = executeWithWorkers(1);

    for (const std::size_t w : {std::size_t{4}, std::size_t{8}}) {
        for (int repeat = 0; repeat < 8; ++repeat) {
            const RunSnapshot cur = executeWithWorkers(w);
            assert(cur.outInts == ref1.outInts);
            assert(cur.outDoubles.size() == ref1.outDoubles.size());
            assert(std::memcmp(
                       cur.outDoubles.data(),
                       ref1.outDoubles.data(),
                       cur.outDoubles.size() * sizeof(double)) == 0);
            assert(std::memcmp(
                       cur.outEnergy.data(),
                       ref1.outEnergy.data(),
                       cur.outEnergy.size() * sizeof(double)) == 0);

            assert(cur.elemSumValue == ref1.elemSumValue);
            assert(cur.elemStats.instructionsExecuted == ref1.elemStats.instructionsExecuted);
            assert(cur.elemStats.functionCalls == ref1.elemStats.functionCalls);
            assert(cur.elemStats.externalCalls == ref1.elemStats.externalCalls);
            assert(cur.elemStats.branchesTaken == ref1.elemStats.branchesTaken);
            assert(cur.elemStats.maximumCallDepth == ref1.elemStats.maximumCallDepth);
            assert(cur.elemStats.gcWorkPerformed == ref1.elemStats.gcWorkPerformed);
            assert(cur.elemBatchDigests == ref1.elemBatchDigests);
            assert(cur.elemDigest == ref1.elemDigest);

            assert(std::holds_alternative<double>(cur.rangeSumValue));
            const double dCur = std::get<double>(cur.rangeSumValue);
            const double dRef = std::get<double>(ref1.rangeSumValue);
            assert(std::memcmp(&dCur, &dRef, sizeof(double)) == 0);
            assert(cur.rangeStats.instructionsExecuted == ref1.rangeStats.instructionsExecuted);
            assert(cur.rangeStats.functionCalls == ref1.rangeStats.functionCalls);
            assert(cur.rangeStats.externalCalls == ref1.rangeStats.externalCalls);
            assert(cur.rangeStats.branchesTaken == ref1.rangeStats.branchesTaken);
            assert(cur.rangeBatchDigests == ref1.rangeBatchDigests);
            assert(cur.rangeDigest == ref1.rangeDigest);

            assert(cur.fixedMaxValue == ref1.fixedMaxValue);
            assert(cur.fixedMaxDigest == ref1.fixedMaxDigest);
        }
    }

    std::cout << "  -> Verified 100% bit-identical arrays, reductions, stats & digests across 1, 4, 8 threads! "
              << "(elemDigest=0x" << std::hex << ref1.elemDigest
              << ", rangeDigest=0x" << ref1.rangeDigest << std::dec << ")\n";
}

void testAotPackedFastPathAndRestrictHelpers() {
    std::cout << "[4/4] Testing AotRuntime tryBindPackedInt32Array / tryBindPackedDoubleArray & __restrict batch helpers...\n";

    const std::string src = R"RS(
module AotSimdSuite;

public void SaxpyIntRange(int start, int count, int alpha, int[] x, int[] y, int[] dst) {
    int end = start + count;
    for (int i = start; i < end; i = i + 1) {
        dst[i] = unchecked(alpha * x[i] + y[i]);
    }
}

public void ScaleBiasDoubleFull(double scale, double bias, double[] data) {
    for (int i = 0; i < data.length; i = i + 1) {
        data[i] = data[i] * scale + bias;
    }
}

public double FmaDoubleRange(int start, int count, double alpha, double[] x, double[] y, double[] dst) {
    int end = start + count;
    double acc = 0.0;
    for (int i = start; i < end; i = i + 1) {
        double v = alpha * x[i] + y[i];
        dst[i] = v;
        acc = acc + v;
    }
    return acc;
}

public int SingleElemSwapAdd(int idx, int[] arr, double[] darr) {
    int prev = arr[idx];
    arr[idx] = unchecked(prev + (int)darr[idx]);
    darr[idx] = darr[idx] * 2.0;
    return arr.length + darr.length + prev;
}
)RS";

    auto mirO2 = compileMirModules("aot_simd_suite.rscript", src, optimization::Level::Aggressive);
    auto mirO0 = compileMirModules("aot_simd_suite_o0.rscript", src, optimization::Level::None);

    diagnostics::DiagnosticBag diagsO2;
    aot::GenerationOptions genOpts;
    genOpts.programName = "P2cSimdProg";
    genOpts.cppNamespace = "generated_p2c_simd";
    genOpts.emitLineDirectives = false;
    auto genO2 = aot::CppGenerator{}.generate(mirO2, diagsO2, genOpts);
    assert(!diagsO2.hasErrors());

    diagnostics::DiagnosticBag diagsO0;
    aot::GenerationOptions genOptsO0 = genOpts;
    genOptsO0.cppNamespace = "generated_p2c_simd_o0";
    auto genO0 = aot::CppGenerator{}.generate(mirO0, diagsO0, genOptsO0);
    assert(!diagsO0.hasErrors());

    assert(genO2.source.find("RS_AOT_RESTRICT") != std::string::npos);
    assert(genO2.source.find("RS_AOT_PRAGMA_SIMD_LOOP") != std::string::npos);
    assert(genO2.source.find("acquirePackedInt32ArrayCache") != std::string::npos);
    assert(genO2.source.find("acquirePackedDoubleArrayCache") != std::string::npos);
    assert(genO2.source.find("loadElementInt32Fast") != std::string::npos);
    assert(genO2.source.find("storeElementInt32Fast") != std::string::npos);
    assert(genO2.source.find("loadElementDoubleFast") != std::string::npos);
    assert(genO2.source.find("storeElementDoubleFast") != std::string::npos);

    assert(genO0.source.find("RS_AOT_RESTRICT") != std::string::npos);
    assert(genO0.source.find("RS_AOT_PRAGMA_SIMD_LOOP") != std::string::npos);

    // Direct AotRuntime ExecutionContext helpers & __restrict batch SIMD functions
    auto heap = std::make_shared<runtime::ManagedHeap>();
    const auto intArrTid = semantic::stableTypeId("int[]");
    const auto dblArrTid = semantic::stableTypeId("double[]");
    auto iArrX = heap->allocateTypedArray(intArrTid, semantic::PrimitiveType::Int, 0, 64, std::int64_t{3});
    auto iArrY = heap->allocateTypedArray(intArrTid, semantic::PrimitiveType::Int, 0, 64, std::int64_t{10});
    auto iArrDst = heap->allocateTypedArray(intArrTid, semantic::PrimitiveType::Int, 0, 64, std::int64_t{0});
    auto dArrX = heap->allocateTypedArray(dblArrTid, semantic::PrimitiveType::Double, 0, 64, 2.5);
    auto dArrY = heap->allocateTypedArray(dblArrTid, semantic::PrimitiveType::Double, 0, 64, 1.25);
    auto dArrDst = heap->allocateTypedArray(dblArrTid, semantic::PrimitiveType::Double, 0, 64, 0.0);
    assert(iArrX && iArrY && iArrDst && dArrX && dArrY && dArrDst);

    static constexpr const char* kDummyMods[] = {"Dummy"};
    static constexpr aot::ProgramDescriptor kEmptyProg{
        aot::RuntimeAbiMajor, aot::RuntimeAbiMinor, "Dummy", 1, kDummyMods, 1, nullptr, 0, nullptr, 0, nullptr, 0
    };
    aot::ExecutionContext ctx(kEmptyProg, heap, nullptr);

    std::int32_t* xI32 = nullptr;
    std::int32_t* yI32 = nullptr;
    std::int32_t* dstI32 = nullptr;
    std::uint32_t lenXI = 0, lenYI = 0, lenDstI = 0;
    assert(ctx.tryBindPackedInt32Array(runtime::Value{*iArrX}, xI32, lenXI) && lenXI == 64u);
    assert(ctx.tryBindPackedInt32Array(runtime::Value{*iArrY}, yI32, lenYI) && lenYI == 64u);
    assert(ctx.tryBindPackedInt32Array(runtime::Value{*iArrDst}, dstI32, lenDstI) && lenDstI == 64u);

    double* wrongDbl = nullptr;
    std::uint32_t wrongLen = 0;
    assert(!ctx.tryBindPackedDoubleArray(runtime::Value{*iArrX}, wrongDbl, wrongLen));

    assert(aot::packedBuffersDisjoint(dstI32, 64 * sizeof(std::int32_t), xI32, 64 * sizeof(std::int32_t)));
    aot::simdFmaPackedInt32(dstI32, xI32, yI32, 64, 4);
    for (std::size_t i = 0; i < 64; ++i) {
        assert(dstI32[i] == 22);
    }

    double* xF64 = nullptr;
    double* yF64 = nullptr;
    double* dstF64 = nullptr;
    std::uint32_t lenXD = 0, lenYD = 0, lenDstD = 0;
    assert(ctx.tryBindPackedDoubleArray(runtime::Value{*dArrX}, xF64, lenXD) && lenXD == 64u);
    assert(ctx.tryBindPackedDoubleArray(runtime::Value{*dArrY}, yF64, lenYD) && lenYD == 64u);
    assert(ctx.tryBindPackedDoubleArray(runtime::Value{*dArrDst}, dstF64, lenDstD) && lenDstD == 64u);
    aot::simdFmaPackedDouble(dstF64, xF64, yF64, 64, 3.0);
    for (std::size_t i = 0; i < 64; ++i) {
        assert(dstF64[i] == 8.75);
    }
}

} // namespace

int main() {
    testStaticVerifierRejectionsAndAllowances();
    testParallelSchedulerAliasingAndLivenessChecks();
    testMultiCoreStrictDeterminismParity();
    testAotPackedFastPathAndRestrictHelpers();
    std::cout << "ALL P2-C CORE VERIFICATION TESTS PASSED!\n";
    return 0;
}
