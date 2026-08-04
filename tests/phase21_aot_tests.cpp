#include "realscript_aot_generated.h"

#include "realscript/aot_cpp/AotRuntime.h"
#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
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
        "/tests/fixtures/phase21_aot.rs";
    realscript::compiler::Compilation compilation({{
        "tests/fixtures/phase21_aot.rs", readFile(path)}});
    const auto build = compilation.build();
    require(!build.diagnostics.hasErrors(),
        "Phase 21 AOT fixture compilation failed");
    realscript::bytecode::Lowerer lowerer;
    std::vector<realscript::bytecode::Module> modules;
    for (const auto& module : build.modules) modules.push_back(lowerer.lower(module));
    return modules;
}

void testAotCollectionParity() {
    auto modules = compileFixture();
    realscript::runtime::Interpreter interpreter(modules);
    realscript::aot::Program aot(realscript_generated::Phase21AotProgram());
    const auto interpreted = interpreter.invoke("Phase21.Aot::main");
    const auto compiled = aot.invoke("Phase21.Aot::main");
    require(interpreted.succeeded && compiled.succeeded,
        "Phase 21 interpreter or AOT collection execution failed");
    require(std::get<std::int64_t>(interpreted.value) == 242 &&
            std::get<std::int64_t>(compiled.value) == 242,
        "Phase 21 generated AOT result diverged from the interpreter");
    require(interpreted.instructionsExecuted == compiled.instructionsExecuted,
        "Phase 21 AOT instruction accounting diverged");
}

} // namespace

int main() {
    try {
        testAotCollectionParity();
        std::cout << "Phase 21 compiled AOT tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}
