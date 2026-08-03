#include "realscript_aot_generated.h"

#include "realscript/aot_cpp/AotRuntime.h"
#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/runtime/Runtime.h"

#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string readFile(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot read fixture: " + path);
    std::ostringstream content;
    content << stream.rdbuf();
    return content.str();
}

std::vector<realscript::bytecode::Module> compileFixture() {
    realscript::compiler::Compilation compilation;
    compilation.addSource({
        "tests/fixtures/phase5_model.rs",
        readFile(std::string(REALSCRIPT_SOURCE_DIR) +
            "/tests/fixtures/phase5_model.rs"),
    });
    compilation.addSource({
        "tests/fixtures/phase5_app.rs",
        readFile(std::string(REALSCRIPT_SOURCE_DIR) +
            "/tests/fixtures/phase5_app.rs"),
    });
    auto build = compilation.build();
    require(!build.diagnostics.hasErrors(), "Phase 5 fixture compilation failed");
    realscript::bytecode::Lowerer lowerer;
    std::vector<realscript::bytecode::Module> modules;
    for (const auto& module : build.modules) {
        modules.push_back(lowerer.lower(module));
    }
    return modules;
}

void requireSameValue(
    const realscript::runtime::ExecutionResult& interpreter,
    const realscript::runtime::ExecutionResult& aot,
    const realscript::runtime::ManagedHeap* interpreterHeap,
    const realscript::runtime::ManagedHeap* aotHeap,
    const std::string& name) {
    require(interpreter.succeeded == aot.succeeded,
        name + " backend success state diverged");
    require(interpreter.error.code == aot.error.code,
        name + " backend error code diverged");
    require(interpreter.instructionsExecuted == aot.instructionsExecuted,
        name + " backend instruction accounting diverged");
    if (!interpreter.succeeded) {
        require(interpreter.error.stackTrace == aot.error.stackTrace,
            name + " backend stack trace diverged");
        return;
    }
    require(realscript::runtime::valueType(interpreter.value) ==
            realscript::runtime::valueType(aot.value),
        name + " backend result type diverged");
    require(realscript::runtime::valueToString(interpreter.value, interpreterHeap) ==
            realscript::runtime::valueToString(aot.value, aotHeap),
        name + " backend result value diverged");
}

void testDifferentialExecution() {
    auto modules = compileFixture();
    realscript::runtime::Interpreter interpreter(std::move(modules));
    realscript::aot::Program aot(realscript_generated::Phase5FixtureProgram());

    const std::vector<std::pair<std::string, std::vector<realscript::runtime::Value>>>
        calls = {
            {"Phase5.App::main", {}},
            {"Phase5.App::sumArray", {std::int64_t{8}}},
            {"Phase5.App::objectMath", {}},
            {"Phase5.App::structMath", {}},
            {"Phase5.App::enumMath", {}},
            {"Phase5.App::longMath", {}},
            {"Phase5.App::doubleMath", {}},
            {"Phase5.App::greeting", {}},
            {"Phase5.App::failDivision", {std::int64_t{0}}},
            {"Phase5.App::failOverflow", {}},
            {"Phase5.App::failLongOverflow", {}},
            {"Phase5.App::failBounds", {}},
            {"Phase5.App::failNull", {}},
            {"Phase5.App::failNegativeLength", {}},
            {"Phase5.App::recurse", {std::int64_t{5}}},
        };
    for (const auto& [name, arguments] : calls) {
        const auto interpreted = interpreter.invoke(name, arguments);
        const auto compiled = aot.invoke(name, arguments);
        requireSameValue(
            interpreted,
            compiled,
            interpreter.heap().get(),
            aot.heap().get(),
            name);
    }

    const auto main = aot.invoke("Phase5.App::main");
    require(main.succeeded && std::get<std::int64_t>(main.value) == 60,
        "AOT fixture returned the wrong aggregate result");
    const auto longValue = aot.invoke("Phase5.App::longMath");
    require(longValue.succeeded &&
            std::get<realscript::runtime::LongValue>(longValue.value).value ==
                2147483650LL,
        "AOT long arithmetic returned the wrong result");
    const auto doubleValue = aot.invoke("Phase5.App::doubleMath");
    require(doubleValue.succeeded &&
            std::fabs(std::get<double>(doubleValue.value) - 6.0) < 1e-12,
        "AOT double arithmetic returned the wrong result");
}

void testBudgetsTracingAndGcRoots() {
    realscript::aot::Program aot(realscript_generated::Phase5FixtureProgram());
    realscript::runtime::HeapConfig config;
    config.initialCollectionThresholdBytes = 1;
    config.maximumHeapBytes = 1024 * 1024;
    aot.setHeap(std::make_shared<realscript::runtime::ManagedHeap>(config));

    std::vector<realscript::runtime::TraceEvent> trace;
    realscript::runtime::ExecutionOptions options;
    options.trace = [&](const auto& event) { trace.push_back(event); };
    options.limits.gcWorkBudget = 1;
    const auto result = aot.invoke("Phase5.App::main", {}, options);
    require(result.succeeded && result.statistics.gcWorkPerformed != 0,
        "AOT frames were not visible to incremental GC");
    require(!trace.empty() &&
            trace.front().kind == realscript::runtime::TraceEventKind::FunctionEnter,
        "AOT tracing did not report function entry");

    realscript::runtime::ExecutionOptions limited;
    limited.limits.instructionBudget = 3;
    const auto exhausted = aot.invoke("Phase5.App::main", {}, limited);
    require(!exhausted.succeeded &&
            exhausted.error.code ==
                realscript::runtime::ErrorCode::InstructionBudgetExceeded,
        "AOT execution ignored the instruction budget");

    realscript::runtime::ExecutionOptions recursive;
    recursive.limits.recursionLimit = 3;
    const auto recursion = aot.invoke(
        "Phase5.App::recurse",
        {std::int64_t{10}},
        recursive);
    require(!recursion.succeeded &&
            recursion.error.code ==
                realscript::runtime::ErrorCode::RecursionLimitExceeded,
        "AOT execution ignored the recursion budget");
}

void testModuleQueryAndMetadata() {
    RsRuntimeApiV1 api{
        sizeof(RsRuntimeApiV1),
        realscript::aot::RuntimeAbiMajor,
        realscript::aot::RuntimeAbiMinor,
        nullptr,
    };
    RsModuleExportsV1 exports{};
    exports.size = sizeof(exports);
    require(rs_module_query_v1(&api, &exports) == RS_STATUS_V1_OK,
        "AOT C ABI query failed");
    require(exports.program_descriptor != nullptr &&
            exports.function_count >= 20 && exports.content_hash != 0,
        "AOT C ABI query returned incomplete metadata");
    const auto* descriptor = static_cast<
        const realscript::aot::ProgramDescriptor*>(exports.program_descriptor);
    require(descriptor->moduleCount == 2 && descriptor->typeCount >= 3 &&
            descriptor->functionCount == exports.function_count &&
            descriptor->sourceMapCount != 0,
        "AOT descriptor lost module, type, or source-map metadata");
    const realscript::aot::TypeDescriptor* enumDescriptor = nullptr;
    for (std::uint32_t index = 0; index < descriptor->typeCount; ++index) {
        if (descriptor->types[index].kind ==
            realscript::semantic::TypeKind::Enum) {
            enumDescriptor = &descriptor->types[index];
            break;
        }
    }
    require(enumDescriptor && enumDescriptor->enumMemberCount == 2,
        "AOT descriptor lost enum metadata");

    const RsFunctionEntryV1* mainEntry = nullptr;
    for (std::uint32_t index = 0; index < exports.function_count; ++index) {
        const auto& entry = exports.functions[index];
        const auto* function = static_cast<
            const realscript::aot::FunctionDescriptor*>(entry.backend_data);
        if (function && std::string(function->name) == "Phase5.App::main") {
            mainEntry = &entry;
            break;
        }
    }
    require(mainEntry && mainEntry->entry_point &&
            mainEntry->backend_kind == RS_BACKEND_V1_NATIVE_AOT &&
            mainEntry->version == realscript::aot::GeneratedModuleVersion,
        "AOT function table lost its stable native entry point");

    realscript::aot::ExecutionContext context(
        *descriptor,
        std::make_shared<realscript::runtime::ManagedHeap>(),
        nullptr);
    realscript::runtime::Value result;
    require(mainEntry->entry_point(&context, nullptr, 0, &result) ==
            RS_STATUS_V1_OK &&
            std::get<std::int64_t>(result) == 60,
        "AOT C ABI entry point did not execute generated native code");
    require(mainEntry->entry_point(nullptr, nullptr, 0, &result) ==
            RS_STATUS_V1_INVALID_ARGUMENT,
        "AOT C ABI entry point accepted a null execution context");

    auto incompatible = api;
    incompatible.abi_major += 1;
    exports = {};
    exports.size = sizeof(exports);
    require(rs_module_query_v1(&incompatible, &exports) ==
            RS_STATUS_V1_ABI_MISMATCH,
        "AOT module query accepted an incompatible runtime ABI");
}

} // namespace

int main() {
    try {
        testDifferentialExecution();
        testBudgetsTracingAndGcRoots();
        testModuleQueryAndMetadata();
        std::cout << "Phase 5 generated AOT tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}
