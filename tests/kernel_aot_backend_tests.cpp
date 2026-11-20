#include "realscript/aot_cpp/AotCpp.h"
#include "realscript/aot_cpp/AotRuntime.h"
#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/compiler/ModuleBuildCache.h"
#include "realscript/hot_reload/HotReload.h"
#include "realscript/optimization/Optimizer.h"
#include "realscript/runtime/ParallelJob.h"
#include "realscript/runtime/Runtime.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace kernel_aot_ns {
const realscript::aot::ProgramDescriptor& KernelAotSuiteProgram() noexcept;
}

namespace kernel_aot_trusted_ns {
const realscript::aot::ProgramDescriptor& KernelAotTrustedProgram() noexcept;
}

namespace kernel_aot_plugin_ns {
const realscript::aot::ProgramDescriptor& KernelAotPluginProgram() noexcept;
}

namespace {

using namespace realscript;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string readFixtureFile(const std::string& relativePath) {
#ifdef REALSCRIPT_SOURCE_DIR
    const auto path = std::filesystem::path(REALSCRIPT_SOURCE_DIR) / relativePath;
#else
    const auto path = std::filesystem::current_path() / relativePath;
#endif
    std::ifstream in(path, std::ios::binary);
    require(in.good(), "Failed to open fixture file: " + path.string());
    std::ostringstream oss;
    oss << in.rdbuf();
    return oss.str();
}

std::shared_ptr<const runtime::ProgramImage> compileFixtureForVm(
    const std::string& virtualName,
    const std::string& source,
    optimization::Level optLevel = optimization::Level::Aggressive) {
    compiler::Compilation comp({{virtualName, source}});
    auto build = comp.build();
    if (build.diagnostics.hasErrors()) {
        std::ostringstream oss;
        for (const auto& d : build.diagnostics.items()) {
            oss << d.sourceName << " [" << d.code << "] " << d.message << '\n';
        }
        throw std::runtime_error("Fixture compilation failed:\n" + oss.str());
    }
    diagnostics::DiagnosticBag optDiag;
    optimization::Options optOpts;
    optOpts.level = optLevel;
    auto optimized = optimization::Optimizer{}.optimize(std::move(build.modules), optDiag, optOpts);
    require(!optDiag.hasErrors(), "Optimization failed");

    bytecode::Lowerer lowerer;
    std::vector<bytecode::Module> bcMods;
    for (const auto& m : optimized.modules) {
        bcMods.push_back(lowerer.lower(m));
    }
    runtime::RuntimeError linkErr;
    auto linked = runtime::ProgramImage::link(std::move(bcMods), linkErr);
    require(linked.has_value(), "Program link failed: " + linkErr.message);
    return std::make_shared<const runtime::ProgramImage>(std::move(*linked));
}

template <typename InvokerA, typename InvokerB>
void assertIntParity(
    const std::string& label,
    InvokerA& vm,
    InvokerB& aotProg,
    const std::string& fn,
    const std::vector<runtime::Value>& args) {
    const auto vmRes = vm.invoke(fn, args);
    const auto aotRes = aotProg.invoke(fn, args);
    require(vmRes.succeeded && aotRes.succeeded,
        label + " failed (" + fn + "): vm=" + vmRes.error.message + ", aot=" + aotRes.error.message);
    require(std::holds_alternative<std::int64_t>(vmRes.value) &&
            std::holds_alternative<std::int64_t>(aotRes.value),
        label + ": expected int64 return");
    require(std::get<std::int64_t>(vmRes.value) == std::get<std::int64_t>(aotRes.value),
        label + ": VM (" + std::to_string(std::get<std::int64_t>(vmRes.value)) +
            ") != AOT (" + std::to_string(std::get<std::int64_t>(aotRes.value)) + ")");
}

runtime::AwaitableHandle unpackAwaitableFromResult(const runtime::ExecutionResult& res) {
    require(res.succeeded && res.value.rawTag() == runtime::ValueTag::Struct,
        "Expected Struct Awaitable return, got error: " + res.error.message);
    const auto f0 = runtime::structValueFieldAt(res.value.asStructUnchecked(), 0);
    require(f0.has_value() && std::holds_alternative<runtime::LongValue>(*f0),
        "Expected packed handle field inside Awaitable struct");
    return runtime::AwaitableHandle::unpack64(
        static_cast<std::uint64_t>(std::get<runtime::LongValue>(*f0).value));
}

runtime::Value makeAwaitableStructArg(
    const char* qualifiedTypeName,
    runtime::AwaitableHandle handle) {
    std::vector<runtime::Value> fields = {
        runtime::Value{runtime::LongValue{static_cast<std::int64_t>(handle.pack64())}},
    };
    return runtime::Value{runtime::makeStructValueFromFields(
        semantic::stableTypeId(qualifiedTypeName), std::move(fields))};
}

// ---------------------------------------------------------------------------
// 1. Kernel.AotSuite VM vs C++17 AOT Backend Parity & SIMD Kernels
// ---------------------------------------------------------------------------
void testAotSuiteParityAndSimdKernels() {
    const std::string src = readFixtureFile("tests/fixtures/kernel_aot_suite.rscript");
    auto vmProg = compileFixtureForVm("tests/fixtures/kernel_aot_suite.rscript", src);

    runtime::EngineRuntime vm(vmProg);
    aot::Program aotProg(kernel_aot_ns::KernelAotSuiteProgram());

    // Ensure distinct ManagedHeap + AsyncRuntimeContext for AOT.
    auto aotHeap = std::make_shared<runtime::ManagedHeap>();
    auto aotAsync = std::make_shared<runtime::AsyncRuntimeContext>();
    aotProg.setHeap(aotHeap);
    aotProg.setAsyncRuntime(aotAsync);

    assertIntParity("OOP/Delegates/Generics/Collections", vm, aotProg,
        "Kernel.AotSuite::RunOopDelegatesGenericsAndCollections",
        {runtime::Value{std::int64_t{3}}, runtime::Value{std::int64_t{5}}});

    assertIntParity("ExactNumerics", vm, aotProg, "Kernel.AotSuite::RunExactNumerics", {});

    // Sequence coroutine progression parity across 3 wait_ticks yields.
    {
        const auto runSequence = [](auto& exec) -> std::int64_t {
            const auto created = exec.invoke("Kernel.AotSuite::CreateSequenceWorker");
            require(created.succeeded, "CreateSequenceWorker failed: " + created.error.message);
            const auto obj = std::get<runtime::ObjectRef>(created.value);
            auto pin = exec.heap()->retain(runtime::Value{obj});
            require(exec.invoke("Kernel.AotSuite::SequenceWorker.Run",
                {runtime::Value{obj}, runtime::Value{runtime::LongValue{10}}, runtime::Value{std::int64_t{7}}}).succeeded,
                "SequenceWorker.Run start failed");
            for (int tick = 0; tick < 3; ++tick) {
                require(exec.invoke("Kernel.AotSuite::SequenceWorker.$sequence_Run_1",
                    {runtime::Value{obj}}).succeeded,
                    "SequenceWorker step failed");
            }
            const auto read = exec.invoke("Kernel.AotSuite::SequenceWorker.Read", {runtime::Value{obj}});
            require(read.succeeded, "SequenceWorker.Read failed");
            return std::get<std::int64_t>(read.value);
        };
        const std::int64_t vmVal = runSequence(vm);
        const std::int64_t aotVal = runSequence(aotProg);
        require(vmVal == 19 && vmVal == aotVal, "SequenceWorker parity mismatch");
    }

    assertIntParity("AsyncSyncFastPath", vm, aotProg,
        "Kernel.AotSuite::RunSyncFastPath",
        {runtime::Value{std::int64_t{7}}, runtime::Value{std::int64_t{9}}});

    assertIntParity("AsyncDeepChainAndPolymorphism", vm, aotProg,
        "Kernel.AotSuite::RunDeepChainAndPolymorphism",
        {runtime::Value{std::int64_t{10}}});

    assertIntParity("CrossAwaitTryFinally(throw)", vm, aotProg,
        "Kernel.AotSuite::RunCrossAwaitTryFinally", {runtime::Value{true}});
    assertIntParity("CrossAwaitTryFinally(ok)", vm, aotProg,
        "Kernel.AotSuite::RunCrossAwaitTryFinally", {runtime::Value{false}});

    // Host-driven external suspension parity on both VM and AOT.
    {
        const auto runHostDriven = [](auto& exec) -> std::int64_t {
            auto asyncCtx = exec.asyncRuntime();
            auto heap = exec.heap();
            const auto gateH = asyncCtx->awaitables().allocatePending();
            const auto gateArg = makeAwaitableStructArg("RealScript.Async::Awaitable__int", gateH);
            const auto boxArr = heap->allocateTypedArray(
                semantic::stableTypeId("int[]"), semantic::PrimitiveType::Int, 0, 1, std::int64_t{-1});
            require(boxArr.has_value(), "allocateTypedArray outBox failed");
            auto pinBox = heap->retain(runtime::Value{*boxArr});

            const auto started = exec.invoke(
                "Kernel.AotSuite::HostDrivenWorker", {gateArg, runtime::Value{*boxArr}});
            require(started.succeeded, "HostDrivenWorker launch failed: " + started.error.message);
            require(heap->arrayGet(*boxArr, 0) == std::optional<runtime::Value>{runtime::Value{std::int64_t{-1}}},
                "HostDrivenWorker completed before external gate resolved");

            require(asyncCtx->awaitables().trySetResult(gateH, runtime::Value{std::int64_t{14}}),
                "trySetResult on external gate failed");
            asyncCtx->stepResumeQueue(runtime::ResumeQueue::Update);
            asyncCtx->stepResumeQueue(runtime::ResumeQueue::Update);

            const auto outVal = heap->arrayGet(*boxArr, 0);
            require(outVal.has_value(), "Missing outBox[0] after HostDrivenWorker completion");
            return std::get<std::int64_t>(*outVal);
        };
        require(runHostDriven(vm) == 143 && runHostDriven(aotProg) == 143,
            "HostDrivenWorker VM vs AOT mismatch");
    }

    // Packed array SIMD range kernels (SaxpyIntRange & FmaDoubleRange).
    {
        constexpr std::size_t N = 64;
        const auto intTid = semantic::stableTypeId("int[]");
        const auto dblTid = semantic::stableTypeId("double[]");

        const auto allocAndRunSimd = [&](auto& exec, std::vector<std::int64_t>& outInt, double& outDbl) {
            auto heap = exec.heap();
            auto xI = *heap->allocateTypedArray(intTid, semantic::PrimitiveType::Int, 0, N, runtime::Value{std::int64_t{0}});
            auto yI = *heap->allocateTypedArray(intTid, semantic::PrimitiveType::Int, 0, N, runtime::Value{std::int64_t{0}});
            auto dI = *heap->allocateTypedArray(intTid, semantic::PrimitiveType::Int, 0, N, runtime::Value{std::int64_t{0}});
            auto xD = *heap->allocateTypedArray(dblTid, semantic::PrimitiveType::Double, 0, N, runtime::Value{0.0});
            auto yD = *heap->allocateTypedArray(dblTid, semantic::PrimitiveType::Double, 0, N, runtime::Value{0.0});
            auto dD = *heap->allocateTypedArray(dblTid, semantic::PrimitiveType::Double, 0, N, runtime::Value{0.0});
            auto p1 = heap->retain(runtime::Value{xI});
            auto p2 = heap->retain(runtime::Value{yI});
            auto p3 = heap->retain(runtime::Value{dI});
            auto p4 = heap->retain(runtime::Value{xD});
            auto p5 = heap->retain(runtime::Value{yD});
            auto p6 = heap->retain(runtime::Value{dD});

            std::vector<std::int64_t> xv(N), yv(N);
            std::vector<double> xdv(N), ydv(N);
            for (std::size_t i = 0; i < N; ++i) {
                xv[i] = static_cast<std::int64_t>(i + 1);
                yv[i] = static_cast<std::int64_t>(i * 2);
                xdv[i] = static_cast<double>(i) * 0.5;
                ydv[i] = 1.25;
            }
            require(heap->writePackedIntArray(xI, xv.data(), N) && heap->writePackedIntArray(yI, yv.data(), N),
                "writePackedIntArray failed");
            require(heap->writePackedDoubleArray(xD, xdv.data(), N) && heap->writePackedDoubleArray(yD, ydv.data(), N),
                "writePackedDoubleArray failed");

            const auto sRes = exec.invoke("Kernel.AotSuite::SaxpyIntRange", {
                runtime::Value{std::int64_t{0}}, runtime::Value{static_cast<std::int64_t>(N)},
                runtime::Value{std::int64_t{3}}, runtime::Value{xI}, runtime::Value{yI}, runtime::Value{dI},
            });
            require(sRes.succeeded, "SaxpyIntRange failed: " + sRes.error.message);
            require(heap->readPackedIntArray(dI, outInt), "readPackedIntArray failed");

            const auto fRes = exec.invoke("Kernel.AotSuite::FmaDoubleRange", {
                runtime::Value{std::int64_t{0}}, runtime::Value{static_cast<std::int64_t>(N)},
                runtime::Value{2.5}, runtime::Value{xD}, runtime::Value{yD}, runtime::Value{dD},
            });
            require(fRes.succeeded, "FmaDoubleRange failed: " + fRes.error.message);
            outDbl = std::get<double>(fRes.value);
        };

        std::vector<std::int64_t> vmInts, aotInts;
        double vmDbl = 0.0;
        double aotDbl = 0.0;
        allocAndRunSimd(vm, vmInts, vmDbl);
        allocAndRunSimd(aotProg, aotInts, aotDbl);
        require(vmInts == aotInts && vmInts.size() == N && vmInts[10] == (3 * 11 + 20),
            "SaxpyIntRange VM vs AOT mismatch");
        require(std::abs(vmDbl - aotDbl) < 1e-9, "FmaDoubleRange VM vs AOT mismatch");
    }

    // Verify CustomAwaitFallbackAsync & duck-typed fallback compiled cleanly in AOT without UB.
    {
        require(aotProg.resolveSourceLocation("Kernel.AotSuite::CustomDuckCompileFallbackAsync", 0).has_value(),
            "AOT program missing generic fallback function CustomDuckCompileFallbackAsync");
        const auto fbVm = vm.invoke("Kernel.AotSuite::CustomAwaitFallbackAsync", {runtime::Value{std::int64_t{8}}});
        const auto fbAot = aotProg.invoke("Kernel.AotSuite::CustomAwaitFallbackAsync", {runtime::Value{std::int64_t{8}}});
        require(fbVm.succeeded && fbAot.succeeded,
            "CustomAwaitFallbackAsync failed: VM=" + fbVm.error.message + " AOT=" + fbAot.error.message);

        runtime::AwaitableHandle vmH{};
        runtime::AwaitableHandle aotH{};
        require(runtime::tryExtractAwaitableHandle(fbVm.value, vmH) &&
                runtime::tryExtractAwaitableHandle(fbAot.value, aotH),
            "CustomAwaitFallbackAsync must pack CustomTaskInt.__handle");
        runtime::Value vmOut{};
        runtime::Value aotOut{};
        runtime::RuntimeError vmErr{};
        runtime::RuntimeError aotErr{};
        require(vm.asyncRuntime()->awaitables().getResult(vmH, vmOut, vmErr) == runtime::AwaitStatus::Succeeded &&
                aotProg.asyncRuntime()->awaitables().getResult(aotH, aotOut, aotErr) == runtime::AwaitStatus::Succeeded,
            "CustomAwaitFallbackAsync CustomTaskInt handle did not succeed");
        require(vmOut == aotOut && std::get<std::int64_t>(aotOut) == 50,
            "CustomAwaitFallbackAsync VM vs AOT result mismatch");
        (void)vm.asyncRuntime()->awaitables().discard(vmH);
        (void)aotProg.asyncRuntime()->awaitables().discard(aotH);
    }
}

// ---------------------------------------------------------------------------
// 2. Kernel.AotTrusted (TrustedRelease + Scalar-Unboxed Coroutines) Parity
// ---------------------------------------------------------------------------
void testAotTrustedScalarCoroutinesParity() {
    const std::string src = readFixtureFile("tests/fixtures/kernel_aot_trusted.rscript");
    auto vmProg = compileFixtureForVm("tests/fixtures/kernel_aot_trusted.rscript", src);

    runtime::EngineRuntime vm(vmProg);
    aot::Program aotProg(kernel_aot_trusted_ns::KernelAotTrustedProgram());
    aotProg.setHeap(std::make_shared<runtime::ManagedHeap>());
    aotProg.setAsyncRuntime(std::make_shared<runtime::AsyncRuntimeContext>());

    runtime::ExecutionOptions trustedOpts;
    trustedOpts.trustedRelease = true;

    {
        const auto rVm = vm.invoke("Kernel.AotTrusted::RunSyncScalarBatch", {runtime::Value{std::int64_t{120}}});
        const auto rAot = aotProg.invoke(
            "Kernel.AotTrusted::RunSyncScalarBatch", {runtime::Value{std::int64_t{120}}}, trustedOpts);
        require(rVm.succeeded && rAot.succeeded &&
                std::get<runtime::LongValue>(rVm.value).value ==
                    std::get<runtime::LongValue>(rAot.value).value,
            "RunSyncScalarBatch TrustedRelease VM vs AOT mismatch");
    }
    {
        const auto rVm = vm.invoke("Kernel.AotTrusted::RunPooledSuspendResumeBatch", {runtime::Value{std::int64_t{32}}});
        const auto rAot = aotProg.invoke(
            "Kernel.AotTrusted::RunPooledSuspendResumeBatch", {runtime::Value{std::int64_t{32}}}, trustedOpts);
        require(rVm.succeeded && rAot.succeeded &&
                std::get<runtime::LongValue>(rVm.value).value ==
                    std::get<runtime::LongValue>(rAot.value).value,
            "RunPooledSuspendResumeBatch TrustedRelease VM vs AOT mismatch");
    }

    // RefLocalCoro across two async yields.
    {
        const auto runRefLocal = [&](auto& exec, const runtime::ExecutionOptions& opts) -> std::int64_t {
            auto asyncCtx = exec.asyncRuntime();
            const auto gA = asyncCtx->awaitables().allocatePending();
            const auto gB = asyncCtx->awaitables().allocatePending();
            const auto launched = exec.invoke("Kernel.AotTrusted::RefLocalCoro", {
                runtime::Value{std::int64_t{5}},
                makeAwaitableStructArg("RealScript.Async::Awaitable__int", gA),
                makeAwaitableStructArg("RealScript.Async::Awaitable__int", gB),
            }, opts);
            const auto h = unpackAwaitableFromResult(launched);
            require(asyncCtx->awaitables().trySetResult(gA, runtime::Value{std::int64_t{3}}), "set gA");
            asyncCtx->stepResumeQueue(runtime::ResumeQueue::Update);
            require(asyncCtx->awaitables().trySetResult(gB, runtime::Value{std::int64_t{7}}), "set gB");
            asyncCtx->stepResumeQueue(runtime::ResumeQueue::Update);
            runtime::Value out;
            runtime::RuntimeError err;
            require(asyncCtx->awaitables().getResult(h, out, err) == runtime::AwaitStatus::Succeeded,
                "RefLocalCoro did not succeed: " + err.message);
            return std::get<std::int64_t>(out);
        };
        const auto vmOut = runRefLocal(vm, {});
        const auto aotOut = runRefLocal(aotProg, trustedOpts);
        require(vmOut == 55 && aotOut == 55, "RefLocalCoro VM vs AOT mismatch");
    }

    // ThrowAfterSuspendScalarCoro + FaultDivideAfterTwoSuspends fault cleanly without aborting.
    {
        auto asyncCtx = aotProg.asyncRuntime();
        const auto gate = asyncCtx->awaitables().allocatePending();
        const auto launched = aotProg.invoke("Kernel.AotTrusted::ThrowAfterSuspendScalarCoro",
            {makeAwaitableStructArg("RealScript.Async::Awaitable__int", gate)}, trustedOpts);
        const auto h = unpackAwaitableFromResult(launched);
        require(asyncCtx->awaitables().trySetResult(gate, runtime::Value{std::int64_t{-5}}), "set throw gate");
        asyncCtx->stepResumeQueue(runtime::ResumeQueue::Update);
        runtime::Value out;
        runtime::RuntimeError err;
        require(asyncCtx->awaitables().getResult(h, out, err) == runtime::AwaitStatus::Faulted &&
                err.code == runtime::ErrorCode::ScriptException,
            "ThrowAfterSuspendScalarCoro did not fault with ScriptException");
    }
    {
        auto asyncCtx = aotProg.asyncRuntime();
        const auto gA = asyncCtx->awaitables().allocatePending();
        const auto gB = asyncCtx->awaitables().allocatePending();
        const auto launched = aotProg.invoke("Kernel.AotTrusted::FaultDivideAfterTwoSuspends", {
            runtime::Value{std::int64_t{0}},
            makeAwaitableStructArg("RealScript.Async::Awaitable__int", gA),
            makeAwaitableStructArg("RealScript.Async::Awaitable__int", gB),
        }, trustedOpts);
        const auto h = unpackAwaitableFromResult(launched);
        (void)asyncCtx->awaitables().trySetResult(gA, runtime::Value{std::int64_t{10}});
        asyncCtx->stepResumeQueue(runtime::ResumeQueue::Update);
        (void)asyncCtx->awaitables().trySetResult(gB, runtime::Value{std::int64_t{2}});
        asyncCtx->stepResumeQueue(runtime::ResumeQueue::Update);
        runtime::Value out;
        runtime::RuntimeError err;
        require(asyncCtx->awaitables().getResult(h, out, err) == runtime::AwaitStatus::Faulted &&
                err.code == runtime::ErrorCode::DivisionByZero,
            "FaultDivideAfterTwoSuspends did not trap with DivisionByZero");
    }

    {
        const auto rVm = vm.invoke("Kernel.AotTrusted::StepPackedMath", {runtime::Value{std::int64_t{64}}});
        const auto rAot = aotProg.invoke(
            "Kernel.AotTrusted::StepPackedMath", {runtime::Value{std::int64_t{64}}}, trustedOpts);
        require(rVm.succeeded && rAot.succeeded &&
                std::get<std::int64_t>(rVm.value) == std::get<std::int64_t>(rAot.value),
            "StepPackedMath TrustedRelease VM vs AOT mismatch");
    }
}

// ---------------------------------------------------------------------------
// 3. CppGenerator #line Directive Emission Control
// ---------------------------------------------------------------------------
void testAotCppLineDirectivesGeneration() {
    const std::string src = R"RS(
module Kernel.AotLines;
#line 321 "ui/PanelLogic.rscript"
public int Compute(int x) {
    return x * 2 + 1;
}
#line default
)RS";

    compiler::Compilation comp({{"$generated/PanelLogic.g.rscript", src}});
    auto build = comp.build();
    require(!build.diagnostics.hasErrors(), "AotLines compile failed");

    diagnostics::DiagnosticBag diag;
    aot::CppGenerator gen;

    aot::GenerationOptions withLines;
    withLines.emitLineDirectives = true;
    const auto genWith = gen.generate(build.modules, diag, withLines);
    require(!diag.hasErrors(), "AOT withLines generate failed");
    require(genWith.source.find("#line ") != std::string::npos &&
            genWith.source.find("$generated/PanelLogic.g.rscript") != std::string::npos,
        "AOT generated C++ missing #line directives");

    aot::GenerationOptions withoutLines;
    withoutLines.emitLineDirectives = false;
    const auto genWithout = gen.generate(build.modules, diag, withoutLines);
    require(!diag.hasErrors(), "AOT withoutLines generate failed");
    require(genWithout.source.find("#line ") == std::string::npos,
        "AOT generated C++ emitted #line when emitLineDirectives=false");
}

// ---------------------------------------------------------------------------
// 4. Pillar 2 & 4: AotRuntime IAwaitScheduler Parity (Immediate / Suspended /
//    Faulted), AOT [ValueLifecycle] Sweep, & AotSimdVectorizer Span<T>
// ---------------------------------------------------------------------------
class ParityCustomAwaitScheduler final : public runtime::IAwaitScheduler {
public:
    semantic::SymbolId targetTaskTypeId = 0;
    bool allowCompleted = true;
    bool faultTryGet = false;
    std::int64_t nextValue = 0;
    int suspendCount = 0;
    std::function<void()> savedResume;

    bool handlesValue(const runtime::Value& awaited) const noexcept override {
        return awaited.rawTag() == runtime::ValueTag::Struct &&
            awaited.asStructUnchecked().typeId == targetTaskTypeId;
    }

    bool isCompleted(const runtime::Value&) const noexcept override {
        return allowCompleted;
    }

    bool tryGetResult(
        const runtime::Value& awaited,
        bool hasResult,
        runtime::Value& outResult,
        runtime::RuntimeError& outError) override {
        if (faultTryGet) {
            outError.code = runtime::ErrorCode::ScriptException;
            outError.message = "aot-vm-sched-fault";
            return false;
        }
        if (hasResult) {
            if (nextValue != 0) {
                outResult = runtime::Value{nextValue};
            } else {
                const auto f0 = runtime::structValueFieldAt(awaited.asStructUnchecked(), 0);
                if (f0 && std::holds_alternative<runtime::LongValue>(*f0)) {
                    outResult = runtime::Value{std::get<runtime::LongValue>(*f0).value};
                } else {
                    outResult = runtime::Value{std::int64_t{0}};
                }
            }
        }
        return true;
    }

    bool onSuspend(
        const runtime::Value&,
        std::function<void()> resumeCallback,
        runtime::RuntimeError&) override {
        ++suspendCount;
        savedResume = std::move(resumeCallback);
        return true;
    }

    bool tryResolveAsyncReturn(
        semantic::SymbolId,
        const runtime::Value*,
        runtime::Value&,
        runtime::RuntimeError&) override {
        return false;
    }
};

void testAotAwaitSchedulerValueLifecycleAndSpanSimd() {
    const std::string src = readFixtureFile("tests/fixtures/kernel_aot_suite.rscript");
    auto vmProg = compileFixtureForVm("tests/fixtures/kernel_aot_suite.rscript", src);

    runtime::EngineRuntime vm(vmProg);
    aot::Program aotProg(kernel_aot_ns::KernelAotSuiteProgram());
    auto aotHeap = std::make_shared<runtime::ManagedHeap>();
    auto aotAsync = std::make_shared<runtime::AsyncRuntimeContext>();
    aotProg.setHeap(aotHeap);
    aotProg.setAsyncRuntime(aotAsync);

    const auto taskTid = semantic::stableTypeId("Kernel.AotSuite::AotSchedTaskInt");

    // 1. Custom C++ IAwaitScheduler parity (immediate-ready, suspended/resumed, faulted).
    const auto runSchedImmediate = [&](auto& exec) -> std::int64_t {
        ParityCustomAwaitScheduler sched;
        sched.targetTaskTypeId = taskTid;
        sched.allowCompleted = true;
        sched.faultTryGet = false;
        sched.nextValue = 30;
        runtime::ExecutionOptions opts;
        opts.awaitScheduler = &sched;
        const auto res = exec.invoke(
            "Kernel.AotSuite::ComputeAotSchedulerAsync",
            {runtime::Value{std::int64_t{5}}, runtime::Value{runtime::LongValue{10}}},
            opts);
        require(res.succeeded && res.value.rawTag() == runtime::ValueTag::Struct,
            "ComputeAotSchedulerAsync immediate failed: " + res.error.message);
        const auto f0 = runtime::structValueFieldAt(res.value.asStructUnchecked(), 0);
        require(f0 && std::holds_alternative<runtime::LongValue>(*f0),
            "Missing AotSchedTaskInt.__handle on immediate");
        const auto h = runtime::AwaitableHandle::unpack64(
            static_cast<std::uint64_t>(std::get<runtime::LongValue>(*f0).value));
        runtime::Value got;
        runtime::RuntimeError gotErr;
        require(exec.asyncRuntime()->awaitables().getResult(h, got, gotErr) == runtime::AwaitStatus::Succeeded &&
                std::holds_alternative<std::int64_t>(got),
            "Immediate ComputeAotSchedulerAsync Awaitable not completed");
        return std::get<std::int64_t>(got);
    };
    require(runSchedImmediate(vm) == 40 && runSchedImmediate(aotProg) == 40,
        "Immediate IAwaitScheduler parity mismatch between VM and AOT");

    const auto runSchedSuspended = [&](auto& exec) -> std::int64_t {
        ParityCustomAwaitScheduler sched;
        sched.targetTaskTypeId = taskTid;
        sched.allowCompleted = false;
        sched.faultTryGet = false;
        sched.nextValue = 77;
        runtime::ExecutionOptions opts;
        opts.awaitScheduler = &sched;
        const auto res = exec.invoke(
            "Kernel.AotSuite::ComputeAotSchedulerAsync",
            {runtime::Value{std::int64_t{6}}, runtime::Value{runtime::LongValue{99}}},
            opts);
        require(res.succeeded && sched.suspendCount == 1 && sched.savedResume != nullptr,
            "Expected ComputeAotSchedulerAsync to suspend on custom IAwaitScheduler");
        const auto f0 = runtime::structValueFieldAt(res.value.asStructUnchecked(), 0);
        require(f0 && std::holds_alternative<runtime::LongValue>(*f0),
            "Missing AotSchedTaskInt.__handle on suspend");
        const auto h = runtime::AwaitableHandle::unpack64(
            static_cast<std::uint64_t>(std::get<runtime::LongValue>(*f0).value));
        require(!exec.asyncRuntime()->awaitables().isCompleted(h),
            "Outer AotSchedTaskInt must remain pending before resumeCallback");
        sched.allowCompleted = true;
        sched.savedResume();
        exec.asyncRuntime()->stepResumeQueue(runtime::ResumeQueue::Update);
        exec.asyncRuntime()->stepResumeQueue(runtime::ResumeQueue::Update);
        runtime::Value got;
        runtime::RuntimeError gotErr;
        require(exec.asyncRuntime()->awaitables().getResult(h, got, gotErr) == runtime::AwaitStatus::Succeeded &&
                std::holds_alternative<std::int64_t>(got),
            "Resumed ComputeAotSchedulerAsync did not complete");
        return std::get<std::int64_t>(got);
    };
    require(runSchedSuspended(vm) == 89 && runSchedSuspended(aotProg) == 89,
        "Suspended/resumed IAwaitScheduler parity mismatch between VM and AOT");

    const auto runSchedFaulted = [&](auto& exec) {
        ParityCustomAwaitScheduler sched;
        sched.targetTaskTypeId = taskTid;
        sched.allowCompleted = true;
        sched.faultTryGet = true;
        runtime::ExecutionOptions opts;
        opts.awaitScheduler = &sched;
        const auto res = exec.invoke(
            "Kernel.AotSuite::ComputeAotSchedulerAsync",
            {runtime::Value{std::int64_t{1}}, runtime::Value{runtime::LongValue{1}}},
            opts);
        if (res.succeeded && res.value.rawTag() == runtime::ValueTag::Struct) {
            const auto f0 = runtime::structValueFieldAt(res.value.asStructUnchecked(), 0);
            require(f0 && std::holds_alternative<runtime::LongValue>(*f0),
                "Missing AotSchedTaskInt.__handle on fault");
            const auto h = runtime::AwaitableHandle::unpack64(
                static_cast<std::uint64_t>(std::get<runtime::LongValue>(*f0).value));
            runtime::Value got;
            runtime::RuntimeError gotErr;
            require(exec.asyncRuntime()->awaitables().getResult(h, got, gotErr) == runtime::AwaitStatus::Faulted &&
                    gotErr.message.find("aot-vm-sched-fault") != std::string::npos,
                "Expected identical Faulted Awaitable on VM & AOT: " + gotErr.message);
        } else {
            require(!res.succeeded && res.error.message.find("aot-vm-sched-fault") != std::string::npos,
                "Expected identical scheduler fault on VM & AOT: " + res.error.message);
        }
    };
    runSchedFaulted(vm);
    runSchedFaulted(aotProg);

    // 2. AOT [ValueLifecycle] heap-sweep Dispose() parity via ResourceServer.
    {
        auto path1 = aotHeap->allocateString("aot_holder_sweep.png");
        require(path1.has_value(), "aotHeap allocateString failed");
        const auto holderRes = aotProg.invoke("Kernel.AotSuite::SpawnAotOrphanHolder", {runtime::Value{*path1}});
        require(holderRes.succeeded && std::holds_alternative<runtime::LongValue>(holderRes.value),
            "SpawnAotOrphanHolder failed: " + holderRes.error.message);
        const std::int64_t resId1 = std::get<runtime::LongValue>(holderRes.value).value;

        auto path2 = aotHeap->allocateString("aot_array_sweep.png");
        require(path2.has_value(), "aotHeap allocateString failed");
        const auto arrRes = aotProg.invoke("Kernel.AotSuite::SpawnAotOrphanArray", {runtime::Value{*path2}});
        require(arrRes.succeeded && std::holds_alternative<runtime::LongValue>(arrRes.value),
            "SpawnAotOrphanArray failed: " + arrRes.error.message);
        const std::int64_t resId2 = std::get<runtime::LongValue>(arrRes.value).value;

        runtime::RootSet emptyRoots;
        aotHeap->collect(emptyRoots);

        const auto use1 = aotProg.invoke("Kernel.AotSuite::QueryAotResUseCount", {runtime::Value{runtime::LongValue{resId1}}});
        const auto disp1 = aotProg.invoke("Kernel.AotSuite::QueryAotResDisposed", {runtime::Value{runtime::LongValue{resId1}}});
        const auto use2 = aotProg.invoke("Kernel.AotSuite::QueryAotResUseCount", {runtime::Value{runtime::LongValue{resId2}}});
        require(use1.succeeded && std::get<std::int64_t>(use1.value) == 0 &&
                disp1.succeeded && std::get<bool>(disp1.value),
            "Expected AOT GC sweep to Dispose() orphaned ResourceHandle in class");
        require(use2.succeeded && std::get<std::int64_t>(use2.value) == 0,
            "Expected AOT GC sweep to Dispose() orphaned ResourceHandle[] array elements");
    }

    // 3. AotSimdVectorizer Span<T> numeric loops verified via VM/AOT parity + codegen marker.
    {
        const auto resolveParamTypeId = [&](const std::string& fnName, std::size_t paramIdx) -> semantic::SymbolId {
            if (const auto sid = vmProg->findFunction(fnName)) {
                if (const auto* loc = vmProg->resolveFunction(*sid);
                    loc && loc->function && paramIdx < loc->function->parameterTypeIds.size()) {
                    return loc->function->parameterTypeIds[paramIdx];
                }
            }
            return 0;
        };
        const semantic::SymbolId effSpanIntTid =
            resolveParamTypeId("Kernel.AotSuite::SumIntSpan", 0);
        const semantic::SymbolId effSpanFloatTid =
            resolveParamTypeId("Kernel.AotSuite::ScaleFloatSpan", 0);
        const semantic::SymbolId effRoFloatTid =
            resolveParamTypeId("Kernel.AotSuite::ScaleFloatSpan", 2);
        require(effSpanIntTid != 0 && effSpanFloatTid != 0 && effRoFloatTid != 0,
            "Expected resolved Span parameter type IDs");

        constexpr std::size_t kLen = 32;
        const auto runSpan = [&](auto& exec, semantic::SymbolId intSpanId, semantic::SymbolId floatSpanId, semantic::SymbolId roFloatSpanId) -> std::pair<std::int64_t, float> {
            auto h = exec.heap();
            auto intArr = h->allocateTypedArray(semantic::stableTypeId("int[]"), semantic::PrimitiveType::Int, 0, kLen, std::int64_t{0});
            auto srcArr = h->allocateTypedArray(semantic::stableTypeId("float[]"), semantic::PrimitiveType::Float, 0, kLen, runtime::Value{runtime::FloatValue{0.0f}});
            auto dstArr = h->allocateTypedArray(semantic::stableTypeId("float[]"), semantic::PrimitiveType::Float, 0, kLen, runtime::Value{runtime::FloatValue{0.0f}});
            require(intArr && srcArr && dstArr, "Span parity array alloc failed");
            auto p1 = h->retain(runtime::Value{*intArr});
            auto p2 = h->retain(runtime::Value{*srcArr});
            auto p3 = h->retain(runtime::Value{*dstArr});
            for (std::size_t i = 0; i < kLen; ++i) {
                h->arraySet(*intArr, i, runtime::Value{static_cast<std::int64_t>(i + 1)});
                h->arraySet(*srcArr, i, runtime::Value{runtime::FloatValue{static_cast<float>(i + 1)}});
            }
            const auto intSpan = h->makeNativeSpanValue(intSpanId, *intArr, 0, static_cast<std::int32_t>(kLen));
            const auto dstSpan = h->makeNativeSpanValue(floatSpanId, *dstArr, 0, static_cast<std::int32_t>(kLen));
            const auto srcSpan = h->makeNativeSpanValue(roFloatSpanId, *srcArr, 0, static_cast<std::int32_t>(kLen));

            const auto sumRes = exec.invoke("Kernel.AotSuite::SumIntSpan", {intSpan});
            require(sumRes.succeeded, "SumIntSpan failed: " + sumRes.error.message);
            const auto scaleRes = exec.invoke(
                "Kernel.AotSuite::ScaleFloatSpan",
                {dstSpan, runtime::Value{runtime::FloatValue{2.0f}}, srcSpan});
            require(scaleRes.succeeded, "ScaleFloatSpan failed: " + scaleRes.error.message);
            return {
                std::get<std::int64_t>(sumRes.value),
                std::get<runtime::FloatValue>(scaleRes.value).value
            };
        };

        const auto [vmSum, vmFloat] = runSpan(vm, effSpanIntTid, effSpanFloatTid, effRoFloatTid);
        const auto [aotSum, aotFloat] = runSpan(aotProg, effSpanIntTid, effSpanFloatTid, effRoFloatTid);
        require(vmSum == 528 && aotSum == 528, "SumIntSpan VM vs AOT mismatch");
        require(vmFloat == aotFloat && std::abs(aotFloat - 1072.0f) < 0.01f,
            "ScaleFloatSpan VM vs AOT mismatch");

        compiler::Compilation comp({{"tests/fixtures/kernel_aot_suite.rscript", src}});
        auto build = comp.build();
        require(!build.diagnostics.hasErrors(), "AotSuite rebuild for vectorizer check failed");
        diagnostics::DiagnosticBag optDiag;
        optimization::Options optOpts;
        optOpts.level = optimization::Level::Aggressive;
        auto optResult = optimization::Optimizer{}.optimize(std::move(build.modules), optDiag, optOpts);
        require(!optDiag.hasErrors(), "AotSuite optimization failed");
        diagnostics::DiagnosticBag diag;
        aot::CppGenerator gen;
        const auto generated = gen.generate(optResult.modules, diag);
        require(!diag.hasErrors(), "AOT C++ codegen failed: " + generated.source);
        require(generated.source.find("SIMD Fast-Loop") != std::string::npos,
            "Expected AotSimdVectorizer to emit SIMD Fast-Loop for Array/Span kernels");
    }
}

// ---------------------------------------------------------------------------
// 5. L0 AttributeDescriptor Emission, IExecutionPolicy & Weak Delegate Defaults
// ---------------------------------------------------------------------------
class AotTestPolicy final : public runtime::IExecutionPolicy {
public:
    runtime::ManagedHeap* foreignHeap = nullptr;
    semantic::SymbolId interceptFnId = 0;
    bool interceptActive = true;
    bool denyFieldWrites = false;
    std::size_t interceptedCount = 0;

    [[nodiscard]] runtime::ManagedHeap* resolveHeap(
        runtime::ObjectRef reference,
        runtime::ManagedHeap* defaultHeap) const noexcept override {
        if (foreignHeap && reference.heapId == static_cast<std::uint32_t>(foreignHeap->heapId())) {
            return foreignHeap;
        }
        return defaultHeap;
    }

    bool checkFieldWrite(
        const runtime::ManagedHeap*,
        const runtime::ManagedHeap*,
        runtime::ObjectRef,
        semantic::SymbolId,
        std::size_t,
        std::string_view,
        const runtime::Value&,
        runtime::RuntimeError& outError) const override {
        if (denyFieldWrites) {
            outError.code = runtime::ErrorCode::CapabilityDenied;
            outError.message = "AOT policy denied field write";
            return false;
        }
        return true;
    }

    bool interceptCall(
        semantic::SymbolId functionId,
        runtime::ValueSpan arguments,
        bool& outIntercepted,
        runtime::Value& outResult,
        runtime::RuntimeError&) override {
        if (interceptActive && functionId == interceptFnId) {
            ++interceptedCount;
            outIntercepted = true;
            const std::int64_t x =
                (!arguments.empty() && std::holds_alternative<std::int64_t>(arguments[0]))
                    ? std::get<std::int64_t>(arguments[0])
                    : 0;
            outResult = runtime::Value{x + 500};
            return true;
        }
        outIntercepted = false;
        return true;
    }
};

bool aotPolicyFnImpl(
    aot::ExecutionContext&,
    const runtime::Value* args,
    std::size_t argCount,
    runtime::Value& result) {
    const std::int64_t x =
        (argCount >= 1 && std::holds_alternative<std::int64_t>(args[0]))
            ? std::get<std::int64_t>(args[0])
            : 0;
    result = runtime::Value{x + 1};
    return true;
}

void testAotAttributeDescriptorsPolicyAndWeakDelegates() {
    // A. AttributeDescriptor emission & attributed method exclusion from typed fast-path
    {
        const std::string src = R"RS(
module Kernel.AttrPolicy;

[MyDomainAttr]
public class HudModel {
    [MyDomainAttr]
    public int hp = 10;

    [MyDomainAttr]
    public int Score { get; set; }

    [MyDomainAttr]
    public int CommandOp(int delta) {
        hp = hp + delta;
        return hp;
    }

    public int FastHelper(HudModel m, int x) {
        return m.CommandOp(x);
    }
}
)RS";
        compiler::Compilation comp({{"attr_policy.rscript", src}});
        auto build = comp.build();
        require(!build.diagnostics.hasErrors(), "Kernel.AttrPolicy compilation failed");
        diagnostics::DiagnosticBag optDiag;
        auto optResult = optimization::Optimizer{}.optimize(std::move(build.modules), optDiag);
        require(!optDiag.hasErrors(), "Kernel.AttrPolicy optimization failed");

        diagnostics::DiagnosticBag genDiag;
        aot::CppGenerator gen;
        const auto generated = gen.generate(optResult.modules, genDiag);
        require(!genDiag.hasErrors(), "Kernel.AttrPolicy AOT codegen failed");
        require(generated.source.find("programAttributes[]") != std::string::npos,
            "Expected programAttributes table in generated AOT C++");
        require(generated.source.find("\"Kernel.AttrPolicy::HudModel\", \"MyDomainAttr\"") != std::string::npos,
            "Missing class AttributeDescriptor target");
        require(generated.source.find("\"Kernel.AttrPolicy::HudModel::field:hp\", \"MyDomainAttr\"") != std::string::npos,
            "Missing field AttributeDescriptor target");
        require(generated.source.find("\"Kernel.AttrPolicy::HudModel::property:Score\", \"MyDomainAttr\"") != std::string::npos,
            "Missing property AttributeDescriptor target");
        require(generated.source.find("\"Kernel.AttrPolicy::HudModel::method:CommandOp#1\", \"MyDomainAttr\"") != std::string::npos,
            "Missing method AttributeDescriptor target");
    }

    // B. ExecutionContext::setExecutionPolicy (interceptCall, resolveHeap, checkFieldWrite) + expired weak delegate 6-arg invokeDelegate
    {
        static const semantic::SymbolId kModelTypeId = semantic::stableTypeId("Kernel.AotPol::Model");
        static const semantic::SymbolId kDelTypeId = semantic::stableTypeId("Kernel.AotPol::IntFn");
        static const semantic::SymbolId kFnId = semantic::stableTypeId("Kernel.AotPol::Op");

        static const aot::FieldDescriptor kFields[] = {
            {"hp", semantic::PrimitiveType::Int, 0u, 0u, false, false, false},
        };
        static const aot::TypeDescriptor kTypes[] = {
            {kModelTypeId, semantic::TypeKind::Class, 0u, "Kernel.AotPol", "Model",
             kFields, 1u, nullptr, 0u, nullptr, 0u, false, nullptr, 0u, false, false, false},
            {kDelTypeId, semantic::TypeKind::Class, 0u, "Kernel.AotPol", "IntFn",
             nullptr, 0u, nullptr, 0u, nullptr, 0u, false, nullptr, 0u, true, false, false},
        };
        static const semantic::PrimitiveType kParamTypes[] = {semantic::PrimitiveType::Int};
        static const semantic::SymbolId kParamTypeIds[] = {0u};
        static const aot::FunctionDescriptor kFns[] = {
            {kFnId, "Kernel.AotPol::Op", semantic::PrimitiveType::Int, 0u,
             kParamTypes, kParamTypeIds, 1u, &aotPolicyFnImpl, nullptr},
        };
        static const char* const kModules[] = {"Kernel.AotPol"};
        static const aot::AttributeDescriptor kAttrs[] = {
            {"Kernel.AotPol::Op#1", "InterceptAttr"},
        };
        static const aot::ProgramDescriptor kDesc{
            aot::RuntimeAbiMajor,
            aot::RuntimeAbiMinor,
            "AotPolicySuite",
            0xABCDEF01ULL,
            kModules,
            1u,
            kTypes,
            2u,
            kFns,
            1u,
            nullptr,
            0u,
            kAttrs,
            1u,
        };

        auto localHeap = std::make_shared<runtime::ManagedHeap>();
        auto foreignHeap = std::make_shared<runtime::ManagedHeap>();
        auto foreignObj = foreignHeap->allocateObject(
            kModelTypeId, {runtime::Value{std::int64_t{42}}}, {});
        require(foreignObj.has_value(), "Failed to allocate foreignObj");
        auto foreignPin = foreignHeap->retain(runtime::Value{*foreignObj});

        aot::ExecutionContext ctx(kDesc, localHeap, nullptr);
        AotTestPolicy policy;
        policy.foreignHeap = foreignHeap.get();
        policy.interceptFnId = kFnId;
        ctx.setExecutionPolicy(&policy);
        require(&ctx.program() == &kDesc, "ExecutionContext::program() mismatch");

        // 1. resolveHeap allows reading field on foreignHeap
        runtime::Value loaded{};
        require(ctx.loadField(kModelTypeId, 0u, runtime::Value{*foreignObj}, loaded) &&
                std::holds_alternative<std::int64_t>(loaded) &&
                std::get<std::int64_t>(loaded) == 42,
            "AOT resolveHeap loadField failed");

        // 2. checkFieldWrite denial maps to ErrorCode::CapabilityDenied
        policy.denyFieldWrites = true;
        require(!ctx.storeField(kModelTypeId, 0u, runtime::Value{*foreignObj}, runtime::Value{std::int64_t{99}}) &&
                ctx.error().code == runtime::ErrorCode::CapabilityDenied,
            "Expected CapabilityDenied from AOT checkFieldWrite");
        policy.denyFieldWrites = false;

        // 3. interceptCall overrides invoke result
        runtime::Value outVal{};
        const runtime::Value arg{std::int64_t{12}};
        require(ctx.invoke(kFnId, &arg, 1u, outVal) &&
                std::get<std::int64_t>(outVal) == 512 && policy.interceptedCount == 1u,
            "AOT interceptCall did not return intercepted value");
        policy.interceptActive = false;
        require(ctx.invoke(kFnId, &arg, 1u, outVal) &&
                std::get<std::int64_t>(outVal) == 13,
            "AOT non-intercepted invoke expected 13");

        // 4. Expired weak delegate via 6-arg invokeDelegate returns default int64_t{0}
        auto tempTarget = localHeap->allocateObject(
            kModelTypeId, {runtime::Value{std::int64_t{9}}}, {});
        require(tempTarget.has_value(), "Failed to allocate tempTarget");
        auto weakDel = localHeap->allocateWeakDelegate(kDelTypeId, *tempTarget, kFnId);
        require(weakDel.has_value(), "allocateWeakDelegate failed in AOT test");
        auto delPin = localHeap->retain(runtime::Value{*weakDel});

        localHeap->collect(runtime::RootSet{});
        require(!localHeap->isAlive(*tempTarget), "tempTarget should be collected");

        const runtime::Value delArg{ *weakDel };
        runtime::Value delOut{};
        require(ctx.invokeDelegate(
                    kDelTypeId, &delArg, 1u, semantic::PrimitiveType::Int, 0u, delOut),
            "6-arg invokeDelegate on expired weak delegate must succeed as no-op/default: " +
                ctx.error().message);
        require(std::holds_alternative<std::int64_t>(delOut) &&
                std::get<std::int64_t>(delOut) == 0,
            "6-arg invokeDelegate on expired weak delegate must return default int64_t{0}");
    }
}

namespace {

bool hpCalcV1Body(
    aot::ExecutionContext&,
    const runtime::Value* args,
    std::size_t argc,
    runtime::Value& out) {
    if (argc != 1u || !std::holds_alternative<std::int64_t>(args[0])) return false;
    out = runtime::Value{std::get<std::int64_t>(args[0]) * 2};
    return true;
}

bool hpCalcV2Body(
    aot::ExecutionContext&,
    const runtime::Value* args,
    std::size_t argc,
    runtime::Value& out) {
    if (argc != 1u || !std::holds_alternative<std::int64_t>(args[0])) return false;
    out = runtime::Value{std::get<std::int64_t>(args[0]) * 10 + 5};
    return true;
}

bool hpCallerBody(
    aot::ExecutionContext& ctx,
    const runtime::Value* args,
    std::size_t argc,
    runtime::Value& out);

aot::HotPatchSlot kHpSlots[] = {
    {semantic::stableTypeId("Kernel.AotHp::Calc"), "Kernel.AotHp::Calc", &hpCalcV1Body, &hpCalcV1Body, 0u},
    {semantic::stableTypeId("Kernel.AotHp::Caller"), "Kernel.AotHp::Caller", &hpCallerBody, &hpCallerBody, 0u},
};

bool hpCalcThunk(
    aot::ExecutionContext& ctx,
    const runtime::Value* args,
    std::size_t argc,
    runtime::Value& out) {
    if (const auto over = ctx.resolveHotPatchImplementation(kHpSlots[0].symbolId)) {
        return over(ctx, args, argc, out);
    }
    const auto slotImpl = kHpSlots[0].patchedImplementation;
    if (slotImpl && slotImpl != &hpCalcV1Body) {
        return slotImpl(ctx, args, argc, out);
    }
    return hpCalcV1Body(ctx, args, argc, out);
}

bool hpCallerBody(
    aot::ExecutionContext& ctx,
    const runtime::Value* args,
    std::size_t argc,
    runtime::Value& out) {
    if (argc != 1u) return false;
    runtime::Value inner{};
    if (!hpCalcThunk(ctx, args, argc, inner)) return false;
    out = runtime::Value{std::get<std::int64_t>(inner) + 1};
    return true;
}

bool hpCallerThunk(
    aot::ExecutionContext& ctx,
    const runtime::Value* args,
    std::size_t argc,
    runtime::Value& out) {
    if (const auto over = ctx.resolveHotPatchImplementation(kHpSlots[1].symbolId)) {
        return over(ctx, args, argc, out);
    }
    return hpCallerBody(ctx, args, argc, out);
}

} // namespace

void testAotHotPatchThunksAndApplyMethodBodyHotPatch() {
    // 1. Verify AotCpp GenerationOptions::emitHotPatchThunks emits slot dispatch table + thunks
    {
        const std::string src = R"RS(
module Kernel.AotEmitHp;
public int Helper(int x) { return x * 3; }
public int Entry(int x) { return Helper(x) + 1; }
)RS";
        compiler::Compilation comp({{"emit_hp.rscript", src}});
        auto build = comp.build();
        require(!build.diagnostics.hasErrors(), "AOT emit hot-patch compile failed");
        diagnostics::DiagnosticBag optDiag;
        auto opt = optimization::Optimizer{}.optimize(std::move(build.modules), optDiag);
        require(!optDiag.hasErrors(), "AOT emit hot-patch opt failed");

        aot::GenerationOptions opts;
        opts.programName = "AotEmitHp";
        opts.cppNamespace = "aot_emit_hp_ns";
        opts.emitHotPatchThunks = true;
        diagnostics::DiagnosticBag aotDiag;
        const auto gen = aot::CppGenerator{}.generate(opt.modules, aotDiag, opts);
        require(!aotDiag.hasErrors(), "AotCpp emitHotPatchThunks generation failed");
        require(gen.source.find("programHotPatchSlots") != std::string::npos &&
                gen.source.find("_body(") != std::string::npos &&
                gen.source.find("resolveHotPatchImplementation") != std::string::npos &&
                gen.source.find("patchedImplementation") != std::string::npos,
            "Generated AOT C++ source missing emitHotPatchThunks slot/thunk dispatch");
    }

    // 2. Verify AotRuntime applyMethodBodyHotPatch end-to-end with HotReload ReloadPlan
    const std::string rV1 = R"RS(
module Kernel.AotHp;
public struct Pair { public int a; public int b; }
public int Calc(int x) { return x * 2; }
public int Caller(int x) { return Calc(x) + 1; }
)RS";
    const std::string rV2MethodOnly = R"RS(
module Kernel.AotHp;
public struct Pair { public int a; public int b; }
public int Calc(int x) { return x * 10 + 5; }
public int Caller(int x) { return Calc(x) + 1; }
)RS";
    const std::string rV3LayoutChanged = R"RS(
module Kernel.AotHp;
public struct Pair { public int a; public int b; public int c; }
public int Calc(int x) { return x * 10 + 5; }
public int Caller(int x) { return Calc(x) + 1; }
)RS";

    auto compileMods = [](const std::string& code) {
        compiler::Compilation c({{"hp.rscript", code}});
        auto b = c.build();
        require(!b.diagnostics.hasErrors(), "Compile hp failed");
        bytecode::Lowerer lowerer;
        std::vector<bytecode::Module> bc;
        for (const auto& m : b.modules) bc.push_back(lowerer.lower(m));
        runtime::RuntimeError le;
        auto linked = runtime::ProgramImage::link(bc, le);
        require(linked.has_value(), "Link hp failed");
        return std::make_pair(
            std::make_shared<const runtime::ProgramImage>(std::move(*linked)),
            std::move(bc));
    };

    const auto [imgV1, bcV1] = compileMods(rV1);
    const auto [imgV2, bcV2] = compileMods(rV2MethodOnly);
    const auto [imgV3, bcV3] = compileMods(rV3LayoutChanged);
    (void)bcV1;
    (void)imgV2;
    (void)imgV3;

    const auto optCalcId = imgV1->findFunction("Kernel.AotHp::Calc");
    const auto optCallerId = imgV1->findFunction("Kernel.AotHp::Caller");
    require(optCalcId.has_value() && optCallerId.has_value(),
        "Expected compiled symbolIds for Kernel.AotHp::Calc and Caller");
    const semantic::SymbolId kPairId = semantic::stableTypeId("Kernel.AotHp::Pair");
    const semantic::SymbolId kCalcId = *optCalcId;
    const semantic::SymbolId kCallerId = *optCallerId;
    kHpSlots[0].symbolId = kCalcId;
    kHpSlots[0].originalImplementation = &hpCalcV1Body;
    kHpSlots[0].patchedImplementation = &hpCalcV1Body;
    kHpSlots[0].revision = 0u;
    kHpSlots[1].symbolId = kCallerId;
    kHpSlots[1].originalImplementation = &hpCallerBody;
    kHpSlots[1].patchedImplementation = &hpCallerBody;
    kHpSlots[1].revision = 0u;

    const aot::FieldDescriptor kPairFieldsV1[] = {
        {"a", semantic::PrimitiveType::Int, 0u, 0u, false, false, false},
        {"b", semantic::PrimitiveType::Int, 0u, 1u, false, false, false},
    };
    const aot::TypeDescriptor kTypesV1[] = {
        {kPairId, semantic::TypeKind::Struct, 0u, "Kernel.AotHp", "Pair",
         kPairFieldsV1, 2u, nullptr, 0u, nullptr, 0u, false, nullptr, 0u, false, false, false},
    };
    const semantic::PrimitiveType kOneInt[] = {semantic::PrimitiveType::Int};
    const semantic::SymbolId kOneZeroId[] = {0u};
    const aot::FunctionDescriptor kFnsV1[] = {
        {kCalcId, "Kernel.AotHp::Calc", semantic::PrimitiveType::Int, 0u, kOneInt, kOneZeroId, 1u, &hpCalcThunk, nullptr},
        {kCallerId, "Kernel.AotHp::Caller", semantic::PrimitiveType::Int, 0u, kOneInt, kOneZeroId, 1u, &hpCallerThunk, nullptr},
    };
    const aot::FunctionDescriptor kFnsV2[] = {
        {kCalcId, "Kernel.AotHp::Calc", semantic::PrimitiveType::Int, 0u, kOneInt, kOneZeroId, 1u, &hpCalcV2Body, nullptr},
        {kCallerId, "Kernel.AotHp::Caller", semantic::PrimitiveType::Int, 0u, kOneInt, kOneZeroId, 1u, &hpCallerThunk, nullptr},
    };
    static const char* const kMods[] = {"Kernel.AotHp"};

    const aot::ProgramDescriptor descV1{
        aot::RuntimeAbiMajor, aot::RuntimeAbiMinor, "AotHpV1", 0x1111u,
        kMods, 1u, kTypesV1, 1u, kFnsV1, 2u, nullptr, 0u, nullptr, 0u,
        kHpSlots, 2u,
    };
    const aot::ProgramDescriptor descV2{
        aot::RuntimeAbiMajor, aot::RuntimeAbiMinor, "AotHpV2", 0x2222u,
        kMods, 1u, kTypesV1, 1u, kFnsV2, 2u, nullptr, 0u, nullptr, 0u,
        nullptr, 0u,
    };

    aot::Program aotProg(descV1);

    // Before patch: Calc(7) == 14, Caller(7) == 15 + verify AOT ProfileCollector steady_clock span recording
    auto aotProfiler = std::make_shared<runtime::ProfileCollector>();
    runtime::ExecutionOptions profOpts;
    profOpts.profile = aotProfiler;
    const auto preRes = aotProg.invoke(kCallerId, {runtime::Value{std::int64_t{7}}}, profOpts);
    require(preRes.succeeded && std::get<std::int64_t>(preRes.value) == 15,
        "Pre-patch Caller(7) expected 15");
    require(!aotProfiler->snapshot().traceEvents.empty(),
        "AOT invoke with ProfileCollector must record steady_clock traceEvents");

    // Prepare real ReloadPlan for method-body-only edit and apply patch
    const hot_reload::ReloadPlan methodBodyPlan = hot_reload::prepare(*imgV1, bcV2);
    require(methodBodyPlan.compatible && !methodBodyPlan.structuralMigrationRequired &&
            methodBodyPlan.layoutChangedTypes.empty(),
        "Expected method-body-only ReloadPlan to be compatible without structural migration");

    const auto patchOk = aotProg.applyMethodBodyHotPatch(methodBodyPlan, descV2);
    require(patchOk.applied && patchOk.revision == 1u && patchOk.patchedMethodCount >= 1u &&
            kHpSlots[0].patchedImplementation == &hpCalcV2Body && kHpSlots[0].revision == 1u,
        "applyMethodBodyHotPatch failed for method-body-only edit: " + patchOk.message);

    // After patch: both direct invoke AND thunked caller see v2 implementation (7 * 10 + 5 = 75; +1 = 76)
    const auto postCalc = aotProg.invoke(kCalcId, {runtime::Value{std::int64_t{7}}});
    const auto postCaller = aotProg.invoke(kCallerId, {runtime::Value{std::int64_t{7}}});
    require(postCalc.succeeded && std::get<std::int64_t>(postCalc.value) == 75 &&
            postCaller.succeeded && std::get<std::int64_t>(postCaller.value) == 76,
        "Post-patch AOT execution expected 75 and 76");

    // 3. STRICT rejection when structuralMigrationRequired is true
    {
        hot_reload::ReloadOptions structOpts;
        structOpts.allowStructuralChanges = true;
        const hot_reload::ReloadPlan structuralPlan = hot_reload::prepare(*imgV1, bcV3, structOpts);
        require(structuralPlan.structuralMigrationRequired && !structuralPlan.layoutChangedTypes.empty(),
            "Expected structural ReloadPlan from layout change");

        const auto rejStruct = aotProg.applyMethodBodyHotPatch(structuralPlan, descV2);
        require(!rejStruct.applied && aotProg.hotPatchRevision() == 1u,
            "applyMethodBodyHotPatch MUST reject structuralMigrationRequired=true");
    }

    // 4. STRICT rejection when layoutChangedTypes is non-empty even if structuralMigrationRequired flag is false
    {
        hot_reload::ReloadPlan layoutOnlyPlan;
        layoutOnlyPlan.compatible = true;
        layoutOnlyPlan.structuralMigrationRequired = false;
        layoutOnlyPlan.changedFunctions = {kCalcId};
        layoutOnlyPlan.layoutChangedTypes = {kPairId};
        const auto rejLayout = aotProg.applyMethodBodyHotPatch(
            layoutOnlyPlan, {{kCalcId, &hpCalcV1Body}});
        require(!rejLayout.applied && aotProg.hotPatchRevision() == 1u &&
                kHpSlots[0].patchedImplementation == &hpCalcV2Body,
            "applyMethodBodyHotPatch MUST reject non-empty layoutChangedTypes");
    }
}

void testAotPluginMacroDependency() {
    aot::Program program(kernel_aot_plugin_ns::KernelAotPluginProgram());
    const auto res = program.invoke(
        "Kernel.AotPlugin::RunAotPluginMacro",
        {runtime::Value{std::int64_t{10}}});
    require(res.succeeded && std::holds_alternative<std::int64_t>(res.value) &&
            std::get<std::int64_t>(res.value) == 1465,
        "Kernel.AotPlugin::RunAotPluginMacro(10) expected 1465, got: " +
            res.error.message);
}

void testAotActiveGcProfileAndTelemetry() {
    aot::Program aotProg(kernel_aot_ns::KernelAotSuiteProgram());
    auto heap = std::make_shared<runtime::ManagedHeap>();
    auto asyncCtx = std::make_shared<runtime::AsyncRuntimeContext>();
    aotProg.setHeap(heap);
    aotProg.setAsyncRuntime(asyncCtx);

    const auto boxArr = heap->allocateTypedArray(
        semantic::stableTypeId("int[]"), semantic::PrimitiveType::Int, 0, 16, std::int64_t{7});
    require(boxArr.has_value(), "allocateTypedArray seed failed");
    auto pinArr = heap->retain(runtime::Value{*boxArr});
    heap->requestCollection();
    runtime::ShadowStack emptyRoots;
    (void)heap->step(emptyRoots, 1);

    auto profile = std::make_shared<runtime::ProfileCollector>();
    runtime::FrameTelemetryCollector telemetry;
    runtime::ActiveFrameTelemetryScope telemetryScope(&telemetry);
    telemetry.beginFrame(1u);

    runtime::ExecutionOptions opts;
    opts.profile = profile;
    opts.limits.gcWorkBudget = 8;

    const auto suiteRes = aotProg.invoke(
        "Kernel.AotSuite::RunOopDelegatesGenericsAndCollections",
        {runtime::Value{std::int64_t{3}}, runtime::Value{std::int64_t{5}}},
        opts);
    require(suiteRes.succeeded, "AOT suite invoke under telemetry failed: " + suiteRes.error.message);

    // Also verify trustedRelease-compiled AOT code honors ActiveFrameTelemetryScope.
    aot::Program trustedProg(kernel_aot_trusted_ns::KernelAotTrustedProgram());
    trustedProg.setHeap(std::make_shared<runtime::ManagedHeap>());
    trustedProg.setAsyncRuntime(std::make_shared<runtime::AsyncRuntimeContext>());
    runtime::ExecutionOptions trustedOpts;
    trustedOpts.trustedRelease = true;
    const auto trustedRes = trustedProg.invoke(
        "Kernel.AotTrusted::RunSyncScalarBatch",
        {runtime::Value{std::int64_t{32}}},
        trustedOpts);
    require(trustedRes.succeeded, "AOT trusted invoke under telemetry failed: " + trustedRes.error.message);

    const auto snap = profile->snapshot();
    require(!snap.traceEvents.empty() && !snap.functions.empty(),
        "Expected ProfileCollector events & function summaries from AOT invoke");
    bool hasNonZeroAotInst = false;
    for (const auto& fp : snap.functions) {
        if (fp.calls > 0u && fp.instructions > 0u) {
            hasNonZeroAotInst = true;
            break;
        }
    }
    require(hasNonZeroAotInst, "Expected non-zero AOT ProfileCollector function instructions");

    const auto frameSample = telemetry.endFrame(heap.get());
    require(frameSample.totalInstructions > 0u && frameSample.hotFunctionCount >= 1u,
        "Expected FrameTelemetryCollector to record AOT self instructions");
}

void testAotParallelBatchExecution() {
    const std::string src = readFixtureFile("tests/fixtures/kernel_aot_suite.rscript");
    auto program = compileFixtureForVm("tests/fixtures/kernel_aot_suite.rscript", src);

    compiler::Compilation comp({{"tests/fixtures/kernel_aot_suite.rscript", src}});
    auto build = comp.build();
    require(!build.diagnostics.hasErrors(), "AotSuite rebuild for parallel batch check failed");
    diagnostics::DiagnosticBag optDiag;
    optimization::Options optOpts;
    optOpts.level = optimization::Level::Aggressive;
    const auto optResult = optimization::Optimizer{}.optimize(std::move(build.modules), optDiag, optOpts);
    require(!optDiag.hasErrors(), "AotSuite optimization failed");
    diagnostics::DiagnosticBag genDiag;
    const auto generated = aot::CppGenerator{}.generate(optResult.modules, genDiag);
    require(!genDiag.hasErrors(), "AOT C++ codegen failed");
    const auto checkEmittedBatch = [&](const char* qualName) {
        const auto pos = generated.source.find(qualName);
        require(pos != std::string::npos, std::string("Missing AOT descriptor for ") + qualName);
        const auto lineEnd = generated.source.find('\n', pos);
        const auto line = generated.source.substr(
            pos, lineEnd == std::string::npos ? std::string::npos : (lineEnd - pos));
        require(line.find("_parallel_batch") != std::string::npos,
            std::string("Expected AOT descriptor with _parallel_batch for ") + qualName);
    };
    checkEmittedBatch("Kernel.AotSuite::ParallelStepElement");
    checkEmittedBatch("Kernel.AotSuite::ParallelRangeStep");

    auto vmHeap = std::make_shared<runtime::ManagedHeap>();
    auto aotHeap = std::make_shared<runtime::ManagedHeap>();
    constexpr std::size_t kCount = 128;
    auto initArrays = [&](runtime::ManagedHeap& h) {
        const auto arrTypeId = semantic::stableTypeId("float[]");
        auto src = h.allocateTypedArray(
            arrTypeId,
            semantic::PrimitiveType::Float,
            0,
            kCount,
            runtime::Value{runtime::FloatValue{0.0f}});
        auto dst = h.allocateTypedArray(
            arrTypeId,
            semantic::PrimitiveType::Float,
            0,
            kCount,
            runtime::Value{runtime::FloatValue{0.0f}});
        require(src.has_value() && dst.has_value(), "Packed float array allocation failed");
        for (std::size_t i = 0; i < kCount; ++i) {
            require(h.arraySet(
                *src, i, runtime::Value{runtime::FloatValue{static_cast<float>(i + 1) * 0.5f}}),
                "arraySet float failed");
        }
        return std::pair<runtime::ObjectRef, runtime::ObjectRef>{*src, *dst};
    };

    auto [vmSrc, vmDst] = initArrays(*vmHeap);
    auto [aotSrc, aotDst] = initArrays(*aotHeap);

    runtime::ParallelJobScheduler scheduler(4);
    runtime::ParallelJobConfig elemCfg;
    elemCfg.workerCount = 4;
    elemCfg.batchSize = 16;
    elemCfg.reduceOp = runtime::ParallelReduceOp::Sum;

    const auto vmElem = scheduler.dispatchFor(
        program,
        vmHeap,
        "Kernel.AotSuite::ParallelStepElement",
        kCount,
        {runtime::Value{vmDst}, runtime::Value{vmSrc}, runtime::Value{runtime::FloatValue{2.5f}}},
        elemCfg);
    require(vmElem.succeeded && vmElem.totalItemsProcessed == kCount,
        "VM parallel dispatchFor failed: " + vmElem.error.message);

    auto [vmRangeSrc, vmRangeDst] = initArrays(*vmHeap);
    auto [aotRangeSrc, aotRangeDst] = initArrays(*aotHeap);
    runtime::ParallelJobConfig rangeCfg;
    rangeCfg.workerCount = 4;
    rangeCfg.batchSize = 32;
    const auto vmRange = scheduler.dispatchRange(
        program,
        vmHeap,
        "Kernel.AotSuite::ParallelRangeStep",
        kCount,
        {runtime::Value{vmRangeDst}, runtime::Value{vmRangeSrc}, runtime::Value{runtime::FloatValue{1.5f}}},
        rangeCfg);
    require(vmRange.succeeded && vmRange.totalItemsProcessed == kCount,
        "VM parallel dispatchRange failed: " + vmRange.error.message);

    aot::Program aotProg(kernel_aot_ns::KernelAotSuiteProgram());
    aotProg.setHeap(aotHeap);
    aotProg.attachToProgramImage(*program);
    aotProg.resetParallelBatchExecutionCount();

    const auto aotElem = scheduler.dispatchFor(
        program,
        aotHeap,
        "Kernel.AotSuite::ParallelStepElement",
        kCount,
        {runtime::Value{aotDst}, runtime::Value{aotSrc}, runtime::Value{runtime::FloatValue{2.5f}}},
        elemCfg);
    require(aotElem.succeeded && aotElem.totalItemsProcessed == kCount,
        "AOT parallel dispatchFor failed: " + aotElem.error.message);
    require(aotElem.usedNativeBatchKernel,
        "Expected AOT parallel dispatchFor to use native batch kernel");
    require(aotProg.parallelBatchExecutionCount() > 0,
        "Expected non-zero AOT parallelBatchExecutionCount after dispatchFor");
    require(std::fabs(std::get<runtime::FloatValue>(aotElem.value).value -
                      std::get<runtime::FloatValue>(vmElem.value).value) < 1e-3f,
        "AOT parallel reduction mismatch against VM");

    aotProg.resetParallelBatchExecutionCount();
    const auto aotRange = scheduler.dispatchRange(
        program,
        aotHeap,
        "Kernel.AotSuite::ParallelRangeStep",
        kCount,
        {runtime::Value{aotRangeDst}, runtime::Value{aotRangeSrc}, runtime::Value{runtime::FloatValue{1.5f}}},
        rangeCfg);
    require(aotRange.succeeded && aotRange.totalItemsProcessed == kCount,
        "AOT parallel dispatchRange failed: " + aotRange.error.message);
    require(aotRange.usedNativeBatchKernel,
        "Expected AOT parallel dispatchRange to use native batch kernel");
    require(aotProg.parallelBatchExecutionCount() > 0,
        "Expected non-zero AOT parallelBatchExecutionCount after dispatchRange");

    for (std::size_t i = 0; i < kCount; ++i) {
        const auto ea = aotHeap->arrayGet(aotDst, i);
        const auto eb = vmHeap->arrayGet(vmDst, i);
        require(ea && eb && std::fabs(std::get<runtime::FloatValue>(*ea).value -
                                      std::get<runtime::FloatValue>(*eb).value) < 1e-4f,
            "AOT parallel element dst parity mismatch against VM");
        const auto ra = aotHeap->arrayGet(aotRangeDst, i);
        const auto rb = vmHeap->arrayGet(vmRangeDst, i);
        require(ra && rb && std::fabs(std::get<runtime::FloatValue>(*ra).value -
                                      std::get<runtime::FloatValue>(*rb).value) < 1e-4f,
            "AOT parallel range dst parity mismatch against VM");
    }
}

void testAotAllocAttributionRemarksAndCli() {
    // 1. AotRuntime GC.Alloc attribution into FrameTelemetryCollector
    {
        aot::Program aotProg(kernel_aot_ns::KernelAotSuiteProgram());
        auto heap = std::make_shared<runtime::ManagedHeap>();
        auto asyncCtx = std::make_shared<runtime::AsyncRuntimeContext>();
        aotProg.setHeap(heap);
        aotProg.setAsyncRuntime(asyncCtx);

        runtime::FrameTelemetryCollector telemetry;
        runtime::ActiveFrameTelemetryScope telemetryScope(&telemetry);
        telemetry.beginFrame(1u);

        const auto suiteRes = aotProg.invoke(
            "Kernel.AotSuite::RunOopDelegatesGenericsAndCollections",
            {runtime::Value{std::int64_t{3}}, runtime::Value{std::int64_t{5}}});
        require(suiteRes.succeeded, "AOT suite invoke failed: " + suiteRes.error.message);

        const auto sample = telemetry.endFrame(heap.get());
        require(sample.frameAllocCount > 0u,
            "Expected AotRuntime FrameTelemetryCollector frameAllocCount > 0");
        require(sample.frameAllocBytes > 0u,
            "Expected AotRuntime FrameTelemetryCollector frameAllocBytes > 0");
        require(sample.hotAllocationCount >= 1u,
            "Expected AotRuntime FrameTelemetryCollector hotAllocationCount >= 1");
    }

    // 2. CppGenerator optimization remarks + ModuleBuildCache warm-cache remark bypass + rsaot CLI
    const std::string src = R"RS(
module Kernel.AotRemarks;

public class RemarkFault {
    public string msg;
    public RemarkFault(string m) { msg = m; }
}

public class Item {
    public int val = 0;
}

public int PureLoop(int n) {
    int sum = 0;
    for (int i = 0; i < n; i = i + 1) {
        sum = sum + i;
    }
    return sum;
}

public Item AllocWork(int x) {
    Item it = new Item();
    it.val = x;
    return it;
}

public int BoxedTryCatch(int x) {
    int r = 0;
    try {
        if (x > 0) {
            throw new RemarkFault("fail");
        }
    } catch (RemarkFault ex) {
        r = ex.msg.Length;
    }
    return r;
}
)RS";

    compiler::Compilation comp({{"aot_remarks.rscript", src}});
    auto build = comp.build();
    require(!build.diagnostics.hasErrors(), "AotRemarks compile failed");
    diagnostics::DiagnosticBag optDiag;
    optimization::Options optOpts;
    optOpts.level = optimization::Level::Aggressive;
    const auto optResult = optimization::Optimizer{}.optimize(std::move(build.modules), optDiag, optOpts);
    require(!optDiag.hasErrors(), "AotRemarks optimization failed");

    diagnostics::DiagnosticBag genDiag;
    aot::GenerationOptions aotOpts;
    aotOpts.programName = "RemarksTest";
    aotOpts.cppNamespace = "remarks_test_ns";
    aotOpts.collectOptimizationRemarks = true;
    const auto generated = aot::CppGenerator{}.generate(optResult.modules, genDiag, aotOpts);
    require(!genDiag.hasErrors(), "AotRemarks codegen failed");
    require(!generated.optimizationRemarks.empty(), "Expected non-empty optimizationRemarks");

    bool sawPurePassed = false;
    bool sawTryMissed = false;
    bool sawAllocSite = false;
    for (const auto& remark : generated.optimizationRemarks) {
        if (remark.kind == aot::OptimizationRemarkKind::Passed &&
            remark.pass == "aot-lowering" &&
            remark.functionName.find("PureLoop") != std::string::npos) {
            sawPurePassed = true;
        } else if (remark.kind == aot::OptimizationRemarkKind::Missed &&
                   remark.pass == "aot-lowering" &&
                   remark.functionName.find("BoxedTryCatch") != std::string::npos &&
                   !remark.message.empty()) {
            sawTryMissed = true;
        } else if (remark.kind == aot::OptimizationRemarkKind::Analysis &&
                   remark.pass == "gc-alloc" &&
                   remark.functionName.find("AllocWork") != std::string::npos &&
                   remark.line > 0u) {
            sawAllocSite = true;
        }
    }
    require(sawPurePassed, "Missing Passed aot-lowering remark on PureLoop");
    require(sawTryMissed, "Missing Missed aot-lowering remark on BoxedTryCatch");
    require(sawAllocSite, "Missing Analysis gc-alloc remark with line > 0 on AllocWork");

    struct LocalTempDirGuard {
        std::filesystem::path dir;
        explicit LocalTempDirGuard(const std::string& name) {
            dir = std::filesystem::temp_directory_path() / name;
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
            std::filesystem::create_directories(dir, ec);
        }
        ~LocalTempDirGuard() {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }
        LocalTempDirGuard(const LocalTempDirGuard&) = delete;
        LocalTempDirGuard& operator=(const LocalTempDirGuard&) = delete;
    };

    LocalTempDirGuard guard("rs_aot_remarks_test");
    compiler::ModuleBuildCache cache(guard.dir / "cache");
    compiler::ModuleBuildCacheOptions cacheOpts;
    cacheOpts.cacheDirectory = guard.dir / "cache";
    cacheOpts.optimizationOptions.level = optimization::Level::Aggressive;
    const std::vector<compiler::SourceFile> cachedSources = {{"aot_remarks.rscript", src}};

    const auto cold = cache.buildAot(cachedSources, cacheOpts, aotOpts);
    require(cold.succeeded(), "ModuleBuildCache cold buildAot failed");
    require(!cold.aotCacheHit, "Expected cold buildAot aotCacheHit == false");
    require(!cold.program.optimizationRemarks.empty(),
        "Expected non-empty optimizationRemarks on cold buildAot");

    const auto warm = cache.buildAot(cachedSources, cacheOpts, aotOpts);
    require(warm.succeeded(), "ModuleBuildCache warm buildAot failed");
    require(!warm.program.optimizationRemarks.empty(),
        "Expected warm buildAot to preserve optimizationRemarks when collectOptimizationRemarks is enabled");

#ifdef REALSCRIPT_RSAOT_PATH
    const auto srcFile = guard.dir / "aot_remarks.rscript";
    const auto outDir = guard.dir / "out";
    const auto jsonFile = guard.dir / "remarks.json";
    const auto cliLog = guard.dir / "cli.log";
    std::filesystem::create_directories(outDir);
    {
        std::ofstream outSrc(srcFile, std::ios::binary | std::ios::trunc);
        require(outSrc.is_open(), "Failed to write temp aot_remarks.rscript");
        outSrc << src;
    }

    const std::string cmdLine =
        "\"" + std::string(REALSCRIPT_RSAOT_PATH) + "\" \"" + srcFile.string() +
        "\" --output-dir \"" + outDir.string() +
        "\" --program-name RemarksCli --opt-remarks --opt-remarks-json \"" +
        jsonFile.string() + "\" > \"" + cliLog.string() + "\" 2>&1";
#ifdef _WIN32
    const std::string wrappedCmd = "\"" + cmdLine + "\"";
    const int cliRc = std::system(wrappedCmd.c_str());
#else
    const int cliRc = std::system(cmdLine.c_str());
#endif
    require(cliRc == 0, "rsaot --opt-remarks-json exited non-zero");

    std::ifstream inJson(jsonFile, std::ios::binary);
    require(inJson.is_open(), "Missing remarks.json output from rsaot");
    std::ostringstream jsonBuf;
    jsonBuf << inJson.rdbuf();
    const std::string jsonText = jsonBuf.str();
    require(jsonText.find("\"kind\"") != std::string::npos &&
            jsonText.find("\"pass\"") != std::string::npos &&
            jsonText.find("\"aot-lowering\"") != std::string::npos &&
            jsonText.find("\"gc-alloc\"") != std::string::npos,
        "rsaot remarks.json missing expected remark fields");
#endif
}

void testWaveRegressionAttributedTypedAndStructInt32() {
    const std::string src = R"RS(
module Kernel.AotWaveReg;

public struct GridCoord {
    public int x;
    public int y;
}

public class AttrTagAttribute {
    public AttrTagAttribute() {}
}

[AttrTag]
public int TaggedScale(int v) {
    GridCoord c = new GridCoord();
    c.x = v;
    c.y = v * 2;
    return c.x + c.y;
}

public int CallTagged(int v) {
    return TaggedScale(v) + 1;
}

public void VecAddLoop(int[] dst, int[] src, int n) {
    for (int i = 0; i < n; i = i + 1) {
        dst[i] = unchecked(dst[i] + src[i]);
    }
}
)RS";

    compiler::Compilation comp({{"aot_wave_reg.rscript", src}});
    auto build = comp.build();
    require(!build.diagnostics.hasErrors(), "Kernel.AotWaveReg compile failed");

    diagnostics::DiagnosticBag optDiag;
    optimization::Options optOpts;
    optOpts.level = optimization::Level::Aggressive;
    optOpts.enableInlining = false;
    auto optResult = optimization::Optimizer{}.optimize(std::move(build.modules), optDiag, optOpts);
    require(!optDiag.hasErrors(), "Kernel.AotWaveReg optimization failed");

    diagnostics::DiagnosticBag genDiag;
    aot::GenerationOptions genOpts;
    genOpts.programName = "WaveRegAot";
    genOpts.cppNamespace = "wave_reg_aot_ns";
    const auto generated = aot::CppGenerator{}.generate(optResult.modules, genDiag, genOpts);
    require(!genDiag.hasErrors(), "Kernel.AotWaveReg codegen failed");

    require(generated.source.find("#define REALSCRIPT_AOT_GENERATED_TU 1") != std::string::npos,
        "Expected REALSCRIPT_AOT_GENERATED_TU guard at top of generated AOT .cpp");
    require(generated.header.find("REALSCRIPT_AOT_GENERATED_TU") == std::string::npos,
        "Generated public AOT .h must NOT define REALSCRIPT_AOT_GENERATED_TU");
    require(generated.source.find("std::int32_t f_0 = std::int32_t{0}; // x") != std::string::npos,
        "Expected std::int32_t field f_0 inside generated AotStruct_ for GridCoord");

    const std::string srcWithFn = generated.source;
    require(srcWithFn.find("_typed(") != std::string::npos &&
            srcWithFn.find("TaggedScale") != std::string::npos,
        "Expected custom-attributed TaggedScale to emit typed AOT implementation");
    require(srcWithFn.find("context.executionPolicy() != nullptr") != std::string::npos,
        "Expected typed direct call to check context.executionPolicy() != nullptr");
    require(srcWithFn.find("TrustedRelease || FastAccounting") != std::string::npos,
        "Expected vectorized ECS loop to allow FastAccounting fast path");
}

bool auditHotPatchLiteralProbe(
    aot::ExecutionContext& ctx,
    const runtime::Value*,
    std::size_t,
    runtime::Value& result) {
    runtime::Value firstVal;
    runtime::Value secondVal;
    if (!ctx.constantStringIndexed(0u, "tu_alpha_literal", firstVal)) {
        return false;
    }
    if (!ctx.constantStringIndexed(0u, "tu_beta_patched_literal", secondVal)) {
        return false;
    }
    const auto heap = ctx.heap();
    if (!heap ||
        firstVal.rawTag() != runtime::ValueTag::ObjectRef ||
        secondVal.rawTag() != runtime::ValueTag::ObjectRef) {
        return false;
    }
    const auto s1 = heap->stringView(firstVal.asObjectRefUnchecked());
    const auto s2 = heap->stringView(secondVal.asObjectRefUnchecked());
    const bool ok = s1.has_value() && s2.has_value() &&
        *s1 == std::string_view("tu_alpha_literal") &&
        *s2 == std::string_view("tu_beta_patched_literal");
    result = runtime::Value{ok};
    return true;
}

void testAuditRegressionULongAsyncCoroAndHotPatchStrings() {
    const std::string src = readFixtureFile("tests/fixtures/kernel_aot_suite.rscript");
    auto vmProg = compileFixtureForVm("tests/fixtures/kernel_aot_suite.rscript", src);

    runtime::EngineRuntime vm(vmProg);
    aot::Program aotProg(kernel_aot_ns::KernelAotSuiteProgram());
    auto aotHeap = std::make_shared<runtime::ManagedHeap>();
    auto aotAsync = std::make_shared<runtime::AsyncRuntimeContext>();
    aotProg.setHeap(aotHeap);
    aotProg.setAsyncRuntime(aotAsync);

    // 1. VM vs AOT unsigned ulong arithmetic, division/remainder, checked overflow & SIMD parity.
    {
        const std::vector<runtime::Value> arithArgs{
            runtime::Value{runtime::ULongValue{0x8000000000000005ULL}},
            runtime::Value{runtime::ULongValue{19ULL}},
        };
        const auto vmArith = vm.invoke("Kernel.AotSuite::ULongArithAndDiv", arithArgs);
        const auto aotArith = aotProg.invoke("Kernel.AotSuite::ULongArithAndDiv", arithArgs);
        require(vmArith.succeeded && aotArith.succeeded &&
                std::holds_alternative<runtime::ULongValue>(vmArith.value) &&
                std::holds_alternative<runtime::ULongValue>(aotArith.value) &&
                std::get<runtime::ULongValue>(vmArith.value).value ==
                    std::get<runtime::ULongValue>(aotArith.value).value,
            "ULongArithAndDiv VM vs AOT parity mismatch");

        const auto vmOkAdd = vm.invoke("Kernel.AotSuite::ULongCheckedAdd",
            {runtime::Value{runtime::ULongValue{100ULL}}, runtime::Value{runtime::ULongValue{200ULL}}});
        const auto aotOkAdd = aotProg.invoke("Kernel.AotSuite::ULongCheckedAdd",
            {runtime::Value{runtime::ULongValue{100ULL}}, runtime::Value{runtime::ULongValue{200ULL}}});
        require(vmOkAdd.succeeded && aotOkAdd.succeeded &&
                std::get<runtime::ULongValue>(vmOkAdd.value).value == 300ULL &&
                std::get<runtime::ULongValue>(aotOkAdd.value).value == 300ULL,
            "ULongCheckedAdd(100, 200) parity mismatch");

        const std::vector<runtime::Value> ovArgs{
            runtime::Value{runtime::ULongValue{std::numeric_limits<std::uint64_t>::max()}},
            runtime::Value{runtime::ULongValue{1ULL}},
        };
        const auto vmOv = vm.invoke("Kernel.AotSuite::ULongCheckedAdd", ovArgs);
        const auto aotOv = aotProg.invoke("Kernel.AotSuite::ULongCheckedAdd", ovArgs);
        require(!vmOv.succeeded && vmOv.error.code == runtime::ErrorCode::IntegerOverflow &&
                !aotOv.succeeded && aotOv.error.code == runtime::ErrorCode::IntegerOverflow,
            "ULongCheckedAdd(UINT64_MAX, 1) must trap with IntegerOverflow on both VM and AOT");

        const auto ulongArrTid = semantic::stableTypeId(
            semantic::arrayTypeName(semantic::PrimitiveType::ULong, ""));
        auto vmSrc = vm.heap()->allocateTypedArray(
            ulongArrTid, semantic::PrimitiveType::ULong, 0, 16u, runtime::Value{runtime::ULongValue{0ULL}});
        auto vmDst = vm.heap()->allocateTypedArray(
            ulongArrTid, semantic::PrimitiveType::ULong, 0, 16u, runtime::Value{runtime::ULongValue{0ULL}});
        auto aotSrc = aotHeap->allocateTypedArray(
            ulongArrTid, semantic::PrimitiveType::ULong, 0, 16u, runtime::Value{runtime::ULongValue{0ULL}});
        auto aotDst = aotHeap->allocateTypedArray(
            ulongArrTid, semantic::PrimitiveType::ULong, 0, 16u, runtime::Value{runtime::ULongValue{0ULL}});
        require(vmSrc && vmDst && aotSrc && aotDst, "Failed to allocate ulong[] arrays");
        for (std::size_t i = 0; i < 16u; ++i) {
            const auto v = runtime::Value{runtime::ULongValue{0x8000000000000000ULL + i}};
            require(vm.heap()->arraySet(*vmSrc, i, v) && aotHeap->arraySet(*aotSrc, i, v),
                "arraySet failed for ULongSimdScale");
        }
        const auto vmSimd = vm.invoke("Kernel.AotSuite::ULongSimdScale",
            {runtime::Value{*vmDst}, runtime::Value{*vmSrc}, runtime::Value{std::int64_t{16}}});
        const auto aotSimd = aotProg.invoke("Kernel.AotSuite::ULongSimdScale",
            {runtime::Value{*aotDst}, runtime::Value{*aotSrc}, runtime::Value{std::int64_t{16}}});
        require(vmSimd.succeeded && aotSimd.succeeded,
            "ULongSimdScale execution failed");
        for (std::size_t i = 0; i < 16u; ++i) {
            const auto ev = vm.heap()->arrayGet(*vmDst, i);
            const auto ea = aotHeap->arrayGet(*aotDst, i);
            require(ev && ea &&
                    std::get<runtime::ULongValue>(*ev).value == std::get<runtime::ULongValue>(*ea).value,
                "ULongSimdScale element parity mismatch");
        }
    }

    // 2. Suspended AOT typed async coroutine keeps string + string-holding struct live across GC.
    {
        const auto taskTid = semantic::stableTypeId("Kernel.AotSuite::AotSchedTaskInt");
        const auto runAsyncWithGc = [&](auto& exec, runtime::ManagedHeap& targetHeap) -> std::int64_t {
            ParityCustomAwaitScheduler sched;
            sched.targetTaskTypeId = taskTid;
            sched.allowCompleted = false;
            sched.nextValue = 5;
            auto dynPrefix = targetHeap.allocateString("coro_gc_prefix");
            require(dynPrefix.has_value(), "allocateString prefix failed");
            runtime::ExecutionOptions opts;
            opts.awaitScheduler = &sched;
            const auto res = exec.invoke(
                "Kernel.AotSuite::ComputeAsyncStringGcSurvival",
                {runtime::Value{*dynPrefix}, runtime::Value{std::int64_t{3}}, runtime::Value{runtime::LongValue{77}}},
                opts);
            require(res.succeeded && res.value.rawTag() == runtime::ValueTag::Struct &&
                    sched.suspendCount == 1 && sched.savedResume != nullptr,
                "ComputeAsyncStringGcSurvival did not suspend as expected: " + res.error.message);
            const auto f0 = runtime::structValueFieldAt(res.value.asStructUnchecked(), 0);
            require(f0 && std::holds_alternative<runtime::LongValue>(*f0), "Missing task handle");
            const auto h = runtime::AwaitableHandle::unpack64(
                static_cast<std::uint64_t>(std::get<runtime::LongValue>(*f0).value));

            // Collect while coroutine frame holds the only references to dyn & pair.Label!
            targetHeap.collect(runtime::RootSet{});
            targetHeap.collect(runtime::RootSet{});

            sched.allowCompleted = true;
            sched.savedResume();
            exec.asyncRuntime()->stepResumeQueue(runtime::ResumeQueue::Update);
            exec.asyncRuntime()->stepResumeQueue(runtime::ResumeQueue::Update);
            runtime::Value got;
            runtime::RuntimeError gotErr;
            require(exec.asyncRuntime()->awaitables().getResult(h, got, gotErr) == runtime::AwaitStatus::Succeeded &&
                    std::holds_alternative<std::int64_t>(got),
                "Resumed ComputeAsyncStringGcSurvival failed: " + gotErr.message);
            return std::get<std::int64_t>(got);
        };
        const auto vmVal = runAsyncWithGc(vm, *vm.heap());
        const auto aotVal = runAsyncWithGc(aotProg, *aotHeap);
        require(vmVal == 80 && aotVal == 80,
            "ComputeAsyncStringGcSurvival expected 80 (string GC survival across await)");
    }

    // 3. ExecutionContext::constantStringIndexed collision safety across distinct strings in slot 0.
    {
        const semantic::SymbolId probeFnId = semantic::stableTypeId("Kernel.AuditStringSlot::Probe");
        const aot::FunctionDescriptor fns[] = {
            {probeFnId, "Kernel.AuditStringSlot::Probe", semantic::PrimitiveType::Bool, 0u,
             nullptr, nullptr, 0u, &auditHotPatchLiteralProbe, nullptr},
        };
        static const char* const mods[] = {"Kernel.AuditStringSlot"};
        const aot::ProgramDescriptor desc{
            aot::RuntimeAbiMajor, aot::RuntimeAbiMinor, "AuditStrSlot", 0x9999u,
            mods, 1u, nullptr, 0u, fns, 1u, nullptr, 0u, nullptr, 0u, nullptr, 0u,
        };
        aot::Program probeProg(desc);
        probeProg.setHeap(aotHeap);
        const auto res = probeProg.invoke(probeFnId);
        require(res.succeeded && std::holds_alternative<bool>(res.value) && std::get<bool>(res.value),
            "ExecutionContext::constantStringIndexed slot collision overwrote or returned wrong string literal");
    }

    // 4. Direct ExecutionContext with contentHash == 0 must still populate type/function maps
    // and never hit stale descriptorCache entries across reused stack descriptors.
    {
        const semantic::SymbolId zeroFnA = semantic::stableTypeId("Kernel.AuditZeroHash::FnA");
        const semantic::SymbolId zeroFnB = semantic::stableTypeId("Kernel.AuditZeroHash::FnB");
        auto probeImpl = +[](
            aot::ExecutionContext&,
            const runtime::Value*,
            std::size_t,
            runtime::Value& result) -> bool {
            result = runtime::Value{true};
            return true;
        };
        auto runZeroHash = [&](semantic::SymbolId activeFnId, semantic::SymbolId otherFnId) {
            const aot::FunctionDescriptor fn{
                activeFnId, "Kernel.AuditZeroHash::Probe", semantic::PrimitiveType::Bool, 0u,
                nullptr, nullptr, 0u, probeImpl, nullptr,
            };
            aot::ProgramDescriptor desc{};
            desc.name = "ZeroHashProgram";
            desc.contentHash = 0u;
            desc.functions = &fn;
            desc.functionCount = 1u;

            aot::ExecutionContext ctx(desc, aotHeap, nullptr);
            runtime::Value outVal{};
            require(ctx.invoke(activeFnId, nullptr, 0u, outVal) &&
                    std::holds_alternative<bool>(outVal) &&
                    std::get<bool>(outVal),
                "ExecutionContext(contentHash == 0) failed to populate/invoke active function");
            runtime::Value staleVal{};
            require(!ctx.invoke(otherFnId, nullptr, 0u, staleVal),
                "ExecutionContext(contentHash == 0) unexpectedly resolved wrong/stale function");
        };
        runZeroHash(zeroFnA, zeroFnB);
        runZeroHash(zeroFnB, zeroFnA);
    }
}

} // namespace

int main() {
    try {
        std::vector<std::pair<const char*, void (*)()>> tests = {
            {"Kernel.AotSuite VM vs AOT parity & SIMD kernels", testAotSuiteParityAndSimdKernels},
            {"Kernel.AotTrusted TrustedRelease & scalar coroutines parity", testAotTrustedScalarCoroutinesParity},
            {"CppGenerator #line directive emission control", testAotCppLineDirectivesGeneration},
            {"Pillar 2 & 4: AotRuntime IAwaitScheduler, [ValueLifecycle] sweep & Span SIMD", testAotAwaitSchedulerValueLifecycleAndSpanSimd},
            {"L0 AttributeDescriptor emission, IExecutionPolicy & AOT weak delegate default return", testAotAttributeDescriptorsPolicyAndWeakDelegates},
            {"AotCpp emitHotPatchThunks & AotRuntime applyMethodBodyHotPatch", testAotHotPatchThunksAndApplyMethodBodyHotPatch},
            {"realscript_add_aot_library COMPILER_PLUGINS dependency & macro expansion", testAotPluginMacroDependency},
            {"AotRuntime active GC, ProfileCollector & ActiveFrameTelemetryScope (incl. TrustedRelease)", testAotActiveGcProfileAndTelemetry},
            {"AotRuntime native ParallelJobScheduler _parallel_batch & SIMD range execution", testAotParallelBatchExecution},
            {"AotRuntime GC.Alloc attribution, CppGenerator optimization remarks & rsaot CLI", testAotAllocAttributionRemarksAndCli},
            {"Wave 3 regression: AOT typed attributed call, int32 AotStruct & FastAccounting SIMD", testWaveRegressionAttributedTypedAndStructInt32},
            {"Audit regression: ulong arithmetic/overflow/SIMD, async string GC & literal slot collision", testAuditRegressionULongAsyncCoroAndHotPatchStrings},
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
