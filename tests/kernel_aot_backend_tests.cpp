#include "realscript/aot_cpp/AotCpp.h"
#include "realscript/aot_cpp/AotRuntime.h"
#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/optimization/Optimizer.h"
#include "realscript/runtime/Runtime.h"

#include <cmath>
#include <cstdint>
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

} // namespace

int main() {
    try {
        std::vector<std::pair<const char*, void (*)()>> tests = {
            {"Kernel.AotSuite VM vs AOT parity & SIMD kernels", testAotSuiteParityAndSimdKernels},
            {"Kernel.AotTrusted TrustedRelease & scalar coroutines parity", testAotTrustedScalarCoroutinesParity},
            {"CppGenerator #line directive emission control", testAotCppLineDirectivesGeneration},
            {"Pillar 2 & 4: AotRuntime IAwaitScheduler, [ValueLifecycle] sweep & Span SIMD", testAotAwaitSchedulerValueLifecycleAndSpanSimd},
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
