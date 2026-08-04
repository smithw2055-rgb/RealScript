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
    return (double)a + (double)b + (double)c + (double)d +
        (double)e + (double)f + (double)g + (double)h;
}
)");
    require(
        result.succeeded &&
            std::get<double>(result.value) == 36.5,
        "native value aliases produced the wrong result");
}


void testNativeEventSyntax() {
    realscript::text::SourceText source(R"(
module Phase18.Events;
delegate void ChangedHandler(int amount);
class Counter
{
    event ChangedHandler Changed;
    int total;
    void Run()
    {
        Changed += OnChanged;
        Changed += amount => total = total + amount;
    }
    void OnChanged(int amount) { total = total + amount; }
}
)", "event-syntax.rs");
    realscript::diagnostics::DiagnosticBag diagnostics;
    realscript::syntax::Parser parser(source, diagnostics);
    auto unit = parser.parseCompilationUnit();
    require(!diagnostics.hasErrors(),
        "native event syntax failed to parse:\n" +
            diagnosticsText(diagnostics));
    require(unit.delegates.size() == 1 &&
            unit.classes.size() == 1 &&
            unit.classes.front().events.size() == 1,
        "native delegate or event declaration was not retained");
    const auto& statements =
        unit.classes.front().methods.front().body.statements;
    require(statements.size() == 2 &&
            statements[0]->kind() ==
                realscript::syntax::SyntaxKind::EventSubscriptionStatement &&
            statements[1]->kind() ==
                realscript::syntax::SyntaxKind::EventSubscriptionStatement,
        "native event subscription statements were not retained");
    const auto& subscription = static_cast<const
        realscript::syntax::EventSubscriptionStatementSyntax&>(
            *statements[1]);
    require(subscription.handler->kind() ==
            realscript::syntax::SyntaxKind::LambdaExpression,
        "native event lambda was not retained");
}

void testNativeEventsExecution() {
    const auto result = execute(R"(
module Phase18;
delegate void ChangedHandler(int amount);
class Counter
{
    event ChangedHandler Changed;
    int total;

    Counter()
    {
        Changed += OnChanged;
        Changed += amount => total = total + amount;
    }

    void OnChanged(int amount)
    {
        total = total + amount;
    }

    int Run()
    {
        Changed(3);
        Changed -= OnChanged;
        Changed(2);
        return total;
    }
}

int main()
{
    Counter counter = new Counter();
    return counter.Run();
}
)");
    require(
        result.succeeded &&
            std::get<std::int64_t>(result.value) == 8,
        "native event execution produced the wrong result");
}

void testNativeEventDiagnostics() {
    realscript::compiler::Compilation compilation({{
        "bad-events.rs",
        R"(
module Phase18.BadEvents;
delegate void ChangedHandler(int amount);
class Counter
{
    event ChangedHandler Changed;
    void Wrong(string value) {}
    void Run() { Changed += Wrong; }
}
)"}});
    const auto build = compilation.build();
    require(build.diagnostics.hasErrors(),
        "invalid native event handler was accepted");
    bool found = false;
    for (const auto& diagnostic : build.diagnostics.items()) {
        found = found || diagnostic.code == "RS8305";
    }
    require(found,
        "invalid native event handler did not produce RS8305");
}


void testNativeGenericSyntax() {
    realscript::text::SourceText source(R"(
module Phase18.Generics;
class Pair<TLeft, TRight>
{
    TLeft left;
    TRight right;
}
T Identity<T>(T value) { return value; }
int main()
{
    Pair<int, List<string>> value =
        new Pair<int, List<string>>();
    return Identity<int>(1);
}
)", "generic-syntax.rs");
    realscript::diagnostics::DiagnosticBag diagnostics;
    realscript::syntax::Parser parser(source, diagnostics);
    auto unit = parser.parseCompilationUnit();
    require(!diagnostics.hasErrors(),
        "native generic syntax failed to parse:\n" +
            diagnosticsText(diagnostics));
    require(unit.classes.size() == 1 &&
            unit.classes.front().typeParameters.size() == 2 &&
            unit.functions.size() == 2 &&
            unit.functions.front().typeParameters.size() == 1,
        "native generic declarations were not retained");
    const auto& mainBody = unit.functions.back().body.statements;
    const auto& declaration = static_cast<const
        realscript::syntax::VariableDeclarationStatementSyntax&>(
            *mainBody.front());
    require(declaration.type.typeArguments.size() == 2 &&
            declaration.type.typeArguments[1].typeArguments.size() == 1,
        "nested native generic type arguments were not retained");
    const auto& returned = static_cast<const
        realscript::syntax::ReturnStatementSyntax&>(
            *mainBody.back());
    const auto& call = static_cast<const
        realscript::syntax::CallExpressionSyntax&>(
            *returned.expression);
    require(call.typeArguments.size() == 1,
        "native generic call arguments were not retained");
}

void testNativeGenericSpecialization() {
    realscript::compiler::Compilation compilation({{
        "native-generics.rs",
        R"(
module Phase18.NativeGenerics;
class Box<T>
{
    T value;
    Box(T initial) { value = initial; }
    T Get() { return value; }
}
T Identity<T>(T value) { return value; }
int main()
{
    Box<int> box = new Box<int>(Identity<int>(4));
    List<int> values = new List<int>(2);
    values.Add(box.Get());
    values.Add(3);
    return values.Get(0) + values.Get(1);
}
)"}});
    const auto build = compilation.build();
    require(!build.diagnostics.hasErrors(),
        "native generic specialization failed:\n" +
            diagnosticsText(build.diagnostics));
    require(build.nativeGenericInstantiations.size() >= 3,
        "native generic specialization metadata was not retained");

    realscript::bytecode::Lowerer lowerer;
    std::vector<realscript::bytecode::Module> modules;
    for (const auto& sourceModule : build.modules) {
        auto module = lowerer.lower(sourceModule);
        realscript::diagnostics::DiagnosticBag diagnostics;
        require(realscript::bytecode::verifyModule(module, diagnostics),
            "native generic bytecode verification failed:\n" +
                diagnosticsText(diagnostics));
        modules.push_back(std::move(module));
    }
    realscript::runtime::Interpreter interpreter(std::move(modules));
    const auto result = interpreter.invoke(
        "Phase18.NativeGenerics::main");
    require(result.succeeded &&
            std::get<std::int64_t>(result.value) == 7,
        "native generic specialization produced the wrong result");
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
    run("native interface contracts", testNativeInterfaceContracts);
    run("native interface diagnostics", testNativeInterfaceDiagnostics);
    run("native attributes", testNativeAttributes);
    run("native reference parameters", testNativeReferenceParameters);
    run("native reference diagnostics", testNativeReferenceDiagnostics);
    run("native value aliases", testNativeValueAliases);
    run("native event syntax", testNativeEventSyntax);
    run("native event execution", testNativeEventsExecution);
    run("native event diagnostics", testNativeEventDiagnostics);
    run("native generic syntax", testNativeGenericSyntax);
    run("native generic specialization", testNativeGenericSpecialization);
    run("native sequence diagnostics", testNativeSequenceDiagnostics);
    run("yield outside sequence diagnostics", testYieldOutsideSequenceDiagnostics);
    return failures == 0 ? 0 : 1;
}
