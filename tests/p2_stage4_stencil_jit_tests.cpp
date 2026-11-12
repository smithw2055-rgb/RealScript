#include "realscript/jit/Jit.h"

#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/game/GameScripting.h"
#include "realscript/optimization/Optimizer.h"
#include "realscript/runtime/Runtime.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
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
        throw std::runtime_error("StencilJit verification failure: " + msg);
    }
}

struct BuiltProgram {
    std::vector<mir::Module> mirModules;
    std::shared_ptr<const runtime::ProgramImage> program;
};

BuiltProgram compileProgram(
    const std::string& name,
    const std::string& src,
    optimization::Level optLevel = optimization::Level::Aggressive) {
    compiler::Compilation comp({{name, src}});
    auto build = comp.build();
    if (build.diagnostics.hasErrors()) {
        std::string err = "RealScript compile failed:\n";
        for (const auto& d : build.diagnostics.items()) {
            err += d.code + ": " + d.message + "\n";
        }
        throw std::runtime_error(err);
    }
    BuiltProgram out;
    out.mirModules = build.modules;

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
    out.program = std::make_shared<const runtime::ProgramImage>(std::move(*linked));
    return out;
}

void assertParity(
    const std::string& label,
    const runtime::ExecutionResult& actual,
    const runtime::ExecutionResult& expected) {
    if (actual.succeeded != expected.succeeded) {
        throw std::runtime_error(
            label + ": succeeded mismatch (actual=" +
            (actual.succeeded ? "true" : "false:" + actual.error.message) +
            ", expected=" +
            (expected.succeeded ? "true" : "false:" + expected.error.message) + ")");
    }
    if (actual.succeeded) {
        require(actual.value == expected.value, label + ": return value mismatch");
        require(
            actual.instructionsExecuted == expected.instructionsExecuted,
            label + ": instructionsExecuted mismatch (" +
                std::to_string(actual.instructionsExecuted) + " vs " +
                std::to_string(expected.instructionsExecuted) + ")");
        require(
            actual.statistics.branchesTaken == expected.statistics.branchesTaken,
            label + ": branchesTaken mismatch (" +
                std::to_string(actual.statistics.branchesTaken) + " vs " +
                std::to_string(expected.statistics.branchesTaken) + ")");
        require(
            actual.statistics.functionCalls == expected.statistics.functionCalls,
            label + ": functionCalls mismatch (" +
                std::to_string(actual.statistics.functionCalls) + " vs " +
                std::to_string(expected.statistics.functionCalls) + ")");
        require(
            actual.determinismDigest == expected.determinismDigest,
            label + ": determinismDigest mismatch");
    } else {
        require(
            actual.error.code == expected.error.code,
            label + ": ErrorCode mismatch (" +
                std::string(runtime::errorCodeName(actual.error.code)) + " vs " +
                std::string(runtime::errorCodeName(expected.error.code)) + ")");
        require(
            actual.instructionsExecuted == expected.instructionsExecuted,
            label + ": error instructionsExecuted mismatch (" +
                std::to_string(actual.instructionsExecuted) + " vs " +
                std::to_string(expected.instructionsExecuted) + ")");
    }
}

void testExecutableCodeArenaWx() {
    std::cout << "[1/6] Testing ExecutableCodeArena W^X lifecycle & ABI probe...\n";
    require(jit::StencilJit::nativeAvailable(), "StencilJit::nativeAvailable() must be true on Linux x86_64");

    jit::ExecutableCodeArena arena(4096);
    require(!arena.sealed(), "fresh arena must not be sealed");

#if defined(_WIN32)
    // Win64 x86_64: int add7(int x) -> lea eax, [rcx + 7]; ret -> 8D 41 07 C3
    static constexpr std::uint8_t kAdd7[] = {0x8D, 0x41, 0x07, 0xC3};
#else
    // SysV x86_64: int add7(int x) -> lea eax, [rdi + 7]; ret -> 8D 47 07 C3
    static constexpr std::uint8_t kAdd7[] = {0x8D, 0x47, 0x07, 0xC3};
#endif
    std::uint8_t* fnPtrRaw = arena.emit(kAdd7, sizeof(kAdd7), 16);
    require(fnPtrRaw != nullptr, "arena.emit failed during RW phase");
    require((reinterpret_cast<std::uintptr_t>(fnPtrRaw) & 15u) == 0u, "emitted code must be 16B aligned");
    require(arena.allocatedBytes() == sizeof(kAdd7), "allocatedBytes mismatch");
    require(arena.seal(), "arena.seal() RW -> RX mprotect failed");
    require(arena.sealed(), "arena.sealed() must be true after seal()");

    // Strict W^X: emitting into a sealed arena MUST fail and never reopen write permissions
    require(arena.emit(kAdd7, sizeof(kAdd7), 16) == nullptr, "sealed arena must reject further writes (W^X)");

    using Add7Fn = int (*)(int);
    Add7Fn fn = nullptr;
    std::memcpy(&fn, &fnPtrRaw, sizeof(fn));
    require(fn(35) == 42 && fn(-7) == 0, "sealed native machine code returned wrong result");

    // Move semantics
    jit::ExecutableCodeArena movedArena = std::move(arena);
    require(movedArena.sealed() && movedArena.allocatedBytes() == sizeof(kAdd7), "moved arena lost state");
    require(fn(100) == 107, "moved arena code pointer must remain valid until movedArena destruction");
}

void testScalarOverflowDivBudgetAndFloats() {
    std::cout << "[2/6] Testing x86_64 scalar ALU, hardware `jo` overflow, div/rem, float/double & exact budget...\n";
    const char* src = R"RS(
module P2.ScalarSuite;

int helperSquareDiff(int x, int y) {
    return (x * x) - (y * y);
}

public int CheckedAdd(int a, int b) { return a + b; }
public int CheckedSub(int a, int b) { return a - b; }
public int CheckedMul(int a, int b) { return a * b; }
public int UncheckedAdd(int a, int b) { return unchecked(a + b); }
public int UncheckedSub(int a, int b) { return unchecked(a - b); }
public int UncheckedMul(int a, int b) { return unchecked(a * b); }
public int DivOp(int a, int b) { return a / b; }
public int RemOp(int a, int b) { return a % b; }

public int ControlAndInlineLeaf(int n) {
    int acc = 0;
    for (int i = 1; i <= n; i = i + 1) {
        if ((i % 3) == 0) {
            acc = acc + helperSquareDiff(i, 2);
        } else if ((i % 3) == 1) {
            acc = acc - (i / 2);
        } else {
            acc = unchecked(acc + i * 5);
        }
    }
    return acc;
}

public double DoublePoly(double x, int steps) {
    double acc = 0.0;
    for (int i = 1; i <= steps; i = i + 1) {
        double d = (double)i;
        if (d * x >= 2.5) {
            acc = acc + (d * x) / (d + 1.0);
        } else {
            acc = acc - (x - 0.5);
        }
    }
    return acc;
}

public bool DoubleNanCheck(double a, double b) {
    double nanVal = 0.0 / 0.0;
    bool c1 = nanVal < a;
    bool c2 = nanVal <= b;
    bool c3 = nanVal > a;
    bool c4 = nanVal >= b;
    return (!c1) && (!c2) && (!c3) && (!c4) && (a < b);
}
)RS";

    auto built = compileProgram("p2_scalar.rscript", src);
    jit::StencilJit sjit;
    auto compiled = sjit.compile(built.program);
    require(compiled.succeeded(), "StencilJit compile failed: " + compiled.error);
    require(
        compiled.nativeFunctionCount == compiled.totalFunctionCount && compiled.fallbackFunctionCount == 0,
        "All P2.ScalarSuite functions must compile to NativeMachineCode");
    require(compiled.compileTimeMicroseconds < 1000.0,
        "Single-module StencilJit compile must be < 1000 us, got " +
            std::to_string(compiled.compileTimeMicroseconds) + " us");

    // Dump ControlAndInlineLeaf machine code for optional disassembly verification
    const auto codeBytes = compiled.module->inspectMachineCode("P2.ScalarSuite::ControlAndInlineLeaf");
    require(codeBytes.size() > 32, "inspectMachineCode returned empty bytes");
    {
        std::ofstream binOut((std::filesystem::temp_directory_path() / "realscript_control_leaf_x64.bin").string(), std::ios::binary | std::ios::trunc);
        binOut.write(reinterpret_cast<const char*>(codeBytes.data()), static_cast<std::streamsize>(codeBytes.size()));
    }

    // Also compile in portable ThreadedStencils mode to verify cross-platform fallback parity
    jit::StencilJitOptions threadedOpts;
    threadedOpts.enableNativeCodegen = false;
    auto threaded = sjit.compile(built.program, threadedOpts);
    require(threaded.succeeded() && threaded.threadedFunctionCount == threaded.totalFunctionCount,
        "ThreadedStencils mode failed");

    runtime::Interpreter interp(built.program);
    const std::int64_t iMax = std::numeric_limits<std::int32_t>::max();
    const std::int64_t iMin = std::numeric_limits<std::int32_t>::min();

    struct CallCase {
        const char* fn;
        std::vector<runtime::Value> args;
    };
    const std::vector<CallCase> cases = {
        {"P2.ScalarSuite::CheckedAdd", {std::int64_t{12345}, std::int64_t{67890}}},
        {"P2.ScalarSuite::CheckedAdd", {iMax, std::int64_t{1}}},     // jo overflow!
        {"P2.ScalarSuite::CheckedAdd", {iMin, std::int64_t{-1}}},    // jo underflow!
        {"P2.ScalarSuite::CheckedSub", {iMin, std::int64_t{1}}},     // jo overflow!
        {"P2.ScalarSuite::CheckedSub", {iMax, std::int64_t{-1}}},    // jo overflow!
        {"P2.ScalarSuite::CheckedMul", {std::int64_t{100000}, std::int64_t{100000}}}, // imul jo overflow!
        {"P2.ScalarSuite::CheckedMul", {iMin, std::int64_t{-1}}},    // imul jo overflow!
        {"P2.ScalarSuite::CheckedMul", {std::int64_t{-40000}, std::int64_t{30000}}},
        {"P2.ScalarSuite::UncheckedAdd", {iMax, std::int64_t{1}}},
        {"P2.ScalarSuite::UncheckedSub", {iMin, std::int64_t{1}}},
        {"P2.ScalarSuite::UncheckedMul", {std::int64_t{100000}, std::int64_t{100000}}},
        {"P2.ScalarSuite::DivOp", {std::int64_t{-100}, std::int64_t{7}}},
        {"P2.ScalarSuite::DivOp", {std::int64_t{42}, std::int64_t{0}}},  // div by zero!
        {"P2.ScalarSuite::DivOp", {iMin, std::int64_t{-1}}},             // INT32_MIN / -1 overflow (no SIGFPE!)
        {"P2.ScalarSuite::RemOp", {std::int64_t{-100}, std::int64_t{7}}},
        {"P2.ScalarSuite::RemOp", {std::int64_t{42}, std::int64_t{0}}},  // rem by zero!
        {"P2.ScalarSuite::RemOp", {iMin, std::int64_t{-1}}},             // INT32_MIN % -1 == 0 (no SIGFPE!)
        {"P2.ScalarSuite::ControlAndInlineLeaf", {std::int64_t{250}}},
        {"P2.ScalarSuite::DoublePoly", {1.75, std::int64_t{200}}},
        {"P2.ScalarSuite::DoubleNanCheck", {1.5, 2.5}},
    };

    for (std::size_t idx = 0; idx < cases.size(); ++idx) {
        const auto& c = cases[idx];
        const auto exp = interp.invoke(c.fn, c.args);
        const auto actNative = compiled.module->invoke(c.fn, c.args);
        const auto actThreaded = threaded.module->invoke(c.fn, c.args);
        assertParity(std::string("Native[") + c.fn + "#" + std::to_string(idx) + "]", actNative, exp);
        assertParity(std::string("Threaded[") + c.fn + "#" + std::to_string(idx) + "]", actThreaded, exp);
    }

    // Exact instructionBudget boundary verification (exact vs exact - 1 vs mid-superinstruction)
    const auto fullRun = interp.invoke("P2.ScalarSuite::ControlAndInlineLeaf", {std::int64_t{60}});
    require(fullRun.succeeded && fullRun.instructionsExecuted > 20, "baseline run failed");
    for (std::uint64_t delta : {0ULL, 1ULL, 2ULL, 3ULL, 4ULL, 5ULL, 17ULL}) {
        runtime::Limits lim;
        lim.instructionBudget = fullRun.instructionsExecuted - delta;
        const auto expB = interp.invoke("P2.ScalarSuite::ControlAndInlineLeaf", {std::int64_t{60}}, lim);
        const auto actNativeB = compiled.module->invoke("P2.ScalarSuite::ControlAndInlineLeaf", {std::int64_t{60}}, lim);
        const auto actThreadB = threaded.module->invoke("P2.ScalarSuite::ControlAndInlineLeaf", {std::int64_t{60}}, lim);
        assertParity("BudgetNative-" + std::to_string(delta), actNativeB, expB);
        assertParity("BudgetThreaded-" + std::to_string(delta), actThreadB, expB);
    }

    // Strict Determinism mode delegation parity
    runtime::ExecutionOptions strictOpts;
    strictOpts.determinism.mode = runtime::DeterminismMode::Strict;
    const auto expStrict = interp.invoke("P2.ScalarSuite::ControlAndInlineLeaf", {std::int64_t{60}}, strictOpts);
    const auto actStrict = compiled.module->invoke("P2.ScalarSuite::ControlAndInlineLeaf", {std::int64_t{60}}, strictOpts);
    assertParity("StrictDeterminism", actStrict, expStrict);
}

void testPackedArraysBoundsAndWidening() {
    std::cout << "[3/6] Testing P0 packed int32[] / double[] / bool[] in-place read/write, BCE & bounds traps...\n";
    const char* src = R"RS(
module P2.ArraySuite;

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
        weights[k] = ((double)primes[k] * 1.5) - 0.25;
    }
    double acc = 0.0;
    for (int k = 0; k < pCount; k = k + 1) {
        if (weights[k] > 1.0) {
            acc = acc + weights[k];
        }
    }
    return pCount * 100000 + (int)((float)acc);
}

public int MutateAndSumIntArray(int[] arr, int scale) {
    int sum = 0;
    for (int i = 0; i < arr.Length; i = i + 1) {
        arr[i] = arr[i] * scale + 1;
        sum = unchecked(sum + arr[i]);
    }
    return sum;
}

public double ScaleAndDotDoubleArray(double[] a, double[] b, double alpha) {
    double dot = 0.0;
    for (int i = 0; i < a.Length; i = i + 1) {
        a[i] = a[i] * alpha;
        dot = dot + a[i] * b[i];
    }
    return dot;
}

public int ReadAt(int[] arr, int idx) {
    return arr[idx];
}

public void WriteAt(int[] arr, int idx, int v) {
    arr[idx] = v;
}
)RS";

    auto built = compileProgram("p2_arrays.rscript", src);
    jit::StencilJit sjit;
    auto compiled = sjit.compile(built.program);
    require(compiled.succeeded(), "ArraySuite compile failed: " + compiled.error);
    require(
        compiled.module->functionTier("P2.ArraySuite::SieveAndDotProduct") == jit::StencilTier::NativeMachineCode &&
        compiled.module->functionTier("P2.ArraySuite::MutateAndSumIntArray") == jit::StencilTier::NativeMachineCode &&
        compiled.module->functionTier("P2.ArraySuite::ScaleAndDotDoubleArray") == jit::StencilTier::NativeMachineCode,
        "ArraySuite kernels must compile to NativeMachineCode");

    auto jitHeap = std::make_shared<runtime::ManagedHeap>();
    auto interpHeap = std::make_shared<runtime::ManagedHeap>();
    compiled.module->setHeap(jitHeap);
    runtime::Interpreter interp(built.program, interpHeap);

    const auto actSieve = compiled.module->invoke("P2.ArraySuite::SieveAndDotProduct", {std::int64_t{200}});
    const auto expSieve = interp.invoke("P2.ArraySuite::SieveAndDotProduct", {std::int64_t{200}});
    assertParity("SieveAndDotProduct(200)", actSieve, expSieve);

    // Pre-allocated packed int32[] in-place mutation & bounds checks
    const auto intArrTid = semantic::stableTypeId("int[]");
    const auto dblArrTid = semantic::stableTypeId("double[]");
    runtime::RuntimeError err;
    auto jitIntArr = jitHeap->allocateTypedArray(intArrTid, semantic::PrimitiveType::Int, 0, 16, std::int64_t{0}, &err);
    auto expIntArr = interpHeap->allocateTypedArray(intArrTid, semantic::PrimitiveType::Int, 0, 16, std::int64_t{0}, &err);
    require(jitIntArr.has_value() && expIntArr.has_value() && jitHeap->isPackedArray(*jitIntArr), "packed int[] alloc failed");
    for (std::size_t i = 0; i < 16; ++i) {
        (void)jitHeap->arraySet(*jitIntArr, i, std::int64_t(i + 1));
        (void)interpHeap->arraySet(*expIntArr, i, std::int64_t(i + 1));
    }

    const auto actMut = compiled.module->invoke("P2.ArraySuite::MutateAndSumIntArray", {*jitIntArr, std::int64_t{3}});
    const auto expMut = interp.invoke("P2.ArraySuite::MutateAndSumIntArray", {*expIntArr, std::int64_t{3}});
    assertParity("MutateAndSumIntArray", actMut, expMut);
    for (std::size_t i = 0; i < 16; ++i) {
        require(*jitHeap->arrayGet(*jitIntArr, i) == *interpHeap->arrayGet(*expIntArr, i), "in-place int[] element diverged");
    }

    // Null & out-of-bounds checks
    assertParity(
        "ReadNull",
        compiled.module->invoke("P2.ArraySuite::ReadAt", {runtime::NullArray{}, std::int64_t{0}}),
        interp.invoke("P2.ArraySuite::ReadAt", {runtime::NullArray{}, std::int64_t{0}}));
    assertParity(
        "ReadNegIndex",
        compiled.module->invoke("P2.ArraySuite::ReadAt", {*jitIntArr, std::int64_t{-1}}),
        interp.invoke("P2.ArraySuite::ReadAt", {*expIntArr, std::int64_t{-1}}));
    assertParity(
        "ReadOobIndex",
        compiled.module->invoke("P2.ArraySuite::ReadAt", {*jitIntArr, std::int64_t{16}}),
        interp.invoke("P2.ArraySuite::ReadAt", {*expIntArr, std::int64_t{16}}));
    assertParity(
        "WriteOobIndex",
        compiled.module->invoke("P2.ArraySuite::WriteAt", {*jitIntArr, std::int64_t{99}, std::int64_t{7}}),
        interp.invoke("P2.ArraySuite::WriteAt", {*expIntArr, std::int64_t{99}, std::int64_t{7}}));

    // Widened PackedI64 array transparent handling
    const std::int64_t wideVal = (std::int64_t{1} << 40) + 77;
    require(jitHeap->arraySet(*jitIntArr, 5, wideVal, &err), "host wide set jit failed");
    require(interpHeap->arraySet(*expIntArr, 5, wideVal, &err), "host wide set interp failed");
    assertParity(
        "ReadWidenedIntArray",
        compiled.module->invoke("P2.ArraySuite::ReadAt", {*jitIntArr, std::int64_t{5}}),
        interp.invoke("P2.ArraySuite::ReadAt", {*expIntArr, std::int64_t{5}}));

    // Packed double[] dot product
    auto jitDblA = jitHeap->allocateTypedArray(dblArrTid, semantic::PrimitiveType::Double, 0, 32, 1.25, &err);
    auto jitDblB = jitHeap->allocateTypedArray(dblArrTid, semantic::PrimitiveType::Double, 0, 32, 2.5, &err);
    auto expDblA = interpHeap->allocateTypedArray(dblArrTid, semantic::PrimitiveType::Double, 0, 32, 1.25, &err);
    auto expDblB = interpHeap->allocateTypedArray(dblArrTid, semantic::PrimitiveType::Double, 0, 32, 2.5, &err);
    assertParity(
        "ScaleAndDotDoubleArray",
        compiled.module->invoke("P2.ArraySuite::ScaleAndDotDoubleArray", {*jitDblA, *jitDblB, 2.0}),
        interp.invoke("P2.ArraySuite::ScaleAndDotDoubleArray", {*expDblA, *expDblB, 2.0}));
}

void testComplexFallbackAndHotspotTiering() {
    std::cout << "[4/6] Testing mixed-module fallback (host bindings / exceptions / OOP) & hotness tier-up...\n";
    game::GameApi api;
    require(
        api.function("P2.Host", "hostScale", [](int x) -> int { return x * 7; }),
        "failed to register P2.Host::hostScale");

    const char* src = R"RS(
module P2.MixedModule;
import P2.Host;

class CustomErr {
    public int Code;
}

public int PureHotKernel(int n) {
    int s = 0;
    for (int i = 1; i <= n; i = i + 1) {
        s = s + i;
    }
    return s;
}

public int HostBindingCaller(int x) {
    return hostScale(x) + 10;
}

public int ExceptionCatcher(int trigger) {
    try {
        if (trigger > 0) {
            CustomErr e = new CustomErr();
            e.Code = trigger * 11;
            throw e;
        }
        return 99;
    } catch (CustomErr ex) {
        return ex.Code + 1;
    }
}
)RS";

    game::GameScriptCompiler gsc(api);
    auto builtGame = gsc.compile({{"p2_mixed.rscript", src}});
    require(builtGame.succeeded(), "GameScriptCompiler failed on P2.MixedModule");
    runtime::RuntimeError linkErr;
    auto linkedMixed = runtime::ProgramImage::link(builtGame.modules, linkErr);
    require(linkedMixed.has_value(), "link mixed failed: " + linkErr.message);
    const auto program = std::make_shared<const runtime::ProgramImage>(std::move(*linkedMixed));
    const auto bindings = api.bindings();

    jit::StencilJit sjit;
    auto compiled = sjit.compile(program);
    require(compiled.succeeded(), "MixedModule compile failed: " + compiled.error);
    require(compiled.nativeFunctionCount >= 1, "PureHotKernel should be NativeMachineCode");
    require(compiled.fallbackFunctionCount >= 2, "HostBindingCaller & ExceptionCatcher should be fallback");
    compiled.module->setBindings(bindings);

    runtime::Interpreter interp(program);
    interp.setBindingRegistry(bindings);

    assertParity(
        "Mixed::PureHotKernel",
        compiled.module->invoke("P2.MixedModule::PureHotKernel", {std::int64_t{500}}),
        interp.invoke("P2.MixedModule::PureHotKernel", {std::int64_t{500}}));
    assertParity(
        "Mixed::HostBindingCaller",
        compiled.module->invoke("P2.MixedModule::HostBindingCaller", {std::int64_t{6}}),
        interp.invoke("P2.MixedModule::HostBindingCaller", {std::int64_t{6}}));
    assertParity(
        "Mixed::ExceptionCatcher(throw)",
        compiled.module->invoke("P2.MixedModule::ExceptionCatcher", {std::int64_t{5}}),
        interp.invoke("P2.MixedModule::ExceptionCatcher", {std::int64_t{5}}));
    assertParity(
        "Mixed::ExceptionCatcher(nothrow)",
        compiled.module->invoke("P2.MixedModule::ExceptionCatcher", {std::int64_t{0}}),
        interp.invoke("P2.MixedModule::ExceptionCatcher", {std::int64_t{0}}));

    // Verify lazy hot-spot tier-up (hotnessThreshold = 4)
    jit::StencilJitOptions lazyOpts;
    lazyOpts.hotnessThreshold = 4;
    auto lazyRes = sjit.compile(program, lazyOpts);
    require(lazyRes.succeeded(), "lazy StencilJit compile failed");
    require(
        lazyRes.nativeFunctionCount == 0 && lazyRes.fallbackFunctionCount >= 2 &&
            lazyRes.module->functionTier("P2.MixedModule::PureHotKernel") == jit::StencilTier::ThreadedStencils,
        "lazy module must start PureHotKernel in ThreadedStencils before threshold");
    for (int call = 1; call <= 6; ++call) {
        const auto r = lazyRes.module->invoke("P2.MixedModule::PureHotKernel", {std::int64_t{100}});
        require(r.succeeded && std::get<std::int64_t>(r.value) == 5050, "lazy invocation result mismatch");
        if (call < 4) {
            require(
                lazyRes.module->functionTier("P2.MixedModule::PureHotKernel") == jit::StencilTier::ThreadedStencils,
                "promoted too early before hotnessThreshold");
        } else {
            require(
                lazyRes.module->functionTier("P2.MixedModule::PureHotKernel") == jit::StencilTier::NativeMachineCode &&
                    lazyRes.module->nativeFunctionCount() >= 1 &&
                    lazyRes.module->machineCodeBytes() > 0,
                "failed to auto-promote to NativeMachineCode at hotnessThreshold");
        }
    }
}

void testBenchmarksAndCompileSpeedup() {
    std::cout << "[5/6] Measuring compile latency (StencilJit vs ToolchainJit) & hot-loop throughput...\n";
    const char* benchSrc = R"RS(
module Bench.StencilSuite;

public int IntegerLoop10k() {
    int total = 0;
    int index = 0;
    while (index < 10000) {
        total = total + index;
        index = index + 1;
    }
    return total;
}

public int ScalarAlu100k(int iters) {
    int acc = 0;
    for (int i = 1; i <= iters; i = i + 1) {
        acc = unchecked(acc + (i * 3) - (i / 2));
    }
    return acc;
}

public int PackedIntArrayMac(int reps) {
    int[] arr = new int[1024];
    for (int i = 0; i < 1024; i = i + 1) {
        arr[i] = i + 1;
    }
    int sum = 0;
    for (int r = 0; r < reps; r = r + 1) {
        for (int i = 0; i < 1024; i = i + 1) {
            sum = unchecked(sum + arr[i] * 3 - i);
        }
    }
    return sum;
}

public double PackedDoubleDot(int reps) {
    double[] a = new double[1024];
    double[] b = new double[1024];
    for (int i = 0; i < 1024; i = i + 1) {
        a[i] = (double)i * 0.01;
        b[i] = 1.5;
    }
    double sum = 0.0;
    for (int r = 0; r < reps; r = r + 1) {
        for (int i = 0; i < 1024; i = i + 1) {
            sum = sum + a[i] * b[i];
        }
    }
    return sum;
}
)RS";

    auto built = compileProgram("bench_stencil.rscript", benchSrc);
    jit::StencilJit sjit;

    // Warmup + median of 15 StencilJit::compile runs
    std::vector<double> stencilCompileUs;
    jit::StencilCompileResult nativeCompiled;
    for (int i = 0; i < 15; ++i) {
        auto c = sjit.compile(built.program);
        require(c.succeeded(), "benchmark StencilJit::compile failed");
        stencilCompileUs.push_back(c.compileTimeMicroseconds);
        nativeCompiled = std::move(c);
    }
    std::sort(stencilCompileUs.begin(), stencilCompileUs.end());
    const double medianStencilUs = stencilCompileUs[stencilCompileUs.size() / 2];

    // Measure external ToolchainJit cold compile on the exact same MIR modules when configured
    double toolchainColdUs = 0.0;
#if defined(REALSCRIPT_JIT_COMPILER) && defined(REALSCRIPT_JIT_INCLUDE_DIR) && defined(REALSCRIPT_JIT_SUPPORT_LIBRARY)
    {
        jit::ToolchainOptions tcOpts;
        tcOpts.compiler = REALSCRIPT_JIT_COMPILER;
        tcOpts.includeDirectory = REALSCRIPT_JIT_INCLUDE_DIR;
        tcOpts.supportLibrary = REALSCRIPT_JIT_SUPPORT_LIBRARY;
#if defined(REALSCRIPT_JIT_CACHE_DIR)
        tcOpts.outputDirectory = REALSCRIPT_JIT_CACHE_DIR;
#else
        tcOpts.outputDirectory = std::filesystem::temp_directory_path() / "realscript_p2_tc_bench_cache";
#endif
#if defined(_WIN32)
#if defined(_DEBUG)
        tcOpts.compilerArguments = {"/MDd"};
#else
        tcOpts.compilerArguments = {"/MD", "/O2"};
#endif
#else
        tcOpts.compilerArguments = {"-O2"};
#endif
        tcOpts.reuseCachedLibrary = false;
        tcOpts.generation.programName = "BenchStencilVsToolchain";
        std::error_code ec;
        std::filesystem::remove_all(tcOpts.outputDirectory, ec);
        if (jit::ToolchainJit::available(tcOpts)) {
            jit::ToolchainJit tjit;
            const auto t0 = std::chrono::steady_clock::now();
            const auto tcRes = tjit.compile(built.mirModules, tcOpts);
            const auto t1 = std::chrono::steady_clock::now();
            require(tcRes.succeeded(), "ToolchainJit compile failed: " + tcRes.error);
            toolchainColdUs = std::chrono::duration<double, std::micro>(t1 - t0).count();
        }
        std::filesystem::remove_all(tcOpts.outputDirectory, ec);
    }
#endif

    jit::StencilJitOptions threadedOpts;
    threadedOpts.enableNativeCodegen = false;
    auto threadedCompiled = sjit.compile(built.program, threadedOpts);

    runtime::Interpreter interp(built.program);
    runtime::ExecutionOptions fastOpts;
    fastOpts.limits.instructionBudget = 200'000'000;
    fastOpts.limits.gcWorkBudget = 0;

    const auto benchKernel = [&](
        const char* name,
        const char* qualifiedFn,
        const std::vector<runtime::Value>& args,
        int samples,
        int innerIters) {
        const auto expRes = interp.invoke(qualifiedFn, args, fastOpts);
        const auto natRes = nativeCompiled.module->invoke(qualifiedFn, args, fastOpts);
        assertParity(name, natRes, expRes);

        const auto medianMs = [&](auto&& fn) {
            fn(); // warmup
            std::vector<double> times;
            for (int s = 0; s < samples; ++s) {
                const auto s0 = std::chrono::steady_clock::now();
                for (int k = 0; k < innerIters; ++k) fn();
                const auto s1 = std::chrono::steady_clock::now();
                times.push_back(
                    std::chrono::duration<double, std::milli>(s1 - s0).count() / innerIters);
            }
            std::sort(times.begin(), times.end());
            return times[times.size() / 2];
        };

        const double interpMs = medianMs([&] {
            const auto r = interp.invoke(qualifiedFn, args, fastOpts);
            if (!r.succeeded) throw std::runtime_error("interp failed");
        });
        const double threadedMs = medianMs([&] {
            const auto r = threadedCompiled.module->invoke(qualifiedFn, args, fastOpts);
            if (!r.succeeded) throw std::runtime_error("threaded failed");
        });
        const double nativeMs = medianMs([&] {
            const auto r = nativeCompiled.module->invoke(qualifiedFn, args, fastOpts);
            if (!r.succeeded) throw std::runtime_error("native failed");
        });
        const double speedup = interpMs / nativeMs;
        std::cout << "  - " << std::left << std::setw(22) << name
                  << " | Interp: " << std::fixed << std::setprecision(4) << interpMs << " ms"
                  << " | Threaded: " << threadedMs << " ms"
                  << " | StencilJit x86_64: " << nativeMs << " ms"
                  << " | Speedup: " << std::setprecision(2) << speedup << "x\n";
        require(nativeMs < interpMs, std::string(name) + ": StencilJit native must beat interpreter");
    };

    std::cout << "  - StencilJit::compile median: " << std::fixed << std::setprecision(2)
              << medianStencilUs << " us (" << nativeCompiled.nativeFunctionCount
              << " native fns, " << nativeCompiled.machineCodeBytes << " bytes)";
    if (toolchainColdUs > 0.0) {
        std::cout << " vs ToolchainJit cold: " << toolchainColdUs << " us ("
                  << std::setprecision(1) << (toolchainColdUs / medianStencilUs) << "x faster!)";
        require(toolchainColdUs / medianStencilUs > 1000.0,
            "StencilJit compile should be > 1000x faster than external ToolchainJit");
    }
    std::cout << "\n";
    require(medianStencilUs < 1000.0, "StencilJit compile latency must be < 1000 us (< 1 ms)");

    benchKernel("IntegerLoop(10k)", "Bench.StencilSuite::IntegerLoop10k", {}, 9, 20);
    benchKernel("ScalarAlu(100k)", "Bench.StencilSuite::ScalarAlu100k", {std::int64_t{100000}}, 9, 5);
    benchKernel("PackedInt32[1024]x200", "Bench.StencilSuite::PackedIntArrayMac", {std::int64_t{200}}, 9, 3);
    benchKernel("PackedF64[1024]x200", "Bench.StencilSuite::PackedDoubleDot", {std::int64_t{200}}, 9, 3);
}

} // namespace

int main() {
    try {
        testExecutableCodeArenaWx();
        testScalarOverflowDivBudgetAndFloats();
        testPackedArraysBoundsAndWidening();
        testComplexFallbackAndHotspotTiering();
        testBenchmarksAndCompileSpeedup();
        std::cout << "[6/6] All P2-A StencilJit verification & performance tests PASSED!\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[FAIL] " << ex.what() << "\n";
        return 1;
    }
}
