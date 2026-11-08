#include "realscript/aot_cpp/AotCpp.h"
#include "realscript/aot_cpp/AotRuntime.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/game/GameScripting.h"
#include "realscript/optimization/Optimizer.h"
#include "realscript/runtime/Runtime.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace bench_ns_math {
const realscript::aot::ProgramDescriptor& BenchProgram_mathProgram() noexcept;
} // namespace bench_ns_math

namespace bench_ns_ecs {
const realscript::aot::ProgramDescriptor& BenchProgram_ecsProgram() noexcept;
} // namespace bench_ns_ecs

using namespace realscript;

namespace {

void requireCond(bool cond, const std::string& msg) {
    if (!cond) {
        std::cerr << "[FAIL] " << msg << "\n";
        std::exit(1);
    }
}

std::string readFixtureFile(const std::string& relativePath) {
    const std::string fullPath = std::string(REALSCRIPT_SOURCE_DIR) + "/" + relativePath;
    std::ifstream in(fullPath, std::ios::binary);
    requireCond(static_cast<bool>(in), "Failed to open fixture file: " + fullPath);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct HostTransform {
    float x;
    float y;
    float z;
};

struct HostVelocity {
    float vx;
    float vy;
    float vz;
};

struct HostGridPos {
    std::int32_t gx;
    std::int32_t gy;
};

std::int64_t nativeStepCalc(std::int64_t v) {
    return (v * 733LL + 12345LL) % 1000003LL;
}

std::int64_t nativeComputeSum(std::int64_t n) {
    std::int64_t acc = 1;
    for (std::int64_t i = 0; i < n; ++i) {
        acc = nativeStepCalc(acc + i);
    }
    return acc;
}

} // namespace

int main() {
    std::cout << "=== [Test 1] Zero-Budget AOT Trusted Release Mode vs RAW Mode ===\n";
    {
        const std::string mathScript = readFixtureFile("tests/fixtures/aot_trusted_math_bench.rs");

        compiler::Compilation comp({{"aot_trusted_math_bench.rs", mathScript}});
        auto res = comp.build();
        requireCond(!res.diagnostics.hasErrors(), "math_bench compile failed");

        diagnostics::DiagnosticBag optDiags;
        optimization::Optimizer opt;
        optimization::Options optOpts;
        optOpts.level = optimization::Level::Aggressive;
        auto optRes = opt.optimize(res.modules, optDiags, optOpts);
        requireCond(!optDiags.hasErrors(), "math_bench optimization failed");

        // Verify in-process CppGenerator codegen for both trustedRelease = false and true
        {
            diagnostics::DiagnosticBag diagsDefault;
            aot::GenerationOptions genDefault;
            genDefault.programName = "BenchProgram_math";
            genDefault.cppNamespace = "bench_ns_math";
            genDefault.trustedRelease = false;
            aot::CppGenerator generator;
            const auto outDefault = generator.generate(optRes.modules, diagsDefault, genDefault);
            requireCond(!diagsDefault.hasErrors() && !outDefault.source.empty(), "default AOT codegen failed");

            diagnostics::DiagnosticBag diagsTrusted;
            aot::GenerationOptions genTrusted = genDefault;
            genTrusted.trustedRelease = true;
            const auto outTrusted = generator.generate(optRes.modules, diagsTrusted, genTrusted);
            requireCond(!diagsTrusted.hasErrors() && !outTrusted.source.empty(), "trustedRelease AOT codegen failed");
            requireCond(
                outTrusted.source.find("_typed<false, false, true>") != std::string::npos,
                "trustedRelease codegen should dispatch to TrustedRelease=true specialization");
        }

        const aot::ProgramDescriptor& desc = bench_ns_math::BenchProgram_mathProgram();
        aot::Program aotProg(desc);

        const std::int64_t N = 500000;
        const std::int64_t nativeExpected = nativeComputeSum(N);

        // 1. Default AOT (Accounting + Guards)
        runtime::ExecutionOptions optsRaw;
        optsRaw.limits.instructionBudget = 100'000'000;
        optsRaw.determinism.mode = runtime::DeterminismMode::Off;
        optsRaw.trustedRelease = false;
        auto t0 = std::chrono::steady_clock::now();
        auto resRaw = aotProg.invoke("math_bench::computeSum", {N}, optsRaw);
        auto t1 = std::chrono::steady_clock::now();
        requireCond(resRaw.succeeded, "AOT raw execution failed: " + resRaw.error.message);
        requireCond(resRaw.instructionsExecuted > N, "Default AOT mode must account instructions");

        // 2. Trusted Release AOT (Zero-Budget / Zero-Guard)
        runtime::ExecutionOptions optsTrusted;
        optsTrusted.determinism.mode = runtime::DeterminismMode::Off;
        optsTrusted.trustedRelease = true;
        auto t2 = std::chrono::steady_clock::now();
        auto resTrusted = aotProg.invoke("math_bench::computeSum", {N}, optsTrusted);
        auto t3 = std::chrono::steady_clock::now();
        requireCond(resTrusted.succeeded, "AOT trusted execution failed: " + resTrusted.error.message);

        // 3. Native C++ baseline
        auto t4 = std::chrono::steady_clock::now();
        volatile std::int64_t sink = nativeComputeSum(N);
        auto t5 = std::chrono::steady_clock::now();
        (void)sink;

        requireCond(
            std::get<std::int64_t>(resRaw.value) == nativeExpected,
            "RAW result mismatch with native");
        requireCond(
            std::get<std::int64_t>(resTrusted.value) == nativeExpected,
            "Trusted result mismatch with native");
        requireCond(
            resTrusted.instructionsExecuted == 0 &&
                resTrusted.statistics.instructionsExecuted == 0,
            "Trusted release mode must bypass instruction accounting");

        double rawMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        double trustedMs = std::chrono::duration<double, std::milli>(t3 - t2).count();
        double nativeMs = std::chrono::duration<double, std::milli>(t5 - t4).count();

        std::cout << "  Native C++ Baseline : " << nativeMs << " ms\n";
        std::cout << "  AOT Default (Raw)   : " << rawMs << " ms\n";
        std::cout << "  AOT Trusted Release : " << trustedMs << " ms (Speedup vs Raw: "
                  << (trustedMs > 0.0 ? (rawMs / trustedMs) : 1.0) << "x)\n";
    }

    std::cout << "\n=== [Test 2] Zero-Copy Host C++ Contiguous Struct ECS Query (Interpreter & AOT) ===\n";
    {
        const std::string ecsScript = readFixtureFile("tests/fixtures/aot_trusted_ecs_bench.rs");

        game::GameApi api;
        game::GameScriptCompiler compiler(api);
        auto compiled = compiler.compile({{"aot_trusted_ecs_bench.rs", ecsScript}});
        requireCond(compiled.succeeded(), "ECS script compilation failed");

        game::ScriptRuntime runtime(compiled.program);

        const auto ecsSystemsType = runtime.findType("ecs_bench::EcsSystems");
        requireCond(ecsSystemsType.has_value(), "ecs_bench::EcsSystems type not found");
        const auto stepMovementMethod = runtime.findMethod(*ecsSystemsType, "stepMovement", 1);
        const auto stepGridMethod = runtime.findMethod(*ecsSystemsType, "stepGrid", 2);
        const auto stepFilteredGridMethod = runtime.findMethod(*ecsSystemsType, "stepFilteredGrid", 2);
        const auto verifyEdgeCasesMethod = runtime.findMethod(*ecsSystemsType, "verifyEdgeCases", 0);
        requireCond(
            stepMovementMethod.has_value() &&
                stepGridMethod.has_value() &&
                stepFilteredGridMethod.has_value() &&
                verifyEdgeCasesMethod.has_value(),
            "EcsSystems static methods not found");

        const std::size_t entityCount = 10000;
        std::vector<HostTransform> hostTransforms(entityCount);
        std::vector<HostVelocity> hostVelocities(entityCount);
        std::vector<HostGridPos> hostGrids(entityCount);

        for (std::size_t i = 0; i < entityCount; ++i) {
            hostTransforms[i] = HostTransform{
                static_cast<float>(i),
                static_cast<float>(i * 2),
                static_cast<float>(i * 3)};
            hostVelocities[i] = HostVelocity{1.5f, -0.5f, 2.0f};
            hostGrids[i] = HostGridPos{static_cast<std::int32_t>(i * 10), static_cast<std::int32_t>(i * 20)};
        }

        // Verify layout validation rejects invalid field offset / strideBytes == 0
        runtime::RuntimeError err;
        {
            const std::vector<runtime::NativeFieldLayout> validFields = {
                {0, runtime::NativeFieldKind::Float32},
                {4, runtime::NativeFieldKind::Float32},
                {8, runtime::NativeFieldKind::Float32},
            };
            requireCond(
                !runtime.bindEcsRawColumn(
                    "ecs_bench::Transform",
                    hostTransforms.data(),
                    entityCount,
                    0,
                    validFields,
                    err,
                    false),
                "bindEcsRawColumn must reject strideBytes == 0");

            std::vector<runtime::NativeFieldLayout> outOfBoundsFields = validFields;
            outOfBoundsFields[2].byteOffset = sizeof(HostTransform); // 12 + 4 > 12
            requireCond(
                !runtime.bindEcsRawColumn(
                    "ecs_bench::Transform",
                    hostTransforms.data(),
                    entityCount,
                    sizeof(HostTransform),
                    outOfBoundsFields,
                    err,
                    false),
                "bindEcsRawColumn must reject field offset exceeding strideBytes");
        }

        // Bind host vectors directly via zero-copy raw column view
        err = {};
        requireCond(
            runtime.bindEcsRawColumnAuto<HostTransform>(
                "ecs_bench::Transform",
                hostTransforms.data(),
                hostTransforms.size(),
                err,
                false),
            "Failed to bind Transform raw column: " + err.message);

        const HostVelocity* constVelocityPtr = hostVelocities.data();
        requireCond(
            runtime.bindEcsRawColumnAuto<const HostVelocity>(
                "ecs_bench::Velocity",
                constVelocityPtr,
                hostVelocities.size(),
                err,
                false),
            "Failed to bind Velocity raw column: " + err.message);

        // HostGridPos has std::int32_t fields, which bindEcsRawColumnAuto auto-detects via sizeof(HostGridPos)
        requireCond(
            runtime.bindEcsRawColumnAuto<HostGridPos>(
                "ecs_bench::GridPos",
                hostGrids.data(),
                hostGrids.size(),
                err),
            "Failed to bind GridPos raw column: " + err.message);

        // Step 1: Execute via Bytecode Interpreter first to verify Interpreter Zero-Copy support
        auto interpRes = runtime.invokeStatic(*stepMovementMethod, {runtime::FloatValue{0.5f}});
        requireCond(interpRes.succeeded, "Interpreter stepMovement failed: " + interpRes.error.message);

        for (std::size_t i = 0; i < 10; ++i) {
            float expectedX = static_cast<float>(i) + 1.5f * 0.5f;
            float expectedY = static_cast<float>(i * 2) - 0.5f * 0.5f;
            float expectedZ = static_cast<float>(i * 3) + 2.0f * 0.5f;
            requireCond(std::fabs(hostTransforms[i].x - expectedX) < 1e-4f, "Interpreter X mismatch");
            requireCond(std::fabs(hostTransforms[i].y - expectedY) < 1e-4f, "Interpreter Y mismatch");
            requireCond(std::fabs(hostTransforms[i].z - expectedZ) < 1e-4f, "Interpreter Z mismatch");
        }
        std::cout << "  [OK] Interpreter Zero-Copy ECS Raw Column Read/Write verified.\n";

        // Step 2: Attach CMake-built AOT backend with Trusted Release enabled
        const aot::ProgramDescriptor& ecsAotDesc = bench_ns_ecs::BenchProgram_ecsProgram();
        runtime.setAotBackend(&ecsAotDesc);
        requireCond(
            runtime.activeBackend() == game::ScriptExecutionBackend::Aot,
            "Failed to attach ECS AOT backend");
        runtime::ExecutionOptions execOpts;
        execOpts.determinism.mode = runtime::DeterminismMode::Off;
        execOpts.trustedRelease = true;

        auto t0 = std::chrono::steady_clock::now();
        auto aotRes = runtime.invokeStatic(*stepMovementMethod, {runtime::FloatValue{1.0f}}, execOpts);
        auto t1 = std::chrono::steady_clock::now();
        requireCond(aotRes.succeeded, "AOT stepMovement failed: " + aotRes.error.message);

        for (std::size_t i = 0; i < entityCount; ++i) {
            float expectedX = static_cast<float>(i) + 1.5f * 1.5f;
            float expectedY = static_cast<float>(i * 2) - 0.5f * 1.5f;
            float expectedZ = static_cast<float>(i * 3) + 2.0f * 1.5f;
            if (std::fabs(hostTransforms[i].x - expectedX) > 1e-3f ||
                std::fabs(hostTransforms[i].y - expectedY) > 1e-3f ||
                std::fabs(hostTransforms[i].z - expectedZ) > 1e-3f) {
                requireCond(false, "AOT Zero-Copy Transform mismatch at index " + std::to_string(i));
            }
        }

        // Verify int32_t field auto-detection in GridPos
        auto gridRes = runtime.invokeStatic(*stepGridMethod, {std::int64_t{5}, std::int64_t{-3}}, execOpts);
        requireCond(gridRes.succeeded, "AOT stepGrid failed: " + gridRes.error.message);
        for (std::size_t i = 0; i < 20; ++i) {
            requireCond(
                hostGrids[i].gx == static_cast<std::int32_t>(i * 10 + 5),
                "AOT GridPos gx mismatch");
            requireCond(
                hostGrids[i].gy == static_cast<std::int32_t>(i * 20 - 3),
                "AOT GridPos gy mismatch");
        }

        // Verify in-place RawColumnArray slot reuse when rebinding per-frame
        const auto heapBeforeRebind = runtime.heap()->statistics().objectsAllocated;
        std::vector<HostTransform> hostTransforms2(entityCount, HostTransform{10.0f, 20.0f, 30.0f});
        requireCond(
            runtime.bindEcsRawColumnAuto<HostTransform>(
                "ecs_bench::Transform",
                hostTransforms2.data(),
                hostTransforms2.size(),
                err,
                false),
            "Rebind Transform raw column failed: " + err.message);
        const auto heapAfterRebind = runtime.heap()->statistics().objectsAllocated;
        requireCond(
            heapAfterRebind == heapBeforeRebind,
            "Rebinding RawColumnArray must update existing HeapSlot in-place with zero new allocations");

        auto rebindStepRes = runtime.invokeStatic(*stepMovementMethod, {runtime::FloatValue{2.0f}}, execOpts);
        requireCond(rebindStepRes.succeeded, "AOT stepMovement after rebind failed");
        requireCond(
            std::fabs(hostTransforms2[0].x - 13.0f) < 1e-4f &&
                std::fabs(hostTransforms2[0].y - 19.0f) < 1e-4f &&
                std::fabs(hostTransforms2[0].z - 34.0f) < 1e-4f,
            "Rebound hostTransforms2 values were not updated in-place");

        // Measure 200 frames of 10,000 entities and verify ZERO heap allocations!
        const auto allocBeforeFrames = runtime.heap()->statistics().objectsAllocated;
        auto benchStart = std::chrono::steady_clock::now();
        for (int frame = 0; frame < 200; ++frame) {
            auto r = runtime.invokeStatic(*stepMovementMethod, {runtime::FloatValue{0.016f}}, execOpts);
            requireCond(r.succeeded, "Frame stepMovement failed");
        }
        auto benchEnd = std::chrono::steady_clock::now();
        const auto allocAfterFrames = runtime.heap()->statistics().objectsAllocated;
        const auto allocDiff = allocAfterFrames - allocBeforeFrames;

        double singleStepMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        double avgFrameMs = std::chrono::duration<double, std::milli>(benchEnd - benchStart).count() / 200.0;

        std::cout << "  [OK] AOT Zero-Copy ECS (10,000 entities) single step : " << singleStepMs << " ms\n";
        std::cout << "  [OK] AOT Zero-Copy ECS (10,000 entities) 200-frame avg: " << avgFrameMs << " ms/frame\n";
        std::cout << "  [OK] GC Heap Allocations across 200 frames            : " << allocDiff << "\n";
        requireCond(allocDiff == 0, "Zero-Copy ECS AOT loop must perform ZERO heap allocations per frame");

        // Step 3: Verify sparse/filtered ECS writeback works in AOT Trusted mode when sparse entities exist
        const auto grid10 = *runtime.createStruct("ecs_bench::GridPos", {std::int64_t{100}, std::int64_t{200}}, err);
        const auto grid20 = *runtime.createStruct("ecs_bench::GridPos", {std::int64_t{300}, std::int64_t{400}}, err);
        const auto activeVal = *runtime.createStruct("ecs_bench::ActiveTag", {std::int64_t{1}}, err);
        requireCond(runtime.setEcsEntityComponent(10, "ecs_bench::GridPos", grid10, err), err.message);
        requireCond(runtime.setEcsEntityComponent(10, "ecs_bench::ActiveTag", activeVal, err), err.message);
        requireCond(runtime.setEcsEntityComponent(20, "ecs_bench::GridPos", grid20, err), err.message);

        auto filteredRes = runtime.invokeStatic(
            *stepFilteredGridMethod,
            {std::int64_t{7}, std::int64_t{9}},
            execOpts);
        requireCond(
            filteredRes.succeeded && std::get<std::int64_t>(filteredRes.value) == 10,
            "AOT stepFilteredGrid should visit only entity 10");
        const auto updatedGrid10 = runtime.getEcsEntityComponent(10, "ecs_bench::GridPos", err);
        const auto untouchedGrid20 = runtime.getEcsEntityComponent(20, "ecs_bench::GridPos", err);
        requireCond(updatedGrid10.has_value() && untouchedGrid20.has_value(), "getEcsEntityComponent failed");
        requireCond(
            *runtime::structValueFieldAt(std::get<runtime::StructValue>(*updatedGrid10), 0) ==
                runtime::Value{std::int64_t{107}} &&
            *runtime::structValueFieldAt(std::get<runtime::StructValue>(*updatedGrid10), 1) ==
                runtime::Value{std::int64_t{209}},
            "AOT sparse ECS writeback must update entity 10");
        requireCond(
            *runtime::structValueFieldAt(std::get<runtime::StructValue>(*untouchedGrid20), 0) ==
                runtime::Value{std::int64_t{300}},
            "Entity 20 without ActiveTag must remain untouched");
        std::cout << "  [OK] AOT Sparse/Filtered ECS Writeback verified.\n";

        // Step 4: Verify StructWithStatic, StructWithEnum, and reassigned array locals in AOT
        requireCond(
            !runtime.bindEcsRawColumnAuto<HostGridPos>(
                "ecs_bench::StructWithStatic",
                hostGrids.data(),
                hostGrids.size(),
                err),
            "bindEcsRawColumnAuto must reject structs with static fields");
        auto edgeRes = runtime.invokeStatic(*verifyEdgeCasesMethod, {}, execOpts);
        requireCond(
            edgeRes.succeeded && std::get<std::int64_t>(edgeRes.value) == (32 + 5 + 6 + 10 + 40),
            "verifyEdgeCases failed or returned wrong sum");
        std::cout << "  [OK] AOT StructWithEnum, StructWithStatic & multi-assign array locals verified.\n";
    }

    std::cout << "\nAll Trusted AOT & Zero-Copy ECS tests passed successfully!\n";
    return 0;
}
