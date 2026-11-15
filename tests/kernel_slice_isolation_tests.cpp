#include "realscript/aot_cpp/AotRuntime.h"
#include "realscript/bindgen/HostApi.h"
#include "realscript/hot_reload/HotReload.h"
#include "realscript/jit/StencilJit.h"
#include "realscript/runtime/Runtime.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace {

using namespace realscript;

// Critical ABI invariant: runtime::Value and runtime::ObjectRef stay 16-byte aligned.
static_assert(
    sizeof(runtime::Value) == 16 && alignof(runtime::Value) == 16 &&
        sizeof(runtime::ObjectRef) == 16,
    "SlimRuntime Value/ObjectRef ABI invariants must hold");

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bytecode::Module makeComputeConstantModule(std::int64_t aVal, std::int64_t bVal) {
    bytecode::Module mod;
    mod.name = "SliceIso.Math";
    mod.version = bytecode::Version{};

    bytecode::Function fn;
    fn.symbolId = semantic::stableTypeId("SliceIso.Math::Compute");
    fn.accessibility = semantic::Accessibility::Public;
    fn.name = "Compute";
    fn.returnType = semantic::PrimitiveType::Int;
    fn.registerTypes = {
        semantic::PrimitiveType::Int,
        semantic::PrimitiveType::Int,
        semantic::PrimitiveType::Int,
    };
    fn.registerTypeIds = {0u, 0u, 0u};

    bytecode::BasicBlock bb;
    bb.id = 0;

    bytecode::Instruction i0;
    i0.opcode = bytecode::Opcode::ConstantInt;
    i0.result = 0;
    i0.integerImmediate = aVal;

    bytecode::Instruction i1;
    i1.opcode = bytecode::Opcode::ConstantInt;
    i1.result = 1;
    i1.integerImmediate = bVal;

    bytecode::Instruction i2;
    i2.opcode = bytecode::Opcode::AddInt;
    i2.result = 2;
    i2.operands.push_back(0);
    i2.operands.push_back(1);

    bb.instructions.push_back(i0);
    bb.instructions.push_back(i1);
    bb.instructions.push_back(i2);

    bb.terminator.kind = bytecode::TerminatorKind::ReturnValue;
    bb.terminator.value = 2;

    fn.blocks.push_back(std::move(bb));
    mod.functions.push_back(std::move(fn));
    return mod;
}

void testSliceIsolationBindGenStencilJitAndHotReload() {
    // 1. BindGenRuntime slice (HostApi works without CompilerCore or RsBindParser)
    {
        bindgen::HostApi api;
        const bool ok = api.function(
            "SliceIso.Native",
            "SumTwo",
            [](std::int64_t x, std::int64_t y) -> std::int64_t { return x + y; });
        require(ok && api.valid(), "HostApi::function registration failed in slice isolation test");
        require(
            api.nativeBindingNames().count("SliceIso.Native::__rs_native_SumTwo") == 1u,
            "HostApi missing expected native binding name");
        const auto sources = api.generatedSources();
        require(sources.size() == 1u, "HostApi::generatedSources expected 1 module source");
        require(
            sources[0].content.find("SumTwo") != std::string::npos,
            "HostApi generated source missing registered function");
    }

    // 2. StencilJit slice (compiles & executes bytecode::Module without CompilerCore/ToolchainJit)
    require(jit::ExecutableCodeArena::systemPageSize() > 0u, "systemPageSize must be non-zero");
    const bytecode::Module modV1 = makeComputeConstantModule(20, 22);
    {
        jit::StencilJit sj;
        const auto compileRes = sj.compile(std::vector<bytecode::Module>{modV1});
        require(
            compileRes.succeeded() && compileRes.module,
            "StencilJit::compile failed in slice isolation test: " + compileRes.error);
        const auto execRes = compileRes.module->invoke("SliceIso.Math::Compute");
        require(
            execRes.succeeded && std::holds_alternative<std::int64_t>(execRes.value) &&
                std::get<std::int64_t>(execRes.value) == 42,
            "StencilJit Compute expected 42");
    }

    // 3. HotReload slice (prepares & applies v1 -> v2 runtime reload without CompilerCore)
    {
        runtime::RuntimeError loadErr;
        auto linkedV1 = runtime::ProgramImage::link(
            std::vector<bytecode::Module>{modV1},
            loadErr);
        require(
            linkedV1.has_value(),
            "ProgramImage::link(v1) failed: " + loadErr.message);
        auto programV1 = std::make_shared<const runtime::ProgramImage>(std::move(*linkedV1));
        runtime::EngineRuntime engine(programV1);
        const auto v1Res = engine.invoke("SliceIso.Math::Compute");
        require(
            v1Res.succeeded && std::get<std::int64_t>(v1Res.value) == 42,
            "EngineRuntime v1 Compute expected 42");

        const bytecode::Module modV2 = makeComputeConstantModule(70, 29);
        const auto plan = hot_reload::apply(engine, std::vector<bytecode::Module>{modV2});
        require(plan.compatible && plan.program != nullptr, "HotReload apply v2 should be compatible");

        const auto v2Res = engine.invoke("SliceIso.Math::Compute");
        require(
            v2Res.succeeded && std::get<std::int64_t>(v2Res.value) == 99,
            "EngineRuntime v2 Compute after HotReload expected 99");
    }
}

} // namespace

int main() {
    try {
        testSliceIsolationBindGenStencilJitAndHotReload();
        std::cout << "[PASS] Slice isolation: BindGenRuntime + StencilJit + HotReload without CompilerCore\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[FAIL] Slice isolation: " << ex.what() << '\n';
        return 1;
    }
}
