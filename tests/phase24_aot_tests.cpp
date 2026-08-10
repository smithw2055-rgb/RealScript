#include "realscript_aot_generated.h"

#include "realscript/aot_cpp/AotRuntime.h"
#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/optimization/Optimizer.h"
#include "realscript/runtime/Runtime.h"

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
    const auto path = std::string(REALSCRIPT_SOURCE_DIR) +
        "/tests/fixtures/phase24_aot.rs";
    realscript::compiler::Compilation compilation({{
        "tests/fixtures/phase24_aot.rs", readFile(path)}});
    auto build = compilation.build();
    require(!build.diagnostics.hasErrors(),
        "Phase 24 AOT fixture compilation failed");
    realscript::optimization::Options options;
    options.level = realscript::optimization::Level::Aggressive;
    auto optimized = realscript::optimization::Optimizer{}.optimize(
        build.modules, build.diagnostics, options);
    require(!build.diagnostics.hasErrors(),
        "Phase 24 interpreter fixture optimization failed");
    realscript::bytecode::Lowerer lowerer;
    std::vector<realscript::bytecode::Module> modules;
    for (const auto& module : optimized.modules) {
        modules.push_back(lowerer.lower(module));
    }
    return modules;
}

void requireParity(
    realscript::runtime::Interpreter& interpreter,
    realscript::aot::Program& aot,
    const std::string& name,
    std::vector<realscript::runtime::Value> arguments,
    std::int64_t expected) {
    const auto interpreted = interpreter.invoke(name, arguments);
    const auto compiled = aot.invoke(name, std::move(arguments));
    require(interpreted.succeeded && compiled.succeeded,
        name + " failed in interpreter or generated AOT");
    require(std::get<std::int64_t>(interpreted.value) == expected &&
            std::get<std::int64_t>(compiled.value) == expected,
        name + " diverged from its expected value");
    require(interpreted.instructionsExecuted == compiled.instructionsExecuted,
        name + " instruction accounting diverged between backends");
}

void testExpressionParity() {
    auto modules = compileFixture();
    realscript::runtime::Interpreter interpreter(std::move(modules));
    realscript::aot::Program aot(realscript_generated::Phase24AotProgram());
    requireParity(interpreter, aot, "Phase24.Aot::Infer",
        {std::int64_t{5}}, 8);
    requireParity(interpreter, aot, "Phase24.Aot::Conditional",
        {true, std::int64_t{0}}, 7);
    requireParity(interpreter, aot, "Phase24.Aot::Conditional",
        {false, std::int64_t{2}}, 5);
    requireParity(interpreter, aot, "Phase24.Aot::Coalesce",
        {true, std::int64_t{0}}, 11);
    requireParity(interpreter, aot, "Phase24.Aot::Coalesce",
        {false, std::int64_t{2}}, 5);
    requireParity(interpreter, aot, "Phase24.Aot::NullConditional",
        {true}, 11);
    requireParity(interpreter, aot, "Phase24.Aot::NullConditional",
        {false}, 9);
    requireParity(interpreter, aot, "Phase24.Aot::NullConditionalValue",
        {true}, 11);
    requireParity(interpreter, aot, "Phase24.Aot::NullConditionalValue",
        {false}, 13);
    const auto nullablePresent = interpreter.invoke(
        "Phase24.Aot::LiftedNullableMembers", {true});
    const auto nullablePresentAot = aot.invoke(
        "Phase24.Aot::LiftedNullableMembers", {true});
    const auto nullableMissing = interpreter.invoke(
        "Phase24.Aot::LiftedNullableMembers", {false});
    const auto nullableMissingAot = aot.invoke(
        "Phase24.Aot::LiftedNullableMembers", {false});
    require(nullablePresent.succeeded && nullablePresentAot.succeeded &&
            nullableMissing.succeeded && nullableMissingAot.succeeded &&
            std::get<bool>(nullablePresent.value) &&
            std::get<bool>(nullablePresentAot.value) &&
            std::get<bool>(nullableMissing.value) &&
            std::get<bool>(nullableMissingAot.value),
        "lifted nullable member behavior diverged between backends");
    requireParity(interpreter, aot, "Phase24.Aot::Initializers", {}, 34);
    requireParity(interpreter, aot, "Phase24.Aot::Arguments", {}, 43);
    requireParity(interpreter, aot, "Phase24.Aot::PatternSwitch", {true}, 41);
    requireParity(interpreter, aot, "Phase24.Aot::PatternSwitch", {false}, 2);
    requireParity(interpreter, aot, "Phase24.Aot::SwitchExpressions",
        {std::int64_t{1}, true}, 51);
    requireParity(interpreter, aot, "Phase24.Aot::SwitchExpressions",
        {std::int64_t{2}, false}, 22);
    requireParity(interpreter, aot, "Phase24.Aot::CatchAndFinally",
        {true}, 117);
    requireParity(interpreter, aot, "Phase24.Aot::CatchAndFinally",
        {false}, 103);
    requireParity(interpreter, aot, "Phase24.Aot::RethrowAndFinally",
        {}, 155);
    requireParity(interpreter, aot, "Phase24.Aot::ReturnRunsFinally",
        {}, 56);
    requireParity(interpreter, aot, "Phase24.Aot::LoopCleanup",
        {}, 16);
    const auto interpretedIs = interpreter.invoke(
        "Phase24.Aot::RuntimeIs", {true});
    const auto compiledIs = aot.invoke(
        "Phase24.Aot::RuntimeIs", {true});
    const auto interpretedIsFalse = interpreter.invoke(
        "Phase24.Aot::RuntimeIs", {false});
    const auto compiledIsFalse = aot.invoke(
        "Phase24.Aot::RuntimeIs", {false});
    const auto interpretedAs = interpreter.invoke(
        "Phase24.Aot::RuntimeAs", {true});
    const auto compiledAs = aot.invoke(
        "Phase24.Aot::RuntimeAs", {true});
    const auto interpretedAsFalse = interpreter.invoke(
        "Phase24.Aot::RuntimeAs", {false});
    const auto compiledAsFalse = aot.invoke(
        "Phase24.Aot::RuntimeAs", {false});
    const auto interpretedTokens = interpreter.invoke(
        "Phase24.Aot::TypeTokens");
    const auto compiledTokens = aot.invoke(
        "Phase24.Aot::TypeTokens");
    require(interpretedIs.succeeded && compiledIs.succeeded &&
            interpretedIsFalse.succeeded && compiledIsFalse.succeeded &&
            interpretedAs.succeeded && compiledAs.succeeded &&
            interpretedAsFalse.succeeded && compiledAsFalse.succeeded &&
            interpretedTokens.succeeded && compiledTokens.succeeded &&
            std::get<bool>(interpretedIs.value) &&
            std::get<bool>(compiledIs.value) &&
            !std::get<bool>(interpretedIsFalse.value) &&
            !std::get<bool>(compiledIsFalse.value) &&
            std::get<std::int64_t>(interpretedAs.value) == 41 &&
            std::get<std::int64_t>(compiledAs.value) == 41 &&
            std::get<std::int64_t>(interpretedAsFalse.value) == -1 &&
            std::get<std::int64_t>(compiledAsFalse.value) == -1 &&
            std::get<bool>(interpretedTokens.value) &&
            std::get<bool>(compiledTokens.value),
        "Phase 24 runtime type operations diverged between backends");
    const auto interpretedUncaught = interpreter.invoke(
        "Phase24.Aot::Uncaught");
    const auto compiledUncaught = aot.invoke(
        "Phase24.Aot::Uncaught");
    require(!interpretedUncaught.succeeded && !compiledUncaught.succeeded &&
            interpretedUncaught.error.code ==
                realscript::runtime::ErrorCode::ScriptException &&
            compiledUncaught.error.code ==
                realscript::runtime::ErrorCode::ScriptException,
        "uncaught script exception diverged between interpreter and AOT");
}

} // namespace

int main() {
    try {
        testExpressionParity();
        std::cout << "Phase 24 generated AOT tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}
