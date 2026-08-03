#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
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

std::string diagnosticsText(
    const realscript::diagnostics::DiagnosticBag& diagnostics) {
    std::string result;
    for (const auto& diagnostic : diagnostics.items()) {
        if (!result.empty()) result.push_back('\n');
        result += diagnostic.code + ": " + diagnostic.message;
    }
    return result;
}

realscript::runtime::ExecutionResult execute(const char* source) {
    realscript::compiler::Compilation compilation({{"phase18.rs", source}});
    const auto build = compilation.build();
    require(
        !build.diagnostics.hasErrors(),
        "native source failed to compile:\n" +
            diagnosticsText(build.diagnostics));
    realscript::bytecode::Lowerer lowerer;
    std::vector<realscript::bytecode::Module> modules;
    for (const auto& sourceModule : build.modules) {
        auto module = lowerer.lower(sourceModule);
        realscript::diagnostics::DiagnosticBag diagnostics;
        const auto verified =
            realscript::bytecode::verifyModule(module, diagnostics);
        require(
            verified,
            "native bytecode verification failed:\n" +
                diagnosticsText(diagnostics));
        modules.push_back(std::move(module));
    }
    realscript::runtime::Interpreter interpreter(std::move(modules));
    return interpreter.invoke("Phase18::main");
}

void testNativeControlFlowExecution() {
    const char* source = R"(
module Phase18;
int main()
{
    int total = 0;
    for (int i = 0; i < 5; i = i + 1)
    {
        if (i == 1) continue;
        switch (i)
        {
            case 3:
                break;
            default:
                total = total + i;
                break;
        }
        if (i == 4) break;
    }

    int j = 0;
    do
    {
        j = j + 1;
        if (j < 2) continue;
        total = total + 10;
    }
    while (j < 3);

    int[] values = new int[3];
    values[0] = 1;
    values[1] = 2;
    values[2] = 3;
    foreach (int value in values)
    {
        total = total + value;
    }
    return total;
}
)";
    const auto result = execute(source);
    require(
        result.succeeded,
        "native control-flow execution failed: " + result.error.message);
    require(
        std::get<std::int64_t>(result.value) == 32,
        "native control-flow result was incorrect");
}

void testNativeInfiniteLoopBreak() {
    const auto result = execute(R"(
module Phase18;
int main()
{
    while (true)
    {
        break;
    }
    return 7;
}
)");
    require(
        result.succeeded &&
            std::get<std::int64_t>(result.value) == 7,
        "break from an infinite loop did not reach the exit block");
}

void testNativeDiagnostics() {
    realscript::compiler::Compilation compilation({{"invalid.rs", R"(
module Invalid;
int main()
{
    break;
    continue;
    return 0;
}
)"}});
    const auto build = compilation.build();
    require(
        build.diagnostics.hasErrors(),
        "invalid native loop control was accepted");
    bool breakFound = false;
    bool continueFound = false;
    for (const auto& diagnostic : build.diagnostics.items()) {
        breakFound = breakFound || diagnostic.code == "RS2212";
        continueFound = continueFound || diagnostic.code == "RS2213";
    }
    require(
        breakFound && continueFound,
        "native loop-control diagnostics were not preserved");
}

void testNoStructuredSourceRewrite() {
    const auto expansion = realscript::compiler::expandLanguageSource(
        "native.rs",
        "module Native; int main(){for(int i=0;i<1;i=i+1){}return 1;}");
    require(
        !expansion.changed,
        "native for statement still used source expansion");
}

void testNativeInterfaceContracts() {
    const char* contracts = R"(
module Phase18.Contracts;
interface IReader
{
    int Read(int value);
}
)";
    const char* app = R"(
module Phase18.App;
import Phase18.Contracts;
class Reader : IReader
{
    int Read(int value)
    {
        return value + 1;
    }
}
int main()
{
    Reader reader = new Reader();
    return reader.Read(41);
}
)";
    realscript::compiler::Compilation compilation({
        {"contracts.rs", contracts},
        {"app.rs", app},
    });
    const auto build = compilation.build();
    require(
        !build.diagnostics.hasErrors(),
        "native interface contract failed:\n" +
            diagnosticsText(build.diagnostics));
    require(
        build.nativeInterfaces.size() == 1,
        "native interface implementation metadata was not retained");
    require(
        build.nativeInterfaces.front().typeName ==
                "Phase18.App::Reader" &&
            build.nativeInterfaces.front().interfaces.size() == 1 &&
            build.nativeInterfaces.front().interfaces.front() ==
                "Phase18.Contracts::IReader",
        "native interface metadata identity was incorrect");

    realscript::bytecode::Lowerer lowerer;
    std::vector<realscript::bytecode::Module> modules;
    for (const auto& sourceModule : build.modules) {
        auto module = lowerer.lower(sourceModule);
        realscript::diagnostics::DiagnosticBag diagnostics;
        require(
            realscript::bytecode::verifyModule(module, diagnostics),
            "native interface bytecode verification failed");
        modules.push_back(std::move(module));
    }
    realscript::runtime::Interpreter interpreter(std::move(modules));
    const auto result = interpreter.invoke("Phase18.App::main");
    require(
        result.succeeded &&
            std::get<std::int64_t>(result.value) == 42,
        "native interface implementation did not execute");
}

void testNativeInterfaceDiagnostics() {
    realscript::compiler::Compilation compilation({{"bad-interface.rs", R"(
module Phase18.Bad;
interface IReader
{
    int Read(int value);
}
class Reader : IReader
{
    long Read(int value)
    {
        return value;
    }
}
)"}});
    const auto build = compilation.build();
    require(
        build.diagnostics.hasErrors(),
        "invalid native interface implementation was accepted");
    bool found = false;
    for (const auto& diagnostic : build.diagnostics.items()) {
        found = found || diagnostic.code == "RS2475";
    }
    require(
        found,
        "native interface signature mismatch did not produce RS2475");
}

void testInterfaceBypassesExpansion() {
    const auto expansion = realscript::compiler::expandLanguageSource(
        "interface.rs",
        "module Native; interface IRun { int Run(); } "
        "class Runner : IRun { int Run(){return 1;} }");
    require(
        !expansion.changed,
        "native interfaces still used source expansion");
}

void testNativeAttributes() {
    realscript::compiler::Compilation compilation({{"attributes.rs", R"(
module Phase18.Attributes;
[Serializable(version = 2), Editor(category = "combat")]
class Unit
{
    [Replicated(channel = "state")]
    int health;

    [Command]
    int Damage(int amount)
    {
        health = health - amount;
        return health;
    }
}
)"}});
    const auto build = compilation.build();
    require(
        !build.diagnostics.hasErrors(),
        "native attributes failed to compile:\n" +
            diagnosticsText(build.diagnostics));
    require(
        build.nativeAttributes.size() == 4,
        "native declaration attributes were not retained");
    require(
        build.nativeAttributes.front().target ==
            "Phase18.Attributes::Unit",
        "native type attribute target was not canonical");
    bool replicated = false;
    bool command = false;
    for (const auto& attribute : build.nativeAttributes) {
        replicated = replicated ||
            (attribute.name == "Replicated" &&
             attribute.target.find("field:health") !=
                 std::string::npos);
        command = command ||
            (attribute.name == "Command" &&
             attribute.target.find("method:Damage#1") !=
                 std::string::npos);
    }
    require(
        replicated && command,
        "native member attribute targets were not retained");
}

void testAttributesBypassExpansion() {
    const auto expansion = realscript::compiler::expandLanguageSource(
        "attributes.rs",
        "module Native; [Serializable] class Unit { int health; }");
    require(
        !expansion.changed,
        "native attributes still used source expansion");
}

void testNativeReferenceParameters() {
    const auto result = execute(R"(
module Phase18;
void Bump(ref int value, out int doubled, in int amount)
{
    value = value + amount;
    doubled = value + value;
}
int main()
{
    int value = 1;
    int doubled;
    Bump(ref value, out doubled, in 2);
    return value + doubled;
}
)");
    require(
        result.succeeded &&
            std::get<std::int64_t>(result.value) == 9,
        "native ref/out/in execution failed");
}

void testNativeReferenceDiagnostics() {
    realscript::compiler::Compilation compilation({{
        "bad-reference.rs",
        R"(
module Phase18.ReferenceBad;
void Mutate(in int value)
{
    value = 4;
}
int main()
{
    int value = 1;
    Mutate(in value);
    return value;
}
)"}});
    const auto build = compilation.build();
    require(build.diagnostics.hasErrors(),
        "assignment to in parameter was accepted");
    bool found = false;
    for (const auto& diagnostic : build.diagnostics.items()) {
        found = found || diagnostic.code == "RS8702";
    }
    require(found,
        "assignment to in parameter did not produce RS8702");
}

void testReferencesBypassExpansion() {
    const auto expansion =
        realscript::compiler::expandLanguageSource(
            "references.rs",
            "module Native; void Bump(ref int value){"
            "value=value+1;} int main(){int value=1;"
            "Bump(ref value);return value;}");
    require(!expansion.changed,
        "native reference parameters still used source expansion");
}

void testNativeValueAliases() {
    const auto result = execute(R"(
module Phase18;
double main()
{
    byte a = 1;
    sbyte b = 2;
    short c = 3;
    ushort d = 4;
    char e = 5;
    uint f = 6;
    ulong g = 7;
    float h = 8.5;
    return a + b + c + d + e + f + g + h;
}
)");
    require(
        result.succeeded &&
            std::get<double>(result.value) == 36.5,
        "native value aliases produced the wrong result");
}

void testAliasesBypassExpansion() {
    const auto expansion =
        realscript::compiler::expandLanguageSource(
            "aliases.rs",
            "module Native; int main(){byte a=1;uint b=2;"
            "float c=3.5;char d=4;return a+b+c+d;}");
    require(!expansion.changed,
        "native value aliases still used source expansion");
}

void testNativeSequenceDiagnostics() {
    realscript::compiler::Compilation compilation({{"bad-sequence.rs", R"(
module Phase18.SequenceBad;
class Behavior
{
    sequence Attack(int target)
    {
        yield wait_ticks(1);
    }
}
)"}});
    const auto build = compilation.build();
    require(build.diagnostics.hasErrors(),
        "invalid native sequence signature was accepted");
    bool found = false;
    for (const auto& diagnostic : build.diagnostics.items()) {
        found = found || diagnostic.code == "RS2490";
    }
    require(found,
        "invalid native sequence did not produce RS2490");
}

void testYieldOutsideSequenceDiagnostics() {
    realscript::compiler::Compilation compilation({{"bad-yield.rs", R"(
module Phase18.YieldBad;
int main()
{
    yield wait_ticks(1);
    return 0;
}
)"}});
    const auto build = compilation.build();
    require(build.diagnostics.hasErrors(),
        "yield outside sequence was accepted");
    bool found = false;
    for (const auto& diagnostic : build.diagnostics.items()) {
        found = found || diagnostic.code == "RS2494";
    }
    require(found,
        "yield outside sequence did not produce RS2494");
}

} // namespace

int main() {
    int failures = 0;
    const auto run = [&](const char* name, auto test) {
        try {
            test();
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << name << ": "
                      << error.what() << '\n';
        }
    };
    run("native structured control flow", testNativeControlFlowExecution);
    run("native infinite loop break", testNativeInfiniteLoopBreak);
    run("native control diagnostics", testNativeDiagnostics);
    run(
        "structured control flow bypasses expansion",
        testNoStructuredSourceRewrite);
    run("native interface contracts", testNativeInterfaceContracts);
    run("native interface diagnostics", testNativeInterfaceDiagnostics);
    run("interfaces bypass expansion", testInterfaceBypassesExpansion);
    run("native attributes", testNativeAttributes);
    run("attributes bypass expansion", testAttributesBypassExpansion);
    run("native reference parameters", testNativeReferenceParameters);
    run("native reference diagnostics", testNativeReferenceDiagnostics);
    run("references bypass expansion", testReferencesBypassExpansion);
    run("native value aliases", testNativeValueAliases);
    run("aliases bypass expansion", testAliasesBypassExpansion);
    run("native sequence diagnostics", testNativeSequenceDiagnostics);
    run("yield outside sequence diagnostics", testYieldOutsideSequenceDiagnostics);
    return failures == 0 ? 0 : 1;
}
