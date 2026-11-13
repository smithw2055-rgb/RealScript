#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/jit/Jit.h"
#include "realscript/optimization/Optimizer.h"
#include "realscript/runtime/Runtime.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace realscript;

namespace {

void requireCond(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(
            "phase4_stencil_jit_arm64_tests FAILED: " + message);
    }
}

std::string formatDiagnostics(const diagnostics::DiagnosticBag& bag) {
    std::string out;
    for (const auto& item : bag.items()) {
        out += "  " + item.code + ": " + item.message + "\n";
    }
    return out;
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
        requireCond(actual.value == expected.value, label + ": return value mismatch");
        requireCond(
            actual.instructionsExecuted == expected.instructionsExecuted,
            label + ": instructionsExecuted mismatch (" +
                std::to_string(actual.instructionsExecuted) + " vs " +
                std::to_string(expected.instructionsExecuted) + ")");
        requireCond(
            actual.statistics.branchesTaken == expected.statistics.branchesTaken,
            label + ": branchesTaken mismatch (" +
                std::to_string(actual.statistics.branchesTaken) + " vs " +
                std::to_string(expected.statistics.branchesTaken) + ")");
        requireCond(
            actual.statistics.functionCalls == expected.statistics.functionCalls,
            label + ": functionCalls mismatch (" +
                std::to_string(actual.statistics.functionCalls) + " vs " +
                std::to_string(expected.statistics.functionCalls) + ")");
        requireCond(
            actual.determinismDigest == expected.determinismDigest,
            label + ": determinismDigest mismatch");
    } else {
        requireCond(
            actual.error.code == expected.error.code,
            label + ": ErrorCode mismatch (" +
                std::string(runtime::errorCodeName(actual.error.code)) + " [" + actual.error.message + "] vs " +
                std::string(runtime::errorCodeName(expected.error.code)) + " [" + expected.error.message + "])");
        requireCond(
            actual.instructionsExecuted == expected.instructionsExecuted,
            label + ": error instructionsExecuted mismatch (" +
                std::to_string(actual.instructionsExecuted) + " vs " +
                std::to_string(expected.instructionsExecuted) + ")");
    }
}

std::uint32_t readWordLE(const std::vector<std::uint8_t>& code, std::size_t byteOff) {
    std::uint32_t w = 0;
    std::memcpy(&w, code.data() + byteOff, sizeof(w));
    return w;
}

// Conservative check that NO emitted AArch64 instruction uses platform-reserved register X18/W18
// (reserved on Apple macOS/iOS Darwin ARM64 and Windows ARM64).
void verifyArm64BinaryInvariants(
    const std::string& fnName,
    const std::vector<std::uint8_t>& code,
    bool expectNeon128Copy = false) {
    requireCond(
        code.size() >= 64u && (code.size() % 4u) == 0u,
        fnName + ": invalid ARM64 code buffer size " + std::to_string(code.size()));

    // Exact AAPCS64 / Darwin ARM64 64-byte stack frame prologue
    requireCond(readWordLE(code, 0) == 0xD10103FFu, fnName + ": expected `sub sp, sp, #64` (0xD10103FF)");
    requireCond(readWordLE(code, 4) == 0xA9007BFDu, fnName + ": expected `stp x29, x30, [sp]` (0xA9007BFD)");
    requireCond(readWordLE(code, 8) == 0x910003FDu, fnName + ": expected `mov x29, sp` (0x910003FD)");
    requireCond(readWordLE(code, 12) == 0xA90153F3u, fnName + ": expected `stp x19, x20, [sp, #16]`");
    requireCond(readWordLE(code, 16) == 0xA9025BF5u, fnName + ": expected `stp x21, x22, [sp, #32]`");
    requireCond(readWordLE(code, 20) == 0xA90363F7u, fnName + ": expected `stp x23, x24, [sp, #48]`");

    bool sawRet = false;
    bool sawNeonLdrQ = false;
    bool sawNeonStrQ = false;

    for (std::size_t off = 0; off < code.size(); off += 4) {
        const std::uint32_t insn = readWordLE(code, off);
        if (insn == 0xD65F03C0u) {
            sawRet = true;
            continue;
        }
        // Unconditional B / BL (op0 top bits 000101 / 100101), B.cond (01010100), NOP/BRK have no GP Rd/Rn X18
        if ((insn & 0x7C000000u) == 0x14000000u ||
            (insn & 0xFF000010u) == 0x54000000u ||
            insn == 0xD503201Fu ||
            (insn & 0xFFE0001Fu) == 0xD4200000u) {
            continue;
        }
        // CBZ / CBNZ: Rt is bits [4:0]
        if ((insn & 0x7E000000u) == 0x34000000u) {
            requireCond((insn & 0x1Fu) != 18u, fnName + ": CBZ/CBNZ touched reserved X18");
            continue;
        }
        // TBZ / TBNZ: Rt is bits [4:0]
        if ((insn & 0x7E000000u) == 0x36000000u) {
            requireCond((insn & 0x1Fu) != 18u, fnName + ": TBZ/TBNZ touched reserved X18");
            continue;
        }
        // BLR / BR: Rn is bits [9:5]
        if ((insn & 0xFFFFFC1Fu) == 0xD63F0000u || (insn & 0xFFFFFC1Fu) == 0xD61F0000u) {
            requireCond(((insn >> 5) & 0x1Fu) != 18u, fnName + ": BR/BLR touched reserved X18");
            continue;
        }
        // Check 128-bit NEON LDR Q / STR Q unsigned immediate:
        // LDR Qt, [Xn, #imm]: 0x3DC00000 | (imm12 << 10) | (Rn << 5) | Rt
        // STR Qt, [Xn, #imm]: 0x3D800000 | (imm12 << 10) | (Rn << 5) | Rt
        if ((insn & 0xFFC00000u) == 0x3DC00000u) {
            sawNeonLdrQ = true;
            requireCond(((insn >> 5) & 0x1Fu) != 18u, fnName + ": LDR Q base touched X18");
            continue;
        }
        if ((insn & 0xFFC00000u) == 0x3D800000u) {
            sawNeonStrQ = true;
            requireCond(((insn >> 5) & 0x1Fu) != 18u, fnName + ": STR Q base touched X18");
            continue;
        }
        // Move wide immediate (MOVZ/MOVN/MOVK): Rd is bits [4:0]
        if ((insn & 0x1F800000u) == 0x12800000u) {
            requireCond((insn & 0x1Fu) != 18u, fnName + ": MOVZ/MOVN/MOVK wrote reserved X18");
            continue;
        }
        // Scalar FP / SIMD instructions have bit 26 (V) == 1; for non-SIMD GP instructions (V == 0),
        // check Rd/Rt [4:0] and Rn [9:5].
        const bool isSimdFp = ((insn >> 26) & 1u) != 0u;
        if (!isSimdFp) {
            const std::uint32_t rdOrRt = insn & 0x1Fu;
            const std::uint32_t rn = (insn >> 5) & 0x1Fu;
            requireCond(
                rdOrRt != 18u && rn != 18u,
                fnName + ": GP instruction at offset " + std::to_string(off) + " touched reserved X18");
        }
    }

    requireCond(sawRet, fnName + ": missing `ret` (0xD65F03C0) in ARM64 machine code");
    if (expectNeon128Copy) {
        requireCond(
            sawNeonLdrQ && sawNeonStrQ,
            fnName + ": expected 128-bit NEON LDR Q / STR Q in large struct copy loop");
    }
}

constexpr const char* kArm64ScalarAndConvSource = R"RS(
module Phase4.Arm64Scalar;

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
public int NegAndNotOp(int a, bool flag) {
    return (!flag) ? unchecked(-a) : a;
}

public float FloatPoly(float x, int steps) {
    float acc = 0.0f;
    for (int i = 1; i <= steps; i = i + 1) {
        float f = (float)i;
        if (f * x >= 2.5f) {
            acc = acc + (f * x) / (f + 1.0f);
        } else {
            acc = acc - (x - 0.5f);
        }
    }
    return -acc;
}

public bool FloatNanCheck(float a, float b) {
    float z = 0.0f;
    float nanVal = z / z;
    bool c1 = nanVal < a;
    bool c2 = nanVal <= b;
    bool c3 = nanVal > a;
    bool c4 = nanVal >= b;
    bool c5 = nanVal == a;
    bool c6 = nanVal != b;
    return (!c1) && (!c2) && (!c3) && (!c4) && (!c5) && c6 && (a < b);
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
    return -acc;
}

public bool DoubleNanCheck(double a, double b) {
    double nanVal = 0.0 / 0.0;
    bool c1 = nanVal < a;
    bool c2 = nanVal <= b;
    bool c3 = nanVal > a;
    bool c4 = nanVal >= b;
    bool c5 = nanVal == a;
    bool c6 = nanVal != b;
    return (!c1) && (!c2) && (!c3) && (!c4) && (!c5) && c6 && (a < b);
}

public int ConvFloatToInt(float v) {
    return (int)v;
}

public float ConvDoubleToFloat(double v) {
    return (float)v;
}

public int ConvLongToInt(long v) {
    return (int)v;
}

public double ConvLongAndFloatChain(int x, long bigConst) {
    long wide = (long)x;
    long fixedBig = 10000000000;
    double d1 = (double)wide;
    double d2 = (double)bigConst;
    float f1 = (float)x + 0.75f;
    double d3 = (double)f1 * 2.0;
    int back = (int)((float)d3);
    if (bigConst == fixedBig) {
        return d1 + d2 + (double)back;
    }
    return d1 - d2 + (double)back;
}

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
)RS";

constexpr const char* kArm64LargeStructAndArraySource = R"RS(
module Phase4.Arm64Structs;

public enum RoleKind {
    Idle = 0,
    Scout = 1,
    Tank = 2
}

public class PayloadNode {
    public int weight;
}

public struct SubVec10 {
    public int a0; public int a1; public int a2; public int a3; public int a4;
    public int a5; public int a6; public int a7; public int a8; public int a9;

    public int Bump(int d) {
        this.a0 += d;
        this.a9 += d * 2;
        return this.a0 + this.a9;
    }
}

public struct MegaStruct32 {
    public int f0;  public int f1;  public int f2;  public int f3;
    public int f4;  public int f5;  public int f6;  public int f7;
    public int f8;  public int f9;  public int f10; public int f11;
    public int f12; public int f13; public int f14; public int f15;
    public int f16; public int f17; public int f18; public int f19;
    public int f20; public int f21; public int f22; public int f23;
    public int f24; public int f25; public int f26; public int f27;
    public int f28; public int f29; public int f30; public int f31;
}

public struct SuperFrame64 {
    public MegaStruct32 left;
    public MegaStruct32 right;
}

public struct SimStats20 {
    public int s0;  public int s1;  public int s2;  public int s3;  public int s4;
    public int s5;  public int s6;  public int s7;  public int s8;  public int s9;
    public int s10; public int s11; public int s12; public int s13; public int s14;
    public int s15; public int s16; public int s17; public int s18; public int s19;
}

public struct FlatAgent {
    public string tag;
    public RoleKind role;
    public PayloadNode node;
    public SimStats20 stats;
    public double energy;
}

public void MutateMegaByRef(ref MegaStruct32 m, int step) {
    m.f0 += step;
    m.f16 += step * 2;
    m.f31 += step * 3;
}

public int MegaKernel(int rounds) {
    MegaStruct32 cur = new MegaStruct32();
    cur.f0 = 3;
    cur.f7 = 5;
    cur.f15 = 11;
    cur.f16 = 13;
    cur.f23 = 17;
    cur.f31 = 19;

    for (int i = 0; i < rounds; i = i + 1) {
        cur.f0 += 1;
        cur.f15 += 2;
        cur.f16 += 3;
        cur.f23 += 4;
        cur.f31 += 5;
        MutateMegaByRef(ref cur, (i % 4) + 1);
    }
    return cur.f0 + cur.f7 + cur.f15 + cur.f16 + cur.f23 + cur.f31;
}

public int SuperFrameKernel(int rounds) {
    SuperFrame64 a = new SuperFrame64();
    SuperFrame64 b = new SuperFrame64();
    a.left.f0 = 10;
    a.left.f19 = 20;
    a.right.f25 = 30;
    a.right.f31 = 40;

    for (int i = 0; i < rounds; i = i + 1) {
        a.left.f19 += 2;
        a.right.f31 += 3;
        b = a;
        a.left.f0 = b.right.f31 - b.left.f19;
    }
    return b.left.f0 + b.left.f19 + b.right.f25 + b.right.f31;
}

// >384 total frame slots (>6144-byte displacement) to exercise A64Emitter::resolveScaledAddr via X9!
public int HugeFrameDisplacementKernel(int rounds) {
    SuperFrame64 s0 = new SuperFrame64();
    SuperFrame64 s1 = new SuperFrame64();
    SuperFrame64 s2 = new SuperFrame64();
    SuperFrame64 s3 = new SuperFrame64();
    SuperFrame64 s4 = new SuperFrame64();
    SuperFrame64 s5 = new SuperFrame64();
    s0.left.f0 = 1;
    s1.left.f15 = 2;
    s2.right.f0 = 3;
    s3.right.f31 = 4;
    s4.left.f31 = 5;
    s5.right.f31 = 6;
    int tailAcc = 7;
    double tailFlt = 1.25;
    for (int i = 0; i < rounds; i = i + 1) {
        s0.left.f0 += 1;
        s3.right.f31 += 2;
        s5.right.f31 += s0.left.f0;
        s4 = s5;
        tailAcc += s4.right.f31 + s1.left.f15 + s2.right.f0;
        tailFlt += 0.5;
    }
    return tailAcc + s3.right.f31 + (int)((float)tailFlt);
}

public int RunUnfoldedMethodLoop(int rounds) {
    SubVec10 v = new SubVec10();
    v.a0 = 4;
    v.a9 = 9;
    int acc = 0;
    for (int i = 0; i < rounds; i = i + 1) {
        acc += v.Bump((i % 8) + 1);
    }
    return acc + v.a0 + v.a9;
}

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

public int BoundsTrapProbe(int len, int idx) {
    int[] arr = new int[len];
    arr[0] = 42;
    return arr[idx];
}

public FlatAgent[] AllocFlatAgents(int count) {
    FlatAgent[] arr = new FlatAgent[count];
    for (int i = 0; i < count; i = i + 1) {
        PayloadNode p = new PayloadNode();
        p.weight = (i + 1) * 10;
        arr[i].tag = "agent";
        arr[i].role = (i % 2 == 0) ? RoleKind.Scout : RoleKind.Tank;
        arr[i].node = p;
        arr[i].stats.s0 = i + 1;
        arr[i].stats.s17 = (i + 1) * 2;
        arr[i].stats.s19 = (i + 1) * 3;
        arr[i].energy = 1.5;
    }
    return arr;
}

public int StepFlatAgents(FlatAgent[] agents, int rounds) {
    int n = agents.Length;
    int checksum = 0;
    for (int r = 0; r < rounds; r = r + 1) {
        for (int i = 0; i < n; i = i + 1) {
            agents[i].stats.s0 += 1;
            agents[i].stats.s17 += 2;
            agents[i].stats.s19 += 3;
            agents[i].energy += 0.5;
            FlatAgent snap = agents[i];
            int roleBonus = (snap.role == RoleKind.Tank) ? 2 : 1;
            checksum = (checksum + snap.stats.s0 + snap.stats.s19 + snap.node.weight + roleBonus) % 100000000;
        }
    }
    int eTag = (agents[n - 1].energy >= 10.0) ? 10 : 1;
    return checksum + eTag;
}

public int RelabelAndLinkAgents(FlatAgent[] agents, string suffix) {
    int n = agents.Length;
    for (int i = 0; i < n; i = i + 1) {
        PayloadNode nextNode = new PayloadNode();
        nextNode.weight = 1000 + i * 7;
        FlatAgent cur = agents[i];
        cur.tag = cur.tag + "_" + suffix;
        cur.node = nextNode;
        cur.stats.s18 = 77 + i;
        agents[i] = cur;
    }
    return n;
}

public int VerifyPostGcAgents(FlatAgent[] agents, int expectedTagLen) {
    int n = agents.Length;
    int sum = 0;
    for (int i = 0; i < n; i = i + 1) {
        FlatAgent cur = agents[i];
        if (cur.tag.Length != expectedTagLen) {
            return -100 - i;
        }
        sum += cur.node.weight + cur.stats.s18;
    }
    return sum;
}

public SimStats20[] AllocBlittableStats(int count) {
    SimStats20[] arr = new SimStats20[count];
    for (int i = 0; i < count; i = i + 1) {
        arr[i].s0 = i + 1;
        arr[i].s16 = i + 2;
        arr[i].s19 = i + 3;
    }
    return arr;
}

public int StepBlittableStats(SimStats20[] arr, int rounds) {
    int n = arr.Length;
    int total = 0;
    for (int r = 0; r < rounds; r = r + 1) {
        for (int i = 0; i < n; i = i + 1) {
            arr[i].s0 += 1;
            arr[i].s16 += 2;
            arr[i].s19 += 3;
            SimStats20 s = arr[i];
            total += s.s0 + s.s16 + s.s19;
        }
    }
    return total;
}
)RS";

} // namespace

int main() {
    try {
        auto scalarBuilt = compileScript("phase4_arm64_scalar.rscript", kArm64ScalarAndConvSource);
        auto structBuilt = compileScript("phase4_arm64_structs.rscript", kArm64LargeStructAndArraySource);

        jit::StencilJit sjit;
        auto scalarJit = sjit.compile(scalarBuilt.program);
        requireCond(scalarJit.succeeded(), "StencilJit scalar compile failed: " + scalarJit.error);
        requireCond(
            scalarJit.fallbackFunctionCount == 0u,
            "Expected 0 InterpreterFallback functions in Phase4.Arm64Scalar, got " +
                std::to_string(scalarJit.fallbackFunctionCount));

        auto structJit = sjit.compile(structBuilt.program);
        requireCond(structJit.succeeded(), "StencilJit struct compile failed: " + structJit.error);

        // -------------------------------------------------------------------
        // [1/6] Binary Structural, ABI Prologue/Epilogue, NEON Q-Copy & X18 Audit
        // -------------------------------------------------------------------
        std::cout << "[1/6] Auditing emitted ARM64 (AArch64) machine code, AAPCS64/Darwin frame prologue, NEON Q-copy & X18 reservation...\n";
        for (const char* fn : {
                 "Phase4.Arm64Scalar::CheckedAdd",
                 "Phase4.Arm64Scalar::CheckedMul",
                 "Phase4.Arm64Scalar::DivOp",
                 "Phase4.Arm64Scalar::NegAndNotOp",
                 "Phase4.Arm64Scalar::FloatPoly",
                 "Phase4.Arm64Scalar::FloatNanCheck",
                 "Phase4.Arm64Scalar::DoublePoly",
                 "Phase4.Arm64Scalar::DoubleNanCheck",
                 "Phase4.Arm64Scalar::ConvFloatToInt",
                 "Phase4.Arm64Scalar::ConvDoubleToFloat",
                 "Phase4.Arm64Scalar::ConvLongToInt",
                 "Phase4.Arm64Scalar::ConvLongAndFloatChain",
                 "Phase4.Arm64Scalar::ControlAndInlineLeaf",
             }) {
            const auto a64 = scalarJit.module->inspectMachineCode(fn, jit::StencilTargetArch::Arm64);
            verifyArm64BinaryInvariants(fn, a64, false);
        }
        for (const char* fn : {
                 "Phase4.Arm64Structs::MegaKernel",
                 "Phase4.Arm64Structs::SuperFrameKernel",
                 "Phase4.Arm64Structs::HugeFrameDisplacementKernel",
                 "Phase4.Arm64Structs::RunUnfoldedMethodLoop",
                 "Phase4.Arm64Structs::SieveAndDotProduct",
                 "Phase4.Arm64Structs::StepFlatAgents",
                 "Phase4.Arm64Structs::StepBlittableStats",
             }) {
            const auto a64 = structJit.module->inspectMachineCode(fn, jit::StencilTargetArch::Arm64);
            const bool expectNeon =
                std::string(fn) == "Phase4.Arm64Structs::SuperFrameKernel" ||
                std::string(fn) == "Phase4.Arm64Structs::HugeFrameDisplacementKernel";
            verifyArm64BinaryInvariants(fn, a64, expectNeon);
        }

        // -------------------------------------------------------------------
        // [2/6] Scalar int/long/float/double/bool, Traps, IEEE-754 NaN & Conversions
        // -------------------------------------------------------------------
        std::cout << "[2/6] Verifying ARM64 ISA execution for scalar ALU, `b.vs` overflow, `sdiv` traps, IEEE-754 NaN `fcmp` & conversions...\n";
        runtime::Interpreter scalarVm(scalarBuilt.program);
        const std::int64_t iMax = std::numeric_limits<std::int32_t>::max();
        const std::int64_t iMin = std::numeric_limits<std::int32_t>::min();

        struct ScalarCallCase {
            const char* fn;
            std::vector<runtime::Value> args;
        };
        const std::vector<ScalarCallCase> scalarCases = {
            {"Phase4.Arm64Scalar::CheckedAdd", {std::int64_t{12345}, std::int64_t{67890}}},
            {"Phase4.Arm64Scalar::CheckedAdd", {iMax, std::int64_t{1}}},     // adds w -> b.vs overflow
            {"Phase4.Arm64Scalar::CheckedAdd", {iMin, std::int64_t{-1}}},    // adds w -> b.vs underflow
            {"Phase4.Arm64Scalar::CheckedSub", {iMin, std::int64_t{1}}},     // subs w -> b.vs overflow
            {"Phase4.Arm64Scalar::CheckedSub", {iMax, std::int64_t{-1}}},    // subs w -> b.vs overflow
            {"Phase4.Arm64Scalar::CheckedMul", {std::int64_t{100000}, std::int64_t{100000}}}, // smull+sxtw overflow
            {"Phase4.Arm64Scalar::CheckedMul", {iMin, std::int64_t{-1}}},    // smull+sxtw overflow
            {"Phase4.Arm64Scalar::CheckedMul", {std::int64_t{-40000}, std::int64_t{30000}}},
            {"Phase4.Arm64Scalar::UncheckedAdd", {iMax, std::int64_t{1}}},
            {"Phase4.Arm64Scalar::UncheckedSub", {iMin, std::int64_t{1}}},
            {"Phase4.Arm64Scalar::UncheckedMul", {std::int64_t{100000}, std::int64_t{100000}}},
            {"Phase4.Arm64Scalar::DivOp", {std::int64_t{-100}, std::int64_t{7}}},
            {"Phase4.Arm64Scalar::DivOp", {std::int64_t{42}, std::int64_t{0}}},  // cbz w1 div0 trap
            {"Phase4.Arm64Scalar::DivOp", {iMin, std::int64_t{-1}}},             // INT32_MIN / -1 overflow trap
            {"Phase4.Arm64Scalar::RemOp", {std::int64_t{-100}, std::int64_t{7}}},
            {"Phase4.Arm64Scalar::RemOp", {std::int64_t{42}, std::int64_t{0}}},  // cbz w1 rem0 trap
            {"Phase4.Arm64Scalar::RemOp", {iMin, std::int64_t{-1}}},             // INT32_MIN % -1 == 0
            {"Phase4.Arm64Scalar::NegAndNotOp", {iMin, false}},
            {"Phase4.Arm64Scalar::NegAndNotOp", {std::int64_t{99}, true}},
            {"Phase4.Arm64Scalar::FloatPoly", {1.75f, std::int64_t{160}}},
            {"Phase4.Arm64Scalar::FloatNanCheck", {1.25f, 2.5f}},
            {"Phase4.Arm64Scalar::DoublePoly", {1.75, std::int64_t{200}}},
            {"Phase4.Arm64Scalar::DoubleNanCheck", {1.5, 2.5}},
            {"Phase4.Arm64Scalar::ConvFloatToInt", {-12345.75f}},
            {"Phase4.Arm64Scalar::ConvFloatToInt", {1.0e20f}}, // out-of-int32-range trap
            {"Phase4.Arm64Scalar::ConvFloatToInt", {std::numeric_limits<float>::quiet_NaN()}}, // NaN trap
            {"Phase4.Arm64Scalar::ConvDoubleToFloat", {3.25}},
            {"Phase4.Arm64Scalar::ConvDoubleToFloat", {1.0e300}}, // float overflow -> RangeError trap
            {"Phase4.Arm64Scalar::ConvLongToInt", {runtime::LongValue{-12345678LL}}},
            {"Phase4.Arm64Scalar::ConvLongToInt", {runtime::LongValue{5000000000LL}}}, // >int32 range trap
            {"Phase4.Arm64Scalar::ConvLongAndFloatChain", {std::int64_t{-42000}, runtime::LongValue{10000000000LL}}},
            {"Phase4.Arm64Scalar::ConvLongAndFloatChain", {std::int64_t{1250}, runtime::LongValue{-9999999999LL}}},
            {"Phase4.Arm64Scalar::ControlAndInlineLeaf", {std::int64_t{250}}},
        };

        for (std::size_t idx = 0; idx < scalarCases.size(); ++idx) {
            const auto& c = scalarCases[idx];
            const auto expVm = scalarVm.invoke(c.fn, c.args);
            const auto actHost = scalarJit.module->invoke(c.fn, c.args);
            const auto actArm64 = scalarJit.module->invokeSimulatedArm64(c.fn, c.args);
            assertParity(std::string("Host[") + c.fn + "#" + std::to_string(idx) + "]", actHost, expVm);
            assertParity(std::string("Arm64[") + c.fn + "#" + std::to_string(idx) + "]", actArm64, expVm);
        }

        // -------------------------------------------------------------------
        // [3/6] >384-Slot Large Frame Displacement Materialization via X9
        // -------------------------------------------------------------------
        std::cout << "[3/6] Verifying >384-slot (>6KB) stack frame displacement materialization (`X9` scratch) in ARM64...\n";
        runtime::ExecutionOptions highLimits;
        highLimits.limits.instructionBudget = 500'000'000ULL;
        highLimits.limits.recursionLimit = 256;

        auto vmHeap = std::make_shared<runtime::ManagedHeap>();
        runtime::Interpreter structVm(structBuilt.program, vmHeap);

        auto armHeap = std::make_shared<runtime::ManagedHeap>();
        structJit.module->setHeap(armHeap);

        const auto vmHuge = structVm.invoke(
            "Phase4.Arm64Structs::HugeFrameDisplacementKernel", {std::int64_t{120}}, highLimits);
        requireCond(vmHuge.succeeded, "VM HugeFrameDisplacementKernel failed: " + vmHuge.error.message);
        const auto armHuge = structJit.module->invokeSimulatedArm64(
            "Phase4.Arm64Structs::HugeFrameDisplacementKernel", {std::int64_t{120}}, highLimits);
        assertParity("Arm64[HugeFrameDisplacementKernel]", armHuge, vmHuge);

        // -------------------------------------------------------------------
        // [4/6] >16-Field MegaStruct32, 64-Slot SuperFrame64, StackSlotRef & Unfolded Method
        // -------------------------------------------------------------------
        std::cout << "[4/6] Verifying zero-allocation MegaStruct32, 64-slot SuperFrame64, StackSlotRef & unfolded leaf method on ARM64...\n";
        const auto vmMega = structVm.invoke("Phase4.Arm64Structs::MegaKernel", {std::int64_t{200}}, highLimits);
        const auto vmSuper = structVm.invoke("Phase4.Arm64Structs::SuperFrameKernel", {std::int64_t{200}}, highLimits);
        const auto vmMethod = structVm.invoke("Phase4.Arm64Structs::RunUnfoldedMethodLoop", {std::int64_t{350}}, highLimits);
        requireCond(vmMega.succeeded && vmSuper.succeeded && vmMethod.succeeded, "VM struct kernels failed");

        {
            const std::uint64_t obj0 = armHeap->statistics().objectsAllocated;
            runtime::resetHeapStructStorageAllocations();
            const auto armMega = structJit.module->invokeSimulatedArm64(
                "Phase4.Arm64Structs::MegaKernel", {std::int64_t{200}}, highLimits);
            const auto armSuper = structJit.module->invokeSimulatedArm64(
                "Phase4.Arm64Structs::SuperFrameKernel", {std::int64_t{200}}, highLimits);
            const auto armMethod = structJit.module->invokeSimulatedArm64(
                "Phase4.Arm64Structs::RunUnfoldedMethodLoop", {std::int64_t{350}}, highLimits);
            const std::uint64_t obj1 = armHeap->statistics().objectsAllocated;
            const std::uint64_t boxes = runtime::heapStructStorageAllocations();

            assertParity("Arm64[MegaKernel]", armMega, vmMega);
            assertParity("Arm64[SuperFrameKernel]", armSuper, vmSuper);
            assertParity("Arm64[RunUnfoldedMethodLoop]", armMethod, vmMethod);
            requireCond(
                obj1 == obj0 && boxes == 0u,
                "Expected 0 heap/boxing allocations in ARM64 Mega/Super/Method kernels");
        }

        // -------------------------------------------------------------------
        // [5/6] Packed Arrays, Bounds Traps, BlittableStruct[] & 24-Slot FlatStruct[] + GC
        // -------------------------------------------------------------------
        std::cout << "[5/6] Verifying ARM64 Packed arrays, bounds traps, SimStats20[] & 24-slot FlatAgent[] + post-GC survival...\n";
        const auto vmSieve = structVm.invoke("Phase4.Arm64Structs::SieveAndDotProduct", {std::int64_t{300}}, highLimits);
        const auto armSieve = structJit.module->invokeSimulatedArm64(
            "Phase4.Arm64Structs::SieveAndDotProduct", {std::int64_t{300}}, highLimits);
        assertParity("Arm64[SieveAndDotProduct]", armSieve, vmSieve);

        for (std::int64_t badIdx : {std::int64_t{-1}, std::int64_t{8}, std::int64_t{99}}) {
            const auto vmTrap = structVm.invoke(
                "Phase4.Arm64Structs::BoundsTrapProbe", {std::int64_t{8}, badIdx}, highLimits);
            const auto armTrap = structJit.module->invokeSimulatedArm64(
                "Phase4.Arm64Structs::BoundsTrapProbe", {std::int64_t{8}, badIdx}, highLimits);
            assertParity("Arm64[BoundsTrap#" + std::to_string(badIdx) + "]", armTrap, vmTrap);
        }

        // Blittable SimStats20[]
        const auto vmBlitArr = structVm.invoke("Phase4.Arm64Structs::AllocBlittableStats", {std::int64_t{24}}, highLimits);
        const auto armBlitArr = structJit.module->invokeSimulatedArm64(
            "Phase4.Arm64Structs::AllocBlittableStats", {std::int64_t{24}}, highLimits);
        requireCond(vmBlitArr.succeeded && armBlitArr.succeeded, "AllocBlittableStats failed");
        auto vmBlitRoot = vmHeap->retain(std::get<runtime::ObjectRef>(vmBlitArr.value));
        auto armBlitRoot = armHeap->retain(std::get<runtime::ObjectRef>(armBlitArr.value));
        (void)vmBlitRoot;
        (void)armBlitRoot;

        {
            const std::uint64_t obj0 = armHeap->statistics().objectsAllocated;
            runtime::resetHeapStructStorageAllocations();
            const auto vmBlitStep = structVm.invoke(
                "Phase4.Arm64Structs::StepBlittableStats", {vmBlitArr.value, std::int64_t{60}}, highLimits);
            const auto armBlitStep = structJit.module->invokeSimulatedArm64(
                "Phase4.Arm64Structs::StepBlittableStats", {armBlitArr.value, std::int64_t{60}}, highLimits);
            assertParity("Arm64[StepBlittableStats]", armBlitStep, vmBlitStep);
            requireCond(
                armHeap->statistics().objectsAllocated == obj0 &&
                    runtime::heapStructStorageAllocations() == 0u,
                "Expected 0 allocations in ARM64 StepBlittableStats");
        }

        // 24-slot FlatAgent[] (string + Enum + ObjectRef + 20-field SimStats20 + double) + Full GC
        const auto vmFlatArr = structVm.invoke("Phase4.Arm64Structs::AllocFlatAgents", {std::int64_t{24}}, highLimits);
        const auto armFlatArr = structJit.module->invokeSimulatedArm64(
            "Phase4.Arm64Structs::AllocFlatAgents", {std::int64_t{24}}, highLimits);
        requireCond(vmFlatArr.succeeded && armFlatArr.succeeded, "AllocFlatAgents failed");
        auto vmFlatRoot = vmHeap->retain(std::get<runtime::ObjectRef>(vmFlatArr.value));
        auto armFlatRoot = armHeap->retain(std::get<runtime::ObjectRef>(armFlatArr.value));
        (void)vmFlatRoot;
        (void)armFlatRoot;

        {
            const std::uint64_t obj0 = armHeap->statistics().objectsAllocated;
            runtime::resetHeapStructStorageAllocations();
            const auto vmFlatStep = structVm.invoke(
                "Phase4.Arm64Structs::StepFlatAgents", {vmFlatArr.value, std::int64_t{80}}, highLimits);
            const auto armFlatStep = structJit.module->invokeSimulatedArm64(
                "Phase4.Arm64Structs::StepFlatAgents", {armFlatArr.value, std::int64_t{80}}, highLimits);
            assertParity("Arm64[StepFlatAgents]", armFlatStep, vmFlatStep);
            requireCond(
                armHeap->statistics().objectsAllocated == obj0 &&
                    runtime::heapStructStorageAllocations() == 0u,
                "Expected 0 allocations in ARM64 StepFlatAgents");

            const auto vmRelabel = structVm.invoke(
                "Phase4.Arm64Structs::RelabelAndLinkAgents", {vmFlatArr.value, std::string("a64")}, highLimits);
            const auto armRelabel = structJit.module->invokeSimulatedArm64(
                "Phase4.Arm64Structs::RelabelAndLinkAgents", {armFlatArr.value, std::string("a64")}, highLimits);
            assertParity("Arm64[RelabelAndLinkAgents]", armRelabel, vmRelabel);

            { runtime::ShadowStack ss; vmHeap->collectFull(ss); }
            { runtime::ShadowStack ss; armHeap->collectFull(ss); }

            // "agent" (5) + "_" (1) + "a64" (3) = 9
            const auto vmPostGc = structVm.invoke(
                "Phase4.Arm64Structs::VerifyPostGcAgents", {vmFlatArr.value, std::int64_t{9}}, highLimits);
            const auto armPostGc = structJit.module->invokeSimulatedArm64(
                "Phase4.Arm64Structs::VerifyPostGcAgents", {armFlatArr.value, std::int64_t{9}}, highLimits);
            assertParity("Arm64[VerifyPostGcAgents]", armPostGc, vmPostGc);
        }

        // -------------------------------------------------------------------
        // [6/6] Exact Instruction-Budget Exhaustion & Partial Superinstruction Rollback
        // -------------------------------------------------------------------
        std::cout << "[6/6] Verifying ARM64 unsigned borrow (`subs` + `b.lo`) exact instructionBudget rollback...\n";
        const auto fullRun = scalarVm.invoke(
            "Phase4.Arm64Scalar::ControlAndInlineLeaf", {std::int64_t{60}});
        requireCond(fullRun.succeeded && fullRun.instructionsExecuted > 20, "baseline ControlAndInlineLeaf failed");
        for (std::uint64_t delta : {0ULL, 1ULL, 2ULL, 3ULL, 4ULL, 5ULL, 17ULL}) {
            runtime::ExecutionOptions limOpts;
            limOpts.limits.instructionBudget = fullRun.instructionsExecuted - delta;
            const auto expB = scalarVm.invoke(
                "Phase4.Arm64Scalar::ControlAndInlineLeaf", {std::int64_t{60}}, limOpts);
            const auto armB = scalarJit.module->invokeSimulatedArm64(
                "Phase4.Arm64Scalar::ControlAndInlineLeaf", {std::int64_t{60}}, limOpts);
            assertParity("Arm64Budget-" + std::to_string(delta), armB, expB);
        }

        std::cout << "ALL phase4_stencil_jit_arm64_tests PASSED!\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << "\n";
        return 1;
    }
}
