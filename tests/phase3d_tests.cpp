#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/diagnostics/Diagnostic.h"
#include "realscript/runtime/Runtime.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

realscript::compiler::BuildResult build(
    const std::vector<realscript::compiler::SourceFile>& files,
    const realscript::compiler::BuildSnapshot* previous = nullptr) {
    realscript::compiler::Compilation compilation;
    for (const auto& file : files) compilation.addSource(file);
    return compilation.build(previous);
}

bool hasDiagnostic(
    const realscript::compiler::BuildResult& result,
    const std::string& code) {
    for (const auto& diagnostic : result.diagnostics.items()) {
        if (diagnostic.code == code) return true;
    }
    return false;
}

std::vector<realscript::bytecode::Module> compile(
    const std::vector<realscript::compiler::SourceFile>& files) {
    auto result = build(files);
    if (result.diagnostics.hasErrors()) {
        std::string message = "source compilation failed";
        for (const auto& diagnostic : result.diagnostics.items()) {
            message += "\n" + diagnostic.code + ": " + diagnostic.message;
        }
        throw std::runtime_error(message);
    }
    realscript::bytecode::Lowerer lowerer;
    std::vector<realscript::bytecode::Module> modules;
    for (const auto& mir : result.modules) {
        auto module = lowerer.lower(mir);
        realscript::diagnostics::DiagnosticBag diagnostics;
        require(realscript::bytecode::verifyModule(module, diagnostics),
            "Phase 3D bytecode verification failed");
        modules.push_back(std::move(module));
    }
    return modules;
}

const char* objectSource = R"(
module Phase3D.Objects;

class Counter
{
    int value;

    Counter(int initial)
    {
        this.value = initial;
    }

    int Add(int amount)
    {
        value = value + amount;
        return value;
    }

    int Pick(int value) { return 1; }
    int Pick(long value) { return 2; }

    static int Twice(int value) { return value * 2; }
    static int Answer { get { return 42; } }

    int Value
    {
        get { return value; }
        set { this.value = value; }
    }

    int Auto { get; set; }
}

int main()
{
    Counter counter = new Counter(10);
    counter.Value = 20;
    counter.Auto = 2;
    return counter.Add(counter.Auto) + Counter.Twice(5) +
        Counter.Answer + counter.Pick(1) + counter.Pick(2147483648);
}
)";

void testMethodsConstructorsAndProperties() {
    auto modules = compile({{"objects.rs", objectSource}});
    require(modules.size() == 1, "expected one Phase 3D module");
    require(modules.front().version.major == 0 &&
            modules.front().version.minor == 6,
        "Phase 3D must use .rsbc 0.6");
    const auto disassembly = realscript::bytecode::disassembleModule(modules.front());
    require(disassembly.find("Counter..ctor") != std::string::npos,
        "constructor was not emitted as a stable member function");
    require(disassembly.find("Counter.Add") != std::string::npos,
        "instance method reference is missing");
    require(disassembly.find("Counter.get_Value") != std::string::npos &&
            disassembly.find("Counter.set_Value") != std::string::npos,
        "property accessors are missing");

    realscript::runtime::Interpreter interpreter(std::move(modules));
    const auto result = interpreter.invoke("Phase3D.Objects::main");
    require(result.succeeded &&
            std::get<std::int64_t>(result.value) == 77,
        "methods, constructors, static members, overloads, or properties returned the wrong result");
}

void testCrossModuleMemberBinding() {
    auto modules = compile({
        {"model.rs", R"(
module Phase3D.Model;
class Widget
{
    int value;
    Widget(int value) { this.value = value; }
    int Double() { return value * 2; }
    int Value { get { return value; } }
}
Widget create(int value) { return new Widget(value); }
)"},
        {"app.rs", R"(
module Phase3D.App;
import Phase3D.Model;
int main()
{
    Widget widget = create(21);
    return widget.Double() + widget.Value;
}
)"},
    });
    realscript::runtime::Interpreter interpreter(std::move(modules));
    const auto result = interpreter.invoke("Phase3D.App::main");
    require(result.succeeded &&
            std::get<std::int64_t>(result.value) == 63,
        "cross-module constructor, method, or property binding failed");
}

void testValueReceiverShadowsTypeName() {
    auto modules = compile({{"shadow.rs", R"(
module Phase3D.Shadowing;
class Counter
{
    int value;
    Counter(int value) { this.value = value; }
    int Read() { return value; }
    static int ReadStatic() { return 99; }
}
int main()
{
    Counter Counter = new Counter(9);
    return Counter.Read();
}
)"}});
    realscript::runtime::Interpreter interpreter(std::move(modules));
    const auto result = interpreter.invoke("Phase3D.Shadowing::main");
    require(result.succeeded &&
            std::get<std::int64_t>(result.value) == 9,
        "a local value did not shadow a type name during member binding");
}

void testPropertyAccessContextDiagnostics() {
    const auto context = build({{"context.rs", R"(
module Phase3D.InvalidContext;
class Sample
{
    int Instance { get { return 1; } }
    static int Static { get { return 2; } }
}
int main()
{
    Sample sample = new Sample();
    return Sample.Instance + sample.Static;
}
)"}});
    require(context.diagnostics.hasErrors() &&
            hasDiagnostic(context, "RS2484"),
        "static/instance property context mismatch was not diagnosed");

    const auto readOnly = build({{"readonly.rs", R"(
module Phase3D.ReadOnly;
class Sample { static int Value { get { return 1; } } }
int main() { Sample.Value = 2; return 0; }
)"}});
    require(readOnly.diagnostics.hasErrors() &&
            hasDiagnostic(readOnly, "RS2483"),
        "assignment to a read-only static property was not diagnosed");


    const auto mixedAccessors = build({{"mixed.rs", R"(
module Phase3D.MixedProperty;
class Sample
{
    int value;
    int Value { get { return value; } set; }
}
)"}});
    require(mixedAccessors.diagnostics.hasErrors() &&
            hasDiagnostic(mixedAccessors, "RS2463"),
        "mixed auto and explicit property accessors were not rejected");

    const auto memberConflict = build({{"conflict.rs", R"(
module Phase3D.MemberConflict;
class Sample
{
    int Value;
    int Value { get { return 1; } }
}
)"}});
    require(memberConflict.diagnostics.hasErrors() &&
            hasDiagnostic(memberConflict, "RS2464"),
        "conflicting field and property names were not rejected");
}

void testDispatchChangeInvalidatesDependents() {
    const std::vector<realscript::compiler::SourceFile> initialSources{
        {"model.rs", R"(
module Phase3D.DispatchModel;
class Widget { int Read() { return 1; } }
)"},
        {"app.rs", R"(
module Phase3D.DispatchApp;
import Phase3D.DispatchModel;
int main() { Widget widget = new Widget(); return widget.Read(); }
)"},
    };
    const auto initial = build(initialSources);
    require(!initial.diagnostics.hasErrors(),
        "initial dispatch fixture failed to compile");

    const auto changed = build({
        {"model.rs", R"(
module Phase3D.DispatchModel;
class Widget { static int Read() { return 1; } }
)"},
        initialSources[1],
    }, &initial.snapshot);
    bool appReused = true;
    for (const auto& info : changed.buildInfo) {
        if (info.name == "Phase3D.DispatchApp") appReused = info.reused;
    }
    require(!appReused && changed.diagnostics.hasErrors(),
        "instance-to-static dispatch change did not invalidate its dependent module");
}

void testInstanceCallsCheckNullReceiver() {
    auto modules = compile({{"null.rs", R"(
module Phase3D.Nulls;
class Probe { int Constant() { return 7; } }
int main() { Probe probe = null; return probe.Constant(); }
)"}});
    realscript::runtime::Interpreter interpreter(std::move(modules));
    const auto result = interpreter.invoke("Phase3D.Nulls::main");
    require(!result.succeeded &&
            result.error.code == realscript::runtime::ErrorCode::NullReference,
        "instance method call accepted a null receiver");
}

void testMemberCodecRoundTrip() {
    auto module = compile({{"objects.rs", objectSource}}).front();
    const auto encoded = realscript::bytecode::encodeModule(module);
    realscript::bytecode::Module decoded;
    realscript::diagnostics::DiagnosticBag diagnostics;
    require(realscript::bytecode::decodeModule(encoded, decoded, diagnostics),
        "Phase 3D bytecode failed to decode");
    require(encoded == realscript::bytecode::encodeModule(decoded),
        "Phase 3D bytecode round trip is not canonical");

    bool foundInstanceReference = false;
    for (const auto& reference : decoded.functionReferences) {
        if (reference.name.find("Counter.Add") != std::string::npos) {
            foundInstanceReference = true;
            require(reference.parameterTypes.size() == 2 &&
                    reference.parameterTypes.front() ==
                        realscript::semantic::PrimitiveType::Object &&
                    reference.parameterTypeIds.front() != 0,
                "instance method reference lost the exact implicit-this type");
        }
    }
    require(foundInstanceReference,
        "decoded module did not preserve an instance method reference");
}

} // namespace

int main() {
    try {
        testMethodsConstructorsAndProperties();
        testCrossModuleMemberBinding();
        testValueReceiverShadowsTypeName();
        testPropertyAccessContextDiagnostics();
        testDispatchChangeInvalidatesDependents();
        testInstanceCallsCheckNullReceiver();
        testMemberCodecRoundTrip();
        std::cout << "Phase 3D tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
