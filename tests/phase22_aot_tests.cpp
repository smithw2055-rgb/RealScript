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
        "/tests/fixtures/phase22_aot.rs";
    realscript::compiler::Compilation compilation({{
        "tests/fixtures/phase22_aot.rs", readFile(path)}});
    auto build = compilation.build();
    require(!build.diagnostics.hasErrors(),
        "Phase 22 AOT fixture compilation failed");
    realscript::optimization::Options options;
    options.level = realscript::optimization::Level::Aggressive;
    auto optimized = realscript::optimization::Optimizer{}.optimize(
        build.modules, build.diagnostics, options);
    require(!build.diagnostics.hasErrors(),
        "Phase 22 interpreter fixture optimization failed");
    realscript::bytecode::Lowerer lowerer;
    std::vector<realscript::bytecode::Module> modules;
    for (const auto& module : optimized.modules) {
        modules.push_back(lowerer.lower(module));
    }
    return modules;
}

realscript::runtime::Value runInterpreter(
    realscript::runtime::Interpreter& interpreter) {
    const auto created = interpreter.invoke("Phase22.Aot::Create");
    require(created.succeeded &&
            std::holds_alternative<realscript::runtime::ObjectRef>(
                created.value),
        "Phase 22 interpreter AOT fixture allocation failed");
    const auto object =
        std::get<realscript::runtime::ObjectRef>(created.value);
    auto root = interpreter.heap()->retain(object);
    require(interpreter.invoke(
        "Phase22.Aot::Behavior.Run",
        {object, realscript::runtime::LongValue{41}, std::int64_t{5}})
        .succeeded, "Phase 22 interpreter sequence entry failed");
    for (int index = 0; index < 3; ++index) {
        require(interpreter.invoke(
            "Phase22.Aot::Behavior.$sequence_Run_1", {object}).succeeded,
            "Phase 22 interpreter continuation failed");
    }
    const auto result = interpreter.invoke(
        "Phase22.Aot::Behavior.Read", {object});
    require(result.succeeded, "Phase 22 interpreter result read failed");
    return result.value;
}

realscript::runtime::Value runAot(realscript::aot::Program& program) {
    const auto created = program.invoke("Phase22.Aot::Create");
    require(created.succeeded &&
            std::holds_alternative<realscript::runtime::ObjectRef>(
                created.value),
        "Phase 22 generated AOT fixture allocation failed");
    const auto object =
        std::get<realscript::runtime::ObjectRef>(created.value);
    auto root = program.heap()->retain(object);
    require(program.invoke(
        "Phase22.Aot::Behavior.Run",
        {object, realscript::runtime::LongValue{41}, std::int64_t{5}})
        .succeeded, "Phase 22 generated AOT sequence entry failed");
    for (int index = 0; index < 3; ++index) {
        require(program.invoke(
            "Phase22.Aot::Behavior.$sequence_Run_1", {object}).succeeded,
            "Phase 22 generated AOT continuation failed");
    }
    const auto result = program.invoke(
        "Phase22.Aot::Behavior.Read", {object});
    require(result.succeeded, "Phase 22 generated AOT result read failed");
    return result.value;
}

void testCompiledCoroutineParity() {
    realscript::runtime::Interpreter interpreter(compileFixture());
    realscript::aot::Program aot(
        realscript_generated::Phase22AotProgram());
    const auto interpreted = runInterpreter(interpreter);
    const auto compiled = runAot(aot);
    require(std::holds_alternative<std::int64_t>(interpreted) &&
            std::holds_alternative<std::int64_t>(compiled) &&
            std::get<std::int64_t>(interpreted) == 17 &&
            std::get<std::int64_t>(compiled) == 17,
        "Phase 22 generated AOT coroutine result diverged");
}

} // namespace

int main() {
    try {
        testCompiledCoroutineParity();
        std::cout << "Phase 22 compiled AOT tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}
