#include "realscript/aot_cpp/AotCpp.h"
#include "realscript/aot_cpp/AotRuntime.h"
#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/jit/Jit.h"
#include "realscript/optimization/Optimizer.h"
#include "realscript/runtime/Runtime.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <sstream>
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
        throw std::runtime_error(
            "phase3_jit_aot_large_flatstruct_fastpath_tests FAILED: " + message);
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

bool containsByteSequence(
    const std::vector<std::uint8_t>& hay,
    const std::vector<std::uint8_t>& needle) {
    if (needle.empty() || hay.size() < needle.size()) {
        return false;
    }
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool ok = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (hay[i + j] != needle[j]) {
                ok = false;
                break;
            }
        }
        if (ok) {
            return true;
        }
    }
    return false;
}

constexpr const char* kPhase3SuiteSource = R"RS(
module Phase3.FastPathSuite;

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

public int static_cast_int(double v) {
    if (v >= 100.0) {
        return 100;
    }
    if (v >= 10.0) {
        return 10;
    }
    return 1;
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
        auto built = compileScript(
            "phase3_fastpaths.rscript",
            kPhase3SuiteSource,
            optimization::Level::Aggressive);

        runtime::ExecutionOptions highLimits;
        highLimits.limits.instructionBudget = 500'000'000ULL;
        highLimits.limits.recursionLimit = 256;

        // -------------------------------------------------------------------
        // Inspect generated C++17 AOT source for newly unlocked fast paths
        // -------------------------------------------------------------------
        diagnostics::DiagnosticBag aotDiags;
        aot::GenerationOptions genOpts;
        genOpts.programName = "Phase3FastPathProg";
        genOpts.cppNamespace = "phase3_fastpath_ns";
        genOpts.trustedRelease = false;
        aot::CppGenerator generator;
        const auto genOut = generator.generate(built.mirModules, aotDiags, genOpts);
        requireCond(
            !aotDiags.hasErrors() && !genOut.source.empty(),
            "C++17 AOT generation failed:\n" + formatDiagnostics(aotDiags));

                auto findMirScalarSymbol = [&](const std::string& qualifiedName) -> std::string {
            for (const auto& m : built.mirModules) {
                for (const auto& fn : m.functions) {
                    if (fn.name == qualifiedName || (m.name + "::" + fn.name) == qualifiedName) {
                        std::ostringstream oss;
                        oss << "fn_" << std::hex << std::setw(16) << std::setfill('0')
                            << static_cast<std::uint64_t>(fn.symbolId) << "_scalar";
                        return oss.str();
                    }
                }
            }
            throw std::runtime_error("MIR function not found: " + qualifiedName);
        };

        // 1. >16-field direct & 64-slot nested structs MUST enter Typed C++17 AOT (_scalar)
        requireCond(
            genOut.source.find(findMirScalarSymbol("Phase3.FastPathSuite::MegaKernel")) != std::string::npos,
            "Expected MegaKernel (32 fields, fieldIndex >= 16) to emit Typed AOT _scalar");
        requireCond(
            genOut.source.find(findMirScalarSymbol("Phase3.FastPathSuite::SuperFrameKernel")) != std::string::npos,
            "Expected SuperFrameKernel (64-slot nested struct) to emit Typed AOT _scalar");

        // 2. FlatStruct & FlatStruct[] (with string, Enum, ObjectRef, 20-field nested struct) MUST enter Typed C++17 AOT
        requireCond(
            genOut.source.find(findMirScalarSymbol("Phase3.FastPathSuite::StepFlatAgents")) != std::string::npos,
            "Expected StepFlatAgents (FlatStruct[] with string/Enum/ObjectRef) to emit Typed AOT _scalar");
        requireCond(
            genOut.source.find(findMirScalarSymbol("Phase3.FastPathSuite::RelabelAndLinkAgents")) != std::string::npos,
            "Expected RelabelAndLinkAgents (FlatStruct[] GC reference store) to emit Typed AOT _scalar");
        requireCond(
            genOut.source.find(findMirScalarSymbol("Phase3.FastPathSuite::StepBlittableStats")) != std::string::npos,
            "Expected StepBlittableStats (>16-field PackedStruct[]) to emit Typed AOT _scalar");

        // 3. Loop-hoisted TypedStructArrayCache helpers MUST be emitted
        requireCond(
            genOut.source.find("acquireStructArrayCache") != std::string::npos &&
                genOut.source.find("loadElementStructCached") != std::string::npos &&
                genOut.source.find("storeElementStructCached") != std::string::npos,
            "Expected loop-hoisted TypedStructArrayCache (acquire/load/storeElementStructCached) in C++17 AOT");

        // -------------------------------------------------------------------
        // Compile StencilJit (Native x86_64 + ThreadedStencils)
        // -------------------------------------------------------------------
        jit::StencilJit stencilJit;
        jit::StencilJitOptions nativeOpts;
        nativeOpts.enableNativeCodegen = true;
        auto nativeCompiled = stencilJit.compile(built.program, nativeOpts);
        requireCond(
            nativeCompiled.succeeded(),
            "StencilJit native compile failed: " + nativeCompiled.error);

        jit::StencilJitOptions threadedOpts;
        threadedOpts.enableNativeCodegen = false;
        auto threadedCompiled = stencilJit.compile(built.program, threadedOpts);
        requireCond(
            threadedCompiled.succeeded(),
            "StencilJit threaded compile failed: " + threadedCompiled.error);

        if (jit::ExecutableCodeArena::nativeCodeExecutionSupported() &&
            nativeCompiled.module->nativeBackendActive()) {
            for (const char* fn : {
                     "Phase3.FastPathSuite::MegaKernel",
                     "Phase3.FastPathSuite::SuperFrameKernel",
                     "Phase3.FastPathSuite::RunUnfoldedMethodLoop",
                     "Phase3.FastPathSuite::StepFlatAgents",
                     "Phase3.FastPathSuite::StepBlittableStats",
                 }) {
                requireCond(
                    nativeCompiled.module->functionTier(fn) == jit::StencilTier::NativeMachineCode,
                    std::string("Expected NativeMachineCode tier for ") + fn);
            }
            const auto superCode = nativeCompiled.module->inspectMachineCode(
                "Phase3.FastPathSuite::SuperFrameKernel");
            // 0xF3 0x48 0xA5 = rep movsq for >6-slot (64-slot) flat slice copy!
            requireCond(
                containsByteSequence(superCode, {0xF3u, 0x48u, 0xA5u}),
                "Expected x86_64 `rep movsq` (F3 48 A5) in SuperFrameKernel machine code");
        }

        auto vmHeap = std::make_shared<runtime::ManagedHeap>();
        runtime::Interpreter vm(built.program, vmHeap);

        auto jitHeap = std::make_shared<runtime::ManagedHeap>();
        nativeCompiled.module->setHeap(jitHeap);

        auto thHeap = std::make_shared<runtime::ManagedHeap>();
        threadedCompiled.module->setHeap(thHeap);

        // -------------------------------------------------------------------
        // [1/5] >16-Field MegaStruct32 & 64-Slot SuperFrame64 Parity & 0-Alloc
        // -------------------------------------------------------------------
        std::cout << "[1/5] Testing >16-field MegaStruct32 & 64-slot SuperFrame64 in StencilJit (x86_64) & Typed C++17 AOT...\n";
        const auto vmMega = vm.invoke("Phase3.FastPathSuite::MegaKernel", {std::int64_t{250}}, highLimits);
        const auto vmSuper = vm.invoke("Phase3.FastPathSuite::SuperFrameKernel", {std::int64_t{250}}, highLimits);
        requireCond(vmMega.succeeded && vmSuper.succeeded, "VM Mega/Super kernel failed");

        {
            const std::uint64_t obj0 = jitHeap->statistics().objectsAllocated;
            runtime::resetHeapStructStorageAllocations();
            const auto jitMega = nativeCompiled.module->invoke(
                "Phase3.FastPathSuite::MegaKernel", {std::int64_t{250}}, highLimits);
            const auto jitSuper = nativeCompiled.module->invoke(
                "Phase3.FastPathSuite::SuperFrameKernel", {std::int64_t{250}}, highLimits);
            const std::uint64_t obj1 = jitHeap->statistics().objectsAllocated;
            const std::uint64_t boxes = runtime::heapStructStorageAllocations();

            requireCond(jitMega.succeeded && jitMega.value == vmMega.value, "StencilJit MegaKernel mismatch");
            requireCond(jitSuper.succeeded && jitSuper.value == vmSuper.value, "StencilJit SuperFrameKernel mismatch");
            requireCond(
                jitMega.instructionsExecuted == vmMega.instructionsExecuted &&
                    jitSuper.instructionsExecuted == vmSuper.instructionsExecuted,
                "StencilJit instruction accounting mismatch on Mega/Super kernels");
            requireCond(obj1 == obj0 && boxes == 0u, "Expected 0 allocations in StencilJit Mega/Super kernels");
        }
        {
            const auto thMega = threadedCompiled.module->invoke(
                "Phase3.FastPathSuite::MegaKernel", {std::int64_t{250}}, highLimits);
            const auto thSuper = threadedCompiled.module->invoke(
                "Phase3.FastPathSuite::SuperFrameKernel", {std::int64_t{250}}, highLimits);
            requireCond(
                thMega.succeeded && thMega.value == vmMega.value &&
                    thSuper.succeeded && thSuper.value == vmSuper.value,
                "Threaded StencilJit Mega/Super mismatch");
        }

        // -------------------------------------------------------------------
        // [2/5] Pass 4 Leaf Struct-Method Unfolding -> NativeMachineCode
        // -------------------------------------------------------------------
        std::cout << "[2/5] Testing Pass 4 unfolded leaf struct method (SubVec10.Bump) in NativeMachineCode...\n";
        const auto vmMethod = vm.invoke(
            "Phase3.FastPathSuite::RunUnfoldedMethodLoop", {std::int64_t{400}}, highLimits);
        requireCond(vmMethod.succeeded, "VM RunUnfoldedMethodLoop failed");
        {
            const std::uint64_t obj0 = jitHeap->statistics().objectsAllocated;
            runtime::resetHeapStructStorageAllocations();
            const auto jitMethod = nativeCompiled.module->invoke(
                "Phase3.FastPathSuite::RunUnfoldedMethodLoop", {std::int64_t{400}}, highLimits);
            requireCond(
                jitMethod.succeeded && jitMethod.value == vmMethod.value,
                "StencilJit RunUnfoldedMethodLoop mismatch");
            requireCond(
                jitHeap->statistics().objectsAllocated == obj0 &&
                    runtime::heapStructStorageAllocations() == 0u,
                "Expected 0 allocations in RunUnfoldedMethodLoop");
        }

        // -------------------------------------------------------------------
        // [3/5] FlatStruct[] (string + Enum + ObjectRef + 20-field nested) & GC Survival
        // -------------------------------------------------------------------
        std::cout << "[3/5] Testing FlatStruct[] (string + Enum + Object + 20-slot nested) fast paths & GC write barriers...\n";
        const auto vmFlatArr = vm.invoke("Phase3.FastPathSuite::AllocFlatAgents", {std::int64_t{32}}, highLimits);
        requireCond(vmFlatArr.succeeded, "VM AllocFlatAgents failed");
        auto vmFlatRoot = vmHeap->retain(std::get<runtime::ObjectRef>(vmFlatArr.value));
        (void)vmFlatRoot;

        // Verify runtime array storage is genuinely SlotStorage::FlatStruct with stride == 24
        {
            runtime::FlatStructArrayFastView view{};
            requireCond(
                vmHeap->tryBindFlatStructArrayFastView(
                    std::get<runtime::ObjectRef>(vmFlatArr.value), view),
                "FlatAgent[] must use SlotStorage::FlatStruct");
            requireCond(
                view.length == 32u && view.flatStride == 24u,
                "Expected FlatAgent[] length=32 and flat leaf stride=24, got stride=" +
                    std::to_string(view.flatStride));
        }

        const auto vmFlatStep = vm.invoke(
            "Phase3.FastPathSuite::StepFlatAgents",
            {vmFlatArr.value, std::int64_t{120}},
            highLimits);
        requireCond(vmFlatStep.succeeded, "VM StepFlatAgents failed: " + vmFlatStep.error.message);

        // StencilJit Native x86_64 execution + 0-alloc + GC survival
        const auto jitFlatArr = nativeCompiled.module->invoke(
            "Phase3.FastPathSuite::AllocFlatAgents", {std::int64_t{32}}, highLimits);
        requireCond(jitFlatArr.succeeded, "StencilJit AllocFlatAgents failed");
        auto jitFlatRoot = jitHeap->retain(std::get<runtime::ObjectRef>(jitFlatArr.value));
        (void)jitFlatRoot;
        {
            const std::uint64_t obj0 = jitHeap->statistics().objectsAllocated;
            runtime::resetHeapStructStorageAllocations();
            const auto jitFlatStep = nativeCompiled.module->invoke(
                "Phase3.FastPathSuite::StepFlatAgents",
                {jitFlatArr.value, std::int64_t{120}},
                highLimits);
            const std::uint64_t obj1 = jitHeap->statistics().objectsAllocated;
            const std::uint64_t boxes = runtime::heapStructStorageAllocations();

            requireCond(
                jitFlatStep.succeeded && jitFlatStep.value == vmFlatStep.value,
                "StencilJit StepFlatAgents mismatch vs VM");
            requireCond(
                obj1 == obj0 && boxes == 0u,
                "Expected 0 allocations in StencilJit StepFlatAgents, got obj=" +
                    std::to_string(obj1 - obj0) + ", boxes=" + std::to_string(boxes));

            // Mutate string & ObjectRef inside FlatStruct[] via StencilJit, then force full GC!
            const auto relabel = nativeCompiled.module->invoke(
                "Phase3.FastPathSuite::RelabelAndLinkAgents",
                {jitFlatArr.value, std::string("jit")},
                highLimits);
            requireCond(relabel.succeeded, "StencilJit RelabelAndLinkAgents failed: " + relabel.error.message);
            { runtime::ShadowStack ss; jitHeap->collectFull(ss); }
            // "agent" (5) + "_" (1) + "jit" (3) = 9
            const auto postGc = nativeCompiled.module->invoke(
                "Phase3.FastPathSuite::VerifyPostGcAgents",
                {jitFlatArr.value, std::int64_t{9}},
                highLimits);
            requireCond(postGc.succeeded, "StencilJit VerifyPostGcAgents failed: " + postGc.error.message);

            const auto vmRelabel = vm.invoke(
                "Phase3.FastPathSuite::RelabelAndLinkAgents",
                {vmFlatArr.value, std::string("jit")},
                highLimits);
            requireCond(vmRelabel.succeeded, "VM RelabelAndLinkAgents failed");
            { runtime::ShadowStack ss; vmHeap->collectFull(ss); }
            const auto vmPostGc = vm.invoke(
                "Phase3.FastPathSuite::VerifyPostGcAgents",
                {vmFlatArr.value, std::int64_t{9}},
                highLimits);
            requireCond(
                vmPostGc.succeeded && postGc.value == vmPostGc.value,
                "Post-GC FlatStruct[] verification mismatch between StencilJit and VM");
        }

        // ThreadedStencils parity on FlatStruct[]
        {
            const auto thFlatArr = threadedCompiled.module->invoke(
                "Phase3.FastPathSuite::AllocFlatAgents", {std::int64_t{32}}, highLimits);
            auto thRoot = thHeap->retain(std::get<runtime::ObjectRef>(thFlatArr.value));
            (void)thRoot;
            const auto thStep = threadedCompiled.module->invoke(
                "Phase3.FastPathSuite::StepFlatAgents",
                {thFlatArr.value, std::int64_t{120}},
                highLimits);
            requireCond(
                thStep.succeeded && thStep.value == vmFlatStep.value,
                "Threaded StencilJit StepFlatAgents mismatch");
        }

        // -------------------------------------------------------------------
        // [4/5] >16-Field PackedStruct[] (BlittableStruct) Lock-Free Cache Parity
        // -------------------------------------------------------------------
        std::cout << "[4/5] Testing >16-field PackedStruct[] (SimStats20[]) lock-free cache parity...\n";
        const auto vmBlitArr = vm.invoke("Phase3.FastPathSuite::AllocBlittableStats", {std::int64_t{32}}, highLimits);
        auto vmBlitRoot = vmHeap->retain(std::get<runtime::ObjectRef>(vmBlitArr.value));
        (void)vmBlitRoot;
        const auto vmBlitStep = vm.invoke(
            "Phase3.FastPathSuite::StepBlittableStats",
            {vmBlitArr.value, std::int64_t{150}},
            highLimits);
        requireCond(vmBlitStep.succeeded, "VM StepBlittableStats failed");

        const auto jitBlitArr = nativeCompiled.module->invoke(
            "Phase3.FastPathSuite::AllocBlittableStats", {std::int64_t{32}}, highLimits);
        auto jitBlitRoot = jitHeap->retain(std::get<runtime::ObjectRef>(jitBlitArr.value));
        (void)jitBlitRoot;
        {
            const std::uint64_t obj0 = jitHeap->statistics().objectsAllocated;
            runtime::resetHeapStructStorageAllocations();
            const auto jitBlitStep = nativeCompiled.module->invoke(
                "Phase3.FastPathSuite::StepBlittableStats",
                {jitBlitArr.value, std::int64_t{150}},
                highLimits);
            requireCond(
                jitBlitStep.succeeded && jitBlitStep.value == vmBlitStep.value,
                "StencilJit StepBlittableStats mismatch");
            requireCond(
                jitHeap->statistics().objectsAllocated == obj0 &&
                    runtime::heapStructStorageAllocations() == 0u,
                "Expected 0 allocations in StencilJit StepBlittableStats");
        }

        // -------------------------------------------------------------------
        // [5/5] ToolchainJit Typed C++17 AOT (Default + TrustedRelease) & 4-Tier Benchmark
        // -------------------------------------------------------------------
#if defined(REALSCRIPT_JIT_COMPILER) && defined(REALSCRIPT_JIT_INCLUDE_DIR) && \
    defined(REALSCRIPT_JIT_SUPPORT_LIBRARY) && defined(REALSCRIPT_JIT_CACHE_DIR)
        std::cout << "[5/5] Compiling & benchmarking Typed C++17 AOT (Default + TrustedRelease) vs StencilJit vs VM...\n";
        {
            jit::ToolchainOptions tcDefault;
            tcDefault.compiler = REALSCRIPT_JIT_COMPILER;
            tcDefault.includeDirectory = REALSCRIPT_JIT_INCLUDE_DIR;
            tcDefault.supportLibrary = REALSCRIPT_JIT_SUPPORT_LIBRARY;
            tcDefault.outputDirectory = std::filesystem::path(REALSCRIPT_JIT_CACHE_DIR) / "phase3_default";
            tcDefault.reuseCachedLibrary = false;
            tcDefault.generation.trustedRelease = false;
#if defined(_WIN32)
#if defined(_DEBUG)
            tcDefault.compilerArguments = {"/MDd"};
#else
            tcDefault.compilerArguments = {"/MD", "/O2"};
#endif
#endif

            jit::ToolchainOptions tcTrusted = tcDefault;
            tcTrusted.outputDirectory = std::filesystem::path(REALSCRIPT_JIT_CACHE_DIR) / "phase3_trusted";
            tcTrusted.generation.trustedRelease = true;

            if (jit::ToolchainJit::available(tcDefault)) {
                jit::ToolchainJit tcJit;
                auto aotDefault = tcJit.compile(built.mirModules, tcDefault);
                requireCond(
                    aotDefault.succeeded(),
                    "Default Typed C++17 AOT compile failed: " + aotDefault.error + "\n" +
                        aotDefault.compilerOutput);

                auto aotTrusted = tcJit.compile(built.mirModules, tcTrusted);
                requireCond(
                    aotTrusted.succeeded(),
                    "TrustedRelease Typed C++17 AOT compile failed: " + aotTrusted.error + "\n" +
                        aotTrusted.compilerOutput);

                for (int mode = 0; mode < 2; ++mode) {
                    auto& mod = (mode == 0) ? *aotDefault.module : *aotTrusted.module;
                    const std::string tag = (mode == 0) ? "AOT-Default" : "AOT-TrustedRelease";
                    auto aotHeap = std::make_shared<runtime::ManagedHeap>();
                    mod.setHeap(aotHeap);

                    // 1) MegaKernel & SuperFrameKernel & RunUnfoldedMethodLoop with 0 allocations
                    const std::uint64_t sObj0 = aotHeap->statistics().objectsAllocated;
                    runtime::resetHeapStructStorageAllocations();
                    const auto rMega = mod.invoke(
                        "Phase3.FastPathSuite::MegaKernel", {std::int64_t{250}}, highLimits);
                    const auto rSuper = mod.invoke(
                        "Phase3.FastPathSuite::SuperFrameKernel", {std::int64_t{250}}, highLimits);
                    const auto rMethod = mod.invoke(
                        "Phase3.FastPathSuite::RunUnfoldedMethodLoop", {std::int64_t{400}}, highLimits);
                    requireCond(
                        rMega.succeeded && rMega.value == vmMega.value,
                        tag + " MegaKernel mismatch: " + rMega.error.message);
                    requireCond(
                        rSuper.succeeded && rSuper.value == vmSuper.value,
                        tag + " SuperFrameKernel mismatch: " + rSuper.error.message);
                    requireCond(
                        rMethod.succeeded && rMethod.value == vmMethod.value,
                        tag + " RunUnfoldedMethodLoop mismatch: " + rMethod.error.message);
                    requireCond(
                        aotHeap->statistics().objectsAllocated == sObj0 &&
                            runtime::heapStructStorageAllocations() == 0u,
                        tag + " allocated unexpectedly during struct kernels");

                    // 2) FlatStruct[] StepFlatAgents 0-alloc + RelabelAndLinkAgents GC write-barrier survival
                    const auto aotFlat = mod.invoke(
                        "Phase3.FastPathSuite::AllocFlatAgents", {std::int64_t{32}}, highLimits);
                    requireCond(aotFlat.succeeded, tag + " AllocFlatAgents failed");
                    auto aotFlatRoot = aotHeap->retain(std::get<runtime::ObjectRef>(aotFlat.value));
                    (void)aotFlatRoot;

                    const std::uint64_t fObj0 = aotHeap->statistics().objectsAllocated;
                    runtime::resetHeapStructStorageAllocations();
                    const auto rFlatStep = mod.invoke(
                        "Phase3.FastPathSuite::StepFlatAgents",
                        {aotFlat.value, std::int64_t{120}},
                        highLimits);
                    requireCond(
                        rFlatStep.succeeded && rFlatStep.value == vmFlatStep.value,
                        tag + " StepFlatAgents mismatch: " + rFlatStep.error.message);
                    requireCond(
                        aotHeap->statistics().objectsAllocated == fObj0 &&
                            runtime::heapStructStorageAllocations() == 0u,
                        tag + " allocated unexpectedly in StepFlatAgents");

                    const auto rRelabel = mod.invoke(
                        "Phase3.FastPathSuite::RelabelAndLinkAgents",
                        {aotFlat.value, std::string("aot")},
                        highLimits);
                    requireCond(rRelabel.succeeded, tag + " RelabelAndLinkAgents failed: " + rRelabel.error.message);
                    { runtime::ShadowStack ss; aotHeap->collectFull(ss); }
                    const auto rPostGc = mod.invoke(
                        "Phase3.FastPathSuite::VerifyPostGcAgents",
                        {aotFlat.value, std::int64_t{9}},
                        highLimits);
                    requireCond(
                        rPostGc.succeeded && std::get<std::int64_t>(rPostGc.value) > 30000LL,
                        tag + " VerifyPostGcAgents failed after full GC");

                    // 3) >16-field PackedStruct[] StepBlittableStats 0-alloc parity
                    const auto aotBlit = mod.invoke(
                        "Phase3.FastPathSuite::AllocBlittableStats", {std::int64_t{32}}, highLimits);
                    auto aotBlitRoot = aotHeap->retain(std::get<runtime::ObjectRef>(aotBlit.value));
                    (void)aotBlitRoot;
                    const std::uint64_t bObj0 = aotHeap->statistics().objectsAllocated;
                    runtime::resetHeapStructStorageAllocations();
                    const auto rBlitStep = mod.invoke(
                        "Phase3.FastPathSuite::StepBlittableStats",
                        {aotBlit.value, std::int64_t{150}},
                        highLimits);
                    requireCond(
                        rBlitStep.succeeded && rBlitStep.value == vmBlitStep.value,
                        tag + " StepBlittableStats mismatch");
                    requireCond(
                        aotHeap->statistics().objectsAllocated == bObj0 &&
                            runtime::heapStructStorageAllocations() == 0u,
                        tag + " allocated unexpectedly in StepBlittableStats");
                }

                // Timed 4-tier microbenchmark on 64-element FlatAgent[] x 2000 rounds (128,000 24-slot FlatStruct[] ops)
                constexpr std::int64_t kBenchCount = 64;
                constexpr std::int64_t kBenchRounds = 2000;

                auto benchTier = [&](const char* tierName, runtime::ManagedHeap& h, auto&& allocFn, auto&& stepFn) -> std::pair<double, std::int64_t> {
                    const auto arrRes = allocFn(kBenchCount);
                    requireCond(arrRes.succeeded, std::string(tierName) + " bench alloc failed: " + arrRes.error.message);
                    auto heldRoot = h.retain(std::get<runtime::ObjectRef>(arrRes.value));
                    (void)heldRoot;
                    const auto t0 = std::chrono::steady_clock::now();
                    const auto stepRes = stepFn(arrRes.value, kBenchRounds);
                    const auto t1 = std::chrono::steady_clock::now();
                    requireCond(stepRes.succeeded, std::string(tierName) + " bench step failed (code=" + std::to_string(static_cast<int>(stepRes.error.code)) + ", exec=" + std::to_string(stepRes.instructionsExecuted) + "): " + stepRes.error.message);
                    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
                    return {ms, std::get<std::int64_t>(stepRes.value)};
                };

                auto bVmHeap = std::make_shared<runtime::ManagedHeap>();
                runtime::Interpreter bVm(built.program, bVmHeap);
                const auto [vmMs, vmAns] = benchTier("VM", *bVmHeap,
                    [&](std::int64_t n) { return bVm.invoke("Phase3.FastPathSuite::AllocFlatAgents", {n}, highLimits); },
                    [&](const runtime::Value& a, std::int64_t r) { return bVm.invoke("Phase3.FastPathSuite::StepFlatAgents", {a, r}, highLimits); });

                auto bJitHeap = std::make_shared<runtime::ManagedHeap>();
                nativeCompiled.module->setHeap(bJitHeap);
                const auto [jitMs, jitAns] = benchTier("StencilJit", *bJitHeap,
                    [&](std::int64_t n) { return nativeCompiled.module->invoke("Phase3.FastPathSuite::AllocFlatAgents", {n}, highLimits); },
                    [&](const runtime::Value& a, std::int64_t r) { return nativeCompiled.module->invoke("Phase3.FastPathSuite::StepFlatAgents", {a, r}, highLimits); });

                auto bAotDefHeap = std::make_shared<runtime::ManagedHeap>();
                aotDefault.module->setHeap(bAotDefHeap);
                const auto [aotDefMs, aotDefAns] = benchTier("AOT-Default", *bAotDefHeap,
                    [&](std::int64_t n) { return aotDefault.module->invoke("Phase3.FastPathSuite::AllocFlatAgents", {n}, highLimits); },
                    [&](const runtime::Value& a, std::int64_t r) { return aotDefault.module->invoke("Phase3.FastPathSuite::StepFlatAgents", {a, r}, highLimits); });

                auto bAotTrHeap = std::make_shared<runtime::ManagedHeap>();
                aotTrusted.module->setHeap(bAotTrHeap);
                const auto [aotTrMs, aotTrAns] = benchTier("AOT-TrustedRelease", *bAotTrHeap,
                    [&](std::int64_t n) { return aotTrusted.module->invoke("Phase3.FastPathSuite::AllocFlatAgents", {n}, highLimits); },
                    [&](const runtime::Value& a, std::int64_t r) { return aotTrusted.module->invoke("Phase3.FastPathSuite::StepFlatAgents", {a, r}, highLimits); });

                requireCond(
                    jitAns == vmAns && aotDefAns == vmAns && aotTrAns == vmAns,
                    "Benchmark checksum mismatch across execution tiers");

                std::cout << std::fixed << std::setprecision(2)
                          << "  [Bench 64x2000 FlatAgent[24-slot] kernel] "
                          << "VM=" << vmMs << " ms | "
                          << "StencilJit(x86_64)=" << jitMs << " ms (" << (vmMs / std::max(jitMs, 0.01)) << "x) | "
                          << "C++17 Typed AOT=" << aotDefMs << " ms (" << (vmMs / std::max(aotDefMs, 0.01)) << "x) | "
                          << "C++17 TrustedRelease AOT=" << aotTrMs << " ms (" << (vmMs / std::max(aotTrMs, 0.01)) << "x)\n";
            }
        }
#endif

        // -------------------------------------------------------------------
        // [6/7] Non-Zero Base FrameOffset (x86_64 + SimARM64) & Win64 RSI/RDI ABI
        // -------------------------------------------------------------------
        std::cout << "[6/7] Testing non-zero baseFrameOffset (x86_64 + SimARM64) & Win64 RSI/RDI prologue/epilogue...\n";
        if (nativeCompiled.module && nativeCompiled.module->nativeBackendActive()) {
            const auto code = nativeCompiled.module->inspectMachineCode(
                "Phase3.FastPathSuite::StepFlatAgents", jit::StencilTargetArch::X86_64);
            auto containsSeq = [&](const std::vector<std::uint8_t>& needle) {
                for (std::size_t i = 0; i + needle.size() <= code.size(); ++i) {
                    bool match = true;
                    for (std::size_t j = 0; j < needle.size(); ++j) {
                        if (code[i + j] != needle[j]) {
                            match = false;
                            break;
                        }
                    }
                    if (match) return true;
                }
                return false;
            };
            // Prologue: push rbx (0x53); push rsi (0x56); push rdi (0x57)
            requireCond(
                containsSeq({0x53u, 0x56u, 0x57u}) && containsSeq({0x5Fu, 0x5Eu, 0x5Bu}),
                "Expected X64 StencilJit prologue/epilogue to preserve callee-saved RSI & RDI");

            // Invoke MegaKernel & StepFlatAgents with baseFrameOffset == 37 on native x86_64 and SimARM64
            const auto offMegaX64 = nativeCompiled.module->invokeWithBaseFrameOffset(
                "Phase3.FastPathSuite::MegaKernel", 37u, false, {std::int64_t{250}}, highLimits);
            const auto offMegaA64 = nativeCompiled.module->invokeWithBaseFrameOffset(
                "Phase3.FastPathSuite::MegaKernel", 37u, true, {std::int64_t{250}}, highLimits);
            requireCond(
                offMegaX64.succeeded && offMegaX64.value == vmMega.value &&
                    offMegaA64.succeeded && offMegaA64.value == vmMega.value,
                "Non-zero baseFrameOffset MegaKernel mismatch on x86_64 or SimARM64");

            for (int simArm = 0; simArm <= 1; ++simArm) {
                auto offHeap = std::make_shared<runtime::ManagedHeap>();
                nativeCompiled.module->setHeap(offHeap);
                const auto offArr = nativeCompiled.module->invoke(
                    "Phase3.FastPathSuite::AllocFlatAgents", {std::int64_t{32}}, highLimits);
                requireCond(offArr.succeeded, "AllocFlatAgents failed in offset test");
                auto offRoot = offHeap->retain(std::get<runtime::ObjectRef>(offArr.value));
                (void)offRoot;
                const auto offRes = nativeCompiled.module->invokeWithBaseFrameOffset(
                    "Phase3.FastPathSuite::StepFlatAgents",
                    37u,
                    simArm == 1,
                    {offArr.value, std::int64_t{120}},
                    highLimits);
                requireCond(
                    offRes.succeeded && offRes.value == vmFlatStep.value,
                    std::string("Non-zero baseFrameOffset StepFlatAgents mismatch on ") +
                        (simArm == 1 ? "SimARM64" : "x86_64") + ": " + offRes.error.message);
            }
            nativeCompiled.module->setHeap(jitHeap);

            // Also test nested StackSlotRef + large sub-struct slice ops under non-zero baseFrameOffset
            const char* kOffsetCaptureSource = R"RS(
module Phase3.OffsetCaptureSuite;

public delegate int BumpFn(int k);

public struct BigSub {
    public int A0; public int A1; public int A2; public int A3;
    public int A4; public int A5; public int A6; public int A7;
}

public struct Holder {
    public int Tag;
    public BigSub Sub;
}

public int StepHolderRef(ref Holder h, int k, int delta) {
    h.Tag = h.Tag + delta;
    h.Sub.A0 = h.Sub.A0 + k;
    h.Sub.A7 = h.Sub.A7 + k * 2;
    BigSub snap = h.Sub;
    return snap.A0 + snap.A7 + h.Tag;
}

public int RunCaptureWithSlice(int n) {
    Holder h = new Holder();
    h.Tag = 10;
    h.Sub.A0 = 1;
    h.Sub.A7 = 2;
    int bump = 3;
    int captured = 5;
    BumpFn fn = k => captured = captured + k + bump;
    int acc = 0;
    for (int i = 0; i < n; i = i + 1) {
        int d = fn(i);
        acc = acc + StepHolderRef(ref h, i, d);
    }
    BigSub endSnap = h.Sub;
    return acc + endSnap.A0 + endSnap.A7;
}
)RS";
            auto capBuilt = compileScript("phase3_offset_cap.rscript", kOffsetCaptureSource, optimization::Level::Aggressive);
            jit::StencilJit capJit;
            auto capMod = capJit.compile(capBuilt.program, nativeOpts);
            requireCond(capMod.succeeded(), "OffsetCapture StencilJit compile failed");
            auto capVmHeap = std::make_shared<runtime::ManagedHeap>();
            runtime::Interpreter capVm(capBuilt.program, capVmHeap);
            const auto capVmRes = capVm.invoke("Phase3.OffsetCaptureSuite::RunCaptureWithSlice", {std::int64_t{40}}, highLimits);
            requireCond(capVmRes.succeeded, "VM RunCaptureWithSlice failed");

            auto capJitHeap = std::make_shared<runtime::ManagedHeap>();
            capMod.module->setHeap(capJitHeap);
            const auto capX64 = capMod.module->invokeWithBaseFrameOffset(
                "Phase3.OffsetCaptureSuite::RunCaptureWithSlice", 41u, false, {std::int64_t{40}}, highLimits);
            const auto capA64 = capMod.module->invokeWithBaseFrameOffset(
                "Phase3.OffsetCaptureSuite::RunCaptureWithSlice", 41u, true, {std::int64_t{40}}, highLimits);
            requireCond(
                capX64.succeeded && capX64.value == capVmRes.value &&
                    capA64.succeeded && capA64.value == capVmRes.value,
                "Non-zero baseFrameOffset capture + slice mismatch vs VM");
        }

        // -------------------------------------------------------------------
        // [7/7] Mid-Loop Young/Full GC Cache Invalidation & Pool Pinning Stress
        // -------------------------------------------------------------------
        std::cout << "[7/7] Testing mid-loop GC epoch cache refresh & ValuePool string pinning on FlatStruct[]...\n";
        {
            const char* kGcStressSource = R"RS(
module Phase3.GcStressSuite;

public class GcNode {
    public int Id;
}

public struct PooledItem {
    public int Seq;
    public string Label;
    public GcNode Node;
    public int F0;
    public int F1;
    public int F2;
    public int F3;
    public int F4;
    public int F5;
    public int F6;
    public int F7;
}

public int RunMidLoopGcStress(int count, int rounds) {
    PooledItem[] arr = new PooledItem[count];
    for (int i = 0; i < count; i = i + 1) {
        GcNode n = new GcNode();
        n.Id = i + 100;
        arr[i].Seq = i;
        arr[i].Label = "it_" + i;
        arr[i].Node = n;
        arr[i].F7 = i * 3;
    }
    for (int r = 0; r < rounds; r = r + 1) {
        for (int i = 0; i < count; i = i + 1) {
            GcNode fresh = new GcNode();
            fresh.Id = (r + 1) * 1000 + i;
            PooledItem cur = arr[i];
            cur.Seq = cur.Seq + 1;
            cur.Label = "r" + r + "_" + i;
            cur.Node = fresh;
            cur.F7 = cur.F7 + fresh.Id;
            arr[i] = cur;
        }
    }
    int checksum = 0;
    for (int i = 0; i < count; i = i + 1) {
        PooledItem p = arr[i];
        if (p.Label != ("r" + (rounds - 1) + "_" + i)) {
            return -1000 - i;
        }
        if (p.Node.Id != rounds * 1000 + i) {
            return -2000 - i;
        }
        PooledItem expected = p;
        expected.Label = "r" + (rounds - 1) + "_" + i;
        if (p != expected) {
            return -3000 - i;
        }
        checksum = (checksum + p.Seq + p.F7 + p.Label.Length) % 100000000;
    }
    return checksum;
}

public PooledItem MakeSamplePooledItem() {
    PooledItem p = new PooledItem();
    p.Label = "r" + 11 + "_" + 0;
    return p;
}

public bool CheckPooledItemLabel(PooledItem a, PooledItem b) {
    return a.Label == "r11_0" && a.Label != "r11_1" && a.Label != null && a == b;
}
)RS";
            auto gcBuilt = compileScript("phase3_gc_stress.rscript", kGcStressSource, optimization::Level::Aggressive);
            runtime::HeapConfig tightCfg;
            tightCfg.initialCollectionThresholdBytes = 2048;
            tightCfg.maximumHeapBytes = 32 * 1024 * 1024;
            runtime::ExecutionOptions tightOpts = highLimits;
            tightOpts.limits.gcWorkBudget = 128;

            auto gcVmHeap = std::make_shared<runtime::ManagedHeap>(tightCfg);
            runtime::Interpreter gcVm(gcBuilt.program, gcVmHeap);
            const auto gcVmRes = gcVm.invoke(
                "Phase3.GcStressSuite::RunMidLoopGcStress", {std::int64_t{128}, std::int64_t{12}}, tightOpts);
            requireCond(gcVmRes.succeeded && std::get<std::int64_t>(gcVmRes.value) > 0, "VM GC stress failed");

            jit::StencilJit gcJit;
            auto gcJitCompiled = gcJit.compile(gcBuilt.program, nativeOpts);
            requireCond(gcJitCompiled.succeeded(), "GC stress StencilJit compile failed");
            auto gcJitHeap = std::make_shared<runtime::ManagedHeap>(tightCfg);
            gcJitCompiled.module->setHeap(gcJitHeap);
            const auto gcJitRes = gcJitCompiled.module->invoke(
                "Phase3.GcStressSuite::RunMidLoopGcStress", {std::int64_t{128}, std::int64_t{12}}, tightOpts);
            requireCond(
                gcJitRes.succeeded && gcJitRes.value == gcVmRes.value,
                "StencilJit GC stress mismatch vs VM");
            requireCond(
                gcJitCompiled.module->functionTier("Phase3.GcStressSuite::CheckPooledItemLabel") ==
                    jit::StencilTier::NativeMachineCode,
                "CheckPooledItemLabel should compile natively via StencilJit");
            const auto sampleItemA = gcJitCompiled.module->invoke(
                "Phase3.GcStressSuite::MakeSamplePooledItem", {}, highLimits);
            const auto sampleItemB = gcJitCompiled.module->invoke(
                "Phase3.GcStressSuite::MakeSamplePooledItem", {}, highLimits);
            requireCond(sampleItemA.succeeded && sampleItemB.succeeded, "MakeSamplePooledItem failed in StencilJit");
            const auto checkLabelRes = gcJitCompiled.module->invoke(
                "Phase3.GcStressSuite::CheckPooledItemLabel", {sampleItemA.value, sampleItemB.value}, highLimits);
            requireCond(
                checkLabelRes.succeeded && std::get<bool>(checkLabelRes.value),
                "StencilJit CheckPooledItemLabel failed heap-aware string/struct equality");

#if defined(REALSCRIPT_JIT_COMPILER) && defined(REALSCRIPT_JIT_INCLUDE_DIR) && \
    defined(REALSCRIPT_JIT_SUPPORT_LIBRARY) && defined(REALSCRIPT_JIT_CACHE_DIR)
            jit::ToolchainOptions gcTcOpts;
            gcTcOpts.compiler = REALSCRIPT_JIT_COMPILER;
            gcTcOpts.includeDirectory = REALSCRIPT_JIT_INCLUDE_DIR;
            gcTcOpts.supportLibrary = REALSCRIPT_JIT_SUPPORT_LIBRARY;
            gcTcOpts.outputDirectory = std::filesystem::path(REALSCRIPT_JIT_CACHE_DIR) / "phase3_gc_stress";
            gcTcOpts.reuseCachedLibrary = false;
#if defined(_WIN32)
#if defined(_DEBUG)
            gcTcOpts.compilerArguments = {"/MDd"};
#else
            gcTcOpts.compilerArguments = {"/MD", "/O2"};
#endif
#endif
            if (jit::ToolchainJit::available(gcTcOpts)) {
                jit::ToolchainJit tcJit;
                for (int tr = 0; tr <= 1; ++tr) {
                    gcTcOpts.outputDirectory = std::filesystem::path(REALSCRIPT_JIT_CACHE_DIR) /
                        (tr == 0 ? "phase3_gc_stress_def" : "phase3_gc_stress_tr");
                    gcTcOpts.generation.trustedRelease = (tr == 1);
                    auto aotGcMod = tcJit.compile(gcBuilt.mirModules, gcTcOpts);
                    requireCond(aotGcMod.succeeded(), "AOT GC stress compile failed: " + aotGcMod.error);
                    auto aotGcHeap = std::make_shared<runtime::ManagedHeap>(tightCfg);
                    aotGcMod.module->setHeap(aotGcHeap);
                    const auto aotGcRes = aotGcMod.module->invoke(
                        "Phase3.GcStressSuite::RunMidLoopGcStress",
                        {std::int64_t{128}, std::int64_t{12}},
                        tightOpts);
                    requireCond(
                        aotGcRes.succeeded && aotGcRes.value == gcVmRes.value,
                        "Typed AOT GC stress mismatch vs VM (trusted=" + std::to_string(tr) + ")");
                }
            }
#endif
        }

        std::cout << "All Phase 3 StencilJit & C++17 AOT large-struct + FlatStruct[] fast-path tests passed!\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[FAIL] " << ex.what() << "\n";
        return 1;
    }
}
