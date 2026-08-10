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
        "/tests/fixtures/phase23_aot.rs";
    realscript::compiler::Compilation compilation({{
        "tests/fixtures/phase23_aot.rs", readFile(path)}});
    auto build = compilation.build();
    require(!build.diagnostics.hasErrors(),
        "Phase 23 AOT fixture compilation failed");
    realscript::optimization::Options options;
    options.level = realscript::optimization::Level::Aggressive;
    auto optimized = realscript::optimization::Optimizer{}.optimize(
        build.modules, build.diagnostics, options);
    require(!build.diagnostics.hasErrors(),
        "Phase 23 interpreter fixture optimization failed");
    realscript::bytecode::Lowerer lowerer;
    std::vector<realscript::bytecode::Module> modules;
    for (const auto& module : optimized.modules) modules.push_back(lowerer.lower(module));
    return modules;
}

void requireParity(
    realscript::runtime::Interpreter& interpreter,
    realscript::aot::Program& aot,
    const std::string& name,
    realscript::runtime::Value argument,
    realscript::runtime::Value expected) {
    const auto interpreted = interpreter.invoke(name, {argument});
    const auto compiled = aot.invoke(name, {std::move(argument)});
    require(interpreted.succeeded && compiled.succeeded,
        name + " failed in interpreter or generated AOT");
    require(interpreted.value == expected && compiled.value == expected,
        name + " diverged from its exact expected value");
    require(interpreted.instructionsExecuted == compiled.instructionsExecuted,
        name + " instruction accounting diverged between backends");
}

void requireParity2(
    realscript::runtime::Interpreter& interpreter,
    realscript::aot::Program& aot,
    const std::string& name,
    realscript::runtime::Value left,
    realscript::runtime::Value right,
    const realscript::runtime::Value& expected) {
    const auto interpreted = interpreter.invoke(name, {left, right});
    const auto compiled = aot.invoke(name, {std::move(left), std::move(right)});
    require(interpreted.succeeded && compiled.succeeded,
        name + " failed in interpreter or generated AOT");
    require(interpreted.value == expected && compiled.value == expected,
        name + " diverged from its exact expected value");
    require(interpreted.instructionsExecuted == compiled.instructionsExecuted,
        name + " instruction accounting diverged between backends");
}

void testAotExactNumericParity() {
    auto modules = compileFixture();
    realscript::runtime::Interpreter interpreter(modules);
    realscript::aot::Program aot(realscript_generated::Phase23AotProgram());
    requireParity(interpreter, aot, "Phase23.Aot::EchoByte",
        realscript::runtime::ByteValue{200},
        realscript::runtime::ByteValue{200});
    requireParity(interpreter, aot, "Phase23.Aot::EchoULong",
        realscript::runtime::ULongValue{0xf000000000000001ULL},
        realscript::runtime::ULongValue{0xf000000000000001ULL});
    requireParity(interpreter, aot, "Phase23.Aot::EchoFloat",
        realscript::runtime::FloatValue{2.5f},
        realscript::runtime::FloatValue{2.5f});
    requireParity(interpreter, aot, "Phase23.Aot::EchoChar",
        realscript::runtime::CharValue{u'Q'},
        realscript::runtime::CharValue{u'Q'});
    requireParity(interpreter, aot, "Phase23.Aot::WidenUInt",
        realscript::runtime::UIntValue{4000000000U},
        realscript::runtime::ULongValue{4000000000ULL});
    requireParity(interpreter, aot, "Phase23.Aot::WidenFloat",
        realscript::runtime::FloatValue{6.25f}, 6.25);
    requireParity2(interpreter, aot, "Phase23.Aot::AddUInt",
        realscript::runtime::UIntValue{3000000000U},
        realscript::runtime::UIntValue{1000000000U},
        realscript::runtime::UIntValue{4000000000U});
    requireParity2(interpreter, aot, "Phase23.Aot::AddUIntUnchecked",
        realscript::runtime::UIntValue{4000000000U},
        realscript::runtime::UIntValue{4000000000U},
        realscript::runtime::UIntValue{3705032704U});
    requireParity2(interpreter, aot, "Phase23.Aot::SubtractULong",
        realscript::runtime::ULongValue{0xf000000000000010ULL},
        realscript::runtime::ULongValue{0xf000000000000000ULL},
        realscript::runtime::ULongValue{16});
    requireParity2(interpreter, aot, "Phase23.Aot::MultiplyFloat",
        realscript::runtime::FloatValue{1.5f},
        realscript::runtime::FloatValue{2.0f},
        realscript::runtime::FloatValue{3.0f});
    requireParity(interpreter, aot, "Phase23.Aot::CastByteUnchecked",
        std::int64_t{300}, realscript::runtime::ByteValue{44});
    requireParity(interpreter, aot, "Phase23.Aot::MutateStruct",
        std::int64_t{5}, std::int64_t{12});
    requireParity(interpreter, aot, "Phase23.Aot::MutateStructField",
        std::int64_t{5}, std::int64_t{12});
    requireParity(interpreter, aot, "Phase23.Aot::MutateStructIndexer",
        std::int64_t{5}, std::int64_t{13});
    requireParity(interpreter, aot, "Phase23.Aot::RefFieldWriteback",
        std::int64_t{5}, std::int64_t{6});
    requireParity(interpreter, aot, "Phase23.Aot::RefIndexerWriteback",
        std::int64_t{5}, std::int64_t{6});
    requireParity(interpreter, aot, "Phase23.Aot::RefLocalWriteback",
        std::int64_t{5}, std::int64_t{7});
    requireParity(interpreter, aot, "Phase23.Aot::NullableRoundTrip",
        true, std::int64_t{7});
    requireParity(interpreter, aot, "Phase23.Aot::NullableRoundTrip",
        false, std::int64_t{-1});
    requireParity(interpreter, aot, "Phase23.Aot::BoxRoundTrip",
        std::int64_t{23}, std::int64_t{23});
    const auto interpretedDefensiveCopy = interpreter.invoke(
        "Phase23.Aot::InDefensiveCopy");
    const auto compiledDefensiveCopy = aot.invoke(
        "Phase23.Aot::InDefensiveCopy");
    require(interpretedDefensiveCopy.succeeded &&
            compiledDefensiveCopy.succeeded &&
            interpretedDefensiveCopy.value ==
                realscript::runtime::Value{std::int64_t{22}} &&
            compiledDefensiveCopy.value ==
                realscript::runtime::Value{std::int64_t{22}},
        "in defensive-copy semantics diverged between interpreter and AOT");
    requireParity(interpreter, aot, "Phase23.Aot::RefReturnWriteback",
        std::int64_t{5}, std::int64_t{8});
    const auto interpretedOverflow = interpreter.invoke(
        "Phase23.Aot::CastByteChecked", {std::int64_t{300}});
    const auto compiledOverflow = aot.invoke(
        "Phase23.Aot::CastByteChecked", {std::int64_t{300}});
    require(!interpretedOverflow.succeeded && !compiledOverflow.succeeded &&
            interpretedOverflow.error.code ==
                realscript::runtime::ErrorCode::IntegerOverflow &&
            compiledOverflow.error.code ==
                realscript::runtime::ErrorCode::IntegerOverflow,
        "checked narrowing overflow diverged between interpreter and AOT");
}

} // namespace

int main() {
    try {
        testAotExactNumericParity();
        std::cout << "Phase 23 compiled AOT tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}
