#include "realscript/aot_cpp/AotCpp.h"
#include "realscript/aot_cpp/AotRuntime.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/diagnostics/Diagnostic.h"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
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

realscript::compiler::BuildResult buildFixtures() {
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
    return compilation.build();
}

bool hasDiagnostic(
    const realscript::diagnostics::DiagnosticBag& diagnostics,
    const std::string& code) {
    for (const auto& item : diagnostics.items()) {
        if (item.code == code) return true;
    }
    return false;
}

std::int32_t nativeAdd(std::int32_t left, std::int32_t right) {
    return left + right;
}

std::int32_t nativeThrows(std::int32_t) {
    throw std::runtime_error("host failure");
}

constexpr realscript::semantic::SymbolId ExternalAddId = 0x9988776655443322ULL;
constexpr realscript::semantic::SymbolId ExternalCallerId = 0x1122334455667788ULL;
constexpr realscript::semantic::PrimitiveType IntParameters[] = {
    realscript::semantic::PrimitiveType::Int,
    realscript::semantic::PrimitiveType::Int,
};
constexpr realscript::semantic::SymbolId NoTypeIds[] = {0, 0};

bool externalCaller(
    realscript::aot::ExecutionContext& context,
    const realscript::runtime::Value* arguments,
    std::size_t argumentCount,
    realscript::runtime::Value& result) {
    if (!context.consume("call")) return false;
    const realscript::aot::CallSignature signature{
        ExternalAddId,
        "Host::add",
        realscript::semantic::PrimitiveType::Int,
        0,
        IntParameters,
        NoTypeIds,
        2,
    };
    return context.call(signature, arguments, argumentCount, result);
}

constexpr realscript::aot::FunctionDescriptor ExternalCallerFunction{
    ExternalCallerId,
    "Tests::externalCaller",
    realscript::semantic::PrimitiveType::Int,
    0,
    IntParameters,
    NoTypeIds,
    2,
    &externalCaller,
    nullptr,
};
constexpr const char* ExternalModules[] = {"Tests"};
constexpr realscript::aot::ProgramDescriptor ExternalProgram{
    realscript::aot::RuntimeAbiMajor,
    realscript::aot::RuntimeAbiMinor,
    "ExternalProgram",
    1,
    ExternalModules,
    1,
    nullptr,
    0,
    &ExternalCallerFunction,
    1,
    nullptr,
    0,
};

void testGeneratorDeterminismAndShape() {
    auto build = buildFixtures();
    require(!build.diagnostics.hasErrors(), "Phase 5 fixtures did not compile");

    realscript::aot::CppGenerator generator;
    realscript::diagnostics::DiagnosticBag firstDiagnostics;
    realscript::aot::GenerationOptions options;
    options.programName = "Phase5Fixture";
    const auto first = generator.generate(build.modules, firstDiagnostics, options);
    realscript::diagnostics::DiagnosticBag secondDiagnostics;
    const auto second = generator.generate(build.modules, secondDiagnostics, options);

    require(!firstDiagnostics.hasErrors() && !secondDiagnostics.hasErrors(),
        "AOT generation reported diagnostics for valid MIR");
    require(first.contentHash != 0 && first.contentHash == second.contentHash,
        "AOT content hash is not deterministic");
    require(first.header == second.header && first.source == second.source &&
            first.manifest == second.manifest,
        "AOT C++ output is not deterministic");
    require(first.source.find("switch (currentBlock)") != std::string::npos &&
            first.source.find("context.binary") != std::string::npos &&
            first.source.find("context.newArray") != std::string::npos &&
            first.source.find("context.newObject") != std::string::npos,
        "AOT output did not lower MIR control flow and runtime intrinsics");
    require(first.source.find(
                "template <bool FastAccounting, bool DeterminismAccounting>") !=
                std::string::npos &&
            first.source.find("typedRegister_") != std::string::npos &&
            first.source.find("goto typedBlock_") != std::string::npos &&
            first.source.find("_typed<true, false>") != std::string::npos &&
            first.source.find("_typed<false, true>") != std::string::npos &&
            first.source.find("rawAccounting.consume(") !=
                std::string::npos,
        "AOT output did not select typed primitive scalar lowering");
    require(first.source.find("range-proven checked arithmetic values:") !=
                std::string::npos &&
            first.source.find(
                "checked arithmetic proven safe by integer range analysis") !=
                std::string::npos,
        "AOT output did not audit range-proven checked arithmetic lowering");
    require(first.source.find("#line ") != std::string::npos &&
            first.manifest.find("sourceMapCount") != std::string::npos,
        "AOT source mapping metadata is missing");
    require(first.source.find("Interpreter") == std::string::npos &&
            first.source.find("bytecode::Opcode") == std::string::npos,
        "AOT output incorrectly embeds the bytecode interpreter");
    require(first.manifest.find("Phase5.App::main") != std::string::npos &&
            first.manifest.find("Phase5.Model::Counter.Add") != std::string::npos,
        "AOT manifest lost stable function exports");

    auto withoutLines = options;
    withoutLines.emitLineDirectives = false;
    realscript::diagnostics::DiagnosticBag noLineDiagnostics;
    const auto noLines = generator.generate(
        build.modules, noLineDiagnostics, withoutLines);
    require(!noLineDiagnostics.hasErrors() &&
            noLines.source.find("#line ") == std::string::npos &&
            noLines.manifest.find("\"sourceMapCount\": 0") ==
                std::string::npos,
        "AOT no-line-directives mode lost independent source-map metadata");
}

void testGeneratorRejectsInvalidInputs() {
    realscript::aot::CppGenerator generator;
    realscript::diagnostics::DiagnosticBag emptyDiagnostics;
    const auto empty = generator.generate({}, emptyDiagnostics);
    require(empty.source.empty() && hasDiagnostic(emptyDiagnostics, "RS7000"),
        "AOT generator accepted an empty program");

    auto build = buildFixtures();
    require(!build.diagnostics.hasErrors(), "Phase 5 fixtures did not compile");
    auto duplicate = build.modules;
    duplicate.push_back(build.modules.front());
    realscript::diagnostics::DiagnosticBag duplicateDiagnostics;
    (void)generator.generate(duplicate, duplicateDiagnostics);
    require(hasDiagnostic(duplicateDiagnostics, "RS7003") ||
            hasDiagnostic(duplicateDiagnostics, "RS7004"),
        "AOT generator accepted duplicate stable identities");

    realscript::diagnostics::DiagnosticBag noFunctionDiagnostics;
    realscript::mir::Module noFunctions;
    noFunctions.name = "Types.Only";
    (void)generator.generate({noFunctions}, noFunctionDiagnostics);
    require(hasDiagnostic(noFunctionDiagnostics, "RS7006"),
        "AOT generator accepted a program without functions");

    realscript::diagnostics::DiagnosticBag symbolDiagnostics;
    realscript::aot::GenerationOptions invalid;
    invalid.querySymbol = "bad-symbol";
    (void)generator.generate(build.modules, symbolDiagnostics, invalid);
    require(hasDiagnostic(symbolDiagnostics, "RS7001"),
        "AOT generator accepted an invalid C query symbol");
}

void testTypedNativeThunksAndExternalCalls() {
    auto bindings = std::make_shared<realscript::runtime::BindingRegistry>();
    require(bindings->bind(
            ExternalAddId,
            realscript::aot::makeNativeThunk(&nativeAdd)),
        "failed to register a typed native thunk");

    realscript::aot::Program program(ExternalProgram);
    program.setBindings(bindings);
    const auto result = program.invoke(
        ExternalCallerId,
        {std::int64_t{20}, std::int64_t{22}});
    require(result.succeeded && std::get<std::int64_t>(result.value) == 42,
        "AOT external call did not use the typed native thunk");
    require(result.statistics.externalCalls == 1 &&
            result.statistics.functionCalls == 1,
        "AOT external-call statistics are incorrect");

    realscript::runtime::RuntimeError error;
    const realscript::bytecode::FunctionReference reference{
        ExternalAddId,
        "Host::add",
        realscript::semantic::PrimitiveType::Int,
        0,
        {realscript::semantic::PrimitiveType::Int,
         realscript::semantic::PrimitiveType::Int},
        {0, 0},
    };
    const auto wrong = bindings->invoke(
        reference,
        {true, std::int64_t{2}},
        error);
    require(!wrong && error.code == realscript::runtime::ErrorCode::TypeMismatch,
        "typed native thunk accepted an invalid argument type");

    auto throwing = realscript::aot::makeNativeThunk(&nativeThrows);
    realscript::runtime::RuntimeError throwError;
    const auto thrown = throwing(reference, {std::int64_t{1}}, throwError);
    require(!thrown &&
            throwError.code == realscript::runtime::ErrorCode::ExternalFunctionUnresolved,
        "typed native thunk allowed a C++ exception to escape");
}

bool descriptorRejected(const realscript::aot::ProgramDescriptor& descriptor) {
    try {
        realscript::aot::Program program(descriptor);
        (void)program;
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

void testProgramDescriptorValidation() {
    auto invalid = ExternalProgram;
    invalid.abiMajor = realscript::aot::RuntimeAbiMajor + 1;
    require(descriptorRejected(invalid),
        "AOT Program accepted an incompatible runtime ABI");

    invalid = ExternalProgram;
    invalid.moduleNames = nullptr;
    require(descriptorRejected(invalid),
        "AOT Program accepted a missing module table");

    invalid = ExternalProgram;
    invalid.functions = nullptr;
    require(descriptorRejected(invalid),
        "AOT Program accepted a missing function table");

    auto duplicateFunction = ExternalCallerFunction;
    constexpr realscript::aot::FunctionDescriptor duplicateFunctions[] = {
        ExternalCallerFunction,
        ExternalCallerFunction,
    };
    (void)duplicateFunction;
    invalid = ExternalProgram;
    invalid.functions = duplicateFunctions;
    invalid.functionCount = 2;
    require(descriptorRejected(invalid),
        "AOT Program accepted duplicate function identities");

    auto malformedFunction = ExternalCallerFunction;
    malformedFunction.parameterTypeIds = nullptr;
    invalid = ExternalProgram;
    invalid.functions = &malformedFunction;
    require(descriptorRejected(invalid),
        "AOT Program accepted incomplete exact signature metadata");
}

} // namespace

int main() {
    try {
        testGeneratorDeterminismAndShape();
        testGeneratorRejectsInvalidInputs();
        testTypedNativeThunksAndExternalCalls();
        testProgramDescriptorValidation();
        std::cout << "Phase 5 generator/runtime tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}
