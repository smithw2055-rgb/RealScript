#include "realscript/tooling/GovernanceFuzzer.h"

#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/diagnostics/Diagnostic.h"
#include "realscript/jit/Jit.h"
#include "realscript/optimization/Optimizer.h"
#include "realscript/runtime/Runtime.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace realscript::tooling {
namespace {

class SplitMix64 {
public:
    explicit constexpr SplitMix64(std::uint64_t seed) noexcept
        : state_(seed == 0 ? 0x9E3779B97F4A7C15ULL : seed) {}

    [[nodiscard]] std::uint64_t next() noexcept {
        std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31U);
    }

    [[nodiscard]] std::uint32_t rangeU32(std::uint32_t lo, std::uint32_t hiInclusive) noexcept {
        if (hiInclusive <= lo) return lo;
        const std::uint64_t span = static_cast<std::uint64_t>(hiInclusive - lo) + 1ULL;
        return lo + static_cast<std::uint32_t>(next() % span);
    }

    [[nodiscard]] std::int32_t rangeI32(std::int32_t lo, std::int32_t hiInclusive) noexcept {
        if (hiInclusive <= lo) return lo;
        const auto span = static_cast<std::uint64_t>(
            static_cast<std::int64_t>(hiInclusive) - static_cast<std::int64_t>(lo) + 1LL);
        return static_cast<std::int32_t>(static_cast<std::int64_t>(lo) + static_cast<std::int64_t>(next() % span));
    }

    [[nodiscard]] bool chance(std::uint32_t pct) noexcept {
        return rangeU32(0, 99) < pct;
    }

private:
    std::uint64_t state_;
};

std::string makeIntExpr(SplitMix64& rng, int depth, const std::vector<std::string>& vars) {
    if (depth <= 0 || rng.chance(30)) {
        if (!vars.empty() && rng.chance(65)) {
            return vars[rng.rangeU32(0, static_cast<std::uint32_t>(vars.size() - 1))];
        }
        const int val = rng.rangeI32(-25, 40);
        return val < 0 ? ("(" + std::to_string(val) + ")") : std::to_string(val);
    }

    const std::string lhs = makeIntExpr(rng, depth - 1, vars);
    const std::string rhs = makeIntExpr(rng, depth - 1, vars);
    const std::uint32_t opChoice = rng.rangeU32(0, 6);
    switch (opChoice) {
    case 0: // Checked addition (can trap on INT32_MAX/MIN inputs!)
        return "(" + lhs + " + " + rhs + ")";
    case 1: // Checked subtraction
        return "(" + lhs + " - " + rhs + ")";
    case 2: // Checked multiplication
        return "(" + lhs + " * " + std::to_string(rng.rangeI32(-5, 7)) + ")";
    case 3: // Unchecked wrap-around arithmetic
        return "unchecked(" + lhs + " * " + rhs + " + " + std::to_string(rng.rangeI32(1, 31)) + ")";
    case 4: // Safe or potentially zero-dividing division
        if (rng.chance(25)) {
            return "(" + lhs + " / " + rhs + ")";
        }
        return "(" + lhs + " / ((" + rhs + " == 0) ? 3 : " + rhs + "))";
    case 5: // Remainder
        if (rng.chance(25)) {
            return "(" + lhs + " % " + rhs + ")";
        }
        return "(" + lhs + " % ((" + rhs + " == 0) ? 5 : " + rhs + "))";
    default:
        return "unchecked(" + lhs + " - " + rhs + ")";
    }
}

std::string makeBoolCond(SplitMix64& rng, const std::vector<std::string>& vars) {
    const char* cmpOps[] = {"<", "<=", ">", ">=", "==", "!="};
    const std::string a = makeIntExpr(rng, 1, vars);
    const std::string b = makeIntExpr(rng, 1, vars);
    const char* op1 = cmpOps[rng.rangeU32(0, 5)];
    std::string cond = "(" + a + " " + op1 + " " + b + ")";
    if (rng.chance(40)) {
        const std::string c = makeIntExpr(rng, 1, vars);
        const std::string d = makeIntExpr(rng, 1, vars);
        const char* op2 = cmpOps[rng.rangeU32(0, 5)];
        const char* logic = rng.chance(50) ? "&&" : "||";
        cond = "(" + cond + " " + logic + " (" + c + " " + op2 + " " + d + "))";
    }
    return cond;
}

std::string makeDoubleExpr(SplitMix64& rng, int depth, const std::vector<std::string>& dblVars) {
    if (depth <= 0 || rng.chance(35)) {
        if (!dblVars.empty() && rng.chance(65)) {
            return dblVars[rng.rangeU32(0, static_cast<std::uint32_t>(dblVars.size() - 1))];
        }
        const int num = rng.rangeI32(-16, 24);
        return std::to_string(num) + "." + std::to_string(rng.rangeU32(0, 7) * 125);
    }
    const std::string l = makeDoubleExpr(rng, depth - 1, dblVars);
    const std::string r = makeDoubleExpr(rng, depth - 1, dblVars);
    switch (rng.rangeU32(0, 3)) {
    case 0: return "(" + l + " + " + r + ")";
    case 1: return "(" + l + " - " + r + ")";
    case 2: return "(" + l + " * " + r + ")";
    default: return "(" + l + " / 2.0)";
    }
}

bool valuesBitwiseEqual(const runtime::Value& a, const runtime::Value& b, std::string& reason) {
    if (a.rawTag() != b.rawTag()) {
        reason = "ValueTag mismatch (" + std::to_string(static_cast<int>(a.rawTag())) +
            " vs " + std::to_string(static_cast<int>(b.rawTag())) + ")";
        return false;
    }
    switch (a.rawTag()) {
    case runtime::ValueTag::Monostate:
    case runtime::ValueTag::NullString:
    case runtime::ValueTag::NullObject:
    case runtime::ValueTag::NullArray:
        return true;
    case runtime::ValueTag::Bool: {
        const bool ba = std::get<bool>(a);
        const bool bb = std::get<bool>(b);
        if (ba != bb) {
            reason = std::string("bool mismatch (") + (ba ? "true" : "false") +
                " vs " + (bb ? "true" : "false") + ")";
            return false;
        }
        return true;
    }
    case runtime::ValueTag::Int64: {
        const std::int64_t ia = std::get<std::int64_t>(a);
        const std::int64_t ib = std::get<std::int64_t>(b);
        if (ia != ib) {
            reason = "int mismatch (" + std::to_string(ia) + " vs " + std::to_string(ib) + ")";
            return false;
        }
        return true;
    }
    case runtime::ValueTag::Long: {
        const std::int64_t la = std::get<runtime::LongValue>(a).value;
        const std::int64_t lb = std::get<runtime::LongValue>(b).value;
        if (la != lb) {
            reason = "long mismatch (" + std::to_string(la) + " vs " + std::to_string(lb) + ")";
            return false;
        }
        return true;
    }
    case runtime::ValueTag::Float: {
        const float fa = std::get<runtime::FloatValue>(a).value;
        const float fb = std::get<runtime::FloatValue>(b).value;
        if (std::isnan(fa) && std::isnan(fb)) return true;
        std::uint32_t bitsA = 0, bitsB = 0;
        std::memcpy(&bitsA, &fa, sizeof(bitsA));
        std::memcpy(&bitsB, &fb, sizeof(bitsB));
        if (bitsA != bitsB) {
            std::ostringstream oss;
            oss << "float bitwise mismatch (0x" << std::hex << bitsA << " vs 0x" << bitsB << ")";
            reason = oss.str();
            return false;
        }
        return true;
    }
    case runtime::ValueTag::Double: {
        const double da = std::get<double>(a);
        const double db = std::get<double>(b);
        if (std::isnan(da) && std::isnan(db)) return true;
        std::uint64_t bitsA = 0, bitsB = 0;
        std::memcpy(&bitsA, &da, sizeof(bitsA));
        std::memcpy(&bitsB, &db, sizeof(bitsB));
        if (bitsA != bitsB) {
            std::ostringstream oss;
            oss << "double bitwise mismatch (0x" << std::hex << bitsA << " vs 0x" << bitsB << ")";
            reason = oss.str();
            return false;
        }
        return true;
    }
    default:
        if (!(a == b)) {
            reason = "Value operator== mismatch";
            return false;
        }
        return true;
    }
}

bool compareExecutionResults(
    const std::string& contextLabel,
    const runtime::ExecutionResult& expected,
    const runtime::ExecutionResult& actual,
    std::string& outFailure) {
    if (expected.succeeded != actual.succeeded) {
        outFailure = contextLabel + ": succeeded mismatch (interp=" +
            (expected.succeeded ? "true" : std::string("false:") + runtime::errorCodeName(expected.error.code)) +
            ", jit=" +
            (actual.succeeded ? "true" : std::string("false:") + runtime::errorCodeName(actual.error.code)) + ")";
        return false;
    }
    if (!expected.succeeded) {
        if (expected.error.code != actual.error.code) {
            outFailure = contextLabel + ": ErrorCode mismatch (interp=" +
                std::string(runtime::errorCodeName(expected.error.code)) +
                ", jit=" + std::string(runtime::errorCodeName(actual.error.code)) + ")";
            return false;
        }
        return true;
    }
    std::string bitReason;
    if (!valuesBitwiseEqual(expected.value, actual.value, bitReason)) {
        outFailure = contextLabel + ": " + bitReason;
        return false;
    }
    return true;
}

void writeU16LE(std::vector<std::uint8_t>& buf, std::size_t off, std::uint16_t val) noexcept {
    if (off + 2 > buf.size()) return;
    buf[off] = static_cast<std::uint8_t>(val & 0xFFu);
    buf[off + 1] = static_cast<std::uint8_t>((val >> 8u) & 0xFFu);
}

void writeU32LE(std::vector<std::uint8_t>& buf, std::size_t off, std::uint32_t val) noexcept {
    if (off + 4 > buf.size()) return;
    buf[off] = static_cast<std::uint8_t>(val & 0xFFu);
    buf[off + 1] = static_cast<std::uint8_t>((val >> 8u) & 0xFFu);
    buf[off + 2] = static_cast<std::uint8_t>((val >> 16u) & 0xFFu);
    buf[off + 3] = static_cast<std::uint8_t>((val >> 24u) & 0xFFu);
}

} // namespace

std::string generateRandomProgram(std::uint64_t seed, std::size_t programIndex) {
    SplitMix64 rng(seed ^ (static_cast<std::uint64_t>(programIndex) * 0x9E3779B97F4A7C15ULL));
    const std::string modName = "Fuzz.Prog" + std::to_string(programIndex);

    const std::uint32_t arrLen = rng.rangeU32(4, 12);
    const std::uint32_t loopBound = rng.rangeU32(3, 10);
    const std::int32_t structInitBias = rng.rangeI32(-15, 25);

    std::ostringstream src;
    src << "module " << modName << ";\n\n";

    // 1. Value-type `struct` with fields, constructor, and pure computation method
    src << "public struct FuzzState {\n"
        << "    public int alpha;\n"
        << "    public int beta;\n"
        << "    public double scale;\n"
        << "    public FuzzState(int a, int b, double s) {\n"
        << "        alpha = a;\n"
        << "        beta = b;\n"
        << "        scale = s;\n"
        << "    }\n"
        << "    public int Step(int delta) {\n"
        << "        alpha = unchecked(alpha + delta);\n"
        << "        beta = unchecked(beta * 3 - delta);\n"
        << "        return unchecked(alpha * 2 + beta);\n"
        << "    }\n"
        << "}\n\n";

    // 2. Native-eligible integer + checked/unchecked overflow + branch + bounded loop + 1D packed int[] kernel
    const std::vector<std::string> intVars = {"a", "b", "acc", "i"};
    src << "public int IntArrayKernel(int a, int b) {\n"
        << "    int[] arr = new int[" << arrLen << "];\n"
        << "    int acc = " << makeIntExpr(rng, 2, {"a", "b"}) << ";\n"
        << "    for (int i = 0; i < " << arrLen << "; i = i + 1) {\n"
        << "        arr[i] = unchecked(acc + i * " << rng.rangeI32(2, 9) << " - b);\n"
        << "    }\n"
        << "    int idx = 0;\n"
        << "    while (idx < " << loopBound << ") {\n"
        << "        int slot = idx % " << arrLen << ";\n"
        << "        int i = idx;\n"
        << "        if " << makeBoolCond(rng, intVars) << " {\n"
        << "            acc = " << makeIntExpr(rng, 2, intVars) << ";\n"
        << "            arr[slot] = unchecked(arr[slot] + acc);\n"
        << "        } else {\n"
        << "            acc = unchecked(acc - arr[slot] + " << rng.rangeI32(-9, 13) << ");\n"
        << "        }\n"
        << "        idx = idx + 1;\n"
        << "    }\n";
    if (rng.chance(25)) {
        // Occasionally probe dynamic array indexing (tests IndexOutOfBounds parity on extreme inputs!)
        src << "    if (a == -777) {\n"
            << "        return arr[b];\n"
            << "    }\n";
    }
    src << "    return acc + arr[0] - arr[" << (arrLen - 1) << "];\n"
        << "}\n\n";

    // 3. Native-eligible float/double arithmetic + casts + branch + bounded loop + 1D packed double[] kernel
    const std::vector<std::string> dblVars = {"x", "acc", "d"};
    src << "public double FloatArrayKernel(double x, int steps) {\n"
        << "    double[] buf = new double[" << arrLen << "];\n"
        << "    double acc = " << makeDoubleExpr(rng, 2, {"x"}) << ";\n"
        << "    int limit = steps;\n"
        << "    if (limit < 0) {\n"
        << "        limit = 0;\n"
        << "    }\n"
        << "    if (limit > " << loopBound << ") {\n"
        << "        limit = " << loopBound << ";\n"
        << "    }\n"
        << "    for (int i = 0; i < " << arrLen << "; i = i + 1) {\n"
        << "        double d = (double)i + 0.5;\n"
        << "        buf[i] = " << makeDoubleExpr(rng, 1, dblVars) << ";\n"
        << "    }\n"
        << "    for (int k = 0; k < limit; k = k + 1) {\n"
        << "        double d = buf[k % " << arrLen << "];\n"
        << "        float f = (float)d;\n"
        << "        if ((double)f >= x) {\n"
        << "            acc = acc + " << makeDoubleExpr(rng, 2, dblVars) << ";\n"
        << "        } else {\n"
        << "            acc = acc - ((double)f * 0.5);\n"
        << "        }\n"
        << "    }\n"
        << "    return acc + buf[0];\n"
        << "}\n\n";

    // 4. Struct + array + checked/unchecked hybrid kernel
    src << "public int StructKernel(int x, int y) {\n"
        << "    FuzzState st = new FuzzState(x, y + (" << structInitBias << "), 1.25);\n"
        << "    int[] hist = new int[" << arrLen << "];\n"
        << "    for (int i = 0; i < " << arrLen << "; i = i + 1) {\n"
        << "        if ((i % 2) == 0) {\n"
        << "            st.alpha = st.alpha + i;\n"
        << "        } else {\n"
        << "            st.beta = unchecked(st.beta - x + i);\n"
        << "        }\n"
        << "        hist[i] = unchecked(st.alpha + st.beta);\n"
        << "    }\n"
        << "    int stepVal = st.Step(3);\n"
        << "    int scaled = (int)(st.scale * 4.0);\n"
        << "    return unchecked(stepVal + hist[" << (arrLen / 2) << "] + scaled);\n"
        << "}\n";

    return src.str();
}

DifferentialFuzzReport runDifferentialFuzz(const DifferentialFuzzOptions& options) {
    DifferentialFuzzReport report;
    SplitMix64 rng(options.seed);
    jit::StencilJit stencilJit;

    const std::int64_t i32Max = std::numeric_limits<std::int32_t>::max();
    const std::int64_t i32Min = std::numeric_limits<std::int32_t>::min();

    for (std::size_t pIdx = 0; pIdx < options.programCount; ++pIdx) {
        const std::string modName = "Fuzz.Prog" + std::to_string(pIdx);
        const std::string sourceCode = generateRandomProgram(options.seed, pIdx);
        ++report.programsGenerated;

        compiler::Compilation compilation({{"fuzz_" + std::to_string(pIdx) + ".rscript", sourceCode}});
        auto built = compilation.build();
        if (built.diagnostics.hasErrors()) {
            std::string msg = "Generated fuzz program #" + std::to_string(pIdx) + " failed to compile:";
            for (const auto& d : built.diagnostics.items()) {
                msg += " [" + d.code + ": " + d.message + "]";
            }
            report.failures.push_back(std::move(msg));
            continue;
        }

        // Alternate between O0 and O2 optimization to fuzz both unoptimized and SROA/folded pipelines
        optimization::Options optOpts;
        optOpts.level = (pIdx % 2 == 0)
            ? optimization::Level::Aggressive
            : optimization::Level::None;
        if (optOpts.level != optimization::Level::None) {
            auto optRes = optimization::Optimizer{}.optimize(
                std::move(built.modules), built.diagnostics, optOpts);
            built.modules = std::move(optRes.modules);
        }

        bytecode::Lowerer lowerer;
        std::vector<bytecode::Module> bcModules;
        bool bcValid = true;
        for (const auto& mirMod : built.modules) {
            auto bc = lowerer.lower(mirMod);
            diagnostics::DiagnosticBag verBag;
            if (!bytecode::verifyModule(bc, verBag)) {
                report.failures.push_back(
                    "Generated fuzz program #" + std::to_string(pIdx) + " failed bytecode verification");
                bcValid = false;
                break;
            }
            bcModules.push_back(std::move(bc));
        }
        if (!bcValid) continue;

        runtime::RuntimeError linkErr;
        auto linked = runtime::ProgramImage::link(std::move(bcModules), linkErr);
        if (!linked) {
            report.failures.push_back(
                "ProgramImage::link failed on fuzz program #" + std::to_string(pIdx) + ": " + linkErr.message);
            continue;
        }
        const auto program = std::make_shared<const runtime::ProgramImage>(std::move(*linked));

        jit::StencilJitOptions nativeOpts;
        nativeOpts.enableNativeCodegen = true;
        nativeOpts.enableThreadedFallback = true;
        nativeOpts.hotnessThreshold = 0;
        const auto nativeCompiled = stencilJit.compile(program, nativeOpts);
        if (!nativeCompiled.succeeded()) {
            report.failures.push_back(
                "StencilJit (native) failed on fuzz program #" + std::to_string(pIdx) + ": " + nativeCompiled.error);
            continue;
        }
        report.nativeX64FunctionsCompiled += nativeCompiled.nativeFunctionCount;

        jit::StencilJitOptions threadedOpts;
        threadedOpts.enableNativeCodegen = false;
        threadedOpts.enableThreadedFallback = true;
        threadedOpts.hotnessThreshold = 0;
        const auto threadedCompiled = stencilJit.compile(program, threadedOpts);
        if (!threadedCompiled.succeeded()) {
            report.failures.push_back(
                "StencilJit (threaded) failed on fuzz program #" + std::to_string(pIdx) + ": " + threadedCompiled.error);
            continue;
        }
        report.threadedFunctionsCompiled += threadedCompiled.threadedFunctionCount;

        runtime::ExecutionOptions execOpts;
        execOpts.limits.instructionBudget = options.instructionBudget;

        const auto runTriple = [&](const std::string& fnQual, const std::vector<runtime::Value>& args, std::size_t caseNo) {
            auto interpHeap = std::make_shared<runtime::ManagedHeap>();
            auto nativeHeap = std::make_shared<runtime::ManagedHeap>();
            auto threadHeap = std::make_shared<runtime::ManagedHeap>();

            runtime::Interpreter interp(program, interpHeap);
            nativeCompiled.module->setHeap(nativeHeap);
            threadedCompiled.module->setHeap(threadHeap);

            const auto exp = interp.invoke(fnQual, args, execOpts);
            const auto actNative = nativeCompiled.module->invoke(fnQual, args, execOpts);
            const auto actThreaded = threadedCompiled.module->invoke(fnQual, args, execOpts);
            ++report.invocationsExecuted;

            if (!exp.succeeded) {
                ++report.runtimeErrorsMatched;
            }

            std::string errMsg;
            if (!compareExecutionResults(fnQual + "[native#" + std::to_string(caseNo) + "]", exp, actNative, errMsg)) {
                report.failures.push_back(errMsg);
            }
            if (!compareExecutionResults(fnQual + "[threaded#" + std::to_string(caseNo) + "]", exp, actThreaded, errMsg)) {
                report.failures.push_back(errMsg);
            }
        };

        // Test 1: IntArrayKernel (normal + zero + overflow + out-of-bounds triggers)
        {
            const std::string fn = modName + "::IntArrayKernel";
            ++report.functionsTested;
            std::vector<std::vector<runtime::Value>> inputs = {
                {std::int64_t{0}, std::int64_t{0}},
                {i32Max, std::int64_t{5}},       // Checked overflow stress!
                {i32Min, std::int64_t{-3}},      // Checked underflow stress!
                {std::int64_t{-777}, std::int64_t{99}}, // Out-of-bounds array trap stress!
            };
            while (inputs.size() < options.inputsPerFunction) {
                inputs.push_back({
                    static_cast<std::int64_t>(rng.rangeI32(-200, 200)),
                    static_cast<std::int64_t>(rng.rangeI32(-50, 50)),
                });
            }
            for (std::size_t ci = 0; ci < inputs.size(); ++ci) {
                runTriple(fn, inputs[ci], ci);
            }
        }

        // Test 2: FloatArrayKernel
        {
            const std::string fn = modName + "::FloatArrayKernel";
            ++report.functionsTested;
            std::vector<std::vector<runtime::Value>> inputs = {
                {0.0, std::int64_t{4}},
                {-3.25, std::int64_t{8}},
                {12.5, std::int64_t{0}},
            };
            while (inputs.size() < options.inputsPerFunction) {
                const double dv = static_cast<double>(rng.rangeI32(-40, 40)) * 0.25;
                inputs.push_back({dv, static_cast<std::int64_t>(rng.rangeI32(1, 10))});
            }
            for (std::size_t ci = 0; ci < inputs.size(); ++ci) {
                runTriple(fn, inputs[ci], ci);
            }
        }

        // Test 3: StructKernel
        {
            const std::string fn = modName + "::StructKernel";
            ++report.functionsTested;
            std::vector<std::vector<runtime::Value>> inputs = {
                {std::int64_t{7}, std::int64_t{-4}},
                {i32Max, std::int64_t{1}},
            };
            while (inputs.size() < options.inputsPerFunction) {
                inputs.push_back({
                    static_cast<std::int64_t>(rng.rangeI32(-100, 100)),
                    static_cast<std::int64_t>(rng.rangeI32(-100, 100)),
                });
            }
            for (std::size_t ci = 0; ci < inputs.size(); ++ci) {
                runTriple(fn, inputs[ci], ci);
            }
        }
    }

    return report;
}

BytecodeMutationFuzzReport runBytecodeMutationFuzz(const BytecodeMutationFuzzOptions& options) {
    BytecodeMutationFuzzReport report;
    SplitMix64 rng(options.seed);

    // Build diverse seed bytecode modules covering structs, arrays, branches, loops, and exception handlers
    const std::vector<std::string> seedSources = {
        generateRandomProgram(options.seed, 0),
        R"RS(
module Fuzz.BytecodeSeed;
public class SeedErr {
    public int code;
}
public int RunSeed(int x) {
    try {
        int[] data = new int[4];
        data[0] = x + 1;
        if (x < 0) {
            SeedErr e = new SeedErr();
            e.code = x;
            throw e;
        }
        return data[0];
    } catch (SeedErr ex) {
        return ex.code;
    }
}
)RS",
    };

    std::vector<bytecode::Module> seedModules;
    std::vector<std::vector<std::uint8_t>> seedBlobs;
    for (std::size_t si = 0; si < seedSources.size(); ++si) {
        compiler::Compilation comp({{"seed_" + std::to_string(si) + ".rscript", seedSources[si]}});
        auto built = comp.build();
        if (built.diagnostics.hasErrors() || built.modules.empty()) {
            report.failures.push_back("Failed to compile bytecode seed module #" + std::to_string(si));
            return report;
        }
        bytecode::Lowerer lowerer;
        auto mod = lowerer.lower(built.modules.front());
        diagnostics::DiagnosticBag verBag;
        if (!bytecode::verifyModule(mod, verBag)) {
            report.failures.push_back("Seed bytecode module failed initial verification");
            return report;
        }
        seedBlobs.push_back(bytecode::encodeModule(mod));
        seedModules.push_back(std::move(mod));
    }

    const auto checkDecodeAndVerify = [&](
        const std::vector<std::uint8_t>& mutatedBytes,
        bool mustReject,
        const char* tag) {
        ++report.totalMutations;
        try {
            bytecode::Module decoded;
            diagnostics::DiagnosticBag diags;
            const bool decodedOk = bytecode::decodeModule(mutatedBytes, decoded, diags);
            bool verifiedOk = false;
            if (decodedOk && !diags.hasErrors()) {
                verifiedOk = bytecode::verifyModule(decoded, diags) && !diags.hasErrors();
            }
            if (mustReject && verifiedOk) {
                report.failures.push_back(
                    std::string("Corrupted .rsbc mutation unexpectedly passed decode+verify: ") + tag);
                return;
            }
            if (!verifiedOk) {
                ++report.rejectedSafelyCount;
            } else {
                ++report.validAfterBenignMutationCount;
                // Even if a random bit-flip hit a benign constant/hash, linking & running bounded must not crash
                runtime::RuntimeError linkErr;
                if (auto img = runtime::ProgramImage::link({std::move(decoded)}, linkErr)) {
                    const auto prog = std::make_shared<const runtime::ProgramImage>(std::move(*img));
                    if (!prog->modules().empty() && !prog->modules().front().functions.empty()) {
                        runtime::Interpreter interp(prog);
                        runtime::Limits lim;
                        lim.instructionBudget = 5000;
                        lim.recursionLimit = 32;
                        const auto& fn = prog->modules().front().functions.front();
                        std::vector<runtime::Value> dummyArgs;
                        for (const auto pt : fn.parameterTypes) {
                            if (pt == semantic::PrimitiveType::Int) dummyArgs.emplace_back(std::int64_t{1});
                            else if (pt == semantic::PrimitiveType::Double) dummyArgs.emplace_back(1.0);
                            else break;
                        }
                        if (dummyArgs.size() == fn.parameterTypes.size()) {
                            (void)interp.invoke(fn.symbolId, dummyArgs, lim);
                        }
                    }
                }
            }
        } catch (const std::exception& ex) {
            report.failures.push_back(
                std::string("Unhandled exception during decode/verify on ") + tag + ": " + ex.what());
        } catch (...) {
            report.failures.push_back(
                std::string("Unhandled unknown exception during decode/verify on ") + tag);
        }
    };

    for (std::size_t si = 0; si < seedModules.size(); ++si) {
        const auto& validBytes = seedBlobs[si];
        const auto& validMod = seedModules[si];

        // 1. Magic & Version & Section Directory Header Tampering (mustReject = true)
        {
            // Corrupt magic "RSBC"
            for (std::size_t off = 0; off < 4 && off < validBytes.size(); ++off) {
                auto m = validBytes;
                m[off] ^= 0x5Au;
                ++report.headerVersionMutations;
                checkDecodeAndVerify(m, true, "magic-tamper");
            }
            // Corrupt major & minor version
            for (const auto& [maj, minVer] : std::vector<std::pair<std::uint16_t, std::uint16_t>>{
                     {1, 7}, {0, 0}, {0, 5}, {0, 99}, {0xFFFFu, 0xFFFFu}}) {
                auto m = validBytes;
                writeU16LE(m, 4, maj);
                writeU16LE(m, 6, minVer);
                ++report.headerVersionMutations;
                checkDecodeAndVerify(m, true, "version-tamper");
            }
            // Corrupt sectionCount & section offsets/sizes
            for (std::uint32_t badSecCount : {0u, 3u, 99u, 0xFFFFFFFFu}) {
                auto m = validBytes;
                writeU32LE(m, 12, badSecCount);
                ++report.headerVersionMutations;
                checkDecodeAndVerify(m, true, "section-count-tamper");
            }
            if (validBytes.size() >= 28) {
                auto m = validBytes;
                writeU32LE(m, 20, 0xFFFFFFF0u); // section offset out of bounds
                writeU32LE(m, 24, 0x100u);
                ++report.headerVersionMutations;
                checkDecodeAndVerify(m, true, "section-offset-overflow");
            }
        }

        // 2. Systematic & Random Truncations (mustReject = true)
        {
            const std::vector<std::size_t> fixedCutPoints = {
                0, 1, 3, 4, 8, 15, 16, 28, validBytes.size() / 4,
                validBytes.size() / 2, (validBytes.size() * 3) / 4,
                validBytes.size() > 0 ? (validBytes.size() - 1) : 0
            };
            for (const std::size_t cut : fixedCutPoints) {
                if (cut >= validBytes.size()) continue;
                std::vector<std::uint8_t> truncated(validBytes.begin(), validBytes.begin() + static_cast<std::ptrdiff_t>(cut));
                ++report.truncationMutations;
                checkDecodeAndVerify(truncated, true, "truncation-fixed");
            }
            for (std::size_t k = 0; k < 20 && validBytes.size() > 1; ++k) {
                const std::size_t cut = rng.rangeU32(0, static_cast<std::uint32_t>(validBytes.size() - 1));
                std::vector<std::uint8_t> truncated(validBytes.begin(), validBytes.begin() + static_cast<std::ptrdiff_t>(cut));
                ++report.truncationMutations;
                checkDecodeAndVerify(truncated, true, "truncation-random");
            }
        }

        // 3. Out-Of-Bounds Register Indices (both structural .rsbc & raw SectionCode binary patching, mustReject = true)
        if (!validMod.functions.empty() && !validMod.functions.front().blocks.empty()) {
            // (a) Instruction result register out of bounds
            if (!validMod.functions.front().blocks.front().instructions.empty()) {
                auto mMod = validMod;
                mMod.functions.front().blocks.front().instructions.front().result = 99999u;
                ++report.outOfBoundsRegisterMutations;
                checkDecodeAndVerify(bytecode::encodeModule(mMod), true, "oob-result-register");
            }
            // (b) Instruction operand register out of bounds
            {
                auto mMod = validMod;
                bool patchedOperand = false;
                for (auto& fn : mMod.functions) {
                    for (auto& blk : fn.blocks) {
                        for (auto& inst : blk.instructions) {
                            if (!inst.operands.empty()) {
                                std::vector<bytecode::Register> regs(inst.operands.begin(), inst.operands.end());
                                regs[0] = 88888u;
                                inst.operands.assign(regs.data(), regs.size());
                                patchedOperand = true;
                                break;
                            }
                        }
                        if (patchedOperand) break;
                    }
                    if (patchedOperand) break;
                }
                if (patchedOperand) {
                    ++report.outOfBoundsRegisterMutations;
                    checkDecodeAndVerify(bytecode::encodeModule(mMod), true, "oob-operand-register");
                }
            }
            // (c) Terminator value / condition register out of bounds
            {
                auto mMod = validMod;
                for (auto& blk : mMod.functions.front().blocks) {
                    if (blk.terminator.kind == bytecode::TerminatorKind::ReturnValue) {
                        blk.terminator.value = 77777u;
                        ++report.outOfBoundsRegisterMutations;
                        checkDecodeAndVerify(bytecode::encodeModule(mMod), true, "oob-return-register");
                        break;
                    }
                }
            }
            // (d) Local variable slot out of bounds on LoadLocal / StoreLocal
            {
                auto mMod = validMod;
                for (auto& blk : mMod.functions.front().blocks) {
                    for (auto& inst : blk.instructions) {
                        if (inst.opcode == bytecode::Opcode::LoadLocal ||
                            inst.opcode == bytecode::Opcode::StoreLocal) {
                            inst.index = 65432u;
                            ++report.outOfBoundsRegisterMutations;
                            checkDecodeAndVerify(bytecode::encodeModule(mMod), true, "oob-local-slot");
                            break;
                        }
                    }
                }
            }
        }

        // 4. Out-Of-Bounds Block Indices (Terminator targets, ExceptionHandlers, SequencePoints, raw >16-bit target)
        if (!validMod.functions.empty() && !validMod.functions.front().blocks.empty()) {
            // (a) Terminator target block ID out of bounds
            {
                auto mMod = validMod;
                auto& firstBlk = mMod.functions.front().blocks.front();
                firstBlk.terminator.kind = bytecode::TerminatorKind::Jump;
                firstBlk.terminator.target = 4242u;
                ++report.outOfBoundsBlockMutations;
                checkDecodeAndVerify(bytecode::encodeModule(mMod), true, "oob-jump-target-block");
            }
            // (b) Branch falseTarget block ID out of bounds
            {
                auto mMod = validMod;
                bool patchedBranch = false;
                for (auto& fn : mMod.functions) {
                    for (auto& blk : fn.blocks) {
                        if (blk.terminator.kind == bytecode::TerminatorKind::Branch) {
                            blk.terminator.falseTarget = 9001u;
                            patchedBranch = true;
                            break;
                        }
                    }
                    if (patchedBranch) break;
                }
                if (patchedBranch) {
                    ++report.outOfBoundsBlockMutations;
                    checkDecodeAndVerify(bytecode::encodeModule(mMod), true, "oob-branch-false-target");
                }
            }
            // (c) ExceptionHandler handlerBlock out of bounds
            if (!validMod.functions.front().exceptionHandlers.empty()) {
                auto mMod = validMod;
                mMod.functions.front().exceptionHandlers.front().handlerBlock = 55555u;
                ++report.outOfBoundsBlockMutations;
                checkDecodeAndVerify(bytecode::encodeModule(mMod), true, "oob-exception-handler-block");
            }
            // (d) Debug sequencePoint blockId out of bounds
            if (!validMod.functions.front().debugInfo.sequencePoints.empty()) {
                auto mMod = validMod;
                mMod.functions.front().debugInfo.sequencePoints.front().blockId = 60000u;
                ++report.outOfBoundsBlockMutations;
                checkDecodeAndVerify(bytecode::encodeModule(mMod), true, "oob-sequence-point-block");
            }
        }

        // 5. Random Bit-Flip Fuzzing across the entire .rsbc binary
        for (std::size_t mIdx = 0; mIdx < options.mutationsPerSeedModule && !validBytes.empty(); ++mIdx) {
            auto mutated = validBytes;
            const std::uint32_t flipCount = rng.rangeU32(1, 4);
            for (std::uint32_t f = 0; f < flipCount; ++f) {
                const std::size_t bytePos = rng.rangeU32(0, static_cast<std::uint32_t>(mutated.size() - 1));
                const auto bitMask = static_cast<std::uint8_t>(1u << rng.rangeU32(0, 7));
                mutated[bytePos] ^= bitMask;
            }
            ++report.bitFlipMutations;
            checkDecodeAndVerify(mutated, false, "random-bit-flip");
        }
    }

    return report;
}

} // namespace realscript::tooling
