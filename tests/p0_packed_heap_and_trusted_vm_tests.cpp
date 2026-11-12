#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/optimization/Optimizer.h"
#include "realscript/runtime/Runtime.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace realscript;

namespace {

void require(bool cond, const std::string& msg) {
    if (!cond) {
        throw std::runtime_error("P0 test failure: " + msg);
    }
}

std::shared_ptr<const runtime::ProgramImage> compileAndLink(
    const std::string& name,
    const std::string& src,
    optimization::Level optLevel = optimization::Level::Aggressive) {
    compiler::Compilation comp({{name, src}});
    auto build = comp.build();
    if (build.diagnostics.hasErrors()) {
        std::string err = "compile failed:\n";
        for (const auto& d : build.diagnostics.items()) {
            err += d.code + ": " + d.message + "\n";
        }
        throw std::runtime_error(err);
    }
    optimization::Options opts;
    opts.level = optLevel;
    auto opt = optimization::Optimizer{}.optimize(std::move(build.modules), build.diagnostics, opts);
    bytecode::Lowerer lowerer;
    std::vector<bytecode::Module> bc;
    for (const auto& m : opt.modules) {
        bc.push_back(lowerer.lower(m));
    }
    runtime::RuntimeError linkErr;
    auto linked = runtime::ProgramImage::link(std::move(bc), linkErr);
    require(linked.has_value(), "ProgramImage::link failed: " + linkErr.message);
    return std::make_shared<const runtime::ProgramImage>(std::move(*linked));
}

// ============================================================================
// 1. P0-A: Unboxed Packed Array Memory Footprint, Widening, GC & Snapshot Tests
// ============================================================================
void testUnboxedPackedHeapStorageAndGc() {
    std::cout << "[P0-A] Testing ManagedHeap unboxed packed arrays, widening, GC & snapshots...\n";
    static_assert(sizeof(runtime::Value) == 16, "Value ABI must remain 16 bytes");

    runtime::ManagedHeap heap;
    runtime::RuntimeError err;
    const auto zeroArr = heap.allocateTypedArray(
        0, semantic::PrimitiveType::Byte, 0, 0, runtime::Value{runtime::ByteValue{0}}, &err);
    require(zeroArr.has_value() && heap.objectSizeBytes(*zeroArr).has_value(), "zeroArr failed");
    const std::size_t hdrBytes = *heap.objectSizeBytes(*zeroArr);

    // 1. Verify packed vs boxed classification & exact memory footprint
    const auto byteArr = heap.allocateTypedArray(
        0, semantic::PrimitiveType::Byte, 0, 1024, runtime::Value{runtime::ByteValue{7}}, &err);
    require(byteArr.has_value(), "byte[1024] allocation failed");
    require(heap.isPackedArray(*byteArr), "byte[] must use PackedB8 storage");
    require(heap.objectSizeBytes(*byteArr) == hdrBytes + 1024u,
        "byte[1024] must occupy 1024 bytes (16x reduction vs 16384B boxed)");

    const auto shortArr = heap.allocateTypedArray(
        0, semantic::PrimitiveType::Short, 0, 1024, runtime::Value{runtime::ShortValue{-12}}, &err);
    require(shortArr.has_value() && heap.isPackedArray(*shortArr), "short[] must use PackedB16 storage");
    require(heap.objectSizeBytes(*shortArr) == hdrBytes + 2048u, "short[1024] must occupy 2048 bytes (8x reduction)");

    const auto intArr = heap.allocateTypedArray(
        0, semantic::PrimitiveType::Int, 0, 1024, runtime::Value{std::int64_t{42}}, &err);
    require(intArr.has_value() && heap.isPackedArray(*intArr), "int[] must use PackedI32 storage");
    require(heap.objectSizeBytes(*intArr) == hdrBytes + 4096u, "int[1024] must occupy 4096 bytes (4x reduction)");

    const auto floatArr = heap.allocateTypedArray(
        0, semantic::PrimitiveType::Float, 0, 1024, runtime::Value{runtime::FloatValue{1.5f}}, &err);
    require(floatArr.has_value() && heap.isPackedArray(*floatArr), "float[] must use PackedI32 storage");
    require(heap.objectSizeBytes(*floatArr) == hdrBytes + 4096u, "float[1024] must occupy 4096 bytes");

    const auto dblArr = heap.allocateTypedArray(
        0, semantic::PrimitiveType::Double, 0, 1024, runtime::Value{3.25}, &err);
    require(dblArr.has_value() && heap.isPackedArray(*dblArr), "double[] must use PackedI64 storage");
    require(heap.objectSizeBytes(*dblArr) == hdrBytes + 8192u, "double[1024] must occupy 8192 bytes (2x reduction)");

    const semantic::SymbolId enumTid = semantic::stableTypeId("Game::Rank");
    const auto enumArr = heap.allocateTypedArray(
        0, semantic::PrimitiveType::Enum, enumTid, 512, runtime::Value{runtime::EnumValue{enumTid, 3}}, &err);
    require(enumArr.has_value() && heap.isPackedArray(*enumArr), "enum[] must use PackedI32 EnumInline storage");
    require(heap.objectSizeBytes(*enumArr) == hdrBytes + 2048u, "enum[512] must occupy 2048 bytes");

    const auto objArr = heap.allocateTypedArray(
        0, semantic::PrimitiveType::Object, 123, 64, runtime::Value{runtime::NullObject{}}, &err);
    require(objArr.has_value() && !heap.isPackedArray(*objArr), "Object[] must remain Boxed for GC scanning");

    // 2. Verify PackedArrayFastView direct pointer read/write
    std::int32_t* rawIntPtr = nullptr;
    std::uint32_t rawIntLen = 0;
    require(heap.tryBindPackedInt32Array(*intArr, rawIntPtr, rawIntLen) && rawIntLen == 1024u,
        "tryBindPackedInt32Array failed");
    require((reinterpret_cast<std::uintptr_t>(rawIntPtr) & 15u) == 0u,
        "PackedByteBufferPool buffers must be 16-byte aligned for SIMD");
    rawIntPtr[0] = -999999;
    rawIntPtr[1023] = 777777;
    require(std::get<std::int64_t>(*heap.arrayGet(*intArr, 0)) == -999999, "direct packed int32 write [0] mismatch");
    require(std::get<std::int64_t>(*heap.arrayGet(*intArr, 1023)) == 777777, "direct packed int32 write [1023] mismatch");

    // 3. Verify lossless automatic widening from PackedI32 -> PackedI64 when host writes >32-bit int64_t
    const std::int64_t bigHostInt = (std::int64_t{1} << 42) + 12345;
    require(heap.arraySet(*intArr, 500, runtime::Value{bigHostInt}, &err),
        "host 64-bit int64_t store into int[] failed: " + err.message);
    require(heap.isPackedArray(*intArr) && heap.objectSizeBytes(*intArr) == hdrBytes + 8192u,
        "int[1024] should transparently widen to PackedI64 (8192B) upon >32-bit host store");
    require(std::get<std::int64_t>(*heap.arrayGet(*intArr, 500)) == bigHostInt,
        "widened int64_t element [500] did not round-trip losslessly");
    require(std::get<std::int64_t>(*heap.arrayGet(*intArr, 0)) == -999999 &&
            std::get<std::int64_t>(*heap.arrayGet(*intArr, 1023)) == 777777,
        "existing elements were not preserved across PackedI32 -> PackedI64 widening");

    // 4. Verify Snapshot & Restore roundtrip across all packed array widths
    auto r1 = heap.retain(*byteArr);
    auto r2 = heap.retain(*shortArr);
    auto r3 = heap.retain(*intArr);
    auto r4 = heap.retain(*floatArr);
    auto r5 = heap.retain(*dblArr);
    auto r6 = heap.retain(*enumArr);
    (void)r1; (void)r2; (void)r3; (void)r4; (void)r5; (void)r6;

    const auto snap = heap.snapshot();
    // Mutate elements after snapshot, then rollback via heap.restore(snap)
    require(heap.arraySet(*intArr, 500, runtime::Value{std::int64_t{0}}, &err), "post-snap mutate failed");
    require(heap.arraySet(*enumArr, 10, runtime::Value{runtime::EnumValue{enumTid, 99}}, &err), "post-snap enum mutate failed");
    require(heap.restore(snap, &err), "ManagedHeap::restore failed on packed arrays: " + err.message);
    require(heap.isPackedArray(*byteArr) && heap.isPackedArray(*intArr) &&
            heap.isPackedArray(*dblArr) && heap.isPackedArray(*enumArr),
        "restored arrays must preserve unboxed packed storage");
    require(std::get<std::int64_t>(*heap.arrayGet(*intArr, 500)) == bigHostInt,
        "restored widened int[] lost 64-bit value");
    runtime::EnumValue restoredEnum{};
    require(heap.arrayGet(*enumArr, 10)->tryGetEnum(restoredEnum) &&
            restoredEnum.typeId == enumTid && restoredEnum.value == 3,
        "restored enum[] element mismatch");

    // 5. Verify PackedByteBufferPool recycling under GC churn without memory leaks
    runtime::ManagedHeap churnHeap;
    for (int cycle = 0; cycle < 20; ++cycle) {
        for (int k = 0; k < 64; ++k) {
            auto tmp = churnHeap.allocateTypedArray(
                0, semantic::PrimitiveType::Int, 0, 256, runtime::Value{std::int64_t{cycle + k}}, &err);
            require(tmp.has_value(), "churn allocation failed");
        }
        churnHeap.collect();
    }
    require(churnHeap.liveObjects() == 0 && churnHeap.liveBytes() == 0,
        "all unreachable packed arrays must be swept cleanly by GC");
}

// ============================================================================
// 2. P0-B: Trusted Fast Path Semantics, Exact Instruction Budget & Overflow
// ============================================================================
void testTrustedFastPathSemanticsAndDeterminism() {
    std::cout << "[P0-B] Testing Slot-Type Proof Pass, Trusted FastOpcodes, overflow & budget parity...\n";

    const char* src = R"RS(
module P0.TrustedVm;

public int SieveAndDotProduct(int n) {
    bool[] composite = new bool[n + 1];
    int[] primes = new int[n + 1];
    int pCount = 0;
    for (int i = 2; i <= n; i = i + 1) {
        if (!composite[i]) {
            primes[pCount] = i;
            pCount = pCount + 1;
            for (int j = i * 2; j <= n; j = j + i) {
                composite[j] = true;
            }
        }
    }
    double[] weights = new double[pCount];
    for (int k = 0; k < pCount; k = k + 1) {
        weights[k] = (primes[k] * 1.5) - 0.25;
    }
    double acc = 0.0;
    for (int k = 0; k < pCount; k = k + 1) {
        if (weights[k] > 1.0) {
            acc = acc + weights[k];
        }
    }
    return pCount * 100000 + (int)acc;
}

public int CheckedAddOverflow(int a, int b) {
    return a + b;
}

public int CheckedMulOverflow(int a, int b) {
    return a * b;
}

public int UncheckedWrapAround(int a, int b) {
    return unchecked(a + b);
}

public int TightScalarLoop(int iters) {
    int acc = 0;
    for (int i = 1; i <= iters; i = i + 1) {
        acc = unchecked(acc + (i * 3) - (i / 2));
    }
    return acc;
}
)RS";

    auto program = compileAndLink("p0_trusted.rscript", src);
    auto heap = std::make_shared<runtime::ManagedHeap>();
    runtime::Interpreter interp(program, heap);

    // 1. Sieve + packed bool[], int[], double[] + trusted scalar arithmetic
    const auto sieveRes = interp.invoke("P0.TrustedVm::SieveAndDotProduct", {std::int64_t{100}});
    require(sieveRes.succeeded, "SieveAndDotProduct failed: " + sieveRes.error.message);
    // 25 primes <= 100, sum of primes = 1060, sum of (p*1.5 - 0.25) = 1590 - 6.25 = 1583.75 -> (int)1583
    require(std::get<std::int64_t>(sieveRes.value) == 25 * 100000 + 1583,
        "SieveAndDotProduct unexpected result: " + std::to_string(std::get<std::int64_t>(sieveRes.value)));

    // 2. Checked integer overflow must still raise ErrorCode::IntegerOverflow
    const auto ovfAdd = interp.invoke(
        "P0.TrustedVm::CheckedAddOverflow",
        {std::int64_t{std::numeric_limits<std::int32_t>::max()}, std::int64_t{1}});
    require(!ovfAdd.succeeded && ovfAdd.error.code == runtime::ErrorCode::IntegerOverflow,
        "Trusted AddIntTrustedChecked must trap on INT32_MAX + 1");

    const auto ovfMul = interp.invoke(
        "P0.TrustedVm::CheckedMulOverflow",
        {std::int64_t{100000}, std::int64_t{100000}});
    require(!ovfMul.succeeded && ovfMul.error.code == runtime::ErrorCode::IntegerOverflow,
        "Trusted MulIntTrustedChecked must trap on 100000 * 100000");

    const auto wrapAdd = interp.invoke(
        "P0.TrustedVm::UncheckedWrapAround",
        {std::int64_t{std::numeric_limits<std::int32_t>::max()}, std::int64_t{1}});
    require(wrapAdd.succeeded &&
            std::get<std::int64_t>(wrapAdd.value) == std::numeric_limits<std::int32_t>::min(),
        "Trusted AddIntTrustedUnchecked must wrap INT32_MAX + 1 to INT32_MIN");

    // 3. Exact instruction budget accounting & Strict Determinism digest stability
    runtime::ExecutionOptions detOpts1;
    detOpts1.determinism.mode = runtime::DeterminismMode::Strict;
    const auto detRun1 = interp.invoke("P0.TrustedVm::TightScalarLoop", {std::int64_t{5000}}, detOpts1);

    runtime::ExecutionOptions detOpts2;
    detOpts2.determinism.mode = runtime::DeterminismMode::Strict;
    const auto detRun2 = interp.invoke("P0.TrustedVm::TightScalarLoop", {std::int64_t{5000}}, detOpts2);

    require(detRun1.succeeded && detRun2.succeeded, "Strict determinism run failed");
    require(detRun1.statistics.instructionsExecuted == detRun2.statistics.instructionsExecuted &&
            detRun1.statistics.instructionsExecuted > 0,
        "Instruction count must be non-zero and deterministic");
    require(detRun1.determinismDigest == detRun2.determinismDigest &&
            detRun1.determinismDigest != 0,
        "Strict determinism digest must be non-zero and reproducible");

    // Setting budget to exact executed - 1 MUST fail with InstructionLimitExceeded
    runtime::Limits tightLimits;
    tightLimits.instructionBudget = detRun1.statistics.instructionsExecuted - 1;
    const auto overBudget = interp.invoke("P0.TrustedVm::TightScalarLoop", {std::int64_t{5000}}, tightLimits);
    require(!overBudget.succeeded && overBudget.error.code == runtime::ErrorCode::InstructionBudgetExceeded,
        "Instruction budget (exact - 1) must fail with InstructionLimitExceeded");

    tightLimits.instructionBudget = detRun1.statistics.instructionsExecuted;
    const auto exactBudget = interp.invoke("P0.TrustedVm::TightScalarLoop", {std::int64_t{5000}}, tightLimits);
    require(exactBudget.succeeded && exactBudget.value == detRun1.value,
        "Instruction budget (exact) must succeed");

    // 4. Quick throughput smoke benchmark (100k array + scalar iterations)
    const char* benchSrc = R"RS(
module P0.Bench;
public int Kernel(int n) {
    int[] arr = new int[1024];
    for (int i = 0; i < 1024; i = i + 1) {
        arr[i] = i;
    }
    int sum = 0;
    for (int rep = 0; rep < n; rep = rep + 1) {
        for (int i = 0; i < 1024; i = i + 1) {
            sum = unchecked(sum + arr[i] * 3 - i);
        }
    }
    return sum;
}
)RS";
    auto benchProg = compileAndLink("p0_bench.rscript", benchSrc);
    auto benchHeap = std::make_shared<runtime::ManagedHeap>();
    runtime::Interpreter benchInterp(benchProg, benchHeap);
    const auto t0 = std::chrono::steady_clock::now();
    runtime::Limits benchLimits; benchLimits.instructionBudget = 50'000'000;
    const auto benchRes = benchInterp.invoke("P0.Bench::Kernel", {std::int64_t{200}}, benchLimits);
    const auto t1 = std::chrono::steady_clock::now();
    require(benchRes.succeeded, "P0 benchmark kernel failed");
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::cout << "[P0-B] 204,800 packed-int32 + trusted-ALU loop iterations completed in "
              << ms << " ms (heap liveBytes=" << benchHeap->liveBytes() << ")\n";
}

} // namespace

int main() {
    try {
        testUnboxedPackedHeapStorageAndGc();
        testTrustedFastPathSemanticsAndDeterminism();
        std::cout << "All P0 packed heap and trusted VM tests passed!\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[FAIL] " << ex.what() << "\n";
        return 1;
    }
}
