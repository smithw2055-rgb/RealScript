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

std::string diagnosticsText(
    const realscript::diagnostics::DiagnosticBag& diagnostics) {
    std::string result;
    for (const auto& diagnostic : diagnostics.items()) {
        result += diagnostic.code + ": " + diagnostic.message + "\n";
    }
    return result;
}

std::string readFile(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot read fixture: " + path);
    std::ostringstream content;
    content << stream.rdbuf();
    return content.str();
}

void testGrowableCollectionsAndEnumeratorProtocol() {
    const auto path = std::string(REALSCRIPT_SOURCE_DIR) +
        "/tests/fixtures/phase21_aot.rs";
    realscript::compiler::Compilation compilation({{
        "tests/fixtures/phase21_aot.rs", readFile(path)}});
    const auto build = compilation.build();
    require(!build.diagnostics.hasErrors(),
        "Phase 21 collection compilation failed:\n" +
            diagnosticsText(build.diagnostics));

    realscript::bytecode::Lowerer lowerer;
    std::vector<realscript::bytecode::Module> modules;
    for (const auto& sourceModule : build.modules) {
        auto module = lowerer.lower(sourceModule);
        realscript::diagnostics::DiagnosticBag diagnostics;
        require(realscript::bytecode::verifyModule(module, diagnostics),
            "Phase 21 bytecode verification failed:\n" +
                diagnosticsText(diagnostics));
        modules.push_back(std::move(module));
    }
    realscript::runtime::Interpreter interpreter(std::move(modules));
    const auto result = interpreter.invoke("Phase21.Aot::main");
    require(result.succeeded,
        "Phase 21 collection execution failed: " + result.error.message);
    require(std::get<std::int64_t>(result.value) == 242,
        "growable collections or enumerator protocol returned " +
            std::to_string(std::get<std::int64_t>(result.value)) +
            " instead of 242");
}

void testCrossModuleSpecializationCache() {
    realscript::compiler::Compilation compilation({
        {"box.rs", R"(
module Phase21.Box;
class Box<T> { T value; Box(T initial) { value = initial; } T Get() { return value; } }
)"},
        {"left.rs", R"(
module Phase21.Left;
import Phase21.Box;
int Left() { Box<int> value = new Box<int>(3); return value.Get(); }
)"},
        {"right.rs", R"(
module Phase21.Right;
import Phase21.Box;
int Right() { Box<int> value = new Box<int>(4); return value.Get(); }
)"},
    });
    const auto build = compilation.build();
    require(!build.diagnostics.hasErrors(),
        "cross-module specialization failed:\n" +
            diagnosticsText(build.diagnostics));
    std::size_t boxSpecializations = 0;
    for (const auto& value : build.nativeGenericInstantiations) {
        if (value.genericName == "Box" &&
            value.generatedName == "Box__int") ++boxSpecializations;
    }
    require(boxSpecializations == 1,
        "the deterministic specialization cache emitted Box<int> more than once");
}

void testInferenceAndGenericContracts() {
    realscript::compiler::Compilation compilation({{
        "phase21-contracts.rs", R"(
module Phase21.Contracts;

interface IValue<T> { T Get(); }
delegate T Factory<T>();

class IntValue : IValue<int>
{
    int Get() { return 7; }
}

T Identity<T>(T value) { return value; }
int Make() { return 5; }

int main()
{
    IValue<int> value = new IntValue();
    Factory<int> factory = Make;
    int current = value.Get();
    return Identity(current) + factory();
}
)"}});
    const auto build = compilation.build();
    require(!build.diagnostics.hasErrors(),
        "generic inference/interface/delegate compilation failed:\n" +
            diagnosticsText(build.diagnostics));
    realscript::bytecode::Lowerer lowerer;
    std::vector<realscript::bytecode::Module> modules;
    for (const auto& module : build.modules) modules.push_back(lowerer.lower(module));
    realscript::runtime::Interpreter interpreter(std::move(modules));
    const auto result = interpreter.invoke("Phase21.Contracts::main");
    require(result.succeeded &&
            std::get<std::int64_t>(result.value) == 12,
        "generic inference/interface/delegate execution produced the wrong result");
}

void testGenericMemberMethodsAndConstraints() {
    realscript::compiler::Compilation compilation({{
        "phase21-members.rs", R"(
module Phase21.Members;
class Converter
{
    T Echo<T>(T value) where T : struct { return value; }
}
int main()
{
    Converter converter = new Converter();
    int first = converter.Echo(8);
    return first + converter.Echo<int>(4);
}
)"}});
    const auto build = compilation.build();
    require(!build.diagnostics.hasErrors(),
        "generic member specialization failed:\n" +
            diagnosticsText(build.diagnostics));
    realscript::bytecode::Lowerer lowerer;
    std::vector<realscript::bytecode::Module> modules;
    for (const auto& module : build.modules) modules.push_back(lowerer.lower(module));
    realscript::runtime::Interpreter interpreter(std::move(modules));
    const auto result = interpreter.invoke("Phase21.Members::main");
    require(result.succeeded && std::get<std::int64_t>(result.value) == 12,
        "generic member specialization produced the wrong result");

    realscript::compiler::Compilation invalid({{
        "phase21-bad-constraint.rs", R"(
module Phase21.BadConstraint;
T ReferenceOnly<T>(T value) where T : class { return value; }
int main() { return ReferenceOnly<int>(1); }
)"}});
    const auto rejected = invalid.build();
    bool foundConstraintDiagnostic = false;
    for (const auto& diagnostic : rejected.diagnostics.items()) {
        foundConstraintDiagnostic = foundConstraintDiagnostic ||
            diagnostic.code == "RS8530";
    }
    require(rejected.diagnostics.hasErrors() && foundConstraintDiagnostic,
        "a generic class constraint accepted an int type argument");
}

} // namespace

int main() {
    try {
        testGrowableCollectionsAndEnumeratorProtocol();
        testCrossModuleSpecializationCache();
        testInferenceAndGenericContracts();
        testGenericMemberMethodsAndConstraints();
        std::cout << "Phase 21 generics and collections tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}
